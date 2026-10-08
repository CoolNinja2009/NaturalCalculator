#include "evaluator.h"
#include "expr_tree.h"
#include "layout.h"
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
    std::cout << "All calculator edge-case tests passed\n";
    return 0;
}