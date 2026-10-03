# Scale run (2026-09-30, RTX 2060 2026-10-03): 20 to 512 sorts per batch

Every earlier result used 20 sorts per batch, the draw-call use case of the task brief. With 20
sorts the GPU is mostly idle, which hides that the single-dispatch configurations launch a
1024-thread group for every sort, however small. The scale run (`run_all.bat scale`, package
495478d) measures the 19 algorithms of [shaders/algorithms_scale.txt](../../shaders/algorithms_scale.txt)
at 20, 128, 256 and 512 sorts per batch, to see which configuration holds up when the groups
compete for the GPU. The rows join with the final run's data (same algorithm names and
dispatches; `algorithm_id` + `sorts_per_iteration`).

## Files

| file | content |
|---|---|
| all_results.csv | every benchmark results row of the four benchmark runs on three machines (11400 rows = 6 GPUs x 19 algorithms x 4 sort counts x 25 workload rows: 9 distributions + 16 sweep sizes; schema 5, + column `source`); smoke runs left out |
| all_samples.csv.gz | the per-iteration samples of those rows (1,442,100 rows, 141 MB unpacked, 11.2 MB gzip -9) |
| all_wave_probe.csv | the wave probe rows of the four benchmark runs (8 rows) |
| aggregate_summary.txt | the aggregation summary (runs, GPUs, algorithms, problems) |

The per-run result files (without the samples, dxdiag and logs) are in
_test/external/JONAS-CPH_20260930_1458, _test/external/STIMULATOR_20260930_1545,
_test/external/IMS-KCOMBS-INT_20260930_1154 and _test/external/JONAS-CPH_20261003_1635 (RTX
2060). The raw zips with the full samples and logs:
`C:\Users\jonas\Desktop\GpuSort-portable-495478d\results\JONAS-CPH_20260930_1458.zip`,
`G:\My Drive\STIMULATOR_20260930_1545.zip`,
`C:\Users\jonas\Downloads\IMS-KCOMBS-INT_20260930_1154.zip` and
`D:\git\gpu_single_pass_sort\dist\GpuSort-portable-495478d\results\JONAS-CPH_20261003_1635.zip`.

Regenerate: `python tools/aggregate_results.py <JONAS-CPH 20260930 zip> "G:\My Drive\STIMULATOR_20260930_1545.zip"
<IMS-KCOMBS-INT zip> <JONAS-CPH 20261003 zip> -o _test/scale` (in this order; it writes an uncompressed all_samples.csv,
which is git-ignored here). The
results page's scaling section is built from all_results.csv:
`python tools/results_page/prep_scale.py tools/results_page/scale.js _test/scale/all_results.csv`.

## Machines

| computer | GPU | vendor / device | driver | wave | iterations at 20 / 128 / 256 / 512 sorts |
|---|---|---|---|---|---|
| JONAS-CPH | NVIDIA GeForce RTX 5080 | 10DE / 2C02 | 32.0.16.1714 | 32 | 1000 / 300 / 200 / 150 |
| STIMULATOR | AMD Radeon RX 7900 XTX | 1002 / 744C | 32.0.11037.4004 | 32 + [WaveSize(32)] | 1000 / 300 / 200 / 150 |
| STIMULATOR | AMD Radeon(TM) Graphics (Ryzen 7000 iGPU, integrated) | 1002 / 164E | 32.0.11037.4004 | 32 + [WaveSize(32)] | 300 / 90 / 60 / 45 |
| IMS-KCOMBS-INT (i9-13900K) | NVIDIA GeForce RTX 3080 Ti | 10DE / 2208 | 32.0.16.1692 | 32 | 1000 / 300 / 200 / 150 |
| IMS-KCOMBS-INT (i9-13900K) | Intel(R) UHD Graphics 770 (integrated) | 8086 / 4680 | 32.0.101.6129 | 16 (no [WaveSize]) | 300 / 90 / 60 / 45 |
| JONAS-CPH (2026-10-03, RTX 2060 in place of the RTX 5080) | NVIDIA GeForce RTX 2060 | 10DE / 1F08 | 32.0.16.1714 | 32 | 1000 / 300 / 200 / 150 |

All four runs used the same package (495478d). Wall time of the whole plan (smoke + full): 7
min on JONAS-CPH, 25 min on STIMULATOR, 34 min on IMS-KCOMBS-INT, 14 min on JONAS-CPH with the
RTX 2060 (estimates: 8.2 / 28.1 / 24.5 / 15.8 min; full run: RTX 3080 Ti 334 s, UHD 770 1500 s,
RTX 2060 774 s).

IMS-KCOMBS-INT is a different machine from IMS-MDETURCK (i7-12700, UHD 770 with driver
31.0.101.3616), whose pass4 results are in _test/external. The RTX 3080 Ti's timestamps tick in
1.024 µs steps (every sample is a multiple of 1.024 µs), so its medians are quantized and many
algorithms tie; where they tie, the tables below name the first of them.

The RTX 2060 ran on 2026-10-03 in JONAS-CPH with the RTX 5080 removed (the only GPU in the
machine, in the 5080's slot; in the final run it was a second GPU next to the 5080). Its
timestamps have 32 ns steps, but 26.6 % of its samples in this run are multiples of 1.024 µs
(final run: 29.1 %), so its medians often land on 1.024 µs multiples and some algorithms tie;
where they tie, the tables name the first of them.

## Method

- Plan per machine (summary.txt): smoke run (a few serial iterations per combo, every sort count)
  then the full run: `--algo-file algorithms_scale.txt --sorts 20,128,256,512`, default wave size
  on every GPU.
- Iteration policy (fewer iterations for larger batches, so a run stays short): discrete GPUs
  `--iterations 1000,300,200,150`, integrated GPUs `--iterations-integrated 300,90,60,45`, for
  20 / 128 / 256 / 512 sorts; 5 warmup iterations. Every GPU x workload x algorithm runs once per
  sort count.
- Each sort of a batch is drawn independently from the workload's size distribution (the same
  distributions as the final run, see the README's workload table); the sweep workload gives all
  sorts of a batch the same size.
- Flush `full_d50` (256 MB flush + 50 µs spin drain), stable power off (normal clocks).
- Algorithms (19): the references with a 1024-thread group per sort, `pass0_bitonic`, `s1_radix`,
  `s1_rank512_bitreg2048_radix`, `s1_rank512_bitreg4096_radix`, `s3_rank512_bitreg2048_radix2`,
  `s4_3tier_rx3r`; the tier-sized multi-dispatch families `t2_rank512_bitonic`,
  `t3_rank128_rank512_bitonic`, `m3_ref`, `m3_x512`, `m4_b128_p`, `m4_b128`, `m4_b128e8`,
  `m4_b64`, `m4_b256`; and the single dispatches with smaller groups `s7_256`, `s7_512`,
  `s7_256b`, `x7_all_256`. Their dispatches are in algorithms_scale.txt (copied unchanged from
  algorithms_final.txt).
- **0 verification failures**, no GPU errors, every wave probe OK, every run exit code 0 (smoke
  and full, all four runs).
- The 20-sort values agree with the final run within ~4 % (realistic_mix, final / scale:
  RTX 5080 s1 8.88 / 8.80, m4_b128_p 10.38 / 10.06; RX 7900 XTX s1 9.84 / 9.92, m4_b128_p
  9.44 / 9.44; Radeon iGPU s1 40.06 / 40.18, m4_b128_p 23.40 / 23.52; UHD 770 s1 80.55 / 77.55,
  m4_b128_p 58.38 / 57.89 µs), or within one timestamp tick on the RTX 3080 Ti (s1 14.34 /
  14.34, m4_b128_p 12.29 / 11.26 µs). **Except the RTX 2060**: its scale run is faster than its
  final run (median 6 % over the 171 common rows, up to 27 %; realistic_mix s1 20.42 / 16.38,
  m4_b128_p 19.39 / 18.13, s7_512 15.71 / 14.34 µs), most likely because of the different
  hardware setup (only GPU, other slot; not investigated), see _test/external/notes.md. Its
  final-run and scale-run numbers are not directly comparable.

## Results

Median µs per batch. "best" = the fastest of the 19 algorithms; s1 = `s1_rank512_bitreg2048_radix`
(the final run's discrete recommendation); the ratios are the median divided by the best median.
Workloads: realistic_mix = 15 % empty, 72 % 16-512, 10 % 513-2048, 3 % 2049-8192; mostly_medium =
uniform 129-512; mostly_mid = uniform 513-2048; mostly_empty = 70 % empty, rest 1-64; worst_case =
all 8192. Every workload and algorithm is on the results page (tools/results_page, section
"Scaling with the number of sorts").

### RTX 5080


realistic_mix:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256` | 8.03 | 8.80 | 1.10 | 10.06 | 1.25 |
| 128 | `m3_ref` | 14.69 | 15.22 | 1.04 | 14.75 | 1.00 |
| 256 | `m4_b128_p` | 16.46 | 17.79 | 1.08 | 16.46 | 1.00 |
| 512 | `m3_ref` | 19.78 | 25.87 | 1.31 | 20.03 | 1.01 |

mostly_medium:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `t2_rank512_bitonic` | 4.45 | 4.67 | 1.05 | 6.64 | 1.49 |
| 128 | `t2_rank512_bitonic` | 6.30 | 6.91 | 1.10 | 12.85 | 2.04 |
| 256 | `t2_rank512_bitonic` | 8.08 | 10.37 | 1.28 | 12.16 | 1.50 |
| 512 | `t2_rank512_bitonic` | 12.13 | 17.38 | 1.43 | 17.46 | 1.44 |

mostly_mid:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256` | 8.10 | 8.74 | 1.08 | 9.57 | 1.18 |
| 128 | `m4_b128e8` | 14.00 | 16.10 | 1.15 | 15.82 | 1.13 |
| 256 | `s7_256` | 21.98 | 24.67 | 1.12 | 26.94 | 1.23 |
| 512 | `m3_x512` | 29.89 | 44.98 | 1.50 | 30.37 | 1.02 |

mostly_empty:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256b` | 2.50 | 2.59 | 1.04 | 4.53 | 1.81 |
| 128 | `s7_256b` | 2.98 | 3.20 | 1.08 | 4.77 | 1.60 |
| 256 | `s7_256b` | 4.03 | 4.45 | 1.10 | 5.12 | 1.27 |
| 512 | `s7_256b` | 6.14 | 6.86 | 1.12 | 7.82 | 1.27 |

worst_case:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s1_rank512_bitreg4096_radix` | 17.47 | 18.30 | 1.05 | 18.34 | 1.05 |
| 128 | `s1_rank512_bitreg4096_radix` | 37.47 | 39.33 | 1.05 | 39.62 | 1.06 |
| 256 | `s1_rank512_bitreg4096_radix` | 67.01 | 71.04 | 1.06 | 71.46 | 1.07 |
| 512 | `s1_rank512_bitreg4096_radix` | 114.38 | 120.93 | 1.06 | 121.15 | 1.06 |

### RTX 3080 Ti


realistic_mix:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m3_x512` | 11.26 | 14.34 | 1.27 | 11.26 | 1.00 |
| 128 | `s1_rank512_bitreg2048_radix` | 21.50 | 21.50 | 1.00 | 21.50 | 1.00 |
| 256 | `m3_ref` | 23.55 | 26.62 | 1.13 | 23.55 | 1.00 |
| 512 | `m4_b128_p` | 27.65 | 39.94 | 1.44 | 27.65 | 1.00 |

mostly_medium:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256b` | 6.14 | 7.17 | 1.17 | 7.17 | 1.17 |
| 128 | `s7_256b` | 7.17 | 11.26 | 1.57 | 9.73 | 1.36 |
| 256 | `s7_256b` | 11.26 | 16.38 | 1.45 | 18.43 | 1.64 |
| 512 | `m4_b128e8` | 14.34 | 27.65 | 1.93 | 19.46 | 1.36 |

mostly_mid:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m3_ref` | 11.26 | 14.34 | 1.27 | 11.26 | 1.00 |
| 128 | `x7_all_256` | 16.38 | 26.62 | 1.62 | 37.89 | 2.31 |
| 256 | `x7_all_256` | 25.60 | 45.06 | 1.76 | 34.82 | 1.36 |
| 512 | `x7_all_256` | 40.96 | 80.90 | 1.98 | 46.08 | 1.12 |

mostly_empty:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s1_rank512_bitreg2048_radix` | 3.07 | 3.07 | 1.00 | 3.07 | 1.00 |
| 128 | `s1_rank512_bitreg4096_radix` | 3.07 | 4.10 | 1.33 | 5.12 | 1.67 |
| 256 | `s7_512` | 3.07 | 5.12 | 1.67 | 6.14 | 2.00 |
| 512 | `s7_256b` | 4.10 | 8.19 | 2.00 | 9.22 | 2.25 |

worst_case:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s1_radix` | 24.58 | 24.58 | 1.00 | 24.58 | 1.00 |
| 128 | `m3_x512` | 56.32 | 57.34 | 1.02 | 58.37 | 1.04 |
| 256 | `m3_x512` | 101.38 | 108.03 | 1.07 | 108.54 | 1.07 |
| 512 | `s7_256b` | 165.89 | 187.39 | 1.13 | 189.44 | 1.14 |

### RTX 2060


realistic_mix:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256` | 14.34 | 16.38 | 1.14 | 18.13 | 1.26 |
| 128 | `m3_ref` | 26.62 | 31.74 | 1.19 | 26.62 | 1.00 |
| 256 | `m4_b128_p` | 32.93 | 49.15 | 1.49 | 32.93 | 1.00 |
| 512 | `m4_b64` | 47.17 | 85.06 | 1.80 | 52.38 | 1.11 |

mostly_medium:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256b` | 8.13 | 9.02 | 1.11 | 9.41 | 1.16 |
| 128 | `s7_256b` | 16.91 | 22.53 | 1.33 | 18.94 | 1.12 |
| 256 | `m4_b128e8` | 22.53 | 38.82 | 1.72 | 26.88 | 1.19 |
| 512 | `m4_b128e8` | 34.43 | 70.72 | 2.05 | 44.06 | 1.28 |

mostly_mid:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256` | 14.34 | 16.38 | 1.14 | 20.26 | 1.41 |
| 128 | `x7_all_256` | 32.67 | 59.97 | 1.84 | 39.46 | 1.21 |
| 256 | `x7_all_256` | 53.76 | 108.45 | 2.02 | 63.70 | 1.18 |
| 512 | `x7_all_256` | 96.80 | 206.69 | 2.14 | 112.64 | 1.16 |

mostly_empty:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_256b` | 4.58 | 4.70 | 1.03 | 5.86 | 1.28 |
| 128 | `s7_256b` | 5.82 | 8.10 | 1.39 | 10.24 | 1.76 |
| 256 | `s7_256b` | 7.04 | 11.81 | 1.68 | 14.37 | 2.04 |
| 512 | `s7_256b` | 10.18 | 19.12 | 1.88 | 22.53 | 2.21 |

worst_case:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m3_x512` | 35.36 | 36.86 | 1.04 | 37.09 | 1.05 |
| 128 | `m3_x512` | 128.98 | 165.33 | 1.28 | 162.93 | 1.26 |
| 256 | `m3_x512` | 224.72 | 290.94 | 1.29 | 291.17 | 1.30 |
| 512 | `m3_x512` | 417.73 | 561.15 | 1.34 | 547.30 | 1.31 |

Ties at 1.024 µs multiples: realistic_mix at 20 sorts `s7_256`, `s7_512` and `s7_256b` (14.34),
at 128 sorts `m3_ref` and `m4_b128_p` (26.62); mostly_mid at 20 sorts the same three `s7_*`
(14.34).

### RX 7900 XTX


realistic_mix:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m4_b128_p` | 9.44 | 9.92 | 1.05 | 9.44 | 1.00 |
| 128 | `s1_rank512_bitreg2048_radix` | 15.24 | 15.24 | 1.00 | 15.56 | 1.02 |
| 256 | `m4_b128_p` | 17.00 | 18.90 | 1.11 | 17.00 | 1.00 |
| 512 | `m4_b128_p` | 20.14 | 28.88 | 1.43 | 20.14 | 1.00 |

mostly_medium:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m4_b128_p` | 5.00 | 7.24 | 1.45 | 5.00 | 1.00 |
| 128 | `s7_256b` | 5.96 | 9.80 | 1.64 | 7.80 | 1.31 |
| 256 | `m4_b128e8` | 9.32 | 15.02 | 1.61 | 12.36 | 1.33 |
| 512 | `m4_b128e8` | 11.84 | 25.36 | 2.14 | 14.92 | 1.26 |

mostly_mid:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s1_radix` | 9.30 | 9.88 | 1.06 | 9.40 | 1.01 |
| 128 | `x7_all_256` | 14.60 | 19.00 | 1.30 | 23.72 | 1.62 |
| 256 | `x7_all_256` | 21.70 | 28.84 | 1.33 | 26.12 | 1.20 |
| 512 | `x7_all_256` | 33.56 | 50.88 | 1.52 | 38.38 | 1.14 |

mostly_empty:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s1_rank512_bitreg2048_radix` | 1.72 | 1.72 | 1.00 | 1.84 | 1.07 |
| 128 | `t2_rank512_bitonic` | 1.84 | 2.08 | 1.13 | 2.24 | 1.22 |
| 256 | `t2_rank512_bitonic` | 1.88 | 2.64 | 1.40 | 2.96 | 1.57 |
| 512 | `t2_rank512_bitonic` | 2.36 | 3.92 | 1.66 | 4.44 | 1.88 |

worst_case:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s1_rank512_bitreg2048_radix` | 20.04 | 20.04 | 1.00 | 20.16 | 1.01 |
| 128 | `s1_radix` | 50.20 | 51.82 | 1.03 | 51.42 | 1.02 |
| 256 | `s1_rank512_bitreg2048_radix` | 81.80 | 81.80 | 1.00 | 81.84 | 1.00 |
| 512 | `s1_rank512_bitreg4096_radix` | 243.22 | 243.44 | 1.00 | 250.00 | 1.03 |

### Radeon iGPU


realistic_mix:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m4_b128e8` | 23.06 | 40.18 | 1.74 | 23.52 | 1.02 |
| 128 | `m4_b128_p` | 127.12 | 245.96 | 1.93 | 127.12 | 1.00 |
| 256 | `m4_b128_p` | 259.54 | 490.66 | 1.89 | 259.54 | 1.00 |
| 512 | `m4_b128_p` | 508.72 | 969.52 | 1.91 | 508.72 | 1.00 |

mostly_medium:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m4_b128e8` | 15.92 | 38.06 | 2.39 | 18.66 | 1.17 |
| 128 | `m4_b128e8` | 84.54 | 232.64 | 2.75 | 99.76 | 1.18 |
| 256 | `m4_b128e8` | 164.60 | 464.12 | 2.82 | 195.36 | 1.19 |
| 512 | `m4_b128e8` | 324.12 | 924.28 | 2.85 | 386.32 | 1.19 |

mostly_mid:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m4_b128e8` | 62.76 | 109.46 | 1.74 | 64.10 | 1.02 |
| 128 | `m4_b256` | 369.00 | 683.62 | 1.85 | 373.84 | 1.01 |
| 256 | `m4_b256` | 729.52 | 1361.6 | 1.87 | 740.26 | 1.01 |
| 512 | `m4_b128` | 1455.8 | 2719.6 | 1.87 | 1474.7 | 1.01 |

mostly_empty:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m4_b64` | 2.96 | 4.40 | 1.49 | 3.88 | 1.31 |
| 128 | `m4_b64` | 15.16 | 24.42 | 1.61 | 20.32 | 1.34 |
| 256 | `m4_b64` | 28.58 | 48.14 | 1.68 | 39.92 | 1.40 |
| 512 | `m4_b64` | 55.32 | 94.84 | 1.71 | 79.24 | 1.43 |

worst_case:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s1_radix` | 257.52 | 260.62 | 1.01 | 257.60 | 1.00 |
| 128 | `s1_radix` | 1639.5 | 1659.5 | 1.01 | 1646.8 | 1.00 |
| 256 | `s1_radix` | 3277.6 | 3317.0 | 1.01 | 3293.6 | 1.00 |
| 512 | `s1_radix` | 6555.4 | 6632.0 | 1.01 | 6589.4 | 1.01 |

### UHD 770


realistic_mix:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `x7_all_256` | 52.24 | 77.55 | 1.48 | 57.89 | 1.11 |
| 128 | `m4_b256` | 185.75 | 408.67 | 2.20 | 270.37 | 1.46 |
| 256 | `m4_b256` | 381.48 | 826.07 | 2.17 | 485.23 | 1.27 |
| 512 | `m4_b128e8` | 729.63 | 1592.5 | 2.18 | 914.38 | 1.25 |

mostly_medium:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `m4_b64` | 31.56 | 39.09 | 1.24 | 34.01 | 1.08 |
| 128 | `m4_b256` | 138.83 | 204.12 | 1.47 | 159.92 | 1.15 |
| 256 | `m4_b256` | 268.67 | 399.04 | 1.49 | 306.59 | 1.14 |
| 512 | `m4_b256` | 526.72 | 794.79 | 1.51 | 601.51 | 1.14 |

mostly_mid:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `x7_all_256` | 81.09 | 247.13 | 3.05 | 124.04 | 1.53 |
| 128 | `x7_all_256` | 449.48 | 1533.8 | 3.41 | 611.98 | 1.36 |
| 256 | `x7_all_256` | 894.74 | 3062.4 | 3.42 | 1205.8 | 1.35 |
| 512 | `x7_all_256` | 1805.6 | 6136.4 | 3.40 | 2419.7 | 1.34 |

mostly_empty:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_512` | 4.53 | 6.20 | 1.37 | 9.32 | 2.06 |
| 128 | `s7_256b` | 14.61 | 23.33 | 1.60 | 26.28 | 1.80 |
| 256 | `s7_256b` | 27.03 | 43.75 | 1.62 | 46.35 | 1.71 |
| 512 | `s7_256b` | 52.29 | 83.49 | 1.60 | 86.51 | 1.65 |

worst_case:

| sorts | best | best median | s1 median | s1 / best | m4_b128_p median | m4_b128_p / best |
|---:|---|---:|---:|---:|---:|---:|
| 20 | `s7_512` | 321.30 | 507.87 | 1.58 | 573.67 | 1.79 |
| 128 | `s7_512` | 1926.8 | 3167.8 | 1.64 | 3191.0 | 1.66 |
| 256 | `s7_512` | 4022.8 | 6304.1 | 1.57 | 6365.2 | 1.58 |
| 512 | `s7_512` | 8362.0 | 12568.0 | 1.50 | 12685.9 | 1.52 |

### worst_case: the large tier's group size

At 512 sorts of 8192 elements, grouped by the path that sorts 2049+ elements (median / best median
on that GPU):

| large-sort path | algorithms | RTX 5080 | RTX 3080 Ti | RTX 2060 | RX 7900 XTX | Radeon iGPU | UHD 770 |
|---|---|---:|---:|---:|---:|---:|---:|
| pass2 LDS radix P @1024 | s1_radix, s1_rank512_bitreg2048_radix, s1_rank512_bitreg4096_radix, m3_ref, m4_b128_p | 1.00-1.08 | 1.13-1.14 | 1.31-1.35 | 1.00-1.03 | 1.00-1.01 | 1.50-1.52 |
| radix2 / radix3 @1024 | s3_rank512_bitreg2048_radix2, s4_3tier_rx3r | 1.09-1.24 | 1.25-1.33 | 1.35-1.37 | 1.23-1.28 | 1.14-1.17 | 2.38-2.57 |
| ballot radix X @512 | m3_x512, s7_512 | 1.16 | 1.03-1.04 | 1.00-1.02 | 1.17 | 1.24-1.25 | 1.00-1.05 |
| ballot radix X @256 | m4_b128, m4_b128e8, m4_b64, m4_b256, s7_256, s7_256b, x7_all_256 | 1.70 | 1.00-1.10 | 1.15-1.17 | 1.46-1.49 | 1.53-1.54 | 1.23-1.35 |
| LDS bitonic @1024 | pass0_bitonic, t2_rank512_bitonic, t3_rank128_rank512_bitonic | 2.71-2.74 | 2.60-2.61 | 3.63-3.67 | 1.66-1.71 | 2.33-2.34 | 1.80-1.81 |

On the RTX 5080, the RX 7900 XTX and the Radeon iGPU the 256-thread large tiers are 1.5-1.7x
slower at 512 sorts (1.5-2.0x at 20 sorts): more, smaller groups do not make up for the slower
per-sort algorithm even when the GPU is full. The other three GPUs differ. On the **RTX 3080 Ti** the
pass2 radix @1024 is tied for the best at 20 sorts (24.58 µs), but from 128 sorts the X radix in
512- and 256-thread groups is faster: by one timestamp tick at 128 sorts (m3_x512 56.32 vs
s1 57.34 µs), clearly at 512 sorts (s7_256b 165.89, m3_x512 171.01, m4_b128_p 189.44 µs). On the **UHD 770** the X radix in 512-thread groups is the best at every
batch size (s7_512: 321.30 µs at 20 sorts, 8362.0 at 512), and the pass2 radix @1024 is 1.5-1.8x
slower (m4_b128_p 573.67 / 12685.9 µs). On the **RTX 2060** `m3_x512` (X radix @512 for
2049+) is the best at every batch size (35.36 µs at 20 sorts, 417.73 at 512; `s7_512` 425.98),
and the pass2 radix @1024 is 1.04-1.05x slower at 20 sorts and 1.26-1.35x from 128 sorts
(m4_b128_p 1.05 / 1.26 / 1.30 / 1.31x); the X radix @256 is in between (1.15-1.17x at 512).

**Cross-GPU pattern for the large tier.** At high batch counts (128-512 sorts) the better of the
two X radix @512 configurations (`m3_x512`, `s7_512`) is the fastest or within 4 % of the fastest
on the RTX 2060, the RTX 3080 Ti and the UHD 770 (on the 3080 Ti at 512 sorts the X radix @256 is 3 % ahead: s7_256b 165.89 vs
m3_x512 171.01 µs), while the pass2 radix in 1024-thread groups wins on the RTX 5080, the RX
7900 XTX and the Radeon iGPU (X @512 1.14-1.16x, 1.17-1.48x and 1.24-1.25x slower there). No
configuration of the set combines m4_b128_p's small / mid tiers with the X radix @512 for 2049+
(`m3_x512` has the 3-tier small / mid split, `m4_b128` and its variants use X @256); that
combination is the obvious next candidate (open item).

The RX 7900
XTX's worst_case grows faster than the work from 256 to 512 sorts (best 81.8 -> 243.2 µs, 3.0x for
2x the elements; RTX 5080 67.0 -> 114.4 µs). 512 x 8192 elements are 16 MB of input plus 16 MB of
output; a cache capacity effect is a plausible cause but was not investigated.

## Conclusions

- **realistic_mix: `m4_b128_p` is the best or within 2 % at every batch size on the RTX 5080, the
  RTX 3080 Ti, the RX 7900 XTX and the Radeon iGPU**, except 20 sorts on the RTX 5080 (s7_256
  8.03, s1 8.80, m4_b128_p 10.06 µs: the cost of 4 dispatches on an idle high-end GPU). On the RTX
  3080 Ti it is the best (or tied for the best at the 1.024 µs tick) at all four batch sizes. On
  the RTX 2060 it equals the best at 128 and 256 sorts (26.62 / 32.93 µs) and is 1.26x behind at
  20 sorts (s7_256 14.34 µs, tied with s7_512 and s7_256b; s1 16.38, m4_b128_p 18.13 µs) and 1.11x
  at 512 sorts (m4_b64 47.17 µs; `m4_b128`, which differs from m4_b128_p only in the X radix @256
  for 2049+, 47.34 vs 52.38 µs). The single-dispatch `s1_rank512_bitreg2048_radix` falls behind as
  the batch grows: at 512 sorts it is 1.31x (RTX 5080), 1.44x (RTX 3080 Ti), 1.80x (RTX 2060),
  1.43x (RX 7900 XTX), 1.91x (Radeon iGPU) and 2.18x (UHD 770) slower than the best (RTX 2060 at
  20 / 128 / 256 / 512 sorts: 1.14 / 1.19 / 1.49 / 1.80x). Mixed batches need tier-sized groups once the GPU is busy: a
  1024-thread group for a 50-element sort takes occupancy the other sorts need.
- **Intel UHD 770: no configuration of the set is the best everywhere.** On realistic_mix
  m4_b128_p is 1.11x (20 sorts), 1.46x (128), 1.27x (256) and 1.25x (512) slower than the best;
  the best are `x7_all_256` (20 sorts, 52.24 µs), `m4_b256` (128 / 256 sorts, 185.75 / 381.48 µs)
  and `m4_b128e8` (512 sorts, 729.63 µs). All three sort 2049+ with the X radix @256, where
  m4_b128_p uses the pass2 radix @1024. The UHD 770 prefers the pass7 radix X in
  256 / 512-thread groups for mid and large sorts: `s7_512` is the best on worst_case at every
  batch size (1.52-1.79x faster than m4_b128_p), `x7_all_256` the best on mostly_mid at every
  batch size (1.34-1.53x faster) and on mostly_large / sparse_keys from 128 sorts. m4_b128_p is
  still 1.3-1.7x faster than s1 on realistic_mix at every batch size there. An Intel-tuned tier set
  (tier-sized small groups plus X @256 / @512 for the large tier) is an open item.
- **worst_case: on the RTX 5080, the RX 7900 XTX and the Radeon iGPU large sorts still need
  1024-thread groups; on the RTX 2060, the RTX 3080 Ti and the UHD 770 smaller groups win.** Every configuration with the pass2 LDS radix @1024 for 2049+ (including
  m4_b128_p) is within 8 % of the best there; the 256-thread ballot radix tiers are 1.5-1.7x
  slower (table above). RX 7900 XTX at 512 sorts: 243 µs, RTX 5080: 114 µs. On the RTX 3080 Ti
  m4_b128_p is 1.04-1.14x behind the X tiers from 128 sorts (189.44 vs 165.89 µs at 512), on the
  RTX 2060 1.05 / 1.26 / 1.30 / 1.31x behind `m3_x512` (X radix @512, the best at every batch
  size; 547.30 vs 417.73 µs at 512), on the UHD 770 1.52-1.79x behind s7_512.
- **mostly_medium (129-512)**: `m4_b128e8` (bitonic E8 in 128 threads) wins on AMD at large counts
  (RX 7900 XTX at 512 sorts 11.84 vs m4_b128_p 14.92 µs, 21 % faster; Radeon iGPU 324.1 vs 386.3
  µs, 16 % faster, and the best at every count) and on the RTX 3080 Ti at 512 sorts (14.34 vs
  19.46 µs). `t2_rank512_bitonic` (rank sort in 512-thread groups) wins at every count on the RTX
  5080 (12.13 vs m4_b128_p 17.46 µs at 512 sorts). On the UHD 770 `m4_b256` (rank sort up to 256
  in 256 threads) is the best from 128 sorts (526.72 vs 601.51 µs at 512). On the RTX 2060
  `s7_256b` wins at 20 / 128 sorts and `m4_b128e8` at 256 / 512 (34.43 vs 44.06 µs at 512).
- **mostly_mid (513-2048) on the RTX 2060**: `x7_all_256` (X radix @256 for every size) is the
  best from 128 sorts (96.80 vs m4_b128_p 112.64 µs at 512, 1.16x), as on the RTX 3080 Ti, the RX
  7900 XTX and the UHD 770.
- **mostly_empty**: `s7_256b` is the best on the RTX 5080, the RTX 2060 (every batch size) and the
  UHD 770 (128-512 sorts),
  `t2_rank512_bitonic` on the RX 7900 XTX (128-512 sorts), `m4_b64` on the Radeon iGPU. m4_b128_p
  is 1.00-2.25x slower than the best here, but the absolute times are small (RX 7900 XTX at 512
  sorts: 4.44 vs 2.36 µs; RTX 3080 Ti 9.22 vs 4.10 µs; RTX 2060 up to 2.21x, 22.53 vs 10.18 µs).
- **Recommendation: `m4_b128_p` stays the single default**: the best or near the best on the
  mixed workload at every batch size on every GPU measured except the Intel UHD 770 (RTX 2060:
  1.00-1.26x the best, behind only at 20 and 512 sorts), and on the large workloads on the RTX
  5080, RX 7900 XTX and Radeon iGPU. On the UHD 770 it is correct and
  well ahead of s1 but 1.1-1.5x behind the best on realistic_mix (see above).
  `s1_rank512_bitreg2048_radix` remains the option for the RTX 5080 with small batches (about 20
  sorts): it beats m4_b128_p by 13 % on realistic_mix (8.80 vs 10.06 µs) and by 43 % on
  mostly_empty (2.59 vs 4.53 µs), but it is 1.29x slower at 512 sorts (25.87 vs 20.03 µs). On the
  RTX 3080 Ti it has no such advantage (realistic_mix at 20 sorts: 14.34 vs 11.26 µs). On the RTX
  2060 it is 10 % faster than m4_b128_p at 20 sorts in this run (16.38 vs 18.13 µs; in the final
  run it was 5 % slower, 20.42 vs 19.39), but the 256 / 512-thread single dispatches `s7_*` are
  faster still (14.34 µs), and s1 is 1.19x or more behind m4_b128_p from 128 sorts on.
- Open tuning opportunities: a tier set with m4_b128_p's small / mid tiers and the X radix in
  512-thread groups for 2049+ (the large-tier winner on the RTX 2060, RTX 3080 Ti and UHD 770 at
  high batch counts, see "Cross-GPU pattern" above); an Intel tier set with the X radix in 256 /
  512-thread groups for the large tier; for the 129-512 tier at 128-512 sorts, B8 instead of B4 in the 128-thread tier
  on AMD and the RTX 3080 Ti (m4_b128e8), rank sort in 512-thread groups on the RTX 5080
  (t2_rank512_bitonic).
