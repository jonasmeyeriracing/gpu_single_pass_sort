#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using ShaderDefines = std::vector<std::pair<std::string, std::string>>;

// What the benchmark does between the upload of an iteration's data and the timed sort (see
// GpuBenchmark::Record). The default (--flush-mode, else kDefaultFlushMode) is the measurement of
// record; the others are diagnostics that separate the cost of cold shader code from the cost of
// cold data, and the flush's tail from both.
//
// A flush mode is a kind (what runs before the sort) plus a drain (what runs right before the start
// timestamp, after the last barrier of the kind's work):
//   DrainKind::Group (pass5): a one-group dispatch of the flush shader on a private 4 KB buffer, then
//     a UAV barrier. It forces the barriers recorded before it (the one after the 256 MB flush, the
//     upload transitions) to be executed, and whatever tail of the flush they wait for, before the
//     timed window opens. Without it the 7900 XTX timed ~13 us of flush tail in every 'full'
//     iteration (see _test/pass5/notes.md).
//   DrainKind::Spin (pass6 diagnostic): an ALU-only spin dispatch (kSpinGroups groups, a dependent
//     integer chain, one store per thread to the same private 4 KB buffer, no other memory traffic)
//     of about drainUs microseconds (the loop count comes from a per-GPU calibration, see
//     GpuBenchmark::CalibrateDrainSpin), then a UAV barrier. On the 7900 XTX the one-group drain
//     only removed ~1 of the ~13 us: something after the flush lasts longer than a tiny dispatch;
//     the spin lets the GPU wait a known time without adding memory traffic.
//   DrainKind::None: nothing (the timestamp follows the kind's last barrier).
enum class FlushKind
{
    Full,   // upload, 256 MB read+write flush: shader code and data both cold (evicted from L2)
    FullRo, // diagnostic: upload, 256 MB *read-only* flush (leaves no dirty lines)
    Code,   // flush, upload: code cold, input freshly written (warm in L2, like keys produced by a
            // previous pass)
    Data,   // upload, flush, an untimed run of the same dispatches on a private copy of the iteration:
            // code warm, data cold
    None,   // upload (no flush): code and data warm
};

enum class DrainKind
{
    None,
    Group, // pass5 drain
    Spin,  // ALU spin of drainUs microseconds
};

struct FlushMode
{
    FlushKind kind = FlushKind::Full;
    DrainKind drain = DrainKind::Group;
    uint32_t drainUs = 0; // DrainKind::Spin only (1..kMaxDrainUs)

    bool operator==(const FlushMode&) const = default;
};

constexpr uint32_t kMaxDrainUs = 1000;

// Names (algorithms.txt 'flush' lines, --flush-mode, the CSV column flush_mode):
//   <kind>[_dg|_d<N>] with kind full, full_ro, code, data or none; _dg = the pass5 group drain,
//   _d<N> = a spin drain of N us (1..kMaxDrainUs), _d0 = no drain. Without a suffix: the pass5 mode
//   of that name, i.e. the group drain for full / code / data / none and no drain for full_ro.
//   full_legacy = full_d0 (the 'full' of pass0-pass4: no drain).
// FlushModeName returns the canonical name: the plain pass5 name where one exists (full,
// full_legacy, full_ro, code, data, none), else the kind with its drain suffix (full_d20, ...).
std::string FlushModeName(const FlushMode& mode);
bool ParseFlushMode(const std::string& name, FlushMode& mode);

// The default flush mode (for algorithms without a 'flush' line, unless --flush-mode): the
// measurement of record. Since the final set it is full_d50 (256 MB flush, then a ~50 us spin
// drain): on the RX 7900 XTX the post-flush penalty of the pass5 one-group drain ('full', ~12 us on
// mostly_empty) only decays with a longer wait (full_d5 11.5, d10 8.9, d20 3.8, d50 / d100 1.76 vs
// @data 1.68 us; _test/pass7/notes.md "AMD results"); the RTX 5080 and the Ryzen iGPU are flat from
// d5 / d0. RESULTS OF EARLIER PACKAGES (up to 627724b) USE 'full' AS THE DEFAULT and are not
// directly comparable on the small workloads; 'full' remains available as a flush mode.
constexpr FlushMode kDefaultFlushMode = {FlushKind::Full, DrainKind::Spin, 50};

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
    FlushMode flush = kDefaultFlushMode;
    int pass = -1;                       // 'pass' line: the pass that introduced it (-1 = not given)
    std::string description;             // 'desc' line
    std::vector<std::string> tags;       // 'tag' lines
};

// Algorithms are registered by the shader directory itself, in <shaderDir>/algorithms.txt:
//
//   # comment
//   algorithm <name>
//   dispatch <file.hlsl> <entryPoint> <groupSize> [NAME=VALUE ...]
//   flush <mode>      (optional: overrides --flush-mode for this algorithm; see FlushModeName)
//   pass <N>          (optional, 0..99: the pass that introduced the algorithm; CSV column pass)
//   desc <text>       (optional: a one-line description, the rest of the line; CSV column description)
//   tag <word> [...]  (optional, repeatable: tags such as rec_discrete; CSV column tags)
//
// so every _test/passN snapshot carries its own algorithm list. 'file' (--algo-file) selects another
// list in the shader directory (a relative path is relative to shaderDir), e.g. a diagnostic set
// that uses the same shaders. Throws on parse errors.
std::vector<AlgorithmDesc> LoadAlgorithms(const std::filesystem::path& shaderDir,
                                          const std::filesystem::path& file = "algorithms.txt");
