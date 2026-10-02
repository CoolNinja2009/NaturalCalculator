#include "evaluator.h"
#include "expr_tree.h"
#include "workspace.h"
#include <cassert>
#include <cmath>
#include <iostream>

static Expression expressionFrom(const char* text) {
    Expression expression;
    for (const char* p = text; *p; ++p) {
        if (*p >= '0' && *p <= '9') insertDigit(expression, *p);
        else if (*p == 'x' || *p == 'y') insertVariable(expression, *p);
        else if (*p == '+' || *p == '-' || *p == '*') {
            if (expression.cursor.row != expression.root.get()) moveRight(expression);
            insertOperator(expression, *p);
        }
        else if (*p == '^') { insertPower(expression); }
        else if (*p == '=') { if (expression.cursor.row != expression.root.get()) moveRight(expression); insertEquals(expression); }
        else if (*p == '!') insertOperator(expression, '!');
        else if (*p == '(') insertOpenParen(expression);
        else if (*p == ')') insertCloseParen(expression);
        else if (*p == '.') insertDigit(expression, *p);
    }
    return expression;
}

static void assertNear(double actual, double expected) {
    assert(std::fabs(actual - expected) < 1e-9);
}

static void testVariableEvaluation() {
    Expression expression = expressionFrom("x+2*y");
    assertNear(evaluate(expression.root.get(), { 3.0, 4.0 }), 11.0);
}

static void testSingleVariableEquation() {
    Expression equation = expressionFrom("x+2y=x-3");
    char variable = 0;
    double value = 0.0;
    std::string message;
    assert(solveSingleVariableEquation(equation.root.get(), variable, value, message));
    assert(variable == 'y');
    assertNear(value, -1.5);
}

static void testSystems() {
    Expression first = expressionFrom("x+y=5");
    Expression second = expressionFrom("x-y=6");
    double x = 0.0;
    double y = 0.0;
    std::string message;
    assert(solveTwoVariableSystem(first.root.get(), second.root.get(), x, y, message));
    assertNear(x, 5.5);
    assertNear(y, -0.5);

    Expression parallel = expressionFrom("x+y=5");
    Expression inconsistent = expressionFrom("2x+2y=12");
    assert(!solveTwoVariableSystem(parallel.root.get(), inconsistent.root.get(), x, y, message));
    assert(message == "No unique solution");

    Expression malformed = expressionFrom("x+y");
    assert(!solveTwoVariableSystem(malformed.root.get(), second.root.get(), x, y, message));
}

static void testQuadratics() {
    QuadraticResult two = solveQuadratic(1.0, -5.0, 6.0);
    assert(two.rootCount == 2);
    assertNear(two.first, 3.0);
    assertNear(two.second, 2.0);

    QuadraticResult repeated = solveQuadratic(1.0, 2.0, 1.0);
    assert(repeated.rootCount == 1);
    assertNear(repeated.first, -1.0);

    QuadraticResult none = solveQuadratic(1.0, 0.0, 1.0);
    assert(none.rootCount == 0);

    QuadraticResult linear = solveQuadratic(0.0, 2.0, -6.0);
    assert(linear.rootCount == 1);
    assertNear(linear.first, 3.0);

    QuadraticResult identity = solveQuadratic(0.0, 0.0, 0.0);
    assert(!identity.valid);
    QuadraticResult contradiction = solveQuadratic(0.0, 0.0, 1.0);
    assert(!contradiction.valid);

    Expression typed = expressionFrom("x^2+5x+6=0");
    QuadraticResult typedResult;
    std::string message;
    assert(solveQuadraticEquation(typed.root.get(), typedResult, message));
    assert(typedResult.rootCount == 2);
    assertNear(typedResult.first, -2.0);
    assertNear(typedResult.second, -3.0);
}

static void testInvalidLinearTerms() {
    Expression nonlinear = expressionFrom("x*y=1");
    char variable = 0;
    double value = 0.0;
    std::string message;
    assert(!solveSingleVariableEquation(nonlinear.root.get(), variable, value, message));
    assert(message == "Equation must be linear");

    Expression duplicate = expressionFrom("x=1=2");
    assert(!solveSingleVariableEquation(duplicate.root.get(), variable, value, message));
    assert(message == "Use one '=' per equation");

    Expression assignment = expressionFrom("x=2+34");
    assert(isLinearEquation(assignment.root.get()));
    assert(solveSingleVariableEquation(assignment.root.get(), variable, value, message));
    assert(variable == 'x');
    assertNear(value, 36.0);
    Expression nonlinearEquation = expressionFrom("x=y!");
    assert(!isLinearEquation(nonlinearEquation.root.get()));
}

static void testAssignmentsPersist() {
    Workspace workspace;
    workspace.current() = expressionFrom("x=5");
    assert(workspace.commitCurrent());
    assert(workspace.hasSolvedValues());
    assertNear(workspace.solvedValues().x, 5.0);

    workspace.current() = expressionFrom("x*2");
    assert(workspace.commitCurrent(workspace.solvedValues()));
    assert(workspace.history().back()->result == "10");

    workspace.clearVariables();
    workspace.current() = expressionFrom("x");
    assert(workspace.commitCurrent(workspace.solvedValues()));
    assert(workspace.history().back()->result == "0");
    assert(workspace.history().size() == 3);

    Workspace rootWorkspace;
    rootWorkspace.current() = expressionFrom("x=4");
    assert(rootWorkspace.commitCurrent());
    Expression rootAssignment;
    insertVariable(rootAssignment, 'y');
    insertEquals(rootAssignment);
    insertSqrt(rootAssignment);
    insertVariable(rootAssignment, 'x');
    rootWorkspace.current() = std::move(rootAssignment);
    assert(rootWorkspace.commitCurrent(rootWorkspace.solvedValues()));
    assert(rootWorkspace.history().back()->result == "y = 2");
}

static void testGeneralEquations() {
    Workspace workspace;
    workspace.current() = expressionFrom("2^x+x=8");
    assert(workspace.commitCurrent());
    assert(workspace.history().back()->result.find("x =") == 0);
    assert(workspace.history().back()->result.find("2.4") != std::string::npos);
    assertNear(workspace.solvedValues().x, std::stod(workspace.history().back()->result.substr(4)));

    workspace.current() = expressionFrom("x^2+2x-4");
    assert(workspace.commitCurrent());
    assert(workspace.history().back()->result.find("x1 =") == 0);
    assert(workspace.history().back()->result.find(", x2 =") != std::string::npos);

    workspace.current() = expressionFrom("x^2+1=0");
    assert(workspace.commitCurrent());
    assert(workspace.history().back()->result == "\xE2\x88\x85");

    workspace.current() = expressionFrom("2^x+1=0");
    assert(workspace.commitCurrent());
    assert(workspace.history().back()->result == "\xE2\x88\x85");

    workspace.current() = expressionFrom("2^x^2=1");
    assert(workspace.commitCurrent());
    assert(workspace.history().back()->result.find("x = 0") == 0);

    workspace.current() = expressionFrom("x^2=2");
    assert(workspace.commitCurrent());
    assert(workspace.history().back()->result.find("\xE2\x88\x9A") != std::string::npos);
    assert(workspace.history().back()->result.find("x1 = \xE2\x88\x9A" "2") == 0);
    assert(workspace.history().back()->result.find(" = ") != std::string::npos);

    Expression squareRootPower;
    insertDigit(squareRootPower, '2');
    insertPower(squareRootPower);
    insertDigit(squareRootPower, '1');
    insertFraction(squareRootPower);
    insertDigit(squareRootPower, '2');
    assert(evaluateToString(squareRootPower.root.get()).find("\xE2\x88\x9A" "2 = ") == 0);
}

static void testFactorials() {
    Expression five = expressionFrom("5");
    insertOperator(five, '!');
    assertNear(evaluate(five.root.get()), 120.0);

    Expression repeated = expressionFrom("3");
    insertOperator(repeated, '!');
    insertOperator(repeated, '!');
    assertNear(evaluate(repeated.root.get()), 720.0);

    Expression fractional = expressionFrom("2.5");
    insertOperator(fractional, '!');
    bool failed = false;
    try { evaluate(fractional.root.get()); }
    catch (const std::runtime_error& error) {
        failed = std::string(error.what()) == "Factorial needs a non-negative integer";
    }
    assert(failed);

    Expression negative;
    insertOpenParen(negative);
    insertOperator(negative, '-');
    insertDigit(negative, '3');
    insertCloseParen(negative);
    insertOperator(negative, '!');
    failed = false;
    try { evaluate(negative.root.get()); }
    catch (const std::runtime_error& error) {
        failed = std::string(error.what()) == "Factorial needs a non-negative integer";
    }
    assert(failed);

    Expression cancellation = expressionFrom("200!-200!");
    assertNear(evaluate(cancellation.root.get()), 0.0);
}

static void testStandaloneClosingParen() {
    Expression expression = expressionFrom("5+4)");
    assert(expression.toPlainString() == "5 + 4)");
    backspace(expression);
    assert(expression.toPlainString() == "5 + 4");
}

static void testPowerEditing() {
    Expression expression;
    insertDigit(expression, '2');
    insertPower(expression);
    insertDigit(expression, '2');
    insertPower(expression);
    std::cerr << "Power: [" << expression.toPlainString() << "]\n";
    backspace(expression);
    assert(expression.toPlainString() == "(2)^(2)");
}

int main() {
    testVariableEvaluation();
    testSingleVariableEquation();
    testSystems();
    testQuadratics();
    testInvalidLinearTerms();
    testAssignmentsPersist();
    testGeneralEquations();
    testFactorials();
    testStandaloneClosingParen();
    testPowerEditing();
    std::cout << "All calculator edge-case tests passed\n";
    return 0;
}