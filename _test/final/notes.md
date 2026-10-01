# Final run (2026-09-29 / 2026-09-30)

The final benchmark: `run_all.bat final` (shaders\algorithms_final.txt, 69 algorithm entries incl.
the flush-mode variants of s1_rank512_bitreg2048_radix) on three machines, merged with
`tools/aggregate_results.py`. The interactive results page built from this data is
tools/results_page/ (published: https://claude.ai/artifact/3PHgWLiq9ej94L2pM3wWZZ).

## Files

| file | content |
|---|---|
| all_results.csv | every benchmark results row of the three machines (12,075 rows = 7 GPU configurations x 1725; schema 4 for STIMULATOR and JONAS-CPH, schema 5 for IMS-KCOMBS-INT, + column `source`); smoke runs left out |
| all_samples.csv.gz | the per-iteration samples of those rows (3,864,000 rows, 387 MB unpacked, 30 MB gzip -9); `python tools/results_page/prep.py` reads it directly |
| all_wave_probe.csv | the wave probe rows of every benchmark run (10 rows) |
| aggregate_summary.txt | the aggregation summary (runs, GPUs, algorithms, problems) |

The per-run result files (without the samples and logs) are in
_test/external/STIMULATOR_20260929_1529, _test/external/JONAS-CPH_20260929_1658 and
_test/external/IMS-KCOMBS-INT_20260930_1653. The raw zips with the full samples and logs:
`G:\My Drive\STIMULATOR_20260929_1529.zip`,
`C:\Users\jonas\Desktop\GpuSort-portable-3ebe93b\results\JONAS-CPH_20260929_1658.zip` and
`C:\Users\jonas\Downloads\IMS-KCOMBS-INT_20260930_1653.zip`.

Regenerate: `python tools/aggregate_results.py "G:\My Drive\STIMULATOR_20260929_1529.zip"
<JONAS-CPH results folder or zip> <IMS-KCOMBS-INT zip> -o <dir>` (the default output dir is this
folder; it writes an uncompressed all_samples.csv, which is git-ignored here; the archived files
were made with the JONAS-CPH folder, not its zip, which only changes the `source` column).

## Machines

| computer | GPU | vendor / device | driver | wave | package | iterations |
|---|---|---|---|---|---|---|
| STIMULATOR | AMD Radeon RX 7900 XTX | 1002 / 744C | 32.0.11037.4004 | 32 (default) and 64, both with [WaveSize] | 99a07b4 | 1000 |
| STIMULATOR | AMD Radeon(TM) Graphics (Ryzen 7000 iGPU, integrated) | 1002 / 164E | 32.0.11037.4004 | 32 (default only) | 99a07b4 | 300 |
| JONAS-CPH | NVIDIA GeForce RTX 5080 | 10DE / 2C02 | 32.0.16.1714 | 32 | 3ebe93b | 1000 |
| JONAS-CPH | NVIDIA GeForce RTX 2060 | 10DE / 1F08 | 32.0.16.1714 | 32 | 3ebe93b | 1000 |
| IMS-KCOMBS-INT (i9-13900K) | NVIDIA GeForce RTX 3080 Ti | 10DE / 2208 | 32.0.16.1692 | 32 | 495478d | 1000 |
| IMS-KCOMBS-INT (i9-13900K) | Intel(R) UHD Graphics 770 (integrated) | 8086 / 4680 | 32.0.101.6129 | 16 (no [WaveSize]) | 495478d | 300 |

The three packages differ only in CPU-side changes: 3ebe93b ("ShaderCompiler: harden the DXC
compile / reflection path") and 495478d (the `--sorts` option of the scale run, results CSV schema
5). At 20 sorts per batch the shaders, algorithm list, data, timing method and workloads are
identical, so the machines are comparable (aggregate_summary.txt warns about the different commits
anyway).

IMS-KCOMBS-INT is not the Intel machine of the earlier external runs (IMS-MDETURCK, i7-12700, UHD
770 with driver 31.0.101.3616, pass4 shaders only; see _test/external/notes.md). Both have a UHD
770, but with different drivers; the numbers here are the first of the final set on Intel. The RTX
3080 Ti's timestamps tick in 1.024 µs steps (every sample of its run is a multiple of 1.024 µs,
although the reported frequency is 1 GHz), so its medians are quantized to about ±1 µs and many
algorithms tie.

Wall time of the full run on IMS-KCOMBS-INT: RTX 3080 Ti 546 s, UHD 770 2907 s (the whole plan
incl. the smoke run: 62 min).

## Method

- Plan per machine (summary.txt): smoke run (16 serial iterations per combo, `--dred`, every sweep
  size) then the full run, default wave size on every GPU; on STIMULATOR also smoke + full at
  `--wave-size 64` on the discrete GPU only (the iGPU runs its default wave size only).
- Iteration policy: 1000 iterations on discrete GPUs, 300 on integrated ones
  (`--iterations 1000 --iterations-integrated 300`), 5 warmup iterations, 20 sorts per iteration
  (one batch), 10 workloads (9 distributions + the 16-size sweep).
- Default flush `full_d50` (256 MB flush + the 50 µs drain, see _test/external/notes.md); the
  `s1_rank512_bitreg2048_radix@<mode>` entries measure the other flush modes (code, data, full,
  full_legacy, none) for comparison.
- Stable power off (normal clocks).
- **0 verification failures**, no GPU errors, every wave probe OK, every run exit code 0 (smoke and
  full, all three machines).

## Key medians (µs per batch of 20 sorts, full_d50)

| GPU config | algorithm | mostly_empty | realistic_mix | mostly_mid | worst_case |
|---|---|---:|---:|---:|---:|
| RTX 5080 | pass0_bitonic | 4.06 | 12.51 | 12.38 | 44.56 |
| RTX 5080 | s1_rank512_bitreg2048_radix | **2.69** | **8.88** | **8.70** | **18.27** |
| RTX 5080 | m4_b128_p | 4.90 | 10.38 | 9.60 | 18.30 |
| RX 7900 XTX wave32 | pass0_bitonic | 3.08 | 11.54 | 11.56 | 41.48 |
| RX 7900 XTX wave32 | s1_rank512_bitreg2048_radix | **1.76** | 9.84 | 9.88 | 20.08 |
| RX 7900 XTX wave32 | m4_b128_p | 1.80 | **9.44** | **9.32** | **20.06** |
| RX 7900 XTX wave64 | pass0_bitonic | 3.08 | 11.72 | 11.60 | 41.72 |
| RX 7900 XTX wave64 | s1_rank512_bitreg2048_radix | **1.96** | 16.84 | 16.96 | **22.32** |
| RX 7900 XTX wave64 | m4_b128_p | 2.04 | **16.44** | **16.56** | **22.32** |
| RTX 2060 | pass0_bitonic | 8.29 | 23.70 | 23.97 | 91.30 |
| RTX 2060 | s1_rank512_bitreg2048_radix | **6.14** | 20.42 | **17.89** | **38.34** |
| RTX 2060 | m4_b128_p | 6.75 | **19.39** | 20.99 | 38.37 |
| Radeon iGPU (integrated) | pass0_bitonic | 10.76 | 72.22 | 141.56 | 597.16 |
| Radeon iGPU (integrated) | s1_rank512_bitreg2048_radix | 4.40 | 40.06 | 109.34 | 260.60 |
| Radeon iGPU (integrated) | m4_b128_p | **3.88** | **23.40** | **63.96** | **257.76** |
| RTX 3080 Ti | pass0_bitonic | 5.12 | 18.43 | 18.43 | 62.46 |
| RTX 3080 Ti | s1_rank512_bitreg2048_radix | **3.07** | 14.34 | 14.34 | **24.58** |
| RTX 3080 Ti | m4_b128_p | **3.07** | **12.29** | **11.26** | **24.58** |
| UHD 770 (integrated) | pass0_bitonic | 14.01 | 74.71 | 138.98 | 595.47 |
| UHD 770 (integrated) | s1_rank512_bitreg2048_radix | **7.08** | 80.55 | 244.58 | **511.07** |
| UHD 770 (integrated) | m4_b128_p | 12.40 | **58.38** | **123.54** | 518.78 |

Bold: the faster of the two candidates (both bold: equal medians). Fastest entry of the whole set
per config (median, not counting the `@` flush variants, for reference): 5080 realistic_mix s7_256
8.05, worst_case s1_rank512_bitreg4096_radix 17.41; XTX wave32 realistic_mix s3_radix2_k8 9.24,
worst_case s1_radix 20.00; 2060 realistic_mix s7_512 15.71, worst_case m3_x512 36.77; iGPU
realistic_mix m4_b128e8 22.98, worst_case t2_rank512_radix_waveops 251.30; 3080 Ti realistic_mix
m3_x512 11.26, worst_case 24.58 (m3_ref, s1 and m4_b128_p among others: tied at the 1.024 µs
tick); UHD 770 mostly_empty t2_rank512_bitreg16 4.69, realistic_mix m2_x513 46.95, mostly_mid
x7_all_256 81.22, worst_case s7_512 310.23. The better of the two candidates is within ~10 % of the
fastest entry everywhere except on the RTX 2060 (s7_512 is 19 % / 13 % faster on realistic_mix /
mostly_mid) and on the UHD 770 (the fastest entry is 1.24x faster on realistic_mix, 1.52x on
mostly_mid, 1.65x on worst_case and 1.51x on mostly_empty).

**UHD 770: the large and mid tiers want the pass7 radix X in 256 / 512-thread groups.** On
worst_case `s7_512` (one 512-thread dispatch, X above 2048) takes 310.23 µs against 511.07 for s1
and 518.78 for m4_b128_p (whose large tier is the pass2 radix in 1024-thread groups); `m3_x512`
takes 346.95. On mostly_mid `x7_all_256` (X for every size, 256 threads) is the fastest, 81.22 vs
123.54 µs for m4_b128_p. This is what _test/external/notes.md (conclusion 3) predicted from the
pass4 numbers of the other UHD 770 machine: a 1024-thread group at SIMD16 occupies 64 of a
subslice's hardware threads, so smaller groups with more keys per thread run more sorts at once.
On the mixed workload smaller groups everywhere win as well (realistic_mix: m2_x513 = rank sort
≤ 512 @512 + X 513+ @256, 46.95; m4_b128_p 58.38; s1 80.55).

## Recommendation

**Updated by the scale run (_test/scale/notes.md, 20 to 512 sorts per batch): `m4_b128_p` is now
the single recommended default; `s1_rank512_bitreg2048_radix` only for high-end NVIDIA GPUs with
small batches (about 20 sorts).** The recommendation of the final run (20 sorts per batch):

- **Discrete GPUs: `s1_rank512_bitreg2048_radix`.** 1.2-2.5x faster than pass0_bitonic on every
  discrete GPU and workload of the table above at the default wave size; best or within ~5 % of
  m4_b128_p on the 7900 XTX and the 2060, and ahead of it on the RTX 5080 (by 45 % on
  mostly_empty, 15 % on realistic_mix). (The RTX 3080 Ti, measured later, does not follow this:
  see below.)
- **Integrated GPUs: `m4_b128_p`.** On the Ryzen iGPU it is 1.7x faster than s1 on realistic_mix
  (23.4 vs 40.1 µs) and mostly_mid (64.0 vs 109.3), and 3.1x / 2.2x faster than pass0_bitonic.
- **AMD: wave32** (the default, WAVE_SIZE=32 + [WaveSize(32)]). Wave64 on the 7900 XTX costs
  ~70 % on realistic_mix / mostly_mid (16.8 vs 9.8 µs for s1) and ~10 % on worst_case.
- **Intel UHD 770 (wave16): m4_b128_p is correct (0 failures) and 1.4x faster than s1 on
  realistic_mix (58.38 vs 80.55 µs) and 2.0x on mostly_mid, but not the best configuration there**
  (see above, and the scale run: 1.25-1.46x behind the fastest on realistic_mix at 128-512 sorts).
  No configuration of the set is best on every workload on Intel; an Intel-tuned tier set (X in
  256 / 512-thread groups for the large tier) is an open item.
- RTX 3080 Ti: unlike the 5080, s1 has no lead at 20 sorts; m4_b128_p is ahead on
  realistic_mix (12.29 vs 14.34 µs) and mostly_mid (11.26 vs 14.34), equal on mostly_empty and
  worst_case (at the 1.024 µs timestamp resolution).
