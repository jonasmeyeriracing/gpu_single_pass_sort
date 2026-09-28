GpuSort portable benchmark package
==================================

Runs the GpuSort DX12 sort benchmark on this machine. Nothing needs to be installed: no Visual
Studio, Windows SDK, VC++ redistributable or git. Needs Windows 10/11 x64 and a GPU driver with
D3D12, shader model 6.6 and wave ops (any AMD RDNA, NVIDIA Turing or newer, Intel Arc).

How to run
----------
1. Unzip the whole package into a folder (for example the Desktop). Do not run it from inside the
   zip view in Explorer.
2. Close games and other GPU-heavy programs.
3. Double-click run_all.bat. (Only the wave probe, a few seconds: open a command prompt in this
   folder and run "run_all.bat probe", see "Modes" below.)
4. Every GPU run first shows an OK/Cancel box titled "... Run k/N: <kind> <shader set> <wave>".
   Press OK to start that run. Cancel stops the script (the results so far are kept).
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
end), wave_probe.log (console output) and summary.txt (plan, exit code, the probe's Summary), and
is zipped as usual. Exit code 0 = every configuration OK, 1 = a warning or error, 3 = device lost.

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
<set> is "current" (shaders\) or "passN" (_test\passN\). No _waveNN = default wave size.
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
    GpuSort.exe --wave-probe                     (only the wave probe, every wave configuration of
                                                  every GPU; one prompt; what "run_all.bat probe" runs)
    GpuSort.exe --wave-probe --wave-size 64      (only the wave probe of that configuration; still asks)
The exe finds shaders\ next to itself; --shaders selects a snapshot.
