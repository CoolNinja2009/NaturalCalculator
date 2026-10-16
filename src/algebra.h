// algebra.h
//
// Free-variable algebra for the Enter/solve flow. x and y are unknowns
// unless the user declared them with an assignment ("x = 5"); this module
// keeps undeclared ones symbolic instead of letting them evaluate as 0.
//
// It finds which variables occur free in a row (an integral, sum, product
// or derivative binds its own variable), substitutes declared values,
// collects like terms ("2x + x" -> "3x"), and isolates y (or x) in a single
// equation with both unknowns, so "2x + y = 9" answers "y = 9 - 2x".

#pragma once
#include <memory>
#include <string>
#include "expr_tree.h"
#include "evaluator.h"

struct FreeVariables {
    bool x = false;
    bool y = false;
    bool any() const { return x || y; }
};

// Variables occurring free in items [begin, end) of `row`.
FreeVariables collectFreeVariables(const Row* row, size_t begin = 0, size_t end = (size_t)-1);

// Free variables that `context` has not declared.
FreeVariables unknownVariables(const Row* row, const EvaluationContext& context,
                               size_t begin = 0, size_t end = (size_t)-1);

// Deep copy of `row` with every free occurrence of a declared x/y replaced
// by its value, so syntactic solvers see only the real unknowns.
std::unique_ptr<Row> substituteDeclared(const Row* row, const EvaluationContext& context);

// Deep copy of `row` with variable `from` renamed to `to`.
std::unique_ptr<Row> renameVariable(const Row* row, char from, char to);

// Evaluate items [begin, end) of `row`. On failure `error` holds the reason.
bool evaluateRange(const Row* row, size_t begin, size_t end, const EvaluationContext& context,
                   double& value, std::string& error);

// Canonical text of an expression whose unknowns stay symbolic, e.g.
// "2(x + 3) + x" -> "3x + 6". Returns false for unsupported constructs.
bool simplifyExpression(const Row* row, const EvaluationContext& context, std::string& out);

// One equation whose unknowns are both x and y: "y = ..." (or "x = ...")
// when one of them can be isolated, otherwise the simplified relation.
bool solveForOneVariable(const Row* equation, const EvaluationContext& context, std::string& out);
