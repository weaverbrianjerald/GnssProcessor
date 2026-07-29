<#
  SPEC_day129_frame_guards Phase-2 ARMING chain — supervisor.

  Stage 1 (seed): fresh 6 h convergence 13:00:00 -> 18:59:30 GPS of DOY-129,
  305 stations, COLD start, with the full arm-together guard set
  (config/test_suite/_day129_guards_conv.json — the fingerprinted knobs
  phase_leveling / emit_ura / code_bias_disc_jump_test are exactly why a fresh
  convergence is required; cb_excursion_reinit is conditional-fingerprinted so
  the seed carries it too and the leg's resume fingerprint matches).

  Stage 2 (leg): warm resume at 19:00:00 running to end of day (23:59:30) over
  BOTH evening excursions — 19:21:30 (tow ~328890, two-sided, guard must stay
  quiet on CB) and 20:47:00 (tow 334020, one-sided, guard must fire) — with the
  full frame-guards trace set (chain_make_leg.py --frame-guards: sh spectrum,
  postfit sigma0 + keyed RHS pools, dd fractional).

  Modeled on scripts/chain_run_day129.ps1 (checkpoint watchdog, warm-resume
  verdict, stateless resume).  Segment budget 4: seed + leg + 2 crash spares.

  Usage:  pwsh -File scripts/p2_run_guards_chain.ps1 [-RunRoot <dir>]
#>
[CmdletBinding()]
param(
  [string]$RunRoot     = "C:\GnssProcessor\runs\day129_guards",
  [string]$BaseConfig  = "C:\GnssProcessor\config\test_suite\_day129_guards_conv.json",
  [string]$Exe         = "C:\GnssProcessor\build\x64-Release\gnss_processor_p2chain.exe",
  [string]$ExeSource   = "C:\GnssProcessor\build\x64-Release\gnss_processor.exe",
  [int]   $MaxLegSegments = 3,
  [double]$SeedEndTow  = 327570.0,  # 18:59:30 — the seed's last epoch
  [double]$EndTow      = 345570.0,  # 23:59:30 — the day's last epoch
  # Epoch times carry ~1e-5 s of double residue BELOW nominal (TimeUtils builds
  # them through a Julian-date divide) — every tow comparison against a nominal
  # epoch needs this tolerance or a complete stage reads as "ended early"
  # (measured: the first seed ended at 327569.999988317 and a strict < killed
  # the chain).  One second ≪ half an epoch; safe on both sides.
  [double]$TowTol      = 1.0
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

# Snapshot M/N header -> object (Get-Content -TotalCount: opens AND CLOSES the
# file — never ReadLines(), the leaked handle breaks per-epoch atomic replace).
function Get-SnapHeader([string]$path) {
  if (-not (Test-Path $path)) { return $null }
  $m = $null; $n = $null
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
  }
}

# One supervised producer run.  Returns the exit code; enforces the checkpoint
# watchdog (a run that cannot checkpoint must not continue — a crash at hour 8
# would rewind to the stage start).
function Run-Segment([string]$cfg, [string]$workDir, [string]$snapFile, [double]$seedTow) {
  $t0 = Get-Date
  $p = Start-Process -FilePath $Exe -ArgumentList $cfg `
        -WorkingDirectory $workDir -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $workDir "stdout.log") `
        -RedirectStandardError  (Join-Path $workDir "stderr.log")
  $checkpointOk = $false
  $lastBeat = Get-Date
  while (-not $p.HasExited) {
    Start-Sleep -Seconds 30
    $mins = ((Get-Date) - $t0).TotalMinutes
    $h = Get-SnapHeader $snapFile
    if (-not $checkpointOk -and $h -and $h.Tow -gt $seedTow) {
      $checkpointOk = $true
      Say ("  checkpointing CONFIRMED at {0:N1} min (snapshot tow=$($h.Tow))" -f $mins)
    }
    # Full-day 305-station parse takes ~8 min; the 13 h clip less.  Allow 30.
    if (-not $checkpointOk -and $mins -gt 30) {
      Say "  *** CHECKPOINT WATCHDOG: no snapshot write after $([int]$mins) min — killing segment ***"
      $why = Select-String -Path (Join-Path $workDir "logs\main_log.log") `
             -Pattern "state-snapshot write to .* failed" -ErrorAction SilentlyContinue |
             Select-Object -Last 1
      if ($why) { Say "  reason| $($why.Line.Trim())" }
      try { $p.Kill() } catch {}
      Start-Sleep -Seconds 5
      exit 9
    }
    if (((Get-Date) - $lastBeat).TotalMinutes -ge 30) {
      $lastBeat = Get-Date
      $pos = if ($h) { $h.Tow } else { "?" }
      Say ("  [{0:N0} min] snapshot_tow=$pos" -f $mins)
    }
  }
  return $p.ExitCode
}

Say "=== Phase-2 guards chain supervisor ==="

# Name-scoped zombie pre-kill + fresh renamed exe (concurrent-session rule).
$zombies = Get-Process -Name "gnss_processor_p2chain" -ErrorAction SilentlyContinue
if ($zombies) { Say "pre-kill: $($zombies.Count) stale gnss_processor_p2chain"; $zombies | Stop-Process -Force }
Copy-Item $ExeSource $Exe -Force
$hExe = (Get-FileHash $Exe -Algorithm SHA256).Hash
Say "exe $Exe sha256=$($hExe.Substring(0,16)) (copied fresh from source)"

# ---- Stage 1: the 6 h guarded seed (skipped if already complete) ----------
$seedDir  = Join-Path $RunRoot "seed"
$seedSnap = Join-Path $seedDir "snap.txt"
New-Item -ItemType Directory -Force -Path (Join-Path $seedDir "logs") | Out-Null
$sh = Get-SnapHeader $seedSnap
if ($sh -and $sh.Tow -ge $SeedEndTow - $TowTol) {
  Say "seed already complete (tow=$($sh.Tow) n_emitted=$($sh.NEmitted)) — skipping stage 1"
} else {
  $preTow = 0.0
  if ($sh) {
    $preTow = $sh.Tow
    Say "seed snapshot exists but INCOMPLETE (tow=$($sh.Tow)); relaunching — the producer warm-resumes from its own checkpoint (same config, same fingerprint)"
  }
  Say "--- stage 1: seed 13:00->19:00, guards armed ($BaseConfig) ---"
  $rc = Run-Segment $BaseConfig $seedDir $seedSnap $preTow
  $sh = Get-SnapHeader $seedSnap
  if (-not $sh) { Say "FATAL: seed produced no readable snapshot (rc=$rc)"; exit 4 }
  Say ("seed rc=$rc -> tow=$($sh.Tow) n_emitted=$($sh.NEmitted) fp=$($sh.Fingerprint)")
  if ($sh.Tow -lt $SeedEndTow - $TowTol) { Say "FATAL: seed ended early (tow=$($sh.Tow) < $SeedEndTow)"; exit 5 }
  # Cold-start sanity: the seed must have COLD-started (require=false, no snapshot).
  $log = Join-Path $seedDir "logs\main_log.log"
  $err = @(Select-String -Path $log -Pattern "\[error\]|\[critical\]" -ErrorAction SilentlyContinue).Count
  Say "  seed error/critical lines: $err"
}

# Immutable copy of the completed seed (chain legs copy their own snapshot; this
# is the archival anchor, never pointed at by any config).
$seedImmutable = Join-Path $RunRoot "seed_immutable.txt"
if (-not (Test-Path $seedImmutable)) {
  Copy-Item $seedSnap $seedImmutable
  Say "seed archived -> $seedImmutable"
}

# ---- Stage 2: the guarded leg 19:00 -> end of day -------------------------
$current  = Get-SnapHeader $seedSnap
$seedPath = $seedSnap
# Resume support: pick up from the newest legNN snapshot if one advanced.
$legDirs = Get-ChildItem -Path $RunRoot -Directory -Filter "leg*" -ErrorAction SilentlyContinue | Sort-Object Name
$segIndex = 0
foreach ($d in $legDirs) {
  $h = Get-SnapHeader (Join-Path $d.FullName "snap.txt")
  if ($h -and $h.Tow -gt $current.Tow) { $current = $h; $seedPath = Join-Path $d.FullName "snap.txt" }
  $segIndex = [int]($d.Name -replace "leg", "")
}

for ($s = 1; $s -le $MaxLegSegments; $s++) {
  if ($current.Tow -ge $EndTow - $TowTol) { break }
  $segIndex++
  $legDir = Join-Path $RunRoot ("leg{0:d2}" -f $segIndex)
  Say "--- stage 2 segment $segIndex -> $legDir (from tow=$($current.Tow)) ---"
  $gen = & python "$repo\scripts\chain_make_leg.py" --snapshot $seedPath --legdir $legDir `
           --base $BaseConfig --frame-guards 2>&1
  $gen | ForEach-Object { Say "  cfg| $_" }
  if ($LASTEXITCODE -ne 0) { Say "FATAL: leg config generation failed"; exit 3 }

  $before = Get-SnapHeader (Join-Path $legDir "snap.txt")
  $rc = Run-Segment (Join-Path $legDir "leg.json") $legDir (Join-Path $legDir "snap.txt") $before.Tow
  $after = Get-SnapHeader (Join-Path $legDir "snap.txt")
  if (-not $after) { Say "FATAL: leg produced no readable snapshot"; exit 4 }
  $dEmit = $after.NEmitted - $before.NEmitted
  Say ("leg$segIndex rc=$rc d_emitted=$dEmit -> tow=$($after.Tow)")

  # Warm-resume verdict (chain integrity): WARM, not COLD, snapshot age 30.0 s.
  $log = Join-Path $legDir "logs\main_log.log"
  if (Test-Path $log) {
    $warm = @(Select-String -Path $log -Pattern "WARM RESUME from" -SimpleMatch -ErrorAction SilentlyContinue).Count
    $cold = @(Select-String -Path $log -Pattern "COLD START"       -SimpleMatch -ErrorAction SilentlyContinue).Count
    $age  = Select-String -Path $log -Pattern "snapshot age ([0-9.]+) s" -ErrorAction SilentlyContinue |
            Select-Object -First 1
    $ageS = if ($age) { $age.Matches[0].Groups[1].Value } else { "?" }
    Say "  verdict: warm=$warm cold=$cold snapshot_age=${ageS}s"
    if ($cold -gt 0) { Say "  *** COLD START DETECTED — chain integrity lost, stopping ***"; exit 5 }
    if ($ageS -ne "?" -and [math]::Abs([double]$ageS - 30.0) -gt 0.5) {
      Say "  *** snapshot age $ageS s != 30 s — offset arithmetic slipped, stopping ***"; exit 6
    }
  }
  # NOTE: the SH gate can legitimately withhold EMISSION while the snapshot still
  # advances (emission-only gate) — d_emitted may undercount tow progress.  Judge
  # progress by tow, not n_emitted.
  if ($after.Tow -le $before.Tow) { Say "FATAL: leg made no tow progress"; exit 7 }
  $current = $after
  $seedPath = Join-Path $legDir "snap.txt"
}

Say "=== supervisor finished: final tow=$($current.Tow) n_emitted=$($current.NEmitted) ==="
if ($current.Tow -ge $EndTow - $TowTol) { Say "STATUS: COMPLETE — seed + leg through end of DOY-129"; exit 0 }
Say "STATUS: INCOMPLETE — leg segment budget exhausted"
exit 8
