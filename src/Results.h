#pragma once

#include "Benchmark.h"

#include <string>
#include <vector>

struct Stats
{
    double min = 0, median = 0, mean = 0, p95 = 0, max = 0;
};

Stats ComputeStats(std::vector<double> values);

struct ComboRecord
{
    uint32_t workloadId = 0;
    std::string algorithm;
    ComboResult result;
};

// --smoke: an algorithm that failed verification (its remaining workloads were skipped).
struct SmokeFailure
{
    std::string algorithm;
    std::string workload; // the first failing workload
    std::string reason;   // if non-empty: not run at all, for this reason (e.g. the wave probe)
};

struct GpuRecord
{
    std::string name;
    std::string driver;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t dedicatedVideoMemory = 0;
    uint64_t timestampFrequency = 0;
    uint32_t waveLaneCountMin = 0;  // D3D12_FEATURE_DATA_D3D12_OPTIONS1
    uint32_t waveLaneCountMax = 0;
    uint32_t waveSize = 0;          // WAVE_SIZE the shaders were compiled with
    bool waveSizeAttribute = false; // compiled with [WaveSize(WAVE_SIZE)]
    std::vector<std::string> waveProbe; // wave probe report lines (WaveProbeReport::lines)
    std::vector<std::string> waveProbeSummary; // one verdict per wave configuration (WaveProbeReport::summary)
    bool waveProbeWarning = false;      // the probe found the wrong lane count / a broken lane mapping
    double wallSeconds = 0;
    // Run-time estimate made up front (main.cpp): wall seconds per iteration from the calibration
    // (or a rough guess if !calibrated) and the estimate for this GPU's whole run.
    bool calibrated = false;
    double secondsPerIterationEstimate = 0;
    double estimatedSeconds = 0;
    std::vector<ComboRecord> combos;
    std::vector<SmokeFailure> smokeFailed; // --smoke only
    std::string error; // non-empty if the GPU run aborted
};

struct AlgorithmInfo
{
    std::string name;
    FlushMode flush = FlushMode::Full;
    std::vector<size_t> dxilBytes; // per dispatch (DXIL container size, for the first wave configuration)
};

struct RunInfo
{
    std::string date;
    std::string computerName;
    std::string label; // --label
    std::string shaderDir;
    std::string commandLine;
    uint32_t iterations = 0;
    uint32_t warmup = 0;
    std::vector<uint32_t> workloadIds;
    std::vector<std::string> algorithms;
    std::vector<AlgorithmInfo> algorithmInfos;
    FlushMode defaultFlush = FlushMode::Full; // --flush-mode
    uint32_t dxilWaveSize = 0;                // WAVE_SIZE of the algorithmInfos DXIL sizes
    std::vector<std::string> skippedAdapters;
    std::vector<GpuRecord> gpus;
    double totalSeconds = 0;
    double estimatedSeconds = 0;        // up-front run-time estimate, all GPUs (0: none, e.g. --wave-probe)
    uint32_t calibrationIterations = 0; // iterations per GPU of the calibration behind it
};

// Workload size distribution table (over the measured iterations).
std::string FormatSizeDistribution(const std::vector<uint32_t>& workloadIds, uint32_t iterations);

std::string FormatResults(const RunInfo& info);
