<#
.SYNOPSIS
  Configure / build GnssProcessor with Visual Studio 2026 (Enterprise, v18).

.DESCRIPTION
  Replaces the old VS 2022 vcvars64 pattern. Two things changed with the
  2026 upgrade, and BOTH matter:

    1. vcvars64.bat moved:
         2022: ...\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\
         2026: ...\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\
       ("18" is the install *major version* dir, not a year.)

    2. `cmake` on PATH is the standalone 3.18.3 in C:\Program Files\CMake,
       which vcvars does NOT override. VS 2026 bundles CMake 4.3.1 and its
       own Ninja. This script pins the BUNDLED cmake by absolute path so the
       CLI matches what the IDE uses -- otherwise a CLI configure and an IDE
       configure disagree on generator internals and thrash the build tree.

  Configure args mirror CMakeSettings.json exactly (vcpkg toolchain +
  pinned Python 3.13), so a CLI-configured tree and an IDE-configured tree
  are interchangeable.

.EXAMPLE
  # compile check (Debug is for compile checks and gnss_tests ONLY -- never data runs)
  .\scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_core

  # full Debug build + configure if needed
  .\scripts\vs2026_build.ps1 -Config x64-Debug

  # Release, for every data run
  .\scripts\vs2026_build.ps1 -Config x64-Release -Target gnss_processor

  # force a fresh configure (after a toolchain change)
  .\scripts\vs2026_build.ps1 -Config x64-Debug -Reconfigure
#>
[CmdletBinding()]
param(
    [ValidateSet('x64-Debug','x64-Release')]
    [string]$Config = 'x64-Debug',
    [string]$Target,
    [switch]$Reconfigure,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'

$vsRoot  = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise'
$vcvars  = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
$cmakeEx = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaEx = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

foreach ($p in @($vcvars, $cmakeEx)) {
    if (-not (Test-Path $p)) { throw "Not found: $p  (is VS 2026 Enterprise installed?)" }
}

$proj      = 'C:\GnssProcessor'
$buildRoot = "$proj\build\$Config"
$buildType = if ($Config -eq 'x64-Debug') { 'Debug' } else { 'Release' }

# Mirrors CMakeSettings.json "cmakeCommandArgs" verbatim.
$cfgArgs = @(
    '-G','Ninja'
    "-DCMAKE_BUILD_TYPE=$buildType"
    '-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake'
    '-DVCPKG_TARGET_TRIPLET=x64-windows'
    '-DPython_EXECUTABLE=C:/Python313/python.exe'
    '-DPython_ROOT_DIR=C:/Python313'
    '-DPython_INCLUDE_DIRS=C:/Python313/include'
    '-DPython_LIBRARY_DIRS=C:/Python313/libs'
    '-DPython_LIBRARIES=C:/Python313/libs/python313.lib'
    '-DPython_FIND_STRATEGY=LOCATION'
    '-DPython_FIND_REGISTRY=NEVER'
)

if ($Reconfigure -and (Test-Path "$buildRoot\CMakeCache.txt")) {
    Write-Host "Removing stale cache: $buildRoot\CMakeCache.txt" -ForegroundColor Yellow
    Remove-Item "$buildRoot\CMakeCache.txt" -Force
    Remove-Item "$buildRoot\CMakeFiles" -Recurse -Force -ErrorAction SilentlyContinue
}

$needConfigure = -not (Test-Path "$buildRoot\CMakeCache.txt")

# Everything runs inside ONE cmd invocation so the vcvars environment
# (INCLUDE/LIB/PATH for cl.exe) survives into cmake.
#
# Do NOT try to prepend to PATH here: cmd expands %PATH% when it PARSES the
# whole `a && b && c` line, i.e. BEFORE vcvars runs, so `set PATH=x;%PATH%`
# writes back the *pre-vcvars* PATH and silently destroys the compiler
# entries vcvars just added -- yielding "No CMAKE_CXX_COMPILER could be
# found".
#
# Ninja needs no pinning: vcvars64 already puts VS 2026's own ninja (1.13.2)
# on PATH, so it is toolchain-consistent by construction. Passing it via
# -DCMAKE_MAKE_PROGRAM instead would need the space in "C:\Program Files"
# quoted through both PowerShell and cmd -- unquoted it truncates to
# 'C:\Program' and CMake reports the make program missing.
$inner = @()
$inner += "`"$vcvars`" >nul"
if ($needConfigure) {
    $inner += "`"$cmakeEx`" -S `"$proj`" -B `"$buildRoot`" $($cfgArgs -join ' ')"
}
if (-not $ConfigureOnly) {
    $buildCmd = "`"$cmakeEx`" --build `"$buildRoot`""
    if ($Target) { $buildCmd += " --target $Target" }
    $inner += $buildCmd
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
Write-Host "=== VS 2026 ($Config)$(if($needConfigure){' [configuring]'})$(if($Target){" target=$Target"}) ===" -ForegroundColor Cyan
cmd /c ($inner -join ' && ') 2>&1
$rc = $LASTEXITCODE
$sw.Stop()

$color = if ($rc -eq 0) { 'Green' } else { 'Red' }
Write-Host ("=== exit $rc  ({0:N0}s) ===" -f $sw.Elapsed.TotalSeconds) -ForegroundColor $color
exit $rc
