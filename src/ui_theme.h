// ui_theme.h
//
// Visual tokens and drawing primitives for the calculator chrome: fonts,
// per-theme palettes, anti-aliased shapes, keycaps with hover/press states
// and Segoe Fluent icons. main.cpp owns layout and input; this file owns how
// things look, so both themes stay consistent and contrast is decided once.

#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "layout.h"

enum class KeyRole { Digit, Function, Operator, Equals, Danger, Count };
enum class KeyState { Normal, Hover, Pressed, Count };

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) glyphs.
namespace Icon {
constexpr wchar_t Backspace = 0xE750;
constexpr wchar_t Sun = 0xE706;
constexpr wchar_t Moon = 0xE708;
}

bool isIconLabel(const std::wstring& label);

struct UiFonts {
    HFONT title = nullptr;     // app name in the header
    HFONT pill = nullptr;      // header buttons
    HFONT key = nullptr;       // digits and operators
    HFONT keyLong = nullptr;   // multi-letter function keys ("sin⁻¹")
    HFONT result = nullptr;    // evaluated results in the history
    HFONT note = nullptr;      // hints, errors, the performance overlay
    HFONT icon = nullptr;      // header icons
    HFONT keyIcon = nullptr;   // keypad icons
    void create();
    void destroy();
};

struct KeyColors {
    COLORREF fill;
    COLORREF text;
    COLORREF border;
};

struct UiPalette {
    bool dark = false;
    COLORREF body;           // window background behind the keypad
    COLORREF topBar;         // header strip
    COLORREF title;
    COLORREF divider;        // header and screen separators
    COLORREF pill;           // header buttons
    COLORREF pillBorder;
    COLORREF pillText;
    COLORREF pillActive;     // selected toggle segment
    COLORREF pillActiveText;
    COLORREF screen;         // LCD history + editor background
    COLORREF screenMuted;    // "=" prefix and hints
    COLORREF screenRule;     // dividers between history entries
    COLORREF result;
    COLORREF error;
    COLORREF selection;
    COLORREF keyShadow;      // 1px drop under each keycap
    KeyColors keys[(int)KeyRole::Count];
};

// Palette for the normal (non-Pro) chrome. The LCD colors come from the
// theme, so the screen keeps its identity; key and header colors are tuned
// for 4.5:1 label contrast in both themes.
UiPalette makePalette(const Theme& theme);

COLORREF mixColor(COLORREF a, COLORREF b, double t);
KeyColors keyColorsForState(KeyColors base, KeyRole role, KeyState state, bool dark);

// Anti-aliased rounded rectangle; `border` == CLR_INVALID skips the outline.
void fillRoundRect(HDC hdc, const RECT& r, float radius, COLORREF fill, COLORREF border);

// Gradient from opaque `color` to transparent across `band`, marking content
// that continues past a scroll edge. `opaqueAtTop` picks the solid side.
void fadeEdge(HDC hdc, const RECT& band, COLORREF color, bool opaqueAtTop);

void drawKeyCap(HDC hdc, const RECT& r, KeyRole role, KeyState state, const std::wstring& label,
                const UiPalette& palette, const UiFonts& fonts);

// Header button: text or a single icon glyph, with hover/press feedback.
void drawHeaderButton(HDC hdc, const RECT& r, const std::wstring& label, KeyState state,
                      bool active, const UiPalette& palette, const UiFonts& fonts);

// Two-segment toggle ("DEG | RAD"); `firstSelected` picks the lit segment.
void drawSegmentedToggle(HDC hdc, const RECT& r, const wchar_t* first, const wchar_t* second,
                         bool firstSelected, KeyState state, const UiPalette& palette,
                         const UiFonts& fonts);

// Width a header button needs for `label` (text padding or icon square).
int headerButtonWidth(HDC hdc, const std::wstring& label, const UiFonts& fonts, int height);
