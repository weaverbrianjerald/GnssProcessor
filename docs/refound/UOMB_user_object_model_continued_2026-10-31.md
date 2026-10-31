# UOMB: the user side on the identity layer, the five behaviour fixes and the register of what is not built (continuation of card UOM)

**STATUS**

- **id:** UOMB
- **topic:** consumer
- **status:** open
- **summary:** Continuation of [UOM](UOM_user_object_model_2026-10-13.md), whose file reached the size bar. Wave C (step U8, the user side on `common/GnssIdentity.hpp`) ported from its base `ff7766a` to main `e639032` as 13 commits on branch `uom/wave-c-port`; the five behaviour fixes of UOM section 11 ported to main, one branch and one commit each; steps UB4, UB5 and UB8 to UB13 triaged; the five decisions of UOM section 7 answered. The bars are pre-registered in section 2; results are appended below the line.
- **source:** [UOM](UOM_user_object_model_2026-10-13.md) sections 5, 6, 7, 11, 15 and 17; the branches `uom/u8-helpers`, `uom/u8-c1`, `uom/u8-c2`, `uom/u8-c3` (wave C on `ff7766a`) and `uom/ub3-ambiguity-std`, `uom/ub-log-stamp`, `uom/ub6-dd-innovations`, `uom/ub-baseline-trace`, `uom/ub7-nlwl-pivot` (the fixes on `f823a36`)
- **knobs / posture:** none. Wave C is an organization step (identical output). The five fixes change behaviour without a knob (one behaviour is right); each is a separate branch and its adoption is the author's
- **dates in the text:** 2026-10-31
- **hashes / run dirs:** base main `e639032`; reference executable `gnss_processor_uomcref.exe` (Release, worktree `D:/GnssProcessorSupport/worktrees/uomref`, sha256 prefix `724a0d43969cbe92`); candidate worktree `D:/GnssProcessorSupport/worktrees/uomc`, branch `uom/wave-c-port`, head tree `85be81de83017ea3838cfb354b9a05c1da377673`; fix worktrees `D:/GnssProcessorSupport/worktrees/uomfix_{ub3,g9,ub6,g10,ub7}` on `uom/fix-{ub3,g9,ub6,g10,ub7}`; runs under `D:/GnssProcessorSupport/worktrees/uomc/runs/`; records under `D:/GnssProcessorSupport/gates/uomc/`
- **gates (frozen at pre-registration):** section 2

**Outcome.** Pending. Results are appended under dated headings below the line and this paragraph is revised when they land.

## 1. Scope and method

Card UOM ended with wave C (the user side onto the identity layer) built and gated on a base 455 commits behind main, five behaviour fixes graded on a base 476 commits behind, and steps UB4, UB5 and UB8 to UB13 unbuilt. This card ports the first two onto the current main, triages the third and answers the decisions of UOM section 7.

Method. Each wave C commit is re-applied to the current code: cherry-picked where it applies, re-implemented where main has moved (card NOM steps N4 to N6, card RKD and the real-time work changed many of the files). A site that no longer exists is listed as gone. A new site of the same idiom on main is converted only when the conversion is string-identical for every input, otherwise it is listed as left as is. Code comments added by the port are plain statements of what and why. The reference is a Release build of main `e639032`, the candidate a Release build of the branch with the same flags and the same toolchain, both renamed copies kept beside their DLLs. Every comparison is of the reference against the candidate on the same data root (`C:/GnssProcessor/data`, read only).

## 2. Pre-registration (2026-10-31)

Written and committed before any candidate output was compared with a reference output. The reference runs (quick set, full set, both 2 h goldens) had finished and their logs had been read for counts only. Bars are applied literally; a missed bar is recorded as missed.

### 2.1 Wave C

| bar | rule |
| --- | --- |
| W1, user arms | `scripts/uom_gate.py cmp` (without `--allow-inserted`) reads IDENTICAL on all 16 arms of the quick set and all 16 of the full set: equal `arm.json`, every file under `logs/` equal (CSVs byte for byte, logs and stdout after the gate's masks), equal exit codes |
| W2, network | `scripts/prm_ab.py cmp --all-csv` reads IDENTICAL for both 2 h goldens (field and sim beds, goldens as generated, start offsets as in `prm_ab.py make`): `products.txt` without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv` and every other CSV both runs write |
| W3, suite | Debug `gnss_tests` on the branch head reports 0 failed; the passed and skipped counts are stated beside the same suite built from main the same way (a worktree has no `data/`, so fixture-gated cases skip on both) |
| W4, compile | the head compiles in Debug (`gnss_core`, `gnss_tests`) and in Release (`gnss_processor`) with no new warning other than C4828 |

### 2.2 The five fixes

Each fix is one commit and its test on a branch from main. The reference of every comparison is the reference executable above. A difference outside the declaration is a finding to report and is not corrected in the same step.

| fix | branch | declared movement on main | other arms |
| --- | --- | --- | --- |
| UB3, one ambiguity initial-std source | `uom/fix-ub3` | only arms whose configuration sets `filter.initial_std.ambiguity` other than 1000 m may differ: quick `u_rtk_kin`, `u_baseline`, `u_baseline_if`, `u_sbf`, `u_sbf_te`; full `UA6`, `UA12`, `UA13`, `UA15`, `UA16`, `UA17` (effective values read from the configurations with `extends` followed, before any run was compared) | IDENTICAL: the other 11 quick and 10 full arms |
| G9, log stamps carry a whole minute | `uom/fix-g9` | every CSV of every arm and both goldens identical, exit codes equal; each differing line of a log or of stdout is a reference line holding a `HH:MM:60.000` stamp, the candidate line being the same text with the following minute's `:00.000` stamp (carry through hour and date allowed); the candidate holds no `:60.000`; the number of differing lines equals the number of `:60.000` stamps the reference holds | none outside the stamp lines |
| UB6, DD innovations after the build's own writes | `uom/fix-ub6` | quick set 16 of 16 IDENTICAL; in the full set only `UA12` and `UA13` (2 h baseline) may differ | the other 14 full arms IDENTICAL |
| G10, baseline state trace rows | `uom/fix-g10` | `u_baseline`, `u_baseline_if`, `UA12`, `UA13` differ in `logs/state_trace.csv` (rows gained, header plus `,dzwd_m,dzwd_std_m`, tropo pair `nan`) and in `logs/main_log.log` and `stdout.txt` (the per-epoch warning "Skipped state trace row" lost, nothing else) | every other file of these four arms and every file of the other 28 arms IDENTICAL |
| UB7, one pivot-column resolver | `uom/fix-ub7` | only `u_nlwl` (NL/WL ambiguity basis) may differ, from the first epoch | the other 15 quick and all 16 full arms IDENTICAL |

For every fix the new test file passes on the branch (Debug `gnss_tests`, 0 failed in the suite) and fails on main. Where the test compiles against main it runs there unchanged. Where it uses the fix's new interface, the fix's behaviour is reverted in the source file with the new interface kept, and the failing cases are named. G9 additionally runs both 2 h goldens: every CSV and `products.txt` IDENTICAL by `prm_ab.py cmp --all-csv`, and the logs obey the stamp rule above.

<!-- Results are appended below this line under dated headings. -->
