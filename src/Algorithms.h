#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using ShaderDefines = std::vector<std::pair<std::string, std::string>>;

// What the benchmark does between the upload of an iteration's data and the timed sort (see
// GpuBenchmark::Record). The default (--flush-mode, else Full) is the measurement of record; the
// others are diagnostics that separate the cost of cold shader code from the cost of cold data.
//
// pass5: every mode except FullLegacy and FullRo ends with a *drain* right before the start
// timestamp: a one-group dispatch on a private 4 KB buffer, then a UAV barrier. It forces the
// barriers recorded before it (the one after the 256 MB flush, the upload transitions) to be
// executed, and whatever tail of the flush they wait for, before the timed window opens. Without it
// the 7900 XTX timed ~13 us of flush tail in every 'full' iteration (see _test/pass5/notes.md).
enum class FlushMode
{
    Full,       // upload, 256 MB read+write flush, drain: shader code and data both cold (evicted from L2)
    FullLegacy, // pre-pass5 'full': upload, flush, sort (no drain; the flush tail can land in the timed window)
    FullRo,     // diagnostic: upload, 256 MB *read-only* flush (leaves no dirty lines), no drain
    Code,       // flush, upload, drain: code cold, input freshly written (warm in L2, like keys produced
                // by a previous pass)
    Data,       // upload, flush, an untimed run of the same dispatches on a private copy of the
                // iteration, drain: code warm, data cold
    None,       // upload, drain (no flush): code and data warm
};
const char* FlushModeName(FlushMode mode);
bool ParseFlushMode(const std::string& name, FlushMode& mode); // full / full_legacy / full_ro / code / data / none

// One ExecuteIndirect(DISPATCH {numSorts, 1, 1}) of one shader. Every group handles one sort
// (SV_GroupID.x = sort index) and early-outs if that sort is not in its tier.
struct DispatchDesc
{
    std::string file;       // relative to the shader directory
    std::string entry;
    uint32_t groupSize = 0; // passed to the shader as GROUP_SIZE
    ShaderDefines defines;  // extra defines, e.g. MIN_COUNT / MAX_COUNT
};

// An algorithm is a list of dispatches issued back-to-back without barriers.
struct AlgorithmDesc
{
    std::string name;
    std::vector<DispatchDesc> dispatches;
    bool flushGiven = false;             // a 'flush' line overrides --flush-mode for this algorithm
    FlushMode flush = FlushMode::Full;
};

// Algorithms are registered by the shader directory itself, in <shaderDir>/algorithms.txt:
//
//   # comment
//   algorithm <name>
//   dispatch <file.hlsl> <entryPoint> <groupSize> [NAME=VALUE ...]
//   flush <full|full_legacy|full_ro|code|data|none>   (optional: overrides --flush-mode for this algorithm)
//
// so every _test/passN snapshot carries its own algorithm list. Throws on parse errors.
std::vector<AlgorithmDesc> LoadAlgorithms(const std::filesystem::path& shaderDir);
