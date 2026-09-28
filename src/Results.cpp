#include "Results.h"

#include <algorithm>
#include <numeric>

Stats ComputeStats(std::vector<double> v)
{
    Stats s;
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    s.min = v.front();
    s.max = v.back();
    s.median = (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(n);
    // nearest-rank p95
    size_t rank = static_cast<size_t>(0.95 * static_cast<double>(n) + 0.999999);
    rank = std::clamp<size_t>(rank, 1, n);
    s.p95 = v[rank - 1];
    return s;
}

std::string FormatSizeDistribution(const std::vector<uint32_t>& workloadIds, uint32_t iterations)
{
    std::string out;
    out += Format("Workload size distribution (%u measured iterations x %u sorts; counts are sorts per size tier)\n",
                  iterations, kSortsPerIteration);
    out += Format("  %-14s %13s %10s", "workload", "total elems", "elems/iter");
    for (uint32_t t = 0; t < kNumSizeTiers; ++t)
        out += Format(" %9s", SizeTierName(t));
    out += Format(" %6s  %s\n", "max", "distribution");

    for (uint32_t id : workloadIds)
    {
        uint64_t total = 0;
        uint64_t tiers[kNumSizeTiers] = {};
        uint32_t maxSize = 0;
        for (uint32_t it = 0; it < iterations; ++it)
        {
            uint32_t sizes[kSortsPerIteration];
            GenerateSizes(id, it, sizes);
            for (uint32_t s : sizes)
            {
                total += s;
                ++tiers[SizeTier(s)];
                maxSize = std::max(maxSize, s);
            }
        }
        out += Format("  %-14s %13llu %10.1f", Workloads()[id].name, static_cast<unsigned long long>(total),
                      static_cast<double>(total) / iterations);
        for (uint32_t t = 0; t < kNumSizeTiers; ++t)
            out += Format(" %9llu", static_cast<unsigned long long>(tiers[t]));
        out += Format(" %6u  %s\n", maxSize, Workloads()[id].description);
    }
    return out;
}

std::string FormatResults(const RunInfo& info)
{
    std::string out;
    out += "GpuSort benchmark results\n";
    out += "=========================\n";
    out += Format("Date:         %s\n", info.date.c_str());
    out += Format("Command line: %s\n", info.commandLine.c_str());
    out += Format("Shader dir:   %s\n", info.shaderDir.c_str());
    out += Format("Iterations:   %u measured + %u warmup per GPU x workload x algorithm, %u sorts per iteration\n",
                  info.iterations, info.warmup, kSortsPerIteration);
    out += Format("Cache flush:  %llu MB read+write compute pass before every timed sort\n",
                  static_cast<unsigned long long>(GpuBenchmark::kFlushBytes >> 20));
    out += "Timing:       GPU timestamps around the sort ExecuteIndirect dispatches only, per iteration (all 20 sorts)\n";
    out += "GPUs:\n";
    for (size_t g = 0; g < info.gpus.size(); ++g)
    {
        const GpuRecord& gpu = info.gpus[g];
        out += Format("  [%zu] %s  (driver %s, timestamp freq %llu Hz, wall time %.1f s)\n", g, gpu.name.c_str(),
                      gpu.driver.c_str(), static_cast<unsigned long long>(gpu.timestampFrequency), gpu.wallSeconds);
        out += Format("      wave lanes %u-%u, shaders compiled with WAVE_SIZE=%u%s\n", gpu.waveLaneCountMin,
                      gpu.waveLaneCountMax, gpu.waveSize, gpu.waveSizeAttribute ? " + [WaveSize]" : "");
    }
    for (const auto& s : info.skippedAdapters)
        out += Format("  skipped: %s\n", s.c_str());
    out += Format("Total benchmark time: %.1f s (after confirmation)\n\n", info.totalSeconds);

    out += FormatSizeDistribution(info.workloadIds, info.iterations);
    out += "\n";

    size_t algoWidth = 9;
    for (const auto& a : info.algorithms)
        algoWidth = std::max(algoWidth, a.size());

    out += "Sort timings in microseconds (per iteration = 20 sorts)\n";
    for (size_t g = 0; g < info.gpus.size(); ++g)
    {
        const GpuRecord& gpu = info.gpus[g];
        out += Format("\n[%zu] %s  (WAVE_SIZE %u)\n", g, gpu.name.c_str(), gpu.waveSize);
        out += Format("  %-14s %-*s %9s %9s %9s %9s %9s %6s\n", "workload", static_cast<int>(algoWidth), "algorithm",
                      "min", "median", "mean", "p95", "max", "fails");
        uint32_t lastWorkload = UINT32_MAX;
        for (const auto& c : gpu.combos)
        {
            const Stats st = ComputeStats(c.result.timesUs);
            const char* wname = c.workloadId != lastWorkload ? Workloads()[c.workloadId].name : "";
            lastWorkload = c.workloadId;
            out += Format("  %-14s %-*s %9.2f %9.2f %9.2f %9.2f %9.2f %6u\n", wname, static_cast<int>(algoWidth),
                          c.algorithm.c_str(), st.min, st.median, st.mean, st.p95, st.max, c.result.failures);
        }
        // Sweep workload: the same numbers grouped by sort size (measured iteration i has size
        // SweepSizes()[i % n]; all 20 sorts of an iteration have that size).
        bool anySweep = false;
        for (const auto& c : gpu.combos)
            anySweep |= IsSweepWorkload(c.workloadId);
        if (anySweep)
        {
            const std::vector<uint32_t>& sizes = SweepSizes();
            const char* statNames[3] = {"median", "mean", "p95"};
            for (int stat = 0; stat < 3; ++stat)
            {
                out += Format("\n  sweep: %s us per iteration by sort size (all 20 sorts of an iteration have that size)\n",
                              statNames[stat]);
                out += Format("  %-*s", static_cast<int>(algoWidth), "algorithm");
                for (uint32_t size : sizes)
                    out += Format(" %7u", size);
                out += "\n";
                for (const auto& c : gpu.combos)
                {
                    if (!IsSweepWorkload(c.workloadId))
                        continue;
                    out += Format("  %-*s", static_cast<int>(algoWidth), c.algorithm.c_str());
                    for (size_t si = 0; si < sizes.size(); ++si)
                    {
                        std::vector<double> v;
                        for (size_t i = si; i < c.result.timesUs.size(); i += sizes.size())
                            v.push_back(c.result.timesUs[i]);
                        if (v.empty())
                        {
                            out += Format(" %7s", "-");
                            continue;
                        }
                        const Stats st = ComputeStats(v);
                        out += Format(" %7.2f", stat == 0 ? st.median : stat == 1 ? st.mean : st.p95);
                    }
                    out += "\n";
                }
            }
        }
        if (!gpu.error.empty())
            out += Format("  ERROR: %s\n", gpu.error.c_str());
        for (const auto& c : gpu.combos)
        {
            for (const auto& m : c.result.failureMessages)
                out += Format("  failure [%s / %s] %s\n", Workloads()[c.workloadId].name, c.algorithm.c_str(),
                              m.c_str());
        }
    }
    return out;
}
