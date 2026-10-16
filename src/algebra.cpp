// algebra.cpp
//
// See algebra.h. A polynomial here is a sum of monomials; a monomial is a
// coefficient times atoms raised to real exponents. Atoms are the unknowns
// x and y, the constants pi/e/phi (kept symbolic so "2πx" stays readable),
// and opaque sub-expressions such as "sin(x)" or "(x + 1)" carried as text.
// That is enough to collect like terms, distribute numbers over sums, and
// isolate y (or x) when it appears to the first or second power.

#include "algebra.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

const std::string kPi = "\xCF\x80";
const std::string kPhi = "\xCF\x86";
const std::string kMinus = "\xE2\x88\x92";
const std::string kSqrt = "\xE2\x88\x9A";
const std::string kPlusMinus = "\xC2\xB1";
const std::string kDot = "\xC2\xB7";
const std::string kInverse = "\xE2\x81\xBB\xC2\xB9";   // superscript -1
constexpr size_t kAll = (size_t)-1;
constexpr size_t kMaxTerms = 400;

// ------------------------------------------------------------ free variables

bool isClosedCalculusFunction(int id) {
    return id == SciIntegrate || id == SciDiff || id == SciLimit ||
           id == SciSum || id == SciProduct;
}

struct Bound {
    bool x = false;
    bool y = false;
    Bound with(char v) const {
        Bound b = *this;
        if (v == 'y') b.y = true; else b.x = true;
        return b;
    }
};

char boundVariable(const Item* it) { return it->variableName ? it->variableName : 'x'; }

// The variable a calculus function form runs over, chosen the way the
// evaluator does: sum/product(f, var, a, b) and integrate/diff(f, var, ...)
// name it in the second argument; otherwise integrate/diff use the first
// variable written in f, and everything else uses x.
char calculusVariable(const Item* it) {
    const Row* args = it->a.get();
    if (!args) return 'x';
    std::vector<size_t> commas;
    for (size_t i = 0; i < args->items.size(); ++i)
        if (args->items[i]->type == ItemType::Operator && args->items[i]->opChar == ',') commas.push_back(i);
    bool secondIsVariable = !commas.empty() && commas[0] + 1 < args->items.size() &&
                            args->items[commas[0] + 1]->type == ItemType::Variable;
    char second = secondIsVariable ? args->items[commas[0] + 1]->variableName : 0;
    switch (it->functionId) {
        case SciSum:
        case SciProduct:
            return commas.size() == 3 && second ? second : 'x';
        case SciIntegrate:
        case SciDiff: {
            if (second) return second;
            size_t end = commas.empty() ? args->items.size() : commas[0];
            for (size_t i = 0; i < end; ++i)
                if (args->items[i]->type == ItemType::Variable) return args->items[i]->variableName;
            return 'x';
        }
        default:
            return 'x';
    }
}

// Summation/Product index: "y = 1" in the lower limit binds y; default x.
char indexVariable(const Row* lower) {
    if (!lower) return 'x';
    for (size_t i = 0; i < lower->items.size(); ++i) {
        if (lower->items[i]->type != ItemType::Equals) continue;
        if (i > 0 && lower->items[i - 1]->type == ItemType::Variable)
            return lower->items[i - 1]->variableName;
        break;
    }
    return 'x';
}

void collectRow(const Row* row, size_t begin, size_t end, Bound bound, FreeVariables& out);

void collectItem(const Item* it, Bound bound, FreeVariables& out) {
    switch (it->type) {
        case ItemType::Variable:
            if (it->variableName == 'x' && !bound.x) out.x = true;
            if (it->variableName == 'y' && !bound.y) out.y = true;
            return;
        case ItemType::Integral:
            // Without limits the evaluator answers with a symbolic antiderivative.
            if (rowIsEmpty(it->b.get()) && rowIsEmpty(it->c.get())) return;
            collectRow(it->a.get(), 0, kAll, bound.with(boundVariable(it)), out);
            collectRow(it->b.get(), 0, kAll, bound, out);
            collectRow(it->c.get(), 0, kAll, bound, out);
            return;
        case ItemType::Derivative:
            if (rowIsEmpty(it->b.get())) return;   // symbolic derivative
            collectRow(it->a.get(), 0, kAll, bound.with(boundVariable(it)), out);
            collectRow(it->b.get(), 0, kAll, bound, out);
            return;
        case ItemType::Summation:
        case ItemType::Product: {
            Bound inner = bound.with(indexVariable(it->b.get()));
            collectRow(it->a.get(), 0, kAll, inner, out);
            collectRow(it->b.get(), 0, kAll, inner, out);
            collectRow(it->c.get(), 0, kAll, bound, out);
            return;
        }
        case ItemType::Function:
            if (isClosedCalculusFunction(it->functionId)) {
                collectRow(it->a.get(), 0, kAll, bound.with(calculusVariable(it)), out);
                return;
            }
            break;
        default:
            break;
    }
    collectRow(it->a.get(), 0, kAll, bound, out);
    collectRow(it->b.get(), 0, kAll, bound, out);
    collectRow(it->c.get(), 0, kAll, bound, out);
    collectRow(it->d.get(), 0, kAll, bound, out);
}

void collectRow(const Row* row, size_t begin, size_t end, Bound bound, FreeVariables& out) {
    if (!row) return;
    end = std::min(end, row->items.size());
    for (size_t i = begin; i < end; ++i) collectItem(row->items[i].get(), bound, out);
}

// ------------------------------------------------------------- tree copies

std::string numberText(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", value);
    if (std::strtod(buf, nullptr) != value) std::snprintf(buf, sizeof(buf), "%.17g", value);
    return buf;
}

// A declared value as "(5)" or "(-2.5)": the parentheses keep "2x" -> "2(5)"
// and "x^2" -> "(-3)^2" meaning what they meant.
std::unique_ptr<Item> valueItem(double value, Row* parent) {
    auto paren = std::make_unique<Item>(ItemType::Paren);
    paren->a = std::make_unique<Row>();
    paren->a->owner = paren.get();
    paren->a->ownerParentRow = parent;
    if (value == 0.0) value = 0.0;   // no "-0"
    if (value < 0.0) {
        auto minus = std::make_unique<Item>(ItemType::Operator);
        minus->opChar = '-';
        paren->a->items.push_back(std::move(minus));
        value = -value;
    }
    auto number = std::make_unique<Item>(ItemType::Number);
    number->numText = numberText(value);
    paren->a->items.push_back(std::move(number));
    return paren;
}

std::unique_ptr<Row> copyRow(const Row* source, size_t begin, size_t end, Bound bound,
                             const EvaluationContext* values, Item* owner, Row* ownerParent);

std::unique_ptr<Item> copyItem(const Item* it, Bound bound, const EvaluationContext* values,
                               Row* parent) {
    if (values && it->type == ItemType::Variable) {
        if (it->variableName == 'x' && values->xDefined && !bound.x) return valueItem(values->x, parent);
        if (it->variableName == 'y' && values->yDefined && !bound.y) return valueItem(values->y, parent);
    }
    auto copy = std::make_unique<Item>(it->type);
    copy->numText = it->numText;
    copy->variableName = it->variableName;
    copy->opChar = it->opChar;
    copy->nameText = it->nameText;
    copy->constantName = it->constantName;
    copy->functionId = it->functionId;
    copy->isBracket = it->isBracket;
    copy->isBrace = it->isBrace;
    copy->isClosed = it->isClosed;

    // The body of an integral/derivative/sum binds its variable; limits and
    // evaluation points see the outer bindings. Calculus function forms keep
    // their arguments verbatim and read declared values from the context.
    Bound body = bound, lower = bound;
    const EvaluationContext* childValues = values;
    switch (it->type) {
        case ItemType::Integral:
        case ItemType::Derivative:
            body = bound.with(boundVariable(it));
            break;
        case ItemType::Summation:
        case ItemType::Product:
            body = lower = bound.with(indexVariable(it->b.get()));
            break;
        case ItemType::Function:
            if (isClosedCalculusFunction(it->functionId)) childValues = nullptr;
            break;
        default:
            break;
    }
    copy->a = copyRow(it->a.get(), 0, kAll, body, childValues, copy.get(), parent);
    copy->b = copyRow(it->b.get(), 0, kAll, lower, childValues, copy.get(), parent);
    copy->c = copyRow(it->c.get(), 0, kAll, bound, childValues, copy.get(), parent);
    copy->d = copyRow(it->d.get(), 0, kAll, bound, childValues, copy.get(), parent);
    return copy;
}

std::unique_ptr<Row> copyRow(const Row* source, size_t begin, size_t end, Bound bound,
                             const EvaluationContext* values, Item* owner, Row* ownerParent) {
    if (!source) return nullptr;
    auto row = std::make_unique<Row>();
    row->owner = owner;
    row->ownerParentRow = ownerParent;
    end = std::min(end, source->items.size());
    for (size_t i = begin; i < end; ++i)
        row->items.push_back(copyItem(source->items[i].get(), bound, values, row.get()));
    return row;
}

void renameIn(Row* row, char from, char to) {
    if (!row) return;
    for (auto& it : row->items) {
        if (it->type == ItemType::Variable && it->variableName == from) it->variableName = to;
        renameIn(it->a.get(), from, to);
        renameIn(it->b.get(), from, to);
        renameIn(it->c.get(), from, to);
        renameIn(it->d.get(), from, to);
    }
}

// ------------------------------------------------------- number formatting

bool nearInteger(double v) { return std::fabs(v - std::round(v)) < 1e-9 * std::max(1.0, std::fabs(v)); }
double snap(double v) { return nearInteger(v) ? std::round(v) : v; }

bool toRational(double v, long long& p, long long& q) {
    double a = std::fabs(v);
    long long h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    double x = a;
    for (int i = 0; i < 24; ++i) {
        double whole = std::floor(x);
        if (whole > 1e12) break;
        long long ai = (long long)whole;
        long long h2 = ai * h1 + h0, k2 = ai * k1 + k0;
        if (k2 > 1000) break;
        h0 = h1; h1 = h2; k0 = k1; k1 = k2;
        if (std::fabs(a - (double)h1 / (double)k1) < 1e-10 * std::max(1.0, a)) {
            p = v < 0 ? -h1 : h1;
            q = k1;
            return true;
        }
        double frac = x - whole;
        if (frac < 1e-15) break;
        x = 1.0 / frac;
    }
    return false;
}

// Short decimals stay decimals ("0.5x"); other ratios become fractions
// ("x/3"), and everything else falls back to ten significant digits.
bool shortDecimal(double v) {
    for (int decimals = 0; decimals <= 2; ++decimals) {
        double scaled = v * std::pow(10.0, decimals);
        if (nearInteger(scaled)) return true;
    }
    return false;
}

std::string formatNumber(double v) {
    std::string sign = v < 0 ? kMinus : "";
    double a = std::fabs(v);
    if (nearInteger(a) && a < 1e15) return sign + std::to_string((long long)std::llround(a));
    char buf[64];
    if (shortDecimal(a)) {
        std::snprintf(buf, sizeof(buf), "%.2f", a);
        std::string s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return sign + s;
    }
    long long p = 0, q = 1;
    if (toRational(a, p, q)) return sign + std::to_string(p) + "/" + std::to_string(q);
    std::snprintf(buf, sizeof(buf), "%.10g", a);
    return sign + buf;
}

std::string superscriptInteger(long long n) {
    static const char* digits[] = {
        "\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4",
        "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9" };
    std::string out = n < 0 ? "\xE2\x81\xBB" : "";
    std::string plain = std::to_string(n < 0 ? -n : n);
    for (char c : plain) out += digits[c - '0'];
    return out;
}

// ------------------------------------------------------------- polynomials

using Monomial = std::map<std::string, double>;   // atom -> exponent

struct Poly {
    std::map<Monomial, double> terms;              // monomial -> coefficient
};

struct AtomInfo {
    bool hasX = false;
    bool hasY = false;
    bool grouped = true;      // an exponent can follow without parentheses
    bool numeric = false;     // pi, e, phi
    double value = 0.0;
};

void addTerm(Poly& p, const Monomial& m, double c) {
    if (c == 0.0) return;
    auto it = p.terms.find(m);
    if (it == p.terms.end()) {
        p.terms.emplace(m, c);
        return;
    }
    double sum = it->second + c;
    if (std::fabs(sum) <= 1e-12 * (std::fabs(it->second) + std::fabs(c))) p.terms.erase(it);
    else it->second = sum;
}

Monomial multiplyMonomials(const Monomial& a, const Monomial& b) {
    Monomial r = a;
    for (const auto& [atom, exponent] : b) {
        double sum = snap(r[atom] + exponent);
        if (sum == 0.0) r.erase(atom);
        else r[atom] = sum;
    }
    return r;
}

bool isSymbolAtom(const std::string& key) {
    return key == "x" || key == "y" || key == "e" || key == kPi || key == kPhi;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// A single symbol, optionally followed by a superscript exponent.
bool isSymbolPower(const std::string& s) {
    for (const char* symbol : { "x", "y", "e", "\xCF\x80", "\xCF\x86" }) {
        std::string sym = symbol;
        if (!startsWith(s, sym)) continue;
        std::string rest = s.substr(sym.size());
        for (unsigned char c : rest)
            if (c < 0x80) return false;      // superscripts are multi-byte
        return true;
    }
    return false;
}

class Builder {
public:
    Builder(const EvaluationContext& context, bool expand) : context_(context), expand_(expand) {
        atoms_["x"] = { true, false, true, false, 0.0 };
        atoms_["y"] = { false, true, true, false, 0.0 };
        atoms_[kPi] = { false, false, true, true, 3.14159265358979323846 };
        atoms_["e"] = { false, false, true, true, 2.71828182845904523536 };
        atoms_[kPhi] = { false, false, true, true, 1.61803398874989484820 };
    }

    bool ok() const { return ok_; }

    Poly range(const Row* row, size_t begin, size_t end) {
        if (!row) { fail(); return {}; }
        end = std::min(end, row->items.size());
        size_t pos = begin;
        Poly value = parseSum(row, pos, end);
        if (pos != end) fail();
        return value;
    }

    Poly difference(const Poly& a, const Poly& b) { return add(a, b, -1.0); }

    static bool pureConstant(const Poly& p, double& value) {
        if (p.terms.empty()) { value = 0.0; return true; }
        if (p.terms.size() == 1 && p.terms.begin()->first.empty()) {
            value = p.terms.begin()->second;
            return true;
        }
        return false;
    }

    std::string format(const Poly& p) const {
        if (p.terms.empty()) return "0";
        std::vector<std::pair<Monomial, double>> terms(p.terms.begin(), p.terms.end());
        std::stable_sort(terms.begin(), terms.end(),
                         [&](const auto& a, const auto& b) { return displayBefore(a.first, b.first); });

        // "9 − 2x" and "tan(x) − x" read better than "−2x + 9" and
        // "−x + tan(x)": when every power of x and y is negative, lead with
        // the first positive term that has none (a constant or a function).
        int positiveTail = -1;
        bool positivePower = false;
        for (size_t i = 0; i < terms.size(); ++i) {
            bool tail = degreeOf(terms[i].first, "x") == 0.0 && degreeOf(terms[i].first, "y") == 0.0;
            if (!tail) positivePower = positivePower || terms[i].second > 0;
            else if (terms[i].second > 0 && positiveTail < 0) positiveTail = (int)i;
        }
        if (terms[0].second < 0 && !positivePower && positiveTail > 0)
            std::rotate(terms.begin(), terms.begin() + positiveTail, terms.begin() + positiveTail + 1);

        std::string out;
        for (size_t i = 0; i < terms.size(); ++i) {
            double c = terms[i].second;
            std::string body = monomialText(std::fabs(c), terms[i].first);
            if (i == 0) out += (c < 0 ? kMinus : "") + body;
            else out += (c < 0 ? " " + kMinus + " " : std::string(" + ")) + body;
        }
        return out;
    }

    // "y = ..." when `var` occurs to exactly `degree`-th power at most and
    // never inside an opaque atom.
    bool isolate(const Poly& p, char var, int maxDegree, std::string& out) {
        std::string v(1, var);
        if (trapped(p, var) || !contains(p, var)) return false;
        Poly c[3];
        for (const auto& [m, coefficient] : p.terms) {
            Monomial rest = m;
            double degree = 0.0;
            auto it = rest.find(v);
            if (it != rest.end()) { degree = it->second; rest.erase(it); }
            if (degree != 0.0 && degree != 1.0 && degree != 2.0) return false;
            addTerm(c[(int)degree], rest, coefficient);
        }
        int degree = c[2].terms.empty() ? 1 : 2;
        if (degree > maxDegree) return false;
        std::string lhs = v + " = ";

        if (degree == 1) {
            Poly rhs = divide(scale(c[0], -1.0), c[1]);
            if (!ok_) return false;
            out = lhs + format(rhs);
            return true;
        }

        if (c[1].terms.empty()) {                       // a·v² + c = 0
            Poly square = divide(scale(c[0], -1.0), c[2]);
            if (!ok_) return false;
            double value = 0.0;
            if (pureConstant(square, value)) {
                if (value < 0) out = "No real solution";
                else if (value == 0) out = lhs + "0";
                else out = lhs + kPlusMinus + rootText(value);
                return true;
            }
            Poly root;
            if (perfectSquare(square, root)) out = lhs + kPlusMinus + format(root);
            else out = lhs + kPlusMinus + sqrtText(square);
            return true;
        }

        // Quadratic formula, expanding the discriminant so it can cancel.
        bool savedExpand = expand_;
        expand_ = true;
        Poly disc = add(multiply(c[1], c[1]), multiply(c[2], c[0]), -4.0);
        expand_ = savedExpand;
        Poly minusB = scale(c[1], -1.0);
        Poly twoA = scale(c[2], 2.0);
        if (!ok_) return false;
        double d = 0.0, a2 = 0.0;
        bool constantA = pureConstant(twoA, a2);
        if (pureConstant(disc, d)) {
            if (d < 0) { out = "No real solution"; return true; }
            if (constantA) {
                Poly center = scale(minusB, 1.0 / a2);
                std::string spread = formatNumber(std::sqrt(d) / std::fabs(a2));
                out = lhs + (center.terms.empty() ? kPlusMinus + spread
                                                  : format(center) + " " + kPlusMinus + " " + spread);
                return true;
            }
        }
        std::string numerator = minusB.terms.empty() ? kPlusMinus + sqrtText(disc)
                                                     : format(minusB) + " " + kPlusMinus + " " + sqrtText(disc);
        if (constantA && a2 == 1.0) out = lhs + numerator;
        else out = lhs + "(" + numerator + ")/" + denominatorText(twoA);
        return true;
    }

private:
    const EvaluationContext& context_;
    bool expand_ = false;
    bool ok_ = true;
    std::map<std::string, AtomInfo> atoms_;

    void fail() { ok_ = false; }

    static bool isOperator(const Item* it, char op) {
        return it->type == ItemType::Operator && it->opChar == op;
    }

    // ---- parsing (same grammar as the evaluator's RowParser)

    Poly parseSum(const Row* row, size_t& pos, size_t end) {
        if (pos >= end) { fail(); return {}; }
        Poly value = parseTerm(row, pos, end);
        while (ok_ && pos < end) {
            const Item* it = row->items[pos].get();
            if (isOperator(it, '+')) { ++pos; value = add(value, parseTerm(row, pos, end), 1.0); }
            else if (isOperator(it, '-')) { ++pos; value = add(value, parseTerm(row, pos, end), -1.0); }
            else { fail(); break; }
        }
        return value;
    }

    Poly parseTerm(const Row* row, size_t& pos, size_t end) {
        Poly value = parseFactor(row, pos, end);
        while (ok_ && pos < end) {
            const Item* it = row->items[pos].get();
            if (isOperator(it, '*')) {
                ++pos;
                value = multiply(value, parseFactor(row, pos, end));
            } else if (isOperator(it, '%')) {
                ++pos;
                Poly divisor = parseFactor(row, pos, end);
                double a = 0.0, b = 0.0;
                if (pureConstant(value, a) && pureConstant(divisor, b) && b != 0.0) value = constant(std::fmod(a, b));
                else fail();
            } else if (it->type != ItemType::Operator && it->type != ItemType::Equals &&
                       it->type != ItemType::CloseParen) {
                value = multiply(value, parseFactor(row, pos, end));
            } else {
                break;
            }
        }
        return value;
    }

    Poly parseFactor(const Row* row, size_t& pos, size_t end) {
        bool negative = false;
        while (pos < end && (isOperator(row->items[pos].get(), '-') || isOperator(row->items[pos].get(), '+'))) {
            if (isOperator(row->items[pos].get(), '-')) negative = !negative;
            ++pos;
        }
        Poly value = parseAtom(row, pos, end);
        while (ok_ && pos < end && isOperator(row->items[pos].get(), '!')) {
            ++pos;
            value = factorial(value);
        }
        return negative ? scale(value, -1.0) : value;
    }

    Poly parseAtom(const Row* row, size_t& pos, size_t end) {
        if (pos >= end) { fail(); return {}; }
        const Item* it = row->items[pos++].get();
        switch (it->type) {
            case ItemType::Number: {
                std::string text = it->numText;
                while (pos < end && row->items[pos]->type == ItemType::Number) text += row->items[pos++]->numText;
                char* stop = nullptr;
                double value = std::strtod(text.c_str(), &stop);
                if (text.empty() || text == "." || !stop || *stop) { fail(); return {}; }
                return constant(value);
            }
            case ItemType::Variable:
                if (it->variableName == 'x') return atom("x");
                if (it->variableName == 'y') return atom("y");
                fail();
                return {};
            case ItemType::Constant:
                return atom(it->constantName == 'p' ? kPi : it->constantName == 'f' ? kPhi : std::string("e"));
            case ItemType::Paren:
                if (it->isBrace || it->isBracket) { fail(); return {}; }
                return range(it->a.get(), 0, kAll);
            case ItemType::Fraction:
                return divide(range(it->a.get(), 0, kAll), range(it->b.get(), 0, kAll));
            case ItemType::Power:
                return power(range(it->a.get(), 0, kAll), range(it->b.get(), 0, kAll));
            case ItemType::Sqrt:
                return power(range(it->a.get(), 0, kAll), constant(0.5));
            case ItemType::Function:
            case ItemType::Permutation:
            case ItemType::Combination:
            case ItemType::Integral:
            case ItemType::Derivative:
            case ItemType::Summation:
            case ItemType::Product:
                return closedItem(it);
            default:
                fail();
                return {};
        }
    }

    // Items without unknowns are folded to a number by the real evaluator;
    // a function of the unknowns becomes an opaque atom such as "sin(2x)".
    Poly closedItem(const Item* it) {
        // Calculus function forms keep declared variables unsubstituted (the
        // context supplies them), so discount declarations here.
        FreeVariables unknown;
        collectItem(it, Bound{}, unknown);
        if (context_.xDefined) unknown.x = false;
        if (context_.yDefined) unknown.y = false;
        if (!unknown.any()) {
            auto row = std::make_unique<Row>();
            row->items.push_back(copyItem(it, Bound{}, nullptr, row.get()));
            try {
                double value = evaluate(row.get(), context_);
                if (std::isfinite(value)) return constant(value);
            } catch (const std::exception&) {
            }
            fail();
            return {};
        }
        if (it->type != ItemType::Function || !it->a) { fail(); return {}; }

        std::vector<std::string> args;
        bool hasX = false, hasY = false;
        const Row* argRow = it->a.get();
        size_t start = 0;
        for (size_t i = 0; i <= argRow->items.size(); ++i) {
            if (i < argRow->items.size() && !isOperator(argRow->items[i].get(), ',')) continue;
            Poly arg = range(argRow, start, i);
            if (!ok_) return {};
            args.push_back(format(arg));
            hasX = hasX || contains(arg, 'x');
            hasY = hasY || contains(arg, 'y');
            start = i + 1;
        }
        return opaque(functionText(it->functionId, args), hasX, hasY, true);
    }

    static std::string functionText(int id, const std::vector<std::string>& args) {
        std::string joined;
        for (size_t i = 0; i < args.size(); ++i) joined += (i ? ", " : "") + args[i];
        auto call = [&](const std::string& name) { return name + "(" + joined + ")"; };
        switch (id) {
            case SciAbs: return "|" + joined + "|";
            case SciAsin: case SciAcos: case SciAtan: case SciAsec: case SciAcsc: case SciAcot:
            case SciAsinh: case SciAcosh: case SciAtanh: case SciAsech: case SciAcsch: case SciAcoth:
                return call(std::string(sciFunctionName(id) + 1) + kInverse);   // asin -> sin⁻¹
            case SciLog2: return call("log\xE2\x82\x82");
            case SciGamma: return call("\xCE\x93");
            case SciZeta: return call("\xCE\xB6");
            case SciLog1p: return "ln(1 + " + joined + ")";
            case SciExp: return isSymbolPower(joined) ? "e^" + joined : "e^(" + joined + ")";
            default: return call(sciFunctionName(id));
        }
    }

    // ---- arithmetic

    static Poly constant(double value) {
        Poly p;
        addTerm(p, {}, value);
        return p;
    }

    static Poly atom(const std::string& key) {
        Poly p;
        p.terms[{ { key, 1.0 } }] = 1.0;
        return p;
    }

    Poly opaque(const std::string& text, bool hasX, bool hasY, bool grouped) {
        if (!atoms_.count(text)) atoms_[text] = { hasX, hasY, grouped, false, 0.0 };
        return atom(text);
    }

    Poly add(const Poly& a, const Poly& b, double sign) {
        Poly r = a;
        for (const auto& [m, c] : b.terms) addTerm(r, m, sign * c);
        return r;
    }

    Poly scale(const Poly& a, double factor) {
        if (!std::isfinite(factor)) { fail(); return {}; }
        Poly r;
        for (const auto& [m, c] : a.terms) addTerm(r, m, c * factor);
        return r;
    }

    // A sum treated as one factor: "(x + 1)".
    Poly group(const Poly& p) {
        if (p.terms.size() <= 1) return p;
        return opaque("(" + format(p) + ")", contains(p, 'x'), contains(p, 'y'), true);
    }

    Poly multiply(const Poly& a, const Poly& b) {
        if (!ok_) return {};
        // Numbers and single terms distribute; two sums stay factored
        // ("(x + 1)(x − 1)") unless expanding for the solver.
        if (!expand_ && a.terms.size() > 1 && b.terms.size() > 1) return multiply(group(a), group(b));
        if (a.terms.size() * b.terms.size() > kMaxTerms) { fail(); return {}; }
        Poly r;
        for (const auto& [ma, ca] : a.terms)
            for (const auto& [mb, cb] : b.terms) addTerm(r, multiplyMonomials(ma, mb), ca * cb);
        return r;
    }

    Poly divide(const Poly& n, const Poly& d) {
        if (!ok_) return {};
        if (d.terms.empty()) { fail(); return {}; }          // division by zero
        if (d.terms.size() == 1) {
            const auto& [m, c] = *d.terms.begin();
            Monomial inverse;
            for (const auto& [key, exponent] : m) inverse[key] = -exponent;
            Poly r;
            for (const auto& [mn, cn] : n.terms) addTerm(r, multiplyMonomials(mn, inverse), cn / c);
            return r;
        }
        if (n.terms.size() > 1 && !expand_) {
            return opaque("(" + format(n) + ")/(" + format(d) + ")",
                          contains(n, 'x') || contains(d, 'x'), contains(n, 'y') || contains(d, 'y'), false);
        }
        Poly den = group(d);
        return divide(n, den);
    }

    Poly power(const Poly& base, const Poly& exponent) {
        if (!ok_) return {};
        double e = 0.0;
        if (!pureConstant(exponent, e)) {
            double bv = 0.0, ev = 0.0;
            if (numericValue(base, bv) && numericValue(exponent, ev)) return checked(std::pow(bv, ev));
            return opaque(baseText(base) + "^" + exponentText(exponent),
                          contains(base, 'x') || contains(exponent, 'x'),
                          contains(base, 'y') || contains(exponent, 'y'), false);
        }
        if (base.terms.empty()) {
            if (e > 0) return {};
            if (e == 0) return constant(1.0);
            fail();
            return {};
        }
        if (e == 0) return constant(1.0);
        double b = 0.0;
        if (pureConstant(base, b)) return checked(std::pow(b, e));
        if (base.terms.size() == 1) {
            const auto& [m, c] = *base.terms.begin();
            bool plain = true;
            for (const auto& entry : m) plain = plain && entry.second == 1.0;
            if (nearInteger(e) || (c > 0 && plain)) {
                Monomial raised;
                for (const auto& [key, exponentOfAtom] : m) raised[key] = snap(exponentOfAtom * e);
                double coefficient = std::pow(c, e);
                if (!std::isfinite(coefficient)) { fail(); return {}; }
                Poly r;
                addTerm(r, raised, coefficient);
                return r;
            }
            // (x²)^0.5 is |x|, not x: keep it as written.
            return power(opaque("(" + format(base) + ")", contains(base, 'x'), contains(base, 'y'), true), exponent);
        }
        if (expand_ && nearInteger(e) && e > 0 && e <= 8) {
            Poly r = base;
            for (int i = 1; i < (int)std::lround(e) && ok_; ++i) r = multiply(r, base);
            return r;
        }
        return power(group(base), exponent);
    }

    Poly factorial(const Poly& value) {
        double n = 0.0;
        if (pureConstant(value, n)) {
            if (n < 0 || !nearInteger(n) || n > 170) { fail(); return {}; }
            return constant(std::tgamma(std::round(n) + 1.0));
        }
        return opaque(baseText(value) + "!", contains(value, 'x'), contains(value, 'y'), true);
    }

    Poly checked(double value) {
        if (!std::isfinite(value)) { fail(); return {}; }
        return constant(value);
    }

    bool numericValue(const Poly& p, double& value) const {
        value = 0.0;
        for (const auto& [m, c] : p.terms) {
            double term = c;
            for (const auto& [key, exponent] : m) {
                const AtomInfo& info = atoms_.at(key);
                if (!info.numeric) return false;
                term *= std::pow(info.value, exponent);
            }
            value += term;
        }
        return true;
    }

    bool contains(const Poly& p, char var) const {
        for (const auto& term : p.terms)
            for (const auto& entry : term.first) {
                const AtomInfo& info = atoms_.at(entry.first);
                if (var == 'x' ? info.hasX : info.hasY) return true;
            }
        return false;
    }

    // `var` hidden inside an opaque atom such as "sin(y)" or "(x + y)".
    bool trapped(const Poly& p, char var) const {
        for (const auto& term : p.terms)
            for (const auto& entry : term.first) {
                if (entry.first == "x" || entry.first == "y") continue;
                const AtomInfo& info = atoms_.at(entry.first);
                if (var == 'x' ? info.hasX : info.hasY) return true;
            }
        return false;
    }

    // ---- formatting

    bool isConstantMonomial(const Monomial& m) const {
        for (const auto& entry : m)
            if (!atoms_.at(entry.first).numeric) return false;
        return true;
    }

    static double degreeOf(const Monomial& m, const std::string& key) {
        auto it = m.find(key);
        return it == m.end() ? 0.0 : it->second;
    }

    bool displayBefore(const Monomial& a, const Monomial& b) const {
        bool ca = isConstantMonomial(a), cb = isConstantMonomial(b);
        if (ca != cb) return !ca;                                  // constants last
        double da = degreeOf(a, "x") + degreeOf(a, "y"), db = degreeOf(b, "x") + degreeOf(b, "y");
        if (da != db) return da > db;                              // x² before x
        if (degreeOf(a, "x") != degreeOf(b, "x")) return degreeOf(a, "x") > degreeOf(b, "x");
        auto opaqueCount = [&](const Monomial& m) {
            int n = 0;
            for (const auto& entry : m) n += isSymbolAtom(entry.first) ? 0 : 1;
            return n;
        };
        return opaqueCount(a) < opaqueCount(b);
    }

    static int atomRank(const std::string& key) {
        if (key == kPi) return 0;
        if (key == "e") return 1;
        if (key == kPhi) return 2;
        if (key == "x") return 3;
        if (key == "y") return 4;
        return 5;
    }

    std::string atomPowerText(const std::string& key, double exponent) const {
        bool grouped = atoms_.at(key).grouped;
        std::string wrapped = grouped ? key : "(" + key + ")";
        if (exponent == 1.0) return key;
        if (exponent == 0.5)
            return kSqrt + (isSymbolAtom(key) || startsWith(key, "(") ? key : "(" + key + ")");
        if (nearInteger(exponent)) return wrapped + superscriptInteger(std::llround(exponent));
        long long p = 0, q = 1;
        std::string power = toRational(exponent, p, q) ? std::to_string(p) + "/" + std::to_string(q)
                                                       : formatNumber(exponent);
        return wrapped + "^(" + power + ")";
    }

    static std::string joinFactors(const std::vector<std::string>& parts) {
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                const std::string& prev = parts[i - 1];
                const std::string& next = parts[i];
                bool juxtapose = (isSymbolPower(next) || startsWith(next, "(") || startsWith(next, kSqrt)) &&
                                 prev.find('^') == std::string::npos && prev.back() != '!';
                if (!juxtapose) out += kDot;
            }
            out += parts[i];
        }
        return out;
    }

    std::string monomialText(double magnitude, const Monomial& m) const {
        std::vector<std::pair<std::string, double>> atoms(m.begin(), m.end());
        std::stable_sort(atoms.begin(), atoms.end(), [](const auto& a, const auto& b) {
            int ra = atomRank(a.first), rb = atomRank(b.first);
            return ra != rb ? ra < rb : a.first < b.first;
        });
        std::vector<std::string> numer, denom;
        for (const auto& [key, exponent] : atoms) {
            if (exponent > 0) numer.push_back(atomPowerText(key, exponent));
            else denom.push_back(atomPowerText(key, -exponent));
        }

        std::string numText, denText;
        long long p = 0, q = 1;
        if (!shortDecimal(magnitude) && toRational(magnitude, p, q) && q > 1) {
            if (p != 1 || numer.empty()) numText = std::to_string(p);
            denText = std::to_string(q);
        } else if (magnitude != 1.0 || numer.empty()) {
            numText = formatNumber(magnitude);
        }

        std::string factors = joinFactors(numer);
        std::string top = numText;
        if (!factors.empty()) {
            if (!top.empty() && !(isSymbolPower(numer[0]) || startsWith(numer[0], "(") ||
                                  startsWith(numer[0], kSqrt) || std::isalpha((unsigned char)numer[0][0]) ||
                                  (unsigned char)numer[0][0] >= 0x80))
                top += kDot;
            top += factors;
        }
        if (top.empty()) top = "1";

        size_t denCount = (denText.empty() ? 0 : 1) + denom.size();
        if (denCount == 0) return top;
        std::string bottom = denText + joinFactors(denom);
        bool simple = denCount == 1 && (denom.empty() || isSymbolPower(denom[0]) ||
                                        (startsWith(denom[0], "(") && denom[0].find('/') == std::string::npos) ||
                                        denom[0].find(' ') == std::string::npos);
        if (!simple) bottom = "(" + bottom + ")";
        return top + "/" + bottom;
    }

    std::string baseText(const Poly& p) const {
        double value = 0.0;
        if (pureConstant(p, value) && value >= 0) return formatNumber(value);
        std::string text = format(p);
        if (p.terms.size() == 1 && p.terms.begin()->second == 1.0 && p.terms.begin()->first.size() == 1 &&
            p.terms.begin()->first.begin()->second == 1.0 && atoms_.at(p.terms.begin()->first.begin()->first).grouped)
            return text;
        return "(" + text + ")";
    }

    std::string exponentText(const Poly& p) const {
        std::string text = format(p);
        if (isSymbolPower(text) || text.find_first_not_of("0123456789") == std::string::npos) return text;
        return "(" + text + ")";
    }

    std::string sqrtText(const Poly& p) const {
        std::string text = format(p);
        return kSqrt + (isSymbolPower(text) ? text : "(" + text + ")");
    }

    static std::string rootText(double value) {
        double root = std::sqrt(value);
        if (nearInteger(root)) return formatNumber(root);
        if (nearInteger(value)) return kSqrt + formatNumber(value);
        return formatNumber(root);
    }

    std::string denominatorText(const Poly& p) const {
        std::string text = format(p);
        return text.find(' ') == std::string::npos ? text : "(" + text + ")";
    }

    // r = k·m² with k a perfect square and every exponent even.
    bool perfectSquare(const Poly& r, Poly& root) const {
        if (r.terms.size() != 1) return false;
        const auto& [m, c] = *r.terms.begin();
        if (c <= 0) return false;
        double k = std::sqrt(c);
        if (!shortDecimal(k)) return false;
        Monomial half;
        for (const auto& [key, exponent] : m) {
            if (!nearInteger(exponent / 2.0)) return false;
            half[key] = exponent / 2.0;
        }
        root = Poly{};
        addTerm(root, half, k);
        return true;
    }
};

size_t singleEquals(const Row* row) {
    size_t found = kAll;
    for (size_t i = 0; i < row->items.size(); ++i) {
        if (row->items[i]->type != ItemType::Equals) continue;
        if (found != kAll) return kAll;
        found = i;
    }
    return found;
}

} // namespace

FreeVariables collectFreeVariables(const Row* row, size_t begin, size_t end) {
    FreeVariables out;
    collectRow(row, begin, end, Bound{}, out);
    return out;
}

FreeVariables unknownVariables(const Row* row, const EvaluationContext& context, size_t begin, size_t end) {
    FreeVariables out = collectFreeVariables(row, begin, end);
    if (context.xDefined) out.x = false;
    if (context.yDefined) out.y = false;
    return out;
}

std::unique_ptr<Row> substituteDeclared(const Row* row, const EvaluationContext& context) {
    return copyRow(row, 0, kAll, Bound{}, &context, nullptr, nullptr);
}

std::unique_ptr<Row> renameVariable(const Row* row, char from, char to) {
    auto copy = copyRow(row, 0, kAll, Bound{}, nullptr, nullptr, nullptr);
    renameIn(copy.get(), from, to);
    return copy;
}

bool evaluateRange(const Row* row, size_t begin, size_t end, const EvaluationContext& context,
                   double& value, std::string& error) {
    auto part = copyRow(row, begin, end, Bound{}, nullptr, nullptr, nullptr);
    if (!part || part->items.empty()) {
        error = "Incomplete equation";
        return false;
    }
    try {
        value = evaluate(part.get(), context);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool simplifyExpression(const Row* row, const EvaluationContext& context, std::string& out) {
    if (!row) return false;
    Builder builder(context, false);
    Poly p = builder.range(row, 0, kAll);
    if (!builder.ok()) return false;
    out = builder.format(p);
    return true;
}

bool solveForOneVariable(const Row* equation, const EvaluationContext& context, std::string& out) {
    if (!equation) return false;
    size_t eq = singleEquals(equation);
    size_t n = equation->items.size();
    if (eq == kAll || eq == 0 || eq + 1 >= n) return false;

    // Already explicit ("y = 2x + 1", "x = y²"): just simplify the other side.
    for (int side = 0; side < 2; ++side) {
        size_t lb = side == 0 ? 0 : eq + 1, le = side == 0 ? eq : n;
        size_t ob = side == 0 ? eq + 1 : 0, oe = side == 0 ? n : eq;
        if (le - lb != 1 || equation->items[lb]->type != ItemType::Variable) continue;
        char v = equation->items[lb]->variableName;
        if (v != 'x' && v != 'y') continue;
        FreeVariables other = collectFreeVariables(equation, ob, oe);
        if ((v == 'x' && other.x) || (v == 'y' && other.y)) continue;
        Builder builder(context, false);
        Poly rhs = builder.range(equation, ob, oe);
        out = std::string(1, v) + " = " +
              (builder.ok() ? builder.format(rhs) : rowRangeToPlainString(equation, (int)ob, (int)oe));
        return true;
    }

    std::string relation;
    for (bool expand : { false, true }) {
        Builder builder(context, expand);
        Poly left = builder.range(equation, 0, eq);
        Poly right = builder.range(equation, eq + 1, n);
        if (!builder.ok()) {
            if (relation.empty()) return false;
            out = relation;
            return true;
        }
        if (!expand) relation = builder.format(left) + " = " + builder.format(right);
        Poly p = builder.difference(left, right);
        double c = 0.0;
        if (Builder::pureConstant(p, c)) {
            out = c == 0.0 ? "Every value is a solution" : "No solution";
            return true;
        }
        static const std::pair<char, int> order[] = { { 'y', 1 }, { 'x', 1 }, { 'y', 2 }, { 'x', 2 } };
        for (const auto& [var, degree] : order)
            if (builder.isolate(p, var, degree, out)) return true;
    }
    out = relation;
    return true;
}
