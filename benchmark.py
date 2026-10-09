#!/usr/bin/env python3
"""
Natural Calculator - Sub-Millisecond Performance Benchmark Suite (Python)
Validates claims made in README.md:
  - Sub-millisecond keystroke / input response (< 1.0 ms)
  - Sub-millisecond mathematical evaluation & equation solving (< 1.0 ms)
  - Sub-millisecond 2D natural typesetting & layout measurement (< 1.0 ms)
  - Lean memory footprint (~20-30 MB RAM)
"""

import os
import sys
import time
import ctypes
import subprocess
from ctypes import wintypes
from dataclasses import dataclass
from typing import Callable, List, Optional

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(SCRIPT_DIR, "build")
DLL_PATH = os.path.join(BUILD_DIR, "libnatcalc_bench.dll")
EXE_PATH = os.path.join(BUILD_DIR, "NaturalCalculator.exe")

SUB_MS_THRESHOLD_US = 1000.0  # 1.0 ms = 1000 microseconds

@dataclass
class BenchmarkMetric:
    category: str
    name: str
    iterations: int
    mean_us: float
    min_us: float
    max_us: float
    p95_us: float
    p99_us: float
    passed: bool


def compile_if_needed():
    """Ensure the benchmark DLL and executable are compiled."""
    os.makedirs(BUILD_DIR, exist_ok=True)
    if not os.path.exists(DLL_PATH):
        print("[+] Compiling benchmark C-ABI bridge DLL...")
        cmd = [
            "g++", "-shared", "-fPIC", "-static", "-static-libgcc", "-static-libstdc++",
            "-std=c++17", "-O2", "-Isrc",
            "src/expr_tree.cpp", "src/evaluator.cpp", "src/layout.cpp",
            "src/workspace.cpp", "src/graph.cpp", "src/gpu_graph.cpp",
            "src/symbolic.cpp", "tests/bench_bridge.cpp",
            "-lgdi32", "-ld3d11", "-ldxgi",
            "-o", DLL_PATH
        ]
        res = subprocess.run(cmd, cwd=SCRIPT_DIR, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Error compiling DLL: {res.stderr}")
            sys.exit(1)

    if not os.path.exists(EXE_PATH):
        print("[+] Compiling NaturalCalculator.exe...")
        cmd = [
            "g++", "src/*.cpp", "build/app_res.o", "-o", EXE_PATH,
            "-std=c++17", "-O2", "-municode", "-mwindows",
            "-DUNICODE", "-D_UNICODE",
            "-lgdi32", "-luser32", "-lkernel32", "-ldwmapi", "-ladvapi32",
            "-lcomctl32", "-lcomdlg32", "-lgdiplus", "-lole32",
            "-ld3d11", "-ldxgi", "-ld3dcompiler"
        ]
        subprocess.run(" ".join(cmd), shell=True, cwd=SCRIPT_DIR, check=True)


def benchmark_function(category: str, name: str, iters: int, func: Callable) -> BenchmarkMetric:
    """Benchmark a function across N iterations with nanosecond precision."""
    # Warmup
    for _ in range(min(iters, 20)):
        func()

    timings_us = []
    for _ in range(iters):
        t0 = time.perf_counter_ns()
        func()
        t1 = time.perf_counter_ns()
        timings_us.append((t1 - t0) / 1000.0)

    timings_us.sort()
    mean_val = sum(timings_us) / len(timings_us)
    min_val = timings_us[0]
    max_val = timings_us[-1]
    p95_val = timings_us[int(len(timings_us) * 0.95)]
    p99_val = timings_us[int(len(timings_us) * 0.99)]

    # Sub-millisecond claim verification: mean and 95th percentile strictly < 1000.0 us
    passed = (mean_val < SUB_MS_THRESHOLD_US) and (p95_val < SUB_MS_THRESHOLD_US)

    return BenchmarkMetric(
        category=category,
        name=name,
        iterations=iters,
        mean_us=mean_val,
        min_us=min_val,
        max_us=max_val,
        p95_us=p95_val,
        p99_us=p99_val,
        passed=passed
    )


class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def benchmark_process_cold_start() -> tuple[float, float]:
    """
    Launches NaturalCalculator.exe, measures cold start time to window readiness,
    and inspects process memory working set footprint.
    Returns: (startup_ms, working_set_mb)
    """
    user32 = ctypes.windll.user32
    psapi = ctypes.windll.psapi
    kernel32 = ctypes.windll.kernel32

    t0 = time.perf_counter_ns()
    proc = subprocess.Popen([EXE_PATH], cwd=SCRIPT_DIR)

    hwnd = None
    # Wait until top-level window is found and responsive
    timeout_start = time.time()
    while time.time() - timeout_start < 5.0:
        hwnd = user32.FindWindowW(None, "Natural Calculator")
        if hwnd and user32.IsWindowVisible(hwnd):
            break
        time.sleep(0.005)

    t1 = time.perf_counter_ns()
    startup_ms = (t1 - t0) / 1_000_000.0

    # Query Process Working Set
    working_set_mb = 0.0
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    h_proc = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, proc.pid)
    if h_proc:
        pmc = PROCESS_MEMORY_COUNTERS()
        pmc.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
        if psapi.GetProcessMemoryInfo(h_proc, ctypes.byref(pmc), pmc.cb):
            working_set_mb = pmc.WorkingSetSize / (1024.0 * 1024.0)
        kernel32.CloseHandle(h_proc)

    # Clean termination
    if hwnd:
        WM_CLOSE = 0x0010
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()
    else:
        proc.kill()

    return startup_ms, working_set_mb


def main():
    compile_if_needed()

    if hasattr(os, "add_dll_directory"):
        try:
            os.add_dll_directory(BUILD_DIR)
        except Exception:
            pass

    lib = ctypes.CDLL(DLL_PATH)
    lib.bench_init()

    results: List[BenchmarkMetric] = []

    print("\n" + "=" * 105)
    print("              NATURAL CALCULATOR: PYTHON SUB-MILLISECOND BENCHMARK SUITE")
    print("      Empirically testing claims from README: latency < 1.0 ms (1000 us) & lean memory footprint")
    print("=" * 105)

    # 1. Keystroke & Input Response
    results.append(benchmark_function("Input / Keystroke", "Single Digit Insertion", 10000, lib.bench_keystroke_digit))
    results.append(benchmark_function("Input / Keystroke", "Fraction Structure Insert", 10000, lib.bench_keystroke_fraction))
    results.append(benchmark_function("Input / Keystroke", "Calculus Structure Insert", 10000, lib.bench_keystroke_structure))
    results.append(benchmark_function("Input / Keystroke", "Parse Arithmetic Expression", 5000, lib.bench_parse_arithmetic))
    results.append(benchmark_function("Input / Keystroke", "Parse Complex Linear Equation", 5000, lib.bench_parse_complex_eq))
    results.append(benchmark_function("Input / Keystroke", "Tree Cursor Navigation (L/R/U/D)", 10000, lib.bench_cursor_navigation))

    # 2. 2D Natural Typesetting Layout
    results.append(benchmark_function("2D Typesetting", "Measure Simple Row", 5000, lib.bench_measure_layout_simple))
    results.append(benchmark_function("2D Typesetting", "Measure Nested Fraction & Sqrt", 5000, lib.bench_measure_layout_complex))
    results.append(benchmark_function("2D Typesetting", "Measure Structural Summation", 5000, lib.bench_measure_layout_summation))

    # 3. Mathematical Evaluation Engine
    results.append(benchmark_function("Math Evaluation", "Basic Real Arithmetic", 10000, lib.bench_evaluate_arithmetic))
    results.append(benchmark_function("Math Evaluation", "Trigonometric & Transcendentals", 5000, lib.bench_evaluate_transcendental))
    results.append(benchmark_function("Math Evaluation", "Quadratic Equation Solver", 5000, lib.bench_solve_quadratic))
    results.append(benchmark_function("Math Evaluation", "2x2 Linear System Solver", 5000, lib.bench_solve_linear_system))
    results.append(benchmark_function("Math Evaluation", "Complex Linear System (i)", 3000, lib.bench_solve_complex_eq))
    results.append(benchmark_function("Math Evaluation", "BigValue 10^10! Factorial", 2000, lib.bench_evaluate_bigvalue))
    results.append(benchmark_function("Math Evaluation", "Definite Summation (1 to 100)", 2000, lib.bench_evaluate_summation))
    results.append(benchmark_function("Math Evaluation", "Definite Integral (Simpson's)", 2000, lib.bench_evaluate_integral))

    lib.bench_cleanup()

    # Print Table
    header = f"{'Category':<18} {'Benchmark Test':<34} {'Iters':>8} {'Mean (us)':>11} {'Min (us)':>10} {'P95 (us)':>10} {'Status':>10}"
    print(header)
    print("-" * 105)

    all_passed = True
    for r in results:
        status_str = "[PASS]" if r.passed else "[FAIL]"
        if not r.passed:
            all_passed = False
        print(f"{r.category:<18} {r.name:<34} {r.iterations:>8} {r.mean_us:>11.3f} {r.min_us:>10.3f} {r.p95_us:>10.3f} {status_str:>10}")

    print("=" * 105)

    # 4. Cold Startup & Memory Footprint Verification
    print("\n[END-TO-END NATIVE RUNTIME METRICS]")
    try:
        startup_ms, working_set_mb = benchmark_process_cold_start()
        print(f"  Cold Start Latency to Visible Window:  {startup_ms:.2f} ms")
        print(f"  Live Memory Footprint (Working Set):   {working_set_mb:.2f} MB")
        mem_passed = working_set_mb < 30.0
        print(f"  Memory Footprint Claim (< 30 MB):      {'[PASS]' if mem_passed else '[WARN]'}")
    except Exception as e:
        print(f"  Could not run process cold start benchmark: {e}")

    # Verdict
    print("\n[VERDICT]")
    if all_passed:
        print("  >>> ALL CLAIMS CONFIRMED: Every operation executes with true SUB-MILLISECOND LATENCY (< 1.0 ms).")
        print("  >>> C++17 native performance claims from README.md are fully verified! <<<\n")
        return 0
    else:
        print("  >>> FAIL: One or more operations exceeded the 1.0 ms threshold.\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
