# INC-4 full fast-matrix gate: 8 arms (snap_resume and obs included) vs goldens.
# Arms retired with their mechanisms by the retired-key deletion: lane, uwl and wlinj
# (the MW/WL lane layer), jw and cpljw (network.carry_coupling_joint_whiten).  The
# goldens predate the deletion, and allon, snap, snap_resume, obs and estim arm the
# deleted PB emission filter under carry_invariant (consumer arms the deleted T1d
# convergence latch with the SH field), so those goldens are re-taken before the gate
# is used on a later executable.
# Usage: powershell -File scripts/run_inc4_gate.ps1
$ErrorActionPreference = "Continue"
cd C:\GnssProcessor
$exe = "build/x64-Release/gnss_processor_dcmp.exe"
$fail = 0

$arms = @(
    @{n="alloff";   c="config/test_suite/_dcmp_50sta_10ep_alloff.json";   g="runs/decomp_goldens/alloff_a";   seed=$null},
    @{n="allon";    c="config/test_suite/_dcmp_50sta_10ep_allon.json";    g="runs/decomp_goldens/allon_a";    seed=$null},
    @{n="consumer"; c="config/test_suite/_dcmp_50sta_5ep_consumer.json";  g="runs/decomp_goldens/consumer_a"; seed=$null},
    @{n="snap";     c="config/test_suite/_dcmp_50sta_10ep_snap.json";     g="runs/decomp_goldens/snap_a";     seed=$null},
    @{n="snap_resume"; c="config/test_suite/_dcmp_50sta_10ep_snap_resume.json"; g="runs/decomp_goldens/snap_resume_a";
      seed="runs/decomp_goldens/snap_a/logs/network_state_snapshot.json"},
    @{n="obs";      c="config/test_suite/_dcmp_50sta_10ep_obs.json";      g="runs/decomp_goldens/obs_a";      seed=$null},
    @{n="estim";    c="config/test_suite/_dcmp_50sta_10ep_estim.json";    g="runs/decomp_goldens/estim_a";    seed=$null},
    @{n="cpllive";  c="config/test_suite/_dcmp_50sta_10ep_cpllive.json";  g="runs/decomp_goldens/cpllive_a";  seed=$null}
)

foreach ($a in $arms) {
    $dir = "runs/decomp_inc4/$($a.n)"
    if ($a.seed) {
        python scripts/run_decomp_arm.py $exe $a.c $dir --force --seed $a.seed logs/network_state_snapshot.json
    } else {
        python scripts/run_decomp_arm.py $exe $a.c $dir --force
    }
    if ($LASTEXITCODE -ne 0) { Write-Output "ARM-FAIL(run) $($a.n)"; $fail = 1; continue }
    python scripts/compare_network_run.py compare $a.g $dir --maskspec goldens/decomp/maskspec.json
    if ($LASTEXITCODE -ne 0) { Write-Output "ARM-FAIL(compare) $($a.n)"; $fail = 1 }
    else { Write-Output "ARM-PASS $($a.n)" }
    # Standing invariant #4: the coupling-live arm must actually DRIVE the moved
    # arithmetic (0 injected terms = inert wiring the byte gate would still pass).
    if ($a.n -eq "cpllive") {
        $inert = Select-String -Path "$dir/run.log" -Pattern "injected terms: 0 present" -Quiet
        if ($inert) { Write-Output "ARM-WARN(inert-coupling) $($a.n)"; $fail = 1 }
    }
}
if ($fail -eq 0) { Write-Output "GATE: ALL ARMS BYTE-IDENTICAL" } else { Write-Output "GATE: FAILURES PRESENT" }
