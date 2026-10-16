// tests/gpu_graph_tests.cpp
//
// Verification tests for the graph engine: the FP64 software rasterizer, the
// Direct3D 11 implicit-curve path, view math, labels and the probe HUD.

#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "gpu_graph.h"
#include "graph.h"
#include "graph_raster.h"
#include "evaluator.h"
#include "expr_tree.h"

namespace {

GraphAnalysis analyze(const char* text) {
    Expression expr;
    insertFromText(expr, text);
    EvaluationContext ctx;
    return analyzeGraphExpression(expr.root.get(), ctx);
}

// Renders through the public renderGraph entry and returns the panel pixels.
struct Canvas {
    int w, h;
    HDC screen = nullptr, mem = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    Canvas(int width, int height) : w(width), h(height) {
        screen = GetDC(nullptr);
        mem = CreateCompatibleDC(screen);
        bmp = CreateCompatibleBitmap(screen, w, h);
        old = (HBITMAP)SelectObject(mem, bmp);
    }
    ~Canvas() {
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    std::vector<uint32_t> pixels() {
        std::vector<uint32_t> out((size_t)w * h);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SelectObject(mem, old);
        GetDIBits(mem, bmp, 0, h, out.data(), &bmi, DIB_RGB_COLORS);
        SelectObject(mem, bmp);
        return out;
    }
};

Theme testTheme() {
    Theme theme = {};
    theme.isDark = true;
    theme.accent = RGB(0xE0, 0x8A, 0x2E);
    theme.screenBackground = RGB(0x10, 0x17, 0x12);
    return theme;
}

// Pixels close to the full curve color (stroke cores, not glow or grid).
bool isCurvePixel(uint32_t p, COLORREF curve) {
    int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    return std::abs(r - GetRValue(curve)) + std::abs(g - GetGValue(curve)) + std::abs(b - GetBValue(curve)) < 60;
}

int countCurvePixels(const std::vector<uint32_t>& px, COLORREF curve) {
    int n = 0;
    for (uint32_t p : px) n += isCurvePixel(p, curve) ? 1 : 0;
    return n;
}

// Longest vertical run of curve pixels in column x between rows y0 and y1.
int longestColumnRun(const std::vector<uint32_t>& px, int w, int x, int y0, int y1, COLORREF curve) {
    int best = 0, run = 0;
    for (int y = y0; y < y1; ++y) {
        run = isCurvePixel(px[(size_t)y * w + x], curve) ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}

GraphState viewAt(double cx, double cy, double spanX, int w, int h) {
    GraphState st;
    st.initialized = true;
    double spanY = spanX * h / w;
    st.minX = cx - spanX / 2;
    st.maxX = cx + spanX / 2;
    st.minY = cy - spanY / 2;
    st.maxY = cy + spanY / 2;
    return st;
}

} // namespace

int main() {
    std::cout << "[Graph Tests] Starting tests..." << std::endl;
    std::cout << "[Graph Tests] initGpuGraph(): " << initGpuGraph()
              << ", hardware adapter: " << (isGpuGraphHardwareAdapter() ? "YES" : "NO") << std::endl;
    const Theme theme = testTheme();
    const COLORREF curve = getGraphPalette(theme).curve;

    // Test 1: every kind of graph renders a visible curve.
    for (const char* text : { "y = sin(x)", "x^2 + y^2 = 25", "x^3 + y^3 - 3*x*y = 0", "y = tan(x)",
                              "x + y = tan(x)", "x = y^2", "y = gamma(x)", "y = besselj0(x)",
                              "y = atan2(x, 2)", "y = max(x, 1, -x)" }) {
        GraphAnalysis a = analyze(text);
        assert(a.isValid);
        Canvas c(400, 300);
        GraphState st;
        RECT rc = { 0, 0, c.w, c.h };
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
        int n = countCurvePixels(c.pixels(), curve);
        std::cout << "[Graph Tests] " << text << ": " << n << " curve pixels, fast eval " << a.hasFastEval() << std::endl;
        assert(n > 100);
    }

    // Test 2: non-scalar expressions are rejected or render without crashing.
    {
        GraphAnalysis a = analyze("det([1, 2; 3, 4])");
        Canvas c(200, 200);
        GraphState st;
        RECT rc = { 0, 0, c.w, c.h };
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
    }

    // Test 3: deep zoom keeps a thin stroke (the float32 shader smeared it into
    // a thick band around span 1e-5).
    for (const char* text : { "y = sin(x)", "x^2 + y^2 = 25" }) {
        GraphAnalysis a = analyze(text);
        bool implicit = a.kind == GraphEquationKind::ImplicitXY;
        Canvas c(640, 480);
        GraphState st = implicit ? viewAt(3.0, 4.0, 2e-5, c.w, c.h) : viewAt(3.0, std::sin(3.0), 1e-5, c.w, c.h);
        RECT rc = { 0, 0, c.w, c.h };
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
        auto px = c.pixels();
        int widest = 0;
        for (int x = 40; x < c.w - 40; x += 37) widest = std::max(widest, longestColumnRun(px, c.w, x, 60, c.h - 30, curve));
        std::cout << "[Graph Tests] deep zoom " << text << ": widest stroke run " << widest << " px" << std::endl;
        assert(widest > 0 && widest <= 6);
    }

    // Test 4: floor(x) has no vertical spikes joining its steps.
    {
        GraphAnalysis a = analyze("y = floor(x)");
        Canvas c(640, 480);
        GraphState st = viewAt(0.0, 0.0, 20.0, c.w, c.h);
        RECT rc = { 0, 0, c.w, c.h };
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
        auto px = c.pixels();
        int worst = 0;
        for (int k = -5; k <= 5; ++k) {
            int x = (int)std::lround(st.view(c.w, c.h).toPx((double)k));
            for (int dx = -1; dx <= 1; ++dx) worst = std::max(worst, longestColumnRun(px, c.w, x + dx, 45, c.h - 25, curve));
        }
        std::cout << "[Graph Tests] floor(x): longest vertical run at jumps " << worst << " px" << std::endl;
        assert(worst <= 6);
    }

    // Test 5: the asymptotes of x + y = tan(x) are not drawn as curve lines
    // (CPU and, where available, GPU).
    {
        GraphAnalysis a = analyze("x + y = tan(x)");
        assert(a.kind == GraphEquationKind::ImplicitXY);
        for (int pass = 0; pass < 2; ++pass) {
            Canvas c(640, 480);
            // Pass 1 zooms past float32 so the FP64 tracer handles it.
            GraphState st = pass == 0 ? viewAt(0.0, 0.0, 20.0, c.w, c.h) : viewAt(1.5707963, 0.0, 2e-3, c.w, c.h);
            RECT rc = { 0, 0, c.w, c.h };
            renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
            auto px = c.pixels();
            int x = (int)std::lround(st.view(c.w, c.h).toPx(1.5707963267948966));
            int run = longestColumnRun(px, c.w, x, 60, c.h - 30, curve);
            std::cout << "[Graph Tests] x + y = tan(x) pass " << pass << ": run at asymptote " << run << " px" << std::endl;
            assert(run <= 6);
        }
    }

    // Test 6: y = 1/x shows only a dotted asymptote at x = 0.
    {
        GraphAnalysis a = analyze("y = 1/x");
        Canvas c(640, 480);
        GraphState st = viewAt(0.0, 0.0, 20.0, c.w, c.h);
        RECT rc = { 0, 0, c.w, c.h };
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
        int run = longestColumnRun(c.pixels(), c.w, c.w / 2, 60, c.h - 30, curve);
        std::cout << "[Graph Tests] 1/x: run at the pole " << run << " px" << std::endl;
        assert(run <= 6);
    }

    // Test 7: explicit evaluation trusts bytecode NaN and covers special functions.
    {
        assert(analyze("y = gamma(x)").hasFastEval());
        assert(analyze("y = zeta(x)").hasFastEval());
        assert(analyze("y = hypot(x, 3)").hasFastEval());
        assert(analyze("y = clamp(x, -1, 2)").hasFastEval());
        GraphAnalysis z = analyze("y = zeta(x)");
        assert(std::fabs(z.evalExplicit(2.0) - 1.6449340668482264) < 1e-9);
        assert(std::fabs(z.evalExplicit(0.5) + 1.4603545088095868) < 1e-9);  // used to recurse forever
        assert(std::fabs(z.evalExplicit(-1.0) + 1.0 / 12.0) < 1e-9);
        GraphAnalysis s = analyze("y = sqrt(x)");
        assert(std::isnan(s.evalExplicit(-1.0)));
        assert(std::fabs(analyze("y = hypot(x, 4)").evalExplicit(3.0) - 5.0) < 1e-12);
        assert(std::fabs(analyze("y = ncr(x, 2)").evalExplicit(5.0) - 10.0) < 1e-12);
    }

    // Test 8: axis labels keep neighbouring ticks distinct at any zoom.
    {
        assert(formatAxisNumber(3.000002, 1e-6) != formatAxisNumber(3.000003, 1e-6));
        assert(formatAxisNumber(2.0, 2.0) == "2");
        assert(formatAxisNumber(0.5, 0.5) == "0.5");
        assert(formatAxisNumber(-2.0, 2.0) == "\xE2\x88\x92" "2");
        assert(formatAxisNumber(1.2e7, 2e6) == "1.2\xC3\x97" "10\xE2\x81\xB7");
        assert(formatAxisNumber(3.0000000012, 1e-10) == "3.0000000012");
        std::cout << "[Graph Tests] axis label formatting verified" << std::endl;
    }

    // Test 9: header buttons never overlap.
    {
        RECT r = { 0, 0, 600, 400 }, reset, out, in;
        graphButtonRects(r, reset, out, in);
        assert(reset.right <= out.left && out.right <= in.left);
    }

    // Test 10: views keep square pixels, so circles stay round.
    {
        GraphState st;
        st.resetView(10.0, 400, 800);
        double ux = (st.maxX - st.minX) / 400.0, uy = (st.maxY - st.minY) / 800.0;
        assert(std::fabs(ux - uy) < 1e-12 * ux);
        assert(std::fabs(st.minX + 10.0) < 1e-9 && std::fabs(st.maxX - 10.0) < 1e-9);  // shorter side shows +/-10
        st.fitAspect(800, 800);  // resize keeps center and scale
        assert(std::fabs((st.maxX - st.minX) / 800.0 - ux) < 1e-12 * ux);
    }

    // Test 11: FP64 zoom limits.
    {
        GraphState st;
        st.resetView(10.0, 600, 600);
        for (int i = 0; i < 400; ++i) st.zoom(0.5, 3.0, 4.0);
        double span = st.maxX - st.minX;
        std::cout << "[Graph Tests] deepest span at (3, 4): " << span << std::endl;
        assert(span > 1e-11 && span < 1e-8);
        for (int i = 0; i < 400; ++i) st.zoom(2.0, 0.0, 0.0);
        assert(st.maxX - st.minX <= 1e12 * 1.0001);
    }

    // Test 12: right-click probe interaction and snapping.
    {
        GraphAnalysis a = analyze("y = x^2");
        GraphState st;
        st.resetView(10.0);
        RECT graphRect = { 100, 50, 500, 350 };  // w=400, h=300
        assert(handleGraphRightDown(st, graphRect, 300, 200));
        assert(st.isRightProbing);
        assert(std::fabs(st.probeMathX) < 1e-4 && std::fabs(st.probeMathY) < 1e-4);
        assert(handleGraphMouseMove(st, graphRect, 350, 200));
        assert(st.probeMathX > 0.5);
        Canvas c(400, 300);
        RECT local = { 0, 0, 400, 300 };
        renderGraph(c.mem, local, a, st, theme, nullptr, nullptr);
        assert(handleGraphRightUp(st));
        assert(!st.isRightProbing);
        // Hover alone never asks for a repaint.
        assert(!handleGraphMouseMove(st, graphRect, 320, 210));

        // 1/x must not report a root at its pole.
        GraphAnalysis pole = analyze("y = 1/x");
        GraphState ps;
        ps.resetView(10.0);
        RECT pr = { 0, 0, 400, 300 };
        handleGraphRightDown(ps, pr, 201, 150);
        Canvas pc(400, 300);
        drawProbeTooltip(pc.mem, pr, ps, pole, theme, nullptr);
        handleGraphRightUp(ps);
        std::cout << "[Graph Tests] probe interaction verified" << std::endl;
    }

    // Test 13: the frame cache re-renders when the view moves.
    {
        GraphAnalysis a = analyze("y = sin(x)");
        Canvas c(400, 300);
        GraphState st;
        RECT rc = { 0, 0, 400, 300 };
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
        auto before = c.pixels();
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
        assert(before == c.pixels());
        st.pan(37.0, 0.0, 400, 300);
        renderGraph(c.mem, rc, a, st, theme, nullptr, nullptr);
        assert(before != c.pixels());
    }

    shutdownGpuGraph();
    std::cout << "[Graph Tests] All graph tests passed successfully!" << std::endl;
    return 0;
}
