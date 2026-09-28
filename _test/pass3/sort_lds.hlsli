// The one groupshared array shared by bitonic_reg.hlsli, radix_sort.hlsli and rank_sort1.hlsli, so a
// shader that contains several of them (single_pass.hlsl) still uses exactly 32 KB.
#ifndef GPUSORT_SORT_LDS_HLSLI
#define GPUSORT_SORT_LDS_HLSLI

#include "common.hlsli"

// 8192 * 4 bytes = 32 KB = the entire D3D12 groupshared limit; no other groupshared allowed.
groupshared uint gsLds[MAX_SORT_SIZE];

#endif
