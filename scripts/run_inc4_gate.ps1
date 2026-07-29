# INC-4 full fast-matrix gate: 8 arms + snap_resume + obs vs goldens.
# Usage: powershell -File scripts/run_inc4_gate.ps1
$ErrorActionPreference = "Continue"
cd C:\GnssProcessor
$exe = "build/x64-Release/gnss_processor_dcmp.exe"
$fail = 0

$arms = @(
    @{n="alloff";   c="config/test_suite/_dcmp_50sta_10ep_alloff.json";   g="runs/decomp_goldens/alloff_a";   seed=$null},
    @{n="allon";    c="config/test_suite/_dcmp_50sta_10ep_allon.json";    g="runs/decomp_goldens/allon_a";    seed=$null},
    @{n="jw";       c="config/test_suite/_dcmp_50sta_10ep_jw.json";       g="runs/decomp_goldens/jw_a";       seed=$null},
    @{n="wlinj";    c="config/test_suite/_dcmp_50sta_10ep_wlinj.json";    g="runs/decomp_goldens/wlinj_a";    seed=$null},
    @{n="lane";     c="config/test_suite/_dcmp_50sta_10ep_lane.json";     g="runs/decomp_goldens/lane_a";     seed=$null},
    @{n="uwl";      c="config/test_suite/_dcmp_50sta_10ep_uwl.json";      g="runs/decomp_goldens/uwl_a";      seed=$null},
    @{n="consumer"; c="config/test_suite/_dcmp_50sta_5ep_consumer.json";  g="runs/decomp_goldens/consumer_a"; seed=$null},
    @{n="snap";     c="config/test_suite/_dcmp_50sta_10ep_snap.json";     g="runs/decomp_goldens/snap_a";     seed=$null},
    @{n="snap_resume"; c="config/test_suite/_dcmp_50sta_10ep_snap_resume.json"; g="runs/decomp_goldens/snap_resume_a";
      seed="runs/decomp_goldens/snap_a/logs/network_state_snapshot.json"},
    @{n="obs";      c="config/test_suite/_dcmp_50sta_10ep_obs.json";      g="runs/decomp_goldens/obs_a";      seed=$null},
    @{n="estim";    c="config/test_suite/_dcmp_50sta_10ep_estim.json";    g="runs/decomp_goldens/estim_a";    seed=$null},
    @{n="cpllive";  c="config/test_suite/_dcmp_50sta_10ep_cpllive.json";  g="runs/decomp_goldens/cpllive_a";  seed=$null},
    @{n="cpljw";    c="config/test_suite/_dcmp_50sta_10ep_cpljw.json";    g="runs/decomp_goldens/cpljw_a";    seed=$null}
)

foreach ($a in $arms) {
    $dir = "runs/decomp_inc4/$($a.n)"
    if ($a.seed) {
        python scripts/run_decomp_arm.py $exe $a.c $dir --force --seed $a.seed logs/network_state_snapshot.json
    } else {
        python scripts/run_decomp_arm.py $exe $a.c $dir --force
    }
    if ($LASTEXITCODE -ne 0) { Write-Output "ARM-FAIL(run) $($a.n)"; $fail = 1; continue }
    $warn = Select-String -Path "$dir/run.log" -Pattern "parallel_carry.*parallel_reduce|forces pcarry" -Quiet
    if ($warn) { Write-Output "ARM-WARN(mutex) $($a.n)" ; $fail = 1 }
    python scripts/compare_network_run.py compare $a.g $dir --maskspec goldens/decomp/maskspec.json
    if ($LASTEXITCODE -ne 0) { Write-Output "ARM-FAIL(compare) $($a.n)"; $fail = 1 }
    else { Write-Output "ARM-PASS $($a.n)" }
    # Standing invariant #4: the coupling-live arms must actually DRIVE the moved
    # arithmetic (0 injected terms = inert wiring the byte gate would still pass).
    if ($a.n -eq "cpllive" -or $a.n -eq "cpljw") {
        $inert = Select-String -Path "$dir/run.log" -Pattern "injected terms: 0 present" -Quiet
        if ($inert) { Write-Output "ARM-WARN(inert-coupling) $($a.n)"; $fail = 1 }
    }
}
if ($fail -eq 0) { Write-Output "GATE: ALL ARMS BYTE-IDENTICAL" } else { Write-Output "GATE: FAILURES PRESENT" }
