#include "Workloads.h"

namespace
{
enum WorkloadId : uint32_t
{
    kMostlyEmpty = 0,
    kMostlySmall = 1,
    kRealisticMix = 2,
    kMostlyLarge = 3,
    kWorstCase = 4,
    kEdges = 5,
    kMostlyMid = 6,
    kMostlyMedium = 7,
    kSweep = 8,
    kSparseKeys = 9,
};

// Deterministic sizes around every size-tier boundary the shaders use (powers of two and their
// neighbours), cycled through in order so a few iterations cover all of them.
constexpr uint32_t kEdgeSizes[] = {
    0,   1,   2,   3,   31,  32,   33,   63,   64,   65,   127,  128,  129,  255,  256,
    257, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097, 8191, 8192,
};
constexpr uint32_t kNumEdgeSizes = static_cast<uint32_t>(sizeof(kEdgeSizes) / sizeof(kEdgeSizes[0]));

// Sizes of the sweep workload, one per iteration (cycled).
constexpr uint32_t kSweepSizes[] = {32,   64,   128,  256,  384,  512,  768,  1024,
                                    1536, 2048, 2560, 3072, 4096, 5120, 6144, 8192};
constexpr uint32_t kNumSweepSizes = static_cast<uint32_t>(sizeof(kSweepSizes) / sizeof(kSweepSizes[0]));

uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t IterationSeed(uint32_t workloadId, uint32_t iteration)
{
    return SplitMix64((static_cast<uint64_t>(workloadId + 1) << 32) | iteration);
}

uint32_t DrawSize(uint32_t workloadId, Pcg32& rng)
{
    switch (workloadId)
    {
    case kMostlyEmpty:
        return rng.Bounded(100) < 70 ? 0 : rng.Range(1, 64);
    case kMostlySmall:
        return rng.Range(0, 128);
    case kRealisticMix:
    {
        const uint32_t p = rng.Bounded(100);
        if (p < 15)
            return 0;
        if (p < 87)
            return rng.Range(16, 512);
        if (p < 97)
            return rng.Range(513, 2048);
        return rng.Range(2049, 8192);
    }
    case kMostlyLarge:
        return rng.Range(2048, 8192);
    case kMostlyMid:
        return rng.Range(513, 2048);
    case kSparseKeys:
        return rng.Range(1, kMaxSortSize);
    case kMostlyMedium:
        return rng.Range(129, 512);
    case kWorstCase:
    default:
        return kMaxSortSize;
    }
}
} // namespace

const std::vector<WorkloadDesc>& Workloads()
{
    static const std::vector<WorkloadDesc> list = {
        {"mostly_empty", "70% empty, rest 1-64"},
        {"mostly_small", "uniform 0-128"},
        {"realistic_mix", "15% empty, 72% 16-512, 10% 513-2048, 3% 2049-8192"},
        {"mostly_large", "uniform 2048-8192"},
        {"worst_case", "all 8192"},
        {"edges", "tier boundaries 0-8192 (30 fixed sizes, cycled)"},
        {"mostly_mid", "uniform 513-2048"},
        {"mostly_medium", "uniform 129-512"},
        {"sweep", "all 20 sorts the same size, 16 sizes 32-8192 cycled per iteration"},
        {"sparse_keys", "uniform 1-8192; each 4-bit key digit is constant within a sort with p = 1/2"},
    };
    return list;
}

int FindWorkload(const std::string& name)
{
    const auto& list = Workloads();
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (name == list[i].name)
            return static_cast<int>(i);
    }
    return -1;
}

const std::vector<uint32_t>& SweepSizes()
{
    static const std::vector<uint32_t> list(kSweepSizes, kSweepSizes + kNumSweepSizes);
    return list;
}

bool IsSweepWorkload(uint32_t workloadId)
{
    return workloadId == kSweep;
}

void GenerateSizes(uint32_t workloadId, uint32_t iteration, uint32_t sizes[kSortsPerIteration])
{
    if (workloadId == kSweep)
    {
        for (uint32_t i = 0; i < kSortsPerIteration; ++i)
            sizes[i] = kSweepSizes[iteration % kNumSweepSizes];
        return;
    }
    if (workloadId == kEdges)
    {
        for (uint32_t i = 0; i < kSortsPerIteration; ++i)
            sizes[i] = kEdgeSizes[(uint64_t(iteration) * kSortsPerIteration + i) % kNumEdgeSizes];
        return;
    }
    Pcg32 rng(IterationSeed(workloadId, iteration), 1);
    for (uint32_t i = 0; i < kSortsPerIteration; ++i)
        sizes[i] = DrawSize(workloadId, rng);
}

void GenerateIteration(uint32_t workloadId, uint32_t iteration, IterationData& out)
{
    uint32_t sizes[kSortsPerIteration];
    GenerateSizes(workloadId, iteration, sizes);

    uint32_t cursor = 0;
    for (uint32_t i = 0; i < kSortsPerIteration; ++i)
    {
        out.sorts[i].offset = cursor;
        out.sorts[i].count = sizes[i];
        cursor += (sizes[i] + kOffsetAlignment - 1) / kOffsetAlignment * kOffsetAlignment;
    }

    out.elements.assign(cursor, 0u);
    Pcg32 rng(IterationSeed(workloadId, iteration), 2);
    for (uint32_t i = 0; i < kSortsPerIteration; ++i)
    {
        uint32_t* dst = out.elements.data() + out.sorts[i].offset;
        for (uint32_t j = 0; j < out.sorts[i].count; ++j)
            dst[j] = rng.Next(); // random key16 in the high half, random payload16 in the low half
        if (workloadId == kSparseKeys)
        {
            // Real sort keys often have constant fields: per sort, each 4-bit digit of the key is
            // either random or one constant value (p = 1/2 each).
            Pcg32 keyRng(IterationSeed(workloadId, iteration), 3 + i);
            const uint32_t digitMask = keyRng.Next() & 15u;
            uint32_t varyBits = 0;
            for (uint32_t d = 0; d < 4; ++d)
            {
                if (digitMask & (1u << d))
                    varyBits |= 0xFu << (16 + 4 * d);
            }
            const uint32_t constBits = keyRng.Next() & 0xFFFF0000u & ~varyBits;
            for (uint32_t j = 0; j < out.sorts[i].count; ++j)
                dst[j] = (dst[j] & (varyBits | 0xFFFFu)) | constBits;
        }
    }
}

const char* SizeTierName(uint32_t tier)
{
    static const char* names[kNumSizeTiers] = {"0", "1-64", "65-128", "129-512", "513-2048", "2049-8192"};
    return tier < kNumSizeTiers ? names[tier] : "?";
}

uint32_t SizeTier(uint32_t count)
{
    if (count == 0)
        return 0;
    if (count <= 64)
        return 1;
    if (count <= 128)
        return 2;
    if (count <= 512)
        return 3;
    if (count <= 2048)
        return 4;
    return 5;
}
