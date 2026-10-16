// main.cpp
//
// Native Win32 + GDI application shell. No MFC, no WinRT/XAML, no
// RichEdit/common-controls, no network, no CRT-heavy startup: just
// CreateWindowExW + a WndProc + GDI drawing. This is what keeps process
// start to a handful of milliseconds -- user32.dll/gdi32.dll are always
// already resident in the OS, so there is essentially nothing extra to
// load.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <winreg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <utility>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cstdint>
#include "expr_tree.h"
#include "layout.h"
#include "evaluator.h"
#include "workspace.h"
#include "graph.h"
#include "gpu_graph.h"
#include "ui_theme.h"
#include "resource.h"

namespace {

// ------------------------------------------------------------- app state

struct ButtonDef {
    RECT rect{};
    std::wstring label;
    int action = 0; // Action enum below
    bool isOperator = false;  // right-hand arithmetic column (+ - x /)
    bool isAccent = false;    // the big "=" key
    bool isFunction = false;  // ( ) x^y sqrt DEL -- dark "function" keycaps
    bool isDanger = false;    // AC
};

enum Action {
    ActDigit0, ActDigit1, ActDigit2, ActDigit3, ActDigit4,
    ActDigit5, ActDigit6, ActDigit7, ActDigit8, ActDigit9,
    ActDot, ActPlus, ActMinus, ActMul, ActFrac,
    ActOpenParen, ActCloseParen, ActOpenBracket, ActComma, ActSemicolon,
    ActPower, ActSqrt,
    ActEquals, ActClear, ActClearAll, ActToggleTheme, ActBackspace,
    ActVariableX, ActVariableY, ActQuadratic,
    ActPermutation, ActCombination,
    ActPi, ActE, ActPhi,
    ActToggleAngle, ActToggleScientific, ActToggle2nd,
    // Scientific functions matching SciFunction enum in expr_tree.h:
    ActSciFirst,
    ActSciLast = ActSciFirst + SciFunctionCount - 1
};

// Header buttons, laid out right to left by layoutTopBar().
enum TopAction { TopVarX, TopVarY, TopQuadratic, TopAngle, TopSci, TopTheme, TopCount };

struct App {
    HWND hwnd = nullptr;
    Workspace workspace;
    bool dark = false;
    bool caretVisible = true;
    int scrollY = 0;
    bool scrollToBottomPending = false;
    DWORD lastDeleteTick = 0;
    bool allSelected = false;
    bool rangeSelected = false;
    Cursor selectionAnchor;
    bool outputSelecting = false;
    int outputEntry = -1;
    int outputAnchor = 0;
    int outputCaret = 0;
    bool editorSelecting = false;
    int editorAnchor = 0;
    int editorCaret = 0;
    int editorTextCursor = -1;
    EvaluationContext values;
    bool proMode = false;
    bool degrees = true;   // Pro Max angle unit: true = DEG, false = RAD
    int keypadPage = 0;    // 0 = 1st, 1 = 2nd, 2 = Calc/Matrix
    GraphState graphState;
    DWORD proModeStarted = 0;
    DWORD explosionStarted = 0;
    bool explosionActive = false;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::Image* explosionImage = nullptr;
    IStream* explosionStream = nullptr;
    UINT explosionFrameCount = 0;
    Gdiplus::Image* brickImage = nullptr;
    Gdiplus::TextureBrush* brickBrush = nullptr;
    IStream* brickStream = nullptr;
    int petX = -1;
    int petY = -1;
    bool petDragging = false;
    POINT petDragOffset{};
    std::vector<ButtonDef> buttons;
    RECT topBarRect{}, historyRect{}, editorRect{}, buttonAreaRect{}, graphRect{};
    HFONT uiFont = nullptr;
    HFONT uiFontSmall = nullptr;
    HICON appIcon = nullptr;
    std::vector<std::unique_ptr<Expression>> undo;

    // --- Chrome: fonts, header layout, pointer feedback ---
    UiFonts fonts;
    RECT topRects[TopCount]{};
    bool topVisible[TopCount]{};
    std::wstring title;          // shortened by layoutTopBar() to fit
    int hoverKey = -1;           // index into buttons, -1 = none
    int pressedKey = -1;         // key held down by the mouse
    int flashKey = -1;           // key lit briefly by keyboard input
    int hoverTop = -1;           // TopAction under the pointer
    int pressedTop = -1;
    bool trackingMouse = false;
    RECT lastPetRect{};

    // --- High-performance render caches ---
    HDC backbufferDC = nullptr;
    HBITMAP backbufferBmp = nullptr;
    HBITMAP backbufferOldBmp = nullptr;
    int backbufferW = 0;
    int backbufferH = 0;
    // The back buffer persists between frames, so a section whose inputs
    // hash the same as last time is left as drawn.
    bool backbufferFresh = true;
    uint64_t topBarKey = 0;
    uint64_t historyKey = 0;

    // The keypad rendered once per KeyState over the button area; a frame
    // composes them with a few BitBlts, so hover and press cost almost nothing.
    HDC keyLayerDC[(int)KeyState::Count]{};
    HBITMAP keyLayerBmp[(int)KeyState::Count]{};
    HBITMAP keyLayerOldBmp[(int)KeyState::Count]{};
    bool keyLayerValid[(int)KeyState::Count]{};
    std::vector<char> keyLayerKeyDrawn[(int)KeyState::Count];   // hover/pressed fill in per key
    int keyLayerW = 0;
    int keyLayerH = 0;

    // Ctrl+Shift+P overlay: input-to-pixels latency of the last frame.
    bool perfOverlay = false;
    LONGLONG inputStamp = 0;     // QPC tick of the oldest unpainted input
    double lastInputMs = 0.0;
    double lastPaintMs = 0.0;
    double worstInputMs = 0.0;

    HDC graphCacheDC = nullptr;
    HBITMAP graphCacheBmp = nullptr;
    HBITMAP graphCacheOldBmp = nullptr;
    int graphCacheW = 0;
    int graphCacheH = 0;
    bool graphCacheValid = false;
    std::string graphCacheExpr;
    uint64_t graphCacheStateHash = 0;
    bool graphCacheDark = false;
} g;

void invalidateButtonCache() {
    for (bool& valid : g.keyLayerValid) valid = false;
}

void invalidateGraphCache() {
    g.graphCacheValid = false;
}

constexpr UINT_PTR kGraphRefreshTimerId = 4;
constexpr UINT kGraphRefreshMs = 12;

// An expression edit changes the editor and, in Pro Max, the live graph of
// it. The editor repaints at once; the graph follows a moment later, so a
// keystroke echoes in well under a millisecond and fast typing re-graphs
// once per pause instead of once per key.
void invalidateEdit() {
    InvalidateRect(g.hwnd, &g.editorRect, FALSE);
    if (g.proMode) SetTimer(g.hwnd, kGraphRefreshTimerId, kGraphRefreshMs, nullptr);
}

void cleanupRenderCaches() {
    if (g.backbufferDC) {
        if (g.backbufferOldBmp) SelectObject(g.backbufferDC, g.backbufferOldBmp);
        if (g.backbufferBmp) DeleteObject(g.backbufferBmp);
        DeleteDC(g.backbufferDC);
        g.backbufferDC = nullptr;
        g.backbufferBmp = nullptr;
        g.backbufferOldBmp = nullptr;
        g.backbufferW = g.backbufferH = 0;
    }
    for (int i = 0; i < (int)KeyState::Count; ++i) {
        if (!g.keyLayerDC[i]) continue;
        if (g.keyLayerOldBmp[i]) SelectObject(g.keyLayerDC[i], g.keyLayerOldBmp[i]);
        if (g.keyLayerBmp[i]) DeleteObject(g.keyLayerBmp[i]);
        DeleteDC(g.keyLayerDC[i]);
        g.keyLayerDC[i] = nullptr;
        g.keyLayerBmp[i] = nullptr;
        g.keyLayerOldBmp[i] = nullptr;
        g.keyLayerValid[i] = false;
    }
    g.keyLayerW = g.keyLayerH = 0;
    if (g.graphCacheDC) {
        if (g.graphCacheOldBmp) SelectObject(g.graphCacheDC, g.graphCacheOldBmp);
        if (g.graphCacheBmp) DeleteObject(g.graphCacheBmp);
        DeleteDC(g.graphCacheDC);
        g.graphCacheDC = nullptr;
        g.graphCacheBmp = nullptr;
        g.graphCacheOldBmp = nullptr;
        g.graphCacheW = g.graphCacheH = 0;
        g.graphCacheValid = false;
    }
}

constexpr UINT_PTR kCaretTimerId = 1;
constexpr UINT_PTR kProAnimationTimerId = 2;
constexpr UINT_PTR kFlashTimerId = 3;      // ends a keyboard key flash
constexpr UINT kFlashMs = 110;
constexpr DWORD kProTransitionMs = 720;
constexpr int kMinWidth = 340;
constexpr int kMinHeight = 480;
constexpr wchar_t kSettingsKey[] = L"Software\\NaturalCalculator";
constexpr wchar_t kDarkModeValue[] = L"DarkMode";

void setDarkTitleBar(HWND hwnd, bool dark);

bool loadDarkMode() {
    HKEY key = nullptr;
    DWORD value = 0;
    DWORD valueSize = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    LONG result = RegQueryValueExW(key, kDarkModeValue, nullptr, nullptr,
                                   reinterpret_cast<BYTE*>(&value), &valueSize);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && value != 0;
}

void saveDarkMode(bool dark) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD value = dark ? 1 : 0;
    RegSetValueExW(key, kDarkModeValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

void loadProExplosionAsset() {
    Gdiplus::GdiplusStartupInput startupInput;
    if (Gdiplus::GdiplusStartup(&g.gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok)
        return;

    HRSRC resource = FindResourceW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(IDR_EXPLOSION_SHEET), RT_RCDATA);
    if (!resource) return;
    DWORD resourceSize = SizeofResource(GetModuleHandleW(nullptr), resource);
    HGLOBAL loadedResource = LoadResource(GetModuleHandleW(nullptr), resource);
    const void* source = loadedResource ? LockResource(loadedResource) : nullptr;
    if (!source || resourceSize == 0) return;

    HGLOBAL imageMemory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (!imageMemory) return;
    void* destination = GlobalLock(imageMemory);
    if (!destination) { GlobalFree(imageMemory); return; }
    std::memcpy(destination, source, resourceSize);
    GlobalUnlock(imageMemory);

    if (FAILED(CreateStreamOnHGlobal(imageMemory, TRUE, &g.explosionStream))) {
        GlobalFree(imageMemory);
        return;
    }
    Gdiplus::Image* image = Gdiplus::Image::FromStream(g.explosionStream, FALSE);
    if (!image || image->GetLastStatus() != Gdiplus::Ok) {
        delete image;
        g.explosionStream->Release();
        g.explosionStream = nullptr;
        return;
    }

    if (image->GetWidth() < 2490 || image->GetHeight() < 1680) {
        delete image;
        g.explosionStream->Release();
        g.explosionStream = nullptr;
        return;
    }
    g.explosionFrameCount = 29;
    g.explosionImage = image;
}

void loadProBrickAsset() {
    if (g.brickImage) return;
    if (!g.gdiplusToken) {
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&g.gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok)
            return;
    }

    HRSRC resource = FindResourceW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(IDR_BRICK_TEXTURE), RT_RCDATA);
    if (resource) {
        DWORD resourceSize = SizeofResource(GetModuleHandleW(nullptr), resource);
        HGLOBAL loadedResource = LoadResource(GetModuleHandleW(nullptr), resource);
        const void* source = loadedResource ? LockResource(loadedResource) : nullptr;
        if (source && resourceSize > 0) {
            HGLOBAL imageMemory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
            if (imageMemory) {
                void* destination = GlobalLock(imageMemory);
                if (destination) {
                    std::memcpy(destination, source, resourceSize);
                    GlobalUnlock(imageMemory);
                    if (SUCCEEDED(CreateStreamOnHGlobal(imageMemory, TRUE, &g.brickStream))) {
                        Gdiplus::Image* img = Gdiplus::Image::FromStream(g.brickStream, FALSE);
                        if (img && img->GetLastStatus() == Gdiplus::Ok) {
                            g.brickImage = img;
                            g.brickBrush = new Gdiplus::TextureBrush(img, Gdiplus::WrapModeTile);
                            Gdiplus::Matrix matrix; matrix.Scale(0.5f, 0.5f);
                            g.brickBrush->SetTransform(&matrix);
                            return;
                        }
                        delete img;
                        if (g.brickStream) { g.brickStream->Release(); g.brickStream = nullptr; }
                    }
                } else {
                    GlobalFree(imageMemory);
                }
            }
        }
    }

    const wchar_t* paths[] = { L"src/brick_texture.png", L"brick_texture.png", L"src/brick_texture.jpg", L"brick_texture.jpg" };
    for (const wchar_t* p : paths) {
        Gdiplus::Image* fileImg = Gdiplus::Image::FromFile(p, FALSE);
        if (fileImg && fileImg->GetLastStatus() == Gdiplus::Ok) {
            g.brickImage = fileImg;
            g.brickBrush = new Gdiplus::TextureBrush(fileImg, Gdiplus::WrapModeTile);
            Gdiplus::Matrix matrix; matrix.Scale(0.5f, 0.5f);
            g.brickBrush->SetTransform(&matrix);
            return;
        }
        delete fileImg;
    }
}

void releaseProBrickAsset() {
    delete g.brickBrush;
    g.brickBrush = nullptr;
    delete g.brickImage;
    g.brickImage = nullptr;
    if (g.brickStream) {
        g.brickStream->Release();
        g.brickStream = nullptr;
    }
}

void releaseProExplosionAsset() {
    delete g.explosionImage;
    g.explosionImage = nullptr;
    if (g.explosionStream) {
        g.explosionStream->Release();
        g.explosionStream = nullptr;
    }
    releaseProBrickAsset();
    if (g.gdiplusToken) {
        Gdiplus::GdiplusShutdown(g.gdiplusToken);
        g.gdiplusToken = 0;
    }
}

// ------------------------------------------------------------- layout

void layoutButtons(int areaLeft, int areaTop, int areaW, int areaH) {
    g.buttons.clear();
    const int cols = g.proMode ? 7 : 4;   // Pro Max adds 3 scientific columns
    const int rows = 7; // last row is the wide "=" button spanning all cols
    int gap = 6;
    int cellW = std::max(1, (areaW - gap * (cols + 1)) / cols);
    int cellH = std::max(1, (areaH - gap * (rows + 1)) / rows);

    auto place = [&](int col, int row, int colSpan, const wchar_t* label, Action act,
                      bool isOp = false, bool accent = false, bool isFunc = false,
                      bool danger = false) {
        ButtonDef b;
        b.rect.left = areaLeft + gap + col * (cellW + gap);
        b.rect.top = areaTop + gap + row * (cellH + gap);
        b.rect.right = b.rect.left + cellW * colSpan + gap * (colSpan - 1);
        b.rect.bottom = b.rect.top + cellH;
        b.label = label;
        b.action = act;
        b.isOperator = isOp;
        b.isAccent = accent;
        b.isFunction = isFunc;
        b.isDanger = danger;
        g.buttons.push_back(b);
    };

    // Row 0: quick-access clear row, like the AC/DEL pair on a real
    // scientific calculator, sitting above the main keypad.
    static const wchar_t kBackspaceLabel[] = { Icon::Backspace, 0 };
    place(0, 0, 2, L"AC", ActClearAll, false, false, false, true);
    place(2, 0, 2, kBackspaceLabel, ActBackspace, false, false, true);

    place(0, 1, 1, L"(", ActOpenParen, false, false, true);
    place(1, 1, 1, L")", ActCloseParen, false, false, true);
    place(2, 1, 1, L"xʸ", ActPower, false, false, true);
    place(3, 1, 1, L"\u221A", ActSqrt, false, false, true);

    place(0, 2, 1, L"7", ActDigit7);
    place(1, 2, 1, L"8", ActDigit8);
    place(2, 2, 1, L"9", ActDigit9);
    place(3, 2, 1, L"\u00F7", ActFrac, true);

    place(0, 3, 1, L"4", ActDigit4);
    place(1, 3, 1, L"5", ActDigit5);
    place(2, 3, 1, L"6", ActDigit6);
    place(3, 3, 1, L"\u00D7", ActMul, true);

    place(0, 4, 1, L"1", ActDigit1);
    place(1, 4, 1, L"2", ActDigit2);
    place(2, 4, 1, L"3", ActDigit3);
    place(3, 4, 1, L"\u2212", ActMinus, true);

    place(0, 5, 1, L"C", ActClear, false, false, true);
    place(1, 5, 1, L"0", ActDigit0);
    place(2, 5, 1, L".", ActDot);
    place(3, 5, 1, L"+", ActPlus, true);

    if (g.proMode) {
        // Scientific pad: three extra key columns to the right of the
        // classic keypad, only in Calc Pro Max / Scientific mode.
        const wchar_t* toggleLabel = (g.keypadPage == 0) ? L"2nd" :
                                      (g.keypadPage == 1) ? L"Calc" : L"1st";
        auto sciAct = [](int id) -> Action { return (Action)((int)ActSciFirst + id); };
        if (g.keypadPage == 0) {
            place(4, 0, 1, L"\u222B", sciAct(SciIntegrate), false, false, true);
            place(5, 0, 1, L"d/dx", sciAct(SciDiff), false, false, true);
            place(6, 0, 1, toggleLabel, ActToggle2nd, false, false, true);

            place(4, 1, 1, L"sin", sciAct(SciSin), false, false, true);
            place(5, 1, 1, L"sin⁻¹", sciAct(SciAsin), false, false, true);
            place(6, 1, 1, L"sinh", sciAct(SciSinh), false, false, true);
            place(4, 2, 1, L"cos", sciAct(SciCos), false, false, true);
            place(5, 2, 1, L"cos⁻¹", sciAct(SciAcos), false, false, true);
            place(6, 2, 1, L"cosh", sciAct(SciCosh), false, false, true);
            place(4, 3, 1, L"tan", sciAct(SciTan), false, false, true);
            place(5, 3, 1, L"tan⁻¹", sciAct(SciAtan), false, false, true);
            place(6, 3, 1, L"tanh", sciAct(SciTanh), false, false, true);
            place(4, 4, 1, L"ln", sciAct(SciLn), false, false, true);
            place(5, 4, 1, L"log", sciAct(SciLog), false, false, true);
            place(6, 4, 1, L"eˣ", sciAct(SciExp), false, false, true);
            place(4, 5, 1, L"\u03C0", ActPi, false, false, true);
            place(5, 5, 1, L"e", ActE, false, false, true);
            place(6, 5, 1, L"|x|", sciAct(SciAbs), false, false, true);
        } else if (g.keypadPage == 1) {
            place(4, 0, 1, L"nPr", ActPermutation, false, false, true);
            place(5, 0, 1, L"nCr", ActCombination, false, false, true);
            place(6, 0, 1, toggleLabel, ActToggle2nd, false, false, true);

            place(4, 1, 1, L"sec", sciAct(SciSec), false, false, true);
            place(5, 1, 1, L"sec⁻¹", sciAct(SciAsec), false, false, true);
            place(6, 1, 1, L"sech", sciAct(SciSech), false, false, true);
            place(4, 2, 1, L"csc", sciAct(SciCsc), false, false, true);
            place(5, 2, 1, L"csc⁻¹", sciAct(SciAcsc), false, false, true);
            place(6, 2, 1, L"csch", sciAct(SciCsch), false, false, true);
            place(4, 3, 1, L"cot", sciAct(SciCot), false, false, true);
            place(5, 3, 1, L"cot⁻¹", sciAct(SciAcot), false, false, true);
            place(6, 3, 1, L"coth", sciAct(SciCoth), false, false, true);
            place(4, 4, 1, L"log₂", sciAct(SciLog2), false, false, true);
            place(5, 4, 1, L"\u00B3\u221A", sciAct(SciCbrt), false, false, true);
            place(6, 4, 1, L"\u0393", sciAct(SciGamma), false, false, true);
            place(4, 5, 1, L"\u03C6", ActPhi, false, false, true);
            place(5, 5, 1, L"n!", sciAct(SciFact), false, false, true);
            place(6, 5, 1, L"Rnd", sciAct(SciRound), false, false, true);
        } else {
            place(4, 0, 1, L"\u222B", sciAct(SciIntegrate), false, false, true);
            place(5, 0, 1, L"d/dx", sciAct(SciDiff), false, false, true);
            place(6, 0, 1, toggleLabel, ActToggle2nd, false, false, true);

            place(4, 1, 1, L"lim", sciAct(SciLimit), false, false, true);
            place(5, 1, 1, L"\u03A3", sciAct(SciSum), false, false, true);
            place(6, 1, 1, L"\u03A0", sciAct(SciProduct), false, false, true);
            place(4, 2, 1, L"det", sciAct(SciDet), false, false, true);
            place(5, 2, 1, L"A⁻¹", sciAct(SciInv), false, false, true);
            place(6, 2, 1, L"Aᵀ", sciAct(SciTranspose), false, false, true);
            place(4, 3, 1, L"tr", sciAct(SciTrace), false, false, true);
            place(5, 3, 1, L"rref", sciAct(SciRref), false, false, true);
            place(6, 3, 1, L"‖v‖", sciAct(SciNorm), false, false, true);
            place(4, 4, 1, L"u·v", sciAct(SciDot), false, false, true);
            place(5, 4, 1, L"u×v", sciAct(SciCross), false, false, true);
            place(6, 4, 1, L"Iₙ", sciAct(SciEye), false, false, true);
            place(4, 5, 1, L"[", ActOpenBracket, false, false, true);
            place(5, 5, 1, L"]", ActCloseParen, false, false, true);
            place(6, 5, 1, L",", ActComma, false, false, true);
        }
    }

    place(0, 6, g.proMode ? 7 : 4, L"=", ActEquals, false, true);
}

// Header: buttons packed right to left (theme, mode, angle unit, Quadratic,
// y, x), then the longest title that still fits in front of them.
void layoutTopBar() {
    HDC dc = GetDC(g.hwnd);
    if (!dc) return;
    const int height = 28, gap = 6;
    int top = g.topBarRect.top + (g.topBarRect.bottom - g.topBarRect.top - height) / 2;
    int x = g.topBarRect.right - 8;
    for (bool& visible : g.topVisible) visible = false;
    auto place = [&](TopAction action, int width) {
        g.topRects[action] = { x - width, top, x, top + height };
        g.topVisible[action] = true;
        x -= width + gap;
    };
    place(TopTheme, height);
    place(TopSci, headerButtonWidth(dc, g.proMode ? L"PRO MAX" : L"Sci", g.fonts, height));
    if (g.proMode) {
        HFONT old = (HFONT)SelectObject(dc, g.fonts.pill);
        SIZE unit{};
        GetTextExtentPoint32W(dc, L"RAD", 3, &unit);
        SelectObject(dc, old);
        place(TopAngle, 2 * (unit.cx + 16) + 4);
    }
    place(TopQuadratic, headerButtonWidth(dc, L"Quadratic", g.fonts, height));
    place(TopVarY, height);
    place(TopVarX, height);

    int available = x + gap - 12 - (g.topBarRect.left + 14);
    const wchar_t* titles[] = { g.proMode ? L"Calc Pro Max" : L"Natural Calculator",
                                g.proMode ? L"Pro Max" : L"Calculator" };
    g.title.clear();
    HFONT old = (HFONT)SelectObject(dc, g.fonts.title);
    for (const wchar_t* candidate : titles) {
        SIZE size{};
        GetTextExtentPoint32W(dc, candidate, (int)wcslen(candidate), &size);
        if (size.cx <= available) { g.title = candidate; break; }
    }
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
}

int keyAt(POINT point) {
    for (size_t i = 0; i < g.buttons.size(); ++i)
        if (PtInRect(&g.buttons[i].rect, point)) return (int)i;
    return -1;
}

int topActionAt(POINT point) {
    for (int i = 0; i < TopCount; ++i)
        if (g.topVisible[i] && PtInRect(&g.topRects[i], point)) return i;
    return -1;
}

void recomputeLayout(const RECT& rc) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    int topBarH = 44;
    int editorH = std::max(70, h / 6);
    int buttonAreaH = std::min(420, std::max(300, h * 1 / 2));

    if (g.proMode && w >= 600) {
        int calcW = std::clamp((w * 50) / 100, 420, std::max(420, w - 300));
        g.topBarRect = { 0, 0, calcW, topBarH };
        g.buttonAreaRect = { 0, h - buttonAreaH, calcW, h };
        g.editorRect = { 0, g.buttonAreaRect.top - editorH, calcW, g.buttonAreaRect.top };
        g.historyRect = { 0, topBarH, calcW, g.editorRect.top };
        g.graphRect = { calcW, 0, w, h };

        layoutButtons(g.buttonAreaRect.left, g.buttonAreaRect.top,
                      calcW, g.buttonAreaRect.bottom - g.buttonAreaRect.top);
    } else {
        g.topBarRect = { 0, 0, w, topBarH };
        g.buttonAreaRect = { 0, h - buttonAreaH, w, h };
        g.editorRect = { 0, g.buttonAreaRect.top - editorH, w, g.buttonAreaRect.top };
        g.historyRect = { 0, topBarH, w, g.editorRect.top };
        g.graphRect = { 0, 0, 0, 0 };

        layoutButtons(g.buttonAreaRect.left, g.buttonAreaRect.top,
                      w, g.buttonAreaRect.bottom - g.buttonAreaRect.top);
    }
    layoutTopBar();
    g.hoverKey = g.pressedKey = g.flashKey = -1;
    g.hoverTop = g.pressedTop = -1;
    g.backbufferFresh = true;
    invalidateButtonCache();
    invalidateGraphCache();
}

void recomputeLayout() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    recomputeLayout(rc);
}

double proTransitionProgress() {
    if (!g.proMode) return 0.0;
    return std::clamp((GetTickCount() - g.proModeStarted) /
                      (double)kProTransitionMs, 0.0, 1.0);
}

void activateProMode() {
    if (g.proMode) return;
    g.proMode = true;
    g.proModeStarted = GetTickCount();
    g.explosionStarted = g.proModeStarted;
    g.explosionActive = true;
    loadProBrickAsset();

    RECT wr{};
    GetWindowRect(g.hwnd, &wr);
    int curW = wr.right - wr.left;
    int curH = wr.bottom - wr.top;
    if (curW < 960) {
        SetWindowPos(g.hwnd, nullptr, 0, 0, 960, std::max(curH, 560), SWP_NOMOVE | SWP_NOZORDER);
    }

    RECT client{};
    GetClientRect(g.hwnd, &client);
    recomputeLayout(client);
    g.petX = std::max(8, (int)g.topBarRect.right - 62);
    int editorHeight = (int)(g.editorRect.bottom - g.editorRect.top);
    g.petY = (int)g.editorRect.top + std::max(6, (editorHeight - 58) / 2);
    setDarkTitleBar(g.hwnd, true);
    SetTimer(g.hwnd, kProAnimationTimerId, 30, nullptr);
}

// ------------------------------------------------------------- actions

void ensureCaretVisible() {
    // Simple heuristic: keep caret blinking "on" right after any edit so
    // the change is immediately visible.
    g.caretVisible = true;
    KillTimer(g.hwnd, kCaretTimerId);
    SetTimer(g.hwnd, kCaretTimerId, 530, nullptr);
}

void doAction(int action) {
    // Theme, DEG/RAD, Sci and 2nd change the calculator, not the expression:
    // they keep the selection and leave no undo step.
    bool modeSwitch = action == ActToggleTheme || action == ActToggleAngle ||
                      action == ActToggleScientific || action == ActToggle2nd;
    bool editsExpression = !modeSwitch && action != ActEquals;
    if (editsExpression)
        g.undo.push_back(cloneExpression(g.workspace.current()));
    if ((g.allSelected || g.rangeSelected) && editsExpression) {
        if (g.rangeSelected) deleteRange(g.workspace.current(), g.selectionAnchor,
                                          g.workspace.current().cursor);
        else g.workspace.current() = Expression();
        g.allSelected = false;
        g.rangeSelected = false;
    }
    Expression& cur = g.workspace.current();
    switch (action) {
        case ActDigit0: insertDigit(cur, '0'); break;
        case ActDigit1: insertDigit(cur, '1'); break;
        case ActDigit2: insertDigit(cur, '2'); break;
        case ActDigit3: insertDigit(cur, '3'); break;
        case ActDigit4: insertDigit(cur, '4'); break;
        case ActDigit5: insertDigit(cur, '5'); break;
        case ActDigit6: insertDigit(cur, '6'); break;
        case ActDigit7: insertDigit(cur, '7'); break;
        case ActDigit8: insertDigit(cur, '8'); break;
        case ActDigit9: insertDigit(cur, '9'); break;
        case ActDot: insertDigit(cur, '.'); break;
        case ActVariableX: insertVariable(cur, 'x'); break;
        case ActVariableY: insertVariable(cur, 'y'); break;
        case ActPlus: insertOperator(cur, '+'); break;
        case ActMinus: insertOperator(cur, '-'); break;
        case ActMul: insertOperator(cur, '*'); break;
        case ActFrac: insertFraction(cur); break;
        case ActOpenParen: insertOpenParen(cur); break;
        case ActCloseParen: insertCloseParen(cur); break;
        case ActOpenBracket: insertOpenBracket(cur); break;
        case ActComma: insertOperator(cur, ','); break;
        case ActSemicolon: insertOperator(cur, ';'); break;
        case ActPower: insertPower(cur); break;
        case ActSqrt: insertSqrt(cur); break;
        case ActBackspace: backspace(cur); break;
        case ActPermutation: insertPermutation(cur); break;
        case ActCombination: insertCombination(cur); break;
        case ActPi: insertConstant(cur, 'p'); break;
        case ActE: insertConstant(cur, 'e'); break;
        case ActPhi: insertConstant(cur, 'f'); break;
        case ActToggleAngle:
            g.degrees = !g.degrees;
            break;
        case ActToggleScientific:
            if (!g.proMode) {
                activateProMode();
            } else {
                g.proMode = false;
                RECT rc{};
                GetClientRect(g.hwnd, &rc);
                recomputeLayout(rc);
                InvalidateRect(g.hwnd, nullptr, FALSE);
            }
            break;
        case ActToggle2nd: {
            g.keypadPage = (g.keypadPage + 1) % 3;
            RECT rc{};
            GetClientRect(g.hwnd, &rc);
            recomputeLayout(rc);
            InvalidateRect(g.hwnd, &g.buttonAreaRect, FALSE);
            break;
        }
        case ActEquals: {
            normalizeNames(cur);   // resolve "sin(", "pi", "e"... before evaluating
            bool proTrigger = isProModeTrigger(cur.root.get());
            EvaluationContext context = g.values;
            context.bigNumbers = g.proMode;   // super-large results only in Pro Mode
            context.degrees = g.degrees;
            if (g.workspace.commitCurrent(context)) {
                if (g.workspace.hasSolvedValues()) g.values = g.workspace.solvedValues();
                g.scrollToBottomPending = true;
                if (proTrigger) activateProMode();
            }
            break;
        }
        case ActClear:
            g.workspace.current() = Expression();
            g.allSelected = false;
            break;
        case ActClearAll:
            g.workspace.clearAll();
            g.allSelected = false;
            break;
        case ActToggleTheme:
            g.dark = !g.dark;
            saveDarkMode(g.dark);
            setDarkTitleBar(g.hwnd, g.proMode || g.dark);
            invalidateButtonCache();
            invalidateGraphCache();
            break;
        default:
            if (action >= ActSciFirst && action <= ActSciLast) {
                int sci = action - ActSciFirst;
                if (sci == SciIntegrate) insertIntegral(cur);
                else if (sci == SciDiff) insertDerivative(cur);
                else if (sci == SciSum) insertSummation(cur);
                else if (sci == SciProduct) insertProduct(cur);
                else insertFunction(cur, sci);
            }
            break;
    }
    normalizeNames(cur);   // resolve any words left over from button edits
    ensureCaretVisible();
    // Committing, clearing and mode switches touch everything; a plain edit
    // only the editor (and the graph of it).
    bool wholeWindow = action == ActEquals || action == ActClearAll || action == ActToggleTheme ||
                       action == ActToggleAngle || action == ActToggleScientific || action == ActToggle2nd;
    if (wholeWindow) InvalidateRect(g.hwnd, nullptr, FALSE);
    else invalidateEdit();
}

void undoLastEdit() {
    if (g.undo.empty()) return;
    g.workspace.current() = std::move(*g.undo.back());
    g.undo.pop_back();
    g.allSelected = false;
    g.rangeSelected = false;
    g.editorTextCursor = -1;
    g.editorAnchor = g.editorCaret = 0;
    g.editorTextCursor = -1;
    InvalidateRect(g.hwnd, &g.editorRect, FALSE);
}

void copyCurrentExpression() {
    const Expression& expression = g.workspace.current();
    std::string plain = g.editorCaret != g.editorAnchor
        ? expression.toPlainString().substr(
              std::min(g.editorAnchor, g.editorCaret),
              std::max(g.editorAnchor, g.editorCaret) - std::min(g.editorAnchor, g.editorCaret))
        : g.rangeSelected
        ? rowRangeToPlainString(expression.root.get(),
                                std::min(g.selectionAnchor.index, expression.cursor.index),
                                std::max(g.selectionAnchor.index, expression.cursor.index))
        : expression.toPlainString();
    std::wstring text(plain.begin(), plain.end());
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (data) {
        void* target = GlobalLock(data);
        memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(data);
        SetClipboardData(CF_UNICODETEXT, data);
    }
    CloseClipboard();
}

int editorCharacterAtPoint(POINT point) {
    std::string plain = g.workspace.current().toPlainString();
    HDC hdc = GetDC(g.hwnd);
    if (!hdc) return 0;
    HFONT expressionFont = CreateFontW(-fontHeightForDepth(0), 0, 0, 0, FW_NORMAL,
                                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                       L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, expressionFont);
    int result = 0;
    int bestDistance = INT_MAX;
    for (int index = 0; index <= (int)plain.size(); ++index) {
        std::wstring prefix(plain.begin(), plain.begin() + index);
        SIZE size{};
        GetTextExtentPoint32W(hdc, prefix.c_str(), (int)prefix.size(), &size);
        int distance = std::abs(point.x - (g.editorRect.left + 16 + size.cx));
        if (distance < bestDistance) { bestDistance = distance; result = index; }
    }
    SelectObject(hdc, old);
    DeleteObject(expressionFont);
    ReleaseDC(g.hwnd, hdc);
    return result;
}

void editorCursorAtPoint(POINT point) {
    HDC hdc = GetDC(g.hwnd);
    if (!hdc) return;
    Expression& expression = g.workspace.current();
    Size size = measureExpression(hdc, expression.root.get());
    int midY = (g.editorRect.top + g.editorRect.bottom) / 2;
    int baseline = midY + (size.ascent - size.descent) / 2;
    placeCursorAtPoint(expression, hdc, g.editorRect.left + 16, baseline,
                       point.x, point.y);
    ReleaseDC(g.hwnd, hdc);
}

void insertPlainText(Expression& expression, const std::string& text) {
    // Delegates to the tree-layer text builder, which additionally
    // understands function names ("sin(30)"), constants ("pi", "e") and
    // "sqrt(" -- shared by paste, selection-replace and the text editor.
    insertFromText(expression, text);
}

void replaceEditorSelection(const std::string& replacement) {
    Expression& expression = g.workspace.current();
    std::string plain = expression.toPlainString();
    int begin = g.allSelected ? 0 : std::min(g.editorAnchor, g.editorCaret);
    int end = g.allSelected ? (int)plain.size() : std::max(g.editorAnchor, g.editorCaret);
    if (begin < 0 || end > (int)plain.size() || begin >= end) return;
    plain.replace(begin, end - begin, replacement);
    expression = Expression();
    insertPlainText(expression, plain);
    g.editorTextCursor = begin + (int)replacement.size();
    g.editorAnchor = 0;
    g.editorCaret = 0;
    g.allSelected = false;
    g.rangeSelected = false;
}

void clearAllSelectedExpression() {
    g.workspace.current() = Expression();
    g.allSelected = false;
    g.rangeSelected = false;
    g.editorTextCursor = -1;
    g.editorAnchor = 0;
    g.editorCaret = 0;
}

void rebuildEditorText(const std::string& text, int cursorPosition) {
    Expression& expression = g.workspace.current();
    expression = Expression();
    insertPlainText(expression, text);
    g.editorTextCursor = std::clamp(cursorPosition, 0, (int)text.size());
    g.editorAnchor = g.editorCaret = 0;
    g.allSelected = false;
    g.rangeSelected = false;
}

int currentEditorTextCursor() {
    if (g.editorTextCursor >= 0) return g.editorTextCursor;
    if (g.editorCaret != g.editorAnchor) return g.editorCaret;
    return (int)g.workspace.current().toPlainString().size();
}

int editorTextX(HDC hdc, const std::string& text, int index) {
    HFONT font = CreateFontW(-fontHeightForDepth(0), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    std::wstring prefix(text.begin(), text.begin() + std::clamp(index, 0, (int)text.size()));
    SIZE size{};
    GetTextExtentPoint32W(hdc, prefix.c_str(), (int)prefix.size(), &size);
    SelectObject(hdc, old);
    DeleteObject(font);
    return g.editorRect.left + 16 + size.cx;
}

std::wstring wideResult(const std::string& result) {
    if (result.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, result.data(),
                                     (int)result.size(), nullptr, 0);
    if (length <= 0) return std::wstring(result.begin(), result.end());
    std::wstring converted(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, result.data(), (int)result.size(),
                        converted.data(), length);
    return converted;
}

std::wstring outputText(int entryIndex) {
    if (entryIndex < 0 || entryIndex >= (int)g.workspace.history().size()) return {};
    const std::string& result = g.workspace.history()[entryIndex]->result;
    return L"= " + wideResult(result);
}

void copySelectedOutput() {
    if (g.outputEntry < 0 || g.outputAnchor == g.outputCaret) return;
    std::wstring text = outputText(g.outputEntry);
    int begin = std::min(g.outputAnchor, g.outputCaret);
    int end = std::max(g.outputAnchor, g.outputCaret);
    text = text.substr(begin, end - begin);
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (data) {
        void* target = GlobalLock(data);
        memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(data);
        SetClipboardData(CF_UNICODETEXT, data);
    }
    CloseClipboard();
}

void pasteExpression() {
    if (!OpenClipboard(g.hwnd)) return;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) { CloseClipboard(); return; }
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) { CloseClipboard(); return; }
    Expression& expression = g.workspace.current();
    bool replacingEditorSelection = g.editorCaret != g.editorAnchor;
    if (g.allSelected) {
        expression = Expression();
        g.allSelected = false;
    } else if (g.rangeSelected) {
        deleteRange(expression, g.selectionAnchor, expression.cursor);
        g.rangeSelected = false;
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string pasted;
    if (needed > 0) {
        pasted.resize(needed - 1);
        WideCharToMultiByte(CP_UTF8, 0, text, -1, &pasted[0], needed, nullptr, nullptr);
    }
    if (replacingEditorSelection) replaceEditorSelection(pasted);
    else insertPlainText(expression, pasted);
    GlobalUnlock(data);
    CloseClipboard();
    g.allSelected = false;
    g.rangeSelected = false;
    ensureCaretVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void setDarkTitleBar(HWND hwnd, bool dark) {
    BOOL enabled = dark ? TRUE : FALSE;
    // Attribute 20 is used by Windows 10 1903+; older systems simply reject it.
    HRESULT result = DwmSetWindowAttribute(hwnd, 20, &enabled, sizeof(enabled));
    if (FAILED(result)) DwmSetWindowAttribute(hwnd, 19, &enabled, sizeof(enabled));
}

HICON createAppIcon() {
    constexpr int size = 32;
    std::vector<DWORD> pixels(size * size, RGB(0x00, 0x78, 0xD4));
    for (int y = 3; y < size - 3; ++y) {
        for (int x = 3; x < size - 3; ++x) {
            if (x == 3 || y == 3 || x == size - 4 || y == size - 4)
                pixels[y * size + x] = RGB(0x00, 0x3B, 0x6F);
        }
    }
    for (int y = 8; y < 24; ++y) {
        for (int x = 14; x < 18; ++x) pixels[y * size + x] = RGB(255, 255, 255);
    }
    for (int y = 14; y < 18; ++y) {
        for (int x = 8; x < 24; ++x) pixels[y * size + x] = RGB(255, 255, 255);
    }
    HBITMAP color = CreateBitmap(size, size, 1, 32, pixels.data());
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

struct QuadraticDialogState {
    HWND hwnd = nullptr;
    HWND a = nullptr;
    HWND b = nullptr;
    HWND c = nullptr;
    bool accepted = false;
    bool dark = false;
    double values[3]{};
};

LRESULT CALLBACK quadraticDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<QuadraticDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<QuadraticDialogState*>(cs->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_ERASEBKGND && state->dark) return 1;
    if (msg == WM_CREATE) {
        setDarkTitleBar(hwnd, state->dark);
        CreateWindowW(L"STATIC", L"a", WS_CHILD | WS_VISIBLE, 18, 18, 22, 22, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"b", WS_CHILD | WS_VISIBLE, 18, 58, 22, 22, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"c", WS_CHILD | WS_VISIBLE, 18, 98, 22, 22, hwnd, nullptr, nullptr, nullptr);
        state->a = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 48, 15, 150, 26, hwnd, (HMENU)101, nullptr, nullptr);
        state->b = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 48, 55, 150, 26, hwnd, (HMENU)102, nullptr, nullptr);
        state->c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 48, 95, 150, 26, hwnd, (HMENU)103, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Solve", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 48, 137, 72, 28, hwnd, (HMENU)104, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 126, 137, 72, 28, hwnd, (HMENU)105, nullptr, nullptr);
        SetFocus(state->a);
        return 0;
    }
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC ||
        msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN) {
        if (state->dark) {
            static HBRUSH background = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
            static HBRUSH control = CreateSolidBrush(RGB(0x2B, 0x2B, 0x2B));
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
            SetBkColor(dc, RGB(0x2B, 0x2B, 0x2B));
            return reinterpret_cast<LRESULT>(msg == WM_CTLCOLORDLG ? background : control);
        }
    }
    if (msg == WM_COMMAND && LOWORD(wParam) == 104) {
        wchar_t text[64];
        HWND edits[3] = { state->a, state->b, state->c };
        for (int i = 0; i < 3; ++i) {
            GetWindowTextW(edits[i], text, 64);
            wchar_t* end = nullptr;
            state->values[i] = std::wcstod(text, &end);
            if (end == text || !std::isfinite(state->values[i])) return 0;
        }
        state->accepted = true;
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wParam) == 105) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool promptQuadratic(HWND owner, double& a, double& b, double& c) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = quadraticDialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"NaturalCalculatorQuadraticDialog";
        RegisterClassW(&wc);
        registered = true;
    }
    QuadraticDialogState state;
    state.dark = g.dark;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NaturalCalculatorQuadraticDialog",
                                  L"Solve ax^2 + bx + c = 0", WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 240, 210, owner, nullptr,
                                  GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG msg;
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.accepted) { a = state.values[0]; b = state.values[1]; c = state.values[2]; }
    return state.accepted;
}

std::wstring formatQuadraticResult(const QuadraticResult& result) {
    if (!result.valid) return std::wstring(result.message.begin(), result.message.end());
    std::wostringstream out;
    out << std::setprecision(10);
    if (result.rootCount == 0) return L"No real solutions";
    out << L"x1 = " << result.first;
    if (result.rootCount == 2) out << L"\n x2 = " << result.second;
    return out.str();
}

void solveQuadraticFromDialog() {
    double a, b, c;
    if (!promptQuadratic(g.hwnd, a, b, c)) return;
    QuadraticResult result = solveQuadratic(a, b, c);
    MessageBoxW(g.hwnd, formatQuadraticResult(result).c_str(), L"Quadratic result", MB_OK | MB_ICONINFORMATION);
}

// ------------------------------------------------------------- painting

// Lighten/darken a color by a signed delta per channel (clamped), used to
// fake a raised-keycap bevel without needing alpha blending.
COLORREF shade(COLORREF c, int delta) {
    auto ch = [&](int v) { return (BYTE)std::clamp(v + delta, 0, 255); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

COLORREF blendColor(COLORREF from, COLORREF to, double amount) {
    auto blend = [amount](int a, int b) {
        return (BYTE)std::clamp((int)std::lround(a + (b - a) * amount), 0, 255);
    };
    return RGB(blend(GetRValue(from), GetRValue(to)),
               blend(GetGValue(from), GetGValue(to)),
               blend(GetBValue(from), GetBValue(to)));
}

Theme proTheme(Theme theme, double amount) {
    Theme target = darkTheme();
    target.background = RGB(0x0C, 0x05, 0x07);
    target.panelBackground = RGB(0x16, 0x07, 0x09);
    target.text = RGB(0xF5, 0xEB, 0xE1);
    target.operatorColor = RGB(0xFF, 0x55, 0x18);
    target.resultColor = RGB(0xFF, 0xB8, 0x2E);
    target.caret = RGB(0xFF, 0x85, 0x1C);
    target.placeholder = RGB(0x5A, 0x22, 0x1A);
    target.divider = RGB(0x4A, 0x12, 0x0E);
    target.accent = RGB(0xDF, 0x28, 0x10);
    target.screenBackground = RGB(0x0E, 0x05, 0x07);
    target.screenText = RGB(0xF8, 0xEE, 0xE4);
    target.screenOperator = RGB(0xFF, 0x6A, 0x24);
    target.screenResult = RGB(0xFF, 0xBF, 0x36);
    target.screenCaret = RGB(0xFF, 0x90, 0x20);
    target.screenPlaceholder = RGB(0x5A, 0x25, 0x18);

    theme.background = blendColor(theme.background, target.background, amount);
    theme.panelBackground = blendColor(theme.panelBackground, target.panelBackground, amount);
    theme.text = blendColor(theme.text, target.text, amount);
    theme.operatorColor = blendColor(theme.operatorColor, target.operatorColor, amount);
    theme.resultColor = blendColor(theme.resultColor, target.resultColor, amount);
    theme.caret = blendColor(theme.caret, target.caret, amount);
    theme.placeholder = blendColor(theme.placeholder, target.placeholder, amount);
    theme.divider = blendColor(theme.divider, target.divider, amount);
    theme.accent = blendColor(theme.accent, target.accent, amount);
    theme.screenBackground = blendColor(theme.screenBackground, target.screenBackground, amount);
    theme.screenText = blendColor(theme.screenText, target.screenText, amount);
    theme.screenOperator = blendColor(theme.screenOperator, target.screenOperator, amount);
    theme.screenResult = blendColor(theme.screenResult, target.screenResult, amount);
    theme.screenCaret = blendColor(theme.screenCaret, target.screenCaret, amount);
    theme.screenPlaceholder = blendColor(theme.screenPlaceholder, target.screenPlaceholder, amount);
    theme.isDark = amount >= 0.5 || theme.isDark;
    return theme;
}

int petBounceOffset() {
    if (!g.proMode || g.petDragging) return 0;
    return (int)std::lround(std::sin(GetTickCount() / 115.0) * 4.0);
}

RECT petBounds() {
    int top = g.petY + petBounceOffset();
    return { g.petX, top, g.petX + 54, top + 58 };
}

void clampPetToClient() {
    RECT client{};
    GetClientRect(g.hwnd, &client);
    g.petX = std::clamp(g.petX, 0, std::max(0, (int)client.right - 54));
    g.petY = std::clamp(g.petY, 0, std::max(0, (int)client.bottom - 58));
}

void drawPetShape(HDC hdc, const POINT* points, int count, COLORREF fill, COLORREF outline) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    Polygon(hdc, points, count);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void drawPetEllipse(HDC hdc, int left, int top, int right, int bottom,
                    COLORREF fill, COLORREF outline) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    Ellipse(hdc, left, top, right, bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void drawProPet(HDC hdc, int x, int y, POINT cursor) {
    const COLORREF red = RGB(0xC9, 0x0B, 0x2B);
    const COLORREF hotRed = RGB(0xFF, 0x24, 0x3D);
    const COLORREF deepRed = RGB(0x50, 0x03, 0x13);
    const COLORREF amber = RGB(0xFF, 0xC0, 0x35);
    const COLORREF bone = RGB(0xF3, 0xD9, 0xC8);
    const COLORREF black = RGB(0x0A, 0x02, 0x05);
    int sway = (int)std::lround(std::sin(GetTickCount() / 90.0) * 3.0);
    POINT tail[] = { { x + 34, y + 39 }, { x + 42, y + 36 + sway },
                     { x + 45, y + 29 + sway }, { x + 51, y + 28 + sway } };
    HPEN tailPen = CreatePen(PS_SOLID, 3, hotRed);
    HPEN oldTailPen = (HPEN)SelectObject(hdc, tailPen);
    Polyline(hdc, tail, 4);
    SelectObject(hdc, oldTailPen);
    DeleteObject(tailPen);
    POINT tailTip[] = { { x + 47, y + 24 + sway }, { x + 54, y + 28 + sway },
                        { x + 47, y + 34 + sway }, { x + 49, y + 29 + sway } };
    drawPetShape(hdc, tailTip, 4, hotRed, deepRed);

    POINT leftWing[] = { { x + 17, y + 32 }, { x - 5, y + 21 }, { x + 7, y + 36 },
                         { x - 1, y + 46 }, { x + 18, y + 46 } };
    POINT rightWing[] = { { x + 34, y + 31 }, { x + 56, y + 20 }, { x + 44, y + 36 },
                          { x + 52, y + 46 }, { x + 33, y + 46 } };
    drawPetShape(hdc, leftWing, 5, deepRed, black);
    drawPetShape(hdc, rightWing, 5, deepRed, black);

    POINT leftHorn[] = { { x + 12, y + 17 }, { x + 5, y - 5 }, { x + 22, y + 11 } };
    POINT rightHorn[] = { { x + 31, y + 11 }, { x + 49, y - 6 }, { x + 40, y + 20 } };
    drawPetShape(hdc, leftHorn, 3, deepRed, hotRed);
    drawPetShape(hdc, rightHorn, 3, deepRed, hotRed);

    POINT body[] = { { x + 15, y + 29 }, { x + 35, y + 29 }, { x + 39, y + 43 },
                     { x + 32, y + 42 }, { x + 27, y + 56 }, { x + 23, y + 46 },
                     { x + 17, y + 54 }, { x + 18, y + 43 }, { x + 12, y + 45 } };
    drawPetShape(hdc, body, 9, deepRed, hotRed);

    POINT head[] = { { x + 12, y + 16 }, { x + 8, y + 8 }, { x + 18, y + 11 },
                     { x + 25, y + 9 }, { x + 33, y + 12 }, { x + 42, y + 5 },
                     { x + 39, y + 18 }, { x + 41, y + 26 }, { x + 34, y + 34 },
                     { x + 17, y + 34 }, { x + 10, y + 27 } };
    drawPetShape(hdc, head, 11, red, black);

    int gazeX = std::clamp(((int)cursor.x - (x + 25)) / 14, -2, 2);
    int gazeY = std::clamp(((int)cursor.y - (y + 22)) / 18, -1, 1);
    POINT leftEye[] = { { x + 14, y + 18 }, { x + 24, y + 16 }, { x + 20, y + 23 } };
    POINT rightEye[] = { { x + 27, y + 16 }, { x + 37, y + 17 }, { x + 31, y + 23 } };
    drawPetShape(hdc, leftEye, 3, amber, hotRed);
    drawPetShape(hdc, rightEye, 3, amber, hotRed);
    drawPetEllipse(hdc, x + 18 + gazeX, y + 18 + gazeY,
                   x + 20 + gazeX, y + 22 + gazeY, black, black);
    drawPetEllipse(hdc, x + 30 + gazeX, y + 18 + gazeY,
                   x + 32 + gazeX, y + 22 + gazeY, black, black);

    POINT leftBrow[] = { { x + 12, y + 15 }, { x + 25, y + 14 }, { x + 23, y + 17 } };
    POINT rightBrow[] = { { x + 27, y + 14 }, { x + 41, y + 16 }, { x + 29, y + 17 } };
    drawPetShape(hdc, leftBrow, 3, black, black);
    drawPetShape(hdc, rightBrow, 3, black, black);

    POINT mouth[] = { { x + 18, y + 25 }, { x + 35, y + 24 }, { x + 32, y + 32 },
                      { x + 27, y + 30 }, { x + 23, y + 33 }, { x + 19, y + 30 } };
    drawPetShape(hdc, mouth, 6, black, hotRed);
    POINT fangLeft[] = { { x + 20, y + 25 }, { x + 24, y + 26 }, { x + 22, y + 30 } };
    POINT fangRight[] = { { x + 30, y + 25 }, { x + 34, y + 25 }, { x + 32, y + 30 } };
    POINT lowerFang[] = { { x + 24, y + 31 }, { x + 29, y + 30 }, { x + 27, y + 33 } };
    drawPetShape(hdc, fangLeft, 3, bone, bone);
    drawPetShape(hdc, fangRight, 3, bone, bone);
    drawPetShape(hdc, lowerFang, 3, bone, bone);
}

void drawProExplosion(HDC hdc, RECT client, const Theme& theme) {
    if (!g.explosionActive) return;
    DWORD elapsed = GetTickCount() - g.explosionStarted;
    if (g.explosionImage && g.explosionFrameCount > 0) {
        DWORD frameDuration = 70;
        DWORD animationDuration = g.explosionFrameCount * frameDuration;
        if (elapsed >= animationDuration) {
            g.explosionActive = false;
            return;
        }
        UINT frame = elapsed / frameDuration;
        constexpr UINT frameWidth = 498;
        constexpr UINT frameHeight = 280;
        constexpr UINT columns = 5;
        UINT imageWidth = frameWidth;
        UINT imageHeight = frameHeight;
        int clientWidth = client.right - client.left;
        int clientHeight = client.bottom - client.top;
        double scale = std::max(clientWidth * 1.65 / imageWidth,
                                clientHeight * 0.94 / imageHeight);
        int drawWidth = (int)std::lround(imageWidth * scale);
        int drawHeight = (int)std::lround(imageHeight * scale);
        int centerX = client.left + clientWidth / 2;
        int centerY = client.top + clientHeight / 2;
        int sourceX = (frame % columns) * frameWidth;
        int sourceY = (frame / columns) * frameHeight;
        Gdiplus::Graphics graphics(hdc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeLowQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::Rect destination(centerX - drawWidth / 2,
                                  centerY - drawHeight / 2,
                                  drawWidth, drawHeight);
        Gdiplus::Status status = graphics.DrawImage(
            g.explosionImage, destination, sourceX, sourceY, imageWidth, imageHeight,
            Gdiplus::UnitPixel, nullptr);
        if (status == Gdiplus::Ok) return;
    }
    if (elapsed >= 1050) {
        g.explosionActive = false;
        return;
    }

    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int centerX = client.left + width / 2;
    int centerY = client.top + height / 2;
    int maxDimension = std::max(width, height);
    double seconds = elapsed / 1000.0;

    if (elapsed < 95) {
        double flash = 0.82 * (1.0 - elapsed / 95.0);
        HBRUSH flashBrush = CreateSolidBrush(
            blendColor(theme.background, RGB(0xFF, 0xEF, 0xF2), flash));
        FillRect(hdc, &client, flashBrush);
        DeleteObject(flashBrush);
    }

    for (int ring = 0; ring < 3; ++ring) {
        int ringAge = (int)elapsed - ring * 115;
        if (ringAge < 0 || ringAge >= 760) continue;
        double progress = ringAge / 760.0;
        int radius = 18 + (int)(maxDimension * 0.78 * progress);
        COLORREF color = blendColor(RGB(0xFF, 0xF3, 0xF4), theme.accent, progress);
        HPEN pen = CreatePen(PS_SOLID, progress < 0.35 ? 4 : 2, color);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Ellipse(hdc, centerX - radius, centerY - radius,
                centerX + radius, centerY + radius);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    double rayProgress = std::clamp(elapsed / 560.0, 0.0, 1.0);
    HPEN rayPenLight = CreatePen(PS_SOLID, 2, RGB(0xFF, 0xD9, 0xA0));
    COLORREF blendRayCol = blendColor(RGB(0xFF, 0x56, 0x68), theme.accent, rayProgress);
    HPEN rayPenAccent = CreatePen(PS_SOLID, 1, blendRayCol);
    for (int ray = 0; ray < 22; ++ray) {
        double angle = ray * 6.283185307179586 / 22.0;
        double inner = maxDimension * (0.035 + rayProgress * 0.13);
        double outer = maxDimension * (0.18 + rayProgress * 0.82);
        int x1 = centerX + (int)(std::cos(angle) * inner);
        int y1 = centerY + (int)(std::sin(angle) * inner);
        int x2 = centerX + (int)(std::cos(angle) * outer);
        int y2 = centerY + (int)(std::sin(angle) * outer);
        HPEN activePen = (ray % 3 == 0) ? rayPenLight : rayPenAccent;
        HPEN oldPen = (HPEN)SelectObject(hdc, activePen);
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
        SelectObject(hdc, oldPen);
    }
    DeleteObject(rayPenLight);
    DeleteObject(rayPenAccent);

    COLORREF sparkColors[4] = {
        RGB(0xFF, 0xF0, 0xC5),
        RGB(0xFF, 0x2A, 0x48),
        RGB(0xFF, 0x2A, 0x48),
        RGB(0xFF, 0x8B, 0x45)
    };
    HPEN sparkPens[4][2] = {
        { CreatePen(PS_SOLID, 2, sparkColors[0]), CreatePen(PS_SOLID, 4, sparkColors[0]) },
        { CreatePen(PS_SOLID, 2, sparkColors[1]), CreatePen(PS_SOLID, 4, sparkColors[1]) },
        { CreatePen(PS_SOLID, 2, sparkColors[2]), CreatePen(PS_SOLID, 4, sparkColors[2]) },
        { CreatePen(PS_SOLID, 2, sparkColors[3]), CreatePen(PS_SOLID, 4, sparkColors[3]) }
    };

    for (int spark = 0; spark < 96; ++spark) {
        double angle = spark * 6.283185307179586 / 96.0 +
                       std::sin(spark * 17.13) * 0.055;
        double speed = 140.0 + (spark * 47 % 100) * 3.6;
        double lifetime = 0.72 + (spark * 29 % 50) / 100.0;
        if (seconds >= lifetime) continue;
        double distance = speed * seconds;
        int x2 = centerX + (int)(std::cos(angle) * distance);
        int y2 = centerY + (int)(std::sin(angle) * distance + 180.0 * seconds * seconds);
        int trail = 8 + spark % 15;
        int x1 = x2 - (int)(std::cos(angle) * trail);
        int y1 = y2 - (int)(std::sin(angle) * trail);
        int cIdx = (spark % 4 == 0) ? 0 : (spark % 3 == 0 ? 3 : 1);
        int wIdx = (spark % 5 == 0) ? 1 : 0;
        HPEN oldPen = (HPEN)SelectObject(hdc, sparkPens[cIdx][wIdx]);
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
        SelectObject(hdc, oldPen);

        if (spark % 2 == 0) {
            int shardLength = 7 + spark % 10;
            int shardWidth = 3 + spark % 5;
            double perpendicularX = -std::sin(angle);
            double perpendicularY = std::cos(angle);
            POINT shard[] = {
                { x2, y2 },
                { x2 - (int)(std::cos(angle) * shardLength + perpendicularX * shardWidth),
                  y2 - (int)(std::sin(angle) * shardLength + perpendicularY * shardWidth) },
                { x2 - (int)(std::cos(angle) * shardLength - perpendicularX * shardWidth),
                  y2 - (int)(std::sin(angle) * shardLength - perpendicularY * shardWidth) }
            };
            drawPetShape(hdc, shard, 3, sparkColors[cIdx], RGB(0xFF, 0xD6, 0xA1));
        }
    }
    for (int i = 0; i < 4; ++i) {
        DeleteObject(sparkPens[i][0]);
        DeleteObject(sparkPens[i][1]);
    }
}

void paintButtonPro(HDC hdc, const ButtonDef& b, const Theme& theme, double proProgress, KeyState state) {
    (void)theme;
    (void)proProgress;
    int bW = b.rect.right - b.rect.left;
    int bH = b.rect.bottom - b.rect.top;
    if (bW <= 0 || bH <= 0) return;

    // Hardcore sharp chamfer: 6px 45-degree angled corner cuts
    int ch = std::min(6, std::min(bW, bH) / 4);
    // A pressed key sinks one pixel onto its shadow.
    int sink = state == KeyState::Pressed ? 1 : 0;
    int x0 = (int)b.rect.left;
    int y0 = (int)b.rect.top + sink;
    int x1 = (int)b.rect.right;
    int y1 = (int)b.rect.bottom + sink;

    COLORREF fg = RGB(0xF2, 0xE6, 0xDB);
    bool drawnByGdiPlus = false;

    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::Point pts[8] = {
            Gdiplus::Point(x0 + ch, y0),
            Gdiplus::Point(x1 - ch, y0),
            Gdiplus::Point(x1, y0 + ch),
            Gdiplus::Point(x1, y1 - ch),
            Gdiplus::Point(x1 - ch, y1),
            Gdiplus::Point(x0 + ch, y1),
            Gdiplus::Point(x0, y1 - ch),
            Gdiplus::Point(x0, y0 + ch)
        };
        Gdiplus::GraphicsPath btnPath;
        btnPath.AddPolygon(pts, 8);

        // 1. Dark charred faceted drop shadow
        if (!sink) {
            Gdiplus::Point shadowPts[8];
            for (int i = 0; i < 8; ++i) shadowPts[i] = Gdiplus::Point(pts[i].X + 1, pts[i].Y + 2);
            Gdiplus::GraphicsPath shadowPath;
            shadowPath.AddPolygon(shadowPts, 8);
            Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(160, 4, 1, 2));
            graphics.FillPath(&shadowBrush, &shadowPath);
        }

        // 2. Base texture & dulled obsidian wash
        if (g.brickBrush) {
            graphics.FillPath(g.brickBrush, &btnPath);
            
            // Dull and darken the brick texture so it is less visually distracting
            Gdiplus::SolidBrush obsidianDull(Gdiplus::Color(165, 10, 5, 7));
            graphics.FillPath(&obsidianDull, &btnPath);

            // High-tech infernal role tinting
            if (b.isDanger) {
                Gdiplus::SolidBrush dangerTint(Gdiplus::Color(85, 220, 28, 20));
                graphics.FillPath(&dangerTint, &btnPath);
                fg = RGB(0xFF, 0xFF, 0xFF);
            } else if (b.isAccent || b.isOperator) {
                Gdiplus::SolidBrush magmaTint(Gdiplus::Color(75, 235, 75, 12));
                graphics.FillPath(&magmaTint, &btnPath);
                fg = RGB(0xFF, 0xEE, 0x88);
            } else if (b.isFunction) {
                Gdiplus::SolidBrush fnTint(Gdiplus::Color(60, 65, 14, 18));
                graphics.FillPath(&fnTint, &btnPath);
                fg = RGB(0xFF, 0xBA, 0x48);
            } else {
                Gdiplus::SolidBrush numTint(Gdiplus::Color(45, 14, 6, 8));
                graphics.FillPath(&numTint, &btnPath);
                fg = RGB(0xF5, 0xEB, 0xE1);
            }
        } else if (g.brickImage) {
            // Fallback just in case
            graphics.SetClip(&btnPath);
            int imgW = (int)g.brickImage->GetWidth();
            int imgH = (int)g.brickImage->GetHeight();
            int srcW = std::min(bW * 2, imgW);
            int srcH = std::min(bH * 2, imgH);
            int srcX = std::abs((int)(b.rect.left * 2 + 30)) % std::max(1, imgW);
            int srcY = std::abs((int)(b.rect.top * 2 + 40)) % std::max(1, imgH);
            if (srcX + srcW > imgW) srcX = std::max(0, imgW - srcW);
            if (srcY + srcH > imgH) srcY = std::max(0, imgH - srcH);
            graphics.DrawImage(g.brickImage,
                               Gdiplus::Rect((INT)x0, (INT)y0, (INT)bW, (INT)bH),
                               (INT)srcX, (INT)srcY, (INT)srcW, (INT)srcH,
                               Gdiplus::UnitPixel);

            Gdiplus::SolidBrush obsidianDull(Gdiplus::Color(165, 10, 5, 7));
            graphics.FillPath(&obsidianDull, &btnPath);

            if (b.isDanger) {
                Gdiplus::SolidBrush dangerTint(Gdiplus::Color(85, 220, 28, 20));
                graphics.FillPath(&dangerTint, &btnPath);
                fg = RGB(0xFF, 0xFF, 0xFF);
            } else if (b.isAccent || b.isOperator) {
                Gdiplus::SolidBrush magmaTint(Gdiplus::Color(75, 235, 75, 12));
                graphics.FillPath(&magmaTint, &btnPath);
                fg = RGB(0xFF, 0xEE, 0x88);
            } else if (b.isFunction) {
                Gdiplus::SolidBrush fnTint(Gdiplus::Color(60, 65, 14, 18));
                graphics.FillPath(&fnTint, &btnPath);
                fg = RGB(0xFF, 0xBA, 0x48);
            } else {
                Gdiplus::SolidBrush numTint(Gdiplus::Color(45, 14, 6, 8));
                graphics.FillPath(&numTint, &btnPath);
                fg = RGB(0xF5, 0xEB, 0xE1);
            }
            graphics.ResetClip();
        } else {
            COLORREF solidFill = b.isDanger ? RGB(0x8A, 0x06, 0x1A) :
                                 (b.isAccent || b.isOperator) ? RGB(0xB8, 0x24, 0x0C) :
                                 b.isFunction ? RGB(0x28, 0x06, 0x0C) : RGB(0x18, 0x06, 0x09);
            Gdiplus::SolidBrush fallbackBrush(Gdiplus::Color(255, GetRValue(solidFill), GetGValue(solidFill), GetBValue(solidFill)));
            graphics.FillPath(&fallbackBrush, &btnPath);
            if (b.isDanger) fg = RGB(0xFF, 0xFF, 0xFF);
            else if (b.isAccent || b.isOperator) fg = RGB(0xFF, 0xEE, 0x88);
            else if (b.isFunction) fg = RGB(0xFF, 0xBA, 0x48);
            else fg = RGB(0xF5, 0xEB, 0xE1);
        }

        // 3. Razor-sharp chamfered borders
        Gdiplus::Color borderCol = (b.isOperator || b.isAccent) ? Gdiplus::Color(230, 255, 95, 20)
                                  : b.isDanger ? Gdiplus::Color(245, 255, 45, 25)
                                  : b.isFunction ? Gdiplus::Color(190, 200, 80, 28)
                                  : Gdiplus::Color(150, 80, 30, 22);
        float penWidth = (b.isOperator || b.isAccent || b.isDanger) ? 1.4f : 1.1f;
        Gdiplus::Pen borderPen(borderCol, penWidth);
        graphics.DrawPath(&borderPen, &btnPath);

        // 4. Razor top ember highlight
        Gdiplus::Pen topEmber(Gdiplus::Color(170, 255, 195, 120), 1.0f);
        graphics.DrawLine(&topEmber, (INT)(x0 + ch), (INT)(y0 + 1), (INT)(x1 - ch), (INT)(y0 + 1));

        // 5. Tactical HUD corner ticks on operators & accents
        if (b.isOperator || b.isAccent || b.isDanger) {
            Gdiplus::Pen tickPen(Gdiplus::Color(210, 255, 160, 60), 1.2f);
            graphics.DrawLine(&tickPen, (INT)(x0 + ch - 1), (INT)(y0 + 1), (INT)(x0 + 1), (INT)(y0 + ch - 1));
            graphics.DrawLine(&tickPen, (INT)(x1 - 1), (INT)(y1 - ch + 1), (INT)(x1 - ch + 1), (INT)(y1 - 1));
        }

        // 6. Pointer feedback: an ember glow on hover, charred when pressed.
        if (state == KeyState::Hover) {
            Gdiplus::SolidBrush glow(Gdiplus::Color(24, 255, 150, 70));
            graphics.FillPath(&glow, &btnPath);
            Gdiplus::Pen hotEdge(Gdiplus::Color(200, 255, 140, 60), 1.2f);
            graphics.DrawPath(&hotEdge, &btnPath);
        } else if (state == KeyState::Pressed) {
            Gdiplus::SolidBrush charred(Gdiplus::Color(110, 0, 0, 0));
            graphics.FillPath(&charred, &btnPath);
        }

        // Custom sharp geometric glyphs for Pro Max mode
        if (b.action == ActEquals) {
            int midY = (y0 + y1) / 2;
            int barW = std::min(bW * 3 / 5, 46);
            int barL = x0 + (bW - barW) / 2;
            int barR = barL + barW;
            int barH = 3;
            int barC = 2;
            auto drawSharpBar = [&](int barTop) {
                Gdiplus::Point barPts[6] = {
                    Gdiplus::Point(barL + barC, barTop),
                    Gdiplus::Point(barR - barC, barTop),
                    Gdiplus::Point(barR, barTop + barC),
                    Gdiplus::Point(barR - barC, barTop + barH),
                    Gdiplus::Point(barL + barC, barTop + barH),
                    Gdiplus::Point(barL, barTop + barC)
                };
                Gdiplus::GraphicsPath barPath;
                barPath.AddPolygon(barPts, 6);
                Gdiplus::SolidBrush barBrush(Gdiplus::Color(255, 255, 195, 45));
                graphics.FillPath(&barBrush, &barPath);
                Gdiplus::Pen barPen(Gdiplus::Color(255, 255, 245, 180), 1.0f);
                graphics.DrawPath(&barPen, &barPath);
            };
            drawSharpBar(midY - 5);
            drawSharpBar(midY + 3);
            drawnByGdiPlus = true;
        } else if (b.action == ActMul) {
            int midX = (x0 + x1) / 2;
            int midY = (y0 + y1) / 2;
            int s = 6;
            Gdiplus::Pen bladePen(Gdiplus::Color(255, 255, 235, 140), 2.2f);
            bladePen.SetStartCap(Gdiplus::LineCapTriangle);
            bladePen.SetEndCap(Gdiplus::LineCapTriangle);
            graphics.DrawLine(&bladePen, midX - s, midY - s, midX + s, midY + s);
            graphics.DrawLine(&bladePen, midX + s, midY - s, midX - s, midY + s);
            drawnByGdiPlus = true;
        } else if (b.action == ActFrac) {
            int midX = (x0 + x1) / 2;
            int midY = (y0 + y1) / 2;
            int s = 7;
            Gdiplus::Pen slashPen(Gdiplus::Color(255, 255, 235, 140), 2.4f);
            slashPen.SetStartCap(Gdiplus::LineCapTriangle);
            slashPen.SetEndCap(Gdiplus::LineCapTriangle);
            graphics.DrawLine(&slashPen, midX + s, midY - s, midX - s, midY + s);
            drawnByGdiPlus = true;
        }
    }

    if (drawnByGdiPlus) return;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);

    const std::wstring& displayLabel = b.label;
    HFONT fontToUse = isIconLabel(displayLabel) ? g.fonts.keyIcon
                    : (b.isFunction && displayLabel.length() > 2) ? g.uiFontSmall : g.uiFont;
    HFONT old = (HFONT)SelectObject(hdc, fontToUse);
    RECT r = b.rect;
    OffsetRect(&r, 0, sink);
    DrawTextW(hdc, displayLabel.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
}

KeyRole keyRole(const ButtonDef& b) {
    if (b.isDanger) return KeyRole::Danger;
    if (b.isAccent) return KeyRole::Equals;
    if (b.isOperator) return KeyRole::Operator;
    if (b.isFunction) return KeyRole::Function;
    return KeyRole::Digit;
}

void paintButton(HDC hdc, const ButtonDef& b, const Theme& theme, const UiPalette& palette, KeyState state) {
    if (g.proMode) {
        paintButtonPro(hdc, b, theme, proTransitionProgress(), state);
        return;
    }
    drawKeyCap(hdc, b.rect, keyRole(b), state, b.label, palette, g.fonts);
}

// ------------------------------------------------------------- chrome painting

constexpr int kScreenPad = 16;

uint64_t hashMix(uint64_t hash, uint64_t value) {
    return (hash ^ value) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
}

uint64_t hashText(const std::string& text) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (unsigned char c : text) hash = (hash ^ c) * 0x100000001B3ull;
    return hash;
}

uint64_t themeHash(const Theme& theme) {
    uint64_t hash = 0;
    for (COLORREF c : { theme.background, theme.panelBackground, theme.screenBackground,
                        theme.screenText, theme.screenResult, theme.divider })
        hash = hashMix(hash, c);
    return hashMix(hash, theme.isDark);
}

struct HistoryRow {
    int top = 0;             // content coordinates (0 = top of the scroll area)
    int baseline = 0;
    Size size;
    RECT result{};           // result line box
    int textLeft = 0;        // where outputText() starts
    int bottom = 0;          // end of the row, spacing included
    bool error = false;
    HFONT font = nullptr;    // the result is drawn and hit-tested in this font
};

// Vertical layout of the history column, shared by painting and hit-testing
// so a click selects exactly the characters that were drawn. Results sit
// right-aligned under their expression, like a calculator display.
std::vector<HistoryRow> layoutHistory(HDC hdc, int& contentHeight) {
    const auto& hist = g.workspace.history();
    std::vector<HistoryRow> rows(hist.size());
    int left = g.historyRect.left + kScreenPad;
    int right = g.historyRect.right - kScreenPad;
    int y = 12;
    for (size_t i = 0; i < hist.size(); ++i) {
        HistoryRow& row = rows[i];
        row.size = measureExpression(hdc, hist[i]->expr->root.get());
        row.top = y;
        row.baseline = y + row.size.ascent;
        row.error = hist[i]->isError;
        row.font = row.error ? g.fonts.note : g.fonts.result;
        std::wstring text = outputText((int)i);
        SIZE extent{};
        HFONT old = (HFONT)SelectObject(hdc, row.font);
        GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &extent);
        if (extent.cx > right - left && !row.error) {
            // Long results drop to the smaller face before they overflow.
            row.font = g.fonts.note;
            SelectObject(hdc, row.font);
            GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &extent);
        }
        SelectObject(hdc, old);
        int lineH = row.font == g.fonts.result ? 32 : 22;
        int resultTop = y + row.size.height() + 2;
        row.result = { left, resultTop, right, resultTop + lineH };
        row.textLeft = extent.cx <= right - left ? right - extent.cx : left;
        y = resultTop + lineH + 14;
        row.bottom = y;
    }
    contentHeight = y;
    return rows;
}

bool findOutputAtPoint(POINT point, int& entryIndex, int& characterIndex) {
    if (!PtInRect(&g.historyRect, point)) return false;
    HDC hdc = GetDC(g.hwnd);
    if (!hdc) return false;
    int contentH = 0;
    std::vector<HistoryRow> rows = layoutHistory(hdc, contentH);
    int origin = g.historyRect.top - g.scrollY;
    for (size_t i = 0; i < rows.size(); ++i) {
        RECT resultRect = rows[i].result;
        OffsetRect(&resultRect, 0, origin);
        if (!PtInRect(&resultRect, point)) continue;
        std::wstring text = outputText((int)i);
        HFONT old = (HFONT)SelectObject(hdc, rows[i].font);
        int index = 0;
        int bestDistance = INT_MAX;
        for (int candidate = 0; candidate <= (int)text.size(); ++candidate) {
            SIZE prefix{};
            GetTextExtentPoint32W(hdc, text.c_str(), candidate, &prefix);
            int distance = std::abs(point.x - (rows[i].textLeft + prefix.cx));
            if (distance < bestDistance) { bestDistance = distance; index = candidate; }
        }
        SelectObject(hdc, old);
        ReleaseDC(g.hwnd, hdc);
        entryIndex = (int)i;
        characterIndex = index;
        return true;
    }
    ReleaseDC(g.hwnd, hdc);
    return false;
}

void paintTopBarNormal(HDC mem, const UiPalette& pal) {
    HBRUSH bar = CreateSolidBrush(pal.topBar);
    FillRect(mem, &g.topBarRect, bar);
    DeleteObject(bar);
    for (int i = 0; i < TopCount; ++i) {
        if (!g.topVisible[i]) continue;
        KeyState state = g.pressedTop == i ? KeyState::Pressed
                       : g.hoverTop == i ? KeyState::Hover : KeyState::Normal;
        std::wstring label;
        switch (i) {
            case TopVarX: label = L"x"; break;
            case TopVarY: label = L"y"; break;
            case TopQuadratic: label = L"Quadratic"; break;
            case TopSci: label = L"Sci"; break;
            case TopTheme: label = std::wstring(1, g.dark ? Icon::Sun : Icon::Moon); break;
            case TopAngle:
                drawSegmentedToggle(mem, g.topRects[i], L"DEG", L"RAD", g.degrees, state, pal, g.fonts);
                continue;
        }
        drawHeaderButton(mem, g.topRects[i], label, state, false, pal, g.fonts);
    }
}

// Pro Max header: chamfered HUD badges. `lit` marks the active mode.
void drawHudBadge(HDC mem, const RECT& rect, const std::wstring& label, KeyState state, bool lit) {
    int c = 4;
    POINT pts[8] = {
        { rect.left + c, rect.top }, { rect.right - c, rect.top },
        { rect.right, rect.top + c }, { rect.right, rect.bottom - c },
        { rect.right - c, rect.bottom }, { rect.left + c, rect.bottom },
        { rect.left, rect.bottom - c }, { rect.left, rect.top + c }
    };
    COLORREF fill = lit ? RGB(0x8A, 0x06, 0x1A) : RGB(0x1B, 0x07, 0x0A);
    COLORREF border = lit ? RGB(0xFF, 0x55, 0x18) : RGB(0xB8, 0x2A, 0x12);
    if (state == KeyState::Hover) { fill = shade(fill, 22); border = RGB(0xFF, 0x8A, 0x30); }
    if (state == KeyState::Pressed) fill = shade(fill, -10);
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HBRUSH ob = (HBRUSH)SelectObject(mem, b);
    HPEN op = (HPEN)SelectObject(mem, p);
    Polygon(mem, pts, 8);
    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(b);
    DeleteObject(p);
    if (state != KeyState::Pressed) {
        HPEN hiPen = CreatePen(PS_SOLID, 1, RGB(0xFF, 0x88, 0x30));
        HPEN oldHi = (HPEN)SelectObject(mem, hiPen);
        MoveToEx(mem, rect.left + c, rect.top + 1, nullptr);
        LineTo(mem, rect.right - c, rect.top + 1);
        SelectObject(mem, oldHi);
        DeleteObject(hiPen);
    }
    RECT textRect = rect;
    if (state == KeyState::Pressed) OffsetRect(&textRect, 0, 1);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, lit ? RGB(0xFF, 0xF4, 0xEA) : RGB(0xF5, 0xEB, 0xE1));
    HFONT old = (HFONT)SelectObject(mem, isIconLabel(label) ? g.fonts.icon : g.fonts.pill);
    DrawTextW(mem, label.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, old);
}

void paintTopBarPro(HDC mem, const Theme& theme) {
    HBRUSH bar = CreateSolidBrush(theme.panelBackground);
    FillRect(mem, &g.topBarRect, bar);
    DeleteObject(bar);
    for (int i = 0; i < TopCount; ++i) {
        if (!g.topVisible[i]) continue;
        KeyState state = g.pressedTop == i ? KeyState::Pressed
                       : g.hoverTop == i ? KeyState::Hover : KeyState::Normal;
        const RECT& r = g.topRects[i];
        switch (i) {
            case TopVarX: drawHudBadge(mem, r, L"x", state, false); break;
            case TopVarY: drawHudBadge(mem, r, L"y", state, false); break;
            case TopQuadratic: drawHudBadge(mem, r, L"Quadratic", state, false); break;
            case TopSci: drawHudBadge(mem, r, L"PRO MAX", state, true); break;
            case TopTheme: drawHudBadge(mem, r, std::wstring(1, g.dark ? Icon::Sun : Icon::Moon), state, false); break;
            case TopAngle: {
                // DEG | RAD: both units visible, the active one lit.
                int mid = (r.left + r.right) / 2;
                RECT deg = { r.left, r.top, mid + 1, r.bottom };
                RECT rad = { mid - 1, r.top, r.right, r.bottom };
                drawHudBadge(mem, deg, L"DEG", g.degrees ? state : KeyState::Normal, g.degrees);
                drawHudBadge(mem, rad, L"RAD", g.degrees ? KeyState::Normal : state, !g.degrees);
                break;
            }
        }
    }
}

void paintTopBar(HDC mem, const Theme& theme, const UiPalette& pal) {
    if (g.proMode) paintTopBarPro(mem, theme);
    else paintTopBarNormal(mem, pal);

    if (!g.title.empty()) {
        int right = g.topBarRect.right;
        for (int i = 0; i < TopCount; ++i)
            if (g.topVisible[i]) right = std::min(right, (int)g.topRects[i].left);
        RECT r = { g.topBarRect.left + 14, g.topBarRect.top, right - 12, g.topBarRect.bottom };
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, g.proMode ? RGB(0xF5, 0xEB, 0xE1) : pal.title);
        HFONT old = (HFONT)SelectObject(mem, g.fonts.title);
        DrawTextW(mem, g.title.c_str(), -1, &r, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(mem, old);
    }

    HPEN divPen = CreatePen(PS_SOLID, 1, theme.divider);
    HPEN oldPen = (HPEN)SelectObject(mem, divPen);
    MoveToEx(mem, g.topBarRect.left, g.topBarRect.bottom - 1, nullptr);
    LineTo(mem, g.topBarRect.right, g.topBarRect.bottom - 1);
    SelectObject(mem, oldPen);
    DeleteObject(divPen);
}

void paintHistory(HDC mem, const Theme& theme, const Theme& screenTheme, const UiPalette& pal) {
    HBRUSH screenBrush = CreateSolidBrush(pal.screen);
    FillRect(mem, &g.historyRect, screenBrush);
    DeleteObject(screenBrush);

    HRGN clip = CreateRectRgnIndirect(&g.historyRect);
    SelectClipRgn(mem, clip);

    int contentH = 0;
    std::vector<HistoryRow> rows = layoutHistory(mem, contentH);
    int viewH = g.historyRect.bottom - g.historyRect.top;
    int maxScroll = std::max(0, contentH - viewH);
    if (g.scrollToBottomPending) { g.scrollY = maxScroll; g.scrollToBottomPending = false; }
    g.scrollY = std::clamp(g.scrollY, 0, maxScroll);
    int origin = g.historyRect.top - g.scrollY;
    SetBkMode(mem, TRANSPARENT);

    if (rows.empty()) {
        // Empty state: say what the area is for and how to fill it.
        RECT r = g.historyRect;
        r.left += kScreenPad;
        r.right -= kScreenPad;
        int midY = (r.top + r.bottom) / 2;
        RECT line1 = { r.left, midY - 26, r.right, midY - 2 };
        RECT line2 = { r.left, midY + 2, r.right, midY + 22 };
        SetTextColor(mem, pal.screenMuted);
        HFONT old = (HFONT)SelectObject(mem, g.fonts.title);
        DrawTextW(mem, L"No calculations yet", -1, &line1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(mem, g.fonts.note);
        DrawTextW(mem, L"Type below, then press Enter", -1, &line2,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(mem, old);
    }

    const auto& hist = g.workspace.history();
    for (size_t i = 0; i < rows.size(); ++i) {
        const HistoryRow& row = rows[i];
        if (origin + row.bottom < g.historyRect.top || origin + row.top > g.historyRect.bottom) continue;
        drawExpression(mem, hist[i]->expr->root.get(), g.historyRect.left + kScreenPad,
                       origin + row.baseline, screenTheme, nullptr, nullptr);

        RECT rr = row.result;
        OffsetRect(&rr, 0, origin);
        std::wstring text = outputText((int)i);
        HFONT old = (HFONT)SelectObject(mem, row.font);
        if (g.outputEntry == (int)i && g.outputAnchor != g.outputCaret) {
            int begin = std::min(g.outputAnchor, g.outputCaret);
            int end = std::max(g.outputAnchor, g.outputCaret);
            SIZE before{}, selected{};
            GetTextExtentPoint32W(mem, text.c_str(), begin, &before);
            GetTextExtentPoint32W(mem, text.c_str() + begin, end - begin, &selected);
            RECT highlight = { row.textLeft + before.cx, rr.top + 2, row.textLeft + before.cx + selected.cx, rr.bottom - 2 };
            HBRUSH highlightBrush = CreateSolidBrush(pal.selection);
            FillRect(mem, &highlight, highlightBrush);
            DeleteObject(highlightBrush);
        }
        // "= " in the muted tone, the value itself in the result color.
        SIZE prefix{};
        GetTextExtentPoint32W(mem, text.c_str(), 2, &prefix);
        RECT prefixRect = { row.textLeft, rr.top, row.textLeft + prefix.cx, rr.bottom };
        RECT valueRect = { row.textLeft + prefix.cx, rr.top, rr.right + kScreenPad, rr.bottom };
        SetTextColor(mem, pal.screenMuted);
        DrawTextW(mem, text.c_str(), 2, &prefixRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(mem, row.error ? pal.error : pal.result);
        DrawTextW(mem, text.c_str() + 2, (int)text.size() - 2, &valueRect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(mem, old);

        if (i + 1 < rows.size()) {
            int ruleY = origin + row.bottom - 7;
            HPEN rule = CreatePen(PS_SOLID, 1, pal.screenRule);
            HPEN oldRule = (HPEN)SelectObject(mem, rule);
            MoveToEx(mem, g.historyRect.left + kScreenPad, ruleY, nullptr);
            LineTo(mem, g.historyRect.right - kScreenPad, ruleY);
            SelectObject(mem, oldRule);
            DeleteObject(rule);
        }
    }
    // Soft edges where entries continue past the viewport.
    const int fadeH = 18;
    if (g.scrollY > 0)
        fadeEdge(mem, { g.historyRect.left, g.historyRect.top, g.historyRect.right, g.historyRect.top + fadeH },
                 pal.screen, true);
    if (g.scrollY < maxScroll)
        fadeEdge(mem, { g.historyRect.left, g.historyRect.bottom - fadeH, g.historyRect.right, g.historyRect.bottom },
                 pal.screen, false);
    (void)theme;
    SelectClipRgn(mem, nullptr);
    DeleteObject(clip);
}

void paintEditor(HDC mem, const Theme& theme, const Theme& screenTheme, const UiPalette& pal) {
    HBRUSH b = CreateSolidBrush(pal.screen);
    FillRect(mem, &g.editorRect, b);
    DeleteObject(b);
    HPEN p = CreatePen(PS_SOLID, 1, mixColor(pal.screen, theme.screenText, 0.22));
    HPEN op = (HPEN)SelectObject(mem, p);
    MoveToEx(mem, g.editorRect.left, g.editorRect.top, nullptr);
    LineTo(mem, g.editorRect.right, g.editorRect.top);
    SelectObject(mem, op);
    DeleteObject(p);

    Expression& cur = g.workspace.current();
    Size s = measureExpression(mem, cur.root.get());
    int midY = (g.editorRect.top + g.editorRect.bottom) / 2;
    int baseline = midY + (s.ascent - s.descent) / 2;
    if ((g.allSelected || g.rangeSelected || g.editorCaret != g.editorAnchor) && !rowIsEmpty(cur.root.get())) {
        int selectionLeft = g.editorRect.left + 12;
        int selectionRight = g.editorRect.left + 20 + s.width;
        if (g.editorCaret != g.editorAnchor && !g.allSelected && !g.rangeSelected) {
            std::string plain = cur.toPlainString();
            HDC measure = mem;
            HFONT oldFont = (HFONT)SelectObject(measure, g.uiFont);
            std::wstring before(plain.begin(), plain.begin() + std::min(g.editorAnchor, g.editorCaret));
            std::wstring selected(plain.begin() + std::min(g.editorAnchor, g.editorCaret),
                                  plain.begin() + std::max(g.editorAnchor, g.editorCaret));
            SIZE beforeSize{}, selectedSize{};
            HFONT expressionFont = CreateFontW(-fontHeightForDepth(0), 0, 0, 0, FW_NORMAL,
                                               FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                               L"Segoe UI");
            HFONT previousFont = (HFONT)SelectObject(measure, expressionFont);
            GetTextExtentPoint32W(measure, before.c_str(), (int)before.size(), &beforeSize);
            GetTextExtentPoint32W(measure, selected.c_str(), (int)selected.size(), &selectedSize);
            SelectObject(measure, previousFont);
            DeleteObject(expressionFont);
            int origin = g.editorRect.left + kScreenPad;
            selectionLeft = origin + beforeSize.cx;
            selectionRight = selectionLeft + selectedSize.cx;
            SelectObject(measure, oldFont);
        }
        RECT selection = { selectionLeft, baseline - s.ascent - 4,
                           selectionRight, baseline + s.descent + 4 };
        HBRUSH selectionBrush = CreateSolidBrush(pal.selection);
        FillRect(mem, &selection, selectionBrush);
        DeleteObject(selectionBrush);
    }
    CaretInfo caret;
    drawExpression(mem, cur.root.get(), g.editorRect.left + kScreenPad, baseline, screenTheme, &cur.cursor, &caret);

    if (rowIsEmpty(cur.root.get())) {
        // First-run hint beside the empty slot; it disappears on the first key.
        RECT hint = { g.editorRect.left + kScreenPad + s.width + 12, baseline - s.ascent,
                      g.editorRect.right - kScreenPad, baseline + s.descent };
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, pal.screenMuted);
        HFONT old = (HFONT)SelectObject(mem, g.fonts.note);
        DrawTextW(mem, L"Type an expression", -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem, old);
    }

    if (g.editorTextCursor >= 0) {
        caret.valid = true;
        caret.x = editorTextX(mem, cur.toPlainString(), g.editorTextCursor);
        caret.top = baseline - s.ascent;
        caret.bottom = baseline + s.descent;
    }

    if (caret.valid && g.caretVisible) {
        HPEN cp = CreatePen(PS_SOLID, 2, screenTheme.caret);
        HPEN ocp = (HPEN)SelectObject(mem, cp);
        MoveToEx(mem, caret.x, caret.top, nullptr);
        LineTo(mem, caret.x, caret.bottom);
        SelectObject(mem, ocp);
        DeleteObject(cp);
    }
}

// The keypad drawn in `state` over the button area, reused frame after frame
// until the layout or theme changes. The normal layer renders whole; hover and
// pressed variants render one key at a time on first use, so the first hover
// in Pro Max costs one textured key instead of the whole keypad.
HDC keyLayer(HDC compatible, KeyState state, const Theme& theme, const UiPalette& pal, int key) {
    int w = g.buttonAreaRect.right - g.buttonAreaRect.left;
    int h = g.buttonAreaRect.bottom - g.buttonAreaRect.top;
    int i = (int)state;
    if (g.keyLayerW != w || g.keyLayerH != h) {
        for (int k = 0; k < (int)KeyState::Count; ++k) {
            if (!g.keyLayerDC[k]) continue;
            SelectObject(g.keyLayerDC[k], g.keyLayerOldBmp[k]);
            DeleteObject(g.keyLayerBmp[k]);
            DeleteDC(g.keyLayerDC[k]);
            g.keyLayerDC[k] = nullptr;
            g.keyLayerBmp[k] = nullptr;
            g.keyLayerValid[k] = false;
        }
        g.keyLayerW = w;
        g.keyLayerH = h;
    }
    if (!g.keyLayerDC[i]) {
        g.keyLayerDC[i] = CreateCompatibleDC(compatible);
        g.keyLayerBmp[i] = CreateCompatibleBitmap(compatible, w, h);
        g.keyLayerOldBmp[i] = (HBITMAP)SelectObject(g.keyLayerDC[i], g.keyLayerBmp[i]);
        g.keyLayerValid[i] = false;
    }
    // The Pro Max intro is still blending the theme when the first layer
    // renders; use its final color so every layer matches whenever drawn.
    COLORREF fill = g.proMode ? proTheme(g.dark ? darkTheme() : lightTheme(), 1.0).background
                              : theme.background;
    std::vector<char>& drawn = g.keyLayerKeyDrawn[i];
    auto paintKey = [&](size_t index) {
        ButtonDef local = g.buttons[index];
        OffsetRect(&local.rect, -g.buttonAreaRect.left, -g.buttonAreaRect.top);
        RECT clear = local.rect;
        InflateRect(&clear, 1, 2);   // the drop shadow
        HBRUSH background = CreateSolidBrush(fill);
        FillRect(g.keyLayerDC[i], &clear, background);
        DeleteObject(background);
        paintButton(g.keyLayerDC[i], local, theme, pal, state);
        drawn[index] = 1;
    };
    if (!g.keyLayerValid[i]) {
        drawn.assign(g.buttons.size(), 0);
        if (state == KeyState::Normal) {
            RECT all = { 0, 0, w, h };
            HBRUSH background = CreateSolidBrush(fill);
            FillRect(g.keyLayerDC[i], &all, background);
            DeleteObject(background);
            for (size_t k = 0; k < g.buttons.size(); ++k) paintKey(k);
        }
        g.keyLayerValid[i] = true;
    }
    if (key >= 0 && key < (int)drawn.size() && !drawn[key]) paintKey((size_t)key);
    return g.keyLayerDC[i];
}

void paintKeypad(HDC mem, const Theme& theme, const UiPalette& pal) {
    RECT area = g.buttonAreaRect;
    int w = area.right - area.left;
    int h = area.bottom - area.top;
    if (w <= 0 || h <= 0) return;
    BitBlt(mem, area.left, area.top, w, h, keyLayer(mem, KeyState::Normal, theme, pal, -1), 0, 0, SRCCOPY);
    auto overlay = [&](int index, KeyState state) {
        if (index < 0 || index >= (int)g.buttons.size()) return;
        RECT r = g.buttons[index].rect;
        InflateRect(&r, 1, 2);   // include the drop shadow
        IntersectRect(&r, &r, &area);
        BitBlt(mem, r.left, r.top, r.right - r.left, r.bottom - r.top,
               keyLayer(mem, state, theme, pal, index), r.left - area.left, r.top - area.top, SRCCOPY);
    };
    int pressed = g.pressedKey >= 0 ? g.pressedKey : g.flashKey;
    if (g.hoverKey != pressed) overlay(g.hoverKey, KeyState::Hover);
    overlay(pressed, KeyState::Pressed);
}

RECT petPaintRect() {
    RECT r = petBounds();
    InflateRect(&r, 24, 24);   // spikes, glow and bounce
    return r;
}

void paint(HDC hdc, RECT client, const RECT& dirty) {
    Theme theme = g.dark ? darkTheme() : lightTheme();
    double proProgress = proTransitionProgress();
    if (g.proMode) theme = proTheme(theme, proProgress);

    // A copy of the theme tuned for the LCD-style screen area (history +
    // editor): its own background/text/operator/result/caret colors, so the
    // whole screen reads as one continuous dot-matrix display rather than a
    // plain white panel that happens to sit above the keypad.
    Theme screenTheme = theme;
    screenTheme.background = theme.screenBackground;
    screenTheme.panelBackground = theme.screenBackground;
    screenTheme.text = theme.screenText;
    screenTheme.operatorColor = theme.screenOperator;
    screenTheme.resultColor = theme.screenResult;
    screenTheme.caret = theme.screenCaret;
    screenTheme.placeholder = theme.screenPlaceholder;
    UiPalette pal = makePalette(theme);

    int clientW = client.right - client.left;
    int clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) return;

    if (!g.backbufferDC || !g.backbufferBmp || g.backbufferW != clientW || g.backbufferH != clientH) {
        if (g.backbufferDC) {
            if (g.backbufferOldBmp) SelectObject(g.backbufferDC, g.backbufferOldBmp);
            if (g.backbufferBmp) DeleteObject(g.backbufferBmp);
        } else {
            g.backbufferDC = CreateCompatibleDC(hdc);
        }
        g.backbufferBmp = CreateCompatibleBitmap(hdc, clientW, clientH);
        g.backbufferOldBmp = (HBITMAP)SelectObject(g.backbufferDC, g.backbufferBmp);
        g.backbufferW = clientW;
        g.backbufferH = clientH;
        g.backbufferFresh = true;
    }
    HDC mem = g.backbufferDC;
    if (!mem) return;

    // Sections are redrawn only when what they show changed; the Pro intro
    // animation and a freshly sized buffer redraw everything.
    bool animating = g.proMode && (proProgress < 1.0 || g.explosionActive);
    bool full = g.backbufferFresh || animating;
    if (full) {
        HBRUSH bgBrush = CreateSolidBrush(theme.background);
        FillRect(mem, &client, bgBrush);
        DeleteObject(bgBrush);
    }
    bool petOverTop = false, petOverHistory = false;
    if (g.proMode && proProgress > 0.16) {
        RECT now = petPaintRect(), overlap{};
        petOverTop = IntersectRect(&overlap, &now, &g.topBarRect) || IntersectRect(&overlap, &g.lastPetRect, &g.topBarRect);
        petOverHistory = IntersectRect(&overlap, &now, &g.historyRect) || IntersectRect(&overlap, &g.lastPetRect, &g.historyRect);
    }

    // --- top bar ---
    uint64_t topKey = hashMix(themeHash(theme), (uint64_t)g.proMode | ((uint64_t)g.degrees << 1) | ((uint64_t)g.dark << 2));
    topKey = hashMix(topKey, (uint64_t)(g.hoverTop + 1) | ((uint64_t)(g.pressedTop + 1) << 8));
    topKey = hashMix(topKey, ((uint64_t)g.topBarRect.right << 32) | (uint32_t)g.topBarRect.bottom);
    topKey = hashMix(topKey, hashText(std::string(g.title.begin(), g.title.end())));
    // A section outside the update region keeps its pixels. If its inputs
    // changed meanwhile, its stored key still holds the old value, so it is
    // redrawn as soon as it is exposed again.
    RECT overlap{};
    auto exposed = [&](const RECT& r) { return full || IntersectRect(&overlap, &dirty, &r) != FALSE; };
    if (exposed(g.topBarRect) && (full || petOverTop || topKey != g.topBarKey)) {
        paintTopBar(mem, theme, pal);
        g.topBarKey = topKey;
    }

    // --- history (scrollable) ---
    const auto& hist = g.workspace.history();
    uint64_t historyKey = hashMix(themeHash(theme), hist.size());
    if (!hist.empty()) historyKey = hashMix(historyKey, hashText(hist.back()->result));
    if (hist.size() >= 2) historyKey = hashMix(historyKey, hashText(hist[hist.size() - 2]->result));
    historyKey = hashMix(historyKey, (uint64_t)(uint32_t)g.scrollY | ((uint64_t)g.scrollToBottomPending << 32));
    historyKey = hashMix(historyKey, ((uint64_t)(uint32_t)(g.outputEntry + 1) << 40) ^
                                     ((uint64_t)(uint32_t)g.outputAnchor << 20) ^ (uint32_t)g.outputCaret);
    historyKey = hashMix(historyKey, ((uint64_t)g.historyRect.right << 32) ^ ((uint64_t)g.historyRect.top << 16) ^
                                     (uint64_t)g.historyRect.bottom);
    if (exposed(g.historyRect) && (full || petOverHistory || historyKey != g.historyKey)) {
        paintHistory(mem, theme, screenTheme, pal);
        g.historyKey = historyKey;
    }

    // --- editor (current expression): caret and typing ---
    if (exposed(g.editorRect)) paintEditor(mem, theme, screenTheme, pal);

    // --- keypad: cached layers composed with BitBlt ---
    if (exposed(g.buttonAreaRect)) paintKeypad(mem, theme, pal);

    // --- live graph panel in scientific mode (cached off-screen) ---
    if (g.proMode && g.graphRect.right > g.graphRect.left && exposed(g.graphRect)) {
        int gw = g.graphRect.right - g.graphRect.left;
        int gh = g.graphRect.bottom - g.graphRect.top;
        if (gw > 0 && gh > 0) {
            std::string curPlain = g.workspace.current().toPlainString();
            if (curPlain.empty() && !g.workspace.history().empty()) {
                curPlain = g.workspace.history().back()->expr->toPlainString();
            }
            // Everything drawn in the panel; hover is tracked but never drawn.
            auto graphStateHash = [&]() -> uint64_t {
                auto hashDouble = [](double v) -> uint64_t {
                    uint64_t u = 0;
                    std::memcpy(&u, &v, sizeof(u));
                    return u;
                };
                uint64_t hash = 0;
                hash ^= hashDouble(g.graphState.minX) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                hash ^= hashDouble(g.graphState.maxX) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                hash ^= hashDouble(g.graphState.minY) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                hash ^= hashDouble(g.graphState.maxY) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                hash ^= ((uint64_t)g.graphState.probePos.x << 32) | (uint32_t)g.graphState.probePos.y;
                hash ^= (uint64_t)(g.degrees ? 1 : 0) |
                        ((uint64_t)(g.graphState.isRightProbing ? 1 : 0) << 2);
                return hash;
            };
            uint64_t stateHash = graphStateHash();

            bool needGraphRender = !g.graphCacheValid ||
                                   g.graphCacheW != gw ||
                                   g.graphCacheH != gh ||
                                   g.graphCacheExpr != curPlain ||
                                   g.graphCacheStateHash != stateHash ||
                                   g.graphCacheDark != theme.isDark;

            if (needGraphRender) {
                if (!g.graphCacheDC || !g.graphCacheBmp || g.graphCacheW != gw || g.graphCacheH != gh) {
                    if (g.graphCacheDC) {
                        if (g.graphCacheOldBmp) SelectObject(g.graphCacheDC, g.graphCacheOldBmp);
                        if (g.graphCacheBmp) DeleteObject(g.graphCacheBmp);
                    } else {
                        g.graphCacheDC = CreateCompatibleDC(mem);
                    }
                    g.graphCacheBmp = CreateCompatibleBitmap(mem, gw, gh);
                    g.graphCacheOldBmp = (HBITMAP)SelectObject(g.graphCacheDC, g.graphCacheBmp);
                    g.graphCacheW = gw;
                    g.graphCacheH = gh;
                }

                GraphAnalysis analysis = analyzeGraphExpression(g.workspace.current().root.get(), g.values);
                if (!analysis.isValid && g.workspace.current().root->items.empty() && !g.workspace.history().empty()) {
                    analysis = analyzeGraphExpression(g.workspace.history().back()->expr->root.get(), g.values);
                }
                RECT localRect = { 0, 0, gw, gh };
                renderGraph(g.graphCacheDC, localRect, analysis, g.graphState, theme, g.uiFont, g.uiFontSmall);

                g.graphCacheExpr = curPlain;
                // Rendering may square up the view; hash what was actually drawn.
                g.graphCacheStateHash = graphStateHash();
                g.graphCacheDark = theme.isDark;
                g.graphCacheValid = true;
            }

            BitBlt(mem, g.graphRect.left, g.graphRect.top, gw, gh, g.graphCacheDC, 0, 0, SRCCOPY);

            HPEN divPen = CreatePen(PS_SOLID, 1, theme.divider);
            HPEN oldDivPen = (HPEN)SelectObject(mem, divPen);
            MoveToEx(mem, g.graphRect.left, 0, nullptr);
            LineTo(mem, g.graphRect.left, client.bottom);
            SelectObject(mem, oldDivPen);
            DeleteObject(divPen);
        }
    }

    if (g.proMode && proProgress > 0.16) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(g.hwnd, &cursor);
        drawProPet(mem, g.petX, g.petY + petBounceOffset(), cursor);
        g.lastPetRect = petPaintRect();
    }
    drawProExplosion(mem, client, theme);

    if (g.perfOverlay) {
        wchar_t text[112];
        swprintf(text, 112, L"input → pixels %.2f ms  ·  paint %.2f ms  ·  worst %.2f ms",
                 g.lastInputMs, g.lastPaintMs, g.worstInputMs);
        HFONT old = (HFONT)SelectObject(mem, g.fonts.note);
        SIZE size{};
        GetTextExtentPoint32W(mem, text, (int)wcslen(text), &size);
        RECT box = { g.editorRect.right - size.cx - 24, g.editorRect.top + 6,
                     g.editorRect.right - 8, g.editorRect.top + 12 + size.cy };
        fillRoundRect(mem, box, 6.0f, mixColor(pal.screen, theme.screenText, 0.12), CLR_INVALID);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, theme.screenText);
        DrawTextW(mem, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, old);
    }

    g.backbufferFresh = false;
    BitBlt(hdc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
           mem, dirty.left, dirty.top, SRCCOPY);
}

// ------------------------------------------------------------- pointer & key feedback

void stampInput() {
    if (g.inputStamp) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g.inputStamp = now.QuadPart;
}

void invalidateKey(int index) {
    if (index < 0 || index >= (int)g.buttons.size()) return;
    RECT r = g.buttons[index].rect;
    InflateRect(&r, 1, 2);   // the drop shadow
    InvalidateRect(g.hwnd, &r, FALSE);
}

void invalidateTopAction(int index) {
    if (index >= 0 && index < TopCount && g.topVisible[index])
        InvalidateRect(g.hwnd, &g.topRects[index], FALSE);
}

// Hover feedback for keys and header buttons; only the rects that change
// are repainted, so moving the pointer costs a BitBlt or two.
void updateHover(POINT point) {
    bool busy = g.editorSelecting || g.outputSelecting || g.petDragging ||
                g.graphState.isDragging || g.graphState.isRightProbing;
    int key = busy ? -1 : keyAt(point);
    int top = busy ? -1 : topActionAt(point);
    if (key != g.hoverKey) {
        invalidateKey(g.hoverKey);
        invalidateKey(key);
        g.hoverKey = key;
    }
    if (top != g.hoverTop) {
        invalidateTopAction(g.hoverTop);
        invalidateTopAction(top);
        g.hoverTop = top;
    }
}

void releasePressed() {
    invalidateKey(g.pressedKey);
    invalidateTopAction(g.pressedTop);
    g.pressedKey = -1;
    g.pressedTop = -1;
}

// The keypad key a typed character corresponds to, for the press flash.
int actionForChar(wchar_t c) {
    if (c >= L'0' && c <= L'9') return ActDigit0 + (c - L'0');
    switch (c) {
        case L'.': return ActDot;
        case L'+': return ActPlus;
        case L'-': return ActMinus;
        case L'*': return ActMul;
        case L'/': return ActFrac;
        case L'(': return ActOpenParen;
        case L')': return ActCloseParen;
        case L'^': return ActPower;
        case L',': return ActComma;
        case L'[': return ActOpenBracket;
        case 13: return ActEquals;
        case 8: return ActBackspace;
        default: return -1;
    }
}

// Typing lights the matching key for a moment, like a physical keypad.
void flashKeyForAction(int action) {
    if (action < 0) return;
    for (size_t i = 0; i < g.buttons.size(); ++i) {
        if (g.buttons[i].action != action) continue;
        invalidateKey(g.flashKey);
        g.flashKey = (int)i;
        invalidateKey(g.flashKey);
        SetTimer(g.hwnd, kFlashTimerId, kFlashMs, nullptr);
        return;
    }
}

// ------------------------------------------------------------- WndProc

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            initGpuGraph();
            loadProExplosionAsset();
            loadProBrickAsset();
            setDarkTitleBar(hwnd, g.dark);
            g.uiFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g.uiFontSmall = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g.fonts.create();
            recomputeLayout();
            SetTimer(hwnd, kCaretTimerId, 530, nullptr);
            return 0;

        case WM_SIZE:
            recomputeLayout();
            if (g.proMode) clampPetToClient();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = g.proMode ? 880 : kMinWidth;
            mmi->ptMinTrackSize.y = kMinHeight;
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // avoid flicker; WM_PAINT double-buffers the whole client area

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            if (client.right > client.left && client.bottom > client.top) {
                LARGE_INTEGER start{}, end{}, frequency{};
                QueryPerformanceCounter(&start);
                paint(hdc, client, ps.rcPaint);
                QueryPerformanceCounter(&end);
                QueryPerformanceFrequency(&frequency);
                double toMs = 1000.0 / (double)frequency.QuadPart;
                g.lastPaintMs = (end.QuadPart - start.QuadPart) * toMs;
                if (g.inputStamp) {
                    double latency = (end.QuadPart - g.inputStamp) * toMs;
                    // A stamp older than this never got its own repaint.
                    if (latency < 250.0) {
                        g.lastInputMs = latency;
                        g.worstInputMs = std::max(g.worstInputMs, latency);
                    }
                    g.inputStamp = 0;
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kCaretTimerId) {
                g.caretVisible = !g.caretVisible;
                InvalidateRect(hwnd, &g.editorRect, FALSE);
            } else if (wParam == kFlashTimerId) {
                KillTimer(hwnd, kFlashTimerId);
                invalidateKey(g.flashKey);
                g.flashKey = -1;
            } else if (wParam == kGraphRefreshTimerId) {
                KillTimer(hwnd, kGraphRefreshTimerId);
                if (g.proMode) InvalidateRect(hwnd, &g.graphRect, FALSE);
            } else if (wParam == kProAnimationTimerId && g.proMode) {
                int oldY = g.petY + petBounceOffset();
                RECT oldPetRect = { g.petX - 20, oldY - 20, g.petX + 80, oldY + 80 };
                clampPetToClient();
                int newY = g.petY + petBounceOffset();
                RECT newPetRect = { g.petX - 20, newY - 20, g.petX + 80, newY + 80 };

                if (g.explosionActive || proTransitionProgress() < 1.0) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    InvalidateRect(hwnd, &oldPetRect, FALSE);
                    InvalidateRect(hwnd, &newPetRect, FALSE);
                }
            }
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) return DefWindowProcW(hwnd, msg, wParam, lParam);
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            if (keyAt(point) >= 0 || topActionAt(point) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            int outputEntry = -1;
            int outputCharacter = 0;
            if (findOutputAtPoint(point, outputEntry, outputCharacter) ||
                PtInRect(&g.editorRect, point)) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            stampInput();
            RECT currentPetBounds = petBounds();
            if (g.proMode && proTransitionProgress() > 0.16 && PtInRect(&currentPetBounds, pt)) {
                g.petDragging = true;
                g.petDragOffset = { pt.x - currentPetBounds.left, pt.y - currentPetBounds.top };
                SetCapture(hwnd);
                return 0;
            }
            if (g.proMode && PtInRect(&g.graphRect, pt)) {
                if (handleGraphMouseDown(g.graphState, g.graphRect, (int)pt.x, (int)pt.y)) {
                    invalidateGraphCache();
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &g.graphRect, FALSE);
                    return 0;
                }
            }
            int top = topActionAt(pt);
            if (top == TopQuadratic) {
                // Modal dialog: no capture, or its own controls could not be clicked.
                solveQuadraticFromDialog();
                return 0;
            }
            if (top >= 0) {
                g.pressedTop = top;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g.topRects[top], FALSE);
                switch (top) {
                    case TopTheme: doAction(ActToggleTheme); break;
                    case TopVarX: doAction(ActVariableX); break;
                    case TopVarY: doAction(ActVariableY); break;
                    case TopSci: doAction(ActToggleScientific); break;
                    case TopAngle: doAction(ActToggleAngle); break;
                }
                return 0;
            }
            if (PtInRect(&g.editorRect, pt)) {
                g.editorSelecting = true;
                g.editorAnchor = editorCharacterAtPoint(pt);
                g.editorCaret = g.editorAnchor;
                g.editorTextCursor = -1;
                editorCursorAtPoint(pt);
                g.allSelected = false;
                g.rangeSelected = false;
                g.outputEntry = -1;
                SetCapture(hwnd);
                SetFocus(hwnd);
                InvalidateRect(hwnd, &g.editorRect, FALSE);
                return 0;
            }
            int outputEntry = -1;
            int outputCharacter = 0;
            if (findOutputAtPoint(pt, outputEntry, outputCharacter)) {
                g.outputSelecting = true;
                g.outputEntry = outputEntry;
                g.outputAnchor = outputCharacter;
                g.outputCaret = outputCharacter;
                g.allSelected = false;
                g.rangeSelected = false;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g.historyRect, FALSE);
                return 0;
            }
            int key = keyAt(pt);
            if (key >= 0) {
                // Act on press like a physical keypad; the key stays sunk until release.
                int action = g.buttons[key].action;
                invalidateKey(g.flashKey);
                g.flashKey = -1;
                g.pressedKey = key;
                SetCapture(hwnd);
                invalidateKey(key);
                doAction(action);
                if (g.pressedKey >= (int)g.buttons.size() ||
                    (g.pressedKey >= 0 && g.buttons[g.pressedKey].action != action))
                    g.pressedKey = -1;   // the keypad was rebuilt (2nd, Pro Max)
                return 0;
            }
            g.outputEntry = -1;
            g.outputAnchor = 0;
            g.outputCaret = 0;
            SetFocus(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g.petDragging) {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                g.petX = (int)point.x - g.petDragOffset.x;
                g.petY = (int)point.y - g.petDragOffset.y;
                clampPetToClient();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (!g.trackingMouse) {
                    TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
                    g.trackingMouse = TrackMouseEvent(&track) != FALSE;
                }
                updateHover(point);
            }
            if (g.proMode) {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (handleGraphMouseMove(g.graphState, g.graphRect, (int)point.x, (int)point.y)) {
                    invalidateGraphCache();
                    InvalidateRect(hwnd, &g.graphRect, FALSE);
                    if (g.graphState.isDragging || g.graphState.isRightProbing) return 0;
                }
            }
            if (g.editorSelecting) {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (PtInRect(&g.editorRect, point)) {
                    g.editorCaret = editorCharacterAtPoint(point);
                    InvalidateRect(hwnd, &g.editorRect, FALSE);
                }
                return 0;
            }
            if (g.outputSelecting) {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                int entry = -1;
                int character = 0;
                if (findOutputAtPoint(point, entry, character) && entry == g.outputEntry) {
                    g.outputCaret = character;
                    InvalidateRect(hwnd, &g.historyRect, FALSE);
                }
                return 0;
            }
            break;

        case WM_MOUSELEAVE:
            g.trackingMouse = false;
            updateHover({ -1, -1 });
            return 0;

        case WM_LBUTTONUP:
            if (g.petDragging) {
                g.petDragging = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g.pressedKey >= 0 || g.pressedTop >= 0) {
                releasePressed();
                ReleaseCapture();
                return 0;
            }
            if (g.proMode && g.graphState.isDragging) {
                handleGraphMouseUp(g.graphState);
                invalidateGraphCache();
                ReleaseCapture();
                InvalidateRect(hwnd, &g.graphRect, FALSE);
                return 0;
            }
            if (g.editorSelecting) {
                g.editorSelecting = false;
                ReleaseCapture();
                return 0;
            }
            if (g.outputSelecting) {
                g.outputSelecting = false;
                ReleaseCapture();
                return 0;
            }
            // A press that rebuilt the layout (Sci, 2nd, an "=" that starts
            // Pro Max) or only hit a graph button has nothing above to end,
            // yet still holds the capture. Kept, it would starve the window of
            // WM_SETCURSOR and turn clicks on its frame into client clicks.
            if (GetCapture() == hwnd && !g.graphState.isRightProbing) ReleaseCapture();
            break;

        case WM_RBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (g.proMode && PtInRect(&g.graphRect, pt)) {
                if (handleGraphRightDown(g.graphState, g.graphRect, (int)pt.x, (int)pt.y)) {
                    invalidateGraphCache();
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &g.graphRect, FALSE);
                    return 0;
                }
            }
            break;
        }

        case WM_RBUTTONUP: {
            if (g.proMode && g.graphState.isRightProbing) {
                handleGraphRightUp(g.graphState);
                invalidateGraphCache();
                ReleaseCapture();
                InvalidateRect(hwnd, &g.graphRect, FALSE);
                return 0;
            }
            break;
        }

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED: {
            // SetCapture() while this window already holds the capture reports
            // a change to itself. Nothing was lost, so the press that just
            // began must not be cancelled.
            if (msg == WM_CAPTURECHANGED && (HWND)lParam == hwnd) break;
            releasePressed();
            if (g.petDragging) g.petDragging = false;
            if (g.editorSelecting) g.editorSelecting = false;
            if (g.outputSelecting) g.outputSelecting = false;
            if (g.proMode) {
                if (g.graphState.isDragging) handleGraphMouseUp(g.graphState);
                if (g.graphState.isRightProbing) handleGraphRightUp(g.graphState);
                invalidateGraphCache();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }

        case WM_MOUSEWHEEL: {
            stampInput();
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (g.proMode) {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &point);
                if (PtInRect(&g.graphRect, point)) {
                    handleGraphMouseWheel(g.graphState, g.graphRect, (int)point.x, (int)point.y, (short)delta);
                    invalidateGraphCache();
                    InvalidateRect(hwnd, &g.graphRect, FALSE);
                    return 0;
                }
            }
            g.scrollY -= delta / 2;
            if (g.scrollY < 0) g.scrollY = 0;
            InvalidateRect(hwnd, &g.historyRect, FALSE);
            return 0;
        }

        case WM_CHAR: {
            wchar_t c = (wchar_t)wParam;
            stampInput();
            flashKeyForAction(actionForChar(c));
            if (g.allSelected) {
                if (c == 8) {
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    clearAllSelectedExpression();
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                }
                if ((c >= L'0' && c <= L'9') || c == L'.' || c == L'+' || c == L'-' ||
                    c == L'*' || c == L'/' || c == L'(' || c == L')' || c == L'^' ||
                    c == L'[' || c == L']' || c == L',' || c == L';') {
                    int action = -1;
                    if (c >= L'0' && c <= L'9') action = ActDigit0 + (c - L'0');
                    else if (c == L'.') action = ActDot;
                    else if (c == L'+') action = ActPlus;
                    else if (c == L'-') action = ActMinus;
                    else if (c == L'*') action = ActMul;
                    else if (c == L'/') action = ActFrac;
                    else if (c == L'(') action = ActOpenParen;
                    else if (c == L')') action = ActCloseParen;
                    else if (c == L'[') action = ActOpenBracket;
                    else if (c == L']') action = ActCloseParen;
                    else if (c == L'^') action = ActPower;
                    else if (c == L',') action = ActComma;
                    else if (c == L';') action = ActSemicolon;
                    doAction(action);
                    return 0;
                }
                if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) {
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    clearAllSelectedExpression();
                    insertNameLetter(g.workspace.current(), (char)tolower((char)c));
                    normalizeNames(g.workspace.current());
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                }
                if (c == L'!' || c == L'%' || c == L'=' || c == L'{' || c == L'}') {
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    clearAllSelectedExpression();
                    Expression& current = g.workspace.current();
                    if (c == L'!') insertOperator(current, '!');
                    else if (c == L'%') insertOperator(current, '%');
                    else if (c == L'=') insertEquals(current);
                    else if (c == L'{') insertOpenBrace(current);
                    else insertCloseParen(current);
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                }
            }
            if (g.editorTextCursor >= 0 && !g.allSelected && !g.rangeSelected) {
                std::string text = g.workspace.current().toPlainString();
                int position = currentEditorTextCursor();
                if (c == 8) {
                    if (position > 0) text.erase(position - 1, 1), --position;
                } else if (c == 13) {
                    g.editorTextCursor = -1;
                    doAction(ActEquals);
                    return 0;
                } else if ((c >= L'0' && c <= L'9') || c == L'.' || c == L'+' || c == L'-' ||
                           c == L'*' || c == L'/' || c == L'(' || c == L')' || c == L'^' ||
                           c == L'!' || c == L'=' || c == L'%' || c == L'[' || c == L']' || c == L',' || c == L';' ||
                           c == L'{' || c == L'}' ||
                           ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'))) {
                    text.insert(text.begin() + position, (char)std::tolower((char)c));
                    ++position;
                } else return 0;
                g.undo.push_back(cloneExpression(g.workspace.current()));
                rebuildEditorText(text, position);
                invalidateGraphCache();
                ensureCaretVisible();
                invalidateEdit();
                return 0;
            }
            if (!g.allSelected && (c == L'!' || c == L'=' || c == L'%' || c == L',' || c == L';'))
                g.undo.push_back(cloneExpression(g.workspace.current()));
            if (g.editorCaret != g.editorAnchor) {
                if (c == 8) replaceEditorSelection("");
                else if ((c >= L'0' && c <= L'9') || c == L'.' || c == L'+' || c == L'-' ||
                         c == L'*' || c == L'/' || c == L'(' || c == L')' || c == L'^' ||
                         c == L'!' || c == L'=' || c == L'%' || c == L'[' || c == L']' || c == L',' || c == L';' ||
                         c == L'{' || c == L'}' ||
                         ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'))) {
                    char replacement = (char)c;
                    if (replacement == '/') replacement = '/';
                    if (c != L'!' && c != L'=' && c != L'%' && c != L',' && c != L';')
                        g.undo.push_back(cloneExpression(g.workspace.current()));
                    replaceEditorSelection(std::string(1, replacement));
                } else return 0;
                ensureCaretVisible();
                invalidateEdit();
                return 0;
            }
            if (c == 8) { doAction(ActBackspace); return 0; }
            if (c == 13) { doAction(ActEquals); return 0; }
            if (c >= '0' && c <= '9') { doAction(ActDigit0 + (c - '0')); return 0; }
            // Letters build words ("sin", "pi", "phi", "e", "x", "2p2", "2c2"...).
            // They resolve into Function/Constant/Variable/Permutation/Combination
            // items on normalizeNames().
            if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) {
                g.undo.push_back(cloneExpression(g.workspace.current()));
                insertNameLetter(g.workspace.current(), (char)tolower((char)c));
                normalizeNames(g.workspace.current());
                ensureCaretVisible();
                invalidateEdit();
                return 0;
            }
            switch (c) {
                case L'.': doAction(ActDot); return 0;
                case L'+': doAction(ActPlus); return 0;
                case L'-': doAction(ActMinus); return 0;
                case L'*': doAction(ActMul); return 0;
                case L'/': doAction(ActFrac); return 0;
                case L'(': doAction(ActOpenParen); return 0;
                case L')': doAction(ActCloseParen); return 0;
                case L'[': doAction(ActOpenBracket); return 0;
                case L']': doAction(ActCloseParen); return 0;
                case L'{':
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    insertOpenBrace(g.workspace.current());
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case L'}':
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    insertCloseParen(g.workspace.current());
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case L'\u222A':
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    insertOperator(g.workspace.current(), 'U');
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case L'\u2229':
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    insertOperator(g.workspace.current(), 'I');
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case L'\u0394':
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    insertOperator(g.workspace.current(), 'D');
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case L'\u222B':
                    g.undo.push_back(cloneExpression(g.workspace.current()));
                    insertIntegral(g.workspace.current());
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case L',': doAction(ActComma); return 0;
                case L';': doAction(ActSemicolon); return 0;
                case L'^': doAction(ActPower); return 0;
                case L'!': insertOperator(g.workspace.current(), '!'); ensureCaretVisible(); invalidateEdit(); return 0;
                case L'%': insertOperator(g.workspace.current(), '%'); ensureCaretVisible(); invalidateEdit(); return 0;
                case L'=': insertEquals(g.workspace.current()); ensureCaretVisible(); invalidateEdit(); return 0;
                default: return 0;
            }
        }

        case WM_KEYDOWN: {
            Expression& cur = g.workspace.current();
            bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (control && shiftHeld && wParam == 'P') {
                g.perfOverlay = !g.perfOverlay;
                g.worstInputMs = 0.0;
                InvalidateRect(hwnd, &g.editorRect, FALSE);
                return 0;
            }
            if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU) stampInput();
            if (control && wParam == 'Z') { undoLastEdit(); return 0; }
            if (control && wParam == 'A') {
                g.editorAnchor = 0;
                g.editorCaret = 0;
                g.editorTextCursor = -1;
                g.outputEntry = -1;
                g.outputAnchor = 0;
                g.outputCaret = 0;
                g.allSelected = !rowIsEmpty(cur.root.get());
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (control && wParam == 'C') {
                if (g.outputEntry >= 0 && g.outputAnchor != g.outputCaret) copySelectedOutput();
                else copyCurrentExpression();
                return 0;
            }
            if (control && wParam == 'X') {
                if (g.outputEntry >= 0 && g.outputAnchor != g.outputCaret) return 0;
                copyCurrentExpression();
                if (g.editorCaret != g.editorAnchor) replaceEditorSelection("");
                else if (g.rangeSelected) deleteRange(cur, g.selectionAnchor, cur.cursor);
                else cur = Expression();
                g.allSelected = false;
                g.rangeSelected = false;
                invalidateEdit();
                return 0;
            }
            if (control && wParam == 'V') { pasteExpression(); return 0; }
            switch (wParam) {
                case VK_LEFT:
                case VK_RIGHT: {
                    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (shift) {
                        int position = currentEditorTextCursor();
                        if (!g.editorCaret && !g.editorAnchor) g.editorAnchor = position;
                        position += wParam == VK_LEFT ? -1 : 1;
                        g.editorCaret = std::clamp(position, 0, (int)cur.toPlainString().size());
                        g.editorTextCursor = -1;
                        g.rangeSelected = false;
                        g.allSelected = false;
                    } else {
                        // Move the real structural cursor (the same one used
                        // by button/mouse editing) instead of a parallel
                        // flat-text cursor. The flat-text cursor used to
                        // reparse the whole expression from a plain string on
                        // every subsequent keystroke, which could not
                        // faithfully round-trip parens/powers typed mid-
                        // expression and produced mismatched-looking output.
                        if (wParam == VK_LEFT) moveLeft(cur); else moveRight(cur);
                        g.editorTextCursor = -1;
                        g.editorAnchor = g.editorCaret = 0;
                        g.rangeSelected = false;
                        g.allSelected = false;
                    }
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                }
                case VK_UP:
                    // Structural navigation takes priority: if the cursor
                    // is inside a denominator/exponent, Up should move it
                    // to the numerator/base above, matching natural
                    // calculators. Only fall back to recalling a previous
                    // history entry when there's nowhere left to navigate
                    // to inside the current expression -- otherwise Up
                    // could never reach a numerator once you'd arrowed
                    // down into a denominator, and would instead silently
                    // replace what you were typing with old history.
                    if (!moveUp(cur)) g.workspace.recallPrevious();
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case VK_HOME:
                    if (GetKeyState(VK_SHIFT) & 0x8000) {
                        int position = currentEditorTextCursor();
                        if (g.editorCaret == g.editorAnchor) g.editorAnchor = position;
                        g.editorCaret = 0;
                        g.editorTextCursor = -1;
                    } else {
                        cur.cursor.index = 0;
                        g.editorTextCursor = -1;
                        g.editorAnchor = g.editorCaret = 0;
                    }
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case VK_END:
                    if (GetKeyState(VK_SHIFT) & 0x8000) {
                        int position = currentEditorTextCursor();
                        if (g.editorCaret == g.editorAnchor) g.editorAnchor = position;
                        g.editorCaret = (int)cur.toPlainString().size();
                        g.editorTextCursor = -1;
                    } else {
                        cur.cursor.index = (int)cur.cursor.row->items.size();
                        g.editorTextCursor = -1;
                        g.editorAnchor = g.editorCaret = 0;
                    }
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                case VK_DOWN: moveDown(cur); ensureCaretVisible(); invalidateEdit(); return 0;
                case VK_DELETE: {
                    DWORD now = GetTickCount();
                    if (now - g.lastDeleteTick <= 600) {
                        g.workspace.clearVariables();
                        g.values = EvaluationContext{};
                        g.lastDeleteTick = 0;
                    } else {
                        doDelete(cur);
                        g.lastDeleteTick = now;
                    }
                    ensureCaretVisible();
                    invalidateEdit();
                    return 0;
                }
                case VK_ESCAPE:
                    if (g.allSelected) {
                        g.allSelected = false;
                        InvalidateRect(hwnd, &g.editorRect, FALSE);
                    } else {
                        flashKeyForAction(ActClear);
                        doAction(ActClear);
                    }
                    return 0;
                default: return 0;
            }
        }

        case WM_DESTROY:
            KillTimer(hwnd, kCaretTimerId);
            KillTimer(hwnd, kProAnimationTimerId);
            releaseProExplosionAsset();
            releaseProBrickAsset();
            cleanupRenderCaches();
            shutdownGpuGraph();
            if (g.appIcon) DestroyIcon(g.appIcon);
            if (g.uiFont) DeleteObject(g.uiFont);
            if (g.uiFontSmall) DeleteObject(g.uiFontSmall);
            g.fonts.destroy();
            shutdownFonts();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g.dark = loadDarkMode();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint everything ourselves
    wc.lpszClassName = L"NaturalCalcWindowClass";
    g.appIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  32, 32, LR_DEFAULTSIZE);
    if (!g.appIcon) g.appIcon = createAppIcon();
    wc.hIcon = g.appIcon;
    wc.hIconSm = g.appIcon;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Natural Calculator",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 640,
        nullptr, nullptr, hInstance, nullptr);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g.appIcon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g.appIcon);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
