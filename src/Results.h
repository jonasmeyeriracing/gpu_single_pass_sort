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

struct GpuRecord
{
    std::string name;
    std::string driver;
    uint64_t timestampFrequency = 0;
    double wallSeconds = 0;
    std::vector<ComboRecord> combos;
    std::string error; // non-empty if the GPU run aborted
};

struct RunInfo
{
    std::string date;
    std::string shaderDir;
    std::string commandLine;
    uint32_t iterations = 0;
    uint32_t warmup = 0;
    std::vector<uint32_t> workloadIds;
    std::vector<std::string> algorithms;
    std::vector<std::string> skippedAdapters;
    std::vector<GpuRecord> gpus;
    double totalSeconds = 0;
};

// Workload size distribution table (over the measured iterations).
std::string FormatSizeDistribution(const std::vector<uint32_t>& workloadIds, uint32_t iterations);

std::string FormatResults(const RunInfo& info);
