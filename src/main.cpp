// GpuSort: DX12 benchmark / test framework for many small single-group sorts.
#include "Algorithms.h"
#include "Benchmark.h"
#include "Common.h"
#include "CsvOutput.h"
#include "Device.h"
#include "Options.h"
#include "ProgressWindow.h"
#include "Results.h"
#include "ShaderCompiler.h"
#include "WaveProbe.h"
#include "Workloads.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace
{
// Rough guess of the wall time per iteration for the prompt (before any GPU work; dominated by the
// 256 MB cache flush), and the fallback if the calibration fails. Measured full runs (1000
// iterations of 20 sorts): RTX 5080 0.73 ms, RX 7900 XTX 0.78 ms, Ryzen iGPU 9.7 ms, Intel UHD 770
// 7.6 ms. Serial smoke runs (a fence wait per iteration) take longer per iteration. More sorts per
// iteration add the poison / readback of the whole output (numSorts x 32 KB) and the upload: guessed
// +1.5 ms (discrete) / +3 ms (integrated) at 512 sorts, twice that in serial runs.
double GuessSecondsPerIteration(const GpuInfo& g, bool serial, uint32_t numSorts)
{
    double ms = g.uma ? (serial ? 70.0 : 10.0) : (serial ? 20.0 : 0.8);
    if (numSorts > kDefaultSortsPerIteration)
        ms += (g.uma ? 3.0 : 1.5) * (serial ? 2.0 : 1.0) * static_cast<double>(numSorts - kDefaultSortsPerIteration) /
              static_cast<double>(kMaxSortsPerIteration - kDefaultSortsPerIteration);
    return ms / 1000.0;
}

// "1000 iterations" or "1000/300/200/150 iterations at 20/128/256/512 sorts" (one value per sort count).
std::string FormatIterationList(const std::vector<uint32_t>& iterations, const std::vector<uint32_t>& sortCounts)
{
    if (sortCounts.size() == 1)
        return std::to_string(iterations[0]) + " iterations";
    std::string it, n;
    for (size_t ci = 0; ci < sortCounts.size(); ++ci)
    {
        it += (ci ? "/" : "") + std::to_string(iterations[ci]);
        n += (ci ? "/" : "") + std::to_string(sortCounts[ci]);
    }
    return it + " iterations at " + n + " sorts";
}

// The selected workload with the most elements per iteration (mean over a few 20-sort iterations):
// the second calibration workload next to the first one (the upload grows with the data).
uint32_t HeaviestWorkload(const std::vector<uint32_t>& workloadIds)
{
    uint32_t best = workloadIds[0];
    uint64_t bestTotal = 0;
    std::vector<uint32_t> sizes(kDefaultSortsPerIteration);
    for (uint32_t id : workloadIds)
    {
        uint64_t total = 0;
        for (uint32_t it = 0; it < 16; ++it)
        {
            GenerateSizes(id, it, kDefaultSortsPerIteration, sizes.data());
            for (uint32_t s : sizes)
                total += s;
        }
        if (total > bestTotal)
        {
            best = id;
            bestTotal = total;
        }
    }
    return best;
}

// Run-time estimate: iterations per GPU of the up-front calibration (GpuBenchmark::Calibrate).
constexpr uint32_t kCalibrationIterations = 32;
constexpr uint32_t kCalibrationIterationsSerial = 4; // --smoke
constexpr uint32_t kCalibrationIterationsWarp = 2;
// While a GPU runs, its measured rate replaces the calibration once this many iterations are done.
constexpr uint64_t kMeasuredRateMinIterations = 256;
// Spin drain calibration (only if an algorithm uses a spin drain): iterations of the flush work run
// right before it on the run's GpuBenchmark, so the clocks are where the run's flushes put them.
constexpr uint32_t kSpinWarmupIterations = 8;
constexpr uint32_t kSpinWarmupIterationsWarp = 1;

// Windows Developer Mode (HKLM\...\AppModelUnlock, AllowDevelopmentWithoutDevLicense = 1).
// ID3D12Device::SetStablePowerState removes the device if it is off.
bool DeveloperModeEnabled()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
                                    L"AllowDevelopmentWithoutDevLicense", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return st == ERROR_SUCCESS && value == 1;
}

std::string FormatDuration(double seconds)
{
    if (seconds < 90.0)
        return Format("%.0f s", seconds);
    if (seconds < 90.0 * 60.0)
        return Format("%.1f min", seconds / 60.0);
    return Format("%.1f h", seconds / 3600.0);
}

// --smoke: iterations per GPU x workload x algorithm (no warmup), one iteration in flight.
constexpr uint32_t kSmokeIterations = 3;

// A fence wait longer than this is treated as a hung GPU. Hardware batches take well under 0.1 s
// (and Windows TDR fires after 2 s); WARP with GPU-based validation can be much slower.
constexpr uint32_t kFenceTimeoutMsHardware = 10000;
constexpr uint32_t kFenceTimeoutMsWarp = 300000;

fs::path ExeDir()
{
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf).parent_path();
}

fs::path FindShaderDir()
{
    fs::path p = ExeDir();
    for (int i = 0; i < 6; ++i)
    {
        if (fs::exists(p / "shaders" / "algorithms.txt"))
            return p / "shaders";
        if (p == p.parent_path())
            break;
        p = p.parent_path();
    }
    const fs::path cwd = fs::current_path() / "shaders";
    if (fs::exists(cwd / "algorithms.txt"))
        return cwd;
    throw std::runtime_error("could not find a shaders/ directory with algorithms.txt; use --shaders <dir>");
}

// Start time of the run: the results header date, the ISO 8601 timestamp and run id of the CSVs.
struct RunTime
{
    std::string date;    // 2026-09-29 14:30:12 (local)
    std::string iso;     // 2026-09-29T14:30:12.345+02:00
    std::string compact; // 20260929T143012.345
};

RunTime Now()
{
    const auto now = std::chrono::system_clock::now();
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::tm local = tm;
    const long long offsetMinutes = static_cast<long long>(_mkgmtime(&local) - t) / 60; // local - UTC
    const long long absOffset = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
    char buf[64];
    RunTime r;
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    r.date = buf;
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    r.iso = Format("%s.%03lld%c%02lld:%02lld", buf, ms, offsetMinutes < 0 ? '-' : '+', absOffset / 60,
                   absOffset % 60);
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", &tm);
    r.compact = Format("%s.%03lld", buf, ms);
    return r;
}

// Default CSV path for a results / report file: the same path with the extension .csv (or
// <stem>_<suffix>.csv if it already is a .csv file).
fs::path CsvPathFor(const fs::path& out, const wchar_t* suffix)
{
    fs::path p = out;
    if (_wcsicmp(out.extension().c_str(), L".csv") == 0)
        p.replace_filename(out.stem().wstring() + L"_" + suffix + L".csv");
    else
        p.replace_extension(L".csv");
    return p;
}

// <csv stem><suffix>.csv next to 'csv', e.g. results_samples.csv.
fs::path CsvSibling(const fs::path& csv, const wchar_t* suffix)
{
    fs::path p = csv;
    p.replace_filename(csv.stem().wstring() + suffix + L".csv");
    return p;
}

// Name of 'file' for the results header: just the file name if it is in the folder of 'report',
// else the full path.
std::string CsvDisplayName(const fs::path& file, const fs::path& report)
{
    const fs::path abs = fs::absolute(file);
    return WideToUtf8(abs.parent_path() == fs::absolute(report).parent_path() ? abs.filename().wstring()
                                                                                : abs.wstring());
}

// Writes one CSV and logs where (or the error).
void WriteCsv(bool (*write)(const RunInfo&, const fs::path&, std::string&), const RunInfo& info, const fs::path& path)
{
    std::string error;
    if (write(info, path, error))
        Log("CSV written to %s\n", WideToUtf8(fs::absolute(path).wstring()).c_str());
    else
        Log("error: %s\n", error.c_str());
}

std::string CommandLineUtf8()
{
    return WideToUtf8(GetCommandLineW());
}

std::string ComputerName()
{
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(buf, &n) ? WideToUtf8(buf) : "unknown";
}

int RunMain(int argc, wchar_t** argv)
{
    Options opt;
    std::string error;
    if (!ParseOptions(argc, argv, opt, error))
    {
        LogError("error: %s\n\n", error.c_str());
        PrintUsage();
        return 1;
    }
    if (!opt.logPath.empty() && !OpenLogFile(opt.logPath))
    {
        LogError("error: cannot open log file %s\n", WideToUtf8(opt.logPath).c_str());
        return 1;
    }
    if (!opt.label.empty())
        Log("Run: %s\n", opt.label.c_str());
    if (opt.help)
    {
        PrintUsage();
        return 0;
    }
    if (opt.listAdapters)
    {
        PrintAdapterList();
        return 0;
    }
    if (opt.smoke)
    {
        // An explicit --iterations still applies (e.g. for a few samples per sweep size); the smoke
        // run stays serial (one iteration in flight) and has no warmup.
        if (!opt.iterationsGiven)
            opt.iterations = {kSmokeIterations};
        opt.warmup = 0;
    }

    // --- shaders / algorithms -------------------------------------------------------------
    const fs::path shaderDir = opt.shaderDir.empty() ? FindShaderDir() : fs::absolute(opt.shaderDir);
    const fs::path algoFile = opt.algoFile.empty() ? fs::path(L"algorithms.txt") : fs::path(opt.algoFile);
    std::vector<AlgorithmDesc> allAlgorithms = LoadAlgorithms(shaderDir, algoFile);
    std::vector<AlgorithmDesc> algorithms;
    if (opt.algorithms.empty())
        algorithms = allAlgorithms;
    else
    {
        for (const auto& name : opt.algorithms)
        {
            auto it = std::find_if(allAlgorithms.begin(), allAlgorithms.end(),
                                   [&](const AlgorithmDesc& a) { return a.name == name; });
            if (it == allAlgorithms.end())
            {
                std::string list;
                for (const auto& a : allAlgorithms)
                    list += " " + a.name;
                throw std::runtime_error("unknown algorithm '" + name + "' (available:" + list + ")");
            }
            algorithms.push_back(*it);
        }
    }
    for (auto& a : algorithms)
    {
        if (!a.flushGiven)
            a.flush = opt.flushMode;
    }

    std::vector<uint32_t> workloadIds;
    if (opt.workloads.empty())
    {
        for (uint32_t i = 0; i < Workloads().size(); ++i)
            workloadIds.push_back(i);
    }
    else
    {
        for (const auto& name : opt.workloads)
        {
            const int id = FindWorkload(name);
            if (id < 0)
            {
                std::string list;
                for (const auto& w : Workloads())
                    list += std::string(" ") + w.name;
                throw std::runtime_error("unknown workload '" + name + "' (available:" + list + ")");
            }
            workloadIds.push_back(static_cast<uint32_t>(id));
        }
    }

    Log("Shader dir: %s\n", shaderDir.string().c_str());
    if (!opt.algoFile.empty())
        Log("Algorithm list: %s\n", algoFile.string().c_str());
    ShaderCompiler compiler;
    std::string log;
    ComPtr<IDxcBlob> flushShader = compiler.CompileSource(kFlushShaderSource, L"flush.hlsl", "main", {}, log);
    if (!flushShader)
        throw std::runtime_error("flush shader failed to compile:\n" + log);
    ComPtr<IDxcBlob> flushReadOnlyShader =
        compiler.CompileSource(kFlushShaderSource, L"flush.hlsl", "main_ro", {}, log);
    if (!flushReadOnlyShader)
        throw std::runtime_error("read-only flush shader failed to compile:\n" + log);
    ComPtr<IDxcBlob> spinShader = compiler.CompileSource(kFlushShaderSource, L"flush.hlsl", "spin", {}, log);
    if (!spinShader)
        throw std::runtime_error("drain spin shader failed to compile:\n" + log);

    // --- adapters -------------------------------------------------------------------------
    // Both must happen before any device is created.
    if (opt.debugLayer && !EnableD3D12DebugLayer(opt.gpuValidation))
        Log("warning: D3D12 debug layer%s not available\n", opt.gpuValidation ? " / GPU-based validation" : "");
    else if (opt.debugLayer)
        Log("D3D12 debug layer enabled%s\n", opt.gpuValidation ? " with GPU-based validation" : "");
    if (opt.dred)
    {
        if (EnableDred())
            Log("DRED enabled (auto-breadcrumbs, breadcrumb contexts, page-fault reporting)\n");
        else
            Log("warning: DRED not available\n");
    }

    RunInfo info;
    const RunTime runTime = Now();
    info.date = runTime.date;
    info.runTimestamp = runTime.iso;
    info.computerName = ComputerName();
    info.runId = runTime.compact + "_" + info.computerName;
    info.runKind = opt.waveProbe ? "wave_probe" : opt.smoke ? "smoke" : "benchmark";
    info.packageCommit = PackageCommit();
    info.shaderSet = WideToUtf8(
        (shaderDir.has_filename() ? shaderDir.filename() : shaderDir.parent_path().filename()).wstring());
    info.label = opt.label;
    info.shaderDir = shaderDir.string();
    info.algoFile = algoFile.string();
    info.commandLine = CommandLineUtf8();
    info.iterations = opt.iterations[0];
    info.iterationsIntegrated = opt.iterationsIntegrated.empty() ? 0 : opt.iterationsIntegrated[0];
    info.sortCounts = opt.sortCounts;
    for (size_t ci = 0; ci < opt.sortCounts.size(); ++ci)
    {
        info.iterationsPerCount.push_back(IterationsFor(opt, ci, false));
        if (!opt.iterationsIntegrated.empty())
            info.iterationsIntegratedPerCount.push_back(IterationsFor(opt, ci, true));
    }
    const std::vector<uint32_t>& sortCounts = opt.sortCounts;
    const bool multiCount = sortCounts.size() > 1;
    info.warmup = opt.warmup;
    info.workloadIds = workloadIds;
    info.defaultFlush = opt.flushMode;
    for (const auto& a : algorithms)
        info.algorithms.push_back(a.name);

    std::vector<GpuInfo> gpus = EnumerateGpus(opt.warp, opt.gpuFilter, info.skippedAdapters);
    if (opt.integratedOnly && !opt.warp)
    {
        // --integrated-only: e.g. the Ryzen iGPU next to a discrete GPU (run_all.bat pass7, wave64).
        const size_t qualifying = gpus.size();
        std::erase_if(gpus, [&](const GpuInfo& g) {
            if (g.uma)
                return false;
            info.skippedAdapters.push_back(g.name + ": not an integrated (UMA) GPU (--integrated-only)");
            return true;
        });
        if (gpus.empty() && qualifying > 0)
        {
            for (const auto& s : info.skippedAdapters)
                Log("Skipping adapter %s\n", s.c_str());
            throw std::runtime_error("no integrated GPU found (--integrated-only)");
        }
    }
    if (opt.discreteOnly && !opt.warp)
    {
        // --discrete-only: e.g. the RX 7900 XTX at wave64 without the Ryzen iGPU (run_all.bat final).
        const size_t qualifying = gpus.size();
        std::erase_if(gpus, [&](const GpuInfo& g) {
            if (!g.uma)
                return false;
            info.skippedAdapters.push_back(g.name + ": an integrated (UMA) GPU (--discrete-only)");
            return true;
        });
        if (gpus.empty() && qualifying > 0)
        {
            for (const auto& s : info.skippedAdapters)
                Log("Skipping adapter %s\n", s.c_str());
            throw std::runtime_error("no discrete GPU found (--discrete-only)");
        }
    }
    if (opt.waveSize)
    {
        // --wave-size N only runs on the GPUs that support it (e.g. wave64 on AMD next to an NVIDIA GPU).
        const size_t qualifying = gpus.size();
        std::erase_if(gpus, [&](const GpuInfo& g) {
            if (opt.waveSize >= g.waveLaneCountMin && opt.waveSize <= g.waveLaneCountMax)
                return false;
            info.skippedAdapters.push_back(Format("%s: --wave-size %u is outside its wave lane range %u-%u",
                                                  g.name.c_str(), opt.waveSize, g.waveLaneCountMin,
                                                  g.waveLaneCountMax));
            return true;
        });
        if (gpus.empty() && qualifying > 0)
        {
            for (const auto& s : info.skippedAdapters)
                Log("Skipping adapter %s\n", s.c_str());
            throw std::runtime_error(Format("no GPU supports --wave-size %u", opt.waveSize));
        }
    }
    for (const auto& s : info.skippedAdapters)
        Log("Skipping adapter %s\n", s.c_str());
    if (gpus.empty())
        throw std::runtime_error(opt.warp ? "WARP adapter does not qualify (needs SM 6.6)"
                                          : "no qualifying hardware GPU found (needs D3D12 + SM 6.6)");
    // Measured iterations per GPU x sort count: --iterations-integrated on integrated (UMA) GPUs, if given.
    std::vector<std::vector<uint32_t>> gpuIterations(gpus.size());
    bool iterationsDiffer = false;
    for (size_t gi = 0; gi < gpus.size(); ++gi)
    {
        for (size_t ci = 0; ci < sortCounts.size(); ++ci)
        {
            gpuIterations[gi].push_back(IterationsFor(opt, ci, gpus[gi].uma));
            iterationsDiffer |= gpuIterations[gi][ci] != IterationsFor(opt, ci, false);
        }
    }
    for (size_t gi = 0; gi < gpus.size(); ++gi)
    {
        const GpuInfo& g = gpus[gi];
        Log("Using adapter: %s (vendor %04X device %04X, driver %s, %llu MB, wave lanes %u-%u, %s, %s)\n",
            g.name.c_str(), g.vendorId, g.deviceId, g.driver.c_str(),
            static_cast<unsigned long long>(g.dedicatedVideoMemory >> 20), g.waveLaneCountMin, g.waveLaneCountMax,
            g.uma ? "integrated" : "discrete", FormatIterationList(gpuIterations[gi], sortCounts).c_str());
    }

    // --- shader compilation, per wave-size configuration ----------------------------------
    // Every shader gets -D WAVE_SIZE=<n> (the device's WaveLaneCountMin, or --wave-size). If the
    // device reports a range (Min != Max) or --wave-size is given, it also gets
    // -D WAVE_SIZE_REQUIRED=1 and must put [WaveSize(WAVE_SIZE)] on entry points that depend on it
    // (common.hlsli: WAVE_SIZE_ATTR). GPUs with the same configuration share the compiled blobs.
    struct WaveConfig
    {
        uint32_t size = 0;
        bool attribute = false;
    };
    std::vector<WaveConfig> gpuWave(gpus.size());
    for (size_t gi = 0; gi < gpus.size(); ++gi)
    {
        const GpuInfo& g = gpus[gi];
        WaveConfig& wc = gpuWave[gi];
        wc.size = opt.waveSize ? opt.waveSize : g.waveLaneCountMin;
        wc.attribute = opt.waveSize != 0 || g.waveLaneCountMin != g.waveLaneCountMax;
        if (wc.size < g.waveLaneCountMin || wc.size > g.waveLaneCountMax)
            throw std::runtime_error(Format("--wave-size %u is outside the wave lane range %u-%u of %s", wc.size,
                                            g.waveLaneCountMin, g.waveLaneCountMax, g.name.c_str()));
    }
    std::deque<std::pair<std::pair<uint32_t, bool>, std::vector<CompiledAlgorithm>>> compiledByWave; // stable references
    const std::vector<CompiledAlgorithm> noAlgorithms;
    auto compileFor = [&](const WaveConfig& wc) -> const std::vector<CompiledAlgorithm>& {
        if (opt.waveProbe)
            return noAlgorithms; // --wave-probe runs no sort
        for (const auto& entry : compiledByWave)
        {
            if (entry.first == std::make_pair(wc.size, wc.attribute))
                return entry.second;
        }
        Log("Compiling shaders for WAVE_SIZE=%u%s\n", wc.size, wc.attribute ? " with [WaveSize]" : "");
        std::vector<CompiledAlgorithm> compiled;
        for (const auto& algorithm : algorithms)
        {
            CompiledAlgorithm ca;
            ca.name = algorithm.name;
            ca.flush = algorithm.flush;
            std::vector<DispatchStats> stats;
            for (const auto& d : algorithm.dispatches)
            {
                ShaderDefines defines = d.defines;
                defines.emplace_back("GROUP_SIZE", std::to_string(d.groupSize));
                defines.emplace_back("WAVE_SIZE", std::to_string(wc.size));
                if (wc.attribute)
                    defines.emplace_back("WAVE_SIZE_REQUIRED", "1");
                ComPtr<IDxcBlob> blob = compiler.CompileFile(shaderDir / d.file, d.entry, defines, log);
                if (!log.empty())
                    Log("%s [%s:%s]:\n%s\n", blob ? "Shader warnings" : "Shader errors", d.file.c_str(),
                        d.entry.c_str(), log.c_str());
                if (!blob)
                    throw std::runtime_error("failed to compile " + d.file + " for algorithm " + algorithm.name);
                ca.usesWaveOps |= compiler.UsesWaveOps(blob.Get());
                ca.shaders.push_back(blob);
                stats.push_back(compiler.GetDispatchStats(blob.Get()));
            }
            ca.dispatchInfo = FormatDispatchStats(stats);
            std::string sizes;
            for (const auto& s : ca.shaders)
                sizes += Format("%s%zu", sizes.empty() ? "" : " + ", static_cast<size_t>(s->GetBufferSize()));
            Log("Compiled algorithm %s (%zu dispatch%s, DXIL %s bytes, threads/groupshared %s, flush mode %s%s)\n",
                ca.name.c_str(), ca.shaders.size(), ca.shaders.size() == 1 ? "" : "es", sizes.c_str(),
                ca.dispatchInfo.c_str(), FlushModeName(ca.flush).c_str(), ca.usesWaveOps ? ", wave ops" : ", no wave ops");
            compiled.push_back(std::move(ca));
        }
        compiledByWave.push_back({{wc.size, wc.attribute}, std::move(compiled)});
        return compiledByWave.back().second;
    };
    // Compile everything up front, so shader errors show up before the prompt.
    for (const auto& wc : gpuWave)
        compileFor(wc);
    if (!compiledByWave.empty())
    {
        for (size_t i = 0; i < compiledByWave.front().second.size(); ++i)
        {
            const CompiledAlgorithm& ca = compiledByWave.front().second[i];
            AlgorithmInfo ai;
            ai.name = ca.name;
            ai.flush = ca.flush;
            ai.pass = algorithms[i].pass;
            ai.description = algorithms[i].description;
            ai.tags = algorithms[i].tags;
            for (const auto& s : ca.shaders)
                ai.dxilBytes.push_back(static_cast<size_t>(s->GetBufferSize()));
            ai.dispatchInfo = ca.dispatchInfo;
            info.algorithmInfos.push_back(std::move(ai));
        }
        info.dxilWaveSize = compiledByWave.front().first.first;
    }

    // Wave probe (WaveProbe.h), per wave configuration: group sizes 64 / 512 / 1024 plus every group
    // size the selected algorithms use.
    std::vector<uint32_t> probeGroupSizes = {64, 512, 1024};
    for (const auto& a : algorithms)
    {
        for (const auto& d : a.dispatches)
            probeGroupSizes.push_back(d.groupSize);
    }
    std::sort(probeGroupSizes.begin(), probeGroupSizes.end());
    probeGroupSizes.erase(std::unique(probeGroupSizes.begin(), probeGroupSizes.end()), probeGroupSizes.end());
    // --wave-probe without --wave-size: the survey (without [WaveSize] and every [WaveSize(N)] in the
    // device's lane range) instead of the sort shaders' configuration.
    const bool probeSurvey = opt.waveProbe && !opt.waveSize;
    std::deque<WaveProbeSet> probeSets; // stable references
    auto probeFor = [&](const WaveConfig& wc, const GpuInfo& g) -> const WaveProbeSet& {
        for (const auto& s : probeSets)
        {
            if (s.waveSize == wc.size && s.attribute == wc.attribute &&
                (!probeSurvey || (s.laneMin == g.waveLaneCountMin && s.laneMax == g.waveLaneCountMax)))
                return s;
        }
        probeSets.push_back(probeSurvey ? CompileWaveProbeSurvey(compiler, g.waveLaneCountMin, g.waveLaneCountMax,
                                                                 wc.size, wc.attribute, probeGroupSizes)
                                        : CompileWaveProbe(compiler, wc.size, wc.attribute, probeGroupSizes));
        return probeSets.back();
    };
    std::vector<const WaveProbeSet*> gpuProbe(gpus.size());
    for (size_t gi = 0; gi < gpus.size(); ++gi)
        gpuProbe[gi] = &probeFor(gpuWave[gi], gpus[gi]);

    if (!opt.waveProbe)
    {
        for (size_t ci = 0; ci < sortCounts.size(); ++ci)
            Log("\n%s", FormatSizeDistribution(workloadIds, IterationsFor(opt, ci, false), sortCounts[ci]).c_str());
        Log("\n");
    }

    // Iterations per GPU x sort count (every GPU runs every workload x algorithm at every count).
    std::vector<std::vector<uint64_t>> gpuCountIterations(gpus.size());
    std::vector<uint64_t> gpuTotalIterations(gpus.size());
    double estimatedSeconds = 0.0; // rough guess for the prompt; calibrated after the prompt
    for (size_t gi = 0; gi < gpus.size(); ++gi)
    {
        for (size_t ci = 0; ci < sortCounts.size(); ++ci)
        {
            const uint64_t n = uint64_t(gpuIterations[gi][ci] + opt.warmup) * workloadIds.size() * algorithms.size();
            gpuCountIterations[gi].push_back(n);
            gpuTotalIterations[gi] += n;
            estimatedSeconds +=
                static_cast<double>(n) * GuessSecondsPerIteration(gpus[gi], opt.smoke, sortCounts[ci]);
        }
    }
    if (multiCount && !opt.waveProbe)
    {
        Log("Plan: every GPU x workload x algorithm at %zu sort counts (sorts per iteration), in this order:\n",
            sortCounts.size());
        for (size_t gi = 0; gi < gpus.size(); ++gi)
        {
            for (size_t ci = 0; ci < sortCounts.size(); ++ci)
            {
                const double guess = GuessSecondsPerIteration(gpus[gi], opt.smoke, sortCounts[ci]);
                Log("  [%zu] %-32s %3u sorts: %5u iterations (+%u warmup) x %zu workloads x %zu algorithms = %llu "
                    "iterations, ~%s (rough guess, %.2f ms each)\n",
                    gi, gpus[gi].name.c_str(), sortCounts[ci], gpuIterations[gi][ci], opt.warmup, workloadIds.size(),
                    algorithms.size(), static_cast<unsigned long long>(gpuCountIterations[gi][ci]),
                    FormatDuration(static_cast<double>(gpuCountIterations[gi][ci]) * guess).c_str(), guess * 1000.0);
            }
        }
    }

    // --stable-power: only with Developer Mode (checked here, applied after the prompt).
    const bool stablePower = opt.stablePower && !opt.waveProbe;
    const bool developerMode = stablePower && DeveloperModeEnabled();
    if (opt.stablePower && opt.waveProbe)
        Log("--stable-power is ignored with --wave-probe\n");
    else if (stablePower && !developerMode)
        Log("\n*** --stable-power: Windows Developer Mode is OFF, so SetStablePowerState is NOT called (it would\n"
            "*** remove the device). The run continues with normal clocks (stable_power = unavailable).\n\n");

    // --- prompt ---------------------------------------------------------------------------
    if (!opt.warp && !opt.noPrompt)
    {
        std::wstring text;
        if (!opt.label.empty())
            text = L"Run " + Utf8ToWide(opt.label) + L"\n\n";
        if (opt.waveProbe)
        {
            text += L"WAVE PROBE: GpuSort is about to run only its wave probe (a few one-group dispatches of\n"
                    L"a tiny diagnostic shader, well under a second, no sorts) on:\n\n";
            for (size_t gi = 0; gi < gpus.size(); ++gi)
                text += L"    " + Utf8ToWide(gpus[gi].name) + L"\n        " +
                        Utf8ToWide(DescribeWaveProbe(*gpuProbe[gi])) + L"\n";
            text += L"\n";
        }
        else
        {
            if (opt.smoke)
                text += L"SMOKE TEST: a short safety check before any full benchmark run (a GPU fault, TDR or\n"
                        L"bugcheck in new shader code is much cheaper to hit here).\n\n"
                        L"Every algorithm x workload runs a few iterations, one at a time with a fence wait\n"
                        L"after each, and stops at the first GPU fault or hang.\n\n";
            text += L"GpuSort is about to run a GPU benchmark on:\n\n";
            for (const auto& g : gpus)
                text += L"    " + Utf8ToWide(g.name) + L"\n";
            if (multiCount)
            {
                std::vector<uint32_t> discrete, integrated;
                for (size_t ci = 0; ci < sortCounts.size(); ++ci)
                {
                    discrete.push_back(IterationsFor(opt, ci, false));
                    integrated.push_back(IterationsFor(opt, ci, true));
                }
                const std::string integratedIterations =
                    iterationsDiffer ? " (integrated GPUs: " + FormatIterationList(integrated, sortCounts) + ")"
                                     : std::string();
                text += Utf8ToWide(Format("\n%zu workload(s) x %zu algorithm(s) per GPU at %zu sort counts (sorts per\n"
                                          "iteration), %s%s (+%u warmup).\n"
                                          "Estimated duration: roughly %s (a rough guess; a calibrated estimate is\n"
                                          "shown in the progress window and the console right after the start).\n\n",
                                          workloadIds.size(), algorithms.size(), sortCounts.size(),
                                          FormatIterationList(discrete, sortCounts).c_str(),
                                          integratedIterations.c_str(), opt.warmup,
                                          FormatDuration(estimatedSeconds < 1.0 ? 1.0 : estimatedSeconds).c_str()));
            }
            else
            {
                const std::string integratedIterations =
                    iterationsDiffer ? Format(" (integrated GPUs: %u)", IterationsFor(opt, 0, true)) : std::string();
                text += Utf8ToWide(Format("\n%zu workload(s) x %zu algorithm(s) x %u iterations%s (+%u warmup) per GPU.\n"
                                          "Estimated duration: roughly %s (a rough guess; a calibrated estimate is\n"
                                          "shown in the progress window and the console right after the start).\n\n",
                                          workloadIds.size(), algorithms.size(), IterationsFor(opt, 0, false),
                                          integratedIterations.c_str(), opt.warmup,
                                          FormatDuration(estimatedSeconds < 1.0 ? 1.0 : estimatedSeconds).c_str()));
                if (sortCounts[0] != kDefaultSortsPerIteration)
                    text += Utf8ToWide(Format("%u sorts per iteration.\n\n", sortCounts[0]));
            }
            if (stablePower)
                text += developerMode ? L"Stable power: every GPU runs with SetStablePowerState(TRUE) (fixed clocks).\n\n"
                                      : L"Stable power was requested, but Windows Developer Mode is off: running\n"
                                        L"with normal clocks.\n\n";
        }
        text += L"Please pause other GPU work now, then press OK to start.\nCancel exits without running.";
        Log("Waiting for confirmation (message box)...\n");
        std::wstring caption = opt.waveProbe ? L"GpuSort wave probe"
                               : opt.smoke   ? L"GpuSort smoke test"
                                             : L"GpuSort benchmark";
        if (!opt.label.empty())
            caption += L" - " + Utf8ToWide(opt.label);
        const int answer = MessageBoxW(nullptr, text.c_str(), caption.c_str(),
                                       MB_OKCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        if (answer != IDOK)
        {
            Log("Cancelled by user.\n");
            return 2;
        }
    }

    // --- run ------------------------------------------------------------------------------
    const auto runStart = std::chrono::steady_clock::now(); // after the prompt: excludes user wait time
    ProgressWindow window;
    if (!opt.warp)
        window.Start(L"GpuSort benchmark running - please keep the GPUs idle" +
                     (opt.label.empty() ? std::wstring() : L" - " + Utf8ToWide(opt.label)));

    BenchmarkOptions benchOptions;
    benchOptions.fenceTimeoutMs = opt.warp ? kFenceTimeoutMsWarp : kFenceTimeoutMsHardware;
    benchOptions.serial = opt.smoke;
    benchOptions.markers = opt.dred;
    benchOptions.logAddresses = opt.dred;
    benchOptions.testRemoveDevice = opt.testRemove;

    // --- --stable-power ---------------------------------------------------------------------
    std::vector<std::string> gpuStablePower(gpus.size(), "off");
    std::vector<std::string> gpuStablePowerNote(gpus.size());
    if (stablePower)
    {
        for (size_t gi = 0; gi < gpus.size(); ++gi)
        {
            if (!developerMode)
            {
                gpuStablePower[gi] = "unavailable";
                gpuStablePowerNote[gi] = "requested, but Windows Developer Mode is off: SetStablePowerState was not "
                                         "called (it would remove the device); normal clocks";
                continue;
            }
            const HRESULT hr = gpus[gi].device->SetStablePowerState(TRUE);
            if (SUCCEEDED(hr))
            {
                gpuStablePower[gi] = "on";
                gpuStablePowerNote[gi] = "SetStablePowerState(TRUE) (Developer Mode is on)";
            }
            else
            {
                gpuStablePower[gi] = "failed";
                gpuStablePowerNote[gi] =
                    Format("SetStablePowerState(TRUE) returned 0x%08X; normal clocks", static_cast<unsigned>(hr));
            }
            Log("Stable power on %s: %s (%s)\n", gpus[gi].name.c_str(), gpuStablePower[gi].c_str(),
                gpuStablePowerNote[gi].c_str());
        }
    }

    auto newRecord = [&](size_t gi) {
        const GpuInfo& gpu = gpus[gi];
        GpuRecord record;
        record.name = gpu.name;
        record.driver = gpu.driver;
        record.vendorId = gpu.vendorId;
        record.deviceId = gpu.deviceId;
        record.dedicatedVideoMemory = gpu.dedicatedVideoMemory;
        record.uma = gpu.uma;
        record.waveLaneCountMin = gpu.waveLaneCountMin;
        record.waveLaneCountMax = gpu.waveLaneCountMax;
        record.waveSize = gpuWave[gi].size;
        record.waveSizeAttribute = gpuWave[gi].attribute;
        record.iterations = gpuIterations[gi][0];
        record.iterationsPerCount = gpuIterations[gi];
        record.stablePower = gpuStablePower[gi];
        record.stablePowerNote = gpuStablePowerNote[gi];
        return record;
    };

    uint32_t totalFailures = 0;
    bool deviceLost = false;

    // --- run-time estimate: calibrate every GPU up front ----------------------------------
    // A short batch of the per-iteration work without a sort (upload, 256 MB flush, drain,
    // readback) on each GPU, before any sort shader runs. That fixed cost dominates an iteration on
    // every GPU measured so far, so it predicts the run time well; while a GPU runs, its measured
    // rate takes over (the progress window and the console show the refined estimate).
    // Per GPU x sort count; with two calibration workloads (the first and the heaviest selected one:
    // the upload grows with the data) the mean of both.
    std::vector<std::vector<double>> gpuSecondsPerIteration(gpus.size());
    std::vector<std::vector<double>> gpuCountEstimateSeconds(gpus.size());
    std::vector<double> gpuEstimateSeconds(gpus.size());
    std::vector<bool> gpuCalibrated(gpus.size(), false);
    for (size_t gi = 0; gi < gpus.size(); ++gi)
    {
        gpuSecondsPerIteration[gi].assign(sortCounts.size(), 0.0);
        gpuCountEstimateSeconds[gi].assign(sortCounts.size(), 0.0);
    }
    const uint32_t calibrationIterations =
        opt.warp ? kCalibrationIterationsWarp : opt.smoke ? kCalibrationIterationsSerial : kCalibrationIterations;
    if (!opt.waveProbe)
    {
        const uint32_t heavyWorkload = HeaviestWorkload(workloadIds);
        std::vector<uint32_t> calibrationWorkloads = {workloadIds[0]};
        if (heavyWorkload != workloadIds[0])
            calibrationWorkloads.push_back(heavyWorkload);
        info.calibrationIterations = calibrationIterations;
        window.SetText(L"Calibrating the run-time estimate (a few flush passes per GPU) ...");
        std::string calibrationNames;
        for (uint32_t w : calibrationWorkloads)
            calibrationNames += std::string(calibrationNames.empty() ? "" : " and ") + Workloads()[w].name;
        Log("\nRun-time estimate (calibrated per GPU%s: %u iterations of the upload + flush + drain + readback work,\n"
            "without a sort, with the data of %s%s; refined with the measured rate while the run progresses):\n",
            multiCount ? " x sort count" : "", calibrationIterations, calibrationNames.c_str(),
            calibrationWorkloads.size() > 1 ? " (mean)" : "");
        for (size_t gi = 0; gi < gpus.size() && !deviceLost; ++gi)
        {
            const GpuInfo& gpu = gpus[gi];
            gpuSecondsPerIteration[gi].clear();
            for (uint32_t n : sortCounts)
                gpuSecondsPerIteration[gi].push_back(GuessSecondsPerIteration(gpu, opt.smoke, n));
            std::unique_ptr<GpuBenchmark> calibration;
            try
            {
                calibration =
                    std::make_unique<GpuBenchmark>(gpu.device.Get(), flushShader.Get(), flushReadOnlyShader.Get(),
                                                   spinShader.Get(), noAlgorithms, sortCounts, benchOptions);
                for (size_t ci = 0; ci < sortCounts.size(); ++ci)
                {
                    calibration->SetSortCount(sortCounts[ci]);
                    double sum = 0.0;
                    for (uint32_t w : calibrationWorkloads)
                        sum += calibration->Calibrate(w, calibrationIterations);
                    gpuSecondsPerIteration[gi][ci] = sum / static_cast<double>(calibrationWorkloads.size());
                }
                gpuCalibrated[gi] = true;
                calibration.reset();
            }
            catch (const std::exception& e)
            {
                if (dynamic_cast<const DeviceLostError*>(&e) != nullptr || FAILED(gpu.device->GetDeviceRemovedReason()))
                {
                    // As in the run below: never release resources the GPU may still use; stop at once.
                    (void)calibration.release();
                    deviceLost = true;
                    GpuRecord record = newRecord(gi);
                    record.error =
                        std::string("DEVICE LOST (during the run-time calibration, before any sort): ") + e.what();
                    Log("  ERROR on %s: %s\n", gpu.name.c_str(), record.error.c_str());
                    Log("\n%s", FormatDredReport(gpu.device.Get()).c_str());
                    Log("\nStopping: nothing more is submitted to any GPU. Writing partial results.\n");
                    info.gpus.push_back(std::move(record));
                    break;
                }
                calibration.reset();
                Log("  [%zu] %s: calibration failed (%s); using a rough guess\n", gi, gpu.name.c_str(), e.what());
            }
            for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                Log("    %s\n", m.c_str());
            gpuEstimateSeconds[gi] = 0.0;
            gpuCountEstimateSeconds[gi].clear();
            for (size_t ci = 0; ci < sortCounts.size(); ++ci)
            {
                const double e = static_cast<double>(gpuCountIterations[gi][ci]) * gpuSecondsPerIteration[gi][ci];
                gpuCountEstimateSeconds[gi].push_back(e);
                gpuEstimateSeconds[gi] += e;
                if (multiCount)
                    Log("  [%zu] %-32s %3u sorts: %6.2f ms per iteration%s x %llu iterations = ~%s\n", gi,
                        gpu.name.c_str(), sortCounts[ci], gpuSecondsPerIteration[gi][ci] * 1000.0,
                        gpuCalibrated[gi] ? "" : " (guess)",
                        static_cast<unsigned long long>(gpuCountIterations[gi][ci]), FormatDuration(e).c_str());
            }
            if (multiCount)
                Log("  [%zu] %-32s all sort counts: ~%s\n", gi, gpu.name.c_str(),
                    FormatDuration(gpuEstimateSeconds[gi]).c_str());
            else
                Log("  [%zu] %-32s %6.2f ms per iteration%s x %llu iterations = ~%s\n", gi, gpu.name.c_str(),
                    gpuSecondsPerIteration[gi][0] * 1000.0, gpuCalibrated[gi] ? "" : " (guess)",
                    static_cast<unsigned long long>(gpuTotalIterations[gi]),
                    FormatDuration(gpuEstimateSeconds[gi]).c_str());
        }
        double total = 0.0;
        for (double e : gpuEstimateSeconds)
            total += e;
        info.estimatedSeconds = total;
        if (!deviceLost)
            Log("  Estimated total run time: ~%s\n", FormatDuration(total).c_str());
    }

    for (size_t gi = 0; gi < gpus.size() && !deviceLost; ++gi)
    {
        const GpuInfo& gpu = gpus[gi];
        GpuRecord record = newRecord(gi);
        record.calibrated = gpuCalibrated[gi];
        if (!gpuSecondsPerIteration[gi].empty())
            record.secondsPerIterationEstimate = gpuSecondsPerIteration[gi][0];
        record.secondsPerIterationPerCount = gpuSecondsPerIteration[gi];
        record.estimatedSeconds = gpuEstimateSeconds[gi];
        const std::vector<CompiledAlgorithm>& compiled = compileFor(gpuWave[gi]);
        for (const auto& ca : compiled)
            record.dispatchInfo[ca.name] = ca.dispatchInfo;
        const auto gpuStart = std::chrono::steady_clock::now();
        Log("\n=== GPU %zu/%zu: %s (WAVE_SIZE %u%s, %s + %u warmup) ===\n", gi + 1, gpus.size(),
            gpu.name.c_str(), gpuWave[gi].size, gpuWave[gi].attribute ? " + [WaveSize]" : "",
            FormatIterationList(gpuIterations[gi], sortCounts).c_str(), opt.warmup);
        std::unique_ptr<GpuBenchmark> benchPtr;
        try
        {
            benchPtr = std::make_unique<GpuBenchmark>(gpu.device.Get(), flushShader.Get(), flushReadOnlyShader.Get(),
                                                      spinShader.Get(), compiled, sortCounts, benchOptions);
            GpuBenchmark& bench = *benchPtr;
            record.timestampFrequency = bench.TimestampFrequency();
            for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                Log("    %s\n", m.c_str());

            // Wave probe: what wave does the driver really run (before any sort).
            const WaveProbeReport probe = RunWaveProbe(bench, *gpuProbe[gi]);
            record.waveProbe = probe.lines;
            record.waveProbeSummary = probe.summary;
            record.waveProbeWarning = !probe.configOk;
            record.waveProbeRows = probe.rows;
            for (const auto& line : probe.lines)
                Log("  %s\n", line.c_str());
            for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                Log("    %s\n", m.c_str());
            if (!probe.configOk && probeSurvey)
            {
                Log("\n  ********************************************************************************\n"
                    "  WARNING: WAVE PROBE on %s: %s.\n"
                    "  ********************************************************************************\n\n",
                    gpu.name.c_str(), probe.problem.c_str());
            }
            else if (!probe.configOk)
            {
                Log("\n  ********************************************************************************\n"
                    "  WARNING: WAVE PROBE on %s: the shaders are compiled for WAVE_SIZE=%u%s, but\n"
                    "  %s.\n"
                    "  Results of algorithms that use wave ops are not meaningful in this configuration.\n",
                    gpu.name.c_str(), gpuWave[gi].size, gpuWave[gi].attribute ? " + [WaveSize]" : "",
                    probe.problem.c_str());
                if (opt.smoke && !opt.waveProbe)
                {
                    // Smoke mode: do not run them (their sort failures would only be confusing).
                    size_t marked = 0;
                    for (const auto& ca : compiled)
                    {
                        if (!ca.usesWaveOps)
                            continue;
                        record.smokeFailed.push_back({ca.name, "", "wave probe: " + probe.problem});
                        ++marked;
                    }
                    Log("  Smoke test: %zu algorithm(s) that use wave ops are marked failed without running.\n",
                        marked);
                }
                Log("  ********************************************************************************\n\n");
            }

            // Spin drain (DrainKind::Spin): calibrate its loop rate on this GPU right before the runs,
            // after a few iterations of the flush work (clocks as in the run).
            const bool needSpin = std::any_of(compiled.begin(), compiled.end(), [](const CompiledAlgorithm& ca) {
                return ca.flush.drain == DrainKind::Spin;
            });
            if (needSpin)
            {
                window.SetText(L"Calibrating the spin drain ...");
                bench.Calibrate(workloadIds[0], opt.warp ? kSpinWarmupIterationsWarp : kSpinWarmupIterations);
                record.drainSpin = bench.CalibrateDrainSpin();
                record.drainSpinCalibrated = true;
                const GpuBenchmark::SpinCalibration& c = record.drainSpin;
                Log("  Drain spin calibration: %.2f loop iterations per us (%u vs 1 iterations: %.2f us apart; "
                    "1-iteration dispatch %.2f us); check: %u us = %u iterations measured %.2f us\n",
                    c.iterationsPerUs, c.iterations, c.spanUs, c.overheadUs, GpuBenchmark::kSpinCheckUs,
                    c.checkIterations, c.checkUs);
                for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                    Log("    %s\n", m.c_str());
            }

            // (--wave-probe: no sorts, nothing more to submit.)
            for (size_t ci = 0; ci < sortCounts.size() && !opt.waveProbe; ++ci)
            {
                const uint32_t numSorts = sortCounts[ci];
                const uint32_t iterations = gpuIterations[gi][ci];
                const uint64_t totalIterations = gpuCountIterations[gi][ci];
                bench.SetSortCount(numSorts);
                if (multiCount)
                    Log("\n  --- %u sorts per iteration (%zu/%zu): %u iterations + %u warmup, %u per command list ---\n",
                        numSorts, ci + 1, sortCounts.size(), iterations, opt.warmup, bench.BatchSize());
                const auto countStart = std::chrono::steady_clock::now();

                // Run-time estimate: the iterations still to run at this sort count at its measured rate
                // (the calibration until kMeasuredRateMinIterations are done), plus the estimates of the
                // later sort counts and GPUs.
                uint64_t gpuIterationsDone = 0; // completed combos at this count (skipped ones count as done)
                auto remainingSeconds = [&](uint64_t doneNow) {
                    const double countElapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - countStart).count();
                    const double rate = doneNow >= kMeasuredRateMinIterations
                                            ? countElapsed / static_cast<double>(doneNow)
                                            : gpuSecondsPerIteration[gi][ci];
                    double remaining = static_cast<double>(totalIterations - std::min(doneNow, totalIterations)) * rate;
                    for (size_t laterCount = ci + 1; laterCount < sortCounts.size(); ++laterCount)
                        remaining += gpuCountEstimateSeconds[gi][laterCount];
                    for (size_t later = gi + 1; later < gpus.size(); ++later)
                        remaining += gpuEstimateSeconds[later];
                    return remaining;
                };

                for (size_t wi = 0; wi < workloadIds.size(); ++wi)
                {
                    const uint32_t workloadId = workloadIds[wi];
                    for (size_t ai = 0; ai < compiled.size(); ++ai)
                    {
                        const char* wname = Workloads()[workloadId].name;
                        const std::string& aname = compiled[ai].name;
                        // --smoke: an algorithm with a verification failure on this GPU skips its remaining
                        // workloads and sort counts; the other algorithms keep running.
                        const auto failed = std::find_if(record.smokeFailed.begin(), record.smokeFailed.end(),
                                                         [&](const SmokeFailure& sf) { return sf.algorithm == aname; });
                        if (failed != record.smokeFailed.end())
                        {
                            gpuIterationsDone += iterations + opt.warmup;
                            Log("  %-14s %-20s skipped (%s)\n", wname, aname.c_str(),
                                failed->reason.empty() ? "failed verification earlier in this smoke run"
                                                       : failed->reason.c_str());
                            continue;
                        }
                        uint32_t lastLogged = 0;
                        auto progress = [&](uint32_t done, uint32_t total, uint32_t failures) {
                            const double elapsed =
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
                            const double remaining = remainingSeconds(gpuIterationsDone + done);
                            const std::string countLine =
                                multiCount ? Format("Sorts per iteration: %u (%zu/%zu)\n", numSorts, ci + 1, sortCounts.size())
                                           : std::string();
                            window.SetText(Utf8ToWide(Format(
                                "GPU %zu/%zu: %s\n%sWorkload %zu/%zu: %s\nAlgorithm %zu/%zu: %s\n"
                                "Iteration %u / %u (incl. %u warmup)\nVerification failures: %u (this run), %u (total)\n"
                                "Elapsed: %s, estimated remaining: ~%s (total ~%s)",
                                gi + 1, gpus.size(), gpu.name.c_str(), countLine.c_str(), wi + 1, workloadIds.size(), wname,
                                ai + 1, compiled.size(), aname.c_str(), done, total, opt.warmup, failures,
                                totalFailures + failures, FormatDuration(elapsed).c_str(),
                                FormatDuration(remaining).c_str(), FormatDuration(elapsed + remaining).c_str())));
                            if (done == total || done - lastLogged >= 250)
                            {
                                lastLogged = done;
                                if (done != total)
                                    Log("  %-14s %-20s %5u / %u  failures %u\n", wname, aname.c_str(), done, total,
                                        failures);
                            }
                        };
                        progress(0, iterations + opt.warmup, 0);
                        // Recorded before running, so a device loss leaves the partial result in place.
                        record.combos.push_back({workloadId, aname, {}, compiled[ai].flush, numSorts, iterations});
                        ComboResult& result = record.combos.back().result;
                        const std::string label =
                            gpu.name + "/" + aname + "/" + wname +
                            (multiCount || numSorts != kDefaultSortsPerIteration ? Format("/%u sorts", numSorts) : "");
                        Log("  starting %s\n", label.c_str());
                        bench.Run(workloadId, ai, iterations, opt.warmup, label, progress, result);
                        gpuIterationsDone += result.iterationsRun;
                        totalFailures += result.failures;
                        const Stats st = ComputeStats(result.timesUs);
                        const double perIteration = result.iterationsRun ? 1000.0 / result.iterationsRun : 0.0;
                        Log("  %-14s %-20s done: median %8.2f us, mean %8.2f us, failures %u (%.1f s; CPU per iteration: "
                            "generate %.2f ms, record %.2f ms, verify %.2f ms)\n",
                            wname, aname.c_str(), st.median, st.mean, result.failures, result.wallSeconds,
                            result.generateSeconds * perIteration, result.recordSeconds * perIteration,
                            result.verifySeconds * perIteration);
                        for (const auto& m : result.failureMessages)
                            Log("    FAIL %s\n", m.c_str());
                        for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                            Log("    %s\n", m.c_str());
                        if (opt.smoke && result.failures > 0)
                        {
                            // A device loss or fence timeout still stops everything (it throws). A
                            // verification failure only takes this algorithm out of the rest of the run.
                            record.smokeFailed.push_back({aname, wname});
                            Log("  Smoke test: %s failed verification on %s; skipping its remaining workloads on "
                                "this GPU\n",
                                aname.c_str(), wname);
                        }
                    }
                    const double elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
                    const double remaining = remainingSeconds(gpuIterationsDone);
                    if (multiCount)
                        Log("  [workload %zu/%zu done on this GPU at %u sorts] elapsed %s, estimated remaining ~%s (total "
                            "~%s)\n",
                            wi + 1, workloadIds.size(), numSorts, FormatDuration(elapsed).c_str(),
                            FormatDuration(remaining).c_str(), FormatDuration(elapsed + remaining).c_str());
                    else
                        Log("  [workload %zu/%zu done on this GPU] elapsed %s, estimated remaining ~%s (total ~%s)\n",
                            wi + 1, workloadIds.size(), FormatDuration(elapsed).c_str(), FormatDuration(remaining).c_str(),
                            FormatDuration(elapsed + remaining).c_str());
                }
            }
        }
        catch (const std::exception& e)
        {
            // Any failure while the device is removed (e.g. a Map or Close returning
            // DXGI_ERROR_DEVICE_REMOVED) counts as a device loss, not just DeviceLostError.
            const bool lost = dynamic_cast<const DeviceLostError*>(&e) != nullptr ||
                              FAILED(gpu.device->GetDeviceRemovedReason());
            record.error = e.what();
            Log("  ERROR on %s: %s\n", gpu.name.c_str(), e.what());
            if (!record.combos.empty())
            {
                const ComboRecord& c = record.combos.back();
                Log("  (while running %s / %s at %u sorts per iteration, %u iterations completed)\n",
                    Workloads()[c.workloadId].name, c.algorithm.c_str(), c.sortsPerIteration, c.result.iterationsRun);
            }
            if (!record.combos.empty())
                totalFailures += record.combos.back().result.failures;
            for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                Log("    %s\n", m.c_str());
            if (lost)
            {
                deviceLost = true;
                // Work may still be in flight (a hung GPU, or WARP which keeps executing after
                // removal): releasing the buffers now could free memory the GPU is still using.
                // Leak the benchmark (and with it every resource) on purpose; the process exits soon.
                (void)benchPtr.release();
                record.error = "DEVICE LOST: " + record.error;
                Log("\n%s", FormatDredReport(gpu.device.Get()).c_str());
                Log("\nStopping: nothing more is submitted to any GPU. Writing partial results.\n");
            }
        }
        record.wallSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - gpuStart).count();
        info.gpus.push_back(std::move(record));
    }
    window.Stop();

    info.totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();

    bool anyError = false;
    bool anySmokeFailed = false;
    bool anyProbeWarning = false;
    for (const auto& g : info.gpus)
    {
        anyError |= !g.error.empty();
        anySmokeFailed |= !g.smokeFailed.empty();
        anyProbeWarning |= g.waveProbeWarning;
    }

    // --- --wave-probe: probe report only ---------------------------------------------------
    if (opt.waveProbe)
    {
        std::string text = "GpuSort wave probe\n==================\n";
        text += Format("Date:         %s\nComputer:     %s\n", info.date.c_str(), info.computerName.c_str());
        if (!info.label.empty())
            text += Format("Label:        %s\n", info.label.c_str());
        text += Format("Command line: %s\n", info.commandLine.c_str());
        const fs::path probeCsv = !opt.csvPath.empty()  ? fs::path(opt.csvPath)
                                  : !opt.outPath.empty() ? CsvPathFor(fs::path(opt.outPath), L"wave_probe")
                                                         : ExeDir() / "wave_probe.csv";
        text += Format("CSV file:     %s\n",
                       CsvDisplayName(probeCsv, opt.outPath.empty() ? ExeDir() / "wave_probe.txt"
                                                                    : fs::path(opt.outPath))
                           .c_str());
        for (size_t g = 0; g < info.gpus.size(); ++g)
        {
            const GpuRecord& gpu = info.gpus[g];
            text += Format("[%zu] %s  (driver %s, vendor %04X device %04X)\n", g, gpu.name.c_str(), gpu.driver.c_str(),
                           gpu.vendorId, gpu.deviceId);
            text += Format("      wave lanes %u-%u, sort shaders would be compiled with WAVE_SIZE=%u%s\n",
                           gpu.waveLaneCountMin, gpu.waveLaneCountMax, gpu.waveSize,
                           gpu.waveSizeAttribute ? " + [WaveSize]" : "");
            text += Format("      probed: %s\n", DescribeWaveProbe(*gpuProbe[g]).c_str());
            for (const auto& line : gpu.waveProbe)
                text += Format("      %s\n", line.c_str());
            if (!gpu.error.empty())
                text += Format("      ERROR: %s\n", gpu.error.c_str());
        }
        for (const auto& s : info.skippedAdapters)
            text += Format("  skipped: %s\n", s.c_str());

        // One verdict line per GPU x wave configuration.
        text += "\nSummary\n-------\n";
        for (size_t g = 0; g < gpus.size(); ++g)
        {
            if (g >= info.gpus.size())
            {
                text += Format("[%zu] %s: not run (stopped after a device loss)\n", g, gpus[g].name.c_str());
                continue;
            }
            const GpuRecord& gpu = info.gpus[g];
            for (const auto& line : gpu.waveProbeSummary)
                text += Format("[%zu] %s, %s\n", g, gpu.name.c_str(), line.c_str());
            if (!gpu.error.empty())
                text += Format("[%zu] %s: ERROR: %s\n", g, gpu.name.c_str(), gpu.error.c_str());
        }
        if (deviceLost)
            text += "Overall: DEVICE LOST (exit code 3)\n";
        else if (anyError)
            text += "Overall: ERROR (exit code 1)\n";
        else if (anyProbeWarning)
            text += "Overall: WAVE PROBE WARNING (exit code 1)\n";
        else
            text += "Overall: OK (exit code 0)\n";
        Log("\n%s", text.c_str());
        if (!opt.outPath.empty())
        {
            const fs::path outPath(opt.outPath);
            if (outPath.has_parent_path())
            {
                std::error_code ec;
                fs::create_directories(outPath.parent_path(), ec);
            }
            std::ofstream out(outPath, std::ios::binary);
            if (out)
            {
                out << text;
                Log("\nWave probe report written to %s\n", fs::absolute(outPath).string().c_str());
            }
            else
            {
                Log("\nerror: could not write %s\n", outPath.string().c_str());
            }
        }
        WriteCsv(WriteWaveProbeCsv, info, probeCsv);
        if (deviceLost)
            return 3;
        return (anyError || anyProbeWarning) ? 1 : 0;
    }

    // --- results --------------------------------------------------------------------------
    const fs::path outPath = opt.outPath.empty() ? ExeDir() / "results.txt" : fs::path(opt.outPath);
    const fs::path resultsCsv = opt.csvPath.empty() ? CsvPathFor(outPath, L"results") : fs::path(opt.csvPath);
    const fs::path samplesCsv = CsvSibling(resultsCsv, L"_samples");
    const fs::path probeCsv = CsvSibling(resultsCsv, L"_wave_probe");
    // (No "wave probe" in this line: run_all.bat collects the lines containing it.)
    info.csvFiles = CsvDisplayName(resultsCsv, outPath) + " (per GPU x algorithm x workload), " +
                    (opt.noSamples ? std::string() : CsvDisplayName(samplesCsv, outPath) + " (per iteration), ") +
                    CsvDisplayName(probeCsv, outPath) + " (per wave configuration); columns: CSV_FORMAT.md";

    const std::string text = FormatResults(info);
    Log("\n%s", text.c_str());

    if (outPath.has_parent_path())
    {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
    }
    std::ofstream out(outPath, std::ios::binary);
    if (out)
    {
        out << text;
        Log("\nResults written to %s\n", fs::absolute(outPath).string().c_str());
    }
    else
    {
        Log("\nerror: could not write %s\n", outPath.string().c_str());
    }
    WriteCsv(WriteResultsCsv, info, resultsCsv);
    if (!opt.noSamples)
        WriteCsv(WriteSamplesCsv, info, samplesCsv);
    WriteCsv(WriteWaveProbeCsv, info, probeCsv);

    if (anyProbeWarning)
        Log("\nWARNING: the wave probe found a wrong lane count or lane mapping (see \"WAVE PROBE WARNING\" above).\n");
    if (deviceLost)
        return 3;
    return (totalFailures > 0 || anyError || anySmokeFailed) ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        return RunMain(argc, argv);
    }
    catch (const std::exception& e)
    {
        LogError("error: %s\n", e.what());
        return 1;
    }
}
