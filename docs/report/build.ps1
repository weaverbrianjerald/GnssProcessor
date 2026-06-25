#!/usr/bin/env pwsh
# Build the GnssProcessor mathematical reference.
#
#   ./build.ps1            # build combined volume + all standalone PDFs
#   ./build.ps1 -Regen     # regenerate the code-derived tables first
#   ./build.ps1 -Clean     # clean intermediates and PDFs, then build
#
# Requires a TeX distribution with latexmk + biber on PATH (MiKTeX or
# TeX Live).  -Regen additionally needs Python for gen_report_tables.py.
[CmdletBinding()]
param(
    [switch]$Regen,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if ($Regen) {
    Write-Host '== regenerating code-derived tables =='
    python ../../scripts/gen_report_tables.py
}
if ($Clean) {
    Write-Host '== cleaning =='
    latexmk -C
}

Write-Host '== building (combined volume + standalone components) =='
latexmk            # @default_files in latexmkrc builds every target
Write-Host '== done; PDFs:'
Get-ChildItem -Filter *.pdf | Select-Object Name, Length
