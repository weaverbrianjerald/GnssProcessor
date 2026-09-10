<#
.SYNOPSIS
  Worktree-local clone of scripts/vs2026_build.ps1.

.DESCRIPTION
  scripts\vs2026_build.ps1 hard-codes $proj = 'C:\GnssProcessor', so running it
  from a git worktree configures and builds the MAIN tree.  This wrapper is
  byte-identical to it except that the project root is resolved from the
  script's OWN location ($PSScriptRoot\..), so a worktree gets its own
  build\x64-Debug / build\x64-Release.  Everything else -- the vcvars64 path,
  the pinned bundled CMake 4.3.1, the configure args, the "never touch PATH
  inside the cmd chain" rule -- is verbatim from the canonical wrapper.

  NOT a replacement for scripts\vs2026_build.ps1: the main tree keeps using
  that one.  This exists so a worktree agent never writes into
  C:\GnssProcessor\build.
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

foreach ($p in @($vcvars, $cmakeEx)) {
    if (-not (Test-Path $p)) { throw "Not found: $p  (is VS 2026 Enterprise installed?)" }
}

# THE ONE DIFFERENCE from scripts\vs2026_build.ps1: root from $PSScriptRoot.
$proj      = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildRoot = "$proj\build\$Config"
$buildType = if ($Config -eq 'x64-Debug') { 'Debug' } else { 'Release' }

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

# Everything in ONE cmd invocation so the vcvars environment survives into
# cmake.  Do NOT prepend to PATH inside the chain -- cmd expands %PATH% when it
# parses the whole `a && b` line, i.e. BEFORE vcvars runs.
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
Write-Host "=== VS 2026 ($Config) proj=$proj$(if($needConfigure){' [configuring]'})$(if($Target){" target=$Target"}) ===" -ForegroundColor Cyan
cmd /c ($inner -join ' && ') 2>&1
$rc = $LASTEXITCODE
$sw.Stop()

$color = if ($rc -eq 0) { 'Green' } else { 'Red' }
Write-Host ("=== exit $rc  ({0:N0}s) ===" -f $sw.Elapsed.TotalSeconds) -ForegroundColor $color
exit $rc
