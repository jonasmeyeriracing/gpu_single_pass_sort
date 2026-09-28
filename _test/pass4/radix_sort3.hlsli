// LDS radix sort, version 3 (pass4): radix_sort2.hlsli with a size-dependent table scan, so one
// radix is (meant to be) the best at every size. Same sort: one group per sort, 16-bit key =
// value >> 16, stable, 4 passes of 4-bit digits (LSD first), blocked arrangement, 1024 threads, up to
// 8 keys per thread (kpt = max(ceil(count / 1024), RS2_MIN_KPT)), swizzled exchange slots, the
// aliasing fix for count > RS2_TABLE_BASE, and the per-wave tables of radix_sort2.hlsli (all helpers
// and RS2_* settings come from there; RS2_SKIP is not supported).
//
// Table scan, chosen per sort (group-uniform):
//  - count <= RS3_PERWAVE_MAX (default 4096, i.e. at most 16 active 32-lane waves at kpt 8): every
//    active wave scans the table itself, exactly as radix_sort2 (8 table reads + 7 shuffles per wave,
//    then one shuffle per key for its offset). Barriers per pass: A (table written), B (after the
//    scatter): 7 in total.
//  - larger counts: only wave 0 runs the table-scan tasks, over all active waves: task (j, g) sums
//    word j of its RS2_WPT waves, an exclusive scan over the groups (lane - 8, lane - 16, ...) and a
//    butterfly for the totals give, per task, the offset of its first wave; it then walks its waves
//    again and writes each wave's offset word (digit base + count in the waves below) over that
//    wave's table word. Barrier B'. Every wave reads its 8 offset words (broadcast reads) and the
//    scatter selects the key's word with Rs2Select8 (ALU, no shuffle). Barriers per pass: A, B', B:
//    11 in total, 14 with aliasing (count > 7936: the offsets live in the wave's own exchange slots,
//    so barrier C after the offset reads, before the scatter overwrites them).
//    At full occupancy this issues far fewer shuffles / LDS operations than the per-wave scan
//    (pass3: radix_sort2 lost 0.5-1.2 us to the pass2 radix at 5-8k, attributed to MIO pressure).
//
// RS3_ROLLED = 1: the 4 passes are a [loop] instead of unrolled (about 1/4 of the code; the pass
// index, shift and "last pass" become runtime values). For the instruction-cache experiment.
//
// Shuffles stay inside RS2_TASK_LANES lanes (wave64: each 32-lane half, see common.hlsli); the wave
// prefix scans use wave_scan.hlsli (wave intrinsics for wave64).
// Requires lane index == SV_GroupIndex % WAVE_SIZE (true on all known implementations for 1D groups);
// otherwise the sort is not stable, which the CPU verification would catch.
#ifndef GPUSORT_RADIX_SORT3_HLSLI
#define GPUSORT_RADIX_SORT3_HLSLI

#include "radix_sort2.hlsli"

#if RS2_SKIP
#error "radix_sort3: RS2_SKIP is not supported"
#endif
#ifndef RS3_PERWAVE_MAX
#define RS3_PERWAVE_MAX 4096
#endif
#ifndef RS3_ROLLED
#define RS3_ROLLED 0
#endif
#if RS3_ROLLED
#define RS3_PASS_LOOP [loop]
#else
#define RS3_PASS_LOOP [unroll]
#endif

// Digit bases for this lane's table-scan tasks: word j = 2q + r holds digits 4q + r and
// 4q + r + 2 (radix_sort2.hlsli); tot = the totals of all task words over all waves. Returns
// baseLo | (baseHi << 16). All lanes must be active (shuffles).
void Rs3DigitBases(uint tot[RS2_R], uint lane, out uint baseW[RS2_R])
{
    uint part[RS2_R];
    uint s4[RS2_R];
    uint xs[RS2_R];
    [unroll]
    for (uint r = 0; r < RS2_R; ++r)
    {
        const uint j = (lane + r * WAVE_SIZE) & 7u;
        part[r] = Rs2TaskRead(tot, j ^ 1u, lane);
        s4[r] = (tot[r] & 0xFFFFu) + (tot[r] >> 16) + (part[r] & 0xFFFFu) + (part[r] >> 16);
        xs[r] = s4[r];
    }
    [unroll]
    for (uint dq = 2; dq < 8; dq <<= 1) // task distance 2, 4 = group distance 1, 2
    {
        uint ys[RS2_R];
        [unroll]
        for (uint r = 0; r < RS2_R; ++r)
            ys[r] = Rs2TaskRead(xs, ((lane + r * WAVE_SIZE) - dq) & 7u, lane);
        [unroll]
        for (uint r = 0; r < RS2_R; ++r)
        {
            if (((lane + r * WAVE_SIZE) & 7u) >= dq)
                xs[r] += ys[r];
        }
    }
    [unroll]
    for (uint r = 0; r < RS2_R; ++r)
    {
        const uint j = (lane + r * WAVE_SIZE) & 7u;
        const uint base = xs[r] - s4[r]; // first position of digit 4q
        const uint aLo = tot[r] & 0xFFFFu;
        const uint bLo = part[r] & 0xFFFFu;
        const uint bHi = part[r] >> 16;
        uint baseLo; // digit 4q + r
        uint baseHi; // digit 4q + r + 2
        if ((j & 1u) == 0)
        {
            baseLo = base;
            baseHi = base + aLo + bLo;
        }
        else
        {
            baseLo = base + bLo;
            baseHi = base + bLo + aLo + bHi;
        }
        baseW[r] = baseLo | (baseHi << 16);
    }
}

// Sorts gInput[offset, offset + count) into gOutput[offset, ...) by the high 16 bits, stable.
// count must be 1..8192. Must be called by all threads of the group; count must be group-uniform.
void RadixSort3(uint tid, uint offset, uint count)
{
    const bool alias = count > RS2_TABLE_BASE;       // group-uniform
    const bool perWave = count <= RS3_PERWAVE_MAX;   // group-uniform
    uint kpt = (count + GROUP_SIZE - 1u) / GROUP_SIZE;
    kpt = alias ? RS2_KPT_MAX : clamp(kpt, (uint)RS2_MIN_KPT, (uint)RS2_KPT_MAX);
    const uint numThreads = (count + kpt - 1u) / kpt; // <= GROUP_SIZE
    const uint activeWaves = (numThreads + WAVE_SIZE - 1u) >> WAVE_BITS;
    const uint lane = tid & (WAVE_SIZE - 1u);
    const uint wave = tid >> WAVE_BITS;
    const bool waveActive = wave < activeWaves; // wave-uniform
    const uint first = tid * kpt;
    const uint taskLane = lane & (RS2_TASK_LANES - 1u);

    uint v[RS2_KPT_MAX];
    [unroll]
    for (uint k = 0; k < RS2_KPT_MAX; ++k)
    {
        v[k] = 0;
        if (k < kpt && first + k < count)
            v[k] = gInput[offset + first + k];
    }

    RS3_PASS_LOOP
    for (uint pass = 0; pass < 4; ++pass)
    {
        const uint shift = 16u + 4u * pass;
        const bool isLast = pass == 3u; // group-uniform

        // 1.-3. counts, wave prefix, table words (as radix_sort2)
        uint localRanks = 0;
        uint pre[RS2_PRE_WORDS];
        [unroll]
        for (uint i = 0; i < RS2_PRE_WORDS; ++i)
            pre[i] = 0;
        if (waveActive)
        {
            uint countLo = 0; // digits 0..7
            uint countHi = 0; // digits 8..15
            [unroll]
            for (uint k = 0; k < RS2_KPT_MAX; ++k)
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
            uint c8[4];
            c8[0] = Rs2Spread(countLo & 0xFFFFu);
            c8[1] = Rs2Spread(countLo >> 16);
            c8[2] = Rs2Spread(countHi & 0xFFFFu);
            c8[3] = Rs2Spread(countHi >> 16);
            uint tw[8]; // table words: prefix + own count (used by the last lane only)
#if RS2_PREFIX8
            [unroll]
            for (uint q = 0; q < 4; ++q)
            {
                pre[q] = WaveInclusiveSumShfl(c8[q], lane) - c8[q]; // fields never borrow
                tw[2 * q + 0] = (pre[q] & 0x00FF00FFu) + (c8[q] & 0x00FF00FFu);
                tw[2 * q + 1] = ((pre[q] >> 8) & 0x00FF00FFu) + ((c8[q] >> 8) & 0x00FF00FFu);
            }
#else
            [unroll]
            for (uint j = 0; j < 8; ++j)
            {
                const uint c16 = ((j & 1u) ? (c8[j >> 1] >> 8) : c8[j >> 1]) & 0x00FF00FFu;
                pre[j] = WaveInclusiveSumShfl(c16, lane) - c16;
                tw[j] = pre[j] + c16;
            }
#endif
            if (lane == WAVE_SIZE - 1u)
            {
                [unroll]
                for (uint j = 0; j < 8; ++j)
                    gsLds[Rs2TableAddr(alias, j, wave)] = tw[j];
            }
        }
        GroupMemoryBarrierWithGroupSync(); // A

        // 4. table scan
        uint off[RS2_R]; // per-wave mode: this wave's offset words of this lane's tasks
        uint offW[8];    // single-wave mode: this wave's 8 offset words
        [unroll]
        for (uint r = 0; r < RS2_R; ++r)
            off[r] = 0;
        [unroll]
        for (uint j = 0; j < 8; ++j)
            offW[j] = 0;
        if (perWave)
        {
            if (waveActive)
            {
                uint tot[RS2_R]; // both digits' totals over all waves (16-bit fields)
                uint exc[RS2_R]; // both digits' counts in the waves below this one
                [unroll]
                for (uint r = 0; r < RS2_R; ++r)
                {
                    const uint t = taskLane + r * RS2_TASK_LANES;
                    const uint j = t & 7u;
                    const uint g = t >> 3;
                    uint a = 0;
                    uint e = 0;
                    RS2_WPT_LOOP
                    for (uint i = 0; i < RS2_WPT; ++i)
                    {
                        const uint w2 = g * RS2_WPT + i;
                        if (w2 < activeWaves)
                        {
                            const uint x = gsLds[Rs2TableAddr(alias, j, w2)];
                            a += x;
                            e += (w2 < wave) ? x : 0u;
                        }
                    }
                    tot[r] = a;
                    exc[r] = e;
                }
#if RS2_R == 1
                [unroll]
                for (uint m = 8; m < RS2_TASK_LANES; m <<= 1)
                {
                    tot[0] += WaveReadLaneAt(tot[0], lane ^ m);
                    exc[0] += WaveReadLaneAt(exc[0], lane ^ m);
                }
#endif
                uint baseW[RS2_R];
                Rs3DigitBases(tot, lane, baseW);
                [unroll]
                for (uint r = 0; r < RS2_R; ++r)
                    off[r] = baseW[r] + exc[r]; // 16-bit fields, no carry (<= 8192 each)
            }
        }
        else
        {
            if (wave == 0) // wave-uniform; wave 0 is always active
            {
                uint tot[RS2_R];  // totals over all waves
                uint gsum[RS2_R]; // sums over this task's waves
                [unroll]
                for (uint r = 0; r < RS2_R; ++r)
                {
                    const uint t = taskLane + r * RS2_TASK_LANES;
                    const uint j = t & 7u;
                    const uint g = t >> 3;
                    uint a = 0;
                    RS2_WPT_LOOP
                    for (uint i = 0; i < RS2_WPT; ++i)
                    {
                        const uint w2 = g * RS2_WPT + i;
                        if (w2 < activeWaves)
                            a += gsLds[Rs2TableAddr(alias, j, w2)];
                    }
                    tot[r] = a;
                    gsum[r] = a;
                }
                uint gexc[RS2_R]; // sums over the waves of the groups below this task's group
#if RS2_R == 1
                {
                    uint inc = gsum[0];
                    [unroll]
                    for (uint m = 8; m < RS2_TASK_LANES; m <<= 1)
                    {
                        const uint y = WaveReadLaneAt(inc, (lane & ~(RS2_TASK_LANES - 1u)) |
                                                               ((taskLane - m) & (RS2_TASK_LANES - 1u)));
                        if (taskLane >= m)
                            inc += y;
                    }
                    gexc[0] = inc - gsum[0];
                }
                [unroll]
                for (uint m = 8; m < RS2_TASK_LANES; m <<= 1)
                    tot[0] += WaveReadLaneAt(tot[0], lane ^ m);
#else
                [unroll]
                for (uint r = 0; r < RS2_R; ++r)
                    gexc[r] = 0; // a single group
#endif
                uint baseW[RS2_R];
                Rs3DigitBases(tot, lane, baseW);
                if (lane < RS2_TASK_LANES) // the repeated lanes would write the same values
                {
                    [unroll]
                    for (uint r = 0; r < RS2_R; ++r)
                    {
                        const uint t = taskLane + r * RS2_TASK_LANES;
                        const uint j = t & 7u;
                        const uint g = t >> 3;
                        uint running = baseW[r] + gexc[r];
                        RS2_WPT_LOOP
                        for (uint i = 0; i < RS2_WPT; ++i)
                        {
                            const uint w2 = g * RS2_WPT + i;
                            if (w2 < activeWaves)
                            {
                                const uint addr = Rs2TableAddr(alias, j, w2);
                                const uint x = gsLds[addr];
                                gsLds[addr] = running;
                                running += x;
                            }
                        }
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync(); // B'
            if (waveActive)
            {
                [unroll]
                for (uint j = 0; j < 8; ++j)
                    offW[j] = gsLds[Rs2TableAddr(alias, j, wave)];
            }
        }

        // 5. scatter
        if (alias && !isLast)
            GroupMemoryBarrierWithGroupSync(); // C: every wave has read the table before it is overwritten
        if (waveActive)
        {
            [unroll]
            for (uint k = 0; k < RS2_KPT_MAX; ++k)
            {
                if (k < kpt) // group-uniform: all lanes take part in the shuffle
                {
                    const uint d = (v[k] >> shift) & 15u;
                    const uint word = ((d >> 2) << 1) | (d & 1u);
                    uint ow;
                    if (perWave) // group-uniform
                        ow = Rs2TaskRead(off, word, lane);
                    else
                        ow = Rs2Select8(offW, word);
                    const uint pos = ((ow >> (((d >> 1) & 1u) * 16u)) & 0xFFFFu) + Rs2Prefix(pre, d) +
                                     ((localRanks >> (4u * k)) & 15u);
                    if (first + k < count)
                    {
                        if (isLast)
                            gOutput[offset + min(pos, count - 1u)] = v[k];
                        else
                            gsLds[Rs2Addr(pos)] = v[k];
                    }
                }
            }
        }
        if (isLast)
            return;
        GroupMemoryBarrierWithGroupSync(); // B
        if (waveActive)
        {
            [unroll]
            for (uint k = 0; k < RS2_KPT_MAX; ++k)
            {
                if (k < kpt && first + k < count)
                    v[k] = gsLds[Rs2Addr(first + k)];
            }
        }
    }
}

#endif
