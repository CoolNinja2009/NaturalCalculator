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
    ID3D11Texture2D* stagingTextures[2] = { nullptr, nullptr };
    int stagingIndex = 0;
    bool stagingValid[2] = { false, false };

    int currentWidth = 0;
    int currentHeight = 0;

    std::unordered_map<std::string, ID3D11PixelShader*> shaderCache;

    bool initialized = false;
    bool available = false;
    bool isWarp = false;
    std::string lastCompileError;
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
    output.uv = float2((id % 2 == 1) ? 1.0 : 0.0, (id >= 2) ? 1.0 : 0.0);
    output.pos = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}
)";

std::string buildShaderSource(const std::string& mathExpr, GraphEquationKind kind) {
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
    if (a == 0.0) {
        if (b == 0.0) return 1.0;
        if (b > 0.0) return 0.0;
        return asfloat(0x7FC00000);
    }
    if (a < 0.0) {
        if (frac(b) != 0.0) return asfloat(0x7FC00000);
        float res = pow(-a, b);
        return (fmod(abs(b), 2.0) >= 0.5) ? -res : res;
    }
    return pow(a, b);
}

float gpu_sinc(float v) {
    if (abs(v) < 1e-6) return 1.0;
    return sin(v) / v;
}
)";

    if (kind == GraphEquationKind::ExplicitY) {
        oss << R"(
float evalCurve(float x) {
    return )" << mathExpr << R"(;
}
)";
    } else if (kind == GraphEquationKind::ExplicitX) {
        oss << R"(
float evalCurve(float y) {
    return )" << mathExpr << R"(;
}
)";
    } else {
        oss << R"(
float evalF(float x, float y) {
    return )" << mathExpr << R"(;
}
)";
    }

    oss << R"(
float4 PSMain(VS_OUTPUT input) : SV_Target {
    float2 center = float2((uBounds.x + uBounds.y) * 0.5, (uBounds.w + uBounds.z) * 0.5);
    float2 halfSize = float2((uBounds.y - uBounds.x) * 0.5, (uBounds.w - uBounds.z) * 0.5);

    // High precision relative coordinates to prevent float32 underflow at high zoom depths
    float relX = (input.uv.x - 0.5) * (2.0 * halfSize.x);
    float relY = (0.5 - input.uv.y) * (2.0 * halfSize.y);

    float mathX = center.x + relX;
    float mathY = center.y + relY;

    float dx = max((2.0 * halfSize.x) / uScreen.x, 1e-7);
    float dy = max((2.0 * halfSize.y) / uScreen.y, 1e-7);

    float4 color = uBgCol;

    // 1. Desmos-grade Adaptive Grid (Major + Minor Subdivisions)
    if (uSteps.x > 0.0 && uSteps.y > 0.0) {
        float offsetX = fmod(center.x, uSteps.x);
        float offsetY = fmod(center.y, uSteps.y);
        float2 gridCoord = float2((relX + offsetX) / uSteps.x, (relY + offsetY) / uSteps.y);

        float2 majorDist = abs(frac(gridCoord + 0.5) - 0.5);
        float2 pixPerMajor = float2(uSteps.x / dx, uSteps.y / dy);
        float2 distPixMajor = majorDist * pixPerMajor;
        float majorLine = min(distPixMajor.x, distPixMajor.y);
        float majorAlpha = saturate(1.2 - majorLine) * uGridCol.a;

        float minorStepX = uSteps.x * 0.2;
        float minorStepY = uSteps.y * 0.2;
        float minorOffX = fmod(center.x, minorStepX);
        float minorOffY = fmod(center.y, minorStepY);
        float2 minorCoord = float2((relX + minorOffX) / minorStepX, (relY + minorOffY) / minorStepY);
        float2 minorDist = abs(frac(minorCoord + 0.5) - 0.5);
        float2 pixPerMinor = pixPerMajor * 0.2;
        float2 distPixMinor = minorDist * pixPerMinor;
        float minorLine = min(distPixMinor.x, distPixMinor.y);
        float minorAlpha = saturate(1.0 - minorLine) * (uGridCol.a * 0.35);

        float totalGridAlpha = max(majorAlpha, minorAlpha);
        color = lerp(color, float4(uGridCol.rgb, 1.0), totalGridAlpha);
    }

    // 2. Bold Coordinate Axes (x=0, y=0)
    float2 axisDistPix = float2(abs(mathX) / dx, abs(mathY) / dy);
    float axisLine = min(axisDistPix.x, axisDistPix.y);
    float axisAlpha = saturate(1.3 - axisLine * 0.8) * uAxisCol.a;
    color = lerp(color, float4(uAxisCol.rgb, 1.0), axisAlpha);
)";

    if (kind == GraphEquationKind::ExplicitY) {
        oss << R"(
    // 3. Explicit Y = f(x) with Subpixel Tangent Distance and Dotted Vertical Asymptotes
    float y0 = evalCurve(mathX);
    float yLeft = evalCurve(mathX - dx);
    float yRight = evalCurve(mathX + dx);

    float jump = max(abs(y0 - yLeft), abs(yRight - y0));
    bool isPole = false;
    if (jump > dy * 35.0) {
        if ((y0 * yLeft < 0.0) || (y0 * yRight < 0.0) || (yLeft * yRight < 0.0)) {
            isPole = true;
        }
    }

    if (isPole) {
        // Dotted vertical line down the asymptote
        float dotPhase = fmod(input.uv.y * uScreen.y, 8.0);
        if (dotPhase < 4.0) {
            float asymAlpha = saturate(0.65) * uCurveCol.a;
            color = lerp(color, float4(uCurveCol.rgb, 1.0), asymAlpha);
        }
    } else if (isfinite(y0)) {
        float distY = abs(mathY - y0) / dy;
        float slope = (yRight - yLeft) / (2.0 * dx);
        float slopePix = slope * (dx / dy);
        float distPix = distY / sqrt(1.0 + slopePix * slopePix);

        if (distPix <= 2.2) {
            float core = saturate(1.6 - distPix);
            float glow = saturate(1.0 - distPix * 0.35) * 0.35;
            float alpha = saturate(core + glow) * uCurveCol.a;
            color = lerp(color, float4(uCurveCol.rgb, 1.0), alpha);
        }
    }
)";
    } else if (kind == GraphEquationKind::ExplicitX) {
        oss << R"(
    // 3. Explicit X = f(y) with Subpixel Tangent Distance and Dotted Horizontal Asymptotes
    float x0 = evalCurve(mathY);
    float xUp = evalCurve(mathY + dy);
    float xDown = evalCurve(mathY - dy);

    float jump = max(abs(x0 - xDown), abs(xUp - x0));
    bool isPole = false;
    if (jump > dx * 35.0) {
        if ((x0 * xDown < 0.0) || (x0 * xUp < 0.0) || (xDown * xUp < 0.0)) {
            isPole = true;
        }
    }

    if (isPole) {
        float dotPhase = fmod(input.uv.x * uScreen.x, 8.0);
        if (dotPhase < 4.0) {
            float asymAlpha = saturate(0.65) * uCurveCol.a;
            color = lerp(color, float4(uCurveCol.rgb, 1.0), asymAlpha);
        }
    } else if (isfinite(x0)) {
        float distX = abs(mathX - x0) / dx;
        float slope = (xUp - xDown) / (2.0 * dy);
        float slopePix = slope * (dy / dx);
        float distPix = distX / sqrt(1.0 + slopePix * slopePix);

        if (distPix <= 2.2) {
            float core = saturate(1.6 - distPix);
            float glow = saturate(1.0 - distPix * 0.35) * 0.35;
            float alpha = saturate(core + glow) * uCurveCol.a;
            color = lerp(color, float4(uCurveCol.rgb, 1.0), alpha);
        }
    }
)";
    } else if (kind == GraphEquationKind::ImplicitXY) {
        oss << R"(
    // 3. Implicit F(x, y) = 0 with Strict Zero-Crossing Gate (Eliminates Exterior Blobs)
    float f0 = evalF(mathX, mathY);
    if (isfinite(f0)) {
        float fR = evalF(mathX + dx, mathY);
        float fL = evalF(mathX - dx, mathY);
        float fU = evalF(mathX, mathY + dy);
        float fD = evalF(mathX, mathY - dy);

        float minVal = min(f0, min(min(fR, fL), min(fU, fD)));
        float maxVal = max(f0, max(max(fR, fL), max(fU, fD)));

        // Strict Zero-Crossing Verification
        if (minVal <= 0.0 && maxVal >= 0.0) {
            float df_dx = (fR - fL) * 0.5;
            float df_dy = (fU - fD) * 0.5;
            float grad = length(float2(df_dx / dx, df_dy / dy));

            if (grad > 1e-6) {
                float distPix = abs(f0) / (grad * max(dx, dy));
                if (distPix <= 2.2) {
                    float core = saturate(1.6 - distPix);
                    float glow = saturate(1.0 - distPix * 0.35) * 0.35;
                    float alpha = saturate(core + glow) * uCurveCol.a;
                    color = lerp(color, float4(uCurveCol.rgb, 1.0), alpha);
                }
            }
        }
    }
)";
    }

    oss << R"(
    return color;
}
)";
    return oss.str();
}

std::string buildDefaultShaderSource() {
    return buildShaderSource("1.0", GraphEquationKind::None);
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
            if (it->opChar == '+' || it->opChar == '-' || it->opChar == '*') {
                out += " ";
                out += it->opChar;
                out += " ";
                return true;
            }
            if (it->opChar == '/') {
                out += " / ";
                return true;
            }
            // Reject any unhandled or illegal operator punctuation (e.g. ';', '!', ',')
            return false;
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

} // namespace

// Convert GraphAnalysis to HLSL expression
bool analysisToHLSL(const GraphAnalysis& analysis, std::string& outExpr) {
    if (!analysis.isValid) return false;

    if (analysis.kind == GraphEquationKind::ExplicitY) {
        std::string rightStr;
        const Row* right = analysis.rightRow ? analysis.rightRow.get() : analysis.leftRow.get();
        if (!rowToHLSL(right, rightStr)) return false;
        outExpr = "(" + rightStr + ")";
        return true;
    } else if (analysis.kind == GraphEquationKind::ExplicitX) {
        std::string rightStr;
        const Row* right = analysis.rightRow ? analysis.rightRow.get() : analysis.leftRow.get();
        if (!rowToHLSL(right, rightStr)) return false;
        outExpr = "(" + rightStr + ")";
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

namespace {

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
    if (s_gpu.stagingTextures[0]) { s_gpu.stagingTextures[0]->Release(); s_gpu.stagingTextures[0] = nullptr; }
    if (s_gpu.stagingTextures[1]) { s_gpu.stagingTextures[1]->Release(); s_gpu.stagingTextures[1] = nullptr; }
    s_gpu.stagingIndex = 0;
    s_gpu.stagingValid[0] = false;
    s_gpu.stagingValid[1] = false;

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
    hr = s_gpu.device->CreateTexture2D(&td, nullptr, &s_gpu.stagingTextures[0]);
    if (FAILED(hr)) return false;
    hr = s_gpu.device->CreateTexture2D(&td, nullptr, &s_gpu.stagingTextures[1]);
    if (FAILED(hr)) return false;

    s_gpu.currentWidth = width;
    s_gpu.currentHeight = height;
    return true;
}

ID3D11PixelShader* getOrCreatePixelShader(const std::string& mathExpr, GraphEquationKind kind) {
    std::string cacheKey = std::to_string(static_cast<int>(kind)) + ":" + mathExpr;
    auto it = s_gpu.shaderCache.find(cacheKey);
    if (it != s_gpu.shaderCache.end()) return it->second;

    std::string hlslSource = buildShaderSource(mathExpr, kind);
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
        if (errorBlob) {
            const char* errStr = static_cast<const char*>(errorBlob->GetBufferPointer());
            s_gpu.lastCompileError = errStr ? errStr : "Unknown compile error";
            OutputDebugStringA("[GPU Graph] HLSL compilation failed:\n");
            OutputDebugStringA(s_gpu.lastCompileError.c_str());
            OutputDebugStringA("\n");
            errorBlob->Release();
        } else {
            s_gpu.lastCompileError = "D3DCompile failed without error blob";
        }
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

    s_gpu.shaderCache[cacheKey] = ps;
    return ps;
}

} // namespace

bool isGpuGraphAvailable() {
    if (!s_gpu.initialized) initGpuGraph();
    return s_gpu.available;
}

bool isGpuGraphHardwareAdapter() {
    if (!s_gpu.initialized) initGpuGraph();
    return s_gpu.available && !s_gpu.isWarp;
}

const char* getGpuGraphLastError() {
    return s_gpu.lastCompileError.c_str();
}

bool initGpuGraph() {
    if (s_gpu.initialized) return s_gpu.available;
    s_gpu.initialized = true;
    s_gpu.available = false;
    s_gpu.isWarp = false;

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

    if (SUCCEEDED(hr)) {
        s_gpu.isWarp = false;
    } else {
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
        s_gpu.isWarp = true;
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

namespace {

struct GpuGraphCacheKey {
    std::string title;
    int w = 0, h = 0;
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
    COLORREF bg = 0, curve = 0, grid = 0, axis = 0;
    bool isDark = false;
    bool degreesMode = false;
    bool isValid = false;

    bool operator==(const GpuGraphCacheKey& o) const {
        return w == o.w && h == o.h &&
               minX == o.minX && maxX == o.maxX &&
               minY == o.minY && maxY == o.maxY &&
               bg == o.bg && curve == o.curve && grid == o.grid && axis == o.axis &&
               isDark == o.isDark && degreesMode == o.degreesMode &&
               isValid == o.isValid && title == o.title;
    }
};

static GpuGraphCacheKey s_lastCacheKey;
static std::vector<uint32_t> s_cachedPixels;
static bool s_hasCachedPixels = false;

} // namespace

void shutdownGpuGraph() {
    s_cachedPixels.clear();
    s_hasCachedPixels = false;
    s_lastCacheKey = {};

    for (auto& pair : s_gpu.shaderCache) {
        if (pair.second) pair.second->Release();
    }
    s_gpu.shaderCache.clear();

    if (s_gpu.constantBuffer) { s_gpu.constantBuffer->Release(); s_gpu.constantBuffer = nullptr; }
    if (s_gpu.psDefault) { s_gpu.psDefault->Release(); s_gpu.psDefault = nullptr; }
    if (s_gpu.vsQuad) { s_gpu.vsQuad->Release(); s_gpu.vsQuad = nullptr; }
    if (s_gpu.rtView) { s_gpu.rtView->Release(); s_gpu.rtView = nullptr; }
    if (s_gpu.stagingTextures[0]) { s_gpu.stagingTextures[0]->Release(); s_gpu.stagingTextures[0] = nullptr; }
    if (s_gpu.stagingTextures[1]) { s_gpu.stagingTextures[1]->Release(); s_gpu.stagingTextures[1] = nullptr; }
    s_gpu.stagingIndex = 0;
    s_gpu.stagingValid[0] = false;
    s_gpu.stagingValid[1] = false;
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
            activePS = getOrCreatePixelShader(hlslExpr, analysis.kind);
        }
        if (!activePS) {
            // Expression contains non-GPU scalar construct (e.g. limit/matrix), fallback to CPU
            return false;
        }
    } else {
        activePS = s_gpu.psDefault;
    }

    // Grid steps
    double rangeX = state.maxX - state.minX;
    double rangeY = state.maxY - state.minY;
    double xStep = chooseStep(rangeX, 8);
    double yStep = chooseStep(rangeY, 8);

    GraphPalette pal = getGraphPalette(theme);
    GpuGraphCacheKey currentKey;
    currentKey.title = analysis.isValid ? analysis.title : "";
    currentKey.w = w;
    currentKey.h = h;
    currentKey.minX = state.minX;
    currentKey.maxX = state.maxX;
    currentKey.minY = state.minY;
    currentKey.maxY = state.maxY;
    currentKey.bg = pal.bg;
    currentKey.curve = pal.curve;
    currentKey.grid = pal.grid;
    currentKey.axis = pal.axis;
    currentKey.isDark = theme.isDark;
    currentKey.degreesMode = degreesMode;
    currentKey.isValid = analysis.isValid;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    bool cacheHit = s_hasCachedPixels && (currentKey == s_lastCacheKey) &&
                    (s_cachedPixels.size() >= (size_t)(w * h));
    if (cacheHit) {
        SetDIBitsToDevice(
            hdc,
            graphRect.left, graphRect.top,
            w, h,
            0, 0,
            0, h,
            s_cachedPixels.data(),
            &bmi,
            DIB_RGB_COLORS
        );
    } else {
        if (!resizeTargets(w, h)) return false;

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

        colorrefToFloat4(pal.curve, 1.0f, params->curveCol);
        colorrefToFloat4(pal.grid, 0.65f, params->gridCol);
        colorrefToFloat4(pal.axis, 0.95f, params->axisCol);
        colorrefToFloat4(pal.bg, 1.0f, params->bgCol);

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

        int writeIdx = s_gpu.stagingIndex;
        int readIdx = 1 - s_gpu.stagingIndex;

        // Dispatch asynchronous copy to the current staging buffer
        s_gpu.context->CopyResource(s_gpu.stagingTextures[writeIdx], s_gpu.rtTexture);
        s_gpu.stagingValid[writeIdx] = true;

        // Double-buffered asynchronous readback:
        // When interactively dragging/panning and the previous staging texture has valid data,
        // attempt a non-blocking map (DO_NOT_WAIT) on the already-completed buffer to eliminate CPU stalls.
        int targetIdx = writeIdx;
        bool mappedSuccess = false;
        if (state.isDragging && s_gpu.stagingValid[readIdx]) {
            HRESULT hrTry = s_gpu.context->Map(s_gpu.stagingTextures[readIdx], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (SUCCEEDED(hrTry)) {
                targetIdx = readIdx;
                mappedSuccess = true;
            }
        }

        if (!mappedSuccess) {
            // Immediate synchronous map for static/first-time frames
            hr = s_gpu.context->Map(s_gpu.stagingTextures[writeIdx], 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(hr)) return false;
            targetIdx = writeIdx;
        }

        if (s_cachedPixels.size() < (size_t)(w * h)) {
            s_cachedPixels.resize(w * h);
        }
        const uint8_t* src = (const uint8_t*)mapped.pData;
        uint8_t* dst = (uint8_t*)s_cachedPixels.data();
        if (mapped.RowPitch == (UINT)(w * 4)) {
            memcpy(dst, src, (size_t)(w * h * 4));
        } else {
            for (int y = 0; y < h; ++y) {
                memcpy(dst + y * w * 4, src + y * mapped.RowPitch, w * 4);
            }
        }
        s_gpu.context->Unmap(s_gpu.stagingTextures[targetIdx], 0);

        // Advance double-buffer index
        s_gpu.stagingIndex = 1 - s_gpu.stagingIndex;

        SetDIBitsToDevice(
            hdc,
            graphRect.left, graphRect.top,
            w, h,
            0, 0,
            0, h,
            s_cachedPixels.data(),
            &bmi,
            DIB_RGB_COLORS
        );

        s_lastCacheKey = currentKey;
        s_hasCachedPixels = true;
    }

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
            std::string label = formatAxisNumber(x, xStep);
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
            std::string label = formatAxisNumber(y, yStep);
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

    // Hardware Accelerated Pill Badge: "[GPU D3D11]" / "[WARP D3D11]"
    int badgeW = 78;
    RECT gpuBadge = { resetBtn.left - badgeW - 8, by, resetBtn.left - 8, by + btnSize };
    HBRUSH gpuBrush = CreateSolidBrush(theme.isDark ? RGB(0x1B, 0x38, 0x28) : RGB(0xD4, 0xF0, 0xDF));
    HPEN gpuPen = CreatePen(PS_SOLID, 1, theme.isDark ? RGB(0x2E, 0x7D, 0x48) : RGB(0x63, 0xC4, 0x86));
    HBRUSH oldB = (HBRUSH)SelectObject(hdc, gpuBrush);
    HPEN oldP = (HPEN)SelectObject(hdc, gpuPen);
    RoundRect(hdc, gpuBadge.left, gpuBadge.top, gpuBadge.right, gpuBadge.bottom, 8, 8);
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(gpuBrush);
    DeleteObject(gpuPen);

    SetTextColor(hdc, theme.isDark ? RGB(0x5A, 0xDE, 0x8B) : RGB(0x19, 0x87, 0x54));
    SelectObject(hdc, smallFont ? smallFont : font);
    const char* badgeText = s_gpu.isWarp ? "WARP D3D11" : "GPU D3D11";
    DrawTextA(hdc, badgeText, -1, &gpuBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Header Title
    int titleMaxRight = gpuBadge.left - 8;
    int titleLeft = graphRect.left + 12;
    if (titleMaxRight > titleLeft + 30) {
        std::string titleText = analysis.isValid ? analysis.title : "Graph Engine (Direct3D 11)";
        RECT titleRect = { titleLeft, by, titleMaxRight, by + btnSize };
        HBRUSH tagBrush = CreateSolidBrush(theme.isDark ? RGB(0x28, 0x2C, 0x34) : RGB(0xEA, 0xEE, 0xF2));
        HPEN tagPen = CreatePen(PS_SOLID, 1, theme.isDark ? RGB(0x3E, 0x44, 0x51) : RGB(0xCF, 0xD4, 0xDA));
        oldB = (HBRUSH)SelectObject(hdc, tagBrush);
        oldP = (HPEN)SelectObject(hdc, tagPen);
        RoundRect(hdc, titleRect.left, titleRect.top, titleRect.right, titleRect.bottom, 8, 8);
        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(tagBrush);
        DeleteObject(tagPen);

        SetTextColor(hdc, theme.isDark ? RGB(0xF0, 0xF2, 0xF5) : RGB(0x21, 0x25, 0x29));
        SelectObject(hdc, font ? font : smallFont);
        RECT titleTextRect = { titleRect.left + 8, titleRect.top, titleRect.right - 8, titleRect.bottom };
        DrawTextA(hdc, titleText.c_str(), -1, &titleTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // Right-Click Probe Tooltip
    if (state.isRightProbing) {
        drawProbeTooltip(hdc, localRect, state, analysis, theme, font ? font : smallFont);
    }


    SelectObject(hdc, oldFont);
    return true;
}
