# Card CM (SPEC_event_harness s24.8) detached run driver.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/cm_driver.ps1 `
#       -Jobs "C:/GnssProcessor/runs/_cm_a0|C:/GnssProcessor/runs/_cm_a0/arm.json;C:/GnssProcessor/runs/_cm_a2|..." `
#       [-Exe C:\GnssProcessor\build\x64-Release\gnss_processor_cm.exe] [-Worker cm] [-Log <path>]
#
# For each job "<rundir>|<config>" in order: claim a box-wide producer slot
# (scripts/ev_slot.py claim <worker> --max-wait 540; exit 3 = no slot yet, call again),
# launch through scripts/ev_launch.ps1 (cwd = rundir, stdout.txt / stderr.txt / exitcode.txt
# in the run dir), wait for exitcode.txt, release the slot.  Never reads the process list
# and never kills anything.  Run two drivers to use both slots.
param([Parameter(Mandatory=$true)][string]$Jobs,
      [string]$Exe = 'C:\GnssProcessor\build\x64-Release\gnss_processor_cm.exe',
      [string]$Worker = 'cm',
      [string]$Log = '')
$ErrorActionPreference = 'Continue'
$root = 'C:\GnssProcessor'
if (-not $Log) { $Log = Join-Path $env:TEMP "cm_driver_$PID.log" }
function L($m) { "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m" | Out-File -Append -FilePath $Log -Encoding utf8 }
L "driver start pid=$PID exe=$Exe"
foreach ($j in ($Jobs -split ';')) {
    if (-not $j.Trim()) { continue }
    $parts = $j.Split('|'); $rundir = $parts[0].Trim(); $cfg = $parts[1].Trim()
    New-Item -ItemType Directory -Force -Path $rundir | Out-Null
    $lock = $null
    while ($true) {
        $out = & python "$root/scripts/ev_slot.py" claim $Worker --max-wait 540 2>&1
        $rc = $LASTEXITCODE
        if ($rc -eq 0) {
            $lock = ($out | ForEach-Object { "$_" } | Where-Object { $_ -like '*slot*.lock' } | Select-Object -Last 1).Trim()
            break
        }
        if ($rc -eq 3) { L "no slot within 540 s for $rundir - claiming again"; continue }
        L "claim FAILED rc=$rc for $rundir : $out"; Start-Sleep -Seconds 30
    }
    L "claimed $lock -> $rundir ($cfg)"
    $t0 = Get-Date
    & "$root\scripts\ev_launch.ps1" -Exe $Exe -Config $cfg -RunDir $rundir
    while (-not (Test-Path (Join-Path $rundir 'exitcode.txt'))) { Start-Sleep -Seconds 5 }
    $ec = (Get-Content (Join-Path $rundir 'exitcode.txt')) -join ' / '
    & python "$root/scripts/ev_slot.py" release $lock | Out-Null
    L "released $lock ; $rundir exitcode.txt = $ec ; wall $([math]::Round(((Get-Date) - $t0).TotalSeconds)) s"
}
L "driver done"
