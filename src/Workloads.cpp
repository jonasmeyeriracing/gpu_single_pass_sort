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
};

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

void GenerateSizes(uint32_t workloadId, uint32_t iteration, uint32_t sizes[kSortsPerIteration])
{
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
