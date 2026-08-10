# RealTimeEngine seam-decomposition byte-identity gate.
#
# Runs the arm matrix against ONE named binary and captures every per-epoch
# artifact into runs/rtseam_goldens/<Label>/<arm>/.  Run it once on the pre-cut
# tree (-Label pre) and again after each increment (-Label incN), then diff with
# scripts/rt_seam_compare.py.
#
# -Exe is REQUIRED and has no default ON PURPOSE.  scripts/run_inc4_gate.ps1
# hardcodes gnss_processor_dcmp.exe with no override, which under the
# renamed-exe directive yields a stale-binary FALSE GREEN; this gate refuses to
# guess.  Copy the freshly-linked Release exe to a run-scoped name first:
#
#   Copy-Item build\x64-Release\gnss_processor.exe build\x64-Release\gnss_rtseam.exe
#   scripts\rt_seam_gate.ps1 -Exe build\x64-Release\gnss_rtseam.exe -Label pre
#
# Release ONLY (standing directive): Debug's unoptimized Eigen turns a
# minutes-long run into hours, and verification integers are build-config
# sensitive at the ppm level, so a Debug capture can never be compared against a
# Release one.
param(
    [Parameter(Mandatory = $true)][string] $Exe,
    [Parameter(Mandatory = $true)][string] $Label,
    [string[]] $Arms = @('sbf', 'sbf_bssd', 'sbf_te', 'cl'),
    [string]   $Root = 'C:\GnssProcessor'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { throw "Exe not found: $Exe" }
$exeFull  = (Resolve-Path $Exe).Path
$exeStamp = (Get-Item $exeFull).LastWriteTimeUtc.ToString('o')
Write-Host "Gate binary : $exeFull"
Write-Host "  linked at : $exeStamp"
Write-Host "Label       : $Label"

$outRoot = Join-Path $Root "runs\rtseam_goldens\$Label"
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

# Provenance: which binary produced this capture.  A capture whose exe stamp
# predates the source edit it is supposed to certify is the stale-binary trap.
@{ exe = $exeFull; exe_mtime_utc = $exeStamp; label = $Label
   git_head = (git -C $Root rev-parse HEAD)
   git_dirty = [bool](git -C $Root status --porcelain)
} | ConvertTo-Json | Set-Content (Join-Path $outRoot 'capture_manifest.json')

$logDir = Join-Path $Root 'logs'

foreach ($arm in $Arms) {
    $cfg = Join-Path $Root "config\_rtseam_$arm.json"
    if (-not (Test-Path $cfg)) { throw "Arm config missing: $cfg" }

    Write-Host "`n=== arm $arm ==="
    # Each arm starts from a clean logs/ so a stale file from a previous arm can
    # never be captured as this arm's output (an artifact that silently persists
    # compares identical for the wrong reason).
    if (Test-Path $logDir) { Remove-Item -Recurse -Force $logDir }

    $armOut = Join-Path $outRoot $arm
    New-Item -ItemType Directory -Force -Path $armOut | Out-Null

    # Config is POSITIONAL (src/main.cpp: argv[1]), and log_dir is the relative
    # path "logs" -- so the run must happen from $Root or the artifacts land
    # somewhere the capture never sees.
    Push-Location $Root
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $exeFull $cfg *>&1 | Tee-Object -FilePath (Join-Path $armOut 'console.log') | Out-Null
    $rc = $LASTEXITCODE
    $sw.Stop()
    Pop-Location
    Write-Host ("  exit {0} in {1:N1}s" -f $rc, $sw.Elapsed.TotalSeconds)

    if (Test-Path $logDir) {
        Copy-Item -Recurse -Force (Join-Path $logDir '*') $armOut
    } else {
        Write-Warning "  arm $arm produced NO logs/ directory"
    }

    # A non-zero exit or an empty artifact set is a run failure, not a pass with
    # nothing to compare.  Record it so the comparator can refuse the arm rather
    # than report "0 files differ".
    @{ arm = $arm; exit_code = $rc; seconds = $sw.Elapsed.TotalSeconds
       artifacts = @(Get-ChildItem $armOut -File | ForEach-Object { $_.Name })
    } | ConvertTo-Json | Set-Content (Join-Path $armOut 'arm_status.json')
}

Write-Host "`nCaptured to $outRoot"
