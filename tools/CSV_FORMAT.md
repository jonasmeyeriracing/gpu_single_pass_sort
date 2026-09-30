# GpuSort CSV output (schema_version 5)

> **Default flush mode changed (schema_version 4, the final set):** algorithms without a `flush`
> line now run with `full_d50` (256 MB flush, then a ~50 탎 ALU spin drain). Packages up to
> 627724b (schema_version 1-3) used `full` (flush + the pass5 one-group drain), which on the
> RX 7900 XTX still timed a ~12 탎 post-flush penalty in most small-workload iterations
> (mostly_empty median 14.7 탎 `full` vs 1.8 탎 `full_d50`); pass0-pass4 used `full_legacy`.
> **Rows with different `flush_mode` values are not directly comparable**; compare within one
> flush mode (the RTX 5080 and the Ryzen iGPU show at most a few tenths of a 탎 between them).

Every GpuSort.exe benchmark / smoke run writes, next to its results `.txt` (`--out`, default
`results.txt` next to the exe):

| file | rows |
|---|---|
| `<stem>.csv` | one per GPU x algorithm x workload; the sweep workload: one per sort size |
| `<stem>_samples.csv` | one per measured iteration (`--no-samples`: not written) |
| `<stem>_wave_probe.csv` | one per GPU x probed wave configuration |

`--csv <file>` sets the first file's path; the other two follow its stem. A `--wave-probe` run
writes only the wave probe CSV, as `<stem>.csv` (default `wave_probe.csv` next to the exe). In a
run_all.bat results folder: `current.csv`, `current_samples.csv`, `current_wave_probe.csv`,
`smoke_pass4_wave64.csv`, ..., in probe mode `wave_probe.csv`, in diag mode `diag_flush.csv`
(+ `diag_flush_stable.csv`, the same with `--stable-power`), in final mode `final_smoke.csv`,
`final.csv` (+ `final_smoke_wave64.csv`, `final_wave64.csv` on GPUs with a wave size range), and in
scale mode `scale_smoke.csv`, `scale.csv`.

Format: UTF-8 without BOM, a header row, CRLF line ends, RFC 4180 quoting (a field with a comma,
quote or line break is quoted, inner quotes doubled), `.` as the decimal separator whatever the
Windows locale, times in microseconds with 3 decimals. Empty field = not applicable / no value.
Columns are identified by their header name; new columns are only ever appended, and
`schema_version` changes if a column is renamed, removed or changes meaning.

Schema versions: **1** (packages up to 60814ed); **2** (pass6, package 6a12132): the results CSV
appends `stable_power` and `drain_spin_iters_per_us`, and `flush_mode` can carry a drain suffix
(`full_d20`, ...; see below); **3** (the pass7 merge): the results CSV appends `dispatch_info`;
**4** (the final set): the results CSV appends `pass`, `description` and `tags`,
`iterations_requested` is per GPU (`--iterations-integrated`), and the default flush mode is
`full_d50` (see the note at the top); **5** (the scale run): `sorts_per_iteration` is the row's
`--sorts` value (it was always 20) and part of the row key, `iterations_requested` is per row
(GPU x sort count), and the samples CSV appends `sorts_per_iteration`.
The wave probe columns are unchanged (the wave probe file's `schema_version` is that of the package
as well). `tools/aggregate_results.py` reads all five, leaves the results columns a row's version
does not have empty, and fills in `sorts_per_iteration` = 20 for samples files without the column.

`tools/aggregate_results.py` merges these files from many results folders / zips into
`all_results.csv`, `all_samples.csv` and `all_wave_probe.csv` (see its `--help`).

## Keys

- `run_id`: `<yyyymmdd>T<hhmmss>.<ms>_<computer>` (local start time): one per GpuSort.exe run, the
  same in all three files of the run.
- A results row is identified by `run_id, gpu_index, wave_size, algorithm, flush_mode, workload,
  sweep_size, sorts_per_iteration`; `samples.csv` rows join on the same columns (`sweep_size` is
  filled for the sweep workload in both; samples files before schema 5 have no
  `sorts_per_iteration`: 20). `algorithm_id` (the full name from algorithms.txt) is unique per run
  and sort count.

## `<stem>.csv` (results)

| column | meaning |
|---|---|
| schema_version | 5 (4: `sorts_per_iteration` always 20, `iterations_requested` per GPU; 3: also without `pass`, `description`, `tags`, default flush `full`; 2: also without `dispatch_info`; 1: packages up to 60814ed, also without `stable_power` and `drain_spin_iters_per_us`) |
| run_id | see Keys |
| run_timestamp | run start, ISO 8601 local time with UTC offset, e.g. `2026-09-29T14:30:12.345+02:00` |
| computer | Windows computer name |
| label | `--label` (run_all.bat: `k/N: full current wave64` etc.) |
| package_commit | git commit the exe was built from: short sha, `<sha>-dirty` (uncommitted changes to src/shaders/_test/tools/the project), `unknown` (built without git) |
| shader_set | shader directory name: `shaders` (current) or `passN` (`_test\passN`) |
| gpu_index | 0, 1, ... in the run's GPU order (high performance first) |
| gpu_name | DXGI adapter description |
| vendor_id, device_id | PCI ids, hex (`0x10DE` NVIDIA, `0x1002` AMD, `0x8086` Intel, `0x1414` WARP) |
| driver_version | UMD driver version, e.g. `32.0.15.8157` |
| is_integrated | 1 = UMA (integrated GPU, WARP), 0 = discrete |
| dedicated_vram_mb | DXGI dedicated video memory, MB |
| wave_lane_min, wave_lane_max | D3D12 WaveLaneCountMin / Max |
| wave_size | WAVE_SIZE the shaders were compiled with |
| wave_size_attr | 1 = compiled with `[WaveSize(wave_size)]` |
| timestamp_freq_hz | GPU timestamp frequency (time resolution = 1 / this) |
| algorithm | algorithm name without its `@suffix` (e.g. `s4_3tier_rx3r`) |
| flush_mode | what ran between the upload and the timed sort (see "Flush modes" below): `full_d50` (the default, the measurement of record since schema 4), `full` (the default up to schema 3), `full_legacy`, `full_ro`, `code`, `data`, `none`, or one of these with a drain suffix, e.g. `full_d20` |
| workload | `mostly_empty`, `mostly_small`, `realistic_mix`, `mostly_large`, `worst_case`, `edges`, `mostly_mid`, `mostly_medium`, `sweep`, `sparse_keys` |
| sweep_size | sweep workload: the sort size of this row's iterations (all sorts of an iteration have it); empty otherwise |
| sorts_per_iteration | sorts per iteration (one batch; each dispatch runs this many groups): the run's `--sorts` value of this row (default 20; always 20 up to schema 4). An iteration of N sorts draws N sizes from the workload's distribution, and its first 20 are those of the 20-sort iteration |
| iterations | measured iterations the statistics are over (sweep: those of this size) |
| warmup | warmup iterations per algorithm x workload (not in the statistics) |
| min_us, median_us, mean_us, p95_us, p99_us, max_us | GPU time of one iteration (all `sorts_per_iteration` sorts), microseconds; p95 / p99 nearest rank |
| stddev_us | sample standard deviation (n - 1) |
| verify_failures | iterations whose output was not sorted correctly (warmup included; sweep: of this size) |
| total_elements_per_iter_mean | mean elements per iteration (sum of the iteration's sort sizes) |
| algorithm_id | full algorithm name from algorithms.txt, e.g. `s4_3tier_rx3r@data` |
| run_kind | `benchmark`, `smoke` (`--smoke`: few iterations, one in flight, a safety check, not a measurement) |
| iterations_requested | measured iterations requested for this row: `--iterations`, or `--iterations-integrated` on an integrated (UMA) GPU, at this row's sort count (v5; v4: per GPU; up to v3 always `--iterations`) |
| wave_probe_ok | 1 = the wave probe found the configuration the shaders were compiled for, 0 = WAVE PROBE WARNING, empty = the probe did not run |
| gpu_error | non-empty if the GPU's run aborted (e.g. `DEVICE LOST: ...`); the rows hold the partial results |
| stable_power | (v2) `off` (not requested), `on` (`--stable-power`: `ID3D12Device::SetStablePowerState(TRUE)` succeeded, fixed clocks), `unavailable` (requested, but Windows Developer Mode is off: not called, normal clocks), `failed` (the call returned an error, normal clocks) |
| drain_spin_iters_per_us | (v2) the GPU's calibrated spin drain rate (loop iterations per microsecond; a `_d<N>` drain runs round(N x this) iterations); empty if the run has no spin drain |
| dispatch_info | (v3) the occupancy inputs of the algorithm's dispatches, for this GPU's wave configuration, in dispatch order, separated by `;`: `<threads per group>t/<groupshared bytes>B`, e.g. `1024t/32768B;256t/8192B;512t/2048B` (threads from the shader reflection, groupshared bytes = the sum of the DXIL's groupshared variables; `?` if unknown) |
| pass | (v4) the pass (`_test/passN`) that introduced the algorithm (`pass` line in the algorithm list, e.g. shaders/algorithms_final.txt); empty if the list has none |
| description | (v4) the algorithm's one-line description (`desc` line); empty if none |
| tags | (v4) the algorithm's tags (`tag` lines), `;`-separated, e.g. `rec_discrete;reference` (the recommended configuration for discrete GPUs), `rec_integrated`, `method` (a flush-method variant of the reference); empty if none |

### Flush modes

`<kind>[_dg|_d<N>]`, kind = `full` (upload, 256 MB read+write flush), `full_ro` (upload, 256 MB
read-only flush), `code` (flush, then upload), `data` (upload, flush, untimed warm-up run of the same
sort on a private copy), `none` (upload only). The drain runs right before the start timestamp:

| suffix | drain |
|---|---|
| none | the pass5 default of that kind: the group drain for `full` / `code` / `data` / `none`, no drain for `full_ro` (the default *mode* is `full_d50`, i.e. `full` with a 50 탎 spin drain) |
| `_dg` | group drain (pass5): one group of the flush shader on a private 4 KB buffer + UAV barrier |
| `_d<N>` | spin drain (pass6), N = 1..1000: an ALU-only dispatch of about N us (4 groups x 64 threads, a dependent integer chain, one 16-byte store per thread to the private 4 KB buffer; loop count from the per-GPU calibration, `drain_spin_iters_per_us`) + UAV barrier |
| `_d0` | no drain |

Names are canonical: `full_d0` is written as `full_legacy` (the pre-pass5 `full`), `full_ro_d0` as
`full_ro`, `full_dg` as `full`, `code_dg` as `code`, etc.

## `<stem>_samples.csv`

| column | meaning |
|---|---|
| run_id, gpu_index, wave_size, algorithm, flush_mode, workload | join keys (as in the results CSV) |
| iteration | measured iteration index, 0-based (the data is deterministic per workload x iteration) |
| time_us | GPU time of this iteration (all its sorts), microseconds |
| largest_sort | largest of the iteration's sort sizes |
| total_elements | sum of the iteration's sort sizes |
| sweep_size | sweep workload: the sort size (= largest_sort); empty otherwise |
| sorts_per_iteration | (v5) as in the results CSV; a join key. Not in samples files before schema 5 (all 20) |

## `<stem>_wave_probe.csv` / `wave_probe.csv`

One row per GPU x probed configuration: in a benchmark / smoke run the shaders' configuration and
the one without `[WaveSize]`; in a `--wave-probe` run without `--wave-size` every configuration
(without `[WaveSize]` and `[WaveSize(N)]` for every N in the lane range).

| column | meaning |
|---|---|
| schema_version, run_id, run_timestamp, computer, label, package_commit | as in the results CSV |
| run_kind | `benchmark`, `smoke` or `wave_probe` |
| gpu_index, gpu_name, vendor_id, device_id, driver_version, is_integrated, wave_lane_min, wave_lane_max | as in the results CSV |
| wave_size, wave_size_attr | the sort shaders' configuration on this GPU |
| probe_variant | `without [WaveSize]` or `[WaveSize(N)]` |
| probe_wave_size | N of `[WaveSize(N)]`, 0 = without |
| sort_config | 1 = this is the configuration the sort shaders use |
| group_sizes | group sizes probed, e.g. `64/512/1024` (empty: did not run) |
| observed_lanes | lane count(s) the driver used, e.g. `32` or `32 (group 64), 64 (groups 512/1024)` |
| verdict | `OK` / `WARNING`; empty = not judged (outside `--wave-probe`, only the sort configuration is) |
| problem | reason for a WARNING, or the cross-lane mismatches of an OK |
| all_threads_wrote, lane_mapping, waves, readlane_xor32, readlane_xor1, readlane_plus16, readlane_last, prefixsum, activesum, countbits, ballot, shflscan_x8, shflscan_after_if, shflscan_after_select | each `OK`, `FAIL` or `n/a` (not applicable, e.g. readlane_xor32 below 64 lanes) |
| gpu_error | as in the results CSV |
