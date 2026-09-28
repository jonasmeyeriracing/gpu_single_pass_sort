#pragma once

#include "Common.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <string>
#include <vector>

struct GpuInfo
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    std::string name;
    std::string driver;          // UMD driver version, e.g. "32.0.16.1088"
    uint64_t dedicatedVideoMemory = 0;
    bool isWarp = false;
    // D3D12_FEATURE_DATA_D3D12_OPTIONS1 (0 if wave ops are not supported).
    uint32_t waveLaneCountMin = 0;
    uint32_t waveLaneCountMax = 0;
};

// Enables the D3D12 debug layer, optionally with GPU-based validation (must be called before any
// device is created). Returns false if the layer (or GBV) is not available.
bool EnableD3D12DebugLayer(bool gpuBasedValidation);

// Enables DRED auto-breadcrumbs, breadcrumb contexts and page-fault reporting (must be called
// before any device is created). Returns false if DRED is not available.
bool EnableDred();

// Human-readable DRED report for a removed device: removal reason, per command list the last
// completed breadcrumb op (plus the ops around it and marker strings), and the page-fault VA with
// the allocations it hit. Returns a note instead if DRED data is unavailable.
std::string FormatDredReport(ID3D12Device* device);

// Returns (and clears) messages stored by the debug layer's info queue; empty if the layer is off.
std::vector<std::string> DrainDebugMessages(ID3D12Device* device);

// Prints every DXGI adapter with its LUID / ids / flags (diagnostics, no GPU work).
void PrintAdapterList();

// Enumerates qualifying adapters (hardware, D3D12 device creation succeeds, SM 6.6+), in
// high-performance-first order. With 'warp' only the WARP adapter is returned (if it qualifies).
// 'filter' is a case-insensitive name substring; empty = all.
// Adapters that were skipped are described in 'skipped'.
std::vector<GpuInfo> EnumerateGpus(bool warp, const std::string& filter, std::vector<std::string>& skipped);
