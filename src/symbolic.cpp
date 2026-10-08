// symbolic.cpp
//
// Symbolic & numerical calculus engine for NaturalCalculator.
// Parses Row expressions into symbolic AST, performs symbolic integration (with "+ C"),
// symbolic differentiation, and definite integral evaluations.

#include "symbolic.h"
#include <vector>
#include <memory>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace {

enum class SymType {
    Constant,
    Variable,
    Add,
    Sub,
    Mul,
    Div,
    Pow,
    Sin,
    Cos,
    Tan,
    Exp,
    Ln,
    Sqrt,
    Abs
};

struct SymNode {
    SymType type;
    double val = 0.0;
    char var = 'x';
    std::shared_ptr<SymNode> left;
    std::shared_ptr<SymNode> right;

    SymNode(double v) : type(SymType::Constant), val(v) {}
    SymNode(char v) : type(SymType::Variable), var(v) {}
    SymNode(SymType t, std::shared_ptr<SymNode> l, std::shared_ptr<SymNode> r = nullptr)
        : type(t), left(l), right(r) {}

    bool isConst() const { return type == SymType::Constant; }
    bool isConstVal(double v) const { return isConst() && std::fabs(val - v) < 1e-9; }
    bool isVar(char v) const { return type == SymType::Variable && var == v; }

    bool dependsOn(char v) const {
        if (type == SymType::Constant) return false;
        if (type == SymType::Variable) return var == v;
        bool dep = false;
        if (left) dep = dep || left->dependsOn(v);
        if (right) dep = dep || right->dependsOn(v);
        return dep;
    }
};

using SymPtr = std::shared_ptr<SymNode>;

SymPtr num(double v) { return std::make_shared<SymNode>(v); }
SymPtr varNode(char v) { return std::make_shared<SymNode>(v); }

SymPtr add(SymPtr l, SymPtr r);
SymPtr sub(SymPtr l, SymPtr r);
SymPtr mul(SymPtr l, SymPtr r);
SymPtr div(SymPtr l, SymPtr r);
SymPtr pwr(SymPtr l, SymPtr r);
SymPtr fnSin(SymPtr l) { return std::make_shared<SymNode>(SymType::Sin, l); }
SymPtr fnCos(SymPtr l) { return std::make_shared<SymNode>(SymType::Cos, l); }
SymPtr fnTan(SymPtr l) { return std::make_shared<SymNode>(SymType::Tan, l); }
SymPtr fnExp(SymPtr l) { return std::make_shared<SymNode>(SymType::Exp, l); }
SymPtr fnLn(SymPtr l) { return std::make_shared<SymNode>(SymType::Ln, l); }
SymPtr fnSqrt(SymPtr l) { return std::make_shared<SymNode>(SymType::Sqrt, l); }

SymPtr add(SymPtr l, SymPtr r) {
    if (l->isConstVal(0.0)) return r;
    if (r->isConstVal(0.0)) return l;
    if (l->isConst() && r->isConst()) return num(l->val + r->val);
    return std::make_shared<SymNode>(SymType::Add, l, r);
}

SymPtr sub(SymPtr l, SymPtr r) {
    if (r->isConstVal(0.0)) return l;
    if (l->isConst() && r->isConst()) return num(l->val - r->val);
    return std::make_shared<SymNode>(SymType::Sub, l, r);
}

SymPtr mul(SymPtr l, SymPtr r) {
    if (l->isConstVal(0.0) || r->isConstVal(0.0)) return num(0.0);
    if (l->isConstVal(1.0)) return r;
    if (r->isConstVal(1.0)) return l;
    if (l->isConst() && r->isConst()) return num(l->val * r->val);
    // Combine nested constants: c1 * (c2 * x) -> (c1 * c2) * x
    if (l->isConst() && r->type == SymType::Mul && r->left->isConst()) {
        return mul(num(l->val * r->left->val), r->right);
    }
    // c1 * (x / c2) -> (c1 / c2) * x
    if (l->isConst() && r->type == SymType::Div && r->right->isConst()) {
        double factor = l->val / r->right->val;
        if (factor == 1.0) return r->left;
        return mul(num(factor), r->left);
    }
    return std::make_shared<SymNode>(SymType::Mul, l, r);
}

SymPtr div(SymPtr l, SymPtr r) {
    if (l->isConstVal(0.0)) return num(0.0);
    if (r->isConstVal(1.0)) return l;
    if (l->isConst() && r->isConst()) {
        double d = l->val / r->val;
        if (std::floor(d) == d) return num(d);
    }
    // (c1 * x) / c2 -> (c1 / c2) * x
    if (l->type == SymType::Mul && l->left->isConst() && r->isConst()) {
        double factor = l->left->val / r->val;
        if (factor == 1.0) return l->right;
        return mul(num(factor), l->right);
    }
    return std::make_shared<SymNode>(SymType::Div, l, r);
}

SymPtr pwr(SymPtr l, SymPtr r) {
    if (r->isConstVal(0.0)) return num(1.0);
    if (r->isConstVal(1.0)) return l;
    if (l->isConstVal(0.0)) return num(0.0);
    if (l->isConstVal(1.0)) return num(1.0);
    if (l->isConst() && r->isConst()) return num(std::pow(l->val, r->val));
    return std::make_shared<SymNode>(SymType::Pow, l, r);
}

// Format double to clean integer or short float string
std::string formatDouble(double v) {
    if (std::fabs(v) < 1e-12) return "0";
    if (std::floor(v) == v && std::fabs(v) < 1e12) {
        return std::to_string((long long)v);
    }
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

std::string formatSymNode(SymPtr n) {
    if (!n) return "";
    switch (n->type) {
        case SymType::Constant:
            return formatDouble(n->val);
        case SymType::Variable:
            return std::string(1, n->var);
        case SymType::Add:
            return formatSymNode(n->left) + " + " + formatSymNode(n->right);
        case SymType::Sub:
            return formatSymNode(n->left) + " - " + formatSymNode(n->right);
        case SymType::Mul: {
            if (n->left->isConst()) {
                double c = n->left->val;
                if (c == 1.0) return formatSymNode(n->right);
                if (c == -1.0) return "-" + formatSymNode(n->right);
                if (n->right->type == SymType::Variable || n->right->type == SymType::Pow) {
                    return formatDouble(c) + formatSymNode(n->right);
                }
            }
            std::string ls = formatSymNode(n->left);
            std::string rs = formatSymNode(n->right);
            return ls + " * " + rs;
        }
        case SymType::Div: {
            if (n->left->isConst() && n->right->type == SymType::Pow) {
                return "(" + formatDouble(n->left->val) + "/" + formatDouble(n->right->right->val) + ")" + formatSymNode(n->right->left) + "^" + formatSymNode(n->right->right);
            }
            if (n->left->type == SymType::Pow && n->right->isConst()) {
                double denom = n->right->val;
                if (denom != 0.0) {
                    return "(1/" + formatDouble(denom) + ")" + formatSymNode(n->left);
                }
            }
            return "(" + formatSymNode(n->left) + ")/(" + formatSymNode(n->right) + ")";
        }
        case SymType::Pow: {
            std::string base = formatSymNode(n->left);
            std::string exp = formatSymNode(n->right);
            if (n->left->type == SymType::Variable) return base + "^" + exp;
            return "(" + base + ")^" + exp;
        }
        case SymType::Sin: return "sin(" + formatSymNode(n->left) + ")";
        case SymType::Cos: return "cos(" + formatSymNode(n->left) + ")";
        case SymType::Tan: return "tan(" + formatSymNode(n->left) + ")";
        case SymType::Exp: return "e^(" + formatSymNode(n->left) + ")";
        case SymType::Ln:  return "ln(" + formatSymNode(n->left) + ")";
        case SymType::Sqrt: return "sqrt(" + formatSymNode(n->left) + ")";
        case SymType::Abs: return "|" + formatSymNode(n->left) + "|";
    }
    return "";
}

// ------------------------------------------------------------- Row Parser
class SymParser {
    const Row* row = nullptr;
    size_t pos = 0;
    size_t endPos = 0;

public:
    SymParser(const Row* r, size_t start, size_t finish)
        : row(r), pos(start), endPos(finish) {}

    const Item* peek() const {
        if (!row || pos >= endPos) return nullptr;
        return row->items[pos].get();
    }

    bool atEnd() const { return !row || pos >= endPos; }

    bool peekIsOp(char c) const {
        const Item* it = peek();
        return it && it->type == ItemType::Operator && it->opChar == c;
    }

    SymPtr parseAtom() {
        if (atEnd()) return num(0.0);
        const Item* it = peek();
        pos++;
        switch (it->type) {
            case ItemType::Number:
                return num(std::stod(it->numText));
            case ItemType::Variable:
                return varNode(it->variableName);
            case ItemType::Constant:
                if (it->constantName == 'p') return num(3.141592653589793);
                if (it->constantName == 'f') return num(1.618033988749895);
                return num(2.718281828459045);
            case ItemType::Paren: {
                SymParser inner(it->a.get(), 0, it->a ? it->a->items.size() : 0);
                return inner.parseExpr();
            }
            case ItemType::Fraction: {
                SymParser numP(it->a.get(), 0, it->a ? it->a->items.size() : 0);
                SymParser denP(it->b.get(), 0, it->b ? it->b->items.size() : 0);
                return div(numP.parseExpr(), denP.parseExpr());
            }
            case ItemType::Power: {
                SymParser baseP(it->a.get(), 0, it->a ? it->a->items.size() : 0);
                SymParser expP(it->b.get(), 0, it->b ? it->b->items.size() : 0);
                return pwr(baseP.parseExpr(), expP.parseExpr());
            }
            case ItemType::Sqrt: {
                SymParser radP(it->a.get(), 0, it->a ? it->a->items.size() : 0);
                return fnSqrt(radP.parseExpr());
            }
            case ItemType::Function: {
                SymParser argP(it->a.get(), 0, it->a ? it->a->items.size() : 0);
                SymPtr arg = argP.parseExpr();
                switch (it->functionId) {
                    case SciSin: return fnSin(arg);
                    case SciCos: return fnCos(arg);
                    case SciTan: return fnTan(arg);
                    case SciExp: return fnExp(arg);
                    case SciLn:  return fnLn(arg);
                    case SciAbs: return std::make_shared<SymNode>(SymType::Abs, arg);
                    default: return arg;
                }
            }
            default: break;
        }
        return num(0.0);
    }

    SymPtr parseFactor() {
        bool neg = false;
        while (peekIsOp('-') || peekIsOp('+')) {
            if (peekIsOp('-')) neg = !neg;
            pos++;
        }
        SymPtr atom = parseAtom();
        // check exponentiation
        if (peekIsOp('^')) {
            pos++;
            atom = pwr(atom, parseFactor());
        }
        return neg ? mul(num(-1.0), atom) : atom;
    }

    SymPtr parseTerm() {
        SymPtr term = parseFactor();
        for (;;) {
            if (peekIsOp('*')) {
                pos++;
                term = mul(term, parseFactor());
                continue;
            }
            const Item* nxt = peek();
            if (nxt && nxt->type != ItemType::Operator && nxt->type != ItemType::Equals && nxt->type != ItemType::CloseParen) {
                // implicit multiplication
                term = mul(term, parseFactor());
                continue;
            }
            break;
        }
        return term;
    }

    SymPtr parseExpr() {
        if (atEnd()) return num(0.0);
        SymPtr expr = parseTerm();
        for (;;) {
            if (peekIsOp('+')) {
                pos++;
                expr = add(expr, parseTerm());
            } else if (peekIsOp('-')) {
                pos++;
                expr = sub(expr, parseTerm());
            } else {
                break;
            }
        }
        return expr;
    }
};

SymPtr symDiffNode(SymPtr n, char v) {
    if (!n->dependsOn(v)) return num(0.0);
    if (n->isVar(v)) return num(1.0);
    switch (n->type) {
        case SymType::Add: return add(symDiffNode(n->left, v), symDiffNode(n->right, v));
        case SymType::Sub: return sub(symDiffNode(n->left, v), symDiffNode(n->right, v));
        case SymType::Mul:
            return add(mul(symDiffNode(n->left, v), n->right), mul(n->left, symDiffNode(n->right, v)));
        case SymType::Div:
            return div(sub(mul(symDiffNode(n->left, v), n->right), mul(n->left, symDiffNode(n->right, v))),
                       pwr(n->right, num(2.0)));
        case SymType::Pow:
            if (!n->right->dependsOn(v)) {
                return mul(mul(n->right, pwr(n->left, sub(n->right, num(1.0)))), symDiffNode(n->left, v));
            }
            break;
        case SymType::Sin:
            return mul(fnCos(n->left), symDiffNode(n->left, v));
        case SymType::Cos:
            return mul(mul(num(-1.0), fnSin(n->left)), symDiffNode(n->left, v));
        case SymType::Tan:
            return div(symDiffNode(n->left, v), pwr(fnCos(n->left), num(2.0)));
        case SymType::Exp:
            return mul(fnExp(n->left), symDiffNode(n->left, v));
        case SymType::Ln:
            return div(symDiffNode(n->left, v), n->left);
        case SymType::Sqrt:
            return div(symDiffNode(n->left, v), mul(num(2.0), fnSqrt(n->left)));
        default: break;
    }
    return num(0.0);
}

SymPtr symIntNode(SymPtr n, char v) {
    if (!n->dependsOn(v)) {
        return mul(n, varNode(v));
    }
    if (n->isVar(v)) {
        return mul(num(0.5), pwr(varNode(v), num(2.0)));
    }
    switch (n->type) {
        case SymType::Add: return add(symIntNode(n->left, v), symIntNode(n->right, v));
        case SymType::Sub: return sub(symIntNode(n->left, v), symIntNode(n->right, v));
        case SymType::Mul:
            if (!n->left->dependsOn(v)) return mul(n->left, symIntNode(n->right, v));
            if (!n->right->dependsOn(v)) return mul(n->right, symIntNode(n->left, v));
            break;
        case SymType::Div:
            if (!n->right->dependsOn(v)) return div(symIntNode(n->left, v), n->right);
            if (n->left->isConstVal(1.0) && n->right->isVar(v)) {
                return fnLn(varNode(v));
            }
            break;
        case SymType::Pow:
            if (n->left->isVar(v) && !n->right->dependsOn(v)) {
                if (n->right->isConstVal(-1.0)) return fnLn(varNode(v));
                SymPtr newExp = add(n->right, num(1.0));
                return div(pwr(varNode(v), newExp), newExp);
            }
            break;
        case SymType::Sin:
            if (n->left->isVar(v)) return mul(num(-1.0), fnCos(varNode(v)));
            break;
        case SymType::Cos:
            if (n->left->isVar(v)) return fnSin(varNode(v));
            break;
        case SymType::Exp:
            if (n->left->isVar(v)) return fnExp(varNode(v));
            break;
        default: break;
    }
    return n;
}

// Adaptive Simpson integration helper
double adaptiveSimpsonRec(const std::function<double(double)>& f,
                          double a, double b, double fa, double fb, double fc,
                          double whole, double tol, int depth) {
    double c = (a + b) / 2.0;
    double d = (a + c) / 2.0;
    double e = (c + b) / 2.0;
    double fd = f(d);
    double fe = f(e);
    double left = (c - a) / 6.0 * (fa + 4.0 * fd + fc);
    double right = (b - c) / 6.0 * (fc + 4.0 * fe + fb);
    double delta = left + right - whole;
    if (depth <= 0 || std::fabs(delta) <= 15.0 * tol) {
        return left + right + delta / 15.0;
    }
    return adaptiveSimpsonRec(f, a, c, fa, fc, fd, left, tol / 2.0, depth - 1) +
           adaptiveSimpsonRec(f, c, b, fc, fb, fe, right, tol / 2.0, depth - 1);
}

double simpsonIntegrate(const std::function<double(double)>& f, double a, double b) {
    if (a == b) return 0.0;
    double c = (a + b) / 2.0;
    double fa = f(a);
    double fb = f(b);
    double fc = f(c);
    double whole = (b - a) / 6.0 * (fa + 4.0 * fc + fb);
    return adaptiveSimpsonRec(f, a, b, fa, fb, fc, whole, 1e-8, 18);
}

} // namespace

std::string symbolicIntegrate(const Row* row, char var, bool appendConstant) {
    if (!row || row->items.empty()) return appendConstant ? "C" : "0";
    SymParser parser(row, 0, row->items.size());
    SymPtr root = parser.parseExpr();
    SymPtr intNode = symIntNode(root, var);
    std::string res = formatSymNode(intNode);
    if (res.empty() || res == "0") res = "0";
    if (appendConstant) {
        if (res == "0") return "C";
        return res + " + C";
    }
    return res;
}

std::string symbolicDifferentiate(const Row* row, char var) {
    if (!row || row->items.empty()) return "0";
    SymParser parser(row, 0, row->items.size());
    SymPtr root = parser.parseExpr();
    SymPtr diffNode = symDiffNode(root, var);
    std::string res = formatSymNode(diffNode);
    if (res.empty()) return "0";
    return res;
}

double evalDefiniteIntegral(const Row* row, double a, double b, char var, const EvaluationContext& context) {
    if (!row) return 0.0;
    auto integrand = [&](double t) -> double {
        EvaluationContext sub = context;
        sub.degrees = false; // Calculus operates strictly in radians
        if (var == 'y') sub.y = t;
        else sub.x = t;
        return evaluate(row, sub);
    };
    return simpsonIntegrate(integrand, a, b);
}

double evalDerivativeAtPoint(const Row* row, double x0, char var, const EvaluationContext& context) {
    if (!row) return 0.0;
    double h = 1e-6;
    EvaluationContext subP = context;
    EvaluationContext subM = context;
    subP.degrees = false; // Calculus operates strictly in radians
    subM.degrees = false;
    if (var == 'y') { subP.y = x0 + h; subM.y = x0 - h; }
    else { subP.x = x0 + h; subM.x = x0 - h; }
    double fp = evaluate(row, subP);
    double fm = evaluate(row, subM);
    return (fp - fm) / (2.0 * h);
}
