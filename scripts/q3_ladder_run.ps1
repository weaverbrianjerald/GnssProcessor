# Card Q3 ladder-leg arm runner (docs/SPEC_clock_temporal_anchor.md §8).
#
# Serialized: refuses to start while any gnss_processor* is alive, because every
# comparison in this leg is same-build + same-config-chain + non-overlapping, and
# two producers sharing C:\GnssProcessor\logs would cross-contaminate the harvest.
#
# Usage: .\q3_ladder_run.ps1 -Arm cta4_q3e6 [-Exe ...]
#   config  -> config\test_suite\_<Arm>.json
#   run dir -> runs\<Arm>   (console.txt + logs\ harvested by mtime)
param(
    [Parameter(Mandatory=$true)][string]$Arm,
    [string]$Exe = "C:\GnssProcessor\build\x64-Release\gnss_processor_cta4.exe"
)
$ErrorActionPreference = "Stop"
$zombies = Get-Process | Where-Object { $_.ProcessName -like "gnss_processor*" }
if ($zombies) { throw "producer already running: $($zombies.ProcessName -join ', ') - runs cannot overlap" }
$Config = "C:\GnssProcessor\config\test_suite\_$Arm.json"
if (-not (Test-Path $Config)) { throw "no config: $Config" }
$RunDir = "C:\GnssProcessor\runs\$Arm"
New-Item -ItemType Directory -Force $RunDir | Out-Null
New-Item -ItemType Directory -Force (Join-Path $RunDir "logs") | Out-Null
$start = Get-Date
& $Exe $Config *> (Join-Path $RunDir "console.txt")
$code = $LASTEXITCODE
Get-ChildItem C:\GnssProcessor\logs -File | Where-Object { $_.LastWriteTime -ge $start } |
    Copy-Item -Destination (Join-Path $RunDir "logs") -Force
"ARM=$Arm exit=$code wall=$([int]((Get-Date)-$start).TotalSeconds)s -> $RunDir"
if ($code -ne 0) { exit $code }
