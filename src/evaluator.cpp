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
    if (left->type == ItemType::Name) return left->nameText == right->nameText;
    if (left->type == ItemType::Constant) return left->constantName == right->constantName;
    if (left->type == ItemType::Function) return left->functionId == right->functionId;
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

// Distinguishable subtype so Pro Mode can catch "too large" and re-run the
// expression in log10-domain arithmetic (see BigValue / evaluateProToString).
struct FactorialTooLargeError : std::runtime_error {
    FactorialTooLargeError() : std::runtime_error("Factorial result is too large") {}
};

double factorial(double value) {
    if (value < 0.0 || std::floor(value) != value)
        throw std::runtime_error("Factorial needs a non-negative integer");
    if (value > 170.0)
        throw FactorialTooLargeError();
    double result = 1.0;
    for (int i = 2; i <= (int)value; ++i) result *= i;
    return result;
}

double permutation(double nVal, double rVal) {
    if (nVal < 0.0 || std::floor(nVal) != nVal || rVal < 0.0 || std::floor(rVal) != rVal)
        throw std::runtime_error("Permutation needs non-negative integers");
    if (rVal > nVal)
        throw std::runtime_error("r cannot exceed n in nPr");
    if (rVal == 0.0) return 1.0;
    if (nVal > 170.0 && (nVal - rVal) < 170.0)
        throw FactorialTooLargeError();
    double result = 1.0;
    for (double i = 0.0; i < rVal; ++i) {
        result *= (nVal - i);
        if (std::isinf(result) || result > 1.79e308) throw FactorialTooLargeError();
    }
    return result;
}

double combination(double nVal, double rVal) {
    if (nVal < 0.0 || std::floor(nVal) != nVal || rVal < 0.0 || std::floor(rVal) != rVal)
        throw std::runtime_error("Combination needs non-negative integers");
    if (rVal > nVal)
        throw std::runtime_error("r cannot exceed n in nCr");
    if (rVal == 0.0 || rVal == nVal) return 1.0;
    double k = std::min(rVal, nVal - rVal);
    double result = 1.0;
    for (double i = 1.0; i <= k; ++i) {
        result = result * (nVal - (k - i)) / i;
        if (std::isinf(result) || result > 1.79e308) throw FactorialTooLargeError();
    }
    return std::round(result);
}

// --- scientific functions (Pro Mode) ---------------------------------------

constexpr double kSciPi = 3.14159265358979323846;
constexpr double kSciE = 2.71828182845904523536;
constexpr double kSciPhi = 1.61803398874989484820;
constexpr double kLog10E = 0.43429448190325182765;   // log10(e)

double angleToRadians(double value, bool degrees) {
    return degrees ? value * (kSciPi / 180.0) : value;
}

double angleFromRadians(double value, bool degrees) {
    return degrees ? value * (180.0 / kSciPi) : value;
}

// Plain double-precision evaluation of a scientific function, with explicit
// domain errors. Overflow (e.g. exp(1000)) yields +-inf, which the Pro Mode
// wrapper detects and re-runs in log10 space instead of reporting.
double applySciFunction(int id, double x, bool degrees) {
    switch (id) {
        case SciSin: return std::sin(angleToRadians(x, degrees));
        case SciCos: return std::cos(angleToRadians(x, degrees));
        case SciTan: {
            double cosine = std::cos(angleToRadians(x, degrees));
            if (std::fabs(cosine) < 1e-15)
                throw std::runtime_error("tan is undefined here");
            return std::sin(angleToRadians(x, degrees)) / cosine;
        }
        case SciSec: {
            double cosine = std::cos(angleToRadians(x, degrees));
            if (std::fabs(cosine) < 1e-15)
                throw std::runtime_error("sec is undefined here");
            return 1.0 / cosine;
        }
        case SciCsc: {
            double sine = std::sin(angleToRadians(x, degrees));
            if (std::fabs(sine) < 1e-15)
                throw std::runtime_error("csc is undefined here");
            return 1.0 / sine;
        }
        case SciCot: {
            double sine = std::sin(angleToRadians(x, degrees));
            if (std::fabs(sine) < 1e-15)
                throw std::runtime_error("cot is undefined here");
            return std::cos(angleToRadians(x, degrees)) / sine;
        }
        case SciAsin:
            if (x < -1.0 || x > 1.0)
                throw std::runtime_error("asin needs input in -1..1");
            return angleFromRadians(std::asin(x), degrees);
        case SciAcos:
            if (x < -1.0 || x > 1.0)
                throw std::runtime_error("acos needs input in -1..1");
            return angleFromRadians(std::acos(x), degrees);
        case SciAtan: return angleFromRadians(std::atan(x), degrees);
        case SciAsec:
            if (std::fabs(x) < 1.0)
                throw std::runtime_error("asec needs input outside (-1, 1)");
            return angleFromRadians(std::acos(1.0 / x), degrees);
        case SciAcsc:
            if (std::fabs(x) < 1.0)
                throw std::runtime_error("acsc needs input outside (-1, 1)");
            return angleFromRadians(std::asin(1.0 / x), degrees);
        case SciAcot:
            if (x == 0.0) return angleFromRadians(kSciPi / 2.0, degrees);
            return angleFromRadians(std::atan(1.0 / x), degrees);
        case SciSinh: return std::sinh(x);
        case SciCosh: return std::cosh(x);
        case SciTanh: return std::tanh(x);
        case SciAsinh: return std::asinh(x);
        case SciAcosh:
            if (x < 1.0) throw std::runtime_error("acosh needs input >= 1");
            return std::acosh(x);
        case SciAtanh:
            if (x <= -1.0 || x >= 1.0) throw std::runtime_error("atanh needs input in (-1, 1)");
            return std::atanh(x);
        case SciSech: return 1.0 / std::cosh(x);
        case SciCsch:
            if (x == 0.0) throw std::runtime_error("csch is undefined at 0");
            return 1.0 / std::sinh(x);
        case SciCoth:
            if (x == 0.0) throw std::runtime_error("coth is undefined at 0");
            return 1.0 / std::tanh(x);
        case SciAsech:
            if (x <= 0.0 || x > 1.0) throw std::runtime_error("asech needs input in (0, 1]");
            return std::acosh(1.0 / x);
        case SciAcsch:
            if (x == 0.0) throw std::runtime_error("acsch is undefined at 0");
            return std::asinh(1.0 / x);
        case SciAcoth:
            if (std::fabs(x) <= 1.0) throw std::runtime_error("acoth needs input outside [-1, 1]");
            return std::atanh(1.0 / x);
        case SciLn:
            if (!(x > 0.0)) throw std::runtime_error("ln needs a positive number");
            return std::log(x);
        case SciLog:
            if (!(x > 0.0)) throw std::runtime_error("log needs a positive number");
            return std::log10(x);
        case SciLog2:
            if (!(x > 0.0)) throw std::runtime_error("log2 needs a positive number");
            return std::log2(x);
        case SciExp: return std::exp(x);
        case SciExpm1: return std::expm1(x);
        case SciLog1p:
            if (x <= -1.0) throw std::runtime_error("log1p needs input > -1");
            return std::log1p(x);
        case SciCbrt: return std::cbrt(x);
        case SciAbs: return std::fabs(x);
        case SciFloor: return std::floor(x);
        case SciCeil: return std::ceil(x);
        case SciRound: return std::round(x);
        case SciTrunc: return std::trunc(x);
        case SciSgn: return (x > 0.0) ? 1.0 : ((x < 0.0) ? -1.0 : 0.0);
        case SciGamma:
            if (x <= 0.0 && std::floor(x) == x)
                throw std::runtime_error("gamma is undefined at non-positive integers");
            return std::tgamma(x);
        case SciLgamma:
            if (x <= 0.0 && std::floor(x) == x)
                throw std::runtime_error("lgamma is undefined at non-positive integers");
            return std::lgamma(x);
        case SciErf: return std::erf(x);
        case SciErfc: return std::erfc(x);
        case SciFact: return factorial(x);
        case SciDeg: return x * (180.0 / kSciPi);
        case SciRad: return x * (kSciPi / 180.0);
    }
    throw std::runtime_error("Unknown function");
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
            case ItemType::Constant:
                pos++;
                if (it->constantName == 'p') return kSciPi;
                if (it->constantName == 'f') return kSciPhi;
                return kSciE;
            case ItemType::Permutation: {
                pos++;
                double n = evaluate(it->a.get(), context);
                double r = evaluate(it->b.get(), context);
                return permutation(n, r);
            }
            case ItemType::Combination: {
                pos++;
                double n = evaluate(it->a.get(), context);
                double r = evaluate(it->b.get(), context);
                return combination(n, r);
            }
            case ItemType::Function: {
                pos++;
                int functionId = it->functionId;
                double arg = evaluate(it->a.get(), context);
                return applySciFunction(functionId, arg, context.degrees);
            }
            case ItemType::Name:
                throw std::runtime_error("Unknown name");
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
            if (peekIsOperatorChar('%')) {
                pos++;
                double div = parseFactor();
                if (div == 0.0) throw std::runtime_error("Division by zero");
                v = std::fmod(v, div);
                continue;
            }
            // Implicit multiplication: next token exists and is not a
            // flat operator (+, -, *, %) -> another atom starts here.
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

// --- Pro Mode "super large" arithmetic -------------------------------------
//
// Values beyond double range are carried as (sign, log10|v|) pairs:
// multiplication turns into log addition, factorials of astronomic
// arguments come from lgamma(), and powers like 2^10000000000 or
// 10000000000! produce real answers formatted as "m * 10^e". Results whose
// log10 lands within +/-15 are round-tripped back to a plain double and
// formatted exactly like ordinary results.

struct BigValue {
    bool isZero = true;      // exactly zero
    bool negative = false;   // sign; meaningless when isZero
    double log10Abs = 0.0;   // log10(|value|); meaningless when isZero
};

std::string formatFiniteDouble(double v) {
    // Shared with evaluateToString: plain fixed notation, falling back to
    // scientific for very large/small magnitudes.
    double av = std::fabs(v);
    char buf[64];
    if (av != 0.0 && (av >= 1e15 || av < 1e-9)) {
        std::snprintf(buf, sizeof(buf), "%.6e", v);
        return std::string(buf);
    }
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

BigValue bigFromDouble(double value) {
    BigValue v;
    if (value == 0.0) return v;
    v.isZero = false;
    v.negative = value < 0.0;
    v.log10Abs = std::log10(std::fabs(value));
    return v;
}

// True when a double is (within roundtrip fuzz of) a whole number. Values
// that came through log10() can be off by an ulp, e.g. pow(10, log10(123))
// = 123.00000000000001.
bool bigIsIntegral(double value) {
    if (!std::isfinite(value)) return false;
    double rounded = std::round(value);
    if (rounded == 0.0) return value == 0.0;
    return std::fabs(value - rounded) <= 1e-9 * rounded;
}

BigValue bigAdd(const BigValue& a, const BigValue& b) {
    if (a.isZero) return b;
    if (b.isZero) return a;
    BigValue r;
    r.isZero = false;
    if (a.negative == b.negative) {
        // |a| + |b| = 10^hi * (1 + 10^-d)
        double hi = std::max(a.log10Abs, b.log10Abs);
        double d = std::fabs(a.log10Abs - b.log10Abs);
        r.negative = a.negative;
        r.log10Abs = d > 17.0 ? hi : hi + std::log10(1.0 + std::pow(10.0, -d));
        return r;
    }
    // Signs differ: |a| - |b|.
    const BigValue& big = a.log10Abs >= b.log10Abs ? a : b;
    const BigValue& small = a.log10Abs >= b.log10Abs ? b : a;
    double d = big.log10Abs - small.log10Abs;
    if (d > 17.0) {                    // the smaller magnitude is negligible
        r.negative = big.negative;
        r.log10Abs = big.log10Abs;
        return r;
    }
    double t = 1.0 - std::pow(10.0, -d);
    if (t <= 0.0) return BigValue{};   // exact cancellation
    r.negative = big.negative;
    r.log10Abs = big.log10Abs + std::log10(t);
    return r;
}

BigValue bigSub(const BigValue& a, const BigValue& b) {
    BigValue negated = b;
    if (!negated.isZero) negated.negative = !negated.negative;
    return bigAdd(a, negated);
}

BigValue bigMul(const BigValue& a, const BigValue& b) {
    if (a.isZero || b.isZero) return BigValue{};
    BigValue r;
    r.isZero = false;
    r.negative = a.negative != b.negative;
    r.log10Abs = a.log10Abs + b.log10Abs;
    if (!std::isfinite(r.log10Abs)) throw std::runtime_error("Result is too large");
    return r;
}

BigValue bigDiv(const BigValue& a, const BigValue& b) {
    if (b.isZero) throw std::runtime_error("Division by zero");
    if (a.isZero) return BigValue{};
    BigValue r;
    r.isZero = false;
    r.negative = a.negative != b.negative;
    r.log10Abs = a.log10Abs - b.log10Abs;
    return r;
}

BigValue bigPow(const BigValue& base, const BigValue& exponent) {
    if (base.isZero) {
        if (exponent.isZero) return bigFromDouble(1.0);   // 0^0 = 1, like std::pow
        if (exponent.negative) throw std::runtime_error("Division by zero");
        return BigValue{};                                // 0^positive = 0
    }
    // The exponent's actual value (it is itself stored in log10 space).
    double expValue = 0.0;
    if (!exponent.isZero) {
        expValue = std::pow(10.0, exponent.log10Abs);
        if (exponent.negative) expValue = -expValue;
    }
    BigValue r;
    r.isZero = false;
    r.negative = false;
    if (base.negative) {
        if (!exponent.isZero && !std::isfinite(expValue)) {
            // Beyond 2^53 every finite double is a whole number and the
            // low bit reads 0, so a huge exponent counts as even.
            r.negative = false;
        } else {
            if (!bigIsIntegral(expValue)) throw std::runtime_error("Complex result");
            r.negative = std::fmod(std::round(expValue), 2.0) != 0.0;
        }
    }
    r.log10Abs = expValue * base.log10Abs;
    if (!std::isfinite(r.log10Abs)) throw std::runtime_error("Result is too large");
    return r;
}

BigValue bigSqrt(const BigValue& v) {
    if (v.isZero) return BigValue{};
    if (v.negative) throw std::runtime_error("Root of negative number");
    BigValue r;
    r.isZero = false;
    r.log10Abs = v.log10Abs / 2.0;
    return r;
}

BigValue bigFactorial(const BigValue& v) {
    if (v.isZero) return bigFromDouble(1.0);   // 0! = 1
    if (v.negative)
        throw std::runtime_error("Factorial needs a non-negative integer");
    double n = std::pow(10.0, v.log10Abs);
    if (!std::isfinite(n)) throw std::runtime_error("Result is too large");
    if (!bigIsIntegral(n) || std::round(n) < 1.0)
        throw std::runtime_error("Factorial needs a non-negative integer");
    n = std::round(n);
    double log10Factorial = std::lgamma(n + 1.0) / std::log(10.0);
    if (!std::isfinite(log10Factorial)) throw std::runtime_error("Result is too large");
    BigValue r;
    r.isZero = false;
    r.log10Abs = log10Factorial;
    return r;
}

BigValue bigPermutation(const BigValue& n, const BigValue& r) {
    double nVal = n.isZero ? 0.0 : (n.negative ? -1.0 : 1.0) * std::pow(10.0, n.log10Abs);
    double rVal = r.isZero ? 0.0 : (r.negative ? -1.0 : 1.0) * std::pow(10.0, r.log10Abs);
    if (!std::isfinite(nVal) || !std::isfinite(rVal) ||
        nVal < 0.0 || !bigIsIntegral(nVal) || rVal < 0.0 || !bigIsIntegral(rVal)) {
        throw std::runtime_error("Permutation needs non-negative integers");
    }
    nVal = std::round(nVal);
    rVal = std::round(rVal);
    if (rVal > nVal) throw std::runtime_error("r cannot exceed n in nPr");
    if (rVal == 0.0) return bigFromDouble(1.0);
    double log10P = (std::lgamma(nVal + 1.0) - std::lgamma(nVal - rVal + 1.0)) / std::log(10.0);
    if (!std::isfinite(log10P)) throw std::runtime_error("Result is too large");
    BigValue res;
    res.isZero = false;
    res.negative = false;
    res.log10Abs = log10P;
    return res;
}

BigValue bigCombination(const BigValue& n, const BigValue& r) {
    double nVal = n.isZero ? 0.0 : (n.negative ? -1.0 : 1.0) * std::pow(10.0, n.log10Abs);
    double rVal = r.isZero ? 0.0 : (r.negative ? -1.0 : 1.0) * std::pow(10.0, r.log10Abs);
    if (!std::isfinite(nVal) || !std::isfinite(rVal) ||
        nVal < 0.0 || !bigIsIntegral(nVal) || rVal < 0.0 || !bigIsIntegral(rVal)) {
        throw std::runtime_error("Combination needs non-negative integers");
    }
    nVal = std::round(nVal);
    rVal = std::round(rVal);
    if (rVal > nVal) throw std::runtime_error("r cannot exceed n in nCr");
    if (rVal == 0.0 || rVal == nVal) return bigFromDouble(1.0);
    double log10C = (std::lgamma(nVal + 1.0) - std::lgamma(rVal + 1.0) - std::lgamma(nVal - rVal + 1.0)) / std::log(10.0);
    if (!std::isfinite(log10C)) throw std::runtime_error("Result is too large");
    BigValue res;
    res.isZero = false;
    res.negative = false;
    res.log10Abs = log10C;
    return res;
}

// Scientific functions in log10 space.
BigValue applySciFunctionBig(int id, const BigValue& x, bool degrees) {
    if (id == SciAbs) {
        BigValue r = x;
        r.negative = false;
        return r;
    }
    if (id == SciExp) {
        if (x.isZero) return bigFromDouble(1.0);
        double argument = std::pow(10.0, x.log10Abs);
        if (x.negative) argument = -argument;
        if (!std::isfinite(argument)) throw std::runtime_error("Result is too large");
        double log10Result = argument * kLog10E;
        if (!std::isfinite(log10Result)) throw std::runtime_error("Result is too large");
        BigValue r;
        r.isZero = false;
        r.log10Abs = log10Result;
        return r;
    }
    if (id == SciLn || id == SciLog || id == SciLog2) {
        if (x.isZero || x.negative)
            throw std::runtime_error("Logarithm needs a positive number");
        double result = (id == SciLn) ? x.log10Abs / kLog10E :
                        (id == SciLog2 ? x.log10Abs / std::log10(2.0) : x.log10Abs);
        if (!std::isfinite(result)) throw std::runtime_error("Result is too large");
        return bigFromDouble(result);
    }
    if (id == SciCbrt) {
        BigValue r = x;
        r.log10Abs = x.log10Abs / 3.0;
        return r;
    }
    if (id == SciSgn) {
        if (x.isZero) return bigFromDouble(0.0);
        return bigFromDouble(x.negative ? -1.0 : 1.0);
    }
    if (id == SciFloor || id == SciCeil || id == SciRound || id == SciTrunc) {
        if (x.log10Abs > 15.0) return x; // already effectively an integer
        double val = (x.negative ? -1.0 : 1.0) * std::pow(10.0, x.log10Abs);
        return bigFromDouble(applySciFunction(id, val, degrees));
    }
    if (id == SciCosh || id == SciSinh) {
        if (x.isZero) return bigFromDouble(id == SciCosh ? 1.0 : 0.0);
        double magnitude = std::pow(10.0, x.log10Abs);
        if (std::isfinite(magnitude) && magnitude <= 700.0) {
            double value = x.negative ? -magnitude : magnitude;
            double result = id == SciCosh ? std::cosh(value) : std::sinh(value);
            if (std::isfinite(result)) return bigFromDouble(result);
        }
        double log10Result = magnitude * kLog10E - std::log10(2.0);
        if (!std::isfinite(log10Result)) throw std::runtime_error("Result is too large");
        BigValue r;
        r.isZero = false;
        r.negative = (id == SciSinh) && x.negative;
        r.log10Abs = log10Result;
        return r;
    }
    if (id == SciTanh) {
        if (!x.isZero && x.log10Abs > 20.0)
            return bigFromDouble(x.negative ? -1.0 : 1.0);
        double value = x.isZero ? 0.0
                                : (x.negative ? -1.0 : 1.0) * std::pow(10.0, x.log10Abs);
        return bigFromDouble(std::tanh(value));
    }
    if (id == SciAtan) {
        if (!x.isZero && x.log10Abs > 15.0)
            return bigFromDouble(x.negative ? -kSciPi / 2.0 : kSciPi / 2.0);
        double value = x.isZero ? 0.0
                                : (x.negative ? -1.0 : 1.0) * std::pow(10.0, x.log10Abs);
        return bigFromDouble(applySciFunction(SciAtan, value, degrees));
    }
    if (id == SciAsin || id == SciAcos) {
        if (!x.isZero && x.log10Abs > 0.0)
            throw std::runtime_error(id == SciAsin ? "asin needs input in -1..1"
                                                   : "acos needs input in -1..1");
        double value = x.isZero ? 0.0
                                : (x.negative ? -1.0 : 1.0) * std::pow(10.0, x.log10Abs);
        return bigFromDouble(applySciFunction(id, value, degrees));
    }
    // Trig / reciprocal trig / other: argument reduction limit
    if (!x.isZero && x.log10Abs > 15.0) {
        bool isTrig = (id == SciSin || id == SciCos || id == SciTan ||
                       id == SciSec || id == SciCsc || id == SciCot);
        throw std::runtime_error(isTrig ? "Argument too large for trig"
                                        : "Argument too large for function");
    }
    double value = x.isZero ? 0.0
                            : (x.negative ? -1.0 : 1.0) * std::pow(10.0, x.log10Abs);
    return bigFromDouble(applySciFunction(id, value, degrees));
}

BigValue bigEvaluate(const Row* root, const EvaluationContext& context);

// Mirrors RowParser exactly, but every value lives in the log10 domain.
struct BigParser {
    const std::vector<std::unique_ptr<Item>>& items;
    const EvaluationContext& context;
    size_t pos = 0;
    size_t end = 0;

    BigParser(const Row* row, const EvaluationContext& values)
        : items(row->items), context(values), pos(0), end(row->items.size()) {}

    bool atEnd() const { return pos >= end; }
    const Item* peek() const { return atEnd() ? nullptr : items[pos].get(); }

    bool peekIsOperatorChar(char c) const {
        const Item* it = peek();
        return it && it->type == ItemType::Operator && it->opChar == c;
    }

    BigValue parseNumberLiteral() {
        std::string numberText = items[pos++]->numText;
        while (!atEnd() && items[pos]->type == ItemType::Number)
            numberText += items[pos++]->numText;
        if (numberText.empty() || numberText == ".")
            throw std::runtime_error("Invalid number");
        try {
            return bigFromDouble(std::stod(numberText));
        } catch (const std::out_of_range&) {
            // Literal too large for a double (e.g. a 400-digit integer):
            // derive log10 directly from its digits.
            if (numberText.find('.') != std::string::npos)
                throw std::runtime_error("Invalid number");
            std::string digits;
            for (char c : numberText)
                if (c >= '0' && c <= '9') digits += c;
            size_t firstSignificant = digits.find_first_not_of('0');
            if (firstSignificant == std::string::npos) return BigValue{};
            digits = digits.substr(firstSignificant);
            std::string head = digits.substr(0, 15);
            double significand = std::stod(head.substr(0, 1) + "." +
                                           (head.size() > 1 ? head.substr(1) : std::string("0")));
            BigValue v;
            v.isZero = false;
            v.log10Abs = (double)(digits.size() - 1) + std::log10(significand);
            return v;
        } catch (...) {
            throw std::runtime_error("Invalid number");
        }
    }

    BigValue parseAtom() {
        if (atEnd())
            throw std::runtime_error("Incomplete expression");
        const Item* it = items[pos].get();
        switch (it->type) {
            case ItemType::Number:
                return parseNumberLiteral();
            case ItemType::Variable:
                pos++;
                if (it->variableName == 'x') return bigFromDouble(context.x);
                if (it->variableName == 'y') return bigFromDouble(context.y);
                throw std::runtime_error("Unknown variable");
            case ItemType::Fraction: {
                pos++;
                BigValue numerator = bigEvaluate(it->a.get(), context);
                BigValue denominator = bigEvaluate(it->b.get(), context);
                return bigDiv(numerator, denominator);
            }
            case ItemType::Paren:
                pos++;
                return bigEvaluate(it->a.get(), context);
            case ItemType::Power:
                pos++;
                return bigPow(bigEvaluate(it->a.get(), context),
                              bigEvaluate(it->b.get(), context));
            case ItemType::Sqrt:
                pos++;
                return bigSqrt(bigEvaluate(it->a.get(), context));
            case ItemType::Constant:
                pos++;
                if (it->constantName == 'p') return bigFromDouble(kSciPi);
                if (it->constantName == 'f') return bigFromDouble(kSciPhi);
                return bigFromDouble(kSciE);
            case ItemType::Permutation: {
                pos++;
                BigValue n = bigEvaluate(it->a.get(), context);
                BigValue r = bigEvaluate(it->b.get(), context);
                return bigPermutation(n, r);
            }
            case ItemType::Combination: {
                pos++;
                BigValue n = bigEvaluate(it->a.get(), context);
                BigValue r = bigEvaluate(it->b.get(), context);
                return bigCombination(n, r);
            }
            case ItemType::Function: {
                pos++;
                int functionId = it->functionId;
                BigValue arg = bigEvaluate(it->a.get(), context);
                return applySciFunctionBig(functionId, arg, context.degrees);
            }
            case ItemType::Name:
                throw std::runtime_error("Unknown name");
            case ItemType::Operator:
                throw std::runtime_error("Unexpected operator");
            case ItemType::Equals:
                throw std::runtime_error("Equation needs two lines");
            case ItemType::CloseParen:
                throw std::runtime_error("Unmatched closing parenthesis");
        }
        throw std::runtime_error("Unknown item");
    }

    BigValue parseFactor() {
        bool negated = false;
        while (peekIsOperatorChar('-') || peekIsOperatorChar('+')) {
            if (peekIsOperatorChar('-')) negated = !negated;
            pos++;
        }
        BigValue v = parseAtom();
        while (peekIsOperatorChar('!')) {
            pos++;
            v = bigFactorial(v);
        }
        if (negated && !v.isZero) v.negative = !v.negative;
        return v;
    }

    // '*' explicit, or bare adjacency of two atoms (implicit multiplication,
    // e.g. "2(3+4)").
    BigValue parseTerm() {
        BigValue v = parseFactor();
        for (;;) {
            if (peekIsOperatorChar('*')) {
                pos++;
                v = bigMul(v, parseFactor());
                continue;
            }
            if (peekIsOperatorChar('%')) {
                pos++;
                BigValue divisor = parseFactor();
                if (divisor.isZero) throw std::runtime_error("Division by zero");
                double num = (v.negative ? -1.0 : 1.0) * std::pow(10.0, v.log10Abs);
                double den = (divisor.negative ? -1.0 : 1.0) * std::pow(10.0, divisor.log10Abs);
                v = bigFromDouble(std::fmod(num, den));
                continue;
            }
            const Item* next = peek();
            if (next && next->type != ItemType::Operator) {
                v = bigMul(v, parseFactor());
                continue;
            }
            break;
        }
        return v;
    }

    BigValue parseRow() {
        if (atEnd()) return BigValue{}; // empty row evaluates to 0
        BigValue v = parseTerm();
        for (;;) {
            if (peekIsOperatorChar('+')) {
                pos++;
                v = bigAdd(v, parseTerm());
            } else if (peekIsOperatorChar('-')) {
                pos++;
                v = bigSub(v, parseTerm());
            } else {
                break;
            }
        }
        if (!atEnd())
            throw std::runtime_error("Malformed expression");
        return v;
    }
};

BigValue bigEvaluate(const Row* root, const EvaluationContext& context) {
    if (!root) return BigValue{};
    BigParser parser(root, context);
    return parser.parseRow();
}

std::string formatBigValue(const BigValue& v) {
    if (v.isZero) return "0";
    if (v.log10Abs >= -15.0 && v.log10Abs <= 15.0) {
        double value = std::pow(10.0, v.log10Abs);
        return formatFiniteDouble(v.negative ? -value : value);
    }
    double exponent = std::floor(v.log10Abs);
    double mantissa = std::pow(10.0, v.log10Abs - exponent);
    char mant[64];
    std::snprintf(mant, sizeof(mant), "%.10g", mantissa);
    if (std::string(mant) == "10") {   // renormalise "10 * 10^e"
        exponent += 1.0;
        std::snprintf(mant, sizeof(mant), "1");
    }
    char exponentText[64];
    std::snprintf(exponentText, sizeof(exponentText), "%.0f", exponent);
    std::string sign = v.negative ? "-" : "";
    if (std::string(mant) == "1")
        return sign + "10^" + exponentText;
    return sign + mant + " * 10^" + exponentText;
}

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
            case ItemType::Name:
            case ItemType::Constant:
            case ItemType::Function:
            case ItemType::Permutation:
            case ItemType::Combination:
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
            case ItemType::Name:
            case ItemType::Constant:
            case ItemType::Function:
            case ItemType::Permutation:
            case ItemType::Combination:
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
        return formatFiniteDouble(v);
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
}

std::string evaluateToString(const Row* root) {
    return evaluateToString(root, EvaluationContext{});
}

std::string evaluateProToString(const Row* root, const EvaluationContext& context) {
    bool bigNeeded = false;
    try {
        double value = evaluate(root, context);
        bigNeeded = !std::isfinite(value);
        // A plain 0 can still hide a real tiny value that underflowed the
        // double path (exp(-1000) and friends); Pro Mode reruns the whole
        // expression in log10 space and keeps that answer when it is
        // genuinely non-zero.
        if (!bigNeeded && value == 0.0) {
            try {
                BigValue big = bigEvaluate(root, context);
                if (!big.isZero) return formatBigValue(big);
            } catch (const std::exception&) {
                // log10 path does not apply here; keep the plain result.
            }
        }
    } catch (const FactorialTooLargeError&) {
        bigNeeded = true;
    }
    if (!bigNeeded) return evaluateToString(root, context);
    return formatBigValue(bigEvaluate(root, context));
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
    const bool firstIsLinear = equation(first, left1, right1);
    const bool secondIsLinear = equation(second, left2, right2);
    if (firstIsLinear && secondIsLinear) {
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

    auto containsVariables = [](const Row* row, bool& hasX, bool& hasY, auto&& self) -> void {
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
    containsVariables(first, hasX, hasY, containsVariables);
    containsVariables(second, hasX, hasY, containsVariables);
    if (!hasX || !hasY || !first || !second) {
        message = "Use two equations involving x and y";
        return false;
    }

    auto equationResidual = [](const Row* row, double xValue, double yValue, double& output) {
        if (!row) return false;
        size_t equals = row->items.size();
        for (size_t i = 0; i < row->items.size(); ++i) {
            if (row->items[i]->type == ItemType::Equals) {
                if (equals != row->items.size()) return false;
                equals = i;
            }
        }
        if (equals == 0 || equals + 1 >= row->items.size()) return false;
        EvaluationContext trial;
        trial.x = xValue;
        trial.y = yValue;
        try {
            RowParser left(row, trial, 0, equals);
            RowParser right(row, trial, equals + 1, row->items.size());
            output = left.parseRow() - right.parseRow();
            return std::isfinite(output);
        } catch (const std::exception&) {
            return false;
        }
    };

    constexpr double seeds[] = { -1000.0, -100.0, -10.0, -1.0, -0.1, 0.1, 1.0, 10.0, 100.0, 1000.0 };
    constexpr double tolerance = 1e-8;
    auto residualNorm = [](double firstValue, double secondValue) {
        return std::max(std::fabs(firstValue), std::fabs(secondValue));
    };
    for (double seedX : seeds) {
        for (double seedY : seeds) {
            double candidateX = seedX;
            double candidateY = seedY;
            for (int iteration = 0; iteration < 100; ++iteration) {
                double firstValue = 0.0, secondValue = 0.0;
                if (!equationResidual(first, candidateX, candidateY, firstValue) ||
                    !equationResidual(second, candidateX, candidateY, secondValue))
                    break;
                if (residualNorm(firstValue, secondValue) < tolerance) {
                    x = candidateX;
                    y = candidateY;
                    message.clear();
                    return true;
                }

                double stepX = 1e-5 * std::max(1.0, std::fabs(candidateX));
                double stepY = 1e-5 * std::max(1.0, std::fabs(candidateY));
                double firstXPlus = 0.0, firstXMinus = 0.0, secondXPlus = 0.0, secondXMinus = 0.0;
                double firstYPlus = 0.0, firstYMinus = 0.0, secondYPlus = 0.0, secondYMinus = 0.0;
                if (!equationResidual(first, candidateX + stepX, candidateY, firstXPlus) ||
                    !equationResidual(first, candidateX - stepX, candidateY, firstXMinus) ||
                    !equationResidual(second, candidateX + stepX, candidateY, secondXPlus) ||
                    !equationResidual(second, candidateX - stepX, candidateY, secondXMinus) ||
                    !equationResidual(first, candidateX, candidateY + stepY, firstYPlus) ||
                    !equationResidual(first, candidateX, candidateY - stepY, firstYMinus) ||
                    !equationResidual(second, candidateX, candidateY + stepY, secondYPlus) ||
                    !equationResidual(second, candidateX, candidateY - stepY, secondYMinus))
                    break;

                double j11 = (firstXPlus - firstXMinus) / (2.0 * stepX);
                double j21 = (secondXPlus - secondXMinus) / (2.0 * stepX);
                double j12 = (firstYPlus - firstYMinus) / (2.0 * stepY);
                double j22 = (secondYPlus - secondYMinus) / (2.0 * stepY);
                double determinant = j11 * j22 - j12 * j21;
                double determinantScale = std::max(1.0, std::fabs(j11 * j22) + std::fabs(j12 * j21));
                if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-14 * determinantScale)
                    break;

                double deltaX = (-firstValue * j22 + j12 * secondValue) / determinant;
                double deltaY = (j21 * firstValue - j11 * secondValue) / determinant;
                if (!std::isfinite(deltaX) || !std::isfinite(deltaY))
                    break;

                double currentNorm = residualNorm(firstValue, secondValue);
                bool improved = false;
                for (double amount = 1.0; amount >= 1.0 / 1024.0; amount *= 0.5) {
                    double nextX = candidateX + amount * deltaX;
                    double nextY = candidateY + amount * deltaY;
                    if (std::fabs(nextX) > 1e6 || std::fabs(nextY) > 1e6)
                        continue;
                    double nextFirst = 0.0, nextSecond = 0.0;
                    if (equationResidual(first, nextX, nextY, nextFirst) &&
                        equationResidual(second, nextX, nextY, nextSecond) &&
                        residualNorm(nextFirst, nextSecond) < currentNorm) {
                        candidateX = nextX;
                        candidateY = nextY;
                        improved = true;
                        break;
                    }
                }
                if (!improved) break;
            }
        }
    }

    message = "No unique solution";
    return false;
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

bool isProModeTrigger(const Row* expression) {
    if (!expression || expression->items.size() != 5) return false;
    const auto& items = expression->items;
    return items[0]->type == ItemType::Number && items[0]->numText == "2000" &&
           items[1]->type == ItemType::Operator && items[1]->opChar == '!' &&
           items[2]->type == ItemType::Operator && items[2]->opChar == '-' &&
           items[3]->type == ItemType::Number && items[3]->numText == "1999" &&
           items[4]->type == ItemType::Operator && items[4]->opChar == '!';
}
