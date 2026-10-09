#include <iostream>
#include <string>
std::string buildShaderSource(const std::string& mathExpr) {
    std::string s = R"(cbuffer Params : register(b0) { float4 uBounds; float4 uScreen; float4 uCurveCol; float4 uGridCol; float4 uAxisCol; float4 uBgCol; float4 uSteps; };
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float toRad(float a) { return uScreen.w > 0.5 ? radians(a) : a; }
float gpu_pow(float a, float b) { return pow(abs(a), b); }
float evalF(float x, float y) { return )" + mathExpr + R"(; }
float4 PSMain(VS_OUTPUT input) : SV_Target { return float4(1,1,1,1); }
)";
    return s;
}
int main() { std::cout << buildShaderSource("1 * i") << std::endl; }
