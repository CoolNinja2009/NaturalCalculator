// workspace.cpp
#include "workspace.h"
#include "evaluator.h"
#include <cmath>
#include <cstdio>

bool Workspace::commitCurrent(const EvaluationContext& context) {
    if (rowIsEmpty(current_->root.get())) return false;
    solvedValues_ = context;
    hasSolvedValues_ = true;

    auto entry = std::make_unique<HistoryEntry>();
    entry->expr = std::move(current_);

    if (isProModeTrigger(entry->expr->root.get())) {
        double log10Value = (std::lgamma(2000.0) + std::log(1999.0)) / std::log(10.0);
        double exponent = std::floor(log10Value);
        double mantissa = std::pow(10.0, log10Value - exponent);
        char expanded[96];
        std::snprintf(expanded, sizeof(expanded), "1999 * 1999! = %.10g * 10^%.0f (approx.)",
                      mantissa, exponent);
        entry->result = expanded;
        entry->isError = false;
    } else {
    bool hasEquation = hasEquals(entry->expr->root.get());
    QuadraticResult quadratic;
    char variable = 0;
    double value = 0.0;
    std::string message;
    bool isQuadratic = solveQuadraticEquation(entry->expr->root.get(), quadratic, message);
    if (hasEquation || isQuadratic) {
        if (isQuadratic) {
            if (quadratic.rootCount == 0) entry->result = "\xE2\x88\x85";
            else if (quadratic.rootCount == 1) {
                char valueText[64];
                std::snprintf(valueText, sizeof(valueText), "%.10g", quadratic.first);
                entry->result = quadratic.firstExact.empty() ?
                    "x = " + std::string(valueText) :
                    "x = " + quadratic.firstExact + " = " + valueText;
            } else {
                char firstText[64], secondText[64];
                std::snprintf(firstText, sizeof(firstText), "%.10g", quadratic.first);
                std::snprintf(secondText, sizeof(secondText), "%.10g", quadratic.second);
                entry->result = quadratic.firstExact.empty() ?
                    "x1 = " + std::string(firstText) + ", x2 = " + secondText :
                    "x1 = " + quadratic.firstExact + " = " + firstText +
                    ", x2 = " + quadratic.secondExact + " = " + secondText;
            }
            entry->isError = false;
        } else if (solveVariableAssignment(entry->expr->root.get(), context, variable, value, message)) {
            char result[96];
            std::snprintf(result, sizeof(result), "%c = %.10g", variable, value);
            entry->result = result;
            entry->isError = false;
            if (variable == 'x') solvedValues_.x = value;
            else solvedValues_.y = value;
        } else if (solveSingleVariableEquation(entry->expr->root.get(), variable, value, message)) {
            char result[96];
            std::snprintf(result, sizeof(result), "%c = %.10g (the other variable is free)", variable, value);
            entry->result = result;
            entry->isError = false;
            if (variable == 'x') solvedValues_.x = value;
            else solvedValues_.y = value;
        } else {
            std::vector<double> roots;
            if (solveGeneralEquation(entry->expr->root.get(), context, variable, roots, message)) {
                if (!message.empty()) entry->result = message;
                else if (roots.size() == 1) {
                    char result[96];
                    std::snprintf(result, sizeof(result), "%c = %.10g", variable, roots[0]);
                    entry->result = result;
                    if (variable == 'x') solvedValues_.x = roots[0];
                    else solvedValues_.y = roots[0];
                } else {
                    entry->result.clear();
                    for (size_t i = 0; i < roots.size(); ++i) {
                        char result[96];
                        std::snprintf(result, sizeof(result), "%c%zu = %.10g",
                                      variable, i + 1, roots[i]);
                        if (i) entry->result += ", ";
                        entry->result += result;
                    }
                }
                entry->isError = false;
            } else {
                entry->result = message;
                entry->isError = false;
            }
        }
    } else {
        try {
            if (context.bigNumbers) {
                // Pro Mode: normal formatting while the result fits a
                // double, log10-domain "m * 10^e" beyond that.
                entry->result = evaluateProToString(entry->expr->root.get(), context);
            } else {
                double v = evaluate(entry->expr->root.get(), context);
                entry->result = evaluateToString(entry->expr->root.get(), context);
                (void)v;
            }
            entry->isError = false;
        } catch (const std::exception& e) {
            entry->result = e.what();
            entry->isError = true;
        }
    }
    }

    history_.push_back(std::move(entry));
    if (history_.size() >= 2) {
        const Row* prev = history_[history_.size() - 2]->expr->root.get();
        const Row* current = history_.back()->expr->root.get();
        char assignedVariable = 0;
        double assignedValue = 0.0;
        std::string assignmentMessage;
        bool previousIsAssignment =
            solveVariableAssignment(prev, solvedValues_, assignedVariable, assignedValue, assignmentMessage);
        bool currentIsAssignment =
            solveVariableAssignment(current, solvedValues_, assignedVariable, assignedValue, assignmentMessage);
        auto collectVariablesInRow = [](const Row* row, bool& hasX, bool& hasY, auto&& self) -> void {
            if (!row) return;
            for (const auto& item : row->items) {
                if (item->type == ItemType::Variable) {
                    hasX = hasX || item->variableName == 'x';
                    hasY = hasY || item->variableName == 'y';
                }
                self(item->a.get(), hasX, hasY, self);
                self(item->b.get(), hasX, hasY, self);
            }
        };
        bool hasX = false, hasY = false;
        collectVariablesInRow(prev, hasX, hasY, collectVariablesInRow);
        collectVariablesInRow(current, hasX, hasY, collectVariablesInRow);
        if (hasEquals(prev) && hasEquals(current) &&
            !previousIsAssignment && !currentIsAssignment && hasX && hasY) {
            double x = 0.0, y = 0.0;
            std::string message;
            if (solveTwoVariableSystem(prev, current, x, y, message)) {
                char result[128];
                std::snprintf(result, sizeof(result), "x = %.10g, y = %.10g", x, y);
                history_[history_.size() - 2]->result = "Solved as part of the system";
                history_[history_.size() - 2]->isError = false;
                history_.back()->result = result;
                history_.back()->isError = false;
                solvedValues_ = { x, y };
                hasSolvedValues_ = true;
            } else {
                history_.back()->result = message;
                history_.back()->isError = true;
            }
        }
    }
    current_ = std::make_unique<Expression>();
    recallIndex_ = history_.size();
    return true;
}

bool Workspace::recallPrevious() {
    if (history_.empty()) return false;
    if (recallIndex_ > history_.size()) recallIndex_ = history_.size();
    if (recallIndex_ == 0) return false;
    --recallIndex_;
    current_ = cloneExpression(*history_[recallIndex_]->expr);
    return true;
}
