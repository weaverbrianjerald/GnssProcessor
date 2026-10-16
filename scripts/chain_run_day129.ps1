<#
  DOY-129 continuous 24 h network run — supervisor.

  Runs the 305-station PPP-RTK network producer from the 6 h converged snapshot
  through to the end of GPS day 2024-129 (23:59:30, tow 345570).

  Strategy: ONE continuous segment, not a chain of hourly legs.  Every leg seam
  costs a measured 162 m rms_clock_m spike decaying over ~20 epochs plus a
  permanent ~5 m step against a continuous reference, so seams are minimised
  rather than scheduled.  Crash recovery does not need seams: the producer
  writes its state snapshot EVERY emitted epoch (state_snapshot_interval_s 10
  vs 30 s epochs), so a killed run loses at most one epoch.

  If a segment dies for any reason, the supervisor derives the next segment's
  start_offset_hours from the snapshot the dead segment actually reached and
  relaunches.  Each segment gets its own working directory because the producer
  truncates every log/CSV and deletes the whole logs dir at startup
  (LogManager.cpp:127-130) — a shared CWD would destroy prior diagnostics.

  Stateless across invocations: re-running this script picks up from the newest
  segment snapshot on disk.  Safe to run after a reboot.

  Usage:  pwsh -File scripts/chain_run_day129.ps1 [-RunRoot <dir>] [-MaxSegments 12]
#>
[CmdletBinding()]
param(
  [string]$RunRoot     = "C:\GnssProcessor\runs\day129_cont",
  [string]$Seed        = "C:\GnssProcessor\val_out\night305_conv_immutable.txt",
  [string]$BaseConfig  = "C:\GnssProcessor\config\test_suite\_night305_resume.json",
  [string]$Exe         = "C:\GnssProcessor\build\x64-Release\gnss_processor_chain.exe",
  [string]$ExeSource   = "C:\GnssProcessor\build\x64-Release\gnss_processor.exe",
  [int]   $MaxSegments = 12,
  [double]$EndTow      = 345570.0   # 23:59:30 — the day's last epoch
)

$ErrorActionPreference = "Stop"
$repo = "C:\GnssProcessor"
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null
$chainLog = Join-Path $RunRoot "chain.log"

function Say([string]$m) {
  $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
  $line = "[$ts] $m"
  Write-Host $line
  Add-Content -Path $chainLog -Value $line
}

# Snapshot M/N header -> object.  Authoritative counter ledger; exit code alone
# does NOT prove a segment did work (n_emitted is restored from the snapshot on
# resume, so the zero-emit guard at Teardown.cpp:1059 is unreachable).
#
# NB: read with Get-Content -TotalCount, which opens and CLOSES the file.  Do NOT
# use [System.IO.File]::ReadLines() here -- breaking out of that lazy enumeration
# leaks the StreamReader handle into this (long-lived) supervisor process, and the
# producer's per-epoch atomic replace then fails with "Access is denied" on EVERY
# epoch.  The run keeps emitting products but never checkpoints, so a crash loses
# the entire run.  Cost that bug 12 minutes of a 22 h run to find.
function Get-SnapHeader([string]$path) {
  if (-not (Test-Path $path)) { return $null }
  $m = $null; $n = $null
  # M and N are lines 2-3 (after the "# gnss_network_state_snapshot v1" banner).
  foreach ($line in (Get-Content -Path $path -TotalCount 8 -ErrorAction SilentlyContinue)) {
    if     ($line.StartsWith("M,")) { $m = $line.Split(",") }
    elseif ($line.StartsWith("N,")) { $n = $line.Split(",") }
  }
  if (-not $m -or -not $n) { return $null }
  [pscustomobject]@{
    Week        = [int]$m[1]
    Tow         = [double]$m[2]
    Fingerprint = $m[5]
    NEmitted    = [int]$n[1]
    NSkipped    = [int]$n[2]
    GateWithheld= [int]$n[3]
    Runaway     = [int]$n[5]
  }
}

# ---- pre-flight -----------------------------------------------------------
Say "=== DOY-129 continuous run supervisor ==="

# Name-scoped zombie pre-kill (CLAUDE.md standing rule).
$zombies = Get-Process -Name "gnss_processor_chain" -ErrorAction SilentlyContinue
if ($zombies) { Say "pre-kill: $($zombies.Count) stale gnss_processor_chain"; $zombies | Stop-Process -Force }

# Release-only, renamed copy so a concurrent rebuild cannot lock the exe.
if (-not (Test-Path $Exe)) {
  Copy-Item $ExeSource $Exe -Force
  Say "created renamed exe $Exe"
}
$hExe = (Get-FileHash $Exe -Algorithm SHA256).Hash
$hSrc = (Get-FileHash $ExeSource -Algorithm SHA256).Hash
if ($hExe -ne $hSrc) {
  Copy-Item $ExeSource $Exe -Force
  $hExe = (Get-FileHash $Exe -Algorithm SHA256).Hash
  Say "exe refreshed from source (hash drift)"
}
Say "exe $Exe sha256=$($hExe.Substring(0,16))"

# ---- resume point: newest segment snapshot, else the seed -----------------
$segDirs = Get-ChildItem -Path $RunRoot -Directory -Filter "seg*" -ErrorAction SilentlyContinue |
           Sort-Object Name
$current = $null; $segIndex = 0
foreach ($d in $segDirs) {
  $h = Get-SnapHeader (Join-Path $d.FullName "snap.txt")
  if ($h -and ($null -eq $current -or $h.Tow -gt $current.Tow)) { $current = $h }
  $segIndex = [int]($d.Name -replace "seg", "")
}
if ($current) {
  Say "resuming chain from seg$('{0:d2}' -f $segIndex) snapshot: tow=$($current.Tow) n_emitted=$($current.NEmitted)"
  $seedPath = Join-Path (Join-Path $RunRoot ("seg{0:d2}" -f $segIndex)) "snap.txt"
} else {
  $h0 = Get-SnapHeader $Seed
  if (-not $h0) { Say "FATAL: seed snapshot $Seed unreadable"; exit 2 }
  Say "cold chain start from seed: tow=$($h0.Tow) n_emitted=$($h0.NEmitted) fp=$($h0.Fingerprint)"
  $current = $h0; $seedPath = $Seed
}

# ---- segment loop ---------------------------------------------------------
$consecutiveNoProgress = 0

for ($s = 1; $s -le $MaxSegments; $s++) {
  if ($current.Tow -ge $EndTow) { Say "END OF DAY reached (tow=$($current.Tow) >= $EndTow)"; break }

  $segIndex++
  $legDir = Join-Path $RunRoot ("seg{0:d2}" -f $segIndex)
  Say "--- segment $segIndex -> $legDir  (from tow=$($current.Tow), n_emitted=$($current.NEmitted)) ---"

  # Generate the leg config; the generator also copies the seed snapshot into
  # the leg dir so the leg reads and writes only its OWN file.
  $gen = & python "$repo\scripts\chain_make_leg.py" --snapshot $seedPath --legdir $legDir --base $BaseConfig 2>&1
  $gen | ForEach-Object { Say "  cfg| $_" }
  if ($LASTEXITCODE -ne 0) { Say "FATAL: config generation failed"; exit 3 }

  $before = Get-SnapHeader (Join-Path $legDir "snap.txt")
  $t0 = Get-Date

  $p = Start-Process -FilePath $Exe -ArgumentList "$legDir\leg.json" `
        -WorkingDirectory $legDir -NoNewWindow -PassThru `
        -RedirectStandardOutput "$legDir\stdout.log" `
        -RedirectStandardError  "$legDir\stderr.log"

  # Poll rather than -Wait so we can (a) fail fast if checkpointing is broken and
  # (b) leave a progress trail in chain.log for an unattended multi-hour run.
  # The checkpoint watchdog is the important one: if the per-epoch snapshot write
  # is failing (file locked, permissions, full disk) the run still emits products
  # normally and exits 0 -- but a crash at hour 20 would rewind to hour 6.
  $snapFile   = Join-Path $legDir "snap.txt"
  $seedTow    = $before.Tow
  $checkpointOk = $false
  $lastBeat   = Get-Date
  while (-not $p.HasExited) {
    Start-Sleep -Seconds 30
    $mins = ((Get-Date) - $t0).TotalMinutes
    $h = Get-SnapHeader $snapFile
    if (-not $checkpointOk -and $h -and $h.Tow -gt $seedTow) {
      $checkpointOk = $true
      Say ("  checkpointing CONFIRMED at {0:N1} min (snapshot advanced to tow=$($h.Tow))" -f $mins)
    }
    # Setup (full-day 305-station parse) takes ~8 min; allow 25 before judging.
    if (-not $checkpointOk -and $mins -gt 25) {
      Say "  *** CHECKPOINT WATCHDOG: no snapshot write after $([int]$mins) min ***"
      $why = Select-String -Path (Join-Path $legDir "logs\main_log.log") `
             -Pattern "state-snapshot write to .* failed" -ErrorAction SilentlyContinue |
             Select-Object -Last 1
      if ($why) { Say "  reason| $($why.Line.Trim())" }
      Say "  killing segment — a run that cannot checkpoint must not continue"
      try { $p.Kill() } catch {}
      Start-Sleep -Seconds 5
      exit 9
    }
    if (((Get-Date) - $lastBeat).TotalMinutes -ge 30) {
      $lastBeat = Get-Date
      $ep = @(Select-String -Path (Join-Path $legDir "logs\main_log.log") `
              -Pattern "Network (D0) TOW" -SimpleMatch -ErrorAction SilentlyContinue).Count
      $pos = if ($h) { $h.Tow } else { "?" }
      Say ("  [{0:N0} min] epochs=$ep snapshot_tow=$pos" -f $mins)
    }
  }
  $elapsed = ((Get-Date) - $t0).TotalMinutes
  $after = Get-SnapHeader (Join-Path $legDir "snap.txt")
  $rc = $p.ExitCode

  if (-not $after) { Say "FATAL: segment produced no readable snapshot"; exit 4 }

  $dEmit = $after.NEmitted - $before.NEmitted
  $dTow  = $after.Tow - $before.Tow
  Say ("segment $segIndex rc=$rc elapsed={0:N1} min  d_emitted=$dEmit  d_tow={1:N0}s  -> tow=$($after.Tow)" -f $elapsed, $dTow)

  # Warm-resume verdict: must be WARM, must not be COLD, and the snapshot age
  # must read 30.0 s (anything else means the offset arithmetic slipped).
  $log = Join-Path $legDir "logs\main_log.log"
  if (Test-Path $log) {
    $warm = @(Select-String -Path $log -Pattern "WARM RESUME from" -SimpleMatch -ErrorAction SilentlyContinue).Count
    $cold = @(Select-String -Path $log -Pattern "COLD START"       -SimpleMatch -ErrorAction SilentlyContinue).Count
    $age  = Select-String -Path $log -Pattern "snapshot age ([0-9.]+) s" -ErrorAction SilentlyContinue |
            Select-Object -First 1
    $ageS = if ($age) { $age.Matches[0].Groups[1].Value } else { "?" }
    $dd   = Select-String -Path $log -Pattern "restored invariant DD carry \(([0-9]+) carried DD" -ErrorAction SilentlyContinue |
            Select-Object -First 1
    $ddN  = if ($dd) { $dd.Matches[0].Groups[1].Value } else { "?" }
    Say "  verdict: warm=$warm cold=$cold snapshot_age=${ageS}s carried_DD=$ddN"
    if ($cold -gt 0) { Say "  *** COLD START DETECTED — chain integrity lost, stopping ***"; exit 5 }
    if ($ageS -ne "?" -and [math]::Abs([double]$ageS - 30.0) -gt 0.5) {
      Say "  *** snapshot age $ageS s != 30 s — offset arithmetic slipped, stopping ***"; exit 6
    }
  }

  if ($dEmit -le 0) {
    $consecutiveNoProgress++
    Say "  *** segment made NO progress (d_emitted=$dEmit), attempt $consecutiveNoProgress ***"
    $tail = Get-Content $log -Tail 15 -ErrorAction SilentlyContinue
    $tail | ForEach-Object { Say "  log| $_" }
    if ($consecutiveNoProgress -ge 2) { Say "FATAL: two consecutive no-progress segments; stopping"; exit 7 }
  } else {
    $consecutiveNoProgress = 0
  }

  if ($rc -ne 0) {
    Say "  segment exited rc=$rc (0xC0000409/-1073740791 = uncaught throw, e.g. HB-1)"
    $tail = Get-Content $log -Tail 20 -ErrorAction SilentlyContinue
    $tail | ForEach-Object { Say "  log| $_" }
  }

  $current  = $after
  $seedPath = Join-Path $legDir "snap.txt"

  if ($after.Tow -ge $EndTow) { Say "END OF DAY reached (tow=$($after.Tow))"; break }
  if ($rc -eq 0 -and $dEmit -gt 0) {
    Say "  segment exited cleanly before end of day — relaunching to continue"
  }
}

Say "=== supervisor finished: final tow=$($current.Tow) n_emitted=$($current.NEmitted) ==="
if ($current.Tow -ge $EndTow) { Say "STATUS: COMPLETE — full 24 h of DOY-129 processed"; exit 0 }
Say "STATUS: INCOMPLETE — segment budget exhausted before end of day"
exit 8
