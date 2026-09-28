# pass0: baseline LDS bitonic sort

## What it is

`pass0_bitonic` is the reference point for later passes. It has one algorithm with one dispatch:

- `[numthreads(1024,1,1)]`, one group per sort (`SV_GroupID.x` = sort index), dispatched with
  `ExecuteIndirect` and args `{20,1,1}`.
- The whole sort is loaded into `groupshared uint[8192]` (32 KB, the full D3D12 limit), padded to the
  next power of two with `0xFFFFFFFF`.
- Classic bitonic network: `log2(n)*(log2(n)+1)/2` compare-exchange stages, each followed by
  `GroupMemoryBarrierWithGroupSync`. That is 91 stages for 8192 elements and 78 for 4096. Each thread
  handles `n/2/1024` pairs per stage.
- It compares full 32-bit values. The key is in the high 16 bits, so this also yields key order.
- Only `count` elements are written back. Empty sorts and group indices past `numSorts` exit early.

To re-run this snapshot: `GpuSort.exe --shaders _test/pass0` (the directory contains `algorithms.txt`,
`common.hlsli`, `pass0_bitonic.hlsl`).

## Results (1000 iterations + 5 warmup, 20 sorts/iteration, µs per iteration, see results.txt)

| workload      | 5080 median | 5080 p95 | 2060 median | 2060 p95 |
|---------------|------------:|---------:|------------:|---------:|
| mostly_empty  |        4.26 |     5.12 |        8.19 |    10.27 |
| mostly_small  |        4.80 |     5.70 |        9.09 |    10.05 |
| realistic_mix |       13.18 |    43.68 |       23.44 |    65.73 |
| mostly_large  |       44.16 |    45.02 |       89.34 |    96.26 |
| worst_case    |       44.86 |    45.66 |       91.36 |    98.18 |

0 verification failures anywhere. Wall time was 3.8 s on the 5080 and 10.9 s on the 2060, about 15 s
in total. results.txt still says "Total run time 18.7 s" because that build started the clock before
the confirmation popup; that number includes about 4 s spent waiting for OK. This is fixed now. The 256 MB cache flush dominates: about 0.7 ms per iteration on the 5080 and about 2 ms on
the 2060.

## Observations

- **The largest sort sets the latency.** mostly_large is almost identical to worst_case because
  nearly every iteration has at least one sort above 4096. That sort pads to 8192 and runs the full
  91-stage network, and the other 19 groups finish in its shadow. realistic_mix is bimodal for the
  same reason: the median is 13 µs, but p95 is 44 µs on the 5080, the same as worst_case, whenever one
  of the rare 2049–8192 sorts shows up (about 3% of sorts, so about 45% of iterations contain one).
- **The floor is high.** mostly_empty (at most 64 elements) still costs about 4.3 µs on the 5080 and
  about 8 µs on the 2060, with a minimum of 2.1 µs and 4.8 µs. That is fixed overhead: the dispatch
  itself, launching 1024-thread groups, the descriptor read and the global load latency. For small
  sorts 1024 threads is far too many; mostly_small (at most 128) is barely slower than mostly_empty.
- **The GPU is barely used.** Only 20 groups run, one per SM, on an 84-SM 5080 and a 30-SM 2060. For
  large sorts the cost is the serial chain of 91 barriers × LDS latency, not throughput. The 2060 is
  about 2× slower than the 5080 on the large workloads, which roughly tracks clock × per-SM LDS and
  barrier latency, not SM count.
- **Outliers.** max is occasionally 250–530 µs on the 5080 and about 7 ms on the 2060 (3 of the 5
  2060 workloads each had one). These are single iterations, most likely OS/WDDM scheduling or other
  desktop GPU work (the machine was in use). The mean is skewed by them, so compare medians and p95.
- Ideas for later passes follow directly from this: size tiers (small groups for small sorts), fewer
  barriers (sort in registers or across waves for small strides, as the notes suggest), and rank sort
  for the mid tier.

## Framework notes discovered in this pass

- DXGI lists the "Parsec Virtual Display Adapter" as a second "NVIDIA GeForce RTX 5080" with its own
  LUID, and D3D12 device creation succeeds on it. The framework now asks D3DKMT for the adapter type
  and skips indirect-display and non-render adapters (`--list-adapters` shows the details).
- WARP on this OS (10.0.26100) supports SM 6.6, so `--warp` works for correctness debugging.
- Verification was checked with deliberately broken shader variants: skipping the last merge level,
  writing one extra element into padding, writing nothing, flipping a bit, and leaving a gap between
  tier dispatches. All were detected.
