# pass5: flush-tail fix (drain), revised cold-code finding, run-time estimate, wave64 root cause

RTX 5080 only (driver 32.0.16.1714) for the new measurements. Inputs from other machines (7900 XTX,
Ryzen iGPU, Intel UHD 770) are archived and analysed in _test/external/notes.md. No sort shader
changed: `_test/pass5/` = `shaders/` = the pass4 shaders (one comment in `common.hlsli` corrected,
DXIL bit-identical at W = 16 / 32 / 64) with a new `algorithms.txt`.

**Measurement change (read this first).** The default flush mode `full` now ends with a *drain*
(a one-group dispatch + UAV barrier) right before the start timestamp. Results of pass0-pass4 and
all external results so far were measured without it (now flush mode `full_legacy`) and are **not
directly comparable** with `full` results from pass5 on. On the RTX 5080 the difference is small
(`full_legacy` is 0.1-0.35 µs slower, about 0.25 µs on average); on the RX 7900 XTX the legacy method
added ~13 µs to most iterations.

## Summary

1. **Flush tail.** The 7900 XTX's `full` numbers carried ~13 µs of the 256 MB flush's tail in the
   timed window (bimodal, `full` minima = `@data` minima). Fix: drain dispatch + barrier before the
   start timestamp, the default for `full`, `code`, `data` and `none`. On the 5080 the tail was only
   0.19-0.30 µs (ref2048, per workload); the drained `full` reproduces pass4's `full` to within
   ~0.1 µs (legacy mode in this run measured ~0.3 µs slower than pass4, run-to-run drift).
2. **Cold code, revised.** The tail explains almost nothing of pass4's cold-code findings on the
   5080. With the drain, the fixed cold cost of an iteration (ref2048, whole-batch workloads) is
   data cold 1.24-1.68 µs (`full` - `code`) > code cold 0.80-1.30 µs (`full` - `data`), together
   ≈ `full` - `none` (1.8-2.4 µs). The big code effects remain unchanged: +3.3 / +3.7 µs at the first
   radix sizes after a path switch (2560 / 3072) and +6.2 µs for the 4-tier's pass2 radix at 5120.
   Of the serial smoke's "code fully cold" penalty for `s1_radix`, 0.6 µs was flush tail; 1.4 µs
   remain.
3. **Read-only flush (`full_ro`) is not a replacement on NVIDIA**: it measures ~1 µs *faster* than
   the drained `full` (mostly_empty 2.56 vs 3.65, faster than `@data` 2.85), i.e. a read-only
   stream leaves the sort's data warmer (or the dirty lines of the read+write flush make later
   misses slower). Kept as an AMD diagnostic.
4. **Run-time estimate**: calibrated once per GPU up front; 5080 full run estimated 112 s, actual
   105.6 s.
5. **Wave64 root cause**: pass4's "32-lane halves" explanation is refuted (probe + analysis). Most
   likely a driver miscompile of the pass2 shuffle scans at wave64; not confirmed. New probe tests
   distinguish the two remaining mechanisms on the next AMD run.

## What changed

### Framework
- **Drain** (`src/Benchmark.cpp`, `Record`): after the flush's UAV barrier, `Dispatch(1)` of the
  flush PSO on a new private 4 KB buffer (`m_drainBuffer`, 256 x uint4, one element per thread)
  through its own descriptor table (heap slots 128-130: two null SRVs + the UAV, >= 4 KB after the
  sort's table so no descriptor cache line is shared), then a UAV barrier, then the start timestamp.
  Used by every mode except `full_legacy` and `full_ro`.
- **Flush modes** (`FlushMode`, `--flush-mode`, `flush` lines in algorithms.txt): `full` (drained,
  default), `full_legacy` (pre-pass5 `full`), `full_ro` (new read-only flush entry point
  `main_ro`, no drain; its store condition is provably never true), `code` / `data` / `none` (now
  with the drain).
- **Run-time estimate** (`GpuBenchmark::Calibrate`, main.cpp): see "Run-time estimate" below.
- **Wave probe** (`src/WaveProbe.cpp`): three shuffle-scan replicas of the pass2 radix, see "Wave64
  root cause".
- results.txt header: the drain, the incomparability note, the flush-mode legend and the run-time
  estimate (per GPU: ms per iteration, estimate, actual). The algorithm list's flush-mode column is
  wider (`full_legacy`). `GpuInfo::uma` (D3D12_FEATURE_ARCHITECTURE) for the prompt's guess. The
  progress window is 25 px taller (one more line).
- README_PORTABLE.txt: realistic run times per GPU class and the estimate.

### Algorithm set (`shaders/algorithms.txt` = `_test/pass5/algorithms.txt`, 16 algorithms)
The pass4 shaders. The reference `s1_rank512_bitreg2048_radix` in every mode (`full`, `@legacy`,
`@ro`, `@data`, `@code`, `@none`), `s1_rank512_bitreg4096_radix`, `s4_4tier_4096` (+ `@legacy`,
`@data`), `s4_3tier_rx3r` (+ `@legacy`, `@data`, `@code`), `s1_radix` (+ `@legacy`). The next
external run (the package runs `shaders/` by default) therefore shows the drained method, the
legacy method and the read-only flush side by side on AMD and Intel.

## Safety runs

| step | result |
|---|---|
| WARP `--debug --gbv --iterations 5`, 16 algorithms x 10 workloads (8 parallel shards), final build | 160 combos, 0 failures, 0 debug messages besides the GBV startup notice |
| WARP `--smoke`, 16 algorithms x 10 workloads, final build | 160 combos, 0 failures; wave probe incl. the 3 new tests OK (W = 4) |
| WARP `--wave-probe --debug --gbv` | OK, new tests OK |
| dxc: every dispatch of the set at W = 16 / 32 / 64 before and after the `common.hlsli` comment edit | disassembly identical |
| **hardware** `--smoke --dred --iterations 16`, RTX 5080 | 160 combos, 0 failures, no device errors; probe incl. new tests OK (W = 32); 6.8 s |
| **hardware** full run, 16 algorithms x 10 workloads x 1005 iterations | 160 combos, 0 failures, no nvlddmkm / dxgkrnl / WHEA events; 105.6 s |

Two hardware runs in total (one smoke, one full), each after its prompt.

## The flush tail: what was wrong and the fix

**Symptom (7900 XTX, package 5e087c3, _test/external).** In the default `full` mode every
algorithm was ~13 µs slower than the same shader in `@data` / `@code` mode, on every workload
(ref2048 full - @data: 12.7-13.1 µs on 9 of 10 workloads, at wave32 and wave64). The distribution
was bimodal: mostly_empty min 1.20, median 14.68, mean 11.67 (about a quarter of the iterations
tail-free); the `full` minima equal the `@data` minima. The serial smoke did not show it
(mostly_empty 2.66). The Ryzen iGPU and the UHD 770 did not show it. On the 5080: mostly_empty full
3.62 vs @data 3.04 vs @code 2.37, which pass4 read as "code cold costs 0.6 µs, data cold 1.25 µs".

**What differs between the modes** (pass4 recording, per iteration):

| mode | between the 256 MB flush's UAV barrier and the start timestamp |
|---|---|
| full (pass0-pass4) | nothing |
| code | the upload copies + their transition barrier |
| data | the untimed warm-up dispatches + a UAV barrier |
| serial smoke | (same as full, but every iteration is its own command list) |

**Hypothesis.** A driver may record a barrier lazily and execute its wait and cache maintenance
(for a UAV barrier after a 256 MB read+write pass: wait for the flush's waves and outstanding
writes, write back / invalidate caches) at the next dispatch or copy. In `full` mode the next
dispatch after the flush barrier is the timed sort, after the start timestamp, so that tail lands
inside the timed window. In `code` / `data` the upload copies / warm-up dispatches come first and
absorb it. Constant ~13 µs regardless of the sort and "sometimes 0" fit that picture (whether the
flush's write-back is still draining when the sort starts depends on timing).

**Options considered.**
- *Timestamp placement* (start timestamp before the barrier): puts the barrier wait, i.e. the whole
  tail, inside the window. Worse.
- *Read-only flush* (no dirty lines): removes the write-back part of the tail but also changes what
  the flush does to the caches (a read stream may be allocated differently from writes, e.g.
  evict-first on NVIDIA, MALL policies on AMD), so it is not a like-for-like replacement for the
  measurement of record. Kept as a diagnostic mode (`full_ro`, no drain): on AMD it tells whether
  the tail is the flush's dirty data (full_ro fast) or the barrier/timestamp placement (full_ro
  slow).
- *Separate command lists / ExecuteCommandLists with a fence* between flush and sort: works (the
  serial smoke is tail-free), but changes batching and adds per-iteration submission cost and GPU
  idle (clock drops on AMD, see external notes). Too heavy.
- **Drain dispatch (chosen):** after the flush's UAV barrier, a one-group dispatch of the flush
  shader on a private 4 KB buffer through its own descriptor table (128 descriptors, >= 4 KB, away
  from the sort's), then a second UAV barrier, then the start timestamp. The first barrier must be
  executed before the drain can start, the second one only has to wait for one tiny group. It is
  deterministic, adds one tiny untimed dispatch per iteration (the wall time per iteration did not
  grow: 0.66 ms in this run, 0.73 ms in pass4), touches no data, descriptor or
  code of the sort, and uses the flush PSO (no new shader). The sort's data stays as cold as before.

**Default method now** (`full`, measurement of record): upload, poison, 256 MB read+write flush,
UAV barrier, **drain + UAV barrier**, start timestamp, sort, end timestamp, readback. `code`, `data`
and `none` also end with the drain now, so every mode's timed window starts the same way; the old
method is `full_legacy`. **Results of pass0-pass4 (and every external result so far) are
`full_legacy` results and not directly comparable with pass5+ `full` numbers.**

| mode | per iteration (pass5) | code | data |
|---|---|---|---|
| full (default) | upload, flush, **drain**, sort | L2-cold | cold |
| full_legacy | upload, flush, sort (pass0-pass4 'full') | L2-cold | cold |
| full_ro | upload, read-only flush, sort (diagnostic, no drain) | L2-cold | cold (as far as a read stream evicts) |
| code | flush, upload, **drain**, sort | L2-cold | warm in L2 |
| data | upload, flush, warm-up run, **drain**, sort | warm | cold |
| none | upload, **drain**, sort | warm | warm |

## Results: full run (RTX 5080, driver 32.0.16.1714)

1000 iterations per combo, µs per iteration (20 sorts), median / p95. `@legacy` = `full_legacy`,
`@ro` = `full_ro`. Full tables (min / mean / max, sweep mean and p95) in results.txt.

| algorithm | mostly_empty | mostly_small | realistic_mix | mostly_large | worst_case | edges | mostly_mid | mostly_medium | sweep | sparse_keys |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 3.65 / 4.45 | 3.84 / 4.54 | 10.00 / 17.22 | 17.25 / 18.75 | 19.20 / 21.60 | 18.80 / 20.00 | 9.66 / 10.34 | 5.76 / 6.40 | 8.45 / 18.82 | 17.41 / 19.39 |
| s1_rank512_bitreg2048_radix@legacy | 3.90 / 4.99 | 4.03 / 4.99 | 10.29 / 17.60 | 17.47 / 19.01 | 19.39 / 21.95 | 19.10 / 20.19 | 9.89 / 10.62 | 5.95 / 6.62 | 8.66 / 19.20 | 17.66 / 19.55 |
| s1_rank512_bitreg2048_radix@ro | 2.56 / 3.14 | 2.69 / 3.17 | 8.90 / 16.35 | 16.22 / 17.82 | 18.14 / 20.38 | 17.92 / 18.62 | 8.70 / 9.15 | 4.61 / 4.99 | 8.26 / 18.02 | 16.42 / 18.34 |
| s1_rank512_bitreg2048_radix@data | 2.85 / 3.36 | 3.01 / 3.49 | 8.90 / 15.39 | 16.29 / 17.73 | 18.30 / 18.66 | 17.92 / 18.40 | 8.74 / 9.12 | 4.74 / 5.09 | 7.15 / 18.18 | 16.45 / 18.34 |
| s1_rank512_bitreg2048_radix@code | 2.30 / 2.82 | 2.37 / 2.78 | 8.64 / 15.74 | 15.78 / 17.28 | 17.70 / 19.81 | 17.50 / 18.11 | 8.42 / 8.80 | 4.10 / 4.54 | 6.77 / 17.60 | 15.97 / 17.89 |
| s1_rank512_bitreg2048_radix@none | 1.57 / 1.79 | 1.70 / 1.92 | 8.03 / 14.59 | 15.39 / 16.74 | 17.34 / 17.50 | 15.65 / 17.34 | 7.87 / 8.16 | 3.62 / 3.81 | 6.08 / 17.28 | 15.58 / 17.47 |
| s1_rank512_bitreg4096_radix | 3.62 / 4.45 | 3.81 / 4.48 | 10.02 / 17.02 | 16.64 / 18.24 | 18.38 / 20.96 | 17.95 / 19.26 | 9.66 / 10.43 | 5.73 / 6.37 | 8.45 / 18.11 | 16.80 / 18.78 |
| s4_4tier_4096 | 3.65 / 4.48 | 3.84 / 4.54 | 9.95 / 19.94 | 17.15 / 21.73 | 18.85 / 24.42 | 18.58 / 22.24 | 9.70 / 10.37 | 5.73 / 6.34 | 8.50 / 19.58 | 17.54 / 22.14 |
| s4_4tier_4096@legacy | 3.90 / 4.86 | 4.03 / 4.90 | 10.14 / 20.64 | 17.38 / 22.72 | 19.04 / 25.22 | 18.82 / 23.10 | 9.89 / 10.59 | 5.95 / 6.56 | 8.77 / 20.19 | 17.78 / 22.94 |
| s4_4tier_4096@data | 2.88 / 3.39 | 3.01 / 3.49 | 8.70 / 15.01 | 15.97 / 17.25 | 17.89 / 18.27 | 17.60 / 17.95 | 8.74 / 9.12 | 4.74 / 5.06 | 7.12 / 17.82 | 16.19 / 18.02 |
| s4_3tier_rx3r | 3.65 / 4.45 | 3.81 / 4.45 | 10.21 / 19.33 | 20.03 / 21.47 | 21.86 / 22.78 | 21.57 / 22.62 | 10.11 / 10.75 | 5.76 / 6.34 | 8.42 / 21.60 | 20.16 / 21.79 |
| s4_3tier_rx3r@legacy | 3.87 / 4.90 | 4.00 / 4.90 | 10.53 / 19.36 | 20.19 / 21.73 | 22.08 / 23.01 | 21.86 / 22.85 | 10.27 / 11.01 | 5.95 / 6.62 | 8.77 / 21.95 | 20.35 / 22.18 |
| s4_3tier_rx3r@data | 2.85 / 3.36 | 3.01 / 3.52 | 9.18 / 18.30 | 19.20 / 20.51 | 20.99 / 21.38 | 20.93 / 21.31 | 9.18 / 9.54 | 4.74 / 5.09 | 7.22 / 20.93 | 19.36 / 20.99 |
| s4_3tier_rx3r@code | 2.30 / 2.82 | 2.40 / 2.85 | 8.93 / 17.95 | 18.69 / 20.13 | 20.61 / 21.18 | 20.32 / 21.02 | 8.86 / 9.31 | 4.13 / 4.58 | 6.83 / 20.54 | 18.91 / 20.61 |
| s1_radix | 11.07 / 13.50 | 10.98 / 13.50 | 12.16 / 16.38 | 17.38 / 18.94 | 19.49 / 20.70 | 18.99 / 19.84 | 11.71 / 13.86 | 10.94 / 13.47 | 11.54 / 19.14 | 17.63 / 19.39 |
| s1_radix@legacy | 11.17 / 13.63 | 11.10 / 13.66 | 12.16 / 16.77 | 17.57 / 19.17 | 19.68 / 21.06 | 19.33 / 20.16 | 11.92 / 14.18 | 11.07 / 13.66 | 11.86 / 19.42 | 17.86 / 19.84 |

### Before / after: the reference in every mode

`s1_rank512_bitreg2048_radix`, median (min). pass4 columns: the pass4 full run (legacy method,
`@data` / `@code` without the drain).

| workload | full (drained) | full_legacy | full_ro | data | code | none | pass4 full | pass4 data | pass4 code |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| mostly_empty | 3.65 (2.37) | 3.90 (2.40) | 2.56 (2.02) | 2.85 (2.34) | 2.30 (1.82) | 1.57 (1.44) | 3.65 | 3.04 | 2.37 |
| mostly_small | 3.84 (2.53) | 4.03 (2.75) | 2.69 (2.37) | 3.01 (2.53) | 2.37 (2.05) | 1.70 (1.57) | 3.74 | 3.10 | 2.43 |
| realistic_mix | 10.00 (4.48) | 10.29 (4.58) | 8.90 (4.06) | 8.90 (4.32) | 8.64 (3.52) | 8.03 (3.26) | 10.08 | 8.77 | 8.86 |
| mostly_large | 17.25 (14.08) | 17.47 (14.46) | 16.22 (13.06) | 16.29 (13.41) | 15.78 (12.74) | 15.39 (12.61) | 17.12 | 16.10 | 16.70 |
| worst_case | 19.20 (17.98) | 19.39 (18.05) | 18.14 (17.70) | 18.30 (17.92) | 17.70 (17.44) | 17.34 (16.93) | 19.07 | 18.11 | 18.72 |
| edges | 18.80 (6.30) | 19.10 (6.59) | 17.92 (6.11) | 17.92 (6.30) | 17.50 (6.05) | 15.65 (5.54) | 18.72 | 17.81 | 16.51 |
| mostly_mid | 9.66 (8.48) | 9.89 (8.67) | 8.70 (8.38) | 8.74 (8.45) | 8.42 (8.10) | 7.87 (7.68) | 9.63 | 8.67 | 8.58 |
| mostly_medium | 5.76 (4.51) | 5.95 (4.58) | 4.61 (4.00) | 4.74 (4.29) | 4.10 (3.58) | 3.62 (3.23) | 5.73 | 4.80 | 4.26 |
| sweep | 8.45 (2.59) | 8.66 (2.75) | 8.26 (2.40) | 7.15 (2.37) | 6.77 (1.98) | 6.08 (1.47) | 8.40 | 7.10 | 6.93 |
| sparse_keys | 17.41 (13.38) | 17.66 (14.08) | 16.42 (13.02) | 16.45 (13.02) | 15.97 (12.48) | 15.58 (12.19) | 17.38 | 16.38 | 16.64 |

`full_legacy` minus drained `full`, medians, per algorithm (the flush tail on the 5080):

| algorithm | mostly_empty | mostly_small | realistic_mix | mostly_large | worst_case | edges | mostly_mid | mostly_medium | sweep | sparse_keys |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | +0.25 | +0.19 | +0.29 | +0.22 | +0.19 | +0.30 | +0.23 | +0.19 | +0.21 | +0.25 |
| s4_4tier_4096 | +0.25 | +0.19 | +0.19 | +0.23 | +0.19 | +0.24 | +0.19 | +0.22 | +0.27 | +0.24 |
| s4_3tier_rx3r | +0.22 | +0.19 | +0.32 | +0.16 | +0.22 | +0.29 | +0.16 | +0.19 | +0.35 | +0.19 |
| s1_radix | +0.10 | +0.12 | +0.00 | +0.19 | +0.19 | +0.34 | +0.21 | +0.13 | +0.32 | +0.23 |

- **On the 5080 the tail is small and constant**: 0.2-0.3 µs on every workload and algorithm, with
  no bimodality (min 2.37 drained vs 2.40 legacy). In the serial smoke it is larger: ref2048
  mostly_empty 3.41 drained vs 4.19 legacy, rx3r 3.38 vs 4.32, s1_radix 12.48 vs 13.14 (0.7-0.9 µs;
  with a CPU wait before each command list the GPU starts the flush from idle).
- **Run-to-run:** the `@legacy` rows are the pass4 method but measure 0.2-0.45 µs above pass4's
  `full` (same shaders, same driver), i.e. this run was a little slower overall; the new `@data`
  rows are within -0.2..+0.2 µs of pass4's `@data` (drain vs none). So the drained `full` ≈ pass4's
  `full` in absolute terms on this machine, and all pass4 conclusions about the 5080's ranking stand.
- **`full_ro`** is ~1 µs faster than the drained `full` on every workload except sweep: not
  equivalent (see summary point 3). The tail hypothesis cannot be tested with it on NVIDIA; on the
  7900 XTX, `@ro` vs `@legacy` will show whether the 13 µs are the flush's dirty lines.

### Cold data vs cold code, re-checked with the drain

ref2048, medians, whole-batch workloads:

| workload | data cold (full - code) | code cold (full - data) | both (full - none) | sum of the two |
|---|---:|---:|---:|---:|
| mostly_empty | 1.35 | 0.80 | 2.08 | 2.15 |
| mostly_small | 1.47 | 0.83 | 2.14 | 2.30 |
| realistic_mix | 1.36 | 1.10 | 1.97 | 2.46 |
| mostly_large | 1.47 | 0.96 | 1.86 | 2.43 |
| worst_case | 1.50 | 0.90 | 1.86 | 2.40 |
| mostly_mid | 1.24 | 0.92 | 1.79 | 2.16 |
| mostly_medium | 1.66 | 1.02 | 2.14 | 2.68 |
| sparse_keys | 1.44 | 0.96 | 1.83 | 2.40 |

Sweep medians by size, `full` - `@data` (code cold) and `@legacy` - `full` (tail):

| | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ref2048 full - @data | +0.74 | +0.77 | +0.74 | +0.90 | +1.02 | +1.06 | +0.77 | +0.96 | +0.96 | +0.82 | **+3.25** | **+3.65** | +0.94 | +0.85 | +0.96 | +0.90 |
| ref2048 @legacy - full | +0.25 | +0.26 | +0.19 | +0.19 | +0.16 | +0.19 | +0.32 | +0.19 | +0.16 | +0.35 | +0.51 | +0.19 | +0.24 | +0.22 | +0.11 | +0.19 |
| 4tier full - @data | +0.77 | +0.73 | +0.76 | +0.80 | +0.99 | +1.12 | +0.96 | +0.96 | +0.93 | +0.93 | **+3.35** | **+3.47** | +1.04 | **+6.20** | +0.83 | +0.96 |
| rx3r full - @data | +0.77 | +0.74 | +0.86 | +0.86 | +1.02 | +1.12 | +0.87 | +0.99 | +0.90 | +0.93 | +1.22 | +1.09 | +0.97 | +0.76 | +0.93 | +0.85 |
| pass4 ref2048 full - @data | +0.44 | +0.54 | +0.48 | +0.54 | +0.73 | +0.80 | +0.83 | +0.90 | +0.86 | +0.92 | +3.33 | +3.77 | +0.98 | +0.96 | +0.93 | +0.86 |

**Revised cold-code conclusion (replaces pass4 conclusion 1 for the default measurement):**
- The flush tail was *not* hiding in the code-cold numbers on the 5080: it is 0.2-0.3 µs and it
  inflated `full` and `@data` alike in pass4 (`@data` also had its barrier right before the
  timestamp, but after a tiny dispatch, so less tail). pass4's reading "`@code` faster than `@data`"
  was right, and the drain makes it clearer.
- **For whole batches, cold data is the larger fixed cost** (1.2-1.7 µs, about two dependent DRAM
  round trips: descriptors, then keys), cold code the smaller (0.8-1.1 µs, a little more than pass4's
  0.44-0.8 at the small sizes, because the drained `@data` got ~0.2 µs faster). The two add up to
  `full` - `none` within ~0.3-0.5 µs.
- **Cold code still dominates where the instruction stream is really cold**: the first sizes after a
  path switch (+3.25 / +3.65 µs at 2560 / 3072 for ref2048, +6.20 µs at 5120 for the 4-tier, both
  as in pass4), the unrolled radix_sort3, and the serial smoke. None of these moved with the drain.
  The rolled radix (`s4_3tier_rx3r`) again pays no switch penalty (+1.09-1.22 µs at 2560 / 3072).
- **Serial smoke ("code fully cold")**: `s1_radix` mostly_empty serial 12.48 vs batched 11.07
  (drained) = 1.4 µs, vs 13.14 / 11.17 = 2.0 µs legacy; pass4 reported 13.02 vs 10.85. So roughly a
  third of that serial penalty was flush tail, two thirds is real cold code. (The unrolled
  radix_sort3's 16.8 vs 7.5 µs serial floor in pass4 is ~10x the tail and stands; not re-measured.)
- So code size and layout remain the target for the large, rarely used tiers (switch penalties
  and the engine's fully cold case), while for the common small sorts the fixed cost is mostly data
  latency, which no shader change removes.

### Sweep, median by sort size

| algorithm | 32 | 64 | 128 | 256 | 384 | 512 | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 6144 | 8192 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 3.62 | 3.68 | 3.81 | 4.32 | 4.99 | 5.92 | 7.33 | 7.55 | 9.66 | 9.78 | 14.26 | 15.23 | 13.52 | 14.24 | 15.39 | 19.17 |
| s1_rank512_bitreg2048_radix@legacy | 3.87 | 3.94 | 4.00 | 4.51 | 5.15 | 6.11 | 7.65 | 7.74 | 9.82 | 10.13 | 14.77 | 15.42 | 13.76 | 14.46 | 15.50 | 19.36 |
| s1_rank512_bitreg2048_radix@ro | 2.53 | 2.56 | 2.69 | 3.17 | 3.81 | 4.74 | 6.50 | 6.56 | 8.64 | 8.90 | 13.18 | 14.21 | 12.42 | 13.17 | 14.18 | 18.11 |
| s1_rank512_bitreg2048_radix@data | 2.88 | 2.91 | 3.07 | 3.42 | 3.97 | 4.86 | 6.56 | 6.59 | 8.70 | 8.96 | 11.01 | 11.58 | 12.58 | 13.39 | 14.43 | 18.27 |
| s1_rank512_bitreg2048_radix@code | 2.27 | 2.27 | 2.37 | 2.82 | 3.36 | 4.32 | 6.40 | 6.34 | 8.38 | 8.61 | 13.15 | 14.37 | 12.10 | 12.74 | 13.63 | 17.66 |
| s1_rank512_bitreg2048_radix@none | 1.73 | 1.79 | 1.89 | 2.40 | 3.01 | 3.90 | 5.92 | 5.92 | 8.00 | 8.26 | 10.30 | 10.93 | 11.78 | 12.38 | 13.31 | 17.36 |
| s1_rank512_bitreg4096_radix | 3.71 | 3.74 | 3.78 | 4.35 | 5.02 | 5.95 | 7.52 | 7.55 | 9.63 | 9.92 | 14.46 | 15.04 | 15.87 | 16.10 | 14.78 | 18.42 |
| s4_4tier_4096 | 3.71 | 3.74 | 3.74 | 4.29 | 4.96 | 5.98 | 7.49 | 7.58 | 9.60 | 9.92 | 12.21 | 12.85 | 11.92 | 19.58 | 15.23 | 18.85 |
| s4_4tier_4096@legacy | 3.87 | 3.94 | 4.13 | 4.51 | 5.22 | 6.11 | 7.65 | 7.74 | 9.86 | 10.10 | 12.43 | 13.30 | 12.11 | 20.16 | 15.46 | 19.17 |
| s4_4tier_4096@data | 2.94 | 3.01 | 2.98 | 3.49 | 3.97 | 4.86 | 6.53 | 6.62 | 8.67 | 8.99 | 8.86 | 9.38 | 10.88 | 13.38 | 14.40 | 17.89 |
| s4_3tier_rx3r | 3.71 | 3.78 | 3.87 | 4.32 | 5.12 | 5.98 | 7.49 | 7.68 | 9.97 | 10.53 | 11.65 | 12.29 | 13.74 | 16.54 | 18.40 | 21.86 |
| s4_3tier_rx3r@legacy | 3.87 | 3.90 | 4.06 | 4.45 | 5.22 | 6.08 | 7.71 | 7.81 | 10.24 | 10.62 | 11.81 | 12.51 | 13.84 | 16.80 | 18.53 | 22.06 |
| s4_3tier_rx3r@data | 2.94 | 3.04 | 3.01 | 3.46 | 4.10 | 4.86 | 6.62 | 6.69 | 9.07 | 9.60 | 10.43 | 11.20 | 12.77 | 15.78 | 17.47 | 21.01 |
| s4_3tier_rx3r@code | 2.27 | 2.30 | 2.46 | 2.75 | 3.46 | 4.35 | 6.30 | 6.40 | 8.78 | 9.22 | 10.11 | 11.20 | 12.22 | 15.20 | 17.02 | 20.58 |
| s1_radix | 10.88 | 10.88 | 10.91 | 10.88 | 10.91 | 10.94 | 11.07 | 11.20 | 11.26 | 12.06 | 12.29 | 13.81 | 13.78 | 14.43 | 15.49 | 19.42 |
| s1_radix@legacy | 11.01 | 11.04 | 11.07 | 11.10 | 11.07 | 11.07 | 11.14 | 11.52 | 11.52 | 12.21 | 12.42 | 14.40 | 13.86 | 14.62 | 15.58 | 19.71 |

The configuration ranking of pass4 is unchanged: `s1_rank512_bitreg4096_radix` has the best large
workloads (mostly_large 16.64, worst_case 18.38, edges 17.95), `s4_4tier_4096` the best
realistic_mix median (9.95) with 2.7-3.0 µs worse p95 (realistic_mix 19.94 vs 17.22, mostly_large 21.73 vs 18.75 for ref2048), `s4_3tier_rx3r` is 2.8-3.5 µs slower on
5-8k batches, and `s4_4tier_4096@data` is the per-size optimum with warm code (8.86 / 9.38 / 10.88
at 2560 / 3072 / 4096).

## Run-time estimate

Users on slow integrated GPUs had no idea a run takes 20-25 minutes (the prompt's guess assumed
2.5 ms per iteration for every GPU). Now:
- **Prompt** (before any GPU work): a rough guess per GPU, 0.8 ms per iteration for a discrete GPU
  and 10 ms for a UMA one (D3D12_FEATURE_ARCHITECTURE::UMA; iGPUs and WARP), 20 / 70 ms for a
  serial smoke run, marked as a guess.
- **Calibration, once per GPU up front** (right after the OK, before the first GPU's wave probe and
  before any sort shader runs): a `GpuBenchmark` without algorithms runs `Calibrate()`: an untimed
  batch of 8, then 32 timed iterations of the per-iteration work *without a sort* (upload, poison,
  256 MB flush, drain, timestamps, readback) with the data of the first selected workload; the
  CPU-side data generation is outside the timed part (a run overlaps it with the GPU). 2 iterations
  on WARP, 4 serial ones for `--smoke`. The fixed cost dominates an iteration on every GPU measured
  (the sort adds 0.1-5 % on average), so wall s/iteration x iterations predicts the run. It costs
  ~30 ms on the 5080 and ~0.4 s on the iGPUs. If it fails, the guess is used; a device loss during
  the calibration stops the run like any other device loss.
- **Printed**: per GPU `[i] <name>  0.75 ms per iteration x N iterations = ~X` and the total, in
  the console; the progress window shows "Elapsed: ..., estimated remaining: ~... (total ~...)",
  refined with the GPU's measured rate once 256 iterations are done (plus the calibrated estimates
  of the GPUs still to come); the console prints the same after every workload. The results header
  has "Run-time estimate: ~X s up front" and per GPU "run-time estimate: 0.75 ms per iteration
  (calibrated up front) -> ~X s for this GPU, actual Y s".
- Iteration counts and the flush size are unchanged.

## Wave64 root cause (analysis; no AMD hardware here)

**What failed.** Package 3409bf6 (pass2 shaders = git dd0de70 `shaders/`; `git diff dd0de70
3409bf6 -- shaders` is empty) on the RX 7900 XTX with `--wave-size 64`: `s1_radix` (the pass2 LDS
radix for every size) failed 3 of 3 mostly_empty iterations with output[0] of a small sort (count
47, 46, 4) still holding the poison 0xDEADBEEF and index 1 holding a real key. Regenerating the
workload shows that each reported sort is the *first non-empty sort* of its iteration (the
verifier reports only the first failure per iteration), so probably every non-empty sort failed,
not a data-dependent few. The keys at index 1 are genuine input keys (0x13B6: input lane 26, true
rank 3; 0x4510: true rank 12; 0xD431: the largest of the 4 keys).

**pass4's explanation is refuted.** pass4 assumed a per-lane-index `WaveReadLaneAt` only reads
within the reader's 32-lane half at wave64. Two independent arguments against it:
1. The wave probe (package 095c4a9) shows cross-half reads working on both AMD GPUs at
   [WaveSize(64)]: `lane ^ 32` and `(lane + 16) % 64` return the right lanes.
2. The model cannot produce the symptom. For a count-4 sort (kpt = 1, lanes 0..3 of wave 0 hold
   keys) the lane prefixes only read lanes 0..2, all in the lower half; the half-wave model only
   *under*-counts the wave totals, so every digit base stays 0 and the key with the smallest digit
   is written to output[0], with a wrong or stale value, but written. An emulation of the pass2 code
   under that model (and under "cross-half reads return 0 / own value / 0xFFFFFFFF / lane mod 32")
   always writes output[0]. pass4's emulation statement "output slots stay unwritten, which
   matches the poison at index 0" was wrong (most likely an artifact of how it initialised LDS).

**What the pass2 code does differently at wave64** (dxc 1.8.2502.11, the same version as the
shipped dxcompiler.dll; `s1_radix` = single_pass.hlsl, GROUP_SIZE 1024, RANK_MAX 0, LARGE_RADIX 1):

| build | `waveReadLaneAt` | other wave ops |
|---|---:|---|
| pass2, W = 32 | 272 | none |
| pass4 (fixed), W = 32 | 272 (bit-identical DXIL) | none |
| pass2, W = 64 | 416 | none |
| pass4 (fixed), W = 64 | 0 | 64 `wavePrefixOp`, 32 `waveActiveOp` |

- The shuffle indices are **masked** (`(lane - d) & (WAVE_SIZE - 1)`, in the DXIL `add i32 %lane,
  63-d+1` / `and i32 .., 63`), so there is no out-of-range or underflowed lane index; the `lane >= d`
  guard is a branch-free `select`. The pass2 DXIL is a well-defined program, and a CPU emulation
  with correct wave semantics sorts the exact failing sorts (and all 16 non-empty sorts of the 3
  iterations) correctly at W = 64.
- Step 2 (lane prefixes of the digit counts): 8 interleaved chains of 6 shuffles, every lane
  active. The 16-bit-field packing (`RS_PREFIX8` = 0 because 63 x 8 > 255) is used at W = 64 by the
  old *and* the fixed code, so it is not the cause.
- Step 4 (wave 0 scans the per-wave table): **the only control flow that exists only at wave64.**
  With 16 waves per group, `if (w < RS_NUM_WAVES) sum += gsLds[...]` becomes `if (lane < 16)`, a
  divergent branch around the groupshared load right before the shuffle scan and
  `WaveReadLaneAt(inclusive, 63)` (8 times per pass). At W = 32 (32 waves) the condition is always
  true and dxc removes it.
- The fixed code replaces every one of these shuffles with `WavePrefixSum` / `WaveActiveSum`.

**Candidates.**

| candidate | verdict | why |
|---|---|---|
| per-lane `WaveReadLaneAt` limited to 32-lane halves (pass4) | ruled out | probe passes; model writes output[0] |
| cross-half reads return 0 / own value / all-ones / lane mod 32 | ruled out | emulation: output[0] written (all-ones would give "0xDEAD after 0xDEAD" at a late index) |
| out-of-range / underflowed lane index | ruled out | indices are masked `& 63` in the DXIL |
| 16-bit field overflow (W = 64-only packing) | ruled out | the fixed code uses the same packing and passes |
| barrier / LDS race | ruled out (low) | barriers identical in old and new; new passes 1000-iteration runs |
| lane != SV_GroupIndex % 64 | ruled out | probe: lane mapping OK |
| **H1: after the `lane < 16` divergent load the scan runs without reconvergence** | possible | only lanes 0..15 active: their prefixes stay right, but `WaveReadLaneAt(inclusive, 63)` reads an inactive lane (stale register): wrong digit totals in every pass; in the emulation this leaves output[0] unwritten |
| **H2: the driver's wave64 lowering of the interleaved per-lane-index shuffle chains** (bpermute + permlane64 + select sequence, a hazard / missing wait, or a wrong rotate pattern match that drops the `lane >= d` guard) | possible | a dropped guard (wrap-around) leaves output[0] unwritten and gives "index 1 after 0xDEAD" in 2 of 3 emulated iterations; one-shot probe patterns would not catch it |

The timing supports a heavy emulation sequence rather than a single half-wave instruction: the old
`s1_radix` took 29 µs at wave64 on mostly_empty, the fixed one 11.4 µs (smoke) and the old one at
wave32 9.2 µs, i.e. ~18 µs for 416 shuffles.

**Most likely root cause: a driver compiler bug (32.0.11037.4004, wave64) in the pass2 shuffle-scan
code**, H1 or H2. Confidence that it is a driver issue rather than a shader bug: high (the pass2
DXIL is valid and its logic is correct on the failing data; the only change of the fix is removing
exactly these shuffles; simple cross-half shuffles work). H1 vs H2: undecided (slight lean to H1,
the one wave64-only piece of control flow). Proven: DXIL validity and differences, correctness of
the logic on the exact failing data, and that the half-wave model cannot leave output[0] unwritten.
Inferred: everything about how AMD's compiler lowers these ops.

**Probe tests added (pass5, `src/WaveProbe.cpp`).** Every wave probe now also runs, per thread:
- `shflscan x8`: the pass2 `WaveInclusiveSumShfl` verbatim (loop bound and lane from the WAVE_SIZE
  define and SV_GroupIndex, masked index, `lane >= d` guard) on 8 interleaved chains of pseudo-random
  values 0..3, every lane active (the step-2 pattern). Checked against a CPU reference.
- `shflscan after if`: wave 0 loads a 16-entry groupshared table under `if (lane < 16)`, then the
  same shuffle scan and `WaveReadLaneAt(inclusive, WAVE_SIZE - 1)` (the step-4 pattern; divergent at
  W >= 32, and exactly the pass2 code shape at W = 64).
- `shflscan after select`: the same with a branch-free load (control).

They are only checked where the observed lane count equals the WAVE_SIZE define, and a mismatch is
reported as a cross-lane note, not as a failed configuration (the current shaders use these scans
only at W <= 32, where every run passes). On the next AMD run (`run_all.bat probe`, or any run with
`--wave-size 64`), read the [WaveSize(64)] line:

| shflscan x8 | after if | after select | meaning |
|---|---|---|---|
| FAIL | - | - | H2: the shuffle-scan chains themselves are miscompiled at wave64 with every lane active |
| OK | FAIL | OK | H1: no reconvergence / inactive lanes after the divergent load |
| OK | OK | OK | the bug needs the full sort (register pressure, scheduling); next: split `RS_WAVE_INTRINSICS` into a step-2 and a step-4 switch and smoke-run the two hybrids of the old `s1_radix` at `--wave-size 64` |

They pass on WARP (W = 4, with GBV) and on the RTX 5080 (W = 32).

**Register bitonic (pass2) at wave64.** 9 per-lane-index `WaveReadLaneAt` (`hwLane ^ (1 << q)`), all
lanes active. For counts <= 64 all real data sit in lanes 0..7 and the only cross-half stage pairs
them with padding, so `s1_bitreg` passing mostly_empty at wave64 says nothing about cross-half
shuffles. (It was slow, 10 µs vs 4.2 at wave32, partly real extra work: 512 padded elements and 45
stages vs 36.)

Corrected accordingly: _test/pass4/notes.md (intro, analysis points 2-3, "proven vs assumed"), the
pass4 addenda in _test/pass2/notes.md and _test/pass3/notes.md, and the comment in
`shaders/common.hlsli` (comment only, DXIL unchanged). The shader comments in the pass2-pass4
snapshots are left as they were (historical).

## Recommendations and next steps

- **Configurations** (unchanged by the new method): RTX 5080 `s1_rank512_bitreg4096_radix`; with
  warm code `s4_4tier_4096`; portable `s1_rank512_bitreg2048_radix` (2048 is also the right radix
  switch on both iGPUs, see _test/external/notes.md); engine case with fully cold code:
  `s4_3tier_rx3r` remains the candidate.
- **Next AMD run** (the default package run does it): confirm the drained `full` ≈ `@data` +
  code-cold cost on the 7900 XTX (i.e. the 13 µs are gone), and read `@ro` vs `@legacy` (dirty
  lines or barrier placement). Run `run_all.bat probe` to read the three new shuffle-scan tests at
  [WaveSize(64)].
- **Intel**: try a 256- or 512-thread radix group for the large tier (occupancy, see external
  notes), and a wave16-specific table scan.
- **AMD thresholds**: a variant with radix_sort2 for 513-4096 (the iGPU prefers radix from 768) and
  RANK_MAX 128 on the 7900 XTX, now measurable without the tail.

To re-run: `GpuSort.exe --shaders _test/pass5` (16 algorithms, ~2 min on the 5080).
