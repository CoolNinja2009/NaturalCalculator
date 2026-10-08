// symbolic.h
//
// Symbolic & numerical calculus engine for NaturalCalculator.
// Supports symbolic indefinite integration (with "+ C"), symbolic differentiation,
// definite integration over limits [a, b], and numerical differentiation.

#pragma once

#include <string>
#include <memory>
#include "expr_tree.h"
#include "evaluator.h"

// Symbolically integrates a Row with respect to variable `var` (default 'x').
// Appends " + C" for indefinite integrals.
std::string symbolicIntegrate(const Row* row, char var = 'x', bool appendConstant = true);

// Symbolically differentiates a Row with respect to variable `var` (default 'x').
std::string symbolicDifferentiate(const Row* row, char var = 'x');

// Evaluates definite integral of a Row from a to b with respect to variable `var`.
double evalDefiniteIntegral(const Row* row, double a, double b, char var, const EvaluationContext& context);

// Evaluates derivative of a Row at point x0 with respect to variable `var`.
double evalDerivativeAtPoint(const Row* row, double x0, char var, const EvaluationContext& context);
