// Single-dispatch sort: one 1024-thread group per sort handles every size (1..8192) and picks the
// algorithm per sort with a group-uniform branch on count:
//   count <= RANK_MAX   : rank sort, one element per thread (rank_sort1.hlsli); RANK_MAX = 0: never
//   count <= BITREG_MAX : register/wave bitonic (bitonic_reg.hlsli); BITREG_MAX = 0: never
//   otherwise           : LARGE_RADIX = 0: register/wave bitonic, 1: LDS radix sort (radix_sort.hlsli)
// All paths share the one 32 KB groupshared array (sort_lds.hlsli).
#include "common.hlsli"
#include "rank_sort1.hlsli"

#ifndef RANK_MAX
#define RANK_MAX 512
#endif
#ifndef BITREG_MAX
#define BITREG_MAX 0
#endif
#ifndef LARGE_RADIX
#define LARGE_RADIX 0
#endif

#if RANK_MAX > GROUP_SIZE
#error "single_pass: RANK_MAX must be <= GROUP_SIZE (one element per thread)"
#endif

#if LARGE_RADIX
#include "radix_sort.hlsli"
#endif
#if !LARGE_RADIX || BITREG_MAX > 0
#include "bitonic_reg.hlsli"
#endif

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
    if (count == 0)
        return;

#if RANK_MAX > 0
    if (count <= RANK_MAX)
    {
        RankSort1(tid, offset, count);
        return;
    }
#endif
#if BITREG_MAX > 0 && LARGE_RADIX
    if (count <= BITREG_MAX)
    {
        BitonicRegSort(tid, offset, count);
        return;
    }
#endif
#if LARGE_RADIX
    RadixSort(tid, offset, count);
#else
    BitonicRegSort(tid, offset, count);
#endif
}
