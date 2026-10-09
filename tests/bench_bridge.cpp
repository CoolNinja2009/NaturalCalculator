#define NOMINMAX
#include <windows.h>
#include "expr_tree.h"
#include "evaluator.h"
#include "layout.h"
#include "workspace.h"

static HDC g_benchDC = nullptr;

extern "C" {

__declspec(dllexport) void bench_init() {
    if (!g_benchDC) {
        HDC screen = GetDC(nullptr);
        g_benchDC = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
}

__declspec(dllexport) void bench_cleanup() {
    if (g_benchDC) {
        DeleteDC(g_benchDC);
        g_benchDC = nullptr;
    }
}

__declspec(dllexport) void bench_keystroke_digit() {
    Expression expr;
    insertDigit(expr, '7');
}

__declspec(dllexport) void bench_keystroke_fraction() {
    Expression expr;
    insertDigit(expr, '1');
    insertFraction(expr);
    insertDigit(expr, '2');
}

__declspec(dllexport) void bench_keystroke_structure() {
    Expression expr;
    insertSummation(expr);
    insertIntegral(expr);
}

__declspec(dllexport) void bench_parse_arithmetic() {
    Expression expr;
    insertFromText(expr, "3 + 4 * 2 / (1 - 5)^2");
}

__declspec(dllexport) void bench_parse_complex_eq() {
    Expression expr;
    insertFromText(expr, "-6 + 24i = 3x + 5yi + i*(5x - 3y)");
}

__declspec(dllexport) void bench_cursor_navigation() {
    Expression expr;
    insertFromText(expr, "(3 + 4) / 5");
    moveLeft(expr);
    moveLeft(expr);
    moveRight(expr);
    moveUp(expr);
    moveDown(expr);
}

__declspec(dllexport) void bench_measure_layout_simple() {
    static Expression* expr = nullptr;
    if (!expr) {
        expr = new Expression();
        insertFromText(*expr, "123 + 456 * 789");
    }
    Size s = measureExpression(g_benchDC, expr->root.get());
    (void)s;
}

__declspec(dllexport) void bench_measure_layout_complex() {
    static Expression* expr = nullptr;
    if (!expr) {
        expr = new Expression();
        insertFromText(*expr, "((x + 1)/(x - 1))^2 + sqrt(x^3 + 5x - 2)");
    }
    Size s = measureExpression(g_benchDC, expr->root.get());
    (void)s;
}

__declspec(dllexport) void bench_measure_layout_summation() {
    static Expression* expr = nullptr;
    if (!expr) {
        expr = new Expression();
        insertSummation(*expr);
        insertVariable(*expr, 'x');
        insertEquals(*expr);
        insertDigit(*expr, '1');
        moveUp(*expr);
        insertDigit(*expr, '1');
        insertDigit(*expr, '0');
        moveRight(*expr);
        insertVariable(*expr, 'x');
    }
    Size s = measureExpression(g_benchDC, expr->root.get());
    (void)s;
}

__declspec(dllexport) double bench_evaluate_arithmetic() {
    static Expression* expr = nullptr;
    static EvaluationContext ctx;
    if (!expr) {
        expr = new Expression();
        insertFromText(*expr, "123.45 + 678.90 * 42.0");
    }
    return evaluate(expr->root.get(), ctx);
}

__declspec(dllexport) double bench_evaluate_transcendental() {
    static Expression* expr = nullptr;
    static EvaluationContext ctx;
    if (!expr) {
        expr = new Expression();
        insertFromText(*expr, "sin(45)^2 + cos(45)^2 + ln(100)");
    }
    return evaluate(expr->root.get(), ctx);
}

__declspec(dllexport) int bench_solve_quadratic() {
    static Expression* expr = nullptr;
    if (!expr) {
        expr = new Expression();
        insertFromText(*expr, "x^2 - 7x + 12 = 0");
    }
    QuadraticResult res;
    std::string msg;
    return solveQuadraticEquation(expr->root.get(), res, msg) ? 1 : 0;
}

__declspec(dllexport) int bench_solve_linear_system() {
    static Expression* e1 = nullptr;
    static Expression* e2 = nullptr;
    if (!e1) {
        e1 = new Expression();
        e2 = new Expression();
        insertFromText(*e1, "x + y = 10");
        insertFromText(*e2, "2x - y = 5");
    }
    double x = 0, y = 0;
    std::string msg;
    return solveTwoVariableSystem(e1->root.get(), e2->root.get(), x, y, msg) ? 1 : 0;
}

__declspec(dllexport) int bench_solve_complex_eq() {
    static Expression* expr = nullptr;
    if (!expr) {
        expr = new Expression();
        insertFromText(*expr, "-6 + 24i = 3x + 5yi + i*(5x - 3y)");
    }
    double x = 0, y = 0;
    std::string msg;
    return solveComplexEquation(expr->root.get(), x, y, msg) ? 1 : 0;
}

__declspec(dllexport) void bench_evaluate_bigvalue() {
    static Expression* expr = nullptr;
    static EvaluationContext ctx;
    ctx.bigNumbers = true;
    if (!expr) {
        expr = new Expression();
        insertFromText(*expr, "10000000000!");
    }
    std::string s = evaluateProToString(expr->root.get(), ctx);
    (void)s;
}

__declspec(dllexport) double bench_evaluate_summation() {
    static Expression* expr = nullptr;
    static EvaluationContext ctx;
    if (!expr) {
        expr = new Expression();
        insertSummation(*expr);
        insertVariable(*expr, 'x');
        insertEquals(*expr);
        insertDigit(*expr, '1');
        moveUp(*expr);
        insertDigit(*expr, '1');
        insertDigit(*expr, '0');
        insertDigit(*expr, '0');
        moveRight(*expr);
        insertVariable(*expr, 'x');
    }
    return evaluate(expr->root.get(), ctx);
}

__declspec(dllexport) double bench_evaluate_integral() {
    static Expression* expr = nullptr;
    static EvaluationContext ctx;
    if (!expr) {
        expr = new Expression();
        insertIntegral(*expr);
        insertVariable(*expr, 'x');
        moveDown(*expr);
        insertDigit(*expr, '0');
        moveUp(*expr);
        insertDigit(*expr, '1');
    }
    return evaluate(expr->root.get(), ctx);
}

} // extern "C"
