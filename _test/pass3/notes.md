# pass3: radix fixed cost, mid tier, thresholds, per-size sweep

**Hardware change during this pass.** The machine hard-reset at about 14:34-14:38. There was no
bugcheck record and no nvlddmkm event this time. At that point this pass had not run anything on a
GPU: shaders were being written, and only the SDK `dxc.exe` had run, on the CPU. Afterwards the user
updated the NVIDIA driver (32.0.16.1088 -> **32.0.16.1714**) and physically removed the RTX 2060.
**All pass3 numbers are RTX 5080 only**, measured on the new driver. The pass2 reference algorithms
were re-measured in the same run.

## What changed

### Framework

- **Workload `sweep`** (id 8). All 20 sorts of an iteration have the same size. Iteration i uses
  size `{32, 64, 128, 256, 384, 512, 768, 1024, 1536, 2048, 2560, 3072, 4096, 5120, 6144, 8192}[i % 16]`,
  so an iteration's time is the latency of one sort of that size (20 run in parallel on 84 SMs).
  results.txt adds per-GPU tables of median / mean / p95 **by size** (1000 iterations = 62-63
  samples per size).
- **Workload `sparse_keys`** (id 9). Sizes are uniform 1-8192. Within each sort, every 4-bit key
  digit is constant with p = 1/2, a model of real sort keys with constant fields. It exercises and
  measures the radix pass skipping. Existing workload ids and seeds are unchanged.
- `--smoke --iterations N`: an explicit `--iterations` now also applies in smoke mode, which stays
  serial with no warmup. The smoke run used 48 iterations to get 3 samples per sweep size.
- `.gitignore`: `.claude/` (another agent's worktree).

### Shaders (all new files; the pass2 files are unchanged and still used by the references)

**`radix_sort2.hlsli`** is the LDS radix sort with a cheaper per-pass structure. It is the same
algorithm as pass2: 16-bit key, stable, 4 × 4-bit LSD passes, blocked arrangement, 1024 threads,
up to 8 keys per thread.
- **Every wave scans the table itself**, so there is no serial wave-0 section and one barrier less
  per pass. The table scan is spread over the lanes. Lane (j = lane & 7, g = lane >> 3) sums
  digit-pair word j over 8 consecutive waves, both the total and the part below its own wave. A
  2-step butterfly (lane ^ 8, ^ 16) combines the groups, then a 2-step scan over the 4 digit groups
  gives the digit bases. Per wave that is 8 conflict-free LDS reads and 7 shuffles, plus 1 shuffle
  per key to fetch its offset.
- **Table words pair digits (4q+r, 4q+r+2)**, which is what `bytes & 0x00FF00FF` of the 8-bit
  lane-prefix words gives directly, with no unpacking.
- **Swizzled exchange slots**: `i ^ ((i >> 5) & 7)`. The blocked read-back is bank-conflict-free
  for kpt = 1/2/4/8; pass2 had 8-way conflicts at kpt = 8.
- **`RS2_MIN_KPT`**: kpt = max(ceil(count / 1024), MIN_KPT). With 8, only ceil(count / 256) waves
  work and the rest only join the barriers.
- **Aliasing fix (count > 7936)**: each wave's table lives in the exchange slots of its last lane.
  That lane read those slots back itself in the previous pass, so no barrier is needed before the
  table write. One barrier (C) remains before the scatter.
- **`RS2_SKIP`**: skips passes whose digit is constant over the whole sort. During the load, each
  wave ORs (key | ~key << 16) with a shuffle butterfly and writes one extra table word per wave.
  After pass 0's first barrier every wave derives the same pass mask. Passes 1-3 with a constant
  digit are skipped entirely (no barriers); pass 0 only skips its scatter. If every digit is
  constant, the input order is written out directly.
- Correct for wave sizes 4..128: 2 tasks per lane for W = 4, and 16-bit prefix fields for W ≥ 64.
  Compiled for all of them; W = 4 (WARP) and W = 32 were executed.

**`bitonic_reg_t.hlsli`** is the pass2 register/wave bitonic as HLSL 2021 templates,
`BitonicRegSortT<E, EB>`. It allows E = 1, 2 and 4 (N / 1024 elements per thread, i.e. more threads
per sort) and several E in one shader. `<8, 3>` is the pass2 code.

**`single_pass3.hlsl`**: one 1024-thread dispatch with group-uniform tiers:
rank ≤ `RANK_MAX` → bitonic E = `MID_E` ≤ `MID_MAX` → bitonic E8 ≤ `BITREG_MAX` →
`RADIX` = 2 (radix_sort2), 1 (pass2 radix), or 0 (bitonic E8).

**`ubench.hlsli`** (diagnostic only): with `UB_OP` set, every group first runs 256 dependent
repetitions of one wave operation before sorting. The result is written only when gNumSorts has an
impossible value.

### Barriers per sort (32 lanes)

| path | barriers |
|---|---|
| rank sort | 1 |
| bitonic E8 | 2(n-8): 0 up to 256, 2 / 4 / 6 / 8 / 10 at 512 / 1024 / 2048 / 4096 / 8192 |
| bitonic E4 | 2(n-7): 2 at 256, 6 at 1024, 8 at 2048, 10 at 4096 |
| bitonic E2 / E1 | 2(n-6) / 2(n-5): E2 10 at 2048, E1 10 at 1024 |
| radix (pass2) | 11, **17** at count > 7936 |
| **radix_sort2** | **7**, **10** at count > 7936 (> 7904 with RS2_SKIP) |
| radix_sort2 + skip | 1 + 2 per varying digit after the first; e.g. 1 if all keys are equal, 3 with one varying digit |

## Safety runs

| step | result |
|---|---|
| dxc, `single_pass3.hlsl` × 8 configurations × W = 4/8/16/32/64/128 | all compile, no `alloca`, no warnings |
| WARP `--debug --gbv --iterations 5`: the 29 new algorithms × 10 workloads (4 parallel shards) | 290 combos, 0 failures, 0 messages besides the GBV startup notice |
| WARP `--smoke`, all 32 diagnostic algorithms × 10 workloads | 320 combos, 0 failures |
| **hardware** `--smoke --dred --iterations 48` (RTX 5080, 32 algorithms × 10 workloads) | 320 combos, 0 failures, no device errors, no nvlddmkm / dxgkrnl / WHEA events |
| **hardware** full run (8 algorithms × 10 workloads × 1005 iterations) | 80 combos, 0 failures, no driver events |

The three pass2 reference shaders are unchanged and were GBV-validated in pass2. They were left out
of this pass's GBV run: with WARP's 4-lane waves, the old radix unrolls a 64 × 8 serial scan, and GBV
took 30+ s per combo instead of about 1 s. They did run in the WARP smoke and both hardware runs.
Review points: every loop has a compile-time bound; barriers are only under group-uniform conditions
(count, or the pass mask, which every wave computes from the same LDS words); wave ops are only under
wave-uniform conditions (the per-key offset shuffle is hoisted out of the per-key `valid` test);
every groupshared index is masked to < 8192 (ubench < 2048).

Two hardware runs in total: one smoke (54.3 s of benchmark time after the popup) and one full run
(57.3 s).

## Microbenchmarks (smoke run, serial, 48 iterations; mostly_empty; delta to `ub_none` / 256)

Each op runs 256 times dependently in every thread of a 1024-thread group (32 waves), before a small
sort. The cost per op is for the whole group:

| op | delta (µs) | per op (ns) | vs shuffle |
|---|---:|---:|---:|
| ALU (imad chain) | 2.62 | 10 | 0.5x |
| WaveReadLaneAt, per-lane source | 5.37 | 21 | 1x |
| WaveReadLaneAt, uniform source | 5.44 | 21 | 1x |
| WaveActiveBallot + countbits | 5.29 | 21 | 1x |
| WaveActiveSum | 5.29 | 21 | 1x |
| WaveActiveBitOr | 5.73 | 22 | 1.1x |
| WavePrefixCountBits | 5.87 | 23 | 1.1x |
| **WavePrefixSum** | **34.67** | **135** | **6.4x** |
| **WaveMatch** + countbits | **46.25** | **181** | **8.6x** |
| GroupMemoryBarrierWithGroupSync (+1 LDS store/load) | 14.00 | 55 | 2.6x |

On driver 1714, WaveActiveSum / WaveActiveBitOr / ballot / WavePrefixCountBits cost the same as a
shuffle; pass2 estimated WaveActiveSum as slow as WavePrefixSum. **WavePrefixSum and WaveMatch are
still slow.** So a ballot/popcount-based ranking (WavePrefixCountBits) is affordable, and a
WaveMatch-based 8-bit multisplit is not.

## Results: full run (RTX 5080, driver 32.0.16.1714)

1000 iterations per combo. µs per iteration (20 sorts), median / p95; best median per column in
bold.

Algorithms (full names are in algorithms.txt):
- `s1_*`: the pass2 shaders (references). `s1_radix` is the pass2 radix for every size.
- `s3_radix2_k8`: radix_sort2 (MIN_KPT 8) for every size.
- `s3_bitE4_rx2`: bitonic E = 4 up to 4096, then radix_sort2.
- `s3_rank512_bitreg2048|4096_radix2`: rank ≤ 512, bitonic E8 ≤ 2048 / 4096, radix_sort2 above.
- `..._radix2s`: the same with RS2_SKIP.

| algorithm | mostly_empty | mostly_small | realistic_mix | mostly_large | worst_case | edges | mostly_mid | mostly_medium | sparse_keys |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg4096_radix | **3.71 / 4.58** | 3.86 / 4.61 | 10.08 / 16.96 | **16.58 / 18.18** | **18.30 / 20.99** | **17.92 / 18.88** | 9.70 / 10.37 | **5.76 / 6.37** | 16.80 / 18.85 |
| s1_rank512_bitreg2048_radix | 3.74 / 4.61 | 3.87 / 4.64 | 10.03 / 17.28 | 17.18 / 18.75 | 19.20 / 21.47 | 18.80 / 19.84 | 9.70 / 10.37 | **5.76 / 6.37** | 17.41 / 19.33 |
| s1_radix | 10.94 / 13.28 | 10.88 / 13.31 | 11.84 / 16.45 | 17.34 / 18.88 | 19.49 / 20.70 | 19.07 / 19.78 | 11.71 / 13.82 | 10.88 / 13.31 | 17.63 / 19.52 |
| s3_radix2_k8 | 8.45 / 11.84 | 8.45 / 11.87 | 10.58 / 17.60 | 18.30 / 19.78 | 20.16 / 21.57 | 19.74 / 20.45 | 10.21 / 12.80 | 8.58 / 12.26 | 18.46 / 20.45 |
| s3_bitE4_rx2 | 4.96 / 5.76 | 4.99 / 5.73 | **9.98 / 17.50** | 17.82 / 19.23 | 19.62 / 21.31 | 19.20 / 19.90 | **9.66 / 10.24** | 6.43 / 7.10 | 18.08 / 19.87 |
| s3_rank512_bitreg2048_radix2 | 3.74 / 4.64 | **3.84 / 4.64** | 10.02 / 17.54 | 17.82 / 19.42 | 19.65 / 21.47 | 19.20 / 20.00 | 9.98 / 10.69 | 5.79 / 6.40 | 18.05 / 20.06 |
| s3_rank512_bitreg4096_radix2 | **3.71 / 4.61** | **3.84 / 4.61** | 10.27 / 17.66 | 17.89 / 19.49 | 19.71 / 21.57 | 19.20 / 20.16 | 10.00 / 10.75 | 5.79 / 6.43 | 18.08 / 20.03 |
| s3_rank512_bitreg2048_radix2s | **3.71 / 4.64** | 3.87 / 4.58 | 10.10 / 18.85 | 19.10 / 20.54 | 21.12 / 22.56 | 20.64 / 21.54 | 9.76 / 10.43 | **5.76 / 6.43** | **15.55 / 18.53** |

**Sweep, median by sort size** (all 20 sorts of an iteration have this size, about 62 samples per
size):

| algorithm | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg4096_radix | 3.71 | 3.78 | 3.90 | 4.35 | 4.99 | 5.98 | **7.42** | **7.49** | **9.57** | 9.95 | 14.37 | 15.04 | 15.81 | 16.26 | **14.77** | **18.34** |
| s1_rank512_bitreg2048_radix | 3.71 | **3.68** | **3.84** | 4.35 | **4.96** | 5.98 | **7.42** | 7.55 | 9.68 | **9.82** | 14.10 | 15.01 | 13.58 | **14.34** | 15.39 | 19.23 |
| s1_radix | 10.85 | 10.85 | 10.82 | 10.91 | 10.91 | 10.91 | 10.94 | 11.30 | 11.33 | 12.00 | 12.22 | 13.97 | 13.63 | 14.48 | 15.41 | 19.50 |
| s3_radix2_k8 | 8.38 | 8.42 | 8.51 | 8.06 | 8.61 | 8.19 | 8.45 | 8.67 | 9.66 | 10.53 | **11.26** | **13.25** | 13.34 | 14.98 | 16.64 | 20.13 |
| s3_bitE4_rx2 | 4.99 | 4.93 | 4.90 | 5.63 | 6.40 | 6.46 | 7.55 | 7.55 | 9.63 | 9.92 | 15.41 | 15.71 | 16.10 | 16.00 | 16.21 | 19.58 |
| s3_rank512_bitreg2048_radix2 | **3.65** | 3.78 | 3.90 | **4.29** | 5.15 | 6.11 | 7.58 | 7.65 | 9.95 | 10.45 | 13.49 | 14.75 | **13.01** | 14.59 | 16.16 | 19.65 |
| s3_rank512_bitreg4096_radix2 | 3.74 | 3.74 | 3.90 | 4.32 | 4.99 | 6.14 | 7.55 | 7.58 | 9.90 | 10.37 | 14.94 | 15.57 | 16.02 | 16.43 | 16.22 | 19.76 |
| s3_rank512_bitreg2048_radix2s | 3.71 | 3.78 | 3.87 | 4.32 | 4.99 | **5.86** | 7.46 | 7.52 | 9.62 | 9.97 | 15.65 | 15.94 | 13.92 | 15.41 | 17.18 | 21.15 |

**Sweep, p95 by sort size:**

| algorithm | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg4096_radix | 4.67 | 4.61 | 4.70 | **4.77** | **5.41** | 6.62 | 7.94 | **7.97** | **10.40** | 10.46 | 15.30 | 15.62 | 16.61 | 17.41 | 18.40 | 20.93 |
| s1_rank512_bitreg2048_radix | 4.58 | 4.58 | **4.58** | 5.09 | 5.76 | 6.62 | 8.03 | 8.38 | 10.43 | 10.43 | 15.81 | 16.29 | 17.18 | 17.66 | 18.62 | 21.15 |
| s1_radix | 13.38 | 13.41 | 13.44 | 13.63 | 13.44 | 13.41 | 12.32 | 11.74 | 13.60 | 12.48 | 14.50 | 14.98 | **14.34** | **16.51** | 17.38 | **20.70** |
| s3_radix2_k8 | 12.29 | 12.16 | 12.38 | 12.10 | 12.29 | 12.32 | 8.99 | 9.31 | 12.86 | 11.04 | **11.71** | **14.59** | 15.65 | 16.90 | 18.75 | 21.34 |
| s3_bitE4_rx2 | 5.50 | 6.08 | 5.50 | 6.21 | 7.39 | 7.10 | 8.00 | 8.29 | **10.40** | 10.40 | 15.90 | 16.29 | 16.96 | 17.12 | 18.34 | 21.34 |
| s3_rank512_bitreg2048_radix2 | 4.64 | 4.51 | 4.61 | 5.02 | 5.73 | 6.75 | 8.19 | 8.06 | 10.75 | 11.14 | 15.20 | 16.00 | 17.18 | 18.02 | **16.58** | 21.66 |
| s3_rank512_bitreg4096_radix2 | **4.16** | 4.67 | **4.58** | 4.96 | 6.02 | 6.56 | 7.94 | 8.32 | 10.59 | 10.88 | 15.36 | 16.32 | 16.61 | 17.95 | 18.59 | 21.66 |
| s3_rank512_bitreg2048_radix2s | 4.51 | **4.45** | 4.93 | 4.90 | 5.63 | **6.46** | **7.81** | 8.16 | 10.50 | **10.34** | 17.28 | 17.34 | 17.92 | 18.94 | 19.74 | 22.59 |

The sweep means in results.txt contain single outliers of 20-360 µs; medians and p95 are not
affected.

**Driver check** (pass2 references, old driver 1088 → new 1714). `s1_rank512_bitreg4096_radix`:
- realistic_mix: 10.56 → 10.08
- mostly_large: 17.34 → 16.58
- worst_case: 19.04 → 18.30
- mostly_mid: 10.24 → 9.70
- small workloads: unchanged (3.68 → 3.71)

The `s1_radix` floor went from 11.4 to 10.9. The new driver is 0-5 % faster; nothing looks off.

**Code-layout / run-to-run noise.** The three pass2 references run the *same* pass2 radix code at
size 8192. Yet the sweep measures 18.34 / 19.23 / 19.50, and worst_case measures 18.30 / 19.20 /
19.49. So at large sizes, differences below about 1 µs between two different shaders are not
meaningful (see next steps).

## Diagnostic smoke run (serial, 48 iterations = 3 per sweep size; rough)

Serial submission biases smoke timings upward, because the GPU idles between iterations:

| floor | smoke | full run |
|---|---:|---:|
| rank sort | about 4.2 µs | 3.7 µs |
| radix | 11-13 µs | 8.4-10.9 µs |

So compare only rows within the smoke run. Below are the sweep medians of the per-path curves; the
full table, including the combinations and microbenchmarks, is in smoke_results.txt. The bold marks
the minimum over all rows of the smoke run.

| algorithm | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg4096_radix | 4.10 | **3.90** | 4.32 | 4.74 | 5.54 | 6.14 | 7.74 | 8.22 | 10.05 | 10.24 | 15.04 | 15.39 | 16.22 | 17.18 | 18.50 | **20.64** |
| s1_rank512_bitreg2048_radix | 4.16 | 4.22 | 4.19 | 5.12 | 5.44 | 6.27 | 7.78 | 8.06 | 10.11 | 10.43 | 15.17 | 16.00 | 16.86 | 17.38 | 18.30 | 21.57 |
| s1_radix | 13.25 | 12.83 | 13.06 | 13.60 | 13.06 | 13.02 | 13.54 | 12.99 | 12.99 | 14.05 | 14.11 | 14.72 | 15.52 | 16.83 | **17.09** | 20.67 |
| s3_radix2_k1 | 11.33 | 11.26 | 11.78 | 11.52 | 11.78 | 12.70 | 13.57 | 14.53 | 14.11 | 15.30 | 15.10 | 15.94 | 17.38 | 17.76 | 19.17 | 20.96 |
| s3_radix2_k4 | 11.14 | 11.07 | 11.55 | 11.14 | 11.46 | 11.65 | 11.84 | 11.78 | 12.42 | 13.79 | 14.18 | 15.20 | 16.83 | 17.70 | 18.85 | 21.06 |
| s3_radix2_k8 | 12.06 | 12.06 | 11.55 | 11.58 | 11.81 | 11.55 | 11.94 | 12.32 | 12.70 | 13.15 | 13.73 | 14.69 | 15.55 | 16.70 | 18.02 | 20.74 |
| s3_radix2_k4_skip | 14.02 | 13.92 | 13.70 | 13.89 | 13.92 | 13.79 | 14.14 | 14.37 | 15.46 | 15.55 | 15.97 | 16.74 | 18.46 | 19.55 | 20.51 | 22.75 |
| s3_radix2_k8_skip | 14.21 | 14.43 | 14.37 | 14.27 | 14.27 | 14.30 | 14.34 | 15.10 | 15.14 | 15.84 | 15.58 | 16.64 | 17.60 | 18.59 | 19.97 | 23.23 |
| s3_bitE1_rx2 | 4.80 | 5.18 | 5.60 | 6.56 | 6.98 | 7.07 | 8.70 | 8.93 | 12.67 | 13.22 | 13.63 | 14.43 | **14.85** | **16.45** | 18.02 | 21.18 |
| s3_bitE2_rx2 | 5.25 | 4.96 | 5.60 | 6.27 | 6.94 | 7.04 | 8.19 | 8.45 | 11.36 | 11.71 | 14.30 | 14.34 | 15.87 | 17.06 | 18.24 | **20.64** |
| s3_bitE4_rx2 | 5.28 | 5.66 | 5.63 | 6.14 | 6.88 | 6.91 | 7.87 | 7.87 | 9.98 | **10.08** | 15.90 | 16.26 | 16.67 | 17.12 | 18.14 | 21.22 |
| s3_bitE8 | 5.86 | 6.02 | 6.24 | 6.18 | 7.01 | 7.30 | 8.29 | 8.19 | 10.56 | 10.72 | 15.46 | 15.87 | 16.64 | 29.34 | 29.66 | 31.33 |
| s3_rank1024_bitE8 | 3.97 | 4.13 | 3.97 | 4.54 | 5.34 | 6.59 | 8.70 | 12.45 | 10.40 | 10.50 | 15.49 | 16.19 | 16.83 | 29.09 | 29.57 | 31.14 |
| s3_rank512_bitreg2048_radix2 | 4.29 | 4.22 | 4.38 | 4.64 | 5.28 | 6.21 | 7.87 | **7.84** | 10.37 | 11.01 | 14.75 | 15.74 | 16.10 | 17.38 | 18.85 | 21.41 |
| s3_rank512_bitreg4096_radix2 | 3.97 | 3.97 | **3.87** | **4.45** | **5.06** | 6.05 | **7.71** | 8.00 | 10.43 | 11.10 | 15.39 | 15.78 | 16.45 | 17.44 | 18.50 | 21.02 |
| s3_rank512_radix2 | 4.51 | 4.61 | 4.26 | 4.83 | 5.44 | 6.21 | 11.52 | 12.96 | 13.15 | 13.02 | **13.38** | **14.14** | 15.23 | 16.83 | 17.60 | 21.31 |
| s3_rank512_bitreg2048_radix2k4 | 4.29 | 4.32 | 4.26 | 5.15 | 5.38 | 6.24 | 7.74 | 8.19 | 10.46 | 11.01 | 15.07 | 15.65 | 17.47 | 18.66 | 19.30 | 21.63 |
| s3_rank512_bitreg2048_radix2s | 4.06 | 4.29 | 4.22 | 4.77 | 5.47 | 6.05 | 8.10 | 7.97 | 10.40 | 10.62 | 16.64 | 16.93 | 17.66 | 18.72 | 19.68 | 22.46 |
| s3_rank512_bitE2_2048_radix2 | 4.29 | 4.48 | 4.38 | 4.74 | **5.06** | 6.34 | 8.19 | 8.42 | 11.68 | 11.65 | 14.98 | 15.46 | 16.42 | 17.82 | 18.53 | 21.41 |
| s3_rank512_bitE4_4096_radix2 | 4.26 | 4.13 | 4.13 | 4.77 | 5.50 | 6.27 | 7.94 | 8.29 | 10.18 | 10.24 | 16.03 | 16.16 | 17.73 | 17.57 | 18.69 | 21.73 |
| s3_rank256_bitreg2048_radix2 | 4.67 | 4.32 | 4.26 | 4.67 | 6.94 | 7.36 | 7.84 | 8.54 | 10.05 | 11.07 | 15.04 | 15.52 | 16.51 | 17.76 | 18.85 | 21.60 |

## Conclusions

**1. radix_sort2 cuts the radix fixed cost by 23 %, but is not faster for large sorts.**

Full-run sweep medians, pass2 radix → radix_sort2 k8, by sort size:

| size | pass2 radix → radix_sort2 k8 | change |
|---|---:|---:|
| ≤ 1024 | 10.8-11.3 → **8.1-8.7** | about -2.5 µs |
| 1536 / 2048 | 11.33 / 12.00 → **9.66 / 10.53** | about -1.5 µs |
| 2560 / 3072 / 4096 | 12.22 / 13.97 / 13.63 → **11.26 / 13.25 / 13.34** | about -0.3 to -1.0 µs |
| 5120 / 6144 / 8192 | 14.48 / 15.41 / 19.50 → 14.98 / 16.64 / 20.13 | about +0.5 to +1.2 µs |

On the whole-batch workloads the large sorts dominate, so radix_sort2 loses about 1 µs there
(mostly_large 17.8 vs 16.6-17.3, worst_case 19.7 vs 18.3-19.5). That is at the edge of the
code-layout noise above.

The likely reason: at full occupancy radix_sort2 issues more MIO operations (shuffles and LDS
accesses share the pipe on NVIDIA).
- **radix_sort2**, per wave and pass: 8 table reads + 7 scan shuffles + 8 per-key offset shuffles
  ≈ 23. Over 32 waves that is about 740 per pass.
- **pass2**: 8 table reads per wave, plus about 56 in wave 0: about 310 per pass.
- **Difference**: about 0.16 µs per pass. With 4 passes, this is about the measured regression.

Removing 4 barriers (about 55 ns each, see the microbenchmarks) and the 8-way bank conflicts did
not pay for that at 5-8k.

`RS2_MIN_KPT` (smoke):
- k8 is best from 2560 up: 13.73 vs 14.18 (k4) and 15.10 (k1) at 2560.
- k4 is slightly better at 768-1536.
- k1 is worst everywhere.

The radix floor is still 8.4 µs versus 3.7 µs for rank sort, so radix does not pay below 2048 anyway.

**2. RS2_SKIP (skip constant digits) is not worth it as implemented.**
- sparse_keys: 2.5 µs faster (15.55 vs 18.05).
- Random keys: 1.3-1.5 µs slower on the large workloads (mostly_large 19.10 vs 17.82, worst_case
  21.12 vs 19.65).

The extra runtime work is about 12 shuffles/LDS ops per wave in pass 0, i.e. about 0.2 µs. The
rest of the cost is structural: `lastPass` is no longer a compile-time constant, so every pass
contains both the LDS and the global scatter, which gives more code and branches. See the
code-size note in next steps.

**3. Mid tier: fewer elements per thread does not help.**

Sweep medians, full run (E4 / E8) and smoke (E1 / E2):

| size | E4 | E8 | E1 (smoke) | E2 (smoke) |
|---|---:|---:|---:|---:|
| 768 | 7.55 | 7.42 | 8.70 | 8.19 |
| 1024 | 7.55 | 7.49-7.65 | 8.93 | 8.45 |
| 1536 | 9.63 | 9.57-9.95 | 12.67 | 11.36 |
| 2048 | 9.92 | 9.82-10.45 | 13.22 | 11.71 |

In the smoke run, E8 measured 8.29 / 8.19 / 10.56 / 10.72 at the same sizes.
- E4 ties with E8 (mostly_mid 9.66 vs 9.70).
- E1 and E2 are 1-3 µs slower: 2-3x more waves, but more lane stages per element. Every lane
  stage is one shuffle per element on the MIO pipe (E8: 30 lane stages per element at N = 2048,
  E2: 50), plus more transposes and barriers.
- The 5080 is not latency-bound enough for the extra parallelism to pay. The 2060 in pass2 was more
  latency-sensitive, so it should be re-checked there.

**4. The rank sort threshold stays at 512.**
- Sweep at 384 / 512: rank 4.96-5.15 / 5.86-6.14 vs bitonic E4 6.40 / 6.46 and E8 7.01 / 7.30
  (smoke).
- At 768, rank (smoke 8.70) loses to bitonic (7.4-7.9).
- `s3_rank256_*` lost on mostly_medium in the smoke run (6.96 vs 5.92).

The crossover is between 512 and 768.

**5. Bitonic → radix crossover: exactly at 2048 (the power-of-two padding of bitonic).**
- At 2048: bitonic 9.82-9.95 vs radix_sort2 10.53 / pass2 radix 12.00.
- At 2560 bitonic pads to 4096: 14.10-15.41 vs radix_sort2 11.26 / pass2 radix 12.22.
- At 4096: bitonic 15.81-16.10 vs radix 13.0-13.6.

The whole-batch workloads barely show this, because they are dominated by their largest sort.

## Recommended single-pass configurations

**RTX 5080 (driver 1714):**

| tier | sizes | algorithm |
|---|---|---|
| rank sort | ≤ 512 | rank sort, 1 element per thread |
| bitonic | 513-2048 | register bitonic, E = 8 (E = 4 is equivalent) |
| radix | 2049-4096 | radix_sort2 (k8) |
| radix | > 4096 | pass2 radix_sort |

This is the per-size optimum of the sweep. The 3-algorithm version (radix above 2048) with the
pass2 radix, `s1_rank512_bitreg2048_radix`, is within noise: realistic_mix 10.03 / 17.28,
mostly_large 17.18, worst_case 19.20, mostly_mid 9.70. The 4-tier combination (both radix variants
in one shader) was not built or measured this pass.

On the 5080 the three-tier configurations are within 0.5 µs of each other on realistic_mix
(9.98-10.27 median). Only the p95 and the large workloads separate them, and those differences
(≤ 1 µs) are at the noise level measured above.

**RTX 2060:** the card was removed during this pass, so there are no new measurements. From pass2
(driver 1088): rank ≤ 512, bitonic E8 ≤ 2048, pass2 radix above (`s1_rank512_bitreg2048_radix`;
bitreg 4096 tied). radix_sort2 and bitonic E4 are unmeasured on Turing.

**Portable (best in aggregate over both GPUs): rank ≤ 512, bitonic E8 ≤ 2048, pass2 radix_sort
above**, i.e. `s1_rank512_bitreg2048_radix` (pass2 code). It is the configuration the 2060 preferred
in pass2, and on the 5080 it is within noise of the best on every workload. Its threshold of 2048
is exactly the bitonic/radix crossover from this pass's sweep. Optionally, radix_sort2 for
2049-4096 is +1 µs on the 5080 at 2.5-3k, but unverified on Turing.

## Next steps

1. **Code size / instruction cache.** Several fixed costs look too large for the work they do:
   - The radix floor is 8.4 µs for 32 keys, against 3.7 µs for rank sort.
   - RS2_SKIP costs 1-1.5 µs for about 0.2 µs of work.
   - Identical radix work at 8192 varies by 1.2 µs between shaders that only differ in their
     small-size tiers.

   The 256 MB flush before every sort also evicts shader code from L2, so a large, fully unrolled
   shader probably pays instruction fetches from DRAM. Test with a `--flush` variant that spares
   shader code (or a no-flush mode for comparison), and with a rolled `[loop]` over the 4 radix
   passes. If confirmed, code size is a first-class optimisation target for this use case, where the
   sort runs once per frame and is cold.
2. **Radix at 5-8k:** keep radix_sort2's per-wave scan only when few waves are active
   (≤ 16, i.e. count ≤ 4096 at k8). At full occupancy, go back to the cheaper single-wave table scan
   plus RsSelect8, keeping the swizzle and the aliasing fix. Both paths fit in one shader behind a
   group-uniform branch.
3. **8-bit digits (2 passes):** on driver 1714, ballot / WavePrefixCountBits cost the same as a
   shuffle, so a ballot-based multisplit (8 ballots per key slot for 8 bits) is now affordable.
   WaveMatch (8.6x) is not. LDS budget: 32 waves × 256 digits × 16 bit = 16 KB of histograms, so
   the exchange needs 16-bit indices plus re-fetching payloads from global memory.
4. **Turing:** re-run `--shaders _test/pass3` (and `_test/pass2`) when a 2060-class card is back.
   Radix_sort2's lower barrier count and E4 are the most likely to behave differently there.
5. Rank sort at 513-768 with 2 elements per thread (halves the LDS reads per element) could move
   the rank threshold to about 640-768. It is a small expected gain.

To re-run: `GpuSort.exe --shaders _test/pass3` (8-algorithm full-run set). The diagnostic set is
`algorithms_diag.txt`; copy it over algorithms.txt in a scratch copy of the directory and run
`--smoke --iterations 48`.
