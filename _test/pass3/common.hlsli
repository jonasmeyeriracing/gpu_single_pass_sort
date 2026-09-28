// Shared bindings for all sort shaders. Must match the root signature in src/Benchmark.cpp.
#ifndef GPUSORT_COMMON_HLSLI
#define GPUSORT_COMMON_HLSLI

#define MAX_SORT_SIZE 8192

#ifndef GROUP_SIZE
#define GROUP_SIZE 1024
#endif

// Size tier handled by this dispatch (inclusive); sorts outside it early-out.
#ifndef MIN_COUNT
#define MIN_COUNT 1
#endif
#ifndef MAX_COUNT
#define MAX_COUNT MAX_SORT_SIZE
#endif

// Wave size the shader is compiled for. The framework passes -D WAVE_SIZE=<WaveLaneCountMin> (or
// --wave-size), and -D WAVE_SIZE_REQUIRED=1 when the device supports a range of wave sizes (or
// --wave-size is given): then entry points that depend on WAVE_SIZE must carry WAVE_SIZE_ATTR so
// the driver really runs them with that wave size. Must be a power of two, 4..128.
#ifndef WAVE_SIZE
#define WAVE_SIZE 32
#endif
#if WAVE_SIZE == 4
#define WAVE_BITS 2
#elif WAVE_SIZE == 8
#define WAVE_BITS 3
#elif WAVE_SIZE == 16
#define WAVE_BITS 4
#elif WAVE_SIZE == 32
#define WAVE_BITS 5
#elif WAVE_SIZE == 64
#define WAVE_BITS 6
#elif WAVE_SIZE == 128
#define WAVE_BITS 7
#else
#error "WAVE_SIZE must be a power of two in 4..128"
#endif
#if defined(WAVE_SIZE_REQUIRED) && WAVE_SIZE_REQUIRED
#define WAVE_SIZE_ATTR [WaveSize(WAVE_SIZE)]
#else
#define WAVE_SIZE_ATTR
#endif

// Shuffle span (pass4): a WaveReadLaneAt whose source lane differs per lane only ever reads a lane
// in the same aligned group of SHUFFLE_SPAN = 2^SHUFFLE_SPAN_BITS lanes, at most 32. On the AMD RX
// 7900 XTX (driver 32.0.11037.4004) with [WaveSize(64)], the pass2 radix sort, whose shuffle scans
// read across the two 32-lane halves of a wave64, returned wrong results, while every wave32 run
// passed, and a CPU emulation shows the shader logic is correct for wave64. The likely cause (not
// confirmed) is that such shuffles only work inside each 32-lane half there (RDNA's ds_bpermute
// permutes within 32 lanes in wave64 mode). So wider waves use wave intrinsics (scans) or narrower
// "virtual waves" (bitonic, radix table scan) instead; see _test/pass4/notes.md. For
// WAVE_SIZE <= 32 the default changes nothing. SHUFFLE_SPAN_TEST = 1 sets the span to half the wave
// (as for wave64), to run those code paths on 32-lane hardware or WARP.
#ifndef SHUFFLE_SPAN_BITS
#if defined(SHUFFLE_SPAN_TEST) && SHUFFLE_SPAN_TEST
#define SHUFFLE_SPAN_BITS (WAVE_BITS - 1)
#elif WAVE_BITS < 5
#define SHUFFLE_SPAN_BITS WAVE_BITS
#else
#define SHUFFLE_SPAN_BITS 5
#endif
#endif
#if SHUFFLE_SPAN_BITS < 0 || SHUFFLE_SPAN_BITS > WAVE_BITS
#error "SHUFFLE_SPAN_BITS must be 0..WAVE_BITS"
#endif
#define SHUFFLE_SPAN (1u << SHUFFLE_SPAN_BITS)

cbuffer SortConstants : register(b0)
{
    uint gNumSorts;
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

// Per sort: x = offset (elements, 64-aligned), y = count (0..8192)
StructuredBuffer<uint2> gSortDescs : register(t0);
// Elements: (key16 << 16) | payload16
StructuredBuffer<uint> gInput : register(t1);
RWStructuredBuffer<uint> gOutput : register(u0);

#endif
