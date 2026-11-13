<#
.SYNOPSIS
  Configure / build GnssProcessor with Visual Studio 2026 (Community, v18),
  compiler pinned to MSVC 14.51.

.DESCRIPTION
  Replaces the old VS 2022 vcvars64 pattern. Three things matter:

    1. vcvars64.bat location:
         2022: ...\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\
         2026: ...\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\
       ("18" is the install *major version* dir, not a year.) Enterprise was
       used until 2026-11-12; Community carries the same compiler.

    2. `cmake` on PATH is the standalone 3.18.3 in C:\Program Files\CMake,
       which vcvars does NOT override. VS 2026 bundles CMake 4.3.1 and its
       own Ninja. This script pins the BUNDLED cmake by absolute path so the
       CLI matches what the IDE uses -- otherwise a CLI configure and an IDE
       configure disagree on generator internals and thrash the build tree.

    3. The compiler is pinned: vcvars64 runs with -vcvars_ver=14.51, the
       compiler every baseline since 2026-08-12 was built with (cl 19.51.36263).
       The installer's default toolset is newer (14.52 in VS 18.12); moving to
       it changes outputs, so it is a deliberate, measured step, not an
       install side effect. A tree configured with any other compiler (an IDE
       configure, the old Enterprise path) is reconfigured automatically.

  Configure args mirror CMakeSettings.json exactly (vcpkg toolchain +
  pinned Python 3.13). The IDE configures its own trees (build\ide-x64-Debug,
  build\ide-x64-Release, set in CMakeSettings.json): CMakeSettings.json cannot
  pin the compiler version, so an IDE configure of these trees would move them
  to the installer's default compiler.

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

$vsRoot  = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$msvcVer = '14.51'   # pinned compiler (see .DESCRIPTION item 3)
$vcvars  = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
$cmakeEx = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaEx = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

foreach ($p in @($vcvars, $cmakeEx)) {
    if (-not (Test-Path $p)) { throw "Not found: $p  (is VS 2026 Community installed?)" }
}
if (-not (Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Directory -Filter "$msvcVer.*" -ErrorAction SilentlyContinue)) {
    throw "MSVC $msvcVer is not installed in $vsRoot. Add 'MSVC Build Tools v$msvcVer for x64/x86' (Visual Studio Installer: Modify, Individual components)."
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

# A tree configured with another compiler cannot be reused: mixing compilers in
# one tree fails to configure or leaves objects from the other compiler.
$cache = "$buildRoot\CMakeCache.txt"
if (-not $Reconfigure -and (Test-Path $cache)) {
    $hit = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:\w+=(.*)$' | Select-Object -First 1
    $have = if ($hit) { $hit.Matches[0].Groups[1].Value } else { '' }
    $want = ("$vsRoot\VC\Tools\MSVC\$msvcVer." -replace '\\', '/')
    if (-not $have.StartsWith($want, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "Tree was configured with another compiler ($have); reconfiguring for MSVC $msvcVer" -ForegroundColor Yellow
        $Reconfigure = $true
    }
}

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
$inner += "`"$vcvars`" -vcvars_ver=$msvcVer >nul"
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
