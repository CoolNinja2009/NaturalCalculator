// graph.cpp
//
// Live 2D interactive graphing engine for NaturalCalculator.
// Renders explicit functions y = f(x) and implicit 2D equations F(x, y) = 0
// with smooth curves, marching-squares contouring, auto-grid, pan, zoom,
// and hover coordinates.

#include "graph.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace {

std::string serializeRow(const Row* r) {
    if (!r) return "";
    return rowRangeToPlainString(r, 0, (int)r->items.size());
}

std::unique_ptr<Row> cloneRowSlice(const Row* sourceRow, size_t start, size_t end) {
    std::string text = rowRangeToPlainString(sourceRow, (int)start, (int)end);
    Expression expr;
    insertFromText(expr, text);
    return std::move(expr.root);
}

void collectVars(const Row* row, bool& hasX, bool& hasY) {
    if (!row) return;
    for (const auto& it : row->items) {
        if (it->type == ItemType::Variable) {
            if (it->variableName == 'x') hasX = true;
            if (it->variableName == 'y') hasY = true;
        }
        if (it->a) collectVars(it->a.get(), hasX, hasY);
        if (it->b) collectVars(it->b.get(), hasX, hasY);
    }
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
        // trim trailing zeros
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

} // namespace

double GraphAnalysis::evalExplicit(double x) const {
    if (!isValid) return NAN;
    EvaluationContext ctx = baseContext;
    ctx.degrees = false; // Cartesian mathematical graphing always operates in radians
    ctx.x = x;
    try {
        if (rightRow) return evaluate(rightRow.get(), ctx);
        if (leftRow) return evaluate(leftRow.get(), ctx);
    } catch (...) {}
    return NAN;
}

double GraphAnalysis::evalImplicit(double x, double y) const {
    if (!isValid) return NAN;
    EvaluationContext ctx = baseContext;
    ctx.degrees = false; // Cartesian mathematical graphing always operates in radians
    ctx.x = x;
    ctx.y = y;
    try {
        double l = leftRow ? evaluate(leftRow.get(), ctx) : 0.0;
        double r = rightRow ? evaluate(rightRow.get(), ctx) : 0.0;
        return l - r;
    } catch (...) {}
    return NAN;
}

GraphAnalysis analyzeGraphExpression(const Row* root, const EvaluationContext& context) {
    GraphAnalysis result;
    result.baseContext = context;
    if (!root || root->items.empty()) return result;

    // Check for equals
    size_t equalsPos = root->items.size();
    size_t equalsCount = 0;
    for (size_t i = 0; i < root->items.size(); ++i) {
        if (root->items[i]->type == ItemType::Equals) {
            equalsPos = i;
            equalsCount++;
        }
    }

    if (equalsCount > 1) return result;

    if (equalsCount == 1) {
        if (equalsPos == 0 || equalsPos + 1 >= root->items.size()) return result;

        result.leftRow = cloneRowSlice(root, 0, equalsPos);
        result.rightRow = cloneRowSlice(root, equalsPos + 1, root->items.size());

        bool lx = false, ly = false, rx = false, ry = false;
        collectVars(result.leftRow.get(), lx, ly);
        collectVars(result.rightRow.get(), rx, ry);

        bool leftIsOnlyY = (result.leftRow->items.size() == 1 &&
                            result.leftRow->items[0]->type == ItemType::Variable &&
                            result.leftRow->items[0]->variableName == 'y');
        bool rightIsOnlyY = (result.rightRow->items.size() == 1 &&
                             result.rightRow->items[0]->type == ItemType::Variable &&
                             result.rightRow->items[0]->variableName == 'y');

        if (leftIsOnlyY && !ry) {
            // y = f(x)
            result.kind = GraphEquationKind::ExplicitY;
            result.isValid = true;
            result.title = "y = " + serializeRow(result.rightRow.get());
            return result;
        }
        if (rightIsOnlyY && !ly) {
            // f(x) = y -> swap left and right so rightRow is f(x)
            result.kind = GraphEquationKind::ExplicitY;
            result.isValid = true;
            std::swap(result.leftRow, result.rightRow);
            result.title = "y = " + serializeRow(result.rightRow.get());
            return result;
        }

        // Implicit equation with x and/or y
        if (lx || ly || rx || ry) {
            result.kind = GraphEquationKind::ImplicitXY;
            result.isValid = true;
            result.title = serializeRow(result.leftRow.get()) + " = " + serializeRow(result.rightRow.get());
            return result;
        }
        return result;
    }

    // No equals: check variables in expression
    bool hasX = false, hasY = false;
    collectVars(root, hasX, hasY);

    if (hasX && !hasY) {
        // Single expression in x: interpreted as y = f(x)
        result.kind = GraphEquationKind::ExplicitY;
        result.isValid = true;
        result.rightRow = cloneRowSlice(root, 0, root->items.size());
        result.title = "y = " + serializeRow(result.rightRow.get());
        return result;
    }

    if (hasX && hasY) {
        // Implicit f(x, y) = 0
        result.kind = GraphEquationKind::ImplicitXY;
        result.isValid = true;
        result.leftRow = cloneRowSlice(root, 0, root->items.size());
        result.title = serializeRow(result.leftRow.get()) + " = 0";
        return result;
    }

    return result;
}

void GraphState::resetView(double range) {
    minX = -range;
    maxX = range;
    minY = -range;
    maxY = range;
    initialized = true;
}

void GraphState::zoom(double factor, double centerMathX, double centerMathY) {
    if (factor <= 0.0) return;
    double w = (maxX - minX) * factor;
    double h = (maxY - minY) * factor;
    double xRatio = (centerMathX - minX) / (maxX - minX);
    double yRatio = (centerMathY - minY) / (maxY - minY);
    minX = centerMathX - w * xRatio;
    maxX = minX + w;
    minY = centerMathY - h * yRatio;
    maxY = minY + h;
}

void GraphState::pan(double dxPixels, double dyPixels, int pixelWidth, int pixelHeight) {
    if (pixelWidth <= 0 || pixelHeight <= 0) return;
    double mathDx = (dxPixels / pixelWidth) * (maxX - minX);
    double mathDy = (dyPixels / pixelHeight) * (maxY - minY);
    minX -= mathDx;
    maxX -= mathDx;
    minY += mathDy;
    maxY += mathDy;
}

POINT GraphState::mathToPixel(double mx, double my, const RECT& rect) const {
    double w = rect.right - rect.left;
    double h = rect.bottom - rect.top;
    int px = rect.left + (int)std::lround((mx - minX) / (maxX - minX) * w);
    int py = rect.bottom - (int)std::lround((my - minY) / (maxY - minY) * h);
    return { px, py };
}

void GraphState::pixelToMath(int px, int py, const RECT& rect, double& mx, double& my) const {
    double w = rect.right - rect.left;
    double h = rect.bottom - rect.top;
    mx = minX + ((px - rect.left) / w) * (maxX - minX);
    my = minY + ((rect.bottom - py) / h) * (maxY - minY);
}

bool handleGraphMouseDown(GraphState& state, const RECT& graphRect, int x, int y) {
    POINT pt = { x, y };
    if (!PtInRect(&graphRect, pt)) return false;

    // Check zoom buttons in top-right of graphRect
    int btnSize = 24;
    int gap = 6;
    int bx = graphRect.right - btnSize - 10;
    int by = graphRect.top + 10;

    RECT resetBtn = { bx - 2 * (btnSize + gap), by, bx - 2 * (btnSize + gap) + btnSize * 2, by + btnSize };
    RECT zoomOutBtn = { bx - (btnSize + gap), by, bx - gap, by + btnSize };
    RECT zoomInBtn = { bx, by, bx + btnSize, by + btnSize };

    if (PtInRect(&zoomInBtn, pt)) {
        state.zoom(0.75, (state.minX + state.maxX) * 0.5, (state.minY + state.maxY) * 0.5);
        return true;
    }
    if (PtInRect(&zoomOutBtn, pt)) {
        state.zoom(1.333333, (state.minX + state.maxX) * 0.5, (state.minY + state.maxY) * 0.5);
        return true;
    }
    if (PtInRect(&resetBtn, pt)) {
        state.resetView(10.0);
        return true;
    }

    state.isDragging = true;
    state.lastMousePos = pt;
    return true;
}

bool handleGraphMouseMove(GraphState& state, const RECT& graphRect, int x, int y) {
    POINT pt = { x, y };
    if (state.isDragging) {
        int dx = x - state.lastMousePos.x;
        int dy = y - state.lastMousePos.y;
        state.pan(dx, dy, graphRect.right - graphRect.left, graphRect.bottom - graphRect.top);
        state.lastMousePos = pt;
        return true;
    }
    if (PtInRect(&graphRect, pt)) {
        state.isHovering = true;
        state.hoverPos = pt;
        state.pixelToMath(x, y, graphRect, state.hoverMathX, state.hoverMathY);
        return true;
    }
    if (state.isHovering) {
        state.isHovering = false;
        return true;
    }
    return false;
}

bool handleGraphMouseUp(GraphState& state) {
    if (state.isDragging) {
        state.isDragging = false;
        return true;
    }
    return false;
}

bool handleGraphMouseWheel(GraphState& state, const RECT& graphRect, int x, int y, short delta) {
    POINT pt = { x, y };
    if (!PtInRect(&graphRect, pt)) return false;
    double mx, my;
    state.pixelToMath(x, y, graphRect, mx, my);
    double factor = (delta > 0) ? 0.8 : 1.25;
    state.zoom(factor, mx, my);
    return true;
}

void renderGraph(HDC hdc, const RECT& graphRect, const GraphAnalysis& analysis,
                 GraphState& state, const Theme& theme, HFONT font, HFONT smallFont) {
    int w = graphRect.right - graphRect.left;
    int h = graphRect.bottom - graphRect.top;
    if (w <= 10 || h <= 10) return;

    if (!state.initialized) state.resetView(10.0);

    // 1. Double buffer memory DC
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
    RECT localRect = { 0, 0, w, h };

    // 2. Background
    COLORREF bg = theme.screenBackground;
    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(mem, &localRect, bgBrush);
    DeleteObject(bgBrush);

    // Graph colors based on theme
    COLORREF gridColor = theme.isDark ? RGB(0x28, 0x2D, 0x36) : RGB(0xEB, 0xEE, 0xF2);
    COLORREF axisColor = theme.isDark ? RGB(0x5A, 0x62, 0x70) : RGB(0x9E, 0xA8, 0xB6);
    COLORREF textColor = theme.isDark ? RGB(0x8C, 0x94, 0xA0) : RGB(0x73, 0x7D, 0x8C);
    COLORREF curveColor = theme.isDark ? RGB(0x00, 0xD4, 0xFF) : RGB(0x00, 0x66, 0xD6);
    if (theme.accent == RGB(0xD9, 0x10, 0x32) || theme.accent == RGB(0xDA, 0x0A, 0x31)) {
        // Pro Mode theme
        gridColor = RGB(0x2A, 0x0E, 0x16);
        axisColor = RGB(0x6D, 0x18, 0x2A);
        textColor = RGB(0xBB, 0x63, 0x76);
        curveColor = RGB(0xFF, 0x33, 0x55);
    }

    // 3. Grid lines & numeric ticks
    double xStep = chooseStep(state.maxX - state.minX, 8);
    double yStep = chooseStep(state.maxY - state.minY, 8);

    HPEN gridPen = CreatePen(PS_SOLID, 1, gridColor);
    HPEN oldPen = (HPEN)SelectObject(mem, gridPen);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, textColor);
    HFONT oldFont = (HFONT)SelectObject(mem, smallFont);

    // Vertical grid
    double startX = std::floor(state.minX / xStep) * xStep;
    for (double x = startX; x <= state.maxX + xStep * 0.5; x += xStep) {
        POINT pt = state.mathToPixel(x, 0.0, localRect);
        MoveToEx(mem, pt.x, 0, nullptr);
        LineTo(mem, pt.x, h);

        if (std::fabs(x) > 1e-9) {
            std::string label = formatNumberShort(x);
            POINT axisPt = state.mathToPixel(x, 0.0, localRect);
            int labelY = std::clamp((int)axisPt.y + 4, 18, h - 18);
            RECT lr = { pt.x - 30, labelY, pt.x + 30, labelY + 16 };
            DrawTextA(mem, label.c_str(), -1, &lr, DT_CENTER | DT_SINGLELINE);
        }
    }

    // Horizontal grid
    double startY = std::floor(state.minY / yStep) * yStep;
    for (double y = startY; y <= state.maxY + yStep * 0.5; y += yStep) {
        POINT pt = state.mathToPixel(0.0, y, localRect);
        MoveToEx(mem, 0, pt.y, nullptr);
        LineTo(mem, w, pt.y);

        if (std::fabs(y) > 1e-9) {
            std::string label = formatNumberShort(y);
            POINT axisPt = state.mathToPixel(0.0, y, localRect);
            int labelX = std::clamp((int)axisPt.x + 6, 6, w - 44);
            RECT lr = { labelX, pt.y - 8, labelX + 38, pt.y + 8 };
            DrawTextA(mem, label.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE);
        }
    }

    // 4. Main Axes
    HPEN axisPen = CreatePen(PS_SOLID, 2, axisColor);
    SelectObject(mem, axisPen);

    POINT originPt = state.mathToPixel(0.0, 0.0, localRect);
    // Y Axis (x = 0)
    if (originPt.x >= 0 && originPt.x <= w) {
        MoveToEx(mem, originPt.x, 0, nullptr);
        LineTo(mem, originPt.x, h);
    }
    // X Axis (y = 0)
    if (originPt.y >= 0 && originPt.y <= h) {
        MoveToEx(mem, 0, originPt.y, nullptr);
        LineTo(mem, w, originPt.y);
    }
    DeleteObject(axisPen);
    DeleteObject(gridPen);

    // 5. Draw Function / Curve
    if (analysis.isValid) {
        HPEN curvePen = CreatePen(PS_SOLID, 2, curveColor);
        SelectObject(mem, curvePen);

        if (analysis.kind == GraphEquationKind::ExplicitY) {
            bool inSegment = false;
            int lastPy = 0;
            double lastMathY = 0.0;

            for (int px = 0; px < w; ++px) {
                double mx, dummyMy;
                state.pixelToMath(px, 0, localRect, mx, dummyMy);
                double my = analysis.evalExplicit(mx);

                if (!std::isfinite(my) || std::isnan(my)) {
                    inSegment = false;
                    continue;
                }

                POINT pt = state.mathToPixel(mx, my, localRect);
                int py = pt.y;

                if (!inSegment) {
                    MoveToEx(mem, px, py, nullptr);
                    inSegment = true;
                } else {
                    // Check for vertical asymptote jump (e.g. tan(x), 1/x)
                    double mathDiff = std::fabs(my - lastMathY);
                    int pixelDiff = std::abs(py - lastPy);
                    if (pixelDiff > h * 0.75 && mathDiff > (state.maxY - state.minY) * 0.5) {
                        MoveToEx(mem, px, py, nullptr);
                    } else {
                        LineTo(mem, px, py);
                    }
                }
                lastPy = py;
                lastMathY = my;
            }
        } else if (analysis.kind == GraphEquationKind::ImplicitXY) {
            // Marching squares 2D contouring with dynamic resolution
            int gridCols = std::clamp(w / 5, 60, 160);
            int gridRows = std::clamp(h / 5, 60, 160);
            std::vector<std::vector<double>> values(gridRows + 1, std::vector<double>(gridCols + 1));

            for (int r = 0; r <= gridRows; ++r) {
                int py = (r * h) / gridRows;
                for (int c = 0; c <= gridCols; ++c) {
                    int px = (c * w) / gridCols;
                    double mx, my;
                    state.pixelToMath(px, py, localRect, mx, my);
                    values[r][c] = analysis.evalImplicit(mx, my);
                }
            }

            for (int r = 0; r < gridRows; ++r) {
                int py0 = (r * h) / gridRows;
                int py1 = ((r + 1) * h) / gridRows;
                for (int c = 0; c < gridCols; ++c) {
                    int px0 = (c * w) / gridCols;
                    int px1 = ((c + 1) * w) / gridCols;

                    double tl = values[r][c];
                    double tr = values[r][c + 1];
                    double bl = values[r + 1][c];
                    double br = values[r + 1][c + 1];

                    if (!std::isfinite(tl) || !std::isfinite(tr) ||
                        !std::isfinite(bl) || !std::isfinite(br)) continue;

                    int mask = 0;
                    if (tl > 0) mask |= 1;
                    if (tr > 0) mask |= 2;
                    if (br > 0) mask |= 4;
                    if (bl > 0) mask |= 8;

                    if (mask == 0 || mask == 15) continue;

                    auto interp = [](int p0, int p1, double v0, double v1) -> int {
                        double denom = v0 - v1;
                        if (std::fabs(denom) < 1e-12) return (p0 + p1) / 2;
                        double t = v0 / denom;
                        return p0 + (int)std::lround(t * (p1 - p0));
                    };

                    POINT topEdge = { interp(px0, px1, tl, tr), py0 };
                    POINT rightEdge = { px1, interp(py0, py1, tr, br) };
                    POINT bottomEdge = { interp(px0, px1, bl, br), py1 };
                    POINT leftEdge = { px0, interp(py0, py1, tl, bl) };

                    auto drawSeg = [&](POINT p1, POINT p2) {
                        MoveToEx(mem, p1.x, p1.y, nullptr);
                        LineTo(mem, p2.x, p2.y);
                    };

                    switch (mask) {
                        case 1: case 14: drawSeg(leftEdge, topEdge); break;
                        case 2: case 13: drawSeg(topEdge, rightEdge); break;
                        case 3: case 12: drawSeg(leftEdge, rightEdge); break;
                        case 4: case 11: drawSeg(rightEdge, bottomEdge); break;
                        case 5: drawSeg(leftEdge, topEdge); drawSeg(rightEdge, bottomEdge); break;
                        case 6: case 9: drawSeg(topEdge, bottomEdge); break;
                        case 7: case 8: drawSeg(leftEdge, bottomEdge); break;
                        case 10: drawSeg(leftEdge, bottomEdge); drawSeg(topEdge, rightEdge); break;
                    }
                }
            }
        }
        DeleteObject(curvePen);
    }

    // 6. Header / Formula badge & Controls
    {
        COLORREF headerBg = theme.isDark ? RGB(0x18, 0x1A, 0x20) : RGB(0xF3, 0xF5, 0xF8);
        HBRUSH hb = CreateSolidBrush(headerBg);
        RECT headerRect = { 0, 0, w, 34 };
        FillRect(mem, &headerRect, hb);
        DeleteObject(hb);

        HPEN hp = CreatePen(PS_SOLID, 1, axisColor);
        HPEN op = (HPEN)SelectObject(mem, hp);
        MoveToEx(mem, 0, 34, nullptr);
        LineTo(mem, w, 34);
        SelectObject(mem, op);
        DeleteObject(hp);

        // Title text
        SelectObject(mem, font);
        SetTextColor(mem, theme.screenText);
        std::string titleStr = analysis.isValid ? ("// " + analysis.title + " //") : "// LIVE GRAPH ENGINE //";
        RECT titleRect = { 12, 6, w - 110, 30 };
        DrawTextA(mem, titleStr.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Zoom / Reset buttons in header: sharp angular HUD buttons
        SelectObject(mem, smallFont);
        int btnW = 24, btnH = 22, btnY = 6;
        int bx = w - 10 - btnW;

        auto drawBtn = [&](int left, int width, const wchar_t* label) {
            RECT br = { left, btnY, left + width, btnY + btnH };
            COLORREF btnBg = theme.isDark ? RGB(0x1F, 0x0A, 0x0E) : RGB(0xE0, 0xE4, 0xEB);
            COLORREF borderCol = theme.isDark ? RGB(0xC0, 0x35, 0x18) : RGB(0xC0, 0xC5, 0xD0);
            HBRUSH btnBrush = CreateSolidBrush(btnBg);
            HPEN btnPen = CreatePen(PS_SOLID, 1, borderCol);
            HBRUSH oldB = (HBRUSH)SelectObject(mem, btnBrush);
            HPEN oldP = (HPEN)SelectObject(mem, btnPen);
            Rectangle(mem, br.left, br.top, br.right, br.bottom);
            SelectObject(mem, oldB);
            SelectObject(mem, oldP);
            DeleteObject(btnBrush);
            DeleteObject(btnPen);

            SetTextColor(mem, theme.isDark ? RGB(0xF5, 0xEB, 0xE1) : theme.text);
            DrawTextW(mem, label, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };

        drawBtn(bx, btnW, L"+");
        drawBtn(bx - btnW - 4, btnW, L"\u2212");
        drawBtn(bx - 2 * btnW - 48, 44, L"RESET");
    }

    // 7. Hover Tooltip Coordinates
    if (state.isHovering && analysis.isValid) {
        int hx = state.hoverPos.x - graphRect.left;
        int hy = state.hoverPos.y - graphRect.top;
        if (hx >= 0 && hx < w && hy >= 35 && hy < h) {
            double curY = analysis.kind == GraphEquationKind::ExplicitY ?
                          analysis.evalExplicit(state.hoverMathX) : state.hoverMathY;

            POINT trackPt = analysis.kind == GraphEquationKind::ExplicitY && std::isfinite(curY) ?
                            state.mathToPixel(state.hoverMathX, curY, localRect) : POINT{ hx, hy };

            // Indicator dot
            HBRUSH dotBrush = CreateSolidBrush(curveColor);
            HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            SelectObject(mem, dotBrush);
            SelectObject(mem, dotPen);
            Ellipse(mem, trackPt.x - 4, trackPt.y - 4, trackPt.x + 5, trackPt.y + 5);
            DeleteObject(dotPen);
            DeleteObject(dotBrush);

            // Coordinate label badge
            std::string coordStr = "(" + formatNumberShort(state.hoverMathX) + ", " +
                                   formatNumberShort(std::isfinite(curY) ? curY : state.hoverMathY) + ")";
            SelectObject(mem, smallFont);
            SIZE textSize{};
            GetTextExtentPoint32A(mem, coordStr.c_str(), (int)coordStr.size(), &textSize);

            int badgeX = std::clamp((int)trackPt.x + 8, 4, (int)(w - textSize.cx - 12));
            int badgeY = std::clamp((int)trackPt.y - 20, 38, (int)(h - textSize.cy - 6));
            RECT badgeRect = { badgeX, badgeY, badgeX + textSize.cx + 8, badgeY + textSize.cy + 4 };

            HBRUSH badgeBrush = CreateSolidBrush(theme.isDark ? RGB(0x10, 0x12, 0x16) : RGB(0x20, 0x24, 0x2A));
            FillRect(mem, &badgeRect, badgeBrush);
            DeleteObject(badgeBrush);

            SetTextColor(mem, RGB(255, 255, 255));
            RECT textR = { badgeX + 4, badgeY + 2, badgeX + textSize.cx + 4, badgeY + textSize.cy + 2 };
            DrawTextA(mem, coordStr.c_str(), -1, &textR, DT_LEFT | DT_SINGLELINE);
        }
    }

    // 8. Blit to target DC
    BitBlt(hdc, graphRect.left, graphRect.top, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldFont);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}
