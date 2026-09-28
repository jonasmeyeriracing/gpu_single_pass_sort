// LDS radix sort (CUB BlockRadixSort style): one group per sort, 16-bit key = value >> 16, stable,
// 4 passes of 4-bit digits (LSD first). Output = the full 32-bit input values.
//
// Arrangement: "blocked". kpt = ceil(count / GROUP_SIZE) (1..8) keys per thread; thread t holds
// elements t*kpt .. t*kpt + kpt-1 (only those < count are valid, the rest are not counted and
// not moved). The rank order (digit, then original position) is therefore (digit, wave, lane,
// key slot), which is exactly what the scans below compute, so every pass is stable.
//
// Per pass:
//  1. Per-thread digit counts in two registers (16 nibble counters; at most 8 keys per thread) and
//     each key's rank among the thread's earlier keys with the same digit (4-bit fields).
//  2. Wave-level exclusive prefix of the counts (shuffle scan): on 4 words of four 8-bit fields
//     if WAVE_SIZE <= 32 (the exclusive prefix is at most 31 * 8 = 248), else on 8 words of two
//     16-bit fields. Fields never carry into each other.
//  3. The last lane of every wave stores the wave's per-digit totals (8 words of two 16-bit fields)
//     in groupshared memory, transposed (word j of wave w at RS_WT_BASE + j * RS_NUM_WAVES + w, so
//     the reads in step 4 are conflict-free). Barrier.
//  4. Wave 0 scans the table: exclusive prefix over the waves (shuffle scan; if there are more
//     waves than lanes, each lane first sums RS_SCAN_PER_LANE consecutive waves serially), the
//     per-digit totals (last lane's inclusive sum), and a 16-entry serial scan of those gives the
//     digit bases.
//     It writes digitBase + waveOffset back in place. Barrier.
//  5. Scatter: position = table[digit][wave] + lanePrefix + rank within the thread. The last pass
//     writes straight to gOutput; the others write groupshared memory, barrier, and read back the
//     blocked slots.
// Barriers: 3 per pass (after the totals, after the scan, after the scatter), 2 in the last pass =
// 11 in total. The table (RS_NUM_WAVES * 8 words = 256 for 32-lane waves) lives at the top of the
// 8192-word exchange buffer. Only if count > RS_WT_BASE (7936 for 32-lane waves) does it overlap
// the data; then passes 2-4 need a barrier before the totals are written and passes 1-3 one before
// the scatter: 17 in total.
//
// Groupshared: the one 32 KB array (sort_lds.hlsli), used as exchange buffer [0, count) and table
// [RS_WT_BASE, 8192).
// Requires lane index == SV_GroupIndex % WAVE_SIZE (true on all known implementations for 1D groups);
// otherwise the sort is not stable, which the CPU verification would catch.
#ifndef GPUSORT_RADIX_SORT_HLSLI
#define GPUSORT_RADIX_SORT_HLSLI

#include "common.hlsli"
#include "sort_lds.hlsli"
#include "wave_scan.hlsli"

// RS_WAVE_INTRINSICS = 1: scans with WavePrefixSum / WaveActiveSum (slow on NVIDIA, see
// wave_scan.hlsli); 0 (default): WaveReadLaneAt shuffle scans.
#ifndef RS_WAVE_INTRINSICS
#define RS_WAVE_INTRINSICS 0
#endif

#define RS_KPT_MAX ((MAX_SORT_SIZE + GROUP_SIZE - 1) / GROUP_SIZE)
#if RS_KPT_MAX > 8
#error "radix_sort: at most 8 keys per thread (4-bit counters / ranks); GROUP_SIZE must be >= 1024"
#endif
#define RS_NUM_WAVES (GROUP_SIZE / WAVE_SIZE)
#if RS_NUM_WAVES * WAVE_SIZE != GROUP_SIZE
#error "radix_sort: GROUP_SIZE must be a multiple of WAVE_SIZE"
#endif
#define RS_WT_BASE (MAX_SORT_SIZE - RS_NUM_WAVES * 8)
#define RS_SCAN_PER_LANE ((RS_NUM_WAVES + WAVE_SIZE - 1) / WAVE_SIZE)
#ifndef RS_PREFIX8
#if (WAVE_SIZE - 1) * RS_KPT_MAX <= 255
#define RS_PREFIX8 1
#else
#define RS_PREFIX8 0
#endif
#endif
#if RS_PREFIX8 && (WAVE_SIZE - 1) * RS_KPT_MAX > 255
#error "radix_sort: 8-bit lane prefix fields would overflow for this wave size"
#endif
#define RS_RADIX_BITS 4
#define RS_PASSES 4

// Word i of w[0..7] (i = 0..7), without dynamic register indexing.
uint RsSelect8(uint w[8], uint i)
{
    const bool b0 = (i & 1u) != 0;
    const bool b1 = (i & 2u) != 0;
    const bool b2 = (i & 4u) != 0;
    const uint s0 = b0 ? w[1] : w[0];
    const uint s1 = b0 ? w[3] : w[2];
    const uint s2 = b0 ? w[5] : w[4];
    const uint s3 = b0 ? w[7] : w[6];
    const uint t0 = b1 ? s1 : s0;
    const uint t1 = b1 ? s3 : s2;
    return b2 ? t1 : t0;
}

// Sorts gInput[offset, offset + count) into gOutput[offset, ...) by the high 16 bits, stable.
// count must be 1..8192. Must be called by all threads of the group; count must be group-uniform.
void RadixSort(uint tid, uint offset, uint count)
{
    const uint kpt = (count + GROUP_SIZE - 1) / GROUP_SIZE; // 1..RS_KPT_MAX
    const uint first = tid * kpt;
    const uint lane = tid & (WAVE_SIZE - 1u);
    const uint wave = tid >> WAVE_BITS;
    const bool aliasTable = count > RS_WT_BASE; // group-uniform

    uint v[RS_KPT_MAX];
    [unroll]
    for (uint k = 0; k < RS_KPT_MAX; ++k)
    {
        v[k] = 0;
        if (k < kpt && first + k < count)
            v[k] = gInput[offset + first + k];
    }

    [unroll]
    for (uint pass = 0; pass < RS_PASSES; ++pass)
    {
        const uint shift = 16 + RS_RADIX_BITS * pass;
        const bool lastPass = pass == RS_PASSES - 1;

        // 1. per-thread digit counts (nibbles) and ranks within the thread
        uint countLo = 0; // digits 0..7
        uint countHi = 0; // digits 8..15
        uint localRanks = 0;
        [unroll]
        for (uint k = 0; k < RS_KPT_MAX; ++k)
        {
            if (k < kpt && first + k < count)
            {
                const uint d = (v[k] >> shift) & 15u;
                const uint sh = (d & 7u) * 4u;
                const bool hi = d >= 8u;
                localRanks |= (((hi ? countHi : countLo) >> sh) & 15u) << (4u * k);
                const uint inc = 1u << sh;
                countLo += hi ? 0u : inc;
                countHi += hi ? inc : 0u;
            }
        }

        // 2. counts as 16-bit pairs (word j = digits 2j, 2j+1) and the wave-level exclusive prefix
        uint count16[8];
        uint lanePrefix[8];
        [unroll]
        for (uint j = 0; j < 8; ++j)
        {
            const uint x = (j < 4) ? countLo : countHi;
            const uint sh = (j & 3u) * 8u;
            count16[j] = ((x >> sh) & 15u) | (((x >> (sh + 4u)) & 15u) << 16);
        }
#if RS_PREFIX8
        [unroll]
        for (uint q = 0; q < 4; ++q)
        {
            // digits 4q .. 4q+3 as four 8-bit fields
            const uint x = (q < 2) ? countLo : countHi;
            const uint sh = (q & 1u) * 16u;
            const uint c8 = ((x >> sh) & 15u) | (((x >> (sh + 4u)) & 15u) << 8) |
                            (((x >> (sh + 8u)) & 15u) << 16) | (((x >> (sh + 12u)) & 15u) << 24);
#if RS_WAVE_INTRINSICS
            const uint p8 = WavePrefixSum(c8);
#else
            const uint p8 = WaveInclusiveSumShfl(c8, lane) - c8; // fields never borrow
#endif
            lanePrefix[2 * q + 0] = (p8 & 0xFFu) | (((p8 >> 8) & 0xFFu) << 16);
            lanePrefix[2 * q + 1] = ((p8 >> 16) & 0xFFu) | ((p8 >> 24) << 16);
        }
#else
        [unroll]
        for (uint j = 0; j < 8; ++j)
        {
#if RS_WAVE_INTRINSICS
            lanePrefix[j] = WavePrefixSum(count16[j]);
#else
            lanePrefix[j] = WaveInclusiveSumShfl(count16[j], lane) - count16[j];
#endif
        }
#endif

        // 3. per-wave totals to the table
        if (aliasTable && pass > 0)
            GroupMemoryBarrierWithGroupSync(); // everyone has read back the previous pass's data
        if (lane == WAVE_SIZE - 1u)
        {
            [unroll]
            for (uint j = 0; j < 8; ++j)
                gsLds[RS_WT_BASE + j * RS_NUM_WAVES + wave] = lanePrefix[j] + count16[j];
        }
        GroupMemoryBarrierWithGroupSync();

        // 4. wave 0: table[j][w] = digitBase + sum of waves < w
        if (wave == 0)
        {
            uint lanePre[8];
            uint total[8];
            [unroll]
            for (uint j = 0; j < 8; ++j)
            {
                uint sum = 0;
                [unroll]
                for (uint c = 0; c < RS_SCAN_PER_LANE; ++c)
                {
                    const uint w = lane * RS_SCAN_PER_LANE + c;
                    if (w < RS_NUM_WAVES)
                        sum += gsLds[RS_WT_BASE + j * RS_NUM_WAVES + w];
                }
#if RS_WAVE_INTRINSICS
                lanePre[j] = WavePrefixSum(sum);
                total[j] = WaveActiveSum(sum);
#else
                const uint inclusive = WaveInclusiveSumShfl(sum, lane);
                lanePre[j] = inclusive - sum;
                total[j] = WaveReadLaneAt(inclusive, WAVE_SIZE - 1u);
#endif
            }
            uint running = 0; // digit base (exclusive scan over the 16 digit totals)
            [unroll]
            for (uint j = 0; j < 8; ++j)
            {
                const uint tLo = total[j] & 0xFFFFu;
                const uint tHi = total[j] >> 16;
                uint acc = (running | ((running + tLo) << 16)) + lanePre[j];
                running += tLo + tHi;
                [unroll]
                for (uint c = 0; c < RS_SCAN_PER_LANE; ++c)
                {
                    const uint w = lane * RS_SCAN_PER_LANE + c;
                    if (w < RS_NUM_WAVES)
                    {
                        const uint idx = RS_WT_BASE + j * RS_NUM_WAVES + w;
                        const uint x = gsLds[idx];
                        gsLds[idx] = acc;
                        acc += x;
                    }
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        uint off[8];
        [unroll]
        for (uint j = 0; j < 8; ++j)
            off[j] = gsLds[RS_WT_BASE + j * RS_NUM_WAVES + wave] + lanePrefix[j];

        // 5. scatter
        if (aliasTable && !lastPass)
            GroupMemoryBarrierWithGroupSync(); // everyone has read the table before it is overwritten
        [unroll]
        for (uint k = 0; k < RS_KPT_MAX; ++k)
        {
            if (k < kpt && first + k < count)
            {
                const uint d = (v[k] >> shift) & 15u;
                const uint word = RsSelect8(off, d >> 1);
                const uint pos = ((word >> ((d & 1u) * 16u)) & 0xFFFFu) + ((localRanks >> (4u * k)) & 15u);
                if (lastPass)
                    gOutput[offset + min(pos, count - 1u)] = v[k];
                else
                    gsLds[pos & (MAX_SORT_SIZE - 1u)] = v[k];
            }
        }
        if (!lastPass)
        {
            GroupMemoryBarrierWithGroupSync();
            [unroll]
            for (uint k = 0; k < RS_KPT_MAX; ++k)
            {
                if (k < kpt && first + k < count)
                    v[k] = gsLds[(first + k) & (MAX_SORT_SIZE - 1u)];
            }
        }
    }
}

#endif
