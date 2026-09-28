# pass4: wave64 fix, flush modes (cold code vs cold data), 4-tier shader, radix_sort3

RTX 5080 only (driver 32.0.16.1714). This pass has two new findings:

1. The shaders were **wrong on AMD at wave64**. That is fixed here, for all shader sets
   (see "Wave64 correctness bug"). The fix is confirmed by emulation, but not yet on AMD hardware.
2. **Cold shader code**, not cold data, dominates the fixed cost of the large shaders. Code size
   and layout are now the main optimisation target (see conclusions 1-3).

## What changed

### Framework

- **`--flush-mode full|code|data|none`** (default `full` = the previous behaviour). It sets what
  happens between the data upload and the timed sort. A `flush <mode>` line in algorithms.txt
  overrides it per algorithm, so one run can compare the modes.

  | mode | order per iteration | shader code | data |
  |---|---|---|---|
  | full (default) | upload, 256 MB flush, sort | L2-cold | cold |
  | code | 256 MB flush, upload, sort | L2-cold | warm in L2 (freshly written, like keys produced by a previous pass) |
  | data | upload, flush, **untimed run of the same dispatches on a private copy** of the iteration (own buffers and descriptor table), sort | warm | cold |
  | none | upload, sort | warm | warm |

  The warm-up run in `data` mode warms everything the shader touches except the sort's own
  buffers: code, and any register-spill memory or constants.
- results.txt: a line per algorithm with its flush mode and DXIL container size; the header
  explains the modes.
- **`--smoke`: a verification failure no longer stops the run.** That algorithm skips its
  remaining workloads on that GPU and every other algorithm keeps running. results.txt lists the
  failed algorithms as `SMOKE FAILED: <algorithm> on <gpu> (first failing workload ...)`. A device
  loss or fence timeout still stops everything (exit code 3). Exit code 1 as before.
- **tools/run_all.bat**: a smoke run with exit code 1 no longer stops the script. The remaining
  smoke runs still run, the `SMOKE FAILED:` lines are copied into summary.txt, and the script stops
  before the first full run. A device loss (3) or Cancel (2) stops at once. Tested with DRYRUN for
  failure, device loss and success. README_PORTABLE.txt is updated.

### Shaders (shaders/ = this snapshot)

- **Wave64 fix** in `common.hlsli`, `wave_scan.hlsli`, `radix_sort.hlsli`, `radix_sort2.hlsli`,
  `bitonic_reg.hlsli` and `bitonic_reg_t.hlsli`, also applied to the pass2 and pass3 snapshots (see
  below). The code is unchanged for WAVE_SIZE ≤ 32 (bit-identical DXIL).
- **`radix_sort3.hlsli`**: radix_sort2 with a table scan chosen per sort (group-uniform):
  - count ≤ `RS3_PERWAVE_MAX` (default 4096): every active wave scans the table itself, as in
    radix_sort2.
  - larger counts: *single-wave scan*. Wave 0 runs the table-scan tasks over all waves. Task
    (j, g) sums word j over its waves; an exclusive scan over the groups and a butterfly give its
    first offset. It then walks its waves and writes each wave's offset word over the table word.
    Barrier B'. Every wave then reads its 8 offset words (broadcast reads) and selects the key's
    offset with `Rs2Select8` (ALU, no per-key shuffle).
  - `RS3_ROLLED=1`: the 4 passes are a `[loop]` (pass, shift and "last pass" are runtime
    values). DXIL shrinks from 37.4 KB to 14.0 KB.
- **`single_pass4.hlsl`**: 1024-thread single dispatch with group-uniform tiers:
  rank ≤ `RANK_MAX` → bitonic E8 ≤ `BITREG_MAX` → sort kind `MID_RADIX` ≤ `MID_RADIX_MAX` →
  `LARGE_RADIX`. Kinds: 0 = bitonic E8, 1 = pass2 radix, 2 = radix_sort2, 3 = radix_sort3. The
  4-tier configuration is rank ≤ 512, bitonic ≤ 2048, radix_sort2 ≤ 4096 (or 5120), pass2 radix
  above.
- `ubench.hlsli`: two code-size microbenchmarks, `UB_OP` 11 and 12. Both run the dependent chain
  `x = (x ^ (x >> 7)) * K(i)`: 11 fully unrolled (distinct immediates, straight-line code),
  12 as a `[loop]`.
- `SHUFFLE_SPAN_TEST=1` and `RS2_PREFIX8=0` / `RS_PREFIX8=0`: test switches that run the wave64
  code paths on 32-lane hardware and on WARP.

### Barriers per sort (32 lanes)

| path | barriers |
|---|---|
| rank sort | 1 |
| bitonic E8 | 2(n-8): 0 up to 256, 2 / 4 / 6 / 8 / 10 at 512 / 1024 / 2048 / 4096 / 8192 |
| pass2 radix | 11, 17 at count > 7936 |
| radix_sort2 | 7, 10 at count > 7936 |
| radix_sort3, count ≤ RS3_PERWAVE_MAX | 7 (as radix_sort2) |
| radix_sort3, single-wave scan | 11 (3 per pass, 2 in the last), **14** at count > 7936 (barrier C after the offset reads) |

At wave64 (after the fix) bitonic uses 32-lane virtual waves. It then has the wave32 barrier
counts (10 at 8192) instead of 2(n-9) (8 at 8192). The radix barrier counts do not depend on the
wave size.

## Wave64 correctness bug (AMD RX 7900 XTX) and fix

**Report.** The user ran the portable package 3409bf6 (the pass2 shaders) on an AMD RX 7900 XTX
(driver 32.0.11037.4004, wave lanes 32-64):
- With the default wave size (WAVE_SIZE = 32 + `[WaveSize(32)]`), all 15 algorithms × 8 workloads
  passed.
- With `--wave-size 64`, everything passed up to `s1_radix` (the pass2 radix for every size) on
  mostly_empty. That combination failed 3 of 3 iterations, always on small sorts (count 4, 46, 47):
  "key order violated at index 1 (0x.... after 0xDEAD)", i.e. output[0] still held the poison
  value. The smoke run stopped there.
- The only other code with wave ops that had run at wave64 was bitonic (`s1_bitreg`) on
  mostly_empty, i.e. on sorts of at most 64 elements.
- `s1_radix` at wave64 took 29 µs on mostly_empty, vs 9.2 µs at wave32.

**Analysis** (CPU emulation in Python, per-thread uint32 registers, per-wave lane operations,
barrier-separated phases; counts 1..8192 including every tier and aliasing boundary):
1. **The shader logic is correct at wave64.** With HLSL semantics (`WaveReadLaneAt` reads any lane
   of the wave), the pass2 radix, radix_sort2, radix_sort3 and the register bitonic (E = 8, 4, 1)
   sort correctly for W = 4, 8, 16, 32, 64 and 128.
2. **A pessimistic hardware model reproduces the report.** In this model, in a wave wider than
   32 lanes a `WaveReadLaneAt` only reads inside the reading lane's own 32-lane half (source =
   own half + (index & 31)). RDNA's `ds_bpermute_b32` behaves this way in wave64 mode (to my
   knowledge; compilers have to emulate a full-wave permute there). Under this model, with the old
   code:
   - pass2 radix: wrong at every size. The lane-63 wave totals miss the lower half, so digit bases
     and scatter positions are wrong. Output slots stay unwritten, which matches the poison at
     index 0.
   - radix_sort2: wrong at every size (two keys are scattered to the same slot).
   - Register bitonic E8: **correct at 40 elements, wrong at 300, 1000, 4000 and 8192**. Up to 64
     elements all real elements sit in lanes 0..7, and the lane-bit-5 exchange only ever meets
     padding. This matches `s1_bitreg` passing mostly_empty at wave64. Every bitonic and radix
     configuration would have failed on the later workloads.
3. **Likely root cause:** a `WaveReadLaneAt` whose source lane differs per lane does not read across
   the two 32-lane halves of a wave64 with this driver. The shaders relied on HLSL semantics that
   this driver does not seem to provide at wave64. **Not confirmed on hardware** (there is no
   wave64 device here). The shaders had never run at wave64 before: NVIDIA is always 32 lanes,
   WARP 4.

**Fix.** `common.hlsli` defines a **shuffle span**, `SHUFFLE_SPAN_BITS` = min(WAVE_BITS, 5). A
`WaveReadLaneAt` with a per-lane source index only ever reads within the reading lane's aligned
group of `SHUFFLE_SPAN` lanes. For wider waves:
- `wave_scan.hlsli`: `WaveInclusiveSumShfl` / `WaveOrShfl` use `WavePrefixSum` / `WaveActiveBitOr`
  (`WAVE_SCAN_INTRINSICS`). This covers the pass2 radix, radix_sort2 and radix_sort3 lane-prefix
  scans and the RS2_SKIP OR.
- `radix_sort.hlsli`: `RS_WAVE_INTRINSICS` defaults to 1, so no `WaveReadLaneAt` is left (the wave-0
  total is `WaveActiveSum`).
- `bitonic_reg.hlsli` / `bitonic_reg_t.hlsli`: layouts use 5 lane bits (`BR_LANE_BITS` /
  `BRT_LANE_BITS`), i.e. a wave64 holds two 32-lane *virtual waves*. Lane stages stay inside a
  virtual wave, and lane bit 5 goes through the LDS transposes like a wave bit. The sort is still
  padded to at least one whole hardware wave, so `active` stays wave-uniform.
- `radix_sort2.hlsli` / `radix_sort3.hlsli`: the table-scan tasks run on `RS2_TASK_LANES` = 32 lanes.
  Lanes 32..63 repeat the tasks of lanes 0..31 (redundant work, same values; only lanes < 32 write
  in radix_sort3). The task shuffles are `lane ^ 8`, `lane ^ 16`, and reads within an 8-lane group.
- **Snapshots:** the patched files replace the pass2 and pass3 copies. pass2 gets `common.hlsli`,
  `wave_scan.hlsli`, `radix_sort.hlsli` and `bitonic_reg.hlsli`; pass3 gets the same plus
  `radix_sort2.hlsli` and `bitonic_reg_t.hlsli`. Both snapshot copies were byte-identical (pass3's
  wave_scan: up to line endings) to the pre-fix shaders/ files. So the patched snapshots are
  "old code + fix".
- **Nothing changes for WAVE_SIZE ≤ 32.** The SDK dxc gives bit-identical DXIL (disassembly)
  before and after the fix for W = 4, 8, 16 and 32. This covers every algorithm of the pass2 set,
  the pass3 diagnostic set in the pass3 snapshot, and the same set in shaders/. At W = 64 / 128,
  32 of 32 pass3 and 12 of 21 pass2 algorithms change; the others have no wave ops. So all
  NVIDIA / WARP results of pass2 and pass3 still hold for the patched snapshots.

**Verification.**

| check | result |
|---|---|
| Emulation, HLSL semantics, W = 4..128. Pass2 radix; radix_sort2 (k8, k1, task lanes 16 / 8 at W = 32); radix_sort3 (hybrid, always single-wave, per-wave k1, task lanes 16 / 8); bitonic E = 8 / 4 / 1 (plus lane bits 1 / 4). | all correct |
| Emulation, "32-lane halves" model, W = 64 and 128: the **fixed** radix_sort2, radix_sort3 and bitonic E = 8 / 4 / 1 | all correct |
| Same model, the **old** code at W = 64 (negative control) | radix_sort2 wrong at 4 / 46 / 300 / 3000 / 8192; bitonic E8 correct at 40, wrong at 300 / 1000 / 4000 / 8192 |
| The fixed pass2 radix at wave64 | no `WaveReadLaneAt` left; its intrinsic scans compute the values of the shuffle scans, which the HLSL-semantics emulation checks at W = 64 |
| dxc, W = 4..128: all 42 algorithms of algorithms_diag.txt, the pass3 diagnostic set, the pass2 set | compile, no warnings, no `alloca` |
| Wave64 code paths at other widths (`SHUFFLE_SPAN_TEST=1`: span = half the wave, intrinsic scans, 16-bit lane-prefix fields): 8 `v64_*` algorithms with the pass2, pass3 and pass4 shaders, on WARP (W = 4, span 2) and the RTX 5080 (W = 32, span 16) | WARP GBV + WARP smoke + hardware smoke: 0 failures |

**Proven vs assumed.**
- *Proven* (by emulation, DXIL comparison and runs):
  - At every wave size 4..128, the fixed shaders are correct under HLSL semantics.
  - At 64 / 128 they are also correct under the "32-lane halves" model.
  - The code paths wave64 uses (virtual waves, repeated tasks, intrinsic scans, 16-bit prefixes)
    run correctly on real hardware and on WARP at other widths.
  - Nothing changes for W ≤ 32.
- *Assumed:*
  - The root cause is the one above.
  - At wave64 the 7900 XTX executes `WavePrefixSum`, `WaveActiveSum`, `WaveActiveBitOr` and a
    `WaveReadLaneAt` *within* a 32-lane half correctly.
  - The emulation does not model memory ordering or other compiler bugs.
- **A wave64 run on the 7900 XTX is needed to confirm the fix.** The new package runs it by default:
  `run_all.bat` adds `--wave-size 64` for GPUs with a 32-64 lane range.

## Safety runs

| step | result |
|---|---|
| dxc, algorithms_diag.txt (42 algorithms) × W = 4/8/16/32/64/128 | 252 compiles, no warnings, no `alloca` |
| WARP `--smoke`, 42 algorithms × 10 workloads | 420 combos, 0 failures |
| WARP `--debug --gbv --iterations 5`, 42 algorithms × 10 workloads (14 parallel shards) | 420 combos, 0 failures, 0 messages besides the GBV startup notice |
| WARP `--smoke`, final algorithms.txt (15) | 150 combos, 0 failures |
| **hardware** `--smoke --dred --iterations 48`, RTX 5080, 42 algorithms × 10 workloads | 420 combos, 0 failures, no device errors, no nvlddmkm / dxgkrnl / WHEA events; 58.8 s |
| **hardware** full run, 15 algorithms × 10 workloads × 1005 iterations | 150 combos, 0 failures, no driver events; 110.1 s |

Two hardware runs in total (one smoke, one full). The full run uses only shader configurations
that ran in the smoke run, in flush modes that also ran there. Review points:
- Every loop is compile-time bounded, or bounded by `RS2_WPT` (≤ 256 at W = 4).
- Barriers are only under group-uniform conditions (count, perWave, alias, the pass index).
- Wave ops are only under wave-uniform conditions (waveActive, wave == 0, k < kpt).
- Every groupshared index is masked to < 8192.
- In radix_sort3's write-back, each table word is written by exactly one lane (the emulation
  asserts this).

## Results: full run (RTX 5080, driver 32.0.16.1714)

1000 iterations per combo. µs per iteration (20 sorts), median / p95; best median per column in
bold. Full names and DXIL sizes are in algorithms.txt and results.txt. `@data` / `@code`: same
shader, other flush mode.

| algorithm | DXIL KB | mostly_empty | mostly_small | realistic_mix | mostly_large | worst_case | edges | mostly_mid | mostly_medium | sparse_keys |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg4096_radix | 30.3 | 3.62 / 4.64 | 3.74 / 4.70 | 9.98 / 17.02 | **16.45 / 18.40** | **18.24 / 21.18** | **17.82 / 19.07** | 9.63 / 10.40 | 5.70 / 6.37 | **16.70 / 18.88** |
| s1_rank512_bitreg2048_radix | 30.3 | 3.65 / 4.74 | 3.74 / 4.64 | 10.08 / 17.57 | 17.12 / 18.78 | 19.07 / 21.76 | 18.72 / 19.97 | 9.63 / 10.43 | 5.73 / 6.46 | 17.38 / 19.46 |
| s4_4tier_4096 | 50.0 | 3.63 / 4.67 | 3.74 / 4.64 | **9.84** / 20.80 | 16.93 / 23.14 | 18.72 / 25.79 | 18.37 / 23.33 | **9.60 / 10.37** | 5.70 / 6.43 | 17.34 / 23.62 |
| s4_4tier_5120 | 50.0 | 3.65 / 4.61 | 3.74 / 4.70 | 9.89 / 21.18 | 17.57 / 23.65 | 19.04 / 25.79 | 18.72 / 25.28 | **9.60 / 10.37** | 5.68 / 6.43 | 17.95 / 23.65 |
| s4_3tier_rx3 | 42.5 | **3.62 / 4.70** | 3.74 / 4.64 | 10.27 / 22.14 | 19.01 / 22.53 | 20.74 / 26.62 | 20.32 / 22.34 | 9.86 / 10.78 | **5.66 / 6.53** | 19.30 / 23.10 |
| s4_3tier_rx3r | 19.0 | 3.65 / 4.67 | 3.74 / 4.67 | 10.29 / 19.10 | 19.87 / 21.31 | 21.73 / 22.75 | 21.50 / 22.59 | 10.02 / 10.82 | 5.70 / 6.43 | 20.13 / 21.98 |
| s1_radix | 25.2 | 10.91 / 13.44 | 10.82 / 13.57 | 11.81 / 16.48 | 17.28 / 18.75 | 19.33 / 20.96 | 18.88 / 19.84 | 11.55 / 13.98 | 10.85 / 13.60 | 17.54 / 19.42 |
| s4_radix2 | 25.3 | 8.35 / 11.94 | 8.35 / 12.03 | 10.85 / 17.47 | 18.22 / 19.78 | 20.00 / 21.54 | 19.55 / 20.64 | 10.11 / 12.90 | 8.54 / 12.35 | 18.46 / 20.26 |
| s4_radix3_rolled | 14.0 | 8.64 / 9.34 | 8.70 / 9.44 | 10.56 / 19.04 | 19.84 / 21.28 | 21.70 / 22.53 | 21.52 / 22.40 | 10.43 / 11.26 | 8.90 / 9.66 | 20.10 / 21.98 |
| s1_rank512_bitreg2048_radix@data | 30.3 | 3.04 / 3.81 | 3.10 / 3.87 | 8.77 / 15.20 | 16.10 / 17.54 | 18.11 / 18.50 | 17.81 / 18.21 | 8.67 / 9.06 | 4.80 / 5.34 | 16.38 / 18.24 |
| s1_rank512_bitreg2048_radix@code | 30.3 | 2.37 / 3.14 | 2.43 / 3.07 | 8.86 / 15.90 | 16.70 / 17.98 | 18.72 / 20.77 | 16.51 / 18.46 | 8.58 / 9.09 | 4.26 / 4.86 | 16.64 / 18.34 |
| s4_4tier_4096@data | 50.0 | 3.10 / 3.81 | 3.14 / 3.84 | 8.61 / 14.91 | 15.78 / 17.09 | 17.74 / 18.08 | 17.47 / 17.86 | 8.67 / 9.02 | 4.77 / 5.34 | 16.13 / 17.86 |
| s4_3tier_rx3@data | 42.5 | 3.01 / 3.78 | 3.10 / 3.84 | 8.93 / 17.06 | 17.92 / 19.23 | 19.78 / 20.13 | 19.46 / 19.87 | 9.02 / 9.38 | 4.77 / 5.34 | 18.24 / 20.03 |
| s4_3tier_rx3r@data | 19.0 | 3.07 / 3.90 | 3.14 / 3.84 | 9.06 / 18.14 | 19.01 / 20.35 | 20.83 / 21.18 | 20.80 / 21.12 | 9.12 / 9.50 | 4.77 / 5.38 | 19.30 / 20.99 |
| s4_3tier_rx3r@code | 19.0 | 2.34 / 3.07 | 2.43 / 3.10 | 9.09 / 17.95 | 19.52 / 20.70 | 21.57 / 21.98 | 19.26 / 21.12 | 8.99 / 9.54 | 4.22 / 4.80 | 19.33 / 21.09 |

(Bold = best of the default-flush rows. Among all rows the `@data` / `@code` rows are faster
everywhere; they measure different conditions.)

**Sweep, median by sort size** (about 62 samples per size; iterations cycle through the sizes in
this order, so each size follows the one on its left):

| algorithm | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg4096_radix | 3.58 | 3.65 | 3.78 | 4.35 | 4.96 | 5.89 | 7.42 | 7.52 | 9.54 | 9.87 | 14.32 | 14.99 | 15.81 | 16.06 | 14.66 | 18.22 |
| s1_rank512_bitreg2048_radix | 3.58 | 3.71 | 3.78 | 4.22 | 4.83 | 5.79 | 7.33 | 7.46 | 9.50 | 9.78 | 14.24 | 15.26 | 13.46 | 14.24 | 15.30 | 19.07 |
| s4_4tier_4096 | 3.65 | 3.71 | 3.81 | 4.26 | 4.99 | 5.86 | 7.49 | 7.52 | 9.62 | 9.76 | 12.00 | 13.07 | 11.86 | 20.46 | 15.25 | 18.67 |
| s4_4tier_5120 | 3.62 | 3.68 | 3.81 | 4.35 | 4.96 | 5.86 | 7.46 | 7.52 | 9.55 | 9.76 | 12.19 | 12.59 | 11.81 | 13.33 | 21.36 | 19.10 |
| s4_3tier_rx3 | 3.62 | 3.65 | 3.74 | 4.32 | 5.02 | 5.86 | 7.46 | 7.58 | 9.79 | 10.32 | 19.15 | 19.01 | 13.20 | 16.38 | 17.44 | 20.78 |
| s4_3tier_rx3r | 3.65 | 3.68 | 3.84 | 4.22 | 5.06 | 5.92 | 7.46 | 7.55 | 9.95 | 10.38 | 11.47 | 12.18 | 13.73 | 16.48 | 18.21 | 21.76 |
| s1_radix | 10.85 | 10.75 | 10.88 | 10.85 | 10.88 | 10.75 | 10.85 | 11.26 | 11.18 | 11.87 | 12.11 | 14.06 | 13.55 | 14.34 | 15.33 | 19.36 |
| s4_radix2 | 8.38 | 8.35 | 8.42 | 7.97 | 8.61 | 8.26 | 8.35 | 8.61 | 9.54 | 10.42 | 11.23 | 13.26 | 13.23 | 14.77 | 16.43 | 20.00 |
| s4_radix3_rolled | 8.74 | 8.67 | 8.86 | 8.42 | 8.90 | 8.64 | 8.80 | 9.02 | 9.94 | 10.75 | 11.55 | 12.06 | 13.63 | 16.56 | 18.30 | 21.71 |
| s1_rank512_bitreg2048_radix@data | 3.14 | 3.17 | 3.30 | 3.68 | 4.10 | 4.99 | 6.50 | 6.56 | 8.64 | 8.86 | 10.91 | 11.49 | 12.48 | 13.28 | 14.37 | 18.21 |
| s1_rank512_bitreg2048_radix@code | 2.50 | 2.40 | 2.43 | 2.91 | 3.52 | 4.74 | 6.46 | 6.43 | 8.51 | 8.94 | 13.39 | 14.64 | 12.54 | 13.84 | 14.75 | 18.77 |
| s4_4tier_4096@data | 3.10 | 3.17 | 3.20 | 3.49 | 4.10 | 4.90 | 6.53 | 6.56 | 8.66 | 8.86 | **8.77** | **9.34** | **10.75** | **13.25** | **14.30** | **17.76** |
| s4_3tier_rx3@data | 2.98 | 3.01 | 3.30 | 3.65 | 4.19 | 4.93 | 6.56 | 6.62 | 8.90 | 9.38 | 10.24 | 10.69 | 12.22 | 14.78 | 16.38 | 19.87 |
| s4_3tier_rx3r@data | 3.10 | 3.14 | 3.14 | 3.55 | 4.19 | 4.99 | 6.56 | 6.66 | 8.96 | 9.50 | 10.40 | 11.15 | 12.67 | 15.65 | 17.41 | 20.90 |
| s4_3tier_rx3r@code | 2.37 | 2.37 | 2.46 | 2.91 | 3.55 | 4.77 | 6.50 | 6.50 | 8.90 | 9.50 | 10.37 | 11.31 | 12.64 | 16.29 | 18.11 | 21.70 |

The full sweep p95 table is in results.txt.

**Cold-code penalty by size, `full` minus `data`** (same shader, sweep medians):

| algorithm | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | +0.44 | +0.54 | +0.48 | +0.54 | +0.73 | +0.80 | +0.83 | +0.90 | +0.86 | +0.92 | **+3.33** | **+3.77** | +0.98 | +0.96 | +0.93 | +0.86 |
| s4_4tier_4096 | +0.55 | +0.54 | +0.61 | +0.77 | +0.89 | +0.96 | +0.96 | +0.96 | +0.96 | +0.90 | **+3.23** | **+3.73** | +1.11 | **+7.21** | +0.95 | +0.91 |
| s4_3tier_rx3 | +0.64 | +0.64 | +0.44 | +0.67 | +0.83 | +0.93 | +0.90 | +0.96 | +0.89 | +0.94 | **+8.91** | **+8.32** | +0.98 | **+1.60** | +1.06 | +0.91 |
| s4_3tier_rx3r | +0.55 | +0.54 | +0.70 | +0.67 | +0.87 | +0.93 | +0.90 | +0.89 | +0.99 | +0.88 | +1.07 | +1.03 | +1.06 | +0.83 | +0.80 | +0.86 |

## Diagnostic smoke run (serial: one iteration per command list, 48 iterations = 3 per sweep size)

The full table, including the `v64_*` validation algorithms and the microbenchmarks, is in
smoke_results.txt. Whole-batch workloads, median / p95:

| algorithm | mostly_empty | mostly_small | realistic_mix | mostly_large | worst_case | edges | mostly_mid | mostly_medium |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg4096_radix | 3.92 / 4.90 | 4.02 / 5.41 | 13.94 / 17.66 | **17.66 / 18.91** | **20.51 / 21.18** | **18.40 / 19.20** | **9.92 / 10.66** | 5.87 / 7.04 |
| s1_rank512_bitreg2048_radix | 4.05 / 5.41 | 4.00 / 5.34 | 14.70 / 18.27 | 17.95 / 19.33 | 21.04 / 21.57 | 19.15 / 20.61 | 9.95 / 10.98 | 5.94 / 7.20 |
| s1_radix | 12.75 / 14.02 | 12.91 / 14.08 | 13.42 / 16.64 | 17.46 / 19.17 | 20.29 / 20.90 | 19.30 / 20.42 | 13.34 / 14.62 | 12.96 / 14.27 |
| s4_radix2 | 11.33 / 12.70 | 11.52 / 12.96 | 12.05 / 17.86 | 18.66 / 19.84 | 21.15 / 21.60 | 20.06 / 20.58 | 12.48 / 13.82 | 11.74 / 12.99 |
| s4_radix3 | 16.48 / 18.02 | 16.37 / 17.79 | 16.75 / 19.42 | 20.42 / 21.34 | 24.80 / 25.41 | 21.57 / 23.04 | 17.10 / 17.98 | 16.61 / 17.18 |
| s4_radix3_rolled | 8.74 / 10.05 | 8.93 / 10.37 | **10.94** / 19.26 | 20.22 / 21.54 | 22.02 / 22.46 | 21.82 / 22.30 | 10.77 / 11.62 | 9.02 / 10.37 |
| s4_4tier_4096 | 4.06 / 5.54 | 4.10 / 5.09 | 12.29 / 21.28 | 22.38 / 23.26 | 24.80 / 25.95 | 22.27 / 24.16 | **9.92** / 11.36 | 5.90 / 6.82 |
| s4_3tier_rx3 | 4.08 / 5.44 | 4.08 / 5.63 | 18.69 / 22.78 | 20.91 / 22.43 | 25.22 / 26.11 | 21.30 / 22.24 | 10.34 / 11.07 | **5.82 / 6.88** |
| s4_3tier_rx3r | **3.87 / 4.90** | **3.95 / 5.25** | 11.18 / 19.42 | 20.22 / 21.60 | 22.19 / 22.50 | 21.95 / 23.52 | 10.40 / 11.62 | 5.95 / 7.04 |
| s1_rank512_bitreg2048_radix@data | 2.94 / 3.78 | 3.10 / 3.68 | 10.45 / 15.39 | 16.18 / 17.57 | 18.21 / 18.91 | 17.98 / 19.04 | 8.83 / 9.76 | 4.86 / 5.54 |
| s1_rank512_bitreg2048_radix@code | 3.63 / 4.06 | 3.78 / 4.13 | 13.94 / 17.82 | 17.55 / 18.50 | 20.51 / 21.06 | 18.75 / 19.23 | 9.47 / 9.86 | 5.55 / 6.05 |
| s1_rank512_bitreg2048_radix@none | 1.82 / 2.27 | 1.95 / 2.34 | 10.21 / 15.68 | 16.40 / 24.83 | 17.98 / 18.21 | 16.38 / 17.73 | 8.32 / 8.54 | 3.78 / 4.06 |
| s4_3tier_rx3@data | 2.88 / 3.65 | 3.18 / 3.87 | 9.54 / 17.25 | 18.03 / 19.33 | 19.87 / 20.16 | 19.65 / 19.97 | 9.22 / 9.92 | 4.85 / 5.63 |
| s4_3tier_rx3r@data | 3.02 / 3.81 | 3.01 / 3.84 | 9.86 / 18.24 | 19.20 / 20.19 | 20.94 / 21.22 | 20.96 / 21.92 | 9.30 / 10.02 | 4.83 / 5.41 |

(Bold = best of the default-flush rows.)

Smoke sweep medians, single paths (µs; 3 samples per size):

| algorithm | 32 | 256 | 1024 | 2048 | 2560 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_radix | 13.02 | 12.80 | 13.22 | 14.02 | 13.92 | 15.07 | 16.03 | 16.96 | 20.90 |
| s4_radix2 | 11.90 | 11.68 | 12.32 | 13.12 | 13.63 | 16.13 | 16.26 | 17.57 | 21.02 |
| s4_radix2@data | 7.36 | 6.94 | 7.74 | 9.44 | 10.43 | 12.26 | 14.11 | 15.84 | 19.23 |
| s4_radix2@none | 6.98 | 6.78 | 7.42 | 9.22 | 10.62 | 12.77 | 13.63 | 15.33 | 18.82 |
| s4_radix3 (unrolled) | 16.77 | 16.38 | 16.99 | 17.66 | 17.73 | 18.56 | 21.25 | 22.11 | 24.93 |
| s4_radix3@data | 7.49 | 7.14 | 8.38 | 9.63 | 10.62 | 12.64 | 15.17 | 16.96 | 20.35 |
| s4_radix3_sw (always single-wave scan) | 14.66 | 13.98 | 15.07 | 16.70 | 16.42 | 18.21 | 19.01 | 20.54 | 23.01 |
| s4_radix3_rolled | 8.77 | 8.74 | 9.60 | 11.07 | 11.81 | 13.76 | 16.99 | 18.56 | 22.08 |
| s4_radix3_rolled@data | 8.00 | 7.46 | 8.22 | 9.79 | 10.69 | 12.90 | 15.71 | 17.50 | 21.06 |
| s4_bitE8 | 5.95 | 5.66 | 8.16 | 10.56 | 15.42 | 16.61 | 28.61 | 29.73 | 31.20 |
| s4_bitE8@data | 5.12 | 5.25 | 7.17 | 9.41 | 14.21 | 15.36 | 27.74 | 28.58 | 30.02 |

### Code-size microbenchmarks (smoke, mostly_empty medians)

Each is a prelude before `s4_3tier_rx3` (4.08 µs full / 2.88 µs data). Deltas are to that
baseline:

| prelude | full | data | delta (data) | per repetition | cold penalty (full − data) |
|---|---:|---:|---:|---:|---:|
| none (the sort alone) | 4.08 | 2.88 | - | - | 1.20 |
| 512 reps, `[loop]` | 15.71 | 14.94 | 12.06 | 23.6 ns | 0.77 |
| 512 reps, unrolled (~1.5k instructions, +8 KB DXIL) | 9.22 | 8.45 | 5.57 | 10.9 ns | 0.77 |
| 2048 reps, `[loop]` | 52.74 | 52.00 | 49.12 | 24.0 ns | 0.74 |
| 2048 reps, unrolled (~6k instructions, +33 KB DXIL) | 29.04 | 26.66 | 23.78 | 11.6 ns | **2.38** |

Straight-line code is cheap to fetch cold. 512 unrolled repetitions cost nothing extra; 2048
repetitions (roughly 4× the GPU code) cost about +1.6 µs. The instruction fetcher streams
sequential code well. The sort shaders lose far more than that (next section), so their cost is
not code *volume* alone.

## Conclusions

**1. Cold shader code, not cold data, is the big fixed cost of the large shaders.**
- *Data cold* (flush vs none, with the code warm), ref: 2.94 vs 1.82 µs at mostly_empty (smoke),
  and +0.5 to 1.0 µs at every size in the full run. That is about two dependent DRAM round trips:
  descriptor, then data.
- *Code cold*, sweep (full run): 0 to 1 µs when the path ran in the previous iteration.
  **+3.3 µs** at the first two radix sizes after a switch from bitonic (ref, 4-tier). **+8.9 µs**
  for the unrolled radix3 3-tier. **+7.2 µs** for the 4-tier at 5120, its first pass2-radix size.
- Rolled radix3 (14 KB DXIL): **+1.0 µs** at every size, i.e. the data-only cost. Rolling the pass
  loop removes the code penalty.
- *Code fully cold* (the serial smoke, see 2): the unrolled radix3 has a floor of 16.8 µs vs
  7.5 µs with its code warm. Rolled: 8.8 vs 8.0. radix_sort2: 11.9 vs 7.4. pass2 radix: 13.0 vs
  (full run) 10.9.
- The microbenchmark shows that sequential cold code streams well. The penalty therefore comes
  from the shape of the code, not its volume. My guess: every jump into cold code (uniform
  branches over untaken paths, a new phase after each barrier) waits a full memory latency. The
  big shaders have many such jumps, the rolled loop has few.
- Other things the `data` warm-up also warms: register spills (local memory), constant or
  descriptor caches. These cannot be told apart here (no SASS / profiler access). Spills are
  plausible for the big unrolled shaders at 64 registers per thread.

**2. The 256 MB flush does not make code reliably cold.** It evicts L2 (the data effects are
clear), but code used by the previous iteration or two survives it. That code is most likely in the
SMs' instruction caches, which a data flush does not touch.
- In the full run, a size whose path matches the previous iteration's path pays no code penalty.
  Only the first one or two sizes after a path switch pay (table above).
- In the serial smoke run (one command list per iteration, CPU wait in between) the unrolled
  shaders pay the full penalty at *every* size: s4_radix2 is 11.9 µs at 32 elements vs 8.4 in the
  full run, s1_radix 13.0 vs 10.85. The rolled radix3 is the same in both (8.77 vs 8.74). The
  "serial smoke bias" that pass3 attributed to GPU idle is this code effect: code does not survive
  from one command list to the next.
- So the default mode measures code that is *partially* warm, depending on workload order. The
  whole-batch workloads mix all paths in most iterations, so their code is mostly warm.

**3. What is realistic for the engine case** (the sort runs once per frame, after a lot of other
work):
- Code: fully cold, colder than in our default mode. Other shaders evict it from the SM
  instruction caches and from L2.
- Data: probably *warm* in L2 (the keys were just written by a previous pass), like the `code`
  mode.
- **The serial smoke numbers are the closest model of that case**, and the default full run is
  optimistic for large shaders. I kept the default (`full`) unchanged for comparability with
  pass0-pass3. For the next pass I recommend a flush that also evicts instruction caches (next
  steps), then measuring with `code` ordering (data warm, code fully cold).

**4. With warm code, the 4-tier shader is the per-size optimum** (`s4_4tier_4096@data`):
- It is the best row at every size ≥ 2048: 8.77 / 9.34 / 10.75 µs at 2560 / 3072 / 4096, vs
  10.91 / 11.49 / 12.48 for the reference. At 5120-8192 its pass2 radix path equals the reference.
- Whole batches: realistic_mix 8.61 / 14.91 (ref@data 8.77 / 15.20), mostly_large 15.78 (16.10),
  worst_case 17.74 (18.11).
- Switch point: with warm code, radix_sort2 is slower than the pass2 radix at 5120 (smoke `@data`:
  14.11 vs 13.54), so 4096 is the right switch. In the default mode the 4096 and 5120 variants tie
  on the whole batches (realistic_mix 9.84 vs 9.89).
- With code as in the default mode, the 4-tier has the best realistic_mix median (9.84 vs 9.98)
  but much worse p95 values: realistic_mix 20.80 vs 17.02, mostly_large 23.14 vs 18.40. Its pass2
  radix sits at the end of a 50 KB shader and is cold whenever the previous iterations did not use
  it (+7 µs at 5120).

**5. radix_sort3: the single-wave scan does not beat the pass2 radix.** With warm code, at
5120 / 6144 / 8192:

| | 5120 | 6144 | 8192 |
|---|---:|---:|---:|
| radix_sort3, single-wave scan | 14.78 | 16.38 | 19.87 |
| radix_sort2, per-wave scan | 14.11 | 15.84 | 19.23 |
| pass2 radix | 13.28 | 14.37 | 18.21 |

The pass3 hypothesis (MIO pressure of the per-wave scan) is refuted. What remains different in
the pass2 radix:
- kpt = ceil(count / 1024), so all 1024 threads work with 5-6 keys at 5-6k, vs 8 keys on 640-768
  threads at MIN_KPT 8.
- No slot swizzle.
- No per-key offset shuffle.

The 4-tier therefore does *not* collapse to 3 tiers this pass. `RS3_PERWAVE_MAX` 3072 / 5120 made
no difference in the smoke run.

**6. The rolled radix is the robust small-code option.** `s4_3tier_rx3r` (19 KB DXIL):
- In the serial smoke (code fully cold), it is the best on realistic_mix among all default-flush
  rows: 11.18 vs 13.94 (ref4096) and 12.29 (4-tier).
- In the full run it is the best at 2560 / 3072 (11.47 / 12.18 vs 14.24-15.26 for the reference).
- It loses 1.5-3.5 µs at 5-8k (the radix3 single-wave scan and the loop overhead): mostly_large
  19.87 vs 16.45, worst_case 21.73 vs 18.24.
- The loop costs about 0.5 µs of warm floor: rolled@data 8.00 vs 7.49 unrolled@data (smoke,
  32 elements).

**7. Code size of the tiers (DXIL):** bitonic E8 alone 8.9 KB, pass2 radix alone 25.2 KB,
radix_sort2 25.3 KB, radix_sort3 37.4 KB (unrolled) / 14.0 KB (rolled). Combined: reference 30.3 KB,
4-tier 50.0 KB, rx3 3-tier 42.5 KB, rolled rx3 3-tier 19.0 KB.

## Recommended configurations

**RTX 5080, default measurement (code partly warm, data cold): `s1_rank512_bitreg4096_radix`**
(pass2 code), unchanged.
- It is the best or within 0.15 µs on all 9 workloads, with the best p95 on the large workloads.
  realistic_mix: 9.98 / 17.02, mostly_large 16.45 / 18.40, worst_case 18.24 / 21.18.
- `s4_4tier_4096` has the best realistic_mix / mostly_mid medians (9.84 / 9.60) but +3.8-5.4 µs p95
  on large sorts.

**RTX 5080, if the sort's code can be kept warm** (e.g. several sort dispatches back to back, or the
sort shader is run once as a warm-up in the same command list): **`s4_4tier_4096`**:
- rank ≤ 512, bitonic E8 ≤ 2048, radix_sort2 ≤ 4096, pass2 radix above.
- It is the best everywhere in `@data`: realistic_mix 8.61 / 14.91, worst_case 17.74 / 18.08.

**RTX 5080, engine case (code fully cold, many small sorts):** `s4_3tier_rx3r`:
- rank ≤ 512, bitonic E8 ≤ 2048, rolled radix_sort3.
- It is the best realistic_mix in the serial smoke (11.18 vs 13.94), but 2-3 µs worse on batches of
  5-8k sorts.
- It is a candidate, not yet the recommendation: the default measurement does not model this case
  well, and it rests on 3-sample smoke numbers. The next pass should measure it with an
  instruction-cache flush.

**Portable: `s1_rank512_bitreg2048_radix`** (pass2 code, now wave64-safe), unchanged.
- It is within noise of the best on the 5080: realistic_mix 10.08 / 17.57, mostly_large 17.12.
- On the 7900 XTX at wave32 (see "AMD" below) it is within noise of the best radix combinations.
- The AMD data suggests a lower rank threshold (129-512 is faster with bitonic there). That needs a
  full AMD run.

## AMD RX 7900 XTX: wave32 smoke of the pass2 shaders (package 3409bf6)

**3-iteration smoke numbers (serial), rough.** µs per iteration, median / p95; best median of the
7900 XTX per workload, against the same algorithm in this pass's 5080 full run where available:

| workload | best on the 7900 XTX (wave32) | 7900 XTX | ref2048 on the 7900 XTX | ref2048 on the 5080 (full run) |
|---|---|---:|---:|---:|
| mostly_empty | rank-based single dispatch (s1_rank1024_radix) | 1.60 / 1.92 | 1.64 / 1.68 | 3.65 / 4.74 |
| mostly_small | rank-based (s1_rank512_bitreg / _radix) | 2.00 / 2.32 | 2.12 / 2.12 | 3.74 / 4.64 |
| realistic_mix | s1_rank512_radix | 11.44 / 17.04 | 11.60 / 16.88 | 10.08 / 17.57 |
| mostly_large | s1_rank512_bitreg2048_radix | 33.60 / 34.24 | 33.60 / 34.24 | 17.12 / 18.78 |
| worst_case | t2_rank512_radix | 37.60 / 38.80 | 37.84 / 38.84 | 19.07 / 21.76 |
| edges | s1_radix | 32.96 / 33.00 | 33.16 / 33.28 | 18.72 / 19.97 |
| mostly_mid | s1_radix | 10.08 / 10.20 | 14.52 / 15.08 | 9.63 / 10.43 |
| mostly_medium | s1_bitreg (bitonic for all sizes) | 5.24 / 5.28 | 7.28 / 7.32 | 5.73 / 6.46 |

- **Small sorts are about 2× faster than on the 5080** (rank floor 1.6 vs 3.6 µs). **Large
  sorts are 2× slower**: radix 33-38 µs vs 17-19 µs, and bitonic E8 at 8192 is about 65 µs vs 31.
- **Rank sort loses to bitonic at 129-512 on AMD** (mostly_medium: rank tiers 7.2-7.6 vs bitonic
  5.24). On the 5080 rank wins that range (5.7 vs 6.6). So an AMD configuration wants
  RANK_MAX ≈ 128 (mostly_small: rank 2.0 vs bitonic 4.2).
- **513-2048 on AMD:** the pass2 radix (10.08) beat bitonic (s1_bitreg 10.60; s1_rank512_bitreg
  13.64). Three samples are too few to set a threshold.
- The integrated "AMD Radeon(TM) Graphics" is 5-15× slower (realistic_mix 44.6 µs with ref2048).
- The wave64 run of that package failed (see above), so there are no valid wave64 numbers yet.

## Next steps

1. **Instruction-cache flush.** Extend the flush with a shader that executes a large amount of
   distinct code on every SM (several hundred KB of straight-line code, or many PSOs). Then re-run
   the key algorithms with `full` and `code` ordering, to measure code that is really cold, as in
   the engine case. Consider making that the default then (it changes all absolute numbers).
2. **Small, rolled large-sort code:**
   - A rolled pass2-style radix: kpt = ceil(count / 1024), wave-0 table scan, `RsSelect8`.
   - Or radix_sort2 rolled with `RS2_MIN_KPT` 1 above 4096 (all 1024 threads active).
   - Target: the pass2 radix's large-size speed at radix3_rolled's code size (14 KB). A single rolled
     radix for 2049-8192 would then give a small 3-tier shader.
3. **Order the code by likelihood.** Put the common tiers first and the rare large paths in a
   compact block. Measure path switches with the sweep order (or randomize the sweep order, so that
   path-switch costs appear in every size instead of only after a switch).
4. **AMD:** run the new package on the 7900 XTX (wave32 and wave64) to confirm the wave64 fix, then
   a full AMD run to set RANK_MAX (≈128?) and the bitonic/radix threshold there.
5. **Warm-up in the engine:** if the sort can run right after another dispatch of the same PSO, the
   warm-code numbers apply. A tiny dummy dispatch of the sort PSO early in the frame would not help,
   because other shaders evict the code again.

To re-run: `GpuSort.exe --shaders _test/pass4` (15-algorithm full set). The diagnostic set is
`algorithms_diag.txt`. To run it, copy it over algorithms.txt in a scratch copy of the directory and
use `--smoke --dred --iterations 48`.
