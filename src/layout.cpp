// layout.cpp
#include "layout.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

Theme lightTheme() {
    Theme t;
    // Warm taupe "calculator body" instead of near-white, so the cream
    // keycaps actually read as physical buttons against it.
    t.background      = RGB(0xD6, 0xD2, 0xC8);
    t.panelBackground = RGB(0xEA, 0xE7, 0xDF);
    t.text             = RGB(0x1B, 0x1B, 0x1B);
    t.operatorColor    = RGB(0x00, 0x5A, 0x9E);
    t.resultColor      = RGB(0x00, 0x78, 0xD4);
    t.caret             = RGB(0x00, 0x5A, 0x9E);
    t.placeholder       = RGB(0xC6, 0xC6, 0xC6);
    t.divider           = RGB(0xBB, 0xB6, 0xAA);
    t.accent            = RGB(0xE0, 0x7A, 0x1E);
    t.isDark = false;

    // Classic dot-matrix LCD panel: sage-green background, near-black text.
    t.screenBackground  = RGB(0xC6, 0xD3, 0xB8);
    t.screenText        = RGB(0x23, 0x28, 0x1C);
    t.screenOperator    = RGB(0x1E, 0x3E, 0x27);
    t.screenResult      = RGB(0x0C, 0x5B, 0x2E);
    t.screenCaret        = RGB(0x23, 0x28, 0x1C);
    t.screenPlaceholder  = RGB(0x8E, 0x99, 0x80);
    return t;
}

Theme darkTheme() {
    Theme t;
    t.background      = RGB(0x1A, 0x1A, 0x1C);
    t.panelBackground = RGB(0x2B, 0x2B, 0x2B);
    t.text             = RGB(0xF0, 0xF0, 0xF0);
    t.operatorColor    = RGB(0x6C, 0xC2, 0xFF);
    t.resultColor      = RGB(0x60, 0xCD, 0xFF);
    t.caret             = RGB(0x6C, 0xC2, 0xFF);
    t.placeholder       = RGB(0x5A, 0x5A, 0x5A);
    t.divider           = RGB(0x3A, 0x3A, 0x3A);
    t.accent            = RGB(0xE0, 0x8A, 0x2E);
    t.isDark = true;

    // Dark mint-on-black LCD, reminiscent of an old backlit display.
    t.screenBackground  = RGB(0x10, 0x17, 0x12);
    t.screenText        = RGB(0xD6, 0xEE, 0xDA);
    t.screenOperator    = RGB(0x86, 0xE0, 0xA6);
    t.screenResult      = RGB(0x8F, 0xE3, 0xB0);
    t.screenCaret        = RGB(0xD6, 0xEE, 0xDA);
    t.screenPlaceholder  = RGB(0x4A, 0x5A, 0x4C);
    return t;
}

namespace {

constexpr int kBaseFontPx = 26;
constexpr double kShrink = 0.74;
constexpr int kMinFontPx = 10;
constexpr int kMaxDepth = 6;

std::unordered_map<int, HFONT> g_fontCache;

int fontPxForDepth(int depth) {
    if (depth > kMaxDepth) depth = kMaxDepth;
    double px = kBaseFontPx;
    for (int i = 0; i < depth; ++i) px *= kShrink;
    int ipx = (int)std::round(px);
    return std::max(ipx, kMinFontPx);
}

HFONT fontForDepth(int depth) {
    int px = fontPxForDepth(depth);
    auto it = g_fontCache.find(px);
    if (it != g_fontCache.end()) return it->second;
    HFONT f = CreateFontW(
        -px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontCache[px] = f;
    return f;
}

struct FontMetricsCache {
    TEXTMETRICW tm;
    bool have = false;
};
std::unordered_map<int, FontMetricsCache> g_tmCache;

TEXTMETRICW textMetricsForDepth(HDC hdc, int depth) {
    int px = fontPxForDepth(depth);
    auto& c = g_tmCache[px];
    if (!c.have) {
        HFONT f = fontForDepth(depth);
        HFONT old = (HFONT)SelectObject(hdc, f);
        GetTextMetricsW(hdc, &c.tm);
        SelectObject(hdc, old);
        c.have = true;
    }
    return c.tm;
}

int scaledPx(int basePx, int depth) {
    // Small structural paddings (gaps, bar thickness, paren glyph widths)
    // scale down gently with depth so nested structures don't look chunky.
    double factor = 1.0;
    for (int i = 0; i < depth && i < kMaxDepth; ++i) factor *= 0.85;
    int v = (int)std::round(basePx * factor);
    return std::max(v, 1);
}

// Forward decls (measure/draw are mutually recursive through Row<->Item).
Size measureRowImpl(HDC hdc, const Row* row, int depth);
Size measureItemImpl(HDC hdc, const Item* it, int depth);
void drawRowImpl(HDC hdc, const Row* row, int depth, int x, int baselineY,
                  const Theme& theme, const Cursor* cursor, CaretInfo* outCaret);
void drawItemImpl(HDC hdc, const Item* it, int depth, int x, int baselineY,
                   const Theme& theme, const Cursor* cursor, CaretInfo* outCaret);

Size placeholderSize(int depth) {
    // Approximate without an HDC: use fontPx directly for a stable box.
    int px = fontPxForDepth(depth);
    Size s;
    s.width = std::max(px / 2, 10);
    s.ascent = (int)(px * 0.72);
    s.descent = (int)(px * 0.22);
    return s;
}

Size measureNumber(HDC hdc, const Item* it, int depth) {
    HFONT f = fontForDepth(depth);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE sz{};
    std::wstring w(it->numText.begin(), it->numText.end());
    if (w.empty()) w = L" ";
    GetTextExtentPoint32W(hdc, w.c_str(), (int)w.size(), &sz);
    SelectObject(hdc, old);
    TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
    Size s;
    s.width = sz.cx;
    s.ascent = tm.tmAscent;
    s.descent = tm.tmDescent;
    return s;
}

Size measureVariable(HDC hdc, const Item* it, int depth) {
    Item display(ItemType::Number);
    display.numText = std::string(1, it->variableName);
    return measureNumber(hdc, &display, depth);
}

Size measureNameText(HDC hdc, const std::string& text, int depth) {
    HFONT f = fontForDepth(depth);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE sz{};
    std::wstring w(text.begin(), text.end());
    if (w.empty()) w = L"?";
    GetTextExtentPoint32W(hdc, w.c_str(), (int)w.size(), &sz);
    SelectObject(hdc, old);
    TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
    return { sz.cx, tm.tmAscent, tm.tmDescent };
}

std::wstring functionNameGlyph(const Item* it) {
    if (it->functionId == SciIntegrate) return L"\u222B";
    if (it->functionId == SciDiff) return L"d/dx";
    const char* name = sciFunctionName(it->functionId);
    return std::wstring(name, name + std::char_traits<char>::length(name));
}

Size measureFunctionName(HDC hdc, const Item* it, int depth) {
    std::wstring w = functionNameGlyph(it);
    HFONT f = fontForDepth(depth);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, w.c_str(), (int)w.size(), &sz);
    SelectObject(hdc, old);
    TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
    return { sz.cx, tm.tmAscent, tm.tmDescent };
}

const wchar_t* opGlyph(char c) {
    switch (c) {
        case '+': return L" + ";
        case '-': return L" \u2212 ";
        case '*': return L" \u00D7 ";
        case '%': return L" % ";
        case '!': return L"!";
        case ',': return L", ";
        case ';': return L"; ";
        case 'U': return L" U ";
        case 'I': return L" \u2229 ";
        case 'D': return L" \u0394 ";
        default: return L" ";
    }
}

Size measureOperator(HDC hdc, const Item* it, int depth) {
    HFONT f = fontForDepth(depth);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE sz{};
    const wchar_t* g = opGlyph(it->opChar);
    GetTextExtentPoint32W(hdc, g, (int)wcslen(g), &sz);
    SelectObject(hdc, old);
    TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
    Size s;
    s.width = sz.cx;
    s.ascent = tm.tmAscent;
    s.descent = tm.tmDescent;
    return s;
}

HFONT createParenFont(int depth, int innerHeight) {
    int height = std::max(fontPxForDepth(depth), innerHeight + scaledPx(6, depth));
    return CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

int measureParenWidth(HDC hdc, int depth, int innerHeight) {
    HFONT font = createParenFont(depth, innerHeight);
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE size{};
    GetTextExtentPoint32W(hdc, L"(", 1, &size);
    SelectObject(hdc, old);
    DeleteObject(font);
    return size.cx;
}

Size measureEquals(HDC hdc, int depth) {
    HFONT f = fontForDepth(depth);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE sz{};
    const wchar_t* glyph = L" = ";
    GetTextExtentPoint32W(hdc, glyph, 3, &sz);
    SelectObject(hdc, old);
    TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
    return { sz.cx, tm.tmAscent, tm.tmDescent };
}

Size measureRowImpl(HDC hdc, const Row* row, int depth) {
    if (rowIsEmpty(row)) return placeholderSize(depth);
    Size total;
    for (auto& it : row->items) {
        Size s = measureItemImpl(hdc, it.get(), depth);
        total.width += s.width;
        total.ascent = std::max(total.ascent, s.ascent);
        total.descent = std::max(total.descent, s.descent);
    }
    return total;
}

Size measureItemImpl(HDC hdc, const Item* it, int depth) {
    switch (it->type) {
        case ItemType::Number:   return measureNumber(hdc, it, depth);
        case ItemType::Variable: return measureVariable(hdc, it, depth);
        case ItemType::Equals:   return measureEquals(hdc, depth);
        case ItemType::CloseParen: {
            Item display(ItemType::Number);
            display.numText = ")";
            return measureNumber(hdc, &display, depth);
        }
        case ItemType::Operator: return measureOperator(hdc, it, depth);
        case ItemType::Fraction: {
            Size num = measureRowImpl(hdc, it->a.get(), depth + 1);
            Size den = measureRowImpl(hdc, it->b.get(), depth + 1);
            int gap = scaledPx(4, depth);
            int bar = scaledPx(2, depth);
            int hpad = scaledPx(6, depth);
            Size s;
            s.width = std::max(num.width, den.width) + 2 * hpad;
            s.ascent = num.height() + gap + bar;
            s.descent = den.height() + gap;
            return s;
        }
        case ItemType::Power: {
            // Power's "base" is a row (whatever atom the '^' key wrapped),
            // measured at the same depth; only the exponent shrinks.
            Size baseRow = measureRowImpl(hdc, it->a.get(), depth);
            Size exp = measureRowImpl(hdc, it->b.get(), depth + 1);
            int raise = (int)(baseRow.ascent * 0.55);
            Size s;
            s.width = baseRow.width + exp.width + scaledPx(2, depth);
            s.ascent = std::max(baseRow.ascent, raise + exp.height());
            s.descent = baseRow.descent;
            return s;
        }
        case ItemType::Paren: {
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            Size s;
            s.width = inner.width + 2 * glyphW;
            s.ascent = inner.ascent + scaledPx(2, depth);
            s.descent = inner.descent + scaledPx(2, depth);
            return s;
        }
        case ItemType::Sqrt: {
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int radicalW = scaledPx(16, depth);
            int roofPad = scaledPx(5, depth);
            Size s;
            s.width = inner.width + radicalW + scaledPx(4, depth);
            s.ascent = inner.ascent + roofPad;
            s.descent = inner.descent;
            return s;
        }
        case ItemType::Name:
            return measureNameText(hdc, it->nameText, depth);
        case ItemType::Constant: {
            Size s = measureNameText(hdc, "e", depth);
            if (it->constantName == 'p') {
                // pi renders as the Greek glyph; same metrics, wider glyph.
                HFONT f = fontForDepth(depth);
                HFONT old = (HFONT)SelectObject(hdc, f);
                SIZE sz{};
                GetTextExtentPoint32W(hdc, L"\u03C0", 1, &sz);
                SelectObject(hdc, old);
                s.width = sz.cx;
            } else if (it->constantName == 'f') {
                HFONT f = fontForDepth(depth);
                HFONT old = (HFONT)SelectObject(hdc, f);
                SIZE sz{};
                GetTextExtentPoint32W(hdc, L"\u03C6", 1, &sz);
                SelectObject(hdc, old);
                s.width = sz.cx;
            }
            return s;
        }
        case ItemType::Permutation:
        case ItemType::Combination: {
            Size nSize = measureRowImpl(hdc, it->a.get(), depth + 1);
            Size rSize = measureRowImpl(hdc, it->b.get(), depth + 1);
            const wchar_t* glyph = (it->type == ItemType::Permutation) ? L"P" : L"C";
            HFONT f = fontForDepth(depth);
            HFONT old = (HFONT)SelectObject(hdc, f);
            SIZE opSz{};
            GetTextExtentPoint32W(hdc, glyph, 1, &opSz);
            SelectObject(hdc, old);
            TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
            int raise = (int)(tm.tmAscent * 0.45);
            int drop = (int)(tm.tmDescent * 0.5) + scaledPx(3, depth);
            int pad = scaledPx(1, depth);
            Size s;
            s.width = nSize.width + pad + opSz.cx + pad + rSize.width;
            s.ascent = std::max((int)tm.tmAscent, raise + nSize.ascent);
            s.descent = std::max((int)tm.tmDescent, drop + rSize.descent);
            return s;
        }
        case ItemType::Function: {
            // "sin" + tall paren-wrapped argument, like a Paren with a name.
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            Size name = measureFunctionName(hdc, it, depth);
            int gap = scaledPx(2, depth);
            Size s;
            s.width = name.width + gap + inner.width + 2 * glyphW;
            s.ascent = std::max(inner.ascent + scaledPx(2, depth), name.ascent);
            s.descent = std::max(inner.descent + scaledPx(2, depth), name.descent);
            return s;
        }
        case ItemType::Integral: {
            Size integrand = measureRowImpl(hdc, it->a.get(), depth);
            Size lower = measureRowImpl(hdc, it->b.get(), depth + 1);
            Size upper = measureRowImpl(hdc, it->c.get(), depth + 1);
            TEXTMETRICW tm = textMetricsForDepth(hdc, depth);

            int intW = scaledPx(15, depth);
            int baseH = tm.tmAscent + tm.tmDescent;
            int ascent = std::max(integrand.ascent, upper.height() + scaledPx(4, depth));
            int descent = std::max(integrand.descent, lower.height() + scaledPx(4, depth));
            ascent = std::max(ascent, (int)(baseH * 1.05));
            descent = std::max(descent, (int)(baseH * 1.05));

            int limitsW = std::max(upper.width, lower.width);
            int pad = scaledPx(4, depth);

            std::wstring diffStr = L" d";
            diffStr.push_back(it->variableName ? (wchar_t)it->variableName : L'x');
            HFONT f = fontForDepth(depth);
            HFONT oldFont = (HFONT)SelectObject(hdc, f);
            SIZE diffSz{};
            GetTextExtentPoint32W(hdc, diffStr.c_str(), (int)diffStr.size(), &diffSz);
            SelectObject(hdc, oldFont);

            Size s;
            s.width = intW + limitsW + pad + integrand.width + scaledPx(6, depth) + diffSz.cx;
            s.ascent = ascent + scaledPx(2, depth);
            s.descent = descent + scaledPx(2, depth);
            return s;
        }
        case ItemType::Derivative: {
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            std::wstring dStr = L"d/d";
            dStr.push_back(it->variableName ? (wchar_t)it->variableName : L'x');
            HFONT f = fontForDepth(depth);
            HFONT oldFont = (HFONT)SelectObject(hdc, f);
            SIZE dSz{};
            GetTextExtentPoint32W(hdc, dStr.c_str(), (int)dStr.size(), &dSz);
            SelectObject(hdc, oldFont);

            int totalW = dSz.cx + scaledPx(2, depth) + inner.width + 2 * glyphW;
            Size evalPt = measureRowImpl(hdc, it->b.get(), depth + 1);
            std::wstring atStr = L"|_";
            atStr.push_back(it->variableName ? (wchar_t)it->variableName : L'x');
            atStr += L"=";
            oldFont = (HFONT)SelectObject(hdc, fontForDepth(depth + 1));
            SIZE atSz{};
            GetTextExtentPoint32W(hdc, atStr.c_str(), (int)atStr.size(), &atSz);
            SelectObject(hdc, oldFont);
            int barW = atSz.cx + evalPt.width + scaledPx(4, depth);
            totalW += barW;

            TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
            Size s;
            s.width = totalW;
            s.ascent = std::max(inner.ascent + scaledPx(2, depth), (int)tm.tmAscent);
            s.descent = std::max(inner.descent + scaledPx(2, depth), evalPt.height());
            return s;
        }
    }
    return Size{};
}

Size measureExpressionImpl(HDC hdc, const Row* root) { return measureRowImpl(hdc, root, 0); }

void maybeCaptureCaret(const Row* row, int index, int x, int baselineY,
                        int rowAscent, int rowDescent,
                        const Cursor* cursor, CaretInfo* outCaret) {
    if (!cursor || !outCaret || outCaret->valid) return;
    if (cursor->row == row && cursor->index == index) {
        outCaret->valid = true;
        outCaret->x = x;
        outCaret->top = baselineY - rowAscent;
        outCaret->bottom = baselineY + rowDescent;
    }
}

void drawRowImpl(HDC hdc, const Row* row, int depth, int x, int baselineY,
                  const Theme& theme, const Cursor* cursor, CaretInfo* outCaret) {
    if (rowIsEmpty(row)) {
        Size ph = placeholderSize(depth);
        HPEN pen = CreatePen(PS_DOT, 1, theme.placeholder);
        HPEN old = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, x, baselineY - ph.ascent, x + ph.width, baselineY + ph.descent);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, old);
        DeleteObject(pen);
        maybeCaptureCaret(row, 0, x, baselineY, ph.ascent, ph.descent, cursor, outCaret);
        return;
    }

    Size rowSize = measureRowImpl(hdc, row, depth);
    int curX = x;
    for (size_t i = 0; i < row->items.size(); ++i) {
        maybeCaptureCaret(row, (int)i, curX, baselineY, rowSize.ascent, rowSize.descent, cursor, outCaret);
        Item* it = row->items[i].get();
        Size s = measureItemImpl(hdc, it, depth);
        drawItemImpl(hdc, it, depth, curX, baselineY, theme, cursor, outCaret);
        curX += s.width;
    }
    maybeCaptureCaret(row, (int)row->items.size(), curX, baselineY, rowSize.ascent, rowSize.descent, cursor, outCaret);
}

void drawText(HDC hdc, int depth, int x, int baselineY, const std::wstring& w, COLORREF color) {
    HFONT f = fontForDepth(depth);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
    TextOutW(hdc, x, baselineY - tm.tmAscent, w.c_str(), (int)w.size());
    SelectObject(hdc, old);
}

void drawItemImpl(HDC hdc, const Item* it, int depth, int x, int baselineY,
                   const Theme& theme, const Cursor* cursor, CaretInfo* outCaret) {
    switch (it->type) {
        case ItemType::Number: {
            std::wstring w(it->numText.begin(), it->numText.end());
            drawText(hdc, depth, x, baselineY, w, theme.text);
            return;
        }
        case ItemType::Variable: {
            std::wstring w(1, (wchar_t)it->variableName);
            drawText(hdc, depth, x, baselineY, w, theme.operatorColor);
            return;
        }
        case ItemType::Equals:
            drawText(hdc, depth, x, baselineY, L" = ", theme.operatorColor);
            return;
        case ItemType::CloseParen:
            drawText(hdc, depth, x, baselineY, L")", theme.text);
            return;
        case ItemType::Operator: {
            drawText(hdc, depth, x, baselineY, opGlyph(it->opChar), theme.operatorColor);
            return;
        }
        case ItemType::Name: {
            std::wstring w(it->nameText.begin(), it->nameText.end());
            drawText(hdc, depth, x, baselineY, w.empty() ? std::wstring(L"?") : w, theme.text);
            return;
        }
        case ItemType::Constant:
            drawText(hdc, depth, x, baselineY,
                     it->constantName == 'p' ? L"\u03C0" : (it->constantName == 'f' ? L"\u03C6" : L"e"), theme.operatorColor);
            return;
        case ItemType::Permutation:
        case ItemType::Combination: {
            Size nSize = measureRowImpl(hdc, it->a.get(), depth + 1);
            const wchar_t* glyph = (it->type == ItemType::Permutation) ? L"P" : L"C";
            HFONT f = fontForDepth(depth);
            HFONT old = (HFONT)SelectObject(hdc, f);
            SIZE opSz{};
            GetTextExtentPoint32W(hdc, glyph, 1, &opSz);
            SelectObject(hdc, old);
            TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
            int raise = (int)(tm.tmAscent * 0.45);
            int drop = (int)(tm.tmDescent * 0.5) + scaledPx(3, depth);
            int pad = scaledPx(1, depth);

            // Draw superscript n
            drawRowImpl(hdc, it->a.get(), depth + 1, x, baselineY - raise, theme, cursor, outCaret);
            // Draw central P/C
            int opX = x + nSize.width + pad;
            drawText(hdc, depth, opX, baselineY, glyph, theme.operatorColor);
            // Draw subscript r
            int rX = opX + opSz.cx + pad;
            drawRowImpl(hdc, it->b.get(), depth + 1, rX, baselineY + drop, theme, cursor, outCaret);
            return;
        }
        case ItemType::Function: {
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            Size name = measureFunctionName(hdc, it, depth);
            int gap = scaledPx(2, depth);
            drawText(hdc, depth, x, baselineY, functionNameGlyph(it), theme.operatorColor);
            int px = x + name.width + gap;
            HFONT parenFont = createParenFont(depth, inner.height());
            HFONT oldFont = (HFONT)SelectObject(hdc, parenFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, theme.text);
            TEXTMETRICW metrics{};
            GetTextMetricsW(hdc, &metrics);
            TextOutW(hdc, px, baselineY - metrics.tmAscent, L"(", 1);
            drawRowImpl(hdc, it->a.get(), depth, px + glyphW, baselineY, theme, cursor, outCaret);
            TextOutW(hdc, px + glyphW + inner.width, baselineY - metrics.tmAscent, L")", 1);
            SelectObject(hdc, oldFont);
            DeleteObject(parenFont);
            return;
        }
        case ItemType::Fraction: {
            Size num = measureRowImpl(hdc, it->a.get(), depth + 1);
            Size den = measureRowImpl(hdc, it->b.get(), depth + 1);
            int gap = scaledPx(4, depth);
            int bar = scaledPx(2, depth);
            int hpad = scaledPx(6, depth);
            int w = std::max(num.width, den.width) + 2 * hpad;

            int numBaselineY = baselineY - gap - bar - num.descent;
            int denBaselineY = baselineY + gap + bar + den.ascent;
            int numX = x + hpad + (std::max(num.width, den.width) - num.width) / 2;
            int denX = x + hpad + (std::max(num.width, den.width) - den.width) / 2;

            drawRowImpl(hdc, it->a.get(), depth + 1, numX, numBaselineY, theme, cursor, outCaret);
            drawRowImpl(hdc, it->b.get(), depth + 1, denX, denBaselineY, theme, cursor, outCaret);

            HPEN pen = CreatePen(PS_SOLID, bar, theme.text);
            HPEN old = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, x + 1, baselineY, nullptr);
            LineTo(hdc, x + w - 1, baselineY);
            SelectObject(hdc, old);
            DeleteObject(pen);
            return;
        }
        case ItemType::Power: {
            Size baseRow = measureRowImpl(hdc, it->a.get(), depth);
            int raise = (int)(baseRow.ascent * 0.55);
            drawRowImpl(hdc, it->a.get(), depth, x, baselineY, theme, cursor, outCaret);
            int expX = x + baseRow.width + scaledPx(2, depth);
            int expBaselineY = baselineY - raise;
            drawRowImpl(hdc, it->b.get(), depth + 1, expX, expBaselineY, theme, cursor, outCaret);
            return;
        }
        case ItemType::Paren: {
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            HFONT parenFont = createParenFont(depth, inner.height());
            HFONT oldFont = (HFONT)SelectObject(hdc, parenFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, theme.text);
            TEXTMETRICW metrics{};
            GetTextMetricsW(hdc, &metrics);
            const wchar_t* openG = it->isBrace ? L"{" : (it->isBracket ? L"[" : L"(");
            const wchar_t* closeG = it->isBrace ? L"}" : (it->isBracket ? L"]" : L")");
            TextOutW(hdc, x, baselineY - metrics.tmAscent, openG, 1);
            drawRowImpl(hdc, it->a.get(), depth, x + glyphW, baselineY, theme, cursor, outCaret);
            TextOutW(hdc, x + glyphW + inner.width, baselineY - metrics.tmAscent, closeG, 1);
            SelectObject(hdc, oldFont);
            DeleteObject(parenFont);
            return;
        }
        case ItemType::Integral: {
            Size integrand = measureRowImpl(hdc, it->a.get(), depth);
            Size lower = measureRowImpl(hdc, it->b.get(), depth + 1);
            Size upper = measureRowImpl(hdc, it->c.get(), depth + 1);
            TEXTMETRICW tm = textMetricsForDepth(hdc, depth);

            int intW = scaledPx(15, depth);
            int baseH = tm.tmAscent + tm.tmDescent;
            int ascent = std::max(integrand.ascent, upper.height() + scaledPx(4, depth));
            int descent = std::max(integrand.descent, lower.height() + scaledPx(4, depth));
            ascent = std::max(ascent, (int)(baseH * 1.05));
            descent = std::max(descent, (int)(baseH * 1.05));

            int topY = baselineY - ascent;
            int bottomY = baselineY + descent;
            int limitsW = std::max(upper.width, lower.width);
            int pad = scaledPx(4, depth);

            // Draw smooth vector integral sign ∫ using PolyBezier
            int penW = std::max(2, scaledPx(2, depth));
            HPEN intPen = CreatePen(PS_SOLID, penW, theme.operatorColor);
            HPEN oldPen = (HPEN)SelectObject(hdc, intPen);

            int x0 = x + 1;
            int x1 = x + intW - 1;
            int xm = x + (intW * 46) / 100;
            int y0 = topY + 2;
            int y1 = bottomY - 2;
            int ym = baselineY;
            int hookR = std::min(13, (bottomY - topY) * 18 / 100);

            POINT pts[7] = {
                { (LONG)(x1 - 1), (LONG)(y0 + hookR) },
                { (LONG)x1, (LONG)y0 },
                { (LONG)(xm + (hookR * 45) / 100), (LONG)y0 },
                { (LONG)xm, (LONG)ym },
                { (LONG)(xm - (hookR * 45) / 100), (LONG)y1 },
                { (LONG)x0, (LONG)y1 },
                { (LONG)(x0 + 1), (LONG)(y1 - hookR) }
            };
            PolyBezier(hdc, pts, 7);
            SelectObject(hdc, oldPen);
            DeleteObject(intPen);

            // Upper limit: sits cleanly at top-right of the top hook
            int upperX = x + intW + scaledPx(2, depth);
            int upperY = topY + upper.ascent;
            drawRowImpl(hdc, it->c.get(), depth + 1, upperX, upperY, theme, cursor, outCaret);

            // Lower limit: sits cleanly at lower-right of the bottom hook
            int lowerX = x + intW + scaledPx(2, depth);
            int lowerY = bottomY - lower.descent;
            drawRowImpl(hdc, it->b.get(), depth + 1, lowerX, lowerY, theme, cursor, outCaret);

            // Integrand: sits on baseline to the right of limits
            int integrandX = x + intW + limitsW + pad;
            drawRowImpl(hdc, it->a.get(), depth, integrandX, baselineY, theme, cursor, outCaret);

            // Differential "dx"
            int diffX = integrandX + integrand.width + scaledPx(6, depth);
            std::wstring diffStr = L"d";
            diffStr.push_back(it->variableName ? (wchar_t)it->variableName : L'x');
            drawText(hdc, depth, diffX, baselineY, diffStr, theme.operatorColor);
            return;
        }
        case ItemType::Derivative: {
            std::wstring dStr = L"d/d";
            dStr.push_back(it->variableName ? (wchar_t)it->variableName : L'x');
            drawText(hdc, depth, x, baselineY, dStr, theme.operatorColor);
            HFONT f = fontForDepth(depth);
            HFONT oldFont = (HFONT)SelectObject(hdc, f);
            SIZE dSz{};
            GetTextExtentPoint32W(hdc, dStr.c_str(), (int)dStr.size(), &dSz);
            SelectObject(hdc, oldFont);

            int px = x + dSz.cx + scaledPx(2, depth);
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            HFONT parenFont = createParenFont(depth, inner.height());
            oldFont = (HFONT)SelectObject(hdc, parenFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, theme.text);
            TEXTMETRICW metrics{};
            GetTextMetricsW(hdc, &metrics);
            TextOutW(hdc, px, baselineY - metrics.tmAscent, L"(", 1);
            drawRowImpl(hdc, it->a.get(), depth, px + glyphW, baselineY, theme, cursor, outCaret);
            TextOutW(hdc, px + glyphW + inner.width, baselineY - metrics.tmAscent, L")", 1);
            SelectObject(hdc, oldFont);
            DeleteObject(parenFont);

            int barX = px + 2 * glyphW + inner.width + scaledPx(2, depth);
            std::wstring atStr = L"|_";
            atStr.push_back(it->variableName ? (wchar_t)it->variableName : L'x');
            atStr += L"=";
            drawText(hdc, depth + 1, barX, baselineY, atStr, theme.operatorColor);
            oldFont = (HFONT)SelectObject(hdc, fontForDepth(depth + 1));
            SIZE atSz{};
            GetTextExtentPoint32W(hdc, atStr.c_str(), (int)atStr.size(), &atSz);
            SelectObject(hdc, oldFont);
            int ptX = barX + atSz.cx + scaledPx(2, depth);
            drawRowImpl(hdc, it->b.get(), depth + 1, ptX, baselineY + scaledPx(2, depth), theme, cursor, outCaret);
            return;
        }
        case ItemType::Sqrt: {
            Size inner = measureRowImpl(hdc, it->a.get(), depth);
            int radicalW = scaledPx(16, depth);
            int roofPad = scaledPx(5, depth);
            int top = baselineY - inner.ascent - roofPad;
            int bot = baselineY + inner.descent;

            HPEN pen = CreatePen(PS_SOLID, std::max(1, scaledPx(2, depth) / 2), theme.text);
            HPEN old = (HPEN)SelectObject(hdc, pen);
            POINT pts[4] = {
                { x, top + (bot - top) * 2 / 3 },
                { x + radicalW / 3, bot },
                { x + radicalW, top },
                { x + radicalW + inner.width + scaledPx(4, depth), top }
            };
            Polyline(hdc, pts, 4);
            SelectObject(hdc, old);
            DeleteObject(pen);

            drawRowImpl(hdc, it->a.get(), depth, x + radicalW, baselineY, theme, cursor, outCaret);
            return;
        }
    }
}

struct HitCandidate {
    Row* row = nullptr;
    int index = 0;
    Item* number = nullptr;
    int numberOffset = 0;
    long long distance = std::numeric_limits<long long>::max();
};

void considerHit(HitCandidate& best, Row* row, int index, Item* number,
                 int numberOffset, int x, int y, int pointX, int pointY) {
    long long dx = (long long)pointX - x;
    long long dy = (long long)pointY - y;
    long long distance = dx * dx + 4 * dy * dy;
    if (distance < best.distance) {
        best = { row, index, number, numberOffset, distance };
    }
}

int numberPrefixWidth(HDC hdc, const Item* item, int depth, int count) {
    HFONT font = fontForDepth(depth);
    HFONT old = (HFONT)SelectObject(hdc, font);
    std::wstring prefix(item->numText.begin(), item->numText.begin() + count);
    SIZE size{};
    GetTextExtentPoint32W(hdc, prefix.c_str(), (int)prefix.size(), &size);
    SelectObject(hdc, old);
    return size.cx;
}

void findRowHit(HDC hdc, Row* row, int depth, int x, int baselineY,
                int pointX, int pointY, HitCandidate& best);

void findItemHit(HDC hdc, Row* parent, int index, Item* item, int depth,
                 int x, int baselineY, int pointX, int pointY,
                 HitCandidate& best) {
    Size size = measureItemImpl(hdc, item, depth);
    considerHit(best, parent, index, nullptr, 0, x, baselineY, pointX, pointY);
    considerHit(best, parent, index + 1, nullptr, 0, x + size.width, baselineY, pointX, pointY);
    switch (item->type) {
        case ItemType::Number:
            for (int offset = 0; offset <= (int)item->numText.size(); ++offset) {
                int caretX = x + numberPrefixWidth(hdc, item, depth, offset);
                if (offset == 0)
                    considerHit(best, parent, index, nullptr, 0, caretX, baselineY, pointX, pointY);
                else if (offset == (int)item->numText.size())
                    considerHit(best, parent, index + 1, nullptr, 0, caretX, baselineY, pointX, pointY);
                else
                    considerHit(best, parent, index, item, offset, caretX, baselineY, pointX, pointY);
            }
            return;
        case ItemType::Paren: {
            Size inner = measureRowImpl(hdc, item->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            findRowHit(hdc, item->a.get(), depth, x + glyphW, baselineY,
                       pointX, pointY, best);
            return;
        }
        case ItemType::Power: {
            Size base = measureRowImpl(hdc, item->a.get(), depth);
            int expX = x + base.width + scaledPx(2, depth);
            int expBaseline = baselineY - (int)(base.ascent * 0.55);
            findRowHit(hdc, item->a.get(), depth, x, baselineY,
                       pointX, pointY, best);
            findRowHit(hdc, item->b.get(), depth + 1, expX, expBaseline,
                       pointX, pointY, best);
            return;
        }
        case ItemType::Permutation:
        case ItemType::Combination: {
            Size nSize = measureRowImpl(hdc, item->a.get(), depth + 1);
            const wchar_t* glyph = (item->type == ItemType::Permutation) ? L"P" : L"C";
            HFONT f = fontForDepth(depth);
            HFONT old = (HFONT)SelectObject(hdc, f);
            SIZE opSz{};
            GetTextExtentPoint32W(hdc, glyph, 1, &opSz);
            SelectObject(hdc, old);
            TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
            int raise = (int)(tm.tmAscent * 0.45);
            int drop = (int)(tm.tmDescent * 0.5) + scaledPx(3, depth);
            int pad = scaledPx(1, depth);
            int opX = x + nSize.width + pad;
            int rX = opX + opSz.cx + pad;
            findRowHit(hdc, item->a.get(), depth + 1, x, baselineY - raise,
                       pointX, pointY, best);
            findRowHit(hdc, item->b.get(), depth + 1, rX, baselineY + drop,
                       pointX, pointY, best);
            return;
        }
        case ItemType::Fraction: {
            Size numerator = measureRowImpl(hdc, item->a.get(), depth + 1);
            Size denominator = measureRowImpl(hdc, item->b.get(), depth + 1);
            int gap = scaledPx(4, depth);
            int bar = scaledPx(2, depth);
            int hpad = scaledPx(6, depth);
            int maxWidth = std::max(numerator.width, denominator.width);
            int numeratorX = x + hpad + (maxWidth - numerator.width) / 2;
            int denominatorX = x + hpad + (maxWidth - denominator.width) / 2;
            int numeratorY = baselineY - gap - bar - numerator.descent;
            int denominatorY = baselineY + gap + bar + denominator.ascent;
            findRowHit(hdc, item->a.get(), depth + 1, numeratorX, numeratorY,
                       pointX, pointY, best);
            findRowHit(hdc, item->b.get(), depth + 1, denominatorX, denominatorY,
                       pointX, pointY, best);
            return;
        }
        case ItemType::Sqrt: {
            int radicalWidth = scaledPx(16, depth);
            findRowHit(hdc, item->a.get(), depth, x + radicalWidth, baselineY,
                       pointX, pointY, best);
            return;
        }
        case ItemType::Function: {
            Size inner = measureRowImpl(hdc, item->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            Size name = measureFunctionName(hdc, item, depth);
            int gap = scaledPx(2, depth);
            findRowHit(hdc, item->a.get(), depth, x + name.width + gap + glyphW,
                       baselineY, pointX, pointY, best);
            return;
        }
        case ItemType::Integral: {
            Size integrand = measureRowImpl(hdc, item->a.get(), depth);
            Size lower = measureRowImpl(hdc, item->b.get(), depth + 1);
            Size upper = measureRowImpl(hdc, item->c.get(), depth + 1);
            TEXTMETRICW tm = textMetricsForDepth(hdc, depth);

            int intW = scaledPx(15, depth);
            int baseH = tm.tmAscent + tm.tmDescent;
            int ascent = std::max(integrand.ascent, upper.height() + scaledPx(4, depth));
            int descent = std::max(integrand.descent, lower.height() + scaledPx(4, depth));
            ascent = std::max(ascent, (int)(baseH * 1.05));
            descent = std::max(descent, (int)(baseH * 1.05));

            int topY = baselineY - ascent;
            int bottomY = baselineY + descent;
            int limitsW = std::max(upper.width, lower.width);
            int pad = scaledPx(4, depth);

            int upperX = x + intW + scaledPx(2, depth);
            int upperY = topY + upper.ascent;
            int lowerX = x + intW + scaledPx(2, depth);
            int lowerY = bottomY - lower.descent;
            int integrandX = x + intW + limitsW + pad;

            findRowHit(hdc, item->c.get(), depth + 1, upperX, upperY, pointX, pointY, best);
            findRowHit(hdc, item->b.get(), depth + 1, lowerX, lowerY, pointX, pointY, best);
            findRowHit(hdc, item->a.get(), depth, integrandX, baselineY, pointX, pointY, best);
            return;
        }
        case ItemType::Derivative: {
            std::wstring dStr = L"d/d";
            dStr.push_back(item->variableName ? (wchar_t)item->variableName : L'x');
            HFONT f = fontForDepth(depth);
            HFONT oldFont = (HFONT)SelectObject(hdc, f);
            SIZE dSz{};
            GetTextExtentPoint32W(hdc, dStr.c_str(), (int)dStr.size(), &dSz);
            SelectObject(hdc, oldFont);

            int px = x + dSz.cx + scaledPx(2, depth);
            Size inner = measureRowImpl(hdc, item->a.get(), depth);
            int glyphW = measureParenWidth(hdc, depth, inner.height());
            findRowHit(hdc, item->a.get(), depth, px + glyphW, baselineY, pointX, pointY, best);

            int barX = px + 2 * glyphW + inner.width + scaledPx(2, depth);
            std::wstring atStr = L"|_";
            atStr.push_back(item->variableName ? (wchar_t)item->variableName : L'x');
            atStr += L"=";
            oldFont = (HFONT)SelectObject(hdc, fontForDepth(depth + 1));
            SIZE atSz{};
            GetTextExtentPoint32W(hdc, atStr.c_str(), (int)atStr.size(), &atSz);
            SelectObject(hdc, oldFont);
            int ptX = barX + atSz.cx + scaledPx(2, depth);
            findRowHit(hdc, item->b.get(), depth + 1, ptX, baselineY + scaledPx(2, depth), pointX, pointY, best);
            return;
        }
        case ItemType::Name:
        case ItemType::Constant:
        case ItemType::Variable:
        case ItemType::Operator:
        case ItemType::Equals:
        case ItemType::CloseParen:
            return;
    }
}

void findRowHit(HDC hdc, Row* row, int depth, int x, int baselineY,
                int pointX, int pointY, HitCandidate& best) {
    if (rowIsEmpty(row)) {
        considerHit(best, row, 0, nullptr, 0, x, baselineY, pointX, pointY);
        return;
    }
    int cursorX = x;
    for (size_t index = 0; index < row->items.size(); ++index) {
        Item* item = row->items[index].get();
        considerHit(best, row, (int)index, nullptr, 0, cursorX, baselineY, pointX, pointY);
        findItemHit(hdc, row, (int)index, item, depth, cursorX, baselineY,
                    pointX, pointY, best);
        cursorX += measureItemImpl(hdc, item, depth).width;
    }
    considerHit(best, row, (int)row->items.size(), nullptr, 0,
                cursorX, baselineY, pointX, pointY);
}

} // namespace

Size measureExpression(HDC hdc, const Row* root) { return measureExpressionImpl(hdc, root); }

void drawExpression(HDC hdc, const Row* root, int originX, int baselineY,
                     const Theme& theme, const Cursor* cursor, CaretInfo* outCaret) {
    if (outCaret) *outCaret = CaretInfo{};
    drawRowImpl(hdc, root, 0, originX, baselineY, theme, cursor, outCaret);
}

bool placeCursorAtPoint(Expression& expression, HDC hdc, int originX,
                        int baselineY, int pointX, int pointY) {
    HitCandidate hit;
    findRowHit(hdc, expression.root.get(), 0, originX, baselineY,
               pointX, pointY, hit);
    if (!hit.row) return false;
    if (hit.number && hit.numberOffset > 0 &&
        hit.numberOffset < (int)hit.number->numText.size()) {
        std::string left = hit.number->numText.substr(0, hit.numberOffset);
        std::string right = hit.number->numText.substr(hit.numberOffset);
        auto leftItem = std::make_unique<Item>(ItemType::Number);
        leftItem->numText = std::move(left);
        auto rightItem = std::make_unique<Item>(ItemType::Number);
        rightItem->numText = std::move(right);
        hit.row->items[hit.index] = std::move(leftItem);
        hit.row->items.insert(hit.row->items.begin() + hit.index + 1,
                              std::move(rightItem));
        expression.cursor.row = hit.row;
        expression.cursor.index = hit.index + 1;
    } else {
        expression.cursor.row = hit.row;
        expression.cursor.index = hit.index;
    }
    return true;
}

int fontHeightForDepth(int depth) { return fontPxForDepth(depth); }

void shutdownFonts() {
    for (auto& kv : g_fontCache) DeleteObject(kv.second);
    g_fontCache.clear();
    g_tmCache.clear();
}
