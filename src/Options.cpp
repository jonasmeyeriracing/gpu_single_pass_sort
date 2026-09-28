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
        "  --warp               Run only on the WARP software adapter (no prompt, no window)\n"
        "  --no-prompt          Skip the confirmation message box\n"
        "  --debug              Enable the D3D12 debug layer\n"
        "  --list-adapters      Print all DXGI adapters (LUID, ids, flags) and exit\n"
        "  --help               Show this help\n");
}
