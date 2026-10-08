// evaluator.cpp
#include "evaluator.h"
#include "symbolic.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <numeric>
#include <functional>

bool MathSet::contains(const std::string& s) const {
    return std::find(elements.begin(), elements.end(), s) != elements.end();
}

void MathSet::add(const std::string& s) {
    if (!contains(s)) elements.push_back(s);
}

std::string MathSet::toString() const {
    if (elements.empty()) return "{}";
    std::string res = "{";
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) res += ", ";
        res += elements[i];
    }
    res += "}";
    return res;
}

MathSet setUnion(const MathSet& a, const MathSet& b) {
    MathSet res = a;
    for (const auto& elem : b.elements) res.add(elem);
    return res;
}

MathSet setIntersection(const MathSet& a, const MathSet& b) {
    MathSet res;
    for (const auto& elem : a.elements) {
        if (b.contains(elem)) res.add(elem);
    }
    return res;
}

MathSet setProduct(const MathSet& a, const MathSet& b) {
    MathSet res;
    for (const auto& x : a.elements) {
        for (const auto& y : b.elements) {
            res.add("(" + x + ", " + y + ")");
        }
    }
    return res;
}

MathSet setDelta(const MathSet& a, const MathSet& b) {
    MathSet res;
    for (const auto& elem : a.elements) {
        if (!b.contains(elem)) res.add(elem);
    }
    for (const auto& elem : b.elements) {
        if (!a.contains(elem)) res.add(elem);
    }
    return res;
}

std::string Matrix::toString() const {
    if (rows == 0 || cols == 0) return "[]";
    std::string s = "[";
    for (size_t r = 0; r < rows; ++r) {
        if (rows > 1) s += "[";
        for (size_t c = 0; c < cols; ++c) {
            double val = at(r, c);
            if (std::fabs(val) < 1e-12) val = 0.0;
            char buf[64];
            if (std::floor(val) == val && std::fabs(val) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%.0f", val);
            } else {
                std::snprintf(buf, sizeof(buf), "%.6g", val);
            }
            s += buf;
            if (c + 1 < cols) s += ", ";
        }
        if (rows > 1) s += "]";
        if (r + 1 < rows) s += ", ";
    }
    s += "]";
    return s;
}

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
    return sameRow(left->a.get(), right->a.get()) &&
           sameRow(left->b.get(), right->b.get()) &&
           sameRow(left->c.get(), right->c.get());
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
        collectVariables(item->c.get(), hasX, hasY);
        collectVariables(item->d.get(), hasX, hasY);
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

// --- Matrix Algorithms & Helpers -------------------------------------------

Matrix matrixAdd(const Matrix& a, const Matrix& b) {
    if (a.rows != b.rows || a.cols != b.cols)
        throw std::runtime_error("Matrix dimensions must match for addition");
    Matrix r(a.rows, a.cols);
    for (size_t i = 0; i < a.data.size(); ++i) r.data[i] = a.data[i] + b.data[i];
    return r;
}

Matrix matrixSub(const Matrix& a, const Matrix& b) {
    if (a.rows != b.rows || a.cols != b.cols)
        throw std::runtime_error("Matrix dimensions must match for subtraction");
    Matrix r(a.rows, a.cols);
    for (size_t i = 0; i < a.data.size(); ++i) r.data[i] = a.data[i] - b.data[i];
    return r;
}

Matrix matrixMul(const Matrix& a, const Matrix& b) {
    if (a.cols != b.rows)
        throw std::runtime_error("Matrix dimensions incompatible for multiplication");
    Matrix r(a.rows, b.cols);
    for (size_t i = 0; i < a.rows; ++i) {
        for (size_t k = 0; k < a.cols; ++k) {
            double aik = a.at(i, k);
            if (std::fabs(aik) < 1e-15) continue;
            for (size_t j = 0; j < b.cols; ++j) {
                r.at(i, j) += aik * b.at(k, j);
            }
        }
    }
    return r;
}

Matrix matrixScalarMul(const Matrix& a, double k) {
    Matrix r(a.rows, a.cols);
    for (size_t i = 0; i < a.data.size(); ++i) r.data[i] = a.data[i] * k;
    return r;
}

Matrix matrixEye(size_t n) {
    if (n == 0) throw std::runtime_error("Matrix dimension must be positive");
    Matrix r(n, n);
    for (size_t i = 0; i < n; ++i) r.at(i, i) = 1.0;
    return r;
}

Matrix matrixZeros(size_t r, size_t c) {
    if (r == 0 || c == 0) throw std::runtime_error("Matrix dimensions must be positive");
    return Matrix(r, c);
}

Matrix matrixOnes(size_t r, size_t c) {
    if (r == 0 || c == 0) throw std::runtime_error("Matrix dimensions must be positive");
    Matrix m(r, c);
    std::fill(m.data.begin(), m.data.end(), 1.0);
    return m;
}

Matrix matrixTranspose(const Matrix& a) {
    Matrix r(a.cols, a.rows);
    for (size_t i = 0; i < a.rows; ++i) {
        for (size_t j = 0; j < a.cols; ++j) {
            r.at(j, i) = a.at(i, j);
        }
    }
    return r;
}

double matrixTrace(const Matrix& a) {
    if (!a.isSquare()) throw std::runtime_error("trace requires a square matrix");
    double tr = 0.0;
    for (size_t i = 0; i < a.rows; ++i) tr += a.at(i, i);
    return tr;
}

double matrixDet(Matrix a) {
    if (!a.isSquare()) throw std::runtime_error("det requires a square matrix");
    size_t n = a.rows;
    double det = 1.0;
    for (size_t i = 0; i < n; ++i) {
        size_t pivot = i;
        for (size_t j = i + 1; j < n; ++j) {
            if (std::fabs(a.at(j, i)) > std::fabs(a.at(pivot, i))) pivot = j;
        }
        if (std::fabs(a.at(pivot, i)) < 1e-12) return 0.0;
        if (pivot != i) {
            for (size_t k = 0; k < n; ++k) std::swap(a.at(i, k), a.at(pivot, k));
            det = -det;
        }
        det *= a.at(i, i);
        for (size_t j = i + 1; j < n; ++j) {
            double factor = a.at(j, i) / a.at(i, i);
            for (size_t k = i; k < n; ++k) a.at(j, k) -= factor * a.at(i, k);
        }
    }
    return std::fabs(det) < 1e-12 ? 0.0 : det;
}

Matrix matrixInv(Matrix a) {
    if (!a.isSquare()) throw std::runtime_error("inv requires a square matrix");
    size_t n = a.rows;
    Matrix inv = matrixEye(n);
    for (size_t i = 0; i < n; ++i) {
        size_t pivot = i;
        for (size_t j = i + 1; j < n; ++j) {
            if (std::fabs(a.at(j, i)) > std::fabs(a.at(pivot, i))) pivot = j;
        }
        if (std::fabs(a.at(pivot, i)) < 1e-12)
            throw std::runtime_error("Matrix is singular (non-invertible)");
        if (pivot != i) {
            for (size_t k = 0; k < n; ++k) {
                std::swap(a.at(i, k), a.at(pivot, k));
                std::swap(inv.at(i, k), inv.at(pivot, k));
            }
        }
        double div = a.at(i, i);
        for (size_t k = 0; k < n; ++k) {
            a.at(i, k) /= div;
            inv.at(i, k) /= div;
        }
        for (size_t j = 0; j < n; ++j) {
            if (j != i) {
                double factor = a.at(j, i);
                for (size_t k = 0; k < n; ++k) {
                    a.at(j, k) -= factor * a.at(i, k);
                    inv.at(j, k) -= factor * inv.at(i, k);
                }
            }
        }
    }
    return inv;
}

double matrixRank(Matrix a) {
    size_t m = a.rows;
    size_t n = a.cols;
    size_t rank = 0;
    for (size_t col = 0; col < n && rank < m; ++col) {
        size_t pivot = rank;
        for (size_t r = rank + 1; r < m; ++r) {
            if (std::fabs(a.at(r, col)) > std::fabs(a.at(pivot, col))) pivot = r;
        }
        if (std::fabs(a.at(pivot, col)) < 1e-12) continue;
        if (pivot != rank) {
            for (size_t k = 0; k < n; ++k) std::swap(a.at(rank, k), a.at(pivot, k));
        }
        double div = a.at(rank, col);
        for (size_t k = col; k < n; ++k) a.at(rank, k) /= div;
        for (size_t r = 0; r < m; ++r) {
            if (r != rank && std::fabs(a.at(r, col)) > 1e-12) {
                double factor = a.at(r, col);
                for (size_t k = col; k < n; ++k) a.at(r, k) -= factor * a.at(rank, k);
            }
        }
        ++rank;
    }
    return (double)rank;
}

Matrix matrixRref(Matrix a) {
    size_t m = a.rows;
    size_t n = a.cols;
    size_t lead = 0;
    for (size_t r = 0; r < m && lead < n; ++r) {
        size_t i = r;
        while (std::fabs(a.at(i, lead)) < 1e-12) {
            ++i;
            if (i == m) {
                i = r;
                ++lead;
                if (lead == n) return a;
            }
        }
        if (i != r) {
            for (size_t k = 0; k < n; ++k) std::swap(a.at(r, k), a.at(i, k));
        }
        double div = a.at(r, lead);
        if (std::fabs(div) > 1e-12) {
            for (size_t k = 0; k < n; ++k) a.at(r, k) /= div;
        }
        for (size_t j = 0; j < m; ++j) {
            if (j != r) {
                double factor = a.at(j, lead);
                for (size_t k = 0; k < n; ++k) a.at(j, k) -= factor * a.at(r, k);
            }
        }
        ++lead;
    }
    for (size_t i = 0; i < a.data.size(); ++i) {
        if (std::fabs(a.data[i]) < 1e-12) a.data[i] = 0.0;
    }
    return a;
}

double vectorDot(const Matrix& u, const Matrix& v) {
    if (u.data.size() != v.data.size() || u.data.empty())
        throw std::runtime_error("dot requires vectors of equal non-zero dimension");
    double d = 0.0;
    for (size_t i = 0; i < u.data.size(); ++i) d += u.data[i] * v.data[i];
    return d;
}

Matrix vectorCross(const Matrix& u, const Matrix& v) {
    if (u.data.size() != 3 || v.data.size() != 3)
        throw std::runtime_error("cross requires 3-dimensional vectors");
    Matrix r(1, 3);
    r.at(0, 0) = u.data[1] * v.data[2] - u.data[2] * v.data[1];
    r.at(0, 1) = u.data[2] * v.data[0] - u.data[0] * v.data[2];
    r.at(0, 2) = u.data[0] * v.data[1] - u.data[1] * v.data[0];
    return r;
}

double matrixNorm(const Matrix& a) {
    double sum = 0.0;
    for (double x : a.data) sum += x * x;
    return std::sqrt(sum);
}

// --- Calculus Solvers ------------------------------------------------------

static double adaptiveSimpsonRec(const std::function<double(double)>& f,
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

static double numericalIntegrate(const std::function<double(double)>& f, double a, double b) {
    if (a == b) return 0.0;
    double c = (a + b) / 2.0;
    double fa = f(a);
    double fb = f(b);
    double fc = f(c);
    double whole = (b - a) / 6.0 * (fa + 4.0 * fc + fb);
    return adaptiveSimpsonRec(f, a, b, fa, fb, fc, whole, 1e-9, 20);
}

static double numericalDiff(const std::function<double(double)>& f, double x0) {
    double h = 1e-5 * std::max(1.0, std::fabs(x0));
    double fp2 = f(x0 + 2.0 * h);
    double fp1 = f(x0 + h);
    double fm1 = f(x0 - h);
    double fm2 = f(x0 - 2.0 * h);
    return (-fp2 + 8.0 * fp1 - 8.0 * fm1 + fm2) / (12.0 * h);
}

static double numericalLimit(const std::function<double(double)>& f, double c, int dir = 0) {
    double steps[] = { 1e-3, 1e-5, 1e-7, 1e-9 };
    double lastVal = 0.0;
    bool found = false;
    for (double h : steps) {
        try {
            if (dir > 0) {
                double right = f(c + h);
                if (std::isfinite(right)) { lastVal = right; found = true; }
            } else if (dir < 0) {
                double left = f(c - h);
                if (std::isfinite(left)) { lastVal = left; found = true; }
            } else {
                double right = f(c + h);
                double left = f(c - h);
                if (std::isfinite(right) && std::isfinite(left)) {
                    lastVal = (right + left) / 2.0;
                    found = true;
                } else if (std::isfinite(right)) {
                    lastVal = right; found = true;
                } else if (std::isfinite(left)) {
                    lastVal = left; found = true;
                }
            }
        } catch (...) {}
    }
    if (found) return lastVal;
    return f(c);
}

// --- Special & Number Theory Algorithms ------------------------------------

static double besselJ0(double x) {
    double ax = std::fabs(x);
    if (ax < 8.0) {
        double y = x * x;
        double ans1 = 57568490574.0 + y * (-13362590354.0 + y * (651619640.7
                      + y * (-11214424.18 + y * (77392.33017 + y * (-184.9052456)))));
        double ans2 = 57568490574.0 + y * (1029532985.0 + y * (9494680.718
                      + y * (59272.64853 + y * (267.8532712 + y * 1.0))));
        return ans1 / ans2;
    } else {
        double z = 8.0 / ax;
        double y = z * z;
        double xx = ax - 0.785398164;
        double p0 = 1.0 + y * (-0.1098628627e-2 + y * (0.2734510407e-4
                    + y * (-0.2073370639e-5 + y * 0.2093887211e-6)));
        double q0 = -0.1562499995e-1 + y * (0.1430488765e-3
                    + y * (-0.6911147651e-5 + y * (0.7621095161e-6 - y * 0.934945152e-7)));
        return std::sqrt(0.636619772 / ax) * (p0 * std::cos(xx) - z * q0 * std::sin(xx));
    }
}

static double besselJ1(double x) {
    double ax = std::fabs(x);
    if (ax < 8.0) {
        double y = x * x;
        double ans1 = x * (72362614232.0 + y * (-7895059235.0 + y * (242396853.1
                      + y * (-2972635.139 + y * (15704.48260 + y * (-30.16036606))))));
        double ans2 = 144725228464.0 + y * (2300535178.0 + y * (18583304.74
                      + y * (99447.43394 + y * (376.9991397 + y * 1.0))));
        return ans1 / ans2;
    } else {
        double z = 8.0 / ax;
        double y = z * z;
        double xx = ax - 2.356194491;
        double p1 = 1.0 + y * (0.183105e-2 + y * (-0.3516396496e-4
                    + y * (0.2217540007e-5 - y * 0.2093887211e-6)));
        double q1 = 0.04687499995 + y * (-0.2002690873e-3
                    + y * (0.8449199096e-5 + y * (-0.8836181e-6 + y * 0.1016669e-6)));
        double ans = std::sqrt(0.636619772 / ax) * (p1 * std::cos(xx) - z * q1 * std::sin(xx));
        return (x < 0.0) ? -ans : ans;
    }
}

static double besselY0(double x) {
    if (x <= 0.0) throw std::runtime_error("bessely0 needs positive input");
    if (x < 8.0) {
        double j0 = besselJ0(x);
        double y = x * x;
        double ans1 = -2957821389.0 + y * (7062834065.0 + y * (-512359803.6
                      + y * (10879881.29 + y * (-86327.92757 + y * 228.4622733))));
        double ans2 = 40076544269.0 + y * (745249964.8 + y * (7189466.438
                      + y * (47447.26470 + y * (226.1030244 + y * 1.0))));
        return (ans1 / ans2) + 0.636619772 * j0 * std::log(x);
    } else {
        double z = 8.0 / x;
        double y = z * z;
        double xx = x - 0.785398164;
        double p0 = 1.0 + y * (-0.1098628627e-2 + y * (0.2734510407e-4
                    + y * (-0.2073370639e-5 + y * 0.2093887211e-6)));
        double q0 = -0.1562499995e-1 + y * (0.1430488765e-3
                    + y * (-0.6911147651e-5 + y * (0.7621095161e-6 - y * 0.934945152e-7)));
        return std::sqrt(0.636619772 / x) * (p0 * std::sin(xx) + z * q0 * std::cos(xx));
    }
}

static double besselY1(double x) {
    if (x <= 0.0) throw std::runtime_error("bessely1 needs positive input");
    if (x < 8.0) {
        double j1 = besselJ1(x);
        double y = x * x;
        double ans1 = x * (-4900604943.0 + y * (1275274390.0 + y * (-51534381.39
                      + y * (734926.4532 + y * (-4237.922814 + y * 8.511934407)))));
        double ans2 = 2499580570.0 + y * (424441966.4 + y * (3733650.367
                      + y * (22459.04002 + y * (102.0426058 + y * 1.0))));
        return (ans1 / ans2) + 0.636619772 * (j1 * std::log(x) - 1.0 / x);
    } else {
        double z = 8.0 / x;
        double y = z * z;
        double xx = x - 2.356194491;
        double p1 = 1.0 + y * (0.183105e-2 + y * (-0.3516396496e-4
                    + y * (0.2217540007e-5 - y * 0.2093887211e-6)));
        double q1 = 0.04687499995 + y * (-0.2002690873e-3
                    + y * (0.8449199096e-5 + y * (-0.8836181e-6 + y * 0.1016669e-6)));
        return std::sqrt(0.636619772 / x) * (p1 * std::sin(xx) + z * q1 * std::cos(xx));
    }
}

static double lambertW0(double x) {
    if (x < -0.3678794411714423215955)
        throw std::runtime_error("lambertw needs input >= -1/e");
    if (std::fabs(x) < 1e-15) return x;
    double w = (x < 1.0) ? (x / (1.0 + x / (1.0 + x * 0.5)))
                         : (std::log(x) - std::log(std::log(x)));
    for (int iter = 0; iter < 20; ++iter) {
        double ew = std::exp(w);
        double f = w * ew - x;
        double fp = ew * (w + 1.0);
        double fpp = ew * (w + 2.0);
        double step = f / (fp - (f * fpp) / (2.0 * fp));
        w -= step;
        if (std::fabs(step) < 1e-15) break;
    }
    return w;
}

static double riemannZeta(double s) {
    if (s == 1.0) throw std::runtime_error("zeta is undefined at 1");
    if (s == 0.0) return -0.5;
    if (s == -1.0) return -1.0 / 12.0;
    if (s == 2.0) return (kSciPi * kSciPi) / 6.0;
    if (s == 4.0) return (kSciPi * kSciPi * kSciPi * kSciPi) / 90.0;
    if (s > 1.0) {
        double sum = 0.0;
        int N = 40;
        for (int k = 1; k <= N; ++k) sum += std::pow(k, -s);
        double term1 = std::pow(N, 1.0 - s) / (s - 1.0);
        double term2 = 0.5 * std::pow(N, -s);
        double term3 = (s / 12.0) * std::pow(N, -s - 1.0);
        return sum + term1 + term2 - term3;
    }
    double reflection = std::pow(2.0, s) * std::pow(kSciPi, s - 1.0) *
                        std::sin(kSciPi * s / 2.0) * std::tgamma(1.0 - s) * riemannZeta(1.0 - s);
    return reflection;
}

static double isNumberPrime(double x) {
    if (x <= 1.0 || std::floor(x) != x) return 0.0;
    long long n = (long long)x;
    if (n == 2 || n == 3) return 1.0;
    if (n % 2 == 0 || n % 3 == 0) return 0.0;
    for (long long i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0.0;
    }
    return 1.0;
}

static double calcGcd(double a, double b) {
    long long ia = std::llround(std::fabs(a));
    long long ib = std::llround(std::fabs(b));
    return (double)std::gcd(ia, ib);
}

static double calcLcm(double a, double b) {
    long long ia = std::llround(std::fabs(a));
    long long ib = std::llround(std::fabs(b));
    if (ia == 0 || ib == 0) return 0.0;
    return (double)std::lcm(ia, ib);
}

static std::vector<std::pair<size_t, size_t>> getArgSlices(const Row* row, size_t start = 0, size_t finish = static_cast<size_t>(-1)) {
    std::vector<std::pair<size_t, size_t>> slices;
    if (!row || row->items.empty()) return slices;
    if (finish == static_cast<size_t>(-1)) finish = row->items.size();
    size_t s = start;
    for (size_t i = start; i < finish; ++i) {
        if (row->items[i]->type == ItemType::Operator && row->items[i]->opChar == ',') {
            slices.emplace_back(s, i);
            s = i + 1;
        }
    }
    slices.emplace_back(s, finish);
    return slices;
}

static std::vector<std::pair<size_t, size_t>> getSemicolonSlices(const Row* row) {
    std::vector<std::pair<size_t, size_t>> slices;
    if (!row || row->items.empty()) return slices;
    size_t s = 0;
    for (size_t i = 0; i < row->items.size(); ++i) {
        if (row->items[i]->type == ItemType::Operator && row->items[i]->opChar == ';') {
            slices.emplace_back(s, i);
            s = i + 1;
        }
    }
    slices.emplace_back(s, row->items.size());
    return slices;
}

EvalValue evalValueAdd(const EvalValue& a, const EvalValue& b) {
    if (a.isNumber() && b.isNumber()) return EvalValue(a.num + b.num);
    if (a.isMatrix() && b.isMatrix()) return EvalValue(matrixAdd(a.mat, b.mat));
    throw std::runtime_error("Cannot add scalar and matrix directly");
}

EvalValue evalValueSub(const EvalValue& a, const EvalValue& b) {
    if (a.isNumber() && b.isNumber()) return EvalValue(a.num - b.num);
    if (a.isMatrix() && b.isMatrix()) return EvalValue(matrixSub(a.mat, b.mat));
    throw std::runtime_error("Cannot subtract scalar and matrix directly");
}

EvalValue evalValueMul(const EvalValue& a, const EvalValue& b) {
    if (a.isNumber() && b.isNumber()) return EvalValue(a.num * b.num);
    if (a.isNumber() && b.isMatrix()) return EvalValue(matrixScalarMul(b.mat, a.num));
    if (a.isMatrix() && b.isNumber()) return EvalValue(matrixScalarMul(a.mat, b.num));
    if (a.isMatrix() && b.isMatrix()) return EvalValue(matrixMul(a.mat, b.mat));
    throw std::runtime_error("Invalid operands for multiplication");
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
        case SciSinc: return (std::fabs(x) < 1e-15) ? 1.0 : (std::sin(x) / x);
        case SciBesselJ0: return besselJ0(x);
        case SciBesselJ1: return besselJ1(x);
        case SciBesselY0: return besselY0(x);
        case SciBesselY1: return besselY1(x);
        case SciLambertW: return lambertW0(x);
        case SciZeta: return riemannZeta(x);
        case SciIsPrime: return isNumberPrime(x);
        case SciDet:
        case SciTrace:
        case SciRank:
        case SciNorm:
            return x;
        case SciEye:
            return 1.0;
        default:
            break;
    }
    throw std::runtime_error("Unknown function");
}

static std::unique_ptr<Item> copyItem(const Item* source) {
    if (!source) return nullptr;
    auto it = std::make_unique<Item>(source->type);
    it->numText = source->numText;
    it->opChar = source->opChar;
    it->variableName = source->variableName;
    it->nameText = source->nameText;
    it->constantName = source->constantName;
    it->functionId = source->functionId;
    it->isBracket = source->isBracket;
    it->isBrace = source->isBrace;
    if (source->a) it->a = cloneRow(source->a.get());
    if (source->b) it->b = cloneRow(source->b.get());
    if (source->c) it->c = cloneRow(source->c.get());
    if (source->d) it->d = cloneRow(source->d.get());
    return it;
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

    MathSet parseSetFromRow(const Row* row) {
        MathSet s;
        if (!row || row->items.empty()) return s;
        auto slices = getArgSlices(row);
        for (auto& sl : slices) {
            if (sl.first >= sl.second) continue;
            if (sl.second == sl.first + 1) {
                const Item* it = row->items[sl.first].get();
                if (it->type == ItemType::Variable) {
                    s.add(std::string(1, it->variableName));
                    continue;
                }
                if (it->type == ItemType::Name) {
                    s.add(it->nameText);
                    continue;
                }
            }
            try {
                RowParser cellParser(row, context, sl.first, sl.second);
                EvalValue val = cellParser.parseRowValue();
                if (val.isSymbolic()) s.add(val.text);
                else if (val.isSet()) s.add(val.setVal.toString());
                else if (val.isNumber()) {
                    double num = val.num;
                    char buf[64];
                    if (std::floor(num) == num && std::fabs(num) < 1e12) {
                        std::snprintf(buf, sizeof(buf), "%.0f", num);
                    } else {
                        std::snprintf(buf, sizeof(buf), "%.6g", num);
                    }
                    s.add(buf);
                } else {
                    s.add(rowRangeToPlainString(row, (int)sl.first, (int)sl.second));
                }
            } catch (...) {
                s.add(rowRangeToPlainString(row, (int)sl.first, (int)sl.second));
            }
        }
        return s;
    }

    EvalValue parseAtomValue() {
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
                    return EvalValue(std::stod(numberText));
                } catch (...) {
                    throw std::runtime_error("Invalid number");
                }
            }
            case ItemType::Variable:
                pos++;
                if (it->variableName == 'x') return EvalValue(context.x);
                if (it->variableName == 'y') return EvalValue(context.y);
                throw std::runtime_error("Unknown variable");
            case ItemType::Fraction: {
                pos++;
                EvalValue n = evaluateValue(it->a.get(), context);
                EvalValue d = evaluateValue(it->b.get(), context);
                if (n.isNumber() && d.isNumber()) {
                    if (d.num == 0.0) throw std::runtime_error("Division by zero");
                    return EvalValue(n.num / d.num);
                }
                if (n.isMatrix() && d.isNumber()) {
                    if (d.num == 0.0) throw std::runtime_error("Division by zero");
                    return EvalValue(matrixScalarMul(n.mat, 1.0 / d.num));
                }
                if (n.isMatrix() && d.isMatrix()) {
                    return EvalValue(matrixMul(n.mat, matrixInv(d.mat)));
                }
                if (n.isNumber() && d.isMatrix()) {
                    return EvalValue(matrixScalarMul(matrixInv(d.mat), n.num));
                }
                throw std::runtime_error("Invalid fraction");
            }
            case ItemType::Paren: {
                pos++;
                if (it->isBrace) {
                    return EvalValue(parseSetFromRow(it->a.get()));
                }
                if (it->isBracket) {
                    const Row* inner = it->a.get();
                    if (!inner || inner->items.empty()) return EvalValue(Matrix(0, 0));
                    bool hasNestedBrackets = false;
                    for (const auto& child : inner->items) {
                        if (child->type == ItemType::Paren && child->isBracket) {
                            hasNestedBrackets = true;
                            break;
                        }
                    }
                    if (hasNestedBrackets) {
                        std::vector<std::vector<double>> rowsData;
                        for (const auto& child : inner->items) {
                            if (child->type == ItemType::Paren && child->isBracket) {
                                std::vector<double> rowElements;
                                auto slices = getArgSlices(child->a.get());
                                for (auto& sl : slices) {
                                    RowParser cellParser(child->a.get(), context, sl.first, sl.second);
                                    rowElements.push_back(cellParser.parseRowValue().asNumber());
                                }
                                rowsData.push_back(rowElements);
                            }
                        }
                        if (rowsData.empty()) return EvalValue(Matrix(0, 0));
                        size_t numRows = rowsData.size();
                        size_t numCols = rowsData[0].size();
                        for (size_t r = 1; r < numRows; ++r) {
                            if (rowsData[r].size() != numCols)
                                throw std::runtime_error("Matrix rows must have equal lengths");
                        }
                        Matrix m(numRows, numCols);
                        for (size_t r = 0; r < numRows; ++r)
                            for (size_t c = 0; c < numCols; ++c)
                                m.at(r, c) = rowsData[r][c];
                        return EvalValue(m);
                    }
                    auto rowSlices = getSemicolonSlices(inner);
                    if (rowSlices.size() > 1) {
                        std::vector<std::vector<double>> rowsData;
                        for (auto& rsl : rowSlices) {
                            std::vector<double> rowElements;
                            auto colSlices = getArgSlices(inner, rsl.first, rsl.second);
                            for (auto& csl : colSlices) {
                                RowParser cellParser(inner, context, csl.first, csl.second);
                                rowElements.push_back(cellParser.parseRowValue().asNumber());
                            }
                            rowsData.push_back(rowElements);
                        }
                        size_t numRows = rowsData.size();
                        size_t numCols = rowsData.empty() ? 0 : rowsData[0].size();
                        for (size_t r = 1; r < numRows; ++r) {
                            if (rowsData[r].size() != numCols)
                                throw std::runtime_error("Matrix rows must have equal lengths");
                        }
                        Matrix m(numRows, numCols);
                        for (size_t r = 0; r < numRows; ++r)
                            for (size_t c = 0; c < numCols; ++c)
                                m.at(r, c) = rowsData[r][c];
                        return EvalValue(m);
                    }
                    auto colSlices = getArgSlices(inner);
                    if (colSlices.size() > 1) {
                        Matrix m(1, colSlices.size());
                        for (size_t c = 0; c < colSlices.size(); ++c) {
                            RowParser cellParser(inner, context, colSlices[c].first, colSlices[c].second);
                            m.at(0, c) = cellParser.parseRowValue().asNumber();
                        }
                        return EvalValue(m);
                    }
                    RowParser cellParser(inner, context, 0, inner->items.size());
                    EvalValue singleVal = cellParser.parseRowValue();
                    if (singleVal.isMatrix()) return singleVal;
                    Matrix m(1, 1);
                    m.at(0, 0) = singleVal.asNumber();
                    return EvalValue(m);
                }
                return evaluateValue(it->a.get(), context);
            }
            case ItemType::Power: {
                pos++;
                EvalValue base = evaluateValue(it->a.get(), context);
                EvalValue exp = evaluateValue(it->b.get(), context);
                if (base.isNumber() && exp.isNumber()) {
                    return EvalValue(std::pow(base.num, exp.num));
                }
                if (base.isMatrix() && exp.isNumber()) {
                    double p = exp.num;
                    if (p == -1.0) return EvalValue(matrixInv(base.mat));
                    if (p >= 0.0 && std::floor(p) == p) {
                        int ip = static_cast<int>(p);
                        if (!base.mat.isSquare()) throw std::runtime_error("Matrix power requires square matrix");
                        Matrix res = matrixEye(base.mat.rows);
                        Matrix cur = base.mat;
                        while (ip > 0) {
                            if (ip & 1) res = matrixMul(res, cur);
                            cur = matrixMul(cur, cur);
                            ip >>= 1;
                        }
                        return EvalValue(res);
                    }
                    throw std::runtime_error("Matrix power requires integer exponent or -1");
                }
                throw std::runtime_error("Invalid power operation");
            }
            case ItemType::Sqrt: {
                pos++;
                EvalValue val = evaluateValue(it->a.get(), context);
                if (val.isNumber()) {
                    if (val.num < 0.0) throw std::runtime_error("Root of negative number");
                    return EvalValue(std::sqrt(val.num));
                }
                Matrix res = val.mat;
                for (double& d : res.data) {
                    if (d < 0.0) throw std::runtime_error("Root of negative number");
                    d = std::sqrt(d);
                }
                return EvalValue(res);
            }
            case ItemType::Constant:
                pos++;
                if (it->constantName == 'p') return EvalValue(kSciPi);
                if (it->constantName == 'f') return EvalValue(kSciPhi);
                return EvalValue(kSciE);
            case ItemType::Permutation: {
                pos++;
                double n = evaluateValue(it->a.get(), context).asNumber();
                double r = evaluateValue(it->b.get(), context).asNumber();
                return EvalValue(permutation(n, r));
            }
            case ItemType::Combination: {
                pos++;
                double n = evaluateValue(it->a.get(), context).asNumber();
                double r = evaluateValue(it->b.get(), context).asNumber();
                return EvalValue(combination(n, r));
            }
            case ItemType::Integral: {
                pos++;
                const Row* integrand = it->a.get();
                const Row* lower = it->b.get();
                const Row* upper = it->c.get();
                char var = it->variableName ? it->variableName : 'x';
                bool lowerEmpty = rowIsEmpty(lower);
                bool upperEmpty = rowIsEmpty(upper);

                if (lowerEmpty && upperEmpty) {
                    std::string res = symbolicIntegrate(integrand, var, true);
                    return EvalValue(res);
                } else {
                    double aVal = lowerEmpty ? 0.0 : evaluateValue(lower, context).asNumber();
                    double bVal = upperEmpty ? 0.0 : evaluateValue(upper, context).asNumber();
                    return EvalValue(evalDefiniteIntegral(integrand, aVal, bVal, var, context));
                }
            }
            case ItemType::Derivative: {
                pos++;
                const Row* exprRow = it->a.get();
                const Row* evalPt = it->b.get();
                char var = it->variableName ? it->variableName : 'x';
                bool ptEmpty = rowIsEmpty(evalPt);

                if (ptEmpty) {
                    std::string res = symbolicDifferentiate(exprRow, var);
                    return EvalValue(res);
                } else {
                    double x0 = evaluateValue(evalPt, context).asNumber();
                    return EvalValue(evalDerivativeAtPoint(exprRow, x0, var, context));
                }
            }
            case ItemType::Function: {
                pos++;
                int functionId = it->functionId;
                const Row* argRow = it->a.get();
                if (!argRow || argRow->items.empty())
                    throw std::runtime_error("Function argument missing");
                auto slices = getArgSlices(argRow);

                // --- Calculus ---
                if (functionId == SciIntegrate) {
                    auto detectVarInSlice = [&](size_t start, size_t finish) -> char {
                        for (size_t k = start; k < finish; ++k) {
                            if (argRow->items[k]->type == ItemType::Variable)
                                return argRow->items[k]->variableName;
                        }
                        return 'x';
                    };
                    if (slices.size() == 1) {
                        char var = detectVarInSlice(slices[0].first, slices[0].second);
                        auto subRow = std::make_unique<Row>();
                        for (size_t k = slices[0].first; k < slices[0].second; ++k) {
                            subRow->items.push_back(copyItem(argRow->items[k].get()));
                        }
                        std::string res = symbolicIntegrate(subRow.get(), var, true);
                        return EvalValue(res);
                    }
                    if (slices.size() == 2) {
                        bool secondIsVar = (slices[1].first < slices[1].second &&
                                            argRow->items[slices[1].first]->type == ItemType::Variable);
                        if (secondIsVar) {
                            char var = argRow->items[slices[1].first]->variableName;
                            double b = (var == 'y') ? context.y : context.x;
                            auto integrand = [&](double t) -> double {
                                EvaluationContext sub = context;
                                if (var == 'y') sub.y = t; else sub.x = t;
                                RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                                return fP.parseRowValue().asNumber();
                            };
                            return EvalValue(numericalIntegrate(integrand, 0.0, b));
                        }
                        char var = detectVarInSlice(slices[0].first, slices[0].second);
                        RowParser bP(argRow, context, slices[1].first, slices[1].second);
                        double b = bP.parseRowValue().asNumber();
                        auto integrand = [&](double t) -> double {
                            EvaluationContext sub = context;
                            if (var == 'y') sub.y = t; else sub.x = t;
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            return fP.parseRowValue().asNumber();
                        };
                        return EvalValue(numericalIntegrate(integrand, 0.0, b));
                    }
                    if (slices.size() == 3) {
                        bool secondIsVar = (slices[1].first < slices[1].second &&
                                            argRow->items[slices[1].first]->type == ItemType::Variable);
                        if (secondIsVar) {
                            char var = argRow->items[slices[1].first]->variableName;
                            RowParser bP(argRow, context, slices[2].first, slices[2].second);
                            double b = bP.parseRowValue().asNumber();
                            auto integrand = [&](double t) -> double {
                                EvaluationContext sub = context;
                                if (var == 'y') sub.y = t; else sub.x = t;
                                RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                                return fP.parseRowValue().asNumber();
                            };
                            return EvalValue(numericalIntegrate(integrand, 0.0, b));
                        }
                        char var = detectVarInSlice(slices[0].first, slices[0].second);
                        RowParser aP(argRow, context, slices[1].first, slices[1].second);
                        RowParser bP(argRow, context, slices[2].first, slices[2].second);
                        double a = aP.parseRowValue().asNumber();
                        double b = bP.parseRowValue().asNumber();
                        auto integrand = [&](double t) -> double {
                            EvaluationContext sub = context;
                            if (var == 'y') sub.y = t; else sub.x = t;
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            return fP.parseRowValue().asNumber();
                        };
                        return EvalValue(numericalIntegrate(integrand, a, b));
                    }
                    if (slices.size() == 4) {
                        char var = 'x';
                        if (slices[1].first < slices[1].second &&
                            argRow->items[slices[1].first]->type == ItemType::Variable) {
                            var = argRow->items[slices[1].first]->variableName;
                        }
                        RowParser aP(argRow, context, slices[2].first, slices[2].second);
                        RowParser bP(argRow, context, slices[3].first, slices[3].second);
                        double a = aP.parseRowValue().asNumber();
                        double b = bP.parseRowValue().asNumber();
                        auto integrand = [&](double t) -> double {
                            EvaluationContext sub = context;
                            if (var == 'y') sub.y = t; else sub.x = t;
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            return fP.parseRowValue().asNumber();
                        };
                        return EvalValue(numericalIntegrate(integrand, a, b));
                    }
                    throw std::runtime_error("integrate expects (f, a, b) or (f, var, a, b)");
                }
                if (functionId == SciDiff) {
                    auto detectVarInSlice = [&](size_t start, size_t finish) -> char {
                        for (size_t k = start; k < finish; ++k) {
                            if (argRow->items[k]->type == ItemType::Variable)
                                return argRow->items[k]->variableName;
                        }
                        return 'x';
                    };
                    if (slices.size() == 1) {
                        char var = detectVarInSlice(slices[0].first, slices[0].second);
                        auto subRow = std::make_unique<Row>();
                        for (size_t k = slices[0].first; k < slices[0].second; ++k) {
                            subRow->items.push_back(copyItem(argRow->items[k].get()));
                        }
                        double pt = (var == 'y') ? context.y : context.x;
                        if (pt != 0.0) {
                            return EvalValue(evalDerivativeAtPoint(subRow.get(), pt, var, context));
                        }
                        std::string res = symbolicDifferentiate(subRow.get(), var);
                        return EvalValue(res);
                    }
                    if (slices.size() == 2) {
                        bool secondIsVar = (slices[1].first < slices[1].second &&
                                            argRow->items[slices[1].first]->type == ItemType::Variable);
                        if (secondIsVar) {
                            char var = argRow->items[slices[1].first]->variableName;
                            double x0 = (var == 'y') ? context.y : context.x;
                            auto f = [&](double t) -> double {
                                EvaluationContext sub = context;
                                if (var == 'y') sub.y = t; else sub.x = t;
                                RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                                return fP.parseRowValue().asNumber();
                            };
                            return EvalValue(numericalDiff(f, x0));
                        }
                        char var = detectVarInSlice(slices[0].first, slices[0].second);
                        RowParser x0P(argRow, context, slices[1].first, slices[1].second);
                        double x0 = x0P.parseRowValue().asNumber();
                        auto f = [&](double t) -> double {
                            EvaluationContext sub = context;
                            if (var == 'y') sub.y = t; else sub.x = t;
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            return fP.parseRowValue().asNumber();
                        };
                        return EvalValue(numericalDiff(f, x0));
                    }
                    if (slices.size() == 3) {
                        char var = 'x';
                        if (slices[1].first < slices[1].second &&
                            argRow->items[slices[1].first]->type == ItemType::Variable) {
                            var = argRow->items[slices[1].first]->variableName;
                        }
                        RowParser x0P(argRow, context, slices[2].first, slices[2].second);
                        double x0 = x0P.parseRowValue().asNumber();
                        auto f = [&](double t) -> double {
                            EvaluationContext sub = context;
                            if (var == 'y') sub.y = t; else sub.x = t;
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            return fP.parseRowValue().asNumber();
                        };
                        return EvalValue(numericalDiff(f, x0));
                    }
                    throw std::runtime_error("diff expects (f, x0), (f, var, x0), or (f)");
                }
                if (functionId == SciLimit) {
                    if (slices.size() >= 2) {
                        RowParser cP(argRow, context, slices[1].first, slices[1].second);
                        double c = cP.parseRowValue().asNumber();
                        int dir = 0;
                        if (slices.size() >= 3) {
                            RowParser dP(argRow, context, slices[2].first, slices[2].second);
                            dir = (dP.parseRowValue().asNumber() < 0) ? -1 : 1;
                        }
                        auto f = [&](double t) -> double {
                            EvaluationContext sub = context;
                            sub.x = t;
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            return fP.parseRowValue().asNumber();
                        };
                        return EvalValue(numericalLimit(f, c, dir));
                    }
                    throw std::runtime_error("limit expects (f, c) or (f, c, dir)");
                }
                if (functionId == SciSum) {
                    if (slices.size() == 3) {
                        RowParser aP(argRow, context, slices[1].first, slices[1].second);
                        RowParser bP(argRow, context, slices[2].first, slices[2].second);
                        long long a = static_cast<long long>(std::round(aP.parseRowValue().asNumber()));
                        long long b = static_cast<long long>(std::round(bP.parseRowValue().asNumber()));
                        if (std::abs(b - a) > 5000000) throw std::runtime_error("Sum range too large");
                        double total = 0.0;
                        for (long long k = a; k <= b; ++k) {
                            EvaluationContext sub = context;
                            sub.x = static_cast<double>(k);
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            total += fP.parseRowValue().asNumber();
                        }
                        return EvalValue(total);
                    }
                    if (slices.size() == 4) {
                        char var = 'x';
                        if (slices[1].first < slices[1].second &&
                            argRow->items[slices[1].first]->type == ItemType::Variable) {
                            var = argRow->items[slices[1].first]->variableName;
                        }
                        RowParser aP(argRow, context, slices[2].first, slices[2].second);
                        RowParser bP(argRow, context, slices[3].first, slices[3].second);
                        long long a = static_cast<long long>(std::round(aP.parseRowValue().asNumber()));
                        long long b = static_cast<long long>(std::round(bP.parseRowValue().asNumber()));
                        if (std::abs(b - a) > 5000000) throw std::runtime_error("Sum range too large");
                        double total = 0.0;
                        for (long long k = a; k <= b; ++k) {
                            EvaluationContext sub = context;
                            if (var == 'y') sub.y = static_cast<double>(k);
                            else sub.x = static_cast<double>(k);
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            total += fP.parseRowValue().asNumber();
                        }
                        return EvalValue(total);
                    }
                    throw std::runtime_error("sum expects (f, a, b)");
                }
                if (functionId == SciProduct) {
                    if (slices.size() == 3) {
                        RowParser aP(argRow, context, slices[1].first, slices[1].second);
                        RowParser bP(argRow, context, slices[2].first, slices[2].second);
                        long long a = static_cast<long long>(std::round(aP.parseRowValue().asNumber()));
                        long long b = static_cast<long long>(std::round(bP.parseRowValue().asNumber()));
                        if (std::abs(b - a) > 5000000) throw std::runtime_error("Product range too large");
                        double total = 1.0;
                        for (long long k = a; k <= b; ++k) {
                            EvaluationContext sub = context;
                            sub.x = static_cast<double>(k);
                            RowParser fP(argRow, sub, slices[0].first, slices[0].second);
                            total *= fP.parseRowValue().asNumber();
                        }
                        return EvalValue(total);
                    }
                    throw std::runtime_error("product expects (f, a, b)");
                }

                // --- Matrix operations ---
                if (functionId == SciEye) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    size_t n = static_cast<size_t>(std::max(1.0, std::round(p.parseRowValue().asNumber())));
                    return EvalValue(matrixEye(n));
                }
                if (functionId == SciZeros) {
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    size_t r = static_cast<size_t>(std::max(1.0, std::round(p1.parseRowValue().asNumber())));
                    size_t c = r;
                    if (slices.size() >= 2) {
                        RowParser p2(argRow, context, slices[1].first, slices[1].second);
                        c = static_cast<size_t>(std::max(1.0, std::round(p2.parseRowValue().asNumber())));
                    }
                    return EvalValue(matrixZeros(r, c));
                }
                if (functionId == SciOnes) {
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    size_t r = static_cast<size_t>(std::max(1.0, std::round(p1.parseRowValue().asNumber())));
                    size_t c = r;
                    if (slices.size() >= 2) {
                        RowParser p2(argRow, context, slices[1].first, slices[1].second);
                        c = static_cast<size_t>(std::max(1.0, std::round(p2.parseRowValue().asNumber())));
                    }
                    return EvalValue(matrixOnes(r, c));
                }
                if (functionId == SciDet) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    EvalValue v = p.parseRowValue();
                    return EvalValue(v.isMatrix() ? matrixDet(v.mat) : v.num);
                }
                if (functionId == SciInv) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    EvalValue v = p.parseRowValue();
                    if (v.isMatrix()) return EvalValue(matrixInv(v.mat));
                    if (v.num == 0.0) throw std::runtime_error("Division by zero");
                    return EvalValue(1.0 / v.num);
                }
                if (functionId == SciTranspose) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    EvalValue v = p.parseRowValue();
                    return v.isMatrix() ? EvalValue(matrixTranspose(v.mat)) : v;
                }
                if (functionId == SciTrace) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    EvalValue v = p.parseRowValue();
                    return EvalValue(v.isMatrix() ? matrixTrace(v.mat) : v.num);
                }
                if (functionId == SciRank) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    EvalValue v = p.parseRowValue();
                    return EvalValue(v.isMatrix() ? static_cast<double>(matrixRank(v.mat)) : (v.num != 0.0 ? 1.0 : 0.0));
                }
                if (functionId == SciRref) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    EvalValue v = p.parseRowValue();
                    return v.isMatrix() ? EvalValue(matrixRref(v.mat)) : v;
                }
                if (functionId == SciNorm) {
                    RowParser p(argRow, context, slices[0].first, slices[0].second);
                    EvalValue v = p.parseRowValue();
                    return EvalValue(v.isMatrix() ? matrixNorm(v.mat) : std::fabs(v.num));
                }
                if (functionId == SciDot) {
                    if (slices.size() < 2) throw std::runtime_error("dot expects (u, v)");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    EvalValue v1 = p1.parseRowValue();
                    EvalValue v2 = p2.parseRowValue();
                    if (v1.isMatrix() && v2.isMatrix()) return EvalValue(vectorDot(v1.mat, v2.mat));
                    return EvalValue(v1.asNumber() * v2.asNumber());
                }
                if (functionId == SciCross) {
                    if (slices.size() < 2) throw std::runtime_error("cross expects (u, v)");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    EvalValue v1 = p1.parseRowValue();
                    EvalValue v2 = p2.parseRowValue();
                    if (v1.isMatrix() && v2.isMatrix()) return EvalValue(vectorCross(v1.mat, v2.mat));
                    throw std::runtime_error("cross product requires 3D vectors");
                }

                // --- Statistics ---
                if (functionId == SciMean || functionId == SciMedian || functionId == SciStddev ||
                    functionId == SciVar || functionId == SciMin || functionId == SciMax) {
                    std::vector<double> vals;
                    if (slices.size() == 1) {
                        RowParser p(argRow, context, slices[0].first, slices[0].second);
                        EvalValue v = p.parseRowValue();
                        if (v.isMatrix()) vals = v.mat.data;
                        else vals.push_back(v.num);
                    } else {
                        for (auto& sl : slices) {
                            RowParser p(argRow, context, sl.first, sl.second);
                            EvalValue v = p.parseRowValue();
                            if (v.isMatrix()) {
                                for (double d : v.mat.data) vals.push_back(d);
                            } else {
                                vals.push_back(v.num);
                            }
                        }
                    }
                    if (vals.empty()) throw std::runtime_error("Empty data for statistical function");
                    if (functionId == SciMin) return EvalValue(*std::min_element(vals.begin(), vals.end()));
                    if (functionId == SciMax) return EvalValue(*std::max_element(vals.begin(), vals.end()));
                    if (functionId == SciMean) {
                        double sum = 0.0;
                        for (double x : vals) sum += x;
                        return EvalValue(sum / vals.size());
                    }
                    if (functionId == SciMedian) {
                        std::sort(vals.begin(), vals.end());
                        size_t n = vals.size();
                        if (n % 2 == 1) return EvalValue(vals[n / 2]);
                        return EvalValue((vals[n / 2 - 1] + vals[n / 2]) * 0.5);
                    }
                    if (functionId == SciVar || functionId == SciStddev) {
                        double sum = 0.0;
                        for (double x : vals) sum += x;
                        double mean = sum / vals.size();
                        double vsum = 0.0;
                        for (double x : vals) vsum += (x - mean) * (x - mean);
                        double var = vals.size() > 1 ? vsum / (vals.size() - 1) : 0.0;
                        return EvalValue(functionId == SciStddev ? std::sqrt(var) : var);
                    }
                }

                // --- Two-argument math / Number theory ---
                if (functionId == SciGcd) {
                    if (slices.size() < 2) throw std::runtime_error("gcd expects two arguments");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    return EvalValue(calcGcd(p1.parseRowValue().asNumber(), p2.parseRowValue().asNumber()));
                }
                if (functionId == SciLcm) {
                    if (slices.size() < 2) throw std::runtime_error("lcm expects two arguments");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    return EvalValue(calcLcm(p1.parseRowValue().asNumber(), p2.parseRowValue().asNumber()));
                }
                if (functionId == SciNcrFn) {
                    if (slices.size() < 2) throw std::runtime_error("nCr expects two arguments");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    return EvalValue(combination(p1.parseRowValue().asNumber(), p2.parseRowValue().asNumber()));
                }
                if (functionId == SciNprFn) {
                    if (slices.size() < 2) throw std::runtime_error("nPr expects two arguments");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    return EvalValue(permutation(p1.parseRowValue().asNumber(), p2.parseRowValue().asNumber()));
                }
                if (functionId == SciBeta) {
                    if (slices.size() < 2) throw std::runtime_error("beta expects two arguments");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    double a = p1.parseRowValue().asNumber();
                    double b = p2.parseRowValue().asNumber();
                    if (a <= 0.0 || b <= 0.0) throw std::runtime_error("beta needs positive inputs");
                    return EvalValue(std::exp(std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b)));
                }
                if (functionId == SciHypot) {
                    if (slices.size() < 2) throw std::runtime_error("hypot expects two arguments");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    return EvalValue(std::hypot(p1.parseRowValue().asNumber(), p2.parseRowValue().asNumber()));
                }
                if (functionId == SciAtan2) {
                    if (slices.size() < 2) throw std::runtime_error("atan2 expects two arguments (y, x)");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    double y = p1.parseRowValue().asNumber();
                    double x = p2.parseRowValue().asNumber();
                    return EvalValue(context.degrees ? std::atan2(y, x) * (180.0 / kSciPi) : std::atan2(y, x));
                }
                if (functionId == SciClamp) {
                    if (slices.size() < 3) throw std::runtime_error("clamp expects three arguments (val, lo, hi)");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    RowParser p3(argRow, context, slices[2].first, slices[2].second);
                    double val = p1.parseRowValue().asNumber();
                    double lo = p2.parseRowValue().asNumber();
                    double hi = p3.parseRowValue().asNumber();
                    return EvalValue(std::clamp(val, lo, hi));
                }
                if (functionId == SciLerp) {
                    if (slices.size() < 3) throw std::runtime_error("lerp expects three arguments (a, b, t)");
                    RowParser p1(argRow, context, slices[0].first, slices[0].second);
                    RowParser p2(argRow, context, slices[1].first, slices[1].second);
                    RowParser p3(argRow, context, slices[2].first, slices[2].second);
                    double a = p1.parseRowValue().asNumber();
                    double b = p2.parseRowValue().asNumber();
                    double t = p3.parseRowValue().asNumber();
                    return EvalValue(a + t * (b - a));
                }

                // --- Standard 1-argument functions ---
                RowParser singleP(argRow, context, slices[0].first, slices[0].second);
                EvalValue v = singleP.parseRowValue();
                if (v.isNumber()) {
                    return EvalValue(applySciFunction(functionId, v.num, context.degrees));
                }
                Matrix m = v.mat;
                for (double& d : m.data) d = applySciFunction(functionId, d, context.degrees);
                return EvalValue(m);
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

    EvalValue parseFactorValue() {
        bool neg = false;
        while (peekIsOperatorChar('-') || peekIsOperatorChar('+')) {
            if (peekIsOperatorChar('-')) neg = !neg;
            pos++;
        }
        EvalValue v = parseAtomValue();
        while (peekIsOperatorChar('!')) {
            pos++;
            if (v.isNumber()) {
                v = EvalValue(factorial(v.num));
            } else {
                for (double& d : v.mat.data) d = factorial(d);
            }
        }
        return neg ? evalValueSub(EvalValue(0.0), v) : v;
    }

    EvalValue parseTermValue() {
        EvalValue v = parseFactorValue();
        for (;;) {
            if (peekIsOperatorChar('*')) {
                pos++;
                EvalValue next = parseFactorValue();
                if (v.isSet() && next.isSet()) {
                    v = EvalValue(setProduct(v.asSet(), next.asSet()));
                } else {
                    v = evalValueMul(v, next);
                }
                continue;
            }
            if (peekIsOperatorChar('%')) {
                pos++;
                EvalValue div = parseFactorValue();
                if (v.isNumber() && div.isNumber()) {
                    if (div.num == 0.0) throw std::runtime_error("Division by zero");
                    v = EvalValue(std::fmod(v.num, div.num));
                } else {
                    throw std::runtime_error("Modulo requires numbers");
                }
                continue;
            }
            const Item* nxt = peek();
            if (nxt && nxt->type != ItemType::Operator) {
                v = evalValueMul(v, parseFactorValue());
                continue;
            }
            break;
        }
        return v;
    }

    EvalValue parseRowValue() {
        if (atEnd()) return EvalValue(0.0);
        size_t firstTermStart = pos;
        EvalValue v = parseTermValue();
        for (;;) {
            if (peekIsOperatorChar('+')) {
                pos++;
                v = evalValueAdd(v, parseTermValue());
            } else if (peekIsOperatorChar('U')) {
                pos++;
                EvalValue next = parseTermValue();
                if (v.isSet() && next.isSet()) v = EvalValue(setUnion(v.asSet(), next.asSet()));
                else throw std::runtime_error("Union requires sets");
            } else if (peekIsOperatorChar('I')) {
                pos++;
                EvalValue next = parseTermValue();
                if (v.isSet() && next.isSet()) v = EvalValue(setIntersection(v.asSet(), next.asSet()));
                else throw std::runtime_error("Intersection requires sets");
            } else if (peekIsOperatorChar('D')) {
                pos++;
                EvalValue next = parseTermValue();
                if (v.isSet() && next.isSet()) v = EvalValue(setDelta(v.asSet(), next.asSet()));
                else throw std::runtime_error("Delta requires sets");
            } else if (peekIsOperatorChar('-')) {
                size_t secondTermStart = pos + 1;
                if (secondTermStart < items.size() &&
                    isOversizedFactorialRange(items, firstTermStart, pos)) {
                    size_t termLength = pos - firstTermStart;
                    size_t scan = secondTermStart + termLength;
                    if (scan <= items.size() &&
                        sameRange(items, firstTermStart, pos, secondTermStart, scan)) {
                        pos = scan;
                        v = EvalValue(0.0);
                        firstTermStart = pos;
                        continue;
                    }
                }
                pos++;
                v = evalValueSub(v, parseTermValue());
            } else {
                break;
            }
        }
        if (!atEnd())
            throw std::runtime_error("Malformed expression");
        return v;
    }

    double parseAtom() { return parseAtomValue().asNumber(); }
    double parseFactor() { return parseFactorValue().asNumber(); }
    double parseTerm() { return parseTermValue().asNumber(); }
    double parseRow() { return parseRowValue().asNumber(); }
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
            case ItemType::Integral:
            case ItemType::Derivative:
                throw std::runtime_error("Calculus not supported in Pro Big mode");
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
            case ItemType::Integral:
            case ItemType::Derivative:
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
            case ItemType::Integral:
            case ItemType::Derivative:
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

EvalValue evaluateValue(const Row* root, const EvaluationContext& context) {
    if (!root) return EvalValue(0.0);
    if (root->items.size() == 5 &&
        root->items[2]->type == ItemType::Operator && root->items[2]->opChar == '-' &&
        isOversizedFactorialRange(root->items, 0, 2) &&
        sameRange(root->items, 0, 2, 3, 5)) {
        return EvalValue(0.0);
    }
    RowParser p(root, context);
    return p.parseRowValue();
}

EvalValue evaluateValue(const Row* root) {
    return evaluateValue(root, EvaluationContext{});
}

double evaluate(const Row* root, const EvaluationContext& context) {
    return evaluateValue(root, context).asNumber();
}

double evaluate(const Row* root) {
    return evaluate(root, EvaluationContext{});
}

std::string evaluateToString(const Row* root, const EvaluationContext& context) {
    try {
        EvalValue val = evaluateValue(root, context);
        if (val.isSymbolic()) return val.text;
        if (val.isSet()) return val.setVal.toString();
        if (val.isMatrix()) return val.mat.toString();
        double v = val.num;
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
        EvalValue val = evaluateValue(root, context);
        if (val.isSymbolic()) return val.text;
        if (val.isSet()) return val.setVal.toString();
        if (val.isMatrix()) return val.mat.toString();
        double value = val.num;
        bigNeeded = !std::isfinite(value);
        if (!bigNeeded && value == 0.0) {
            try {
                BigValue big = bigEvaluate(root, context);
                if (!big.isZero) return formatBigValue(big);
            } catch (const std::exception&) {
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
