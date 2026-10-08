# Natural Calculator

A native, dependency-free Windows calculator: Windows-Calculator-style UI,
Casio fx-991ES-Plus-style **structural** math input (real fraction stacks,
exponents, radicals — not a text box), and a multiline calculation
workspace. Pure Win32 + GDI, C++17, no MFC, no .NET, no Electron, no
network access.

## Why it starts fast

- Only links `user32.dll`, `gdi32.dll`, `kernel32.dll`, `msvcrt.dll` —
  DLLs already resident in every Windows process. Nothing extra to fault in.
- No common controls, no RichEdit, no XAML/WinRT activation. One window
  class, one `WndProc`, everything else is hand-rolled GDI.
- Statically linked C/C++ runtime (`-static -static-libgcc
  -static-libstdc++`) — no MSVCRT redistributable hunting, no DLL search
  path resolution at launch.
- Stripped binary (~250 KB).
- No background threads, no timers except a single 530ms caret blink, no
  network calls of any kind.

On real Windows hardware this class of app (single `CreateWindowExW` +
GDI paint, static CRT) typically shows its first frame in single-digit
milliseconds; the ≈100ms budget is mostly OS process-creation overhead
you don't control, not app code. I built and cross-compiled this in a
Linux sandbox and verified it produces a clean, warning-free PE64 binary
with the minimal import table above — I could not run/profile it on
actual Windows here, so treat "instant" as an architectural property
(verified: tiny import table, no heavy subsystems) rather than a
benchmarked number until you've run it on your machine.

## Architecture

```
Keyboard / Button Input
        |
        v
Structured Expression Editor   (expr_tree.h/.cpp)
        |
        v
Expression Tree (Row/Item model)
        |
        +---> Renderer   (layout.h/.cpp)   — GDI typesetting
        +---> Evaluator  (evaluator.h/.cpp) — BODMAS-correct recursive descent
```

**The expression is never a string.** It's a `Row` — an ordered list of
`Item`s (numbers, operators, or *structural* items). A structural item
(Fraction, Power, Sqrt, Paren) owns one or two child `Row`s of its own
(numerator/denominator, base/exponent, radicand, inner). This is the same
"list of boxes" model used by Casio's Natural Display and by MathQuill.
The renderer and the evaluator both walk this one tree — there's no
separate parse step and no string round-tripping.

- `expr_tree.h/.cpp` — the model, plus all editing operations: digit/op
  insertion, `/` → builds a Fraction (consuming the atom to its left as
  the numerator, exactly like typing `1 + 2/3` gives `1 + ²⁄₃`, not
  `(1+2)/3`), `^` → Power, √ → Sqrt, `(` / `)` → Paren, and
  structure-aware cursor motion (left/right walk into/out of structures;
  up/down hop between numerator↔denominator and base↔exponent) and
  structure-aware backspace/delete (never corrupts the tree — deleting
  into a non-empty structure "enters" it first rather than eating its
  contents; an empty structure is removed outright).
- `layout.h/.cpp` — a small recursive typesetter: every item reports
  `(width, ascent, descent)` around a shared baseline, exactly like real
  text layout, so fractions/exponents/radicals interleave correctly with
  plain numbers and with each other at arbitrary nesting depth. Same pass
  also locates the caret's pixel position for the given tree cursor, so
  rendering and caret placement can never drift out of sync.
- `evaluator.h/.cpp` — recursive-descent parser over the *tree* (not a
  string) with standard `+ -` / `* implicit-adjacency` precedence.
  Division never appears as a flat operator — a Fraction node *is* the
  division, so its precedence is automatically correct (as tightly bound
  as whatever the user wrapped, exactly like on paper).
- `workspace.h/.cpp` — history of completed (expression tree, result)
  pairs plus the live editable expression; each history entry keeps its
  full structured tree so past calculations still render as real math.
- `main.cpp` — the Win32 shell: window, custom-drawn button grid,
  keyboard routing (`WM_CHAR` for digits/operators/Enter/Backspace,
  `WM_KEYDOWN` for arrows/Delete), double-buffered painting, scrollable
  history, light/dark theme toggle.

## Build

**On Windows, with MinGW-w64:**
```
mingw32-make
```
**Cross-compiling from Linux/macOS with MinGW-w64** (this is how it was
built and verified here):
```
make CXX=x86_64-w64-mingw32-g++ WINDRES=x86_64-w64-mingw32-windres
```
Output: `build/NaturalCalculator.exe`, no installer, no runtime deps —
copy it anywhere and run it.

## Keyboard shortcuts

- Type `x+y=5`, press Enter, then type `x-y=6` and press Enter to solve the
  two-variable system. The second line displays both values.
- Once Calc Pro Max is active, results that overflow a double — like
  `10000000000!`, `2^10000000000`, `(10^400)^(1/2)` or 400-digit integers —
  are computed in log-space and displayed as `m * 10^e` instead of erroring.
- Type a quadratic directly in the main window, such as `x^2+5x+6=0`, and
  press Enter to display its roots. The coefficient dialog follows dark mode.
- Nonlinear single-variable equations such as `2^x+x=8` are solved numerically
  over `-1000` to `1000` (including repeated roots when detectable);
  irrational quadratic roots and square-root powers such as `2^(1/2)` show an
  exact radical form followed by its decimal value. Equations with no detected
  real roots display `∅`.
- A single equation that reduces to one variable, such as `x+2y=x-3`,
  reports that variable and explains when the other remains free.
- Press Up to recall the previous submitted expression, like a shell history.
- Press `Ctrl+A` to highlight the current expression, `Ctrl+C` to copy it,
  `Ctrl+X` to copy and clear it, and `Ctrl+V` to paste calculator text.
- Drag across a history output with the left mouse button, then press
  `Ctrl+C` to copy just that output selection. `Ctrl+A` remains editor-only.
- Drag across characters in the editor with the left mouse button, then press
  `Ctrl+C` to copy only that character range.
- Press Escape to remove the highlight before clearing the expression.
- Press `Ctrl+Z` to undo the last edit. Home and End move to the start or end
  of the current structural row.
- Press Delete twice quickly to reset saved `x` and `y` values while keeping
  the current expression and calculation history.
- Type `x` or `y` directly, or use the matching buttons in the top bar.
- Once Calc Pro Max is active, a full scientific keypad appears: `sin`,
  `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`, `ln`, `log`,
  `exp`, `abs`, plus `π` and `e` constants and a DEG/RAD toggle in the top
  bar (visible mode, click to switch). The same functions can also be typed
  as words in any case — `sin(30)`, `Log(100)`, `exp(2)`, `pi/2` — and pasted
  text resolves them the same way. Unrecognised words report `Unknown name`.
- Domain errors are honest: `tan(90)` in DEG, `asin(2)`, `ln(0)` and friends
  report what went wrong instead of returning garbage. In log-space Pro Mode,
  `exp(-1000)` still returns a real tiny value (`5.08 * 10^-435`), and trig
  on arguments too large for honest reduction (beyond 10^15) reports
  `Argument too large for trig` rather than a plausible wrong number.
- Typing or pasting `)` while the cursor is inside a `sin(...)` call or a
  `√(...)` steps out of it, like a Casio natural-display calculator.

**With MSVC** (not wired into the Makefile, but the source has no
MinGW-specific dependencies): create a new empty C++ Windows app project,
add all files in `src/`, set the subsystem to Windows, build.

## What's implemented vs. simplified (being upfront about scope)

Implemented and working (verified by clean compile + code review of the
logic, per the caveat above about not running it live on Windows):
- Structural insertion of fractions/powers/roots/parens, with `/`
  correctly consuming only the immediately-preceding atom (`1+2/3` →
  `1 + 2⁄3`, matching your spec example).
- Nested fractions/powers/roots to arbitrary depth, with the renderer
  shrinking font size per nesting level.
- Full tree-aware cursor navigation and non-corrupting backspace/delete.
- BODMAS-correct evaluation including implicit multiplication (e.g.
  `2(3+4)`), unary minus, and all the example expressions in the spec.
- Multiline workspace: Enter commits the current line into scrollable
  history (each entry keeps full math formatting) and starts a fresh line.
- Light/dark theme, resizable window, custom flat/rounded buttons.

Deliberately simplified, flagged here rather than silently glossed over:
- **Click-to-position-cursor inside the expression** isn't implemented —
  clicking the editor focuses the window, but you navigate with the
  keyboard (arrows) rather than clicking into the middle of a fraction.
  Hit-testing arbitrary tree positions is a solid follow-up if wanted.
- **Up/Down navigation** only jumps directly between a structure's own
  two rows (numerator↔denominator, base↔exponent) from within them; it
  doesn't yet bubble through several levels of unrelated nesting to find
  the "nearest" row above/below the way a fully general text-editor
  caret-column model would.
- Parens are drawn with simple GDI `Arc()` curves rather than a custom
  glyph — legible and cheap to draw, but not pixel-perfect Casio styling.
- The dark/light theme choice is saved per Windows user and restored on the
  next launch.
