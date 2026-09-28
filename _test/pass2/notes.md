# pass2: faster large tier (register bitonic, LDS radix), single dispatch

## What was built

### Framework

- **Wave size plumbing.** `EnumerateGpus` queries `D3D12_FEATURE_DATA_D3D12_OPTIONS1` (`WaveOps`,
  `WaveLaneCountMin/Max`); adapters without wave ops are skipped. Shaders are now compiled after
  adapter enumeration, once per distinct wave configuration, with `-D WAVE_SIZE=<WaveLaneCountMin>`.
  If Min != Max, or `--wave-size N` is given, they also get `-D WAVE_SIZE_REQUIRED=1`, and
  `common.hlsli`'s `WAVE_SIZE_ATTR` then expands to `[WaveSize(WAVE_SIZE)]` on the entry points.
  `WAVE_BITS` is derived in `common.hlsli`, which rejects any size that is not a power of two in 4..128.
  results.txt prints the lane range and the compiled `WAVE_SIZE` per GPU. Both NVIDIA GPUs report
  32-32. WARP reports **4-4**, so every WARP run tests the shaders at wave size 4. The code
  paths for 64 and 128 (8-bit vs 16-bit radix prefix fields, `RS_NUM_WAVES < WAVE_SIZE`) were
  compiled with dxc for 4/8/16/32/64/128 but could not be executed: no device here supports them.
- **Workload `mostly_medium`** (uniform 129-512) is appended as id 7. The existing ids and seeds are
  unchanged.

### Shaders (all share one `groupshared uint gsLds[8192]` = 32 KB, `sort_lds.hlsli`)

**`bitonic_reg.hlsli` / `bitonic_reg.hlsl`: register/wave bitonic.** One group per sort. The sort is
padded to N = 2^n (next power of two, at least `BR_ELEMS * WAVE_SIZE`) and runs on N / `BR_ELEMS`
threads; the remaining threads only take part in the barriers. Each element has a logical index. A
*layout* with window base B makes the index bits [B, B + L) local, where L = log2(E) + log2(W):
the low log2(E) window bits select the register and the rest select the lane. All other bits select
the wave.
- Stage on a register bit: pure register min/max, with compile-time register indices (no local arrays; the
  DXIL was checked for `alloca`).
- Stage on a lane bit: `WaveReadLaneAt(v, lane ^ (1 << q))`.
- Stage on a wave bit: *transpose*. Write all elements to LDS at their logical index, one barrier,
  read back in the layout whose window is the L bits ending at the stage bit. The next L-1 stages
  are then local too. Every level s > L needs two transposes (window [s-L, s-1], then back to
  [0, L)). The load from global memory already uses the B = 0 layout, and the store happens
  from it.
- **One barrier per transpose, no write-after-read barrier:** in any layout a thread owns a fixed set
  of indices, and the next transpose writes exactly the addresses the thread itself read in the
  previous one.
- **Barriers (E = 8, 32 lanes, L = 8): 10 at N = 8192** (pass0: 91), 8 at 4096, 6 at 2048,
  4 at 1024, none up to 256. E = 16: 8/6/4/2. E = 32: 6/4/2/0.
- **Directions:** values inside descending blocks are stored complemented (~v). Every
  compare-exchange is then ascending (register stage: min/max; lane stage: min or max by lane bit),
  plus one XOR per element per level.
- **LDS swizzle:** `addr = i ^ (((i >> 5) ^ (i >> 10)) & 31)`. The bank of index bit k is k mod 5,
  so every layout's 5 lane bits hit 32 distinct banks. It is a permutation of [0, 2^n), and indices
  are also masked with `& 8191`.
- **Control flow:** per level, "runs": an optional transpose, a runtime loop over the run's lane
  stages, then all register stages as one unrolled block. The first version picked each stage at
  runtime through a chain of uniform branches. In the hardware smoke it measured 42.9 µs at 8192 and
  9.9 µs for a single-wave sort on the 5080; the run structure measures 32.4 and 6.1. A fully
  unrolled per-n version made DXC take 2-4 min per shader (and about 800 MB), so it was dropped.
- Variants: `BR_ELEMS` = 8 (1024 threads), 16 (512), 32 (256). `BR_LANE_BITS` (default
  `WAVE_BITS`; 0 = LDS only, no wave intrinsics) was used only in the diagnostic run.

**`radix_sort.hlsli` / `radix_sort.hlsl`: LDS radix sort.** 16-bit key, stable, 4 passes of 4-bit
digits (LSD), 1024 threads, blocked arrangement with kpt = ceil(count / 1024) keys per thread
(1..8). Only `count` elements are processed; there is no padding. Output is the full 32-bit value.
Per pass:
1. Per-thread digit counts: 16 nibble counters in 2 registers, plus each key's rank among its thread's
   earlier keys (4-bit fields).
2. Wave exclusive prefix of the counts: 4 words × four 8-bit fields for W ≤ 32 (max prefix
   31 × 8 = 248), else 8 words × two 16-bit fields.
3. The last lane of each wave writes its wave's 8 total words to a table at the top of the LDS
   array (transposed so the reads are conflict-free). Barrier.
4. Wave 0 scans the table over the waves and computes the digit bases from the 16 totals, in place.
   Barrier.
5. Scatter to LDS at digitBase + waveOffset + lanePrefix + localRank. Barrier. Read back the blocked
   slots. The last pass scatters straight to `gOutput`.

- **Barriers: 11** (3 per pass, 2 in the last). LDS = exchange buffer [0, count) plus a table of
  256 words at [7936, 8192) for 32 lanes. The table aliases the data only if count > 7936. Then
  two more barriers per pass are needed (before the table write and before the scatter): **17 at
  8192**.
- The wave scans are Hillis-Steele `WaveReadLaneAt` shuffle scans (`wave_scan.hlsli`, 5 steps for 32
  lanes). `RS_WAVE_INTRINSICS=1` uses `WavePrefixSum` / `WaveActiveSum` instead (see results).
- For W < 32 (more waves than lanes), each lane of wave 0 sums `RS_SCAN_PER_LANE` consecutive waves
  serially. This path runs on WARP (W = 4).

**`rank_sort1.hlsli`**: the pass1 rank sort with 1 element per thread, as a function (count ≤
`GROUP_SIZE`) for the single-dispatch shader. The unchanged `rank_sort.hlsl` is still used by the t2
variants.

**`single_pass.hlsl`**: one 1024-thread group per sort, one ExecuteIndirect. It uses a group-uniform
branch on count: `count <= RANK_MAX` → rank sort; `<= BITREG_MAX` → bitonic_reg (E = 8); otherwise
radix (`LARGE_RADIX=1`) or bitonic_reg.

### Algorithms in this run (15)

| name | dispatches |
|---|---|
| `pass0` (`pass0_bitonic`) | reference, 1024-thread LDS bitonic, all sizes |
| `t2_bitonic` (`t2_rank512_bitonic`) | reference: rank 512 ≤512 + pass0 bitonic 513+ |
| `t2_bitreg` / `t2_bitreg16` / `t2_bitreg32` | rank 512 + bitonic_reg E = 8 / 16 / 32 (1024 / 512 / 256 threads) |
| `t2_radix` | rank 512 + radix (shuffle scans) |
| `t2_radix_waveops` | rank 512 + radix with `WavePrefixSum` / `WaveActiveSum` |
| `s1_r512_bitreg`, `s1_r1024_bitreg` | 1 dispatch: rank ≤ 512 / ≤ 1024, bitonic_reg above |
| `s1_r512_radix`, `s1_r1024_radix` | 1 dispatch: rank ≤ 512 / ≤ 1024, radix above |
| `s1_r512_br2048_rx`, `s1_r512_br4096_rx` | 1 dispatch: rank ≤ 512, bitonic_reg ≤ 2048 / ≤ 4096, radix above |
| `s1_bitreg`, `s1_radix` | 1 dispatch: bitonic_reg / radix for every size |

(Full names in algorithms.txt and results.txt.)

## Safety runs

Every shader change went through the full sequence. Checks that apply to every loop and index:
- Every loop has a compile-time trip count, or is bounded by count ≤ 8192 (n ≤ 13 levels, ≤ 13
  runs per level).
- No loop depends on a wave-intrinsic result.
- Every groupshared index is masked or provably < 8192.
- Global writes are `i < count` or `min(pos, count - 1)`.
- dxc for W = 4..128: no `alloca` (local arrays) anywhere.

| step | result |
|---|---|
| WARP `--debug --gbv --iterations 5`, all algorithms × 8 workloads (run after each change; final: 15 × 8 = 120 combos) | 0 failures, 0 debug-layer / GBV messages (only the GBV startup notice) |
| WARP `--smoke` (final: 120 combos) | 0 failures |
| hardware `--smoke --dred`, 1st shader set (12 algorithms, 192 combos) | 0 failures, no device errors, no nvlddmkm/dxgkrnl events |
| hardware `--smoke --dred`, diagnostic set (9 algorithms, 144 combos) | same |
| hardware `--smoke --dred`, 13-algorithm set (208 combos) | same |
| hardware `--smoke --dred`, final 15-algorithm set (240 combos) | same |
| full hardware run (this results.txt) | 0 failures, no driver events |

One WARP anomaly happened during development, in the very first version of the radix shader. On
W = 4 it ran a 64 × 8 fully unrolled redundant scan in every wave, and WARP took seconds per
iteration. In that run, `s1_rank512_radix` reported 3 wrong results on small sorts: these take the
rank-sort path, which is identical in all other variants. The next combination then hit the 300 s
WARP fence timeout. After the scan was rewritten, this never reproduced in any later WARP run
(including 5 GBV runs). The most likely explanation is WARP misbehaving on that huge shader, but it
is not proven. That shader version was never run on hardware.

Wall time of the full run: **341.6 s** (86.5 s on the RTX 5080, 255.0 s on the RTX 2060), 1000
iterations + 5 warmup per combo.

## Results (µs per iteration = 20 sorts, median / p95; best median per column in bold)

**RTX 5080**

| algorithm | mostly_empty | mostly_small | realistic_mix | mostly_large | worst_case | edges | mostly_mid | mostly_medium |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pass0 | 5.09 / 5.86 | 5.76 / 6.46 | 13.94 / 45.02 | 45.41 / 46.27 | 46.08 / 46.88 | 44.74 / 46.05 | 13.63 / 14.37 | 7.42 / 8.16 |
| t2_bitonic | 4.10 / 5.76 | 4.13 / 5.73 | 15.55 / 46.08 | 46.27 / 47.90 | 47.01 / 48.38 | 45.60 / 47.26 | 14.72 / 16.48 | 5.60 / 6.37 |
| t2_bitreg | 4.18 / 5.79 | 4.16 / 6.05 | 12.30 / 30.94 | 32.35 / 34.05 | 33.60 / 35.07 | 31.33 / 34.14 | 11.46 / 13.18 | 5.57 / 6.40 |
| t2_bitreg16 | 4.13 / 5.92 | 4.16 / 5.73 | 12.61 / 29.06 | 30.30 / 32.10 | 31.74 / 33.34 | 29.25 / 32.54 | 11.87 / 13.70 | **5.54 / 6.43** |
| t2_bitreg32 | 4.13 / 6.11 | 4.13 / 5.98 | 15.36 / 31.52 | 32.32 / 33.89 | 33.22 / 35.07 | 32.22 / 34.21 | 14.66 / 16.77 | 5.60 / 6.46 |
| t2_radix | 4.19 / 6.21 | 4.19 / 6.18 | 13.98 / 18.08 | 18.53 / 20.45 | 20.51 / 22.98 | 18.94 / 21.47 | 13.09 / 16.42 | 5.57 / 6.59 |
| t2_radix_waveops | 4.22 / 6.27 | 4.13 / 6.24 | 19.42 / 24.61 | 21.57 / 27.68 | 23.65 / 30.43 | 22.37 / 27.14 | 15.81 / 24.35 | 5.60 / 6.53 |
| s1_r512_bitreg | **3.68 / 4.70** | **3.81 / 4.70** | 10.61 / 29.34 | 31.17 / 32.13 | 32.32 / 33.25 | 31.65 / 32.67 | 10.34 / 11.14 | 5.73 / 6.46 |
| s1_r1024_bitreg | **3.68 / 4.74** | **3.81 / 4.74** | 11.14 / 29.38 | 31.15 / 32.19 | 32.35 / 33.34 | 31.68 / 32.58 | 11.04 / 11.90 | 5.70 / 6.50 |
| s1_r512_radix | 3.74 / 4.70 | 3.94 / 4.77 | 13.76 / 17.09 | 18.02 / 19.49 | 20.03 / 22.14 | 19.55 / 20.48 | 12.22 / 15.07 | 5.82 / 6.56 |
| s1_r1024_radix | 3.71 / 4.74 | **3.81 / 4.99** | 13.84 / 17.02 | 17.66 / 19.23 | 19.74 / 21.70 | 19.23 / 20.19 | 11.94 / 14.94 | 5.73 / 6.50 |
| s1_r512_br2048_rx | **3.68 / 4.64** | 3.84 / 4.64 | **10.56 / 17.63** | 17.60 / 19.20 | 19.62 / 22.18 | 19.10 / 20.03 | 10.27 / 11.10 | 5.76 / 6.53 |
| s1_r512_br4096_rx | **3.68 / 4.77** | **3.81 / 4.80** | **10.56 / 17.73** | **17.34 / 19.14** | **19.04 / 21.73** | **18.53 / 19.87** | **10.24 / 11.07** | 5.76 / 6.53 |
| s1_bitreg | 5.57 / 6.50 | 5.57 / 6.43 | 10.64 / 29.31 | 31.17 / 32.13 | 32.35 / 33.15 | 31.81 / 32.70 | 10.27 / 11.01 | 6.62 / 7.49 |
| s1_radix | 11.39 / 13.92 | 11.26 / 14.02 | 12.26 / 16.80 | 17.89 / 19.36 | 19.97 / 21.47 | 19.52 / 20.48 | 12.10 / 14.37 | 11.30 / 13.95 |

**RTX 2060**

| algorithm | mostly_empty | mostly_small | realistic_mix | mostly_large | worst_case | edges | mostly_mid | mostly_medium |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pass0 | 8.32 / 10.82 | 9.47 / 10.24 | 24.64 / 67.07 | 89.50 / 95.68 | 92.16 / 98.30 | 65.54 / 71.68 | 24.27 / 29.79 | 12.29 / 12.70 |
| t2_bitonic | 5.86 / 6.14 | 6.08 / 6.14 | 23.98 / 66.91 | 89.30 / 95.52 | 92.13 / 98.30 | 65.57 / 71.68 | 24.54 / 30.11 | **10.24 / 10.37** |
| t2_bitreg | **5.79 / 6.14** | **6.05 / 6.14** | 23.46 / 45.06 | 49.06 / 55.30 | 52.56 / 59.01 | 45.06 / 51.14 | 18.43 / 23.71 | **10.24 / 10.34** |
| t2_bitreg16 | 6.08 / 6.14 | 6.70 / 7.78 | 23.68 / 47.10 | 75.70 / 83.97 | 90.34 / 96.80 | 48.99 / 61.15 | 24.48 / 29.89 | **10.24 / 10.34** |
| t2_bitreg32 | 5.95 / 6.14 | 6.59 / 7.62 | 30.58 / 52.77 | 79.87 / 90.11 | 102.40 / 108.80 | 53.31 / 66.53 | 32.67 / 38.46 | **10.24 / 10.34** |
| t2_radix | 5.82 / 6.14 | 6.06 / 6.14 | 22.69 / 28.67 | 31.04 / 37.82 | 38.40 / 44.67 | 28.67 / 34.50 | 23.17 / 28.99 | **10.24 / 10.40** |
| t2_radix_waveops | 5.82 / 6.14 | 6.08 / 6.14 | 40.70 / 45.18 | 47.90 / 54.46 | 53.71 / 59.68 | 44.54 / 49.98 | 40.80 / 46.30 | **10.24 / 10.37** |
| s1_r512_bitreg | 5.95 / 6.14 | 6.14 / 6.24 | **17.66 / 44.77** | 48.67 / 55.04 | 52.45 / 58.56 | 45.06 / 51.10 | **17.28 / 23.90** | **10.24 / 10.50** |
| s1_r1024_bitreg | 5.95 / 6.14 | 6.14 / 6.21 | 22.85 / 44.77 | 48.74 / 54.98 | 52.46 / 58.69 | 45.06 / 50.88 | 22.75 / 28.67 | **10.24 / 10.40** |
| s1_r512_radix | 5.98 / 6.14 | 6.14 / 6.21 | 19.84 / 27.49 | 30.85 / 37.66 | 38.91 / 45.06 | 28.45 / 33.60 | 19.81 / 24.29 | **10.24 / 10.56** |
| s1_r1024_radix | 5.98 / 6.14 | 6.14 / 6.30 | 20.48 / 28.22 | 31.25 / 38.34 | 38.91 / 45.06 | 28.67 / 34.66 | 23.10 / 28.90 | **10.24 / 11.30** |
| s1_r512_br2048_rx | 6.08 / 6.14 | 6.14 / 6.40 | 18.14 / 28.06 | **30.72 / 36.86** | 38.53 / 44.77 | **27.46 / 32.99** | 17.86 / 23.74 | **10.24 / 11.30** |
| s1_r512_br4096_rx | 6.05 / 6.14 | 6.14 / 6.37 | 18.27 / 28.67 | **30.72 / 36.86** | **38.24 / 44.35** | 28.10 / 33.54 | 17.89 / 23.94 | **10.24 / 11.36** |
| s1_bitreg | 10.40 / 11.42 | 10.43 / 11.46 | 18.16 / 44.54 | 49.09 / 55.23 | 52.74 / 58.88 | 45.06 / 50.14 | 17.76 / 23.90 | 12.22 / 12.29 |
| s1_radix | 18.40 / 18.46 | 18.43 / 18.56 | 19.31 / 26.98 | 31.07 / 37.06 | 38.82 / 45.06 | 28.38 / 33.66 | 19.33 / 23.26 | 18.43 / 19.52 |

The 2060's timestamps are quantized in steps of about 1.024 µs (6.14, 10.24, 12.29, ... =
6/10/12 × 1.024), so its small-workload medians are only accurate to about ±1 µs and many tie.

### Diagnostic run (hardware `--smoke`, 3 iterations only, one iteration in flight: rough numbers)

Run on an intermediate shader state to find out why the first versions were slow. Every variant
handles all sizes in one dispatch. `d_rx*` here is the radix sort *with* `WavePrefixSum` /
`WaveActiveSum` (before the shuffle scans). `d_rx_prefix16` forces the 16-bit lane prefix, i.e.
16 more `WavePrefixSum`s per sort. Medians in µs:

| variant | 5080 small | 5080 mid | 5080 worst | 2060 mid | 2060 worst |
|---|---:|---:|---:|---:|---:|
| pass0 | 5.79 | 13.95 | 45.82 | 30.72 | 96.48 |
| bitreg E8 (runs) | 5.89 | 10.53 | 32.35 | 24.93 | 51.20 |
| bitreg E16 | 6.94 | 10.85 | 30.50 | 24.58 | 52.35 |
| bitreg E32 | 9.79 | 13.47 | 32.16 | 30.50 | 67.55 |
| bitreg E32, 2 lane bits | 7.84 | 15.81 | 36.61 | 33.12 | 75.14 |
| bitreg E32, LDS only (no shuffles) | 9.15 | 17.47 | 41.50 | 34.82 | 75.30 |
| bitreg E16, LDS only | 8.03 | 13.82 | 40.16 | 24.93 | 70.88 |
| radix, WavePrefixSum (4 + 16 wave ops / pass) | 20.70 | 21.63 | 27.97 | 40.96 | 55.26 |
| radix, 16 more WavePrefixSum | 24.26 | 24.74 | 30.27 | 45.06 | 59.87 |

## Conclusions

- **The large tier is 2.3-2.9x faster.** At worst_case (20 × 8192) the best algorithm measures
  19.0 µs vs 46.1 µs for pass0 on the 5080, and 38.2 vs 92.2 on the 2060. mostly_large:
  17.3 vs 45.4 (5080), 30.7 vs 89.5 (2060).
- **Best large-tier algorithm: LDS radix sort on both GPUs** for sorts above ~2-4k
  (t2 worst_case: radix 20.5 / bitreg 33.6 / bitreg16 31.7 on the 5080, radix 38.4 / bitreg 52.6
  on the 2060). **Register bitonic wins for 513-2048** (mostly_mid, single dispatch:
  bitreg 10.3 vs radix 12.2 on the 5080, 17.3 vs 19.8 on the 2060). The radix sort has a
  fixed cost of 4 passes × 3 barriers plus scans that does not shrink with count. bitonic_reg at
  N ≤ 2048 needs only 4-6 barriers.
- **bitonic_reg: 8 elements × 1024 threads is the robust choice.** 16/256 threads are slightly
  faster on the 5080 (30.3 vs 32.4 on mostly_large) but much slower on the 2060 (75.7 vs 49.1).
  Turing needs the parallelism. 32 elements per thread is worse on both.
- **Wave intrinsics:** `WavePrefixSum` / `WaveActiveSum` are very slow in these 1024-thread groups:
  about 0.25 µs (5080) and 0.35 µs (2060) per call from the diagnostic run. Replacing them with
  `WaveReadLaneAt` shuffle scans made radix 3 µs faster on the 5080 and 15-18 µs faster on the 2060
  (t2_radix vs t2_radix_waveops). `WaveReadLaneAt` itself is cheap: the LDS-only bitonic variants
  (more barriers, no shuffles) were slower everywhere.
- **Single vs 2 dispatches:** single dispatch wins or ties everywhere on the 5080. On mostly_empty /
  mostly_small it is 3.68 vs 4.10-4.19 µs (-10%). On mostly_mid it is 10.3 vs 11.5 (bitreg). On the
  large workloads radix loses the 0.5-1 µs empty-dispatch penalty from pass1. On the 2060 it is
  better on mid-size (mostly_mid 17.3 vs 18.4, realistic_mix 17.7-18.1 vs 22.7-23.5) and about
  equal on small workloads (6.0-6.1 vs 5.8-6.1, within the timer quantization).
- **Crossovers:**
  - **rank vs bitonic_reg:** rank sort is still best up to 512. On mostly_medium (129-512) the rank
    tiers measure 5.5-5.8 vs 6.6 for s1_bitreg on the 5080, and 10.2 vs 12.2 on the 2060. For tiny
    sorts, a single-wave bitonic_reg sort costs 5.6 / 10.4 µs vs 3.7 / 6.0 for rank.
  - **Rank up to 1024 does not pay:** s1_r1024_* is slower than s1_r512_* on mostly_mid on both GPUs
    (5080 11.0 vs 10.3; 2060 22.8 vs 17.3). The crossover is at or below ~512.
  - **bitonic_reg vs radix: between 2048 and 4096.** On the 5080 br4096 is slightly better than
    br2048 (17.3 vs 17.6 mostly_large); on the 2060 it is a tie.
- **Best overall: `s1_r512_br4096_rx` (5080) and `s1_r512_br2048_rx` (2060)**, i.e. one 1024-thread
  dispatch with rank ≤ 512, bitonic_reg up to 2-4k, and radix above. On realistic_mix it gives
  10.6 / 17.7 µs (median / p95) vs pass0 13.9 / 45.0 on the 5080, and 18.1 / 28.1 vs 24.6 / 67.1
  on the 2060. The p95 drops by ~60% because the rare 2k-8k sort got much cheaper.
- `s1_radix` / `s1_bitreg` for every size are not good enough for small sorts. The radix floor is
  about 11 µs (5080) and 18 µs (2060) even for 1-element sorts.

## Next-step suggestions

1. **Radix fixed cost.** 11 µs even for tiny sorts means ~2.5 µs per pass of latency (scans,
   3 barriers, serial wave-0 section). Options:
   - 8-bit digits in 2 passes, with a match/ballot-based per-wave histogram (halves the passes and
     barriers)
   - merge steps 3 and 4 (every wave scans the 256-word table with shuffles instead of waiting for
     wave 0)
   - skip passes whose digit is constant across the sort (check with one block OR/AND reduction
     first)
2. **bitonic_reg for 513-2048 at 1 element per thread** (1024 threads, E = 1-2) could beat the
   E = 8 / 128-256-thread version at the low end: more parallelism per stage, especially on Turing.
3. **Merge rank + bitonic_reg + radix into the final shader** with thresholds per GPU class
   (bitreg ≤ 2048 on Turing, ≤ 4096 on Blackwell), and reconsider the 1024-thread group for
   rank ≤ 512. On the 5080 the 512-thread rank tier (t2) is still marginally faster on
   mostly_medium, 5.57 vs 5.76.
4. **Why `WavePrefixSum` is this slow** (NVIDIA driver 32.0.16.1088) deserves a small targeted
   microbenchmark, as does `WaveActiveBallot` / `WaveMatch` before building an 8-bit radix on them.
5. Test wave size 64 (AMD) and 16 (Intel) hardware if available. The code is written for 4-128,
   but only 4 (WARP) and 32 were executed.

To re-run this snapshot: `GpuSort.exe --shaders _test/pass2`. (After the run, only comments in
`bitonic_reg.hlsli` changed; the code is identical.)

## pass4 addendum: wave64 fix

The shader files of this snapshot were patched in pass4 (`common.hlsli`, `wave_scan.hlsli`,
`radix_sort.hlsli`, `bitonic_reg.hlsli`): on an AMD RX 7900 XTX at `--wave-size 64` the radix sort
returned wrong results. Waves wider than 32 lanes now use wave intrinsics for the scans and 32-lane
virtual waves for bitonic. For WAVE_SIZE <= 32 the compiled code is bit-identical, so the results
above are unaffected. Details: _test/pass4/notes.md.

**Correction (pass5):** pass4 named "per-lane-index `WaveReadLaneAt` does not cross the two 32-lane
halves of a wave64" as the likely cause. That is refuted: the wave probe shows cross-half reads
working on that GPU at wave64, and that model cannot leave output[0] unwritten as reported. The
most likely cause is a driver miscompile of this snapshot's shuffle scans at wave64 (not confirmed);
see _test/pass5/notes.md, "Wave64 root cause". The fix itself is confirmed on hardware: 0 failures
at wave64 on the 7900 XTX and a Ryzen iGPU (_test/external/notes.md).
