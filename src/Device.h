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
};

// Enables the D3D12 debug layer (must be called before any device is created).
bool EnableD3D12DebugLayer();

// Returns (and clears) messages stored by the debug layer's info queue; empty if the layer is off.
std::vector<std::string> DrainDebugMessages(ID3D12Device* device);

// Prints every DXGI adapter with its LUID / ids / flags (diagnostics, no GPU work).
void PrintAdapterList();

// Enumerates qualifying adapters (hardware, D3D12 device creation succeeds, SM 6.6+), in
// high-performance-first order. With 'warp' only the WARP adapter is returned (if it qualifies).
// 'filter' is a case-insensitive name substring; empty = all.
// Adapters that were skipped are described in 'skipped'.
std::vector<GpuInfo> EnumerateGpus(bool warp, const std::string& filter, std::vector<std::string>& skipped);
