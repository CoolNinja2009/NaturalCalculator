// layout.h
//
// Recursive typesetter for the expression tree, using raw GDI (no
// RichEdit, no Direct2D/DirectWrite dependency -- keeps the binary small
// and startup instant). Every item reports (width, ascent, descent):
// if drawn with its baseline at Y, it spans [Y-ascent, Y+descent]
// vertically and `width` horizontally. A Row is just its children laid
// out left-to-right sharing one baseline, with row-ascent/descent = the
// max over children -- exactly like normal text layout, which is what
// lets fractions, exponents and roots interleave with plain numbers
// correctly.

#pragma once
#include <windows.h>
#include "expr_tree.h"

struct Size {
    int width = 0;
    int ascent = 0;
    int descent = 0;
    int height() const { return ascent + descent; }
};

struct Theme {
    COLORREF background;
    COLORREF panelBackground;
    COLORREF text;
    COLORREF operatorColor;
    COLORREF resultColor;
    COLORREF caret;
    COLORREF placeholder;
    COLORREF divider;
    COLORREF accent;
    bool isDark;

    // LCD-style screen colors (history + editor), styled after a classic
    // dot-matrix calculator display rather than a generic white panel.
    COLORREF screenBackground;
    COLORREF screenText;
    COLORREF screenOperator;
    COLORREF screenResult;
    COLORREF screenCaret;
    COLORREF screenPlaceholder;
};

Theme lightTheme();
Theme darkTheme();

// Caret pixel position/extent found during a draw pass, in the same
// coordinate space passed to drawExpression (i.e. relative to the origin
// given, not client-window-absolute unless origin==the window origin).
struct CaretInfo {
    bool valid = false;
    int x = 0, top = 0, bottom = 0;
};

// Measures the whole expression without drawing (used to size/scroll the
// editing area).
Size measureExpression(HDC hdc, const Row* root);

// Draws the whole expression with its top-left-ish anchor such that the
// root row's baseline is at (originX, baselineY). If `cursor` is
// non-null and matches a position visited during the draw, `outCaret` is
// filled in.
void drawExpression(HDC hdc, const Row* root, int originX, int baselineY,
                     const Theme& theme, const Cursor* cursor, CaretInfo* outCaret);

// Moves the expression cursor to the closest insertion point in the rendered
// tree. A click inside a number splits that digit run at the selected offset.
bool placeCursorAtPoint(Expression& expression, HDC hdc, int originX,
                        int baselineY, int pointX, int pointY);

// Font size (in logical points, negative = pixel height convention used
// by CreateFont) for a given nesting depth. Exposed for layout math in
// main.cpp (e.g. computing line heights for the workspace).
int fontHeightForDepth(int depth);

// Releases cached GDI font handles. Call once at process exit.
void shutdownFonts();
