#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

#include "expr_tree.h"
#include "evaluator.h"
#include "layout.h"
#include "workspace.h"

using Clock = std::chrono::high_resolution_clock;

struct BenchmarkResult {
    std::string category;
    std::string name;
    int iterations = 0;
    double avgUs = 0.0;
    double minUs = 0.0;
    double maxUs = 0.0;
    double p95Us = 0.0;
    bool passed = true; // Claim: sub-millisecond (< 1000 us)
};

static HDC g_memDC = nullptr;

static void initGdi() {
    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
}

static void cleanupGdi() {
    if (g_memDC) {
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
}

template <typename Func>
BenchmarkResult runBenchmark(const std::string& category, const std::string& name, int iterations, Func&& func) {
    // Warmup
    for (int i = 0; i < std::min(iterations, 20); ++i) {
        func();
    }

    std::vector<double> timings;
    timings.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        auto t0 = Clock::now();
        func();
        auto t1 = Clock::now();
        double elapsedUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        timings.push_back(elapsedUs);
    }

    std::sort(timings.begin(), timings.end());
    double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    double avg = sum / (double)timings.size();
    double minVal = timings.front();
    double maxVal = timings.back();
    size_t idx95 = (size_t)(timings.size() * 0.95);
    if (idx95 >= timings.size()) idx95 = timings.size() - 1;
    double p95 = timings[idx95];

    BenchmarkResult res;
    res.category = category;
    res.name = name;
    res.iterations = iterations;
    res.avgUs = avg;
    res.minUs = minVal;
    res.maxUs = maxVal;
    res.p95Us = p95;
    // Sub-millisecond claim verification: average and P95 latency must be strictly under 1000 microseconds (1.0 ms)
    res.passed = (avg < 1000.0) && (p95 < 1000.0);
    return res;
}

static void printHeader() {
    std::cout << "\n=========================================================================================================\n";
    std::cout << "                         NATURAL CALCULATOR: SUB-MILLISECOND PERFORMANCE BENCHMARK                        \n";
    std::cout << "                 Verifying sub-millisecond input, layout, and evaluation claims (< 1.0 ms)                \n";
    std::cout << "=========================================================================================================\n";
    std::cout << std::left
              << std::setw(18) << "Category"
              << std::setw(34) << "Benchmark Test"
              << std::right
              << std::setw(9) << "Iters"
              << std::setw(12) << "Avg (us)"
              << std::setw(12) << "Min (us)"
              << std::setw(12) << "P95 (us)"
              << std::setw(10) << "Status"
              << "\n";
    std::cout << "---------------------------------------------------------------------------------------------------------\n";
}

static void printResult(const BenchmarkResult& r) {
    std::cout << std::left
              << std::setw(18) << r.category
              << std::setw(34) << r.name
              << std::right
              << std::setw(9) << r.iterations
              << std::fixed << std::setprecision(3)
              << std::setw(12) << r.avgUs
              << std::setw(12) << r.minUs
              << std::setw(12) << r.p95Us
              << std::setw(10) << (r.passed ? " [PASS] " : " [FAIL] ")
              << "\n";
}

int main() {
    initGdi();
    std::vector<BenchmarkResult> results;

    // -------------------------------------------------------------
    // CATEGORY 1: Keystroke & Input Response (AST Operations)
    // -------------------------------------------------------------
    results.push_back(runBenchmark("Input / Keystroke", "Single Digit Insertion", 10000, []() {
        Expression expr;
        insertDigit(expr, '7');
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Single Operator Insertion", 10000, []() {
        Expression expr;
        insertDigit(expr, '5');
        insertOperator(expr, '+');
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Fraction Structure Insert", 10000, []() {
        Expression expr;
        insertDigit(expr, '1');
        insertFraction(expr);
        insertDigit(expr, '2');
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Power & Sqrt Insert", 10000, []() {
        Expression expr;
        insertVariable(expr, 'x');
        insertPower(expr);
        insertDigit(expr, '2');
        insertSqrt(expr);
        insertDigit(expr, '9');
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Integral Structure Insert", 10000, []() {
        Expression expr;
        insertIntegral(expr);
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Summation Structure Insert", 10000, []() {
        Expression expr;
        insertSummation(expr);
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Parse Arithmetic String", 5000, []() {
        Expression expr;
        insertFromText(expr, "3 + 4 * 2 / (1 - 5)^2");
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Parse Complex Expression", 5000, []() {
        Expression expr;
        insertFromText(expr, "-6 + 24i = 3x + 5yi + i*(5x - 3y)");
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Cursor Nav (moveLeft/Right)", 10000, []() {
        Expression expr;
        insertFromText(expr, "(3 + 4) / 5");
        moveLeft(expr);
        moveLeft(expr);
        moveRight(expr);
        moveUp(expr);
        moveDown(expr);
    }));

    results.push_back(runBenchmark("Input / Keystroke", "Backspace Range Delete", 10000, []() {
        Expression expr;
        insertFromText(expr, "12345");
        moveLeft(expr);
        // Backspace
        deleteRange(expr, expr.cursor, { expr.cursor.row, expr.cursor.index - 1 });
    }));

    // -------------------------------------------------------------
    // CATEGORY 2: 2D Natural Layout & Typesetting
    // -------------------------------------------------------------
    Expression layoutExprSimple;
    insertFromText(layoutExprSimple, "123 + 456 * 789");

    results.push_back(runBenchmark("2D Typesetting", "Measure Simple Row", 5000, [&]() {
        Size s = measureExpression(g_memDC, layoutExprSimple.root.get());
        (void)s;
    }));

    Expression layoutExprNested;
    insertFromText(layoutExprNested, "((x + 1)/(x - 1))^2 + sqrt(x^3 + 5x - 2)");

    results.push_back(runBenchmark("2D Typesetting", "Measure Nested Fractions & Sqrt", 5000, [&]() {
        Size s = measureExpression(g_memDC, layoutExprNested.root.get());
        (void)s;
    }));

    Expression layoutExprCalculus;
    insertSummation(layoutExprCalculus);
    insertVariable(layoutExprCalculus, 'x');
    insertEquals(layoutExprCalculus);
    insertDigit(layoutExprCalculus, '1');
    moveUp(layoutExprCalculus);
    insertDigit(layoutExprCalculus, '1');
    insertDigit(layoutExprCalculus, '0');
    moveRight(layoutExprCalculus);
    insertVariable(layoutExprCalculus, 'x');

    results.push_back(runBenchmark("2D Typesetting", "Measure Structural Summation", 5000, [&]() {
        Size s = measureExpression(g_memDC, layoutExprCalculus.root.get());
        (void)s;
    }));

    results.push_back(runBenchmark("2D Typesetting", "Hit Test (placeCursorAtPoint)", 5000, [&]() {
        placeCursorAtPoint(layoutExprNested, g_memDC, 0, 50, 45, 50);
    }));

    // -------------------------------------------------------------
    // CATEGORY 3: Mathematical Evaluation Engine
    // -------------------------------------------------------------
    Expression evalArithmetic;
    insertFromText(evalArithmetic, "123.45 + 678.90 * 42.0");
    EvaluationContext ctx;

    results.push_back(runBenchmark("Evaluation", "Basic Real Arithmetic", 10000, [&]() {
        double val = evaluate(evalArithmetic.root.get(), ctx);
        (void)val;
    }));

    Expression evalTrig;
    insertFromText(evalTrig, "sin(45)^2 + cos(45)^2 + ln(100)");

    results.push_back(runBenchmark("Evaluation", "Trigonometric & Transcendentals", 5000, [&]() {
        double val = evaluate(evalTrig.root.get(), ctx);
        (void)val;
    }));

    Expression evalQuadratic;
    insertFromText(evalQuadratic, "x^2 - 7x + 12 = 0");

    results.push_back(runBenchmark("Evaluation", "Quadratic Equation Solver", 5000, [&]() {
        QuadraticResult qres;
        std::string msg;
        solveQuadraticEquation(evalQuadratic.root.get(), qres, msg);
    }));

    Expression evalLin1, evalLin2;
    insertFromText(evalLin1, "x + y = 10");
    insertFromText(evalLin2, "2x - y = 5");

    results.push_back(runBenchmark("Evaluation", "2x2 Linear System Solver", 5000, [&]() {
        double x = 0, y = 0;
        std::string msg;
        solveTwoVariableSystem(evalLin1.root.get(), evalLin2.root.get(), x, y, msg);
    }));

    Expression evalComplexEq;
    insertFromText(evalComplexEq, "-6 + 24i = 3x + 5yi + i*(5x - 3y)");

    results.push_back(runBenchmark("Evaluation", "Complex Linear System (i)", 3000, [&]() {
        double x = 0, y = 0;
        std::string msg;
        solveComplexEquation(evalComplexEq.root.get(), x, y, msg);
    }));

    Expression evalBigFactorial;
    insertFromText(evalBigFactorial, "10000000000!");
    EvaluationContext bigCtx;
    bigCtx.bigNumbers = true;

    results.push_back(runBenchmark("Evaluation", "BigValue 10^10! Factorial", 2000, [&]() {
        std::string res = evaluateProToString(evalBigFactorial.root.get(), bigCtx);
        (void)res;
    }));

    Expression evalSummation;
    insertSummation(evalSummation);
    insertVariable(evalSummation, 'x');
    insertEquals(evalSummation);
    insertDigit(evalSummation, '1');
    moveUp(evalSummation);
    insertDigit(evalSummation, '1');
    insertDigit(evalSummation, '0');
    insertDigit(evalSummation, '0'); // upper = 100
    moveRight(evalSummation);
    insertVariable(evalSummation, 'x'); // body = x

    results.push_back(runBenchmark("Evaluation", "Definite Summation (1 to 100)", 2000, [&]() {
        double res = evaluate(evalSummation.root.get(), ctx);
        (void)res;
    }));

    Expression evalIntegral;
    insertIntegral(evalIntegral);
    insertVariable(evalIntegral, 'x'); // integrand = x
    moveDown(evalIntegral);
    insertDigit(evalIntegral, '0'); // lower = 0
    moveUp(evalIntegral);
    insertDigit(evalIntegral, '1'); // upper = 1

    results.push_back(runBenchmark("Evaluation", "Definite Integral (Simpson's)", 2000, [&]() {
        double res = evaluate(evalIntegral.root.get(), ctx);
        (void)res;
    }));

    // -------------------------------------------------------------
    // CATEGORY 4: Cold Startup & Memory Footprint
    // -------------------------------------------------------------
    results.push_back(runBenchmark("System / Startup", "Workspace Init & Reset", 2000, []() {
        Workspace ws;
        ws.commitCurrent();
    }));

    // Memory footprint analysis
    PROCESS_MEMORY_COUNTERS pmc{};
    BOOL memOk = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    double workingSetMB = memOk ? (double)pmc.WorkingSetSize / (1024.0 * 1024.0) : 0.0;

    // Output all results
    printHeader();
    bool allPassed = true;
    for (const auto& r : results) {
        printResult(r);
        if (!r.passed) allPassed = false;
    }

    std::cout << "=========================================================================================================\n";
    std::cout << "\n[RESOURCE FOOTPRINT METRICS]\n";
    std::cout << "  Process Working Set Memory: " << std::fixed << std::setprecision(2) << workingSetMB << " MB\n";
    std::cout << "  Memory Footprint Claim:     < 30 MB (Standard Win32 Native Application)\n";
    std::cout << "  Memory Status:              " << (workingSetMB < 30.0 ? "[PASS]" : "[WARN]") << "\n";

    std::cout << "\n[BENCHMARK VERDICT]\n";
    if (allPassed) {
        std::cout << "  >>> ALL CLAIMS VERIFIED: Every keystroke, typesetting, and math evaluation operation executes in <<<\n";
        std::cout << "  >>> SUB-MILLISECOND LATENCY (< 1.000 ms), confirming the README specifications.                    <<<\n";
    } else {
        std::cout << "  >>> WARNING: One or more benchmark operations exceeded the 1.0 ms sub-millisecond threshold. <<<\n";
    }
    std::cout << "=========================================================================================================\n\n";

    cleanupGdi();
    return allPassed ? 0 : 1;
}
