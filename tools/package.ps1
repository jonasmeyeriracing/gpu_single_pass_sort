<#
.SYNOPSIS
    Builds GpuSort (Release x64) and creates dist\GpuSort-portable-<gitshortsha>.zip, a
    self-contained package for running the benchmark on another machine (see README_PORTABLE.txt).

.DESCRIPTION
    Package contents:
      GpuSort.exe (static CRT), dxcompiler.dll, dxil.dll
      shaders\                  the current shaders (the whole folder, incl. algorithms_diag_flush.txt
                                for run_all.bat diag)
      _test\passN\              every snapshot's shader files + algorithms.txt + notes.md
                                (no results.txt / smoke_results.txt / algorithms_diag.txt or other
                                algorithm lists: run_all.bat only uses algorithms.txt);
                                SAME_AS_SHADERS.txt marks a snapshot identical to shaders\ so
                                run_all.bat skips it
      run_all.bat, README_PORTABLE.txt, CSV_FORMAT.md (the results CSV columns), package_info.txt

.PARAMETER NoBuild
    Package the existing bin\Release build instead of building first.
#>
param(
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $repo 'bin\Release'

function Find-VsTool([string]$pattern, [string]$fallback)
{
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere)
    {
        $found = & $vswhere -latest -products * -find $pattern 2>$null | Select-Object -First 1
        if ($found) { return $found }
    }
    if ($fallback -and (Test-Path $fallback)) { return $fallback }
    return $null
}

# --- build ------------------------------------------------------------------------------------
if (-not $NoBuild)
{
    $msbuild = Find-VsTool 'MSBuild\**\Bin\amd64\MSBuild.exe' `
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe'
    if (-not $msbuild) { throw 'MSBuild.exe not found (Visual Studio 2022 with C++ is required to build)' }
    Write-Host "Building Release x64 with $msbuild"
    & $msbuild (Join-Path $repo 'GpuSort.sln') /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit code $LASTEXITCODE)" }
}
foreach ($f in 'GpuSort.exe', 'dxcompiler.dll', 'dxil.dll')
{
    if (-not (Test-Path (Join-Path $binDir $f))) { throw "missing $binDir\$f (build first)" }
}

# --- dependency check: only system DLLs + dxcompiler.dll (no VC++ redistributable) -------------
$dumpbin = Find-VsTool 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' $null
if ($dumpbin)
{
    $deps = & $dumpbin /nologo /dependents (Join-Path $binDir 'GpuSort.exe') |
        ForEach-Object { $_.Trim() } | Where-Object { $_ -match '\.dll$' }
    Write-Host "GpuSort.exe depends on: $($deps -join ', ')"
    $bad = $deps | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbased|concrt|vccorlib)' }
    if ($bad) { throw "GpuSort.exe depends on the VC++ runtime ($($bad -join ', ')); build Release with /MT" }
}
else
{
    Write-Warning 'dumpbin.exe not found; skipping the dependency check'
}

# --- version info -----------------------------------------------------------------------------
$sha = 'nogit'
$gitInfo = @()
if (Get-Command git -ErrorAction SilentlyContinue)
{
    $sha = (& git -C $repo rev-parse --short HEAD).Trim()
    $branch = (& git -C $repo rev-parse --abbrev-ref HEAD).Trim()
    $subject = (& git -C $repo log -1 --format='%ci %s').Trim()
    $dirty = & git -C $repo status --porcelain -- src shaders _test GpuSort.vcxproj tools
    $gitInfo += "Git commit:   $sha ($branch) $subject"
    if ($dirty)
    {
        $sha += '-dirty'
        $gitInfo += 'Uncommitted changes (included in this package):'
        $gitInfo += ($dirty | ForEach-Object { "    $_" })
    }
}

$name = "GpuSort-portable-$sha"
$distDir = Join-Path $repo 'dist'
$stage = Join-Path $distDir $name
$zip = Join-Path $distDir "$name.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage | Out-Null

# --- copy -------------------------------------------------------------------------------------
foreach ($f in 'GpuSort.exe', 'dxcompiler.dll', 'dxil.dll')
{
    Copy-Item (Join-Path $binDir $f) $stage
}

# Text files the .bat depends on get CRLF line endings whatever the checkout uses.
function Copy-WithCrlf([string]$src, [string]$dst)
{
    $text = [IO.File]::ReadAllText($src)
    $text = ($text -replace "`r`n", "`n") -replace "`n", "`r`n"
    [IO.File]::WriteAllText($dst, $text, (New-Object Text.UTF8Encoding($false)))
}
Copy-WithCrlf (Join-Path $PSScriptRoot 'run_all.bat') (Join-Path $stage 'run_all.bat')
Copy-WithCrlf (Join-Path $PSScriptRoot 'README_PORTABLE.txt') (Join-Path $stage 'README_PORTABLE.txt')
Copy-WithCrlf (Join-Path $PSScriptRoot 'CSV_FORMAT.md') (Join-Path $stage 'CSV_FORMAT.md')

$shaderPatterns = @('*.hlsl', '*.hlsli', 'algorithms.txt')
function Test-NameLike([string]$name, [string[]]$patterns)
{
    foreach ($pat in $patterns) { if ($name -like $pat) { return $true } }
    return $false
}

# Shader files of a directory: relative name -> SHA256, for the "identical to shaders\" check.
function Get-ShaderHashes([string]$dir)
{
    $h = @{}
    Get-ChildItem -Path $dir -File | Where-Object { Test-NameLike $_.Name $shaderPatterns } | ForEach-Object { $h[$_.Name] = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash }
    return $h
}
function Test-SameHashes($a, $b)
{
    if ($a.Count -ne $b.Count) { return $false }
    foreach ($k in $a.Keys) { if (-not $b.ContainsKey($k) -or $b[$k] -ne $a[$k]) { return $false } }
    return $true
}

$srcShaders = Join-Path $repo 'shaders'
Copy-Item -Recurse $srcShaders (Join-Path $stage 'shaders')
$currentHashes = Get-ShaderHashes $srcShaders

$snapshotLines = @()
$testDir = Join-Path $repo '_test'
$passes = @()
if (Test-Path $testDir)
{
    $passes = Get-ChildItem -Path $testDir -Directory | Where-Object { $_.Name -match '^pass(\d+)$' } |
        Sort-Object { [int]($_.Name -replace '^pass', '') }
}
foreach ($p in $passes)
{
    if (-not (Test-Path (Join-Path $p.FullName 'algorithms.txt'))) { continue }
    $dst = Join-Path $stage "_test\$($p.Name)"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Get-ChildItem -Path $p.FullName -File | Where-Object { Test-NameLike $_.Name ($shaderPatterns + 'notes.md') } |
        Copy-Item -Destination $dst
    $same = Test-SameHashes $currentHashes (Get-ShaderHashes $p.FullName)
    if ($same)
    {
        Set-Content -Path (Join-Path $dst 'SAME_AS_SHADERS.txt') -Encoding ASCII -Value `
            "The shader files and algorithms.txt of $($p.Name) are identical to shaders\ in this package; run_all.bat skips it."
    }
    $snapshotLines += "    _test\$($p.Name)$(if ($same) { '   (identical to shaders\, skipped by run_all.bat)' })"
}

$info = @(
    "GpuSort portable package $name",
    "Built:        $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') on $env:COMPUTERNAME"
) + $gitInfo + @(
    'Shader sets:',
    '    shaders\     (current)'
) + $snapshotLines
Set-Content -Path (Join-Path $stage 'package_info.txt') -Encoding ASCII -Value $info

# --- zip --------------------------------------------------------------------------------------
Compress-Archive -Path $stage -DestinationPath $zip -Force
Remove-Item -Recurse -Force $stage

$size = (Get-Item $zip).Length
Write-Host ''
$info | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ("Package: {0} ({1:N1} MB)" -f $zip, ($size / 1MB))
