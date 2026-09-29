# GpuSort CSV output (schema_version 1)

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
`smoke_pass4_wave64.csv`, ... and, in probe mode, `wave_probe.csv`.

Format: UTF-8 without BOM, a header row, CRLF line ends, RFC 4180 quoting (a field with a comma,
quote or line break is quoted, inner quotes doubled), `.` as the decimal separator whatever the
Windows locale, times in microseconds with 3 decimals. Empty field = not applicable / no value.
Columns are identified by their header name; new columns are only ever appended, and
`schema_version` changes if a column is renamed, removed or changes meaning.

`tools/aggregate_results.py` merges these files from many results folders / zips into
`all_results.csv`, `all_samples.csv` and `all_wave_probe.csv` (see its `--help`).

## Keys

- `run_id`: `<yyyymmdd>T<hhmmss>.<ms>_<computer>` (local start time): one per GpuSort.exe run, the
  same in all three files of the run.
- A results row is identified by `run_id, gpu_index, wave_size, algorithm, flush_mode, workload,
  sweep_size`; `samples.csv` rows join on the same columns (`sweep_size` is filled for the sweep
  workload in both). `algorithm_id` (the full name from algorithms.txt) is also unique per run.

## `<stem>.csv` (results)

| column | meaning |
|---|---|
| schema_version | 1 |
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
| flush_mode | `full` (the measurement of record), `full_legacy`, `full_ro`, `code`, `data`, `none` (see GpuSort.exe --help) |
| workload | `mostly_empty`, `mostly_small`, `realistic_mix`, `mostly_large`, `worst_case`, `edges`, `mostly_mid`, `mostly_medium`, `sweep`, `sparse_keys` |
| sweep_size | sweep workload: the sort size of this row's iterations (all 20 sorts of an iteration have it); empty otherwise |
| sorts_per_iteration | 20 |
| iterations | measured iterations the statistics are over (sweep: those of this size) |
| warmup | warmup iterations per algorithm x workload (not in the statistics) |
| min_us, median_us, mean_us, p95_us, p99_us, max_us | GPU time of one iteration (all 20 sorts), microseconds; p95 / p99 nearest rank |
| stddev_us | sample standard deviation (n - 1) |
| verify_failures | iterations whose output was not sorted correctly (warmup included; sweep: of this size) |
| total_elements_per_iter_mean | mean elements per iteration (sum of the 20 sort sizes) |
| algorithm_id | full algorithm name from algorithms.txt, e.g. `s4_3tier_rx3r@data` |
| run_kind | `benchmark`, `smoke` (`--smoke`: few iterations, one in flight, a safety check, not a measurement) |
| iterations_requested | `--iterations` of the run |
| wave_probe_ok | 1 = the wave probe found the configuration the shaders were compiled for, 0 = WAVE PROBE WARNING, empty = the probe did not run |
| gpu_error | non-empty if the GPU's run aborted (e.g. `DEVICE LOST: ...`); the rows hold the partial results |

## `<stem>_samples.csv`

| column | meaning |
|---|---|
| run_id, gpu_index, wave_size, algorithm, flush_mode, workload | join keys (as in the results CSV) |
| iteration | measured iteration index, 0-based (the data is deterministic per workload x iteration) |
| time_us | GPU time of this iteration (all 20 sorts), microseconds |
| largest_sort | largest of the iteration's 20 sort sizes |
| total_elements | sum of the iteration's 20 sort sizes |
| sweep_size | sweep workload: the sort size (= largest_sort); empty otherwise |

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
