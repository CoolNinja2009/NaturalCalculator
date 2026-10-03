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
#include "expr_tree.h"
#include "layout.h"
#include "evaluator.h"
#include "workspace.h"
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
    ActOpenParen, ActCloseParen, ActPower, ActSqrt,
    ActEquals, ActClear, ActClearAll, ActToggleTheme, ActBackspace,
    ActVariableX, ActVariableY, ActQuadratic
};

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
    DWORD proModeStarted = 0;
    DWORD explosionStarted = 0;
    bool explosionActive = false;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::Image* explosionImage = nullptr;
    IStream* explosionStream = nullptr;
    UINT explosionFrameCount = 0;
    int petX = -1;
    int petY = -1;
    bool petDragging = false;
    POINT petDragOffset{};
    std::vector<ButtonDef> buttons;
    RECT topBarRect{}, historyRect{}, editorRect{}, buttonAreaRect{};
    HFONT uiFont = nullptr;
    HFONT uiFontSmall = nullptr;
    HICON appIcon = nullptr;
    std::vector<std::unique_ptr<Expression>> undo;
} g;

constexpr UINT_PTR kCaretTimerId = 1;
constexpr UINT_PTR kProAnimationTimerId = 2;
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

void releaseProExplosionAsset() {
    delete g.explosionImage;
    g.explosionImage = nullptr;
    if (g.explosionStream) {
        g.explosionStream->Release();
        g.explosionStream = nullptr;
    }
    if (g.gdiplusToken) {
        Gdiplus::GdiplusShutdown(g.gdiplusToken);
        g.gdiplusToken = 0;
    }
}

// ------------------------------------------------------------- layout

void layoutButtons(int areaLeft, int areaTop, int areaW, int areaH) {
    g.buttons.clear();
    const int cols = 4;
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
    place(0, 0, 2, L"AC", ActClearAll, false, false, false, true);
    place(2, 0, 2, L"DEL", ActBackspace, false, false, true);

    place(0, 1, 1, L"(", ActOpenParen, false, false, true);
    place(1, 1, 1, L")", ActCloseParen, false, false, true);
    place(2, 1, 1, L"x^y", ActPower, false, false, true);
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

    place(0, 6, 4, L"=", ActEquals, false, true);
}

void recomputeLayout(const RECT& rc) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    int topBarH = 40;
    int editorH = std::max(70, h / 6);
    int buttonAreaH = std::min(420, std::max(300, h * 1 / 2));

    g.topBarRect = { 0, 0, w, topBarH };
    g.buttonAreaRect = { 0, h - buttonAreaH, w, h };
    g.editorRect = { 0, g.buttonAreaRect.top - editorH, w, g.buttonAreaRect.top };
    g.historyRect = { 0, topBarH, w, g.editorRect.top };

    layoutButtons(g.buttonAreaRect.left, g.buttonAreaRect.top,
                  w, g.buttonAreaRect.bottom - g.buttonAreaRect.top);
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
    RECT client{};
    GetClientRect(g.hwnd, &client);
    g.proMode = true;
    g.proModeStarted = GetTickCount();
    g.explosionStarted = g.proModeStarted;
    g.explosionActive = true;
    g.petX = std::max(8, (int)client.right - 62);
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
    if (action != ActToggleTheme && action != ActEquals)
        g.undo.push_back(cloneExpression(g.workspace.current()));
    bool editsExpression = action != ActToggleTheme && action != ActEquals;
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
        case ActPower: insertPower(cur); break;
        case ActSqrt: insertSqrt(cur); break;
        case ActBackspace: backspace(cur); break;
        case ActEquals: {
            bool proTrigger = isProModeTrigger(cur.root.get());
            if (g.workspace.commitCurrent(g.values)) {
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
            break;
        default: break;
    }
    ensureCaretVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
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
    for (char character : text) {
        if (character >= '0' && character <= '9') insertDigit(expression, character);
        else if (character == 'x' || character == 'y') insertVariable(expression, character);
        else if (character == '.') insertDigit(expression, character);
        else if (character == '+' || character == '-' || character == '*' || character == '!')
            insertOperator(expression, character);
        else if (character == '/') insertFraction(expression);
        else if (character == '=') insertEquals(expression);
        else if (character == '(') insertOpenParen(expression);
        else if (character == ')') insertCloseParen(expression);
        else if (character == '^') insertPower(expression);
    }
}

void replaceEditorSelection(const std::string& replacement) {
    Expression& expression = g.workspace.current();
    std::string plain = expression.toPlainString();
    int begin = std::min(g.editorAnchor, g.editorCaret);
    int end = std::max(g.editorAnchor, g.editorCaret);
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

bool findOutputAtPoint(POINT point, int& entryIndex, int& characterIndex) {
    HDC hdc = GetDC(g.hwnd);
    if (!hdc) return false;
    const auto& history = g.workspace.history();
    int pad = 14;
    int entrySpacing = 10;
    int y = g.historyRect.top + pad - g.scrollY;
    for (size_t i = 0; i < history.size(); ++i) {
        Size size = measureExpression(hdc, history[i]->expr->root.get());
        int resultTop = y + size.height() + 2;
        RECT resultRect = { g.historyRect.left + pad, resultTop,
                            g.historyRect.right - pad, resultTop + 22 };
        if (PtInRect(&resultRect, point)) {
            std::wstring text = outputText((int)i);
            HFONT old = (HFONT)SelectObject(hdc, g.uiFontSmall);
            int index = 0;
            int bestDistance = INT_MAX;
            for (int candidate = 0; candidate <= (int)text.size(); ++candidate) {
                SIZE prefix{};
                GetTextExtentPoint32W(hdc, text.c_str(), candidate, &prefix);
                int distance = std::abs(point.x - (resultRect.left + prefix.cx));
                if (distance < bestDistance) { bestDistance = distance; index = candidate; }
            }
            SelectObject(hdc, old);
            ReleaseDC(g.hwnd, hdc);
            entryIndex = (int)i;
            characterIndex = index;
            return true;
        }
        y += size.height() + entrySpacing + 22 + entrySpacing;
    }
    ReleaseDC(g.hwnd, hdc);
    return false;
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
    std::string pasted;
    for (const wchar_t* p = text; *p; ++p) pasted += (char)std::tolower((char)*p);
    if (replacingEditorSelection) replaceEditorSelection(pasted);
    else insertPlainText(expression, pasted);
    GlobalUnlock(data);
    CloseClipboard();
    g.allSelected = false;
    g.rangeSelected = false;
    ensureCaretVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

RECT topActionRect(int right, int width) {
    RECT r = g.topBarRect;
    r.left = right - width;
    r.right = right;
    r.top += 6;
    r.bottom -= 6;
    return r;
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

RECT topActionRect(int right, int width);

RECT themeToggleRect() {
    return topActionRect(g.topBarRect.right - 8, 52);
}

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
    target.background = RGB(0x09, 0x03, 0x06);
    target.panelBackground = RGB(0x21, 0x06, 0x0C);
    target.text = RGB(0xF7, 0xE8, 0xEB);
    target.operatorColor = RGB(0xFF, 0x47, 0x62);
    target.resultColor = RGB(0xFF, 0x4D, 0x69);
    target.caret = RGB(0xFF, 0xB5, 0xC0);
    target.placeholder = RGB(0x67, 0x2B, 0x38);
    target.divider = RGB(0x6D, 0x13, 0x27);
    target.accent = RGB(0xD9, 0x10, 0x32);
    target.screenBackground = RGB(0x13, 0x04, 0x08);
    target.screenText = RGB(0xFF, 0xE9, 0xED);
    target.screenOperator = RGB(0xFF, 0x65, 0x7A);
    target.screenResult = RGB(0xFF, 0x58, 0x75);
    target.screenCaret = RGB(0xFF, 0xD7, 0xDD);
    target.screenPlaceholder = RGB(0x6D, 0x36, 0x43);

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

    POINT leftWing[] = { { x + 17, y + 32 }, { x + 2, y + 26 }, { x + 9, y + 38 },
                         { x + 3, y + 44 }, { x + 18, y + 46 } };
    POINT rightWing[] = { { x + 34, y + 31 }, { x + 49, y + 25 }, { x + 43, y + 38 },
                          { x + 51, y + 44 }, { x + 33, y + 46 } };
    drawPetShape(hdc, leftWing, 5, deepRed, black);
    drawPetShape(hdc, rightWing, 5, deepRed, black);

    POINT leftHorn[] = { { x + 12, y + 17 }, { x + 7, y - 1 }, { x + 22, y + 11 } };
    POINT rightHorn[] = { { x + 31, y + 11 }, { x + 47, y - 2 }, { x + 40, y + 20 } };
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
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
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
    for (int ray = 0; ray < 22; ++ray) {
        double angle = ray * 6.283185307179586 / 22.0;
        double inner = maxDimension * (0.035 + rayProgress * 0.13);
        double outer = maxDimension * (0.18 + rayProgress * 0.82);
        int x1 = centerX + (int)(std::cos(angle) * inner);
        int y1 = centerY + (int)(std::sin(angle) * inner);
        int x2 = centerX + (int)(std::cos(angle) * outer);
        int y2 = centerY + (int)(std::sin(angle) * outer);
        COLORREF rayColor = (ray % 3 == 0) ? RGB(0xFF, 0xD9, 0xA0) :
                            blendColor(RGB(0xFF, 0x56, 0x68), theme.accent, rayProgress);
        HPEN pen = CreatePen(PS_SOLID, ray % 3 == 0 ? 2 : 1, rayColor);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

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
        COLORREF sparkColor = spark % 4 == 0 ? RGB(0xFF, 0xF0, 0xC5) :
                              (spark % 3 == 0 ? RGB(0xFF, 0x8B, 0x45) : RGB(0xFF, 0x2A, 0x48));
        HPEN pen = CreatePen(PS_SOLID, spark % 5 == 0 ? 4 : 2, sparkColor);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

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
            drawPetShape(hdc, shard, 3, sparkColor, RGB(0xFF, 0xD6, 0xA1));
        }
    }
}

void paintButton(HDC hdc, const ButtonDef& b, const Theme& theme) {
    // Casio-style keycap palette: cream digit keys, dark slate function
    // keys, an orange operator column + "=" key, and a red AC key -- the
    // classic desk-calculator color coding instead of one flat button style.
    COLORREF fill;
    COLORREF fg;
    if (b.isDanger) { fill = theme.isDark ? RGB(0xB0, 0x36, 0x27) : RGB(0xC7, 0x3E, 0x2A); fg = RGB(255, 255, 255); }
    else if (b.isAccent) { fill = theme.accent; fg = RGB(255, 255, 255); }
    else if (b.isOperator) { fill = theme.accent; fg = RGB(255, 255, 255); }
    else if (b.isFunction) { fill = theme.isDark ? RGB(0x3A, 0x3D, 0x45) : RGB(0x50, 0x54, 0x5C); fg = RGB(0xF2, 0xF2, 0xF2); }
    else { fill = theme.isDark ? RGB(0x4E, 0x51, 0x58) : RGB(0xF9, 0xF7, 0xF2); fg = theme.isDark ? RGB(0xF2, 0xF2, 0xF2) : RGB(0x24, 0x24, 0x24); }

    double proProgress = proTransitionProgress();
    if (g.proMode) {
        COLORREF infernalFill = b.isDanger ? RGB(0xA6, 0x06, 0x22) :
                                (b.isAccent || b.isOperator) ? RGB(0xDA, 0x0A, 0x31) :
                                b.isFunction ? RGB(0x35, 0x06, 0x10) : RGB(0x25, 0x07, 0x0D);
        fill = blendColor(fill, infernalFill, proProgress);
        fg = blendColor(fg, RGB(0xFF, 0xE5, 0xE0), proProgress);
    }

    int radius = 6;

    // Drop shadow: a slightly-inset darker copy offset down-right so the
    // key reads as sitting proud of the calculator body.
    HBRUSH shadowBrush = CreateSolidBrush(shade(theme.background, theme.isDark ? -18 : -35));
    HBRUSH oldShadowBrush = (HBRUSH)SelectObject(hdc, shadowBrush);
    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
    HPEN oldPen0 = (HPEN)SelectObject(hdc, nullPen);
    RoundRect(hdc, b.rect.left + 1, b.rect.top + 2, b.rect.right + 1, b.rect.bottom + 2, radius, radius);
    SelectObject(hdc, oldShadowBrush);
    SelectObject(hdc, oldPen0);
    DeleteObject(shadowBrush);

    // The keycap itself.
    HBRUSH brush = CreateSolidBrush(fill);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, shade(fill, theme.isDark ? -35 : -45));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, b.rect.left, b.rect.top, b.rect.right, b.rect.bottom, radius, radius);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    if (g.proMode && proProgress > 0.2) {
        int saved = SaveDC(hdc);
        HRGN keyClip = CreateRoundRectRgn(b.rect.left + 1, b.rect.top + 1,
                                          b.rect.right, b.rect.bottom,
                                          radius, radius);
        SelectClipRgn(hdc, keyClip);
        int keyWidth = (int)(b.rect.right - b.rect.left);
        int keyHeight = (int)(b.rect.bottom - b.rect.top);
        int veinCount = std::max(2, keyWidth / 15);
        COLORREF veinColor = blendColor(fill, RGB(0x90, 0x08, 0x20), 0.82);
        COLORREF emberColor = blendColor(fill, RGB(0xFF, 0x43, 0x32), 0.64);
        HPEN veinPen = CreatePen(PS_SOLID, 1, veinColor);
        HPEN emberPen = CreatePen(PS_SOLID, 1, emberColor);
        for (int vein = 0; vein < veinCount; ++vein) {
            int startX = (int)b.rect.left + 5 + (vein * 17) % std::max(1, keyWidth - 10);
            int startY = (int)b.rect.top + 4 + (vein * 7) % std::max(1, keyHeight - 8);
            HPEN activePen = vein % 3 == 0 ? emberPen : veinPen;
            HPEN oldTexturePen = (HPEN)SelectObject(hdc, activePen);
            MoveToEx(hdc, startX, startY, nullptr);
            LineTo(hdc, startX + 4, startY + 4);
            LineTo(hdc, startX + 1, startY + 9);
            LineTo(hdc, startX + 6, startY + 14);
            if (vein % 2 == 0) {
                MoveToEx(hdc, startX + 2, startY + 7, nullptr);
                LineTo(hdc, startX - 3, startY + 11);
            }
            SelectObject(hdc, oldTexturePen);
        }
        DeleteObject(veinPen);
        DeleteObject(emberPen);

        HBRUSH fleckBrush = CreateSolidBrush(RGB(0xFF, 0x71, 0x3E));
        HPEN oldTexturePen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        HBRUSH oldFleckBrush = (HBRUSH)SelectObject(hdc, fleckBrush);
        for (int fleck = 0; fleck < 3; ++fleck) {
            int px = (int)b.rect.left + 7 + (fleck * 31 + b.action * 3) %
                     std::max(1, keyWidth - 12);
            int py = (int)b.rect.top + 5 + (fleck * 13 + b.action * 7) %
                     std::max(1, keyHeight - 10);
            Ellipse(hdc, px, py, px + 2, py + 2);
        }
        SelectObject(hdc, oldFleckBrush);
        SelectObject(hdc, oldTexturePen);
        DeleteObject(fleckBrush);
        RestoreDC(hdc, saved);
        DeleteObject(keyClip);
    }

    // Bevel highlight along the top edge for a subtle raised look.
    HPEN hi = CreatePen(PS_SOLID, 1, shade(fill, theme.isDark ? 25 : 40));
    HPEN oldHi = (HPEN)SelectObject(hdc, hi);
    MoveToEx(hdc, b.rect.left + radius, b.rect.top + 2, nullptr);
    LineTo(hdc, b.rect.right - radius, b.rect.top + 2);
    SelectObject(hdc, oldHi);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
    DeleteObject(hi);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HFONT old = (HFONT)SelectObject(hdc, g.uiFont);
    RECT r = b.rect;
    DrawTextW(hdc, b.label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
}

void paint(HDC hdc, RECT client) {
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

    HDC mem = CreateCompatibleDC(hdc);
    if (!mem) {
        HBRUSH fallback = CreateSolidBrush(theme.background);
        FillRect(hdc, &client, fallback);
        DeleteObject(fallback);
        return;
    }
    HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right - client.left,
                                        client.bottom - client.top);
    if (!bmp) {
        DeleteDC(mem);
        HBRUSH fallback = CreateSolidBrush(theme.background);
        FillRect(hdc, &client, fallback);
        DeleteObject(fallback);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    if (!oldBmp || oldBmp == HGDI_ERROR) {
        DeleteObject(bmp);
        DeleteDC(mem);
        HBRUSH fallback = CreateSolidBrush(theme.background);
        FillRect(hdc, &client, fallback);
        DeleteObject(fallback);
        return;
    }

    HBRUSH bgBrush = CreateSolidBrush(theme.background);
    FillRect(mem, &client, bgBrush);
    DeleteObject(bgBrush);

    // --- top bar ---
    HBRUSH panelBrush = CreateSolidBrush(theme.panelBackground);
    FillRect(mem, &g.topBarRect, panelBrush);
    DeleteObject(panelBrush);
    {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, theme.isDark ? RGB(0xF0, 0xF0, 0xF0) : RGB(0x2A, 0x2A, 0x2A));
        HFONT old = (HFONT)SelectObject(mem, g.uiFont);
        RECT r = g.topBarRect; r.left += 12;
        DrawTextW(mem, g.proMode ? L"Calc Pro Max" : L"Natural Calculator",
              -1, &r, DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, old);
    }
    // Small slate pill buttons -- same family as the dark function keycaps
    // below, so the whole chrome reads as one coherent design instead of
    // a row of near-invisible white-on-white outlined boxes.
    auto drawPill = [&](RECT rect, const wchar_t* label) {
        COLORREF regularFill = theme.isDark ? RGB(0x3A, 0x3D, 0x45) : RGB(0x50, 0x54, 0x5C);
        COLORREF fill = g.proMode ? blendColor(regularFill, theme.accent, proProgress) : regularFill;
        HBRUSH b = CreateSolidBrush(fill);
        HBRUSH ob = (HBRUSH)SelectObject(mem, b);
        HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
        HPEN op = (HPEN)SelectObject(mem, nullPen);
        RoundRect(mem, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
        SelectObject(mem, ob); SelectObject(mem, op);
        DeleteObject(b);
        SetTextColor(mem, RGB(0xF2, 0xF2, 0xF2));
        HFONT old = (HFONT)SelectObject(mem, g.uiFontSmall);
        DrawTextW(mem, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, old);
    };
    {
        RECT xr = topActionRect(g.topBarRect.right - 68, 28);
        RECT yr = topActionRect(g.topBarRect.right - 100, 28);
        RECT qr = topActionRect(g.topBarRect.right - 132, 76);
        drawPill(xr, L"x");
        drawPill(yr, L"y");
        drawPill(qr, L"Quadratic");
    }
    drawPill(themeToggleRect(), g.dark ? L"Light" : L"Dark");
    HPEN divPen = CreatePen(PS_SOLID, 1, theme.divider);
    HPEN oldPen = (HPEN)SelectObject(mem, divPen);
    MoveToEx(mem, 0, g.topBarRect.bottom, nullptr);
    LineTo(mem, client.right, g.topBarRect.bottom);
    SelectObject(mem, oldPen);
    DeleteObject(divPen);

    // --- history (scrollable) ---
    {
        HBRUSH screenBrush = CreateSolidBrush(theme.screenBackground);
        FillRect(mem, &g.historyRect, screenBrush);
        DeleteObject(screenBrush);

        HRGN clip = CreateRectRgnIndirect(&g.historyRect);
        SelectClipRgn(mem, clip);

        const auto& hist = g.workspace.history();
        int pad = 14;
        int entrySpacing = 10;

        // First pass: compute total content height so we can clamp scroll.
        int totalH = 0;
        std::vector<Size> sizes(hist.size());
        for (size_t i = 0; i < hist.size(); ++i) {
            sizes[i] = measureExpression(mem, hist[i]->expr->root.get());
            totalH += sizes[i].height() + entrySpacing + 22 /* result line */ + entrySpacing;
        }
        int viewH = g.historyRect.bottom - g.historyRect.top;
        int maxScroll = std::max(0, totalH - viewH + pad);
        if (g.scrollToBottomPending) { g.scrollY = maxScroll; g.scrollToBottomPending = false; }
        g.scrollY = std::clamp(g.scrollY, 0, maxScroll);

        int y = g.historyRect.top + pad - g.scrollY;
        for (size_t i = 0; i < hist.size(); ++i) {
            const Size& s = sizes[i];
            int baseline = y + s.ascent;
            if (baseline + s.descent >= g.historyRect.top && baseline - s.ascent <= g.historyRect.bottom) {
                drawExpression(mem, hist[i]->expr->root.get(), g.historyRect.left + pad, baseline,
                                screenTheme, nullptr, nullptr);

                HFONT old = (HFONT)SelectObject(mem, g.uiFontSmall);
                SetTextColor(mem, hist[i]->isError ? RGB(0xD8, 0x3B, 0x3B) : screenTheme.resultColor);
                SetBkMode(mem, TRANSPARENT);
                std::wstring res = L"= " + wideResult(hist[i]->result);
                RECT rr = { g.historyRect.left + pad, y + s.height() + 2,
                            g.historyRect.right - pad, y + s.height() + 24 };
                int selectionBegin = -1;
                int selectionEnd = -1;
                if (g.outputEntry == (int)i && g.outputAnchor != g.outputCaret) {
                    selectionBegin = std::min(g.outputAnchor, g.outputCaret);
                    selectionEnd = std::max(g.outputAnchor, g.outputCaret);
                    SIZE before{}, selected{};
                    GetTextExtentPoint32W(mem, res.c_str(), selectionBegin, &before);
                    GetTextExtentPoint32W(mem, res.c_str() + selectionBegin,
                                          selectionEnd - selectionBegin, &selected);
                    RECT highlight = { rr.left + before.cx, rr.top,
                                       rr.left + before.cx + selected.cx, rr.bottom };
                    HBRUSH highlightBrush = CreateSolidBrush(theme.isDark ? RGB(0x16, 0x4E, 0x73) : RGB(0xC7, 0xE8, 0xFF));
                    FillRect(mem, &highlight, highlightBrush);
                    DeleteObject(highlightBrush);
                }
                DrawTextW(mem, res.c_str(), -1, &rr, DT_LEFT | DT_SINGLELINE);
                SelectObject(mem, old);
            }
            y += s.height() + entrySpacing + 22 + entrySpacing;
        }
        SelectClipRgn(mem, nullptr);
        DeleteObject(clip);
    }

    // --- editor (current expression) ---
    {
        HBRUSH b = CreateSolidBrush(theme.screenBackground);
        FillRect(mem, &g.editorRect, b);
        DeleteObject(b);
        HPEN p = CreatePen(PS_SOLID, 1, shade(theme.screenBackground, theme.isDark ? 20 : -30));
        HPEN op = (HPEN)SelectObject(mem, p);
        MoveToEx(mem, 0, g.editorRect.top, nullptr);
        LineTo(mem, client.right, g.editorRect.top);
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
                int origin = g.editorRect.left + 16;
                selectionLeft = origin + beforeSize.cx;
                selectionRight = selectionLeft + selectedSize.cx;
                SelectObject(measure, oldFont);
            }
            RECT selection = { selectionLeft, baseline - s.ascent - 4,
                               selectionRight, baseline + s.descent + 4 };
            HBRUSH selectionBrush = CreateSolidBrush(theme.isDark ? RGB(0x16, 0x4E, 0x73) : RGB(0xC7, 0xE8, 0xFF));
            FillRect(mem, &selection, selectionBrush);
            DeleteObject(selectionBrush);
        }
        CaretInfo caret;
        drawExpression(mem, cur.root.get(), g.editorRect.left + 16, baseline, screenTheme, &cur.cursor, &caret);

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

    // --- button panel ---
    {
        HBRUSH b = CreateSolidBrush(theme.background);
        FillRect(mem, &g.buttonAreaRect, b);
        DeleteObject(b);
        for (auto& btn : g.buttons) paintButton(mem, btn, theme);
    }

    if (g.proMode && proProgress > 0.16) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(g.hwnd, &cursor);
        drawProPet(mem, g.petX, g.petY + petBounceOffset(), cursor);
    }
    drawProExplosion(mem, client, theme);

    BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ------------------------------------------------------------- WndProc

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            loadProExplosionAsset();
            setDarkTitleBar(hwnd, g.dark);
            g.uiFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g.uiFontSmall = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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
            mmi->ptMinTrackSize.x = kMinWidth;
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
                recomputeLayout(client);
                paint(hdc, client);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kCaretTimerId) {
                g.caretVisible = !g.caretVisible;
                InvalidateRect(hwnd, &g.editorRect, FALSE);
            } else if (wParam == kProAnimationTimerId && g.proMode) {
                clampPetToClient();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) return DefWindowProcW(hwnd, msg, wParam, lParam);
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
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
            RECT currentPetBounds = petBounds();
            if (g.proMode && proTransitionProgress() > 0.16 && PtInRect(&currentPetBounds, pt)) {
                g.petDragging = true;
                g.petDragOffset = { pt.x - currentPetBounds.left, pt.y - currentPetBounds.top };
                SetCapture(hwnd);
                return 0;
            }
            RECT toggleRect = themeToggleRect();
            RECT xRect = topActionRect(g.topBarRect.right - 68, 28);
            RECT yRect = topActionRect(g.topBarRect.right - 100, 28);
            RECT quadraticRect = topActionRect(g.topBarRect.right - 132, 76);
            if (PtInRect(&toggleRect, pt)) { doAction(ActToggleTheme); return 0; }
            if (PtInRect(&xRect, pt)) { doAction(ActVariableX); return 0; }
            if (PtInRect(&yRect, pt)) { doAction(ActVariableY); return 0; }
            if (PtInRect(&quadraticRect, pt)) { solveQuadraticFromDialog(); return 0; }
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
            for (auto& b : g.buttons) {
                if (PtInRect(&b.rect, pt)) { doAction(b.action); return 0; }
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

        case WM_LBUTTONUP:
            if (g.petDragging) {
                g.petDragging = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
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
            break;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            g.scrollY -= delta / 2;
            if (g.scrollY < 0) g.scrollY = 0;
            InvalidateRect(hwnd, &g.historyRect, FALSE);
            return 0;
        }

        case WM_CHAR: {
            wchar_t c = (wchar_t)wParam;
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
                           c == L'!' || c == L'=' || c == L'x' || c == L'X' || c == L'y' || c == L'Y') {
                    text.insert(text.begin() + position, (char)std::tolower((char)c));
                    ++position;
                } else return 0;
                g.undo.push_back(cloneExpression(g.workspace.current()));
                rebuildEditorText(text, position);
                ensureCaretVisible();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (c == L'!' || c == L'=')
                g.undo.push_back(cloneExpression(g.workspace.current()));
            if (g.editorCaret != g.editorAnchor) {
                if (c == 8) replaceEditorSelection("");
                else if ((c >= L'0' && c <= L'9') || c == L'.' || c == L'+' || c == L'-' ||
                         c == L'*' || c == L'/' || c == L'(' || c == L')' || c == L'^' ||
                         c == L'!' || c == L'=' || c == L'x' || c == L'X' || c == L'y' || c == L'Y') {
                    char replacement = (char)c;
                    if (replacement == '/') replacement = '/';
                    replaceEditorSelection(std::string(1, replacement));
                } else return 0;
                ensureCaretVisible();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (c == 8) { doAction(ActBackspace); return 0; }
            if (c == 13) { doAction(ActEquals); return 0; }
            if (c >= '0' && c <= '9') { doAction(ActDigit0 + (c - '0')); return 0; }
            switch (c) {
                case L'.': doAction(ActDot); return 0;
                case L'+': doAction(ActPlus); return 0;
                case L'-': doAction(ActMinus); return 0;
                case L'*': doAction(ActMul); return 0;
                case L'/': doAction(ActFrac); return 0;
                case L'(': doAction(ActOpenParen); return 0;
                case L')': doAction(ActCloseParen); return 0;
                case L'^': doAction(ActPower); return 0;
                case L'!': insertOperator(g.workspace.current(), '!'); ensureCaretVisible(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
                case L'=': insertEquals(g.workspace.current()); ensureCaretVisible(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
                case L'x': case L'X': doAction(ActVariableX); return 0;
                case L'y': case L'Y': doAction(ActVariableY); return 0;
                default: return 0;
            }
        }

        case WM_KEYDOWN: {
            Expression& cur = g.workspace.current();
            bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
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
                InvalidateRect(hwnd, nullptr, FALSE);
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
                    InvalidateRect(hwnd, nullptr, FALSE);
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
                    InvalidateRect(hwnd, nullptr, FALSE);
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
                    InvalidateRect(hwnd, nullptr, FALSE);
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
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case VK_DOWN: moveDown(cur); ensureCaretVisible(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
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
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                case VK_ESCAPE:
                    if (g.allSelected) {
                        g.allSelected = false;
                        InvalidateRect(hwnd, &g.editorRect, FALSE);
                    } else {
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
            if (g.appIcon) DestroyIcon(g.appIcon);
            if (g.uiFont) DeleteObject(g.uiFont);
            if (g.uiFontSmall) DeleteObject(g.uiFontSmall);
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
