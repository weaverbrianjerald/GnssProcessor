# D5 A/B for SPEC_synthetic_minimum_network §13.7 — same-build control + treatment.
# Both arms run from their own cwd because Application::log_dir_ defaults to the
# RELATIVE path "logs", so the run directory is chosen by the working directory.
$ErrorActionPreference = "Stop"
$root = "C:\GnssProcessor"
$exe  = Join-Path $root "build\x64-Release\gnss_processor.exe"
# Snapshot the exe under a run-scoped name so a concurrent rebuild cannot lock or
# swap it mid-run (CLAUDE.md).  Hash BOTH so the two arms are provably one binary.
$snap = Join-Path $root "build\x64-Release\gnss_processor_d5ab.exe"
Copy-Item $exe $snap -Force
Write-Host "exe  sha256: $((Get-FileHash $exe  -Algorithm SHA256).Hash)"
Write-Host "snap sha256: $((Get-FileHash $snap -Algorithm SHA256).Hash)"

foreach ($arm in @(
    @{ dir = "ab_d5_base"; cfg = "_simnet60_tri_slant_trio_480_ab_base.json" },
    @{ dir = "ab_d5_on";   cfg = "_simnet60_tri_slant_trio_480_ab_d5.json"   })) {
    $rd = Join-Path $root ("runs\" + $arm.dir)
    New-Item -ItemType Directory -Force $rd | Out-Null
    $cfgPath = Join-Path $root ("config\test_suite\" + $arm.cfg)
    Write-Host "=== ARM $($arm.dir) : $($arm.cfg) ==="
    Push-Location $rd
    & $snap $cfgPath 2>&1 | Tee-Object -FilePath (Join-Path $rd "run.log") | Select-String -Pattern "invariant #1|carry_invariant|epoch 480|Run complete|ERROR" | Select-Object -First 40
    $code = $LASTEXITCODE
    Pop-Location
    Write-Host "--- $($arm.dir) exit=$code"
}
