// pass7: rank sort with P7_RANK_EPT (1, 2 or 4) elements per thread, for count <= P7_RANK_EPT *
// GROUP_SIZE (and <= P7_LDS_WORDS). The algorithm of rank_sort1.hlsli (pass2): unique words
// (key16 << 16) | index in groupshared memory, rank = number of smaller words, broadcast reads of
// 8 words per step, one barrier, stable. Thread t holds the elements t + e * GROUP_SIZE (coalesced
// loads). With P7_RANK_EPT = 2 every groupshared word read serves two compares (half the LDS reads
// of 1 element per thread) and a 512-element sort fits a 256-thread group.
#ifndef GPUSORT_P7_RANK_HLSLI
#define GPUSORT_P7_RANK_HLSLI

#include "common.hlsli"
#include "p7_lds.hlsli"

#ifndef P7_RANK_EPT
#define P7_RANK_EPT 1
#endif
#if P7_RANK_EPT != 1 && P7_RANK_EPT != 2 && P7_RANK_EPT != 4
#error "p7_rank: P7_RANK_EPT must be 1, 2 or 4"
#endif
#if GROUP_SIZE % 8 != 0
#error "p7_rank: GROUP_SIZE must be a multiple of 8"
#endif

// count must be 1..min(P7_RANK_EPT * GROUP_SIZE, P7_LDS_WORDS) and group-uniform. Must be called by
// all threads of the group (one barrier).
void P7RankSort(uint tid, uint offset, uint count)
{
    // Pad to a multiple of 8 words with 0xFFFFFFFF (> any real word, max 0xFFFF1FFF). paddedCount
    // <= P7_LDS_WORDS (a multiple of 64) and <= P7_RANK_EPT * GROUP_SIZE (a multiple of 8).
    const uint paddedCount = (count + 7u) & ~7u;
    uint value[P7_RANK_EPT];
    uint word[P7_RANK_EPT];
    [unroll]
    for (uint e = 0; e < P7_RANK_EPT; ++e)
    {
        const uint i = tid + e * GROUP_SIZE;
        value[e] = 0;
        word[e] = 0xFFFFFFFFu;
        if (i < count)
        {
            value[e] = gInput[offset + i];
            word[e] = (value[e] & 0xFFFF0000u) | i;
            gsP7[i & P7_LDS_MASK] = word[e];
        }
        else if (i < paddedCount)
        {
            gsP7[i & P7_LDS_MASK] = 0xFFFFFFFFu;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid < count) // element 0 valid; element e > 0 only if tid + e * GROUP_SIZE < count
    {
        uint4 acc[P7_RANK_EPT];
        [unroll]
        for (uint e = 0; e < P7_RANK_EPT; ++e)
            acc[e] = 0;
        const uint numSteps = paddedCount / 8u; // <= P7_LDS_WORDS / 8
        for (uint s = 0; s < numSteps; ++s)
        {
            const uint i = 8u * s;
            const uint4 a = uint4(gsP7[i + 0], gsP7[i + 1], gsP7[i + 2], gsP7[i + 3]);
            const uint4 b = uint4(gsP7[i + 4], gsP7[i + 5], gsP7[i + 6], gsP7[i + 7]);
            [unroll]
            for (uint e = 0; e < P7_RANK_EPT; ++e)
            {
                acc[e] += (uint4)(a < word[e]);
                acc[e] += (uint4)(b < word[e]);
            }
        }
        [unroll]
        for (uint e = 0; e < P7_RANK_EPT; ++e)
        {
            if (tid + e * GROUP_SIZE < count)
                gOutput[offset + (acc[e].x + acc[e].y + acc[e].z + acc[e].w)] = value[e];
        }
    }
}

#endif
