<#
.SYNOPSIS
  Run the D5 churn-card arms (docs/SPEC_d5_churn_limit_cycle.md).

.DESCRIPTION
  Three COLD arms over the same 63-station DOY-129 base, differing by exactly one
  key each: base (D5 v2, as ft63_24h_conv ran), v3 (+corroborate), v4
  (+retap_only).  Cold by construction, so unlike scripts/pc_run_arms.ps1 there
  is no snapshot to re-seed -- but the previous run's snapshot IS deleted first,
  because the producer writes state_snapshot_path at every hour boundary and a
  re-run would otherwise silently WARM-RESUME off its own last output and read as
  a clean cold start (the exact silent-failure the promotion ledger's CHAIN axis
  warns about).

  Release-only by standing directive.  The exe is a renamed copy so a concurrent
  session's link never hits a locked file.

.EXAMPLE
  pwsh scripts/churn_run_arms.ps1 -Arms base,v3,v4 -Smoke
  pwsh scripts/churn_run_arms.ps1 -Arms base,v3,v4
#>
param(
    [string[]] $Arms  = @("base", "v3", "v4"),
    [switch]   $Smoke,
    [string]   $Exe   = "C:\GnssProcessor\build\x64-Release\gnss_processor_churn.exe"
)

$ErrorActionPreference = "Stop"
$root = "C:\GnssProcessor"
$sfx  = if ($Smoke) { "_smoke" } else { "" }

if (-not (Test-Path $Exe)) { throw "missing exe $Exe (copy it from gnss_processor.exe)" }

foreach ($arm in $Arms) {
    $cfg = Join-Path $root "config\test_suite\_ft63_churn_$arm$sfx.json"
    $dir = Join-Path $root "runs\churn_$arm$sfx"
    if (-not (Test-Path $cfg)) { throw "missing config $cfg" }

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    # Wipe BOTH the traces and every snapshot: a stale logs/ dir makes an A/B
    # compare read files the run never wrote, and a stale snapshot warm-resumes.
    Get-ChildItem $dir -Filter "logs" -Directory -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
    Get-ChildItem $dir -Filter "state_snapshot.bin*" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force

    Write-Output "=== churn_$arm$sfx ==="
    $t0 = Get-Date
    Push-Location $dir
    try {
        & $Exe $cfg *> "run.log"
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    Write-Output "    exit=$code  ${secs}s  -> runs\churn_$arm$sfx\run.log"
}
