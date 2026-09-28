# GPU Sorting Notes

Summary of the discussion on replacing/improving the D3D12 bitonic sort used for small per-frame sorts.

## 1. Bitonic sort: pass counts

Bitonic sort requires a power-of-two size, so pad the input to the next power of two with a sentinel that sorts last (`0xFFFFFFFF`, `FLT_MAX`). Example: 4800 elements → 8192 (2^13).

**Compare-exchange stages** for N = 2^k:

    stages = k(k+1)/2

- 1024 elements (k = 10): 55 stages
- 8192 elements (k = 13): 91 stages

**Dispatches** when a 1024-element block (B = 2^b) is sorted in groupshared memory per dispatch. With d = k − b:

    dispatches = 1 + d + d(d+1)/2

Per level s > b: (s − b) global dispatches for strides ≥ B, plus one groupshared dispatch for the remaining strides.

| N    | Block size | d | Dispatches |
|------|-----------:|--:|-----------:|
| 8192 | 1024       | 3 | 10         |
| 8192 | 2048       | 2 | 6          |

## 2. Parallelizing the indirect-args init shader

The original `sortBitonicIndirectInit` looped over (level, pass) serially. Passes are enumerated level-major, with level L having L+1 passes, so the passes before level L form the triangular number L(L+1)/2. For a flat pass index p:

    level = floor((sqrt(8p + 1) - 1) / 2)
    pass  = p - level(level + 1)/2

Each thread then handles one (passIndex, component) pair, e.g. `[numthreads(3, 32, 1)]` with `id.x` = component and `id.y` = pass index, dispatched as `(1, ceil(maxPasses / 32), 1)`.

Bugs noted in the original: a stray `}` and `passIndex` never incremented.

**Lookup-table variant:** store each pass index's level as one byte, packed 16 per `uint4`. 20 levels (210 passes) fit in 14 constant registers. Packing matters because each element of a `static const` array occupies a full 16-byte register in the immediate constant buffer (an unpacked `uint[210]` would use 210 registers).

```hlsl
uint sortPassLevel(uint passIndex)
{
	uint word = sortPassLevelTable[passIndex >> 4][(passIndex >> 2) & 3];
	return (word >> ((passIndex & 3) * 8)) & 0xFF;
}
```

**Design point:** if `maxPasses` is a fixed number of `ExecuteIndirect` commands, write `0` into slots beyond `sortPasses` so excess dispatches become no-ops instead of reusing stale args.

## 3. D3D12 limits

- **Threads per group:** hard limit of 1024 (`D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP`). No known change.
- **Groupshared memory:** historically 32 KB (28 KB for mesh shaders). **Shader Model 6.10** (Agility SDK 1.720-preview, April 2026) lifts this: query `MaxGroupSharedMemoryPerGroup` at runtime and declare usage with `[GroupSharedLimit(<bytes>)]` on the entry point. Shaders without the attribute are still validated against the old limit.
  - Still preview; AMD's preview driver supports only the default size for now.
  - Large LDS use reduces how many groups are resident per CU, so check occupancy.
- Cross-group synchronization: only guaranteed at dispatch boundaries (UAV barrier). Decoupled look-back works in practice; persistent-threads global barriers risk deadlock.
- Wave intrinsics (`WaveReadLaneAt` etc.) avoid LDS traffic and barriers for small strides.

## 4. Choosing a sort for the actual workload

**Workload:** ~10 sorts per frame, typically 2–500 elements, occasionally ~1.5k, max ~5k.

At these sizes cost is dominated by dispatch count, barriers and bubbles, not algorithmic complexity. Large-N sorts (Onesweep-style radix, FidelityFX Parallel Sort, CUB device radix sort) use multiple global passes and would be slower.

### Architecture: one group per sort, one dispatch

- 5k 32-bit keys = 20 KB, which fits in LDS, so every sort can run start to finish in a single group.
- Put per-sort descriptors (offset, count) in a buffer and dispatch 10 groups. Each group reads its own count; the size branch is group-uniform, so choosing an algorithm per size is free.
- Removes the multi-dispatch global passes and the indirect-args init shader.
- Latency is set by the largest sort. The GPU is mostly idle, so this suits async compute.
- Same idea as CUB's segmented sort: bucket segments by size (warp / block / multi-block).

### Algorithm per size tier (inside the group)

| Size               | Algorithm | Notes |
|--------------------|-----------|-------|
| ≤ wave (32/64)     | Wave-level bitonic in registers via `WaveReadLaneAt` | No LDS, no barriers |
| up to ~1–2k        | Rank sort | One barrier, LDS broadcast reads, no padding, stable with index tie-break |
| ~2–5k              | In-group bitonic with multiple elements per thread in registers | In-register swaps for small strides, wave shuffles within a wave, LDS only for large strides; far fewer barriers than 91 |
| (alternative)      | LDS radix sort, 4-bit digits | Fixed 8 passes for 32-bit keys regardless of N; rarely wins at these sizes |

Crossover points are hardware-dependent: time rank sort vs. in-group bitonic at 500, 1.5k and 5k.

**Rank sort sketch:**

```hlsl
groupshared uint gKeys[MAX_SORT_ELEMENTS];

// n = element count; keys loaded into gKeys, then:
GroupMemoryBarrierWithGroupSync();

for(uint i = tid; i < n; i += GROUP_SIZE)
{
	uint key  = gKeys[i];
	uint rank = 0;
	for(uint j = 0; j < n; ++j)
	{
		uint other = gKeys[j];
		rank += (other < key) || (other == key && j < i);
	}
	OutValues[outBase + rank] = InValues[inBase + i];
}
```

### Keys and payloads

- Pack key + index into one 32-bit word when precision allows: 5k elements needs 13 index bits, leaving 19 key bits (often enough for view depth). Keeps the sort keys-only and doubles LDS capacity.
- Rank sort only needs keys in LDS; payload is read from memory by index.
- Float keys: use the sign-flip trick to get order-preserving uints.

## 5. Handling overflow beyond LDS

For the rare sort that doesn't fit in LDS (target: 99.9% fit).

**Cost of spilling:** the scratch buffer stays in L2 at these sizes, so the penalty is mainly L2 latency (roughly an order of magnitude over LDS) and `DeviceMemoryBarrierWithGroupSync` instead of the group barrier. `globallycoherent` is not needed within a single group. The bigger limit is that one group runs on one CU.

| Approach                  | Behaviour when spilled |
|---------------------------|------------------------|
| Bitonic over global       | log²-many sweeps, each latency-bound; slow |
| Rank sort over global     | O(N²) L2 reads; unusable |
| Chunk sort + merge        | log(chunks) memory passes, bulk of work in LDS; degrades smoothly |

### Recommended: chunk sort in LDS, then merge in-group

1. If `n` fits in LDS: normal fast path.
2. Otherwise sort LDS-sized chunks one at a time with the fast-path code and write them to scratch.
3. Merge runs pairwise, ping-ponging between two scratch buffers. 2× capacity = 1 merge round, 4× = 2 rounds.

Use **merge path** partitioning so every thread merges a fixed output slice independently:

```hlsl
// For output position 'diag' in the merge of A[0..aLen) and B[0..bLen),
// return how many elements come from A. Stable: ties favour A.
uint MergePathSplit(uint diag, uint aBase, uint aLen, uint bBase, uint bLen)
{
	uint lo = diag > bLen ? diag - bLen : 0;
	uint hi = min(diag, aLen);
	while(lo < hi)
	{
		uint mid = (lo + hi) >> 1;
		if(Scratch[aBase + mid] <= Scratch[bBase + diag - 1 - mid])
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}
```

Optimization for later: load the A/B tiles for a block of output into LDS before merging (ModernGPU / CUB block merge style).

### Optional escape hatch: indirect fallback

For pathological counts (e.g. 20× capacity): the group skips sorting, appends its descriptor to an overflow list and increments the group count in an indirect args buffer. A following `ExecuteIndirect` runs a multi-group sort (e.g. the existing bitonic chain). Normally its args are zero, so the cost is an empty indirect dispatch plus a barrier.

## 6. Open items

- Benchmark crossover points (rank sort vs. in-group bitonic) on target GPUs.
- Decide key format: what is being sorted and how much key precision is needed (determines whether 32-bit key+index packing works).
- Implement chunk+merge overflow path first; add the indirect fallback only if counts far above capacity show up.
- Evaluate SM 6.10 variable groupshared memory once it's out of preview.
