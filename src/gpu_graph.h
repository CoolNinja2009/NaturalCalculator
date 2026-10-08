// gpu_graph.h
//
// Hardware-accelerated 2D graphing engine for NaturalCalculator using Direct3D 11.
// Performs per-pixel equation inference, anti-aliased subpixel curve rasterization,
// screen-space implicit contouring, and dynamic HLSL shader compilation on the GPU
// with instant, zero-flicker GDI compositing and transparent CPU fallback.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "graph.h"

// Check if hardware GPU acceleration is active and available
bool isGpuGraphAvailable();

// Explicitly initialize GPU graphing resources (called during app startup)
bool initGpuGraph();

// Release all GPU resources on shutdown
void shutdownGpuGraph();

// Render graph onto hdc within graphRect using Direct3D 11 hardware acceleration.
// Returns true on successful GPU render, or false if CPU fallback is needed.
bool renderGraphGpu(HDC hdc, const RECT& graphRect, const GraphAnalysis& analysis,
                    GraphState& state, const Theme& theme, HFONT font, HFONT smallFont,
                    bool degreesMode);
