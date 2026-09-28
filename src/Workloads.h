#pragma once

#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t kSortsPerIteration = 20;
constexpr uint32_t kMaxSortSize = 8192;
constexpr uint32_t kOffsetAlignment = 64; // elements; each sort starts at a multiple of this
constexpr uint32_t kMaxElementsPerIteration = kSortsPerIteration * kMaxSortSize; // 8192 is 64-aligned

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
    SortDesc sorts[kSortsPerIteration];
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

// Sort sizes for one iteration (only the size RNG stream; cheap).
void GenerateSizes(uint32_t workloadId, uint32_t iteration, uint32_t sizes[kSortsPerIteration]);

// Full iteration: sizes, packed offsets and random elements ((key16 << 16) | payload16).
void GenerateIteration(uint32_t workloadId, uint32_t iteration, IterationData& out);

// Size tiers used for the distribution summary.
constexpr uint32_t kNumSizeTiers = 6;
const char* SizeTierName(uint32_t tier);
uint32_t SizeTier(uint32_t count);
