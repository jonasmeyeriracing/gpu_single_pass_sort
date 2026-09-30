#include "Results.h"

#include <algorithm>
#include <cmath>
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
    rank = static_cast<size_t>(0.99 * static_cast<double>(n) + 0.999999);
    rank = std::clamp<size_t>(rank, 1, n);
    s.p99 = v[rank - 1];
    if (n > 1)
    {
        double sq = 0.0;
        for (double x : v)
            sq += (x - s.mean) * (x - s.mean);
        s.stddev = std::sqrt(sq / static_cast<double>(n - 1));
    }
    return s;
}

std::string FormatSizeDistribution(const std::vector<uint32_t>& workloadIds, uint32_t iterations, uint32_t numSorts)
{
    std::string out;
    out += Format("Workload size distribution (%u measured iterations x %u sorts; counts are sorts per size tier)\n",
                  iterations, numSorts);
    out += Format("  %-14s %13s %10s", "workload", "total elems", "elems/iter");
    for (uint32_t t = 0; t < kNumSizeTiers; ++t)
        out += Format(" %9s", SizeTierName(t));
    out += Format(" %6s  %s\n", "max", "distribution");

    std::vector<uint32_t> sizes(numSorts);
    for (uint32_t id : workloadIds)
    {
        uint64_t total = 0;
        uint64_t tiers[kNumSizeTiers] = {};
        uint32_t maxSize = 0;
        for (uint32_t it = 0; it < iterations; ++it)
        {
            GenerateSizes(id, it, numSorts, sizes.data());
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
    out += Format("Computer:     %s\n", info.computerName.c_str());
    if (!info.label.empty())
        out += Format("Label:        %s\n", info.label.c_str());
    out += Format("Command line: %s\n", info.commandLine.c_str());
    out += Format("Shader dir:   %s\n", info.shaderDir.c_str());
    if (!info.algoFile.empty())
        out += Format("Algo list:    %s\n", info.algoFile.c_str());
    if (!info.csvFiles.empty())
        out += Format("CSV files:    %s\n", info.csvFiles.c_str());
    if (info.sortCounts.size() > 1)
    {
        // Several sort counts: every GPU x workload x algorithm ran once per count.
        std::string counts;
        for (size_t ci = 0; ci < info.sortCounts.size(); ++ci)
        {
            const uint32_t it = ci < info.iterationsPerCount.size() ? info.iterationsPerCount[ci] : info.iterations;
            counts += Format("%s%u sorts: %u", counts.empty() ? "" : ", ", info.sortCounts[ci], it);
            if (ci < info.iterationsIntegratedPerCount.size() && info.iterationsIntegratedPerCount[ci] != it)
                counts += Format(" (%u on integrated GPUs)", info.iterationsIntegratedPerCount[ci]);
        }
        out += Format("Iterations:   per sorts-per-iteration count (every GPU x workload x algorithm runs once per count,\n"
                      "              in this order), measured + %u warmup: %s\n",
                      info.warmup, counts.c_str());
    }
    else if (info.iterationsIntegrated && info.iterationsIntegrated != info.iterations)
        out += Format("Iterations:   %u measured (%u on integrated GPUs) + %u warmup per GPU x workload x algorithm, %u sorts\n"
                      "              per iteration\n",
                      info.iterations, info.iterationsIntegrated, info.warmup, info.sortCounts[0]);
    else
        out += Format("Iterations:   %u measured + %u warmup per GPU x workload x algorithm, %u sorts per iteration\n",
                      info.iterations, info.warmup, info.sortCounts[0]);
    out += Format("Cache flush:  %llu MB read+write compute pass before every timed sort, then a DRAIN right before the\n"
                  "              start timestamp (a dispatch on a private 4 KB buffer + UAV barrier), so the tail of the\n"
                  "              flush (barrier wait / cache maintenance a driver defers to the next dispatch) is not\n"
                  "              inside the timed window.\n"
                  "Flush mode:   '%s' for algorithms without their own (the measurement of record).\n"
                  "              *** NOTE: the default is full_d50 (flush, then a ~50 us ALU spin drain) since the final\n"
                  "              *** set; packages up to 627724b used 'full' (flush + the pass5 one-group drain), which\n"
                  "              *** on the RX 7900 XTX still timed ~12 us of post-flush penalty in most small-workload\n"
                  "              *** iterations (mostly_empty median full 14.7 vs full_d50 1.8 us), and pass0-pass4 used\n"
                  "              *** full_legacy (no drain). Results measured with another default are NOT directly\n"
                  "              *** comparable; the flush_mode column / the Algorithms list below says which one ran.\n"
                  "              full = code + data cold (flush, one-group drain, pass5); full_legacy = pre-pass5 full\n"
                  "              (no drain); full_ro = read-only flush, no drain (diagnostic); code = only code cold\n"
                  "              (flush before the upload, drain); data = only data cold (untimed warm-up run of the\n"
                  "              same dispatches, drain); none = no flush (drain)\n"
                  "              Drain suffixes (pass6): <mode>_d<N> = SPIN drain of about N us instead (an ALU-only\n"
                  "              dispatch, %u groups x %u threads, one 16-byte store per thread to the private 4 KB\n"
                  "              buffer, loop count from the per-GPU calibration below, + UAV barrier); _d0 = no drain\n"
                  "              (full_d0 = full_legacy); _dg = the pass5 one-group drain\n",
                  static_cast<unsigned long long>(GpuBenchmark::kFlushBytes >> 20),
                  FlushModeName(info.defaultFlush).c_str(), GpuBenchmark::kSpinGroups, GpuBenchmark::kSpinGroupSize);
    out += info.sortCounts.size() > 1
               ? "Timing:       GPU timestamps around the sort ExecuteIndirect dispatches only, per iteration (all N sorts\n"
                 "              of the batch, N = the sorts per iteration of the table)\n"
               : Format("Timing:       GPU timestamps around the sort ExecuteIndirect dispatches only, per iteration (all %u "
                        "sorts)\n",
                        info.sortCounts[0]);
    out += "GPUs:\n";
    for (size_t g = 0; g < info.gpus.size(); ++g)
    {
        const GpuRecord& gpu = info.gpus[g];
        out += Format("  [%zu] %s  (driver %s, timestamp freq %llu Hz, wall time %.1f s)\n", g, gpu.name.c_str(),
                      gpu.driver.c_str(), static_cast<unsigned long long>(gpu.timestampFrequency), gpu.wallSeconds);
        out += Format("      vendor %04X device %04X, %llu MB VRAM\n", gpu.vendorId, gpu.deviceId,
                      static_cast<unsigned long long>(gpu.dedicatedVideoMemory >> 20));
        out += Format("      wave lanes %u-%u, shaders compiled with WAVE_SIZE=%u%s\n", gpu.waveLaneCountMin,
                      gpu.waveLaneCountMax, gpu.waveSize, gpu.waveSizeAttribute ? " + [WaveSize]" : "");
        if (info.sortCounts.size() > 1)
        {
            std::string counts;
            for (size_t ci = 0; ci < info.sortCounts.size() && ci < gpu.iterationsPerCount.size(); ++ci)
                counts += Format("%s%u at %u sorts", counts.empty() ? "" : ", ", gpu.iterationsPerCount[ci],
                                 info.sortCounts[ci]);
            out += Format("      %s GPU, measured iterations per workload x algorithm: %s\n",
                          gpu.uma ? "integrated (UMA)" : "discrete", counts.c_str());
        }
        else
            out += Format("      %s GPU, %u measured iterations per workload x algorithm\n",
                          gpu.uma ? "integrated (UMA)" : "discrete", gpu.iterations);
        if (gpu.estimatedSeconds > 0 && info.sortCounts.size() > 1)
        {
            std::string rates;
            for (size_t ci = 0; ci < info.sortCounts.size() && ci < gpu.secondsPerIterationPerCount.size(); ++ci)
                rates += Format("%s%.2f ms at %u sorts", rates.empty() ? "" : ", ",
                                gpu.secondsPerIterationPerCount[ci] * 1000.0, info.sortCounts[ci]);
            out += Format("      run-time estimate (%s): %s per iteration -> ~%.0f s for this GPU, actual %.1f s\n",
                          gpu.calibrated ? "calibrated up front" : "rough guess, the calibration failed", rates.c_str(),
                          gpu.estimatedSeconds, gpu.wallSeconds);
        }
        else if (gpu.estimatedSeconds > 0)
            out += Format("      run-time estimate: %.2f ms per iteration (%s) -> ~%.0f s for this GPU, actual %.1f s\n",
                          gpu.secondsPerIterationEstimate * 1000.0,
                          gpu.calibrated ? "calibrated up front" : "rough guess, the calibration failed",
                          gpu.estimatedSeconds, gpu.wallSeconds);
        out += Format("      stable power: %s%s%s\n", gpu.stablePower.c_str(), gpu.stablePowerNote.empty() ? "" : " - ",
                      gpu.stablePowerNote.c_str());
        if (gpu.drainSpinCalibrated)
        {
            const GpuBenchmark::SpinCalibration& c = gpu.drainSpin;
            out += Format("      drain spin: %.2f loop iterations per us (calibrated before the first sort: %u vs 1\n"
                          "              iterations, median of %u each, %.2f us apart; the 1-iteration dispatch + barrier\n"
                          "              takes %.2f us); check: %u us = %u iterations measured %.2f us incl. that overhead\n",
                          c.iterationsPerUs, c.iterations, GpuBenchmark::kSpinReps, c.spanUs, c.overheadUs,
                          GpuBenchmark::kSpinCheckUs, c.checkIterations, c.checkUs);
        }
        for (const auto& line : gpu.waveProbe)
            out += Format("      %s\n", line.c_str());
    }
    for (const auto& s : info.skippedAdapters)
        out += Format("  skipped: %s\n", s.c_str());
    if (info.estimatedSeconds > 0)
        out += Format("Run-time estimate: ~%.0f s up front (per GPU: %u calibration iterations of the upload + flush +\n"
                      "              drain + readback work without a sort, times the iterations of the run)\n",
                      info.estimatedSeconds, info.calibrationIterations);
    out += Format("Total benchmark time: %.1f s (after confirmation)\n\n", info.totalSeconds);

    for (size_t ci = 0; ci < info.sortCounts.size(); ++ci)
    {
        out += FormatSizeDistribution(info.workloadIds,
                                      ci < info.iterationsPerCount.size() ? info.iterationsPerCount[ci] : info.iterations,
                                      info.sortCounts[ci]);
        out += "\n";
    }

    if (!info.algorithmInfos.empty())
    {
        size_t w = 9;
        for (const auto& a : info.algorithmInfos)
            w = std::max(w, a.name.size());
        size_t dw = 10;
        bool anyMeta = false;
        for (const auto& a : info.algorithmInfos)
        {
            dw = std::max(dw, a.dispatchInfo.size());
            anyMeta |= a.pass >= 0 || !a.description.empty() || !a.tags.empty();
        }
        out += Format("Algorithms (flush mode; per dispatch: threads per group t / groupshared bytes B; DXIL container\n"
                      "bytes per dispatch; compiled for WAVE_SIZE=%u%s)\n",
                      info.dxilWaveSize,
                      anyMeta ? "; then the pass that introduced it, its tags [..] and its description" : "");
        for (const auto& a : info.algorithmInfos)
        {
            std::string sizes;
            for (size_t b : a.dxilBytes)
                sizes += Format("%s%zu", sizes.empty() ? "" : " + ", b);
            out += Format("  %-*s  %-11s  %-*s  %s\n", static_cast<int>(w), a.name.c_str(),
                          FlushModeName(a.flush).c_str(), static_cast<int>(dw), a.dispatchInfo.c_str(), sizes.c_str());
            if (anyMeta)
            {
                std::string tags;
                for (const auto& t : a.tags)
                    tags += (tags.empty() ? "" : " ") + t;
                out += Format("  %-*s  %s%s%s%s\n", static_cast<int>(w), "",
                              a.pass >= 0 ? Format("pass %d", a.pass).c_str() : "pass ?",
                              tags.empty() ? "" : (" [" + tags + "]").c_str(), a.description.empty() ? "" : ": ",
                              a.description.c_str());
            }
        }
        out += "\n";
    }

    size_t algoWidth = 9;
    for (const auto& a : info.algorithms)
        algoWidth = std::max(algoWidth, a.size());

    const bool multi = info.sortCounts.size() > 1;
    if (multi)
        out += "Sort timings in microseconds (per iteration = one batch of N sorts; per GPU one table per N)\n";
    else
        out += Format("Sort timings in microseconds (per iteration = %u sorts)\n", info.sortCounts[0]);
    for (size_t g = 0; g < info.gpus.size(); ++g)
    {
        const GpuRecord& gpu = info.gpus[g];
        out += Format("\n[%zu] %s  (WAVE_SIZE %u)\n", g, gpu.name.c_str(), gpu.waveSize);
        for (size_t ci = 0; ci < info.sortCounts.size(); ++ci)
        {
            const uint32_t numSorts = info.sortCounts[ci];
            std::vector<const ComboRecord*> combos;
            for (const auto& c : gpu.combos)
            {
                if (c.sortsPerIteration == numSorts)
                    combos.push_back(&c);
            }
            if (multi)
            {
                const uint32_t it = ci < gpu.iterationsPerCount.size() ? gpu.iterationsPerCount[ci] : gpu.iterations;
                out += Format("\n  === %u sorts per iteration (%u measured iterations per workload x algorithm) ===\n",
                              numSorts, it);
                if (combos.empty())
                {
                    out += "  (not run)\n";
                    continue;
                }
            }
            out += Format("  %-14s %-*s %9s %9s %9s %9s %9s %6s\n", "workload", static_cast<int>(algoWidth), "algorithm",
                          "min", "median", "mean", "p95", "max", "fails");
            uint32_t lastWorkload = UINT32_MAX;
            for (const ComboRecord* c : combos)
            {
                const Stats st = ComputeStats(c->result.timesUs);
                const char* wname = c->workloadId != lastWorkload ? Workloads()[c->workloadId].name : "";
                lastWorkload = c->workloadId;
                out += Format("  %-14s %-*s %9.2f %9.2f %9.2f %9.2f %9.2f %6u\n", wname, static_cast<int>(algoWidth),
                              c->algorithm.c_str(), st.min, st.median, st.mean, st.p95, st.max, c->result.failures);
            }
            // Sweep workload: the same numbers grouped by sort size (measured iteration i has size
            // SweepSizes()[i % n]; all sorts of an iteration have that size).
            bool anySweep = false;
            for (const ComboRecord* c : combos)
                anySweep |= IsSweepWorkload(c->workloadId);
            if (anySweep)
            {
                const std::vector<uint32_t>& sizes = SweepSizes();
                const char* statNames[3] = {"median", "mean", "p95"};
                for (int stat = 0; stat < 3; ++stat)
                {
                    out += Format("\n  sweep: %s us per iteration by sort size (all %u sorts of an iteration have that "
                                  "size)\n",
                                  statNames[stat], numSorts);
                    out += Format("  %-*s", static_cast<int>(algoWidth), "algorithm");
                    for (uint32_t size : sizes)
                        out += Format(" %7u", size);
                    out += "\n";
                    for (const ComboRecord* c : combos)
                    {
                        if (!IsSweepWorkload(c->workloadId))
                            continue;
                        out += Format("  %-*s", static_cast<int>(algoWidth), c->algorithm.c_str());
                        for (size_t si = 0; si < sizes.size(); ++si)
                        {
                            std::vector<double> v;
                            for (size_t i = si; i < c->result.timesUs.size(); i += sizes.size())
                                v.push_back(c->result.timesUs[i]);
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
            if (multi)
            {
                // CPU side of the batches (overlaps the GPU work of the other batch in flight).
                double wall = 0, generate = 0, record = 0, verify = 0;
                uint64_t iterations = 0;
                for (const ComboRecord* c : combos)
                {
                    wall += c->result.wallSeconds;
                    generate += c->result.generateSeconds;
                    record += c->result.recordSeconds;
                    verify += c->result.verifySeconds;
                    iterations += c->result.iterationsRun;
                }
                if (iterations)
                {
                    const double k = 1000.0 / static_cast<double>(iterations);
                    out += Format("\n  %u sorts: wall %.1f s = %.2f ms per iteration (incl. warmup); CPU per iteration "
                                  "(multithreaded, overlaps the GPU): generate %.2f ms, record + upload copy %.2f ms, "
                                  "verify %.2f ms\n",
                                  numSorts, wall, wall * k, generate * k, record * k, verify * k);
                }
            }
        }
        if (!gpu.error.empty())
            out += Format("  ERROR: %s\n", gpu.error.c_str());
        // Fixed prefix "SMOKE FAILED:" (tools/run_all.bat copies these lines into summary.txt).
        for (const auto& sf : gpu.smokeFailed)
        {
            if (!sf.reason.empty())
                out += Format("  SMOKE FAILED: %s on %s (not run: %s)\n", sf.algorithm.c_str(), gpu.name.c_str(),
                              sf.reason.c_str());
            else
                out += Format("  SMOKE FAILED: %s on %s (first failing workload %s; its remaining workloads were "
                              "skipped)\n",
                              sf.algorithm.c_str(), gpu.name.c_str(), sf.workload.c_str());
        }
        for (const auto& c : gpu.combos)
        {
            for (const auto& m : c.result.failureMessages)
            {
                if (multi)
                    out += Format("  failure [%s / %s / %u sorts] %s\n", Workloads()[c.workloadId].name,
                                  c.algorithm.c_str(), c.sortsPerIteration, m.c_str());
                else
                    out += Format("  failure [%s / %s] %s\n", Workloads()[c.workloadId].name, c.algorithm.c_str(),
                                  m.c_str());
            }
        }
    }
    return out;
}
