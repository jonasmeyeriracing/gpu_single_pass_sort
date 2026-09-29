// pass7: register / wave bitonic sort for any group size and a per-dispatch groupshared size.
// The algorithm and code of bitonic_reg_t.hlsli (pass3/pass4: layouts, transposes, directions,
// LDS swizzle, 32-lane virtual waves for wider waves; see bitonic_reg.hlsli for the description),
// with two changes:
//   - GROUP_SIZE may be 32..1024 (bitonic_reg_t: 256..1024), so a small-sort tier can run in a
//     group that matches its work (e.g. 128 threads for 129..512 elements at E = 4) instead of a
//     1024-thread group in which most threads only wait at the barriers;
//   - the groupshared array is gsP7 (p7_lds.hlsli), P7_LDS_WORDS words, instead of 8192 words.
//
//   P7BitonicSort<E, EB>(tid, offset, count), E = 1 << EB elements per thread (EB = 0..5),
//   count <= E * GROUP_SIZE and the sort padded to N = 2^n <= P7_LDS_WORDS.
//
// The sort is padded to N = 2^n, n = clamp(ceil(log2(count)), max(L, EB + WAVE_BITS), maxBits),
// L = EB + P7B_LANE_BITS, and runs on N / E threads (whole hardware waves). The shader that
// includes this must guarantee E * WAVE_SIZE <= P7_LDS_WORDS, E * WAVE_SIZE <= E * GROUP_SIZE and
// count <= min(E * GROUP_SIZE, P7_LDS_WORDS) (p7_sort.hlsl checks this at compile time).
//
// Requires lane index == SV_GroupIndex % WAVE_SIZE (true on all known implementations for 1D groups).
#ifndef GPUSORT_P7_BITONIC_HLSLI
#define GPUSORT_P7_BITONIC_HLSLI

#include "common.hlsli"
#include "p7_lds.hlsli"

#define P7B_MAX_BITS 13 // log2(MAX_SORT_SIZE)
#if GROUP_SIZE == 1024
#define P7B_GROUP_BITS 10
#elif GROUP_SIZE == 512
#define P7B_GROUP_BITS 9
#elif GROUP_SIZE == 256
#define P7B_GROUP_BITS 8
#elif GROUP_SIZE == 128
#define P7B_GROUP_BITS 7
#elif GROUP_SIZE == 64
#define P7B_GROUP_BITS 6
#elif GROUP_SIZE == 32
#define P7B_GROUP_BITS 5
#else
#error "p7_bitonic: GROUP_SIZE must be a power of two, 32..1024"
#endif
#if GROUP_SIZE < WAVE_SIZE
#error "p7_bitonic: GROUP_SIZE must be >= WAVE_SIZE"
#endif

// Lane bits of a layout: the shuffle span (at most 5; a wave64 runs as two 32-lane virtual waves).
#ifndef P7B_LANE_BITS
#define P7B_LANE_BITS SHUFFLE_SPAN_BITS
#endif
#if P7B_LANE_BITS > WAVE_BITS
#error "p7_bitonic: P7B_LANE_BITS must be <= WAVE_BITS"
#endif

// Swizzled groupshared address of logical index i (as BrLdsAddr). The swizzle only changes bits
// 0..4, so for i < N (N a power of two <= P7_LDS_WORDS, P7_LDS_WORDS >= 64) it stays below
// max(N, 32) <= P7_LDS_WORDS; the mask only makes that explicit.
uint P7bLdsAddr(uint i)
{
    return (i ^ (((i >> 5) ^ (i >> 10)) & 31u)) & P7_LDS_MASK;
}

// Logical index of this thread's register 0 in the layout with window base B. Register r holds
// index P7bBaseIndex(...) | (r << B).
template <uint EB>
uint P7bBaseIndex(uint lane, uint wave, uint B)
{
    const uint lowMask = (1u << B) - 1u;
    return (wave & lowMask) | (lane << (B + EB)) | ((wave >> B) << (B + EB + P7B_LANE_BITS));
}

// Start of level s: XOR every element with (bit s-1 ^ bit s) of its index (bit 0 counts as 0 for
// s = 1), so that elements of descending blocks of this level are stored complemented.
template <uint E>
void P7bFlip(inout uint v[E], uint base, uint B, uint s)
{
    const uint lowBit = (s == 1) ? 31u : s - 1; // bit 31 of an index is always 0
    [unroll]
    for (uint r = 0; r < E; ++r)
    {
        const uint i = base | (r << B);
        const uint m = ((i >> lowBit) ^ (i >> s)) & 1u;
        v[r] ^= 0u - m;
    }
}

// Register stage on register bit P (compile-time after unrolling): v[r] = min, v[r | 1 << P] = max.
template <uint E>
void P7bRegStage(inout uint v[E], uint P)
{
    [unroll]
    for (uint r = 0; r < E; ++r)
    {
        if ((r & (1u << P)) == 0)
        {
            const uint r1 = r | (1u << P);
            const uint a = v[r];
            const uint c = v[r1];
            v[r] = min(a, c);
            v[r1] = max(a, c);
        }
    }
}

// Lane stage on lane bit q (< P7B_LANE_BITS): exchange with hardware lane ^ (1 << q), which is in
// the same virtual wave; the lane with bit q clear keeps the min.
template <uint E>
void P7bLaneStage(inout uint v[E], uint q, uint hwLane)
{
    const uint partner = hwLane ^ (1u << q);
    const bool upper = ((hwLane >> q) & 1u) != 0;
    [unroll]
    for (uint r = 0; r < E; ++r)
    {
        const uint other = WaveReadLaneAt(v[r], partner);
        v[r] = upper ? max(v[r], other) : min(v[r], other);
    }
}

// Sorts gInput[offset, offset + count) into gOutput[offset, ...). count must be 1..E * GROUP_SIZE
// (and <= P7_LDS_WORDS). Must be called by all threads of the group (contains barriers); count
// must be group-uniform. Same control flow as BitonicRegSortT (bitonic_reg_t.hlsli).
template <uint E, uint EB>
void P7BitonicSort(uint tid, uint offset, uint count)
{
    const uint L = EB + P7B_LANE_BITS; // local bits
    const uint maxBits = min(min((uint)P7B_MAX_BITS, EB + P7B_GROUP_BITS), (uint)P7_LDS_BITS);
    uint n = (count <= 1) ? 0 : firstbithigh(count - 1) + 1; // ceil(log2(count))
    n = clamp(n, max(L, EB + WAVE_BITS), maxBits);
    const uint numThreads = 1u << (n - EB); // multiple of WAVE_SIZE, <= GROUP_SIZE
    const bool active = tid < numThreads;   // wave-uniform
    const uint lane = tid & ((1u << P7B_LANE_BITS) - 1u); // lane in the virtual wave
    const uint wave = tid >> P7B_LANE_BITS;                // virtual wave
    const uint hwLane = tid & (WAVE_SIZE - 1u);

    uint B = 0;
    uint base = P7bBaseIndex<EB>(lane, wave, 0);

    uint v[E];
    [unroll]
    for (uint r = 0; r < E; ++r)
    {
        const uint i = base | r;
        v[r] = 0xFFFFFFFFu;
        if (active && i < count)
            v[r] = gInput[offset + i];
    }

    // Levels s = 1..n (n <= 13).
    for (uint s = 1; s <= n; ++s)
    {
        if (active)
            P7bFlip<E>(v, base, B, s);
        // Runs; each consumes at least one stage bit, so at most s <= 13 of them.
        uint top = s; // bits >= top are done for this level
        for (uint run = 0; run < P7B_MAX_BITS && top > 0; ++run)
        {
            const uint b = top - 1;
            if (b < B || b >= B + L) // group-uniform
            {
                const uint newB = (b + 1 >= L) ? b + 1 - L : 0;
                if (active)
                {
                    [unroll]
                    for (uint r = 0; r < E; ++r)
                        gsP7[P7bLdsAddr(base | (r << B))] = v[r];
                }
                GroupMemoryBarrierWithGroupSync();
                B = newB;
                base = P7bBaseIndex<EB>(lane, wave, B);
                if (active)
                {
                    [unroll]
                    for (uint r = 0; r < E; ++r)
                        v[r] = gsP7[P7bLdsAddr(base | (r << B))];
                }
            }
            const uint pTop = b - B; // local position of the run's first stage, < L
            if (active)
            {
                // lane stages p = pTop .. EB (at most P7B_LANE_BITS of them)
                for (uint p = pTop; p >= EB && p < L; --p)
                    P7bLaneStage<E>(v, p - EB, hwLane);
                // register stages P = min(pTop, EB - 1) .. 0
                [unroll]
                for (uint k = 0; k < EB; ++k)
                {
                    const uint P = EB - 1 - k;
                    [branch]
                    if (P <= pTop)
                        P7bRegStage<E>(v, P);
                }
            }
            top = B;
        }
    }

    // The last stage (b = 0) always runs in a B = 0 layout. After level n no value is complemented.
    if (active)
    {
        [unroll]
        for (uint r = 0; r < E; ++r)
        {
            const uint i = base | (r << B);
            if (i < count)
                gOutput[offset + i] = v[r];
        }
    }
}

#endif
