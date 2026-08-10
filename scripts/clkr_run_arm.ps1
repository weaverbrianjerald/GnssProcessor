# Card CLK-R arm runner: serialized producer run + trace harvest.
# Usage: .\clkr_run_arm.ps1 -Config <config.json> -RunDir <runs/name>
# Redirects console to <RunDir>/console.txt (never pipe a live producer);
# harvests every C:\GnssProcessor\logs file with mtime >= start into
# <RunDir>/logs before returning (the shared-spdlog-dir rule).
param(
    [Parameter(Mandatory=$true)][string]$Config,
    [Parameter(Mandatory=$true)][string]$RunDir,
    [string]$Exe = "C:\GnssProcessor\build\x64-Release\gnss_processor_clkr.exe"
)
$ErrorActionPreference = "Stop"
$zombies = Get-Process | Where-Object { $_.ProcessName -like "gnss_processor*" }
if ($zombies) { throw "producer already running: $($zombies.ProcessName -join ', ') — runs cannot overlap" }
New-Item -ItemType Directory -Force $RunDir | Out-Null
New-Item -ItemType Directory -Force (Join-Path $RunDir "logs") | Out-Null
$start = Get-Date
& $Exe $Config *> (Join-Path $RunDir "console.txt")
$code = $LASTEXITCODE
Get-ChildItem C:\GnssProcessor\logs -File | Where-Object { $_.LastWriteTime -ge $start } |
    Copy-Item -Destination (Join-Path $RunDir "logs") -Force
"exit=$code wall=$([int]((Get-Date)-$start).TotalSeconds)s -> $RunDir"
if ($code -ne 0) { exit $code }
