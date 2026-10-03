# External results: AMD RX 7900 XTX, AMD Ryzen iGPU, Intel UHD Graphics 770, RTX 3080 Ti

Results of the portable package (tools/package.ps1, tools/run_all.bat) run by other people on
their machines, archived here with the RTX 5080 (this machine, _test/pass4) as the reference.
Everything in this file up to "Conclusions" was measured with the **pre-pass5 flush method**
(`full_legacy` since pass5, see _test/pass5/notes.md), including the 5080 column. That matters on
the 7900 XTX: its `full` numbers contain a ~13 µs measurement artifact (section "The 7900 XTX
flush tail"). The 2026-09-29 runs (section "2026-09-29: flush diagnostic and pass7") used the
pass5 `full` (one-group drain) as the default, which still leaves ~12 µs of the artifact on the
XTX; **since the final set the default is `full_d50`** (see that section). Results measured with
different defaults are not directly comparable on small workloads (check the `flush_mode`
column / the results header).

## What is archived

One folder per run (`<computer>_<date>_<time>`), with the result files of every GPU run
(`*.txt`), `summary.txt` (plan, exit codes), `adapters.txt` (`--list-adapters`),
`package_info.txt` (git commit of the package) and, where present, `wave_probe.txt`. Left out to
keep the repo small: `dxdiag.txt` (120-150 KB each, driver details are in adapters.txt and the
result headers) and the console logs `*.log` (60-170 KB each; they only repeat the results plus
progress lines). Kept logs: `STIMULATOR_20260928_1551/smoke_current_wave64.log` (11 KB, the wave64
failure) and `STIMULATOR_20260928_2131/wave_probe.log` (7 KB). Packages since 60814ed also write
CSV files (tools/CSV_FORMAT.md): kept are the results `*.csv` and `*_wave_probe.csv`, and the
per-iteration `*_samples.csv` as long as a folder's files stay below ~5 MB zipped (both
2026-09-29 folders: 0.5 and 1.8 MB zipped incl. the samples, so the samples are kept; 6 and 18 MB
unpacked). Left out there as well: dxdiag.txt and every `*.log` (50-180 KB; no run failed). The two
final-run folders (STIMULATOR_20260929_1529, JONAS-CPH_20260929_1658) leave out every
`*_samples.csv` (156 and 134 MB); the samples of their benchmark runs are in
_test/final/all_samples.csv.gz, the raw zips are `G:\My Drive\STIMULATOR_20260929_1529.zip` and
`C:\Users\jonas\Desktop\GpuSort-portable-3ebe93b\results\JONAS-CPH_20260929_1658.zip`. The two
scale-run folders (JONAS-CPH_20260930_1458, STIMULATOR_20260930_1545) are archived the same way:
no `*_samples.csv` (30 and 39 MB), no dxdiag.txt, no logs. The samples of their benchmark runs
are in _test/scale/all_samples.csv.gz, the raw zips are
`C:\Users\jonas\Desktop\GpuSort-portable-495478d\results\JONAS-CPH_20260930_1458.zip` and
`G:\My Drive\STIMULATOR_20260930_1545.zip`. The two IMS-KCOMBS-INT folders (scale run
IMS-KCOMBS-INT_20260930_1154, final run IMS-KCOMBS-INT_20260930_1653) are archived the same way:
no `*_samples.csv` (41 + 1 MB and 93 + 2 MB), no dxdiag.txt, no logs. Their benchmark samples are
in _test/scale/all_samples.csv.gz and _test/final/all_samples.csv.gz, the raw zips are
`C:\Users\jonas\Downloads\IMS-KCOMBS-INT_20260930_1154.zip` and
`C:\Users\jonas\Downloads\IMS-KCOMBS-INT_20260930_1653.zip`. The RTX 2060 scale run
(JONAS-CPH_20261003_1635) is archived the same way: no `*_samples.csv` (30 + 0.6 MB), no
dxdiag.txt, no logs. Its benchmark samples are in _test/scale/all_samples.csv.gz, the raw zip is
`D:\git\gpu_single_pass_sort\dist\GpuSort-portable-495478d\results\JONAS-CPH_20261003_1635.zip`
(dist/ is git-ignored).

**Two different Intel machines.** IMS-MDETURCK (i7-12700, UHD 770 driver 31.0.101.3616, 2026-09-28,
pass4 shaders only) and IMS-KCOMBS-INT (i9-13900K, RTX 3080 Ti + UHD 770 driver 32.0.101.6129,
2026-09-30, scale and final sets) are separate computers with different Intel drivers. Both iGPUs
are a UHD Graphics 770 (8086 / 4680, Xe-LP, 32 EUs); compare their numbers with that in mind (see
"2026-09-30: IMS-KCOMBS-INT").

| folder | GPUs | package (shaders) | mode | runs | result |
|---|---|---|---|---|---|
| STIMULATOR_20260928_1551 | 7900 XTX + Ryzen iGPU | 3409bf6 (pass2 shaders) | smoke, wave32 + wave64 | 2 of 4 (stopped) | wave32 OK (15 algorithms x 8 workloads, both GPUs); **wave64 FAILED**: `s1_radix`, mostly_empty, 3 of 3 iterations on the XTX (see _test/pass5/notes.md, "Wave64 root cause") |
| STIMULATOR_20260928_1715 | 7900 XTX + Ryzen iGPU | 5e087c3 (pass4 shaders, patched pass3 snapshot) | default: smoke + full x {current, pass3} x {wave32, wave64} | 8 of 8 | all exit 0, **0 verification failures** (2 x 150 + 2 x 80 combos x 1000 iterations per GPU, plus the smokes) |
| STIMULATOR_20260928_2131 | 7900 XTX + Ryzen iGPU | 095c4a9 | `run_all.bat probe` | 1 | every configuration OK; [WaveSize(64)]: 64 lanes, `readlane^32` OK |
| IMS-MDETURCK_20260928_1226 | Intel UHD Graphics 770 | b8d5c19 (pass4 shaders) | `run_all.bat current`: smoke + full | 2 of 2 | exit 0, **0 verification failures** (150 combos x 1000 iterations); probe OK at 16 lanes |
| STIMULATOR_20260929_1213 | 7900 XTX + Ryzen iGPU | 627724b (pass5 shaders, algorithms_diag_flush.txt) | `run_all.bat diag`: 300 iterations x mostly_empty / realistic_mix / worst_case, normal clocks + `--stable-power` | 2 of 2 | exit 0, 0 failures; the drain-length sweep (see below) |
| STIMULATOR_20260929_1225 | 7900 XTX + Ryzen iGPU | 627724b (_test/pass7) | `run_all.bat pass7`: smoke + full (300 iterations), wave32 both GPUs + wave64 iGPU only | 4 of 4 | exit 0, **0 verification failures** (21 algorithms x 10 workloads, every run; smoke 24 algorithms) |
| STIMULATOR_20260929_1529 | 7900 XTX + Ryzen iGPU | 99a07b4 (final set) | `run_all.bat final`: smoke + full (1000 iterations XTX, 300 iGPU), default wave32 both GPUs + wave64 XTX only | 4 of 4 | exit 0, **0 verification failures** (69 algorithm entries x 10 workloads, every run); see _test/final/notes.md |
| JONAS-CPH_20260929_1658 | RTX 5080 + RTX 2060 | 3ebe93b (final set) | `run_all.bat final`: smoke + full (1000 iterations), wave32 | 2 of 2 | exit 0, **0 verification failures**; see _test/final/notes.md |
| JONAS-CPH_20260930_1458 | RTX 5080 (RTX 2060 removed) | 495478d (scale set) | `run_all.bat scale`: smoke + full, 19 algorithms x 10 workloads x 20 / 128 / 256 / 512 sorts per batch (1000 / 300 / 200 / 150 iterations), wave32 | 2 of 2 | exit 0, **0 verification failures**; see _test/scale/notes.md |
| STIMULATOR_20260930_1545 | 7900 XTX + Ryzen iGPU | 495478d (scale set) | `run_all.bat scale`: as above (iGPU: 300 / 90 / 60 / 45 iterations), default wave32 on both GPUs | 2 of 2 | exit 0, **0 verification failures**; see _test/scale/notes.md |
| IMS-KCOMBS-INT_20260930_1154 | RTX 3080 Ti + UHD 770 (i9-13900K) | 495478d (scale set) | `run_all.bat scale` (noprompt): 19 algorithms x 10 workloads x 20 / 128 / 256 / 512 sorts; 3080 Ti 1000 / 300 / 200 / 150, UHD 770 300 / 90 / 60 / 45 iterations; default wave size (32 / 16) | 2 of 2 | exit 0, **0 verification failures**; probe OK at 32 / 16 lanes; see _test/scale/notes.md |
| IMS-KCOMBS-INT_20260930_1653 | RTX 3080 Ti + UHD 770 (i9-13900K) | 495478d (final set) | `run_all.bat final` (noprompt): smoke + full, 69 algorithm entries x 10 workloads (1000 iterations 3080 Ti, 300 UHD 770), default wave size | 2 of 2 | exit 0, **0 verification failures**; see _test/final/notes.md |
| JONAS-CPH_20261003_1635 | RTX 2060 only (swapped in for the RTX 5080) | 495478d (scale set) | `run_all.bat scale` (noprompt): smoke + full, 19 algorithms x 10 workloads x 20 / 128 / 256 / 512 sorts (1000 / 300 / 200 / 150 iterations), wave32 | 2 of 2 | exit 0, **0 verification failures**; probe OK at 32 lanes; see _test/scale/notes.md |

## Machines

| GPU | computer | architecture | vendor / device | driver | memory | wave lanes | timestamp | shaders compiled with |
|---|---|---|---|---|---|---|---|---|
| NVIDIA GeForce RTX 5080 (reference) | JONAS-CPH | Blackwell GB203, 84 SMs | 10DE / 2C02 | 32.0.16.1714 | 16 GB | 32 | 1 GHz | WAVE_SIZE=32 |
| AMD Radeon RX 7900 XTX | STIMULATOR | RDNA3 Navi 31, 48 WGPs, 96 MB Infinity Cache | 1002 / 744C | 32.0.11037.4004 | 24 GB | 32-64 | 100 MHz | WAVE_SIZE=32 + [WaveSize(32)] (default) or 64 + [WaveSize(64)] |
| AMD Radeon(TM) Graphics | STIMULATOR | Ryzen 7000 desktop iGPU (RDNA2, 2 CUs = 1 WGP), system memory | 1002 / 164E | 32.0.11037.4004 | 485 MB carve-out + shared | 32-64 | 100 MHz | as the XTX |
| Intel UHD Graphics 770 | IMS-MDETURCK (i7-12700) | Xe-LP, 32 EUs (2 subslices), system memory | 8086 / 4680 | 31.0.101.3616 | 128 MB + shared | 16 | 19.2 MHz | WAVE_SIZE=16 (no [WaveSize]) |
| Intel UHD Graphics 770 (a different machine) | IMS-KCOMBS-INT (i9-13900K) | Xe-LP, 32 EUs, system memory | 8086 / 4680 | 32.0.101.6129 | 128 MB + shared | 16 | 19.2 MHz | WAVE_SIZE=16 (no [WaveSize]) |
| NVIDIA GeForce RTX 3080 Ti | IMS-KCOMBS-INT (i9-13900K) | Ampere GA102 | 10DE / 2208 | 32.0.16.1692 | 12 GB | 32 | 1 GHz reported, samples in 1.024 µs steps | WAVE_SIZE=32 |
| NVIDIA GeForce RTX 2060 | JONAS-CPH (with the RTX 5080 in the final run, alone in the scale run) | Turing TU106 | 10DE / 1F08 | 32.0.16.1714 | 6 GB | 32 | 1 GHz, 32 ns steps (27-29 % of samples on 1.024 µs multiples) | WAVE_SIZE=32 |

Wall time of one full run (15 algorithms x 10 workloads x 1005 iterations): 5080 110 s, 7900 XTX
118 s, Ryzen iGPU 1455 s (wave32) / 1531 s (wave64), UHD 770 1144 s. The 256 MB flush of every
iteration dominates (0.73 / 0.78 / 9.7 / 7.6 ms per iteration); the sort adds at most ~5 % on
average. Since pass5 every run prints a calibrated estimate of its run time.

## Correctness

- **Every shader configuration that ships is correct on all four GPUs**: 0 verification failures
  in 1000-iteration runs of 15 algorithms x 10 workloads (pass4 set) and 8 algorithms x 10
  workloads (pass3 set, 7900 XTX + iGPU), at wave32 and wave64 on AMD and wave16 on Intel. The
  smoke runs (16 serial iterations per combo, every sweep size) are clean too.
- **The pass4 wave64 fix is confirmed on hardware** (7900 XTX and the RDNA2 iGPU). The pass2
  shaders failed at wave64 (package 3409bf6); the patched shaders pass everything.
- **The wave probe** (package 095c4a9) passes on both AMD GPUs in every configuration (without
  [WaveSize]: the driver chooses 32 lanes; [WaveSize(32)]; [WaveSize(64)]: 64 lanes, lane =
  SV_GroupIndex % 64, `WaveReadLaneAt(x, lane ^ 32)` correct). That disproves pass4's explanation
  of the wave64 failure ("WaveReadLaneAt cannot read across the 32-lane halves"): see
  _test/pass5/notes.md for the corrected analysis. On Intel the probe reports 16 lanes, lane =
  SV_GroupIndex % 16, all checks OK.

## The 7900 XTX flush tail (measurement artifact)

On the 7900 XTX **every algorithm in the default `full` mode is ~13 µs slower than the same
shader in `@data` / `@code` mode**, on every workload: `s1_rank512_bitreg2048_radix` full minus
@data, medians: mostly_empty 13.04, mostly_small 13.08, mostly_medium 13.00, mostly_mid 13.04,
realistic_mix 12.88, mostly_large 12.84, worst_case 12.72, sparse_keys 12.68 µs (wave64: the same,
12.8-13.1). The distribution is bimodal: mostly_empty `full` has min 1.20, median 14.68, mean 11.67,
i.e. roughly a quarter of the iterations are as fast as @data (min 0.88, median 1.64); the fast
`full` minima equal the @data minima on every workload (mostly_large 13.52 vs 13.88). A constant
additive cost that does not depend on the sort is not the sort: it is the tail of the 256 MB flush
landing inside the timed window. In `@data` / `@code` other work sits between the flush's barrier
and the start timestamp and absorbs it. Neither the Ryzen iGPU (full 4.12 vs @data 4.04) nor the
UHD 770 (6.56 vs 5.99) shows it; on the 5080 a much smaller version may exist (full 3.62 vs @data
3.04 vs @code 2.37, mostly_empty). pass5 fixes the method (a drain dispatch + barrier before the
start timestamp); on the 5080 the tail turned out to be only 0.2-0.3 µs (_test/pass5/notes.md). **All 7900 XTX `full` numbers below are inflated by
~13 µs; use the `@data` / `@code` rows for it.**

A second AMD artifact, in the serial smoke runs: with a CPU fence wait after every iteration the
7900 XTX runs large sorts at about half speed (`s1_rank512_bitreg2048_radix` mostly_large: serial
smoke 33.92 µs, full run @data 17.24, @code 16.16; the same for @data / @code in the smoke: 33.48 /
32.70), while small sorts are not slower. Most likely the GPU clocks drop while it idles between
serial iterations. So pass4's statement "large sorts are 2x slower on AMD than on the 5080" (based
on 3-iteration serial smokes) is wrong: in the batched runs the 7900 XTX is on par with the 5080
for large sorts (mostly_large @code 16.16 vs 16.70, worst_case 18.20 vs 18.72) and about 2x faster
for small ones (mostly_empty 1.48 vs 2.37).

## Cross-vendor tables

Median µs per iteration (20 sorts), 1000 iterations, pre-pass5 flush method. Columns: RTX 5080
(pass4 run), 7900 XTX wave32 / wave64, Ryzen iGPU wave32 / wave64, UHD 770 (wave16). Bold = best
per column among the `full` rows and, separately, among the `@data` / `@code` rows. The XTX `full`
rows carry the ~13 µs flush tail. Values >= 100 are rounded to 1 µs. Full tables (all 15
algorithms, min / mean / p95 / max, sweep per size) are in the result files.

**mostly_empty** (70 % empty, rest 1-64)

| algorithm | 5080 | XTX w32 | XTX w64 | iGPU w32 | iGPU w64 | UHD 770 |
|---|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 3.65 | **14.68** | 14.88 | **4.12** | **3.24** | 6.56 |
| s1_rank512_bitreg4096_radix | **3.62** | **14.68** | **14.84** | **4.12** | **3.24** | 6.56 |
| s4_4tier_4096 | 3.63 | **14.68** | 14.88 | 4.16 | **3.24** | 6.67 |
| s4_3tier_rx3r | 3.65 | 14.72 | 14.88 | 4.16 | **3.24** | **6.51** |
| s1_radix | 10.91 | 22.40 | 24.24 | 32.14 | 36.06 | 117 |
| s4_radix2 | 8.35 | 21.48 | 23.28 | 22.16 | 130 | 121 |
| s1_rank512_bitreg2048_radix@data | 3.04 | 1.64 | 1.84 | **4.04** | **3.16** | 5.99 |
| s1_rank512_bitreg2048_radix@code | 2.37 | **1.48** | **1.64** | 4.12 | 3.24 | 7.45 |
| s4_4tier_4096@data | 3.10 | 1.60 | 1.80 | 4.08 | 3.20 | 6.04 |
| s4_3tier_rx3r@data | 3.07 | 1.60 | 1.84 | 4.08 | 3.20 | **5.94** |
| s4_3tier_rx3r@code | **2.34** | **1.48** | 1.68 | 4.12 | 3.26 | 7.34 |

**mostly_medium** (129-512)

| algorithm | 5080 | XTX w32 | XTX w64 | iGPU w32 | iGPU w64 | UHD 770 |
|---|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 5.73 | 20.20 | **20.80** | **37.44** | **47.56** | 42.06 |
| s1_rank512_bitreg4096_radix | **5.70** | 20.20 | 20.84 | 37.46 | **47.56** | 42.03 |
| s4_4tier_4096 | **5.70** | 20.20 | 20.84 | 37.48 | 53.92 | 43.70 |
| s4_3tier_rx3r | **5.70** | 20.24 | 20.84 | **37.44** | 54.30 | **40.57** |
| s1_radix | 10.85 | 20.92 | 23.08 | 103 | 116 | 342 |
| s4_radix2 | 8.54 | **19.56** | 21.92 | 69.44 | 497 | 336 |
| s1_rank512_bitreg2048_radix@data | 4.80 | 7.20 | 7.76 | 37.38 | 47.48 | 41.35 |
| s1_rank512_bitreg2048_radix@code | 4.26 | **7.00** | **7.64** | 37.24 | **47.36** | 42.60 |
| s4_4tier_4096@data | 4.77 | 7.16 | 7.80 | 37.44 | 53.80 | 42.92 |
| s4_3tier_rx3r@data | 4.77 | 7.20 | 7.80 | **37.22** | 54.12 | **40.16** |
| s4_3tier_rx3r@code | **4.22** | **7.00** | 7.68 | **37.22** | 54.08 | 41.51 |

**realistic_mix** (15 % empty, 72 % 16-512, 10 % 513-2048, 3 % 2049-8192)

| algorithm | 5080 | XTX w32 | XTX w64 | iGPU w32 | iGPU w64 | UHD 770 |
|---|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 10.08 | 22.60 | 29.48 | **39.48** | 127 | 77.32 |
| s1_rank512_bitreg4096_radix | 9.98 | 22.62 | 29.60 | 40.32 | 142 | **76.56** |
| s4_4tier_4096 | **9.84** | 22.52 | 29.56 | 40.10 | 134 | 80.00 |
| s4_3tier_rx3r | 10.29 | 22.76 | 29.52 | 41.46 | 379 | 81.82 |
| s1_radix | 11.81 | 22.28 | 24.32 | 93.20 | **105** | 308 |
| s4_radix2 | 10.85 | **21.80** | **23.60** | 65.94 | 449 | 307 |
| s1_rank512_bitreg2048_radix@data | 8.77 | 9.72 | 16.48 | 39.44 | **127** | **72.01** |
| s1_rank512_bitreg2048_radix@code | 8.86 | **9.58** | **16.36** | **39.36** | 127 | 78.26 |
| s4_4tier_4096@data | **8.61** | 9.72 | 16.60 | 39.48 | 133 | 73.23 |
| s4_3tier_rx3r@data | 9.06 | 9.88 | 16.56 | 40.16 | 379 | 80.96 |
| s4_3tier_rx3r@code | 9.09 | 9.72 | **16.36** | 41.20 | 379 | 83.26 |

**mostly_mid** (513-2048)

| algorithm | 5080 | XTX w32 | XTX w64 | iGPU w32 | iGPU w64 | UHD 770 |
|---|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 9.63 | 22.88 | 29.96 | 109 | 916 | 257 |
| s1_rank512_bitreg4096_radix | 9.63 | 22.88 | 30.00 | 109 | 917 | 258 |
| s4_4tier_4096 | **9.60** | 22.92 | 30.16 | 111 | 908 | **257** |
| s4_3tier_rx3r | 10.02 | 23.04 | 29.88 | 109 | 3277 | 257 |
| s1_radix | 11.55 | 21.88 | 24.28 | 110 | **125** | 354 |
| s4_radix2 | 10.11 | **21.76** | **23.16** | **88.36** | 673 | 355 |
| s1_rank512_bitreg2048_radix@data | 8.67 | 9.84 | 16.84 | **108** | 916 | 256 |
| s1_rank512_bitreg2048_radix@code | **8.58** | **9.60** | **16.64** | 109 | 916 | 258 |
| s4_4tier_4096@data | 8.67 | 9.84 | 16.92 | 111 | **907** | **255** |
| s4_3tier_rx3r@data | 9.12 | 10.00 | 16.76 | 109 | 3277 | 256 |
| s4_3tier_rx3r@code | 8.99 | 9.72 | **16.64** | 109 | 3277 | 258 |

**mostly_large** (2048-8192)

| algorithm | 5080 | XTX w32 | XTX w64 | iGPU w32 | iGPU w64 | UHD 770 |
|---|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 17.12 | 30.08 | 32.48 | 174 | **191** | **422** |
| s1_rank512_bitreg4096_radix | **16.45** | 30.52 | 39.16 | 202 | 629 | 456 |
| s4_4tier_4096 | 16.93 | 30.28 | 32.60 | 180 | 420 | 450 |
| s4_3tier_rx3r | 19.87 | 34.40 | 34.04 | 210 | 684 | 682 |
| s1_radix | 17.28 | **30.04** | **32.38** | **171** | 192 | 430 |
| s4_radix2 | 18.22 | 33.92 | 35.32 | 199 | 894 | 536 |
| s1_rank512_bitreg2048_radix@data | 16.10 | 17.24 | 19.44 | **173** | **191** | **404** |
| s1_rank512_bitreg2048_radix@code | 16.70 | **16.16** | **18.56** | 173 | 191 | 421 |
| s4_4tier_4096@data | **15.78** | 17.44 | 19.60 | 177 | 417 | 426 |
| s4_3tier_rx3r@data | 19.01 | 21.52 | 21.04 | 207 | 682 | 679 |
| s4_3tier_rx3r@code | 19.52 | 20.44 | 20.08 | 209 | 684 | 681 |

**worst_case** (all 8192)

| algorithm | 5080 | XTX w32 | XTX w64 | iGPU w32 | iGPU w64 | UHD 770 |
|---|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix | 19.07 | **32.64** | 34.68 | 260 | **272** | 503 |
| s1_rank512_bitreg4096_radix | **18.24** | 32.68 | **34.64** | 260 | 272 | **503** |
| s4_4tier_4096 | 18.72 | 32.68 | 34.72 | 261 | 273 | 506 |
| s4_3tier_rx3r | 21.73 | 36.64 | 36.04 | 303 | 660 | 886 |
| s1_radix | 19.33 | 32.72 | 34.76 | **257** | 272 | 504 |
| s4_radix2 | 20.00 | 36.32 | 37.60 | 294 | 1151 | 796 |
| s1_rank512_bitreg2048_radix@data | 18.11 | 19.92 | 21.84 | **259** | **271** | **483** |
| s1_rank512_bitreg2048_radix@code | 18.72 | **18.20** | **20.28** | 260 | 271 | 498 |
| s4_4tier_4096@data | **17.74** | 19.84 | 21.84 | 260 | 271 | 486 |
| s4_3tier_rx3r@data | 20.83 | 23.96 | 23.28 | 300 | 659 | 885 |
| s4_3tier_rx3r@code | 21.57 | 22.16 | 21.60 | 303 | 660 | 882 |

**Sweep: where the tiers cross** (median µs, all 20 sorts of an iteration have the given size;
`full` mode, so the XTX numbers include the tail except at 2048 / 2560, which happen to be almost
always tail-free there)

| GPU | path | 768 | 1024 | 1536 | 2048 | 2560 | 3072 | 4096 | 5120 | 8192 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| iGPU w32 | bitonic E8 (ref2048 / ref4096) | 78.9 | 79.8 | 119.6 | 125.3 | 215.6 | 218.8 | 228.9 | - | - |
| iGPU w32 | pass2 radix (s1_radix / ref2048 above 2048) | 103.9 | 105.3 | 112.3 | 120.0 | 127.9 | 136.2 | 155.2 | 170.1 | 260.4 |
| iGPU w32 | radix_sort2 (s4_radix2) | 74.9 | 78.2 | 90.8 | 105.6 | 117.1 | 134.9 | 166.5 | 196.7 | 294.5 |
| UHD 770 | bitonic E8 | 186.8 | 181.2 | 284.5 | 279.0 | 451.5 | 451.3 | 430.6 | - | - |
| UHD 770 | pass2 radix | 340.2 | 349.0 | 354.4 | 367.0 | 379.0 | 391.1 | 408.8 | 419.1 | 503.9 |
| UHD 770 | radix_sort2 | 341.3 | 347.0 | 358.6 | 375.0 | 397.3 | 417.2 | 449.7 | 466.3 | 797.0 |
| XTX w32 | bitonic E8 @data (ref2048@data) | 6.76 | 6.96 | 9.56 | 10.20 | - | - | - | - | - |
| XTX w32 | pass2 radix @data (ref2048@data above 2048) | - | - | - | - | 10.52 | 11.64 | 12.56 | 13.32 | 19.86 |
| XTX w32 | radix_sort2 ≤ 4096 @data (4tier@data) | - | - | - | - | 10.14 | 11.12 | 13.40 | 14.10 | 20.06 |
| 5080 | bitonic E8 @data | 6.50 | 6.56 | 8.64 | 8.86 | - | - | - | - | - |
| 5080 | pass2 radix @data | - | - | - | - | 10.91 | 11.49 | 12.48 | 13.28 | 18.21 |
| 5080 | radix_sort2 ≤ 4096 @data | - | - | - | - | 8.77 | 9.34 | 10.75 | 13.25 | 17.76 |

## Conclusions

**1. Best portable configuration: `s1_rank512_bitreg2048_radix`** (pass2 code: rank ≤ 512,
register bitonic E8 ≤ 2048, pass2 LDS radix above; one 1024-thread dispatch), compiled with
[WaveSize(32)] on AMD. It is the best or within ~2 % of the best `full` row on every workload on
the iGPU and the UHD 770, within noise of the best on the 7900 XTX (@data / @code rows), and
within 0.1-0.7 µs of the best on the 5080. No other configuration is safe everywhere:

| GPU | best measured | notes |
|---|---|---|
| RTX 5080 | `s1_rank512_bitreg4096_radix` (pass4, legacy method); `s4_4tier_4096` with warm code | 4096 vs 2048: 16.45 vs 17.12 on mostly_large; see _test/pass5 for the drained re-measurement |
| RX 7900 XTX (wave32) | ref2048 ≈ `s4_4tier_4096` (@data: realistic_mix 9.72 / 9.72, mostly_large 17.24 / 17.44) | small sorts about 2x faster than the 5080, large ones on par; the radix paths beat bitonic slightly at 513-2048 (mostly_mid s4_radix2 21.76 vs 22.88, full) |
| Ryzen iGPU (wave32) | ref2048 (realistic_mix 39.48, mostly_large 174, worst_case 260) | 4096 is 16 % worse on mostly_large; radix_sort2 beats bitonic from 768 up (mostly_mid 88 vs 109) |
| UHD 770 (wave16) | ref2048 (mostly_large 422 vs 456 for ref4096; realistic_mix 77.3 vs 76.6) | the radix_sort2/3 variants lose 25-75 % at 5-8k (rx3r worst_case 886 vs 503) |

**2. Thresholds.**
- **Radix switch: 2048 on both iGPUs**, not 4096. At 2560-4096 bitonic E8 costs 1.5-1.7x the
  pass2 radix on the Ryzen iGPU (215.6-228.9 vs 127.9-155.2 µs) and 1.05-1.2x on the UHD 770
  (430.6-451.5 vs 379.0-408.8). The 7900 XTX (legacy numbers) is neutral to slightly in favour of
  2048. Only the 5080 measured 4096 slightly better (pass4). So 2048 is the portable choice.
- **513-2048:** bitonic E8 is right on the UHD 770 (the radix has a ~340 µs floor there, see 3)
  and the 5080. On the Ryzen iGPU radix_sort2 is faster from 768 up (74.9-105.6 vs 78.9-125.3 µs);
  on the 7900 XTX the radix paths are ~1 µs faster on mostly_mid. A "rank ≤ 512, radix_sort2
  513-4096, pass2 radix above" configuration is worth a test on AMD (not in the tested sets).
- **Rank ≤ 512** holds on the iGPU and the UHD 770 (rank 37.4 vs register bitonic E4 53.6 on the
  iGPU's mostly_medium, pass3 set). On the 7900 XTX bitonic E4 was 1.9 µs faster than rank at
  129-512 (pass3 set, both with the tail: 18.28 vs 20.16), consistent with the pass2 smoke hint
  (RANK_MAX ≈ 128 on AMD dGPUs).

**3. Intel UHD 770: the radix has a large fixed cost at wave16.** `s1_radix` (the pass2 radix for
every size) takes 117 µs on mostly_empty (5080 10.9, iGPU 32.1) and a flat ~340 µs for 20 sorts of
any size from 32 to 1024 elements (sweep); radix_sort2 (331-347) and the rolled radix_sort3 (104 on
mostly_empty) are no better. The rank / bitonic tiers are fine (mostly_empty 6.5 µs, 1.8x the
5080). The numbers fit an occupancy limit rather than work: a 1024-thread group at SIMD16 needs 64
of the 112 hardware threads of a Xe-LP subslice, so the 32-EU part runs only two such groups at a
time. The radix runs the whole group through 4 passes and 11 barriers whatever the count, with
per-wave digit tables over 64 waves (wave 0 scans 4 waves per lane): about 35 µs per group, so 20
radix groups take ~10 rounds x 34 µs = 340 µs, and the ~6 non-empty groups of mostly_empty ~3
rounds = 117 µs. (Inferred from the architecture and the numbers; there is no profiler data.)
Consequences: never route small sorts to the radix on Intel (the tiered shaders already do not);
for large sorts on Intel a 512- or 256-thread radix (more keys per thread, several groups per
subslice) is the obvious next experiment.

**4. AMD: wave32, not wave64.**
- 7900 XTX (@data rows): wave64 is slower everywhere. Rank tier +12-20 % (mostly_empty 1.84 vs
  1.64, mostly_small 2.40 vs 2.00); **bitonic tier +70 %** (mostly_mid 16.84 vs 9.84; sweep
  768-2048 12.9-17.2 vs 6.8-10.2), because at wave64 the fixed shaders run bitonic as two 32-lane
  virtual waves (pass4 fix: lane bit 5 goes through LDS like a wave bit); radix +10-13 %
  (mostly_large 19.44 vs 17.24, worst_case 21.84 vs 19.92).
- Ryzen iGPU (RDNA2): the rank tier is faster at wave64 (mostly_empty 3.24 vs 4.12, mostly_small
  8.36 vs 10.64), but everything else collapses: register bitonic 8.4x slower (mostly_mid 916 vs
  109), radix_sort2 5-7x (s4_radix2 mostly_empty 130 vs 22), the rolled radix_sort3 at 768-2048 up to
  27x (2755 vs 79.3 µs at 768 in rx3r's sweep); the pass2 radix only +10 %. Slowdowns of that size
  look like register spilling or a register-limited wave count (RDNA2 has 2/3 of RDNA3's VGPRs, and
  a 1024-thread group at wave64 is 16 waves on one CU pair); not verifiable without AMD tools.
- So the default ([WaveSize(32)] on RDNA) is right, and wave64 is only a correctness test.

**5. Relative speed.** Small sorts: the 7900 XTX is the fastest GPU tested (mostly_empty @code 1.48
vs 5080 2.37). Large sorts: 7900 XTX ≈ 5080 (@data / @code). The Ryzen iGPU (1 WGP) is ~10x (large)
to 1.7x (tiny) slower than the 5080; the UHD 770 ~25x (large) to 1.8x (tiny). Both iGPUs are
occupancy-bound on the 1024-thread groups: fewer sorts in flight, so the large tiers dominate.

**6. What the next external run should measure** (done: the 2026-09-29 runs below) (the pass5 package does all of it by default):
the drained `full` method next to `full_legacy`, `full_ro`, `data`, `code` and `none`, to confirm
that the drain removes the 13 µs XTX tail without changing anything else, and whether it is the
flush's dirty lines (full_ro fast) or the barrier placement (full_ro slow).

## 2026-09-29: flush diagnostic and pass7

Two runs of package 627724b on STIMULATOR (driver 32.0.11037.4004 as before). Details and every
pass7 hypothesis: _test/pass7/notes.md, "AMD results".

**Flush diagnostic** (`run_all.bat diag`, STIMULATOR_20260929_1213; `s1_rank512_bitreg2048_radix`
with every flush / drain variant, median µs, 300 iterations):

| flush mode | XTX empty | XTX mix | XTX worst | iGPU empty | iGPU mix | iGPU worst |
|---|---:|---:|---:|---:|---:|---:|
| full_legacy (no drain) | 14.68 | 22.52 | 32.88 | 4.12 | 39.84 | 260.32 |
| full (pass5 group drain) | 13.80 | 21.72 | 32.00 | 4.08 | 39.70 | 260.44 |
| full_d2 | 13.00 | 20.92 | 31.16 | 4.40 | 39.96 | 260.68 |
| full_d5 | 11.48 | 19.44 | 29.68 | 4.40 | 40.04 | 260.64 |
| full_d10 | 8.88 | 16.92 | 27.04 | 4.36 | 40.14 | 260.68 |
| full_d20 | 3.76 | 11.96 | 21.92 | 4.40 | 40.16 | 260.72 |
| **full_d50** | **1.76** | **9.92** | **20.24** | 4.40 | 40.02 | 260.72 |
| full_d100 | 1.76 | 10.04 | 20.40 | 4.40 | 39.98 | 260.68 |
| full_ro (read-only flush, no drain) | 6.00 | 14.18 | 23.48 | 4.12 | 39.88 | 259.68 |
| full_ro_d20 | 1.72 | 9.92 | 19.42 | 4.44 | 40.02 | 259.96 |
| data | 1.68 | 9.70 | 20.18 | 4.32 | 40.00 | 259.92 |
| code | 1.48 | 9.56 | 18.60 | 4.32 | 39.98 | 260.04 |
| none | 1.52 | 9.72 | 34.56 | 4.06 | 39.70 | 259.28 |

- On the 7900 XTX the post-flush penalty is not a fixed tail that a tiny dispatch absorbs: it
  decays with the time the GPU waits after the flush (d5 11.5, d10 8.9, d20 3.8 µs) and is gone
  from ~50 µs (d50 = d100 = 1.76 µs vs @data 1.68 on mostly_empty; worst_case d50 20.24 vs @data
  20.18). A read-only flush leaves less of it (full_ro 6.0), so it is mostly the write-back of the
  flush's dirty lines (or a clock / power state change after the 256 MB burst) overlapping the
  sort. **The default flush mode is now `full_d50`** (Algorithms.h `kDefaultFlushMode`).
- The Ryzen iGPU shows no such effect (every drain within 0.3 µs; the spin drain costs +0.3 µs on
  mostly_empty); the RTX 5080 was flat from d5 (_test/pass5 / pass6 runs).
- `--stable-power` (diag_flush_stable, Developer Mode on): XTX +20-30 % on mix / worst_case, the
  iGPU ~3x slower (clocks locked low). Not used for the final run.

**pass7** (`run_all.bat pass7`, STIMULATOR_20260929_1225): 0 failures on both GPUs at wave32 and
on the iGPU at wave64. The flush mode was the pass5 `full`, so the XTX's small-workload numbers
carry the ~12 µs artifact (mostly_empty ~13.8 µs for every algorithm). Main results (median µs):

| | ref `s1_rank512_bitreg2048_radix` | `m4_b128_p` | best other |
|---|---|---|---|
| iGPU w32 realistic_mix | 39.90 | **23.72** | m4_b128e8 22.82 (X large tier: 263 on mostly_large) |
| iGPU w32 mostly_medium | 37.58 | 18.60 | m4_b128e8 16.04 |
| iGPU w32 mostly_mid | 109.22 | 63.68 | m4_b64 63.14 |
| iGPU w32 worst_case | 260.40 | 258.48 | s1_radix 257.36 |
| XTX w32 realistic_mix | 21.84 | 21.38 | m3_ref_32k 21.08 |
| XTX w32 mostly_medium | 19.44 | 17.00 | s7_256b 16.96 |
| XTX w32 worst_case | 32.16 | 32.32 | s1_radix 32.12 |

- The Ryzen iGPU gains 40-50 % on the small and mid tiers from tier-sized groups (multi-dispatch
  with small groups and small LDS), nothing on the large tier; the pass2 radix in 1024-thread
  groups stays the best large tier (the pass7 radix X in 256 / 512 threads: +52 / +25 %).
- iGPU wave64: small groups do not remove the wave64 collapse (m4_b128_p mostly_mid 384 µs,
  X up to 2 ms for worst_case); keep wave32.
- Per-GPU-class recommendation: discrete GPUs `s1_rank512_bitreg2048_radix` (one dispatch),
  integrated GPUs `m4_b128_p` (4 tier-sized dispatches). Both are tagged in
  shaders/algorithms_final.txt (`rec_discrete`, `rec_integrated`).

**Next:** `run_all.bat final` (shaders/algorithms_final.txt: every distinct algorithm of pass0-7,
default flush `full_d50`, 1000 iterations on discrete / 300 on integrated GPUs, wave64 on the XTX
only), see tools/README_PORTABLE.txt. (Done: _test/final/notes.md, and the scale run
_test/scale/notes.md.)

## 2026-09-30: IMS-KCOMBS-INT (i9-13900K: RTX 3080 Ti + UHD 770)

Package 495478d, `run_all.bat scale` (IMS-KCOMBS-INT_20260930_1154, 34 min) and `run_all.bat
final` (IMS-KCOMBS-INT_20260930_1653, 62 min), both without prompts. Every run exit 0, **0
verification failures**, wave probe OK on both GPUs (3080 Ti 32 lanes, UHD 770 16 lanes, lane =
SV_GroupIndex % lanes, every cross-lane check OK). This is the first hardware run of the pass7
shaders (`p7_*`, used by `m4_b128_p`) at wave16. The results are merged into _test/final and
_test/scale; details there.

**Not the same machine as IMS-MDETURCK.** Same GPU model, different CPU platform and a newer Intel
driver (32.0.101.6129 vs 31.0.101.3616). For the shaders both ran, the two UHD 770s agree within
about 7 % (median µs; IMS-MDETURCK pass4 run with the pre-pass5 flush, IMS-KCOMBS-INT final run
with the same flush, `s1_rank512_bitreg2048_radix@full_legacy`; `s1_radix` with its default):

| | mostly_empty | mostly_medium | realistic_mix | mostly_mid | mostly_large | worst_case |
|---|---:|---:|---:|---:|---:|---:|
| s1_rank512_bitreg2048_radix, IMS-MDETURCK | 6.56 | 42.06 | 77.32 | 257 | 422 | 503 |
| s1_rank512_bitreg2048_radix, IMS-KCOMBS-INT | 7.32 | 40.60 | 82.37 | 245 | 429 | 513 |
| s1_radix, IMS-MDETURCK (full_legacy) | 117 | 342 | 308 | 354 | 430 | 504 |
| s1_radix, IMS-KCOMBS-INT (full_d50) | 126 | 341 | 307 | 357 | 434 | 516 |

So the 1024-thread radix floor of conclusion 3 (~340 µs for 20 radix groups of any size) holds on
the second machine and driver too.

**UHD 770 results (final + scale runs).** Conclusion 3's prediction holds: the pass7 radix X in
256 / 512-thread groups beats the 1024-thread pass2 radix for large sorts. At 20 sorts (final run)
worst_case: `s7_512` 310.23 µs, `m3_x512` 346.95, s1 511.07, m4_b128_p 518.78; mostly_mid:
`x7_all_256` 81.22, m4_b128_p 123.54, s1 244.58; realistic_mix: `m2_x513` 46.95, m4_b128_p 58.38,
s1 80.55. In the scale run `s7_512` is the best on worst_case at every batch size (1.52-1.79x
faster than m4_b128_p) and on realistic_mix m4_b128_p is 1.11-1.46x behind the best (`x7_all_256`,
`m4_b256`, `m4_b128e8`: all with X @256 for 2049+). So no configuration of the set is the best
everywhere on Intel; the recommended default `m4_b128_p` is correct there and 1.3-2.5x faster than
s1 on the mixed and mid workloads, and an Intel-tuned tier set is an open item.

**RTX 3080 Ti.** Behaves like the RTX 5080: on realistic_mix `m4_b128_p` equals the best of the 19
scale algorithms at 20, 128, 256 and 512 sorts (11.26 / 21.50 / 23.55 / 27.65 µs), and s1 falls
behind as the batch grows (1.44x at 512 sorts). Unlike the 5080, its worst_case at 128-512 sorts
is fastest with the X radix in 256 / 512-thread groups (512 sorts: s7_256b 165.89 vs m4_b128_p
189.44 µs). Its timestamps tick in 1.024 µs steps (every sample of both runs is a multiple of
1.024 µs, although the driver reports 1 GHz), so small differences are below its resolution. Wall
time of the full final run: 546 s (3080 Ti), 2907 s (UHD 770).

## 2026-10-03: JONAS-CPH with only the RTX 2060

Same computer as the other JONAS-CPH runs (RTX 5080 reference machine), but with the RTX 2060
swapped in for the RTX 5080: the 2060 was the only GPU installed (PCI bus 1, the 5080's slot; in
the final run JONAS-CPH_20260929_1658 it sat next to the 5080 on PCI bus 7). Driver 32.0.16.1714
as before. Package 495478d, `run_all.bat scale` without prompts (JONAS-CPH_20261003_1635, 14 min;
estimate 15.8 min; full run 774 s). Both runs exit 0, **0 verification failures**, wave probe OK
(32 lanes, lane = SV_GroupIndex % 32, every cross-lane check OK), no nvlddmkm events. The results
are merged into _test/scale; details there.

The 2060's 20-sort values in this run are lower than in its final run: median 6 % over the 171
common algorithm x workload rows, up to 27 % (realistic_mix: s1_rank512_bitreg2048_radix 20.42 ->
16.38, m4_b128_p 19.39 -> 18.13, s7_512 15.71 -> 14.34 µs; worst_case 38.34 -> 36.86 µs for s1).
The RTX 5080's final and scale runs (same two packages) agree much better (median 0.2 %; 15 of 171
rows differ by more than 5 %, all small workloads), so the difference is most likely the hardware
setup (slot, only GPU in the system), but that was not investigated. Compare the
2060's scale and final numbers with that in mind.
