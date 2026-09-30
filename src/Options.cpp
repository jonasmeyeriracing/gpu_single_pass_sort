#include "Options.h"

#include "Benchmark.h"
#include "Common.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

static void SplitList(const std::string& s, std::vector<std::string>& out)
{
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        if (!item.empty())
            out.push_back(item);
    }
}

static bool ParseUInt(const std::wstring& s, uint32_t& value)
{
    if (s.empty())
        return false;
    uint64_t v = 0;
    for (wchar_t c : s)
    {
        if (c < L'0' || c > L'9')
            return false;
        v = v * 10 + static_cast<uint64_t>(c - L'0');
        if (v > 0xFFFFFFFFull)
            return false;
    }
    value = static_cast<uint32_t>(v);
    return true;
}

// Comma-separated unsigned integers ("1000" or "1000,300,200"); false if any item is not one.
static bool ParseUIntList(const std::wstring& s, std::vector<uint32_t>& values)
{
    values.clear();
    size_t start = 0;
    for (;;)
    {
        const size_t comma = s.find(L',', start);
        uint32_t v = 0;
        if (!ParseUInt(s.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start), v))
            return false;
        values.push_back(v);
        if (comma == std::wstring::npos)
            return true;
        start = comma + 1;
    }
}

uint32_t IterationsFor(const Options& o, size_t countIndex, bool integrated)
{
    const std::vector<uint32_t>& list =
        integrated && !o.iterationsIntegrated.empty() ? o.iterationsIntegrated : o.iterations;
    return list.size() == 1 ? list[0] : list.at(countIndex);
}

bool ParseOptions(int argc, wchar_t** argv, Options& o, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        auto next = [&](std::wstring& value) -> bool {
            if (i + 1 >= argc)
            {
                error = "missing value for " + WideToUtf8(arg);
                return false;
            }
            value = argv[++i];
            return true;
        };
        std::wstring value;
        if (arg == L"--help" || arg == L"-h" || arg == L"/?")
            o.help = true;
        else if (arg == L"--warp")
            o.warp = true;
        else if (arg == L"--no-prompt")
            o.noPrompt = true;
        else if (arg == L"--debug")
            o.debugLayer = true;
        else if (arg == L"--gbv")
            o.gpuValidation = o.debugLayer = true;
        else if (arg == L"--dred")
            o.dred = true;
        else if (arg == L"--smoke")
            o.smoke = true;
        else if (arg == L"--test-device-removal")
            o.testRemove = true;
        else if (arg == L"--list-adapters")
            o.listAdapters = true;
        else if (arg == L"--wave-probe")
            o.waveProbe = true;
        else if (arg == L"--stable-power")
            o.stablePower = true;
        else if (arg == L"--integrated-only")
            o.integratedOnly = true;
        else if (arg == L"--discrete-only")
            o.discreteOnly = true;
        else if (arg == L"--shaders")
        {
            if (!next(o.shaderDir))
                return false;
        }
        else if (arg == L"--algo-file")
        {
            if (!next(o.algoFile))
                return false;
        }
        else if (arg == L"--out")
        {
            if (!next(o.outPath))
                return false;
        }
        else if (arg == L"--no-samples")
            o.noSamples = true;
        else if (arg == L"--csv")
        {
            if (!next(o.csvPath))
                return false;
        }
        else if (arg == L"--log")
        {
            if (!next(o.logPath))
                return false;
        }
        else if (arg == L"--label")
        {
            if (!next(value))
                return false;
            o.label = WideToUtf8(value);
        }
        else if (arg == L"--iterations" || arg == L"--iterations-integrated")
        {
            if (!next(value))
                return false;
            std::vector<uint32_t> list;
            if (!ParseUIntList(value, list) || std::find(list.begin(), list.end(), 0u) != list.end())
            {
                error = "invalid value for " + WideToUtf8(arg) + " (N, or N,N,... one per --sorts value; N >= 1): " +
                        WideToUtf8(value);
                return false;
            }
            if (arg == L"--iterations")
            {
                o.iterations = list;
                o.iterationsGiven = true;
            }
            else
                o.iterationsIntegrated = list;
        }
        else if (arg == L"--warmup")
        {
            if (!next(value))
                return false;
            uint32_t n = 0;
            if (!ParseUInt(value, n))
            {
                error = "invalid value for " + WideToUtf8(arg) + ": " + WideToUtf8(value);
                return false;
            }
            o.warmup = n;
        }
        else if (arg == L"--sorts")
        {
            if (!next(value))
                return false;
            std::vector<uint32_t> list;
            bool ok = ParseUIntList(value, list);
            for (size_t k = 0; ok && k < list.size(); ++k)
                ok = list[k] >= 1 && list[k] <= kMaxSortsPerIteration &&
                     std::find(list.begin(), list.begin() + k, list[k]) == list.begin() + k;
            if (!ok)
            {
                error = Format("invalid value for --sorts (N[,N...], each 1..%u, no duplicates): ",
                               kMaxSortsPerIteration) +
                        WideToUtf8(value);
                return false;
            }
            o.sortCounts = list;
        }
        else if (arg == L"--wave-size")
        {
            if (!next(value))
                return false;
            uint32_t n = 0;
            if (!ParseUInt(value, n) || n < 4 || n > 128 || (n & (n - 1)) != 0)
            {
                error = "invalid value for --wave-size (power of two 4..128): " + WideToUtf8(value);
                return false;
            }
            o.waveSize = n;
        }
        else if (arg == L"--algo")
        {
            if (!next(value))
                return false;
            SplitList(WideToUtf8(value), o.algorithms);
        }
        else if (arg == L"--workload")
        {
            if (!next(value))
                return false;
            SplitList(WideToUtf8(value), o.workloads);
        }
        else if (arg == L"--gpu")
        {
            if (!next(value))
                return false;
            o.gpuFilter = WideToUtf8(value);
        }
        else if (arg == L"--flush-mode")
        {
            if (!next(value))
                return false;
            if (!ParseFlushMode(WideToUtf8(value), o.flushMode))
            {
                error = Format("invalid value for --flush-mode (full, full_legacy, full_ro, code, data or none, "
                               "optionally with _dg or _d<0..%u>, see --help): ",
                               kMaxDrainUs) +
                        WideToUtf8(value);
                return false;
            }
        }
        else
        {
            error = "unknown argument: " + WideToUtf8(arg);
            return false;
        }
    }
    if (o.integratedOnly && o.discreteOnly)
    {
        error = "--integrated-only and --discrete-only exclude each other";
        return false;
    }
    for (const std::vector<uint32_t>* list : {&o.iterations, &o.iterationsIntegrated})
    {
        if (list->size() > 1 && list->size() != o.sortCounts.size())
        {
            error = Format("%s has %zu values and --sorts %zu: give one value, or one per sort count",
                           list == &o.iterations ? "--iterations" : "--iterations-integrated", list->size(),
                           o.sortCounts.size());
            return false;
        }
    }
    return true;
}

void PrintUsage()
{
    printf(
        "GpuSort - DX12 benchmark for many small per-group sorts\n"
        "\n"
        "Usage: GpuSort.exe [options]\n"
        "  --shaders <dir>      Shader directory containing algorithms.txt (default: shaders/ found\n"
        "                       next to or above the exe, then ./shaders)\n"
        "  --algo-file <file>   Algorithm list to use instead of algorithms.txt (a relative path is\n"
        "                       relative to the shader directory), e.g. algorithms_diag_flush.txt\n"
        "  --out <file>         Results file (default: results.txt next to the exe)\n"
        "  --csv <file>         Results CSV (default: the --out file with the extension .csv). Next to it:\n"
        "                       <stem>_samples.csv (every measured iteration) and <stem>_wave_probe.csv\n"
        "                       (the wave probe per GPU x wave configuration). With --wave-probe only the\n"
        "                       wave probe CSV, as <file> (default: the --out file with .csv, or\n"
        "                       wave_probe.csv next to the exe). Columns: CSV_FORMAT.md\n"
        "  --no-samples         Do not write <stem>_samples.csv\n"
        "  --log <file>         Also write all console output to <file>\n"
        "  --label <text>       Run label, shown in the prompt / progress window and in the results header\n"
        "  --sorts <N[,N...]>   Sorts per iteration (one batch: each algorithm dispatch runs N groups),\n"
        "                       each 1..%u (default 20). Every GPU x algorithm x workload runs once per\n"
        "                       value, in the given order, e.g. --sorts 20,128,256,512. An iteration of N\n"
        "                       sorts draws N sizes from the workload's distribution (the first 20 are\n"
        "                       those of the 20-sort iteration). CSV column sorts_per_iteration\n"
        "  --iterations <N[,N...]>\n"
        "                       Measured iterations per GPU x workload x algorithm (default 1000): one\n"
        "                       value for every sort count, or one per --sorts value, e.g. --sorts\n"
        "                       20,128,256,512 --iterations 1000,300,200,150\n"
        "  --iterations-integrated <N[,N...]>\n"
        "                       Measured iterations on integrated (UMA) GPUs instead of --iterations, e.g.\n"
        "                       --iterations 1000 --iterations-integrated 300 (an iGPU iteration takes ~10x\n"
        "                       as long; the 256 MB flush dominates); a list as for --iterations. The CSV\n"
        "                       column iterations_requested has each row's count. Default: --iterations\n"
        "                       on every GPU\n"
        "  --warmup <N>         Warmup iterations excluded from stats (default 5)\n"
        "  --algo <a[,b]>       Algorithms to run (default: all in algorithms.txt); repeatable\n"
        "  --workload <w[,x]>   Workloads to run (default: all); repeatable\n"
        "  --gpu <substring>    Only run on GPUs whose name contains <substring> (case-insensitive)\n"
        "  --integrated-only    Only run on integrated GPUs (D3D12 architecture UMA; --list-adapters shows\n"
        "                       \"integrated yes\" for them), e.g. the Ryzen iGPU next to a discrete GPU\n"
        "  --discrete-only      Only run on discrete GPUs (not UMA), e.g. the RX 7900 XTX at --wave-size 64\n"
        "                       without the Ryzen iGPU next to it\n"
        "  --wave-size <N>      Compile the shaders for wave size N with [WaveSize(N)]; GPUs whose\n"
        "                       WaveLaneCountMin..Max does not contain N are skipped. Default: WAVE_SIZE =\n"
        "                       WaveLaneCountMin, plus [WaveSize] only if the device reports a range (Min != Max)\n"
        "  --flush-mode <m>     What happens between the data upload and the timed sort, for algorithms\n"
        "                       without a 'flush' line in algorithms.txt (default %s = full with a ~50 us\n"
        "                       spin drain, since the final set; results of packages up to 627724b used\n"
        "                       full, i.e. the one-group drain, and are not directly comparable on small\n"
        "                       workloads). Every mode except full_legacy / full_ro ends with a drain\n"
        "                       (one-group dispatch + UAV barrier) right before the start timestamp:\n"
        "                         full: upload, 256 MB cache flush, drain: shader code and data cold\n"
        "                         full_legacy: full without the drain (the 'full' of pass0-pass4)\n"
        "                         full_ro: upload, 256 MB read-only flush, no drain (diagnostic)\n"
        "                         code: flush, then upload, drain: code cold, input data warm in L2\n"
        "                         data: upload, flush, untimed run of the same sort on a private copy of\n"
        "                               the data, drain: code warm, data cold\n"
        "                         none: upload, drain (no flush): code and data warm\n"
        "                       A suffix selects the drain of any of these (except full_legacy):\n"
        "                         _d<N>: SPIN drain of about N us (1..%u): an ALU-only dispatch (%u groups\n"
        "                                x %u threads, a dependent integer chain, one 16-byte store per\n"
        "                                thread to a private 4 KB buffer) whose loop count comes from a\n"
        "                                per-GPU calibration at the start of the run, then a UAV barrier\n"
        "                         _d0:   no drain (full_d0 = full_legacy, full_ro_d0 = full_ro)\n"
        "                         _dg:   the one-group drain above (pass5; the default of all but full_ro)\n"
        "                       e.g. full_d50 (the default), full_d20, full_ro_d20, none_d20\n"
        "  --stable-power       ID3D12Device::SetStablePowerState(TRUE) on every GPU (fixed clocks) after\n"
        "                       the prompt. Needs Windows Developer Mode: without it the call would\n"
        "                       remove the device, so GpuSort checks first and, if it is off, runs\n"
        "                       without it (results header / CSV column stable_power: unavailable)\n"
        "  --warp               Run only on the WARP software adapter (no prompt, no window)\n"
        "  --no-prompt          Skip the confirmation message box\n"
        "  --debug              Enable the D3D12 debug layer\n"
        "  --gbv                Enable GPU-based validation (implies --debug; slow)\n"
        "  --dred               Enable DRED auto-breadcrumbs + page-fault reporting; on device removal\n"
        "                       print the last completed GPU operation and the faulting allocation\n"
        "  --smoke              Safety check: 3 iterations (no warmup) of every algorithm x workload,\n"
        "                       one iteration in flight at a time (overrides --warmup; an explicit\n"
        "                       --iterations N still sets the iteration count)\n"
        "  --test-device-removal\n"
        "                       Debug aid: call ID3D12Device5::RemoveDevice after the first batch to\n"
        "                       exercise the device-lost path (use with --warp)\n"
        "  --list-adapters      Print all DXGI adapters (LUID, ids, flags), then the adapters a benchmark\n"
        "                       run would use (with wave lane ranges) and the skipped ones, and exit\n"
        "  --wave-probe         Only run the wave probe on every selected GPU and exit (on hardware after\n"
        "                       the prompt). Every run does it before the first sort: a tiny shader,\n"
        "                       compiled like the sort shaders (WAVE_SIZE, [WaveSize]) and without\n"
        "                       [WaveSize], reports the lane count the driver really uses, whether\n"
        "                       lane = SV_GroupIndex %% WAVE_SIZE, and checks WaveReadLaneAt (lane ^ 32,\n"
        "                       lane ^ 1, +16, last lane), WavePrefixSum, WaveActiveSum / CountBits /\n"
        "                       Ballot. --out <file> also writes the report there. If the lane count\n"
        "                       or mapping is wrong, a run prints a WARNING; --smoke then marks the\n"
        "                       algorithms that use wave ops as failed without running them.\n"
        "                       --wave-probe alone probes every wave configuration in one run (one\n"
        "                       prompt): without [WaveSize] and [WaveSize(N)] for every power of two N\n"
        "                       in each GPU's WaveLaneCountMin..Max, one verdict each; with --wave-size N\n"
        "                       only that configuration. Exit code 0 all OK, 1 any warning, 3 device lost\n"
        "  --help               Show this help\n",
        kMaxSortsPerIteration, FlushModeName(kDefaultFlushMode).c_str(), kMaxDrainUs, GpuBenchmark::kSpinGroups,
        GpuBenchmark::kSpinGroupSize);
}
