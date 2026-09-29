// pass7: the one groupshared array of the pass7 shaders (p7_rank.hlsli, p7_bitonic.hlsli,
// p7_radix.hlsli), sized per dispatch instead of always 32 KB (sort_lds.hlsli).
//
// Why: the groupshared size of a shader is fixed at compile time and is reserved for every group
// of the dispatch, whatever path the group takes. On GPUs with little LDS per CU / subslice (an
// Intel Xe-LP subslice has 64 KB of SLM: 2 groups of 32 KB) the 32 KB array limits how many groups
// of a small-sort tier can run at the same time. A dispatch that only handles sorts up to
// MAX_COUNT needs P7_LDS_WORDS = the next power of two >= MAX_COUNT (p7_sort.hlsl computes it).
#ifndef GPUSORT_P7_LDS_HLSLI
#define GPUSORT_P7_LDS_HLSLI

#include "common.hlsli"

#ifndef P7_LDS_WORDS
#error "p7_lds: P7_LDS_WORDS must be defined by the including shader (power of two, 64..8192)"
#endif
#if P7_LDS_WORDS == 64
#define P7_LDS_BITS 6
#elif P7_LDS_WORDS == 128
#define P7_LDS_BITS 7
#elif P7_LDS_WORDS == 256
#define P7_LDS_BITS 8
#elif P7_LDS_WORDS == 512
#define P7_LDS_BITS 9
#elif P7_LDS_WORDS == 1024
#define P7_LDS_BITS 10
#elif P7_LDS_WORDS == 2048
#define P7_LDS_BITS 11
#elif P7_LDS_WORDS == 4096
#define P7_LDS_BITS 12
#elif P7_LDS_WORDS == 8192
#define P7_LDS_BITS 13
#else
#error "p7_lds: P7_LDS_WORDS must be a power of two in 64..8192"
#endif
#define P7_LDS_MASK (P7_LDS_WORDS - 1u)

// P7_LDS_WORDS * 4 bytes (256 B .. 32 KB); no other groupshared array in a pass7 shader.
groupshared uint gsP7[P7_LDS_WORDS];

#endif
