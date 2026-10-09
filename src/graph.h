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

struct GraphAnalysis {
    GraphEquationKind kind = GraphEquationKind::None;
    bool isValid = false;
    std::string title;
    std::string description;
    // Shared cloned rows for thread-safe/reentrant evaluation
    std::unique_ptr<Row> leftRow;
    std::unique_ptr<Row> rightRow;
    EvaluationContext baseContext;

    double evalExplicit(double x) const;
    double evalExplicitX(double y) const;
    double evalImplicit(double x, double y) const;
};

// Analyzes a row (e.g. from current Expression or history) to determine
// if it is a graph-valid equation or expression.
GraphAnalysis analyzeGraphExpression(const Row* root, const EvaluationContext& context);

struct GraphState {
    double minX = -10.0;
    double maxX = 10.0;
    double minY = -10.0;
    double maxY = 10.0;
    bool initialized = false;

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

    void resetView(double range = 10.0);
    void zoom(double factor, double centerMathX, double centerMathY);
    void pan(double dxPixels, double dyPixels, int pixelWidth, int pixelHeight);

    POINT mathToPixel(double mx, double my, const RECT& rect) const;
    void pixelToMath(int px, int py, const RECT& rect, double& mx, double& my) const;
};

// Render graph onto hdc within graphRect.
void renderGraph(HDC hdc, const RECT& graphRect, const GraphAnalysis& analysis,
                 GraphState& state, const Theme& theme, HFONT font, HFONT smallFont);

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
