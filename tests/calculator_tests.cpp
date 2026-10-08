#include "evaluator.h"
#include "expr_tree.h"
#include "layout.h"
#include "workspace.h"
#include "graph.h"
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
    if (!(std::fabs(actual - expected) < 1e-9))
        std::cerr << "assertNear failed: actual=" << actual << " expected=" << expected << "\n";
    assert(std::fabs(actual - expected) < 1e-9);
}

static double evalText(const char* text, const EvaluationContext& context) {
    Expression expression;
    insertFromText(expression, text);
    return evaluate(expression.root.get(), context);
}

static std::string evalTextString(const char* text, const EvaluationContext& context) {
    Expression expression;
    insertFromText(expression, text);
    return evaluateToString(expression.root.get(), context);
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

    Expression nonlinearFirst;
    insertFromText(nonlinearFirst, "sqrt(3233+x/2)=y-45");
    Expression nonlinearSecond;
    insertFromText(nonlinearSecond, "xy=4");
    Workspace nonlinearWorkspace;
    nonlinearWorkspace.current() = std::move(nonlinearFirst);
    assert(nonlinearWorkspace.commitCurrent());
    nonlinearWorkspace.current() = std::move(nonlinearSecond);
    assert(nonlinearWorkspace.commitCurrent());
    assert(nonlinearWorkspace.history()[nonlinearWorkspace.history().size() - 2]->result ==
           "Solved as part of the system");
    assert(nonlinearWorkspace.history().back()->result.find("x = ") == 0);
    assertNear(nonlinearWorkspace.solvedValues().x * nonlinearWorkspace.solvedValues().y, 4.0);
    assertNear(std::sqrt(3233.0 + nonlinearWorkspace.solvedValues().x / 2.0),
               nonlinearWorkspace.solvedValues().y - 45.0);
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

static void testProModeTriggerCalculation() {
    Workspace workspace;
    workspace.current() = expressionFrom("2000!-1999!");
    assert(isProModeTrigger(workspace.current().root.get()));
    assert(workspace.commitCurrent());
    assert(workspace.history().back()->result.find("1999 * 1999!") == 0);
    assert(workspace.history().back()->result.find("10^") != std::string::npos);

    Expression other = expressionFrom("2000!-1998!");
    assert(!isProModeTrigger(other.root.get()));
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

static void testCloseParenExitsNestedStructure() {
    Expression expression;
    insertOpenParen(expression);
    insertVariable(expression, 'x');
    insertPower(expression);
    insertDigit(expression, '2');
    insertCloseParen(expression);
    insertOperator(expression, '+');
    insertDigit(expression, '1');

    assert(expression.toPlainString() == "((x)^(2)) + 1");
    assertNear(evaluate(expression.root.get(), { 3.0, 0.0 }), 10.0);
}

static void testClickPlacesStructuralCursor() {
    HDC hdc = CreateCompatibleDC(nullptr);
    assert(hdc);
    HFONT font = CreateFontW(-fontHeightForDepth(0), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    assert(font);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SIZE prefix{};
    GetTextExtentPoint32W(hdc, L"12", 2, &prefix);
    SelectObject(hdc, oldFont);
    DeleteObject(font);

    Expression expression = expressionFrom("1234");
    assert(placeCursorAtPoint(expression, hdc, 10, 50, 10 + prefix.cx, 50));
    insertOperator(expression, '+');
    assert(expression.toPlainString() == "12 + 34");

    Expression digits = expressionFrom("1234");
    assert(placeCursorAtPoint(digits, hdc, 10, 50, 10 + prefix.cx, 50));
    insertDigit(digits, '9');
    insertDigit(digits, '8');
    assert(digits.toPlainString() == "129834");
    assertNear(evaluate(digits.root.get()), 129834.0);

    Expression power;
    insertVariable(power, 'x');
    insertPower(power);
    insertDigit(power, '2');
    moveRight(power);
    insertOperator(power, '+');
    insertDigit(power, '3');
    Cursor exponentStart{ power.root->items[0]->b.get(), 0 };
    CaretInfo exponentCaret;
    drawExpression(hdc, power.root.get(), 10, 50, darkTheme(),
                   &exponentStart, &exponentCaret);
    assert(exponentCaret.valid);
    int exponentY = (exponentCaret.top + exponentCaret.bottom) / 2;
    assert(placeCursorAtPoint(power, hdc, 10, 50, exponentCaret.x, exponentY));
    insertDigit(power, '4');
    assert(power.toPlainString() == "(x)^(42) + 3");
    assertNear(evaluate(power.root.get(), { 2.0, 0.0 }), std::pow(2.0, 42.0) + 3.0);
    DeleteDC(hdc);
}

static void testProModeBigNumbers() {
    EvaluationContext pro;
    pro.bigNumbers = true;

    // 10,000,000,000! — far beyond double range; log10 factorials via lgamma.
    Expression hugeFactorial = expressionFrom("10000000000!");
    std::string factorialResult = evaluateProToString(hugeFactorial.root.get(), pro);
    assert(factorialResult.find("* 10^95657055186") != std::string::npos);

    // Outside pro mode the same input keeps its historical error.
    Expression plainFactorial = expressionFrom("10000000000!");
    assert(evaluateToString(plainFactorial.root.get()) == "Factorial result is too large");

    // Astronomic powers: 2^10000000000.
    Expression hugePower = expressionFrom("2^10000000000");
    std::string powerResult = evaluateProToString(hugePower.root.get(), pro);
    assert(powerResult.find("* 10^3010299956") != std::string::npos);

    // Overflowing intermediates cancel exactly in log domain.
    Expression cancel = expressionFrom("10^400");
    insertFraction(cancel);
    insertDigit(cancel, '1');
    insertDigit(cancel, '0');
    insertPower(cancel);
    insertDigit(cancel, '4');
    insertDigit(cancel, '0');
    insertDigit(cancel, '0');
    assert(evaluateProToString(cancel.root.get(), pro) == "1");

    // Huge + small terms stay huge.
    Expression mixed = expressionFrom("10^400+5");
    assert(evaluateProToString(mixed.root.get(), pro) == "10^400");

    // Roots of huge numbers: sqrt(10^400) = 10^200.
    Expression bigRoot;
    insertSqrt(bigRoot);
    insertDigit(bigRoot, '1');
    insertDigit(bigRoot, '0');
    insertPower(bigRoot);
    insertDigit(bigRoot, '4');
    insertDigit(bigRoot, '0');
    insertDigit(bigRoot, '0');
    assert(evaluateProToString(bigRoot.root.get(), pro) == "10^200");

    // Ordinary values are untouched by pro mode.
    Expression simple = expressionFrom("2+3");
    assert(evaluateProToString(simple.root.get(), pro) == "5");

    // Real math errors still surface as errors.
    Expression divisionByZero;
    insertDigit(divisionByZero, '1');
    insertFraction(divisionByZero);
    insertDigit(divisionByZero, '0');
    bool threw = false;
    try { evaluateProToString(divisionByZero.root.get(), pro); }
    catch (const std::exception& error) {
        threw = std::string(error.what()) == "Division by zero";
    }
    assert(threw);

    // Fractional factorial input stays rejected in pro mode too.
    Expression fractional = expressionFrom("2.5!");
    threw = false;
    try { evaluateProToString(fractional.root.get(), pro); }
    catch (const std::exception& error) {
        threw = std::string(error.what()) == "Factorial needs a non-negative integer";
    }
    assert(threw);

    // End-to-end through the workspace commit path.
    Workspace proWorkspace;
    proWorkspace.current() = expressionFrom("10000000000!");
    assert(proWorkspace.commitCurrent(pro));
    assert(proWorkspace.history().back()->result.find("* 10^95657055186") != std::string::npos);
    assert(!proWorkspace.history().back()->isError);
}

static void testSciFunctions() {
    EvaluationContext deg;              // degrees = true by default
    EvaluationContext rad;
    rad.degrees = false;

    // Trig input honours the angle unit.
    assertNear(evalText("sin(30)", deg), 0.5);
    assertNear(evalText("cos(60)", deg), 0.5);
    assertNear(evalText("tan(45)", deg), 1.0);
    assertNear(evalText("sin(30)", rad), -0.9880316240928618);
    assertNear(evalText("sin(pi/2)", rad), 1.0);
    assertNear(evalText("sin(pi/2)", deg), 0.02741213349327057); // sin of 1.5708deg

    // Inverse trig returns angles in the active unit.
    assertNear(evalText("asin(0.5)", deg), 30.0);
    assertNear(evalText("acos(0.5)", deg), 60.0);
    assertNear(evalText("atan(1)", deg), 45.0);
    assertNear(evalText("asin(0.5)", rad), 0.5235987755982989);

    // Hyperbolic functions are angle-independent.
    assertNear(evalText("sinh(1)", deg), 1.1752011936438014);
    assertNear(evalText("cosh(0)", deg), 1.0);
    assertNear(evalText("tanh(0)", deg), 0.0);
    assertNear(evalText("sinh(1)", rad), evalText("sinh(1)", deg));

    // Logs, exp, abs, constants.
    assertNear(evalText("ln(e)", deg), 1.0);
    assertNear(evalText("log(100)", deg), 2.0);
    assertNear(evalText("exp(0)", deg), 1.0);
    assertNear(evalText("exp(1)", deg), 2.718281828459045);
    assertNear(evalText("abs(0-3)", deg), 3.0);
    assertNear(evalText("pi", deg), 3.141592653589793);
    assertNear(evalText("e", deg), 2.718281828459045);
    assertNear(evalText("2pi", deg), 6.283185307179586);
    assertNear(evalText("pi^2", deg), 9.869604401089358);

    // Nesting and precedence alongside the structural operators.
    assertNear(evalText("sqrt(9)+log(100)", deg), 5.0);
    assertNear(evalText("2^3", deg), 8.0);
    assertNear(evalText("sin(30)+cos(60)", deg), 1.0);
    assertNear(evalText("sin(30)cos(60)", deg), 0.25);
    assertNear(evalText("2sin(30)", deg), 1.0);

    // Domain errors surface as error text, never as numeric garbage.
    assert(evalTextString("ln(0)", deg) == "ln needs a positive number");
    assert(evalTextString("ln(0-2)", deg) == "ln needs a positive number");
    assert(evalTextString("log(0)", deg) == "log needs a positive number");
    assert(evalTextString("asin(2)", deg) == "asin needs input in -1..1");
    assert(evalTextString("acos(2)", rad) == "acos needs input in -1..1");
    assert(evalTextString("tan(90)", deg) == "tan is undefined here");
    assert(evalTextString("sqrt(0-1)", deg) == "Root of negative number");
    assert(evalTextString("1/0", deg) == "Division by zero");

    // A word that never resolves stays a name and reports itself.
    Expression unresolved;
    insertNameLetter(unresolved, 's');
    insertNameLetter(unresolved, 'i');
    insertNameLetter(unresolved, 'n');
    normalizeNames(unresolved);   // no paren follows: still a Name
    assert(evaluateToString(unresolved.root.get(), deg) == "Unknown name");
}

static void testSciTypingAndText() {
    EvaluationContext deg;

    // Letter-by-letter typing resolves on the next non-letter edit.
    Expression typed;
    insertNameLetter(typed, 's');
    insertNameLetter(typed, 'i');
    insertNameLetter(typed, 'n');
    insertOpenParen(typed);
    normalizeNames(typed);   // doAction() performs this after button edits
    insertDigit(typed, '3');
    insertDigit(typed, '0');
    assert(typed.root->items[0]->type == ItemType::Function);
    assert(typed.root->items[0]->functionId == SciSin);
    assertNear(evaluate(typed.root.get(), deg), 0.5);

    // "exp(" keeps the leading e from becoming a constant mid-word.
    Expression expTyped;
    insertNameLetter(expTyped, 'e');
    insertNameLetter(expTyped, 'x');
    insertNameLetter(expTyped, 'p');
    insertOpenParen(expTyped);
    normalizeNames(expTyped);
    insertDigit(expTyped, '1');
    assert(expTyped.root->items[0]->type == ItemType::Function);
    assertNear(evaluate(expTyped.root.get(), deg), 2.718281828459045);

    // A lone "e" becomes the constant once a non-letter follows.
    Expression eTyped;
    insertNameLetter(eTyped, 'e');
    insertOperator(eTyped, '+');
    normalizeNames(eTyped);
    insertDigit(eTyped, '1');
    assert(eTyped.root->items[0]->type == ItemType::Constant);
    assertNear(evaluate(eTyped.root.get(), deg), 3.718281828459045);

    // x / y still resolve to variables through the same pipeline.
    Expression xTyped;
    insertNameLetter(xTyped, 'x');
    insertOperator(xTyped, '+');
    normalizeNames(xTyped);
    insertDigit(xTyped, '2');
    EvaluationContext withX;
    withX.x = 5.0;
    assert(xTyped.root->items[0]->type == ItemType::Variable);
    assertNear(evaluate(xTyped.root.get(), withX), 7.0);

    // Serialization survives a copy/paste round-trip.
    Expression roundTrip;
    insertFromText(roundTrip, "sin(pi/4)+2^3");
    std::string plain = roundTrip.toPlainString();
    assert(plain.find("sin(") != std::string::npos);
    assert(plain.find("pi") != std::string::npos);
    Expression reparsed;
    insertFromText(reparsed, plain);
    EvaluationContext rad;
    rad.degrees = false;
    assertNear(evaluate(reparsed.root.get(), rad),
               0.7071067811865476 + 8.0);

    // Case-insensitive text entry.
    assertNear(evalText("SIN(30)", deg), 0.5);
    assertNear(evalText("Log(100)", deg), 2.0);

    // Unrecognized words drop out, like any unrecognized character.
    assertNear(evalText("3q7", deg), 37.0);
}

static void testSciBigMode() {
    EvaluationContext pro;
    pro.bigNumbers = true;
    EvaluationContext rad;
    rad.bigNumbers = true;
    rad.degrees = false;

    // exp overflows a double and reruns in log10 space.
    Expression expBig;
    insertFromText(expBig, "exp(1000)");
    std::string expResult = evaluateProToString(expBig.root.get(), pro);
    assert(expResult.find("* 10^434") != std::string::npos);

    // exp of a huge negative gives a real tiny value, not zero.
    Expression expTiny;
    insertFromText(expTiny, "exp(0-1000)");
    std::string tinyResult = evaluateProToString(expTiny.root.get(), pro);
    assert(tinyResult.find("10^") != std::string::npos);
    assert(tinyResult.find("-435") != std::string::npos);

    // Logs of values beyond double range stay exact.
    Expression lnBig;
    insertFromText(lnBig, "ln(10^1000)");
    assert(evaluateProToString(lnBig.root.get(), pro).find("2302.585") == 0);
    Expression logBig;
    insertFromText(logBig, "log(10^100)");
    assert(evaluateProToString(logBig.root.get(), pro) == "100");

    // Hyperbolics extend into the huge regime.
    Expression coshBig;
    insertFromText(coshBig, "cosh(10^100)");
    // log10(cosh(10^100)) = 10^100*log10(e) - log10(2)
    assert(evaluateProToString(coshBig.root.get(), pro).find("10^4342944819032519") == 0);
    Expression tanhBig;
    insertFromText(tanhBig, "tanh(10^50)");
    assert(evaluateProToString(tanhBig.root.get(), pro) == "1");

    // Trig argument reduction beyond double precision is an honest error.
    Expression sinBig;
    insertFromText(sinBig, "sin(10^400)");
    bool threw = false;
    try { evaluateProToString(sinBig.root.get(), pro); }
    catch (const std::exception& error) {
        threw = std::string(error.what()) == "Argument too large for trig";
    }
    assert(threw);

    // abs keeps the magnitude of a huge negative.
    Expression absBig;
    insertFromText(absBig, "abs(0-10^400)");
    assert(evaluateProToString(absBig.root.get(), pro) == "10^400");

    // End-to-end through the workspace with the angle unit attached.
    Workspace workspace;
    insertFromText(workspace.current(), "sin(30)");
    EvaluationContext degPro;
    degPro.bigNumbers = true;
    assert(workspace.commitCurrent(degPro));
    assert(workspace.history().back()->result == "0.5");
    assert(!workspace.history().back()->isError);
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

static void testCombinatoricsTypingAndEvaluation() {
    EvaluationContext ctx;
    ctx.degrees = false;

    // Direct text / auto-format parsing
    Expression e1;
    insertFromText(e1, "2p2");
    assert(e1.root->items.size() == 1);
    assert(e1.root->items[0]->type == ItemType::Permutation);
    assert(e1.toPlainString() == "²P₂");
    assertNear(evaluate(e1.root.get(), ctx), 2.0);

    Expression e2;
    insertFromText(e2, "2c2");
    assert(e2.root->items.size() == 1);
    assert(e2.root->items[0]->type == ItemType::Combination);
    assert(e2.toPlainString() == "²C₂");
    assertNear(evaluate(e2.root.get(), ctx), 1.0);

    Expression e3;
    insertFromText(e3, "5nPr3");
    assert(e3.toPlainString() == "⁵P₃");
    assertNear(evaluate(e3.root.get(), ctx), 60.0);

    Expression e4;
    insertFromText(e4, "5nCr3");
    assert(e4.toPlainString() == "⁵C₃");
    assertNear(evaluate(e4.root.get(), ctx), 10.0);

    // Interactive simulated keystroke typing: "2", "p", "2"
    Expression typedP;
    insertDigit(typedP, '2');
    insertNameLetter(typedP, 'p');
    insertDigit(typedP, '2');
    normalizeNames(typedP);
    assert(typedP.root->items.size() == 1);
    assert(typedP.root->items[0]->type == ItemType::Permutation);
    assert(typedP.toPlainString() == "²P₂");
    assertNear(evaluate(typedP.root.get(), ctx), 2.0);

    // Interactive simulated keystroke typing: "5", "c", "3"
    Expression typedC;
    insertDigit(typedC, '5');
    insertNameLetter(typedC, 'c');
    insertDigit(typedC, '3');
    normalizeNames(typedC);
    assert(typedC.root->items.size() == 1);
    assert(typedC.root->items[0]->type == ItemType::Combination);
    assert(typedC.toPlainString() == "⁵C₃");
    assertNear(evaluate(typedC.root.get(), ctx), 10.0);

    // Round-trip plain string containing Unicode superscript & subscript
    Expression unicodeP;
    insertFromText(unicodeP, "²P₂");
    assert(unicodeP.root->items[0]->type == ItemType::Permutation);
    assertNear(evaluate(unicodeP.root.get(), ctx), 2.0);

    Expression unicodeC;
    insertFromText(unicodeC, "⁵C₃");
    assert(unicodeC.root->items[0]->type == ItemType::Combination);
    assertNear(evaluate(unicodeC.root.get(), ctx), 10.0);

    // Domain errors: r > n
    Expression badR;
    insertFromText(badR, "3p5");
    bool threw = false;
    try { evaluate(badR.root.get(), ctx); }
    catch (const std::exception& e) {
        threw = (std::string(e.what()).find("exceed") != std::string::npos);
    }
    assert(threw);
}

static void testAllScientificFunctions() {
    EvaluationContext rad;
    rad.degrees = false;
    EvaluationContext deg;
    deg.degrees = true;

    // Reciprocal trig
    assertNear(evalText("sec(0)", rad), 1.0);
    assertNear(evalText("csc(pi/2)", rad), 1.0);
    assertNear(evalText("cot(pi/4)", rad), 1.0);

    // Inverse reciprocal trig
    assertNear(evalText("asec(1)", rad), 0.0);
    assertNear(evalText("acsc(1)", rad), 3.141592653589793 / 2.0);
    assertNear(evalText("acot(1)", rad), 3.141592653589793 / 4.0);

    // Hyperbolic & inverse / reciprocal
    assertNear(evalText("sinh(0)", rad), 0.0);
    assertNear(evalText("cosh(0)", rad), 1.0);
    assertNear(evalText("tanh(0)", rad), 0.0);
    assertNear(evalText("sech(0)", rad), 1.0);
    assertNear(evalText("csch(1)", rad), 1.0 / std::sinh(1.0));
    assertNear(evalText("coth(1)", rad), 1.0 / std::tanh(1.0));
    assertNear(evalText("asinh(0)", rad), 0.0);
    assertNear(evalText("acosh(1)", rad), 0.0);
    assertNear(evalText("atanh(0)", rad), 0.0);
    assertNear(evalText("asech(1)", rad), 0.0);
    assertNear(evalText("acsch(1)", rad), std::asinh(1.0));
    assertNear(evalText("acoth(2)", rad), std::atanh(0.5));

    // Logarithms and roots
    assertNear(evalText("log2(8)", rad), 3.0);
    assertNear(evalText("log1p(0)", rad), 0.0);
    assertNear(evalText("cbrt(27)", rad), 3.0);
    assertNear(evalText("cbrt(-8)", rad), -2.0);
    assertNear(evalText("expm1(0)", rad), 0.0);

    // Rounding & sign
    assertNear(evalText("floor(3.7)", rad), 3.0);
    assertNear(evalText("ceil(3.2)", rad), 4.0);
    assertNear(evalText("round(3.5)", rad), 4.0);
    assertNear(evalText("trunc(-3.7)", rad), -3.0);
    assertNear(evalText("sgn(-5)", rad), -1.0);
    assertNear(evalText("sgn(0)", rad), 0.0);
    assertNear(evalText("sgn(5)", rad), 1.0);

    // Special functions
    assertNear(evalText("gamma(5)", rad), 24.0); // (5-1)! = 24
    assertNear(evalText("lgamma(5)", rad), std::log(24.0));
    assertNear(evalText("erf(0)", rad), 0.0);
    assertNear(evalText("erfc(0)", rad), 1.0);
    assertNear(evalText("fact(5)", rad), 120.0);

    // Angle conversions
    assertNear(evalText("deg(pi)", rad), 180.0);
    assertNear(evalText("rad(180)", rad), 3.141592653589793);

    // Constants
    assertNear(evalText("phi", rad), 1.6180339887498948);
    assertNear(evalText("pi", rad), 3.141592653589793);
    assertNear(evalText("e", rad), 2.718281828459045);

    // Modulo operator %
    assertNear(evalText("10%3", rad), 1.0);
    assertNear(evalText("7.5%2", rad), 1.5);
    assertNear(evalText("14%5", rad), 4.0);
}

static void testBigCombinatoricsAndFunctions() {
    EvaluationContext pro;
    pro.bigNumbers = true;

    // Permutation & combination in Pro / Big mode
    Expression pBig;
    insertFromText(pBig, "1000p2");
    assert(evaluateProToString(pBig.root.get(), pro) == "999000");

    Expression cBig;
    insertFromText(cBig, "1000c2");
    assert(evaluateProToString(cBig.root.get(), pro) == "499500");

    // Huge combinatorics that overflow double (2000C1000 is ~ 10^600)
    Expression cHuge;
    insertFromText(cHuge, "2000c1000");
    std::string resCHuge = evaluateProToString(cHuge.root.get(), pro);
    assert(resCHuge.find("10^") != std::string::npos && (resCHuge.find("600") != std::string::npos || resCHuge.find("599") != std::string::npos || resCHuge.find("601") != std::string::npos));

    // Huge scientific functions
    Expression cbrtBig;
    insertFromText(cbrtBig, "cbrt(10^600)");
    assert(evaluateProToString(cbrtBig.root.get(), pro) == "10^200");

    Expression log2Big;
    insertFromText(log2Big, "log2(10^400)");
    std::string resLog2 = evaluateProToString(log2Big.root.get(), pro);
    assert(resLog2.find("1328.") == 0); // 400 * log2(10) ~ 1328.77

    // Modulo in big mode
    Expression modBig;
    insertFromText(modBig, "100%7");
    assert(evaluateProToString(modBig.root.get(), pro) == "2");
}

static void testCombinatoricsNavigationAndEditing() {
    Expression expr;
    insertDigit(expr, '5');
    insertPermutation(expr);
    insertDigit(expr, '3');
    // Root has Permutation. Cursor is in b (subscript) at index 1.
    assert(expr.toPlainString() == "⁵P₃");
    // Move left into subscript
    moveLeft(expr);
    assert(expr.cursor.row == expr.root->items[0]->b.get());
    assert(expr.cursor.index == 0);
    // Move left out of subscript into superscript a
    moveLeft(expr);
    assert(expr.cursor.row == expr.root->items[0]->a.get());
    assert(expr.cursor.index == 1);
    // Move right into subscript
    moveRight(expr);
    assert(expr.cursor.row == expr.root->items[0]->b.get());
    assert(expr.cursor.index == 0);
    // Move down or up
    moveUp(expr);
    assert(expr.cursor.row == expr.root->items[0]->a.get());
    moveDown(expr);
    assert(expr.cursor.row == expr.root->items[0]->b.get());
}

void testCalculusAndMatrices();
void testNaturalCalculusAndSetOperations();

int main() {
    testVariableEvaluation();
    testSingleVariableEquation();
    testSystems();
    testQuadratics();
    testInvalidLinearTerms();
    testAssignmentsPersist();
    testGeneralEquations();
    testProModeTriggerCalculation();
    testProModeBigNumbers();
    testSciFunctions();
    testSciTypingAndText();
    testSciBigMode();
    testFactorials();
    testStandaloneClosingParen();
    testCloseParenExitsNestedStructure();
    testClickPlacesStructuralCursor();
    testPowerEditing();
    testCombinatoricsTypingAndEvaluation();
    testAllScientificFunctions();
    testBigCombinatoricsAndFunctions();
    testCombinatoricsNavigationAndEditing();
    testCalculusAndMatrices();
    testNaturalCalculusAndSetOperations();
    std::cout << "All calculator edge-case tests passed\n";
    return 0;
}

void testCalculusAndMatrices() {
    EvaluationContext ctx;
    ctx.degrees = false; // radians for calculus tests

    // 1. Calculus: Integration
    {
        Expression e1;
        insertFromText(e1, "integrate(x^2, 0, 1)");
        double v1 = evaluate(e1.root.get(), ctx);
        assertNear(v1, 1.0 / 3.0);

        // UTF-8 ∫ symbol with variable
        Expression e1Glyph;
        insertFromText(e1Glyph, "\xE2\x88\xAB(x^2, 0, 1)");
        double v1Glyph = evaluate(e1Glyph.root.get(), ctx);
        assertNear(v1Glyph, 1.0 / 3.0);

        Expression e1Var;
        insertFromText(e1Var, "\xE2\x88\xAB(y^2, y, 0, 2)");
        double v1Var = evaluate(e1Var.root.get(), ctx);
        assertNear(v1Var, 8.0 / 3.0);

        Expression e2;
        insertFromText(e2, "integrate(sin(x), 0, pi)");
        double v2 = evaluate(e2.root.get(), ctx);
        assertNear(v2, 2.0);
    }

    // 2. Calculus: Differentiation
    {
        Expression e1;
        insertFromText(e1, "diff(x^3, 2)");
        double v1 = evaluate(e1.root.get(), ctx);
        assertNear(v1, 12.0);

        // d/dx syntax with variables
        Expression e1Ddx;
        insertFromText(e1Ddx, "d/dx(x^3, 2)");
        double v1Ddx = evaluate(e1Ddx.root.get(), ctx);
        assertNear(v1Ddx, 12.0);

        Expression e1Ddy;
        insertFromText(e1Ddy, "d/dx(y^4, y, 2)");
        double v1Ddy = evaluate(e1Ddy.root.get(), ctx);
        assertNear(v1Ddy, 32.0);

        // d/dx at context variable
        EvaluationContext ctxDiff = ctx;
        ctxDiff.x = 4.0;
        Expression e1Ctx;
        insertFromText(e1Ctx, "d/dx(x^2)");
        double v1Ctx = evaluate(e1Ctx.root.get(), ctxDiff);
        assertNear(v1Ctx, 8.0);

        Expression e2;
        insertFromText(e2, "diff(sin(x), 0)");
        double v2 = evaluate(e2.root.get(), ctx);
        assertNear(v2, 1.0);
    }

    // 3. Calculus: Limits
    {
        Expression e1;
        insertFromText(e1, "limit(sin(x)/x, 0)");
        double v1 = evaluate(e1.root.get(), ctx);
        assertNear(v1, 1.0);
    }

    // 4. Calculus: Summation & Product
    {
        Expression e1;
        insertFromText(e1, "sum(x^2, 1, 10)");
        double v1 = evaluate(e1.root.get(), ctx);
        assertNear(v1, 385.0);

        Expression e2;
        insertFromText(e2, "product(x, 1, 5)");
        double v2 = evaluate(e2.root.get(), ctx);
        assertNear(v2, 120.0);
    }

    // 5. Matrices: Bracket syntax & operations
    {
        Expression eMat;
        insertFromText(eMat, "[[1, 2], [3, 4]]");
        EvalValue val = evaluateValue(eMat.root.get(), ctx);
        assert(val.isMatrix());
        assert(val.mat.rows == 2 && val.mat.cols == 2);
        assert(val.mat.at(0, 0) == 1 && val.mat.at(0, 1) == 2);
        assert(val.mat.at(1, 0) == 3 && val.mat.at(1, 1) == 4);

        // det
        Expression eDet;
        insertFromText(eDet, "det([[1, 2], [3, 4]])");
        assertNear(evaluate(eDet.root.get(), ctx), -2.0);

        // trace
        Expression eTr;
        insertFromText(eTr, "trace([[1, 2], [3, 4]])");
        assertNear(evaluate(eTr.root.get(), ctx), 5.0);

        // transpose
        Expression eTrans;
        insertFromText(eTrans, "transpose([[1, 2], [3, 4]])");
        EvalValue tVal = evaluateValue(eTrans.root.get(), ctx);
        assert(tVal.isMatrix());
        assert(tVal.mat.at(0, 1) == 3 && tVal.mat.at(1, 0) == 2);

        // matrix arithmetic
        Expression eAdd;
        insertFromText(eAdd, "[[1, 2], [3, 4]] + [[5, 6], [7, 8]]");
        EvalValue addVal = evaluateValue(eAdd.root.get(), ctx);
        assert(addVal.isMatrix());
        assert(addVal.mat.at(0, 0) == 6 && addVal.mat.at(1, 1) == 12);

        Expression eMul;
        insertFromText(eMul, "[[1, 2], [3, 4]] * [[2, 0], [1, 2]]");
        EvalValue mulVal = evaluateValue(eMul.root.get(), ctx);
        assert(mulVal.isMatrix());
        assert(mulVal.mat.at(0, 0) == 4 && mulVal.mat.at(0, 1) == 4);
        assert(mulVal.mat.at(1, 0) == 10 && mulVal.mat.at(1, 1) == 8);

        // inverse
        Expression eInv;
        insertFromText(eInv, "inv([[4, 7], [2, 6]])");
        EvalValue invVal = evaluateValue(eInv.root.get(), ctx);
        assert(invVal.isMatrix());
        assertNear(invVal.mat.at(0, 0), 0.6);
        assertNear(invVal.mat.at(0, 1), -0.7);

        // vector dot & cross & norm
        Expression eDot;
        insertFromText(eDot, "dot([1, 2, 3], [4, 5, 6])");
        assertNear(evaluate(eDot.root.get(), ctx), 32.0);

        Expression eCross;
        insertFromText(eCross, "cross([1, 0, 0], [0, 1, 0])");
        EvalValue crossVal = evaluateValue(eCross.root.get(), ctx);
        assert(crossVal.isMatrix());
        assertNear(crossVal.mat.at(0, 0), 0.0);
        assertNear(crossVal.mat.at(0, 1), 0.0);
        assertNear(crossVal.mat.at(0, 2), 1.0);

        Expression eNorm;
        insertFromText(eNorm, "norm([3, 4])");
        assertNear(evaluate(eNorm.root.get(), ctx), 5.0);
    }

    // 6. Statistics
    {
        Expression eMean;
        insertFromText(eMean, "mean(1, 2, 3, 4, 5)");
        assertNear(evaluate(eMean.root.get(), ctx), 3.0);

        Expression eMed;
        insertFromText(eMed, "median(1, 5, 2, 8, 7)");
        assertNear(evaluate(eMed.root.get(), ctx), 5.0);

        Expression eMin;
        insertFromText(eMin, "min(10, 4, 8)");
        assertNear(evaluate(eMin.root.get(), ctx), 4.0);

        Expression eMax;
        insertFromText(eMax, "max(10, 4, 8)");
        assertNear(evaluate(eMax.root.get(), ctx), 10.0);
    }

    // 7. Number Theory & Advanced
    {
        Expression eGcd;
        insertFromText(eGcd, "gcd(12, 18)");
        assertNear(evaluate(eGcd.root.get(), ctx), 6.0);

        Expression eLcm;
        insertFromText(eLcm, "lcm(4, 6)");
        assertNear(evaluate(eLcm.root.get(), ctx), 12.0);

        Expression ePrime;
        insertFromText(ePrime, "isprime(17)");
        assertNear(evaluate(ePrime.root.get(), ctx), 1.0);

        Expression eNotPrime;
        insertFromText(eNotPrime, "isprime(18)");
        assertNear(evaluate(eNotPrime.root.get(), ctx), 0.0);

        Expression eSinc;
        insertFromText(eSinc, "sinc(0)");
        assertNear(evaluate(eSinc.root.get(), ctx), 1.0);

        Expression eJ0;
        insertFromText(eJ0, "besselj0(0)");
        assertNear(evaluate(eJ0.root.get(), ctx), 1.0);

        Expression eZeta;
        insertFromText(eZeta, "zeta(2)");
        assertNear(evaluate(eZeta.root.get(), ctx), 3.141592653589793 * 3.141592653589793 / 6.0);
    }

    // 8. Graph Equation Analysis & Live Evaluation
    {
        // Explicit y = sin(x)
        Expression g1;
        insertFromText(g1, "sin(x)");
        GraphAnalysis a1 = analyzeGraphExpression(g1.root.get(), ctx);
        assert(a1.isValid);
        assert(a1.kind == GraphEquationKind::ExplicitY);
        assertNear(a1.evalExplicit(0.0), 0.0);
        assertNear(a1.evalExplicit(3.141592653589793 / 2.0), 1.0);

        // Explicit y = x^2 - 4
        Expression g2;
        insertFromText(g2, "x^2 - 4");
        GraphAnalysis a2 = analyzeGraphExpression(g2.root.get(), ctx);
        assert(a2.isValid);
        assert(a2.kind == GraphEquationKind::ExplicitY);
        assertNear(a2.evalExplicit(2.0), 0.0);
        assertNear(a2.evalExplicit(0.0), -4.0);

        // Explicit y = x^3 - x
        Expression g3;
        insertFromText(g3, "y = x^3 - x");
        GraphAnalysis a3 = analyzeGraphExpression(g3.root.get(), ctx);
        assert(a3.isValid);
        assert(a3.kind == GraphEquationKind::ExplicitY);
        assertNear(a3.evalExplicit(1.0), 0.0);
        assertNear(a3.evalExplicit(2.0), 6.0);

        // Implicit x^2 + y^2 = 25
        Expression g4;
        insertFromText(g4, "x^2 + y^2 = 25");
        GraphAnalysis a4 = analyzeGraphExpression(g4.root.get(), ctx);
        assert(a4.isValid);
        assert(a4.kind == GraphEquationKind::ImplicitXY);
        assertNear(a4.evalImplicit(3.0, 4.0), 0.0);
        assertNear(a4.evalImplicit(0.0, 5.0), 0.0);

        // Implicit linear equation 3x + y = 2
        Expression gLine;
        insertFromText(gLine, "3x + y = 2");
        GraphAnalysis aLine = analyzeGraphExpression(gLine.root.get(), ctx);
        assert(aLine.isValid);
        assert(aLine.kind == GraphEquationKind::ImplicitXY);
        assertNear(aLine.evalImplicit(0.0, 2.0), 0.0);
        assertNear(aLine.evalImplicit(1.0, -1.0), 0.0);

        // Non-graphable (plain scalar 2 + 2)
        Expression g5;
        insertFromText(g5, "2 + 2");
        GraphAnalysis a5 = analyzeGraphExpression(g5.root.get(), ctx);
        assert(!a5.isValid);
        assert(a5.kind == GraphEquationKind::None);
    }
}

void testNaturalCalculusAndSetOperations() {
    EvaluationContext ctx;

    // 1. Set operations
    {
        // {x} U {y} -> {x, y}
        Expression sUnion;
        insertFromText(sUnion, "{x} U {y}");
        assert(evaluateToString(sUnion.root.get(), ctx) == "{x, y}");

        // typing "union" converts to U
        Expression sUnionWord;
        insertFromText(sUnionWord, "{x} union {y}");
        assert(evaluateToString(sUnionWord.root.get(), ctx) == "{x, y}");

        // Unicode ∪
        Expression sUnionUnicode;
        insertFromText(sUnionUnicode, "{x} \xE2\x88\xAA {y}");
        assert(evaluateToString(sUnionUnicode.root.get(), ctx) == "{x, y}");

        // {x} ∩ {y} -> {}
        Expression sInter;
        insertFromText(sInter, "{x} \xE2\x88\xA9 {y}");
        assert(evaluateToString(sInter.root.get(), ctx) == "{}");

        // typing "inter" converts to ∩
        Expression sInterWord;
        insertFromText(sInterWord, "{x} inter {y}");
        assert(evaluateToString(sInterWord.root.get(), ctx) == "{}");

        // {x} * {y} -> {(x, y)}
        Expression sProd;
        insertFromText(sProd, "{x} * {y}");
        assert(evaluateToString(sProd.root.get(), ctx) == "{(x, y)}");

        // {x} delta {y} -> {x, y}
        Expression sDeltaWord;
        insertFromText(sDeltaWord, "{x} delta {y}");
        assert(evaluateToString(sDeltaWord.root.get(), ctx) == "{x, y}");

        // Unicode Δ
        Expression sDeltaUnicode;
        insertFromText(sDeltaUnicode, "{x} \xCE\x94 {y}");
        assert(evaluateToString(sDeltaUnicode.root.get(), ctx) == "{x, y}");

        // Numeric sets
        Expression sNumU;
        insertFromText(sNumU, "{1, 2} U {2, 3}");
        assert(evaluateToString(sNumU.root.get(), ctx) == "{1, 2, 3}");

        Expression sNumI;
        insertFromText(sNumI, "{1, 2} \xE2\x88\xA9 {2, 3}");
        assert(evaluateToString(sNumI.root.get(), ctx) == "{2}");

        Expression sNumP;
        insertFromText(sNumP, "{1, 2} * {3, 4}");
        assert(evaluateToString(sNumP.root.get(), ctx) == "{(1, 3), (1, 4), (2, 3), (2, 4)}");

        Expression sNumD;
        insertFromText(sNumD, "{1, 2} \xCE\x94 {2, 3}");
        assert(evaluateToString(sNumD.root.get(), ctx) == "{1, 3}");
    }

    // 2. Natural 2D Integral: Indefinite (empty limits) & Definite (with limits)
    {
        // Indefinite: ∫ (3x^2 + 4) dx = x^3 + 4x + C
        Expression indExpr;
        insertIntegral(indExpr);
        // cursor is inside integrand row (a)
        insertFromText(indExpr, "3x^2 + 4");
        std::string indRes = evaluateToString(indExpr.root.get(), ctx);
        assert(indRes.find("+ C") != std::string::npos);
        assert(indRes == "x^3 + 4x + C");

        // Definite: ∫_1^3 (3x^2 + 4) dx = [x^3 + 4x]_1^3 = (27 + 12) - (1 + 4) = 39 - 5 = 34
        Expression defExpr;
        insertIntegral(defExpr);
        insertFromText(defExpr, "3x^2 + 4");
        // Fill lower limit row (b)
        defExpr.cursor.row = defExpr.root->items[0]->b.get();
        defExpr.cursor.index = 0;
        insertDigit(defExpr, '1');
        // Fill upper limit row (c)
        defExpr.cursor.row = defExpr.root->items[0]->c.get();
        defExpr.cursor.index = 0;
        insertDigit(defExpr, '3');
        std::string defRes = evaluateToString(defExpr.root.get(), ctx);
        assertNear(std::stod(defRes), 34.0);

        // Definite trig: ∫_0^π sin(x) dx = [-cos(x)]_0^π = 1 - (-1) = 2
        Expression sinDef;
        insertIntegral(sinDef);
        insertFromText(sinDef, "sin(x)");
        sinDef.cursor.row = sinDef.root->items[0]->b.get();
        sinDef.cursor.index = 0;
        insertDigit(sinDef, '0');
        sinDef.cursor.row = sinDef.root->items[0]->c.get();
        sinDef.cursor.index = 0;
        insertConstant(sinDef, 'p'); // pi
        std::string sinRes = evaluateToString(sinDef.root.get(), ctx);
        assertNear(std::stod(sinRes), 2.0);
    }

    // 3. Natural 2D Derivative: Symbolic & Numerical
    {
        // Symbolic: d/dx (3x^2 + 4) = 6x
        Expression symDiff;
        insertDerivative(symDiff);
        // cursor is in row a (inner expression)
        insertFromText(symDiff, "3x^2 + 4");
        std::string symRes = evaluateToString(symDiff.root.get(), ctx);
        assert(symRes == "6x");

        // Numerical: d/dx (3x^2 + 4) |_{x = 2} = 6(2) = 12
        Expression numDiff;
        insertDerivative(numDiff);
        insertFromText(numDiff, "3x^2 + 4");
        // Fill row b (evaluation point)
        numDiff.cursor.row = numDiff.root->items[0]->b.get();
        numDiff.cursor.index = 0;
        insertDigit(numDiff, '2');
        std::string numRes = evaluateToString(numDiff.root.get(), ctx);
        assert(std::fabs(std::stod(numRes) - 12.0) < 1e-5);
    }

    // 4. Graphing Sinusoidal Verification
    {
        // In Cartesian coordinate graphs, sin(x) must plot sinusoidal wave in radians
        // even if calculator base context has degrees = true
        EvaluationContext degCtx;
        degCtx.degrees = true;
        Expression gSin;
        insertFromText(gSin, "sin(x)");
        GraphAnalysis aSin = analyzeGraphExpression(gSin.root.get(), degCtx);
        assert(aSin.isValid);
        assert(aSin.kind == GraphEquationKind::ExplicitY);
        // Radians check: sin(0) = 0, sin(pi/2) = 1, sin(pi) = 0, sin(3pi/2) = -1, sin(2pi) = 0
        constexpr double pi = 3.14159265358979323846;
        assertNear(aSin.evalExplicit(0.0), 0.0);
        assertNear(aSin.evalExplicit(pi / 2.0), 1.0);
        assertNear(aSin.evalExplicit(pi), 0.0);
        assertNear(aSin.evalExplicit(3.0 * pi / 2.0), -1.0);
        assertNear(aSin.evalExplicit(2.0 * pi), 0.0);
    }
}