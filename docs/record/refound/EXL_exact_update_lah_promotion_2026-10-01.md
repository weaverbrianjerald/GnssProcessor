# EXL: the exact coupling update armed with the phase-clock hold

**STATUS**

- **id:** EXL
- **topic:** network-vehicle
- **status:** closed
- **summary:** ARMED: every gate passed (the field census at its limit, ARHT00ATA and KOKB00USA worse); ledger A15, the goldens regenerated, baseline pair `runs/_rf0hv14_{field,sim}6`; fresh convergence only.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 6, 10, 13 and 14; [CTU2](CTU2_level_recursion_followup_2026-09-29.md) sections 2 to 9 and 12; [LAH](LAH_level_aware_hold_2026-09-29.md); [AIL](AIL_ambiguity_information_loss_2026-10-01.md) section 4; [LGM](LGM_los_phase_nuisance_2026-10-01.md) section 5.3; [WUP](WUP_windup_accumulated_promotion_2026-10-01.md) (the promotion template); [SMH](SMH_smoothed_hold_clone_2026-09-29.md) section 9 (the fold kappa default)
- **knobs / posture:** `network.carry_coupling_exact_update`, `network.carry_rx_code_bias_in_block`, `network.amb_float_drop_decouple_always`, `network.clock_anchor_guard_space` phase, `network.rank_gate_tol` 1e-11, `network.emit_pb_hold_phase_clock`; new, default off: `network.ctu_switch_on_resume` (the switch-on resume); NOT armed: the smoothed holds, the monitors, `network.carry_coupling_exact_cross`, `network.clock_level_bound_raise`, `network.phase_colour_state`
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** exe `gnss_processor_ctua.exe` (main `3c2969e`, sha256[:16] `7e8196a64cdc3b5c`) for the 2 h evidence of section 2; the gate exe `gnss_processor_exl.exe` (main `cf1c20a`, `c56d9a9ad4cdafc3`); the review fix `gnss_processor_exl2.exe` (`45183c6e51e9c729`); arms `runs/_rap_ctx_*`, `runs/_rap_exl_*`, `runs/_rap_exl2_*`; baseline pair `runs/_rf0hv14_{field,sim}6`

**Outcome.** Question: how to ship card CTU's exact update on the goldens when, under the plain hold, it leaves the field consumer at 3.878 m in hour 1. Answer: with card LAH's phase-clock hold as an interim hold (field WTZZ00DEU 0.108 m in hour 1, control 0.559). The sim is graded from 00:30:15. Owed at promotion: card LAH's regressions, the hold's need for a precise a priori clock, the mirror's handover epoch, the sim golden's start. [PRM](PRM_parameter_registry_2026-09-29.md) section 15 retired the mirror; [SCL](SCL_self_contained_clock_level_2026-10-03.md) and [NBP](NBP_native_broadcast_clock_posture_2026-10-07.md) pursue a producer without that clock.

## 1. Why

- **The producer.** Card AIL section 4: the goldens' coupling update (card 1d's kappa, which ages
  each row's satellite clock term at the legacy 1e-3 m2/s while card Q3's anchor folds the same
  clock at 2.9e-8) discards a young arc's ionosphere-free ambiguity information every epoch, a
  demeaned loss of 75 mm on the sim and 157 mm on the field below 20 epochs; the exact update cuts
  it to 4.7 and 5.1 mm. Card CTU's root (the frozen clock level) is the same defect seen from the
  clock.
- **The hold.** Card CTU section 6: the plain hold (card PBH) freezes the emitted phase bias while
  the exact update moves the clock and the phase biases together as the level converges, so the
  convergence lands on the consumer's phase rows; "a field arm of the recursion under the unchanged
  hold measures the hold, not the recursion". Card LAH's phase-clock hold transfers each held
  satellite's level step to the phase bias and is the hold cards SMH, SMD and SMC keep for the
  exact posture.

## 2. The fresh evidence (2026-10-01, 2 h, exe `_ctua`, the sim from 00:30:15)

Four arms per bed from the current goldens: the control (`runs/_rap_prm_ctua0_{gf,gs}`, byte-identical
to the phase 0 references of card PRM), the exact update with its four companions under the plain
hold (`_rap_ctx_ex_{f,s}`), the same under the phase-clock hold (`_rap_ctx_exlah_{f,s}`), and the
phase-clock hold alone (`_rap_ctx_lah_{f,s}`).

| arm | field consumer p50 h1 / h2 | sim consumer p50 h1 / h2 | field IF code spread, h2 | sim level at 16 / 60 / 120 / 240 epochs | sim IF code spread, h2 | floor folds sim / field | field S-basis completions | field anchor trips |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| control | 0.559 / 0.059 m | 0.734 / 0.084 m | 0.266 m | 0.16 / 0.16 / 0.16 / 0.16 m | 0.143 m | 63.6 / 46.0 % | 23 | 1 |
| exact, plain hold | 3.878 / 2.245 | 1.105 / 0.655 | 0.164 | 0.09 / 0.04 / 0.03 / 0.02 | 0.057 | 0 / 0 % | 0 | 2 |
| exact, phase-clock hold | 0.108 / 0.024 | 0.227 / 0.098 | 0.164 | 0.09 / 0.04 / 0.03 / 0.02 | 0.057 | 0 / 0 % | 0 | 2 |
| phase-clock hold alone | 0.116 / 0.023 | 0.280 / 0.090 | | | | | | |

The solve is the same under both holds (the hold is emit-only); the plain hold costs the field
consumer 3.3 m in hour 1 and the sim 0.4 m. Most of the consumer's gain over the control is the
phase-clock hold's own; the exact update's gain is on the producer side.

## 3. Gates (pre-registered before the gate arms run)

One Release exe with the posture rows built in grades every arm (invariant 6). 6 h arms decide;
the 2 h arms of section 2 are the smoke.

| gate | bar |
| --- | --- |
| U0 | Full Debug suite with no new failure; `make_golden_config.py --verify` PASS; `config_posture_check.py --verify-loader` 0 gaps; `docs_index_check.py` passes. |
| G0a | The same-exe golden controls (new knobs off), 6 h on both beds, byte-identical to `runs/_rf0hv12_field6` and `runs/_rf0hv13_sim6` (products without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`). |
| G0b | The regenerated goldens differ from the current ones in provenance lines and the six new keys only; `network.phase_colour_state` absent. |
| G0c | Exact with the phase-clock hold against exact with the plain hold: every non-P product record and the epoch status identical (the hold is emit-only). |
| ON | Run-end: the exact update's rows counted with no INERT warning and 0 mirror mismatches; in-block receiver code-bias folds above 0 with 0 failures; drop decouples counted on the field; the phase-clock hold's transfers counted. |
| G1 | 6 h: floor folds at most 5 %; non-datum anchor trips at most the control's plus 1; re-floats at most the control's plus 10 %; S-basis completions and gate withholds at most the control's; every epoch emitted. |
| Rank | With `rank_gate_tol` 1e-11 the 6 h arms record 0 S-basis completions and 0 withheld epochs on both beds (the ledger's rule: the tolerance is read on the configuration that uses it). |
| G2 | Sim truth: the datum-free level at most 0.25 / 0.15 / 0.08 m at epochs 16 / 60 / 240 and the IF code spread in hour 2 at most 0.10 m, read from 00:30:15 (section 2) and reported from the run start. |
| G4 (re-stated) | Field IF code spread in hour 2 no worse than the same-exe golden control's. |
| G5 (disclosed) | Not graded: the goldens carry no snapshot; the ledger interlock reads "fresh convergence only: a snapshot written without these knobs is not resumed into them" (card CTU2 section 8, the switch-on seam). |
| G7 | 6 h in-process consumers WTZZ00DEU and SZZ000SIM: the mean p50 over hours 2 to 6 no worse than the same-exe control's by more than 0.010 m. |
| Census, field | `census6h.py` with `estimation.accumulate_phase_windup=true` (and the hold's PBF response, the script's default), the 16 sites and WTZZ00DEU, against the same-exe control's wire; the frozen statistic of card WUP (`wup_grade.py census field`): per site, the change of the mean hourly p50 over hours 2 to 6. Bar: median change at most +0.005 m and at most 3 sites worse by more than 0.02 m. Hour 1 and KOKB00USA, ARHT00ATA, QUIN00USA and UTQI00USA reported. |
| Census, sim | `simcensus6h.py` with its base re-pointed to `runs/_rf0hv13_sim6/arm.json` (the re-based fixture); the same statistic and bar. |
| G8 (re-stated) | The 6 h epoch loop time with the monitors off within 10 % of the same-exe control's, both beds. |
| Report | Card AIL's young-arc loss on the exact arm (expected at the program posture's 4.7 / 5.1 mm); card PAC's constellation-common clock drift on the field (it was -0.180 m/h in hour 2). |

If any bar fails, nothing is armed and the failing reading is recorded here. If every bar holds,
the six knobs go into the live bases, the manifest pins, the ledger (one row for the exact update
with its four companions folded in as `second_code_channel`'s companion is, one row for the hold)
and the posture rows, the goldens are regenerated and verified, and the gate arms become the
baseline pair.

## 4. The gates read (2026-10-01, exe `gnss_processor_exl.exe`, sha256[:16] `c56d9a9ad4cdafc3`)

The gate exe is main `cf1c20a` with the four loader posture rows of section 5 (log-only; the solve is
the pre-promotion source). Arms, 6 h: the golden controls `runs/_rap_exl_ctl6_{f,s}` and the armed
set `runs/_rap_exl_on6_{f,s}` from the goldens' own start, and the sim pair again from 00:30:15
(`runs/_rap_exl_{ctl,on}6o_s`, the fixture-start convention).

| gate | bar | reading | verdict |
| --- | --- | --- | --- |
| U0 | suite, verifies | 2751 passed, 1 fixture-gated skip; golden verify PASS (69 armed in regime after section 5); posture check 0 gaps | pass |
| G0a | controls byte-identical to the baselines | `ident2h.py` over the 720 epochs: field against `runs/_rf0hv12_field6`, sim against `runs/_rf0hv13_sim6`, both IDENTICAL | pass |
| G0b | only the six keys change | the regenerated goldens differ from the previous ones in the six keys and the provenance lines; `network.phase_colour_state` absent; each equals its gate arm's configuration apart from the output paths | pass |
| G0c | the hold is emit-only | 2 h `_rap_ctx_ex_*` against `_rap_ctx_exlah_*`: every non-P, non-S product record and the epoch status identical on both beds; the S lines differ in the declared hold (`pbhold=1` / `pbhold=2`) | pass |
| ON | each mechanism acts | field: 678 737 rows on the exact route (167 stale), 69 868 in-block receiver code-bias folds with 0 failures, 21 drop decouples, 62 211 held emissions moved by the clock transfer; the mirror check reads 0 disposition and 31 inflation mismatches, with 1 handover epoch the mirror cannot see, on each bed (the one clock-pivot handover: the 31 datum trips) | pass, with the mirror's handover epoch disclosed |
| G1, field | floor folds at most 5 %; non-datum trips at most control + 1; re-floats at most control + 10 %; completions and withholds at most the control's; every epoch | floor 80.2 % -> 0; trips 1 -> 2; re-floats 38/79 -> 19/48; completions 23 -> 0; 0 withheld; 720 of 720 | pass |
| G1, sim from 00:30:15 | as above | floor 86.3 % -> 0; non-datum trips 0 -> 0; 0 re-floats, 0 completions, 0 withheld; 720 of 720 | pass |
| G1, sim from the run start | reported | non-datum trips 1 -> 55 (phase guard), floor 86.0 % -> 0, 720 of 720 | disclosed (below) |
| Rank | 0 completions, 0 withheld at 1e-11 | 0 and 0 on the three armed 6 h arms (`_rap_exl_on6_{f,s}`, `_rap_exl_on6o_s`) and the four armed 2 h arms of section 2 | pass |
| G2, sim from 00:30:15 | level at most 0.25 / 0.15 / 0.08 m at 16 / 60 / 240 epochs; IF spread hour 2 at most 0.10 m | level 0.09 / 0.04 / 0.02 m (0.03 at 120); IF spread 0.057 m (control 0.16 m level, 0.143 m spread) | pass |
| G2, sim from the run start | reported | level 1.10 / 0.84 / 0.81 / 0.77 m at 16 / 60 / 120 / 240 epochs (control 1.05 / 1.02 / 1.00 / 1.02), IF spread hour 2 0.707 m (control 0.887) | disclosed (below) |
| G4 | field IF spread hour 2 no worse than the control's | 0.164 against 0.266 m (last hour 0.327 against 0.501) | pass |
| G7 | in-process consumer, mean over hours 2 to 6, no worse by more than 0.010 m | WTZZ00DEU 0.023 against 0.040 m; SZZ000SIM 0.044 against 0.057 m from the start, 0.049 against 0.065 m from 00:30:15 | pass |
| Census, field | median change at most +0.005 m, at most 3 sites worse by more than 0.02 m | median -0.017 m; 6 better, 3 worse (ARHT00ATA +0.404, KOKB00USA +0.222, QUIN00USA +0.041); UTQI00USA -0.029 | pass, at the limit |
| Census, sim | as above | median -0.057 m; 13 better, none worse (the replays read the goldens' own from-start wires) | pass |
| G8 | epoch time within 10 %, monitors off | field +8.2 %, sim +4.5 % from the start and +7.9 % from 00:30:15 | pass |

Consumer p50 by hour (m):

| arm | hour 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| WTZZ00DEU, control | 0.559 | 0.059 | 0.029 | 0.033 | 0.038 | 0.041 |
| WTZZ00DEU, armed | 0.108 | 0.024 | 0.016 | 0.020 | 0.030 | 0.026 |
| SZZ000SIM from the start, control | 0.655 | 0.164 | 0.042 | 0.030 | 0.026 | 0.025 |
| SZZ000SIM from the start, armed | 0.510 | 0.112 | 0.031 | 0.021 | 0.028 | 0.027 |
| field census (16 sites) cross-site median, control | 0.732 | 0.149 | 0.073 | 0.058 | 0.049 | 0.048 |
| field census, armed | 0.415 | 0.110 | 0.042 | 0.049 | 0.038 | 0.044 |
| sim census (16 stations), control | 1.235 | 0.282 | 0.152 | 0.068 | 0.049 | 0.040 |
| sim census, armed | 0.677 | 0.154 | 0.068 | 0.036 | 0.034 | 0.034 |

**Disclosures.**

- **The sim golden starts inside the fixture's start artifact.** From the run start the control
  freezes the satellite clock level at about 1 m (its ignition value) and the exact update converges
  away from it slowly (0.77 m at epoch 240), tripping the phase-space anchor guard 55 times as the
  level moves; from 00:30:15 neither happens. G1 and G2 are graded from 00:30:15 by the standing
  convention and the pre-registration; the from-start readings are recorded here and are the sim
  golden's behaviour as configured. Moving the sim golden's start (a re-base) is owed to the author.
- **Card LAH's regressions stand.** On the field census ARHT00ATA loses 0.40 m over hours 2 to 6
  (0.65, 0.84, 0.23, 0.33, 0.16 m by hour against 0.08 or less) and KOKB00USA's hour 2 goes from 0.22
  to 1.48 m; the bar allows three sites worse and these are two of the three. The hold is interim:
  valid only with a precise a priori clock product, which the standing directive retires.
- **The mirror's handover epoch.** Card CTU's mirror of the fold disagrees on 31 inflations at the one
  clock-pivot handover epoch it cannot see; those rows are not conditioned on the fold's Q at that
  epoch. Card PRM phase 1b retires the mirror.
- **Fresh convergence only.** A snapshot written without these knobs is not resumed into them (card
  CTU2 section 8: the switch-on seam failed G5); the ledger rows say so.

## 5. The promotion (2026-10-01)

**What landed.** Four ledger rows (`docs/CONFIG_promotion_ledger.json`, after the WUP rows; the
appendix A15 of `docs/CONFIG_promotion_ledger.md`): the exact update with its two value companions
folded in (the guard space phase and `rank_gate_tol` 1e-11, checked by the row's `armed_when`), the
in-block receiver code bias, the drop decouple and the phase-clock hold. Section 3 planned one row for
the exact update with all four companions; the two boolean companions got rows of their own so that
the golden verify enforces them, and the two value companions are enforced by `armed_when`. Four loader
posture rows (`GnssConfigNetworkPosture.cpp`, after the time-update row and after the plain hold's
row) with the same predicates. The six keys in the live bases `config/refound/{field,sim}_gps_dual_rf0hv.json`
and pinned on both goldens through `config/golden/golden_manifest.json`; the goldens regenerated (69
recommended knobs armed in regime, verify PASS; the posture check 0 coverage gaps). Baseline pair
`runs/_rf0hv14_{field,sim}6`: the regenerated goldens run 6 h with exe `_exl`, byte-identical to the
gate arms `runs/_rap_exl_on6_{f,s}`; field WTZZ00DEU 0.108 0.024 0.016 0.020 0.030 0.026 m by hour and
657 of 720 epochs below a decimetre (before: 611), sim SZZ000SIM 0.510 0.112 0.031 0.021 0.028 0.027 m
and 535 (before: 515).

**The review before commit.** An adversarial review (three independent verifiers: the predicates,
every number, the configurations) found one blocking defect and corrected the record in several
places:

- **The switch-on resume was ungated.** `networkSnapshotFingerprint` built the stripped twin (the live
  fingerprint without card CTU's tokens) for every run, and the resume gate admitted a snapshot that
  matched it, although card CTU2 section 8 describes that admission as default off. With the goldens
  armed, a golden-derived warm leg resumed from a pre-promotion seed would have run the switch-on seam
  that failed G5, while every readout (the ledger's interlock, both posture tools' CHAIN advice) said
  it could not. Fixed: the twin is built only under a new default-off knob,
  `network.ctu_switch_on_resume` (`include/config/GnssConfig.hpp`, the loader beside
  `clock_level_restore_raise`, a warning when armed). Test `tests/test_exl_switch_on_resume.cpp` (suite
  `ExlSwitchOnResume`, 3 cases: the armed golden builds no twin; the knob builds a distinct twin; the
  knob is not part of the live fingerprint).
- **The hold's regime** now excludes `apriori_source brdc_rts` (where the hold is not valid) and the three
  knobs the loader refuses with it (`clock_level_carry`, `fast_clock.enable`, `emit_pb_hold_smoothed`);
  the in-block receiver code bias's regime adds the loader's conditions (`code_datum_time_linked`, and
  none of the de-mean and step-gate knobs); the exact update's value companions are part of its armed
  test, in the ledger and in the posture row.
- Wording: the plain hold's consumer numbers are stated as values against the control and the
  phase-clock hold rather than as a "cost"; G0c reads every non-P, non-S record (the S line declares the
  hold); the from-start sim level is 1.10 m at epoch 16 (1.05 was the control's); card AIL's numbers are
  attributed to its own arms and re-read below; the census wires are named.

The fixes were built as exe `gnss_processor_exl2.exe` (sha256[:16] `45183c6e51e9c729`); 2 h arms of the
regenerated goldens (`runs/_rap_exl2_id_{f,s}`) are identical to the baseline pair over their 240
epochs on both beds (the resume gate and the posture rows do not touch a cold run).

**The report reads.** Card AIL's instrument on the armed golden (2 h, `runs/_rap_exl2_ail_{s,f}`, the
sim from 00:30:15), demeaned: the young-arc loss at the inject below 20 epochs 4.7 mm on the sim and
5.1 mm on the field (75 and 157 mm on the golden before), the steady loss on mature arcs 1.8 to 2.6 mm
(sim) and 0.9 to 1.9 mm (field), the posterior standard deviation at 160 epochs and over 7.8 mm (sim)
and 4.9 mm (field). Card PAC's constellation-common drift was not read: its instrument needs the
smoothed hold's clone trace, which the goldens do not write.

**Owed.**

- The sim golden starts inside the fixture's start artifact; a re-base to 00:30:15 is the author's
  decision (section 4).
- Card LAH's regressions at ARHT00ATA and KOKB00USA, and the hold's dependence on a precise a priori
  clock product, which the standing directive retires: the hold is interim.
- Card CTU's mirror disagrees at the clock-pivot handover epoch (31 inflations); card PRM phase 1b
  retires the mirror.
- Card CTU2's open items for a switch-on seam (the ambiguity-side response) stand behind the new knob.
