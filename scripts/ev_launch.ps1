# Detached run launcher for the event harness: runs the producer with cwd=rundir
# (so ./logs/ cannot collide with a concurrent run) and always writes exitcode.txt.
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
$p.WaitForExit()
$sw.Stop()
"$($p.ExitCode)`n$([math]::Round($sw.Elapsed.TotalSeconds,1))" |
    Set-Content -Path (Join-Path $RunDir 'exitcode.txt')
