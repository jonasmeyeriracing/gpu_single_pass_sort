@echo off
rem GpuSort portable runner: runs the benchmark on this machine and collects everything in
rem results\<COMPUTERNAME>_<yyyymmdd_hhmm>\ (plus a .zip of it) next to this script.
rem
rem   run_all.bat            current shaders\ + the latest _test\passN snapshot that differs from
rem                          shaders\ (the packager marks identical ones with SAME_AS_SHADERS.txt)
rem   run_all.bat all        current shaders\ + every _test\passN snapshot (identical ones skipped)
rem   run_all.bat current    current shaders\ only
rem   run_all.bat probe      only the wave probe: --list-adapters, then ONE GpuSort.exe --wave-probe
rem                          run (one prompt, a few seconds, no sorts) that probes every GPU without
rem                          [WaveSize] and with [WaveSize(N)] for every N in its wave lane range;
rem                          report in wave_probe.txt (+ wave_probe.log). No dxdiag, smoke or full runs.
rem   run_all.bat diag       flush-tail diagnostic (pass6): current shaders\ with the algorithm list
rem                          shaders\algorithms_diag_flush.txt (the reference sort with every drain /
rem                          flush variant), 300 iterations of mostly_empty, realistic_mix and
rem                          worst_case, default wave size only, results in diag_flush.txt (+ .csv).
rem                          If Windows Developer Mode is on, a second run with --stable-power (fixed
rem                          clocks) follows right after it, without another prompt (diag_flush_stable).
rem                          ONE prompt in total; no dxdiag and no smoke run (the sort shaders are the
rem                          already validated pass5 ones; the new spin drain was validated on WARP
rem                          with GPU-based validation and on an RTX 5080). A few minutes on a discrete
rem                          + integrated GPU machine.
rem   run_all.bat pass7      the low-end / iGPU shader set _test\pass7 (see its notes.md): a smoke run
rem                          (--smoke --dred, algorithm list algorithms_smoke.txt, 16 iterations), then
rem                          the full run (algorithms.txt, 300 iterations), default wave size, every
rem                          GPU; results pass7_smoke.txt / pass7.txt (+ .csv, .log). If an integrated
rem                          GPU reports a wave size range (AMD Ryzen iGPU: 32-64), also a smoke + full
rem                          run at --wave-size <its max> on the integrated GPU(s) only
rem                          (--integrated-only): pass7_smoke_igpu_wave64.txt / pass7_igpu_wave64.txt.
rem                          All smoke runs come first; the same stop rules as the default modes
rem                          (below). Prints the plan with an estimated run time. No other shader set.
rem   run_all.bat final      THE FINAL RUN: current shaders\ with the algorithm list
rem                          shaders\algorithms_final.txt (every distinct algorithm of pass0..pass7,
rem                          see its header), default flush mode full_d50. A smoke run first
rem                          (--smoke --dred, 16 iterations, every GPU, default wave size), then the full
rem                          run (1000 iterations on discrete GPUs, 300 on integrated ones:
rem                          --iterations 1000 --iterations-integrated 300), default wave size, every
rem                          GPU: final_smoke.txt / final.txt. If a DISCRETE GPU reports a wave size range
rem                          (AMD RDNA: 32-64), also a smoke + full run at --wave-size <its max> on the
rem                          discrete GPU(s) only (--discrete-only): final_smoke_wave64.txt /
rem                          final_wave64.txt. Integrated GPUs are left out of that run (the Ryzen iGPU
rem                          is known to be slow and not a shipping configuration at wave64);
rem                          the extra argument igpu64 includes them. Prints the plan with an estimated
rem                          run time per GPU. Stop rules: a device loss or Cancel stops at once; a
rem                          verification failure in the default-wave smoke stops before every full
rem                          run, one in the wave<N> smoke only skips the wave<N> full run.
rem   run_all.bat scale      THE SCALE RUN: current shaders\ with the algorithm list
rem                          shaders\algorithms_scale.txt (19 algorithms of the final set: the large-group
rem                          references and every configuration with smaller groups for small sorts) at
rem                          20, 128, 256 and 512 sorts per iteration (--sorts 20,128,256,512), default
rem                          wave size only, every GPU. A smoke run first (--smoke --dred, 8 iterations
rem                          per combo and sort count), then the full run (1000 / 300 / 200 / 150
rem                          iterations at the four counts on discrete GPUs, 300 / 90 / 60 / 45 on
rem                          integrated ones): scale_smoke.txt / scale.txt. Prints the plan with an
rem                          estimated run time per GPU. Stop rules: a device loss or Cancel stops at
rem                          once; a verification failure in the smoke run stops before the full run.
rem   extra argument igpu64: final mode: also run integrated GPUs with a wave size range at its max
rem   extra argument noprompt: no OK/Cancel popups: passes --no-prompt to every GpuSort.exe run, after
rem                          a 10 s countdown (Ctrl+C aborts). For manual runs at the machine.
rem   extra argument nopause: do not wait for a key at the end
rem
rem Order: adapter list, then a --smoke --dred safety run of every shader set x wave configuration,
rem then the full runs. The smoke runs use --iterations 16 (SMOKE_ITERS) so the sweep workload (which
rem cycles through 16 sort sizes, one per iteration) reaches every size once. A verification failure
rem in a smoke run only takes that algorithm out of the rest of that run (GpuSort keeps testing the
rem other algorithms), and the remaining smoke runs still run; the failed algorithms are listed in
rem summary.txt ("SMOKE FAILED: ..."), and the script then stops before the first full run. A device
rem loss (GPU fault / hang, exit code 3) stops the script at once. Every GPU run
rem shows GpuSort's OK/Cancel prompt ("Run k/N ..."); Cancel stops the script.
rem If a GPU reports a wave size range (e.g. AMD RDNA: 32-64), every shader set also runs with
rem --wave-size <max> (e.g. wave64), on the GPUs that support it.
rem Every GPU run first runs GpuSort's wave probe (the lane count the driver really uses, the lane
rem mapping and a few cross-lane ops, for the run's wave configuration and without [WaveSize]); the
rem script collects those lines in wave_probe.txt, and WAVE PROBE WARNING lines in summary.txt. There
rem is no separate --wave-probe step, so it adds no extra prompt.
rem Every GpuSort run also writes CSV files next to its --out .txt, i.e. into the results folder (and
rem so into the zip): <file>.csv, <file>_samples.csv, <file>_wave_probe.csv (probe mode:
rem wave_probe.csv). Columns: CSV_FORMAT.md; tools\aggregate_results.py merges them.
rem
rem Dry run (prints the GpuSort commands instead of running them; for testing this script):
rem   set DRYRUN=1                       required
rem   set DRYRUN_ADAPTERS=<file>         use this file as the --list-adapters output (default: run
rem                                      GpuSort.exe --list-adapters for real; it does no GPU work)
rem   set DRYRUN_FAIL=<k>                pretend run k returns DRYRUN_FAIL_RC (default 1)
rem   set DRYRUN_DEVMODE=0^|1             diag mode: pretend Developer Mode is off / on (default: read
rem                                      the registry)
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "EXE=%ROOT%GpuSort.exe"
set "MODE=latest"
set "SMOKE_ITERS=16"
set "FINAL_RC=0"
set "SKIPPED_SETS="
set "NOPAUSE="
set "NOPROMPT="
set "NPARG="
set "IGPU64="
if defined DRYRUN set "NOPAUSE=1"
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="all" (set "MODE=all") else if /i "%~1"=="latest" (set "MODE=latest") else if /i "%~1"=="current" (set "MODE=current") else if /i "%~1"=="probe" (set "MODE=probe") else if /i "%~1"=="diag" (set "MODE=diag") else if /i "%~1"=="pass7" (set "MODE=pass7") else if /i "%~1"=="final" (set "MODE=final") else if /i "%~1"=="scale" (set "MODE=scale") else if /i "%~1"=="igpu64" (set "IGPU64=1") else if /i "%~1"=="nopause" (set "NOPAUSE=1") else if /i "%~1"=="noprompt" (set "NOPROMPT=1") else (
    echo Unknown argument "%~1".
    call :usage
    set "NOPAUSE="
    set "FINAL_RC=1"
    goto :end
)
shift
goto :parse_args
:args_done
echo Modes: run_all.bat [latest^|all^|current^|probe^|diag^|pass7^|final^|scale] [igpu64] [noprompt] [nopause]   ^(default latest; probe = wave probe only, seconds; diag = flush diagnostic, minutes; pass7 = low-end shader set; final = the final algorithm set; scale = 20 to 512 sorts per iteration^)
if defined NOPROMPT (
    set "NPARG=--no-prompt"
    echo Running mode: %MODE%, NO POPUPS ^(noprompt^)
) else (
    echo Running mode: %MODE%
)

if not exist "%EXE%" (
    echo ERROR: GpuSort.exe was not found next to run_all.bat.
    echo Unzip the whole package into a folder first, then run run_all.bat from there.
    set "FINAL_RC=1"
    goto :end
)
if not exist "%ROOT%shaders\algorithms.txt" (
    echo ERROR: shaders\algorithms.txt is missing. Unzip the whole package first.
    set "FINAL_RC=1"
    goto :end
)

rem --- results folder --------------------------------------------------------------------------
set "TS="
for /f "usebackq delims=" %%t in (`powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmm" 2^>nul`) do set "TS=%%t"
if not defined TS set "TS=run!RANDOM!"
set "OUT=%ROOT%results\%COMPUTERNAME%_%TS%"
if exist "%OUT%" set "OUT=%OUT%_!RANDOM!"
mkdir "%OUT%" 2>nul
if not exist "%OUT%" (
    echo ERROR: cannot create the results folder "%OUT%".
    set "FINAL_RC=1"
    goto :end
)
set "SUMMARY=%OUT%\summary.txt"
> "%SUMMARY%" echo GpuSort portable run on %COMPUTERNAME%, started %DATE% %TIME%
>> "%SUMMARY%" echo Mode: %MODE%
if defined NOPROMPT >> "%SUMMARY%" echo No popups (noprompt): every GpuSort.exe run got --no-prompt
if defined DRYRUN >> "%SUMMARY%" echo DRY RUN: no GPU runs were executed
if exist "%ROOT%package_info.txt" copy /y "%ROOT%package_info.txt" "%OUT%\package_info.txt" >nul
echo Results folder: %OUT%

rem --- adapters --------------------------------------------------------------------------------
echo.
echo ==== Adapters (GpuSort.exe --list-adapters) ====
if defined DRYRUN if defined DRYRUN_ADAPTERS (
    echo [DRYRUN] using "%DRYRUN_ADAPTERS%" as the adapter list
    copy /y "%DRYRUN_ADAPTERS%" "%OUT%\adapters.txt" >nul
    type "%OUT%\adapters.txt"
    goto :adapters_done
)
"%EXE%" --list-adapters --log "%OUT%\adapters.txt"
set "LA_RC=%ERRORLEVEL%"
if not "%LA_RC%"=="0" (
    echo ERROR: GpuSort.exe --list-adapters failed with exit code %LA_RC%.
    echo Is this Windows 10/11 x64 with a D3D12 GPU driver installed?
    >> "%SUMMARY%" echo --list-adapters failed with exit code %LA_RC%
    set "FINAL_RC=1"
    goto :finish
)
:adapters_done

rem Qualifying GPUs print "      wave lane range: min <a> max <b>". WAVEALT = the largest max of a GPU
rem whose range is not a single size.
set "NGPUS=0"
set "WAVEALT=0"
for /f "usebackq tokens=5,7" %%a in (`findstr /c:"wave lane range:" "%OUT%\adapters.txt"`) do (
    set /a NGPUS+=1
    if %%b GTR %%a if %%b GTR !WAVEALT! set "WAVEALT=%%b"
)
if "%NGPUS%"=="0" (
    echo ERROR: no GPU qualifies for the benchmark ^(needs D3D12, shader model 6.6, wave ops^). See adapters.txt.
    >> "%SUMMARY%" echo no qualifying GPU
    set "FINAL_RC=1"
    goto :finish
)
>> "%SUMMARY%" echo Qualifying GPUs: %NGPUS% (details in adapters.txt)
if /i "%MODE%"=="probe" goto :probe_mode
if /i "%MODE%"=="diag" goto :diag_mode

if not defined DRYRUN (
    echo.
    echo Writing dxdiag.txt ^(driver details^) ...
    start "" /wait dxdiag /whql:off /t "%OUT%\dxdiag.txt"
)
if /i "%MODE%"=="pass7" goto :pass7_mode
if /i "%MODE%"=="final" goto :final_mode
if /i "%MODE%"=="scale" goto :scale_mode

rem --- plan: shader sets x wave configurations ----------------------------------------------------
set "SET_COUNT=1"
set "SET_1_NAME=current"
set "SET_1_DIR=%ROOT%shaders"
if /i "%MODE%"=="current" goto :sets_done
if /i "%MODE%"=="all" (
    for /l %%i in (0,1,99) do if exist "%ROOT%_test\pass%%i\algorithms.txt" call :add_set pass%%i
    goto :sets_done
)
rem latest: the newest snapshot that differs from shaders\. Newer snapshots identical to shaders\
rem (typically the one of the current pass) are reported as skipped.
set "LATEST="
set "NEWER_SAME="
for /l %%i in (0,1,99) do if exist "%ROOT%_test\pass%%i\algorithms.txt" (
    if exist "%ROOT%_test\pass%%i\SAME_AS_SHADERS.txt" (
        set "NEWER_SAME=!NEWER_SAME! pass%%i"
    ) else (
        set "LATEST=%%i"
        set "NEWER_SAME="
    )
)
set "SKIPPED_SETS=%NEWER_SAME%"
if defined LATEST call :add_set pass%LATEST%
:sets_done

set "WAVE_COUNT=1"
set "WAVE_1=0"
if not "%WAVEALT%"=="0" (
    set "WAVE_COUNT=2"
    set "WAVE_2=%WAVEALT%"
)
set /a NRUNS=2*SET_COUNT*WAVE_COUNT
set "RUNNO=0"

echo.
if defined NOPROMPT (
    echo ==== Plan: %NRUNS% GPU runs, no popups ^(noprompt^) ====
) else (
    echo ==== Plan: %NRUNS% GPU runs, each asks for confirmation first ====
)
>> "%SUMMARY%" echo Plan: %NRUNS% GPU runs
for /l %%s in (1,1,%SET_COUNT%) do (
    echo   shader set !SET_%%s_NAME!: !SET_%%s_DIR!
    >> "%SUMMARY%" echo   shader set !SET_%%s_NAME!: !SET_%%s_DIR!
)
if not "%WAVEALT%"=="0" (
    echo   wave configurations: default ^(WaveLaneCountMin^) and --wave-size %WAVEALT%
    >> "%SUMMARY%" echo   wave configurations: default and --wave-size %WAVEALT%
) else (
    echo   wave configuration: default ^(no GPU reports a wave size range^)
)
if defined SKIPPED_SETS (
    echo   skipped as identical to shaders\:!SKIPPED_SETS!
    >> "%SUMMARY%" echo   skipped as identical to shaders\:!SKIPPED_SETS!
)

call :countdown

rem --- phase 1: smoke tests ----------------------------------------------------------------------
set "FINAL_RC=0"
set "SMOKE_FAILS=0"
set "SMOKE_FAILED_RUNS="
for /l %%s in (1,1,%SET_COUNT%) do for /l %%w in (1,1,%WAVE_COUNT%) do (
    call :run smoke %%s %%w
    if "!RC!"=="2" (
        set "FINAL_RC=1"
        goto :stopped
    )
    if "!RC!"=="3" (
        set "FINAL_RC=1"
        goto :smoke_lost
    )
    if not "!RC!"=="0" (
        set /a SMOKE_FAILS+=1
        set "SMOKE_FAILED_RUNS=!SMOKE_FAILED_RUNS! [!LAST_LABEL!]"
        if exist "%OUT%\!FILE!.txt" findstr /c:"SMOKE FAILED:" "%OUT%\!FILE!.txt" >> "%SUMMARY%"
    )
)
if not "!SMOKE_FAILS!"=="0" goto :smoke_failed

rem --- phase 2: full runs ------------------------------------------------------------------------
rem (only reached if every smoke run passed)
for /l %%s in (1,1,%SET_COUNT%) do for /l %%w in (1,1,%WAVE_COUNT%) do (
    call :run full %%s %%w
    if not "!RC!"=="0" set "FINAL_RC=1"
    if "!RC!"=="2" goto :stopped
    if "!RC!"=="3" goto :stopped
)
goto :finish

rem --- probe mode: one GpuSort.exe --wave-probe run -------------------------------------------------
rem It probes every qualifying GPU without [WaveSize] and with [WaveSize(N)] for every power of two N
rem in the GPU's wave lane range (GpuSort.exe --wave-probe without --wave-size), in one process, so
rem there is one prompt. The report goes to wave_probe.txt; its Summary section is copied to summary.txt.
:probe_mode
set "NRUNS=1"
set "RUNNO=1"
echo.
if defined NOPROMPT (
    echo ==== Plan: 1 GPU run ^(wave probe only, no sorts^), no popup ^(noprompt^) ====
) else (
    echo ==== Plan: 1 GPU run ^(wave probe only, no sorts^), it asks for confirmation first ====
)
>> "%SUMMARY%" echo Plan: 1 GPU run (GpuSort.exe --wave-probe: every GPU x wave configuration, no sorts)
set "GPUNO=0"
for /f "usebackq tokens=5,7" %%a in (`findstr /c:"wave lane range:" "%OUT%\adapters.txt"`) do (
    set "CFGS=without [WaveSize]"
    for %%n in (4 8 16 32 64 128) do if %%n GEQ %%a if %%n LEQ %%b set "CFGS=!CFGS!, [WaveSize(%%n)]"
    echo   GPU [!GPUNO!] wave lanes %%a-%%b: !CFGS!
    >> "%SUMMARY%" echo   GPU [!GPUNO!] wave lanes %%a-%%b: !CFGS!
    set /a GPUNO+=1
)
call :countdown
set "LAST_LABEL=1/1: wave probe"
set "FILE=wave_probe"
set CMD="%EXE%" --wave-probe !NPARG! --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
echo.
echo ==== Run !LAST_LABEL! ====
if defined DRYRUN goto :probe_dry
!CMD!
set "RC=!ERRORLEVEL!"
goto :probe_done
:probe_dry
echo [DRYRUN] !CMD!
set "RC=0"
if "!DRYRUN_FAIL!"=="1" (
    if defined DRYRUN_FAIL_RC (set "RC=!DRYRUN_FAIL_RC!") else (set "RC=1")
)
:probe_done
set "RC_TEXT=unexpected exit code"
if "!RC!"=="0" set "RC_TEXT=ok, every wave configuration passed"
if "!RC!"=="1" set "RC_TEXT=WAVE PROBE WARNING or error, see !FILE!.txt"
if "!RC!"=="2" set "RC_TEXT=cancelled at the prompt"
if "!RC!"=="3" set "RC_TEXT=DEVICE LOST - GPU fault or hang, see !FILE!.log"
if not "!RC!"=="0" set "FINAL_RC=1"
echo Run !LAST_LABEL! finished: exit code !RC! ^(!RC_TEXT!^)
>> "%SUMMARY%" echo [!LAST_LABEL!] exit code !RC! (!RC_TEXT!) -^> !FILE!.txt / !FILE!.log
if exist "%OUT%\!FILE!.txt" (
    >> "%SUMMARY%" echo Wave probe summary ^(one verdict per GPU x wave configuration, details in !FILE!.txt^):
    set "INSUM="
    for /f "usebackq delims=" %%l in ("%OUT%\!FILE!.txt") do (
        set "LINE=%%l"
        if defined INSUM >> "%SUMMARY%" echo   !LINE!
        if "!LINE!"=="-------" set "INSUM=1"
    )
)
goto :finish

rem --- diag mode: the flush-tail diagnostic set, normal clocks, then (Developer Mode) stable power ---
rem Run 2 only follows a run 1 that finished with exit code 0, and never asks again: the one prompt
rem (run 1) covers both, its label says so. SetStablePowerState removes the device without Developer
rem Mode, so run 2 is skipped then (GpuSort.exe checks it too and would run without it).
:diag_mode
set "DIAG_ARGS=--shaders "%ROOT%shaders" --algo-file algorithms_diag_flush.txt --iterations 300 --workload mostly_empty,realistic_mix,worst_case"
set "DEVMODE="
if defined DRYRUN if defined DRYRUN_DEVMODE (
    if "%DRYRUN_DEVMODE%"=="1" set "DEVMODE=1"
    goto :devmode_done
)
set "DEVVAL="
for /f "tokens=3" %%v in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" /v AllowDevelopmentWithoutDevLicense 2^>nul ^| findstr /i "AllowDevelopmentWithoutDevLicense"') do set "DEVVAL=%%v"
if /i "!DEVVAL!"=="0x1" set "DEVMODE=1"
:devmode_done
if not exist "%ROOT%shaders\algorithms_diag_flush.txt" (
    echo ERROR: shaders\algorithms_diag_flush.txt is missing. Unzip the whole package first.
    >> "%SUMMARY%" echo shaders\algorithms_diag_flush.txt is missing
    set "FINAL_RC=1"
    goto :finish
)
if defined DEVMODE (set "NRUNS=2") else (set "NRUNS=1")
set "RUNNO=0"
echo.
if defined NOPROMPT (
    echo ==== Plan: %NRUNS% GPU run^(s^), default wave size, no popups ^(noprompt^) ====
) else (
    echo ==== Plan: %NRUNS% GPU run^(s^), default wave size, ONE confirmation popup in total ====
)
echo   algorithm list shaders\algorithms_diag_flush.txt ^(16 algorithms^), 300 iterations ^(+5 warmup^) of
echo   mostly_empty, realistic_mix and worst_case, on every qualifying GPU
echo   run 1: normal clocks -^> diag_flush.txt
>> "%SUMMARY%" echo Plan: %NRUNS% GPU run(s): shaders\algorithms_diag_flush.txt, 300 iterations, mostly_empty / realistic_mix / worst_case, default wave size
if defined DEVMODE (
    echo   run 2: --stable-power ^(SetStablePowerState, fixed clocks^) -^> diag_flush_stable.txt, starts right
    echo          after run 1 WITHOUT another popup
    >> "%SUMMARY%" echo   run 2: --stable-power, no prompt of its own ^(Windows Developer Mode is on^)
) else (
    echo   no --stable-power run: Windows Developer Mode is off ^(SetStablePowerState needs it^)
    >> "%SUMMARY%" echo   no --stable-power run: Windows Developer Mode is off
)
echo   Expected run time per run: under 1 min on a discrete GPU, 2-3 min on an integrated GPU ^(GpuSort
echo   prints a calibrated estimate right after the start^)
call :countdown
set "RUNNO=1"
set "FILE=diag_flush"
if defined DEVMODE (set "LAST_LABEL=1/2: diag flush, normal clocks (then 2/2 with --stable-power, no further prompt)") else (set "LAST_LABEL=1/1: diag flush, normal clocks")
set CMD="%EXE%" !DIAG_ARGS! !NPARG! --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
call :exec
if not "!RC!"=="0" (
    set "FINAL_RC=1"
    if defined DEVMODE (
        echo Run 2 ^(--stable-power^) is not started: run 1 did not finish with exit code 0.
        >> "%SUMMARY%" echo run 2 ^(--stable-power^) not started: run 1 exit code !RC!
    )
    goto :finish
)
if not defined DEVMODE goto :finish
set "RUNNO=2"
set "FILE=diag_flush_stable"
set "LAST_LABEL=2/2: diag flush, --stable-power"
set CMD="%EXE%" !DIAG_ARGS! --stable-power --no-prompt --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
call :exec
if not "!RC!"=="0" set "FINAL_RC=1"
if exist "%OUT%\!FILE!.txt" findstr /c:"stable power:" "%OUT%\!FILE!.txt" >> "%SUMMARY%"
goto :finish

rem --- pass7 mode: the low-end / iGPU shader set _test\pass7 ------------------------------------------
rem Smoke runs first (every one of them; a verification failure stops the script before the full
rem runs, a device loss or Cancel at once), then the full runs. Default wave size on every GPU; if an
rem integrated GPU reports a wave size range (AMD Ryzen iGPU: 32-64), also --wave-size <its max> with
rem --integrated-only (the discrete GPU's wave64 is not part of this pass).
:pass7_mode
set "P7DIR=%ROOT%_test\pass7"
for %%f in (algorithms.txt algorithms_smoke.txt) do if not exist "%P7DIR%\%%f" (
    echo ERROR: _test\pass7\%%f is missing. Unzip the whole package first.
    >> "%SUMMARY%" echo _test\pass7\%%f is missing
    set "FINAL_RC=1"
    goto :finish
)
rem Iterations per GPU: algorithms x 10 workloads x (300 + 5 warmup), smoke: x 16 (no warmup).
set "P7_NALG=0"
for /f "usebackq" %%x in (`findstr /b /c:"algorithm " "%P7DIR%\algorithms.txt"`) do set /a P7_NALG+=1
set "P7_NALG_SMOKE=0"
for /f "usebackq" %%x in (`findstr /b /c:"algorithm " "%P7DIR%\algorithms_smoke.txt"`) do set /a P7_NALG_SMOKE+=1
set /a P7_IT_FULL=P7_NALG*10*305
set /a P7_IT_SMOKE=P7_NALG_SMOKE*10*SMOKE_ITERS
rem Estimate: wall ms per iteration x 10, measured on earlier runs (the 256 MB flush dominates):
rem discrete (RTX 5080 / RX 7900 XTX) 0.8 full / 3.3 serial smoke, Ryzen iGPU 9.7 / 12.5, Intel UHD
rem 770 7.6 / 12.0; plus ~20 s per run for shader compilation, calibration and the wave probe.
rem The adapter lines are "wave lane range: min <a> max <b>  integrated <yes|no>  vendor <id>".
set "IGPUWAVE=0"
set "P7_NIGPU=0"
set "EST_S=0"
set "EST_F=0"
set "GPUNO=0"
echo.
echo ==== pass7: GPUs ====
for /f "usebackq tokens=5,7,9,11" %%a in (`findstr /c:"wave lane range:" "%OUT%\adapters.txt"`) do (
    set "GKIND=discrete"
    if "%%c"=="" set "GKIND=unknown (adapter list of an older GpuSort.exe), counted as discrete"
    set "FMS=8"
    set "SMS=33"
    if /i "%%c"=="yes" (
        set "GKIND=integrated"
        set /a P7_NIGPU+=1
        set "FMS=97"
        set "SMS=125"
        if /i "%%d"=="8086" (
            set "FMS=76"
            set "SMS=120"
        )
        if %%b GTR %%a if %%b GTR !IGPUWAVE! set "IGPUWAVE=%%b"
    )
    set /a GS=P7_IT_SMOKE*SMS/10000, GF=P7_IT_FULL*FMS/10000
    set /a EST_S+=GS, EST_F+=GF
    call :fmt_secs !GF! GFT
    echo   GPU [!GPUNO!] !GKIND!, vendor %%d, wave lanes %%a-%%b: smoke ~!GS! s, full ~!GFT!
    >> "%SUMMARY%" echo   GPU [!GPUNO!] !GKIND!, vendor %%d, wave lanes %%a-%%b
    set /a GPUNO+=1
)
rem The integrated-only runs: every integrated GPU whose range contains IGPUWAVE.
set "EST_WS=0"
set "EST_WF=0"
set "P7_WAVES=0"
if not "%IGPUWAVE%"=="0" (
    set "P7_WAVES=0 %IGPUWAVE%"
    for /f "usebackq tokens=5,7,9,11" %%a in (`findstr /c:"wave lane range:" "%OUT%\adapters.txt"`) do (
        if /i "%%c"=="yes" if %IGPUWAVE% GEQ %%a if %IGPUWAVE% LEQ %%b (
            set "FMS=97"
            set "SMS=125"
            if /i "%%d"=="8086" (
                set "FMS=76"
                set "SMS=120"
            )
            set /a EST_WS+=P7_IT_SMOKE*SMS/10000, EST_WF+=P7_IT_FULL*FMS/10000
        )
    )
)
if "%IGPUWAVE%"=="0" (set "NRUNS=2") else (set "NRUNS=4")
set /a EST_S+=20, EST_F+=20
if not "%IGPUWAVE%"=="0" set /a EST_WS+=20, EST_WF+=20
set /a EST_TOTAL=EST_S+EST_F+EST_WS+EST_WF
call :fmt_secs %EST_S% T_S
call :fmt_secs %EST_F% T_F
call :fmt_secs %EST_WS% T_WS
call :fmt_secs %EST_WF% T_WF
call :fmt_secs %EST_TOTAL% T_TOTAL
set "RUNNO=0"
echo.
if defined NOPROMPT (
    echo ==== Plan: %NRUNS% GPU runs of _test\pass7, no popups ^(noprompt: one 10 s countdown now^) ====
) else (
    echo ==== Plan: %NRUNS% GPU runs of _test\pass7, each asks for confirmation first ^(%NRUNS% popups^) ====
)
>> "%SUMMARY%" echo Plan: %NRUNS% GPU runs of _test\pass7 (%P7_NALG% algorithms, smoke %P7_NALG_SMOKE%), estimated ~%T_TOTAL%
if "%IGPUWAVE%"=="0" (
    echo   1/2 smoke, default wave size, every GPU: algorithms_smoke.txt ^(%P7_NALG_SMOKE% algorithms^), %SMOKE_ITERS% iterations, --dred  ~%T_S%
    echo   2/2 full,  default wave size, every GPU: algorithms.txt ^(%P7_NALG% algorithms^), 300 iterations  ~%T_F%
    if "%P7_NIGPU%"=="0" (
        echo   no integrated GPU: no integrated-only wave run
    ) else (
        echo   no integrated GPU with a wave size range: no integrated-only wave run
    )
) else (
    echo   1/4 smoke, default wave size, every GPU: algorithms_smoke.txt ^(%P7_NALG_SMOKE% algorithms^), %SMOKE_ITERS% iterations, --dred  ~%T_S%
    echo   2/4 smoke, --wave-size %IGPUWAVE%, integrated GPU^(s^) only ^(--integrated-only^), same list  ~%T_WS%
    echo   3/4 full,  default wave size, every GPU: algorithms.txt ^(%P7_NALG% algorithms^), 300 iterations  ~%T_F%
    echo   4/4 full,  --wave-size %IGPUWAVE%, integrated GPU^(s^) only, 300 iterations  ~%T_WF%
)
echo   The full runs start only if every smoke run passed. Estimated total: ~%T_TOTAL%
echo   ^(GpuSort prints a calibrated estimate right after each start^)
if "%IGPUWAVE%"=="0" (
    >> "%SUMMARY%" echo   smoke, then full: default wave size, every GPU
) else (
    >> "%SUMMARY%" echo   smoke, then full: default wave size on every GPU, and --wave-size %IGPUWAVE% on the integrated GPU^(s^) only
)
call :countdown

set "FINAL_RC=0"
set "SMOKE_FAILS=0"
set "SMOKE_FAILED_RUNS="
for %%w in (%P7_WAVES%) do (
    call :p7_run smoke %%w
    if "!RC!"=="2" (
        set "FINAL_RC=1"
        goto :stopped
    )
    if "!RC!"=="3" (
        set "FINAL_RC=1"
        goto :smoke_lost
    )
    if not "!RC!"=="0" (
        set /a SMOKE_FAILS+=1
        set "SMOKE_FAILED_RUNS=!SMOKE_FAILED_RUNS! [!LAST_LABEL!]"
        if exist "%OUT%\!FILE!.txt" findstr /c:"SMOKE FAILED:" "%OUT%\!FILE!.txt" >> "%SUMMARY%"
    )
)
if not "!SMOKE_FAILS!"=="0" goto :smoke_failed
for %%w in (%P7_WAVES%) do (
    call :p7_run full %%w
    if not "!RC!"=="0" set "FINAL_RC=1"
    if "!RC!"=="2" goto :stopped
    if "!RC!"=="3" goto :stopped
)
goto :finish

rem --- final mode: shaders\algorithms_final.txt, the final run ------------------------------------------
rem Runs (in this order): smoke default wave size (every GPU), [smoke --wave-size <W>], full default
rem wave size (every GPU; --iterations 1000 --iterations-integrated 300), [full --wave-size <W>].
rem W = the largest wave lane max of a DISCRETE GPU with a range (AMD RDNA: 64), run with
rem --discrete-only; with igpu64 also integrated GPUs with a range count (no GPU filter; --wave-size
rem skips GPUs whose range does not contain W). Stop rules: Cancel / device loss stop at once; a
rem verification failure in the default-wave smoke stops before every full run, one in the wave<W>
rem smoke only skips the wave<W> full run.
:final_mode
set "FSET=algorithms_final.txt"
set "FDIR=%ROOT%shaders"
if not exist "%FDIR%\%FSET%" (
    echo ERROR: shaders\%FSET% is missing. Unzip the whole package first.
    >> "%SUMMARY%" echo shaders\%FSET% is missing
    set "FINAL_RC=1"
    goto :finish
)
set "F_ITERS=1000"
set "F_ITERS_I=300"
set "F_NALG=0"
for /f "usebackq" %%x in (`findstr /b /c:"algorithm " "%FDIR%\%FSET%"`) do set /a F_NALG+=1
rem Iterations per GPU run: algorithms x 10 workloads x (iterations + 5 warmup); smoke x 16, no warmup.
set /a F_IT_D=F_NALG*10*(F_ITERS+5)
set /a F_IT_I=F_NALG*10*(F_ITERS_I+5)
set /a F_IT_S=F_NALG*10*SMOKE_ITERS
rem Estimate: wall time per iteration in 1/100 ms, measured on earlier runs (the 256 MB flush dominates),
rem + 0.05 ms for the 50 us spin drain of full_d50: discrete (RTX 5080 0.73 / RX 7900 XTX 0.79 ms full,
rem 3.6 / 4.7 ms serial smoke) 85 / 470; RTX 20xx / GTX (not measured: ~1/3 of the memory bandwidth)
rem 190 / 600; Ryzen iGPU 980 / 1400 (wave64 1060 / 1450); Intel UHD 770 770 / 1200; plus
rem F_OVH_S seconds per run for the shader compilation (the whole set, once per wave configuration: ~70 s
rem on a fast desktop CPU), the calibrations and the wave probe.
set "F_OVH_S=90"
rem GPU names: the "  [n] <name>  vendor ..." lines of the benchmark adapter list, in the same order as
rem the "wave lane range" lines.
set "GPUNO=0"
for /f "usebackq tokens=1* delims=]" %%a in (`findstr /b /c:"  [" "%OUT%\adapters.txt"`) do (
    set "GN=%%b"
    set "GN=!GN:~1!"
    set "GN=!GN:  vendor =|!"
    for /f "tokens=1 delims=|" %%n in ("!GN!") do set "GNAME_!GPUNO!=%%n"
    set /a GPUNO+=1
)
rem Wave configuration for the extra run: discrete GPUs with a range (and integrated ones with igpu64).
set "FWAVE=0"
set "F_IGPU_RANGE=0"
for /f "usebackq tokens=5,7,9" %%a in (`findstr /c:"wave lane range:" "%OUT%\adapters.txt"`) do (
    if %%b GTR %%a (
        if /i "%%c"=="yes" (
            set /a F_IGPU_RANGE+=1
            if defined IGPU64 if %%b GTR !FWAVE! set "FWAVE=%%b"
        ) else (
            if %%b GTR !FWAVE! set "FWAVE=%%b"
        )
    )
)
set "EST_S=0"
set "EST_F=0"
set "EST_WS=0"
set "EST_WF=0"
set "GPUNO=0"
echo.
echo ==== final: GPUs ^(estimates from earlier runs of similar GPUs; GpuSort prints a calibrated one^) ====
>> "%SUMMARY%" echo GPUs:
for /f "usebackq tokens=5,7,9,11" %%a in (`findstr /c:"wave lane range:" "%OUT%\adapters.txt"`) do (
    set "GKIND=discrete"
    if "%%c"=="" set "GKIND=unknown (adapter list of an older GpuSort.exe), counted as discrete"
    set "GITERS=%F_ITERS%"
    set "GIT=%F_IT_D%"
    set "FMS=85"
    set "SMS=470"
    set "WFMS=85"
    set "WSMS=470"
    set "GN="
    for %%i in (!GPUNO!) do set "GN=!GNAME_%%i!"
    if not defined GN set "GN=?"
    set "SLOWD="
    if not "!GN:RTX 20=!"=="!GN!" set "SLOWD=1"
    if not "!GN:GTX =!"=="!GN!" set "SLOWD=1"
    if defined SLOWD (
        set "FMS=190"
        set "SMS=600"
        set "WFMS=190"
        set "WSMS=600"
    )
    if /i "%%c"=="yes" (
        set "GKIND=integrated"
        set "GITERS=%F_ITERS_I%"
        set "GIT=%F_IT_I%"
        set "FMS=980"
        set "SMS=1400"
        set "WFMS=1060"
        set "WSMS=1450"
        if /i "%%d"=="8086" (
            set "FMS=770"
            set "SMS=1200"
        )
    )
    set /a GS=F_IT_S*SMS/100000, GF=GIT*FMS/100000
    set /a EST_S+=GS, EST_F+=GF
    set "GW="
    set "INW="
    if not "%FWAVE%"=="0" if %FWAVE% GEQ %%a if %FWAVE% LEQ %%b (
        set "INW=1"
        if /i "%%c"=="yes" if not defined IGPU64 set "INW="
    )
    if defined INW (
        set /a GWS=F_IT_S*WSMS/100000, GWF=GIT*WFMS/100000
        set /a EST_WS+=GWS, EST_WF+=GWF
        call :fmt_secs !GWF! GWFT
        set "GW=; wave%FWAVE% smoke ~!GWS! s, full ~!GWFT!"
    )
    call :fmt_secs !GF! GFT
    echo   GPU [!GPUNO!] !GN!: !GKIND!, vendor %%d, wave lanes %%a-%%b
    echo           smoke ~!GS! s, full ^(!GITERS! iterations^) ~!GFT!!GW!
    >> "%SUMMARY%" echo   GPU [!GPUNO!] !GN!: !GKIND!, vendor %%d, wave lanes %%a-%%b, !GITERS! iterations
    set /a GPUNO+=1
)
if "%FWAVE%"=="0" (
    set "F_WAVES=0"
    set "NRUNS=2"
) else (
    set "F_WAVES=0 %FWAVE%"
    set "NRUNS=4"
)
set /a EST_S+=F_OVH_S, EST_F+=F_OVH_S
if not "%FWAVE%"=="0" set /a EST_WS+=F_OVH_S, EST_WF+=F_OVH_S
set /a EST_TOTAL=EST_S+EST_F+EST_WS+EST_WF
call :fmt_secs %EST_S% T_S
call :fmt_secs %EST_F% T_F
call :fmt_secs %EST_WS% T_WS
call :fmt_secs %EST_WF% T_WF
call :fmt_secs %EST_TOTAL% T_TOTAL
if defined IGPU64 (set "WSCOPE=every GPU with a wave size range") else (set "WSCOPE=discrete GPU(s) only, --discrete-only")
set "RUNNO=0"
echo.
if defined NOPROMPT (
    echo ==== Plan: %NRUNS% GPU runs of shaders\%FSET%, no popups ^(noprompt: one 10 s countdown now^) ====
) else (
    echo ==== Plan: %NRUNS% GPU runs of shaders\%FSET%, each asks for confirmation first ^(%NRUNS% popups^) ====
)
>> "%SUMMARY%" echo Plan: %NRUNS% GPU runs of shaders\%FSET% (%F_NALG% algorithms, default flush full_d50), estimated ~%T_TOTAL%
if "%FWAVE%"=="0" (
    echo   1/2 smoke, default wave size, every GPU: %F_NALG% algorithms, %SMOKE_ITERS% iterations, --dred  ~%T_S%
    echo   2/2 full,  default wave size, every GPU: %F_ITERS% iterations ^(integrated GPUs: %F_ITERS_I%^)  ~%T_F%
) else (
    echo   1/4 smoke, default wave size, every GPU: %F_NALG% algorithms, %SMOKE_ITERS% iterations, --dred  ~%T_S%
    echo   2/4 smoke, --wave-size %FWAVE%, !WSCOPE!: the same  ~%T_WS%
    echo   3/4 full,  default wave size, every GPU: %F_ITERS% iterations ^(integrated GPUs: %F_ITERS_I%^)  ~%T_F%
    echo   4/4 full,  --wave-size %FWAVE%, !WSCOPE!: the same iterations  ~%T_WF%
)
if not "%F_IGPU_RANGE%"=="0" if not defined IGPU64 (
    echo   integrated GPU^(s^) with a wave size range: default wave size only ^(add igpu64 to also run them
    echo   at their max wave size; the Ryzen iGPU is known to be slow at wave64^)
    >> "%SUMMARY%" echo   integrated GPU^(s^) with a wave size range: default wave size only ^(no igpu64^)
)
echo   The full runs start only if the smoke run of the same wave configuration passed ^(the default-wave
echo   smoke gates all of them^). Estimated total: ~%T_TOTAL%
echo   ^(GpuSort prints a calibrated estimate right after each start^)
if "%FWAVE%"=="0" (
    >> "%SUMMARY%" echo   smoke, then full: default wave size, every GPU
) else (
    >> "%SUMMARY%" echo   smoke, then full: default wave size on every GPU, and --wave-size %FWAVE% on !WSCOPE!
)
call :countdown

set "FINAL_RC=0"
set "SMOKE_FAILS=0"
set "SMOKE_FAILED_RUNS="
set "F_SKIP_WAVE="
set "F_DEFAULT_FAILED="
for %%w in (%F_WAVES%) do (
    call :final_run smoke %%w
    if "!RC!"=="2" (
        set "FINAL_RC=1"
        goto :stopped
    )
    if "!RC!"=="3" (
        set "FINAL_RC=1"
        goto :smoke_lost
    )
    if not "!RC!"=="0" (
        set /a SMOKE_FAILS+=1
        set "SMOKE_FAILED_RUNS=!SMOKE_FAILED_RUNS! [!LAST_LABEL!]"
        if exist "%OUT%\!FILE!.txt" findstr /c:"SMOKE FAILED:" "%OUT%\!FILE!.txt" >> "%SUMMARY%"
        if "%%w"=="0" (set "F_DEFAULT_FAILED=1") else (set "F_SKIP_WAVE=%%w")
    )
)
if defined F_DEFAULT_FAILED goto :smoke_failed
if defined F_SKIP_WAVE (
    set "FINAL_RC=1"
    echo.
    echo ************************************************************************************
    echo  SMOKE TEST FAILED in the wave%F_SKIP_WAVE% smoke run:!SMOKE_FAILED_RUNS!
    echo  The algorithms that failed are listed in summary.txt ^("SMOKE FAILED:" lines^). The
    echo  wave%F_SKIP_WAVE% full run is skipped; the default-wave full run follows.
    echo ************************************************************************************
    >> "%SUMMARY%" echo wave%F_SKIP_WAVE% full run SKIPPED: its smoke run failed:!SMOKE_FAILED_RUNS!
)
for %%w in (%F_WAVES%) do (
    set "F_DO=1"
    if "%%w"=="!F_SKIP_WAVE!" set "F_DO="
    if defined F_DO (
        call :final_run full %%w
        if not "!RC!"=="0" set "FINAL_RC=1"
        if "!RC!"=="2" goto :stopped
        if "!RC!"=="3" goto :stopped
    ) else (
        set /a RUNNO+=1
    )
)
goto :finish

rem --- scale mode: shaders\algorithms_scale.txt at 20, 128, 256 and 512 sorts per iteration ------------
rem Runs (in this order): smoke (every GPU, default wave size, --sorts S_SORTS, S_SMOKE_ITERS serial
rem iterations, --dred), then the full run (every GPU, default wave size, --sorts S_SORTS, iterations
rem S_ITERS on discrete / S_ITERS_I on integrated GPUs, one value per sort count). No wave64 run. Stop
rem rules: Cancel / device loss stop at once; a verification failure in the smoke run stops before the
rem full run.
:scale_mode
set "SSET=algorithms_scale.txt"
set "SDIR=%ROOT%shaders"
if not exist "%SDIR%\%SSET%" (
    echo ERROR: shaders\%SSET% is missing. Unzip the whole package first.
    >> "%SUMMARY%" echo shaders\%SSET% is missing
    set "FINAL_RC=1"
    goto :finish
)
rem Sort counts 1-4 and the measured iterations at each (discrete / integrated GPUs): fewer at the
rem large counts, whose iterations take longer (the flush still dominates; see the estimate below).
set "S_N1=20"
set "S_N2=128"
set "S_N3=256"
set "S_N4=512"
set "S_D1=1000"
set "S_D2=300"
set "S_D3=200"
set "S_D4=150"
set "S_I1=300"
set "S_I2=90"
set "S_I3=60"
set "S_I4=45"
set "S_SORTS=%S_N1%,%S_N2%,%S_N3%,%S_N4%"
set "S_ITERS=%S_D1%,%S_D2%,%S_D3%,%S_D4%"
set "S_ITERS_I=%S_I1%,%S_I2%,%S_I3%,%S_I4%"
set "S_SMOKE_ITERS=8"
set "S_NALG=0"
for /f "usebackq" %%x in (`findstr /b /c:"algorithm " "%SDIR%\%SSET%"`) do set /a S_NALG+=1
rem Iterations per GPU run and sort count: algorithms x 10 workloads x (iterations + 5 warmup); smoke
rem x S_SMOKE_ITERS, no warmup.
set /a S_ITD1=S_NALG*10*(S_D1+5), S_ITD2=S_NALG*10*(S_D2+5), S_ITD3=S_NALG*10*(S_D3+5), S_ITD4=S_NALG*10*(S_D4+5)
set /a S_ITI1=S_NALG*10*(S_I1+5), S_ITI2=S_NALG*10*(S_I2+5), S_ITI3=S_NALG*10*(S_I3+5), S_ITI4=S_NALG*10*(S_I4+5)
set /a S_ITS=S_NALG*10*S_SMOKE_ITERS
rem Estimate: wall time per iteration in 1/100 ms at 20 / 128 / 256 / 512 sorts (full; smoke = serial),
rem measured on the RTX 5080 / RTX 2060 scale runs (full_d50; more sorts add the upload, poison and
rem readback of numSorts x 32 KB per iteration and the longer sorts); the integrated GPUs scaled from
rem their final-run 20-sort cost (Ryzen iGPU 9.8 ms, Intel UHD 770 7.7 ms full): discrete 85 / 110 /
rem 140 / 200 full, 470 / 520 / 600 / 800 smoke; RTX 20xx / GTX 190 / 260 / 340 / 480, 600 / 700 / 850 /
rem 1100; Ryzen iGPU 980 / 1100 / 1300 / 1700, 1400 / 1600 / 1900 / 2500; Intel UHD 770 770 / 900 /
rem 1100 / 1500, 1200 / 1400 / 1700 / 2200; plus S_OVH_S seconds per run (shader compilation, the
rem calibrations of every sort count, the wave probe).
set "S_OVH_S=60"
set "GPUNO=0"
for /f "usebackq tokens=1* delims=]" %%a in (`findstr /b /c:"  [" "%OUT%\adapters.txt"`) do (
    set "GN=%%b"
    set "GN=!GN:~1!"
    set "GN=!GN:  vendor =|!"
    for /f "tokens=1 delims=|" %%n in ("!GN!") do set "GNAME_!GPUNO!=%%n"
    set /a GPUNO+=1
)
set "EST_S=0"
set "EST_F=0"
set "GPUNO=0"
echo.
echo ==== scale: GPUs ^(estimates from earlier runs of similar GPUs; GpuSort prints a calibrated one^) ====
>> "%SUMMARY%" echo GPUs:
for /f "usebackq tokens=5,7,9,11" %%a in (`findstr /c:"wave lane range:" "%OUT%\adapters.txt"`) do (
    set "GKIND=discrete"
    if "%%c"=="" set "GKIND=unknown (adapter list of an older GpuSort.exe), counted as discrete"
    set "GITERS=%S_ITERS%"
    set "IT1=%S_ITD1%"
    set "IT2=%S_ITD2%"
    set "IT3=%S_ITD3%"
    set "IT4=%S_ITD4%"
    set "FM1=85"
    set "FM2=110"
    set "FM3=140"
    set "FM4=200"
    set "SM1=470"
    set "SM2=520"
    set "SM3=600"
    set "SM4=800"
    set "GN="
    for %%i in (!GPUNO!) do set "GN=!GNAME_%%i!"
    if not defined GN set "GN=?"
    set "SLOWD="
    if not "!GN:RTX 20=!"=="!GN!" set "SLOWD=1"
    if not "!GN:GTX =!"=="!GN!" set "SLOWD=1"
    if defined SLOWD (
        set "FM1=190"
        set "FM2=260"
        set "FM3=340"
        set "FM4=480"
        set "SM1=600"
        set "SM2=700"
        set "SM3=850"
        set "SM4=1100"
    )
    if /i "%%c"=="yes" (
        set "GKIND=integrated"
        set "GITERS=%S_ITERS_I%"
        set "IT1=%S_ITI1%"
        set "IT2=%S_ITI2%"
        set "IT3=%S_ITI3%"
        set "IT4=%S_ITI4%"
        set "FM1=980"
        set "FM2=1100"
        set "FM3=1300"
        set "FM4=1700"
        set "SM1=1400"
        set "SM2=1600"
        set "SM3=1900"
        set "SM4=2500"
        if /i "%%d"=="8086" (
            set "FM1=770"
            set "FM2=900"
            set "FM3=1100"
            set "FM4=1500"
            set "SM1=1200"
            set "SM2=1400"
            set "SM3=1700"
            set "SM4=2200"
        )
    )
    set /a GS=S_ITS*SM1/100000+S_ITS*SM2/100000+S_ITS*SM3/100000+S_ITS*SM4/100000
    set /a GF1=IT1*FM1/100000, GF2=IT2*FM2/100000, GF3=IT3*FM3/100000, GF4=IT4*FM4/100000
    set /a GF=GF1+GF2+GF3+GF4
    set /a EST_S+=GS, EST_F+=GF
    call :fmt_secs !GF! GFT
    echo   GPU [!GPUNO!] !GN!: !GKIND!, vendor %%d, wave lanes %%a-%%b
    echo           smoke ~!GS! s, full ^(!GITERS! iterations at %S_SORTS% sorts^) ~!GFT! ^(per sort count ~!GF1! / !GF2! / !GF3! / !GF4! s^)
    >> "%SUMMARY%" echo   GPU [!GPUNO!] !GN!: !GKIND!, vendor %%d, wave lanes %%a-%%b, !GITERS! iterations at %S_SORTS% sorts
    set /a GPUNO+=1
)
set "NRUNS=2"
set /a EST_S+=S_OVH_S, EST_F+=S_OVH_S
set /a EST_TOTAL=EST_S+EST_F
call :fmt_secs %EST_S% T_S
call :fmt_secs %EST_F% T_F
call :fmt_secs %EST_TOTAL% T_TOTAL
set "RUNNO=0"
echo.
if defined NOPROMPT (
    echo ==== Plan: %NRUNS% GPU runs of shaders\%SSET%, no popups ^(noprompt: one 10 s countdown now^) ====
) else (
    echo ==== Plan: %NRUNS% GPU runs of shaders\%SSET%, each asks for confirmation first ^(%NRUNS% popups^) ====
)
>> "%SUMMARY%" echo Plan: %NRUNS% GPU runs of shaders\%SSET% (%S_NALG% algorithms, sorts per iteration %S_SORTS%, default flush full_d50, default wave size), estimated ~%T_TOTAL%
echo   1/2 smoke, default wave size, every GPU: %S_NALG% algorithms x 10 workloads at %S_SORTS% sorts per
echo       iteration, %S_SMOKE_ITERS% iterations each, --dred  ~%T_S%
echo   2/2 full,  default wave size, every GPU: the same, %S_ITERS% iterations at %S_SORTS% sorts
echo       ^(integrated GPUs: %S_ITERS_I%^)  ~%T_F%
echo   The full run starts only if the smoke run passed. Estimated total: ~%T_TOTAL%
echo   ^(GpuSort prints a calibrated estimate per GPU and sort count right after each start^)
call :countdown

set "FINAL_RC=0"
set "SMOKE_FAILS=0"
set "SMOKE_FAILED_RUNS="
call :scale_run smoke
if "!RC!"=="2" (
    set "FINAL_RC=1"
    goto :stopped
)
if "!RC!"=="3" (
    set "FINAL_RC=1"
    goto :smoke_lost
)
if not "!RC!"=="0" (
    set /a SMOKE_FAILS+=1
    set "SMOKE_FAILED_RUNS=!SMOKE_FAILED_RUNS! [!LAST_LABEL!]"
    if exist "%OUT%\!FILE!.txt" findstr /c:"SMOKE FAILED:" "%OUT%\!FILE!.txt" >> "%SUMMARY%"
    goto :smoke_failed
)
call :scale_run full
if not "!RC!"=="0" set "FINAL_RC=1"
if "!RC!"=="2" goto :stopped
if "!RC!"=="3" goto :stopped
goto :finish

:smoke_failed
set "FINAL_RC=1"
echo.
echo ************************************************************************************
echo  SMOKE TEST FAILED in !SMOKE_FAILS! smoke run^(s^):!SMOKE_FAILED_RUNS!
echo  All smoke runs were completed; the algorithms that failed are listed in summary.txt
echo  ^("SMOKE FAILED:" lines^). No full benchmark run was started. The smoke logs in the
echo  results folder show the details.
echo ************************************************************************************
>> "%SUMMARY%" echo STOPPED before the full runs: !SMOKE_FAILS! smoke run(s) failed:!SMOKE_FAILED_RUNS!
goto :finish

:smoke_lost
echo.
echo ************************************************************************************
echo  DEVICE LOST in smoke run !LAST_LABEL! ^(!RC_TEXT!^).
echo  Nothing more was started. The smoke log has the DRED report.
echo ************************************************************************************
>> "%SUMMARY%" echo STOPPED: smoke test !LAST_LABEL! lost the device (!RC_TEXT!)
goto :finish

:stopped
echo.
echo Stopping after run !LAST_LABEL!: !RC_TEXT!.
>> "%SUMMARY%" echo STOPPED after !LAST_LABEL!: !RC_TEXT!
goto :finish

rem --- finish: zip + message ---------------------------------------------------------------------
:finish
>> "%SUMMARY%" echo Finished %DATE% %TIME%, overall exit code %FINAL_RC%
set "ZIP=%OUT%.zip"
set "ZIPPED="
powershell -NoProfile -Command "Compress-Archive -Path $env:OUT -DestinationPath $env:ZIP -Force" >nul 2>&1 && if exist "%ZIP%" set "ZIPPED=1"
echo.
echo ====================================================================================
if "%FINAL_RC%"=="0" (echo  All runs finished.) else (echo  Finished with problems - see summary.txt.)
echo  Please bring back this folder:
echo     %OUT%
if defined ZIPPED (
    echo  or this zip of it:
    echo     %ZIP%
)
echo ====================================================================================
type "%SUMMARY%"
goto :end

rem --- subroutines -------------------------------------------------------------------------------

:usage
echo Usage: run_all.bat [latest^|all^|current^|probe^|diag^|pass7^|final^|scale] [igpu64] [noprompt] [nopause]
echo   ^(none^) or latest  current shaders\ + the latest _test\passN snapshot that differs from shaders\
echo   all               current shaders\ + every _test\passN snapshot
echo   current           current shaders\ only
echo   probe             only the wave probe: one GpuSort.exe --wave-probe run ^(one prompt, seconds^)
echo   diag              flush-tail diagnostic: shaders\algorithms_diag_flush.txt, normal clocks + ^(with
echo                     Developer Mode^) --stable-power, one prompt, a few minutes
echo   pass7             low-end shader set _test\pass7: smoke + full run, default wave size, plus
echo                     --wave-size ^<max^> on an integrated GPU with a wave size range ^(AMD iGPU^)
echo   final             THE FINAL RUN: shaders\algorithms_final.txt, smoke + full run ^(1000 iterations
echo                     discrete, 300 integrated^), default wave size on every GPU, plus --wave-size ^<max^>
echo                     on discrete GPUs with a wave size range ^(AMD^)
echo   scale             THE SCALE RUN: shaders\algorithms_scale.txt at 20, 128, 256 and 512 sorts per
echo                     iteration, smoke + full run, default wave size on every GPU
echo   igpu64            final mode: also run integrated GPUs with a wave size range at ^<max^> ^(slow^)
echo   noprompt          no popups ^(--no-prompt for every run^), after a 10 s countdown ^(Ctrl+C aborts^)
echo   nopause           do not wait for a key at the end
exit /b 0

rem :countdown: with noprompt, one warning line and 10 s to abort (Ctrl+C) before the first GPU run.
:countdown
if not defined NOPROMPT exit /b 0
echo.
echo No popups: make sure other GPU work is paused
>> "%SUMMARY%" echo No popups: 10 s countdown before the first GPU run
if defined DRYRUN (
    echo [DRYRUN] 10 s countdown skipped
    exit /b 0
)
echo Starting in 10 seconds - press Ctrl+C to abort.
rem (timeout.exe by full path: a GNU timeout earlier in PATH, e.g. from Git, takes other arguments;
rem  with redirected input it fails at once, then ping waits instead.)
"%SystemRoot%\System32\timeout.exe" /t 10 /nobreak
if errorlevel 1 "%SystemRoot%\System32\ping.exe" -n 11 127.0.0.1 >nul
exit /b 0

rem :add_set <passN>: adds _test\<passN> unless the packager marked it identical to shaders\.
:add_set
if exist "%ROOT%_test\%~1\SAME_AS_SHADERS.txt" (
    set "SKIPPED_SETS=!SKIPPED_SETS! %~1"
    exit /b 0
)
set /a SET_COUNT+=1
set "SET_%SET_COUNT%_NAME=%~1"
set "SET_%SET_COUNT%_DIR=%ROOT%_test\%~1"
exit /b 0

rem :fmt_secs <seconds> <variable>: "<n> s" below 100 s, else "<m.t> min".
:fmt_secs
set /a FS_S=%~1
if %FS_S% LSS 100 (
    set "%~2=%FS_S% s"
    exit /b 0
)
set /a FS_M=FS_S/60, FS_T=(FS_S%%60)*10/60
set "%~2=%FS_M%.%FS_T% min"
exit /b 0

rem :p7_run <smoke|full> <wave size, 0 = default>: one GpuSort run of _test\pass7 (pass7 mode); a
rem wave size means --wave-size <n> --integrated-only. Sets RC, RC_TEXT, LAST_LABEL, FILE.
:p7_run
set /a RUNNO+=1
set "FILE=pass7"
set "WDESC=default wave size, every GPU"
set "XARGS="
if not "%~2"=="0" (
    set "FILE=pass7_igpu_wave%~2"
    set "WDESC=wave%~2, integrated GPU only"
    set "XARGS=--wave-size %~2 --integrated-only"
)
if "%~1"=="smoke" (
    set "FILE=pass7_smoke"
    if not "%~2"=="0" set "FILE=pass7_smoke_igpu_wave%~2"
    set "KARGS=--smoke --dred --algo-file algorithms_smoke.txt --iterations %SMOKE_ITERS%"
) else (
    set "KARGS=--iterations 300"
)
set "LAST_LABEL=!RUNNO!/!NRUNS!: %~1 pass7 !WDESC!"
set CMD="%EXE%" --shaders "%P7DIR%" !KARGS! !XARGS! !NPARG! --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
call :exec
exit /b 0

rem :final_run <smoke|full> <wave size, 0 = default>: one GpuSort run of shaders\algorithms_final.txt
rem (final mode); a wave size means --wave-size <n>, plus --discrete-only unless igpu64. Sets RC,
rem RC_TEXT, LAST_LABEL, FILE.
:final_run
set /a RUNNO+=1
set "FILE=final"
set "WDESC=default wave size, every GPU"
set "XARGS="
if not "%~2"=="0" (
    set "FILE=final_wave%~2"
    if defined IGPU64 (
        set "WDESC=wave%~2, every GPU with a wave size range"
        set "XARGS=--wave-size %~2"
    ) else (
        set "WDESC=wave%~2, discrete GPU only"
        set "XARGS=--wave-size %~2 --discrete-only"
    )
)
if "%~1"=="smoke" (
    set "FILE=final_smoke"
    if not "%~2"=="0" set "FILE=final_smoke_wave%~2"
    set "KARGS=--smoke --dred --algo-file %FSET% --iterations %SMOKE_ITERS%"
) else (
    set "KARGS=--algo-file %FSET% --iterations %F_ITERS% --iterations-integrated %F_ITERS_I%"
)
set "LAST_LABEL=!RUNNO!/!NRUNS!: %~1 final !WDESC!"
set CMD="%EXE%" --shaders "%FDIR%" !KARGS! !XARGS! !NPARG! --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
call :exec
exit /b 0

rem :scale_run <smoke|full>: one GpuSort run of shaders\algorithms_scale.txt at every sort count of
rem S_SORTS (scale mode), default wave size, every GPU. Sets RC, RC_TEXT, LAST_LABEL, FILE.
:scale_run
set /a RUNNO+=1
if "%~1"=="smoke" (
    set "FILE=scale_smoke"
    set "KARGS=--smoke --dred --algo-file %SSET% --sorts %S_SORTS% --iterations %S_SMOKE_ITERS%"
) else (
    set "FILE=scale"
    set "KARGS=--algo-file %SSET% --sorts %S_SORTS% --iterations %S_ITERS% --iterations-integrated %S_ITERS_I%"
)
set "LAST_LABEL=!RUNNO!/!NRUNS!: %~1 scale %S_SORTS% sorts, default wave size, every GPU"
set CMD="%EXE%" --shaders "%SDIR%" !KARGS! !NPARG! --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
call :exec
exit /b 0

rem :run <smoke|full> <set index> <wave index>: one GpuSort run; sets RC, RC_TEXT, LAST_LABEL.
:run
set /a RUNNO+=1
set "KIND=%~1"
set "SNAME=!SET_%~2_NAME!"
set "SDIR=!SET_%~2_DIR!"
set "WS=!WAVE_%~3!"
set "FILE=!SNAME!"
set "WDESC=default wave size"
set "WARG="
if not "!WS!"=="0" (
    set "FILE=!FILE!_wave!WS!"
    set "WDESC=wave!WS!"
    set "WARG=--wave-size !WS!"
)
set "KARGS="
if "!KIND!"=="smoke" (
    set "FILE=smoke_!FILE!"
    set "KARGS=--smoke --dred --iterations %SMOKE_ITERS%"
)
set "LAST_LABEL=!RUNNO!/!NRUNS!: !KIND! !SNAME! !WDESC!"
set CMD="%EXE%" --shaders "!SDIR!" !KARGS! !WARG! !NPARG! --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
rem :exec: runs CMD (or prints it in a dry run) for FILE / LAST_LABEL; sets RC and RC_TEXT, adds the
rem summary line and collects the run's wave probe lines. (Also called directly by the diag mode.)
:exec
echo.
echo ==== Run !LAST_LABEL! ====
if defined DRYRUN goto :run_dry
!CMD!
set "RC=!ERRORLEVEL!"
goto :run_done
:run_dry
echo [DRYRUN] !CMD!
set "RC=0"
if "!DRYRUN_FAIL!"=="!RUNNO!" (
    if defined DRYRUN_FAIL_RC (set "RC=!DRYRUN_FAIL_RC!") else (set "RC=1")
)
:run_done
set "RC_TEXT=unexpected exit code"
if "!RC!"=="0" set "RC_TEXT=ok"
if "!RC!"=="1" set "RC_TEXT=verification failures or errors, see !FILE!.log"
if "!RC!"=="2" set "RC_TEXT=cancelled at the prompt"
if "!RC!"=="3" set "RC_TEXT=DEVICE LOST - GPU fault or hang, see !FILE!.log"
echo Run !LAST_LABEL! finished: exit code !RC! ^(!RC_TEXT!^)
>> "%SUMMARY%" echo [!LAST_LABEL!] exit code !RC! (!RC_TEXT!) -^> !FILE!.txt / !FILE!.log
rem Every GPU run starts with GpuSort's wave probe (lane count / lane mapping / cross-lane checks per
rem GPU, in the .txt header). Collect those lines in wave_probe.txt; warnings also go to summary.txt.
if exist "%OUT%\!FILE!.txt" (
    >> "%OUT%\wave_probe.txt" echo [!LAST_LABEL!] !FILE!.txt
    findstr /i /c:"wave probe" "%OUT%\!FILE!.txt" >> "%OUT%\wave_probe.txt"
    findstr /c:"WAVE PROBE WARNING" "%OUT%\!FILE!.txt" >> "%SUMMARY%"
)
exit /b 0

:end
if not defined NOPAUSE (
    echo.
    pause
)
endlocal & exit /b %FINAL_RC%
