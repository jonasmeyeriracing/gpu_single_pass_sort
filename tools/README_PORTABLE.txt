GpuSort portable benchmark package
==================================

Runs the GpuSort DX12 sort benchmark on this machine. Nothing needs to be installed: no Visual
Studio, Windows SDK, VC++ redistributable or git. Needs Windows 10/11 x64 and a GPU driver with
D3D12, shader model 6.6 and wave ops (any AMD RDNA, NVIDIA Turing or newer, Intel Arc).

The two runs to do now on the AMD and the Intel machine
-------------------------------------------------------
Please run these two, one after the other, from a command prompt in this folder (see "How to run"
for unzipping and the OK/Cancel boxes), and bring back both results zips (one per run):
    run_all.bat diag       the flush diagnostic (see "Flush diagnostic" below). AMD machine
                           (RX 7900 XTX + Ryzen iGPU): about 5-6 min in total (both runs, if
                           Developer Mode is on). Intel UHD 770 machine: about 2-3 min per run.
    run_all.bat pass7      the low-end / iGPU shader set (see "Low-end shader set" below).
                           AMD machine: about 1 min on the RX 7900 XTX + 10.5 min on the Ryzen
                           iGPU, then the optional iGPU wave64 run (another ~10.5 min), plus the
                           smoke tests before them (~2.5 min): ~25 min in total with wave64.
                           Intel UHD 770 machine: about 8 min + ~1 min smoke test.
With "noprompt" there are no OK/Cancel boxes (e.g. "run_all.bat pass7 noprompt"; see "No popups").
To skip the optional wave64 run on the AMD machine, run without noprompt and press Cancel in the
last box ("4/4: full pass7 wave64, integrated GPU only"); everything before it is kept (summary.txt
then says "cancelled at the prompt", which is fine).

How to run
----------
1. Unzip the whole package into a folder (for example the Desktop). Do not run it from inside the
   zip view in Explorer.
2. Close games and other GPU-heavy programs.
3. Double-click run_all.bat. (Only the wave probe, a few seconds: open a command prompt in this
   folder and run "run_all.bat probe"; the flush diagnostic, a few minutes: "run_all.bat diag";
   the low-end shader set: "run_all.bat pass7"; see "Modes" below.)
4. Every GPU run first shows an OK/Cancel box titled "... Run k/N: <kind> <shader set> <wave>".
   Press OK to start that run. Cancel stops the script (the results so far are kept).
   (No boxes at all: add "noprompt", see "No popups" below.)
   While a run is active a small "GpuSort benchmark running" window shows its progress. Please
   leave the PC alone until it closes.
5. At the end the script prints the results folder, results\<COMPUTERNAME>_<yyyymmdd_hhmm>\,
   and zips it to results\<COMPUTERNAME>_<yyyymmdd_hhmm>.zip. Bring back the zip (or the folder).

Modes (from a command prompt in this folder; run_all.bat prints them when it starts):
    run_all.bat            current shaders\ + the latest _test\passN snapshot that differs from
                           shaders\ (default; same as "run_all.bat latest")
    run_all.bat all        current shaders\ + every _test\passN snapshot
    run_all.bat current    current shaders\ only
    run_all.bat probe      only the wave probe: a few seconds, ONE OK/Cancel box, no sorts
                           (see "Wave probe only" below)
    run_all.bat diag       the flush diagnostic: a few minutes, ONE OK/Cancel box
                           (see "Flush diagnostic" below)
    run_all.bat pass7      the low-end / iGPU shader set _test\pass7: smoke + full run, plus an
                           integrated-GPU-only wave64 smoke + full run on AMD; 2 or 4 OK/Cancel
                           boxes (see "Low-end shader set" below)
    add "noprompt" to any mode for no OK/Cancel boxes (see "No popups" below), e.g.
        run_all.bat diag noprompt
    add "nopause" to any mode to skip the "press any key" at the end.
A snapshot whose shaders are identical to shaders\ is skipped (package_info.txt says which).
Typically the newest snapshot is the current pass itself, so the default compares the current
shaders with the previous pass.

Wave probe only (run_all.bat probe)
-----------------------------------
Runs GpuSort.exe --list-adapters, then a single GpuSort.exe --wave-probe run (one OK/Cancel box
for all GPUs). For every qualifying GPU it runs a tiny diagnostic shader (a few one-group
dispatches, no sorts) compiled without [WaveSize] (the driver's choice) and with [WaveSize(N)] for
every power of two N in the GPU's wave lane range (AMD RDNA 32-64: 32 and 64; Intel 16-32: 16 and
32; NVIDIA 32-32: 32), each for group sizes 64 / 512 / 1024. It checks the lane count the driver
really uses, lane = SV_GroupIndex % lane count and a few cross-lane operations, and prints one
verdict per GPU x wave configuration ("OK" or "WAVE PROBE WARNING"). No dxdiag, smoke or full runs.
The results folder then has adapters.txt, wave_probe.txt (the report, with a Summary section at the
end), wave_probe.csv (the same per GPU x wave configuration, machine-readable), wave_probe.log
(console output) and summary.txt (plan, exit code, the probe's Summary), and is zipped as usual. Exit code 0 = every configuration OK, 1 = a warning or error, 3 = device lost.

Flush diagnostic (run_all.bat diag)
-----------------------------------
Measures how long the effect of the 256 MB cache flush lasts after it (on the RX 7900 XTX the
default measurement still carries ~12 us of it). One shader set: shaders\ with the algorithm list
shaders\algorithms_diag_flush.txt: the reference sort (s1_rank512_bitreg2048_radix) with every
flush variant (spin drains of 2 / 5 / 10 / 20 / 50 / 100 us after the flush, the read-only flush,
code / data / none) and the pass0 bitonic sort as a second shader; 300 iterations of mostly_empty,
realistic_mix and worst_case; default wave size only; no dxdiag and no smoke test (the sort shaders
are the already tested ones; the new spin drain was tested on WARP with GPU-based validation and
on an RTX 5080).
  Run 1 (normal clocks) -> diag_flush.txt / .csv / .log.
  Run 2 (only if Windows Developer Mode is on): the same with --stable-power (the driver's fixed
    "stable power" clocks, ID3D12Device::SetStablePowerState) -> diag_flush_stable.txt / .csv / .log.
    It starts right after run 1 WITHOUT another OK/Cancel box (run 1's box says so), and only if
    run 1 finished without errors. Without Developer Mode that call removes the D3D12 device
    (the run would fail), so the script skips run 2 then (summary.txt says so); nothing needs to
    be changed on the machine for run 1.
Expected time: under a minute per run on a discrete GPU, 2-3 minutes per run on an integrated GPU
(both GPUs of a machine run in every run); GpuSort prints a calibrated estimate after the OK.

Low-end shader set (run_all.bat pass7)
--------------------------------------
The shader set _test\pass7 (its notes.md explains every variant): 21 sort variants in smaller,
tier-sized thread groups for integrated GPUs (Ryzen iGPU, Intel UHD 770), plus the portable
reference. Only this shader set, no other; dxdiag.txt is written first as usual.
  1. Smoke test: GpuSort.exe --smoke --dred --shaders _test\pass7 --algo-file algorithms_smoke.txt
     --iterations 16 on every GPU (the 21 variants + 3 validation variants) -> pass7_smoke.txt.
  2. Only on a machine with an integrated GPU that reports a wave size range (the Ryzen iGPU:
     32-64): the same smoke test with --wave-size 64 --integrated-only (the iGPU only, not the RX
     7900 XTX) -> pass7_smoke_igpu_wave64.txt.
  3. Full run: GpuSort.exe --shaders _test\pass7 --iterations 300, default wave size, every GPU
     -> pass7.txt.
  4. Only with step 2: the full run with --wave-size 64 --integrated-only -> pass7_igpu_wave64.txt.
Every run has its own OK/Cancel box (none with noprompt). The same stop rules as the default modes:
the full runs only start if every smoke test passed, a device loss stops at once. The script prints
the plan with an estimated time per run before the first box (AMD machine: ~25 min in total,
Intel UHD 770: ~9 min).

No popups (noprompt)
--------------------
"run_all.bat <mode> noprompt" (any mode) passes --no-prompt to every GpuSort.exe run: no OK/Cancel
boxes. Instead the script prints "No popups: make sure other GPU work is paused" once and counts
down 10 seconds before the first GPU run; press Ctrl+C (then Y) to abort. The progress window still
shows while a run is active. Use it when you start the runs by hand at the machine.

What it runs (default, all, current)
------------------------------------
1. GpuSort.exe --list-adapters: every adapter, and which ones a benchmark run uses.
2. dxdiag /t: driver details (dxdiag.txt).
3. Smoke tests: GpuSort.exe --smoke --dred --iterations 16 for every shader set: 16 iterations
   (enough for the sweep workload to reach each of its 16 sort sizes once) of every algorithm x
   workload, one at a time, with DRED fault reporting. An algorithm that fails verification
   skips its remaining workloads, the other algorithms and the remaining smoke tests still run,
   and summary.txt lists the failed algorithms ("SMOKE FAILED: ..."); the script then stops before
   the long runs. A device loss (GPU fault or hang) stops the script at once.
4. Full runs: 1000 iterations (+5 warmup) of every algorithm x workload for every shader set.
If a GPU reports a range of wave sizes (AMD RDNA: WaveLaneCountMin 32, Max 64), steps 3 and 4 run
twice per shader set: with the default (WAVE_SIZE = 32, forced with [WaveSize(32)]) and with
--wave-size 64 (only on the GPUs that support 64). Expect roughly 2 minutes per full run on a
fast discrete GPU and 20-25 minutes on an integrated GPU (measured: Ryzen 7000 iGPU 24 min, Intel
UHD 770 19 min per full run; a machine with a discrete and an integrated GPU runs both in every
run). Every GPU run prints a calibrated estimate of its run time right after the OK (console and
progress window, refined while it runs; also in the header of the results .txt).
Every GPU run (after its OK) first runs a wave probe: a tiny shader, compiled like the sort shaders
of that run and also without [WaveSize], reports the wave lane count the driver really uses, checks
lane = SV_GroupIndex % WAVE_SIZE and a few cross-lane operations (WaveReadLaneAt lane ^ 32, ...).
The report is in the header of every results .txt ("Wave probe: ..."). If the lane count or the
lane mapping is not what the shaders were compiled for, it prints "WAVE PROBE WARNING", and a smoke
test marks the algorithms that use wave ops as "SMOKE FAILED ... (not run: wave probe: ...)"
instead of running them. No extra prompt: there is no separate probe step in these modes (for the
probe of every wave configuration on its own, use "run_all.bat probe").

Results folder contents
-----------------------
    summary.txt                 plan, every run with its exit code, final status
    package_info.txt            which build / git commit this package is
    adapters.txt                --list-adapters output (vendor/device ids, driver, wave lane ranges)
    dxdiag.txt                  dxdiag report
    wave_probe.txt              the wave probe lines of every run (see "What it runs"); in probe
                                mode the --wave-probe report (+ wave_probe.log)
    smoke_<set>[_waveNN].txt    smoke test results; .log = full console output
    <set>[_waveNN].txt          benchmark results (timings per GPU x workload x algorithm);
                                .log = full console output
    <run>.csv                   next to every <run>.txt above (smoke and full): the same results
                                machine-readable, one row per GPU x algorithm x workload (x sort
                                size for the sweep workload)
    <run>_samples.csv           every measured iteration's time (the largest file, ~15 MB per
                                full run and GPU; about 1.2 MB of that in the zip)
    <run>_wave_probe.csv        the run's wave probe, one row per GPU x wave configuration
    wave_probe.csv              probe mode only: the --wave-probe report as CSV
    diag_flush[_stable].txt     diag mode only: the flush diagnostic (+ .csv, _samples.csv,
                                _wave_probe.csv, .log); _stable = the --stable-power run
    pass7_smoke[_igpu_wave64].txt, pass7[_igpu_wave64].txt
                                pass7 mode only: smoke and full runs of _test\pass7 (+ .csv,
                                _samples.csv, _wave_probe.csv, .log); _igpu_wave64 = the
                                integrated GPU at --wave-size 64
<set> is "current" (shaders\) or "passN" (_test\passN\). No _waveNN = default wave size.
The CSV columns are described in CSV_FORMAT.md; tools\aggregate_results.py (in the repository)
merges the CSVs of the zips from all machines.
Each results .txt header lists the computer name, every GPU with driver version, vendor/device id,
VRAM, wave lane range and the WAVE_SIZE the shaders were compiled with.

Adapters
--------
Every hardware GPU that qualifies is benchmarked, one after the other; results show them as [0],
[1], ... in "high performance first" order. Typically:
  - AMD Radeon RX 7900 XTX (vendor 1002): the discrete GPU. Wave lanes 32-64.
  - "AMD Radeon(TM) Graphics" (vendor 1002, small VRAM): the integrated GPU of a Ryzen 7000/9000
    CPU. It qualifies too and is benchmarked as a separate GPU (slower; not the main target).
  - Microsoft Basic Render Driver (WARP, software): always skipped.
  - Indirect display adapters (Parsec, remote desktop virtual displays): skipped; they show up
    under the name of the real GPU but are not a separate GPU.
adapters.txt lists what was used ("Benchmark adapters") and what was skipped and why.

GpuSort exit codes (in summary.txt)
-----------------------------------
    0  ok
    1  verification failures or errors (details in the run's .log)
    2  cancelled at the prompt
    3  device lost (GPU fault or hang; the .log has a DRED report)
A device loss or Cancel stops the script. After a device loss a reboot is a good idea. Exit code 1
from a smoke test lets the other smoke tests run, but no full run is started.

Running GpuSort.exe directly
----------------------------
    GpuSort.exe --help
    GpuSort.exe --shaders _test\pass2 --wave-size 64 --out my_results.txt
                                                 (also writes my_results.csv, my_results_samples.csv
                                                  and my_results_wave_probe.csv; --no-samples skips
                                                  the samples file)
    GpuSort.exe --wave-probe                     (only the wave probe, every wave configuration of
                                                  every GPU; one prompt; what "run_all.bat probe" runs)
    GpuSort.exe --wave-probe --wave-size 64      (only the wave probe of that configuration; still asks)
    GpuSort.exe --algo-file algorithms_diag_flush.txt --iterations 300
                                                 (another algorithm list in the shader folder)
    GpuSort.exe --stable-power                   (fixed clocks; needs Windows Developer Mode, else it
                                                  runs with normal clocks and says so)
    GpuSort.exe --integrated-only --wave-size 64 (only the integrated GPU(s), e.g. the Ryzen iGPU;
                                                  --list-adapters shows "integrated yes" for them)
The exe finds shaders\ next to itself; --shaders selects a snapshot.
