// graph_raster.cpp
//
// FP64 software rasterizer for the graph panel (see graph_raster.h).

#include "graph_raster.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

// ------------------------------------------------------------ worker pool

// Persistent pool: spawning threads every frame would cost more than a whole
// implicit-curve pass. Intentionally leaked, so no thread is ever joined
// while the process is shutting down.
class WorkerPool {
public:
    WorkerPool() {
        unsigned total = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
        for (unsigned i = 1; i < total; ++i) threads_.emplace_back([this] { workerLoop(); });
    }

    void run(int count, const std::function<void(int)>& fn) {
        if (count <= 0) return;
        if (threads_.empty() || count == 1) {
            for (int i = 0; i < count; ++i) fn(i);
            return;
        }
        std::lock_guard<std::mutex> runLock(runMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &fn;
            count_ = count;
            next_.store(0, std::memory_order_relaxed);
            pending_ = (int)threads_.size();
            ++generation_;
        }
        wake_.notify_all();
        drain();
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
        job_ = nullptr;
    }

private:
    void drain() {
        for (;;) {
            int i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count_) return;
            (*job_)(i);
        }
    }

    void workerLoop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
            }
            drain();
            std::lock_guard<std::mutex> lock(mutex_);
            if (--pending_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex runMutex_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(int)>* job_ = nullptr;
    std::atomic<int> next_{0};
    int count_ = 0;
    int pending_ = 0;
    uint64_t generation_ = 0;
};

WorkerPool& workerPool() {
    static WorkerPool* pool = new WorkerPool();
    return *pool;
}

// ------------------------------------------------------------ colors

struct Rgb {
    float r, g, b;
};

Rgb toRgb(COLORREF c) {
    return { (float)GetRValue(c), (float)GetGValue(c), (float)GetBValue(c) };
}

Rgb mix(const Rgb& a, const Rgb& b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

uint32_t packRgb(const Rgb& c) {
    auto q = [](float v) -> uint32_t { return (uint32_t)std::clamp((int)(v + 0.5f), 0, 255); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

float saturate(double v) { return (float)std::clamp(v, 0.0, 1.0); }

// ------------------------------------------------------------ grid

// Line profiles match the D3D11 shader's look: major grid saturate(1.2 - d),
// minor grid saturate(1 - d) at 35% strength, axes saturate(1.3 - 0.8 d).
constexpr float kGridAlpha = 0.65f;
constexpr float kAxisAlpha = 0.95f;

// Stamps one line at pixel position pos into a 1D alpha profile, combining
// with max(): alpha = saturate(c0 - c1 * distance) * gain.
void stampLine(std::vector<float>& out, double pos, double c0, double c1, float gain) {
    const double reach = c0 / c1;
    const int n = (int)out.size();
    if (!(pos > -reach - 1.0 && pos < n + reach + 1.0)) return;
    int i0 = std::max(0, (int)std::floor(pos - reach - 0.5));
    int i1 = std::min(n - 1, (int)std::ceil(pos + reach - 0.5));
    for (int i = i0; i <= i1; ++i) {
        float a = saturate(c0 - c1 * std::fabs(i + 0.5 - pos)) * gain;
        if (a > out[i]) out[i] = a;
    }
}

// Stamps every visible multiple of step along one axis. Positions come from
// k * step - lo in double, so lines stay exact at any zoom depth.
void stampLineFamily(std::vector<float>& out, double lo, double hi, double pxPerUnit,
                     bool flipped, double step, double c0, double c1, float gain) {
    if (!(step > 0.0) || !std::isfinite(step)) return;
    if (step * pxPerUnit < 2.0) return;  // denser than every 2 px reads as noise
    double first = std::floor(lo / step) - 1.0;
    double last = std::ceil(hi / step) + 1.0;
    if (!(last - first < 20000.0)) return;
    for (double k = first; k <= last; k += 1.0) {
        double v = k * step;
        double pos = flipped ? (hi - v) * pxPerUnit : (v - lo) * pxPerUnit;
        stampLine(out, pos, c0, c1, gain);
    }
}

// Desmos-style subdivisions: quarters for 2x10^k steps, fifths otherwise.
double minorStepFor(double step) {
    double mag = std::pow(10.0, std::floor(std::log10(step)));
    double mantissa = step / mag;
    return step / ((mantissa > 1.5 && mantissa < 3.0) ? 4.0 : 5.0);
}

// ------------------------------------------------------------ strokes

constexpr double kStrokeReach = 2.2;  // pixels beyond which a stroke adds nothing

// Liang-Barsky clip of a segment to [xmin, xmax] x [ymin, ymax].
bool clipToRect(double& x0, double& y0, double& x1, double& y1,
                double xmin, double ymin, double xmax, double ymax) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double p[4] = { -dx, dx, -dy, dy };
    const double q[4] = { x0 - xmin, xmax - x0, y0 - ymin, ymax - y0 };
    double t0 = 0.0, t1 = 1.0;
    for (int k = 0; k < 4; ++k) {
        if (p[k] == 0.0) {
            if (q[k] < 0.0) return false;
            continue;
        }
        double r = q[k] / p[k];
        if (p[k] < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
    }
    double nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
    double nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
    x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
    return true;
}

// ------------------------------------------------------------ explicit curves

// Values further than this many panel extents off-screen are clamped before
// the pixel conversion: no overflow, and the slope change it causes on
// screen is far below a pixel.
constexpr double kValueClampPanels = 1e6;
constexpr double kMarginPx = 3.0;   // stroke reach plus a little
constexpr double kJoinPx = 6.0;     // longest jump joined without refinement
constexpr int kMaxDepth = 48;

// Adaptive sampler for y = f(x) (or x = f(y)). Works in pixel space: s runs
// along the sampling axis, p across it. Every interval is split until its
// chord is within 0.2 px of the curve; intervals whose change refuses to
// shrink as they halve are discontinuities and are left unconnected.
class ExplicitSampler {
public:
    ExplicitSampler(CoverageBuffer& cov, const GraphView& view, const GraphAnalysis& analysis)
        : cov_(cov), view_(view), analysis_(analysis),
          xOfY_(analysis.kind == GraphEquationKind::ExplicitX),
          along_(xOfY_ ? view.h : view.w),
          across_(xOfY_ ? view.w : view.h),
          fast_(analysis.hasFastEval()) {}

    void run() {
        // One sample per pixel center, two pixels past each edge so strokes
        // reach the border. Slow (non-bytecode) evaluation is spread over the
        // worker pool; bytecode is faster than the hand-off.
        const int first = -2, count = along_ + 4;
        std::vector<Pt> base(count);
        auto fill = [&](int i) { base[i] = evaluate(first + i + 0.5); };
        if (fast_) {
            for (int i = 0; i < count; ++i) fill(i);
        } else {
            parallelFor(count, fill);
        }
        // Each pixel interval gets its own refinement budget, so a region of
        // endless detail (sin(1/x) near 0) cannot starve the rest of the curve.
        const long perInterval = std::max(4L, std::min(400L, (fast_ ? 250000L : 6000L) / count));
        for (int i = 1; i < count; ++i) {
            budget_ = perInterval;
            refine(base[i - 1], base[i], 0, 0.0, 0);
        }
        drawAsymptotes();
    }

private:
    struct Pt {
        double s = 0.0;    // pixel coordinate along the sampling axis
        double val = 0.0;  // f(t) in math units
        double p = 0.0;    // pixel coordinate of val across the panel
        bool ok = false;   // val is finite
    };

    Pt evaluate(double s) const {
        Pt pt;
        pt.s = s;
        pt.val = xOfY_ ? analysis_.evalExplicitX(view_.fromPy(s))
                       : analysis_.evalExplicit(view_.fromPx(s));
        pt.ok = std::isfinite(pt.val);
        if (pt.ok) pt.p = toAcross(pt.val);
        return pt;
    }

    Pt sample(double s) {
        --budget_;
        return evaluate(s);
    }

    double toAcross(double val) const {
        double lo = xOfY_ ? view_.minX : view_.minY;
        double hi = xOfY_ ? view_.maxX : view_.maxY;
        double limit = (hi - lo) * kValueClampPanels;
        val = std::clamp(val, lo - limit, hi + limit);
        return xOfY_ ? view_.toPx(val) : view_.toPy(val);
    }

    bool farOff(double p) const { return p < -across_ || p > 2.0 * across_; }

    void emit(const Pt& a, const Pt& b) {
        if (xOfY_) rasterizeSegment(cov_, a.p, a.s, b.p, b.s);
        else rasterizeSegment(cov_, a.s, a.p, b.s, b.p);
    }

    void refine(const Pt& a, const Pt& b, int depth, double parentDp, int stuck) {
        if (!a.ok || !b.ok) {
            if (a.ok || b.ok) {
                domainEdge(a, b, depth);
            } else if (b.s - a.s > 0.25 && budget_ > 0) {
                // Undefined at both ends: look once for an island of definition.
                Pt m = sample(0.5 * (a.s + b.s));
                if (m.ok) {
                    refine(a, m, depth + 1, 0.0, 0);
                    refine(m, b, depth + 1, 0.0, 0);
                }
            }
            return;
        }
        const bool aAbove = a.p < -kMarginPx, bAbove = b.p < -kMarginPx;
        const bool aBelow = a.p > across_ + kMarginPx, bBelow = b.p > across_ + kMarginPx;
        if ((aAbove && bAbove) || (aBelow && bBelow)) return;  // entirely off-screen

        const double width = b.s - a.s;
        const double dp = std::fabs(b.p - a.p);
        // At sub-pixel scale a change that will not shrink as the interval
        // halves is a jump (floor) or a pole (tan), not a steep curve.
        if (width < 1.0 / 16.0 && dp > kJoinPx) {
            stuck = (dp > 0.95 * parentDp) ? stuck + 1 : 0;
            if (stuck >= 4) {
                discontinuity(a, b);
                return;
            }
        }
        if (budget_ <= 0) {
            if (dp <= kJoinPx) emit(a, b);
            return;
        }
        const double ms = a.s + 0.5 * width;
        if (!(ms > a.s && ms < b.s) || depth >= kMaxDepth) {
            // Out of resolution: join unless the interval was behaving like a break.
            if (stuck == 0 || dp <= kJoinPx) emit(a, b);
            else discontinuity(a, b);
            return;
        }
        Pt m = sample(ms);
        if (!m.ok) {
            refine(a, m, depth + 1, dp, stuck);
            refine(m, b, depth + 1, dp, stuck);
            return;
        }
        if (dp <= kJoinPx && std::fabs(m.p - 0.5 * (a.p + b.p)) <= 0.2) {
            emit(a, m);
            emit(m, b);
            return;
        }
        refine(a, m, depth + 1, dp, stuck);
        refine(m, b, depth + 1, dp, stuck);
    }

    // Exactly one end is defined: bisect toward the edge of the domain so the
    // stroke ends where the function does (sqrt(x) reaches the axis).
    void domainEdge(const Pt& a, const Pt& b, int depth) {
        Pt good = a.ok ? a : b;
        Pt bad = a.ok ? b : a;
        const Pt origin = good;
        for (int k = 0; k < 40 && std::fabs(bad.s - good.s) > 1e-4 && budget_ > 0; ++k) {
            double ms = 0.5 * (good.s + bad.s);
            if (ms == good.s || ms == bad.s) break;
            Pt m = sample(ms);
            if (m.ok) good = m;
            else bad = m;
        }
        if (good.s != origin.s) {
            if (a.ok) refine(origin, good, depth + 1, 0.0, 0);
            else refine(good, origin, depth + 1, 0.0, 0);
        }
        // |f| exploding toward an undefined point (1/x sampled exactly at 0).
        if (farOff(good.p) && std::fabs(good.val) > 4.0 * std::fabs(origin.val)) notePole(good.s);
    }

    // Only a genuine pole earns an asymptote: both sides leave the screen and
    // |f| keeps growing toward the break. floor's jumps and sin(1/x)'s
    // bounded oscillation never qualify.
    void discontinuity(const Pt& a, const Pt& b) {
        if (!farOff(a.p) || !farOff(b.p) || budget_ <= 0) return;
        const double w = b.s - a.s;
        Pt outA = sample(a.s - 16.0 * w);
        Pt outB = sample(b.s + 16.0 * w);
        bool grows = (outA.ok && std::fabs(a.val) > 4.0 * std::fabs(outA.val)) ||
                     (outB.ok && std::fabs(b.val) > 4.0 * std::fabs(outB.val));
        if (grows) notePole(0.5 * (a.s + b.s));
    }

    void notePole(double s) {
        if (poles_.size() < 4096) poles_.push_back(s);
    }

    void drawAsymptotes() {
        if (poles_.empty()) return;
        std::sort(poles_.begin(), poles_.end());
        std::vector<double> merged;
        for (double s : poles_) {
            if (merged.empty() || s - merged.back() > 2.0) merged.push_back(s);
        }
        // Densely packed poles (tan(50x) zoomed out) would only hatch the panel.
        if ((int)merged.size() > std::max(4, along_ / 24)) return;
        for (double s : merged) {
            if (s > -1.0 && s < along_ + 1.0) drawDottedLine(s);
        }
    }

    // 4 px dashes every 8 px, shared between the two nearest pixel lines.
    void drawDottedLine(double s) {
        double c = s - 0.5;
        int i0 = (int)std::floor(c);
        float frac = (float)(c - i0);
        for (int k = 0; k < across_; ++k) {
            if ((k & 7) >= 4) continue;
            plot(i0, k, 0.65f * (1.0f - frac));
            plot(i0 + 1, k, 0.65f * frac);
        }
    }

    void plot(int along, int across, float a) {
        int x = xOfY_ ? across : along;
        int y = xOfY_ ? along : across;
        if (x < 0 || y < 0 || x >= cov_.w || y >= cov_.h) return;
        float& dst = cov_.alpha[(size_t)y * cov_.w + x];
        if (a > dst) dst = a;
    }

    CoverageBuffer& cov_;
    const GraphView& view_;
    const GraphAnalysis& analysis_;
    const bool xOfY_;
    const int along_;
    const int across_;
    const bool fast_;
    long budget_ = 0;
    std::vector<double> poles_;
};

// ------------------------------------------------------------ implicit curves

// Marching squares over an FP64 sample grid aligned to pixel centers. Each
// sign change on a cell edge is verified before it becomes a contour point:
// a root's bracket values shrink under bisection, a pole's grow and a jump's
// stay put, so asymptotes and jumps never turn into fake curve segments.
class ImplicitTracer {
public:
    ImplicitTracer(CoverageBuffer& cov, const GraphView& view, const GraphAnalysis& analysis)
        : cov_(cov), view_(view), analysis_(analysis),
          cell_(analysis.hasFastEval() ? 1 : 4),
          cols_((view.w + cell_ - 1) / cell_ + 3),
          rows_((view.h + cell_ - 1) / cell_ + 3) {}

    void run() {
        values_.assign((size_t)rows_ * cols_, 0.0);
        parallelFor(rows_, [&](int r) {
            const double y = view_.fromPy(py(r));
            double* out = &values_[(size_t)r * cols_];
            for (int c = 0; c < cols_; ++c) out[c] = analysis_.evalImplicit(view_.fromPx(px(c)), y);
        });

        hx_.assign((size_t)rows_ * (cols_ - 1), NAN);
        vy_.assign((size_t)(rows_ - 1) * cols_, NAN);
        parallelFor(rows_, [&](int r) {
            for (int c = 0; c + 1 < cols_; ++c)
                hx_[(size_t)r * (cols_ - 1) + c] =
                    crossing(f(r, c), f(r, c + 1), px(c), py(r), px(c + 1), py(r));
            if (r + 1 < rows_) {
                for (int c = 0; c < cols_; ++c)
                    vy_[(size_t)r * cols_ + c] =
                        crossing(f(r, c), f(r + 1, c), px(c), py(r), px(c), py(r + 1));
            }
        });

        // Contour and stroke in horizontal bands so no two threads share a
        // pixel row; cell rows near a band edge are traced by both bands.
        const int band = 32;
        const int bands = (cov_.h + band - 1) / band;
        parallelFor(bands, [&](int b) {
            const int y0 = b * band;
            const int y1 = std::min(cov_.h, y0 + band);
            int rFirst = std::max(0, (int)std::floor((y0 - 0.5 - kStrokeReach) / cell_));
            int rLast = std::min(rows_ - 2, (int)std::ceil((y1 - 0.5 + kStrokeReach) / cell_) + 1);
            for (int r = rFirst; r <= rLast; ++r) {
                for (int c = 0; c + 1 < cols_; ++c) {
                    cellSegments(r, c, [&](double ax, double ay, double bx, double by) {
                        rasterizeSegment(cov_, ax, ay, bx, by, y0, y1);
                    });
                }
            }
        });
    }

private:
    double px(int c) const { return (c - 1) * (double)cell_ + 0.5; }
    double py(int r) const { return (r - 1) * (double)cell_ + 0.5; }
    double f(int r, int c) const { return values_[(size_t)r * cols_ + c]; }
    float hx(int r, int c) const { return hx_[(size_t)r * (cols_ - 1) + c]; }
    float vy(int r, int c) const { return vy_[(size_t)r * cols_ + c]; }

    double F(double x, double y) const {
        return analysis_.evalImplicit(view_.fromPx(x), view_.fromPy(y));
    }

    // Fraction along a->b where F crosses zero, or NaN when the edge holds no
    // genuine root.
    float crossing(double fa, double fb, double ax, double ay, double bx, double by) const {
        if (!std::isfinite(fa) || !std::isfinite(fb)) return NAN;
        const bool sa = fa > 0.0;
        if (sa == (fb > 0.0)) return NAN;
        auto at = [&](double t) { return F(ax + (bx - ax) * t, ay + (by - ay) * t); };
        const double fm = at(0.5);
        if (!std::isfinite(fm)) return NAN;
        // Smooth root: the midpoint stays near the chord. Interpolate on the
        // half that holds the sign change.
        if (std::fabs(fm - 0.5 * (fa + fb)) <= 0.25 * std::fabs(fb - fa)) {
            if ((fm > 0.0) == sa) return (float)(0.5 + 0.5 * fm / (fm - fb));
            return (float)(0.5 * fa / (fa - fm));
        }
        double lo = 0.0, hi = 1.0, flo = fa, fhi = fb;
        const double m0 = std::max(std::fabs(fa), std::fabs(fb));
        if ((fm > 0.0) == sa) {
            lo = 0.5;
            flo = fm;
        } else {
            hi = 0.5;
            fhi = fm;
        }
        for (int k = 0; k < 24; ++k) {
            const double m = std::max(std::fabs(flo), std::fabs(fhi));
            if (m > 64.0 * m0) return NAN;  // blowing up: a pole
            if (m <= 0.5 * m0 && hi - lo <= 0.125)
                return (float)(lo + (hi - lo) * flo / (flo - fhi));
            const double t = 0.5 * (lo + hi);
            const double ft = at(t);
            if (!std::isfinite(ft)) return NAN;
            if ((ft > 0.0) == (flo > 0.0)) {
                lo = t;
                flo = ft;
            } else {
                hi = t;
                fhi = ft;
            }
        }
        return NAN;  // never shrank: a jump
    }

    template <typename Emit>
    void cellSegments(int r, int c, Emit&& emit) const {
        const double tl = f(r, c), tr = f(r, c + 1), bl = f(r + 1, c), br = f(r + 1, c + 1);
        if (!(std::isfinite(tl) && std::isfinite(tr) && std::isfinite(bl) && std::isfinite(br))) return;
        const int mask = (tl > 0.0 ? 1 : 0) | (tr > 0.0 ? 2 : 0) | (br > 0.0 ? 4 : 0) | (bl > 0.0 ? 8 : 0);
        if (mask == 0 || mask == 15) return;

        struct P { double x, y; bool ok; };
        const double x0 = px(c), y0 = py(r), s = cell_;
        auto top = [&] { float t = hx(r, c); return P{ x0 + t * s, y0, !std::isnan(t) }; };
        auto bottom = [&] { float t = hx(r + 1, c); return P{ x0 + t * s, y0 + s, !std::isnan(t) }; };
        auto left = [&] { float t = vy(r, c); return P{ x0, y0 + t * s, !std::isnan(t) }; };
        auto right = [&] { float t = vy(r, c + 1); return P{ x0 + s, y0 + t * s, !std::isnan(t) }; };
        auto seg = [&](const P& a, const P& b) {
            if (a.ok && b.ok) emit(a.x, a.y, b.x, b.y);
        };
        switch (mask) {
            case 1: case 14: seg(left(), top()); break;
            case 2: case 13: seg(top(), right()); break;
            case 3: case 12: seg(left(), right()); break;
            case 4: case 11: seg(right(), bottom()); break;
            case 6: case 9: seg(top(), bottom()); break;
            case 7: case 8: seg(left(), bottom()); break;
            case 5: case 10: {
                // Saddle: the cell center decides which corners connect.
                const bool centerPositive = (tl + tr + br + bl) > 0.0;
                if ((mask == 5) == centerPositive) {
                    seg(top(), right());
                    seg(left(), bottom());
                } else {
                    seg(left(), top());
                    seg(right(), bottom());
                }
                break;
            }
        }
    }

    CoverageBuffer& cov_;
    const GraphView& view_;
    const GraphAnalysis& analysis_;
    const int cell_;
    const int cols_;
    const int rows_;
    std::vector<double> values_;
    std::vector<float> hx_;
    std::vector<float> vy_;
};

} // namespace

void CoverageBuffer::reset(int width, int height) {
    w = width;
    h = height;
    alpha.assign((size_t)w * h, 0.0f);
}

void parallelFor(int count, const std::function<void(int)>& fn) {
    workerPool().run(count, fn);
}

void rasterizeGrid(std::vector<uint32_t>& pixels, const GraphView& view,
                   double xStep, double yStep, const GraphPalette& palette) {
    const int w = view.w, h = view.h;
    pixels.resize((size_t)w * h);
    std::vector<float> colGrid(w, 0.0f), colAxis(w, 0.0f), rowGrid(h, 0.0f), rowAxis(h, 0.0f);
    const double ppx = view.pxPerUnitX(), ppy = view.pxPerUnitY();

    stampLineFamily(colGrid, view.minX, view.maxX, ppx, false, minorStepFor(xStep), 1.0, 1.0, kGridAlpha * 0.35f);
    stampLineFamily(colGrid, view.minX, view.maxX, ppx, false, xStep, 1.2, 1.0, kGridAlpha);
    stampLineFamily(rowGrid, view.minY, view.maxY, ppy, true, minorStepFor(yStep), 1.0, 1.0, kGridAlpha * 0.35f);
    stampLineFamily(rowGrid, view.minY, view.maxY, ppy, true, yStep, 1.2, 1.0, kGridAlpha);
    stampLine(colAxis, view.toPx(0.0), 1.3, 0.8, kAxisAlpha);
    stampLine(rowAxis, view.toPy(0.0), 1.3, 0.8, kAxisAlpha);

    const Rgb bg = toRgb(palette.bg), grid = toRgb(palette.grid), axis = toRgb(palette.axis);
    auto shade = [&](float gridA, float axisA) { return packRgb(mix(mix(bg, grid, gridA), axis, axisA)); };

    // Rows without a horizontal line are copies of one template row.
    std::vector<uint32_t> templ(w);
    for (int i = 0; i < w; ++i) templ[i] = shade(colGrid[i], colAxis[i]);
    for (int j = 0; j < h; ++j) {
        uint32_t* row = &pixels[(size_t)j * w];
        if (rowGrid[j] == 0.0f && rowAxis[j] == 0.0f) {
            std::memcpy(row, templ.data(), sizeof(uint32_t) * w);
            continue;
        }
        for (int i = 0; i < w; ++i)
            row[i] = shade(std::max(colGrid[i], rowGrid[j]), std::max(colAxis[i], rowAxis[j]));
    }
}

void rasterizeExplicitCurve(CoverageBuffer& cov, const GraphView& view, const GraphAnalysis& analysis) {
    if (!analysis.isValid || view.w <= 0 || view.h <= 0) return;
    ExplicitSampler(cov, view, analysis).run();
}

void rasterizeImplicitCurve(CoverageBuffer& cov, const GraphView& view, const GraphAnalysis& analysis) {
    if (!analysis.isValid || view.w <= 0 || view.h <= 0) return;
    ImplicitTracer(cov, view, analysis).run();
}

float curveStrokeAlpha(double dist) {
    if (!(dist < kStrokeReach)) return 0.0f;
    double core = std::clamp(1.6 - dist, 0.0, 1.0);
    double glow = std::clamp(1.0 - dist * 0.35, 0.0, 1.0) * 0.35;
    return (float)std::min(1.0, core + glow);
}

void rasterizeSegment(CoverageBuffer& cov, double x0, double y0, double x1, double y1,
                      int rowBegin, int rowEnd) {
    if (!(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1))) return;
    const double R = kStrokeReach;
    const double pad = R + 2.0;
    if (!clipToRect(x0, y0, x1, y1, -pad, -pad, cov.w + pad, cov.h + pad)) return;
    rowBegin = std::max(rowBegin, 0);
    rowEnd = std::min(rowEnd, cov.h);
    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    const int j0 = std::max(rowBegin, (int)std::floor(std::min(y0, y1) - R - 0.5));
    const int j1 = std::min(rowEnd - 1, (int)std::ceil(std::max(y0, y1) + R - 0.5));
    for (int j = j0; j <= j1; ++j) {
        const double cy = j + 0.5;
        double xa, xb;
        if (std::fabs(dy) < 1e-12) {
            xa = std::min(x0, x1);
            xb = std::max(x0, x1);
        } else {
            // Part of the segment within R rows of this pixel row.
            double ta = (cy - R - y0) / dy, tb = (cy + R - y0) / dy;
            if (ta > tb) std::swap(ta, tb);
            ta = std::max(ta, 0.0);
            tb = std::min(tb, 1.0);
            if (ta > tb) continue;
            xa = x0 + dx * ta;
            xb = x0 + dx * tb;
            if (xa > xb) std::swap(xa, xb);
        }
        const int i0 = std::max(0, (int)std::floor(xa - R - 0.5));
        const int i1 = std::min(cov.w - 1, (int)std::ceil(xb + R - 0.5));
        float* row = &cov.alpha[(size_t)j * cov.w];
        for (int i = i0; i <= i1; ++i) {
            const double cx = i + 0.5;
            double t = len2 > 0.0 ? ((cx - x0) * dx + (cy - y0) * dy) / len2 : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const double ex = x0 + t * dx - cx, ey = y0 + t * dy - cy;
            const double d2 = ex * ex + ey * ey;
            if (d2 >= R * R) continue;
            float a = curveStrokeAlpha(std::sqrt(d2));
            if (a > row[i]) row[i] = a;
        }
    }
}

void compositeCoverage(std::vector<uint32_t>& pixels, const CoverageBuffer& cov, COLORREF color) {
    const Rgb c = toRgb(color);
    const size_t n = std::min(pixels.size(), cov.alpha.size());
    for (size_t k = 0; k < n; ++k) {
        float a = cov.alpha[k];
        if (a <= 0.0f) continue;
        uint32_t p = pixels[k];
        Rgb base{ (float)((p >> 16) & 0xFF), (float)((p >> 8) & 0xFF), (float)(p & 0xFF) };
        pixels[k] = packRgb(mix(base, c, std::min(a, 1.0f)));
    }
}
