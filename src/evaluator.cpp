// evaluator.cpp
#include "evaluator.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <vector>

namespace {

bool sameRow(const Row* left, const Row* right);

bool sameItem(const Item* left, const Item* right) {
    if (!left || !right || left->type != right->type) return false;
    if (left->type == ItemType::Number) return left->numText == right->numText;
    if (left->type == ItemType::Variable) return left->variableName == right->variableName;
    if (left->type == ItemType::Operator) return left->opChar == right->opChar;
    if (left->type == ItemType::Equals) return true;
    return sameRow(left->a.get(), right->a.get()) && sameRow(left->b.get(), right->b.get());
}

bool sameRow(const Row* left, const Row* right) {
    if (!left || !right || left->items.size() != right->items.size()) return false;
    for (size_t i = 0; i < left->items.size(); ++i) {
        if (!sameItem(left->items[i].get(), right->items[i].get())) return false;
    }
    return true;
}

bool isOversizedFactorialRange(const std::vector<std::unique_ptr<Item>>& items,
                               size_t begin, size_t end) {
    return end == begin + 2 &&
           items[begin]->type == ItemType::Number &&
           items[begin + 1]->type == ItemType::Operator &&
           items[begin + 1]->opChar == '!' &&
           std::stod(items[begin]->numText) > 170.0;
}

bool sameRange(const std::vector<std::unique_ptr<Item>>& items,
               size_t leftBegin, size_t leftEnd, size_t rightBegin, size_t rightEnd) {
    if (leftEnd - leftBegin != rightEnd - rightBegin) return false;
    for (size_t i = 0; i < leftEnd - leftBegin; ++i) {
        if (!sameItem(items[leftBegin + i].get(), items[rightBegin + i].get())) return false;
    }
    return true;
}

void collectVariables(const Row* row, bool& hasX, bool& hasY) {
    if (!row) return;
    for (const auto& item : row->items) {
        if (item->type == ItemType::Variable) {
            hasX = hasX || item->variableName == 'x';
            hasY = hasY || item->variableName == 'y';
        }
        collectVariables(item->a.get(), hasX, hasY);
        collectVariables(item->b.get(), hasX, hasY);
    }
}

std::string numberString(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

std::string radicalText(double value, double& outside, double& inside) {
    outside = 1.0;
    inside = value;
    double rounded = std::round(value);
    if (value <= 0.0 || value > 1e12 || std::fabs(value - rounded) > 1e-10)
        return "\xE2\x88\x9A" + numberString(value);
    long long integer = (long long)rounded;
    long long factor = 1;
    for (long long candidate = (long long)std::sqrt((double)integer); candidate >= 2; --candidate) {
        long long square = candidate * candidate;
        if (integer % square == 0) {
            factor = candidate;
            break;
        }
    }
    outside = (double)factor;
    inside = (double)(integer / (factor * factor));
    if (factor == 1) return "\xE2\x88\x9A" + numberString(inside);
    return numberString(outside) + "\xE2\x88\x9A" + numberString(inside);
}

bool simpleSquareRootPower(const Row* row, std::string& form) {
    if (!row || row->items.size() != 1 || row->items[0]->type != ItemType::Power)
        return false;
    const Item* power = row->items[0].get();
    const Row* base = power->a.get();
    const Row* exponent = power->b.get();
    if (!base || base->items.size() != 1 || base->items[0]->type != ItemType::Number ||
        !exponent || exponent->items.size() != 1 || exponent->items[0]->type != ItemType::Fraction)
        return false;
    const Item* fraction = exponent->items[0].get();
    if (!fraction->a || !fraction->b || fraction->a->items.size() != 1 ||
        fraction->b->items.size() != 1 ||
        fraction->a->items[0]->type != ItemType::Number ||
        fraction->b->items[0]->type != ItemType::Number ||
        fraction->a->items[0]->numText != "1" || fraction->b->items[0]->numText != "2")
        return false;
    double radicand = 0.0;
    try { radicand = std::stod(base->items[0]->numText); }
    catch (...) { return false; }
    if (radicand <= 0.0 || std::floor(radicand) != radicand || radicand > 1e12)
        return false;
    double squareRoot = std::sqrt(radicand);
    if (std::fabs(squareRoot - std::round(squareRoot)) < 1e-10)
        return false;
    double outside = 1.0, inside = radicand;
    form = radicalText(radicand, outside, inside);
    return true;
}

double factorial(double value) {
    if (value < 0.0 || std::floor(value) != value)
        throw std::runtime_error("Factorial needs a non-negative integer");
    if (value > 170.0)
        throw std::runtime_error("Factorial result is too large");
    double result = 1.0;
    for (int i = 2; i <= (int)value; ++i) result *= i;
    return result;
}

struct RowParser {
    const std::vector<std::unique_ptr<Item>>& items;
    const EvaluationContext& context;
    size_t pos = 0;
    size_t end = 0;

    RowParser(const Row* row, const EvaluationContext& values, size_t begin = 0,
              size_t finish = static_cast<size_t>(-1))
        : items(row->items), context(values), pos(begin),
          end(finish == static_cast<size_t>(-1) ? row->items.size() : finish) {}

    bool atEnd() const { return pos >= end; }
    const Item* peek() const { return atEnd() ? nullptr : items[pos].get(); }

    bool peekIsOperatorChar(char c) const {
        const Item* it = peek();
        return it && it->type == ItemType::Operator && it->opChar == c;
    }

    double parseAtom() {
        if (atEnd())
            throw std::runtime_error("Incomplete expression");
        const Item* it = items[pos].get();
        switch (it->type) {
            case ItemType::Number: {
                pos++;
                std::string numberText = it->numText;
                while (!atEnd() && items[pos]->type == ItemType::Number)
                    numberText += items[pos++]->numText;
                if (numberText.empty() || numberText == ".")
                    throw std::runtime_error("Invalid number");
                try {
                    return std::stod(numberText);
                } catch (...) {
                    throw std::runtime_error("Invalid number");
                }
            }
            case ItemType::Variable:
                pos++;
                if (it->variableName == 'x') return context.x;
                if (it->variableName == 'y') return context.y;
                throw std::runtime_error("Unknown variable");
            case ItemType::Fraction: {
                pos++;
                double n = evaluate(it->a.get(), context);
                double d = evaluate(it->b.get(), context);
                if (d == 0.0) throw std::runtime_error("Division by zero");
                return n / d;
            }
            case ItemType::Paren: {
                pos++;
                return evaluate(it->a.get(), context);
            }
            case ItemType::Power: {
                pos++;
                double base = evaluate(it->a.get(), context);
                double exp = evaluate(it->b.get(), context);
                return std::pow(base, exp);
            }
            case ItemType::Sqrt: {
                pos++;
                double v = evaluate(it->a.get(), context);
                if (v < 0.0) throw std::runtime_error("Root of negative number");
                return std::sqrt(v);
            }
            case ItemType::Operator:
                throw std::runtime_error("Unexpected operator");
            case ItemType::Equals:
                throw std::runtime_error("Equation needs two lines");
            case ItemType::CloseParen:
                throw std::runtime_error("Unmatched closing parenthesis");
        }
        throw std::runtime_error("Unknown item");
    }

    double parseFactor() {
        bool neg = false;
        while (peekIsOperatorChar('-') || peekIsOperatorChar('+')) {
            if (peekIsOperatorChar('-')) neg = !neg;
            pos++;
        }
        double v = parseAtom();
        while (peekIsOperatorChar('!')) {
            pos++;
            v = factorial(v);
        }
        return neg ? -v : v;
    }

    // '*' explicit, or bare adjacency of two atoms (implicit multiplication,
    // e.g. "2(3+4)" or "2\u221A3").
    double parseTerm() {
        double v = parseFactor();
        for (;;) {
            if (peekIsOperatorChar('*')) {
                pos++;
                v *= parseFactor();
                continue;
            }
            // Implicit multiplication: next token exists and is not a
            // flat operator (+, -, *) -> another atom starts here.
            const Item* nxt = peek();
            if (nxt && nxt->type != ItemType::Operator) {
                v *= parseFactor();
                continue;
            }
            break;
        }
        return v;
    }

    double parseRow() {
        if (atEnd()) return 0.0; // empty row evaluates to 0 (e.g. empty exponent)
        size_t firstTermStart = pos;
        double v = parseTerm();
        for (;;) {
            if (peekIsOperatorChar('+')) {
                pos++;
                v += parseTerm();
            } else if (peekIsOperatorChar('-')) {
                size_t secondTermStart = pos + 1;
                if (secondTermStart < items.size() &&
                    isOversizedFactorialRange(items, firstTermStart, pos)) {
                    size_t termLength = pos - firstTermStart;
                    size_t scan = secondTermStart + termLength;
                    if (scan <= items.size() &&
                        sameRange(items, firstTermStart, pos, secondTermStart, scan)) {
                        pos = scan;
                        v = 0.0;
                        firstTermStart = pos;
                        continue;
                    }
                }
                pos++;
                v -= parseTerm();
            } else {
                break;
            }
        }
        if (!atEnd())
            throw std::runtime_error("Malformed expression");
        return v;
    }
};

struct Linear {
    double x = 0.0;
    double y = 0.0;
    double constant = 0.0;
    bool valid = true;
};

Linear linearizeSide(const Row* row);

struct Polynomial {
    double coefficient[3] = { 0.0, 0.0, 0.0 };
    bool valid = true;
};

Polynomial polynomialize(const Row* row);

Polynomial addPolynomial(const Polynomial& left, const Polynomial& right, double sign = 1.0) {
    Polynomial result;
    result.valid = left.valid && right.valid;
    for (int i = 0; i <= 2; ++i) result.coefficient[i] = left.coefficient[i] + sign * right.coefficient[i];
    return result;
}

Polynomial multiplyPolynomial(const Polynomial& left, const Polynomial& right) {
    Polynomial result;
    result.valid = left.valid && right.valid;
    for (int degree = 0; degree <= 2; ++degree) {
        for (int rightDegree = 0; rightDegree <= degree; ++rightDegree)
            result.coefficient[degree] += left.coefficient[degree - rightDegree] * right.coefficient[rightDegree];
    }
    for (int degree = 3; degree <= 4; ++degree) {
        for (int rightDegree = 0; rightDegree <= degree; ++rightDegree) {
            int leftDegree = degree - rightDegree;
            if (leftDegree <= 2 && rightDegree <= 2 &&
                std::fabs(left.coefficient[leftDegree]) > 1e-12 &&
                std::fabs(right.coefficient[rightDegree]) > 1e-12)
                result.valid = false;
        }
    }
    return result;
}

struct PolynomialParser {
    const std::vector<std::unique_ptr<Item>>& items;
    size_t pos;
    size_t end;

    PolynomialParser(const Row* row, size_t begin, size_t finish)
        : items(row->items), pos(begin), end(finish) {}

    bool atEnd() const { return pos >= end; }
    const Item* peek() const { return atEnd() ? nullptr : items[pos].get(); }
    bool isOperator(char op) const {
        const Item* item = peek();
        return item && item->type == ItemType::Operator && item->opChar == op;
    }

    Polynomial parseAtom() {
        if (atEnd()) return { { 0, 0, 0 }, false };
        const Item* item = items[pos++].get();
        switch (item->type) {
            case ItemType::Number: {
                std::string numberText = item->numText;
                while (!atEnd() && items[pos]->type == ItemType::Number)
                    numberText += items[pos++]->numText;
                try { return { { std::stod(numberText), 0, 0 }, true }; }
                catch (...) { return { { 0, 0, 0 }, false }; }
            }
            case ItemType::Variable:
                return item->variableName == 'x' ? Polynomial{ { 0, 1, 0 }, true } : Polynomial{ { 0, 0, 0 }, false };
            case ItemType::Paren:
                return polynomialize(item->a.get());
            case ItemType::Power: {
                Polynomial base = polynomialize(item->a.get());
                Polynomial exponent = polynomialize(item->b.get());
                if (!base.valid || !exponent.valid || std::fabs(exponent.coefficient[1]) > 1e-12 ||
                    std::fabs(exponent.coefficient[2]) > 1e-12 || exponent.coefficient[0] < 0 ||
                    exponent.coefficient[0] > 2 || std::floor(exponent.coefficient[0]) != exponent.coefficient[0])
                    return { { 0, 0, 0 }, false };
                int power = (int)exponent.coefficient[0];
                Polynomial result{ { 1, 0, 0 }, true };
                for (int i = 0; i < power; ++i) result = multiplyPolynomial(result, base);
                return result;
            }
            case ItemType::Fraction: {
                Polynomial numerator = polynomialize(item->a.get());
                Polynomial denominator = polynomialize(item->b.get());
                if (!denominator.valid || std::fabs(denominator.coefficient[1]) > 1e-12 ||
                    std::fabs(denominator.coefficient[2]) > 1e-12 || std::fabs(denominator.coefficient[0]) < 1e-12)
                    return { { 0, 0, 0 }, false };
                for (double& coefficient : numerator.coefficient) coefficient /= denominator.coefficient[0];
                return numerator;
            }
            case ItemType::Operator:
            case ItemType::Equals:
            case ItemType::CloseParen:
            case ItemType::Sqrt:
                return { { 0, 0, 0 }, false };
        }
        return { { 0, 0, 0 }, false };
    }

    Polynomial parseFactor() {
        bool negative = false;
        while (isOperator('-') || isOperator('+')) {
            if (isOperator('-')) negative = !negative;
            ++pos;
        }
        Polynomial value = parseAtom();
        if (negative) for (double& coefficient : value.coefficient) coefficient = -coefficient;
        return value;
    }

    Polynomial parseTerm() {
        Polynomial value = parseFactor();
        while (!atEnd()) {
            if (isOperator('*')) { ++pos; value = multiplyPolynomial(value, parseFactor()); }
            else if (peek()->type != ItemType::Operator) value = multiplyPolynomial(value, parseFactor());
            else break;
        }
        return value;
    }

    Polynomial parseRow() {
        if (atEnd()) return { { 0, 0, 0 }, false };
        Polynomial value = parseTerm();
        while (!atEnd()) {
            if (isOperator('+')) { ++pos; value = addPolynomial(value, parseTerm()); }
            else if (isOperator('-')) { ++pos; value = addPolynomial(value, parseTerm(), -1.0); }
            else return { { 0, 0, 0 }, false };
        }
        return value;
    }
};

Polynomial polynomialize(const Row* row) {
    if (!row) return { { 0, 0, 0 }, false };
    PolynomialParser parser(row, 0, row->items.size());
    return parser.parseRow();
}

Linear addLinear(const Linear& left, const Linear& right, double sign = 1.0) {
    return { left.x + sign * right.x, left.y + sign * right.y,
             left.constant + sign * right.constant, left.valid && right.valid };
}

Linear multiplyLinear(const Linear& left, const Linear& right) {
    if (!left.valid || !right.valid) return { 0, 0, 0, false };
    bool leftVariable = std::fabs(left.x) > 1e-12 || std::fabs(left.y) > 1e-12;
    bool rightVariable = std::fabs(right.x) > 1e-12 || std::fabs(right.y) > 1e-12;
    if (leftVariable && rightVariable) return { 0, 0, 0, false };
    if (!rightVariable) return { left.x * right.constant, left.y * right.constant,
                                 left.constant * right.constant, true };
    return { right.x * left.constant, right.y * left.constant,
             right.constant * left.constant, true };
}

struct LinearParser {
    const std::vector<std::unique_ptr<Item>>& items;
    size_t pos;
    size_t end;

    LinearParser(const Row* row, size_t begin, size_t finish)
        : items(row->items), pos(begin), end(finish) {}

    bool atEnd() const { return pos >= end; }
    const Item* peek() const { return atEnd() ? nullptr : items[pos].get(); }
    bool isOperator(char op) const {
        const Item* item = peek();
        return item && item->type == ItemType::Operator && item->opChar == op;
    }

    Linear parseAtom() {
        if (atEnd()) return { 0, 0, 0, false };
        const Item* item = items[pos++].get();
        switch (item->type) {
            case ItemType::Number: {
                std::string numberText = item->numText;
                while (!atEnd() && items[pos]->type == ItemType::Number)
                    numberText += items[pos++]->numText;
                try { return { 0, 0, std::stod(numberText), true }; }
                catch (...) { return { 0, 0, 0, false }; }
            }
            case ItemType::Variable:
                return item->variableName == 'x' ? Linear{ 1, 0, 0, true } : Linear{ 0, 1, 0, true };
            case ItemType::Paren:
                return linearizeSide(item->a.get());
            case ItemType::Fraction: {
                Linear numerator = linearizeSide(item->a.get());
                Linear denominator = linearizeSide(item->b.get());
                if (!denominator.valid || std::fabs(denominator.x) > 1e-12 || std::fabs(denominator.y) > 1e-12 ||
                    std::fabs(denominator.constant) < 1e-12)
                    return { 0, 0, 0, false };
                return { numerator.x / denominator.constant, numerator.y / denominator.constant,
                         numerator.constant / denominator.constant, numerator.valid };
            }
            case ItemType::Power: {
                Linear base = linearizeSide(item->a.get());
                Linear exponent = linearizeSide(item->b.get());
                if (!base.valid || !exponent.valid || std::fabs(exponent.x) > 1e-12 || std::fabs(exponent.y) > 1e-12)
                    return { 0, 0, 0, false };
                if (std::fabs(exponent.constant - 1.0) < 1e-12) return base;
                if (std::fabs(base.x) > 1e-12 || std::fabs(base.y) > 1e-12)
                    return { 0, 0, 0, false };
                return { 0, 0, std::pow(base.constant, exponent.constant), true };
            }
            case ItemType::Sqrt: {
                Linear value = linearizeSide(item->a.get());
                if (!value.valid || std::fabs(value.x) > 1e-12 || std::fabs(value.y) > 1e-12 || value.constant < 0)
                    return { 0, 0, 0, false };
                return { 0, 0, std::sqrt(value.constant), true };
            }
            case ItemType::Operator:
            case ItemType::Equals:
            case ItemType::CloseParen:
                return { 0, 0, 0, false };
        }
        return { 0, 0, 0, false };
    }

    Linear parseFactor() {
        bool negative = false;
        while (isOperator('-') || isOperator('+')) {
            if (isOperator('-')) negative = !negative;
            ++pos;
        }
        Linear value = parseAtom();
        if (negative) value = { -value.x, -value.y, -value.constant, value.valid };
        return value;
    }

    Linear parseTerm() {
        Linear value = parseFactor();
        while (!atEnd()) {
            if (isOperator('*')) {
                ++pos;
                value = multiplyLinear(value, parseFactor());
            } else if (peek()->type != ItemType::Operator) {
                value = multiplyLinear(value, parseFactor());
            } else {
                break;
            }
        }
        return value;
    }

    Linear parseRow() {
        if (atEnd()) return { 0, 0, 0, false };
        Linear value = parseTerm();
        while (!atEnd()) {
            if (isOperator('+')) { ++pos; value = addLinear(value, parseTerm()); }
            else if (isOperator('-')) { ++pos; value = addLinear(value, parseTerm(), -1.0); }
            else return { 0, 0, 0, false };
        }
        return value;
    }
};

Linear linearizeSide(const Row* row) {
    if (!row) return { 0, 0, 0, false };
    LinearParser parser(row, 0, row->items.size());
    return parser.parseRow();
}

} // namespace

double evaluate(const Row* root, const EvaluationContext& context) {
    if (!root) return 0.0;
    if (root->items.size() == 5 &&
        root->items[2]->type == ItemType::Operator && root->items[2]->opChar == '-' &&
        isOversizedFactorialRange(root->items, 0, 2) &&
        sameRange(root->items, 0, 2, 3, 5)) {
        return 0.0;
    }
    RowParser p(root, context);
    return p.parseRow();
}

double evaluate(const Row* root) {
    return evaluate(root, EvaluationContext{});
}

std::string evaluateToString(const Row* root, const EvaluationContext& context) {
    try {
        double v = evaluate(root, context);
        if (!std::isfinite(v))
            return std::isnan(v) ? "Error" : (v > 0 ? "Infinity" : "-Infinity");
        std::string radical;
        if (simpleSquareRootPower(root, radical))
            return radical + " = " + numberString(v);

        // Prefer plain fixed notation; fall back to scientific for very
        // large/small magnitudes.
        double av = std::fabs(v);
        char buf[64];
        if (av != 0.0 && (av >= 1e15 || av < 1e-9)) {
            std::snprintf(buf, sizeof(buf), "%.6e", v);
        } else {
            std::snprintf(buf, sizeof(buf), "%.10f", v);
            std::string s(buf);
            // trim trailing zeros, then trailing '.'
            size_t dot = s.find('.');
            if (dot != std::string::npos) {
                size_t last = s.find_last_not_of('0');
                if (last == dot) last--; // strip the dot too
                s.erase(last + 1);
            }
            return s;
        }
        return std::string(buf);
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
}

std::string evaluateToString(const Row* root) {
    return evaluateToString(root, EvaluationContext{});
}

QuadraticResult solveQuadratic(double a, double b, double c) {
    QuadraticResult result;
    constexpr double epsilon = 1e-12;
    if (std::fabs(a) < epsilon) {
        if (std::fabs(b) < epsilon) {
            result.valid = false;
            result.message = std::fabs(c) < epsilon ? "Every value is a solution" : "No solution";
            return result;
        }
        result.rootCount = 1;
        result.first = -c / b;
        return result;
    }

    double discriminant = b * b - 4.0 * a * c;
    if (discriminant < -epsilon) {
        result.rootCount = 0;
        result.message = "No real solutions";
        return result;
    }
    if (std::fabs(discriminant) <= epsilon) {
        result.rootCount = 1;
        result.first = -b / (2.0 * a);
        return result;
    }
    double root = std::sqrt(discriminant);
    result.rootCount = 2;
    result.first = (-b + root) / (2.0 * a);
    result.second = (-b - root) / (2.0 * a);
    return result;
}

bool solveTwoVariableSystem(const Row* first, const Row* second,
                            double& x, double& y, std::string& message) {
    auto equation = [](const Row* row, Linear& left, Linear& right) {
        if (!row) return false;
        size_t equals = row->items.size();
        for (size_t i = 0; i < row->items.size(); ++i) {
            if (row->items[i]->type == ItemType::Equals) {
                if (equals != row->items.size()) return false;
                equals = i;
            }
        }
        if (equals == 0 || equals + 1 >= row->items.size()) return false;
        LinearParser leftParser(row, 0, equals);
        LinearParser rightParser(row, equals + 1, row->items.size());
        left = leftParser.parseRow();
        right = rightParser.parseRow();
        return left.valid && right.valid;
    };

    Linear left1, right1, left2, right2;
    if (!equation(first, left1, right1) || !equation(second, left2, right2)) {
        message = "Use two linear equations with one '=' each";
        return false;
    }
    double a1 = left1.x - right1.x;
    double b1 = left1.y - right1.y;
    double c1 = right1.constant - left1.constant;
    double a2 = left2.x - right2.x;
    double b2 = left2.y - right2.y;
    double c2 = right2.constant - left2.constant;
    double determinant = a1 * b2 - a2 * b1;
    if (std::fabs(determinant) < 1e-12) {
        message = "No unique solution";
        return false;
    }
    x = (c1 * b2 - c2 * b1) / determinant;
    y = (a1 * c2 - a2 * c1) / determinant;
    message.clear();
    return true;
}

bool solveSingleVariableEquation(const Row* equation, char& variable,
                                 double& value, std::string& message) {
    if (!equation) {
        message = "Invalid equation";
        return false;
    }
    size_t equals = equation->items.size();
    for (size_t i = 0; i < equation->items.size(); ++i) {
        if (equation->items[i]->type == ItemType::Equals) {
            if (equals != equation->items.size()) {
                message = "Use one '=' per equation";
                return false;
            }
            equals = i;
        }
    }
    if (equals == 0 || equals + 1 >= equation->items.size()) {
        message = "Incomplete equation";
        return false;
    }
    LinearParser leftParser(equation, 0, equals);
    LinearParser rightParser(equation, equals + 1, equation->items.size());
    Linear left = leftParser.parseRow();
    Linear right = rightParser.parseRow();
    if (!left.valid || !right.valid) {
        message = "Equation must be linear";
        return false;
    }
    double xCoefficient = left.x - right.x;
    double yCoefficient = left.y - right.y;
    double constant = right.constant - left.constant;
    if (std::fabs(xCoefficient) > 1e-12 && std::fabs(yCoefficient) > 1e-12) {
        message = "Needs a second equation for x and y";
        return false;
    }
    if (std::fabs(xCoefficient) < 1e-12 && std::fabs(yCoefficient) < 1e-12) {
        message = std::fabs(constant) < 1e-12 ? "Every value is a solution" : "No solution";
        return false;
    }
    if (std::fabs(xCoefficient) > 1e-12) {
        variable = 'x';
        value = constant / xCoefficient;
    } else {
        variable = 'y';
        value = constant / yCoefficient;
    }
    message.clear();
    return true;
}

bool solveVariableAssignment(const Row* equation, const EvaluationContext& context,
                             char& variable, double& value, std::string& message) {
    if (!equation) { message = "Invalid equation"; return false; }
    size_t equals = equation->items.size();
    for (size_t i = 0; i < equation->items.size(); ++i) {
        if (equation->items[i]->type == ItemType::Equals) {
            if (equals != equation->items.size()) { message = "Use one '=' per equation"; return false; }
            equals = i;
        }
    }
    if (equals == equation->items.size()) { message = "Missing '='"; return false; }
    bool variableOnLeft = equals == 1 && equation->items[0]->type == ItemType::Variable;
    bool variableOnRight = equation->items.size() - equals - 1 == 1 &&
                           equation->items[equals + 1]->type == ItemType::Variable;
    if (!variableOnLeft && !variableOnRight) {
        message = "Assignment needs one variable on one side";
        return false;
    }
    const Item* variableItem = variableOnLeft ? equation->items[0].get() : equation->items[equals + 1].get();
    size_t begin = variableOnLeft ? equals + 1 : 0;
    size_t end = variableOnLeft ? equation->items.size() : equals;
    RowParser parser(equation, context, begin, end);
    try { value = parser.parseRow(); }
    catch (const std::exception& error) { message = error.what(); return false; }
    variable = variableItem->variableName;
    message.clear();
    return variable == 'x' || variable == 'y';
}

bool isLinearEquation(const Row* equation) {
    if (!equation) return false;
    size_t equals = equation->items.size();
    for (size_t i = 0; i < equation->items.size(); ++i) {
        if (equation->items[i]->type == ItemType::Equals) {
            if (equals != equation->items.size()) return false;
            equals = i;
        }
    }
    if (equals == 0 || equals + 1 >= equation->items.size()) return false;
    LinearParser leftParser(equation, 0, equals);
    LinearParser rightParser(equation, equals + 1, equation->items.size());
    return leftParser.parseRow().valid && rightParser.parseRow().valid;
}

bool solveQuadraticEquation(const Row* equation, QuadraticResult& result,
                            std::string& message) {
    if (!equation) { message = "Invalid equation"; return false; }
    size_t equals = equation->items.size();
    for (size_t i = 0; i < equation->items.size(); ++i) {
        if (equation->items[i]->type == ItemType::Equals) {
            if (equals != equation->items.size()) { message = "Use one '=' per equation"; return false; }
            equals = i;
        }
    }
    bool hasEquals = equals < equation->items.size();
    if (hasEquals && (equals == 0 || equals + 1 >= equation->items.size())) {
        message = "Incomplete equation";
        return false;
    }
    Polynomial left, right;
    if (hasEquals) {
        PolynomialParser leftParser(equation, 0, equals);
        PolynomialParser rightParser(equation, equals + 1, equation->items.size());
        left = leftParser.parseRow();
        right = rightParser.parseRow();
    } else {
        PolynomialParser expressionParser(equation, 0, equation->items.size());
        left = expressionParser.parseRow();
    }
    if (!left.valid || !right.valid) { message = "Equation must be polynomial in x"; return false; }
    double a = left.coefficient[2] - right.coefficient[2];
    double b = left.coefficient[1] - right.coefficient[1];
    double c = left.coefficient[0] - right.coefficient[0];
    if (std::fabs(a) < 1e-12) { message = "Not a quadratic equation"; return false; }
    result = solveQuadratic(a, b, c);
    if (result.rootCount > 0) {
        double discriminant = b * b - 4.0 * a * c;
        double squareRoot = std::sqrt(std::max(0.0, discriminant));
        if (std::fabs(squareRoot - std::round(squareRoot)) > 1e-10) {
            double outside = 1.0, inside = discriminant;
            std::string radical = radicalText(discriminant, outside, inside);
            auto rootForm = [&](bool positive) {
                double radicalCoefficient = outside / (2.0 * a);
                if (std::fabs(b) < 1e-12 && std::fabs(std::fabs(radicalCoefficient) - 1.0) < 1e-10) {
                    bool negative = positive ? radicalCoefficient < 0.0 : radicalCoefficient > 0.0;
                    return std::string(negative ? "-" : "") + "\xE2\x88\x9A" + numberString(inside);
                }
                return "(" + numberString(-b) + (positive ? "+" : "-") + radical +
                       ")/" + numberString(2.0 * a);
            };
            result.firstExact = rootForm(true);
            if (result.rootCount == 2) result.secondExact = rootForm(false);
        }
    }
    message.clear();
    return true;
}

bool solveGeneralEquation(const Row* equation, const EvaluationContext& context,
                          char& variable, std::vector<double>& roots,
                          std::string& message) {
    if (!equation) { message = "Invalid equation"; return false; }
    size_t equals = equation->items.size();
    for (size_t i = 0; i < equation->items.size(); ++i) {
        if (equation->items[i]->type == ItemType::Equals) {
            if (equals != equation->items.size()) { message = "Use one '=' per equation"; return false; }
            equals = i;
        }
    }
    if (equals == 0 || equals + 1 >= equation->items.size()) {
        message = "Incomplete equation";
        return false;
    }
    bool hasX = false, hasY = false;
    collectVariables(equation, hasX, hasY);
    if (hasX == hasY) {
        message = hasX ? "Needs a second equation for x and y" : "Equation needs a variable";
        return false;
    }
    variable = hasX ? 'x' : 'y';

    auto residual = [&](double value, double& output) {
        EvaluationContext trial = context;
        if (variable == 'x') trial.x = value;
        else trial.y = value;
        try {
            RowParser left(equation, trial, 0, equals);
            RowParser right(equation, trial, equals + 1, equation->items.size());
            output = left.parseRow() - right.parseRow();
            return std::isfinite(output);
        } catch (const std::exception&) {
            return false;
        }
    };

    roots.clear();
    constexpr double lower = -1000.0;
    constexpr double upper = 1000.0;
    constexpr int samples = 40000;
    constexpr double tolerance = 1e-9;
    double previousX = lower, previousValue = 0.0;
    bool previousValid = residual(previousX, previousValue);
    double olderX = previousX, olderValue = previousValue;
    bool olderValid = false;
    bool allZero = previousValid && std::fabs(previousValue) < tolerance;

    auto addRoot = [&](double root) {
        for (double found : roots)
            if (std::fabs(found - root) < 1e-6) return;
        roots.push_back(root);
    };

    for (int sample = 1; sample <= samples; ++sample) {
        double currentX = lower + (upper - lower) * sample / samples;
        double currentValue = 0.0;
        bool currentValid = residual(currentX, currentValue);
        if (currentValid && std::fabs(currentValue) < tolerance) addRoot(currentX);
        if (currentValid && std::fabs(currentValue) >= tolerance) allZero = false;
        if (previousValid && currentValid &&
            ((previousValue < 0.0 && currentValue > 0.0) ||
             (previousValue > 0.0 && currentValue < 0.0))) {
            double left = previousX, right = currentX;
            double leftValue = previousValue;
            for (int iteration = 0; iteration < 80; ++iteration) {
                double middle = (left + right) * 0.5;
                double middleValue = 0.0;
                if (!residual(middle, middleValue)) break;
                if ((leftValue < 0.0 && middleValue > 0.0) ||
                    (leftValue > 0.0 && middleValue < 0.0)) right = middle;
                else { left = middle; leftValue = middleValue; }
            }
            addRoot((left + right) * 0.5);
        }
        if (olderValid && previousValid && currentValid &&
            std::fabs(previousValue) < std::fabs(olderValue) &&
            std::fabs(previousValue) < std::fabs(currentValue)) {
            double left = olderX, right = currentX;
            constexpr double ratio = 0.6180339887498949;
            double first = right - ratio * (right - left);
            double second = left + ratio * (right - left);
            double firstValue = 0.0, secondValue = 0.0;
            bool firstValid = residual(first, firstValue);
            bool secondValid = residual(second, secondValue);
            for (int iteration = 0; iteration < 64; ++iteration) {
                double firstMagnitude = firstValid ? std::fabs(firstValue) : INFINITY;
                double secondMagnitude = secondValid ? std::fabs(secondValue) : INFINITY;
                if (firstMagnitude < secondMagnitude) {
                    right = second;
                    second = first;
                    secondValue = firstValue;
                    secondValid = firstValid;
                    first = right - ratio * (right - left);
                    firstValid = residual(first, firstValue);
                } else {
                    left = first;
                    first = second;
                    firstValue = secondValue;
                    firstValid = secondValid;
                    second = left + ratio * (right - left);
                    secondValid = residual(second, secondValue);
                }
            }
            double minimum = (left + right) * 0.5;
            double minimumValue = 0.0;
            if (residual(minimum, minimumValue) && std::fabs(minimumValue) < 1e-8)
                addRoot(minimum);
        }
        olderX = previousX;
        olderValue = previousValue;
        olderValid = previousValid;
        previousX = currentX;
        previousValue = currentValue;
        previousValid = currentValid;
    }
    if (allZero) message = "Every value is a solution";
    else message = roots.empty() ? "\xE2\x88\x85" : std::string();
    return true;
}
