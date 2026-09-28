#include "Options.h"

#include "Common.h"

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
        else if (arg == L"--shaders")
        {
            if (!next(o.shaderDir))
                return false;
        }
        else if (arg == L"--out")
        {
            if (!next(o.outPath))
                return false;
        }
        else if (arg == L"--iterations" || arg == L"--warmup")
        {
            if (!next(value))
                return false;
            uint32_t n = 0;
            if (!ParseUInt(value, n) || (arg == L"--iterations" && n == 0))
            {
                error = "invalid value for " + WideToUtf8(arg) + ": " + WideToUtf8(value);
                return false;
            }
            (arg == L"--iterations" ? o.iterations : o.warmup) = n;
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
        else
        {
            error = "unknown argument: " + WideToUtf8(arg);
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
        "  --out <file>         Results file (default: results.txt next to the exe)\n"
        "  --iterations <N>     Measured iterations per GPU x workload x algorithm (default 1000)\n"
        "  --warmup <N>         Warmup iterations excluded from stats (default 5)\n"
        "  --algo <a[,b]>       Algorithms to run (default: all in algorithms.txt); repeatable\n"
        "  --workload <w[,x]>   Workloads to run (default: all); repeatable\n"
        "  --gpu <substring>    Only run on GPUs whose name contains <substring> (case-insensitive)\n"
        "  --wave-size <N>      Compile the shaders for wave size N with [WaveSize(N)] (N must be within\n"
        "                       the device's WaveLaneCountMin..Max). Default: WAVE_SIZE = WaveLaneCountMin,\n"
        "                       plus [WaveSize] only if the device reports a range (Min != Max)\n"
        "  --warp               Run only on the WARP software adapter (no prompt, no window)\n"
        "  --no-prompt          Skip the confirmation message box\n"
        "  --debug              Enable the D3D12 debug layer\n"
        "  --gbv                Enable GPU-based validation (implies --debug; slow)\n"
        "  --dred               Enable DRED auto-breadcrumbs + page-fault reporting; on device removal\n"
        "                       print the last completed GPU operation and the faulting allocation\n"
        "  --smoke              Safety check: 3 iterations (no warmup) of every algorithm x workload,\n"
        "                       one iteration in flight at a time (overrides --iterations/--warmup)\n"
        "  --test-device-removal\n"
        "                       Debug aid: call ID3D12Device5::RemoveDevice after the first batch to\n"
        "                       exercise the device-lost path (use with --warp)\n"
        "  --list-adapters     Print all DXGI adapters (LUID, ids, flags) and exit\n"
        "  --help               Show this help\n");
}
