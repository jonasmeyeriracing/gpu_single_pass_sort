// pass7: tiered sort for low-end / integrated GPUs. One group per sort (SV_GroupID.x = sort index),
// like single_pass.hlsl, but the group size, the groupshared size and the tiers are per dispatch,
// so an algorithm can be one dispatch (all tiers) or several dispatches issued back to back without
// barriers, each handling one size range (MIN_COUNT..MAX_COUNT, common.hlsli; other sorts early-out)
// in a group that fits its work and with only the groupshared memory it needs.
//
// Tiers inside a dispatch (group-uniform branches on count, first match wins):
//   count <= P7_RANK_MAX  : rank sort, P7_RANK_EPT elements per thread (p7_rank.hlsli); 0 = never
//   count <= P7_BIT_MAX   : register/wave bitonic, P7_BIT_E elements per thread (p7_bitonic.hlsli)
//   count <= P7_BIT2_MAX  : register/wave bitonic, P7_BIT2_E elements per thread
//   otherwise             : P7_LARGE = 0: nothing (the tiers above must cover MAX_COUNT),
//                           1: bitonic with P7_LARGE_E elements per thread, 2: radix (p7_radix.hlsli)
// Groupshared: P7_LDS_WORDS = the next power of two >= max(MAX_COUNT, E * WAVE_SIZE of every
// bitonic tier, 64), unless given explicitly (to test the effect of the groupshared size alone).
#include "common.hlsli"

#ifndef P7_RANK_MAX
#define P7_RANK_MAX 0
#endif
#ifndef P7_RANK_EPT
#define P7_RANK_EPT 1
#endif
#ifndef P7_BIT_MAX
#define P7_BIT_MAX 0
#endif
#ifndef P7_BIT_E
#define P7_BIT_E 8
#endif
#ifndef P7_BIT2_MAX
#define P7_BIT2_MAX 0
#endif
#ifndef P7_BIT2_E
#define P7_BIT2_E 8
#endif
#ifndef P7_LARGE
#define P7_LARGE 0
#endif
#ifndef P7_LARGE_E
#define P7_LARGE_E 8
#endif

#if MAX_COUNT > MAX_SORT_SIZE || MIN_COUNT < 1 || MIN_COUNT > MAX_COUNT
#error "p7_sort: need 1 <= MIN_COUNT <= MAX_COUNT <= 8192"
#endif
#if P7_LARGE < 0 || P7_LARGE > 2
#error "p7_sort: P7_LARGE must be 0, 1 or 2"
#endif
#if P7_LARGE == 0 && P7_RANK_MAX < MAX_COUNT && P7_BIT_MAX < MAX_COUNT && P7_BIT2_MAX < MAX_COUNT
#error "p7_sort: without P7_LARGE the rank / bitonic tiers must cover MAX_COUNT"
#endif
#if P7_RANK_MAX > P7_RANK_EPT * GROUP_SIZE
#error "p7_sort: P7_RANK_MAX must be <= P7_RANK_EPT * GROUP_SIZE"
#endif
#if P7_BIT_MAX > P7_BIT_E * GROUP_SIZE || P7_BIT2_MAX > P7_BIT2_E * GROUP_SIZE
#error "p7_sort: a bitonic tier's max must be <= E * GROUP_SIZE"
#endif
#if P7_LARGE == 1 && MAX_COUNT > P7_LARGE_E * GROUP_SIZE
#error "p7_sort: bitonic large tier: MAX_COUNT must be <= P7_LARGE_E * GROUP_SIZE"
#endif

// Groupshared words needed: MAX_COUNT, and a whole hardware wave of every bitonic tier.
#define P7_NEED_0 MAX_COUNT
#if P7_BIT_MAX > 0 && P7_BIT_E * WAVE_SIZE > P7_NEED_0
#define P7_NEED_1 (P7_BIT_E * WAVE_SIZE)
#else
#define P7_NEED_1 P7_NEED_0
#endif
#if P7_BIT2_MAX > 0 && P7_BIT2_E * WAVE_SIZE > P7_NEED_1
#define P7_NEED_2 (P7_BIT2_E * WAVE_SIZE)
#else
#define P7_NEED_2 P7_NEED_1
#endif
#if P7_LARGE == 1 && P7_LARGE_E * WAVE_SIZE > P7_NEED_2
#define P7_NEED (P7_LARGE_E * WAVE_SIZE)
#else
#define P7_NEED P7_NEED_2
#endif
#ifndef P7_LDS_WORDS
#if P7_NEED <= 64
#define P7_LDS_WORDS 64
#elif P7_NEED <= 128
#define P7_LDS_WORDS 128
#elif P7_NEED <= 256
#define P7_LDS_WORDS 256
#elif P7_NEED <= 512
#define P7_LDS_WORDS 512
#elif P7_NEED <= 1024
#define P7_LDS_WORDS 1024
#elif P7_NEED <= 2048
#define P7_LDS_WORDS 2048
#elif P7_NEED <= 4096
#define P7_LDS_WORDS 4096
#elif P7_NEED <= 8192
#define P7_LDS_WORDS 8192
#else
#error "p7_sort: more than 8192 groupshared words needed"
#endif
#elif P7_LDS_WORDS < P7_NEED
#error "p7_sort: P7_LDS_WORDS is too small for this configuration"
#endif

#include "p7_lds.hlsli"

#if P7_RANK_MAX > 0
#include "p7_rank.hlsli"
#endif
#if P7_BIT_MAX > 0 || P7_BIT2_MAX > 0 || P7_LARGE == 1
#include "p7_bitonic.hlsli"
#endif
#if P7_LARGE == 2
#include "p7_radix.hlsli"
#endif

// log2 of the bitonic elements per thread.
#define P7_EB_1 0
#define P7_EB_2 1
#define P7_EB_4 2
#define P7_EB_8 3
#define P7_EB_16 4
#define P7_EB_32 5
#define P7_EB_I(e) P7_EB_##e
#define P7_EB(e) P7_EB_I(e)

[numthreads(GROUP_SIZE, 1, 1)]
WAVE_SIZE_ATTR
void main(uint3 groupId : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint sortIndex = groupId.x;
    if (sortIndex >= gNumSorts)
        return;

    const uint2 desc = gSortDescs[sortIndex];
    const uint offset = desc.x;
    const uint count = min(desc.y, (uint)MAX_SORT_SIZE);
    if (count < MIN_COUNT || count > MAX_COUNT) // MIN_COUNT >= 1: also skips empty sorts
        return;

#if P7_RANK_MAX > 0
    if (count <= P7_RANK_MAX)
    {
        P7RankSort(tid, offset, count);
        return;
    }
#endif
#if P7_BIT_MAX > 0
    if (count <= P7_BIT_MAX)
    {
        P7BitonicSort<P7_BIT_E, P7_EB(P7_BIT_E)>(tid, offset, count);
        return;
    }
#endif
#if P7_BIT2_MAX > 0
    if (count <= P7_BIT2_MAX)
    {
        P7BitonicSort<P7_BIT2_E, P7_EB(P7_BIT2_E)>(tid, offset, count);
        return;
    }
#endif
#if P7_LARGE == 1
    P7BitonicSort<P7_LARGE_E, P7_EB(P7_LARGE_E)>(tid, offset, count);
#elif P7_LARGE == 2
    P7RadixSort(tid, offset, count);
#endif
}
