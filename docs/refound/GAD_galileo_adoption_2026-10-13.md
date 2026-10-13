# GAD: the Galileo package adopted on the goldens

**STATUS**

- **id:** GAD
- **topic:** network-vehicle
- **status:** closed
- **summary:** ARMED 2026-10-13 at the author's decision: the Galileo package of cards GAL2 and GFC in the ledger (A16) and the goldens (77 armed in regime). Every confirmation gate passed: the field baseline is byte-identical to card GFC's graded arm, the same-exe census reproduces it (-0.020 m, 10 better, 2 worse), the sim census -0.030 m (13 better, 1 worse), GPS-only users unchanged at the median (ARHT00ATA +0.20 m). Baseline pair `runs/_rf0hv15_{field,sim}6`; fresh convergence only; 2 to 6 times the GPS-only epoch time depending on the load.
- **source:** the author's decision of 2026-10-13 ("Adopt the Galileo package into the ledger and goldens"); [GAL2](GAL2_galileo_on_the_exact_update_2026-10-08.md) section 6 (the four Galileo keys); [GFC](GFC_galileo_family_clock_datum_2026-10-10.md) sections 6 and 8 (the family-difference datum and the first-sighting family prior); [EXL](EXL_exact_update_lah_promotion_2026-10-01.md) and [WUP](WUP_windup_accumulated_promotion_2026-10-01.md) (the promotion template)
- **knobs / posture:** `processing.active_systems` `["GPS","Galileo"]`, `processing.signal_priorities.Galileo` `["C1C","C1X","L1C","L1X","C5Q","C5X","L5Q","L5X"]`, `estimation.clock_disc_isb_reinflate_std` 10.0, `model_corrections.enable_receiver_cross_system_atx_fallback` true, `network.code_datum_family_difference` true, `network.code_family_birth_prior_sigma_m` 0.10; ledger KEEP_OFF row for `network.code_datum_pair_continuity`; `frequency_mode` stays dual (E1 and E5a)
- **dates in the text:** 2026-10-13
- **hashes / run dirs:** exe `gnss_processor_galp2.exe` (main `3524acb` plus the posture rows of this card, sha256[:16] `c41165a2bc02e090`) for every gate run; the committed posture rows after review `gnss_processor_galp3.exe` (`01aab3ffeea299ae`, 2 h identity arms `runs/_rap_galp3_id_{f2,s2}`); evidence arms `runs/_rap_gfc2_fd_{f6,s6}` (exe `gnss_processor_gfc3.exe`, `33a9a91687b8a119`) and the GPS-only controls `runs/_rap_l5g_ctl_{f6,s6}` (`gnss_processor_l5g0.exe`); same-exe arms `runs/_rap_galp_*`; baseline pair `runs/_rf0hv15_{field,sim}6`; census keys `galp`, `galpc`, `galpg` (field) and `galp`, `galpc` (sim)
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: are the regenerated GPS + Galileo goldens the posture cards GAL2 and GFC graded, and what do they change for every later arm? Yes: the field golden's 6 h run is byte-identical to card GFC's arm, and the sim golden started at 00:30:15 to its sim arm. On one exe the field census is -0.020 m against GPS only (10 better, 2 worse: DAV100ATA, HARB00ZAF), the sim census -0.030 m (13 better, 1 worse); GPS-only users of the new wire are unchanged at the median, with ARHT00ATA +0.20 m. ARMED; baseline pair `runs/_rf0hv15_{field,sim}6`; the open items are in section 4.

## 1. The decision and what it adopts

The author decided on 2026-10-13 to adopt the Galileo package: card GAL2's four keys (Galileo beside GPS on E1 and E5a in both code families, the consumer's inter-system bias re-inflation, the receiver antenna cross-system fallback) and card GFC's two knobs (one Galileo clock datum across the pilot and combined code families, and the first-sighting family prior at 0.10 m). The evidence is in those cards: on the field census the package is 0.020 m better than GPS only at the median site in hours 2 to 6 (10 of 16 sites better, 2 worse: DAV100ATA +0.061 m, HARB00ZAF +0.016 m) and 0.22 m better in hour 1; WTZZ00DEU is better in every hour but hour 3 (+0.002 m); the Galileo clock wire is 0.022 m, level with GPS. The decision is taken with the open items of section 4 known.

The gates of section 3 are not a second decision. They confirm that the regenerated goldens are the posture that was graded, on one exe, and record what changes for every later arm (the baseline pair, the census on the same exe, the cost).

## 2. The change

- **Ledger** (`docs/CONFIG_promotion_ledger.json` and `.md`, appendix A16): six RECOMMEND_SCOPED rows and one KEEP_OFF row (`network.code_datum_pair_continuity`). Regime: the float vehicle on a GPS or GPS + Galileo set; `processing.active_systems` is in regime on a GPS-only set too, so a GPS-only float configuration reads as a gap; the five others need `n_sys >= 2`; the two GFC rows also need the satellite code-bias estimate and the time-linked code datum. The four value rows (the system list, the priority list, the 10 m re-inflation, the 0.10 m prior) carry an explicit `armed_when`.
- **Loader posture rows** (`src/core/config/GnssConfigNetworkPosture.cpp`): the same six rows, so the run's `Network POSTURE` line counts them; `scripts/config_posture_check.py` accepts `model_corrections.*` knob paths (the receiver ATX fallback is the first posture row outside the four sections it read).
- **Goldens** (`config/golden/golden_manifest.json`): the `processing.active_systems` pin becomes `["GPS","Galileo"]` and five pins are added on the same frozen bases; both goldens regenerated. `network.scoped_handover_response` (RECOMMEND_ON, already armed) comes into regime with the second constellation: 70 to 77 recommended knobs armed in regime, 6 unarmed (the 6 documented exceptions).
- **Not changed:** the live bases `config/refound/{field,sim}_gps_dual_rf0hv.json` (named for GPS dual, and already behind the goldens since card ICF); the sim golden's start (it still starts inside the fixture's start artifact and is graded from 00:30:15).

## 3. Gates (frozen before any run was read)

One Release exe with the posture rows built in grades every arm. All arms 6 h.

| gate | bar |
| --- | --- |
| U0 | Full Debug suite with no new failure; `make_golden_config.py --verify` PASS (77 armed in regime, 6 documented exceptions); `config_posture_check.py --verify-loader` 0 gaps; `docs_index_check.py` passes. |
| G0b | The regenerated goldens differ from the previous ones in provenance and the six keys only. The field golden equals `runs/_rap_gfc2_fd_f6/arm.json` apart from the output path; the sim golden equals `runs/_rap_gfc2_fd_s6/arm.json` apart from the output path, the start offset and an explicit default (`code_datum_pair_continuity` false). |
| G0a | `_rf0hv15_field6` byte-identical to `runs/_rap_gfc2_fd_f6` (`ident2h.py`, 720 epochs: products without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`); the sim golden started at 00:30:15 (`_rap_galp_on_s6o`) identical to `runs/_rap_gfc2_fd_s6`; the same-exe GPS-only controls `_rap_galp_ctl_f6` identical to `runs/_rap_l5g_ctl_f6`, `_rap_galp_ctl_s6` reported against `runs/_rf0hv14_sim6`. |
| ON | On the field baseline: family-difference rows and birth-prior rows counted at run end; the run's `Network POSTURE` tally equals the checker's (24/25 ON, 53/58 SCOPED); 720 of 720 epochs emitted on both beds. |
| Sim start | The new sim baseline starts at the golden's own start (inside the fixture's start artifact), a configuration no Galileo arm ran. Graded from 00:30:15: SZZ000SIM no worse than the same-exe GPS-only control `_rap_galp_ctl_s6` by more than 0.010 m in any of hours 2 to 6 (hours counted from 00:30:15). |
| Census, field | `gcensus.py`, the 16 sites and WTZZ00DEU, every key on `gnss_processor_galp2.exe`: the package (`galp`, consumers of the baseline's posture on its wire) against GPS only (`galpc`, consumers and wire of `_rap_galp_ctl_f6`). Bar: paired median over hours 2 to 6 at most 0 and more sites better than worse (on file: -0.0198 m, 10 / 2). |
| Census, GPS-only users | GPS-only consumers on the package wire (`galpg`) against `galpc`. Bar: paired median over hours 2 to 6 at most +0.005 m (card GAL2's G5 clause). |
| Census, sim | `simcensus6h.py` with `SIMCENSUS_FROM_START=1`, the 16 stations: the package (base and wire `_rf0hv15_sim6`) against GPS only (base and wire `_rap_galp_ctl_s6`). Bar: median change of the mean hourly p50 over hours 2 to 6 at most +0.005 m and at most 3 stations worse by more than 0.02 m. |
| Cost (read) | Wall time and the median epoch loop time of each baseline against its same-exe GPS-only control (all five arms run concurrently). |

A failed bar does not reverse the decision silently: it is recorded here with its reading and reported to the author before the commit.

## 4. Open at adoption (from cards GAL2 and GFC)

- DAV100ATA worse on the census (+0.061 m over hours 2 to 6; 52 % of its Galileo code rows in hours 2 to 4 come from five combined-born satellites; intermittent pilot-family coverage).
- The hour-2 clauses of card GFC step 2: the truth-bed consumer +0.014 m against a 0.010 m bar, the field pilot-family users 0.411 m against 0.30 m (read as the convergence of births at ignition).
- SEYG00SYC: the E11 / E12 C5Q gross rows (93 on the both-knobs arm against a bar of 10), a sparse-coverage transient of a newly registered column.
- Nominal yaw steering only (card OWD O7): the noon turns of E21, E27 and E31 fall in the window, each a phase transient of up to about 5 cm.
- E14 and E18 are broadcast unhealthy all day and stay on the wire (card HLT's health mode is built and unarmed).
- GPS-only consumers on the full-package wire were not measured before this card (card GAL2's `g2c_galg` read the four-key wire only); the census of section 3 reads them.
- Cost: about 4 to 5 times the GPS-only epoch time (card GAL2, under concurrent load).
- Card OWD's O8 (GPS clock wire +14 % with Galileo) does not reproduce on the ICF-armed golden (card GFC section 1) and is not formally closed.

## 5. 2026-10-13: the gates read (every number read by the lead from `runs/`)

All runs on `gnss_processor_galp2.exe` (sha256[:16] `c41165a2bc02e090`), the five producer arms concurrently, then the census replays.

| gate | reading | verdict |
| --- | --- | --- |
| U0 | Full Debug suite 3275 run, 3274 passed, 1 skipped (the fixture-gated `FixtureSmokeTest`), 0 failed; `make_golden_config.py --verify` PASS, 77 armed in regime and 6 documented exceptions on each golden (`--selftest` PASS); `--verify-loader` 137 of 137, 0 gaps; `docs_index_check.py` passes. | PASS |
| G0b | Regenerated goldens against the previous ones: the six keys and the provenance lines only. Field golden against `runs/_rap_gfc2_fd_f6/arm.json`: 0 knob differences (the output path only). Sim golden against `runs/_rap_gfc2_fd_s6/arm.json`: `start_offset_hours` and the explicit `code_datum_pair_continuity` false only. | PASS |
| G0a | `_rf0hv15_field6` against `_rap_gfc2_fd_f6`: products, closed-loop trace and epoch status IDENTICAL over 720 epochs; `_rap_galp_on_s6o` against `_rap_gfc2_fd_s6`: IDENTICAL; the GPS-only controls `_rap_galp_ctl_f6` against `_rap_l5g_ctl_f6` and `_rap_galp_ctl_s6` against `runs/_rf0hv14_sim6`: IDENTICAL (card ICF's bound does not move the sim from the run start either). | PASS |
| ON | Field baseline run end: the family difference carried on 720 of 720 kept epochs (719 on veterans, 1 re-seed); the family birth prior folded 27 rows on 25 satellites (max \|mu\| 0.0016 m); the run's `Network POSTURE` line reads ON 24/25, SCOPED 53/58, the checker's tally; 720 of 720 epochs emitted on every arm. | PASS |
| Sim start | Graded from 00:30:15, SZZ000SIM by hour: the new sim baseline 0.100 0.027 0.037 0.021 0.025 0.016 m (595 of 660 epochs below a decimetre) against the GPS-only control from the same start 0.244 0.031 0.040 0.017 0.032 0.026 m (535 of 660); worst hour 4 at +0.004 m. | PASS |
| Census, field | Package `galp` against GPS only `galpc`: hours 2 to 6 paired median -0.0198 m, 10 better, 2 worse (DAV100ATA +0.0605, HARB00ZAF +0.0158); hour 1 -0.2203 m, 14 better, 2 worse (CHTI00NZL 0.500 against 0.212 m, HARB00ZAF). The same-exe control equals the on-file `l5c_ctl` at every site, and `galp` equals `gfc2_fd`. Median site p50 by hour 0.452 0.112 0.045 0.053 0.039 0.044 -> 0.136 0.054 0.041 0.036 0.040 0.037 m. | PASS |
| Census, GPS-only users | GPS-only consumers on the package wire `galpg` against `galpc`: hours 2 to 6 median -0.0000 m, 2 better, 1 worse: ARHT00ATA +0.196 m (hours 3 and 4 1.227 / 0.565 m against 0.804 / 0.216); KOKB00USA -0.014, QUIN00USA -0.016; hour 1 +0.0018 m. | PASS (ARHT00ATA named) |
| Census, sim | Package against GPS only, both from the run start: hours 2 to 6 median -0.0304 m, 13 better, 1 worse (S57000SIM +0.019 m), none worse by more than 0.02 m; hour 1 -0.304 m, 10 better, 6 worse. Median station p50 by hour 0.677 0.154 0.068 0.036 0.034 0.034 -> 0.397 0.065 0.029 0.029 0.025 0.024 m. | PASS |
| Cost (read) | The run log gives the epoch loop's total wall only, so the reading is the MEAN, not the median the gate named: field 712.7 -> 2083.8 ms (2.9 times), sim 649.6 -> 1273.1 ms (2.0 times), the five arms on one shared load; wall (`exitcode.txt`) 599 -> 1585 s and 529 -> 989 s. Card GAL2 read 4 to 6 times under heavier concurrent load. | read |

**The baseline pair.** `runs/_rf0hv15_field6`: WTZZ00DEU 0.058 0.011 0.018 0.010 0.015 0.011 m by hour, 688 of 720 epochs below a decimetre (the same-exe GPS-only control `_rap_galp_ctl_f6`, which carries card ICF's bound that `runs/_rf0hv14_field6` predates: 0.113 0.023 0.016 0.020 0.030 0.026 m, 655). `runs/_rf0hv15_sim6` from the run start: SZZ000SIM 0.137 0.072 0.031 0.024 0.026 0.018 m by hour, 595 of 720 below a decimetre (`runs/_rf0hv14_sim6` 0.510 0.112 0.031 0.021 0.028 0.027 m, 535). Both renamed from `_rap_rf0hv15_*` with their output paths re-pointed, as `_rf0hv14` was.

**What changes for later arms.** Every arm copied from a golden is GPS + Galileo, 2 to 6 times slower than before depending on the load. `scripts/mday_golden_config.py` derives the multi-day goldens (card MDR, DOY 128 to 132) from the field golden, so they become GPS + Galileo too (the multi-day RINEX carry Galileo); card MDR's GPS-only numbers are re-captured, not compared. Tools that grade by constellation-blind medians or band digits now pool Galileo: `rapid_arm.py score` counts Galileo E5a in its band-5 columns, `ftc_step_attrib.py` R1 / R2 pool the two clock gauges (R3 needs `--systems G,E` to grade Galileo), `hold_rewrite.py` pools the two constellations. `scripts/clock_level/census6h.py` and `simcensus6h.py` default to GPS-only bases (`runs/_rf0hv10_field6`, `runs/_rf0hv13_sim6`): a census of a golden wire uses `gcensus.py` with the wire's own arm, or `simcensus6h.py` with `SIMCENSUS_BASE` set to the wire's arm. The flattened integer-ladder configs `config/sim/arl/*_producer.json` pin GPS themselves and are no longer golden copies.

**Owed.** The open items of section 4, with ARHT00ATA for GPS-only users of the wire added; the yaw law and the health mode are the Galileo-specific ones.

**Review (2026-10-13, one pass before the commit).** No blocking defect; every number in this section, the ledger appendix, the manifest descriptions and CLAUDE.md reproduced from `runs/`. Fixed after it:

- The six regimes require dual frequency (`not tri` in the ledger, `!r.tri` in the posture rows): the package was graded dual only, and a GPS-only triple float arm (the L5 arms) read `processing.active_systems` as a gap. The goldens are dual, so their tallies do not move. This amends the regime stated in section 2.
- The two GFC regimes take the parent row's code-bias test (`estimate_sat_code_bias` or `estimate_iono_sh`), the one the loader member mirrors.
- One cost statement everywhere: 2 to 6 times the GPS-only epoch time, depending on the load. The cost row above reads the mean (the run log gives the loop's total only).
- The field golden's description cites the same-exe GPS-only control as the pre-GAD baseline (`runs/_rf0hv14_field6` predates card ICF's bound).
- Card GAL's register row is annotated ARMED; `census6h.py` and `simcensus6h.py` state that their default bases are GPS only; the multi-day goldens are noted above.

The rebuilt exe `gnss_processor_galp3.exe` is identical to the baseline pair over 2 h on both beds (`ident2h.py`, 240 epochs: products, closed-loop trace, epoch status) with the same POSTURE tally; the Debug suite on it: 3275 run, 3274 passed, 1 skipped, 0 failed; `--verify` PASS (77 armed), `--verify-loader` 0 gaps. Not fixed, latent: `config_posture_check.loader_default_true` records every `.value("x", true)` as `network.closed_loop.x`, so an `on()` on a future default-true `model_corrections.*` knob would not be flagged as inverted (no current row is affected).
