// graph.cpp
//
// Live 2D interactive graphing engine for NaturalCalculator.
// Renders explicit functions y = f(x) and implicit 2D equations F(x, y) = 0
// with smooth curves, marching-squares contouring, auto-grid, pan, zoom,
// and hover coordinates.

#include "graph.h"
#include "gpu_graph.h"
#include "graph_raster.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace {

std::string serializeRow(const Row* r) {
    if (!r) return "";
    return rowRangeToPlainString(r, 0, (int)r->items.size());
}

std::unique_ptr<Row> cloneRowSlice(const Row* sourceRow, size_t start, size_t end) {
    std::string text = rowRangeToPlainString(sourceRow, (int)start, (int)end);
    Expression expr;
    insertFromText(expr, text);
    return std::move(expr.root);
}

int countVarInItem(const Item* it, char v);

int countVar(const Row* r, char v) {
    if (!r) return 0;
    int count = 0;
    for (const auto& it : r->items) {
        count += countVarInItem(it.get(), v);
    }
    return count;
}

int countVarInItem(const Item* it, char v) {
    if (!it) return 0;
    int count = 0;
    if (it->type == ItemType::Variable && it->variableName == v) count++;
    if (it->a) count += countVar(it->a.get(), v);
    if (it->b) count += countVar(it->b.get(), v);
    if (it->c) count += countVar(it->c.get(), v);
    if (it->d) count += countVar(it->d.get(), v);
    return count;
}

void collectVars(const Row* row, bool& hasX, bool& hasY) {
    if (!row) return;
    for (const auto& it : row->items) {
        if (it->type == ItemType::Variable) {
            if (it->variableName == 'x') hasX = true;
            if (it->variableName == 'y') hasY = true;
        }
        if (it->a) collectVars(it->a.get(), hasX, hasY);
        if (it->b) collectVars(it->b.get(), hasX, hasY);
    }
}

} // namespace

namespace {

// UTF-8 superscript digits for an exponent, e.g. -12 -> "⁻¹²".
std::string superscriptInt(int e) {
    static const char* const kDigits[10] = {
        "\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4",
        "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9" };
    std::string out;
    if (e < 0) {
        out += "\xE2\x81\xBB";
        e = -e;
    }
    for (char c : std::to_string(e)) out += kDigits[c - '0'];
    return out;
}

// "2.500" -> "2.5", "3.000" -> "3".
void trimFraction(std::string& s) {
    if (s.find('.') == std::string::npos) return;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
}

} // namespace

// Formats v so neighbouring multiples of `step` stay distinct at any zoom:
// fixed notation while it stays short, otherwise "m×10ⁿ" with just enough
// mantissa digits. Output is UTF-8 and negatives use a true minus sign.
std::string formatAxisNumber(double v, double step) {
    if (!std::isfinite(v)) return "?";
    const double av = std::fabs(v);
    if (!(step > 0.0) || !std::isfinite(step)) step = av > 0.0 ? av * 1e-6 : 1e-6;
    if (av < step * 1e-3 || av < 1e-300) return "0";
    const std::string sign = v < 0.0 ? "\xE2\x88\x92" : "";
    const int decimals = std::max(0, (int)std::ceil(-std::log10(step) - 1e-9));
    const int intDigits = av >= 1.0 ? (int)std::floor(std::log10(av)) + 1 : 1;
    char buf[64];
    if (av < 1e7 && av >= 1e-4 && intDigits + decimals <= 12) {
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, av);
        std::string s(buf);
        trimFraction(s);
        return s == "0" ? s : sign + s;
    }
    int sig = (int)std::floor(std::log10(av)) - (int)std::floor(std::log10(step)) + 1;
    sig = std::clamp(sig, 1, 15);
    std::snprintf(buf, sizeof(buf), "%.*e", sig - 1, av);
    std::string s(buf);
    size_t ePos = s.find('e');
    std::string mantissa = s.substr(0, ePos);
    int exponent = ePos == std::string::npos ? 0 : std::atoi(s.c_str() + ePos + 1);
    trimFraction(mantissa);
    return sign + mantissa + "\xC3\x97" "10" + superscriptInt(exponent);
}

std::string formatNumberShort(double v) {
    return formatAxisNumber(v, 0.01);
}

double chooseStep(double range, int targetDivisions) {
    if (range <= 0.0 || !std::isfinite(range)) return 1.0;
    if (targetDivisions <= 0) targetDivisions = 8;
    double rawStep = range / targetDivisions;
    if (rawStep <= 0.0 || !std::isfinite(rawStep)) return 1.0;
    double mag = std::pow(10.0, std::floor(std::log10(rawStep)));
    if (mag <= 0.0 || !std::isfinite(mag)) return 1.0;
    double normalized = rawStep / mag;
    double step;
    if (normalized < 1.5) step = 1.0 * mag;
    else if (normalized < 3.5) step = 2.0 * mag;
    else if (normalized < 7.5) step = 5.0 * mag;
    else step = 10.0 * mag;
    return (step > 0.0 && std::isfinite(step)) ? step : 1.0;
}

GraphPalette getGraphPalette(const Theme& theme) {
    GraphPalette p;
    p.bg = theme.isDark ? RGB(0x18, 0x1A, 0x1F) : RGB(0xF8, 0xF9, 0xFA);
    p.grid = theme.isDark ? RGB(0x28, 0x2D, 0x36) : RGB(0xEB, 0xEE, 0xF2);
    p.axis = theme.isDark ? RGB(0x5A, 0x62, 0x70) : RGB(0x9E, 0xA8, 0xB6);
    p.text = theme.isDark ? RGB(0x8C, 0x94, 0xA0) : RGB(0x73, 0x7D, 0x8C);
    p.curve = theme.accent;
    return p;
}

static bool mapSciFunctionToByteOp(int fnId, ByteOp& outOp) {
    switch (fnId) {
        case SciSin: outOp = ByteOp::Sin; return true;
        case SciCos: outOp = ByteOp::Cos; return true;
        case SciTan: outOp = ByteOp::Tan; return true;
        case SciAsin: outOp = ByteOp::Asin; return true;
        case SciAcos: outOp = ByteOp::Acos; return true;
        case SciAtan: outOp = ByteOp::Atan; return true;
        case SciSec: outOp = ByteOp::Sec; return true;
        case SciCsc: outOp = ByteOp::Csc; return true;
        case SciCot: outOp = ByteOp::Cot; return true;
        case SciAsec: outOp = ByteOp::Asec; return true;
        case SciAcsc: outOp = ByteOp::Acsc; return true;
        case SciAcot: outOp = ByteOp::Acot; return true;
        case SciSinh: outOp = ByteOp::Sinh; return true;
        case SciCosh: outOp = ByteOp::Cosh; return true;
        case SciTanh: outOp = ByteOp::Tanh; return true;
        case SciAsinh: outOp = ByteOp::Asinh; return true;
        case SciAcosh: outOp = ByteOp::Acosh; return true;
        case SciAtanh: outOp = ByteOp::Atanh; return true;
        case SciSech: outOp = ByteOp::Sech; return true;
        case SciCsch: outOp = ByteOp::Csch; return true;
        case SciCoth: outOp = ByteOp::Coth; return true;
        case SciAsech: outOp = ByteOp::Asech; return true;
        case SciAcsch: outOp = ByteOp::Acsch; return true;
        case SciAcoth: outOp = ByteOp::Acoth; return true;
        case SciExp: outOp = ByteOp::Exp; return true;
        case SciExpm1: outOp = ByteOp::Expm1; return true;
        case SciLn: outOp = ByteOp::Ln; return true;
        case SciLog: outOp = ByteOp::Log10; return true;
        case SciLog2: outOp = ByteOp::Log2; return true;
        case SciLog1p: outOp = ByteOp::Log1p; return true;
        case SciCbrt: outOp = ByteOp::Cbrt; return true;
        case SciAbs: outOp = ByteOp::Abs; return true;
        case SciFloor: outOp = ByteOp::Floor; return true;
        case SciCeil: outOp = ByteOp::Ceil; return true;
        case SciRound: outOp = ByteOp::Round; return true;
        case SciTrunc: outOp = ByteOp::Trunc; return true;
        case SciSgn: outOp = ByteOp::Sgn; return true;
        case SciFact: outOp = ByteOp::Fact; return true;
        default: return false;
    }
}

// One-argument functions the VM evaluates through the calculator's own
// sciFunctionValue, after domain checks that keep its throwing path cold.
static bool isSciCallFunction(int fnId) {
    switch (fnId) {
        case SciGamma: case SciLgamma: case SciErf: case SciErfc:
        case SciBesselJ0: case SciBesselJ1: case SciBesselY0: case SciBesselY1:
        case SciSinc: case SciLambertW: case SciDeg: case SciRad: case SciIsPrime:
            return true;
        default:
            return false;
    }
}

static double sciCall(int fnId, double x) {
    if (std::isnan(x)) return NAN;
    switch (fnId) {
        case SciGamma:
        case SciLgamma:
            if (x <= 0.0 && std::floor(x) == x) return NAN;
            break;
        case SciBesselY0:
        case SciBesselY1:
            if (!(x > 0.0)) return NAN;
            break;
        case SciLambertW:
            if (x < -0.36787944117144233) return NAN;
            break;
        default:
            break;
    }
    try {
        return sciFunctionValue(fnId, x, false);
    } catch (...) {
        return NAN;
    }
}

// Riemann zeta for real s != 1: Borwein's acceleration of the alternating
// Dirichlet eta series for s >= 1/2, the functional equation below that.
static double zetaReal(double s) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kTerms = 30;
    if (std::isnan(s) || s == 1.0) return NAN;
    if (std::fabs(s) < 1e-12) return -0.5 - 0.91893853320467274 * s;  // zeta(0) + zeta'(0) s
    if (s < 0.5) {
        return std::pow(2.0, s) * std::pow(kPi, s - 1.0) * std::sin(kPi * s * 0.5) *
               std::tgamma(1.0 - s) * zetaReal(1.0 - s);
    }
    if (s > 60.0) return 1.0 + std::pow(2.0, -s);
    static const std::vector<double> d = [] {
        std::vector<double> v(kTerms + 1);
        double term = 1.0 / kTerms, acc = term;
        v[0] = kTerms * acc;
        for (int i = 1; i <= kTerms; ++i) {
            term *= 4.0 * (kTerms + i - 1) * (kTerms - i + 1) / ((2.0 * i) * (2.0 * i - 1.0));
            acc += term;
            v[i] = kTerms * acc;
        }
        return v;
    }();
    double sum = 0.0;
    for (int k = 0; k < kTerms; ++k) {
        double term = (d[k] - d[kTerms]) / std::pow(k + 1.0, s);
        sum += (k & 1) ? -term : term;
    }
    return (-sum / d[kTerms]) / (1.0 - std::pow(2.0, 1.0 - s));
}

// nCr / nPr: exact for small non-negative integers, the Gamma-function
// extension otherwise so x! style curves stay continuous on the graph.
static double gammaRatio(double n, double r, bool binomial) {
    if (std::isnan(n) || std::isnan(r)) return NAN;
    if (n >= 0.0 && r >= 0.0 && std::floor(n) == n && std::floor(r) == r && n <= 1000.0) {
        if (r > n) return 0.0;
        double k = binomial ? std::min(r, n - r) : r;
        double result = 1.0;
        for (double i = 0.0; i < k; ++i) {
            result *= (n - i);
            if (binomial) result /= (i + 1.0);
        }
        return binomial ? std::round(result) : result;
    }
    double num = std::tgamma(n + 1.0);
    double den = std::tgamma(n - r + 1.0) * (binomial ? std::tgamma(r + 1.0) : 1.0);
    if (std::isinf(den) && std::isfinite(num)) return 0.0;
    return num / den;
}

static double gcdOrLcm(double a, double b, bool lcm) {
    if (!(std::fabs(a) < 9e15 && std::fabs(b) < 9e15)) return NAN;
    long long ia = std::llround(std::fabs(a));
    long long ib = std::llround(std::fabs(b));
    if (!lcm) return (double)std::gcd(ia, ib);
    if (ia == 0 || ib == 0) return 0.0;
    return (double)(ia / std::gcd(ia, ib)) * (double)ib;
}

static double binaryOp(ByteOp op, double a, double b) {
    switch (op) {
        case ByteOp::Atan2: return std::atan2(a, b);
        case ByteOp::Hypot: return std::hypot(a, b);
        case ByteOp::Beta:
            return (a > 0.0 && b > 0.0) ? std::exp(std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b)) : NAN;
        case ByteOp::Gcd: return gcdOrLcm(a, b, false);
        case ByteOp::Lcm: return gcdOrLcm(a, b, true);
        case ByteOp::Min: return (std::isnan(a) || std::isnan(b)) ? NAN : std::min(a, b);
        case ByteOp::Max: return (std::isnan(a) || std::isnan(b)) ? NAN : std::max(a, b);
        case ByteOp::Ncr: return gammaRatio(a, b, true);
        case ByteOp::Npr: return gammaRatio(a, b, false);
        default: return NAN;
    }
}

double BytecodeProgram::execute(double x, double y) const {
    if (!isValid || ops.empty()) return NAN;
    double stack[64];
    int sp = 0;
    size_t constIdx = 0;
    size_t fnIdx = 0;
    for (ByteOp op : ops) {
        switch (op) {
            case ByteOp::PushConst:
                if (sp < 64 && constIdx < consts.size()) stack[sp++] = consts[constIdx++];
                else return NAN;
                break;
            case ByteOp::PushVarX:
                if (sp < 64) stack[sp++] = x;
                else return NAN;
                break;
            case ByteOp::PushVarY:
                if (sp < 64) stack[sp++] = y;
                else return NAN;
                break;
            case ByteOp::Add:
                if (sp >= 2) { double b = stack[--sp]; stack[sp - 1] += b; }
                else return NAN;
                break;
            case ByteOp::Sub:
                if (sp >= 2) { double b = stack[--sp]; stack[sp - 1] -= b; }
                else return NAN;
                break;
            case ByteOp::Mul:
                if (sp >= 2) { double b = stack[--sp]; stack[sp - 1] *= b; }
                else return NAN;
                break;
            case ByteOp::Div:
                if (sp >= 2) {
                    double b = stack[--sp];
                    stack[sp - 1] = (b == 0.0) ? NAN : (stack[sp - 1] / b);
                } else return NAN;
                break;
            case ByteOp::Mod:
                if (sp >= 2) {
                    double b = stack[--sp];
                    stack[sp - 1] = (b == 0.0) ? NAN : std::fmod(stack[sp - 1], b);
                } else return NAN;
                break;
            case ByteOp::Neg:
                if (sp >= 1) stack[sp - 1] = -stack[sp - 1];
                else return NAN;
                break;
            case ByteOp::Pow:
                if (sp >= 2) {
                    double b = stack[--sp];
                    double a = stack[sp - 1];
                    if (a == 0.0) {
                        stack[sp - 1] = (b == 0.0) ? 1.0 : ((b < 0.0) ? NAN : 0.0);
                    } else if (a < 0.0 && std::floor(b) != b) {
                        stack[sp - 1] = NAN;
                    } else {
                        stack[sp - 1] = std::pow(a, b);
                    }
                } else return NAN;
                break;
            case ByteOp::Sqrt:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a < 0.0) ? NAN : std::sqrt(a);
                } else return NAN;
                break;
            case ByteOp::Cbrt:
                if (sp >= 1) stack[sp - 1] = std::cbrt(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Sin:
                if (sp >= 1) stack[sp - 1] = std::sin(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Cos:
                if (sp >= 1) stack[sp - 1] = std::cos(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Tan:
                if (sp >= 1) stack[sp - 1] = std::tan(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Asin:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a < -1.0 || a > 1.0) ? NAN : std::asin(a);
                } else return NAN;
                break;
            case ByteOp::Acos:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a < -1.0 || a > 1.0) ? NAN : std::acos(a);
                } else return NAN;
                break;
            case ByteOp::Atan:
                if (sp >= 1) stack[sp - 1] = std::atan(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Sec:
                if (sp >= 1) {
                    double c = std::cos(stack[sp - 1]);
                    stack[sp - 1] = (c == 0.0) ? NAN : (1.0 / c);
                } else return NAN;
                break;
            case ByteOp::Csc:
                if (sp >= 1) {
                    double s = std::sin(stack[sp - 1]);
                    stack[sp - 1] = (s == 0.0) ? NAN : (1.0 / s);
                } else return NAN;
                break;
            case ByteOp::Cot:
                if (sp >= 1) {
                    double t = std::tan(stack[sp - 1]);
                    stack[sp - 1] = (t == 0.0) ? NAN : (1.0 / t);
                } else return NAN;
                break;
            case ByteOp::Asec:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (std::fabs(a) < 1.0) ? NAN : std::acos(1.0 / a);
                } else return NAN;
                break;
            case ByteOp::Acsc:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (std::fabs(a) < 1.0) ? NAN : std::asin(1.0 / a);
                } else return NAN;
                break;
            case ByteOp::Acot:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    constexpr double kPi = 3.14159265358979323846;
                    stack[sp - 1] = (a == 0.0) ? (kPi * 0.5) : std::atan(1.0 / a);
                } else return NAN;
                break;
            case ByteOp::Sinh:
                if (sp >= 1) stack[sp - 1] = std::sinh(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Cosh:
                if (sp >= 1) stack[sp - 1] = std::cosh(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Tanh:
                if (sp >= 1) stack[sp - 1] = std::tanh(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Asinh:
                if (sp >= 1) stack[sp - 1] = std::asinh(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Acosh:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a < 1.0) ? NAN : std::acosh(a);
                } else return NAN;
                break;
            case ByteOp::Atanh:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (std::fabs(a) >= 1.0) ? NAN : std::atanh(a);
                } else return NAN;
                break;
            case ByteOp::Sech:
                if (sp >= 1) stack[sp - 1] = 1.0 / std::cosh(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Csch:
                if (sp >= 1) {
                    double s = std::sinh(stack[sp - 1]);
                    stack[sp - 1] = (s == 0.0) ? NAN : (1.0 / s);
                } else return NAN;
                break;
            case ByteOp::Coth:
                if (sp >= 1) {
                    double t = std::tanh(stack[sp - 1]);
                    stack[sp - 1] = (t == 0.0) ? NAN : (1.0 / t);
                } else return NAN;
                break;
            case ByteOp::Asech:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a <= 0.0 || a > 1.0) ? NAN : std::acosh(1.0 / a);
                } else return NAN;
                break;
            case ByteOp::Acsch:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a == 0.0) ? NAN : std::asinh(1.0 / a);
                } else return NAN;
                break;
            case ByteOp::Acoth:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (std::fabs(a) <= 1.0) ? NAN : std::atanh(1.0 / a);
                } else return NAN;
                break;
            case ByteOp::Exp:
                if (sp >= 1) stack[sp - 1] = std::exp(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Expm1:
                if (sp >= 1) stack[sp - 1] = std::expm1(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Ln:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a <= 0.0) ? NAN : std::log(a);
                } else return NAN;
                break;
            case ByteOp::Log10:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a <= 0.0) ? NAN : std::log10(a);
                } else return NAN;
                break;
            case ByteOp::Log2:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a <= 0.0) ? NAN : std::log2(a);
                } else return NAN;
                break;
            case ByteOp::Log1p:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a <= -1.0) ? NAN : std::log1p(a);
                } else return NAN;
                break;
            case ByteOp::Abs:
                if (sp >= 1) stack[sp - 1] = std::fabs(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Floor:
                if (sp >= 1) stack[sp - 1] = std::floor(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Ceil:
                if (sp >= 1) stack[sp - 1] = std::ceil(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Round:
                if (sp >= 1) stack[sp - 1] = std::round(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Trunc:
                if (sp >= 1) stack[sp - 1] = std::trunc(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Sgn:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a > 0.0) ? 1.0 : ((a < 0.0) ? -1.0 : 0.0);
                } else return NAN;
                break;
            case ByteOp::Fact:
                if (sp >= 1) {
                    double a = stack[sp - 1];
                    stack[sp - 1] = (a < 0.0 && std::floor(a) == a) ? NAN : std::tgamma(a + 1.0);
                } else return NAN;
                break;
            case ByteOp::SciCall:
                if (sp >= 1 && fnIdx < fnIds.size()) stack[sp - 1] = sciCall(fnIds[fnIdx++], stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Zeta:
                if (sp >= 1) stack[sp - 1] = zetaReal(stack[sp - 1]);
                else return NAN;
                break;
            case ByteOp::Atan2:
            case ByteOp::Hypot:
            case ByteOp::Beta:
            case ByteOp::Gcd:
            case ByteOp::Lcm:
            case ByteOp::Min:
            case ByteOp::Max:
            case ByteOp::Ncr:
            case ByteOp::Npr:
                if (sp >= 2) {
                    double b = stack[--sp];
                    stack[sp - 1] = binaryOp(op, stack[sp - 1], b);
                } else return NAN;
                break;
            case ByteOp::Clamp:
            case ByteOp::Lerp:
                if (sp >= 3) {
                    double c = stack[--sp];
                    double b = stack[--sp];
                    double a = stack[sp - 1];
                    if (op == ByteOp::Clamp) stack[sp - 1] = a < b ? b : (a > c ? c : a);
                    else stack[sp - 1] = a + c * (b - a);
                } else return NAN;
                break;
        }
    }
    return (sp == 1) ? stack[0] : NAN;
}

namespace {

class BytecodeCompiler {
public:
    const std::vector<std::unique_ptr<Item>>& items;
    size_t pos = 0;
    size_t end = 0;
    BytecodeProgram prog;

    explicit BytecodeCompiler(const Row* row)
        : items(row->items), pos(0), end(row->items.size()) {}
    // Compiles only items [begin, finish) of row, e.g. one function argument.
    BytecodeCompiler(const Row* row, size_t begin, size_t finish)
        : items(row->items), pos(begin), end(finish) {}

    bool compile() {
        if (!compileRow()) return false;
        if (pos < end) return false;
        prog.isValid = true;
        return true;
    }

private:
    bool atEnd() const { return pos >= end; }
    const Item* peek() const { return atEnd() ? nullptr : items[pos].get(); }
    bool peekIsOperatorChar(char c) const {
        const Item* it = peek();
        return it && it->type == ItemType::Operator && it->opChar == c;
    }

    void appendProg(const BytecodeProgram& other) {
        for (ByteOp op : other.ops) prog.ops.push_back(op);
        for (double c : other.consts) prog.consts.push_back(c);
        for (int id : other.fnIds) prog.fnIds.push_back(id);
    }

    // Arguments are split at the row's top-level commas, mirroring the
    // evaluator; each compiles onto the stack in order, then the function op.
    bool compileFunction(const Item* it) {
        const Row* arg = it->a.get();
        if (!arg || arg->items.empty()) return false;
        std::vector<std::pair<size_t, size_t>> slices;
        size_t start = 0;
        for (size_t i = 0; i < arg->items.size(); ++i) {
            const Item* a = arg->items[i].get();
            if (a->type == ItemType::Operator && a->opChar == ',') {
                slices.emplace_back(start, i);
                start = i + 1;
            }
        }
        slices.emplace_back(start, arg->items.size());
        for (const auto& slice : slices) {
            if (slice.first >= slice.second) return false;
            BytecodeCompiler sub(arg, slice.first, slice.second);
            if (!sub.compile()) return false;
            appendProg(sub.prog);
        }

        const size_t count = slices.size();
        const int id = it->functionId;
        ByteOp op;
        if (count == 1) {
            if (mapSciFunctionToByteOp(id, op)) {
                prog.ops.push_back(op);
                return true;
            }
            if (id == SciZeta) {
                prog.ops.push_back(ByteOp::Zeta);
                return true;
            }
            if (isSciCallFunction(id)) {
                prog.ops.push_back(ByteOp::SciCall);
                prog.fnIds.push_back(id);
                return true;
            }
            return id == SciMin || id == SciMax || id == SciMean;  // a lone value is its own min/max/mean
        }
        switch (id) {
            case SciAtan2: op = ByteOp::Atan2; break;
            case SciHypot: op = ByteOp::Hypot; break;
            case SciBeta: op = ByteOp::Beta; break;
            case SciGcd: op = ByteOp::Gcd; break;
            case SciLcm: op = ByteOp::Lcm; break;
            case SciNcrFn: op = ByteOp::Ncr; break;
            case SciNprFn: op = ByteOp::Npr; break;
            case SciClamp:
            case SciLerp:
                if (count != 3) return false;
                prog.ops.push_back(id == SciClamp ? ByteOp::Clamp : ByteOp::Lerp);
                return true;
            case SciMin:
            case SciMax:
                for (size_t k = 1; k < count; ++k) prog.ops.push_back(id == SciMin ? ByteOp::Min : ByteOp::Max);
                return true;
            case SciMean:
                for (size_t k = 1; k < count; ++k) prog.ops.push_back(ByteOp::Add);
                prog.ops.push_back(ByteOp::PushConst);
                prog.consts.push_back((double)count);
                prog.ops.push_back(ByteOp::Div);
                return true;
            default:
                return false;
        }
        if (count != 2) return false;
        prog.ops.push_back(op);
        return true;
    }

    bool compileRow() {
        if (atEnd()) {
            prog.ops.push_back(ByteOp::PushConst);
            prog.consts.push_back(0.0);
            return true;
        }
        if (!compileTerm()) return false;
        while (!atEnd()) {
            if (peekIsOperatorChar('+')) {
                pos++;
                if (!compileTerm()) return false;
                prog.ops.push_back(ByteOp::Add);
            } else if (peekIsOperatorChar('-')) {
                pos++;
                if (!compileTerm()) return false;
                prog.ops.push_back(ByteOp::Sub);
            } else {
                break;
            }
        }
        return true;
    }

    bool compileTerm() {
        if (!compileFactor()) return false;
        while (!atEnd()) {
            if (peekIsOperatorChar('*')) {
                pos++;
                if (!compileFactor()) return false;
                prog.ops.push_back(ByteOp::Mul);
            } else if (peekIsOperatorChar('%')) {
                pos++;
                if (!compileFactor()) return false;
                prog.ops.push_back(ByteOp::Mod);
            } else {
                const Item* nxt = peek();
                if (nxt && nxt->type != ItemType::Operator &&
                    nxt->type != ItemType::Equals && nxt->type != ItemType::CloseParen) {
                    if (!compileFactor()) return false;
                    prog.ops.push_back(ByteOp::Mul);
                } else {
                    break;
                }
            }
        }
        return true;
    }

    bool compileFactor() {
        bool neg = false;
        while (peekIsOperatorChar('-') || peekIsOperatorChar('+')) {
            if (peekIsOperatorChar('-')) neg = !neg;
            pos++;
        }
        if (!compileAtom()) return false;
        while (peekIsOperatorChar('!')) {
            pos++;
            prog.ops.push_back(ByteOp::Fact);
        }
        if (neg) {
            prog.ops.push_back(ByteOp::Neg);
        }
        return true;
    }

    bool compileAtom() {
        if (atEnd()) return false;
        const Item* it = peek();
        pos++;
        switch (it->type) {
            case ItemType::Number: {
                // Adjacent digit runs form one number, as in the evaluator.
                std::string text = it->numText;
                while (!atEnd() && items[pos]->type == ItemType::Number) text += items[pos++]->numText;
                try {
                    double val = std::stod(text);
                    prog.ops.push_back(ByteOp::PushConst);
                    prog.consts.push_back(val);
                    return true;
                } catch (...) { return false; }
            }
            case ItemType::Constant: {
                double val = (it->constantName == 'p') ? 3.14159265358979323846 :
                             (it->constantName == 'e') ? 2.71828182845904523536 : 1.61803398874989484820;
                prog.ops.push_back(ByteOp::PushConst);
                prog.consts.push_back(val);
                return true;
            }
            case ItemType::Variable: {
                if (it->variableName == 'x') { prog.ops.push_back(ByteOp::PushVarX); return true; }
                if (it->variableName == 'y') { prog.ops.push_back(ByteOp::PushVarY); return true; }
                return false;
            }
            case ItemType::Fraction: {
                BytecodeCompiler nCompiler(it->a.get());
                if (!nCompiler.compile()) return false;
                BytecodeCompiler dCompiler(it->b.get());
                if (!dCompiler.compile()) return false;
                appendProg(nCompiler.prog);
                appendProg(dCompiler.prog);
                prog.ops.push_back(ByteOp::Div);
                return true;
            }
            case ItemType::Power: {
                BytecodeCompiler baseCompiler(it->a.get());
                if (!baseCompiler.compile()) return false;
                BytecodeCompiler expCompiler(it->b.get());
                if (!expCompiler.compile()) return false;
                appendProg(baseCompiler.prog);
                appendProg(expCompiler.prog);
                prog.ops.push_back(ByteOp::Pow);
                return true;
            }
            case ItemType::Sqrt: {
                BytecodeCompiler radCompiler(it->a.get());
                if (!radCompiler.compile()) return false;
                appendProg(radCompiler.prog);
                prog.ops.push_back(ByteOp::Sqrt);
                return true;
            }
            case ItemType::Paren: {
                if (it->isBrace || it->isBracket) return false;
                BytecodeCompiler innerCompiler(it->a.get());
                if (!innerCompiler.compile()) return false;
                appendProg(innerCompiler.prog);
                return true;
            }
            case ItemType::Function:
                return compileFunction(it);
            case ItemType::Permutation:
            case ItemType::Combination: {
                if (rowIsEmpty(it->a.get()) || rowIsEmpty(it->b.get())) return false;
                BytecodeCompiler nCompiler(it->a.get());
                BytecodeCompiler rCompiler(it->b.get());
                if (!nCompiler.compile() || !rCompiler.compile()) return false;
                appendProg(nCompiler.prog);
                appendProg(rCompiler.prog);
                prog.ops.push_back(it->type == ItemType::Permutation ? ByteOp::Npr : ByteOp::Ncr);
                return true;
            }
            default:
                return false;
        }
    }
};

} // namespace

bool compileRowToBytecode(const Row* row, BytecodeProgram& outProg) {
    if (!row) return false;
    BytecodeCompiler compiler(row);
    if (compiler.compile()) {
        outProg = std::move(compiler.prog);
        return true;
    }
    return false;
}

// Bytecode results are final, NaN included: re-running an undefined point
// through the throwing evaluator cost an exception per sample.
double GraphAnalysis::evalExplicit(double x) const {
    if (!isValid) return NAN;
    const BytecodeProgram& program = rightRow ? rightProgram : leftProgram;
    if (program.isValid) return program.execute(x, 0.0);
    EvaluationContext ctx = baseContext;
    ctx.degrees = false; // Cartesian mathematical graphing always operates in radians
    ctx.x = x;
    try {
        if (rightRow) return evaluate(rightRow.get(), ctx);
        if (leftRow) return evaluate(leftRow.get(), ctx);
    } catch (...) {}
    return NAN;
}

double GraphAnalysis::evalExplicitX(double y) const {
    if (!isValid) return NAN;
    const BytecodeProgram& program = rightRow ? rightProgram : leftProgram;
    if (program.isValid) return program.execute(0.0, y);
    EvaluationContext ctx = baseContext;
    ctx.degrees = false; // Cartesian mathematical graphing always operates in radians
    ctx.y = y;
    try {
        if (rightRow) return evaluate(rightRow.get(), ctx);
        if (leftRow) return evaluate(leftRow.get(), ctx);
    } catch (...) {}
    return NAN;
}

bool GraphAnalysis::hasFastEval() const {
    if (!isValid) return false;
    if (kind == GraphEquationKind::ImplicitXY)
        return leftProgram.isValid && (!rightRow || rightProgram.isValid);
    return rightRow ? rightProgram.isValid : leftProgram.isValid;
}

double GraphAnalysis::evalImplicit(double x, double y) const {
    if (!isValid) return NAN;
    if (leftProgram.isValid && (!rightRow || rightProgram.isValid)) {
        double l = leftProgram.execute(x, y);
        return rightRow ? l - rightProgram.execute(x, y) : l;
    }
    EvaluationContext ctx = baseContext;
    ctx.degrees = false; // Cartesian mathematical graphing always operates in radians
    ctx.x = x;
    ctx.y = y;
    try {
        double l = leftRow ? evaluate(leftRow.get(), ctx) : 0.0;
        double r = rightRow ? evaluate(rightRow.get(), ctx) : 0.0;
        return l - r;
    } catch (...) {}
    return NAN;
}

static GraphAnalysis analyzeGraphExpressionInner(const Row* root, const EvaluationContext& context) {
    GraphAnalysis result;
    result.baseContext = context;
    if (!root || root->items.empty()) return result;

    // Reject complex equations from the grapher to avoid artifacts
    bool hasI = false;
    auto checkI = [&](const Row* r, auto&& self) -> void {
        if (!r) return;
        for (const auto& it : r->items) {
            if (it->type == ItemType::Variable && it->variableName == 'i') hasI = true;
            if (it->type == ItemType::Name && it->nameText == "i") hasI = true;
            if (it->a) self(it->a.get(), self);
            if (it->b) self(it->b.get(), self);
            if (it->c) self(it->c.get(), self);
            if (it->d) self(it->d.get(), self);
        }
    };
    checkI(root, checkI);
    if (hasI) return result;

    // Check for equals
    size_t equalsPos = root->items.size();
    size_t equalsCount = 0;
    for (size_t i = 0; i < root->items.size(); ++i) {
        if (root->items[i]->type == ItemType::Equals) {
            equalsPos = i;
            equalsCount++;
        }
    }

    if (equalsCount > 1) return result;

    if (equalsCount == 1) {
        if (equalsPos == 0 || equalsPos + 1 >= root->items.size()) return result;

        result.leftRow = cloneRowSlice(root, 0, equalsPos);
        result.rightRow = cloneRowSlice(root, equalsPos + 1, root->items.size());

        std::string rawTitle = serializeRow(root);

        bool lx = false, ly = false, rx = false, ry = false;
        collectVars(result.leftRow.get(), lx, ly);
        collectVars(result.rightRow.get(), rx, ry);

        // Case 1: left side is a lone 'y', and right side has no 'y' -> ExplicitY
        if (result.leftRow->items.size() == 1 &&
            result.leftRow->items[0]->type == ItemType::Variable &&
            result.leftRow->items[0]->variableName == 'y' && !ry) {
            result.kind = GraphEquationKind::ExplicitY;
            result.isValid = true;
            result.title = rawTitle;
            return result;
        }

        // Case 2: right side is a lone 'y', and left side has no 'y' -> ExplicitY
        if (result.rightRow->items.size() == 1 &&
            result.rightRow->items[0]->type == ItemType::Variable &&
            result.rightRow->items[0]->variableName == 'y' && !ly) {
            std::swap(result.leftRow, result.rightRow);
            result.kind = GraphEquationKind::ExplicitY;
            result.isValid = true;
            result.title = rawTitle;
            return result;
        }

        // Case 3: left side is a lone 'x', and right side has no 'x' -> ExplicitX
        if (result.leftRow->items.size() == 1 &&
            result.leftRow->items[0]->type == ItemType::Variable &&
            result.leftRow->items[0]->variableName == 'x' && !rx) {
            result.kind = GraphEquationKind::ExplicitX;
            result.isValid = true;
            result.title = rawTitle;
            return result;
        }

        // Case 4: right side is a lone 'x', and left side has no 'x' -> ExplicitX
        if (result.rightRow->items.size() == 1 &&
            result.rightRow->items[0]->type == ItemType::Variable &&
            result.rightRow->items[0]->variableName == 'x' && !lx) {
            std::swap(result.leftRow, result.rightRow);
            result.kind = GraphEquationKind::ExplicitX;
            result.isValid = true;
            result.title = rawTitle;
            return result;
        }

        // Case 5: General implicit 2D equation F(x, y) = G(x, y) (e.g. x + y = tan(x), x^2 + y^2 = 25)
        // Strictly requires BOTH x and y across the equation!
        // Rejects 1-variable equations (e.g. x^2 - 7x + 12 = 0, x = 5) which are scalar algebra, not 2D planar curves.
        if ((lx || rx) && (ly || ry)) {
            result.kind = GraphEquationKind::ImplicitXY;
            result.isValid = true;
            result.title = rawTitle;
            return result;
        }
        return result;
    }

    // Reject incomplete trailing operators (e.g. "x + ", "y - ")
    if (!root->items.empty()) {
        const Item* last = root->items.back().get();
        if (last->type == ItemType::Operator && last->opChar != '!') return result;
    }

    // No equals: check variables in expression
    bool hasX = false, hasY = false;
    collectVars(root, hasX, hasY);

    if (hasX && !hasY) {
        // Single expression in x: interpreted as y = f(x)
        result.kind = GraphEquationKind::ExplicitY;
        result.isValid = true;
        result.rightRow = cloneRowSlice(root, 0, root->items.size());
        result.title = "y = " + serializeRow(result.rightRow.get());
        return result;
    }

    if (!hasX && hasY) {
        // Single expression in y: interpreted as x = f(y)
        result.kind = GraphEquationKind::ExplicitX;
        result.isValid = true;
        result.rightRow = cloneRowSlice(root, 0, root->items.size());
        result.title = "x = " + serializeRow(result.rightRow.get());
        return result;
    }

    if (hasX && hasY) {
        // Implicit f(x, y) = 0
        result.kind = GraphEquationKind::ImplicitXY;
        result.isValid = true;
        result.leftRow = cloneRowSlice(root, 0, root->items.size());
        result.title = serializeRow(result.leftRow.get()) + " = 0";
        return result;
    }

    return result;
}

GraphAnalysis analyzeGraphExpression(const Row* root, const EvaluationContext& context) {
    GraphAnalysis result = analyzeGraphExpressionInner(root, context);
    if (result.isValid) {
        if (result.leftRow) compileRowToBytecode(result.leftRow.get(), result.leftProgram);
        if (result.rightRow) compileRowToBytecode(result.rightRow.get(), result.rightProgram);
        // Header text in natural notation (sin⁻¹(x), x², 1/x ...).
        if (hasEquals(root)) result.displayTitle = rowToDisplayText(root);
        else if (result.kind == GraphEquationKind::ExplicitY) result.displayTitle = L"y = " + rowToDisplayText(result.rightRow.get());
        else if (result.kind == GraphEquationKind::ExplicitX) result.displayTitle = L"x = " + rowToDisplayText(result.rightRow.get());
        else result.displayTitle = rowToDisplayText(result.leftRow.get()) + L" = 0";
    }
    return result;
}

void GraphState::resetView(double range, int pxW, int pxH) {
    minX = -range;
    maxX = range;
    minY = -range;
    maxY = range;
    initialized = true;
    pixelW = 0;
    pixelH = 0;
    if (pxW > 0 && pxH > 0) fitAspect(pxW, pxH);
}

void GraphState::fitAspect(int pxW, int pxH) {
    if (pxW <= 0 || pxH <= 0) return;
    const double spanX = maxX - minX, spanY = maxY - minY;
    if (!(spanX > 0.0) || !(spanY > 0.0)) return;
    double unitsPerPx;
    if (pixelW > 0 && pixelH > 0 && (pixelW != pxW || pixelH != pxH)) {
        unitsPerPx = spanX / pixelW;  // panel resized: keep center and scale
    } else {
        const double ux = spanX / pxW, uy = spanY / pxH;
        if (pixelW == pxW && pixelH == pxH && std::fabs(ux - uy) <= 1e-9 * std::max(ux, uy)) return;
        unitsPerPx = std::max(ux, uy);  // first fit: keep both requested ranges visible
    }
    const double cx = 0.5 * (minX + maxX), cy = 0.5 * (minY + maxY);
    minX = cx - 0.5 * unitsPerPx * pxW;
    maxX = cx + 0.5 * unitsPerPx * pxW;
    minY = cy - 0.5 * unitsPerPx * pxH;
    maxY = cy + 0.5 * unitsPerPx * pxH;
    pixelW = pxW;
    pixelH = pxH;
}

GraphView GraphState::view(int pxW, int pxH) const {
    GraphView v;
    v.minX = minX;
    v.maxX = maxX;
    v.minY = minY;
    v.maxY = maxY;
    v.w = pxW;
    v.h = pxH;
    return v;
}

void GraphState::zoom(double factor, double centerMathX, double centerMathY) {
    if (factor <= 0.0 || !std::isfinite(factor)) return;
    double curW = maxX - minX;
    double curH = maxY - minY;
    if (curW <= 0.0 || curH <= 0.0) return;

    // FP64 limits: the view never shrinks below 1e-10 of its coordinates'
    // magnitude (a pixel then still spans hundreds of double ulps), and never
    // grows beyond 1e12 units.
    const double magnitude = std::max({ 1.0, std::fabs(minX), std::fabs(maxX), std::fabs(minY), std::fabs(maxY) });
    factor = std::max(factor, 1e-10 * magnitude / std::min(curW, curH));
    factor = std::min(factor, 1e12 / std::max(curW, curH));
    if (!(factor > 0.0)) return;
    double newW = curW * factor;
    double newH = curH * factor;

    double xRatio = (centerMathX - minX) / curW;
    double yRatio = (centerMathY - minY) / curH;
    minX = centerMathX - newW * xRatio;
    maxX = minX + newW;
    minY = centerMathY - newH * yRatio;
    maxY = minY + newH;
}

void GraphState::pan(double dxPixels, double dyPixels, int pixelWidth, int pixelHeight) {
    if (pixelWidth <= 0 || pixelHeight <= 0) return;
    double mathDx = (dxPixels / pixelWidth) * (maxX - minX);
    double mathDy = (dyPixels / pixelHeight) * (maxY - minY);
    if (!std::isfinite(mathDx) || !std::isfinite(mathDy)) return;
    constexpr double kMaxCoord = 1e13;
    if (std::fabs(minX - mathDx) > kMaxCoord || std::fabs(maxX - mathDx) > kMaxCoord) return;
    if (std::fabs(minY + mathDy) > kMaxCoord || std::fabs(maxY + mathDy) > kMaxCoord) return;
    minX -= mathDx;
    maxX -= mathDx;
    minY += mathDy;
    maxY += mathDy;
}

POINT GraphState::mathToPixel(double mx, double my, const RECT& rect) const {
    double w = rect.right - rect.left;
    double h = rect.bottom - rect.top;
    int px = rect.left + (int)std::lround((mx - minX) / (maxX - minX) * w);
    int py = rect.bottom - (int)std::lround((my - minY) / (maxY - minY) * h);
    return { px, py };
}

void GraphState::pixelToMath(int px, int py, const RECT& rect, double& mx, double& my) const {
    double w = rect.right - rect.left;
    double h = rect.bottom - rect.top;
    mx = minX + ((px - rect.left) / w) * (maxX - minX);
    my = minY + ((rect.bottom - py) / h) * (maxY - minY);
}

void graphButtonRects(const RECT& graphRect, RECT& resetBtn, RECT& zoomOutBtn, RECT& zoomInBtn) {
    const int size = 24, gap = 6, resetW = 52;
    const int top = graphRect.top + 10;
    const int right = graphRect.right - 10;
    zoomInBtn = { right - size, top, right, top + size };
    zoomOutBtn = { zoomInBtn.left - gap - size, top, zoomInBtn.left - gap, top + size };
    resetBtn = { zoomOutBtn.left - gap - resetW, top, zoomOutBtn.left - gap, top + size };
}

bool handleGraphMouseDown(GraphState& state, const RECT& graphRect, int x, int y) {
    POINT pt = { x, y };
    if (!PtInRect(&graphRect, pt)) return false;

    RECT resetBtn, zoomOutBtn, zoomInBtn;
    graphButtonRects(graphRect, resetBtn, zoomOutBtn, zoomInBtn);
    if (PtInRect(&zoomInBtn, pt)) {
        state.zoom(0.75, (state.minX + state.maxX) * 0.5, (state.minY + state.maxY) * 0.5);
        return true;
    }
    if (PtInRect(&zoomOutBtn, pt)) {
        state.zoom(1.333333, (state.minX + state.maxX) * 0.5, (state.minY + state.maxY) * 0.5);
        return true;
    }
    if (PtInRect(&resetBtn, pt)) {
        state.resetView(10.0, graphRect.right - graphRect.left, graphRect.bottom - graphRect.top);
        return true;
    }

    state.isDragging = true;
    state.lastMousePos = pt;
    return true;
}

bool handleGraphMouseMove(GraphState& state, const RECT& graphRect, int x, int y) {
    POINT pt = { x, y };
    if (state.isRightProbing) {
        state.probePos = { std::clamp(x - (int)graphRect.left, 0, (int)(graphRect.right - graphRect.left)),
                           std::clamp(y - (int)graphRect.top, 0, (int)(graphRect.bottom - graphRect.top)) };
        RECT localRect = { 0, 0, graphRect.right - graphRect.left, graphRect.bottom - graphRect.top };
        state.pixelToMath(state.probePos.x, state.probePos.y, localRect, state.probeMathX, state.probeMathY);
        return true;
    }
    if (state.isDragging) {
        int dx = x - state.lastMousePos.x;
        int dy = y - state.lastMousePos.y;
        state.pan(dx, dy, graphRect.right - graphRect.left, graphRect.bottom - graphRect.top);
        state.lastMousePos = pt;
        return true;
    }
    // Hover is tracked but never drawn, so it must not trigger a repaint.
    if (PtInRect(&graphRect, pt)) {
        state.isHovering = true;
        state.hoverPos = { x - graphRect.left, y - graphRect.top };
        RECT localRect = { 0, 0, graphRect.right - graphRect.left, graphRect.bottom - graphRect.top };
        state.pixelToMath(state.hoverPos.x, state.hoverPos.y, localRect, state.hoverMathX, state.hoverMathY);
    } else {
        state.isHovering = false;
    }
    return false;
}

bool handleGraphMouseUp(GraphState& state) {
    if (state.isDragging) {
        state.isDragging = false;
        return true;
    }
    return false;
}

bool handleGraphRightDown(GraphState& state, const RECT& graphRect, int x, int y) {
    POINT pt = { x, y };
    if (!PtInRect(&graphRect, pt)) return false;
    state.isRightProbing = true;
    state.probePos = { std::clamp(x - (int)graphRect.left, 0, (int)(graphRect.right - graphRect.left)),
                       std::clamp(y - (int)graphRect.top, 0, (int)(graphRect.bottom - graphRect.top)) };
    RECT localRect = { 0, 0, graphRect.right - graphRect.left, graphRect.bottom - graphRect.top };
    state.pixelToMath(state.probePos.x, state.probePos.y, localRect, state.probeMathX, state.probeMathY);
    return true;
}

bool handleGraphRightUp(GraphState& state) {
    if (state.isRightProbing) {
        state.isRightProbing = false;
        return true;
    }
    return false;
}

bool handleGraphMouseWheel(GraphState& state, const RECT& graphRect, int x, int y, short delta) {
    POINT pt = { x, y };
    if (!PtInRect(&graphRect, pt)) return false;
    double mx, my;
    state.pixelToMath(x, y, graphRect, mx, my);
    double factor = (delta > 0) ? 0.8 : 1.25;
    state.zoom(factor, mx, my);
    return true;
}

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

SIZE textSize(HDC hdc, const std::wstring& s) {
    SIZE sz{};
    GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
    return sz;
}

// Text with a 1 px halo in the background color, so labels stay legible
// where a curve or grid line passes underneath.
void drawHaloText(HDC hdc, int x, int y, const std::wstring& s, COLORREF text, COLORREF halo) {
    static const int kOffsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    SetTextColor(hdc, halo);
    for (const auto& o : kOffsets) TextOutW(hdc, x + o[0], y + o[1], s.c_str(), (int)s.size());
    SetTextColor(hdc, text);
    TextOutW(hdc, x, y, s.c_str(), (int)s.size());
}

void drawRoundedBox(HDC hdc, const RECT& r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

// 1-2-5 grid steps: nearest for the first pick, next larger when widening.
double niceStep(double raw) {
    if (!(raw > 0.0) || !std::isfinite(raw)) return 1.0;
    double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double n = raw / mag;
    return (n < 1.5 ? 1.0 : n < 3.5 ? 2.0 : n < 7.5 ? 5.0 : 10.0) * mag;
}

double nextNiceStep(double step) {
    double mag = std::pow(10.0, std::floor(std::log10(step) + 1e-9));
    double m = step / mag;
    return (m < 1.5 ? 2.0 : m < 3.5 ? 5.0 : 10.0) * mag;
}

// One step for both axes (pixels are square), about 80 px apart and widened
// until the x-axis labels fit between neighbouring grid lines. Expects the
// label font to be selected into hdc.
double chooseGridStep(HDC hdc, const GraphView& view) {
    const double ppu = view.pxPerUnitX();
    double step = niceStep(80.0 / ppu);
    for (int iter = 0; iter < 8; ++iter) {
        const double first = std::ceil(view.minX / step), last = std::floor(view.maxX / step);
        int widest = 0;
        for (double k : { first, last, std::floor(0.5 * (first + last)) })
            widest = std::max(widest, (int)textSize(hdc, utf8ToWide(formatAxisNumber(k * step, step))).cx);
        if (widest + 18 <= step * ppu) break;
        step = nextNiceStep(step);
    }
    return step;
}

constexpr int kHeaderBottom = 42;  // labels stay below the header pills

void drawAxisLabels(HDC hdc, const RECT& rc, const GraphView& view, double step, const GraphPalette& pal) {
    const int w = view.w, h = view.h;
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    const int th = tm.tmHeight;
    const double ox = view.toPx(0.0), oy = view.toPy(0.0);

    // x labels sit under the x-axis, pinned to the panel edge when it scrolls away.
    const int xLabelY = std::clamp((int)std::lround(std::clamp(oy, -1e6, 1e6)) + 4, kHeaderBottom, h - th - 3);
    std::vector<RECT> placed;
    double first = std::ceil(view.minX / step), last = std::floor(view.maxX / step);
    for (double k = first; k <= last && last - first < 2000; k += 1.0) {
        const double v = k * step;
        if (std::fabs(v) < step * 1e-6) continue;  // the origin gets its own "0"
        std::wstring s = utf8ToWide(formatAxisNumber(v, step));
        SIZE sz = textSize(hdc, s);
        int lx = (int)std::lround(view.toPx(v)) - sz.cx / 2;
        if (lx < 2 || lx + sz.cx > w - 2) continue;
        placed.push_back({ lx - 3, xLabelY, lx + sz.cx + 3, xLabelY + th });
        drawHaloText(hdc, rc.left + lx, rc.top + xLabelY, s, pal.text, pal.bg);
    }

    // y labels to the right of the y-axis, or along the nearer edge when it
    // scrolls out of view.
    const int axisX = (int)std::lround(std::clamp(ox, -1e6, 1e6));
    first = std::ceil(view.minY / step);
    last = std::floor(view.maxY / step);
    for (double k = first; k <= last && last - first < 2000; k += 1.0) {
        const double v = k * step;
        if (std::fabs(v) < step * 1e-6) continue;
        std::wstring s = utf8ToWide(formatAxisNumber(v, step));
        SIZE sz = textSize(hdc, s);
        int ly = (int)std::lround(view.toPy(v)) - th / 2;
        if (ly < kHeaderBottom || ly + th > h - 2) continue;
        int lx;
        if (axisX < 0) lx = 6;
        else if (axisX > w) lx = w - 6 - sz.cx;
        else if (axisX + 6 + sz.cx <= w - 4) lx = axisX + 6;
        else lx = axisX - 6 - sz.cx;
        RECT r = { lx, ly, lx + sz.cx, ly + th };
        bool clash = false;
        for (const RECT& p : placed) {
            RECT overlap;
            if (IntersectRect(&overlap, &r, &p)) {
                clash = true;
                break;
            }
        }
        if (!clash) drawHaloText(hdc, rc.left + lx, rc.top + ly, s, pal.text, pal.bg);
    }

    if (ox >= 8.0 && ox <= w - 4.0 && oy >= kHeaderBottom && oy <= h - th - 4.0) {
        SIZE sz = textSize(hdc, L"0");
        drawHaloText(hdc, rc.left + axisX - 4 - sz.cx, rc.top + (int)std::lround(oy) + 4, L"0", pal.text, pal.bg);
    }
}

void drawGraphHeader(HDC hdc, const RECT& rc, const GraphAnalysis& analysis, const Theme& theme,
                     HFONT font, HFONT smallFont, bool gpuEngine) {
    RECT resetBtn, zoomOutBtn, zoomInBtn;
    graphButtonRects(rc, resetBtn, zoomOutBtn, zoomInBtn);
    const COLORREF pillFill = theme.isDark ? RGB(0x28, 0x2C, 0x34) : RGB(0xEA, 0xEE, 0xF2);
    const COLORREF pillBorder = theme.isDark ? RGB(0x3E, 0x44, 0x51) : RGB(0xCF, 0xD4, 0xDA);
    HFONT small = smallFont ? smallFont : font;
    if (small) SelectObject(hdc, small);

    auto button = [&](RECT r, const wchar_t* label) {
        drawRoundedBox(hdc, r, pillFill, pillBorder, 6);
        SetTextColor(hdc, theme.isDark ? RGB(0xEB, 0xEB, 0xEB) : RGB(0x21, 0x25, 0x29));
        DrawTextW(hdc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    button(resetBtn, L"Reset");
    button(zoomOutBtn, L"−");
    button(zoomInBtn, L"+");

    // Which engine drew the curve: the D3D11 shader or the FP64 rasterizer.
    const wchar_t* badge = gpuEngine ? (isGpuGraphHardwareAdapter() ? L"GPU D3D11" : L"WARP D3D11") : L"CPU FP64";
    const int badgeW = (int)textSize(hdc, badge).cx + 18;
    RECT badgeRect = { resetBtn.left - 8 - badgeW, resetBtn.top, resetBtn.left - 8, resetBtn.bottom };
    if (gpuEngine) {
        drawRoundedBox(hdc, badgeRect, theme.isDark ? RGB(0x1B, 0x38, 0x28) : RGB(0xD4, 0xF0, 0xDF),
                       theme.isDark ? RGB(0x2E, 0x7D, 0x48) : RGB(0x63, 0xC4, 0x86), 8);
        SetTextColor(hdc, theme.isDark ? RGB(0x5A, 0xDE, 0x8B) : RGB(0x19, 0x87, 0x54));
    } else {
        drawRoundedBox(hdc, badgeRect, theme.isDark ? RGB(0x1A, 0x2B, 0x3C) : RGB(0xDB, 0xEA, 0xF7),
                       theme.isDark ? RGB(0x2E, 0x5F, 0x8A) : RGB(0x7F, 0xB0, 0xDC), 8);
        SetTextColor(hdc, theme.isDark ? RGB(0x6C, 0xC2, 0xFF) : RGB(0x0B, 0x5C, 0x9E));
    }
    DrawTextW(hdc, badge, -1, &badgeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int titleLeft = rc.left + 12;
    const int titleRight = badgeRect.left - 8;
    if (titleRight > titleLeft + 30) {
        std::wstring title = analysis.isValid
            ? (analysis.displayTitle.empty() ? utf8ToWide(analysis.title) : analysis.displayTitle)
            : std::wstring(L"Graph");
        RECT titleRect = { titleLeft, resetBtn.top, titleRight, resetBtn.bottom };
        drawRoundedBox(hdc, titleRect, pillFill, pillBorder, 8);
        if (font) SelectObject(hdc, font);
        SetTextColor(hdc, theme.isDark ? RGB(0xF0, 0xF2, 0xF5) : RGB(0x21, 0x25, 0x29));
        RECT textRect = { titleRect.left + 8, titleRect.top, titleRect.right - 8, titleRect.bottom };
        DrawTextW(hdc, title.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// Everything that changes the rendered pixels (not the chrome on top).
struct FrameKey {
    std::string title;
    GraphEquationKind kind = GraphEquationKind::None;
    bool valid = false;
    int w = 0, h = 0;
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0, step = 0.0;
    COLORREF bg = 0, grid = 0, axis = 0, curve = 0;

    bool operator==(const FrameKey& o) const {
        return valid == o.valid && kind == o.kind && w == o.w && h == o.h &&
               minX == o.minX && maxX == o.maxX && minY == o.minY && maxY == o.maxY &&
               step == o.step && bg == o.bg && grid == o.grid && axis == o.axis &&
               curve == o.curve && title == o.title;
    }
};

// Last composited frame (background, grid, axes, curve), so probe and other
// chrome-only repaints never re-render the curve.
struct FrameCache {
    FrameKey key;
    bool filled = false;
    bool gpu = false;
    std::vector<uint32_t> pixels;
};

FrameCache s_frame;
CoverageBuffer s_coverage;

// Snapping for y = f(x) (or x = f(y) with xOfY): the curve under the
// cursor plus roots, the axis intercept and extrema within 30 px.
template <typename Snap>
void snapExplicit(const GraphAnalysis& analysis, const GraphState& state, bool xOfY,
                  double unitX, double unitY, Snap&& trySnap) {
    auto eval = [&](double t) { return xOfY ? analysis.evalExplicitX(t) : analysis.evalExplicit(t); };
    // Roots, intercepts and extrema are points of interest; the plain curve
    // point under the cursor is not.
    auto snapAt = [&](double t, double v, bool pointOfInterest = true) {
        if (xOfY) trySnap(v, t, pointOfInterest);
        else trySnap(t, v, pointOfInterest);
    };
    const double unitT = xOfY ? unitY : unitX;  // math units per pixel along t
    const double unitV = xOfY ? unitX : unitY;  // and across, along the value axis
    const double viewLo = xOfY ? state.minX : state.minY;
    const double viewHi = xOfY ? state.maxX : state.maxY;
    const double t0 = xOfY ? state.probeMathY : state.probeMathX;
    snapAt(t0, eval(t0), false);

    const int steps = 60;
    double ts[steps + 1], vs[steps + 1];
    for (int i = 0; i <= steps; ++i) {
        ts[i] = t0 + (i - steps / 2) * unitT;
        vs[i] = eval(ts[i]);
    }
    for (int i = 0; i < steps; ++i) {
        const double fa = vs[i], fb = vs[i + 1];
        if (!std::isfinite(fa) || !std::isfinite(fb)) continue;
        // Root: bisect the sign change; a pole converges to huge |f| and is skipped.
        if ((fa <= 0.0) != (fb <= 0.0)) {
            double lo = ts[i], hi = ts[i + 1], flo = fa;
            for (int k = 0; k < 64; ++k) {
                double m = 0.5 * (lo + hi);
                if (m == lo || m == hi) break;
                double fm = eval(m);
                if (!std::isfinite(fm)) break;
                if ((fm <= 0.0) == (flo <= 0.0)) {
                    lo = m;
                    flo = fm;
                } else {
                    hi = m;
                }
            }
            double root = 0.5 * (lo + hi);
            double fr = eval(root);
            if (std::isfinite(fr) && std::fabs(fr) <= 2.0 * unitV) snapAt(root, 0.0);
        }
        // Intercept with the value axis (t = 0).
        if (ts[i] <= 0.0 && ts[i + 1] >= 0.0) snapAt(0.0, eval(0.0));
        // Extremum: the discrete slope changes sign around sample i.
        if (i >= 1 && std::isfinite(vs[i - 1])) {
            const double d0 = vs[i] - vs[i - 1], d1 = fb - vs[i];
            if (d0 * d1 < 0.0) {
                const bool isMax = d0 > 0.0;
                double lo = ts[i - 1], hi = ts[i + 1];
                const double g = 0.6180339887498949;
                double c = hi - g * (hi - lo), d = lo + g * (hi - lo);
                double fc = eval(c), fd = eval(d);
                for (int k = 0; k < 60 && std::isfinite(fc) && std::isfinite(fd); ++k) {
                    if (isMax ? fc > fd : fc < fd) {
                        hi = d; d = c; fd = fc;
                        c = hi - g * (hi - lo); fc = eval(c);
                    } else {
                        lo = c; c = d; fc = fd;
                        d = lo + g * (hi - lo); fd = eval(d);
                    }
                }
                const double te = 0.5 * (lo + hi), fe = eval(te);
                // Poles and jumps fake a slope change; a real extremum stays near the view.
                const double span = viewHi - viewLo;
                if (std::isfinite(fe) && fe > viewLo - span && fe < viewHi + span) snapAt(te, fe);
            }
        }
    }
}

// Snapping for F(x, y) = 0: Newton projection onto the curve and the two
// axis intercepts, all with steps and tolerances in pixels.
template <typename Snap>
void snapImplicit(const GraphAnalysis& analysis, const GraphState& state,
                  double unitX, double unitY, Snap&& trySnap) {
    auto F = [&](double x, double y) { return analysis.evalImplicit(x, y); };
    const double hx = 0.25 * unitX, hy = 0.25 * unitY;
    // Gradient of F per pixel; distance estimate |F| / |grad| is in pixels.
    auto gradPx = [&](double x, double y, double& gx, double& gy) {
        gx = (F(x + hx, y) - F(x - hx, y)) / (2.0 * hx) * unitX;
        gy = (F(x, y + hy) - F(x, y - hy)) / (2.0 * hy) * unitY;
    };
    auto onCurve = [&](double x, double y) {
        double f = F(x, y), gx, gy;
        gradPx(x, y, gx, gy);
        double g = std::hypot(gx, gy);
        return std::isfinite(f) && g > 0.0 && std::fabs(f) / g < 0.5;
    };

    double cx = state.probeMathX, cy = state.probeMathY;
    for (int iter = 0; iter < 16; ++iter) {
        double f = F(cx, cy), gx, gy;
        gradPx(cx, cy, gx, gy);
        double g2 = gx * gx + gy * gy;
        if (!std::isfinite(f) || !(g2 > 0.0) || !std::isfinite(g2)) break;
        double sx = -f * gx / g2, sy = -f * gy / g2;  // Newton step in pixels
        double len = std::hypot(sx, sy);
        if (len > 8.0) {
            sx *= 8.0 / len;
            sy *= 8.0 / len;
        }
        cx += sx * unitX;
        cy += sy * unitY;
        if (len < 1e-3) break;
    }
    if (onCurve(cx, cy)) trySnap(cx, cy, false);

    // Axis intercepts: 1D Newton along each axis from the probe position.
    auto intercept = [&](bool alongX) {
        double t = alongX ? state.probeMathX : state.probeMathY;
        const double unit = alongX ? unitX : unitY, h = 0.25 * unit;
        auto G = [&](double s) { return alongX ? F(s, 0.0) : F(0.0, s); };
        for (int iter = 0; iter < 16; ++iter) {
            double g = G(t), dg = (G(t + h) - G(t - h)) / (2.0 * h) * unit;
            if (!std::isfinite(g) || !(std::fabs(dg) > 0.0) || !std::isfinite(dg)) return;
            double step = std::clamp(-g / dg, -8.0, 8.0);  // pixels
            t += step * unit;
            if (std::fabs(step) < 1e-3) break;
        }
        if (alongX ? onCurve(t, 0.0) : onCurve(0.0, t)) {
            if (alongX) trySnap(t, 0.0, true);
            else trySnap(0.0, t, true);
        }
    };
    if (std::fabs(state.probeMathY) < 30.0 * unitY) intercept(true);
    if (std::fabs(state.probeMathX) < 30.0 * unitX) intercept(false);
}

} // namespace

void drawProbeTooltip(HDC hdc, const RECT& localRect, const GraphState& state,
                      const GraphAnalysis& analysis, const Theme& theme, HFONT font) {
    if (!state.isRightProbing) return;
    const int w = localRect.right - localRect.left;
    const int h = localRect.bottom - localRect.top;
    if (w <= 0 || h <= 0) return;
    const double unitX = (state.maxX - state.minX) / w;  // math units per pixel
    const double unitY = (state.maxY - state.minY) / h;
    const int px = std::clamp((int)state.probePos.x, 0, w);
    const int py = std::clamp((int)state.probePos.y, 0, h);

    // Dotted crosshairs
    HPEN dotPen = CreatePen(PS_DOT, 1, theme.isDark ? RGB(0x60, 0x68, 0x78) : RGB(0x9E, 0xA8, 0xB6));
    HPEN oldPen = (HPEN)SelectObject(hdc, dotPen);
    MoveToEx(hdc, localRect.left, localRect.top + py, nullptr);
    LineTo(hdc, localRect.left + w, localRect.top + py);
    MoveToEx(hdc, localRect.left + px, localRect.top, nullptr);
    LineTo(hdc, localRect.left + px, localRect.top + h);
    SelectObject(hdc, oldPen);
    DeleteObject(dotPen);

    double shownX = state.probeMathX, shownY = state.probeMathY;
    double markerX = px, markerY = py;
    double best = 24.0;  // snap radius in pixels
    // Points of interest (roots, intercepts, extrema) get an 8 px head start
    // over the nearest plain curve point, so they win when the cursor is near.
    auto trySnap = [&](double x, double y, bool pointOfInterest) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        double sx = (x - state.minX) / unitX, sy = (state.maxY - y) / unitY;
        double d = std::hypot(sx - px, sy - py);
        double score = pointOfInterest ? std::max(0.0, d - 8.0) : d;
        if (d < 24.0 && score < best) {
            best = score;
            shownX = x;
            shownY = y;
            markerX = sx;
            markerY = sy;
        }
    };

    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        trySnap(std::round(state.probeMathX), std::round(state.probeMathY), true);
    } else if (analysis.isValid) {
        if (analysis.kind == GraphEquationKind::ImplicitXY) {
            snapImplicit(analysis, state, unitX, unitY, trySnap);
        } else {
            snapExplicit(analysis, state, analysis.kind == GraphEquationKind::ExplicitX, unitX, unitY, trySnap);
        }
    }

    // Glowing target marker
    const int mx = localRect.left + (int)std::lround(markerX);
    const int my = localRect.top + (int)std::lround(markerY);
    HBRUSH glow = CreateSolidBrush(theme.accent);
    HPEN ring = CreatePen(PS_SOLID, 2, RGB(0xFF, 0xFF, 0xFF));
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, glow);
    oldPen = (HPEN)SelectObject(hdc, ring);
    Ellipse(hdc, mx - 5, my - 5, mx + 6, my + 6);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(glow);
    DeleteObject(ring);

    // Coordinates to a tenth of a pixel, so deep zooms show the digits that matter.
    std::wstring text = L"(" + utf8ToWide(formatAxisNumber(shownX, unitX * 0.1)) + L", " +
                        utf8ToWide(formatAxisNumber(shownY, unitY * 0.1)) + L")";
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : nullptr;
    SIZE ts = textSize(hdc, text);
    const int tipW = ts.cx + 20, tipH = ts.cy + 10;
    int tipX = mx + 14, tipY = my - tipH - 8;
    if (tipX + tipW > localRect.left + w - 10) tipX = mx - tipW - 14;
    if (tipY < localRect.top + kHeaderBottom) tipY = my + 14;
    tipX = std::clamp(tipX, (int)localRect.left + 8, std::max((int)localRect.left + 8, (int)localRect.left + w - tipW - 8));
    tipY = std::clamp(tipY, (int)localRect.top + kHeaderBottom, std::max((int)localRect.top + kHeaderBottom, (int)localRect.top + h - tipH - 8));
    RECT tipRect = { tipX, tipY, tipX + tipW, tipY + tipH };
    drawRoundedBox(hdc, tipRect, theme.isDark ? RGB(0x15, 0x18, 0x22) : RGB(0xFA, 0xFA, 0xFD), theme.accent, 6);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme.isDark ? RGB(0xFF, 0xF4, 0xEA) : RGB(0x18, 0x1A, 0x20));
    DrawTextW(hdc, text.c_str(), -1, &tipRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont) SelectObject(hdc, oldFont);
}

void renderGraph(HDC hdc, const RECT& graphRect, const GraphAnalysis& analysis,
                 GraphState& state, const Theme& theme, HFONT font, HFONT smallFont) {
    const int w = graphRect.right - graphRect.left;
    const int h = graphRect.bottom - graphRect.top;
    if (w <= 10 || h <= 10) return;
    if (!state.initialized) state.resetView(10.0);
    state.fitAspect(w, h);
    const GraphView view = state.view(w, h);
    const GraphPalette pal = getGraphPalette(theme);

    HFONT labelFont = smallFont ? smallFont : font;
    HFONT oldFont = labelFont ? (HFONT)SelectObject(hdc, labelFont) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    const double step = chooseGridStep(hdc, view);

    FrameKey key;
    key.valid = analysis.isValid;
    key.kind = analysis.kind;
    key.title = analysis.isValid ? analysis.title : std::string();
    key.w = w;
    key.h = h;
    key.minX = view.minX;
    key.maxX = view.maxX;
    key.minY = view.minY;
    key.maxY = view.maxY;
    key.step = step;
    key.bg = pal.bg;
    key.grid = pal.grid;
    key.axis = pal.axis;
    key.curve = pal.curve;
    if (!s_frame.filled || !(s_frame.key == key)) {
        rasterizeGrid(s_frame.pixels, view, step, step, pal);
        bool gpu = false;
        if (analysis.isValid) {
            s_coverage.reset(w, h);
            if (analysis.kind == GraphEquationKind::ImplicitXY) {
                gpu = renderImplicitCoverageGpu(analysis, view, s_coverage);
                if (!gpu) rasterizeImplicitCurve(s_coverage, view, analysis);
            } else {
                rasterizeExplicitCurve(s_coverage, view, analysis);
            }
            compositeCoverage(s_frame.pixels, s_coverage, pal.curve);
        }
        s_frame.key = key;
        s_frame.filled = true;
        s_frame.gpu = gpu;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, graphRect.left, graphRect.top, w, h, 0, 0, 0, h,
                      s_frame.pixels.data(), &bmi, DIB_RGB_COLORS);

    if (labelFont) SelectObject(hdc, labelFont);
    drawAxisLabels(hdc, graphRect, view, step, pal);
    drawGraphHeader(hdc, graphRect, analysis, theme, font, smallFont, s_frame.gpu);
    if (state.isRightProbing) {
        RECT localRect = graphRect;
        drawProbeTooltip(hdc, localRect, state, analysis, theme, font ? font : smallFont);
    }
    if (oldFont) SelectObject(hdc, oldFont);
}
