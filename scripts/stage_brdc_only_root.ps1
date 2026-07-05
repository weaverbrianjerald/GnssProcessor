<#
.SYNOPSIS
  Stage a brdc-only internal data root for a PPP-RTK NETWORK producer run in
  network.apriori_source = brdc_rts mode.

.DESCRIPTION
  The brdc_rts producer HARD-REFUSES if any precise product is resolvable
  (getOrbitCount()>0 || getClockCount()>0, ApplicationNetwork.cpp:598).  The
  bundled data/<YYYY-DOY>/products/ dirs ship COD SP3+CLK next to the BRDC nav,
  and in POST mode there is no config knob to skip precise-product discovery
  (the skip gates are RealTime-only).  This script builds a parallel root where
  every products/ dir has the BRDC/BIA/ERP/GIM but NO *.SP3 / *.CLK, so the
  producer resolves 0 orbits / 0 clocks and runs on broadcast.

  Only PRODUCT files are copied (small).  obs/ (9.5 GB/day) and static/ (438 MB)
  are DIRECTORY JUNCTIONS back to the source (no admin needed, no copy).  The
  network config paths obs via absolute network.stations[].rinex_path anyway, so
  the obs junction is belt-and-suspenders.

  Idempotent: re-running refreshes the small products copies and leaves the
  junctions in place.  The canonical data/ tree is never modified.

.EXAMPLE
  pwsh scripts/stage_brdc_only_root.ps1
  pwsh scripts/stage_brdc_only_root.ps1 -SrcRoot D:\net305\data -DstRoot D:\net305\data_brdc_rts -Days 2024-200
#>
[CmdletBinding()]
param(
  [string]   $SrcRoot = "C:\GnssProcessor\data",
  [string]   $DstRoot = "C:\GnssProcessor\data_brdc_rts",
  [string[]] $Days    = @("2024-128", "2024-129", "2024-130")
)

$ErrorActionPreference = "Stop"

function Link-Or-Copy-Dir([string]$Link, [string]$Target) {
  if (-not (Test-Path $Target)) { return }
  if (Test-Path $Link) { return }   # already staged
  try {
    New-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null
    Write-Host "  junction  $Link -> $Target"
  } catch {
    Write-Host "  junction failed ($($_.Exception.Message)); copying instead: $Link"
    Copy-Item -Path $Target -Destination $Link -Recurse -Force
  }
}

New-Item -ItemType Directory -Force -Path $DstRoot | Out-Null
Write-Host "Staging brdc-only root: $DstRoot  (from $SrcRoot)"

# static/ (atx + nc) — junction, do not copy.
Link-Or-Copy-Dir (Join-Path $DstRoot "static") (Join-Path $SrcRoot "static")

foreach ($d in $Days) {
  $srcDay = Join-Path $SrcRoot $d
  if (-not (Test-Path $srcDay)) { Write-Host "  skip (absent): $d"; continue }
  Write-Host "  day $d"

  # products/ — copy every file EXCEPT *.SP3 / *.CLK (case-insensitive).
  $srcProd = Join-Path $srcDay "products"
  if (Test-Path $srcProd) {
    $dstProd = Join-Path (Join-Path $DstRoot $d) "products"
    New-Item -ItemType Directory -Force -Path $dstProd | Out-Null
    Get-ChildItem -Path $srcProd -File | Where-Object {
      $_.Extension -notmatch '(?i)^\.(sp3|clk)$'
    } | ForEach-Object {
      Copy-Item $_.FullName -Destination $dstProd -Force
    }
  }

  # obs/ — junction (9.5 GB; belt-and-suspenders, config uses absolute obs paths).
  Link-Or-Copy-Dir (Join-Path (Join-Path $DstRoot $d) "obs") (Join-Path $srcDay "obs")
}

# Verify no precise product leaked onto the discovery path.
$leaks = Get-ChildItem -Path $DstRoot -Recurse -File -Include *.SP3, *.CLK -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -notlike "*\obs\*" }   # obs junction may contain unrelated files
$brdc  = Get-ChildItem -Path $DstRoot -Recurse -File -Include *_MN.rnx -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -notlike "*\obs\*" }
Write-Host ""
if ($leaks) {
  Write-Host "FAIL: precise products still on the products path (brdc_rts will refuse):" -ForegroundColor Red
  $leaks | ForEach-Object { Write-Host "  $($_.FullName)" }
  exit 1
}
Write-Host ("OK: 0 SP3/CLK on the products path; {0} BRDC nav file(s) present." -f @($brdc).Count) -ForegroundColor Green
Write-Host "Point files.internal_data_root at: $DstRoot"
