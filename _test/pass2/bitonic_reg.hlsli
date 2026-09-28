// Register / wave bitonic sort: one group per sort, BR_ELEMS elements per thread in registers.
//
// The sort is padded to N = 2^n elements (next power of two >= count, at least one full wave of
// BR_ELEMS each, at most 8192) with 0xFFFFFFFF, and runs on N / BR_ELEMS threads (whole waves; the
// other threads of the group only take part in the barriers). Compares full 32-bit values.
//
// Layouts. Every element has a logical index i (n bits). A layout with window base B puts the
// L = BR_ELEM_BITS + BR_LANE_BITS index bits [B, B + L) "local" to a wave (BR_LANE_BITS defaults to
// WAVE_BITS; 0 gives an LDS-only variant without wave intrinsics):
//   bits [B, B + BR_ELEM_BITS)         = register index r (0..BR_ELEMS-1)
//   bits [B + BR_ELEM_BITS, B + L)     = lane index
//   the other n - L bits (below B, and from B + L up) = wave index (low bits of the wave index
//   go below B, the rest above the window)
// A compare-exchange stage on index bit b is
//   - in registers (no communication) if b is a register bit,
//   - a WaveReadLaneAt with lane ^ (1 << q) (no barrier) if b is a lane bit,
//   - otherwise not possible in this layout: the sort then changes layout ("transpose") by writing
//     all elements to groupshared memory at their logical index, one barrier, and reading them
//     back in the layout whose window is the L bits ending at b (base b + 1 - L, or 0). All of the
//     next L - 1 stages are then local too.
// Stage bits within a level go from high to low, so each level s > L needs two transposes (window
// [s - L, s - 1], then back to [0, L)); levels 1..L need none (the initial load already uses the
// B = 0 layout). With BR_ELEMS = 8 and 32-lane waves (L = 8) that is 2 * (n - 8) barriers:
// 10 at N = 8192 (pass0: 91), 8 at 4096, 6 at 2048, 4 at 1024, 0 up to 256. BR_ELEMS = 16 (L = 9):
// 8/6/4/2 (0 up to 512); BR_ELEMS = 32 (L = 10): 6/4/2/0.
//
// Why a transpose needs only one barrier (write, sync, read) and no barrier before the next write:
// in any layout every thread owns a fixed set of logical indices, and a transpose writes a thread's
// elements to exactly the addresses of the indices it owns in the current layout, i.e. the
// addresses it read itself in the previous transpose (or none, for the first one). No other
// thread reads those addresses in between, so there is no write-after-read hazard across threads.
//
// Directions: instead of a per-element ascending/descending decision in every stage, an element
// with logical index i is stored complemented (~v) while it is in a descending block, i.e. while
// bit s of i is set during level s. min/max of complemented values is the complemented max/min, so
// every compare-exchange is then a plain ascending one: register stage v[r] = min, v[r | P] = max;
// lane stage: min in the lane with the stage bit clear, max in the other. At the start of level s
// each element is XORed with (bit s-1 ^ bit s) of its index (bit 0 counts as 0 for level 1); after
// the last level (bit n = 0) every value is uncomplemented again.
//
// Groupshared addresses are swizzled, addr = i ^ (((i >> 5) ^ (i >> 10)) & 31): the bank (addr &
// 31) of index bit k is bit (k mod 5), so any 5 consecutive index bits spread over all 32 banks
// and every layout's lane bits (5 consecutive bits for 32-lane waves) access LDS conflict-free.
// The swizzle only changes bits 0..4 as a function of bits >= 5, so it is a permutation of
// [0, 2^n) for every n (the identity for n <= 5).
//
// Control flow (see BitonicRegSort): per level, "runs" of stages; a run is (optional transpose) +
// a runtime loop over its lane stages + one fully unrolled block of register stages. A first
// version with a runtime-selected stage per iteration (several uniform branches per stage) was
// ~30% slower at 8192 and ~4 us slower for single-wave sorts on the RTX 5080; fully unrolling the
// whole sort per n made DXC take minutes per shader.
//
// Requires lane index == SV_GroupIndex % WAVE_SIZE (true on all known implementations for 1D groups).
#ifndef GPUSORT_BITONIC_REG_HLSLI
#define GPUSORT_BITONIC_REG_HLSLI

#include "common.hlsli"
#include "sort_lds.hlsli"

#ifndef BR_ELEMS
#define BR_ELEMS 8
#endif
#if BR_ELEMS == 8
#define BR_ELEM_BITS 3
#elif BR_ELEMS == 16
#define BR_ELEM_BITS 4
#elif BR_ELEMS == 32
#define BR_ELEM_BITS 5
#else
#error "BR_ELEMS must be 8, 16 or 32"
#endif

// Number of lane bits in a layout (default: the whole wave). BR_LANE_BITS = 0 gives an LDS-only
// variant without any wave intrinsics: every non-register stage bit goes through a transpose.
#ifndef BR_LANE_BITS
#define BR_LANE_BITS WAVE_BITS
#endif
#if BR_LANE_BITS > WAVE_BITS
#error "bitonic_reg: BR_LANE_BITS must be <= WAVE_BITS"
#endif
#define BR_LOCAL_BITS (BR_ELEM_BITS + BR_LANE_BITS)
#define BR_MAX_BITS 13 // log2(MAX_SORT_SIZE)

#if (MAX_SORT_SIZE / BR_ELEMS) > GROUP_SIZE
#error "bitonic_reg: GROUP_SIZE * BR_ELEMS must be >= MAX_SORT_SIZE"
#endif
#if BR_LOCAL_BITS > BR_MAX_BITS
#error "bitonic_reg: BR_ELEMS * WAVE_SIZE exceeds MAX_SORT_SIZE"
#endif

static const uint kBrPad = 0xFFFFFFFFu;

// Swizzled groupshared address of logical index i (see above); always < MAX_SORT_SIZE.
uint BrLdsAddr(uint i)
{
    return (i ^ (((i >> 5) ^ (i >> 10)) & 31u)) & (MAX_SORT_SIZE - 1u);
}

// Logical index of this thread's register 0 in the layout with window base B. Register r holds
// index BrBaseIndex(...) | (r << B).
uint BrBaseIndex(uint lane, uint wave, uint B)
{
    const uint lowMask = (1u << B) - 1u;
    return (wave & lowMask) | (lane << (B + BR_ELEM_BITS)) | ((wave >> B) << (B + BR_LOCAL_BITS));
}

// Start of level s: XOR every element with (bit s-1 ^ bit s) of its index (bit 0 counts as 0 for
// s = 1), so that elements of descending blocks of this level are stored complemented.
void BrFlip(inout uint v[BR_ELEMS], uint base, uint B, uint s)
{
    const uint lowBit = (s == 1) ? 31u : s - 1; // bit 31 of an index is always 0
    [unroll]
    for (uint r = 0; r < BR_ELEMS; ++r)
    {
        const uint i = base | (r << B);
        const uint m = ((i >> lowBit) ^ (i >> s)) & 1u;
        v[r] ^= 0u - m;
    }
}

// Register stage on register bit P (compile-time after unrolling): v[r] = min, v[r | 1 << P] = max.
// Always ascending (see "Directions").
void BrRegStage(inout uint v[BR_ELEMS], uint P)
{
    [unroll]
    for (uint r = 0; r < BR_ELEMS; ++r)
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

// Lane stage on lane bit q: exchange with lane ^ (1 << q) (always inside the same wave); the lane
// with bit q clear keeps the min, the other the max.
void BrLaneStage(inout uint v[BR_ELEMS], uint q, uint hwLane)
{
    const uint partner = hwLane ^ (1u << q);
    const bool upper = ((hwLane >> q) & 1u) != 0;
    [unroll]
    for (uint r = 0; r < BR_ELEMS; ++r)
    {
        const uint other = WaveReadLaneAt(v[r], partner);
        v[r] = upper ? max(v[r], other) : min(v[r], other);
    }
}

// Sorts gInput[offset, offset + count) into gOutput[offset, ...). count must be 1..8192. Must be
// called by all threads of the group (contains barriers); count must be group-uniform.
//
// Control flow: a level's stages are processed in "runs". A run starts at the level's highest
// unprocessed bit b; if b is not local, the sort transposes to the window whose top bit is b. The
// run then does every stage from b down to the window base B: first the lane stages (runtime
// loop), then all register stages below them as one fully unrolled block. The next run starts at
// B - 1. So the only per-stage control flow is the lane-stage loop.
void BitonicRegSort(uint tid, uint offset, uint count)
{
    uint n = (count <= 1) ? 0 : firstbithigh(count - 1) + 1; // ceil(log2(count))
    n = clamp(n, (uint)BR_LOCAL_BITS, (uint)BR_MAX_BITS);
    const uint numThreads = 1u << (n - BR_ELEM_BITS); // multiple of 2^BR_LANE_BITS, <= GROUP_SIZE
    const bool active = tid < numThreads;             // wave-uniform if BR_LANE_BITS == WAVE_BITS
    const uint lane = tid & ((1u << BR_LANE_BITS) - 1u);
    const uint wave = tid >> BR_LANE_BITS;
    const uint hwLane = tid & (WAVE_SIZE - 1u);

    uint B = 0;
    uint base = BrBaseIndex(lane, wave, 0);

    uint v[BR_ELEMS];
    [unroll]
    for (uint r = 0; r < BR_ELEMS; ++r)
    {
        const uint i = base | r;
        v[r] = kBrPad;
        if (active && i < count)
            v[r] = gInput[offset + i];
    }

    // Levels s = 1..n (n <= 13).
    for (uint s = 1; s <= n; ++s)
    {
        if (active)
            BrFlip(v, base, B, s);
        // Runs; each consumes at least one stage bit, so at most s <= 13 of them.
        uint top = s; // bits >= top are done for this level
        for (uint run = 0; run < BR_MAX_BITS && top > 0; ++run)
        {
            const uint b = top - 1;
            if (b < B || b >= B + BR_LOCAL_BITS) // group-uniform
            {
                const uint newB = (b + 1 >= BR_LOCAL_BITS) ? b + 1 - BR_LOCAL_BITS : 0;
                if (active)
                {
                    [unroll]
                    for (uint r = 0; r < BR_ELEMS; ++r)
                        gsLds[BrLdsAddr(base | (r << B))] = v[r];
                }
                GroupMemoryBarrierWithGroupSync();
                B = newB;
                base = BrBaseIndex(lane, wave, B);
                if (active)
                {
                    [unroll]
                    for (uint r = 0; r < BR_ELEMS; ++r)
                        v[r] = gsLds[BrLdsAddr(base | (r << B))];
                }
            }
            const uint pTop = b - B; // local position of the run's first stage, < BR_LOCAL_BITS
            if (active)
            {
                // lane stages p = pTop .. BR_ELEM_BITS
                for (uint p = pTop; p >= BR_ELEM_BITS && p < BR_LOCAL_BITS; --p)
                    BrLaneStage(v, p - BR_ELEM_BITS, hwLane);
                // register stages P = min(pTop, BR_ELEM_BITS - 1) .. 0
                [unroll]
                for (uint k = 0; k < BR_ELEM_BITS; ++k)
                {
                    const uint P = BR_ELEM_BITS - 1 - k;
                    [branch]
                    if (P <= pTop)
                        BrRegStage(v, P);
                }
            }
            top = B;
        }
    }

    // The last stage (b = 0) always runs in a B = 0 layout. After level n no value is complemented.
    if (active)
    {
        [unroll]
        for (uint r = 0; r < BR_ELEMS; ++r)
        {
            const uint i = base | (r << B);
            if (i < count)
                gOutput[offset + i] = v[r];
        }
    }
}

#endif
