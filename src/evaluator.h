// evaluator.h
//
// Evaluates a Row (see expr_tree.h) with full BODMAS/PEMDAS precedence.
// Note that '/' never appears as a flat operator in the tree -- pressing
// '/' always builds a Fraction structure at edit time (see
// insertFraction() in expr_tree.cpp), so a Fraction node IS the division:
// evaluating it is simply eval(numerator) / eval(denominator). This means
// precedence of "division" is automatically correct: it's as tightly
// bound as the atom the user wrapped, exactly like on paper.
//
// Grammar walked over a Row's items:
//   Row    := Term (('+' | '-') Term)*
//   Term   := Factor (ImplicitOrStar Factor)*      // '*' or bare adjacency
//   Factor := '-'? Atom
//   Atom   := Number | Fraction | Paren | Power | Sqrt
//
// Throws std::runtime_error with a short human-readable message on:
//   - division by zero
//   - square root of a negative number
//   - a malformed/incomplete expression (e.g. empty operand)

#pragma once
#include "expr_tree.h"
#include <string>
#include <vector>

struct EvaluationContext {
	double x = 0.0;
	double y = 0.0;
	// Pro Mode: when true, expressions whose result overflows a double (or
	// whose factorial exceeds double range) are evaluated in base-10
	// logarithm space and rendered as "m * 10^e" instead of erroring.
	bool bigNumbers = false;
};

// Evaluate the whole expression. Throws std::runtime_error on error.
double evaluate(const Row* root);
double evaluate(const Row* root, const EvaluationContext& context);

// Convenience: evaluate and format to a display string (trims trailing
// zeros, switches to scientific notation for very large/small magnitudes).
// On error, returns the exception's message prefixed with an error glyph.
std::string evaluateToString(const Row* root);
std::string evaluateToString(const Row* root, const EvaluationContext& context);

// Pro Mode variant of evaluateToString: identical output while the result
// fits a double, but falls back to log10-domain arithmetic whenever normal
// evaluation overflows or a factorial exceeds double range, so inputs like
// 10000000000! or 2^10000000000 produce real "m * 10^e" results.
std::string evaluateProToString(const Row* root, const EvaluationContext& context);

struct QuadraticResult {
	bool valid = true;
	int rootCount = 0;
	double first = 0.0;
	double second = 0.0;
	std::string message;
	std::string firstExact;
	std::string secondExact;
};

QuadraticResult solveQuadratic(double a, double b, double c);

bool solveTwoVariableSystem(const Row* first, const Row* second,
							double& x, double& y, std::string& message);

bool solveSingleVariableEquation(const Row* equation, char& variable,
								 double& value, std::string& message);

bool solveVariableAssignment(const Row* equation, const EvaluationContext& context,
							 char& variable, double& value, std::string& message);

bool isLinearEquation(const Row* equation);

bool solveQuadraticEquation(const Row* equation, QuadraticResult& result,
							std::string& message);

bool solveGeneralEquation(const Row* equation, const EvaluationContext& context,
						  char& variable, std::vector<double>& roots,
						  std::string& message);

bool isProModeTrigger(const Row* expression);
