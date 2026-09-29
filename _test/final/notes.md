# Final run (2026-09-29)

The final benchmark: `run_all.bat final` (shaders\algorithms_final.txt, 69 algorithm entries incl.
the flush-mode variants of s1_rank512_bitreg2048_radix) on two machines, merged with
`tools/aggregate_results.py`. The interactive results page built from this data is
tools/results_page/ (published: https://claude.ai/artifact/3PHgWLiq9ej94L2pM3wWZZ).

## Files

| file | content |
|---|---|
| all_results.csv | every benchmark results row of both machines (8625 rows, schema 4, + column `source`); smoke runs left out |
| all_samples.csv.gz | the per-iteration samples of those rows (2,967,000 rows, 285 MB unpacked, gzip -9); `python tools/results_page/prep.py` reads it directly |
| all_wave_probe.csv | the wave probe rows of every run (8 rows) |
| aggregate_summary.txt | the aggregation summary (runs, GPUs, algorithms, problems) |

The per-run result files (without the samples and logs) are in
_test/external/STIMULATOR_20260929_1529 and _test/external/JONAS-CPH_20260929_1658. The raw zips
with the full samples and logs: `G:\My Drive\STIMULATOR_20260929_1529.zip` and
`C:\Users\jonas\Desktop\GpuSort-portable-3ebe93b\results\JONAS-CPH_20260929_1658.zip`.

Regenerate: `python tools/aggregate_results.py "G:\My Drive\STIMULATOR_20260929_1529.zip"
<JONAS-CPH results folder or zip> -o <dir>` (the default output dir is this folder; it writes an
uncompressed all_samples.csv, which is git-ignored here).

## Machines

| computer | GPU | vendor / device | driver | wave | package | iterations |
|---|---|---|---|---|---|---|
| STIMULATOR | AMD Radeon RX 7900 XTX | 1002 / 744C | 32.0.11037.4004 | 32 (default) and 64, both with [WaveSize] | 99a07b4 | 1000 |
| STIMULATOR | AMD Radeon(TM) Graphics (Ryzen 7000 iGPU, integrated) | 1002 / 164E | 32.0.11037.4004 | 32 (default only) | 99a07b4 | 300 |
| JONAS-CPH | NVIDIA GeForce RTX 5080 | 10DE / 2C02 | 32.0.16.1714 | 32 | 3ebe93b | 1000 |
| JONAS-CPH | NVIDIA GeForce RTX 2060 | 10DE / 1F08 | 32.0.16.1714 | 32 | 3ebe93b | 1000 |

The two packages differ only in 3ebe93b ("ShaderCompiler: harden the DXC compile / reflection
path"), a CPU-side change; shaders, algorithm list, timing method and workloads are identical, so
the machines are comparable (aggregate_summary.txt warns about the different commits anyway).

**Intel UHD Graphics 770 (IMS-MDETURCK): final run pending.**

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
  full, both machines).

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

Bold: the faster of the two candidates. Fastest entry of the whole set per config (median, for
reference): 5080 realistic_mix s7_256 8.05, worst_case s1_rank512_bitreg4096_radix 17.41; XTX
wave32 realistic_mix s3_radix2_k8 9.24, worst_case s1_radix 20.00; 2060 realistic_mix s7_512
15.71, worst_case m3_x512 36.77; iGPU realistic_mix m4_b128e8 22.98, worst_case
t2_rank512_radix_waveops 251.30. The better of the two candidates is within ~10 % of the fastest
entry everywhere except on the RTX 2060 (s7_512 is 19 % / 13 % faster on realistic_mix /
mostly_mid).

## Recommendation

- **Discrete GPUs: `s1_rank512_bitreg2048_radix`.** 1.2-2.4x faster than pass0_bitonic on every
  discrete GPU and workload at the default wave size; best or within ~5 % of m4_b128_p on the
  7900 XTX and the 2060, and ahead of it on the RTX 5080 (by 45 % on mostly_empty, 15 % on
  realistic_mix).
- **Integrated GPUs: `m4_b128_p`.** On the Ryzen iGPU it is 1.7x faster than s1 on realistic_mix
  (23.4 vs 40.1 µs) and mostly_mid (64.0 vs 109.3), and 3.1x / 2.2x faster than pass0_bitonic.
- **AMD: wave32** (the default, WAVE_SIZE=32 + [WaveSize(32)]). Wave64 on the 7900 XTX costs
  ~70 % on realistic_mix / mostly_mid (16.8 vs 9.8 µs for s1) and ~10 % on worst_case.
- Pending: confirm the integrated recommendation on the Intel UHD 770 (wave16) with the final run.
