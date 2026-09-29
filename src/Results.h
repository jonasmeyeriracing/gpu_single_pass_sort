#pragma once

#include "Benchmark.h"
#include "WaveProbe.h"

#include <string>
#include <vector>

struct Stats
{
    double min = 0, median = 0, mean = 0, p95 = 0, max = 0;
    double p99 = 0;    // nearest rank, like p95 (results CSV only)
    double stddev = 0; // sample standard deviation (n - 1; 0 for n < 2) (results CSV only)
};

Stats ComputeStats(std::vector<double> values);

struct ComboRecord
{
    uint32_t workloadId = 0;
    std::string algorithm;
    ComboResult result;
    FlushMode flush = FlushMode::Full; // the algorithm's flush mode in this run
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
    bool uma = false; // integrated GPU (D3D12_FEATURE_DATA_ARCHITECTURE::UMA)
    uint64_t timestampFrequency = 0;
    uint32_t waveLaneCountMin = 0;  // D3D12_FEATURE_DATA_D3D12_OPTIONS1
    uint32_t waveLaneCountMax = 0;
    uint32_t waveSize = 0;          // WAVE_SIZE the shaders were compiled with
    bool waveSizeAttribute = false; // compiled with [WaveSize(WAVE_SIZE)]
    std::vector<std::string> waveProbe; // wave probe report lines (WaveProbeReport::lines)
    std::vector<std::string> waveProbeSummary; // one verdict per wave configuration (WaveProbeReport::summary)
    bool waveProbeWarning = false;      // the probe found the wrong lane count / a broken lane mapping
    std::vector<WaveProbeRow> waveProbeRows; // per probed wave configuration (WaveProbeReport::rows)
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
    std::string runTimestamp; // ISO 8601 local time with UTC offset, e.g. 2026-09-29T14:30:12.345+02:00
    std::string runId;        // <yyyymmdd>T<hhmmss>.<ms>_<computer>: unique per GpuSort run (CSV key)
    std::string runKind;      // "benchmark", "smoke" or "wave_probe" (CSV column run_kind)
    std::string packageCommit; // git commit the exe was built from (BuildInfo.h), or "unknown"
    std::string shaderSet;    // name of the shader directory, e.g. "shaders" or "pass4"
    std::string csvFiles;     // the CSV file names, for the results .txt header (empty: none)
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
