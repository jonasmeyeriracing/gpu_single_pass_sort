// Single-dispatch sort, pass3: one 1024-thread group per sort handles every size (1..8192) and picks
// the algorithm per sort with a group-uniform branch on count:
//   count <= RANK_MAX   : rank sort, one element per thread (rank_sort1.hlsli); 0 = never
//   count <= MID_MAX    : register/wave bitonic with MID_E (1, 2, 4, 8) elements per thread
//                         (bitonic_reg_t.hlsli); 0 = never. MID_MAX <= MID_E * 1024.
//   count <= BITREG_MAX : register/wave bitonic, 8 elements per thread; 0 = never
//   otherwise           : RADIX = 2: radix_sort2.hlsli (pass3), 1: radix_sort.hlsli (pass2),
//                         0: register/wave bitonic, 8 elements per thread
// All paths share the one 32 KB groupshared array (sort_lds.hlsli).
// UB_OP (diagnostic only, see ubench.hlsli): every group first runs a microbenchmark prelude.
#include "common.hlsli"
#include "rank_sort1.hlsli"
#include "bitonic_reg_t.hlsli"

#ifndef RANK_MAX
#define RANK_MAX 512
#endif
#ifndef MID_MAX
#define MID_MAX 0
#endif
#ifndef MID_E
#define MID_E 2
#endif
#ifndef BITREG_MAX
#define BITREG_MAX 0
#endif
#ifndef RADIX
#define RADIX 2
#endif

#if RANK_MAX > GROUP_SIZE
#error "single_pass3: RANK_MAX must be <= GROUP_SIZE (one element per thread)"
#endif
#if MID_E == 1
#define MID_EB 0
#elif MID_E == 2
#define MID_EB 1
#elif MID_E == 4
#define MID_EB 2
#elif MID_E == 8
#define MID_EB 3
#else
#error "single_pass3: MID_E must be 1, 2, 4 or 8"
#endif
#if MID_MAX > MID_E * GROUP_SIZE
#error "single_pass3: MID_MAX must be <= MID_E * GROUP_SIZE"
#endif

#if RADIX == 2
#include "radix_sort2.hlsli"
#elif RADIX == 1
#include "radix_sort.hlsli"
#elif RADIX != 0
#error "single_pass3: RADIX must be 0, 1 or 2"
#endif

#ifdef UB_OP
#include "ubench.hlsli"
#endif

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
#if MID_MAX > 0
    if (count <= MID_MAX)
    {
        BitonicRegSortT<MID_E, MID_EB>(tid, offset, count);
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
#if RADIX == 2
    RadixSort2(tid, offset, count);
#elif RADIX == 1
    RadixSort(tid, offset, count);
#else
    BitonicRegSortT<8, 3>(tid, offset, count);
#endif
}
