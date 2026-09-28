// Wave-level scans built from WaveReadLaneAt shuffles (Hillis-Steele, WAVE_BITS steps).
//
// pass2 measured WavePrefixSum at roughly 0.25 us (RTX 5080) / 0.35 us (RTX 2060) per call in a
// 1024-thread group, i.e. hundreds of cycles, while WaveReadLaneAt shuffles are cheap. These helpers
// need every lane of the wave to be active (true where they are used). Correct for any
// power-of-two WAVE_SIZE 4..128.
//
// pass4: the shuffles cross lanes up to WAVE_SIZE / 2 apart. If the wave is wider than the shuffle
// span (SHUFFLE_SPAN, common.hlsli: wave64 on AMD), or WAVE_SCAN_INTRINSICS = 1, the helpers use
// WavePrefixSum / WaveActiveBitOr instead (same results).
#ifndef GPUSORT_WAVE_SCAN_HLSLI
#define GPUSORT_WAVE_SCAN_HLSLI

#include "common.hlsli"

#ifndef WAVE_SCAN_INTRINSICS
#if SHUFFLE_SPAN < WAVE_SIZE
#define WAVE_SCAN_INTRINSICS 1
#else
#define WAVE_SCAN_INTRINSICS 0
#endif
#endif

// Inclusive prefix sum of x over lanes 0..lane. 'lane' must be this thread's lane index.
uint WaveInclusiveSumShfl(uint x, uint lane)
{
#if WAVE_SCAN_INTRINSICS
    return WavePrefixSum(x) + x;
#else
    [unroll]
    for (uint d = 1; d < WAVE_SIZE; d <<= 1)
    {
        const uint t = WaveReadLaneAt(x, (lane - d) & (WAVE_SIZE - 1u)); // always a valid lane
        if (lane >= d)
            x += t;
    }
    return x;
#endif
}

// Bitwise OR of x over all lanes of the wave (xor butterfly, WAVE_BITS steps); every lane gets it.
uint WaveOrShfl(uint x, uint lane)
{
#if WAVE_SCAN_INTRINSICS
    return WaveActiveBitOr(x);
#else
    [unroll]
    for (uint d = 1; d < WAVE_SIZE; d <<= 1)
        x |= WaveReadLaneAt(x, lane ^ d);
    return x;
#endif
}

#endif
