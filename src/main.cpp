// GpuSort: DX12 benchmark / test framework for many small single-group sorts.
#include "Algorithms.h"
#include "Benchmark.h"
#include "Common.h"
#include "Device.h"
#include "Options.h"
#include "ProgressWindow.h"
#include "Results.h"
#include "ShaderCompiler.h"
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
// Rough guess for the prompt (dominated by the 256 MB cache flush per iteration).
constexpr double kEstimatedMsPerIteration = 2.5;

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

std::string Now()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string CommandLineUtf8()
{
    return WideToUtf8(GetCommandLineW());
}

int RunMain(int argc, wchar_t** argv)
{
    Options opt;
    std::string error;
    if (!ParseOptions(argc, argv, opt, error))
    {
        fprintf(stderr, "error: %s\n\n", error.c_str());
        PrintUsage();
        return 1;
    }
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
        opt.iterations = kSmokeIterations;
        opt.warmup = 0;
    }

    // --- shaders / algorithms -------------------------------------------------------------
    const fs::path shaderDir = opt.shaderDir.empty() ? FindShaderDir() : fs::absolute(opt.shaderDir);
    std::vector<AlgorithmDesc> allAlgorithms = LoadAlgorithms(shaderDir);
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
    ShaderCompiler compiler;
    std::string log;
    ComPtr<IDxcBlob> flushShader = compiler.CompileSource(kFlushShaderSource, L"flush.hlsl", "main", {}, log);
    if (!flushShader)
        throw std::runtime_error("flush shader failed to compile:\n" + log);

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
    info.date = Now();
    info.shaderDir = shaderDir.string();
    info.commandLine = CommandLineUtf8();
    info.iterations = opt.iterations;
    info.warmup = opt.warmup;
    info.workloadIds = workloadIds;
    for (const auto& a : algorithms)
        info.algorithms.push_back(a.name);

    std::vector<GpuInfo> gpus = EnumerateGpus(opt.warp, opt.gpuFilter, info.skippedAdapters);
    for (const auto& s : info.skippedAdapters)
        Log("Skipping adapter %s\n", s.c_str());
    if (gpus.empty())
        throw std::runtime_error(opt.warp ? "WARP adapter does not qualify (needs SM 6.6)"
                                          : "no qualifying hardware GPU found (needs D3D12 + SM 6.6)");
    for (const auto& g : gpus)
        Log("Using adapter: %s (driver %s, %llu MB, wave lanes %u-%u)\n", g.name.c_str(), g.driver.c_str(),
            static_cast<unsigned long long>(g.dedicatedVideoMemory >> 20), g.waveLaneCountMin, g.waveLaneCountMax);

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
    auto compileFor = [&](const WaveConfig& wc) -> const std::vector<CompiledAlgorithm>& {
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
                ca.shaders.push_back(blob);
            }
            Log("Compiled algorithm %s (%zu dispatch%s)\n", ca.name.c_str(), ca.shaders.size(),
                ca.shaders.size() == 1 ? "" : "es");
            compiled.push_back(std::move(ca));
        }
        compiledByWave.push_back({{wc.size, wc.attribute}, std::move(compiled)});
        return compiledByWave.back().second;
    };
    // Compile everything up front, so shader errors show up before the prompt.
    for (const auto& wc : gpuWave)
        compileFor(wc);

    Log("\n%s\n", FormatSizeDistribution(workloadIds, opt.iterations).c_str());

    const uint64_t totalIterations = uint64_t(opt.iterations + opt.warmup) * workloadIds.size() * algorithms.size();
    const double estimatedSeconds = static_cast<double>(totalIterations * gpus.size()) * kEstimatedMsPerIteration / 1000.0;

    // --- prompt ---------------------------------------------------------------------------
    if (!opt.warp && !opt.noPrompt)
    {
        std::wstring text;
        if (opt.smoke)
            text = L"SMOKE TEST: a short safety check after the previous crash (GPU fault, TDR and a\n"
                   L"VIDEO_SCHEDULER_INTERNAL_ERROR bugcheck during the last full run).\n\n"
                   L"Every algorithm x workload runs a few iterations, one at a time with a fence wait\n"
                   L"after each, and stops at the first GPU fault or hang.\n\n";
        text += L"GpuSort is about to run a GPU benchmark on:\n\n";
        for (const auto& g : gpus)
            text += L"    " + Utf8ToWide(g.name) + L"\n";
        text += Utf8ToWide(Format("\n%zu workload(s) x %zu algorithm(s) x %u iterations (+%u warmup) per GPU.\n"
                                  "Estimated duration: roughly %.0f seconds.\n\n",
                                  workloadIds.size(), algorithms.size(), opt.iterations, opt.warmup,
                                  estimatedSeconds < 1.0 ? 1.0 : estimatedSeconds));
        text += L"Please pause other GPU work now, then press OK to start.\nCancel exits without running.";
        Log("Waiting for confirmation (message box)...\n");
        const int answer = MessageBoxW(nullptr, text.c_str(), opt.smoke ? L"GpuSort smoke test" : L"GpuSort benchmark",
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
        window.Start(L"GpuSort benchmark running - please keep the GPUs idle");

    BenchmarkOptions benchOptions;
    benchOptions.fenceTimeoutMs = opt.warp ? kFenceTimeoutMsWarp : kFenceTimeoutMsHardware;
    benchOptions.serial = opt.smoke;
    benchOptions.markers = opt.dred;
    benchOptions.logAddresses = opt.dred;
    benchOptions.testRemoveDevice = opt.testRemove;

    uint32_t totalFailures = 0;
    bool deviceLost = false;
    bool smokeStopped = false; // --smoke stops at the first verification failure
    for (size_t gi = 0; gi < gpus.size() && !deviceLost && !smokeStopped; ++gi)
    {
        const GpuInfo& gpu = gpus[gi];
        GpuRecord record;
        record.name = gpu.name;
        record.driver = gpu.driver;
        record.waveLaneCountMin = gpu.waveLaneCountMin;
        record.waveLaneCountMax = gpu.waveLaneCountMax;
        record.waveSize = gpuWave[gi].size;
        record.waveSizeAttribute = gpuWave[gi].attribute;
        const std::vector<CompiledAlgorithm>& compiled = compileFor(gpuWave[gi]);
        const auto gpuStart = std::chrono::steady_clock::now();
        Log("\n=== GPU %zu/%zu: %s (WAVE_SIZE %u%s) ===\n", gi + 1, gpus.size(), gpu.name.c_str(),
            gpuWave[gi].size, gpuWave[gi].attribute ? " + [WaveSize]" : "");
        std::unique_ptr<GpuBenchmark> benchPtr;
        try
        {
            benchPtr = std::make_unique<GpuBenchmark>(gpu.device.Get(), flushShader.Get(), compiled, benchOptions);
            GpuBenchmark& bench = *benchPtr;
            record.timestampFrequency = bench.TimestampFrequency();
            for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                Log("    %s\n", m.c_str());
            for (size_t wi = 0; wi < workloadIds.size() && !smokeStopped; ++wi)
            {
                const uint32_t workloadId = workloadIds[wi];
                for (size_t ai = 0; ai < compiled.size() && !smokeStopped; ++ai)
                {
                    const char* wname = Workloads()[workloadId].name;
                    const std::string& aname = compiled[ai].name;
                    uint32_t lastLogged = 0;
                    auto progress = [&](uint32_t done, uint32_t total, uint32_t failures) {
                        const double elapsed =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
                        window.SetText(Utf8ToWide(Format(
                            "GPU %zu/%zu: %s\nWorkload %zu/%zu: %s\nAlgorithm %zu/%zu: %s\n"
                            "Iteration %u / %u (incl. %u warmup)\nVerification failures: %u (this run), %u (total)\n"
                            "Elapsed: %.0f s",
                            gi + 1, gpus.size(), gpu.name.c_str(), wi + 1, workloadIds.size(), wname, ai + 1,
                            compiled.size(), aname.c_str(), done, total, opt.warmup, failures,
                            totalFailures + failures, elapsed)));
                        if (done == total || done - lastLogged >= 250)
                        {
                            lastLogged = done;
                            if (done != total)
                                Log("  %-14s %-20s %5u / %u  failures %u\n", wname, aname.c_str(), done, total,
                                    failures);
                        }
                    };
                    progress(0, opt.iterations + opt.warmup, 0);
                    // Recorded before running, so a device loss leaves the partial result in place.
                    record.combos.push_back({workloadId, aname, {}});
                    ComboResult& result = record.combos.back().result;
                    const std::string label = gpu.name + "/" + aname + "/" + wname;
                    Log("  starting %s\n", label.c_str());
                    bench.Run(workloadId, ai, opt.iterations, opt.warmup, label, progress, result);
                    totalFailures += result.failures;
                    const Stats st = ComputeStats(result.timesUs);
                    Log("  %-14s %-20s done: median %8.2f us, mean %8.2f us, failures %u (%.1f s)\n", wname,
                        aname.c_str(), st.median, st.mean, result.failures, result.wallSeconds);
                    for (const auto& m : result.failureMessages)
                        Log("    FAIL %s\n", m.c_str());
                    for (const auto& m : DrainDebugMessages(gpu.device.Get()))
                        Log("    %s\n", m.c_str());
                    if (opt.smoke && result.failures > 0)
                    {
                        // Wrong results can be the first sign of a GPU exception; do not keep going.
                        smokeStopped = true;
                        record.error = "smoke test stopped at the first verification failure (" + label + ")";
                        Log("\nSmoke test: stopping at the first verification failure. Nothing more is submitted.\n");
                    }
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
                Log("  (while running %s / %s, %u iterations completed)\n", Workloads()[c.workloadId].name,
                    c.algorithm.c_str(), c.result.iterationsRun);
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

    // --- results --------------------------------------------------------------------------
    const std::string text = FormatResults(info);
    Log("\n%s", text.c_str());

    const fs::path outPath = opt.outPath.empty() ? ExeDir() / "results.txt" : fs::path(opt.outPath);
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

    bool anyError = false;
    for (const auto& g : info.gpus)
        anyError |= !g.error.empty();
    if (deviceLost)
        return 3;
    return (totalFailures > 0 || anyError) ? 1 : 0;
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
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
