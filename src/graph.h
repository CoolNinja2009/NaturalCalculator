// graph.h
//
// Live 2D interactive graphing engine for NaturalCalculator.
// Renders explicit functions y = f(x) and implicit 2D equations F(x, y) = 0
// with smooth curves, marching-squares contouring, auto-grid, pan, zoom,
// and hover coordinates.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "expr_tree.h"
#include "evaluator.h"
#include "layout.h"

enum class GraphEquationKind {
    None,
    ExplicitY,    // y = f(x) or bare expression in x
    ExplicitX,    // x = f(y) e.g. x = y^2
    ImplicitXY    // F(x, y) = G(x, y) e.g. x^2 + y^2 = 25
};

enum class ByteOp : uint8_t {
    PushConst,
    PushVarX,
    PushVarY,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Pow,
    Sqrt,
    Cbrt,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Sec,
    Csc,
    Cot,
    Asec,
    Acsc,
    Acot,
    Sinh,
    Cosh,
    Tanh,
    Asinh,
    Acosh,
    Atanh,
    Sech,
    Csch,
    Coth,
    Asech,
    Acsch,
    Acoth,
    Exp,
    Expm1,
    Ln,
    Log10,
    Log2,
    Log1p,
    Abs,
    Floor,
    Ceil,
    Round,
    Trunc,
    Sgn,
    Fact,
    SciCall,    // any other one-argument scientific function; id taken from fnIds
    Zeta,
    Atan2,
    Hypot,
    Beta,
    Gcd,
    Lcm,
    Min,
    Max,
    Clamp,
    Lerp,
    Ncr,
    Npr
};

struct BytecodeProgram {
    std::vector<ByteOp> ops;
    std::vector<double> consts;
    std::vector<int> fnIds;     // operands of SciCall, consumed in order
    bool isValid = false;

    // Never throws: domain errors and undefined points evaluate to NaN.
    double execute(double x, double y) const;
};

// Compiles a Row AST into flat bytecode; returns true if fully compiled.
bool compileRowToBytecode(const Row* row, BytecodeProgram& outProg);

struct GraphAnalysis {
    GraphEquationKind kind = GraphEquationKind::None;
    bool isValid = false;
    std::string title;
    std::wstring displayTitle;  // natural-notation title shown in the graph header
    std::string description;
    // Cloned AST rows isolated from active editor state for reentrant evaluation
    std::unique_ptr<Row> leftRow;
    std::unique_ptr<Row> rightRow;
    EvaluationContext baseContext;

    // Compiled flat bytecode programs for sub-microsecond evaluation
    BytecodeProgram leftProgram;
    BytecodeProgram rightProgram;

    double evalExplicit(double x) const;
    double evalExplicitX(double y) const;
    double evalImplicit(double x, double y) const;

    // True when bytecode covers the whole equation, so evaluation is cheap,
    // thread-safe and exception-free (the renderer samples more densely then).
    bool hasFastEval() const;
};

struct GraphPalette {
    COLORREF bg;
    COLORREF grid;
    COLORREF axis;
    COLORREF text;
    COLORREF curve;
};

// Returns unified palette matching the active theme for both CPU and GPU render paths.
GraphPalette getGraphPalette(const Theme& theme);

// Shared numeric formatting and step-size calculation for coordinate grids.
double chooseStep(double range, int targetDivisions);
std::string formatNumberShort(double v);
std::string formatAxisNumber(double v, double step);

// Analyzes a row (e.g. from current Expression or history) to determine
// if it is a graph-valid equation or expression.
GraphAnalysis analyzeGraphExpression(const Row* root, const EvaluationContext& context);

// Double-precision mapping between math coordinates and a w x h pixel panel.
// Pixel (i, j) covers [i, i+1) x [j, j+1) with its center at (i + 0.5, j + 0.5);
// pixel y grows downward. All curve geometry is computed through this in
// double so lines stay crisp far beyond float32's zoom limits.
struct GraphView {
    double minX = -10.0, maxX = 10.0, minY = -10.0, maxY = 10.0;
    int w = 0, h = 0;

    double pxPerUnitX() const { return w / (maxX - minX); }
    double pxPerUnitY() const { return h / (maxY - minY); }
    double toPx(double x) const { return (x - minX) * pxPerUnitX(); }
    double toPy(double y) const { return (maxY - y) * pxPerUnitY(); }
    double fromPx(double px) const { return minX + px * ((maxX - minX) / w); }
    double fromPy(double py) const { return maxY - py * ((maxY - minY) / h); }
};

struct GraphState {
    double minX = -10.0;
    double maxX = 10.0;
    double minY = -10.0;
    double maxY = 10.0;
    bool initialized = false;
    // Panel size the bounds were last fitted to (0 until the first render).
    int pixelW = 0;
    int pixelH = 0;

    // Interaction state
    bool isDragging = false;
    POINT lastMousePos{ 0, 0 };
    POINT hoverPos{ 0, 0 };
    bool isHovering = false;
    double hoverMathX = 0.0;
    double hoverMathY = 0.0;

    // Right-click probe state
    bool isRightProbing = false;
    POINT probePos{ 0, 0 };
    double probeMathX = 0.0;
    double probeMathY = 0.0;

    // Shows [-range, range] along the panel's shorter side. With a panel size
    // the view gets square pixels immediately; otherwise on the next render.
    void resetView(double range = 10.0, int pxW = 0, int pxH = 0);
    // Keeps pixels square for a pxW x pxH panel: the first fit widens the
    // longer axis around the center; later resizes keep center and scale.
    void fitAspect(int pxW, int pxH);
    void zoom(double factor, double centerMathX, double centerMathY);
    void pan(double dxPixels, double dyPixels, int pixelWidth, int pixelHeight);
    GraphView view(int pxW, int pxH) const;

    POINT mathToPixel(double mx, double my, const RECT& rect) const;
    void pixelToMath(int px, int py, const RECT& rect, double& mx, double& my) const;
};

// Render graph onto hdc within graphRect: curves are drawn by the FP64
// software rasterizer, or by the D3D11 engine for implicit curves when
// float32 can resolve the current view.
void renderGraph(HDC hdc, const RECT& graphRect, const GraphAnalysis& analysis,
                 GraphState& state, const Theme& theme, HFONT font, HFONT smallFont);

// Header button hit boxes (Reset, zoom out, zoom in) for a graph panel rect.
void graphButtonRects(const RECT& graphRect, RECT& resetBtn, RECT& zoomOutBtn, RECT& zoomInBtn);

// Draw floating tooltip beside probed coordinate
void drawProbeTooltip(HDC hdc, const RECT& localRect, const GraphState& state,
                      const GraphAnalysis& analysis, const Theme& theme, HFONT font);

// Mouse interaction handlers
bool handleGraphMouseDown(GraphState& state, const RECT& graphRect, int x, int y);
bool handleGraphMouseMove(GraphState& state, const RECT& graphRect, int x, int y);
bool handleGraphMouseUp(GraphState& state);
bool handleGraphMouseWheel(GraphState& state, const RECT& graphRect, int x, int y, short delta);
bool handleGraphRightDown(GraphState& state, const RECT& graphRect, int x, int y);
bool handleGraphRightUp(GraphState& state);
