#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Options
{
    std::wstring shaderDir;              // --shaders <dir>; empty = auto-detect
    std::wstring outPath;                // --out <file>; empty = results.txt next to the exe
    uint32_t iterations = 1000;          // --iterations N (measured iterations per gpu/workload/algorithm)
    bool iterationsGiven = false;        // --iterations was given explicitly (then it also applies to --smoke)
    uint32_t warmup = 5;                 // --warmup N (extra iterations excluded from stats)
    std::vector<std::string> algorithms; // --algo name[,name]; empty = all registered
    std::vector<std::string> workloads;  // --workload name[,name]; empty = all
    std::string gpuFilter;               // --gpu <substring>; empty = all qualifying GPUs
    uint32_t waveSize = 0;               // --wave-size N: compile with WAVE_SIZE=N + [WaveSize(N)]; 0 = device min
    bool warp = false;                   // --warp: run only on WARP (no prompt, no window)
    bool noPrompt = false;               // --no-prompt
    bool debugLayer = false;             // --debug: enable the D3D12 debug layer
    bool gpuValidation = false;          // --gbv: GPU-based validation (implies --debug)
    bool dred = false;                   // --dred: DRED auto-breadcrumbs + page-fault reporting
    bool smoke = false;                  // --smoke: 3 iterations (unless --iterations), no warmup, one iteration in flight
    bool testRemove = false;             // --test-device-removal: remove the device after the first batch
    bool listAdapters = false;           // --list-adapters: print DXGI adapters and exit
    bool help = false;
};

// Returns false and sets 'error' on invalid arguments.
bool ParseOptions(int argc, wchar_t** argv, Options& options, std::string& error);
void PrintUsage();
