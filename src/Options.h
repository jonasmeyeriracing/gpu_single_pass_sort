#pragma once

#include "Algorithms.h"
#include "Workloads.h"

#include <cstdint>
#include <string>
#include <vector>

struct Options
{
    std::wstring shaderDir;              // --shaders <dir>; empty = auto-detect
    std::wstring algoFile;               // --algo-file <file>: algorithm list (relative: to the shader dir);
                                         // empty = algorithms.txt
    std::wstring outPath;                // --out <file>; empty = results.txt next to the exe
    std::wstring csvPath;                // --csv <file>; empty = the --out file with the extension .csv
    bool noSamples = false;              // --no-samples: do not write <csv stem>_samples.csv
    std::wstring logPath;                // --log <file>: also write the console output to this file
    std::string label;                   // --label <text>: shown in the prompt, window and results header
    std::vector<uint32_t> sortCounts = {kDefaultSortsPerIteration}; // --sorts N[,N...]: sorts per iteration,
                                         // each 1..kMaxSortsPerIteration; every combo runs once per count, in
                                         // this order
    std::vector<uint32_t> iterations = {1000}; // --iterations N[,N...] (measured iterations per gpu/workload/
                                         // algorithm): one value for every sort count, or one per --sorts entry
    bool iterationsGiven = false;        // --iterations was given explicitly (then it also applies to --smoke)
    std::vector<uint32_t> iterationsIntegrated; // --iterations-integrated N[,N...]: the same on integrated (UMA)
                                         // GPUs instead of --iterations; empty = --iterations everywhere
    uint32_t warmup = 5;                 // --warmup N (extra iterations excluded from stats)
    std::vector<std::string> algorithms; // --algo name[,name]; empty = all registered
    std::vector<std::string> workloads;  // --workload name[,name]; empty = all
    std::string gpuFilter;               // --gpu <substring>; empty = all qualifying GPUs
    bool integratedOnly = false;         // --integrated-only: only GPUs with D3D12 UMA (integrated GPUs)
    bool discreteOnly = false;           // --discrete-only: only GPUs without D3D12 UMA (discrete GPUs)
    uint32_t waveSize = 0;               // --wave-size N: compile with WAVE_SIZE=N + [WaveSize(N)]; 0 = device min
                                         // (GPUs whose lane range does not contain N are skipped)
    FlushMode flushMode = kDefaultFlushMode; // --flush-mode: default for algorithms without a 'flush' line
    bool stablePower = false;            // --stable-power: ID3D12Device::SetStablePowerState(TRUE) per GPU (only if
                                         // Windows Developer Mode is on; else the run continues without it)
    bool warp = false;                   // --warp: run only on WARP (no prompt, no window)
    bool noPrompt = false;               // --no-prompt
    bool debugLayer = false;             // --debug: enable the D3D12 debug layer
    bool gpuValidation = false;          // --gbv: GPU-based validation (implies --debug)
    bool dred = false;                   // --dred: DRED auto-breadcrumbs + page-fault reporting
    bool smoke = false;                  // --smoke: 3 iterations (unless --iterations), no warmup, one iteration in flight
    bool testRemove = false;             // --test-device-removal: remove the device after the first batch
    bool listAdapters = false;           // --list-adapters: print DXGI adapters and exit
    bool waveProbe = false;              // --wave-probe: only run the wave probe (after the prompt) and exit
    bool help = false;
};

// Returns false and sets 'error' on invalid arguments.
bool ParseOptions(int argc, wchar_t** argv, Options& options, std::string& error);

// Measured iterations at options.sortCounts[countIndex] on a GPU (integrated: --iterations-integrated
// if given). A single --iterations / --iterations-integrated value applies to every sort count.
uint32_t IterationsFor(const Options& options, size_t countIndex, bool integrated);
void PrintUsage();
