// gpu_graph.cpp
//
// Hardware-accelerated 2D graphing engine for NaturalCalculator using Direct3D 11.
// Evaluates mathematical equations per-pixel in parallel on the GPU with subpixel
// anti-aliasing, screen-space implicit contouring, and zero-flicker GDI compositing.

#include "gpu_graph.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <cstdint>

typedef HRESULT (WINAPI *PFN_D3DCOMPILE)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines,
    ID3DInclude *pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs
);

namespace {

struct GpuContext {
    HMODULE compilerDll = nullptr;
    PFN_D3DCOMPILE d3dCompile = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    ID3D11VertexShader* vsQuad = nullptr;
    ID3D11PixelShader* psDefault = nullptr;
    ID3D11Buffer* constantBuffer = nullptr;

    ID3D11Texture2D* rtTexture = nullptr;
    ID3D11RenderTargetView* rtView = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;

    int currentWidth = 0;
    int currentHeight = 0;

    std::unordered_map<std::string, ID3D11PixelShader*> shaderCache;

    bool initialized = false;
    bool available = false;
} s_gpu;

struct ShaderConstantParams {
    float bounds[4];   // minX, maxX, minY, maxY
    float screen[4];   // width, height, isDark, isDegrees
    float curveCol[4]; // r, g, b, a
    float gridCol[4];  // r, g, b, a
    float axisCol[4];  // r, g, b, a
    float bgCol[4];    // r, g, b, a
    float steps[4];    // xStep, yStep, 0, 0
};

const char kQuadVS[] = R"(
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT VSMain(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.uv = float2((id == 1 || id == 2) ? 1.0 : 0.0, (id >= 2) ? 1.0 : 0.0);
    output.pos = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}
)";

std::string buildShaderSource(const std::string& mathExpr) {
    std::ostringstream oss;
    oss << R"(
cbuffer Params : register(b0) {
    float4 uBounds;   // minX, maxX, minY, maxY
    float4 uScreen;   // width, height, isDark, isDegrees
    float4 uCurveCol; // r, g, b, a
    float4 uGridCol;  // r, g, b, a
    float4 uAxisCol;  // r, g, b, a
    float4 uBgCol;    // r, g, b, a
    float4 uSteps;    // xStep, yStep, 0, 0
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float toRad(float a) {
    return uScreen.w > 0.5 ? radians(a) : a;
}

float gpu_pow(float a, float b) {
    if (a == 0.0) return 0.0;
    if (frac(b) == 0.0) {
        float res = pow(abs(a), b);
        return (fmod(abs(b), 2.0) != 0.0 && a < 0.0) ? -res : res;
    }
    return pow(abs(a), b);
}

float gpu_sinc(float v) {
    if (abs(v) < 1e-6) return 1.0;
    return sin(v) / v;
}

float evalF(float x, float y) {
    return )" << mathExpr << R"(;
}

float4 PSMain(VS_OUTPUT input) : SV_Target {
    float mathX = lerp(uBounds.x, uBounds.y, input.uv.x);
    float mathY = lerp(uBounds.w, uBounds.z, input.uv.y);

    float4 color = uBgCol;

    // Background Grid
    if (uSteps.x > 0.0 && uSteps.y > 0.0) {
        float2 cell = float2(mathX / uSteps.x, mathY / uSteps.y);
        float2 gridDist = abs(frac(cell + 0.5) - 0.5);
        float2 pixGrid = gridDist / float2(max(abs(ddx(cell.x)), 1e-5), max(abs(ddy(cell.y)), 1e-5));
        float gridLine = min(pixGrid.x, pixGrid.y);
        float gridAlpha = saturate(1.0 - gridLine * 0.8) * uGridCol.a;
        color = lerp(color, float4(uGridCol.rgb, 1.0), gridAlpha);
    }

    // Main Axes
    float2 axisDistPix = float2(abs(mathX) / max(abs(ddx(mathX)), 1e-5), abs(mathY) / max(abs(ddy(mathY)), 1e-5));
    float axisLine = min(axisDistPix.x, axisDistPix.y);
    float axisAlpha = saturate(1.2 - axisLine * 0.7) * uAxisCol.a;
    color = lerp(color, float4(uAxisCol.rgb, 1.0), axisAlpha);

    // Math Curve / Implicit Contour
    float f = evalF(mathX, mathY);
    float grad = length(float2(ddx(f), ddy(f)));
    if (grad > 1e-7 && isfinite(f)) {
        float distPix = abs(f) / grad;
        float core = saturate(1.6 - distPix);
        float glow = saturate(1.0 - distPix * 0.28) * 0.38;
        float alpha = saturate(core + glow) * uCurveCol.a;
        color = lerp(color, float4(uCurveCol.rgb, 1.0), alpha);
    }

    return color;
}
)";
    return oss.str();
}

std::string buildDefaultShaderSource() {
    return buildShaderSource("1.0"); // Evaluates to non-zero -> no curve drawn, only grid and axes
}

// Float formatting for HLSL literals
std::string toHlslFloat(const std::string& num) {
    if (num.find('.') == std::string::npos && num.find('e') == std::string::npos && num.find('E') == std::string::npos) {
        return num + ".0";
    }
    return num;
}

bool itemToHLSL(const Item* it, std::string& out);

bool rowToHLSL(const Row* r, std::string& out) {
    if (!r) return true;
    for (size_t i = 0; i < r->items.size(); ++i) {
        const Item* it = r->items[i].get();
        if (i > 0) {
            const Item* prev = r->items[i - 1].get();
            bool prevIsVal = (prev->type == ItemType::Number || prev->type == ItemType::Variable ||
                              prev->type == ItemType::Constant || prev->type == ItemType::Paren ||
                              prev->type == ItemType::Power || prev->type == ItemType::Sqrt ||
                              prev->type == ItemType::Fraction);
            bool currIsVal = (it->type == ItemType::Number || it->type == ItemType::Variable ||
                              it->type == ItemType::Constant || it->type == ItemType::Paren ||
                              it->type == ItemType::Power || it->type == ItemType::Sqrt ||
                              it->type == ItemType::Fraction || it->type == ItemType::Function);
            if (prevIsVal && currIsVal) {
                out += " * ";
            }
        }
        if (!itemToHLSL(it, out)) return false;
    }
    return true;
}

bool itemToHLSL(const Item* it, std::string& out) {
    if (!it) return true;
    switch (it->type) {
        case ItemType::Number:
            out += toHlslFloat(it->numText);
            return true;
        case ItemType::Variable:
            out += it->variableName;
            return true;
        case ItemType::Constant:
            if (it->constantName == 'p') out += "3.141592653589793";
            else if (it->constantName == 'f') out += "1.618033988749895";
            else out += "2.718281828459045";
            return true;
        case ItemType::Operator:
            out += " ";
            out += it->opChar;
            out += " ";
            return true;
        case ItemType::Fraction:
            out += " ((";
            if (!rowToHLSL(it->a.get(), out)) return false;
            out += ") / (";
            if (!rowToHLSL(it->b.get(), out)) return false;
            out += ")) ";
            return true;
        case ItemType::Power:
            out += " gpu_pow(";
            if (!rowToHLSL(it->a.get(), out)) return false;
            out += ", ";
            if (!rowToHLSL(it->b.get(), out)) return false;
            out += ") ";
            return true;
        case ItemType::Sqrt:
            out += " sqrt(max(0.0, ";
            if (!rowToHLSL(it->a.get(), out)) return false;
            out += ")) ";
            return true;
        case ItemType::Paren:
            out += " (";
            if (!rowToHLSL(it->a.get(), out)) return false;
            out += ") ";
            return true;
        case ItemType::Function: {
            std::string arg;
            if (!rowToHLSL(it->a.get(), arg)) return false;
            switch (it->functionId) {
                case SciSin: out += " sin(toRad(" + arg + ")) "; return true;
                case SciCos: out += " cos(toRad(" + arg + ")) "; return true;
                case SciTan: out += " tan(toRad(" + arg + ")) "; return true;
                case SciAsin: out += " asin(" + arg + ") "; return true;
                case SciAcos: out += " acos(" + arg + ") "; return true;
                case SciAtan: out += " atan(" + arg + ") "; return true;
                case SciSinh: out += " sinh(" + arg + ") "; return true;
                case SciCosh: out += " cosh(" + arg + ") "; return true;
                case SciTanh: out += " tanh(" + arg + ") "; return true;
                case SciAsinh: out += " asinh(" + arg + ") "; return true;
                case SciAcosh: out += " acosh(" + arg + ") "; return true;
                case SciAtanh: out += " atanh(" + arg + ") "; return true;
                case SciExp: out += " exp(" + arg + ") "; return true;
                case SciLn: out += " log(" + arg + ") "; return true;
                case SciLog: out += " (log(" + arg + ") * 0.4342944819032518) "; return true;
                case SciLog2: out += " log2(" + arg + ") "; return true;
                case SciAbs: out += " abs(" + arg + ") "; return true;
                case SciFloor: out += " floor(" + arg + ") "; return true;
                case SciCeil: out += " ceil(" + arg + ") "; return true;
                case SciRound: out += " round(" + arg + ") "; return true;
                case SciSgn: out += " sign(" + arg + ") "; return true;
                case SciSinc: out += " gpu_sinc(" + arg + ") "; return true;
                case SciSec: out += " (1.0 / cos(toRad(" + arg + "))) "; return true;
                case SciCsc: out += " (1.0 / sin(toRad(" + arg + "))) "; return true;
                case SciCot: out += " (1.0 / tan(toRad(" + arg + "))) "; return true;
                case SciCbrt: out += " (sign(" + arg + ") * pow(abs(" + arg + "), 1.0/3.0)) "; return true;
                case SciDeg: out += " degrees(" + arg + ") "; return true;
                case SciRad: out += " radians(" + arg + ") "; return true;
                default:
                    return false; // Discrete or unsupported GPU function -> fallback to CPU
            }
        }
        default:
            return false;
    }
}

// Convert GraphAnalysis to an implicit HLSL zero equation F(x, y) = 0
bool analysisToHLSL(const GraphAnalysis& analysis, std::string& outExpr) {
    if (!analysis.isValid) return false;

    if (analysis.kind == GraphEquationKind::ExplicitY) {
        std::string rightStr;
        const Row* right = analysis.rightRow ? analysis.rightRow.get() : analysis.leftRow.get();
        if (!rowToHLSL(right, rightStr)) return false;
        outExpr = "y - (" + rightStr + ")";
        return true;
    } else if (analysis.kind == GraphEquationKind::ImplicitXY) {
        std::string leftStr, rightStr;
        if (!rowToHLSL(analysis.leftRow.get(), leftStr)) return false;
        if (analysis.rightRow) {
            if (!rowToHLSL(analysis.rightRow.get(), rightStr)) return false;
        } else {
            rightStr = "0.0";
        }
        outExpr = "(" + leftStr + ") - (" + rightStr + ")";
        return true;
    }
    return false;
}

double chooseStep(double range, int targetDivisions) {
    double rawStep = range / targetDivisions;
    double mag = std::pow(10.0, std::floor(std::log10(rawStep)));
    double normalized = rawStep / mag;
    double step;
    if (normalized < 1.5) step = 1.0 * mag;
    else if (normalized < 3.5) step = 2.0 * mag;
    else if (normalized < 7.5) step = 5.0 * mag;
    else step = 10.0 * mag;
    return step;
}

std::string formatNumberShort(double v) {
    char buf[32];
    double av = std::fabs(v);
    if (av < 1e-12) return "0";
    if (std::floor(v) == v && av < 1e6) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else if (av >= 1e4 || av <= 1e-3) {
        std::snprintf(buf, sizeof(buf), "%.1e", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        std::string s(buf);
        size_t dot = s.find('.');
        if (dot != std::string::npos) {
            size_t last = s.find_last_not_of('0');
            if (last == dot) last--;
            s.erase(last + 1);
        }
        return s;
    }
    return std::string(buf);
}

void colorrefToFloat4(COLORREF c, float a, float* out) {
    out[0] = (float)GetRValue(c) / 255.0f;
    out[1] = (float)GetGValue(c) / 255.0f;
    out[2] = (float)GetBValue(c) / 255.0f;
    out[3] = a;
}

bool resizeTargets(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (s_gpu.rtTexture && s_gpu.currentWidth == width && s_gpu.currentHeight == height) {
        return true;
    }

    if (s_gpu.rtView) { s_gpu.rtView->Release(); s_gpu.rtView = nullptr; }
    if (s_gpu.rtTexture) { s_gpu.rtTexture->Release(); s_gpu.rtTexture = nullptr; }
    if (s_gpu.stagingTexture) { s_gpu.stagingTexture->Release(); s_gpu.stagingTexture = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    HRESULT hr = s_gpu.device->CreateTexture2D(&td, nullptr, &s_gpu.rtTexture);
    if (FAILED(hr)) return false;

    hr = s_gpu.device->CreateRenderTargetView(s_gpu.rtTexture, nullptr, &s_gpu.rtView);
    if (FAILED(hr)) return false;

    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = s_gpu.device->CreateTexture2D(&td, nullptr, &s_gpu.stagingTexture);
    if (FAILED(hr)) return false;

    s_gpu.currentWidth = width;
    s_gpu.currentHeight = height;
    return true;
}

ID3D11PixelShader* getOrCreatePixelShader(const std::string& mathExpr) {
    auto it = s_gpu.shaderCache.find(mathExpr);
    if (it != s_gpu.shaderCache.end()) return it->second;

    std::string hlslSource = buildShaderSource(mathExpr);
    ID3DBlob* codeBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = s_gpu.d3dCompile(
        hlslSource.c_str(), hlslSource.size(),
        nullptr, nullptr, nullptr,
        "PSMain", "ps_4_0",
        0, 0,
        &codeBlob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) errorBlob->Release();
        return nullptr;
    }

    ID3D11PixelShader* ps = nullptr;
    hr = s_gpu.device->CreatePixelShader(codeBlob->GetBufferPointer(), codeBlob->GetBufferSize(), nullptr, &ps);
    codeBlob->Release();
    if (errorBlob) errorBlob->Release();

    if (FAILED(hr) || !ps) return nullptr;

    if (s_gpu.shaderCache.size() > 64) {
        // Evict oldest entries if cache grows large
        for (auto& pair : s_gpu.shaderCache) {
            if (pair.second) pair.second->Release();
        }
        s_gpu.shaderCache.clear();
    }

    s_gpu.shaderCache[mathExpr] = ps;
    return ps;
}

} // namespace

bool isGpuGraphAvailable() {
    if (!s_gpu.initialized) initGpuGraph();
    return s_gpu.available;
}

bool initGpuGraph() {
    if (s_gpu.initialized) return s_gpu.available;
    s_gpu.initialized = true;
    s_gpu.available = false;

    s_gpu.compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!s_gpu.compilerDll) return false;

    s_gpu.d3dCompile = reinterpret_cast<PFN_D3DCOMPILE>(reinterpret_cast<void*>(GetProcAddress(s_gpu.compilerDll, "D3DCompile")));
    if (!s_gpu.d3dCompile) return false;

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &s_gpu.device,
        &featureLevel,
        &s_gpu.context
    );

    if (FAILED(hr)) {
        // Fallback to WARP (software rasterizer on GPU architecture)
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &s_gpu.device,
            &featureLevel,
            &s_gpu.context
        );
        if (FAILED(hr)) return false;
    }

    // Compile fullscreen quad vertex shader
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    hr = s_gpu.d3dCompile(kQuadVS, sizeof(kQuadVS) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) errBlob->Release();
        return false;
    }
    hr = s_gpu.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s_gpu.vsQuad);
    vsBlob->Release();
    if (errBlob) errBlob->Release();
    if (FAILED(hr)) return false;

    // Compile default pixel shader
    std::string defSource = buildDefaultShaderSource();
    ID3DBlob* psBlob = nullptr;
    hr = s_gpu.d3dCompile(defSource.c_str(), defSource.size(), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) errBlob->Release();
        return false;
    }
    hr = s_gpu.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &s_gpu.psDefault);
    psBlob->Release();
    if (errBlob) errBlob->Release();
    if (FAILED(hr)) return false;

    // Create constant buffer
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShaderConstantParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = s_gpu.device->CreateBuffer(&bd, nullptr, &s_gpu.constantBuffer);
    if (FAILED(hr)) return false;

    s_gpu.available = true;
    return true;
}

void shutdownGpuGraph() {
    for (auto& pair : s_gpu.shaderCache) {
        if (pair.second) pair.second->Release();
    }
    s_gpu.shaderCache.clear();

    if (s_gpu.constantBuffer) { s_gpu.constantBuffer->Release(); s_gpu.constantBuffer = nullptr; }
    if (s_gpu.psDefault) { s_gpu.psDefault->Release(); s_gpu.psDefault = nullptr; }
    if (s_gpu.vsQuad) { s_gpu.vsQuad->Release(); s_gpu.vsQuad = nullptr; }
    if (s_gpu.rtView) { s_gpu.rtView->Release(); s_gpu.rtView = nullptr; }
    if (s_gpu.rtTexture) { s_gpu.rtTexture->Release(); s_gpu.rtTexture = nullptr; }
    if (s_gpu.stagingTexture) { s_gpu.stagingTexture->Release(); s_gpu.stagingTexture = nullptr; }
    if (s_gpu.context) { s_gpu.context->Release(); s_gpu.context = nullptr; }
    if (s_gpu.device) { s_gpu.device->Release(); s_gpu.device = nullptr; }
    if (s_gpu.compilerDll) { FreeLibrary(s_gpu.compilerDll); s_gpu.compilerDll = nullptr; }

    s_gpu.initialized = false;
    s_gpu.available = false;
}

bool renderGraphGpu(HDC hdc, const RECT& graphRect, const GraphAnalysis& analysis,
                    GraphState& state, const Theme& theme, HFONT font, HFONT smallFont,
                    bool degreesMode) {
    if (!isGpuGraphAvailable()) return false;

    int w = graphRect.right - graphRect.left;
    int h = graphRect.bottom - graphRect.top;
    if (w <= 10 || h <= 10) return false;

    if (!state.initialized) state.resetView(10.0);

    // Translate mathematical expression to HLSL
    ID3D11PixelShader* activePS = nullptr;
    if (analysis.isValid) {
        std::string hlslExpr;
        if (analysisToHLSL(analysis, hlslExpr)) {
            activePS = getOrCreatePixelShader(hlslExpr);
        }
        if (!activePS) {
            // Expression contains non-GPU scalar construct (e.g. limit/matrix), fallback to CPU
            return false;
        }
    } else {
        activePS = s_gpu.psDefault;
    }

    if (!resizeTargets(w, h)) return false;

    // Grid steps
    double rangeX = state.maxX - state.minX;
    double rangeY = state.maxY - state.minY;
    double xStep = chooseStep(rangeX, 8);
    double yStep = chooseStep(rangeY, 8);

    // Set constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = s_gpu.context->Map(s_gpu.constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    ShaderConstantParams* params = (ShaderConstantParams*)mapped.pData;
    params->bounds[0] = (float)state.minX;
    params->bounds[1] = (float)state.maxX;
    params->bounds[2] = (float)state.minY;
    params->bounds[3] = (float)state.maxY;

    params->screen[0] = (float)w;
    params->screen[1] = (float)h;
    params->screen[2] = theme.isDark ? 1.0f : 0.0f;
    params->screen[3] = degreesMode ? 1.0f : 0.0f;

    colorrefToFloat4(theme.accent, 1.0f, params->curveCol);
    COLORREF gridColor = theme.isDark ? RGB(0x2E, 0x33, 0x3D) : RGB(0xDE, 0xE2, 0xE6);
    colorrefToFloat4(gridColor, 0.65f, params->gridCol);
    COLORREF axisColor = theme.isDark ? RGB(0x6A, 0x73, 0x82) : RGB(0x8A, 0x93, 0x9E);
    colorrefToFloat4(axisColor, 0.95f, params->axisCol);
    COLORREF bgColor = theme.isDark ? RGB(0x18, 0x1A, 0x1F) : RGB(0xF8, 0xF9, 0xFA);
    colorrefToFloat4(bgColor, 1.0f, params->bgCol);

    params->steps[0] = (float)xStep;
    params->steps[1] = (float)yStep;
    params->steps[2] = 0.0f;
    params->steps[3] = 0.0f;

    s_gpu.context->Unmap(s_gpu.constantBuffer, 0);

    // Viewport & Render Target
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    s_gpu.context->RSSetViewports(1, &vp);
    s_gpu.context->OMSetRenderTargets(1, &s_gpu.rtView, nullptr);
    s_gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    s_gpu.context->VSSetShader(s_gpu.vsQuad, nullptr, 0);
    s_gpu.context->PSSetShader(activePS, nullptr, 0);
    s_gpu.context->PSSetConstantBuffers(0, 1, &s_gpu.constantBuffer);

    // Draw fullscreen GPU quad
    s_gpu.context->Draw(4, 0);

    // Copy to staging texture and read back into GDI DC
    s_gpu.context->CopyResource(s_gpu.stagingTexture, s_gpu.rtTexture);

    hr = s_gpu.context->Map(s_gpu.stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetDIBitsToDevice(
        hdc,
        graphRect.left, graphRect.top,
        w, h,
        0, 0,
        0, h,
        mapped.pData,
        &bmi,
        DIB_RGB_COLORS
    );

    s_gpu.context->Unmap(s_gpu.stagingTexture, 0);

    // -------------------------------------------------------------
    // GDI Chrome Overlay: Coordinate Numbers, Title, GPU Badge, Zoom Controls
    // -------------------------------------------------------------
    RECT localRect = { 0, 0, w, h };
    SetBkMode(hdc, TRANSPARENT);
    COLORREF textColor = theme.isDark ? RGB(0x8C, 0x94, 0xA0) : RGB(0x6C, 0x75, 0x7D);
    SetTextColor(hdc, textColor);
    HFONT oldFont = (HFONT)SelectObject(hdc, smallFont ? smallFont : font);

    // Vertical grid numbers
    double startX = std::floor(state.minX / xStep) * xStep;
    for (double x = startX; x <= state.maxX + xStep * 0.5; x += xStep) {
        if (std::fabs(x) > 1e-9) {
            std::string label = formatNumberShort(x);
            POINT axisPt = state.mathToPixel(x, 0.0, localRect);
            int labelY = std::clamp((int)axisPt.y + 4, 18, h - 18);
            RECT lr = { graphRect.left + axisPt.x - 30, graphRect.top + labelY,
                        graphRect.left + axisPt.x + 30, graphRect.top + labelY + 16 };
            DrawTextA(hdc, label.c_str(), -1, &lr, DT_CENTER | DT_SINGLELINE);
        }
    }

    // Horizontal grid numbers
    double startY = std::floor(state.minY / yStep) * yStep;
    for (double y = startY; y <= state.maxY + yStep * 0.5; y += yStep) {
        if (std::fabs(y) > 1e-9) {
            std::string label = formatNumberShort(y);
            POINT axisPt = state.mathToPixel(0.0, y, localRect);
            int labelX = std::clamp((int)axisPt.x + 6, 6, w - 44);
            RECT lr = { graphRect.left + labelX, graphRect.top + axisPt.y - 8,
                        graphRect.left + labelX + 38, graphRect.top + axisPt.y + 8 };
            DrawTextA(hdc, label.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE);
        }
    }

    // Origin (0, 0)
    POINT originPt = state.mathToPixel(0.0, 0.0, localRect);
    if (originPt.x >= 12 && originPt.x <= w - 12 && originPt.y >= 12 && originPt.y <= h - 12) {
        RECT orr = { graphRect.left + originPt.x - 14, graphRect.top + originPt.y + 3,
                     graphRect.left + originPt.x - 2,  graphRect.top + originPt.y + 17 };
        DrawTextA(hdc, "0", -1, &orr, DT_RIGHT | DT_SINGLELINE);
    }

    // Header Title
    std::string titleText = analysis.isValid ? analysis.title : "Graph Engine (Direct3D 11)";
    RECT titleRect = { graphRect.left + 12, graphRect.top + 10, graphRect.left + 260, graphRect.top + 34 };
    HBRUSH tagBrush = CreateSolidBrush(theme.isDark ? RGB(0x28, 0x2C, 0x34) : RGB(0xEA, 0xEE, 0xF2));
    HPEN tagPen = CreatePen(PS_SOLID, 1, theme.isDark ? RGB(0x3E, 0x44, 0x51) : RGB(0xCF, 0xD4, 0xDA));
    HBRUSH oldB = (HBRUSH)SelectObject(hdc, tagBrush);
    HPEN oldP = (HPEN)SelectObject(hdc, tagPen);
    RoundRect(hdc, titleRect.left, titleRect.top, titleRect.right, titleRect.bottom, 8, 8);

    // Hardware Accelerated Pill Badge: "[GPU]"
    RECT gpuBadge = { titleRect.right + 6, graphRect.top + 10, titleRect.right + 66, graphRect.top + 34 };
    HBRUSH gpuBrush = CreateSolidBrush(theme.isDark ? RGB(0x1B, 0x38, 0x28) : RGB(0xD4, 0xF0, 0xDF));
    HPEN gpuPen = CreatePen(PS_SOLID, 1, theme.isDark ? RGB(0x2E, 0x7D, 0x48) : RGB(0x63, 0xC4, 0x86));
    SelectObject(hdc, gpuBrush);
    SelectObject(hdc, gpuPen);
    RoundRect(hdc, gpuBadge.left, gpuBadge.top, gpuBadge.right, gpuBadge.bottom, 8, 8);

    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(tagBrush);
    DeleteObject(tagPen);
    DeleteObject(gpuBrush);
    DeleteObject(gpuPen);

    SetTextColor(hdc, theme.isDark ? RGB(0xF0, 0xF2, 0xF5) : RGB(0x21, 0x25, 0x29));
    SelectObject(hdc, font ? font : smallFont);
    RECT titleTextRect = { titleRect.left + 8, titleRect.top, titleRect.right - 8, titleRect.bottom };
    DrawTextA(hdc, titleText.c_str(), -1, &titleTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Draw GPU label inside badge
    SetTextColor(hdc, theme.isDark ? RGB(0x5A, 0xDE, 0x8B) : RGB(0x19, 0x87, 0x54));
    SelectObject(hdc, smallFont ? smallFont : font);
    DrawTextA(hdc, "GPU D3D11", -1, &gpuBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Zoom Buttons
    int btnSize = 24;
    int gap = 6;
    int bx = graphRect.right - btnSize - 10;
    int by = graphRect.top + 10;

    auto drawGraphButton = [&](RECT r, const char* label) {
        HBRUSH b = CreateSolidBrush(theme.isDark ? RGB(0x28, 0x2C, 0x34) : RGB(0xEA, 0xEE, 0xF2));
        HPEN p = CreatePen(PS_SOLID, 1, theme.isDark ? RGB(0x3E, 0x44, 0x51) : RGB(0xCF, 0xD4, 0xDA));
        HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
        HPEN op = (HPEN)SelectObject(hdc, p);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(b);
        DeleteObject(p);
        SetTextColor(hdc, theme.isDark ? RGB(0xEB, 0xEB, 0xEB) : RGB(0x21, 0x25, 0x29));
        DrawTextA(hdc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    RECT resetBtn = { bx - 2 * (btnSize + gap), by, bx - 2 * (btnSize + gap) + btnSize * 2, by + btnSize };
    RECT zoomOutBtn = { bx - (btnSize + gap), by, bx - gap, by + btnSize };
    RECT zoomInBtn = { bx, by, bx + btnSize, by + btnSize };

    drawGraphButton(resetBtn, "Reset");
    drawGraphButton(zoomOutBtn, "-");
    drawGraphButton(zoomInBtn, "+");

    // Crosshair and Hover Coordinate Badge
    if (state.isHovering && !state.isDragging) {
        int hx = graphRect.left + state.hoverPos.x;
        int hy = graphRect.top + state.hoverPos.y;

        HPEN chPen = CreatePen(PS_DOT, 1, theme.isDark ? RGB(0x55, 0x5E, 0x70) : RGB(0xAD, 0xB5, 0xBD));
        HPEN oldCh = (HPEN)SelectObject(hdc, chPen);
        MoveToEx(hdc, graphRect.left, hy, nullptr);
        LineTo(hdc, graphRect.right, hy);
        MoveToEx(hdc, hx, graphRect.top, nullptr);
        LineTo(hdc, hx, graphRect.bottom);
        SelectObject(hdc, oldCh);
        DeleteObject(chPen);

        // Hover coordinate tooltip
        char coordBuf[64];
        std::snprintf(coordBuf, sizeof(coordBuf), "(%s, %s)",
                      formatNumberShort(state.hoverMathX).c_str(),
                      formatNumberShort(state.hoverMathY).c_str());

        RECT tipRect = { hx + 12, hy - 22, hx + 110, hy - 2 };
        if (tipRect.right > graphRect.right - 8) {
            tipRect.left = hx - 110;
            tipRect.right = hx - 12;
        }
        if (tipRect.top < graphRect.top + 8) {
            tipRect.top = hy + 8;
            tipRect.bottom = hy + 28;
        }

        HBRUSH tipBrush = CreateSolidBrush(theme.isDark ? RGB(0x21, 0x25, 0x2B) : RGB(0xFF, 0xFF, 0xFF));
        HPEN tipPen = CreatePen(PS_SOLID, 1, theme.accent);
        HBRUSH ob2 = (HBRUSH)SelectObject(hdc, tipBrush);
        HPEN op2 = (HPEN)SelectObject(hdc, tipPen);
        RoundRect(hdc, tipRect.left, tipRect.top, tipRect.right, tipRect.bottom, 6, 6);
        SelectObject(hdc, ob2);
        SelectObject(hdc, op2);
        DeleteObject(tipBrush);
        DeleteObject(tipPen);

        SetTextColor(hdc, theme.isDark ? RGB(0xF0, 0xF2, 0xF5) : RGB(0x21, 0x25, 0x29));
        DrawTextA(hdc, coordBuf, -1, &tipRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldFont);
    return true;
}
