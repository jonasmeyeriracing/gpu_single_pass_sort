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
