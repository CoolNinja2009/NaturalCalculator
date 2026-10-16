// gpu_graph.cpp
//
// Direct3D 11 implicit-curve engine for NaturalCalculator (see gpu_graph.h).
// Each equation F(x, y) = 0 becomes a pixel shader that evaluates F around
// every pixel, rejects poles and jumps, and writes anti-aliased stroke
// coverage into an R8 target. The coverage is read back synchronously and
// composited by the same pipeline as the FP64 rasterizer, so frames from
// either engine look identical.

#include "gpu_graph.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    ID3D11Buffer* constantBuffer = nullptr;

    ID3D11Texture2D* rtTexture = nullptr;       // R8 stroke coverage
    ID3D11RenderTargetView* rtView = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;  // CPU readback copy

    int currentWidth = 0;
    int currentHeight = 0;

    std::unordered_map<std::string, ID3D11PixelShader*> shaderCache;
    std::unordered_set<std::string> failedShaders;  // never retried every frame

    bool initialized = false;
    bool available = false;
    bool isWarp = false;
    std::string lastCompileError;
} s_gpu;

struct ShaderConstantParams {
    float view[4];    // center.x, center.y, halfSize.x, halfSize.y
    float screen[4];  // width, height, unused, unused
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

std::string buildShaderSource(const std::string& mathExpr) {
    std::ostringstream oss;
    oss << R"(
cbuffer Params : register(b0) {
    float4 uView;    // center.x, center.y, halfSize.x, halfSize.y
    float4 uScreen;  // width, height
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

static const float kNaN = asfloat(0x7FC00000);

// The grapher always plots in radians.
float toRad(float a) {
    return a;
}

float gpu_pow(float a, float b) {
    if (a == 0.0) {
        if (b == 0.0) return 1.0;
        if (b > 0.0) return 0.0;
        return kNaN;
    }
    if (a < 0.0) {
        if (frac(b) != 0.0) return kNaN;
        float res = pow(-a, b);
        return (fmod(abs(b), 2.0) >= 0.5) ? -res : res;
    }
    return pow(a, b);
}

float gpu_sqrt(float v) {
    return v < 0.0 ? kNaN : sqrt(v);
}

float gpu_sinc(float v) {
    if (abs(v) < 1e-6) return 1.0;
    return sin(v) / v;
}

float gpu_asinh(float v) {
    float a = abs(v);
    return sign(v) * log(a + sqrt(a * a + 1.0));
}

float gpu_acosh(float v) {
    return v < 1.0 ? kNaN : log(v + sqrt(v * v - 1.0));
}

float gpu_atanh(float v) {
    return abs(v) >= 1.0 ? kNaN : 0.5 * log((1.0 + v) / (1.0 - v));
}

float evalF(float x, float y) {
    return )" << mathExpr << R"(;
}

// Stroke profile shared with the FP64 rasterizer (curveStrokeAlpha).
float strokeAlpha(float d) {
    if (d >= 2.2) return 0.0;
    return saturate(saturate(1.6 - d) + saturate(1.0 - d * 0.35) * 0.35);
}

// Distance in pixels from p to the zero of F on the segment p -> q (whose
// ends have opposite signs), or -1 when the sign change is not a root: a
// root's bracket values shrink under bisection, a pole's grow and a jump's
// stay put. Measuring along the bracket stays accurate where F bends too
// sharply for the first-order |F| / |grad F| estimate.
float rootDistance(float2 p, float fp, float2 q, float fq, float2 pixelSize) {
    float m0 = max(abs(fp), abs(fq));
    float2 a = p;
    float2 b = q;
    float fa = fp;
    float fb = fq;
    [loop]
    for (int k = 0; k < 16; ++k) {
        float m = max(abs(fa), abs(fb));
        if (m > 64.0 * m0) return -1.0;
        if (k >= 4 && m <= 0.5 * m0) {
            float2 root = lerp(a, b, fa / (fa - fb));
            return length((root - p) / pixelSize);
        }
        float2 mid = 0.5 * (a + b);
        float fm = evalF(mid.x, mid.y);
        if (!isfinite(fm)) return -1.0;
        if ((fm > 0.0) == (fa > 0.0)) {
            a = mid;
            fa = fm;
        } else {
            b = mid;
            fb = fm;
        }
    }
    return -1.0;
}

float4 PSMain(VS_OUTPUT input) : SV_Target {
    float x = uView.x + (input.uv.x - 0.5) * 2.0 * uView.z;
    float y = uView.y + (0.5 - input.uv.y) * 2.0 * uView.w;
    float dx = 2.0 * uView.z / uScreen.x;
    float dy = 2.0 * uView.w / uScreen.y;

    const float4 kEmpty = float4(0.0, 0.0, 0.0, 1.0);
    float f0 = evalF(x, y);
    if (!isfinite(f0)) return kEmpty;
    float2 grad = 0.5 * float2(evalF(x + dx, y) - evalF(x - dx, y),
                               evalF(x, y + dy) - evalF(x, y - dy));  // per pixel
    float g = length(grad);
    if (!(g > 0.0)) return kEmpty;  // flat F (or F == 0 everywhere): no curve, as on the CPU
    float dist = abs(f0) / g;  // first-order distance to F = 0, in pixels
    if (dist >= 3.5) return kEmpty;

    if (f0 != 0.0) {
        // Step just past the estimated curve (and once more, further, where
        // the estimate falls short): a real stroke has a sign change there,
        // which bisection both verifies and measures.
        float2 toward = (f0 > 0.0 ? -grad : grad) / g;
        float2 pixelSize = float2(dx, dy);
        float2 p = float2(x, y);
        float measured = -1.0;
        [unroll]
        for (int probe = 0; probe < 2 && measured < 0.0; ++probe) {
            float2 q = p + toward * (probe == 0 ? dist + 0.75 : 3.5) * pixelSize;
            float fq = evalF(q.x, q.y);
            if (isfinite(fq) && ((fq > 0.0) != (f0 > 0.0) || fq == 0.0))
                measured = rootDistance(p, f0, q, fq, pixelSize);
        }
        if (measured < 0.0) return kEmpty;
        dist = measured;
    }
    float a = strokeAlpha(dist);
    return float4(a, a, a, 1.0);
}
)";
    return oss.str();
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
                              prev->type == ItemType::Fraction || prev->type == ItemType::Function);
            bool currIsVal = (it->type == ItemType::Number || it->type == ItemType::Variable ||
                              it->type == ItemType::Constant || it->type == ItemType::Paren ||
                              it->type == ItemType::Power || it->type == ItemType::Sqrt ||
                              it->type == ItemType::Fraction || it->type == ItemType::Function);
            if (prev->type == ItemType::Number && it->type == ItemType::Number) {
                return false;  // split digit run: leave it to the CPU evaluator
            }
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
            if (it->variableName != 'x' && it->variableName != 'y') return false;
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
            out += " gpu_sqrt(";
            if (!rowToHLSL(it->a.get(), out)) return false;
            out += ") ";
            return true;
        case ItemType::Paren:
            if (it->isBrace || it->isBracket) return false;
            out += " (";
            if (!rowToHLSL(it->a.get(), out)) return false;
            out += ") ";
            return true;
        case ItemType::Function: {
            std::string arg;
            if (rowIsEmpty(it->a.get()) || !rowToHLSL(it->a.get(), arg)) return false;
            arg = "(" + arg + ")";
            switch (it->functionId) {
                case SciSin: out += " sin(toRad" + arg + ") "; return true;
                case SciCos: out += " cos(toRad" + arg + ") "; return true;
                case SciTan: out += " tan(toRad" + arg + ") "; return true;
                case SciAsin: out += " asin" + arg + " "; return true;
                case SciAcos: out += " acos" + arg + " "; return true;
                case SciAtan: out += " atan" + arg + " "; return true;
                case SciSec: out += " (1.0 / cos(toRad" + arg + ")) "; return true;
                case SciCsc: out += " (1.0 / sin(toRad" + arg + ")) "; return true;
                case SciCot: out += " (1.0 / tan(toRad" + arg + ")) "; return true;
                case SciAsec: out += " acos(1.0 / " + arg + ") "; return true;
                case SciAcsc: out += " asin(1.0 / " + arg + ") "; return true;
                case SciAcot: out += " atan(1.0 / " + arg + ") "; return true;
                case SciSinh: out += " sinh" + arg + " "; return true;
                case SciCosh: out += " cosh" + arg + " "; return true;
                case SciTanh: out += " tanh" + arg + " "; return true;
                case SciAsinh: out += " gpu_asinh" + arg + " "; return true;
                case SciAcosh: out += " gpu_acosh" + arg + " "; return true;
                case SciAtanh: out += " gpu_atanh" + arg + " "; return true;
                case SciSech: out += " (1.0 / cosh" + arg + ") "; return true;
                case SciCsch: out += " (1.0 / sinh" + arg + ") "; return true;
                case SciCoth: out += " (1.0 / tanh" + arg + ") "; return true;
                case SciExp: out += " exp" + arg + " "; return true;
                case SciExpm1: out += " (exp" + arg + " - 1.0) "; return true;
                case SciLn: out += " log" + arg + " "; return true;
                case SciLog1p: out += " log(1.0 + " + arg + ") "; return true;
                case SciLog: out += " (log" + arg + " * 0.4342944819032518) "; return true;
                case SciLog2: out += " log2" + arg + " "; return true;
                case SciAbs: out += " abs" + arg + " "; return true;
                case SciFloor: out += " floor" + arg + " "; return true;
                case SciCeil: out += " ceil" + arg + " "; return true;
                case SciTrunc: out += " trunc" + arg + " "; return true;
                case SciSgn: out += " sign" + arg + " "; return true;
                case SciSinc: out += " gpu_sinc" + arg + " "; return true;
                case SciCbrt: out += " (sign" + arg + " * pow(abs" + arg + ", 1.0 / 3.0)) "; return true;
                case SciDeg: out += " degrees" + arg + " "; return true;
                case SciRad: out += " radians" + arg + " "; return true;
                default:
                    return false;  // discrete, multi-argument or special: FP64 path
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

    if (analysis.kind == GraphEquationKind::ExplicitY || analysis.kind == GraphEquationKind::ExplicitX) {
        std::string rightStr;
        const Row* right = analysis.rightRow ? analysis.rightRow.get() : analysis.leftRow.get();
        if (!rowToHLSL(right, rightStr)) return false;
        outExpr = "(" + rightStr + ")";
        return true;
    }
    if (analysis.kind == GraphEquationKind::ImplicitXY) {
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

void releaseTargets() {
    if (s_gpu.rtView) { s_gpu.rtView->Release(); s_gpu.rtView = nullptr; }
    if (s_gpu.rtTexture) { s_gpu.rtTexture->Release(); s_gpu.rtTexture = nullptr; }
    if (s_gpu.stagingTexture) { s_gpu.stagingTexture->Release(); s_gpu.stagingTexture = nullptr; }
    s_gpu.currentWidth = 0;
    s_gpu.currentHeight = 0;
}

bool resizeTargets(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (s_gpu.rtTexture && s_gpu.currentWidth == width && s_gpu.currentHeight == height) return true;
    releaseTargets();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(s_gpu.device->CreateTexture2D(&td, nullptr, &s_gpu.rtTexture))) return false;
    if (FAILED(s_gpu.device->CreateRenderTargetView(s_gpu.rtTexture, nullptr, &s_gpu.rtView))) return false;

    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(s_gpu.device->CreateTexture2D(&td, nullptr, &s_gpu.stagingTexture))) return false;

    s_gpu.currentWidth = width;
    s_gpu.currentHeight = height;
    return true;
}

ID3D11PixelShader* getOrCreatePixelShader(const std::string& mathExpr) {
    auto it = s_gpu.shaderCache.find(mathExpr);
    if (it != s_gpu.shaderCache.end()) return it->second;
    if (s_gpu.failedShaders.count(mathExpr)) return nullptr;

    std::string hlslSource = buildShaderSource(mathExpr);
    ID3DBlob* codeBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    // A new shader compiles on every edit of an implicit equation. Skipping
    // fxc's optimizer halves that latency; the driver optimizes the bytecode
    // anyway, so frame times are unchanged.
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL0 | D3DCOMPILE_SKIP_OPTIMIZATION;
    HRESULT hr = s_gpu.d3dCompile(hlslSource.c_str(), hlslSource.size(), nullptr, nullptr, nullptr,
                                  "PSMain", "ps_4_0", flags, 0, &codeBlob, &errorBlob);
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
        if (s_gpu.failedShaders.size() > 256) s_gpu.failedShaders.clear();
        s_gpu.failedShaders.insert(mathExpr);
        return nullptr;
    }

    ID3D11PixelShader* ps = nullptr;
    hr = s_gpu.device->CreatePixelShader(codeBlob->GetBufferPointer(), codeBlob->GetBufferSize(), nullptr, &ps);
    codeBlob->Release();
    if (errorBlob) errorBlob->Release();
    if (FAILED(hr) || !ps) return nullptr;

    if (s_gpu.shaderCache.size() > 64) {
        // Evict everything once the cache grows large
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
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &s_gpu.device, &featureLevel, &s_gpu.context);
    if (FAILED(hr)) {
        // Fallback to WARP (software rasterizer on GPU architecture)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &s_gpu.device, &featureLevel, &s_gpu.context);
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
    s_gpu.failedShaders.clear();
    releaseTargets();
    if (s_gpu.constantBuffer) { s_gpu.constantBuffer->Release(); s_gpu.constantBuffer = nullptr; }
    if (s_gpu.vsQuad) { s_gpu.vsQuad->Release(); s_gpu.vsQuad = nullptr; }
    if (s_gpu.context) { s_gpu.context->Release(); s_gpu.context = nullptr; }
    if (s_gpu.device) { s_gpu.device->Release(); s_gpu.device = nullptr; }
    if (s_gpu.compilerDll) { FreeLibrary(s_gpu.compilerDll); s_gpu.compilerDll = nullptr; }
    s_gpu.initialized = false;
    s_gpu.available = false;
}

bool gpuCanResolveView(const GraphView& view) {
    if (view.w <= 0 || view.h <= 0) return false;
    const double maxAbs = std::max({ std::fabs(view.minX), std::fabs(view.maxX),
                                     std::fabs(view.minY), std::fabs(view.maxY) });
    const double pixel = std::min((view.maxX - view.minX) / view.w, (view.maxY - view.minY) / view.h);
    const double floatUlp = 1.1920929e-7 * maxAbs;  // float32 spacing at the largest coordinate
    return std::isfinite(pixel) && pixel >= 256.0 * floatUlp;
}

bool renderImplicitCoverageGpu(const GraphAnalysis& analysis, const GraphView& view, CoverageBuffer& cov) {
    if (!analysis.isValid || analysis.kind != GraphEquationKind::ImplicitXY) return false;
    // WARP runs shaders on the CPU, where the FP64 rasterizer is both faster and exact.
    if (!isGpuGraphHardwareAdapter() || !gpuCanResolveView(view)) return false;
    if (cov.w != view.w || cov.h != view.h) return false;

    std::string expr;
    if (!analysisToHLSL(analysis, expr)) return false;
    ID3D11PixelShader* ps = getOrCreatePixelShader(expr);
    if (!ps || !resizeTargets(view.w, view.h)) return false;

    D3D11_MAPPED_SUBRESOURCE mappedCb;
    if (FAILED(s_gpu.context->Map(s_gpu.constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCb))) return false;
    ShaderConstantParams* params = static_cast<ShaderConstantParams*>(mappedCb.pData);
    // Center and half-size (not min/max) keep float32 coordinates well conditioned.
    params->view[0] = (float)(0.5 * (view.minX + view.maxX));
    params->view[1] = (float)(0.5 * (view.minY + view.maxY));
    params->view[2] = (float)(0.5 * (view.maxX - view.minX));
    params->view[3] = (float)(0.5 * (view.maxY - view.minY));
    params->screen[0] = (float)view.w;
    params->screen[1] = (float)view.h;
    params->screen[2] = 0.0f;
    params->screen[3] = 0.0f;
    s_gpu.context->Unmap(s_gpu.constantBuffer, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)view.w;
    vp.Height = (float)view.h;
    vp.MaxDepth = 1.0f;
    s_gpu.context->RSSetViewports(1, &vp);
    s_gpu.context->OMSetRenderTargets(1, &s_gpu.rtView, nullptr);
    s_gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    s_gpu.context->VSSetShader(s_gpu.vsQuad, nullptr, 0);
    s_gpu.context->PSSetShader(ps, nullptr, 0);
    s_gpu.context->PSSetConstantBuffers(0, 1, &s_gpu.constantBuffer);
    s_gpu.context->Draw(4, 0);
    s_gpu.context->CopyResource(s_gpu.stagingTexture, s_gpu.rtTexture);

    // Synchronous readback: a frame's coverage is always the current view's.
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(s_gpu.context->Map(s_gpu.stagingTexture, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < view.h; ++y) {
        const uint8_t* row = src + (size_t)y * mapped.RowPitch;
        float* dst = &cov.alpha[(size_t)y * cov.w];
        for (int x = 0; x < view.w; ++x) dst[x] = row[x] * (1.0f / 255.0f);
    }
    s_gpu.context->Unmap(s_gpu.stagingTexture, 0);
    return true;
}
