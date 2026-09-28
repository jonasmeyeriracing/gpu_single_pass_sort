// Register / wave bitonic sort as HLSL 2021 templates (pass3): the algorithm of bitonic_reg.hlsli
// (see there for layouts, transposes, directions and the LDS swizzle), with the number of elements
// per thread as a template parameter, so that one shader can contain several configurations and
// E = 1 and 2 are possible:
//
//   BitonicRegSortT<E, EB>(tid, offset, count), E = 1 << EB elements per thread (EB = 0..4),
//   count <= E * GROUP_SIZE.
//
// L = EB + BRT_LANE_BITS index bits are local to a wave (EB register bits + the lane bits;
// BRT_LANE_BITS = WAVE_BITS, at most SHUFFLE_SPAN_BITS = 5: pass4 splits a wave64 into two 32-lane
// "virtual waves", see common.hlsli). The sort is padded to N = 2^n, n = clamp(ceil(log2(count)),
// max(L, EB + WAVE_BITS), 13), and runs on N / E threads (whole hardware waves). Barriers (32
// lanes): 2 * (n - L) for n > L: E = 1: 10 at N = 1024; E = 2: 10 at 2048, 8 at 1024; E = 4: 10 at
// 4096, 8 at 2048, 6 at 1024; E = 8: as bitonic_reg.hlsli (10 at 8192, 6 at 2048).
// With E = 1 there are no register stages (every stage is a lane stage or goes through LDS).
// The code of BitonicRegSortT<8, 3> is the code of BitonicRegSort with BR_ELEMS = 8.
//
// Requires lane index == SV_GroupIndex % WAVE_SIZE (true on all known implementations for 1D groups).
#ifndef GPUSORT_BITONIC_REG_T_HLSLI
#define GPUSORT_BITONIC_REG_T_HLSLI

#include "common.hlsli"
#include "sort_lds.hlsli"

#define BRT_MAX_BITS 13 // log2(MAX_SORT_SIZE)
#if GROUP_SIZE == 1024
#define BRT_GROUP_BITS 10
#elif GROUP_SIZE == 512
#define BRT_GROUP_BITS 9
#elif GROUP_SIZE == 256
#define BRT_GROUP_BITS 8
#else
#error "bitonic_reg_t: GROUP_SIZE must be 256, 512 or 1024"
#endif

// Lane bits of a layout (see above). Smaller values (down to 0 = LDS only) test the virtual-wave
// path on narrower hardware.
#ifndef BRT_LANE_BITS
#define BRT_LANE_BITS SHUFFLE_SPAN_BITS
#endif
#if BRT_LANE_BITS > WAVE_BITS
#error "bitonic_reg_t: BRT_LANE_BITS must be <= WAVE_BITS"
#endif

// Swizzled groupshared address of logical index i (same as BrLdsAddr); always < MAX_SORT_SIZE.
uint BrtLdsAddr(uint i)
{
    return (i ^ (((i >> 5) ^ (i >> 10)) & 31u)) & (MAX_SORT_SIZE - 1u);
}

// Logical index of this thread's register 0 in the layout with window base B. Register r holds
// index BrtBaseIndex(...) | (r << B).
template <uint EB>
uint BrtBaseIndex(uint lane, uint wave, uint B)
{
    const uint lowMask = (1u << B) - 1u;
    return (wave & lowMask) | (lane << (B + EB)) | ((wave >> B) << (B + EB + BRT_LANE_BITS));
}

// Start of level s: XOR every element with (bit s-1 ^ bit s) of its index (bit 0 counts as 0 for
// s = 1), so that elements of descending blocks of this level are stored complemented.
template <uint E>
void BrtFlip(inout uint v[E], uint base, uint B, uint s)
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
void BrtRegStage(inout uint v[E], uint P)
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

// Lane stage on lane bit q (< BRT_LANE_BITS): exchange with hardware lane ^ (1 << q), which is in
// the same virtual wave; the lane with bit q clear keeps the min.
template <uint E>
void BrtLaneStage(inout uint v[E], uint q, uint hwLane)
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
// (and <= 8192). Must be called by all threads of the group (contains barriers); count must be
// group-uniform. Same control flow as BitonicRegSort (bitonic_reg.hlsli).
template <uint E, uint EB>
void BitonicRegSortT(uint tid, uint offset, uint count)
{
    const uint L = EB + BRT_LANE_BITS; // local bits
    const uint maxBits = min((uint)BRT_MAX_BITS, EB + BRT_GROUP_BITS);
    uint n = (count <= 1) ? 0 : firstbithigh(count - 1) + 1; // ceil(log2(count))
    n = clamp(n, max(L, EB + WAVE_BITS), maxBits);
    const uint numThreads = 1u << (n - EB); // multiple of WAVE_SIZE, <= GROUP_SIZE
    const bool active = tid < numThreads;   // wave-uniform
    const uint lane = tid & ((1u << BRT_LANE_BITS) - 1u); // lane in the virtual wave
    const uint wave = tid >> BRT_LANE_BITS;                // virtual wave
    const uint hwLane = tid & (WAVE_SIZE - 1u);

    uint B = 0;
    uint base = BrtBaseIndex<EB>(lane, wave, 0);

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
            BrtFlip<E>(v, base, B, s);
        // Runs; each consumes at least one stage bit, so at most s <= 13 of them.
        uint top = s; // bits >= top are done for this level
        for (uint run = 0; run < BRT_MAX_BITS && top > 0; ++run)
        {
            const uint b = top - 1;
            if (b < B || b >= B + L) // group-uniform
            {
                const uint newB = (b + 1 >= L) ? b + 1 - L : 0;
                if (active)
                {
                    [unroll]
                    for (uint r = 0; r < E; ++r)
                        gsLds[BrtLdsAddr(base | (r << B))] = v[r];
                }
                GroupMemoryBarrierWithGroupSync();
                B = newB;
                base = BrtBaseIndex<EB>(lane, wave, B);
                if (active)
                {
                    [unroll]
                    for (uint r = 0; r < E; ++r)
                        v[r] = gsLds[BrtLdsAddr(base | (r << B))];
                }
            }
            const uint pTop = b - B; // local position of the run's first stage, < L
            if (active)
            {
                // lane stages p = pTop .. EB (at most WAVE_BITS of them)
                for (uint p = pTop; p >= EB && p < L; --p)
                    BrtLaneStage<E>(v, p - EB, hwLane);
                // register stages P = min(pTop, EB - 1) .. 0
                [unroll]
                for (uint k = 0; k < EB; ++k)
                {
                    const uint P = EB - 1 - k;
                    [branch]
                    if (P <= pTop)
                        BrtRegStage<E>(v, P);
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
