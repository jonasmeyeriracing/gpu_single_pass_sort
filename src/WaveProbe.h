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
// pass5: plus replicas of the pass2 radix sort's shuffle scans (WaveReadLaneAt(x, (lane - d) &
// (WAVE_SIZE - 1)) with an 'if (lane >= d)' guard): 8 interleaved chains with every lane active
// ("shflscan x8"), and wave 0's table scan after a groupshared load under 'if (lane < 16)'
// ("shflscan after if", the only wave64-specific control flow of the pass2 radix) and after a
// branch-free load ("shflscan after select"). A mismatch there is reported as a cross-lane note,
// not as a failure of the configuration (the current shaders do not use these scans at wave64);
// it tells which mechanism broke the pass2 radix at wave64 (see _test/pass5/notes.md).
//
// It is compiled like the sort shaders (-D WAVE_SIZE, [WaveSize(WAVE_SIZE)] when the configuration
// uses the attribute) and also without [WaveSize] (the driver's own choice), for several group sizes.
// The survey (--wave-probe without --wave-size) instead compiles it without [WaveSize] and with
// [WaveSize(N)] for every power of two N in the device's WaveLaneCountMin..Max, one verdict each.

struct WaveProbeVariant
{
    uint32_t attributeSize = 0;  // compiled with [WaveSize(attributeSize)]; 0 = without [WaveSize]
    uint32_t defineWaveSize = 0; // compiled with -D WAVE_SIZE=defineWaveSize
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

// One probed wave configuration (variant attribute size), for the wave probe CSV.
struct WaveProbeRow
{
    uint32_t attributeSize = 0; // compiled with [WaveSize(attributeSize)]; 0 = without [WaveSize]
    std::string variant;        // "without [WaveSize]" / "[WaveSize(64)]"
    std::string groupSizes;     // group sizes that ran, e.g. "64/512/1024" (empty: did not run)
    std::string observedLanes;  // e.g. "32", or "32 (group 64), 64 (groups 512/1024)"; "none"
    bool sortConfig = false;    // the configuration the sort shaders use
    std::string verdict;        // "OK" / "WARNING"; empty if not judged (outside a survey, only the
                                // sort shaders' configuration gets a verdict)
    std::string problem;        // reason for a WARNING; cross-lane mismatches of an OK verdict
    std::vector<std::string> tests; // per WaveProbeTestColumns(): "OK", "FAIL" or "n/a"
};

// CSV column names of WaveProbeRow::tests, in order.
const std::vector<std::string>& WaveProbeTestColumns();

struct WaveProbeReport
{
    std::vector<WaveProbeRow> rows; // one per variant attribute size, in variant order
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
