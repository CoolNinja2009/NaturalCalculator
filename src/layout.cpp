// layout.cpp
#include "layout.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

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

static std::unordered_map<int, HFONT> g_parenFontCache;
static std::unordered_map<uint32_t, int> g_parenWidthCache;

HFONT createParenFont(int depth, int innerHeight) {
    int height = std::max(fontPxForDepth(depth), innerHeight + scaledPx(6, depth));
    auto it = g_parenFontCache.find(height);
    if (it != g_parenFontCache.end()) return it->second;
    HFONT f = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_parenFontCache[height] = f;
    return f;
}

int measureParenWidth(HDC hdc, int depth, int innerHeight) {
    int height = std::max(fontPxForDepth(depth), innerHeight + scaledPx(6, depth));
    uint32_t key = ((uint32_t)depth << 20) ^ (uint32_t)height;
    auto it = g_parenWidthCache.find(key);
    if (it != g_parenWidthCache.end()) return it->second;

    HFONT font = createParenFont(depth, innerHeight);
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE size{};
    GetTextExtentPoint32W(hdc, L"(", 1, &size);
    SelectObject(hdc, old);
    g_parenWidthCache[key] = size.cx;
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

// ---------------------------------------------------------------- natural function notation
//
// Scientific functions are typeset the way they are written on paper:
// sin⁻¹(x), log₂(x), |x|, ⌊x⌋, eˣ, ∛x, x!, Γ(x), ⁿCᵣ, u · v, ∫ₐᵇ f dx ...
// Only the layout changes: the tree, evaluation and plain-text serialization
// keep the function-call form. Multi-argument notations split the argument
// row at its top-level commas (the commas are hidden; the caret still steps
// over them) and fall back to name(args) until the argument count matches.

enum class FnStyle {
    Named,         // sin(x), Γ(x), gcd(a, b)
    Inverse,       // sin⁻¹(x)
    Subscripted,   // log₂(x), J₀(x)
    Abs,           // |x|
    Floor,         // ⌊x⌋
    Ceil,          // ⌈x⌉
    Round,         // ⌊x⌉
    Norm,          // ‖v‖
    Exp,           // eˣ
    Expm1,         // eˣ − 1
    Log1p,         // ln(1 + x)
    Cbrt,          // ∛x
    Factorial,     // x!
    InverseOf,     // A⁻¹
    Transpose,     // Aᵀ
    Infix,         // u · v, u × v
    Combinatoric,  // ⁿCᵣ, ⁿPᵣ
    Hypot,         // √(a² + b²)
    AngleScale,    // x · 180/π, x · π/180
    Membership,    // n ∈ ℙ
    Indexed,       // Iₙ, 0ₘₓₙ, Jₘₓₙ
    Integral,      // ∫ₐᵇ f dx
    Derivative,    // d/dx (f) |ₓ₌ₐ
    Limit,         // lim_{x→c} f
    BigOperator    // Σ, ∏ with limits
};

struct FnNotation {
    FnStyle style = FnStyle::Named;
    std::wstring name;    // name, base symbol or big-operator glyph
    std::wstring script;  // subscript, infix operator or root index
};

std::wstring widen(const char* s) {
    return std::wstring(s, s + std::char_traits<char>::length(s));
}

FnNotation fnNotation(int id) {
    switch (id) {
        case SciAsin: return { FnStyle::Inverse, L"sin", L"" };
        case SciAcos: return { FnStyle::Inverse, L"cos", L"" };
        case SciAtan: return { FnStyle::Inverse, L"tan", L"" };
        case SciAsec: return { FnStyle::Inverse, L"sec", L"" };
        case SciAcsc: return { FnStyle::Inverse, L"csc", L"" };
        case SciAcot: return { FnStyle::Inverse, L"cot", L"" };
        case SciAsinh: return { FnStyle::Inverse, L"sinh", L"" };
        case SciAcosh: return { FnStyle::Inverse, L"cosh", L"" };
        case SciAtanh: return { FnStyle::Inverse, L"tanh", L"" };
        case SciAsech: return { FnStyle::Inverse, L"sech", L"" };
        case SciAcsch: return { FnStyle::Inverse, L"csch", L"" };
        case SciAcoth: return { FnStyle::Inverse, L"coth", L"" };
        case SciLog2: return { FnStyle::Subscripted, L"log", L"2" };
        case SciBesselJ0: return { FnStyle::Subscripted, L"J", L"0" };
        case SciBesselJ1: return { FnStyle::Subscripted, L"J", L"1" };
        case SciBesselY0: return { FnStyle::Subscripted, L"Y", L"0" };
        case SciBesselY1: return { FnStyle::Subscripted, L"Y", L"1" };
        case SciAbs: return { FnStyle::Abs, L"", L"" };
        case SciFloor: return { FnStyle::Floor, L"", L"" };
        case SciCeil: return { FnStyle::Ceil, L"", L"" };
        case SciRound: return { FnStyle::Round, L"", L"" };
        case SciNorm: return { FnStyle::Norm, L"", L"" };
        case SciExp: return { FnStyle::Exp, L"e", L"" };
        case SciExpm1: return { FnStyle::Expm1, L"e", L"" };
        case SciLog1p: return { FnStyle::Log1p, L"ln", L"" };
        case SciCbrt: return { FnStyle::Cbrt, L"", L"3" };
        case SciFact: return { FnStyle::Factorial, L"", L"" };
        case SciInv: return { FnStyle::InverseOf, L"", L"" };
        case SciTranspose: return { FnStyle::Transpose, L"", L"" };
        case SciDot: return { FnStyle::Infix, L"", L" · " };
        case SciCross: return { FnStyle::Infix, L"", L" × " };
        case SciNcrFn: return { FnStyle::Combinatoric, L"", L"C" };
        case SciNprFn: return { FnStyle::Combinatoric, L"", L"P" };
        case SciHypot: return { FnStyle::Hypot, L"", L"" };
        case SciDeg: return { FnStyle::AngleScale, L"", L"" };
        case SciRad: return { FnStyle::AngleScale, L"", L"" };
        case SciIsPrime: return { FnStyle::Membership, L"", L"" };
        case SciEye: return { FnStyle::Indexed, L"I", L"" };
        case SciZeros: return { FnStyle::Indexed, L"0", L"" };
        case SciOnes: return { FnStyle::Indexed, L"J", L"" };
        case SciIntegrate: return { FnStyle::Integral, L"", L"" };
        case SciDiff: return { FnStyle::Derivative, L"", L"" };
        case SciLimit: return { FnStyle::Limit, L"lim", L"" };
        case SciSum: return { FnStyle::BigOperator, L"Σ", L"" };
        case SciProduct: return { FnStyle::BigOperator, L"∏", L"" };
        case SciGamma: return { FnStyle::Named, L"Γ", L"" };
        case SciLgamma: return { FnStyle::Named, L"ln Γ", L"" };
        case SciZeta: return { FnStyle::Named, L"ζ", L"" };
        case SciBeta: return { FnStyle::Named, L"B", L"" };
        case SciLambertW: return { FnStyle::Named, L"W", L"" };
        case SciStddev: return { FnStyle::Named, L"σ", L"" };
        case SciVar: return { FnStyle::Named, L"σ²", L"" };
        case SciTrace: return { FnStyle::Named, L"tr", L"" };
        default: return { FnStyle::Named, widen(sciFunctionName(id)), L"" };
    }
}

// Name shown as name(args) while a notation's argument count doesn't match yet.
std::wstring fnFallbackName(int id) {
    switch (id) {
        case SciIntegrate: return L"∫";
        case SciDiff: return L"d/dx";
        case SciSum: return L"Σ";
        case SciProduct: return L"∏";
        case SciLimit: return L"lim";
        case SciNcrFn: return L"nCr";
        case SciNprFn: return L"nPr";
        default: return widen(sciFunctionName(id));
    }
}

// A run of items in a row: the whole row (laid out through the cached row
// path) or one comma-separated argument of it.
struct RowRef {
    const Row* row = nullptr;
    size_t begin = 0;
    size_t end = 0;
    bool whole = true;
};

std::vector<RowRef> argumentSlices(const Row* row) {
    std::vector<RowRef> out;
    const size_t n = row ? row->items.size() : 0;
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        const Item* it = row->items[i].get();
        if (it->type == ItemType::Operator && it->opChar == ',') {
            out.push_back({ row, start, i, false });
            start = i + 1;
        }
    }
    out.push_back({ row, start, n, false });
    return out;
}

Size measureSliceImpl(HDC hdc, const RowRef& r, int depth) {
    if (r.whole) return measureRowImpl(hdc, r.row, depth);
    if (r.begin >= r.end) return placeholderSize(depth);
    Size total;
    for (size_t i = r.begin; i < r.end; ++i) {
        Size s = measureItemImpl(hdc, r.row->items[i].get(), depth);
        total.width += s.width;
        total.ascent = std::max(total.ascent, s.ascent);
        total.descent = std::max(total.descent, s.descent);
    }
    return total;
}

// The variable a slice consists of, e.g. the x in integrate(f, x, 0, 1).
char sliceVariable(const RowRef& r) {
    if (!r.row || r.end != r.begin + 1) return 0;
    const Item* it = r.row->items[r.begin].get();
    return it->type == ItemType::Variable ? it->variableName : 0;
}

// First variable of a slice: the integration / differentiation variable,
// found the same way the evaluator finds it.
wchar_t firstVariable(const RowRef& r) {
    for (size_t i = r.begin; r.row && i < r.end; ++i) {
        const Item* it = r.row->items[i].get();
        if (it->type == ItemType::Variable) return (wchar_t)it->variableName;
    }
    return L'x';
}

bool isDelimitedFunction(const Item* it) {
    switch (fnNotation(it->functionId).style) {
        case FnStyle::Named: case FnStyle::Inverse: case FnStyle::Subscripted:
        case FnStyle::Abs: case FnStyle::Floor: case FnStyle::Ceil: case FnStyle::Round:
        case FnStyle::Norm: case FnStyle::Log1p:
            return true;
        default:
            return false;
    }
}

// A + or - between terms at the top level of a slice (a leading sign
// doesn't count): such a body needs parentheses after lim or Σ.
bool hasTopLevelSum(const RowRef& r) {
    for (size_t i = r.begin + 1; r.row && i < r.end; ++i) {
        const Item* it = r.row->items[i].get();
        const Item* prev = r.row->items[i - 1].get();
        if (it->type == ItemType::Operator && (it->opChar == '+' || it->opChar == '-') &&
            prev->type != ItemType::Operator)
            return true;
    }
    return false;
}

// Operands of postfix notations (x!, A⁻¹, x · 180/π) need parentheses
// unless they are a single atom.
bool isAtomicOperand(const RowRef& r) {
    if (!r.row || r.end != r.begin + 1) return false;
    const Item* it = r.row->items[r.begin].get();
    switch (it->type) {
        case ItemType::Number: case ItemType::Variable: case ItemType::Constant:
        case ItemType::Paren: case ItemType::Sqrt:
            return true;
        case ItemType::Function:
            return isDelimitedFunction(it);
        default:
            return false;
    }
}

struct FnText {
    std::wstring text;
    int x = 0;
    int baseline = 0;     // offset from the item baseline (negative raises)
    int depth = 0;
    int parenHeight = 0;  // > 0: stretchy delimiter font for content this tall
    bool isOperator = true;
    int mathPx = 0;       // > 0: Cambria Math at this pixel size (big operators)
};

// Cambria Math ships with Windows and has properly drawn big operators.
std::unordered_map<int, HFONT> g_mathFontCache;

HFONT mathFontPx(int px) {
    auto it = g_mathFontCache.find(px);
    if (it != g_mathFontCache.end()) return it->second;
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Cambria Math");
    g_mathFontCache[px] = f;
    return f;
}

// Ink box of one glyph in the selected font, relative to its origin on the
// baseline: [left, left + width) x [top, top + height), top negative.
struct GlyphInk {
    int left = 0, top = 0, width = 0, height = 0;
    bool valid = false;
};

GlyphInk glyphInk(HDC hdc, HFONT font, wchar_t ch) {
    HFONT old = (HFONT)SelectObject(hdc, font);
    GLYPHMETRICS gm{};
    const MAT2 identity = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    DWORD r = GetGlyphOutlineW(hdc, ch, GGO_METRICS, &gm, 0, nullptr, &identity);
    SelectObject(hdc, old);
    GlyphInk ink;
    if (r == GDI_ERROR) return ink;
    ink.left = gm.gmptGlyphOrigin.x;
    ink.top = -gm.gmptGlyphOrigin.y;
    ink.width = (int)gm.gmBlackBoxX;
    ink.height = (int)gm.gmBlackBoxY;
    ink.valid = ink.height > 0;
    return ink;
}

enum class FnShapeKind { Bar, DoubleBar, FloorLeft, FloorRight, CeilLeft, CeilRight, Radical, Rule, IntegralSign };

struct FnShape {
    FnShapeKind kind = FnShapeKind::Bar;
    int x = 0;
    int top = 0;     // offsets from the item baseline
    int bottom = 0;
    int width = 0;   // delimiter cell, radical sign or rule width
    int extra = 0;   // radical: vinculum length; rule: thickness
    int depth = 0;
};

struct FnRowPart {
    RowRef ref;
    int x = 0;
    int baseline = 0;
    int depth = 0;
};

struct FnLayout {
    Size size;
    std::vector<FnText> texts;
    std::vector<FnShape> shapes;
    std::vector<FnRowPart> rows;
};

// Lays parts out left to right around one baseline. Groups built in a
// separate builder are merged at the pen, which is how delimiters, radicals
// and fractions wrap content whose size must be known first.
class FnBuilder {
public:
    FnBuilder(HDC hdc, int depth) : hdc_(hdc), depth_(depth) {}

    FnLayout take() {
        out_.size.width = std::max(out_.size.width, pen_);
        return std::move(out_);
    }
    int pen() const { return pen_; }
    void setPen(int x) { pen_ = x; }
    void gap(int px) { pen_ += px; }
    int ascent() const { return out_.size.ascent; }
    int descent() const { return out_.size.descent; }

    SIZE extent(const std::wstring& s, int depth) const {
        HFONT old = (HFONT)SelectObject(hdc_, fontForDepth(depth));
        SIZE sz{};
        GetTextExtentPoint32W(hdc_, s.c_str(), (int)s.size(), &sz);
        SelectObject(hdc_, old);
        return sz;
    }
    int raiseFor(int depth) const { return (int)(textMetricsForDepth(hdc_, depth).tmAscent * 0.45); }
    int dropFor(int depth) const {
        return (int)(textMetricsForDepth(hdc_, depth).tmDescent * 0.5) + scaledPx(3, depth);
    }

    // Text at the pen, `shift` pixels below the baseline (negative raises).
    void text(const std::wstring& s, bool isOperator = true, int depth = -1, int shift = 0) {
        if (depth < 0) depth = depth_;
        TEXTMETRICW tm = textMetricsForDepth(hdc_, depth);
        out_.texts.push_back({ s, pen_, shift, depth, 0, isOperator });
        cover(tm.tmAscent - shift, tm.tmDescent + shift);
        pen_ += extent(s, depth).cx;
    }
    void superscript(const std::wstring& s) { text(s, true, depth_ + 1, -raiseFor(depth_)); }
    void subscript(const std::wstring& s) { text(s, true, depth_ + 1, dropFor(depth_)); }

    void row(const RowRef& r, int depth = -1, int shift = 0) {
        if (depth < 0) depth = depth_;
        Size s = measureSliceImpl(hdc_, r, depth);
        out_.rows.push_back({ r, pen_, shift, depth });
        cover(s.ascent - shift, s.descent + shift);
        pen_ += s.width;
    }

    void group(const FnLayout& g, int shift = 0) {
        for (FnText t : g.texts) {
            t.x += pen_;
            t.baseline += shift;
            out_.texts.push_back(std::move(t));
        }
        for (FnShape s : g.shapes) {
            s.x += pen_;
            s.top += shift;
            s.bottom += shift;
            out_.shapes.push_back(s);
        }
        for (FnRowPart r : g.rows) {
            r.x += pen_;
            r.baseline += shift;
            out_.rows.push_back(r);
        }
        cover(g.size.ascent - shift, g.size.descent + shift);
        pen_ += g.size.width;
    }

    // Stretchy parentheses, exactly like the Paren item draws them.
    void parens(const FnLayout& inner) {
        const int h = inner.size.height();
        const int glyphW = measureParenWidth(hdc_, depth_, h);
        out_.texts.push_back({ L"(", pen_, 0, depth_, h, false });
        pen_ += glyphW;
        group(inner);
        out_.texts.push_back({ L")", pen_, 0, depth_, h, false });
        pen_ += glyphW;
        const int pad = scaledPx(2, depth_);
        cover(inner.size.ascent + pad, inner.size.descent + pad);
    }

    // Vector delimiters (|x|, ⌊x⌋, ‖v‖) that stretch with their contents.
    void delimited(FnShapeKind left, FnShapeKind right, const FnLayout& inner) {
        const int pad = scaledPx(2, depth_);
        const int top = -(inner.size.ascent + pad), bottom = inner.size.descent + pad;
        const int w = scaledPx(left == FnShapeKind::DoubleBar ? 10 : 8, depth_);
        out_.shapes.push_back({ left, pen_, top, bottom, w, 0, depth_ });
        pen_ += w;
        group(inner);
        out_.shapes.push_back({ right, pen_, top, bottom, w, 0, depth_ });
        pen_ += w;
        cover(-top, bottom);
    }

    // Radical sign over a group, with an optional root index in its crook.
    void radical(const FnLayout& inner, const std::wstring& index) {
        const int signW = scaledPx(16, depth_);
        const int top = -(inner.size.ascent + scaledPx(5, depth_));
        const int bottom = inner.size.descent;
        if (!index.empty()) {
            const int d = depth_ + 2;
            const int indexW = extent(index, d).cx;
            const int indexBaseline = top + (bottom - top) * 45 / 100;
            const int lead = std::max(0, indexW - signW / 2);
            out_.texts.push_back({ index, pen_ + std::max(0, signW / 2 - indexW) / 2, indexBaseline, d, 0, false });
            cover(-(indexBaseline - (int)textMetricsForDepth(hdc_, d).tmAscent), 0);
            pen_ += lead;
        }
        out_.shapes.push_back({ FnShapeKind::Radical, pen_, top, bottom, signW,
                                inner.size.width + scaledPx(4, depth_), depth_ });
        pen_ += signW;
        group(inner);
        pen_ += scaledPx(4, depth_);
        cover(-top, bottom);
    }

    // Stacked fraction with its bar on the baseline, like the Fraction item.
    void fraction(const FnLayout& num, const FnLayout& den) {
        const int gap = scaledPx(4, depth_), bar = scaledPx(2, depth_), hpad = scaledPx(4, depth_);
        const int inner = std::max(num.size.width, den.size.width);
        const int x0 = pen_;
        pen_ = x0 + hpad + (inner - num.size.width) / 2;
        group(num, -(gap + bar + num.size.descent));
        pen_ = x0 + hpad + (inner - den.size.width) / 2;
        group(den, gap + bar + den.size.ascent);
        out_.shapes.push_back({ FnShapeKind::Rule, x0, 0, 0, inner + 2 * hpad, bar, depth_ });
        pen_ = x0 + inner + 2 * hpad;
    }

    // Math axis (fraction-bar height) relative to the baseline; big
    // operators center on it like in print.
    int mathAxis() const { return -(int)(textMetricsForDepth(hdc_, depth_).tmAscent * 0.3); }

    // Big operator glyph (Σ, ∏) centered on the math axis, limits hung from
    // the glyph's ink box rather than its padded font cell.
    void bigOperator(const std::wstring& glyph, int contentHeight, const FnLayout& lower, const FnLayout& upper) {
        const int fontHeight = std::max(scaledPx(40, depth_), contentHeight + scaledPx(10, depth_));
        HFONT font = createParenFont(depth_, fontHeight);
        GlyphInk ink = glyphInk(hdc_, font, glyph[0]);
        if (!ink.valid) {
            HFONT old = (HFONT)SelectObject(hdc_, font);
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc_, &tm);
            SIZE sz{};
            GetTextExtentPoint32W(hdc_, glyph.c_str(), (int)glyph.size(), &sz);
            SelectObject(hdc_, old);
            ink = { 0, -(int)tm.tmAscent, (int)sz.cx, (int)tm.tmHeight, true };
        }
        const int shift = mathAxis() - (2 * ink.top + ink.height) / 2;  // glyph baseline offset
        const int inkTop = shift + ink.top, inkBottom = inkTop + ink.height;
        const int gap = scaledPx(3, depth_);
        const int centerW = std::max(ink.width, std::max(lower.size.width, upper.size.width));
        const int x0 = pen_;
        out_.texts.push_back({ glyph, x0 + (centerW - ink.width) / 2 - ink.left, shift, depth_, fontHeight, true });
        cover(-inkTop, inkBottom);
        pen_ = x0 + (centerW - upper.size.width) / 2;
        group(upper, inkTop - gap - upper.size.descent);
        pen_ = x0 + (centerW - lower.size.width) / 2;
        group(lower, inkBottom + gap + lower.size.ascent);
        pen_ = x0 + centerW + scaledPx(4, depth_);
    }

    // Integral sign (Cambria Math, scaled to span the integrand) with its
    // limits at the top and bottom right.
    void integralSign(const FnLayout& lower, const FnLayout& upper, const Size& integrand, bool limits) {
        const TEXTMETRICW tm = textMetricsForDepth(hdc_, depth_);
        const int baseH = tm.tmAscent + tm.tmDescent;
        int asc = std::max(integrand.ascent, (int)(baseH * 0.95));
        int desc = std::max(integrand.descent, (int)(baseH * 0.55));
        if (limits) {
            asc = std::max(asc, upper.size.height() + scaledPx(4, depth_));
            desc = std::max(desc, lower.size.height() + scaledPx(4, depth_));
        }
        const int height = asc + desc;
        const int x0 = pen_;
        int signW = scaledPx(15, depth_);
        // Size the glyph so its ink spans the integrand's full height.
        GlyphInk probe = glyphInk(hdc_, mathFontPx(100), L'∫');
        if (probe.valid) {
            const int px = std::max(8, (int)std::lround(100.0 * height / probe.height));
            GlyphInk ink = glyphInk(hdc_, mathFontPx(px), L'∫');
            if (ink.valid) {
                FnText sign{ L"∫", x0 + scaledPx(1, depth_) - ink.left, -asc - ink.top, depth_, 0, true };
                sign.mathPx = px;
                out_.texts.push_back(sign);
                signW = ink.width + scaledPx(2, depth_);
            }
        } else {
            out_.shapes.push_back({ FnShapeKind::IntegralSign, x0, -asc, desc, signW, 0, depth_ });
        }
        cover(asc + scaledPx(2, depth_), desc + scaledPx(2, depth_));
        int limitsW = 0;
        if (limits) {
            pen_ = x0 + signW + scaledPx(1, depth_);
            group(upper, -asc + upper.size.ascent);
            pen_ = x0 + signW - scaledPx(3, depth_);  // the lower hook curls left, leaving room
            group(lower, desc - lower.size.descent);
            limitsW = std::max(upper.size.width + scaledPx(1, depth_), lower.size.width - scaledPx(3, depth_));
        }
        pen_ = x0 + signW + std::max(limitsW, 0) + scaledPx(4, depth_);
    }

    void shape(const FnShape& s) {
        out_.shapes.push_back(s);
        cover(-s.top, s.bottom);
    }

private:
    void cover(int asc, int desc) {
        out_.size.ascent = std::max(out_.size.ascent, asc);
        out_.size.descent = std::max(out_.size.descent, desc);
    }

    HDC hdc_;
    int depth_;
    int pen_ = 0;
    FnLayout out_;
};

FnLayout layoutFunction(HDC hdc, const Item* it, int depth) {
    const FnNotation n = fnNotation(it->functionId);
    const Row* arg = it->a.get();
    const RowRef whole{ arg, 0, arg ? arg->items.size() : 0, true };
    const std::vector<RowRef> args = argumentSlices(arg);
    const int gap = scaledPx(2, depth);
    FnBuilder b(hdc, depth);

    auto groupOf = [&](const RowRef& r) {
        FnBuilder g(hdc, depth);
        g.row(r);
        return g.take();
    };
    auto named = [&](const std::wstring& name) {
        b.text(name);
        b.gap(gap);
        b.parens(groupOf(whole));
    };
    // Postfix notations parenthesize anything but a single atom.
    auto operand = [&](FnBuilder& into, const RowRef& r) {
        if (isAtomicOperand(r)) into.row(r);
        else into.parens(groupOf(r));
    };

    switch (n.style) {
        case FnStyle::Named:
            named(n.name);
            break;
        case FnStyle::Inverse:
            b.text(n.name);
            b.superscript(L"−" L"1");
            b.gap(gap);
            b.parens(groupOf(whole));
            break;
        case FnStyle::Subscripted:
            b.text(n.name);
            b.subscript(n.script);
            b.gap(gap);
            b.parens(groupOf(whole));
            break;
        case FnStyle::Abs:
            b.delimited(FnShapeKind::Bar, FnShapeKind::Bar, groupOf(whole));
            break;
        case FnStyle::Norm:
            b.delimited(FnShapeKind::DoubleBar, FnShapeKind::DoubleBar, groupOf(whole));
            break;
        case FnStyle::Floor:
            b.delimited(FnShapeKind::FloorLeft, FnShapeKind::FloorRight, groupOf(whole));
            break;
        case FnStyle::Ceil:
            b.delimited(FnShapeKind::CeilLeft, FnShapeKind::CeilRight, groupOf(whole));
            break;
        case FnStyle::Round:
            b.delimited(FnShapeKind::FloorLeft, FnShapeKind::CeilRight, groupOf(whole));
            break;
        case FnStyle::Exp:
        case FnStyle::Expm1:
            // e with the argument as its exponent, raised like a Power.
            b.text(n.name);
            b.gap(scaledPx(1, depth));
            b.row(whole, depth + 1, -(int)(textMetricsForDepth(hdc, depth).tmAscent * 0.55));
            if (n.style == FnStyle::Expm1) {
                b.text(L" − ");
                b.text(L"1", false);
            }
            break;
        case FnStyle::Log1p: {
            b.text(n.name);
            b.gap(gap);
            FnBuilder g(hdc, depth);
            g.text(L"1", false);
            g.text(L" + ");
            g.row(whole);
            b.parens(g.take());
            break;
        }
        case FnStyle::Cbrt:
            b.radical(groupOf(whole), n.script);
            break;
        case FnStyle::Factorial:
            operand(b, whole);
            b.text(L"!");
            break;
        case FnStyle::InverseOf:
            operand(b, whole);
            b.superscript(L"−" L"1");
            break;
        case FnStyle::Transpose:
            operand(b, whole);
            b.superscript(L"T");
            break;
        case FnStyle::Infix:
            if (args.size() != 2) {
                named(fnFallbackName(it->functionId));
                break;
            }
            b.row(args[0]);
            b.text(n.script);
            b.row(args[1]);
            break;
        case FnStyle::Combinatoric: {
            if (args.size() != 2) {
                named(fnFallbackName(it->functionId));
                break;
            }
            // Same geometry as the nPr / nCr structures.
            const TEXTMETRICW tm = textMetricsForDepth(hdc, depth);
            const int pad = scaledPx(1, depth);
            b.row(args[0], depth + 1, -(int)(tm.tmAscent * 0.45));
            b.gap(pad);
            b.text(n.script);
            b.gap(pad);
            b.row(args[1], depth + 1, (int)(tm.tmDescent * 0.5) + scaledPx(3, depth));
            break;
        }
        case FnStyle::Hypot: {
            if (args.size() != 2) {
                named(fnFallbackName(it->functionId));
                break;
            }
            FnBuilder g(hdc, depth);
            operand(g, args[0]);
            g.superscript(L"2");
            g.text(L" + ");
            operand(g, args[1]);
            g.superscript(L"2");
            b.radical(g.take(), L"");
            break;
        }
        case FnStyle::AngleScale: {
            // deg(x) = x · 180/π and rad(x) = x · π/180, exactly as computed
            // (rad() ignores the angle mode, so it is not written as x°).
            const bool toDegrees = it->functionId == SciDeg;
            operand(b, whole);
            b.text(L" · ");
            FnBuilder num(hdc, depth + 1), den(hdc, depth + 1);
            if (toDegrees) {
                num.text(L"180", false);
                den.text(L"π");
            } else {
                num.text(L"π");
                den.text(L"180", false);
            }
            b.fraction(num.take(), den.take());
            break;
        }
        case FnStyle::Membership:
            operand(b, whole);
            b.text(L" ∈ ℙ");  // n ∈ ℙ: GDI font fallback supplies both glyphs
            break;
        case FnStyle::Indexed: {
            if (args.size() > 2 || (args.size() == 2 && it->functionId == SciEye)) {
                named(fnFallbackName(it->functionId));
                break;
            }
            const int drop = b.dropFor(depth);
            b.text(n.name, it->functionId != SciZeros);
            b.row(args[0], depth + 1, drop);
            if (args.size() == 2) {
                b.text(L"×", true, depth + 1, drop);
                b.row(args[1], depth + 1, drop);
            }
            break;
        }
        case FnStyle::BigOperator: {
            // sum(f, a, b) / sum(f, var, a, b): Σ with "x = a" below, b above.
            if (args.size() != 3 && args.size() != 4) {
                named(fnFallbackName(it->functionId));
                break;
            }
            const bool namedVar = args.size() == 4;
            FnBuilder lower(hdc, depth + 1), upper(hdc, depth + 1);
            if (namedVar) lower.row(args[1]);
            else lower.text(L"x");
            lower.text(L"=");
            lower.row(args[namedVar ? 2 : 1]);
            upper.row(args[namedVar ? 3 : 2]);
            const Size body = measureSliceImpl(hdc, args[0], depth);
            b.bigOperator(n.name, body.height(), lower.take(), upper.take());
            if (hasTopLevelSum(args[0])) b.parens(groupOf(args[0]));
            else b.row(args[0]);
            break;
        }
        case FnStyle::Integral: {
            // integrate(f) | (f, b) | (f, a, b) | (f, var, b) | (f, var, a, b),
            // read the way the evaluator reads them.
            if (args.size() > 4) {
                named(fnFallbackName(it->functionId));
                break;
            }
            const wchar_t var = firstVariable(args[0]);
            FnBuilder lower(hdc, depth + 1), upper(hdc, depth + 1);
            const bool limits = args.size() >= 2;
            const bool secondIsVar = args.size() >= 2 && sliceVariable(args[1]) != 0;
            // A variable argument is drawn as the differential's variable, so
            // the caret can still reach it.
            const RowRef* varSlice = nullptr;
            if (args.size() == 2) {
                lower.text(L"0", false);
                upper.row(args[1]);
            } else if (args.size() == 3) {
                if (secondIsVar) {
                    varSlice = &args[1];
                    lower.text(L"0", false);
                } else {
                    lower.row(args[1]);
                }
                upper.row(args[2]);
            } else if (args.size() == 4) {
                varSlice = &args[1];
                lower.row(args[2]);
                upper.row(args[3]);
            }
            b.integralSign(lower.take(), upper.take(), measureSliceImpl(hdc, args[0], depth), limits);
            b.row(args[0]);
            b.gap(scaledPx(6, depth));
            b.text(L"d");
            if (varSlice) b.row(*varSlice);
            else b.text(std::wstring(1, args.size() == 2 && secondIsVar ? (wchar_t)sliceVariable(args[1]) : var));
            break;
        }
        case FnStyle::Derivative: {
            // diff(f) | (f, x0) | (f, var) | (f, var, x0)
            if (args.size() > 3) {
                named(fnFallbackName(it->functionId));
                break;
            }
            wchar_t var = firstVariable(args[0]);
            const RowRef* at = nullptr;
            const RowRef* varSlice = nullptr;  // drawn in d/d(var) so the caret reaches it
            if (args.size() == 2) {
                if (char v = sliceVariable(args[1])) {
                    var = (wchar_t)v;
                    varSlice = &args[1];
                } else {
                    at = &args[1];
                }
            } else if (args.size() == 3) {
                if (char v = sliceVariable(args[1])) var = (wchar_t)v;
                varSlice = &args[1];
                at = &args[2];
            }
            FnBuilder num(hdc, depth + 1), den(hdc, depth + 1);
            num.text(L"d");
            den.text(L"d");
            if (varSlice) den.row(*varSlice);
            else den.text(std::wstring(1, var));
            b.fraction(num.take(), den.take());
            b.gap(gap);
            b.parens(groupOf(args[0]));
            if (at) {
                // Evaluation bar |ₓ₌ₐ spanning everything before it.
                const int top = -b.ascent(), bottom = b.descent();
                b.shape({ FnShapeKind::Bar, b.pen(), top, bottom, scaledPx(6, depth), 0, depth });
                b.gap(scaledPx(6, depth));
                FnBuilder sub(hdc, depth + 1);
                sub.text(std::wstring(1, var) + L"=");
                sub.row(*at);
                FnLayout s = sub.take();
                b.group(s, bottom - s.size.descent);
            }
            break;
        }
        case FnStyle::Limit: {
            // limit(f, c) | (f, c, dir): "lim" over "x→c", then f.
            if (args.size() != 2 && args.size() != 3) {
                named(fnFallbackName(it->functionId));
                break;
            }
            FnBuilder under(hdc, depth + 1);
            under.text(L"x→");
            under.row(args[1]);
            if (args.size() == 3) under.row(args[2], depth + 2, -under.raiseFor(depth + 1));
            FnLayout approach = under.take();
            const int limW = b.extent(n.name, depth).cx;
            const int colW = std::max(limW, approach.size.width);
            const int x0 = b.pen();
            b.setPen(x0 + (colW - limW) / 2);
            b.text(n.name);
            b.setPen(x0 + (colW - approach.size.width) / 2);
            b.group(approach, (int)textMetricsForDepth(hdc, depth).tmDescent + approach.size.ascent);
            b.setPen(x0 + colW + 2 * gap);
            if (hasTopLevelSum(args[0])) b.parens(groupOf(args[0]));
            else b.row(args[0]);
            break;
        }
    }
    return b.take();
}

// The structural calculus items (∫, Σ, ∏, d/dx built from the keypad) share
// the builder, so they look exactly like their function-call forms. Their
// limit and evaluation-point rows always show, as placeholders when empty,
// so the caret can reach them.
FnLayout layoutStructure(HDC hdc, const Item* it, int depth) {
    auto whole = [](const Row* r) { return RowRef{ r, 0, r ? r->items.size() : 0, true }; };
    const RowRef body = whole(it->a.get()), lower = whole(it->b.get()), upper = whole(it->c.get());
    const wchar_t var = it->variableName ? (wchar_t)it->variableName : L'x';
    FnBuilder b(hdc, depth);
    switch (it->type) {
        case ItemType::Summation:
        case ItemType::Product: {
            FnBuilder lo(hdc, depth + 1), up(hdc, depth + 1);
            lo.row(lower);
            up.row(upper);
            b.bigOperator(it->type == ItemType::Summation ? L"Σ" : L"∏",
                          measureSliceImpl(hdc, body, depth).height(), lo.take(), up.take());
            b.row(body);
            break;
        }
        case ItemType::Integral: {
            FnBuilder lo(hdc, depth + 1), up(hdc, depth + 1);
            lo.row(lower);
            up.row(upper);
            b.integralSign(lo.take(), up.take(), measureSliceImpl(hdc, body, depth), true);
            b.row(body);
            b.gap(scaledPx(6, depth));
            b.text(std::wstring(L"d") + var);
            break;
        }
        case ItemType::Derivative: {
            FnBuilder num(hdc, depth + 1), den(hdc, depth + 1);
            num.text(L"d");
            den.text(std::wstring(L"d") + var);
            b.fraction(num.take(), den.take());
            b.gap(scaledPx(2, depth));
            FnBuilder inner(hdc, depth);
            inner.row(body);
            b.parens(inner.take());
            // Evaluation bar |ₓ₌ₐ with the point as a subscript.
            const int top = -b.ascent(), bottom = b.descent();
            b.shape({ FnShapeKind::Bar, b.pen(), top, bottom, scaledPx(6, depth), 0, depth });
            b.gap(scaledPx(6, depth));
            FnBuilder at(hdc, depth + 1);
            at.text(std::wstring(1, var) + L"=");
            at.row(lower);
            FnLayout s = at.take();
            b.group(s, bottom - s.size.descent);
            break;
        }
        default:
            break;
    }
    return b.take();
}

Size measureRowImpl(HDC hdc, const Row* row, int depth) {
    if (rowIsEmpty(row)) return placeholderSize(depth);
    if (!row->layoutDirty && row->cachedDepth == depth) {
        return { row->cachedWidth, row->cachedAscent, row->cachedDescent };
    }
    Size total;
    for (auto& it : row->items) {
        Size s = measureItemImpl(hdc, it.get(), depth);
        total.width += s.width;
        total.ascent = std::max(total.ascent, s.ascent);
        total.descent = std::max(total.descent, s.descent);
    }
    row->cachedWidth = total.width;
    row->cachedAscent = total.ascent;
    row->cachedDescent = total.descent;
    row->cachedDepth = depth;
    row->layoutDirty = false;
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
        case ItemType::Function:
            return layoutFunction(hdc, it, depth).size;
        case ItemType::Summation:
        case ItemType::Product:
        case ItemType::Integral:
        case ItemType::Derivative:
            return layoutStructure(hdc, it, depth).size;
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

// Draws one argument slice of a function's row (or the whole row), with
// caret positions for every gap in it, including the hidden commas.
void drawSliceImpl(HDC hdc, const RowRef& r, int depth, int x, int baselineY,
                   const Theme& theme, const Cursor* cursor, CaretInfo* outCaret) {
    if (r.whole) {
        drawRowImpl(hdc, r.row, depth, x, baselineY, theme, cursor, outCaret);
        return;
    }
    if (r.begin >= r.end) {
        Size ph = placeholderSize(depth);
        HPEN pen = CreatePen(PS_DOT, 1, theme.placeholder);
        HPEN old = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, x, baselineY - ph.ascent, x + ph.width, baselineY + ph.descent);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, old);
        DeleteObject(pen);
        maybeCaptureCaret(r.row, (int)r.begin, x, baselineY, ph.ascent, ph.descent, cursor, outCaret);
        return;
    }
    Size size = measureSliceImpl(hdc, r, depth);
    int curX = x;
    for (size_t i = r.begin; i < r.end; ++i) {
        maybeCaptureCaret(r.row, (int)i, curX, baselineY, size.ascent, size.descent, cursor, outCaret);
        const Item* item = r.row->items[i].get();
        drawItemImpl(hdc, item, depth, curX, baselineY, theme, cursor, outCaret);
        curX += measureItemImpl(hdc, item, depth).width;
    }
    maybeCaptureCaret(r.row, (int)r.end, curX, baselineY, size.ascent, size.descent, cursor, outCaret);
}

void drawFnShape(HDC hdc, const FnShape& s, int x, int baselineY, const Theme& theme) {
    const bool integral = s.kind == FnShapeKind::IntegralSign;
    int penW = std::max(1, scaledPx(2, s.depth) / 2);  // same weight as the sqrt radical
    if (integral) penW = std::max(2, scaledPx(2, s.depth));
    if (s.kind == FnShapeKind::Rule) penW = std::max(1, s.extra);
    HPEN pen = CreatePen(PS_SOLID, penW, integral ? theme.operatorColor : theme.text);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    const int left = x + s.x, w = s.width;
    const int top = baselineY + s.top, bottom = baselineY + s.bottom;
    auto line = [&](int x0, int y0, int x1, int y1) {
        MoveToEx(hdc, x0, y0, nullptr);
        LineTo(hdc, x1, y1);
    };
    switch (s.kind) {
        case FnShapeKind::Bar:
            line(left + w / 2, top, left + w / 2, bottom);
            break;
        case FnShapeKind::DoubleBar:
            line(left + w * 3 / 10, top, left + w * 3 / 10, bottom);
            line(left + w * 7 / 10, top, left + w * 7 / 10, bottom);
            break;
        case FnShapeKind::FloorLeft:
        case FnShapeKind::CeilLeft: {
            const int stem = left + w * 3 / 10;
            const int serifY = s.kind == FnShapeKind::FloorLeft ? bottom : top;
            line(stem, top, stem, bottom);
            line(stem, serifY, left + w, serifY);
            break;
        }
        case FnShapeKind::FloorRight:
        case FnShapeKind::CeilRight: {
            const int stem = left + w * 7 / 10;
            const int serifY = s.kind == FnShapeKind::FloorRight ? bottom : top;
            line(stem, top, stem, bottom);
            line(stem, serifY, left, serifY);
            break;
        }
        case FnShapeKind::Radical: {
            POINT pts[4] = {
                { left, top + (bottom - top) * 2 / 3 },
                { left + w / 3, bottom },
                { left + w, top },
                { left + w + s.extra, top }
            };
            Polyline(hdc, pts, 4);
            break;
        }
        case FnShapeKind::Rule:
            line(left + 1, baselineY + s.top, left + w - 1, baselineY + s.top);
            break;
        case FnShapeKind::IntegralSign: {
            // Same curve as the Integral structure.
            const int x0 = left + 1, x1 = left + w - 1, xm = left + (w * 46) / 100;
            const int y0 = top + 2, y1 = bottom - 2;
            const int hookR = std::min(13, (bottom - top) * 18 / 100);
            POINT pts[7] = {
                { (LONG)(x1 - 1), (LONG)(y0 + hookR) },
                { (LONG)x1, (LONG)y0 },
                { (LONG)(xm + (hookR * 45) / 100), (LONG)y0 },
                { (LONG)xm, (LONG)baselineY },
                { (LONG)(xm - (hookR * 45) / 100), (LONG)y1 },
                { (LONG)x0, (LONG)y1 },
                { (LONG)(x0 + 1), (LONG)(y1 - hookR) }
            };
            PolyBezier(hdc, pts, 7);
            break;
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void drawFnLayout(HDC hdc, const FnLayout& layout, int x, int baselineY, const Theme& theme,
                  const Cursor* cursor, CaretInfo* outCaret) {
    SetBkMode(hdc, TRANSPARENT);
    for (const FnText& t : layout.texts) {
        HFONT font = t.mathPx > 0 ? mathFontPx(t.mathPx)
                   : t.parenHeight > 0 ? createParenFont(t.depth, t.parenHeight)
                   : fontForDepth(t.depth);
        HFONT old = (HFONT)SelectObject(hdc, font);
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SetTextColor(hdc, t.isOperator ? theme.operatorColor : theme.text);
        TextOutW(hdc, x + t.x, baselineY + t.baseline - tm.tmAscent, t.text.c_str(), (int)t.text.size());
        SelectObject(hdc, old);
    }
    for (const FnShape& s : layout.shapes) drawFnShape(hdc, s, x, baselineY, theme);
    for (const FnRowPart& r : layout.rows)
        drawSliceImpl(hdc, r.ref, r.depth, x + r.x, baselineY + r.baseline, theme, cursor, outCaret);
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
        case ItemType::Function:
            drawFnLayout(hdc, layoutFunction(hdc, it, depth), x, baselineY, theme, cursor, outCaret);
            return;
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
            return;
        }
        case ItemType::Summation:
        case ItemType::Product:
        case ItemType::Integral:
        case ItemType::Derivative:
            drawFnLayout(hdc, layoutStructure(hdc, it, depth), x, baselineY, theme, cursor, outCaret);
            return;
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
                 HitCandidate& best);

// Hit testing for one argument slice (or whole row) of a function.
void findSliceHit(HDC hdc, const RowRef& r, int depth, int x, int baselineY,
                  int pointX, int pointY, HitCandidate& best) {
    Row* row = const_cast<Row*>(r.row);
    if (r.whole) {
        findRowHit(hdc, row, depth, x, baselineY, pointX, pointY, best);
        return;
    }
    if (r.begin >= r.end) {
        considerHit(best, row, (int)r.begin, nullptr, 0, x, baselineY, pointX, pointY);
        return;
    }
    int cursorX = x;
    for (size_t i = r.begin; i < r.end; ++i) {
        Item* item = row->items[i].get();
        considerHit(best, row, (int)i, nullptr, 0, cursorX, baselineY, pointX, pointY);
        findItemHit(hdc, row, (int)i, item, depth, cursorX, baselineY, pointX, pointY, best);
        cursorX += measureItemImpl(hdc, item, depth).width;
    }
    considerHit(best, row, (int)r.end, nullptr, 0, cursorX, baselineY, pointX, pointY);
}

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
            FnLayout layout = layoutFunction(hdc, item, depth);
            for (const FnRowPart& r : layout.rows)
                findSliceHit(hdc, r.ref, r.depth, x + r.x, baselineY + r.baseline, pointX, pointY, best);
            return;
        }
        case ItemType::Summation:
        case ItemType::Product:
        case ItemType::Integral:
        case ItemType::Derivative: {
            FnLayout layout = layoutStructure(hdc, item, depth);
            for (const FnRowPart& r : layout.rows)
                findSliceHit(hdc, r.ref, r.depth, x + r.x, baselineY + r.baseline, pointX, pointY, best);
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
        hit.row->invalidateLayout();
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
    for (auto& kv : g_parenFontCache) DeleteObject(kv.second);
    g_parenFontCache.clear();
    for (auto& kv : g_mathFontCache) DeleteObject(kv.second);
    g_mathFontCache.clear();
    g_parenWidthCache.clear();
    g_tmCache.clear();
}

// ---------------------------------------------------------------- one-line natural text

namespace {

// Unicode superscript / subscript forms; empty when a character has none.
std::wstring toSuperscript(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c >= L'4' && c <= L'9') {
            out += (wchar_t)(0x2074 + (c - L'4'));
            continue;
        }
        switch (c) {
            case L'0': out += L'⁰'; break;
            case L'1': out += L'¹'; break;
            case L'2': out += L'²'; break;
            case L'3': out += L'³'; break;
            case L'+': out += L'⁺'; break;
            case L'-': case L'−': out += L'⁻'; break;
            case L'(': out += L'⁽'; break;
            case L')': out += L'⁾'; break;
            case L'n': out += L'ⁿ'; break;
            case L'x': out += L'ˣ'; break;
            case L'y': out += L'ʸ'; break;
            case L'T': out += L'ᵀ'; break;
            case L' ': break;
            default: return std::wstring();
        }
    }
    return out;
}

std::wstring toSubscript(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c >= L'0' && c <= L'9') out += (wchar_t)(0x2080 + (c - L'0'));
        else if (c == L'+') out += L'₊';
        else if (c == L'-' || c == L'−') out += L'₋';
        else if (c == L'=') out += L'₌';
        else if (c == L'x') out += L'ₓ';
        else if (c == L'n') out += L'ₙ';
        else if (c != L' ') return std::wstring();
    }
    return out;
}

void appendRangeText(const Row* row, size_t begin, size_t end, std::wstring& out);

std::wstring refText(const RowRef& r) {
    std::wstring s;
    appendRangeText(r.row, r.begin, r.end, s);
    return s;
}

RowRef wholeRow(const Row* row) {
    return { row, 0, row ? row->items.size() : 0, true };
}

// Parenthesized unless it is a single atom.
std::wstring groupedText(const RowRef& r) {
    std::wstring t = refText(r);
    return isAtomicOperand(r) ? t : L"(" + t + L")";
}

std::wstring functionText(const Item* it) {
    const FnNotation n = fnNotation(it->functionId);
    const RowRef whole = wholeRow(it->a.get());
    const std::vector<RowRef> args = argumentSlices(it->a.get());
    const std::wstring all = refText(whole);
    switch (n.style) {
        case FnStyle::Named: return n.name + L"(" + all + L")";
        case FnStyle::Inverse: return n.name + L"⁻¹(" + all + L")";
        case FnStyle::Subscripted: return n.name + toSubscript(n.script) + L"(" + all + L")";
        case FnStyle::Abs: return L"|" + all + L"|";
        case FnStyle::Floor: return L"⌊" + all + L"⌋";
        case FnStyle::Ceil: return L"⌈" + all + L"⌉";
        case FnStyle::Round: return L"⌊" + all + L"⌉";
        case FnStyle::Norm: return L"‖" + all + L"‖";
        case FnStyle::Exp:
        case FnStyle::Expm1: {
            std::wstring sup = toSuperscript(all);
            std::wstring e = sup.empty() ? L"e^" + groupedText(whole) : L"e" + sup;
            return n.style == FnStyle::Expm1 ? e + L" − 1" : e;
        }
        case FnStyle::Log1p: return L"ln(1 + " + all + L")";
        case FnStyle::Cbrt: return L"∛" + groupedText(whole);
        case FnStyle::Factorial: return groupedText(whole) + L"!";
        case FnStyle::InverseOf: return groupedText(whole) + L"⁻¹";
        case FnStyle::Transpose: return groupedText(whole) + L"ᵀ";
        case FnStyle::Infix:
            if (args.size() == 2) return refText(args[0]) + n.script + refText(args[1]);
            break;
        case FnStyle::Combinatoric:
            if (args.size() == 2) {
                std::wstring sup = toSuperscript(refText(args[0])), sub = toSubscript(refText(args[1]));
                if (!sup.empty() && !sub.empty()) return sup + n.script + sub;
            }
            break;
        case FnStyle::Hypot:
            if (args.size() == 2)
                return L"√(" + groupedText(args[0]) + L"² + " + groupedText(args[1]) + L"²)";
            break;
        case FnStyle::AngleScale:
            return groupedText(whole) + (it->functionId == SciDeg ? L"·180/π" : L"·π/180");
        case FnStyle::Membership:
            return groupedText(whole) + L" ∈ ℙ";
        case FnStyle::Indexed:
            if (args.size() == 1 || (args.size() == 2 && it->functionId != SciEye)) {
                std::wstring sub = toSubscript(refText(args[0]));
                if (args.size() == 2) {
                    std::wstring second = toSubscript(refText(args[1]));
                    sub = (sub.empty() || second.empty()) ? std::wstring() : sub + L"ₓ" + second;
                }
                if (!sub.empty()) return n.name + sub;
            }
            break;
        default:
            break;  // calculus forms read best as their call
    }
    return fnFallbackName(it->functionId) + L"(" + all + L")";
}

void appendItemText(const Item* it, bool unary, std::wstring& out) {
    const wchar_t var = it->variableName ? (wchar_t)it->variableName : L'x';
    switch (it->type) {
        case ItemType::Number: out += widen(it->numText.c_str()); break;
        case ItemType::Variable: out += (wchar_t)it->variableName; break;
        case ItemType::Name: out += widen(it->nameText.c_str()); break;
        case ItemType::Constant:
            out += it->constantName == 'p' ? L"π" : (it->constantName == 'f' ? L"φ" : L"e");
            break;
        case ItemType::Equals: out += L" = "; break;
        case ItemType::CloseParen: out += it->isBrace ? L'}' : (it->isBracket ? L']' : L')'); break;
        case ItemType::Operator:
            switch (it->opChar) {
                case '+': out += unary ? L"+" : L" + "; break;
                case '-': out += unary ? L"−" : L" − "; break;
                case '*': out += L" · "; break;
                case '%': out += L" mod "; break;
                case ',': out += L", "; break;
                case ';': out += L"; "; break;
                case 'U': out += L" ∪ "; break;
                case 'I': out += L" ∩ "; break;
                case 'D': out += L" Δ "; break;
                default: out += (wchar_t)it->opChar; break;
            }
            break;
        case ItemType::Fraction:
            out += groupedText(wholeRow(it->a.get())) + L"/" + groupedText(wholeRow(it->b.get()));
            break;
        case ItemType::Power: {
            const RowRef exponent = wholeRow(it->b.get());
            std::wstring sup = toSuperscript(refText(exponent));
            out += groupedText(wholeRow(it->a.get()));
            out += sup.empty() ? L"^" + groupedText(exponent) : sup;
            break;
        }
        case ItemType::Sqrt: out += L"√" + groupedText(wholeRow(it->a.get())); break;
        case ItemType::Paren:
            out += it->isBrace ? L'{' : (it->isBracket ? L'[' : L'(');
            out += refText(wholeRow(it->a.get()));
            out += it->isBrace ? L'}' : (it->isBracket ? L']' : L')');
            break;
        case ItemType::Function: out += functionText(it); break;
        case ItemType::Permutation:
        case ItemType::Combination: {
            const wchar_t letter = it->type == ItemType::Permutation ? L'P' : L'C';
            const std::wstring n = refText(wholeRow(it->a.get())), r = refText(wholeRow(it->b.get()));
            std::wstring sup = toSuperscript(n), sub = toSubscript(r);
            if (!sup.empty() && !sub.empty()) out += sup + letter + sub;
            else out += std::wstring(1, letter) + L"(" + n + L", " + r + L")";
            break;
        }
        case ItemType::Integral:
        case ItemType::Summation:
        case ItemType::Product: {
            out += it->type == ItemType::Integral ? L"∫" : (it->type == ItemType::Summation ? L"Σ" : L"∏");
            const std::wstring lower = refText(wholeRow(it->b.get())), upper = refText(wholeRow(it->c.get()));
            if (!lower.empty() || !upper.empty()) {
                std::wstring sub = toSubscript(lower), sup = toSuperscript(upper);
                if ((sub.empty() && !lower.empty()) || (sup.empty() && !upper.empty()))
                    out += L"_(" + lower + L")^(" + upper + L")";
                else
                    out += sub + sup;
            }
            out += L" " + groupedText(wholeRow(it->a.get()));
            if (it->type == ItemType::Integral) out += std::wstring(L" d") + var;
            break;
        }
        case ItemType::Derivative:
            out += std::wstring(L"d/d") + var + L" " + groupedText(wholeRow(it->a.get()));
            if (!rowIsEmpty(it->b.get())) out += std::wstring(L" |") + var + L"=" + refText(wholeRow(it->b.get()));
            break;
    }
}

void appendRangeText(const Row* row, size_t begin, size_t end, std::wstring& out) {
    if (!row) return;
    end = std::min(end, row->items.size());
    for (size_t i = begin; i < end; ++i) {
        const Item* it = row->items[i].get();
        bool unary = false;
        if (it->type == ItemType::Operator && (it->opChar == '-' || it->opChar == '+')) {
            const Item* prev = i > begin ? row->items[i - 1].get() : nullptr;
            unary = !prev || prev->type == ItemType::Equals ||
                    (prev->type == ItemType::Operator && prev->opChar != '!');
        }
        appendItemText(it, unary, out);
    }
}

} // namespace

std::wstring rowToDisplayText(const Row* row) {
    std::wstring out;
    if (row) appendRangeText(row, 0, row->items.size(), out);
    return out;
}
