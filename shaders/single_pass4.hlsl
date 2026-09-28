// Single-dispatch sort, pass4: one 1024-thread group per sort handles every size (1..8192) and picks
// the algorithm per sort with group-uniform branches on count:
//   count <= RANK_MAX        : rank sort, one element per thread (rank_sort1.hlsli); 0 = never
//   count <= BITREG_MAX      : register/wave bitonic, 8 elements per thread (bitonic_reg_t.hlsli);
//                              0 = never
//   count <= MID_RADIX_MAX   : sort kind MID_RADIX; 0 = never
//   otherwise                : sort kind LARGE_RADIX
// Sort kinds: 0 = register/wave bitonic E = 8, 1 = pass2 radix (radix_sort.hlsli), 2 = pass3 radix
// (radix_sort2.hlsli), 3 = pass4 radix (radix_sort3.hlsli).
// All paths share the one 32 KB groupshared array (sort_lds.hlsli).
// UB_OP (diagnostic only, see ubench.hlsli): every group first runs a microbenchmark prelude.
#include "common.hlsli"
#include "rank_sort1.hlsli"
#include "bitonic_reg_t.hlsli"

#ifndef RANK_MAX
#define RANK_MAX 512
#endif
#ifndef BITREG_MAX
#define BITREG_MAX 2048
#endif
#ifndef MID_RADIX_MAX
#define MID_RADIX_MAX 0
#endif
#ifndef MID_RADIX
#define MID_RADIX 2
#endif
#ifndef LARGE_RADIX
#define LARGE_RADIX 3
#endif

#if RANK_MAX > GROUP_SIZE
#error "single_pass4: RANK_MAX must be <= GROUP_SIZE (one element per thread)"
#endif
#if BITREG_MAX > 8 * GROUP_SIZE
#error "single_pass4: BITREG_MAX must be <= 8 * GROUP_SIZE"
#endif
#if MID_RADIX < 0 || MID_RADIX > 3 || LARGE_RADIX < 0 || LARGE_RADIX > 3
#error "single_pass4: MID_RADIX / LARGE_RADIX must be 0..3"
#endif

#if (MID_RADIX_MAX > 0 && MID_RADIX == 1) || LARGE_RADIX == 1
#include "radix_sort.hlsli"
#endif
#if (MID_RADIX_MAX > 0 && MID_RADIX == 2) || LARGE_RADIX == 2
#include "radix_sort2.hlsli"
#endif
#if (MID_RADIX_MAX > 0 && MID_RADIX == 3) || LARGE_RADIX == 3
#include "radix_sort3.hlsli"
#endif

#ifdef UB_OP
#include "ubench.hlsli"
#endif

#define SP4_SORT_0(tid, offset, count) BitonicRegSortT<8, 3>(tid, offset, count)
#define SP4_SORT_1(tid, offset, count) RadixSort(tid, offset, count)
#define SP4_SORT_2(tid, offset, count) RadixSort2(tid, offset, count)
#define SP4_SORT_3(tid, offset, count) RadixSort3(tid, offset, count)
#define SP4_SORT_I(kind, tid, offset, count) SP4_SORT_##kind(tid, offset, count)
#define SP4_SORT(kind, tid, offset, count) SP4_SORT_I(kind, tid, offset, count)

[numthreads(GROUP_SIZE, 1, 1)]
WAVE_SIZE_ATTR
void main(uint3 groupId : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint sortIndex = groupId.x;
    if (sortIndex >= gNumSorts)
        return;

#ifdef UB_OP
    UbPrelude(tid, sortIndex);
#endif

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
#if BITREG_MAX > 0
    if (count <= BITREG_MAX)
    {
        BitonicRegSortT<8, 3>(tid, offset, count);
        return;
    }
#endif
#if MID_RADIX_MAX > 0
    if (count <= MID_RADIX_MAX)
    {
        SP4_SORT(MID_RADIX, tid, offset, count);
        return;
    }
#endif
    SP4_SORT(LARGE_RADIX, tid, offset, count);
}
