// pass7: LDS radix sort for small groups (256 / 512 threads, up to 32 keys per thread) with a low
// fixed cost at small wave sizes. 16-bit key = value >> 16, stable, 4 passes of 4-bit digits (LSD),
// one group per sort, output = the full 32-bit values. Differences to radix_sort.hlsli (pass2) /
// radix_sort2.hlsli (pass3):
//
//  - Warp-striped arrangement instead of blocked: wave w owns the P7X_KPT * WAVE_SIZE consecutive
//    slots starting at w * kpt * WAVE_SIZE, and key k of lane l is slot waveBase + k * WAVE_SIZE + l.
//    Global loads and the groupshared read-back are contiguous per wave (no bank conflicts, no
//    swizzle). A sort of c keys uses only ceil(c / (kpt * WAVE_SIZE)) waves ("active waves"), so a
//    small sort does its work in one or two waves and the rest of the group only joins the barriers.
//  - Ranking by ballots ("warp multisplit") instead of per-thread digit counters + lane prefix
//    scans: for each key slot k the wave takes 4 ballots of the 4 digit bits; the lanes with the
//    same digit are AND(bit set ? ballot : ~ballot), the key's rank among them is popcount(peers &
//    lanes below). Stable in (wave, k, lane) = original order. Per key: 4 ballots + ~30 ALU ops and
//    one shuffle; no per-thread state besides the keys, so up to 32 keys per thread fit in
//    registers (the per-thread 4-bit / 8-bit counters of pass2/pass3 limit them to 8).
//  - Per-wave running digit counts are distributed over lanes: lane l of a 16-lane "digit group"
//    holds digit l (P7X_DL = 16 lanes, P7X_DPL = 1 digit per lane; waves narrower than 16 lanes hold
//    16 / WAVE_SIZE digits per lane). A key's digit offset is one WaveReadLaneAt from lane `digit`
//    of the reader's own digit group (always inside the shuffle span, common.hlsli).
//
// Per pass:
//  1. Count: for every key slot k < kpt: 4 ballots, and lane l adds popcount(valid & peers(digit
//     l)) to its digit count. The digit lanes write the wave's 16 counts to the table. Barrier A.
//  2. Table scan (every active wave): lane l sums its digit over all active waves (total) and over
//     the waves below its own (split over P7X_G = SHUFFLE_SPAN / 16 lane groups + butterfly), then
//     an exclusive scan over the 16 digit lanes gives the digit bases: run[d] = digit base +
//     count of digit d in the waves below = this wave's first slot for digit d.
//  3. Scatter: for every key slot k: ballots again, pos = run[digit] (shuffle) + rank among the
//     peers; the digit lanes advance run[] by their digit's count at k. The last pass writes
//     gOutput; the others write groupshared memory, barrier B, and read back their slots.
//  Barriers: 2 per pass, 1 in the last = 7. The table (16 words per wave) sits at the top of the
//  array; if count > P7X_T_BASE it would overlap the data ("alias"): then kpt = P7X_KPT_MAX and the
//  table of wave w lives in slots its own digit lanes read back themselves (no barrier needed
//  before the table write), plus one barrier C before the scatter overwrites them: 10 barriers.
//
// Code layout: the key-slot loops are unrolled (the keys stay in registers) and leave with a
// group-uniform 'break' at k = kpt, i.e. one jump past the unused slots. The first version guarded
// every slot with 'if (k < kpt)': a small sort then jumped over each of the 31 unused slot blocks
// separately, and in the serial hardware smoke (code cold) a 256-thread sort of 32 keys took 18 us
// (6 us at 1024 threads with 8 slots).
//
// Groupshared: gsP7 (p7_lds.hlsli), P7_LDS_WORDS = GROUP_SIZE * P7X_KPT_MAX words.
// Requires lane index == SV_GroupIndex % WAVE_SIZE (true on all known implementations for 1D groups,
// checked by the wave probe); otherwise the sort is not stable, which the CPU verification would catch.
#ifndef GPUSORT_P7_RADIX_HLSLI
#define GPUSORT_P7_RADIX_HLSLI

#include "common.hlsli"
#include "p7_lds.hlsli"

#define P7X_NW (GROUP_SIZE / WAVE_SIZE)
#if P7X_NW * WAVE_SIZE != GROUP_SIZE
#error "p7_radix: GROUP_SIZE must be a multiple of WAVE_SIZE"
#endif
// Keys per thread at most; GROUP_SIZE * P7X_KPT_MAX = P7_LDS_WORDS, so with kpt = P7X_KPT_MAX the
// waves own exactly the slots [0, P7_LDS_WORDS).
#define P7X_KPT_MAX (P7_LDS_WORDS / GROUP_SIZE)
#if P7X_KPT_MAX < 1 || P7X_KPT_MAX > 32 || P7X_KPT_MAX * GROUP_SIZE != P7_LDS_WORDS
#error "p7_radix: P7_LDS_WORDS / GROUP_SIZE must be 1..32 keys per thread"
#endif

// Digit lanes: groups of P7X_DL lanes, lane l of a group holds the P7X_DPL digits l * P7X_DPL + m.
// P7X_DL <= SHUFFLE_SPAN, so every per-lane-index shuffle stays inside the shuffle span.
#if SHUFFLE_SPAN >= 16
#define P7X_DL 16
#define P7X_DPL_BITS 0
#elif SHUFFLE_SPAN == 8
#define P7X_DL 8
#define P7X_DPL_BITS 1
#elif SHUFFLE_SPAN == 4
#define P7X_DL 4
#define P7X_DPL_BITS 2
#elif SHUFFLE_SPAN == 2
#define P7X_DL 2
#define P7X_DPL_BITS 3
#else
#error "p7_radix: the shuffle span must be at least 2 lanes"
#endif
#define P7X_DPL (1u << P7X_DPL_BITS)
#define P7X_G (SHUFFLE_SPAN / P7X_DL) // table-scan lane groups (1 or 2)

// Table: 16 words per wave at the top of the array (not aliased).
#define P7X_T_WORDS (16 * P7X_NW)
#if P7X_T_WORDS > P7_LDS_WORDS
#error "p7_radix: the digit table (16 words per wave) does not fit the groupshared array"
#endif
#define P7X_T_BASE (P7_LDS_WORDS - P7X_T_WORDS)
// Aliased table slots: digit l * DPL + m of wave w in slot waveBase + m * WAVE_SIZE + l, read back
// by lane l as key k = m, so kpt = P7X_KPT_MAX must be >= P7X_DPL.
#if P7X_KPT_MAX < P7X_DPL
#error "p7_radix: P7X_KPT_MAX must be >= digits per lane (aliased table)"
#endif

// Ballot masks: bit l = lane l.
#if WAVE_SIZE <= 32
typedef uint P7XMask;
P7XMask P7xBallot(bool b)
{
    return WaveActiveBallot(b).x;
}
uint P7xPopc(P7XMask m)
{
    return countbits(m);
}
#elif WAVE_SIZE == 64
typedef uint2 P7XMask;
P7XMask P7xBallot(bool b)
{
    return WaveActiveBallot(b).xy;
}
uint P7xPopc(P7XMask m)
{
    return countbits(m.x) + countbits(m.y);
}
#else
typedef uint4 P7XMask;
P7XMask P7xBallot(bool b)
{
    return WaveActiveBallot(b);
}
uint P7xPopc(P7XMask m)
{
    return countbits(m.x) + countbits(m.y) + countbits(m.z) + countbits(m.w);
}
#endif

// 32-bit mask of the lowest n bits, n = 0..32.
uint P7xLow32(uint n)
{
    return (n >= 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
}

// Mask of lanes 0..n-1, n = 0..WAVE_SIZE.
P7XMask P7xLowMask(uint n)
{
#if WAVE_SIZE <= 32
    return P7xLow32(n);
#elif WAVE_SIZE == 64
    return uint2(P7xLow32(n), P7xLow32(n - min(n, 32u)));
#else
    return uint4(P7xLow32(n), P7xLow32(n - min(n, 32u)), P7xLow32(n - min(n, 64u)), P7xLow32(n - min(n, 96u)));
#endif
}

// Lanes whose digit is d, from the ballots of the 4 digit bits (not masked by validity).
// ((d >> i) & 1) - 1 is 0 if bit i of d is set (keep the ballot) and ~0 if clear (complement it).
P7XMask P7xPeers(P7XMask b0, P7XMask b1, P7XMask b2, P7XMask b3, uint d)
{
    return (b0 ^ (((d >> 0) & 1u) - 1u)) & (b1 ^ (((d >> 1) & 1u) - 1u)) & (b2 ^ (((d >> 2) & 1u) - 1u)) &
           (b3 ^ (((d >> 3) & 1u) - 1u));
}

// Groupshared address of the table word of digit dd of wave w.
uint P7xTableAddr(bool alias, uint w, uint dd)
{
    const uint top = P7X_T_BASE + (w << 4) + dd;
    const uint own = w * (P7X_KPT_MAX << WAVE_BITS) + ((dd & (P7X_DPL - 1u)) << WAVE_BITS) + (dd >> P7X_DPL_BITS);
    return (alias ? own : top) & P7_LDS_MASK;
}

// Sorts gInput[offset, offset + count) into gOutput[offset, ...) by the high 16 bits, stable.
// count must be 1..P7_LDS_WORDS. Must be called by all threads of the group; count must be
// group-uniform.
void P7RadixSort(uint tid, uint offset, uint count)
{
    const uint lane = tid & (WAVE_SIZE - 1u);
    const uint wave = tid >> WAVE_BITS;
    const bool alias = count > P7X_T_BASE; // group-uniform
    const uint kpt = alias ? P7X_KPT_MAX : (count + GROUP_SIZE - 1u) / GROUP_SIZE; // 1..P7X_KPT_MAX
    const uint waveSlots = kpt << WAVE_BITS;
    const uint waveBase = wave * waveSlots;
    const uint activeWaves = (count + waveSlots - 1u) / waveSlots; // 1..P7X_NW
    const bool waveActive = wave < activeWaves;                    // wave-uniform
    const P7XMask below = P7xLowMask(lane);                        // lanes below this one
    const uint dl = lane & (P7X_DL - 1u);                          // digit lane
    const uint dlGroup = lane & ~(P7X_DL - 1u);                    // first lane of the digit group
    const uint tg = (lane & (SHUFFLE_SPAN - 1u)) / P7X_DL;         // table-scan group, 0..P7X_G-1

    uint v[P7X_KPT_MAX];
    [unroll]
    for (uint k = 0; k < P7X_KPT_MAX; ++k)
    {
        v[k] = 0;
    }
    [unroll]
    for (uint k = 0; k < P7X_KPT_MAX; ++k)
    {
        if (k >= kpt)
            break; // group-uniform
        const uint slot = waveBase + (k << WAVE_BITS) + lane;
        if (slot < count)
            v[k] = gInput[offset + slot];
    }

    [loop]
    for (uint pass = 0; pass < 4; ++pass)
    {
        const uint shift = 16u + 4u * pass;
        const bool lastPass = pass == 3u; // group-uniform

        // 1. counts of this wave's digits -> table
        if (waveActive)
        {
            uint cnt[P7X_DPL];
            [unroll]
            for (uint m = 0; m < P7X_DPL; ++m)
                cnt[m] = 0;
            [unroll]
            for (uint k = 0; k < P7X_KPT_MAX; ++k)
            {
                if (k >= kpt)
                    break; // group-uniform
                {
                    const uint sb = waveBase + (k << WAVE_BITS);
                    const P7XMask valid = P7xLowMask(count > sb ? min(count - sb, (uint)WAVE_SIZE) : 0u);
                    const uint d = (v[k] >> shift) & 15u;
                    const P7XMask b0 = P7xBallot((d & 1u) != 0);
                    const P7XMask b1 = P7xBallot((d & 2u) != 0);
                    const P7XMask b2 = P7xBallot((d & 4u) != 0);
                    const P7XMask b3 = P7xBallot((d & 8u) != 0);
                    [unroll]
                    for (uint m = 0; m < P7X_DPL; ++m)
                        cnt[m] += P7xPopc(valid & P7xPeers(b0, b1, b2, b3, (dl << P7X_DPL_BITS) + m));
                }
            }
            if (lane < P7X_DL)
            {
                [unroll]
                for (uint m = 0; m < P7X_DPL; ++m)
                    gsP7[P7xTableAddr(alias, wave, (dl << P7X_DPL_BITS) + m)] = cnt[m];
            }
        }
        GroupMemoryBarrierWithGroupSync(); // A

        // 2. table scan: run[m] = first slot of digit dl * DPL + m for this wave
        uint run[P7X_DPL];
        [unroll]
        for (uint m = 0; m < P7X_DPL; ++m)
            run[m] = 0;
        if (waveActive)
        {
            uint tot[P7X_DPL]; // digit count over all active waves
            uint exc[P7X_DPL]; // digit count over the waves below this one
            [unroll]
            for (uint m = 0; m < P7X_DPL; ++m)
            {
                tot[m] = 0;
                exc[m] = 0;
            }
            for (uint w2 = tg; w2 < activeWaves; w2 += P7X_G) // at most P7X_NW / P7X_G iterations
            {
                [unroll]
                for (uint m = 0; m < P7X_DPL; ++m)
                {
                    const uint x = gsP7[P7xTableAddr(alias, w2, (dl << P7X_DPL_BITS) + m)];
                    tot[m] += x;
                    exc[m] += (w2 < wave) ? x : 0u;
                }
            }
            [unroll]
            for (uint g = P7X_DL; g < SHUFFLE_SPAN; g <<= 1) // combine the table-scan groups
            {
                [unroll]
                for (uint m = 0; m < P7X_DPL; ++m)
                {
                    tot[m] += WaveReadLaneAt(tot[m], lane ^ g);
                    exc[m] += WaveReadLaneAt(exc[m], lane ^ g);
                }
            }
            // exclusive scan of the per-lane digit sums over the P7X_DL digit lanes of the group
            uint s = 0;
            [unroll]
            for (uint m = 0; m < P7X_DPL; ++m)
                s += tot[m];
            uint inc = s;
            [unroll]
            for (uint dd = 1; dd < P7X_DL; dd <<= 1)
            {
                const uint t = WaveReadLaneAt(inc, dlGroup | ((dl - dd) & (P7X_DL - 1u)));
                if (dl >= dd)
                    inc += t;
            }
            uint base = inc - s;
            [unroll]
            for (uint m = 0; m < P7X_DPL; ++m)
            {
                run[m] = base + exc[m];
                base += tot[m];
            }
        }

        // 3. scatter
        if (alias && !lastPass)
            GroupMemoryBarrierWithGroupSync(); // C: every wave has read the table before it is overwritten
        if (waveActive)
        {
            [unroll]
            for (uint k = 0; k < P7X_KPT_MAX; ++k)
            {
                if (k >= kpt)
                    break; // group-uniform
                {
                    const uint sb = waveBase + (k << WAVE_BITS);
                    const uint nValid = count > sb ? min(count - sb, (uint)WAVE_SIZE) : 0u; // wave-uniform
                    const P7XMask valid = P7xLowMask(nValid);
                    const uint d = (v[k] >> shift) & 15u;
                    const P7XMask b0 = P7xBallot((d & 1u) != 0);
                    const P7XMask b1 = P7xBallot((d & 2u) != 0);
                    const P7XMask b2 = P7xBallot((d & 4u) != 0);
                    const P7XMask b3 = P7xBallot((d & 8u) != 0);
                    const uint rank = P7xPopc(valid & below & P7xPeers(b0, b1, b2, b3, d));
                    // this wave's next slot for digit d: lane d >> DPL_BITS of the own digit group
                    const uint src = dlGroup | (d >> P7X_DPL_BITS);
                    uint o = 0;
                    [unroll]
                    for (uint m = 0; m < P7X_DPL; ++m)
                    {
                        const uint t = WaveReadLaneAt(run[m], src);
                        if ((d & (P7X_DPL - 1u)) == m)
                            o = t;
                    }
                    [unroll]
                    for (uint m = 0; m < P7X_DPL; ++m)
                        run[m] += P7xPopc(valid & P7xPeers(b0, b1, b2, b3, (dl << P7X_DPL_BITS) + m));
                    if (lane < nValid)
                    {
                        const uint pos = o + rank;
                        if (lastPass)
                            gOutput[offset + min(pos, count - 1u)] = v[k];
                        else
                            gsP7[pos & P7_LDS_MASK] = v[k];
                    }
                }
            }
        }
        if (!lastPass)
        {
            GroupMemoryBarrierWithGroupSync(); // B
            if (waveActive)
            {
                [unroll]
                for (uint k = 0; k < P7X_KPT_MAX; ++k)
                {
                    if (k >= kpt)
                        break; // group-uniform
                    const uint slot = waveBase + (k << WAVE_BITS) + lane;
                    if (slot < count)
                        v[k] = gsP7[slot & P7_LDS_MASK];
                }
            }
        }
    }
}

#endif
