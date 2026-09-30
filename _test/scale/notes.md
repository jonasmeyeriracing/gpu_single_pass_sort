# Scale run (2026-09-30): 20 to 512 sorts per batch

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
| all_results.csv | every benchmark results row of both machines (5700 rows = 3 GPUs x 19 algorithms x 4 sort counts x 25 workload rows: 9 distributions + 16 sweep sizes; schema 5, + column `source`); smoke runs left out |
| all_samples.csv.gz | the per-iteration samples of those rows (721,050 rows, 69 MB unpacked, gzip -9) |
| all_wave_probe.csv | the wave probe rows of both benchmark runs (5 rows) |
| aggregate_summary.txt | the aggregation summary (runs, GPUs, algorithms, problems) |

The per-run result files (without the samples, dxdiag and logs) are in
_test/external/JONAS-CPH_20260930_1458 and _test/external/STIMULATOR_20260930_1545. The raw zips
with the full samples and logs:
`C:\Users\jonas\Desktop\GpuSort-portable-495478d\results\JONAS-CPH_20260930_1458.zip` and
`G:\My Drive\STIMULATOR_20260930_1545.zip`.

Regenerate: `python tools/aggregate_results.py <JONAS-CPH zip> "G:\My Drive\STIMULATOR_20260930_1545.zip"
-o _test/scale` (it writes an uncompressed all_samples.csv, which is git-ignored here). The
results page's scaling section is built from all_results.csv:
`python tools/results_page/prep_scale.py tools/results_page/scale.js _test/scale/all_results.csv`.

## Machines

| computer | GPU | vendor / device | driver | wave | iterations at 20 / 128 / 256 / 512 sorts |
|---|---|---|---|---|---|
| JONAS-CPH | NVIDIA GeForce RTX 5080 | 10DE / 2C02 | 32.0.16.1714 | 32 | 1000 / 300 / 200 / 150 |
| STIMULATOR | AMD Radeon RX 7900 XTX | 1002 / 744C | 32.0.11037.4004 | 32 + [WaveSize(32)] | 1000 / 300 / 200 / 150 |
| STIMULATOR | AMD Radeon(TM) Graphics (Ryzen 7000 iGPU, integrated) | 1002 / 164E | 32.0.11037.4004 | 32 + [WaveSize(32)] | 300 / 90 / 60 / 45 |

Both machines ran the same package (495478d). Wall time of the whole plan (smoke + full): 7 min
on JONAS-CPH, 25 min on STIMULATOR (estimates: 8.2 / 28.1 min).

**Pending: the RTX 2060 (removed from JONAS-CPH for now, to be swapped back in) and the Intel UHD
Graphics 770 (IMS-MDETURCK).**

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
  and full, both machines).
- The 20-sort values agree with the final run within ~3 % (realistic_mix, final / scale:
  RTX 5080 s1 8.88 / 8.80, m4_b128_p 10.38 / 10.06; RX 7900 XTX s1 9.84 / 9.92, m4_b128_p
  9.44 / 9.44; Radeon iGPU s1 40.06 / 40.18, m4_b128_p 23.40 / 23.52 µs).

## Results

Median µs per batch. "best" = the fastest of the 19 algorithms; s1 = `s1_rank512_bitreg2048_radix`
(the final run's discrete recommendation); the ratios are the median divided by the best median.
Workloads: realistic_mix = 15 % empty, 72 % 16-512, 10 % 513-2048, 3 % 2049-8192; mostly_medium =
uniform 129-512; mostly_empty = 70 % empty, rest 1-64; worst_case = all 8192. Every workload and
algorithm is on the results page (tools/results_page, section "Scaling with the number of sorts").

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

### worst_case: the large tier's group size

At 512 sorts of 8192 elements, grouped by the path that sorts 2049+ elements (median / best median
on that GPU):

| large-sort path | algorithms | RTX 5080 | RX 7900 XTX | Radeon iGPU |
|---|---|---:|---:|---:|
| pass2 LDS radix P @1024 | s1_radix, s1_rank512_bitreg2048_radix, s1_rank512_bitreg4096_radix, m3_ref, m4_b128_p | 1.00-1.08 | 1.00-1.03 | 1.00-1.01 |
| radix2 / radix3 @1024 | s3_rank512_bitreg2048_radix2, s4_3tier_rx3r | 1.09-1.24 | 1.23-1.28 | 1.14-1.17 |
| ballot radix X @512 | m3_x512, s7_512 | 1.16 | 1.17 | 1.24-1.25 |
| ballot radix X @256 | m4_b128, m4_b128e8, m4_b64, m4_b256, s7_256, s7_256b, x7_all_256 | 1.70 | 1.46-1.49 | 1.53-1.54 |
| LDS bitonic @1024 | pass0_bitonic, t2_rank512_bitonic, t3_rank128_rank512_bitonic | 2.71-2.74 | 1.66-1.71 | 2.33-2.34 |

The 256-thread large tiers are 1.5-1.7x slower at 512 sorts (1.5-2.0x at 20 sorts): more, smaller
groups do not make up for the slower per-sort algorithm even when the GPU is full. The RX 7900
XTX's worst_case grows faster than the work from 256 to 512 sorts (best 81.8 -> 243.2 µs, 3.0x for
2x the elements; RTX 5080 67.0 -> 114.4 µs). 512 x 8192 elements are 16 MB of input plus 16 MB of
output; a cache capacity effect is a plausible cause but was not investigated.

## Conclusions

- **realistic_mix: `m4_b128_p` is the best or within 2 % at every batch size on all three GPUs**,
  except 20 sorts on the RTX 5080 (s7_256 8.03, s1 8.80, m4_b128_p 10.06 µs: the cost of 4
  dispatches on an idle high-end GPU). The single-dispatch `s1_rank512_bitreg2048_radix` falls
  behind as the batch grows: at 512 sorts it is 1.31x (RTX 5080), 1.43x (RX 7900 XTX) and 1.91x
  (Radeon iGPU) slower than the best. Mixed batches need tier-sized groups once the GPU is busy:
  a 1024-thread group for a 50-element sort takes occupancy the other sorts need.
- **worst_case: large sorts still need 1024-thread groups.** Every configuration with the pass2
  LDS radix @1024 for 2049+ (including m4_b128_p) is within 8 % of the best; the 256-thread
  ballot radix tiers are 1.5-1.7x slower (table above). RX 7900 XTX at 512 sorts: 243 µs, RTX
  5080: 114 µs.
- **mostly_medium (129-512)**: `m4_b128e8` (bitonic E8 in 128 threads) wins on AMD at large counts
  (RX 7900 XTX at 512 sorts 11.84 vs m4_b128_p 14.92 µs, 21 % faster; Radeon iGPU 324.1 vs 386.3
  µs, 16 % faster, and the best at every count). `t2_rank512_bitonic` (rank sort in 512-thread
  groups) wins at every count on the RTX 5080 (12.13 vs m4_b128_p 17.46 µs at 512 sorts).
- **mostly_empty**: `s7_256b` is the best on the RTX 5080, `t2_rank512_bitonic` on the RX 7900 XTX
  (128-512 sorts), `m4_b64` on the Radeon iGPU. m4_b128_p is 1.07-1.88x slower than the best here,
  but the absolute times are small (RX 7900 XTX at 512 sorts: 4.44 vs 2.36 µs).
- **Recommendation: `m4_b128_p` becomes the single default**: the best or near the best on the
  mixed and the large workloads at every batch size on every GPU measured.
  `s1_rank512_bitreg2048_radix` remains the option for high-end NVIDIA GPUs with small batches
  (about 20 sorts): on the RTX 5080 it beats m4_b128_p by 13 % on realistic_mix (8.80 vs 10.06 µs)
  and by 43 % on mostly_empty (2.59 vs 4.53 µs), but it is 1.29x slower at 512 sorts (25.87 vs
  20.03 µs).
- Open tuning opportunities for the 129-512 tier at 128-512 sorts: B8 instead of B4 in the
  128-thread tier on AMD (m4_b128e8), rank sort in 512-thread groups on NVIDIA
  (t2_rank512_bitonic).
