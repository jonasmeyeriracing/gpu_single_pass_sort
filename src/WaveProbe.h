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
// The survey (--wave-probe without --wave-size) instead compiles it without [WaveSize] and with
// [WaveSize(N)] for every power of two N in the device's WaveLaneCountMin..Max, one verdict each.

struct WaveProbeVariant
{
    uint32_t attributeSize = 0; // compiled with [WaveSize(attributeSize)]; 0 = without [WaveSize]
    uint32_t groupSize = 0;
    ComPtr<IDxcBlob> shader;
};

struct WaveProbeSet
{
    uint32_t waveSize = 0;  // WAVE_SIZE of the sort shaders of this configuration
    bool attribute = false; // the sort shaders carry [WaveSize(WAVE_SIZE)]
    // Survey: one verdict per variant (without [WaveSize], every [WaveSize(N)]) instead of one for
    // the sort shaders' configuration. laneMin / laneMax: the device's WaveLaneCountMin / Max.
    bool survey = false;
    uint32_t laneMin = 0;
    uint32_t laneMax = 0;
    std::vector<WaveProbeVariant> variants;
};

// Compiles the probe for one wave configuration: with [WaveSize(waveSize)] if 'attribute' (only
// for group sizes >= waveSize), and always without, for every group size. Throws on errors.
WaveProbeSet CompileWaveProbe(ShaderCompiler& compiler, uint32_t waveSize, bool attribute,
                              const std::vector<uint32_t>& groupSizes);

// Compiles the survey for a device with wave lanes laneMin..laneMax: without [WaveSize] for every
// group size, then [WaveSize(N)] for every power of two N (4..128) in laneMin..laneMax, for the
// group sizes >= N. waveSize / attribute: the sort shaders' configuration on that device (only
// marks the matching verdict). Throws on errors.
WaveProbeSet CompileWaveProbeSurvey(ShaderCompiler& compiler, uint32_t laneMin, uint32_t laneMax, uint32_t waveSize,
                                    bool attribute, const std::vector<uint32_t>& groupSizes);

// What a set probes, e.g. "without [WaveSize], [WaveSize(32)], [WaveSize(64)]; groups 64/512/1024".
std::string DescribeWaveProbe(const WaveProbeSet& set);

struct WaveProbeReport
{
    // "Wave probe: ..." lines (one per variant, indented detail lines for mismatches) and verdict
    // lines ("Wave probe verdict...: OK ..." or "WAVE PROBE WARNING...: ..."): one for the sort
    // shaders' configuration, or in a survey one per variant.
    std::vector<std::string> lines;
    // One short verdict per variant in a survey (per configuration otherwise), e.g.
    // "[WaveSize(64)]: OK, 64 lanes".
    std::vector<std::string> summary;
    // The variant that matches the sort shaders' configuration ran with WAVE_SIZE lanes and
    // lane = SV_GroupIndex % WAVE_SIZE for every group size (survey: every variant is OK).
    bool configOk = true;
    std::string problem; // short reason if !configOk
};

// Runs the probe on the benchmark's device and checks the results. Throws DeviceLostError on a
// device loss or fence timeout.
WaveProbeReport RunWaveProbe(GpuBenchmark& bench, const WaveProbeSet& set);
