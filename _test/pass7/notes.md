# pass7: low-end / integrated GPUs (Ryzen iGPU, Intel UHD 770)

Goal: make the sort faster on GPUs with few CUs / subslices, where the portable best
`s1_rank512_bitreg2048_radix` takes ~260 µs (Ryzen iGPU) / ~500 µs (UHD 770) for 20 × 8192 and
39 / 72 µs for realistic_mix, vs 18-20 µs / 10 µs on the RTX 5080 and RX 7900 XTX. This pass
cannot run on those GPUs: it builds a set of variants that each test one hypothesis, validates
them (WARP, emulation, RTX 5080) and measures them on the 5080. The user runs the set on the AMD
machine (7900 XTX + Ryzen iGPU) and the Intel machine; the next pass reads those results.

No framework change. New shaders only (`shaders/p7_*`, copied here), and the pass2 reference
shaders (unchanged copies) for the anchors.

## Contents

- `algorithms.txt`: the 21-algorithm set to run on the other machines (the full-run set).
- `algorithms_smoke.txt`: what the hardware smoke ran: the 21 + 3 validation variants
  (`SHUFFLE_SPAN_TEST=1`, see Safety). Used with `--algo-file algorithms_smoke.txt` (main after
  the merge; `run_all.bat pass7` does that for its smoke runs).
- `results.txt` / `results.csv`: RTX 5080 full run, 300 iterations. `smoke_results.txt`: RTX 5080
  serial smoke (16 iterations, `--dred`).
- Shaders: `p7_sort.hlsl` (entry point), `p7_lds.hlsli`, `p7_rank.hlsli`, `p7_bitonic.hlsli`,
  `p7_radix.hlsli`; reference: `single_pass.hlsl`, `radix_sort.hlsl` + their includes.

## What was built

**`p7_sort.hlsl`**: one group per sort, like single_pass.hlsl, but the group size, the
groupshared size and the tiers are chosen per dispatch. An algorithm can be one dispatch or
several dispatches back to back (no barriers; each handles MIN_COUNT..MAX_COUNT and early-outs
otherwise, so the GPU runs them concurrently). Tiers in a dispatch: rank ≤ `P7_RANK_MAX`
(`P7_RANK_EPT` elements per thread), bitonic ≤ `P7_BIT_MAX` (`P7_BIT_E`), a second bitonic ≤
`P7_BIT2_MAX`, then `P7_LARGE` (1 = bitonic, 2 = p7 radix). Compile-time checks that every tier
fits its group and the groupshared array.

**`p7_lds.hlsli`**: the groupshared array `gsP7[P7_LDS_WORDS]`, by default the next power of two
≥ max(MAX_COUNT, E × WAVE_SIZE of each bitonic tier, 64): a 129-512 tier uses 2 KB, 513-2048 8 KB,
the large tier 32 KB. The size is reserved for every group of a dispatch whatever path it takes,
so the reference's 32 KB also limits its rank and bitonic tiers (Intel: 64 KB SLM per subslice =
2 groups). `P7_LDS_WORDS=8192` forces 32 KB (m3_ref_32k).

**`p7_rank.hlsli`**: the pass2 rank sort with 1, 2 or 4 elements per thread (element t + e ×
GROUP_SIZE); with 2, a 512-element sort runs in 256 threads and every LDS word read serves two
compares.

**`p7_bitonic.hlsli`**: the pass3/pass4 register bitonic (bitonic_reg_t.hlsli) for 32..1024-thread
groups (bitonic_reg_t: 256..1024) and E up to 32, on gsP7. Same code otherwise (wave64: 32-lane
virtual waves).

**`p7_radix.hlsli`** ("X"): an LDS radix sort for small groups (256 / 512 threads, up to 32 keys
per thread; 1024 threads with 8). 16-bit key, stable, 4 × 4-bit LSD passes like pass2/pass3,
but:
- **warp-striped** slots (wave w owns kpt × W consecutive slots, key k of lane l = slot base + k
  × W + l): coalesced global loads, conflict-free read-back, and a small sort only occupies
  ceil(count / (kpt × W)) waves;
- **ballot ranking** ("warp multisplit") instead of per-thread digit counters + lane-prefix
  shuffle scans: per key slot 4 ballots of the digit bits, peers = AND(bit ? ballot : ~ballot),
  rank = popcount(peers & lanes below). No per-thread counters, so up to 32 keys per thread fit
  in registers (pass2/pass3 are limited to 8 by their 4-bit counters);
- per-wave running digit offsets live in 16 "digit lanes" (lane d holds digit d; narrower waves
  hold several digits per lane); a key's offset is one WaveReadLaneAt from lane `digit` of its
  own 16-lane group, always inside the shuffle span;
- table scan by every active wave (no serial wave-0 section), 7 barriers (10 if the table aliases
  the data at count > LDS − 16 × waves, handled like radix_sort2: the table in slots the own
  digit lanes read back);
- rolled pass loop, unrolled key-slot loops that leave with a group-uniform `break` (see Safety:
  the first version skipped each unused slot separately, which cost 12 µs of cold-code jumps).

Why not 8-bit digits (2 passes): stable ranking of 256 digits needs either WaveMatch (8.6× a
shuffle on NVIDIA, pass3; a loop on other vendors) or per-wave 256-counter histograms (16 waves ×
256 × 16 bit = 8 KB next to the 32 KB exchange buffer, over the limit, or aliased with it at the
price of 2 more barriers per pass), plus a 256-entry scan. Not worth it before the 4-bit radix in
small groups is measured. Why not packing several small sorts per group: the framework contract
is one group per sort (SV_GroupID.x = sort index); tier-sized groups get the same occupancy
effect.

## Starting point (what the external runs say)

- **The iGPUs are throughput / occupancy bound, not latency bound.** 20 groups of 1024 threads:
  the Ryzen iGPU (RDNA2, 1 WGP = 2 CUs, max 64 wave32 = 2048 threads) holds at most 2 of them at
  a time, the UHD 770 (2 Xe-LP subslices of 112 hardware threads, 64 KB SLM each; a 1024-thread
  group at SIMD16 = 64 threads) also 2. So 20 sorts run in ~10 rounds and every thread of a group
  counts, even the ones that only wait at barriers.
- **Small-sort tiers waste most of their 1024-thread group.** A 512-element bitonic E4 uses 128
  threads, the other 896 hold thread slots and wait. That is why bitonic E4 lost to rank on the
  iGPU's mostly_medium (53.6 vs 37.4 µs, pass3 set) although it does ~20x less work: the group
  size, not the algorithm, set the occupancy. Rank at 512 on the UHD 770 (71 µs for 20 sorts of
  512) is close to ALU-bound (262k compares per sort).
- **The 32 KB groupshared array is reserved for every group of the single-dispatch shader**,
  whatever path it takes: on Intel that alone limits a subslice to 2 groups (64 KB SLM), on RDNA
  to 4 per WGP (128 KB).
- **Intel radix fixed cost.** Every radix variant costs ~340 µs for 20 sorts of any size from 32
  to 1024 (radix_sort2 with 1 active wave included) and ~117 µs on mostly_empty, rank/bitonic
  tiers of the same 1024-thread shader cost 6.5 µs there. So it is not work, not the group
  dispatch and not the LDS size alone (identical in both), but something in the radix code path
  of a 1024-thread group: most likely register spills (a 1024-thread group on Xe-LP must run at
  SIMD16 with 128 GRFs per hardware thread, i.e. ~60 32-bit values per lane; the unrolled radix
  keeps 8 keys + 4-8 prefix words + counters + ranks + scan temporaries live) or the barrier cost
  with 64 hardware threads per group (7-11 barriers per sort). A smaller group removes both
  constraints: at 256 threads the compiler may use SIMD8 / more registers per lane, and a
  barrier has 16 participants.
- **Ryzen iGPU wave64 collapses** (bitonic 8x, radix_sort2 5-7x): consistent with a 1024-thread
  wave64 group having to fit 64 VGPRs per wave (CU mode) and spilling; a 256-thread group has no
  such limit.

## Variants and hypotheses (the full set, 21 algorithms)

Notation: R = rank sort, B<E> = register bitonic with E elements per thread, P = pass2 radix
(1024 threads, 32 KB), X = pass7 radix (p7_radix.hlsli), @n = threads per group. "m<k>_" =
k dispatches (one per tier, large tier first), "s7_" = one dispatch. Workloads isolate tiers:
mostly_empty (1-64) and mostly_small (0-128) = rank tier; mostly_medium (129-512) = rank / small
bitonic tier; mostly_mid (513-2048) = mid tier; mostly_large (2048-8192) and worst_case (8192) =
large tier; realistic_mix / edges / sparse_keys / sweep = all tiers.

| # | algorithm | tiers | hypothesis | confirms | refutes |
|---|---|---|---|---|---|
| 1 | `s1_rank512_bitreg2048_radix` | 1 dispatch @1024 32 KB: R ≤512, B8 ≤2048, P | anchor (portable best) | - | - |
| 2 | `s1_radix` | P for every size | anchor for the radix fixed cost (Intel 117 / 341 µs) | - | - |
| 3 | `x7_all_256` | X @256 every size | H4: X's fixed cost is low in a small group: on Intel the ~35 µs per radix group goes away | UHD 770 mostly_empty / sweep ≤1024 << s1_radix (e.g. < 30 µs mostly_empty) | ≈ s1_radix |
| 4 | `x7_all_1024` | X @1024 every size | H4b: separates algorithm from group size | ≈ x7_all_256 on Intel small sizes: the old code was the problem (spills / shuffles); ≈ s1_radix: the 1024-thread group is | - |
| 5 | `m3_ref` | P 2049+ @1024 / B8 513-2048 @256 8 KB / R ≤512 @512 2 KB | H1: the same algorithms in tier-sized groups with tier-sized LDS raise throughput on the iGPUs (more groups resident) | iGPU / UHD mostly_medium, mostly_mid clearly below ref (e.g. -20 %+); on the 5080 a small multi-dispatch cost (measured: +1.1-1.6 µs on the small workloads) | ≈ ref on the iGPUs |
| 6 | `m3_ref_32k` | as m3_ref, 32 KB in every dispatch | H1b: is it the thread count or the LDS size? | ≈ m3_ref: thread slots were the limit (a single dispatch with small groups is enough); ≈ ref: LDS was the limit (Intel: expect this, 64 KB SLM per subslice) | - |
| 7 | `m3_x256` | X 2049+ @256 / B8 @256 / R @512 | H2: the large tier is faster on the iGPUs as X in 256 threads (32 keys per thread, 8 waves at wave32, up to 4 groups per WGP by LDS) than P @1024 | iGPU / UHD mostly_large, worst_case below m3_ref (ref: 174 / 260 µs iGPU, 422 / 503 UHD) | ≥ m3_ref |
| 8 | `m3_x512` | X 2049+ @512 | H2b: group size of X for large sorts (16 keys per thread) | ranks X@256 vs X@512 per GPU | - |
| 9 | `m3_b4096_x256` | X 4097+ @256 / B8 513-4096 @512 | H3: with a 512-thread group bitonic is competitive up to 4096 (the 2048 switch was measured with 1024-thread groups) | mostly_large / sweep 2560-4096 below m3_x256 | above (then 2048 stays) |
| 10 | `m3_mid_b4` | mid tier B4 @512 | H5: more threads / fewer registers per thread for 513-2048 (register pressure, iGPU wave64) | mostly_mid below m3_x256 | above: B8 @256 stays |
| 11 | `m2_x513` | X 513+ @256 / R ≤512 | H6: radix from 513 (the iGPU preferred radix_sort2 from 768 up) | mostly_mid below m3_x256 | above |
| 12 | `m3_x1025` | X 1025+ @256 / B8 513-1024 @128 | H6b: radix from 1025 | mostly_mid / sweep 1536-2048 below m3_x256 | above |
| 13 | `m4_b128` | … / B4 129-512 @128 / R ≤128 @128 | H7: bitonic beats rank for 129-512 when it gets its own small group (O(n log²n) vs O(n²) work) | mostly_medium well below m3_x256 on the iGPUs and the 7900 XTX | ≈ or above |
| 14 | `m4_b64` | … / B4 65-512 @128 / R ≤64 @64 | H7b: … even from 65 | mostly_small below m4_b128 | above: rank ≤128 stays |
| 15 | `m4_b256` | … / B4 257-512 @128 / R ≤256 @256 | H7c: rank up to 256, bitonic above | mostly_medium below m4_b128 | above |
| 16 | `m4_b128e8` | … / B8 129-512 @128 / R ≤128 | H8: B8 (half the threads and lane-stage shuffles, twice the registers) vs B4 for 129-512 | mostly_medium below m4_b128 | above |
| 17 | `m3_r2` | … / R ≤512 x2 @256 | H9: rank with 2 elements per thread (half the LDS reads per compare, 256 threads) | mostly_medium below m3_x256 (R @512) | ≈: rank is ALU-bound |
| 18 | `m4_b128_p` | P 2049+ @1024 / B8 / B4 / R ≤128 | fallback: m4_b128's small tiers with the reference large tier, in case X loses | realistic_mix of the best complete config | - |
| 19 | `s7_256` | 1 dispatch @256 32 KB: R ≤512 x2, B8 ≤2048, X | H10: a single dispatch with 256-thread groups gets most of the multi-dispatch gain (no extra dispatches on high-end GPUs) | close to the best m-variant on the iGPUs | far behind: multi-dispatch needed |
| 20 | `s7_512` | 1 dispatch @512: R ≤512, B8 ≤2048, X @512 | H10b | | |
| 21 | `s7_256b` | 1 dispatch @256: R ≤128, B4 ≤512, B8 ≤2048, X | H10c: s7_256 with m4_b128's small tiers | | |

What each GPU class is expected to show:
- **5080 / 7900 XTX (many CUs, latency bound):** every sort has its own SM/WGP, so smaller groups
  only add latency (fewer threads per sort, longer serial chains); multi-dispatch adds ~0.3-1 µs.
  (Written before the 5080 run. Measured: true for X on large sorts, but not up to 2048, where the
  512/256-thread single dispatches were faster than the reference; multi-dispatch cost 1.1-1.6 µs.)
  Expect the reference to stay best or tied for large sorts, and the small-group bitonic tier
  (m4_b128) to win mostly_medium on the XTX (bitonic already won 129-512 there).
- **Ryzen iGPU / UHD 770:** the hypotheses above. The strongest expected effects: H7 (bitonic in
  small groups for 129-512), H1 (tier-sized groups), H4 (Intel radix fixed cost gone at 256).

## Safety runs

| step | result |
|---|---|
| CPU emulation of p7_radix (per-lane state, barrier intervals; W = 4..128, GS 256/512/1024, LDS 2048/8192, `SHUFFLE_SPAN_TEST` on/off, counts around every table / alias boundary + all-equal-digit keys) | 1373 runs sorted and stable; every groupshared access in range; no cross-lane hazard inside a barrier interval; every per-lane shuffle inside its SHUFFLE_SPAN group |
| dxc (SDK 1.8.2502.11 = the shipped dxcompiler), every dispatch of algorithms_smoke.txt × W = 4/8/16/32/64/128 with `[WaveSize]` (+ W = 16/32 without), and again with `SHUFFLE_SPAN_TEST=1` | 248 + 248 compiles, no errors, no warnings, no `alloca` |
| WARP `--debug --gbv --iterations 5`, 24 algorithms × 10 workloads (8 shards); after the radix change again for the 19 algorithms that use it | 240 + 190 combos, 0 failures, 0 debug-layer / GBV messages besides the startup notice |
| WARP `--smoke`, 24 × 10 (before and after the radix change) | 240 + 240 combos, 0 failures |
| **hardware** `--smoke --dred --iterations 16`, RTX 5080, algorithms_smoke.txt | 240 combos, 0 failures, probe OK, no nvlddmkm / dxgkrnl / WHEA events; 13.7 s |
| **hardware** full run, 21 algorithms × 10 workloads × 305 iterations | 210 combos, 0 failures, no driver events; 50.1 s (estimate 45 s) |

Review points: every loop has a compile-time bound (key slots ≤ 32, passes 4, table scan ≤
waves per group, bitonic levels/runs ≤ 13, rank steps ≤ LDS / 8); barriers only under
group-uniform conditions (count, kpt, alias, pass); wave ops (ballots, shuffles) only under
wave-uniform conditions (waveActive, k < kpt / the uniform break), the per-key store condition
comes after them; every groupshared index is < P7_LDS_WORDS by construction and masked.

**One change after the hardware smoke** (the budget was one smoke and one full run). The smoke
showed `x7_all_256` at ~18 µs for 20 sorts of 32 keys vs 6 µs for `x7_all_1024` (serial smoke =
cold code). The only difference at that size: 32 vs 8 unrolled key-slot blocks, each guarded by
`if (k < kpt)`, i.e. 31 jumps over cold code per phase (pass4's cold-code finding). The key-slot
loops now leave with `if (k >= kpt) break;` (one jump to the loop exit; checked in the DXIL: every
slot's test branches to the same exit block). Same computation, same bounds, same wave-op and
barrier placement. Validated with the dxc sweep, WARP GBV (190 combos) and WARP smoke (240) before
the full run; the full run verifies every iteration (0 failures). In the batched full run
`x7_all_256` takes 4.45 µs at 32 keys. (The smoke numbers of the X variants are therefore of the
old code layout; their difference to the full run mixes the fix and batching.)

`SHUFFLE_SPAN_TEST=1` (smoke only): the shuffle span becomes half the wave. On the 5080 that runs
p7_radix's 16-lane digit-group / single table-scan-group path, i.e. the path of Intel's wave16,
which no GPU here has (WARP is 4 lanes, the 5080 32); on WARP 2-lane digit groups with 8 digits
per lane. All passed. The wave64 / wave128 paths (uint2 / uint4 ballot masks) are covered by the
emulation and dxc only.

## Results: RTX 5080 (driver 32.0.16.1714), full run

300 iterations per combo, median µs per iteration (20 sorts), drained `full` flush. Bold = best
per column. p95 / min / max and all sweep tables: results.txt. Columns: mostly_empty,
mostly_small, realistic_mix, mostly_large, worst_case, edges, mostly_mid, mostly_medium, sweep,
sparse_keys.

| algorithm | empty | small | mix | large | worst | edges | mid | medium | sweep | sparse |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 3.23 | 3.58 | 9.81 | 16.90 | 19.04 | 18.37 | 9.38 | 5.47 | 8.13 | **17.12** |
| s1_radix | 10.72 | 10.66 | 11.70 | 16.94 | 19.17 | 18.67 | 11.36 | 10.59 | 11.20 | 17.31 |
| x7_all_256 | 4.48 | 4.69 | 11.23 | 28.56 | 29.30 | 28.50 | 10.40 | 5.92 | 8.18 | 28.43 |
| x7_all_1024 | 4.61 | 4.80 | 9.73 | 18.62 | 20.14 | 19.20 | 10.05 | 5.98 | 8.56 | 18.40 |
| m3_ref | 4.86 | 4.96 | 10.51 | **16.82** | **18.88** | 18.43 | 9.95 | 6.62 | 8.85 | 17.15 |
| m3_ref_32k | 4.56 | 4.80 | 10.43 | 16.83 | 18.98 | **18.34** | 9.89 | 6.56 | 8.91 | 17.22 |
| m3_x256 | 4.77 | 4.96 | 10.38 | 28.54 | 29.20 | 28.75 | 9.98 | 6.69 | 9.07 | 28.51 |
| m3_x512 | 4.70 | 4.99 | 10.14 | 19.23 | 20.50 | 19.65 | 9.98 | 6.75 | 8.99 | 19.01 |
| m3_b4096_x256 | 4.78 | 4.86 | 11.15 | 28.53 | 29.55 | 28.67 | 10.21 | 6.66 | 9.12 | 28.40 |
| m3_mid_b4 | 4.77 | 4.88 | 11.10 | 28.59 | 29.38 | 28.72 | 10.30 | 6.58 | 8.94 | 28.43 |
| m2_x513 | 4.48 | 4.64 | 11.38 | 28.54 | 29.41 | 28.48 | 10.30 | 6.51 | 8.32 | 28.38 |
| m3_x1025 | 4.82 | 4.96 | 11.55 | 28.51 | 29.31 | 28.70 | 10.37 | 6.67 | 8.78 | 28.45 |
| m4_b128 | 4.94 | 5.02 | 10.62 | 28.67 | 29.33 | 29.44 | 10.02 | 7.14 | 9.18 | 28.53 |
| m4_b64 | 5.09 | 5.87 | 10.59 | 28.54 | 29.31 | 29.60 | 10.11 | 7.01 | 8.91 | 28.64 |
| m4_b256 | 4.88 | 5.12 | 10.54 | 28.54 | 29.28 | 29.28 | 10.05 | 7.07 | 8.70 | 28.67 |
| m4_b128e8 | 4.99 | 5.06 | 10.43 | 28.48 | 29.31 | 29.18 | 10.08 | 7.14 | 9.02 | 28.51 |
| m3_r2 | 4.99 | 5.06 | 10.38 | 28.51 | 29.34 | 29.50 | 9.89 | 7.17 | 8.90 | 28.38 |
| m4_b128_p | 4.90 | 5.09 | 10.59 | 16.96 | 19.01 | 18.43 | 10.05 | 7.10 | 9.18 | 17.18 |
| s7_256 | 3.33 | 3.49 | 9.25 | 28.67 | 29.38 | 28.62 | **8.80** | 5.63 | **7.33** | 28.58 |
| s7_512 | **3.17** | 3.46 | 9.25 | 19.47 | 20.74 | 20.18 | 8.90 | **5.31** | 7.38 | 19.33 |
| s7_256b | 3.23 | **3.36** | **9.15** | 28.48 | 29.22 | 28.48 | 8.90 | 5.60 | 7.49 | 28.42 |

Sweep, median by sort size:

| algorithm | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 3.39 | 3.49 | 3.52 | 4.16 | 4.77 | 5.57 | 7.39 | 7.52 | 9.41 | 9.70 | 14.40 | 14.94 | 13.18 | 14.02 | 15.17 | 18.75 |
| s1_radix | 10.78 | 10.50 | 10.62 | 10.78 | 10.66 | 10.66 | 10.82 | 10.62 | 11.01 | 11.84 | 12.06 | 13.86 | 13.33 | 14.14 | 15.04 | 19.18 |
| x7_all_256 | 4.45 | 4.70 | 4.93 | 5.12 | 5.89 | 5.92 | 6.40 | 7.39 | 8.86 | 10.46 | 12.16 | 15.49 | 17.20 | 20.00 | 23.41 | 29.90 |
| x7_all_1024 | 4.70 | 4.70 | 4.83 | 5.18 | 5.60 | 5.98 | 7.23 | 8.74 | 8.51 | 10.62 | 10.88 | 11.94 | 13.84 | 15.14 | 16.70 | 20.30 |
| m3_ref | 4.80 | 4.51 | 4.90 | 4.67 | 6.24 | 6.78 | 7.90 | 8.10 | 9.34 | 10.11 | 12.48 | 13.22 | 13.44 | 14.38 | 15.41 | 18.98 |
| m3_ref_32k | 4.77 | 4.67 | 4.64 | 5.22 | 5.95 | 7.26 | 7.84 | 8.26 | 10.02 | 10.11 | 13.34 | 13.12 | 13.22 | 14.24 | 15.23 | 18.99 |
| m3_x256 | 4.64 | 5.06 | 5.12 | 5.54 | 6.24 | 6.94 | 7.58 | 7.87 | 10.14 | 10.43 | 13.25 | 15.58 | 17.18 | 19.90 | 23.41 | 29.76 |
| m3_x512 | 4.64 | 4.70 | 4.77 | 5.34 | 5.95 | 6.46 | 7.68 | 7.90 | 9.82 | 10.14 | 10.08 | 10.85 | 13.07 | 15.01 | 16.77 | 20.59 |
| m3_b4096_x256 | 4.86 | 4.80 | 4.61 | 4.93 | 5.73 | 6.88 | 7.97 | 8.35 | 9.73 | 10.46 | 15.10 | 16.00 | 16.18 | 22.72 | 23.07 | 29.58 |
| m3_mid_b4 | 4.35 | 4.74 | 4.86 | 5.60 | 5.82 | 6.59 | 8.45 | 8.29 | 9.73 | 10.56 | 13.54 | 16.00 | 17.07 | 20.21 | 23.36 | 29.76 |
| m2_x513 | 4.64 | 4.35 | 4.61 | 4.86 | 5.60 | 6.50 | 6.56 | 7.36 | 8.99 | 10.50 | 12.16 | 14.30 | 17.01 | 20.37 | 23.26 | 30.02 |
| m3_x1025 | 4.61 | 4.42 | 4.70 | 5.18 | 5.70 | 6.59 | 7.52 | 8.06 | 9.38 | 10.59 | 12.13 | 15.52 | 16.80 | 20.06 | 23.25 | 30.14 |
| m4_b128 | 5.09 | 5.28 | 5.15 | 6.59 | 7.10 | 6.56 | 8.03 | 8.03 | 10.02 | 10.53 | 13.34 | 15.58 | 17.09 | 19.90 | 23.31 | 29.95 |
| m4_b64 | 5.25 | 4.99 | 6.21 | 6.27 | 7.10 | 6.82 | 8.00 | 7.84 | 9.76 | 10.56 | 13.86 | 14.14 | 17.10 | 20.06 | 23.28 | 30.14 |
| m4_b256 | 4.51 | 5.12 | 5.06 | 5.44 | 6.85 | 7.10 | 7.97 | 7.78 | 10.18 | 10.05 | 14.05 | 15.62 | 16.88 | 20.22 | 23.50 | 30.16 |
| m4_b128e8 | 5.06 | 5.12 | 4.77 | 7.01 | 7.55 | 6.98 | 7.84 | 8.00 | 9.95 | 10.46 | 13.25 | 14.40 | 17.07 | 20.02 | 23.25 | 29.95 |
| m3_r2 | 5.31 | 4.93 | 5.22 | 5.79 | 6.75 | 7.46 | 7.52 | 7.78 | 9.95 | 10.02 | 13.47 | 16.03 | 17.30 | 20.14 | 23.26 | 30.21 |
| m4_b128_p | 5.15 | 4.80 | 4.99 | 6.75 | 7.07 | 7.33 | 8.32 | 8.03 | 10.02 | 10.43 | 12.99 | 12.83 | 13.18 | 14.46 | 15.38 | 18.91 |
| s7_256 | 3.26 | 3.46 | 3.55 | 4.42 | 5.31 | 5.63 | 6.46 | 6.75 | 8.86 | 9.12 | 14.21 | 16.32 | 17.04 | 20.13 | 23.33 | 30.13 |
| s7_512 | 3.30 | 3.36 | 3.39 | 3.97 | 4.54 | 5.76 | 6.78 | 6.88 | 9.09 | 9.34 | 10.08 | 11.04 | 13.09 | 14.64 | 16.67 | 20.96 |
| s7_256b | 3.14 | 3.17 | 3.55 | 5.22 | 5.60 | 5.50 | 6.66 | 6.66 | 8.77 | 9.12 | 13.47 | 15.94 | 16.94 | 20.29 | 23.33 | 29.82 |

What the 5080 says (a latency-bound GPU: 84 SMs, every sort has its own SM):
- **Multi-dispatch costs ~1.1-1.6 µs on the small workloads** (mostly_empty m3_ref 4.86, m3_ref_32k
  4.56 vs the reference 3.23; mostly_medium 6.6 vs 5.5): each extra ExecuteIndirect of 20 groups
  that mostly early-out. On the large workloads it is free (m3_ref 16.82 / 18.88 vs 16.90 /
  19.04). So multi-dispatch is only worth it where it pays on the low-end GPUs.
- **Single dispatch with smaller groups is as good as or better than the reference on the 5080**
  for everything up to 2048: `s7_512` mostly_empty 3.17 (ref 3.23), mostly_medium 5.31 (5.47),
  mostly_mid 8.90 (9.38), sweep 7.38 (8.13), realistic_mix 9.25 (9.81); `s7_256b` realistic_mix
  9.15. Up to 2048 the same algorithms in 256/512-thread groups run faster (sweep 768-2048:
  s7_512 6.78 / 6.88 / 9.09 / 9.34 vs ref 7.39 / 7.52 / 9.41 / 9.70).
- **X in small groups is latency-bound here, as expected**: 20 × 8192 in 28.5-30 µs at 256 threads
  (8 waves, 32 serial key slots per pass), 20.5 at 512, 20.1 at 1024, vs the pass2 radix 19.0-19.2.
  X @512 is the best radix at 2560-3072 on the 5080 (10.08 / 10.85 vs pass2 14.40 / 14.94 in the
  reference: the reference pays the path switch there, see pass4) and X @1024 at 32-1024 keys is
  4.7-8.7 µs vs the pass2 radix's 10.6-11 floor: the ballot radix removed half of the radix fixed
  cost on the 5080.
- The group-size and tier variants for ≤ 2048 are within ~0.5 µs of each other on the 5080
  (bitonic vs rank at 129-512 included); they only matter where occupancy matters.

## What to run on the other machines

Simplest (main after the merge): `run_all.bat pass7` from the portable package runs all of the
below (the smoke with algorithms_smoke.txt, the full run, and on the AMD machine the iGPU at
wave64 with `--integrated-only`), see tools/README_PORTABLE.txt. By hand:

Use a build of this branch (or of main after the merge); the shader set must be next to the exe
or passed with `--shaders`. Each run shows the prompt and the progress window as usual. First
the smoke (safety: new shaders on new hardware; ~1 min), then the full run:

```
GpuSort.exe --smoke --dred --shaders _test\pass7 --label "pass7 smoke" --out results\pass7_smoke.txt
GpuSort.exe --shaders _test\pass7 --iterations 300 --label "pass7 low-end" --out results\pass7.txt
```

- AMD machine: runs both GPUs (7900 XTX and the Ryzen iGPU) at the default wave32
  ([WaveSize(32)]). Expected: XTX ~1 min, iGPU ~10.5 min (64 050 iterations × ~9.7 ms, the 256 MB
  flush dominates), plus the calibration; the progress window shows the calibrated estimate.
- Optional (round 1b, only if there is time; ~10.5 min): the iGPU at wave64, to test whether the
  small groups remove the wave64 collapse (register-limited 1024-thread wave64 groups):
  `GpuSort.exe --shaders _test\pass7 --iterations 300 --wave-size 64 --gpu "Radeon(TM) Graphics" --label "pass7 iGPU wave64" --out results\pass7_igpu_w64.txt`
  (the substring matches "AMD Radeon(TM) Graphics", not the XTX). Smoke it first the same way
  (`--smoke --dred --wave-size 64 --gpu "Radeon(TM) Graphics"`).
- Intel machine (UHD 770, wave16): the same two commands; ~8 min (64 050 × ~7.6 ms).

Please bring back the results*.txt / .csv of each run (the samples CSV is optional).

## How to read the results (next pass)

Per hypothesis in the table above, compare the named workloads. The single most informative
comparisons:
1. mostly_medium: s1_rank512_bitreg2048_radix vs m3_ref (H1) vs m4_b128 (H7) vs s7_512 / s7_256b
   (H10): group size and tier algorithm for 129-512, where realistic_mix spends most of its time
   on the iGPUs.
2. mostly_empty / mostly_small / sweep ≤ 1024: s1_radix vs x7_all_256 vs x7_all_1024 on Intel
   (H4): what the radix fixed cost is.
3. mostly_large / worst_case: m3_ref (P @1024) vs m3_x256 / m3_x512 / x7_all_1024 (H2) and
   m3_b4096_x256 (H3).
4. m3_ref vs m3_ref_32k (H1b): does the groupshared size or the thread count limit occupancy? If
   the thread count, a single dispatch with small groups (s7_*) is enough and no GPU pays the
   multi-dispatch cost; if the LDS, the low-end configuration needs tier-sized dispatches.

The expected outcome is a per-GPU-class configuration an engine picks at startup (by vendor /
wave size / CU count): e.g. `s7_512`-like single dispatches on discrete GPUs, and a tier-sized
multi-dispatch configuration on the iGPUs if H1/H7 hold.

## Framework changes I would want (not made; src/ and tools/ belong to the other agent)

(After the merge into main: 1 and 2 are done: `run_all.bat pass7`, `--integrated-only`, the
package ships `_test\pass7` incl. algorithms_smoke.txt, and results.txt / results.csv list the
threads per group and groupshared bytes of every dispatch, CSV column `dispatch_info`.)

1. `tools/run_all.bat`: a `pass7` mode that runs the smoke + the 300-iteration full run of
   `_test\pass7` on every GPU (default wave size), and optionally the iGPU at `--wave-size 64`;
   `tools/package.ps1` must then ship `_test\pass7` (it currently packages `shaders/`).
2. results.txt / CSV: the groupshared bytes and the thread count per dispatch (from shader
   reflection: `GetThreadGroupSize`, and the DXIL's groupshared size), next to the DXIL size, so
   the occupancy inputs of every algorithm are in the results.
3. Nice to have: a per-dispatch GPU timestamp mode (timestamps between the dispatches of a
   multi-dispatch algorithm, with barriers, diagnostic only) to attribute the time of the tiers
   on the low-end GPUs without separate workloads.
