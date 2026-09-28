#pragma once

#include "Benchmark.h"
#include "Common.h"
#include "ShaderCompiler.h"

#include <dxcapi.h>

#include <string>
#include <vector>

// Wave probe: a framework diagnostic that checks what wave the driver really runs a shader with,
// before any sort runs. A tiny compute shader (embedded in the exe, one group per dispatch) writes
// per thread WaveGetLaneCount(), WaveGetLaneIndex(), SV_GroupIndex and the results of a few
// cross-lane operations (WaveReadLaneAt with lane ^ 32, lane ^ 1, (lane + 16) % count and count - 1,
// WavePrefixSum, WaveActiveSum, WaveActiveCountBits, WaveActiveBallot). The CPU groups the threads
// into waves (by WaveReadLaneFirst) and checks every value.
//
// It is compiled like the sort shaders (-D WAVE_SIZE, [WaveSize(WAVE_SIZE)] when the configuration
// uses the attribute) and also without [WaveSize] (the driver's own choice), for several group sizes.

struct WaveProbeVariant
{
    bool attribute = false; // compiled with [WaveSize(WAVE_SIZE)]
    uint32_t groupSize = 0;
    ComPtr<IDxcBlob> shader;
};

struct WaveProbeSet
{
    uint32_t waveSize = 0;  // WAVE_SIZE of the sort shaders of this configuration
    bool attribute = false; // the sort shaders carry [WaveSize(WAVE_SIZE)]
    std::vector<WaveProbeVariant> variants;
};

// Compiles the probe for one wave configuration: with [WaveSize(waveSize)] if 'attribute' (only
// for group sizes >= waveSize), and always without, for every group size. Throws on errors.
WaveProbeSet CompileWaveProbe(ShaderCompiler& compiler, uint32_t waveSize, bool attribute,
                              const std::vector<uint32_t>& groupSizes);

struct WaveProbeReport
{
    // "Wave probe: ..." lines (one per variant, indented detail lines for mismatches) and a verdict
    // line ("Wave probe verdict: OK ..." or "WAVE PROBE WARNING: ...").
    std::vector<std::string> lines;
    // The variant that matches the sort shaders' configuration ran with WAVE_SIZE lanes and
    // lane = SV_GroupIndex % WAVE_SIZE for every group size.
    bool configOk = true;
    std::string problem; // short reason if !configOk
};

// Runs the probe on the benchmark's device and checks the results. Throws DeviceLostError on a
// device loss or fence timeout.
WaveProbeReport RunWaveProbe(GpuBenchmark& bench, const WaveProbeSet& set);
