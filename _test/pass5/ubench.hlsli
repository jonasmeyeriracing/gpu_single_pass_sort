// Microbenchmark prelude (pass3 diagnostic run): every thread of every group runs UB_REPS dependent
// repetitions of one operation before it sorts (single_pass3.hlsl with UB_OP = 1..10). The result
// is only written if gNumSorts has an impossible value, so it cannot be optimized away and never
// changes the output. Compared with the same algorithm without the prelude (ub_none) on the same
// workload, (delta / UB_REPS) is the cost of one operation issued by all waves of a 1024-thread group.
//
//   UB_OP  1: ALU (x = x * a + c)              2: WaveReadLaneAt, per-lane source (lane + 1)
//          3: WaveReadLaneAt, uniform source   4: WaveActiveBallot + countbits
//          5: WavePrefixSum                    6: WaveActiveSum
//          7: WaveActiveBitOr                  8: WavePrefixCountBits
//          9: WaveMatch + countbits           10: GroupMemoryBarrierWithGroupSync (+ 1 LDS store / load)
//
// pass4, code size / instruction cache: the same dependent chain x = (x ^ (x >> 7)) * K(i), with an
//         11: fully unrolled loop: UB_REPS copies with distinct immediates K(i) (straight-line code,
//             about 3 instructions = ~48 bytes of GPU code per repetition)
//         12: [loop] (a few instructions of code, K(i) computed at runtime)
// Run with flush modes full / data (code cold / warm), the difference between 11 and 12 is the cost
// of fetching the unrolled code.
#ifndef GPUSORT_UBENCH_HLSLI
#define GPUSORT_UBENCH_HLSLI

#include "common.hlsli"
#include "sort_lds.hlsli"

#ifndef UB_REPS
#define UB_REPS 256
#endif
#if GROUP_SIZE * 2 > MAX_SORT_SIZE
#error "ubench: needs 2 * GROUP_SIZE groupshared words"
#endif

// Must be called by all threads of the group (UB_OP 10 contains barriers).
void UbPrelude(uint tid, uint sortIndex)
{
    const uint lane = tid & (WAVE_SIZE - 1u);
    uint x = tid * 0x9E3779B9u + sortIndex;
#if UB_OP == 11
    [unroll]
#else
    [loop]
#endif
    for (uint i = 0; i < UB_REPS; ++i)
    {
#if UB_OP == 11 || UB_OP == 12
        x = (x ^ (x >> 7)) * ((i * 0x9E3779B9u) | 1u);
#elif UB_OP == 1
        x = x * 1664525u + 1013904223u;
#elif UB_OP == 2
        x = WaveReadLaneAt(x, (lane + 1u) & (WAVE_SIZE - 1u)) + i;
#elif UB_OP == 3
        x = WaveReadLaneAt(x, i & (WAVE_SIZE - 1u)) + lane;
#elif UB_OP == 4
        x = x + countbits(WaveActiveBallot((x & 1u) != 0).x) + 1u;
#elif UB_OP == 5
        x = WavePrefixSum(x) + lane;
#elif UB_OP == 6
        x = WaveActiveSum(x) + lane;
#elif UB_OP == 7
        x = (WaveActiveBitOr(x) ^ i) + lane;
#elif UB_OP == 8
        x = x + WavePrefixCountBits((x & 1u) != 0) + 1u;
#elif UB_OP == 9
        x = x + countbits(WaveMatch(x & 7u).x) + 1u;
#elif UB_OP == 10
        // Two halves alternate, so an iteration's store never races with the previous iteration's load.
        const uint half = (i & 1u) * GROUP_SIZE;
        gsLds[half + tid] = x;
        GroupMemoryBarrierWithGroupSync();
        x = gsLds[half + (tid ^ 1u)] + 1u;
#else
#error "ubench: unknown UB_OP"
#endif
    }
#if UB_OP == 10
    GroupMemoryBarrierWithGroupSync(); // the sort's own groupshared writes come after all loads above
#endif
    if (gNumSorts == 0xDEADBEEFu)
        gOutput[tid] = x;
}

#endif
