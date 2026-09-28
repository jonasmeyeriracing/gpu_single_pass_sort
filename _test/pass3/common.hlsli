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
