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
3. Double-click run_all.bat.
4. Every GPU run first shows an OK/Cancel box titled "... Run k/N: <kind> <shader set> <wave>".
   Press OK to start that run. Cancel stops the script (the results so far are kept).
   While a run is active a small "GpuSort benchmark running" window shows its progress. Please
   leave the PC alone until it closes.
5. At the end the script prints the results folder, results\<COMPUTERNAME>_<yyyymmdd_hhmm>\,
   and zips it to results\<COMPUTERNAME>_<yyyymmdd_hhmm>.zip. Bring back the zip (or the folder).

Options (from a command prompt in this folder):
    run_all.bat            current shaders\ + the latest _test\passN snapshot that differs from
                           shaders\ (default)
    run_all.bat all        current shaders\ + every _test\passN snapshot
    run_all.bat current    current shaders\ only
A snapshot whose shaders are identical to shaders\ is skipped (package_info.txt says which).
Typically the newest snapshot is the current pass itself, so the default compares the current
shaders with the previous pass.

What it runs
------------
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
--wave-size 64 (only on the GPUs that support 64). Expect roughly 1-3 minutes per full run on a
fast GPU, longer on an integrated GPU.

Results folder contents
-----------------------
    summary.txt                 plan, every run with its exit code, final status
    package_info.txt            which build / git commit this package is
    adapters.txt                --list-adapters output (vendor/device ids, driver, wave lane ranges)
    dxdiag.txt                  dxdiag report
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
The exe finds shaders\ next to itself; --shaders selects a snapshot.
