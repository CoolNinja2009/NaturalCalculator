Natural Calculator — complete repo guide for Claude.

The goal: after reading this file you should be able to make correct changes to
any of the ~18k lines of C++ without opening most source files. Anything that
is easy to get wrong is listed in "Gotchas" at the bottom — read that even if
you read nothing else.

## What this is

**Natural Calculator** is a native Windows desktop math workstation: a Casio
fx-991ES-style "2D natural display" calculator with real fraction bars /
exponents / radicals / integrals / matrices / sets, a full algebro-numeric
solver, a live 2D graphing engine (Direct3D 11 GPU + FP64 CPU fallback), and
an animated "Calc Pro Max" theme (brick textures, keycap explosion animation,
a draggable "Hellcat pet" sprite).

Hard constraints of the project (define almost every design decision):

- **Zero runtime dependencies.** Win32 + GDI + Direct3D 11 only. No .NET,
  Electron, WebView, RichEdit, common-controls widgets, DirectWrite, network,
  or third-party libraries of any kind. Everything is hand-rolled.
- **Instant startup, sub-millisecond interaction.** README claims <1ms
  keystroke→pixels latency and ~20–30 MB working set; `benchmark.py`
  empirically verifies those claims and a comment-heavy architecture exists
  to honor them (render caches, arena allocators, bytecode VM, deferred graph
  repaint). Never add anything slow to the input path.
- **C++17, single-target.** Builds to one statically-linked
  `build/NaturalCalculator.exe` via MinGW-w64. UTF-16 everywhere (`-municode`,
  `wWinMain`, `L""` strings).

## Repo layout

```
Makefile                 build + 3 test targets (see "Build & test" below)
benchmark.py             Python ctypes benchmark suite (see below)
README.md                user-facing docs: features, controls, syntax
calc.exe                 local (gitignored) binary — ignore it
.github/workflows/build.yml  CI: builds exe on windows-latest, publishes release per push to main/master
src/                     all product code (11 .cpp / 9 .h, ~15k lines)
tests/                   hand-rolled assert tests + python graph suite
   calculator_tests.cpp  ~1400 lines, 24 tests of evaluator/expr_tree/workspace/graph
   layout_tests.cpp      notation rendering & caret hit-test tests
   gpu_graph_tests.cpp   graph engine (rasterizer, GPU path, view math, probe HUD)
   bench_bridge.cpp      C-ABI exports compiled into build/libnatcalc_bench.dll
   benchmark_suite.cpp   standalone C++ benchmark exe (open_contains main())
   test_graph_suite.py   ctypes tests on the DLL (cardioid, tan poles, zoom bounds…)
tools/                   empty placeholder
build/                   gitignored build output (and libnatcalc_bench.dll)
```

`tests/` is listed in `.gitignore` but its files are force-added and tracked —
new tests belong there and should be committed explicitly (`git add -f` if
they get ignored).

## Module map (src/), biggest first

| File | LOC | Role |
|---|---|---|
| main.cpp | 3330 | Win32 shell: windows, message pump, toolbar/keypad buttons, scrolling, clipboard, theme, Pro-Max switch, render caches |
| evaluator.cpp | 3365 | tree→value evaluation, implicit-multiplication precedence walk, all solvers, BigValue log-space arithmetic, `sciFunctionValue` |
| expr_tree.cpp | 1900 | the structural AST + all editing primitives |
| layout.cpp | 2230 | 2D typesetter (measure/draw/hit-test), natural-notation `toDisplayText` |
| graph.cpp | 1770 | graph panel: analysis, hit-testing, mouse, grid/labels, `renderGraph` orchestration |
| algebra.cpp | 1080 | free variables, substitution, simplification, isolation of y (or x) |
| graph_raster.cpp | 680 | FP64 software rasterizer + worker pool (`parallelFor`) |
| gpu_graph.cpp | 610 | D3D11 device, HLSL generation/compilation, implicit coverage render |
| symbolic.cpp | 475 | symbolic integrate/differentiate, Simpson definite integral, numeric derivative |
| workspace.cpp | 310 | history stack + solve routing on commit |
| ui_theme.cpp | 245 | palettes, anti-aliased keycap/header/toggle drawing, `fillRoundRect` |

Headers are richly commented — `expr_tree.h`, `evaluator.h`, `graph.h`,
`graph_raster.h`, `algebra.h`, `symbolic.h`, `ui_theme.h` each open with
several paragraphs explaining their invariants. Reading a header is almost
always enough; the .cpp is implementation detail.

## The expression model (src/expr_tree.h/.cpp) — the heart

An expression is NOT a string. It is a tree:

- `Row` = ordered `std::vector<std::unique_ptr<Item>>` plus owner back-links.
- `Item` = one node. `ItemType` gives children via `a/b/c/d` unique_ptr<Row>:
  - `Fraction` a= numerator b=denominator; `Power` a=base b=exponent;
    `Paren` a=inner (bracket/brace styling via `isBracket`/`isBrace`);
    `Sqrt` a=radicand; `Function` a=argument;
    `Integral` a=integrand b=lower c=upper; `Derivative` a=expr b=eval point;
    `Summation`/`Product` a=body b=lower c=upper;
    `Permutation`/`Combination` a=n b=r.
  - Leaf items: `Number` (digits + ≤1 '.', in `numText`), `Variable` (char
    `variableName`, 'x'/'y'), `Operator` (`opChar`, + - * % ! , ;), `Equals`,
    `CloseParen`, `Name` (word being typed, resolved later), `Constant`
    ('p'=pi, 'e'=e, 'f'=phi).
- **Division is never an Operator.** '/' at edit time builds a Fraction
  structure (see `insertFraction`), so evaluation of a Fraction node *is* the
  division — that is how paper-accurate precedence is achieved.
- `Expression` = root `Row` + `Cursor { Row* row; int index; }` — a gap
  position sitting *before* items[index] in that row. All editing functions
  act on `expr.cursor` in place.
- `Row` carries hierarchical layout caches (`cachedWidth/Ascent/Descent/…`)
  invalidated by `invalidateLayout()` which bubbles to ownerParentRow.
  `Item`/`Row` override `operator new/delete` with an arena allocator
  (defined in expr_tree.cpp) for allocation speed.
- Names typed as letters accumulate into a `Name` item (`insertNameLetter`);
  `normalizeNames(expr)` converts them (runs after every edit): merges
  adjacent Names, turns `Name + Paren` pairs into Function/Sqrt calls, and
  `pi`/`e`/`x`/`y` words into Constant/Variable leaves.
- `insertFromText(expr, "sin(30)")` builds a tree from calculator-syntax text
  (used for paste, tests, and the text-mode editor). Case-insensitive.
- Serialization for display-only contexts: `toPlainString()` ("1+2/3" ASCII)
  and layout's `rowToDisplayText()` (Unicode pretty, "sin⁻¹(x)", "√x").
- Structured deletion: `backspace`/`doDelete` empty a child row first and
  then collapse the structure, splicing content back into the parent row.
- `cloneExpression` deep-copies for history/undo (no shared editable state).
- `SciFunction` enum (~80 ids: trig/hyperbolic + inverses incl. arc-sec/csc/cot
  families, ln/log/log₂/exp, roots, abs/floor/ceil/round/trunc/sgn, gamma/
  lgamma/erf/erfc/fact, deg/rad, calculus ops Integrate/Diff/Limit/Sum/
  Product, matrix Det/Inv/Transpose/Trace/Rank/Rref/Dot/Cross/Norm/Eye/
  Zeros/Ones, stats Mean/Median/Stddev/Var/Min/Max, number theory Gcd/Lcm/
  IsPrime/Ncr/Npr, advanced Beta/BesselJ0/J1/Y0/Y1/Sinc/LambertW/Hypot/Atan2/
  Zeta/Clamp/Lerp). Names via `sciFunctionName(id)` / `findSciFunction(name, id)`.
- Parentheses are **structural, not glyphs**: `)` only steps out of an
  unclosed Paren, and an item is "closed" (`isClosed`) only after that —
  expression printing then appends the missing `)` itself.

## Editing → evaluation data flow

1. Input (key, button, clipboard) → `doAction(action)` in main.cpp → one of
   the `insert*` editing primitives (each pushes an undo snapshot first via
   `g.undo` + `cloneExpression`).
2. `invalidateEdit()` repaints the editor immediately and (in Pro Max) arms a
   12 ms `kGraphRefreshTimerId` timer so typing re-graphs once per pause, not
   per keystroke.
3. Enter (`ActEquals`) → `normalizeNames` → `workspace.commitCurrent(context)`
   which routes through workspace.cpp `solveEquation`/`evaluateExpression`
   using evaluator's solver stack and pushes a HistoryEntry.
4. Assignments (`x = 5`) store values in `EvaluationContext` (kept by
   `Workspace::solvedValues_`, copied into `g.values`); undeclared x/y stay
   **symbolic** via algebra.cpp (never silently evaluated as 0).

## Layout/typesetting (src/layout.h/.cpp)

- Everything measured/drawn through raw GDI. Each item reports
  `Size{width, ascent, descent, inkAscent, inkDescent}`; a Row lays children
  left→right on one baseline, ascent/descent = max over children — that is
  what lets fractions/exponents/radicals interleave with plain digits.
- API: `measureExpression(HDC, root)`, `drawExpression(HDC, root, originX,
  baselineY, theme, cursor*, &outCaret)` (fills CaretInfo while drawing),
  `placeCursorAtPoint(expr, hdc, originX, baselineY, x, y)` — clicking inside
  a Number splits the digit run.
- Fonts per nesting depth come from `fontHeightForDepth(depth)`; GDI font
  handles cached, freed once via `shutdownFonts()` at exit.
- Three themes exist structurally: layout's `Theme` (`lightTheme()` /
  `darkTheme()`, incl. LCD "screen" colors), the keypad `UiPalette` from
  `makePalette(theme)` (ui_theme.h), and the graph `GraphPalette` from
  `getGraphPalette(theme)`. Keep all three consistent when touching colors.

## Evaluator (src/evaluator.h/.cpp)

- Grammar over a Row: Row := Term (('+'|'-') Term)*, Term := Factor
  (ImplicitOrStar Factor)* (`*` or bare adjacency → implicit
  multiplication), Factor := '-'? Atom, Atom := Number | Fraction | Paren |
  Power | Sqrt. Errors are `std::runtime_error` with short human-readable
  messages ("division by zero" etc.).
- `EvaluationContext { x, y, bigNumbers, degrees, xDefined, yDefined }`.
  Degrees (DEG) is the default angle unit. `bigNumbers` (Pro Max only)
  switches on **log-space `BigValue` arithmetic**: factorials like
  `10000000000!` and powers `2^10000000000` compute in log10 domain and
  format as "m × 10^e" instead of overflowing. `evaluateProToString` is the
  content-method that optionally engages it.
- Result type `EvalValue` = Number | Matrix (`Matrix{rows, cols, data}`,
  row-major, `at(r,c)`) | Symbolic (string) | Set (`MathSet`, with
  `setUnion/Intersection/Product/Delta` free functions).
- Solver stack (all in evaluator.h): `solveQuadratic(a,b,c)` →
  `QuadraticResult` with exact radical strings (`firstExact`/`secondExact`);
  `solveTwoVariableSystem(row1, row2, …)` (Cramer's rule, real x/y);
  `solveComplexEquation(row, …)` (complex coefficients with i, x, y —
  extracts real/imaginary parts by multi-point sampling + Cramer);
  `solveSingleVariableEquation` (linear exact) and
  `solveGeneralEquation` (nonlinear, numeric root finding, e.g.
  `2^x + x = 8`); `solveVariableAssignment`; `isLinearEquation`;
  `solveQuadraticEquation`; `isProModeTrigger` (particular expressions
  activate Pro Max); `evaluateComplexToString`.
- `sciFunctionValue(int id, double x, bool degrees)` is the single-argument
  function used by calculator, grapher and tests alike; throws on domain
  errors — the grapher catches and treats them as undefined points.
- Naming: use `evaluateValue`/`evaluate`/`evaluateToString(…, context, bool&
  failed)`; the grapher and workspace never parse text — they always pass a
  Row.

## Free-variable algebra (src/algebra.h/.cpp)

- `collectFreeVariables` / `unknownVariables(row, context)` (integral/sum/
  product/derivative items bind their own variable), `substituteDeclared`
  (deep copy with declared x/y replaced), `renameVariable`,
  `evaluateRange`.
- `simplifyExpression(row, ctx, out)` collects like terms
  ("2x+3+x" → "3x+6") and is the source of symbolic answers like
  `2x + 3 → "2x + 3"`.
- `solveForOneVariable(equation, ctx, out)` isolates y (or x) in one
  two-unknown equation: `2x + y = 9` → "y = 9 - 2x", `x^2 + y^2 = 25` →
  "y = ±√(25 − x²)". This is what makes a single equation with x and y answer
  symbolically rather than error.

## Symbolic calculus (src/symbolic.h/.cpp)

- `symbolicIntegrate(row, var, appendConstant)` — pattern-matched indefinite
  integration returning a string (+" + C"), `symbolicDifferentiate(row, var)`,
  `evalDefiniteIntegral` (Simpson), `evalDerivativeAtPoint` (numeric).
- Text-only outputs; results are not re-parsed back into trees.

## Graphing (src/graph.h/.cpp, graph_raster.h, gpu_graph.h)

Pipeline on each repaint of `graphRect`:

1. `analyzeGraphExpression(root, context)` classifies: `ExplicitY`
   (y = f(x) or bare expression in x), `ExplicitX` (x = f(y)), `ImplicitXY`
   (F(x,y) = G(x,y))), fills `GraphAnalysis`: display title (natural
   notation), **cloned** AST rows (`leftRow`/`rightRow`, isolated from editor
   state so reentrant evaluation is safe), base context, and
   `BytecodeProgram leftProgram/rightProgram`.
2. Rows are also **compiled to flat bytecode** (`compileRowToBytecode`,
   `ByteOp` enum ~60 ops incl. `SciCall` whose fn ids ride `fnIds[]`).
   `program.execute(x, y)` **never throws** (undefined/NaN everywhere
   instead). `hasFastEval()` true when bytecode covers the whole equation.
3. `renderGraph(hdc, rect, analysis, state, theme, fonts)` in graph.cpp
   orchestrates: build a `GraphView` (FP64 mapping between math coords and
   pixels, `toPx/fromPx/toPy/fromPy`; pixel centers at i+0.5, y down),
   `rasterizeGrid` (background + major/minor grid + axes into a 0x00RRGGBB
   top-down DIB), then curves:
   - **D3D11 GPU path** (`renderImplicitCoverageGpu`): implicit curves only,
     when hardware adapter present AND `gpuCanResolveView(view)` (float32
     must resolve each pixel — ≥256 ulps headroom). AST→HLSL expression text
     via `analysisToHLSL`, shader compiled at runtime with D3DCompiler.
     Same stroke profile and pole/jump rejection as CPU. On any failure it
     returns false and the caller falls back to CPU.
   - **FP64 CPU fallback** (graph_raster.cpp, always used for explicit
     curves): `rasterizeExplicitCurve` (adaptive sampling respecting jumps,
     poles, domain edges) or `rasterizeImplicitCurve` (Marching Squares on
     an FP64 sample grid with pole & jump rejection — asymptotes never
     fabricate curve segments). Both draw into a `CoverageBuffer` of float
     alpha [0,1], combined per-pixel with max(), then `compositeCoverage`
     blends the curve color over the DIB.
   - `parallelFor(count, fn)` = persistent worker pool for band-parallel
     rasterization (fn must not throw or recurse).
4. Blitted to the cached DC; `graphButtonRects` gives the HUD `+ / − / RESET`
   button boxes; `drawProbeTooltip` draws the right-click HUD probe.
- Interaction lives in `GraphState` + `handleGraphMouse*` /
  `handleGraphRight*` functions: left-drag pan, wheel zoom centered on
  cursor, right-drag probe with **curve snapping** (Newton-Raphson) and POI
  snapping (roots / y-intercepts / extrema via derivative sign changes);
  Shift locks to integer grid.
- `GraphState.resetView(range, pxW, pxH)` shows [-range, range] on the
  shorter axis; `fitAspect` keeps square pixels; `zoom` clamps bounds to
  guard against float32 overflow deep in (see tests/test_graph_suite.py:
  "zoom bounds clamping").
- Strict 2D recognition: one-variable equations like `x^2 − 7x + 12 = 0` are
  NOT graphed implicitly (they're solved by the calculator side instead).

## Workspace (src/workspace.h/.cpp)

- History is a vector of `HistoryEntry { unique_ptr<Expression> expr;
  string result; bool isError; }` — frozen clones that still render as 2D
  math. `current()` is the live editable line.
- `commitCurrent(context)`: routes expressions (evaluate), equations
  (`solveEquation` dispatches linear systems across consecutive history
  entries — entering `x + y = 10` then `2x - y = 5` solves as a system —
  plus complex/quadratic/single-variable/general), returns false for empty
  input. `recallPrevious()` (Up on empty line) pulls the last entry back.
- `clearVariables()` (press Delete twice quickly) wipes declared x/y;
  `clearAll()` (AC) empties everything.

## UI chrome (src/main.cpp + ui_theme.h/.cpp)

- `main.cpp` = the app: `wWinMain` (linked with `-municode`), `App g` global
  state struct, `WndProc`, and all placement. Buttons are declarative
  `ButtonDef{rect, label, Action, isOperator/isAccent/isFunction/isDanger}`
  laid out in `layoutButtons` (7 rows; 4 columns normally, **7 columns in
  Pro Max** with three "pages" switched by `keypadPage` — row0 is the
  AC/Backspace row; last row is a wide `=`). Header chips are `TopAction`s
  laid right-to-left in `layoutTopBar` (theme, Sci, DEG|RAD toggle,
  Quadratic, y, x) with the window title shortened to fit.
- Screen rectangles: `topBarRect, historyRect, editorRect, buttonAreaRect,
  graphRect`. `recomputeLayout` handles the Pro Max split (left calc column /
  right graph panel when width ≥ 600). Window lets horizontal overflow scroll
  per-strip via `HScroll` bars (editor and history independently: drag thumb,
  click track, wheel; Shift+wheel over history).
- **Render caches** make repaint cheap and are the reason the app meets its
  latency claims; invalidate only what changed:
  - persistent `backbufferDC/Bmp` with per-section validity `backbufferFresh`
    and content hashes `topBarKey/historyKey`;
  - per-`KeyState` full keypad layers (`keyLayer*`, 3 roles × 3 states)
    composed with BitBlts, hover/press filled per-key via
    `keyLayerKeyDrawn`; `invalidateButtonCache()`;
  - `graphCacheDC/Bmp` keyed on expr string + state hash + dark flag;
    `invalidateGraphCache()` must be called on theme/mode/layout changes.
- Timers: caret blink (530 ms, id 1), Pro-Max transition (30 ms, id 2), key
  flash (110 ms, id 3), graph refresh (12 ms, id 4).
- Pro Max (`activateProMode`): toggled from the Sci chip or triggered by a
  `isProModeTrigger` expression; animates over `kProTransitionMs` (720),
  expands window to ≥960px, loads assets embedded as RT_RCDATA resources
  (`resource.h`: `IDR_EXPLOSION_SHEET` = 29-frame animated sheet, min
  2490×1680; `IDR_BRICK_TEXTURE`) through GDI+ (token started lazily,
  `GdiplusStartup`/`Shutdown` managed manually), spawns the draggable pet,
  forces dark title bar via DWM.
- Dark mode persists in `HKCU\Software\NaturalCalculator` (value DarkMode).
- Perf overlay: `Ctrl+Shift+P` toggles; shows input-to-pixels (QPC-based
  `inputStamp`), paint ms, worst input ms of last frame.
- Clipboard: plain text and LaTeX-ish text paste through `insertFromText`;
  selection is either whole (`allSelected`), a structural range
  (`rangeSelected` + `selectionAnchor` with `deleteRange`), or a text-caret
  selection in the editor.
- ui_theme.h: `KeyRole {Digit,Function,Operator,Equals,Danger}`,
  `KeyState {Normal,Hover,Pressed}`; `drawKeyCap`, `drawHeaderButton`,
  `drawSegmentedToggle`, `headerButtonWidth`; Fluent icon glyphs (`Icon::
  Backspace = U+E750`, Sun/Moon for theme). Contrast decisions are made once
  here — don't restyle ad hoc elsewhere.

## Build & test

```bash
mingw32-make                 # or make CXX=x86_64-w64-mingw32-g++ for cross
mingw32-make test            # builds & runs 3 test exes (see Makefile)
mingw32-make clean
python benchmark.py          # builds the bench DLL + exe itself if missing; exit 1 if any sub-ms claim fails
python tests/test_graph_suite.py   # needs build/libnatcalc_bench.dll first
```

If `make`/`mingw32-make` is not on PATH (as on the dev machine at time of
writing — `g++` and `windres` are, under msys2's mingw64), copy the exact
`g++` commands verbatim out of the Makefile's `test` target (and the
windres/object/link steps from `all`); they run correctly as-is. The `test`
target compiles and runs, in order: `calculator_tests.exe`,
`gpu_graph_tests.exe`, `layout_tests.exe`; each prints a concluding
"All ... passed" line and must exit 0.

- Compiler/linker flags (unchanged in CI, .github/workflows/build.yml):
  `g++ src/*.cpp build/app_res.o -o build/NaturalCalculator.exe -std=c++17
  -O2 -municode -mwindows -DUNICODE -D_UNICODE -lgdi32 -luser32 -lkernel32
  -ldwmapi -ladvapi32 -lcomctl32 -lcomdlg32 -lgdiplus -lole32 -ld3d11 -ldxgi
  -ld3dcompiler`; README suggests adding `-ld3dcompiler` for the GPU engine.
  `app_res.o` comes from `windres src/app.rc` (needs app.ico +
  explosion_sheet.png + brick_texture.png next to it).
- Tests are plain `main()` programs using `<cassert>`/iostream — no test
  framework. They construct trees with the editing primitives
  (`expressionFrom`, `insertFromText`) and assert on
  `evaluate`/`evaluateToString`/analysis. Build line for tests mirrors the
  Makefile `test` target (compile the needed src/*.cpp files directly with
  `-Isrc`; main.cpp is NOT part of test targets).
- Add new tests to `tests/calculator_tests.cpp` (or a matching file); they
  must pass with `make test` before any commit. CI does not run tests, only
  builds + releases (on every push to main/master a release is published).

## Gotchas — read before editing anything

1. **Never treat an expression as a string** except at the edges
   (`insertFromText` input, `toPlainString` output). Precedence correctness
   lives in the structure: '/' → Fraction item, implicit multiplication in
   evaluator's Term rule, Powers wrap *the atom to the left*.
2. **Cursor is a gap position (row, index before which it sits)**, not a
   pointer at an item. After any insert, the cursor usually moves *inside*
   the new structure (into denominator/exponent/radicand/argument). Test
   caret behavior at every structural boundary when touching editing code.
3. `normalizeNames(expr)` must run after edits — main.cpp does it in
   `doAction`; replicate in any new edit path or typed words never resolve.
   `Name+Paren` adjacency is the canonical "sin(" → Function pattern.
4. Zero/undefined convention: workspace keeps undeclared x/y *symbolic*
   (algebra.cpp), but bare `evaluate()` substitutes context x/y = whatever is
   in the context — grapher and numeric solvers depend on that. Don't "fix"
   unknowns to be 0 globally.
5. Invalidations are manual and targeted: change tree → `invalidateLayout()`
   (bubble handles parents); repaint editor + delayed graph →
   `invalidateEdit()`; theme/mode/layout → also `invalidateButtonCache()` +
   `invalidateGraphCache()`. Missing invalidations appear as stale pixels;
   over-invalidations cost the latency claim.
6. Evaluator and `sciFunctionValue` **throw** `std::runtime_error` on domain
   errors; bytecode `execute` and graph sampling never throw (NaN instead).
   Keep that split: UI displays exception messages, graph samples catch.
7. The GPU path is float32 and implicit-curves-only, behind
   `gpuCanResolveView`; the FP64 CPU rasterizer is always available and is
   the only path for explicit curves and deep zooms. If you change stroke
   style, pole/jump rejection, or coverage math, change BOTH
   gpu_graph.cpp (HLSL) and graph_raster.cpp so they stay pixel-matched.
8. Assets: explosion/brick images are embedded via src/app.rc + resource.h
   (`IDR_EXPLOSION_SHEET`, `IDR_BRICK_TEXTURE`); brick falls back to probing
   `src/brick_texture.png` on disk if the resource fails. GDI+ must be
   started once before any `Gdiplus::Image` (main.cpp does it lazily).
9. Unicode throughout: window text and labels are wide (L"…"), math
   serialization is UTF-8 narrow strings. Watch for `TCHAR` traps — there are
   none; it's all explicitly `wchar_t` / `std::string` by location.
10. Layout measurement runs on the hot path every repaint: use the Row-level
    cached metrics (`invalidateLayout` on change) instead of re-querying
    GDI text metrics; the same applies to `UiFonts` handles (created once).
11. `tests/` is in .gitignore but tracked — `git add -f tests/...`.
12. Style: headers open with long comment blocks explaining invariants —
    extend them when you change invariants. Code lives in anonymous
    `namespace {}` inside main.cpp; enums use `ColorLike::Value` casing;
    functions `camelCase`, types `PascalCase`, constants `kCamelCase`.
13. Don't add dependencies, threads beyond `parallelFor`'s pool, or anything
    that could slow cold start / keystroke latency without benchmarking via
    `benchmark.py`.
