// gpu_graph.h
//
// Hardware-accelerated implicit-curve engine for NaturalCalculator using
// Direct3D 11. Equations F(x, y) = 0 are compiled to HLSL pixel shaders that
// evaluate F per pixel and output anti-aliased stroke coverage. Shader math
// is float32, so the grapher only uses this path while float32 can resolve
// the view; deeper zooms (and explicit curves) go to the FP64 rasterizer.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "graph.h"
#include "graph_raster.h"

// Check if GPU acceleration (Hardware or WARP rasterizer) is active and available
bool isGpuGraphAvailable();

// Check if device is backed by true hardware adapter (false if WARP software rasterizer)
bool isGpuGraphHardwareAdapter();

// Returns the last shader compilation error message, if any
const char* getGpuGraphLastError();

// Explicitly initialize GPU graphing resources (called during app startup)
bool initGpuGraph();

// Release all GPU resources on shutdown
void shutdownGpuGraph();

// Convert GraphAnalysis to HLSL math expression for GPU shader compilation
bool analysisToHLSL(const GraphAnalysis& analysis, std::string& outExpr);

// True while float32 shader coordinates still resolve each pixel of view
// with ample headroom (pixel size >= 256 float ulps of the largest coordinate).
bool gpuCanResolveView(const GraphView& view);

// Renders the stroke coverage of an implicit curve F(x, y) = 0 into cov on
// the GPU, with the same stroke profile and pole/jump rejection as the FP64
// rasterizer. Returns false and leaves cov untouched when this path can't be
// used (no hardware adapter, view too deep for float32, unsupported
// construct, compile or device failure); the caller then rasterizes on the CPU.
bool renderImplicitCoverageGpu(const GraphAnalysis& analysis, const GraphView& view,
                               CoverageBuffer& cov);
