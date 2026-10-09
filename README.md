# Natural Calculator

**Natural Calculator** is a native, high-performance Windows mathematical workstation. It fuses Casio fx-991ES Plus style **2D natural structural typesetting** (real fraction bars, exponents, radicals, integrals, matrices) with a **Direct3D 11 GPU-accelerated implicit & explicit graphing engine**, a full **computer algebra and numeric solver**, and a zero-dependency **Win32/GDI** architecture.

No Electron. No .NET. No WebView2. No external runtime dependencies. Instant launch, sub-millisecond input response, and pure C++17 efficiency.

---

## Key Highlights

- **Natural Structural Math Input**: Equations are modeled as a living 2D abstract syntax tree (AST) rather than plain text. Typing `/` generates a real fraction with numerator and denominator boxes; `^` creates superscripts; roots expand dynamically with their radicands.
- **Hardware-Accelerated Live Graphing Engine (Direct3D 11)**:
  - Compiles math expressions in real-time into native HLSL pixel shaders via `D3DCompiler`.
  - Supports explicit curves ($y = f(x)$, $x = g(y)$), implicit contours ($F(x, y) = G(x, y)$ such as $x + y = \tan(x)$, $x^2 + y^2 = 25$), trigonometric relations, and coordinate asymptotes.
  - Screen-space partial derivatives (`ddx`/`ddy`) provide sub-pixel anti-aliasing and smart pole/asymptote rejection.
  - Transparent fallback to an optimized CPU Marching Squares 2D contouring engine.
- **Advanced Graph Probing & Smart Docking HUD**:
  - Right-click and drag across the graph canvas to summon a live HUD probe.
  - **Implicit & Explicit Curve Snapping**: Uses Newton-Raphson gradient descent to cling cleanly to curves.
  - **Point-of-Interest (POI) Snapping**: Automatically docks to roots ($x$-intercepts), $y$-intercepts, and local extrema (minima / maxima via derivative sign-change tracking).
  - **Integer Grid Snapping**: Hold `Shift` while probing to lock to exact integer coordinates.
- **Complex Linear System Solver ($x, y \in \mathbb{R}$ with $i$)**:
  - Solves linear equations with complex numbers and real variables $x$ and $y$ natively (e.g., $2xi + 5y = -6 - 24i$ or $-6 + 24i = 3x + 5yi + i(5x - 3y)$).
  - Dynamically extracts real and imaginary parts using complex multi-point sampling and solves the resulting system using Cramer's rule.
- **Calculus, Matrix & Set Theory ("Calc Pro Max")**:
  - **Structural Calculus Layout**: Integrals ($\int$), derivatives ($d/dx$ with evaluation bars $|_x=a$), summations ($\sum$), and products ($\prod$) are rendered as fully **vertical 2D structural blocks**. Arrow keys dynamically navigate into upper and lower limits, completely abandoning inline parenthesis functions.
  - **Linear Algebra**: Matrix dimensions, determinants, inverses, transpositions, traces, cross/dot products, RREF, and scalar arithmetic.
  - **Set Theory**: Unions ($\cup$), intersections ($\cap$), symmetric differences ($\Delta$), and Cartesian products.
  - **Combinatorics & Stats**: $^nP_r$, $^nC_r$, mean, variance, and standard deviation.
  - **Special Functions**: Bessel functions ($J_0, J_1, Y_0, Y_1$), Lambert $W$, and asymptotic gamma.
  - **Astronomic Log-Space Arithmetic**: Pro Mode handles factorials and powers beyond double precision (e.g. $10000000000!$ or $2^{10000000000}$) formatted in scientific notation ($m \times 10^e$) without overflow errors.
- **Fast Startup & Native UI Optimization**:
  - Direct Win32 API (`CreateWindowExW`, GDI, Direct3D 11).
  - Sub-millisecond cold start with minimal memory footprint (~20-30 MB RAM with D3D11 device active).
  - Immersive "Calc Pro Max" visual styling with hardware-accelerated **Magma and Hell Brick textures**, completely jitter-free window resizing, and a 60 FPS animated **Hellcat Pet** embedded directly into the native Win32 message loop without stalling math execution.

---

## System Architecture

```
                    Keyboard / Mouse / Keypad Input
                                   │
                                   ▼
             Structured Expression Model (src/expr_tree.h/.cpp)
            ┌──────────────────────┴──────────────────────┐
            ▼                                             ▼
2D Layout Typesetter (src/layout.h/.cpp)     Math Evaluator (src/evaluator.h/.cpp)
      • Recursive box layout                       • Recursive-descent tree parser
      • Dynamic font scaling per depth             • Complex linear solver
      • Caret hit-testing & selection              • BigValue log-space arithmetic
            │                                      • Numerical root finding
            ▼                                             │
   Win32 GDI Display                                      ▼
 (Main Window / History)                    Graph Analysis (src/graph.h/.cpp)
                                           ┌──────────────┴──────────────┐
                                           ▼                             ▼
                            GPU Engine (src/gpu_graph.cpp)       CPU Fallback
                              • Dynamic HLSL generation       • Marching Squares
                              • Direct3D 11 pixel shader      • Explicit sampling
                              • Screen-space derivatives
```

### Core Components

| Module | Description |
|---|---|
| `src/expr_tree.h/.cpp` | AST representing expressions as structural `Row` and `Item` nodes. Handles tree-aware editing, cursor navigation, parentheses balancing, text serialization/deserialization, and range selection. |
| `src/layout.h/.cpp` | High-fidelity 2D mathematical typesetter. Calculates baseline bounds, ascents, descents, and child row offsets. Powers mouse click-to-caret hit testing. |
| `src/evaluator.h/.cpp` | Algebraic and numeric engine. Solves single-variable equations, systems of linear equations, quadratics, complex systems, matrix operations, and big log-space values. |
| `src/gpu_graph.h/.cpp` | Direct3D 11 rendering pipeline. Translates mathematical ASTs into HLSL pixel shaders compiled on the fly, rendering anti-aliased curves at display refresh rates. |
| `src/graph.h/.cpp` | Graph coordinates management, CPU Marching Squares renderer, coordinate grids, and smart right-click probing with POI docking. |
| `src/workspace.h/.cpp` | Session management and calculation history stack with preserved 2D structural trees. |
| `src/main.cpp` | Win32 entry point, message pump, UI button layout, keyboard accelerator routing, clipboard interactions, and dark/light theme coordination. |

---

## Building from Source

### Prerequisites
- **Windows 10/11** (x64)
- **MinGW-w64** (GCC 9.0+ with C++17 support) or **MSVC**
- Windows SDK libraries: `d3d11`, `dxgi`, `d3dcompiler`, `gdi32`, `dwmapi`

### Compiling with MinGW-w64 (Recommended)

To compile directly from the command line:

```powershell
g++ src\*.cpp build\app_res.o -o build\NaturalCalculator.exe `
    -std=c++17 -O2 -municode -mwindows `
    -DUNICODE -D_UNICODE `
    -lgdi32 -luser32 -lkernel32 -ldwmapi -ladvapi32 `
    -lcomctl32 -lcomdlg32 -lgdiplus -lole32 `
    -ld3d11 -ldxgi -ld3dcompiler
```

The resulting standalone executable will be generated at `build/NaturalCalculator.exe`.

---

## Usage & Controls

### Expression Editor & Shortcuts
- **Enter**: Solves the current expression or equation and moves it into the history stack.
- **Up / Down**: Navigate between structural rows (e.g. numerator $\leftrightarrow$ denominator, base $\leftrightarrow$ exponent). Up on an empty line recalls previous calculations.
- **Left / Right**: Walk into and out of nested mathematical structures.
- **Ctrl + A**: Select the entire expression in the active editor.
- **Ctrl + C / Ctrl + X**: Copy / cut selected mathematical content.
- **Ctrl + V**: Paste plain text or LaTeX-like equations (auto-parsed into 2D structures).
- **Ctrl + Z**: Undo the last structural or text edit.
- **Delete / Backspace**: Structure-aware deletion (empties nested structures before collapsing them).

### Equation Solving Syntax
- **Linear Systems**:
  - Enter equation 1: `x + y = 10`
  - Press `Enter`, then enter equation 2: `2x - y = 5`
  - The solver outputs the unified solution `x = 5, y = 5`.
- **Complex Linear Equations**:
  - Enter equations containing $i$, $x$, and $y$:
    `-6 + 24i = 3x + 5yi + i(5x - 3y)` or `2xi + 5y = -6 - 24i`
  - Returns `x = ..., y = ... (Complex)`.
- **Quadratics & Polynomials**:
  - Enter `x^2 - 7x + 12 = 0` to display real/exact radical roots.
- **Single-Variable Equations**:
  - Enter non-linear equations such as `2^x + x = 8` to trigger the numerical root finder.

### Graph Interaction
- **Mouse Drag (Left Button)**: Pan the graph viewport across the $X/Y$ plane.
- **Mouse Wheel**: Smooth zoom centered at the mouse cursor.
- **Right Click + Drag**: Activates the **HUD Inspector Probe**:
  - Displays real-time $(x, y)$ coordinates in a floating HUD pill.
  - Snaps to roots, intercepts, and local extrema with a white target reticle.
  - Hold **Shift** while dragging to snap to exact integer grid coordinates.
- **HUD Buttons (`+` / `−` / `RESET`)**: Manual zoom steps and instant view recentering.

---

## License

Released under the **MIT License**. Free for academic, personal, and commercial use.
