// ui_theme.cpp
#include "ui_theme.h"
#include <gdiplus.h>
#include <algorithm>
#include <cmath>

namespace {

bool fontInstalled(const wchar_t* family) {
    HDC dc = CreateCompatibleDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, family, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        *reinterpret_cast<bool*>(p) = true;
        return 0;
    }, reinterpret_cast<LPARAM>(&found), 0);
    DeleteDC(dc);
    return found;
}

HFONT makeFont(const wchar_t* face, int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

Gdiplus::Color gp(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

void roundRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    r = std::min({ r, w / 2.0f, h / 2.0f });
    float d = r * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void drawCenteredText(HDC hdc, RECT r, const std::wstring& text, HFONT font, COLORREF color) {
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
}

} // namespace

bool isIconLabel(const std::wstring& label) {
    return label.size() == 1 && label[0] >= 0xE000 && label[0] <= 0xF8FF;
}

void UiFonts::create() {
    destroy();
    // Segoe UI Variable (Windows 11) has optical sizes: Display for large
    // numerals, Text for small labels. Windows 10 falls back to Segoe UI.
    bool variable = fontInstalled(L"Segoe UI Variable Display");
    const wchar_t* display = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* text = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* icons = fontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    title = makeFont(display, 17, FW_SEMIBOLD);
    pill = makeFont(text, 13, FW_SEMIBOLD);
    key = makeFont(display, 21, FW_NORMAL);
    keyLong = makeFont(text, 15, FW_NORMAL);
    result = makeFont(display, 23, FW_SEMIBOLD);
    note = makeFont(text, 13, FW_NORMAL);
    icon = makeFont(icons, 15, FW_NORMAL);
    keyIcon = makeFont(icons, 18, FW_NORMAL);
}

void UiFonts::destroy() {
    for (HFONT* f : { &title, &pill, &key, &keyLong, &result, &note, &icon, &keyIcon }) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

COLORREF mixColor(COLORREF a, COLORREF b, double t) {
    auto mix = [t](int x, int y) { return (BYTE)std::clamp((int)std::lround(x + (y - x) * t), 0, 255); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

UiPalette makePalette(const Theme& theme) {
    UiPalette p;
    p.dark = theme.isDark;
    p.body = theme.background;
    p.topBar = theme.panelBackground;
    p.divider = theme.divider;
    p.screen = theme.screenBackground;
    p.screenMuted = mixColor(theme.screenText, theme.screenBackground, 0.45);
    p.screenRule = mixColor(theme.screenText, theme.screenBackground, 0.86);
    p.result = theme.screenResult;
    if (!theme.isDark) {
        p.title = RGB(0x24, 0x22, 0x1F);
        p.pill = RGB(0xDD, 0xD8, 0xCE);
        p.pillBorder = RGB(0xC6, 0xC0, 0xB3);
        p.pillText = RGB(0x2B, 0x29, 0x25);
        p.pillActive = RGB(0xC2, 0x52, 0x18);
        p.pillActiveText = RGB(0xFF, 0xFF, 0xFF);
        p.error = RGB(0xB3, 0x26, 0x1E);
        p.selection = mixColor(RGB(0x2F, 0x7F, 0xD3), theme.screenBackground, 0.62);
        p.keyShadow = RGB(0xB2, 0xAC, 0x9F);
        // Only "=" is a solid accent key; operators carry the accent as text.
        p.keys[(int)KeyRole::Digit] = { RGB(0xFB, 0xF9, 0xF4), RGB(0x1F, 0x1E, 0x1C), RGB(0xCF, 0xC9, 0xBD) };
        p.keys[(int)KeyRole::Function] = { RGB(0xE8, 0xE4, 0xDB), RGB(0x2C, 0x2F, 0x34), RGB(0xC9, 0xC3, 0xB7) };
        p.keys[(int)KeyRole::Operator] = { RGB(0xF4, 0xDF, 0xCB), RGB(0xA3, 0x40, 0x0A), RGB(0xDE, 0xC1, 0xA6) };
        p.keys[(int)KeyRole::Equals] = { RGB(0xC2, 0x52, 0x18), RGB(0xFF, 0xFF, 0xFF), RGB(0xA6, 0x45, 0x12) };
        p.keys[(int)KeyRole::Danger] = { RGB(0xC7, 0x3E, 0x2A), RGB(0xFF, 0xFF, 0xFF), RGB(0xA9, 0x33, 0x22) };
    } else {
        p.title = RGB(0xEE, 0xEE, 0xEE);
        p.pill = RGB(0x31, 0x33, 0x39);
        p.pillBorder = RGB(0x3F, 0x42, 0x4A);
        p.pillText = RGB(0xE8, 0xE8, 0xE8);
        p.pillActive = RGB(0xF0, 0x8A, 0x3C);
        p.pillActiveText = RGB(0x1B, 0x10, 0x06);
        p.error = RGB(0xFF, 0x8A, 0x80);
        p.selection = RGB(0x16, 0x4E, 0x73);
        p.keyShadow = RGB(0x0B, 0x0B, 0x0C);
        p.keys[(int)KeyRole::Digit] = { RGB(0x3A, 0x3D, 0x44), RGB(0xF4, 0xF4, 0xF4), RGB(0x47, 0x4B, 0x53) };
        p.keys[(int)KeyRole::Function] = { RGB(0x2C, 0x2E, 0x34), RGB(0xE3, 0xE3, 0xE3), RGB(0x39, 0x3C, 0x43) };
        p.keys[(int)KeyRole::Operator] = { RGB(0x3B, 0x2A, 0x20), RGB(0xFF, 0xAD, 0x6B), RGB(0x4B, 0x35, 0x27) };
        p.keys[(int)KeyRole::Equals] = { RGB(0xF0, 0x8A, 0x3C), RGB(0x1B, 0x10, 0x06), RGB(0xD9, 0x7A, 0x30) };
        p.keys[(int)KeyRole::Danger] = { RGB(0xB0, 0x36, 0x27), RGB(0xFF, 0xFF, 0xFF), RGB(0x96, 0x2D, 0x20) };
    }
    return p;
}

KeyColors keyColorsForState(KeyColors base, KeyRole role, KeyState state, bool dark) {
    if (state == KeyState::Normal) return base;
    bool solid = role == KeyRole::Equals || role == KeyRole::Danger;
    if (state == KeyState::Hover) {
        COLORREF toward = (dark || solid) ? RGB(255, 255, 255) : RGB(0, 0, 0);
        double amount = solid ? 0.10 : (dark ? 0.08 : 0.05);
        base.fill = mixColor(base.fill, toward, amount);
        base.border = mixColor(base.border, toward, amount);
    } else {
        double amount = solid ? 0.16 : (dark ? 0.22 : 0.10);
        base.fill = mixColor(base.fill, RGB(0, 0, 0), amount);
        base.border = mixColor(base.border, RGB(0, 0, 0), amount);
    }
    return base;
}

void fillRoundRect(HDC hdc, const RECT& r, float radius, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    if (w <= 0 || h <= 0) return;
    Gdiplus::GraphicsPath path;
    roundRectPath(path, (float)r.left, (float)r.top, w, h, radius);
    Gdiplus::SolidBrush brush(gp(fill));
    graphics.FillPath(&brush, &path);
    if (border != CLR_INVALID) {
        // Stroke inside the shape so the 1px edge lands on whole pixels.
        Gdiplus::GraphicsPath edge;
        roundRectPath(edge, r.left + 0.5f, r.top + 0.5f, w - 1.0f, h - 1.0f, std::max(0.0f, radius - 0.5f));
        Gdiplus::Pen pen(gp(border), 1.0f);
        graphics.DrawPath(&pen, &edge);
    }
}

void fadeEdge(HDC hdc, const RECT& band, COLORREF color, bool opaqueAtTop) {
    int h = band.bottom - band.top;
    if (h <= 0 || band.right <= band.left) return;
    Gdiplus::Graphics graphics(hdc);
    Gdiplus::Color solid = gp(color, 255), clear = gp(color, 0);
    // Start one pixel outside so the opaque end fully covers the edge row.
    Gdiplus::LinearGradientBrush brush(Gdiplus::Point(0, band.top - 1), Gdiplus::Point(0, band.bottom),
                                       opaqueAtTop ? solid : clear, opaqueAtTop ? clear : solid);
    graphics.FillRectangle(&brush, (INT)band.left, (INT)band.top, (INT)(band.right - band.left), (INT)h);
}

void fadeEdgeH(HDC hdc, const RECT& band, COLORREF color, bool opaqueAtLeft) {
    int w = band.right - band.left;
    if (w <= 0 || band.bottom <= band.top) return;
    Gdiplus::Graphics graphics(hdc);
    Gdiplus::Color solid = gp(color, 255), clear = gp(color, 0);
    // Start one pixel outside so the opaque end fully covers the edge column.
    Gdiplus::LinearGradientBrush brush(Gdiplus::Point(band.left - 1, 0), Gdiplus::Point(band.right, 0),
                                       opaqueAtLeft ? solid : clear, opaqueAtLeft ? clear : solid);
    graphics.FillRectangle(&brush, (INT)band.left, (INT)band.top, (INT)w, (INT)(band.bottom - band.top));
}

void drawKeyCap(HDC hdc, const RECT& r, KeyRole role, KeyState state, const std::wstring& label,
                const UiPalette& palette, const UiFonts& fonts) {
    KeyColors colors = keyColorsForState(palette.keys[(int)role], role, state, palette.dark);
    const float radius = 8.0f;
    // A pressed key sinks onto its shadow instead of moving its neighbours.
    RECT cap = r;
    if (state == KeyState::Pressed) {
        OffsetRect(&cap, 0, 1);
    } else {
        RECT shadow = r;
        OffsetRect(&shadow, 0, 1);
        fillRoundRect(hdc, shadow, radius, palette.keyShadow, CLR_INVALID);
    }
    fillRoundRect(hdc, cap, radius, colors.fill, colors.border);

    HFONT font = isIconLabel(label) ? fonts.keyIcon : (label.size() > 2 ? fonts.keyLong : fonts.key);
    drawCenteredText(hdc, cap, label, font, colors.text);
}

int headerButtonWidth(HDC hdc, const std::wstring& label, const UiFonts& fonts, int height) {
    if (isIconLabel(label)) return height;
    HFONT old = (HFONT)SelectObject(hdc, fonts.pill);
    SIZE size{};
    GetTextExtentPoint32W(hdc, label.c_str(), (int)label.size(), &size);
    SelectObject(hdc, old);
    return std::max(height, (int)size.cx + 20);
}

void drawHeaderButton(HDC hdc, const RECT& r, const std::wstring& label, KeyState state,
                      bool active, const UiPalette& palette, const UiFonts& fonts) {
    COLORREF fill = active ? palette.pillActive : palette.pill;
    COLORREF border = active ? palette.pillActive : palette.pillBorder;
    COLORREF text = active ? palette.pillActiveText : palette.pillText;
    if (state == KeyState::Hover) {
        COLORREF toward = (palette.dark || active) ? RGB(255, 255, 255) : RGB(0, 0, 0);
        fill = mixColor(fill, toward, palette.dark ? 0.08 : 0.06);
    } else if (state == KeyState::Pressed) {
        fill = mixColor(fill, RGB(0, 0, 0), palette.dark ? 0.25 : 0.12);
    }
    fillRoundRect(hdc, r, 8.0f, fill, border);
    drawCenteredText(hdc, r, label, isIconLabel(label) ? fonts.icon : fonts.pill, text);
}

void drawSegmentedToggle(HDC hdc, const RECT& r, const wchar_t* first, const wchar_t* second,
                         bool firstSelected, KeyState state, const UiPalette& palette,
                         const UiFonts& fonts) {
    COLORREF track = palette.pill;
    if (state == KeyState::Hover) track = mixColor(track, palette.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 0.06);
    if (state == KeyState::Pressed) track = mixColor(track, RGB(0, 0, 0), palette.dark ? 0.25 : 0.12);
    fillRoundRect(hdc, r, 8.0f, track, palette.pillBorder);
    int mid = (r.left + r.right) / 2;
    RECT left = { r.left + 2, r.top + 2, mid, r.bottom - 2 };
    RECT right = { mid, r.top + 2, r.right - 2, r.bottom - 2 };
    RECT lit = firstSelected ? left : right;
    fillRoundRect(hdc, lit, 6.0f, palette.pillActive, CLR_INVALID);
    drawCenteredText(hdc, left, first, fonts.pill, firstSelected ? palette.pillActiveText : palette.pillText);
    drawCenteredText(hdc, right, second, fonts.pill, firstSelected ? palette.pillText : palette.pillActiveText);
}
