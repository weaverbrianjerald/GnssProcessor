# Timed run launcher for cost measurements: like scripts/ev_launch.ps1 (cwd = run dir, always writes
# exitcode.txt) and additionally records the process CPU time and the system-wide CPU load sampled every
# 5 s, so a run on a shared machine can be judged by its CPU seconds and by how much of the machine it had.
# Writes <RunDir>/timing.txt: exit, wall_s, cpu_user_s, cpu_kernel_s, sys_load_mean, sys_load_max, samples,
# own_procs_mean (logical processors this process kept busy), other_procs_mean (the rest of the load).
#
# The process runs at the priority class named by $env:NC_PRIORITY (default AboveNormal). On a machine that
# other sessions keep saturated, a normal-priority run is slowed several times over and its timings vary with
# the other load; above normal the repeats of one run agree within about 1 %.
param([Parameter(Mandatory=$true)][string]$Exe,
      [Parameter(Mandatory=$true)][string]$Config,
      [Parameter(Mandatory=$true)][string]$RunDir)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path (Join-Path $RunDir 'logs') | Out-Null
Remove-Item (Join-Path $RunDir 'exitcode.txt') -Force -ErrorAction SilentlyContinue
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $Exe -ArgumentList "`"$Config`"" -WorkingDirectory $RunDir `
     -RedirectStandardOutput (Join-Path $RunDir 'stdout.txt') `
     -RedirectStandardError  (Join-Path $RunDir 'stderr.txt') `
     -NoNewWindow -PassThru
$null = $p.Handle
$prio = if ($env:NC_PRIORITY) { $env:NC_PRIORITY } else { 'AboveNormal' }
try { $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]$prio } catch {}
$loads = New-Object System.Collections.Generic.List[double]
$own   = New-Object System.Collections.Generic.List[double]
$prevCpu = 0.0; $prevT = $sw.Elapsed.TotalSeconds
while (-not $p.WaitForExit(5000)) {
    try {
        $l = (Get-Counter '\Processor(_Total)\% Processor Time' -ErrorAction Stop).CounterSamples[0].CookedValue
        $loads.Add($l)
        $p.Refresh()
        $cpu = $p.TotalProcessorTime.TotalSeconds; $t = $sw.Elapsed.TotalSeconds
        if ($t -gt $prevT) { $own.Add(($cpu - $prevCpu) / ($t - $prevT)) }
        $prevCpu = $cpu; $prevT = $t
    } catch {}
}
$sw.Stop()
$p.Refresh()
$user = $p.UserProcessorTime.TotalSeconds; $kern = $p.PrivilegedProcessorTime.TotalSeconds
$mean = if ($loads.Count) { ($loads | Measure-Object -Average).Average } else { -1 }
$max  = if ($loads.Count) { ($loads | Measure-Object -Maximum).Maximum } else { -1 }
$ownm = if ($own.Count) { ($own | Measure-Object -Average).Average } else { -1 }
$ncpu = [Environment]::ProcessorCount
$other = if ($loads.Count) { $mean * $ncpu / 100 - $ownm } else { -1 }
"$($p.ExitCode)`n$([math]::Round($sw.Elapsed.TotalSeconds,1))" | Set-Content -Path (Join-Path $RunDir 'exitcode.txt')
"exit=$($p.ExitCode) wall_s=$([math]::Round($sw.Elapsed.TotalSeconds,1)) cpu_user_s=$([math]::Round($user,1)) cpu_kernel_s=$([math]::Round($kern,1)) sys_load_mean=$([math]::Round($mean,1)) sys_load_max=$([math]::Round($max,1)) samples=$($loads.Count) own_procs_mean=$([math]::Round($ownm,2)) other_procs_mean=$([math]::Round($other,2))" |
    Set-Content -Path (Join-Path $RunDir 'timing.txt')
