<#
.SYNOPSIS
  Run one or more validation arms of docs/SPEC_prefit_census_observability.md.

.DESCRIPTION
  Re-seeds each arm's state snapshot from the archive BEFORE the run (the
  producer overwrites state_snapshot_path at its first hour boundary, so a
  re-run without re-seeding silently resumes from the previous run's output),
  then runs the RELEASE producer with the run directory as the working
  directory so every trace lands under runs/<arm>/logs.

  Release-only by standing directive; the exe is a renamed copy so a concurrent
  session's link never hits a locked file.

.EXAMPLE
  pwsh scripts/pc_run_arms.ps1 -Arms census,topo
#>
param(
    [string[]] $Arms = @("pcbase", "rhsat2", "census", "topo", "trip", "fuse", "birth18"),
    [string]   $Exe  = "C:\GnssProcessor\build\x64-Release\gnss_processor_pc.exe"
)

$ErrorActionPreference = "Stop"
$root = "C:\GnssProcessor"

# Arm -> snapshot index (matches scripts/pc_make_arms.py).
$seed = @{
    pcbase = 20; rhsat2 = 20; census = 20; topo = 20; trip = 20; fuse = 20
    birth18 = 19; smokeoff = 20; smokeon = 20
}

foreach ($arm in $Arms) {
    $cfg = Join-Path $root "config\test_suite\_ft63_leg19_pc_$arm.json"
    $dir = Join-Path $root "runs\ft63_leg19_pc_$arm"
    if (-not (Test-Path $cfg)) { throw "missing config $cfg (run scripts/pc_make_arms.py)" }
    if (-not $seed.ContainsKey($arm)) { throw "no seed snapshot registered for arm '$arm'" }

    $idx = "{0:d2}" -f $seed[$arm]
    $src = Get-ChildItem (Join-Path $root "runs\ft63_24h_allon\snap_archive") -Filter "snap_${idx}_*.bin" |
           Sort-Object Name | Select-Object -First 1
    if (-not $src) { throw "no snap_${idx}_*.bin in the archive" }

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Get-ChildItem $dir -Filter "logs" -Directory -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
    Copy-Item $src.FullName (Join-Path $dir "state_snapshot.bin") -Force

    Write-Output "=== $arm  (seed $($src.Name)) ==="
    $t0 = Get-Date
    Push-Location $dir
    try {
        & $Exe $cfg *> "run.log"
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    Write-Output "    exit=$code  ${secs}s  -> runs\ft63_leg19_pc_$arm\run.log"
}
