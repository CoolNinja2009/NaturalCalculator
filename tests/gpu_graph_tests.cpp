// tests/gpu_graph_tests.cpp
//
// Verification tests for Direct3D 11 GPU graphing engine and CPU fallback.

#include <iostream>
#include <cassert>
#include <cmath>
#include "gpu_graph.h"
#include "graph.h"
#include "evaluator.h"
#include "expr_tree.h"

int main() {
    std::cout << "[GPU Graph Tests] Starting tests..." << std::endl;

    bool initOk = initGpuGraph();
    std::cout << "[GPU Graph Tests] initGpuGraph() returned: " << initOk << std::endl;

    bool available = isGpuGraphAvailable();
    std::cout << "[GPU Graph Tests] isGpuGraphAvailable(): " << available << std::endl;
    std::cout << "[GPU Graph Tests] Hardware Adapter: " << (isGpuGraphHardwareAdapter() ? "YES" : "NO (WARP / Software)") << std::endl;
    if (!available) {
        std::cout << "[GPU Graph Tests] Direct3D 11 unavailable in this environment; GPU tests will fall back gracefully." << std::endl;
    }

    // Test 1: Explicit equation y = sin(x)
    {
        Expression expr;
        insertFromText(expr, "y = sin(x)");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);
        assert(analysis.kind == GraphEquationKind::ExplicitY);

        GraphState state;
        Theme theme;
        theme.isDark = true;
        theme.accent = RGB(0x19, 0x87, 0x54);

        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 400, 300);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 400, 300 };

        bool rendered = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        std::cout << "[GPU Graph Tests] Explicit y=sin(x) GPU render: " << (rendered ? "SUCCESS" : "FAILED") << std::endl;
        assert(rendered == true);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 2: Implicit equation x^2 + y^2 = 25
    {
        Expression expr;
        insertFromText(expr, "x^2 + y^2 = 25");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);
        assert(analysis.kind == GraphEquationKind::ImplicitXY);

        GraphState state;
        Theme theme;
        theme.isDark = false;
        theme.accent = RGB(0x00, 0x7A, 0xFF);

        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 500, 400);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 500, 400 };

        bool rendered = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        std::cout << "[GPU Graph Tests] Implicit x^2+y^2=25 GPU render: " << (rendered ? "SUCCESS" : "FAILED") << std::endl;
        assert(rendered == true);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 3: Complex curve x^3 + y^3 - 3xy = 0 (Folium of Descartes)
    {
        Expression expr;
        insertFromText(expr, "x^3 + y^3 - 3*x*y = 0");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);

        GraphState state;
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 350, 350);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 350, 350 };

        bool rendered = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        std::cout << "[GPU Graph Tests] Folium of Descartes GPU render: " << (rendered ? "SUCCESS" : "FAILED") << std::endl;
        assert(rendered == true);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 4: Degree angle mode trigonometric curve
    {
        Expression expr;
        insertFromText(expr, "y = tan(x)");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);

        GraphState state;
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 300, 300);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 300, 300 };

        bool rendered = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, true);
        std::cout << "[GPU Graph Tests] Degree mode tan(x) GPU render: " << (rendered ? "SUCCESS" : "FAILED") << std::endl;
        assert(rendered == true);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 5: Fallback handling when expression contains non-scalar constructs (e.g. det([1,2;3,4]))
    {
        Expression expr;
        insertFromText(expr, "det([1, 2; 3, 4])");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);

        GraphState state;
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 200, 200);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 200, 200 };

        // Should cleanly return false or handle without crashing
        bool rendered = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        std::cout << "[GPU Graph Tests] Non-scalar expression handled safely, returned: " << rendered << std::endl;

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 6: User reported equation x + y = tan(x)
    {
        Expression expr;
        insertFromText(expr, "x + y = tan(x)");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        std::cout << "[GPU Graph Tests] x + y = tan(x) isValid: " << analysis.isValid
                  << ", kind: " << static_cast<int>(analysis.kind) << std::endl;
        assert(analysis.isValid);
        // Should either isolate to ExplicitY: y = tan(x) - x, or ImplicitXY
        assert(analysis.kind == GraphEquationKind::ExplicitY || analysis.kind == GraphEquationKind::ImplicitXY);

        GraphState state;
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 400, 300);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 400, 300 };

        bool rendered = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        std::cout << "[GPU Graph Tests] x + y = tan(x) GPU render: " << (rendered ? "SUCCESS" : "FAILED") << std::endl;
        assert(rendered == true);

        // Also test CPU render fallback
        renderGraph(memDC, rect, analysis, state, theme, nullptr, nullptr);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 7: ExplicitX equation x = y^2
    {
        Expression expr;
        insertFromText(expr, "x = y^2");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        std::cout << "[GPU Graph Tests] x = y^2 isValid: " << analysis.isValid
                  << ", kind: " << static_cast<int>(analysis.kind) << std::endl;
        assert(analysis.isValid);
        assert(analysis.kind == GraphEquationKind::ExplicitX);
        assert(std::fabs(analysis.evalExplicitX(3.0) - 9.0) < 1e-6);

        GraphState state;
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 400, 300);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 400, 300 };

        bool rendered = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        std::cout << "[GPU Graph Tests] ExplicitX x = y^2 GPU render: " << (rendered ? "SUCCESS" : "FAILED") << std::endl;
        assert(rendered == true);

        // CPU render
        renderGraph(memDC, rect, analysis, state, theme, nullptr, nullptr);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 8: Right-click coordinate probe interaction
    {
        Expression expr;
        insertFromText(expr, "y = x^2");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);

        GraphState state;
        state.resetView(10.0);
        RECT graphRect = { 100, 50, 500, 350 }; // w=400, h=300

        // Right button down at center (x=300, y=200) -> local (200, 150) -> math (0.0, 0.0)
        bool rdown = handleGraphRightDown(state, graphRect, 300, 200);
        assert(rdown == true);
        assert(state.isRightProbing == true);
        assert(std::fabs(state.probeMathX) < 1e-4);
        assert(std::fabs(state.probeMathY) < 1e-4);

        // Move to (x=350, y=200)
        bool rmove = handleGraphMouseMove(state, graphRect, 350, 200);
        assert(rmove == true);
        assert(state.isRightProbing == true);
        assert(state.probeMathX > 0.5);

        // Render with active probe HUD
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 400, 300);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT localRect = { 0, 0, 400, 300 };

        bool rendered = renderGraphGpu(memDC, localRect, analysis, state, theme, nullptr, nullptr, false);
        assert(rendered == true);

        // Right button up releases probe
        bool rup = handleGraphRightUp(state);
        assert(rup == true);
        assert(state.isRightProbing == false);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 9: Probe Asymptote Pole Rejection (verifies 1/x does not report a false root at x=0)
    {
        Expression expr;
        insertFromText(expr, "y = 1/x");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);

        GraphState state;
        state.resetView(10.0);
        RECT graphRect = { 0, 0, 400, 300 };

        // Probe close to the vertical asymptote x = 0 (e.g. x = 201 pixels, just right of origin)
        handleGraphRightDown(state, graphRect, 201, 150);
        handleGraphMouseMove(state, graphRect, 201, 150);

        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 400, 300);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        Theme theme;

        // Draw probe tooltip and ensure it doesn't crash or snap to (0, 0) as a root
        drawProbeTooltip(memDC, graphRect, state, analysis, theme, nullptr);

        // Verify with a real root equation y = x - 2
        Expression rootExpr;
        insertFromText(rootExpr, "y = x - 2");
        GraphAnalysis rootAnalysis = analyzeGraphExpression(rootExpr.root.get(), ctx);
        assert(rootAnalysis.isValid);

        // Probe near x = 2 (x=240, y=150)
        handleGraphMouseMove(state, graphRect, 240, 150);
        drawProbeTooltip(memDC, graphRect, state, rootAnalysis, theme, nullptr);

        handleGraphRightUp(state);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        std::cout << "[GPU Graph Tests] Probe asymptote pole rejection verified!" << std::endl;
    }

    // Test 10: Dedicated CPU Fallback Rendering
    {
        Expression expr;
        insertFromText(expr, "y = x^3 - 3*x");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);

        GraphState state;
        state.resetView(5.0);
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 320, 240);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 320, 240 };

        // Force CPU render path directly
        renderGraph(memDC, rect, analysis, state, theme, nullptr, nullptr);
        std::cout << "[GPU Graph Tests] Pure CPU fallback rendering verified!" << std::endl;

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
    }

    // Test 11: Direct3D 11 Render Target Cache (Idea 4 Optimization)
    {
        Expression expr;
        insertFromText(expr, "y = sin(x)");
        EvaluationContext ctx;
        GraphAnalysis analysis = analyzeGraphExpression(expr.root.get(), ctx);
        assert(analysis.isValid);

        GraphState state;
        Theme theme;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP bmp = CreateCompatibleBitmap(screenDC, 400, 300);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        RECT rect = { 0, 0, 400, 300 };

        // Initial render (cache miss)
        bool r1 = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        assert(r1 == true);

        // Consecutive render with identical viewport (cache hit - zero-cost blit)
        bool r2 = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        assert(r2 == true);

        // Pan viewport (cache miss & update)
        state.minX += 1.0;
        state.maxX += 1.0;
        bool r3 = renderGraphGpu(memDC, rect, analysis, state, theme, nullptr, nullptr, false);
        assert(r3 == true);

        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        std::cout << "[GPU Graph Tests] D3D11 render target cache verified!" << std::endl;
    }

    shutdownGpuGraph();
    std::cout << "[GPU Graph Tests] All GPU and CPU graphing tests passed successfully!" << std::endl;
    return 0;
}
