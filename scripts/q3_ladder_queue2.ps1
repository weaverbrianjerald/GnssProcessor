# Card Q3 ladder leg — phase 2: the §3 k-CALIBRATION arms, then the consumer
# REPLAY of every graded arm (SPEC §8.4 gates 3 and 6).
#
# k-calibration first because it is a producer run and must not overlap anything;
# the replays are producer-less (runNetwork short-circuits before setup) but are
# still serialized here so the harvest of C:\GnssProcessor\logs stays unambiguous.
param(
    [string[]]$Kcal    = @("cta4_q3e5_kcal", "cta4_q1e7_kcal"),
    [string[]]$Replays = @("cta3_ctl", "cta3_on", "cta3_gf",
                           "cta4_bis_raw_proj", "cta4_bis_gf_raw", "cta4_bis_gf_proj",
                           "cta4_q3e5", "cta4_q3e6", "cta4_q3e7", "cta4_q1e7")
)
$ErrorActionPreference = "Stop"
foreach ($a in $Kcal) {
    "=== $a  $(Get-Date -Format HH:mm:ss) ==="
    & C:\GnssProcessor\scripts\q3_ladder_run.ps1 -Arm $a
}
& python C:\GnssProcessor\scripts\q3_make_replay.py @Replays
foreach ($a in $Replays) {
    $arm = "${a}_replay"
    if (-not (Test-Path "C:\GnssProcessor\config\test_suite\_$arm.json")) { "skip $arm (no config)"; continue }
    "=== $arm  $(Get-Date -Format HH:mm:ss) ==="
    & C:\GnssProcessor\scripts\q3_ladder_run.ps1 -Arm $arm
}
"=== QUEUE2 DONE $(Get-Date -Format HH:mm:ss) ==="
