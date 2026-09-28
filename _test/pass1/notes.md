# pass1: size tiers + rank sort

## What changed

- **`rank_sort.hlsl`** (new): one group per sort for sorts of `MIN_COUNT..MAX_COUNT` elements. Each element
  gets a unique 32-bit word `(key16 << 16) | localIndex` in groupshared memory. Its output position is the
  number of words strictly less than its own, so it needs one compare per pair and no tie-break, and it is
  stable. All lanes read the same LDS address (a broadcast), 8 words per loop step, and each loaded word is
  compared against all `ELEMS_PER_THREAD = ceil(MAX_COUNT / GROUP_SIZE)` elements the thread owns. There
  is one barrier. After the crash (see below) the groupshared array became a plain `groupshared uint[]`
  instead of `uint4[]` with dynamic component stores. The DXIL was already scalar i32 stores before the
  change, so this changes no code; it only makes the source say what the compiled shader does.
- **`algorithms.txt`**: 5 tiered algorithms. The large tier is the unchanged pass0 bitonic shader,
  limited with `MIN_COUNT`. Every tier is one `ExecuteIndirect`, issued back to back with no barriers.

  | name | dispatches (group size: count range) |
  |---|---|
  | `pass0_bitonic` | 1024: 1-8192 (pass0, re-measured in this run) |
  | `t2` = `t2_rank512_bitonic` | rank 512: 1-512, bitonic 1024: 513-8192 |
  | `t3_128` = `t3_rank128_rank512_bitonic` | rank 128: 1-128, rank 512: 129-512, bitonic: 513+ |
  | `t3_64` = `t3_rank64_rank512_bitonic` | rank 64: 1-64, rank 512: 65-512, bitonic: 513+ |
  | `t4_1024` = `t4_rank128_rank512_rank1024_bitonic` | rank 128 / 512 / rank 1024 (1 elem/thread): 513-1024, bitonic: 1025+ |
  | `t4_1024x2` = `t4_rank128_rank512_rank1024x2_bitonic` | rank 128 / 512 / rank 1024 (2 elems/thread): 513-2048, bitonic: 2049+ |

- **Workloads** (appended, so the old ids and seeds are unchanged): `edges` (30 fixed sizes around every
  tier boundary, 0/1/2/3/31/32/33/.../8191/8192, cycled) and `mostly_mid` (uniform 513-2048).

## The crash, and what changed in the framework

The first attempt at this pass ran the full benchmark at 10:54. From 10:54:51 the NVIDIA driver logged
231 × "Error occurred on GPUID:100" (nvlddmkm event 153, the RTX 5080). A TDR followed at 10:55:00,
"Resetting TDR" at 10:55:13, and then the machine bugchecked with **0x119 VIDEO_SCHEDULER_INTERNAL_ERROR
(2, c000000d)** ("driver failed upon the submission of a command") and rebooted. The console output was
lost. Which combination was running is unknown.

**Root cause: not found.** What was checked:

- **WARP with the debug layer + GPU-based validation**, all 6 algorithms × 7 workloads, together and each
  algorithm on its own (`--algo`), 5 iterations, plus 30 iterations without GBV and a `--smoke` run:
  0 verification failures, 0 debug-layer or GBV messages.
- **Code review of `rank_sort.hlsl`**: every groupshared index is below `PADDED_MAX_COUNT`. Stores cover
  `[0, count)` and the pad covers `[count, paddedCount)`, all written before the barrier. Reads cover
  `[0, paddedCount)`. The early returns before the barrier are group-uniform. Global reads are
  `offset + i` with `i < count`. The largest possible rank is `count - 1`, because all words are unique
  and the pad words (`0xFFFFFFFF`) are greater than every real word. Nothing is out of bounds.
- **DXIL** (`dxc -Fc`, all 7 tier configurations): the old `gsWords[i >> 2][i & 3] = x` on
  `groupshared uint4[]` already compiled to a flat `[N x i32]` array with single scalar i32 stores. There
  was no read-modify-write and no race.
- **Code review of the multi-dispatch path**: every dispatch uses the same root signature, so root
  arguments persist across `SetPipelineState`. The indirect args are a single static `{20,1,1}`. Query
  indices are `2s` and `2s+1` for `s < 32`, and the heap has 64 queries. Upload, readback and query
  slots are sized per iteration, not per workload, so more workloads cannot overflow them. The tiers
  write disjoint output ranges, so running them concurrently without barriers is safe.
- **Deliberately broken shaders** (in a scratch directory, not committed): out-of-bounds write, out-of-bounds
  read, and a stray write past the last sort. GBV reports none of these; it validates descriptors and
  resource states, not buffer bounds. The CPU verification catches all of them.

**What the old framework did wrong regardless of the cause:** a removed device reports every fence as
complete (`UINT64_MAX`). The old `WaitForFence` returned immediately in that case and only called
`GetDeviceRemovedReason()` while it was actually blocked. So after a fault the program kept recording and
submitting batch after batch. The 231 driver errors came *before* the TDR, so the GPU kept taking
submissions while it was raising errors, and the bugcheck happened during TDR recovery. Also, the buffers
were bound as root descriptors, which have no bounds checking, so any out-of-bounds access would have been
a real page fault. Context: this machine's WER history contains many older GPU watchdog / TDR events and
bugchecks from before this project (0x141, 0x117, 0x1b0, 0x1b8, 0x3b between May and September). A
driver or system problem is a real possibility. The kernel minidump needs admin rights to read, so it was
not analysed.

**Hardening** (commit 135032f, now permanent):

- t0/t1/u0 are bound through a descriptor table: a shader-visible heap with StructuredBuffer SRV/UAV views
  of exact size (20 descriptors, 163840 elements, and the 256 MB flush buffer). Out-of-bounds reads return
  0 and out-of-bounds writes are dropped; this was checked on WARP. The HLSL is unchanged.
- Every fence wait has a timeout (10 s on hardware) and checks `GetDeviceRemovedReason()` after the
  wait. On removal or timeout the program stops submitting immediately, prints the reason and the DRED
  report, writes partial results and exits with code 3. It intentionally leaks the benchmark's
  resources, because WARP was seen to keep executing after `RemoveDevice`, and freeing the buffers then
  crashed it.
- `--dred`: auto-breadcrumbs, breadcrumb contexts (a marker per upload/flush/sort-dispatch/readback),
  page-fault VA plus allocation names, named objects, and logged buffer VA ranges. `--gbv`: GPU-based
  validation. `--smoke`: 3 iterations, one in flight, "starting gpu/algo/workload" log lines, stops at
  the first verification failure. `--test-device-removal`: exercises the device-lost path on WARP.
- The whole output buffer is poisoned and read back every iteration, and nothing past the last sort
  may change.

After the hardening, `--smoke --dred` on both GPUs was clean (84 combos, 0 failures, no driver events),
and so was this full run (0 failures, 0 nvlddmkm events). The original fault never came back. That fits
a transient driver or system problem, or a problem in the old binding/submission path, but proves
neither.

## Results (1000 iterations + 5 warmup, 20 sorts/iteration, µs per iteration, median / p95; see results.txt)

**Timing caveat:** pass1 binds buffers through descriptor tables, while pass0 used root descriptors.
`pass0_bitonic` was re-measured in this run (column "pass0"). Compare the tiers against that column,
not against the old "pass0 run" numbers. On the 5080 the same shader is now about 1-1.6 µs slower than
in the pass0 run (4.26 → 5.22 on mostly_empty, 44.2 → 45.8 on mostly_large). On the 2060 it is within
about 0.3 µs, except realistic_mix (+1.1). That may be the binding change: bounds-checked typed views
instead of raw root VAs, and the descriptor fetch. It may also be run-to-run or clock variance. This run
cannot tell the two apart. Best median per row in bold.

**RTX 5080**

| workload | pass0 run | pass0 | t2 | t3_128 | t3_64 | t4_1024 | t4_1024x2 |
|---|---:|---:|---:|---:|---:|---:|---:|
| mostly_empty | 4.26 / 5.12 | 5.22 / 5.92 | **4.22 / 5.73** | 4.32 / 6.43 | 4.42 / 6.53 | 4.48 / 6.27 | 4.45 / 6.30 |
| mostly_small | 4.80 / 5.70 | 5.82 / 6.50 | **4.19 / 5.92** | 4.35 / 6.37 | 5.09 / 6.43 | 4.48 / 6.34 | 4.54 / 6.46 |
| realistic_mix | 13.18 / 43.68 | **13.98 / 45.41** | 16.03 / 46.53 | 16.43 / 46.66 | 16.50 / 46.56 | 16.62 / 46.94 | 30.85 / 46.75 |
| mostly_large | 44.16 / 45.02 | **45.79 / 46.62** | 46.72 / 48.22 | 46.82 / 48.48 | 46.78 / 48.48 | 47.01 / 48.70 | 47.04 / 48.74 |
| worst_case | 44.86 / 45.66 | **46.37 / 47.07** | 47.33 / 48.61 | 47.46 / 49.09 | 47.42 / 48.86 | 47.68 / 49.02 | 47.58 / 48.99 |
| edges | - | **45.39 / 46.34** | 46.11 / 47.74 | 46.24 / 47.94 | 46.27 / 48.03 | 46.40 / 48.00 | 46.34 / 48.19 |
| mostly_mid | - | **13.66 / 14.34** | 14.88 / 16.58 | 15.04 / 17.09 | 15.04 / 16.93 | 15.20 / 17.34 | 32.80 / 35.14 |

**RTX 2060**

| workload | pass0 run | pass0 | t2 | t3_128 | t3_64 | t4_1024 | t4_1024x2 |
|---|---:|---:|---:|---:|---:|---:|---:|
| mostly_empty | 8.19 / 10.27 | 8.26 / 10.59 | **5.81 / 6.14** | 5.95 / 6.14 | 5.92 / 6.14 | 6.43 / 7.49 | 6.46 / 7.46 |
| mostly_small | 9.09 / 10.05 | 9.44 / 10.24 | **6.08 / 6.14** | 6.21 / 7.26 | 6.66 / 7.68 | 6.85 / 7.84 | 6.78 / 7.78 |
| realistic_mix | 23.44 / 65.73 | **24.53 / 66.88** | 25.54 / 67.58 | 24.58 / 68.93 | 24.86 / 67.49 | 26.58 / 67.90 | 69.15 / 85.86 |
| mostly_large | 89.34 / 96.26 | 89.47 / 95.55 | **89.06 / 95.30** | 90.18 / 96.51 | 90.11 / 96.26 | 90.30 / 96.51 | 90.11 / 96.42 |
| worst_case | 91.36 / 98.18 | 91.39 / 97.50 | **91.38 / 97.70** | 92.26 / 99.10 | 92.22 / 99.04 | 92.16 / 98.85 | 92.16 / 98.62 |
| edges | - | **65.28 / 71.01** | 65.50 / 71.55 | 65.94 / 77.82 | 65.95 / 71.87 | 71.81 / 89.76 | 86.05 / 92.16 |
| mostly_mid | - | **24.06 / 29.50** | 24.51 / 30.27 | 24.90 / 30.78 | 24.90 / 30.72 | 26.37 / 32.38 | 85.70 / 90.85 |

0 verification failures anywhere. Wall time was 30.9 s on the 5080 and 90.2 s on the 2060. Max
outliers are still about 170-570 µs on the 5080 and about 3 ms a few times on the 2060
(OS/desktop noise, as in pass0).

## Conclusions

- **Small sorts: the 512-thread rank tier clearly pays off.** Compared with pass0_bitonic in the same
  run: mostly_empty 5.22 → 4.22 µs (-19%) and mostly_small 5.82 → 4.19 µs (-28%) on the 5080, and
  8.26 → 5.81 (-30%) and 9.44 → 6.08 (-36%) on the 2060. The 2060's p95 also tightens a lot
  (10.6 → 6.1).
- **The smallest tier (64 or 128 threads) does not pay off.** t3_128 and t3_64 are never faster than t2,
  not even on mostly_empty (every sort ≤ 64) or mostly_small (every sort ≤ 128). t3_64 is worse on
  mostly_small (5.09 vs 4.19 on the 5080), because every iteration there has real work in both of its
  rank dispatches. What costs time is the extra dispatch, not the group size: each additional
  ExecuteIndirect, even an empty one, adds about 0.1-0.3 µs to small workloads.
- **Extra dispatches also cost the large tier.** On the 5080, every tiered variant is about 1 µs slower
  than pass0_bitonic on mostly_large, worst_case, edges and mostly_mid. In mostly_mid the rank dispatches
  get no work at all (every sort is 513-2048), yet t2 is 13.66 → 14.88. So an empty dispatch in front
  of the bitonic dispatch delays the bitonic groups by about 1.2 µs; the back-to-back dispatches do not
  fully overlap. On the 2060 the penalty is 0-0.9 µs.
- **Rank vs bitonic crossover:**
  - ≤ 128 elements: rank sort in a 512-thread group beats the 1024-thread bitonic by a wide margin.
  - 513-1024 elements at 1 element per thread (t4_1024 vs t3_128, mostly_mid): about the same on the 5080
    (15.20 vs 15.04) and worse on the 2060 (26.37 vs 24.90).
  - 513-2048 elements at 2 elements per thread: far worse (32.8 vs 15.0 µs on the 5080, 85.7 vs 24.9 on
    the 2060). O(n²) wins nowhere near 2k.
  - So the crossover is at or below about 1k on the 5080, and lower on the 2060.
  - The 129-512 band is not isolated by any workload. realistic_mix, the only workload with many sorts in
    that band, has its median set by the 513-2048 sorts, which about 88% of iterations contain.
- **realistic_mix gets worse with tiers** (13.98 → 16.03 on the 5080). The median iteration contains a
  mid-size bitonic sort, and that critical path pays the empty/early dispatch delay described above.
  p95 (about 45-47 µs) is still set entirely by the rare 2049-8192 bitonic sort.
- **The biggest lever is still the large bitonic sort.** It sets the latency of every workload with a sort
  above 2k (about 46 µs on the 5080, 90 µs on the 2060), and nothing in pass1 changed it.

## Surprises

- The empty-dispatch penalty on the 5080 (about 1.2 µs before the bitonic dispatch) is larger than
  expected. Four ExecuteIndirects cost clearly more than one or two.
- The 2060 gains more from the small tier than the 5080 in relative terms (-30 to -36%).
- pass0_bitonic itself measured about 1 µs slower on the 5080 than in the pass0 run (see the timing
  caveat).

## Suggested next steps

1. **Faster large tier** (the main lever): bitonic with several elements per thread in registers,
   in-register compare-exchange for small strides, wave shuffles within a wave, and LDS plus barriers only
   for strides ≥ wave × elements. That cuts the 91 barrier stages drastically. Also try rank or merge
   hybrids for 513-2048.
2. **Fewer dispatches**: 2 tiers at most (t2 style). Also try a single 1024-thread dispatch that
   branches per sort size and runs small sorts on only the first wave(s) or a 512-thread subset. That
   would remove the inter-dispatch delay entirely.
3. **Faster rank sort**: pack two 16-bit keys per compare, or use `WaveReadLaneAt` broadcasts instead of LDS
   reads. Add a workload of uniform 129-512 to isolate the mid band and find the real crossover.
4. To settle the binding cost, add a `--root-descriptors` A/B switch (unsafe mode), or accept about 1 µs as
   the price of bounds checking.
5. Keep running `--smoke --dred` before every full hardware run whenever a new shader is added.

To re-run this snapshot: `GpuSort.exe --shaders _test/pass1`.
