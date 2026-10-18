<#
.SYNOPSIS
  Run the iono-float card arms (qz0, qz0_iofloat) — 2026-08-02.

.DESCRIPTION
  Two COLD arms over the 63-station DOY-129 churn-base regime, one axis each:
  qz0 (carry_invariant_qz -> 0 + D5 v5b, ties tight) and qz0_iofloat (+ ties
  released to 50 TECU, mech-B/layer2 off).  Control is the existing
  runs/churn_v5b (same build, qz=1e-4, ties tight, v5b).

  Same hygiene as churn_run_arms.ps1: previous snapshot deleted first (the
  producer writes state_snapshot_path at each hour boundary — a re-run would
  silently WARM-RESUME off its own last output), logs/ wiped, Release renamed
  exe so a concurrent session's link never hits a locked file.

.EXAMPLE
  pwsh scripts/iofloat_run_arms.ps1 -Arms qz0,qz0_iofloat -Smoke
  pwsh scripts/iofloat_run_arms.ps1 -Arms qz0,qz0_iofloat
#>
param(
    [string[]] $Arms  = @("qz0", "qz0_iofloat"),
    [switch]   $Smoke,
    [string]   $Exe   = "C:\GnssProcessor\build\x64-Release\gnss_processor_fable_iofloat.exe"
)

$ErrorActionPreference = "Stop"
$root = "C:\GnssProcessor"
$sfx  = if ($Smoke) { "_smoke" } else { "" }

if (-not (Test-Path $Exe)) { throw "missing exe $Exe (copy it from gnss_processor_churn.exe — SAME build as the reference runs)" }

foreach ($arm in $Arms) {
    $cfg = Join-Path $root "config\test_suite\_ft63_$arm$sfx.json"
    $dir = Join-Path $root "runs\$arm$sfx"
    if (-not (Test-Path $cfg)) { throw "missing config $cfg" }

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Get-ChildItem $dir -Filter "logs" -Directory -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
    Get-ChildItem $dir -Filter "state_snapshot.bin*" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force

    Write-Output "=== $arm$sfx ==="
    $t0 = Get-Date
    Push-Location $dir
    try {
        & $Exe $cfg *> "run.log"
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    Write-Output "    exit=$code  ${secs}s  -> runs\$arm$sfx\run.log"
}
