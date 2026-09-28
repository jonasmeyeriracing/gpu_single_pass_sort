// LDS radix sort, version 2 (pass3). Same sort as radix_sort.hlsli: one group per sort, 16-bit key =
// value >> 16, stable, 4 passes of 4-bit digits (LSD first), blocked arrangement (thread t holds
// slots t*kpt .. t*kpt + kpt-1), up to 8 keys per thread, output = the full 32-bit input values.
// What changed (see _test/pass3/notes.md):
//
//  - No serial wave-0 section and one barrier less per pass. After the per-wave totals are in the
//    table (barrier A), EVERY wave scans the table itself and scatters right away. Per pass: A
//    (after the table write) and B (after the scatter, before the blocked read-back); the last pass
//    scatters straight to gOutput, so 2 + 2 + 2 + 1 = 7 barriers (pass2: 11).
//  - The table scan is spread over the lanes: "task" (j, g) = lane (j = lane & 7 = digit-pair word,
//    g = lane >> 3 = group of RS2_WPT consecutive waves) sums its RS2_WPT table words serially (the
//    sum over all of them and over the waves below this one), a butterfly over the groups (lane ^ 8,
//    lane ^ 16, ...) completes both sums, and a 2-step scan over the 4 digit groups gives the digit
//    bases. Per wave (32 lanes): 8 conflict-free LDS reads + 4 + 1 + 2 shuffles (pass2: wave 0 did
//    8 x 5-step scans while the other 31 waves waited, then every wave read 8 words).
//  - Table words hold two 16-bit counts, digits (4q + r, 4q + r + 2) in word j = 2q + r: that is
//    what (bytes & 0x00FF00FF) of the 8-bit lane-prefix words gives directly.
//  - Swizzled exchange slots: Rs2Addr(i) = i ^ ((i >> 5) & 7). The blocked read-back (slot
//    tid*kpt + k) of a wave then hits 32 different banks for kpt = 1, 2, 4, 8 (pass2: 8-way bank
//    conflicts at kpt = 8).
//  - kpt = max(ceil(count / 1024), RS2_MIN_KPT): with RS2_MIN_KPT = 8 only ceil(count / 256) waves
//    do any work; the other waves only take part in the barriers (pass2: all 32 waves ran the
//    counts, scans and table scan for every sort).
//  - Table aliasing (count > RS2_TABLE_BASE, 7936 for 32 lanes; then kpt = 8): the table of each
//    wave lives in the 8 exchange slots of the wave's LAST lane, which that lane read back itself in
//    the previous pass, so no barrier is needed before the table write (pass2 needed one). One
//    barrier C between the table scan and the scatter remains (the scatter overwrites the table):
//    10 barriers at 8192 (pass2: 17).
//  - RS2_SKIP = 1: skip passes whose digit is the same for every key. While loading, every thread
//    ORs (key | ~key << 16) over its keys; a shuffle butterfly per wave and one extra table word per
//    wave (read after barrier A of pass 0) give the group-wide OR. A digit varies iff some bit of it
//    is set in both halves. Passes 1-3 with a constant digit are skipped completely (no barriers);
//    for pass 0 only its scatter is skipped. If no digit varies, the input order is the result.
//
// Per pass (only the first ceil(numThreads / WAVE_SIZE) waves, "active waves", do anything but barriers):
//  1. Per-thread digit counts (16 nibble counters in 2 registers) and each key's rank among the
//     thread's earlier keys with the same digit (4-bit fields).
//  2. Counts as 8-bit fields (4 words, digits 4q..4q+3 in word q), wave-level exclusive prefix by
//     shuffle scans (at most 31 * 8 = 248 for 32 lanes). For WAVE_SIZE >= 64 the prefix needs more
//     than 8 bits: then 8 words of 16-bit fields, same pairing as the table.
//  3. The last lane writes the wave's 8 table words (prefix + own count, 16-bit fields). Barrier A.
//  4. Table scan (above): per task word j: offset of this wave for both digits = digit base +
//     count of that digit in the waves below.
//  5. Scatter: position = offset (shuffled from the task lane of the digit's word) + lane prefix +
//     rank within the thread. The last pass writes gOutput; the others write groupshared memory,
//     barrier B, read back the blocked slots.
//
// Groupshared: the one 32 KB array (sort_lds.hlsli): exchange slots [0, count), table
// [RS2_TABLE_BASE, 8192) (8 words per wave, plus one OR word per wave if RS2_SKIP).
// Requires lane index == SV_GroupIndex % WAVE_SIZE (true on all known implementations for 1D groups);
// otherwise the sort is not stable, which the CPU verification would catch.
#ifndef GPUSORT_RADIX_SORT2_HLSLI
#define GPUSORT_RADIX_SORT2_HLSLI

#include "common.hlsli"
#include "sort_lds.hlsli"
#include "wave_scan.hlsli"

#ifndef RS2_MIN_KPT
#define RS2_MIN_KPT 8
#endif
#ifndef RS2_SKIP
#define RS2_SKIP 0
#endif

#define RS2_KPT_MAX 8
#if GROUP_SIZE * RS2_KPT_MAX < MAX_SORT_SIZE || GROUP_SIZE > 1024
#error "radix_sort2: GROUP_SIZE must be 1024 (at most 8 keys per thread)"
#endif
#if RS2_MIN_KPT < 1 || RS2_MIN_KPT > RS2_KPT_MAX
#error "radix_sort2: RS2_MIN_KPT must be 1..8"
#endif
#define RS2_NUM_WAVES (GROUP_SIZE / WAVE_SIZE)
#if RS2_NUM_WAVES * WAVE_SIZE != GROUP_SIZE
#error "radix_sort2: GROUP_SIZE must be a multiple of WAVE_SIZE"
#endif

// Table scan tasks: 8 words x RS2_GROUPS wave groups, RS2_R tasks per lane, on the first
// RS2_TASK_LANES lanes; the other lanes repeat the tasks of lane & (RS2_TASK_LANES - 1), so the
// task shuffles (lane ^ 8, lane ^ 16, ...) stay inside the shuffle span (pass4, common.hlsli: a
// wave64 runs the table scan in both 32-lane halves). Default: the shuffle span (min(WAVE_SIZE, 32);
// with fewer than 8 lanes per wave the tasks use uniform-index reads instead, RS2_R > 1). Must be
// WAVE_SIZE or 8..WAVE_SIZE (smaller values test the repeated-task path on 32-lane hardware).
#ifndef RS2_TASK_LANES
#if SHUFFLE_SPAN >= 8
#define RS2_TASK_LANES SHUFFLE_SPAN
#elif WAVE_SIZE >= 8
#define RS2_TASK_LANES 8 // only with SHUFFLE_SPAN_TEST on 8-lane waves
#else
#define RS2_TASK_LANES WAVE_SIZE
#endif
#endif
#if RS2_TASK_LANES > WAVE_SIZE || (RS2_TASK_LANES < 8 && RS2_TASK_LANES != WAVE_SIZE)
#error "radix_sort2: RS2_TASK_LANES must be WAVE_SIZE or 8..WAVE_SIZE"
#endif
#if RS2_TASK_LANES >= 8
#define RS2_R 1
#define RS2_GROUPS (RS2_TASK_LANES / 8)
#else
#define RS2_R (8 / WAVE_SIZE)
#define RS2_GROUPS 1
#endif
#define RS2_WPT ((RS2_NUM_WAVES + RS2_GROUPS - 1) / RS2_GROUPS) // waves per task
#if RS2_WPT <= 16
#define RS2_WPT_LOOP [unroll]
#else
#define RS2_WPT_LOOP [loop]
#endif

#if RS2_SKIP
#define RS2_OR_WORDS RS2_NUM_WAVES
#else
#define RS2_OR_WORDS 0
#endif
#define RS2_TABLE_BASE (MAX_SORT_SIZE - 8 * RS2_NUM_WAVES - RS2_OR_WORDS) // multiple of 8
#define RS2_OR_BASE (MAX_SORT_SIZE - RS2_OR_WORDS)
#define RS2_OR_PER_LANE ((RS2_NUM_WAVES + WAVE_SIZE - 1) / WAVE_SIZE)
#if RS2_OR_PER_LANE <= 8
#define RS2_OR_LOOP [unroll]
#else
#define RS2_OR_LOOP [loop]
#endif

// Lane prefix fields: 8-bit (4 words) if they cannot overflow ((WAVE_SIZE - 1) * 8 <= 255, i.e.
// up to 32 lanes), else 16-bit (8 words). RS2_PREFIX8 = 0 forces the 16-bit path (pass4: to run the
// wave64 path on 32-lane hardware).
#ifndef RS2_PREFIX8
#if (WAVE_SIZE - 1) * RS2_KPT_MAX <= 255
#define RS2_PREFIX8 1
#else
#define RS2_PREFIX8 0
#endif
#endif
#if RS2_PREFIX8 && (WAVE_SIZE - 1) * RS2_KPT_MAX > 255
#error "radix_sort2: 8-bit lane prefix fields would overflow for this wave size"
#endif
#if RS2_PREFIX8
#define RS2_PRE_WORDS 4
#else
#define RS2_PRE_WORDS 8
#endif

// Swizzled groupshared address of exchange slot i (bits 0-2 XOR bits 5-7; stays inside the aligned
// 8-slot block, so slots < count map below the table if count <= RS2_TABLE_BASE).
uint Rs2Addr(uint i)
{
    return (i ^ ((i >> 5) & 7u)) & (MAX_SORT_SIZE - 1u);
}

// Table word j (0..7) of wave w (< RS2_NUM_WAVES). Not aliased: 8 words per wave at the top, wave
// index permuted (bits 0-1 XOR bits 3-4) so the 32 reads of a table-scan step hit 32 banks.
// Aliased: the exchange slots of the wave's last lane.
uint Rs2TableAddr(bool alias, uint j, uint w)
{
    const uint top = RS2_TABLE_BASE + 8u * (w ^ ((w >> 3) & 3u)) + j;
    const uint own = Rs2Addr((w * WAVE_SIZE + WAVE_SIZE - 1u) * RS2_KPT_MAX + j);
    return (alias ? own : top) & (MAX_SORT_SIZE - 1u);
}

// OR word of wave w (pass 0 only). Aliased: the last exchange slot of the wave's second-to-last lane
// (pass 0 has no read-back before it, and barrier C protects it from the scatter).
uint Rs2OrAddr(bool alias, uint w)
{
    const uint own = Rs2Addr((w * WAVE_SIZE + WAVE_SIZE - 2u) * RS2_KPT_MAX + (RS2_KPT_MAX - 1u));
    return (alias ? own : RS2_OR_BASE + w) & (MAX_SORT_SIZE - 1u);
}

// Value x of table-scan task t (0..7) of this lane's task group. All lanes must be active.
uint Rs2TaskRead(uint x[RS2_R], uint t, uint lane)
{
#if RS2_R == 1
    return WaveReadLaneAt(x[0], (lane & ~7u) | (t & 7u));
#else
    uint y = 0;
    [unroll]
    for (uint r = 0; r < RS2_R; ++r)
    {
        const uint v = WaveReadLaneAt(x[r], t & (WAVE_SIZE - 1u));
        if (((t & 7u) >> WAVE_BITS) == r)
            y = v;
    }
    return y;
#endif
}

// Nibbles n0..n3 of x (bits 0-15) to bytes.
uint Rs2Spread(uint x)
{
    uint t = (x | (x << 8)) & 0x00FF00FFu;
    return (t | (t << 4)) & 0x0F0F0F0Fu;
}

// Word i of w[0..3] / w[0..7], without dynamic register indexing.
uint Rs2Select4(uint w[4], uint i)
{
    const uint a = (i & 1u) ? w[1] : w[0];
    const uint b = (i & 1u) ? w[3] : w[2];
    return (i & 2u) ? b : a;
}
uint Rs2Select8(uint w[8], uint i)
{
    const uint a = (i & 1u) ? w[1] : w[0];
    const uint b = (i & 1u) ? w[3] : w[2];
    const uint c = (i & 1u) ? w[5] : w[4];
    const uint d = (i & 1u) ? w[7] : w[6];
    const uint e = (i & 2u) ? b : a;
    const uint f = (i & 2u) ? d : c;
    return (i & 4u) ? f : e;
}

// This lane's exclusive wave prefix for digit d.
uint Rs2Prefix(uint pre[RS2_PRE_WORDS], uint d)
{
#if RS2_PREFIX8
    return (Rs2Select4(pre, d >> 2) >> ((d & 3u) * 8u)) & 0xFFu;
#else
    const uint task = ((d >> 2) << 1) | (d & 1u);
    return (Rs2Select8(pre, task) >> (((d >> 1) & 1u) * 16u)) & 0xFFFFu;
#endif
}

// Sorts gInput[offset, offset + count) into gOutput[offset, ...) by the high 16 bits, stable.
// count must be 1..8192. Must be called by all threads of the group; count must be group-uniform.
void RadixSort2(uint tid, uint offset, uint count)
{
    const bool alias = count > RS2_TABLE_BASE; // group-uniform
    uint kpt = (count + GROUP_SIZE - 1u) / GROUP_SIZE;
    kpt = alias ? RS2_KPT_MAX : clamp(kpt, (uint)RS2_MIN_KPT, (uint)RS2_KPT_MAX);
    const uint numThreads = (count + kpt - 1u) / kpt; // <= GROUP_SIZE
    const uint activeWaves = (numThreads + WAVE_SIZE - 1u) >> WAVE_BITS;
    const uint lane = tid & (WAVE_SIZE - 1u);
    const uint wave = tid >> WAVE_BITS;
    const bool waveActive = wave < activeWaves; // wave-uniform
    const uint first = tid * kpt;

    uint v[RS2_KPT_MAX];
    uint orBits = 0; // low half: OR of the keys, high half: OR of the complemented keys
    [unroll]
    for (uint k = 0; k < RS2_KPT_MAX; ++k)
    {
        v[k] = 0;
        if (k < kpt && first + k < count)
        {
            v[k] = gInput[offset + first + k];
            orBits |= (v[k] >> 16) | (~v[k] & 0xFFFF0000u);
        }
    }

    uint passMask = 15u; // passes whose digit varies (RS2_SKIP: known after barrier A of pass 0)
    uint lastPass = 3u;

    [unroll]
    for (uint pass = 0; pass < 4; ++pass)
    {
        if (pass > 0 && ((passMask >> pass) & 1u) == 0)
            continue; // group-uniform
        const uint shift = 16u + 4u * pass;

        // 1.-3. counts, wave prefix, table words
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
#if RS2_SKIP
            if (pass == 0)
                orBits = WaveOrShfl(orBits, lane);
#endif
            if (lane == WAVE_SIZE - 1u)
            {
                [unroll]
                for (uint j = 0; j < 8; ++j)
                    gsLds[Rs2TableAddr(alias, j, wave)] = tw[j];
#if RS2_SKIP
                if (pass == 0)
                    gsLds[Rs2OrAddr(alias, wave)] = orBits;
#endif
            }
        }
        GroupMemoryBarrierWithGroupSync(); // A

#if RS2_SKIP
        if (pass == 0)
        {
            // Every wave (active or not) computes the same pass mask from the same table words.
            uint o = 0;
            RS2_OR_LOOP
            for (uint c = 0; c < RS2_OR_PER_LANE; ++c)
            {
                const uint w2 = lane + c * WAVE_SIZE;
                if (w2 < activeWaves)
                    o |= gsLds[Rs2OrAddr(alias, w2)];
            }
            o = WaveOrShfl(o, lane);
            const uint varying = o & (o >> 16); // key bits that are 1 in some key and 0 in another
            passMask = 0;
            [unroll]
            for (uint p = 0; p < 4; ++p)
            {
                if (((varying >> (4u * p)) & 15u) != 0)
                    passMask |= 1u << p;
            }
            lastPass = firstbithigh(passMask); // only used if passMask != 0
            if ((passMask & 1u) == 0)
            {
                if (passMask == 0)
                {
                    // Every digit is constant: the input order is the (stable) result.
                    if (waveActive)
                    {
                        [unroll]
                        for (uint k = 0; k < RS2_KPT_MAX; ++k)
                        {
                            if (k < kpt && first + k < count)
                                gOutput[offset + first + k] = v[k];
                        }
                    }
                    return;
                }
                // Digit 0 is constant: nothing moves. The next pass writes only the pair-table
                // words (nobody read them in this pass) and only reads after its own barrier A.
                continue;
            }
        }
#endif
        const bool isLast = pass == lastPass; // group-uniform

        // 4. table scan: off[r] = this wave's offsets of the two digits of task word j
        uint off[RS2_R];
        [unroll]
        for (uint r = 0; r < RS2_R; ++r)
            off[r] = 0;
        if (waveActive)
        {
            const uint taskLane = lane & (RS2_TASK_LANES - 1u);
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
            // Digit bases. Word j = 2q + r holds digits 4q + r and 4q + r + 2; its partner j ^ 1 the
            // other two digits of group q. s4 = total of group q; xs = inclusive scan over q.
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
                off[r] = (baseLo + (exc[r] & 0xFFFFu)) | ((baseHi + (exc[r] >> 16)) << 16);
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
                    const uint ow = Rs2TaskRead(off, ((d >> 2) << 1) | (d & 1u), lane);
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
