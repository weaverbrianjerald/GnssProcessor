# EXL: card CTU's exact coupling update armed with card LAH's phase-clock hold (the promotion)

**STATUS**

- **id:** EXL
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-01 (the author's decision after cards LGM and AIL: arm card CTU's exact update on the goldens, keep card LGM's nuisance state off; then, on fresh evidence that the exact update cannot ship with the goldens' plain phase-bias hold, arm it together with card LAH's phase-clock hold as an interim hold for the precise a-priori-clock posture, with card CTU's failing gates G4 and G8 re-stated as relative and G5 disclosed). Five solve knobs (the exact update and its four measured companions) and one emit knob (the hold), by the promotion path of card WUP; gates in section 3.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 6, 10, 13 and 14; [CTU2](CTU2_level_recursion_followup_2026-09-29.md) sections 2 to 9 and 12; [LAH](LAH_level_aware_hold_2026-09-29.md); [AIL](AIL_ambiguity_information_loss_2026-10-01.md) section 4; [LGM](LGM_los_phase_nuisance_2026-10-01.md) section 5.3; [WUP](WUP_windup_accumulated_promotion_2026-10-01.md) (the promotion template); [SMH](SMH_smoothed_hold_clone_2026-09-29.md) section 9 (the fold kappa default)
- **knobs / posture:** `network.carry_coupling_exact_update`, `network.carry_rx_code_bias_in_block`, `network.amb_float_drop_decouple_always`, `network.clock_anchor_guard_space` phase, `network.rank_gate_tol` 1e-11, `network.emit_pb_hold_phase_clock`; NOT armed: the smoothed holds, the monitors, `network.carry_coupling_exact_cross`, `network.clock_level_bound_raise`, `network.phase_colour_state`
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** exe `gnss_processor_ctua.exe` (main `3c2969e`, sha256[:16] `7e8196a64cdc3b5c`) for the 2 h evidence of section 2; the gate exe is named in section 4; arms `runs/_rap_ctx_*`, `runs/_rap_exl_*`

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

## 4. The promotion (to be filled when the gates have run)
