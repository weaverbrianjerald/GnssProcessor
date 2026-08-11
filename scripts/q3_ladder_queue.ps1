# Card Q3 ladder leg — the serialized 3 h arm queue (SPEC §8).
#
# One producer at a time, in dependency order.  Each link in the chain is ONE
# knob (or one physically-coupled knob PAIR) from the one before it:
#
#   cta3_on            (raw fold, raw stat, shipped floors)   [stored]
#     --stat-->        cta4_bis_raw_proj
#     --floors-->      cta4_q3e5      (rung 0)
#     --q-->           cta4_q3e6 -> cta4_q3e7 -> cta4_q1e7
#   cta3_gf            (gf fold, post stat)                    [stored]
#     --stat-->        cta4_bis_gf_raw / cta4_bis_gf_proj
#
# cta4_tie is the same-length ARMED byte-identity gate against runs/cta3_on.
param([string[]]$Arms = @(
    "cta4_tie",
    "cta4_bis_raw_proj", "cta4_bis_gf_raw", "cta4_bis_gf_proj",
    "cta4_q3e5", "cta4_q3e6", "cta4_q3e7", "cta4_q1e7"
))
$ErrorActionPreference = "Stop"
foreach ($a in $Arms) {
    "=== $a  $(Get-Date -Format HH:mm:ss) ==="
    & C:\GnssProcessor\scripts\q3_ladder_run.ps1 -Arm $a
}
"=== QUEUE DONE $(Get-Date -Format HH:mm:ss) ==="
