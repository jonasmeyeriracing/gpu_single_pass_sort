@echo off
rem GpuSort portable runner: runs the benchmark on this machine and collects everything in
rem results\<COMPUTERNAME>_<yyyymmdd_hhmm>\ (plus a .zip of it) next to this script.
rem
rem   run_all.bat            current shaders\ + the latest _test\passN snapshot
rem   run_all.bat all        current shaders\ + every _test\passN snapshot
rem   run_all.bat current    current shaders\ only
rem   extra argument nopause: do not wait for a key at the end
rem
rem Order: adapter list, then a --smoke --dred safety run of every shader set x wave configuration,
rem then the full runs. Any smoke failure stops the script before the first full run. Every GPU run
rem shows GpuSort's OK/Cancel prompt ("Run k/N ..."); Cancel stops the script.
rem If a GPU reports a wave size range (e.g. AMD RDNA: 32-64), every shader set also runs with
rem --wave-size <max> (e.g. wave64), on the GPUs that support it.
rem
rem Dry run (prints the GpuSort commands instead of running them; for testing this script):
rem   set DRYRUN=1                       required
rem   set DRYRUN_ADAPTERS=<file>         use this file as the --list-adapters output (default: run
rem                                      GpuSort.exe --list-adapters for real; it does no GPU work)
rem   set DRYRUN_FAIL=<k>                pretend run k returns DRYRUN_FAIL_RC (default 1)
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "EXE=%ROOT%GpuSort.exe"
set "MODE=latest"
set "FINAL_RC=0"
set "SKIPPED_SETS="
set "NOPAUSE="
if defined DRYRUN set "NOPAUSE=1"
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="all" (set "MODE=all") else if /i "%~1"=="latest" (set "MODE=latest") else if /i "%~1"=="current" (set "MODE=current") else if /i "%~1"=="nopause" (set "NOPAUSE=1") else (
    echo Unknown argument "%~1". Usage: run_all.bat [latest^|all^|current] [nopause]
    set "NOPAUSE="
    set "FINAL_RC=1"
    goto :end
)
shift
goto :parse_args
:args_done

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

if not defined DRYRUN (
    echo.
    echo Writing dxdiag.txt ^(driver details^) ...
    start "" /wait dxdiag /whql:off /t "%OUT%\dxdiag.txt"
)

rem --- plan: shader sets x wave configurations ----------------------------------------------------
set "SET_COUNT=1"
set "SET_1_NAME=current"
set "SET_1_DIR=%ROOT%shaders"
if /i "%MODE%"=="current" goto :sets_done
set "LATEST="
for /l %%i in (0,1,99) do if exist "%ROOT%_test\pass%%i\algorithms.txt" set "LATEST=%%i"
if not defined LATEST goto :sets_done
if /i "%MODE%"=="all" (
    for /l %%i in (0,1,99) do if exist "%ROOT%_test\pass%%i\algorithms.txt" call :add_set pass%%i
) else (
    call :add_set pass!LATEST!
)
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
echo ==== Plan: %NRUNS% GPU runs, each asks for confirmation first ====
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

rem --- phase 1: smoke tests ----------------------------------------------------------------------
set "FINAL_RC=0"
for /l %%s in (1,1,%SET_COUNT%) do for /l %%w in (1,1,%WAVE_COUNT%) do (
    call :run smoke %%s %%w
    if "!RC!"=="2" (
        set "FINAL_RC=1"
        goto :stopped
    )
    if not "!RC!"=="0" goto :smoke_failed
)

rem --- phase 2: full runs ------------------------------------------------------------------------
for /l %%s in (1,1,%SET_COUNT%) do for /l %%w in (1,1,%WAVE_COUNT%) do (
    call :run full %%s %%w
    if not "!RC!"=="0" set "FINAL_RC=1"
    if "!RC!"=="2" goto :stopped
    if "!RC!"=="3" goto :stopped
)
goto :finish

:smoke_failed
set "FINAL_RC=1"
echo.
echo ************************************************************************************
echo  SMOKE TEST FAILED: run !LAST_LABEL! returned !RC! ^(!RC_TEXT!^).
echo  No full benchmark run was started. The smoke logs in the results folder show
echo  what happened ^(verification failures, DRED report on a GPU fault^).
echo ************************************************************************************
>> "%SUMMARY%" echo STOPPED: smoke test !LAST_LABEL! failed with !RC! (!RC_TEXT!)
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
    set "KARGS=--smoke --dred"
)
set "LAST_LABEL=!RUNNO!/!NRUNS!: !KIND! !SNAME! !WDESC!"
set CMD="%EXE%" --shaders "!SDIR!" !KARGS! !WARG! --label "!LAST_LABEL!" --out "%OUT%\!FILE!.txt" --log "%OUT%\!FILE!.log"
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
exit /b 0

:end
if not defined NOPAUSE (
    echo.
    pause
)
endlocal & exit /b %FINAL_RC%
