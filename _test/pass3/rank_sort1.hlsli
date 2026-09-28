// Rank sort with one element per thread, for count <= GROUP_SIZE, as a function so it can share a
// group (and the groupshared array) with a large-sort path (single_pass.hlsl). Same algorithm as
// rank_sort.hlsl with ELEMS_PER_THREAD = 1: unique words (key16 << 16) | index in groupshared
// memory, rank = number of smaller words, broadcast reads 8 words per step, one barrier. Stable.
#ifndef GPUSORT_RANK_SORT1_HLSLI
#define GPUSORT_RANK_SORT1_HLSLI

#include "common.hlsli"
#include "sort_lds.hlsli"

#if GROUP_SIZE % 8 != 0
#error "rank_sort1: GROUP_SIZE must be a multiple of 8"
#endif

// count must be 1..GROUP_SIZE and group-uniform. Must be called by all threads of the group.
void RankSort1(uint tid, uint offset, uint count)
{
    // Pad to a multiple of 8 words with 0xFFFFFFFF (> any real word, max 0xFFFF1FFF).
    const uint paddedCount = (count + 7u) & ~7u; // <= GROUP_SIZE
    uint value = 0;
    uint word = 0xFFFFFFFFu;
    if (tid < count)
    {
        value = gInput[offset + tid];
        word = (value & 0xFFFF0000u) | tid;
        gsLds[tid] = word;
    }
    else if (tid < paddedCount)
    {
        gsLds[tid] = 0xFFFFFFFFu;
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid < count)
    {
        uint4 acc = 0;
        const uint numSteps = paddedCount / 8u; // <= GROUP_SIZE / 8
        for (uint s = 0; s < numSteps; ++s)
        {
            const uint i = 8u * s;
            const uint4 a = uint4(gsLds[i + 0], gsLds[i + 1], gsLds[i + 2], gsLds[i + 3]);
            const uint4 b = uint4(gsLds[i + 4], gsLds[i + 5], gsLds[i + 6], gsLds[i + 7]);
            acc += (uint4)(a < word);
            acc += (uint4)(b < word);
        }
        gOutput[offset + (acc.x + acc.y + acc.z + acc.w)] = value;
    }
}

#endif
