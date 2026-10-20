<#
.SYNOPSIS
  Sequential overnight arm queue for the FT63 convergence lineage.

.DESCRIPTION
  Waits for the 24 h parent run (runs/ft63_24h_conv) to exit, generates the
  follow-on arm configs (scripts/overnight_make_arms.py -- the resume arms need
  the parent's hourly warm-start entries, so they cannot be built earlier),
  then runs each arm in turn with the run directory as the working directory so
  every trace lands under runs/<arm>/logs.

  Release-only by standing directive; the exe is a renamed copy so a concurrent
  session's link never hits a locked file.  One arm's failure never stops the
  queue -- each result is appended to runs/overnight_queue_summary.txt as it
  lands, so a partially-completed night is still readable.

  The two `*_smoke` arms are 10-epoch gates: if a smoke fails, its long arm is
  SKIPPED and the queue moves on (staged-cadence directive).

.NOTES
  Order is deliberate: the two cheap, certain results (warm-start convergence,
  dual-layer iono) are banked first, then the long multi-GNSS pole, and the
  305-station scale check last because it is the most expensive per unit of
  information and the most acceptable to leave unfinished.
#>
param(
    [string] $Exe = "C:\GnssProcessor\build\x64-Release\gnss_processor_ovn.exe"
)

$ErrorActionPreference = "Continue"
$root    = "C:\GnssProcessor"
$summary = Join-Path $root "runs\overnight_queue_summary.txt"

function Note($msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $msg
    Write-Output $line
    Add-Content -Path $summary -Value $line
}

# ---- 1. wait for the parent 24 h run ---------------------------------------
Note "queue armed; waiting for ft63_24h_conv to finish"
while (Get-Process gnss_processor_conv24 -ErrorAction SilentlyContinue) {
    Start-Sleep -Seconds 30
}
Note "parent run exited; generating arm configs"

# ---- 2. generate the arms (resume arms need the parent's snapshots) --------
Push-Location $root
try { & python scripts\overnight_make_arms.py 2>&1 | ForEach-Object { Note "  gen: $_" } }
finally { Pop-Location }

# ---- 3. run the queue ------------------------------------------------------
# name -> the smoke arm that gates it (empty = no gate)
$arms = @(
    @{ name = "ft63_warm12";      gate = "" },
    @{ name = "ft63_cold12";      gate = "" },
    @{ name = "ft63_l2off_smoke"; gate = "" },
    @{ name = "ft63_l2off";       gate = "ft63_l2off_smoke" },
    @{ name = "ft63_gg_smoke";    gate = "" },
    @{ name = "ft63_gg12h";       gate = "ft63_gg_smoke" },
    @{ name = "net305_screen";    gate = "" }
)

$results = @{}

foreach ($arm in $arms) {
    $name = $arm.name
    $cfg  = Join-Path $root "config\test_suite\_$name.json"
    $dir  = Join-Path $root "runs\$name"

    if ($arm.gate -and $results[$arm.gate] -ne 0) {
        Note "SKIP $name -- its gate ($($arm.gate)) did not pass"
        continue
    }
    if (-not (Test-Path $cfg)) { Note "SKIP $name -- no config at $cfg"; continue }

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (Test-Path "$dir\logs") { Remove-Item -Recurse -Force "$dir\logs" }
    # NOTE: the warm arm's seed snapshot was just placed by the generator --
    # do NOT clear state_snapshot.bin here or the resume has nothing to read.

    Note "START $name"
    $t0 = Get-Date
    Push-Location $dir
    try {
        & $Exe $cfg *> "run.log"
        $code = $LASTEXITCODE
    } catch {
        $code = -999
    } finally { Pop-Location }
    $mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
    $results[$name] = $code

    $ep = 0
    $st = Join-Path $dir "logs\network_epoch_status.csv"
    if (Test-Path $st) { $ep = (Get-Content $st | Measure-Object -Line).Lines - 1 }
    Note "DONE  $name  exit=$code  ${mins} min  epochs=$ep"
}

Note "queue complete"
