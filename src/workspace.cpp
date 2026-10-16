// workspace.cpp
#include "workspace.h"
#include "algebra.h"
#include "evaluator.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

bool containsImaginaryUnit(const Row* row) {
    if (!row) return false;
    for (const auto& item : row->items) {
        if (item->type == ItemType::Name && item->nameText == "i") return true;
        if (item->type == ItemType::Variable && item->variableName == 'i') return true;
        if (containsImaginaryUnit(item->a.get()) || containsImaginaryUnit(item->b.get()) ||
            containsImaginaryUnit(item->c.get()) || containsImaginaryUnit(item->d.get()))
            return true;
    }
    return false;
}

int countEquals(const Row* row) {
    int count = 0;
    for (const auto& item : row->items) count += item->type == ItemType::Equals ? 1 : 0;
    return count;
}

size_t equalsIndex(const Row* row) {
    for (size_t i = 0; i < row->items.size(); ++i)
        if (row->items[i]->type == ItemType::Equals) return i;
    return row->items.size();
}

// Solver output: no "-0", and round-off below 1e-9 of the scale is zero.
double clean(double value, double scale = 1.0) {
    return std::fabs(value) < 1e-9 * std::max(1.0, scale) ? 0.0 : value;
}

std::string valueText(char variable, double value) {
    char text[96];
    std::snprintf(text, sizeof(text), "%c = %.10g", variable, clean(value));
    return text;
}

std::string quadraticText(const QuadraticResult& quadratic, char variable) {
    if (quadratic.rootCount == 0) return "\xE2\x88\x85";
    char first[64], second[64];
    std::snprintf(first, sizeof(first), "%.10g", clean(quadratic.first));
    std::snprintf(second, sizeof(second), "%.10g", clean(quadratic.second));
    std::string v(1, variable);
    if (quadratic.rootCount == 1) {
        return quadratic.firstExact.empty() ? v + " = " + first
                                            : v + " = " + quadratic.firstExact + " = " + first;
    }
    if (quadratic.firstExact.empty()) return v + "1 = " + first + ", " + v + "2 = " + second;
    return v + "1 = " + quadratic.firstExact + " = " + first + ", " +
           v + "2 = " + quadratic.secondExact + " = " + second;
}

enum class Assignment { None, Value, Error };

// "x = <expression without unknowns>" declares x. "y = 2x + 1" with x
// undeclared is a relation between the variables, and "x = x + 1" is an
// equation to solve, so neither of those assigns anything.
Assignment classifyAssignment(const Row* equation, const EvaluationContext& context,
                              char& variable, double& value, std::string& message) {
    if (countEquals(equation) != 1) return Assignment::None;
    size_t eq = equalsIndex(equation);
    size_t n = equation->items.size();
    if (eq == 0 || eq + 1 >= n) return Assignment::None;
    auto loneVariable = [&](size_t index) {
        const Item* it = equation->items[index].get();
        return it->type == ItemType::Variable && (it->variableName == 'x' || it->variableName == 'y');
    };
    bool onLeft = eq == 1 && loneVariable(0);
    bool onRight = n - eq - 1 == 1 && loneVariable(eq + 1);
    if (!onLeft && !onRight) return Assignment::None;

    char target = onLeft ? equation->items[0]->variableName : equation->items[eq + 1]->variableName;
    size_t begin = onLeft ? eq + 1 : 0;
    size_t end = onLeft ? n : eq;
    FreeVariables uses = collectFreeVariables(equation, begin, end);
    if ((target == 'x' && uses.x) || (target == 'y' && uses.y)) return Assignment::None;
    if (unknownVariables(equation, context, begin, end).any()) return Assignment::None;

    variable = target;
    return evaluateRange(equation, begin, end, context, value, message) ? Assignment::Value
                                                                       : Assignment::Error;
}

// One unknown left: exact quadratic roots when possible, then the linear
// solver, then the numeric root finder.
std::string solveSingleUnknown(const Row* equation, char variable, const EvaluationContext& context) {
    std::string message;
    QuadraticResult quadratic;
    // The polynomial solver reads x, so solve for y under that name.
    std::unique_ptr<Row> asX = variable == 'y' ? renameVariable(equation, 'y', 'x') : nullptr;
    if (solveQuadraticEquation(asX ? asX.get() : equation, quadratic, message))
        return quadraticText(quadratic, variable);

    char solved = 0;
    double value = 0.0;
    if (solveSingleVariableEquation(equation, solved, value, message))
        return valueText(solved, value);

    std::vector<double> roots;
    if (!solveGeneralEquation(equation, context, solved, roots, message)) return message;
    if (!message.empty()) return message;
    if (roots.size() == 1) return valueText(solved, roots[0]);
    std::string result;
    for (size_t i = 0; i < roots.size(); ++i) {
        char text[96];
        std::snprintf(text, sizeof(text), "%c%zu = %.10g", solved, i + 1, clean(roots[i]));
        if (i) result += ", ";
        result += text;
    }
    return result;
}

// No unknowns at all ("2 + 2 = 4", or "2x = 10" after declaring x = 5).
std::string statementText(const Row* equation, const EvaluationContext& context, bool& isError) {
    size_t eq = equalsIndex(equation);
    double left = 0.0, right = 0.0;
    std::string error;
    if (!evaluateRange(equation, 0, eq, context, left, error) ||
        !evaluateRange(equation, eq + 1, equation->items.size(), context, right, error)) {
        isError = true;
        return error;
    }
    double scale = std::max({ 1.0, std::fabs(left), std::fabs(right) });
    return std::fabs(left - right) <= 1e-9 * scale ? "True" : "False";
}

} // namespace

bool Workspace::commitCurrent(const EvaluationContext& context) {
    if (rowIsEmpty(current_->root.get())) return false;
    solvedValues_ = context;
    hasSolvedValues_ = true;

    auto entry = std::make_unique<HistoryEntry>();
    entry->expr = std::move(current_);
    const Row* root = entry->expr->root.get();

    if (isProModeTrigger(root)) {
        double log10Value = (std::lgamma(2000.0) + std::log(1999.0)) / std::log(10.0);
        double exponent = std::floor(log10Value);
        double mantissa = std::pow(10.0, log10Value - exponent);
        char expanded[96];
        std::snprintf(expanded, sizeof(expanded), "1999 * 1999! = %.10g * 10^%.0f (approx.)",
                      mantissa, exponent);
        entry->result = expanded;
        entry->isError = false;
    } else if (hasEquals(root)) {
        solveEquation(*entry, context);
    } else {
        evaluateExpression(*entry, context);
    }

    history_.push_back(std::move(entry));
    solveSystemWithPrevious();
    current_ = std::make_unique<Expression>();
    recallIndex_ = history_.size();
    return true;
}

void Workspace::evaluateExpression(HistoryEntry& entry, const EvaluationContext& context) {
    const Row* root = entry.expr->root.get();
    bool containsI = containsImaginaryUnit(root);
    entry.isError = false;

    FreeVariables unknown = unknownVariables(root, context);
    if (unknown.any()) {
        // Undeclared x/y are variables, never 0: answer with the simplified
        // expression rather than a value or roots ("x^2 = 0" solves).
        std::unique_ptr<Row> expression = substituteDeclared(root, context);
        std::string text;
        if (!simplifyExpression(expression.get(), context, text))
            text = rowRangeToPlainString(expression.get(), 0, (int)expression->items.size());
        entry.result = text;
        return;
    }

    try {
        if (containsI) {
            entry.result = evaluateComplexToString(root, context);
        } else if (context.bigNumbers) {
            // Pro Mode: normal formatting while the result fits a
            // double, log10-domain "m * 10^e" beyond that.
            entry.result = evaluateProToString(root, context);
            entry.isError = entry.result == "Error";
        } else {
            // Errors come back as text; the flag lets the display style them.
            entry.result = evaluateToString(root, context, entry.isError);
        }
    } catch (const std::exception& e) {
        entry.result = e.what();
        entry.isError = true;
    }
}

void Workspace::solveEquation(HistoryEntry& entry, const EvaluationContext& context) {
    const Row* root = entry.expr->root.get();
    entry.isError = false;
    std::string message;

    if (countEquals(root) > 1) {
        entry.result = "Use one '=' per equation";
        return;
    }
    size_t eq = equalsIndex(root);
    if (eq == 0 || eq + 1 >= root->items.size()) {
        entry.result = "Incomplete equation";
        return;
    }

    if (containsImaginaryUnit(root)) {
        double x = 0.0, y = 0.0;
        if (solveComplexEquation(root, x, y, message)) {
            char result[128];
            std::snprintf(result, sizeof(result), "x = %.10g, y = %.10g (Complex)", x, y);
            entry.result = result;
        } else {
            entry.result = message;
        }
        return;
    }

    char variable = 0;
    double value = 0.0;
    switch (classifyAssignment(root, context, variable, value, message)) {
        case Assignment::Value:
            entry.result = valueText(variable, value);
            if (variable == 'x') {
                solvedValues_.x = value;
                solvedValues_.xDefined = true;
            } else {
                solvedValues_.y = value;
                solvedValues_.yDefined = true;
            }
            return;
        case Assignment::Error:
            entry.result = message;
            entry.isError = true;
            return;
        case Assignment::None:
            break;
    }

    FreeVariables unknown = unknownVariables(root, context);
    std::unique_ptr<Row> equation = substituteDeclared(root, context);
    if (unknown.x && unknown.y) {
        if (!solveForOneVariable(equation.get(), context, entry.result))
            entry.result = rowRangeToPlainString(equation.get(), 0, (int)equation->items.size());
    } else if (unknown.any()) {
        entry.result = solveSingleUnknown(equation.get(), unknown.x ? 'x' : 'y', context);
    } else {
        entry.result = statementText(equation.get(), context, entry.isError);
    }
}

// Two consecutive equations in the undeclared x and y form a system.
void Workspace::solveSystemWithPrevious() {
    if (history_.size() < 2) return;
    HistoryEntry& previous = *history_[history_.size() - 2];
    HistoryEntry& latest = *history_.back();
    const Row* prev = previous.expr->root.get();
    const Row* current = latest.expr->root.get();
    if (!hasEquals(prev) || !hasEquals(current)) return;
    if (containsImaginaryUnit(prev) || containsImaginaryUnit(current)) return;
    if (solvedValues_.xDefined || solvedValues_.yDefined) return;

    char variable = 0;
    double value = 0.0;
    std::string message;
    if (classifyAssignment(prev, solvedValues_, variable, value, message) != Assignment::None ||
        classifyAssignment(current, solvedValues_, variable, value, message) != Assignment::None)
        return;
    FreeVariables first = collectFreeVariables(prev);
    FreeVariables second = collectFreeVariables(current);
    if (!(first.x || second.x) || !(first.y || second.y)) return;

    double x = 0.0, y = 0.0;
    if (solveTwoVariableSystem(prev, current, x, y, message)) {
        double scale = std::max(std::fabs(x), std::fabs(y));
        char result[128];
        std::snprintf(result, sizeof(result), "x = %.10g, y = %.10g", clean(x, scale), clean(y, scale));
        previous.result = "Solved as part of the system";
        previous.isError = false;
        latest.result = result;
        latest.isError = false;
    } else if (isLinearEquation(prev) && isLinearEquation(current)) {
        // Parallel or identical lines; a failed nonlinear pairing keeps the
        // latest equation's own answer instead.
        latest.result = message;
        latest.isError = true;
    }
}

bool Workspace::recallPrevious() {
    if (history_.empty()) return false;
    if (recallIndex_ > history_.size()) recallIndex_ = history_.size();
    if (recallIndex_ == 0) return false;
    --recallIndex_;
    current_ = cloneExpression(*history_[recallIndex_]->expr);
    return true;
}
