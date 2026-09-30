#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Sorts per iteration (one batch = one ExecuteIndirect per dispatch with {numSorts, 1, 1} groups): a
// run parameter (--sorts); 20 is the default and the average case every result up to the scale run
// used. kMaxSortsPerIteration bounds it (buffer sizes, the batching of iterations per command list).
constexpr uint32_t kDefaultSortsPerIteration = 20;
constexpr uint32_t kMaxSortsPerIteration = 512;
constexpr uint32_t kMaxSortSize = 8192;
constexpr uint32_t kOffsetAlignment = 64; // elements; each sort starts at a multiple of this

// Element capacity of an iteration of 'numSorts' sorts (8192 is 64-aligned, so every sort fits).
constexpr uint32_t MaxElementsPerIteration(uint32_t numSorts)
{
    return numSorts * kMaxSortSize;
}

// Warmup iterations use their own seed space so they never alias measured iterations.
constexpr uint32_t kWarmupIterationBase = 0x80000000u;

// PCG32 (pcg32_random_r from pcg-random.org). Fully specified, so identical on every machine/compiler.
struct Pcg32
{
    uint64_t state = 0;
    uint64_t inc = 1;

    Pcg32(uint64_t seed, uint64_t sequence)
    {
        inc = (sequence << 1u) | 1u;
        Next();
        state += seed;
        Next();
    }

    uint32_t Next()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    // Unbiased value in [0, bound).
    uint32_t Bounded(uint32_t bound)
    {
        const uint32_t threshold = (0u - bound) % bound;
        for (;;)
        {
            const uint32_t r = Next();
            if (r >= threshold)
                return r % bound;
        }
    }

    // Value in [lo, hi] (inclusive).
    uint32_t Range(uint32_t lo, uint32_t hi) { return lo + Bounded(hi - lo + 1); }
};

struct SortDesc
{
    uint32_t offset; // in elements, multiple of kOffsetAlignment
    uint32_t count;
};

struct IterationData
{
    std::vector<SortDesc> sorts;    // numSorts entries
    std::vector<uint32_t> elements; // packed input; size = end of the last sort rounded up to kOffsetAlignment
};

struct WorkloadDesc
{
    const char* name;
    const char* description;
};

// Fixed list; the index is the workload id used for seeding (never reorder, only append).
const std::vector<WorkloadDesc>& Workloads();

// Returns -1 if unknown.
int FindWorkload(const std::string& name);

// Workload "sweep": all sorts of an iteration have the same size, SweepSizes()[iteration % n], so the
// per-iteration time is the latency of one sort of that size (numSorts in parallel). Results are
// grouped by size (measured iteration i has size SweepSizes()[i % n]).
const std::vector<uint32_t>& SweepSizes();
bool IsSweepWorkload(uint32_t workloadId);

// Sort sizes for one iteration of numSorts sorts (only the size RNG stream; cheap). The seed depends
// on (workload, iteration) only and the sizes are drawn in sort order, so the first k sizes are the
// same for every numSorts >= k: the 20-sort iterations are exactly those of the fixed-20 builds, and
// an iteration of N sorts draws N sizes from the same distribution.
void GenerateSizes(uint32_t workloadId, uint32_t iteration, uint32_t numSorts, uint32_t* sizes);

// Full iteration of numSorts sorts: sizes, packed offsets and random elements ((key16 << 16) |
// payload16). Deterministic per (workload, iteration, numSorts); for numSorts = 20 bit-identical to
// the fixed-20 builds.
void GenerateIteration(uint32_t workloadId, uint32_t iteration, uint32_t numSorts, IterationData& out);

// Size tiers used for the distribution summary.
constexpr uint32_t kNumSizeTiers = 6;
const char* SizeTierName(uint32_t tier);
uint32_t SizeTier(uint32_t count);
