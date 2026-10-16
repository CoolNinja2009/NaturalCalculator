// graph_raster.h
//
// Double-precision (FP64) software rasterizer for the live graph panel.
//
// Curve geometry is computed in double on the CPU and only converted to pixel
// space (where float-sized numbers are plenty) right before rasterization.
// That keeps lines one crisp stroke wide at zoom depths far beyond what the
// float32 GPU shader can resolve, and lets explicit curves use adaptive
// sampling that respects jumps, poles and domain edges.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include "graph.h"

// Per-pixel curve coverage in [0, 1]. Segments combine with max(), so joints
// and overlapping strokes never darken.
struct CoverageBuffer {
    int w = 0;
    int h = 0;
    std::vector<float> alpha;

    void reset(int width, int height);
};

// Runs fn(i) for every i in [0, count) on a persistent worker pool and blocks
// until all calls finish. fn must not throw and must not call parallelFor.
void parallelFor(int count, const std::function<void(int)>& fn);

// Background, major/minor grid lines and the two axes as 0x00RRGGBB pixels in
// top-down DIB order. xStep/yStep are the major grid spacings in math units.
void rasterizeGrid(std::vector<uint32_t>& pixels, const GraphView& view,
                   double xStep, double yStep, const GraphPalette& palette);

// y = f(x) or x = f(y): adaptive FP64 sampling into anti-aliased strokes.
void rasterizeExplicitCurve(CoverageBuffer& cov, const GraphView& view,
                            const GraphAnalysis& analysis);

// F(x, y) = 0: marching squares on an FP64 sample grid, with pole and jump
// rejection so asymptotes never turn into fake curve segments.
void rasterizeImplicitCurve(CoverageBuffer& cov, const GraphView& view,
                            const GraphAnalysis& analysis);

// Anti-aliased stroke for one pixel-space segment, restricted to pixel rows
// [rowBegin, rowEnd) so bands can be rasterized in parallel.
void rasterizeSegment(CoverageBuffer& cov, double x0, double y0, double x1, double y1,
                      int rowBegin = 0, int rowEnd = 1 << 30);

// Stroke alpha for a pixel whose center lies dist pixels from the curve:
// a solid ~1.6 px core with a soft glow, matching the D3D11 shader.
float curveStrokeAlpha(double dist);

// Blends color over pixels using cov as alpha.
void compositeCoverage(std::vector<uint32_t>& pixels, const CoverageBuffer& cov, COLORREF color);
