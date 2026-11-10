# SCW: how an injected satellite clock step reaches the wire on the field golden

**STATUS**

- **id:** SCW
- **topic:** clock
- **status:** closed
- **summary:** Pre-registered and closed 2026-11-09. Why the GPS + Galileo field golden's wire does not follow a 0.5 m satellite clock step (card SLV section 7), traced on DOY 132 resumed legs. Three mechanisms in series: every arc of the satellite is freed (IF-TD flags on every non-reference station, reference-gate resets on the rest), the anchored clock holds against the code (about 1 mm per epoch while the ionosphere-free code residual keeps +0.38 to +0.48 m), and the phase-clock hold puts every clock move on the code side with no generation. With centring, the guard's same-epoch re-solve overshoots E02 by 0.38 m in the clock alone; card ARS's deferral removes it. Card SCR's route passes R1 to R5 on G01 and E02 on both seeds (wire within 0.5 mm); E34 at 1.51 h fails R2 on a frame event. Nothing armed at the close. Section 6 (after the close): the sat clock event and the deferral were already armed (2026-11-07); on the current golden the trip generation is the only phase-side declaration of a step (consumer peak error against the null +8.9 / +16.3 / +5.3 mm to -2.5 / +5.5 / -2.0 mm) but fires on every phase-statistic trip of the guard (DOY 132 census HARB +50.6 mm, sim hour 3 +13.4 mm): KEEP_OFF as built. Section 7 (2026-11-10): new key `network.emit_pb_hold_trip_generation_trigger` `clock_event` opens a generation only on a satellite clock event; event legs keep the response, the four 6 h arms equal the baseline record for record; both keys ARMED on the goldens, `_rf0hv18` unchanged.
- **source:** card SLV (`docs/refound/SLV_freed_arc_stale_level_2026-11-07.md` on the unmerged branch `slv/stale-level-response`) sections 6.7 and 7; card [SCR](../../refound/SCR_sat_clock_step_response_2026-10-26.md) sections 2 and 5; card [ARS](ARS_anchor_resolve_coupling_2026-10-26.md) (the guard's same-epoch re-solve against the exact coupling rows); card [SMX](../../refound/SMX_scenario_matrix_2026-10-24.md) (the satellite clock row fails on every posture); card [CRS2](CRS2_resume_hold_and_latches_2026-11-06.md) (the leg consumer resumes off its seed, so consumer channels are not graded here)
- **knobs / posture:** the GPS + Galileo field golden as seeded by card SLV (`evseed_slv_c`; `evseed_slv_g` adds `network.cycle_slip.if_phase_td_reseed_on_flag`); under test, all default off and fingerprint-exempt, so they resume on those seeds: `network.cycle_slip.if_phase_td_sat_clock_event` and `network.emit_pb_hold_trip_generation` (card SCR), `network.cycle_slip.if_phase_td_sat_centre` (card BRD), `network.clock_anchor_defer_coupled` (card ARS), `network.emit_pb_hold` with `network.emit_pb_hold_phase_clock` off (a read-out of the estimator's phase biases; emit-side only)
- **dates in the text:** 2026-11-09
- **hashes / run dirs:** branch `scw/sat-clock-wire` from `main` (`27d6ece`), worktree `D:/GnssProcessorSupport/worktrees/scw` (documentation only; no code change); exe `D:/GnssProcessorSupport/worktrees/scw_exe/gnss_processor_scw1.exe`, a renamed copy of card SLV's `gnss_processor_slv1.exe` (sha256[:16] `cc8c97892ebe782a`, built from `slv/stale-level-response` at `e95fff6`), the exe that wrote the seeds; legs `D:/GnssProcessorSupport/runs_nom/evleg_scw_*`; tools `D:/GnssProcessorSupport/worktrees/scw_tools/` (outside the repository)
- **gates:** section 4, frozen at pre-registration

**Outcome.** Closed 2026-11-09. On the field golden a satellite clock step is read as slips at every station, so its phase is freed; the anchor holds the clock against the code, and the phase-clock hold keeps the wire's phase side fixed with no generation. Card SCR's route makes the wire follow to 0.5 mm. Section 6: the trip generation lets the consumer re-base on a step but, firing on every phase-statistic trip, degrades quiet days. Section 7 limits it to satellite clock events (new key, `clock_event`): the event response is kept, quiet-day output is identical to the baseline, and both keys are armed on the goldens (2026-11-10).

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

Card SLV section 7 injected a 0.5 m `sat_clock_step` (every code and phase observation of one satellite at every station, from tow 553140) on the DOY 132 seeds of the GPS + Galileo field golden and found that the stepped satellite's emitted clock, code biases and phase biases stay within 0.06 m of the null leg for 45 epochs. With `network.cycle_slip.if_phase_td_sat_centre`, E02's emitted clock moves 0.88 m and G01's and E34's ramp to 0.14 to 0.23 m in 10 epochs. Three questions:

1. Without centring, where does the 0.5 m go (the refloated ambiguities, the code residuals, the station states), and why does the anchored clock not move?
2. With centring, why does E02 overshoot by 0.38 m? How do the clock and the phase biases split under `network.emit_pb_hold_phase_clock`, and what do the hold generations do?
3. Does card SCR's route (`network.cycle_slip.if_phase_td_sat_clock_event` with `network.emit_pb_hold_trip_generation`) make the wire follow on these field legs, as it did on card SMX's sim beds? Its 0.75 station-fraction gate might refuse a set of 9 flags at 16 stations.

The producer's wire is graded; the leg consumer resumes about 0.3 m off its seed (card CRS2's fixes are unmerged), so consumer channels are not.

## 2. Measured before the gates (2026-11-09, existing legs only)

Read from card SLV's legs with `scw_trace.py`, `scw_sta.py`, `scw_res.py` and `scw_coll.py`. Notation, each leg minus its null at epoch k after the step: `dC` the emitted clock of the stepped satellite with the median of the same over the other satellites of its constellation removed; `B-C` the code-visible clock d(B) - dC per code signal and `P-C` the phase-visible clock d(P) - dC per phase signal (the observation equation carries +B - C on code and +P - C on phase, `kSatBiasSign` +1 and `kSatClockPartial` -1 in `measurement/ObservationEquation.hpp`); `rIF` the ionosphere-free code residual of the satellite, median over its stations. A step of +0.5 m on the observations, carried by the wire, reads dC = -0.5 m, B-C = +0.5 m and either P-C = +0.5 m or a new hold generation. All values in metres.

**The flag count of card SLV section 7.1 is corrected.** At the step epoch G01 has 17 IF-TD samples: 9 at stations where it is not the chain's reference satellite, all 9 flagged (|dd - med| 0.486 to 0.531 against thresholds 0.107 to 0.239), and 8 where it is the reference, all reset by the reference gate (`refsat_reset`). Every G01 arc is freed. E02: 24 of 24 non-reference samples flagged, 2 reference resets. E34 at 1.51 h: 7 of 7, 1 reset. "9 of 16" counted the reference stations as unflagged.

**Question 1, without centring (legs `evleg_slv_c_{G01,E02}_5`; the `g` legs agree within 1 mm).**

| | k | dC | B-C | P-C, generation | rIF | guard at k |
| --- | --- | --- | --- | --- | --- | --- |
| G01 | 0 | +0.001 | -0.001 to +0.004 | 0.000, 1 | +0.483 | `e_phi` -0.007, `sqrtS_phi` 0.020, `e_clk` -0.001 |
| | 10 | -0.005 | +0.005 to +0.013 | 0.000, 1 | +0.457 | |
| | 45 | -0.059 | +0.059 to +0.063 | 0.000, 1 | +0.418 | |
| E02 | 0 | -0.063 | +0.063 | +0.001, 1 | +0.431 | `e_phi` -0.073, `e_clk` -0.064 (`w_clk` -9.1, not tested in phase space) |
| | 10 | -0.083 | +0.083 | +0.002, 1 | +0.407 | |
| | 45 | -0.106 | +0.105 | +0.002, 1 | +0.377 | |

- **The code keeps the step.** The ionosphere-free code residual carries +0.43 to +0.48 m at the step and still +0.38 to +0.42 m after 45 epochs: the wire's code-visible clock follows 1 to 12 % of the step on G01 and 13 to 21 % on E02. The code biases do not move (d(B) = 0.000 in every epoch: B-C is exactly -dC). The per-signal code residuals fall (G01 median +0.73 at k 0, +0.10 at k 45) because the free per-LOS slant ionosphere absorbs the part of a common step on two bands that an ionospheric term can fit, which is not ionosphere: G01's slant ionosphere moves +0.41 m (band 1) by k 10 and +0.54 m by k 45.
- **The phase step lands in the freed arcs and the satellite's internal phase bias.** At k 0 the ionosphere-free combination of the freed G01 arcs moves +0.44 m (median; +0.36 to +0.50 m at the 9 stations on L1W + L2W with both arcs in the trace). At the 6 stations on L1C + L2W it moves 0.00 to 0.04 m: the L1C phase datum hands over to CEBR00ESP in the same epoch, and the satellite's internal L1C phase bias takes the step for that group. Phase residuals do not move (median 0.000). On a resumed leg a freed restored arc keeps its value at its restored variance (card SLV section 2; at k 0 the arcs' variance is p10 1.1e-4, p50 1.3e-2 m2), so these arcs are tighter than freed arcs in a fresh run, and the phase still lands there.
- **The station states take a share.** Receiver clocks at the satellite's stations move +0.03 m (one satellite among about 17).
- **Why the anchored clock does not move.** The anchor folds the clock at `clock_anchor_q` 2.9e-8 m2/s (0.9 mm per 30 s) under the variance floor 1e-5 m2: a prior of 3.2 mm per epoch. Once every phase arc of the satellite is freed, the step reaches the clock only through the ionosphere-free code, and the solve moves the clock 1.4 mm per epoch on G01 and 0.9 mm on E02 (k 1 to 45). The guard's statistic is the innovation of the ionosphere-free phase clock, whose arcs were freed, so it reads -0.007 m on G01 against a bound of 6 x 0.020 = 0.12 m; the clock statistic `e_clk` reads -0.001 m. No test in the solve sees the step; it is visible only to the slip layer (the flags above) and to the code-row census (block Baarda flags G01 at 12 stations at k 0, t_B 5.6 to 57, none in the null).
- **The wire's phase side is frozen by construction.** Under `network.emit_pb_hold_phase_clock` the held phase bias is p(t0) + [c~(t) - c~(t0)] (`network/emit/PbHold.hpp`), so P-C is constant within a generation whatever the estimator does, and a generation starts only on a first sighting, a gap over 300 s or (with `network.emit_pb_hold_trip_generation`) a tripped clock. On a leg the hold is cold-started (its state is not snapshotted), so generation 1 starts at the leg's first epoch in the leg and the null alike.

**Question 2, with centring (legs `evleg_slv_c_k1148_{E02,G01}`).** The centring removes the common step from the pooled test (E02 column estimates 0.507 and 0.503 m in its two pairs; 0 flags; both reference resets kept). E02's arcs stay carried, the guard trips `phi` (`e_phi` -0.195 m, w 9.7) and the epoch is re-solved with E02's clock prior withdrawn (`clock_anchor_resolve`, the stdout line "withdrawn and the epoch RE-SOLVED before emission (pass 1)").

| | k | dC | B-C | P-C, generation | rIF |
| --- | --- | --- | --- | --- | --- |
| E02 | 0 | -0.879 | +0.879 | +0.001, 1 | -0.224 |
| | 10 | -0.880 | +0.880 | +0.002, 1 | -0.231 |
| | 45 | -0.866 | +0.866 | +0.003, 1 | -0.265 |
| G01 | 0 | -0.080 | +0.079 to +0.083 | 0.000, 1 | +0.357 |
| | 10 | -0.231 | +0.23 | +0.001, 1 | +0.250 |
| | 45 | -0.285 | +0.279 to +0.286 | +0.001, 1 | +0.209 |

- **E02.** The 0.38 m excess appears in the re-solved epoch and stays: the ionosphere-free code residual turns to -0.22 m (the code reads the overshoot), the clock re-couples at k 5 and the anchor holds the wrong value as it held the right one in question 1. The phase-clock hold transfers the whole emitted clock move into the held phase biases (the run-end `|transfer step|` maximum 0.8782 m), so P-C stays at +0.001 and the code side carries the overshoot (B-C +0.879). No generation starts: `network.emit_pb_hold_trip_generation` is off. The other satellites' clocks move at most 0.010 m at k 0 and 0.069 m at k 45. The estimator's own phase biases are hidden by the hold (run-end `|estimate - held|` p50 0.07 m, p90 0.65 m in the null as well), so the split between the clock and the phase bias inside the solve is not readable from these legs.
- **G01.** The centring acts only in the L1W + L2W pair (6 stations); the L1C + L2W pair has 3 non-reference samples, under `if_phase_td_sat_centre_min_stations` 4, so those 3 flags and the 4 L1C reference resets refloat, and the post-solve attribution (T4) refloats 5 more arcs: 12 of 17 freed. The solve moves the clock 0.08 m at k 0 (`e_phi` -0.085 m, under the 0.12 m bound) and 0.29 m by k 45.

**Question 3, read from the code before any leg.** Card SCR's rule (`networkSlipIfTdDriver`, `src/core/network/slip/SlipQc.cpp`) classifies per pair kind and satellite, over non-reference samples only: `n_obs` excludes the reference stations, and the 0.75 fraction is of `n_obs`. Every non-reference sample of all three satellites is flagged, so the fraction does not refuse any of them. The pair grouping does: `if_phase_td_sat_clock_min_stations` 4 applies within one pair kind. G01's L1C + L2W pair has 3 non-reference samples, E34's L1C + L5Q pair has 1.

## 3. The legs

Exe `gnss_processor_scw1.exe` (section header). Every leg 60 epochs, the step 0.5 m (`sat_clock_step`), made with `event_leg.py` of card SLV's worktree (the version that made its legs) and run with `event_leg.py rerun`. Each step leg has a null leg with the same keys on the same snapshot.

| tag | seed | snapshot, step epoch | keys added | satellites |
| --- | --- | --- | --- | --- |
| `scw_{c,g}_r` | `evseed_slv_{c,g}` | `ep1148` (9.56 h), at_ep 10 (tow 553140) | `if_phase_td_sat_clock_event`, `emit_pb_hold_trip_generation` | G01, E02, null |
| `scw_c_r182` | `evseed_slv_c` | `ep0182` (1.51 h), at_ep 1 (tow 523890) | the same | E34, null |
| `scw_c_kh` | `evseed_slv_c` | `ep1148`, at_ep 10 | `if_phase_td_sat_centre`; `emit_pb_hold` and `emit_pb_hold_phase_clock` false | E02, null |
| `scw_c_kd` | `evseed_slv_c` | `ep1148`, at_ep 10 | `if_phase_td_sat_centre`, `clock_anchor_defer_coupled` | E02, null |
| `scw_c_kt` | `evseed_slv_c` | `ep1148`, at_ep 10 | `if_phase_td_sat_centre`, `emit_pb_hold_trip_generation` | E02, null |

Legs are named `evleg_<tag>_<satellite>` and `evleg_<tag>_n`. Fourteen legs, at most four at once.

## 4. Gates (frozen 2026-11-09, before any leg of section 3 ran)

**Card SCR's route (`scw_{c,g}_r`, `scw_c_r182`), each step leg against its own null.** e_k = ||d_k| - 0.5 m| with d_k = dC at epoch k (card SLV's K2 definition, the clock record alone); c_k = the median over the satellite's code signals of |B-C - 0.5 m| at epoch k.

| gate | bar |
| --- | --- |
| R1, the route fires | A `sat_clock_event` naming the satellite and a `cta_decouple` with reason `slip_clk` naming it, both at the step epoch. |
| R2, the clock | Median of e_k over epochs 1 to 10 at most 0.05 m, and over epochs 11 to 45 at most 0.05 m. |
| R3, the code side | Median of c_k over epochs 1 to 10 at most 0.05 m. |
| R4, the phase side | Every phase-bias key of the satellite emitted at the step epoch carries a generation above the null's at that epoch (a `PB_HOLD` `gen_start` with cause `trip`). |
| R5, inertness | Each null leg's `products.txt` without comment lines byte-identical to the null without the keys (`evleg_slv_{c,g}_n95`, `evleg_slv_c_n182`). A difference is read and reported; a guard trip in a null that opens a generation is the designed response (card SCR section 5.4) and fails R5 only if it is not such a trip. |

Reported without a bar: the per-pair classification and the arcs kept and refloated at the step epoch, rIF by epoch, the other satellites' de-commoned clock moves at k 0, 10 and 45, and the `g` legs against the `c` legs.

Predictions (not bars): G01 is classified in the L1W + L2W pair only, keeps its arcs at the 6 flagged stations of that pair and at the 8 reference stations, and refloats the 3 flags of the L1C + L2W pair; E02 is classified in both pairs; E34 in L1X + L5X only, refloating its one L1C + L5Q flag. R1 to R4 pass on all five step legs; R5 passes on all three nulls.

**The overshoot (`scw_c_kh`, `scw_c_kd`, `scw_c_kt`), read without a bar.** Predictions written now:

- H, the estimator's phase biases. With the hold off, the C and B records are byte-identical to `evleg_slv_c_k1148_E02` (and the null's to `evleg_slv_c_k1148_n`): the hold touches no other record. E02's P-C at epochs 0 to 10 is within 0.15 m of +0.5 m on every phase signal: the estimator places the step correctly on the phase side, and the 0.38 m excess is the clock's, carried to the code side alone.
- D, the deferral. The trip at the step epoch is deferred (`cta_resolve_deferred`), the step epoch emits the first solve's clock (dC about -0.19 m), and from epoch 1 E02 folds LOOSE: median e_k over epochs 1 to 10 at most 0.10 m. A pass reads the overshoot as card ARS's defect, the withdrawal after the exact coupling rows were built.
- T, the trip generation. A generation with cause `trip` on E02's four keys at the step epoch; from it the wire's P-C equals that of leg H within 0.01 m, and C and B are byte-identical to `evleg_slv_c_k1148_E02`.

Nothing is armed by this card. Arming, a golden or ledger change and a merge are the author's decisions.

## 5. Results (2026-11-09)

All 14 legs of section 3 exit 0 (105 to 138 s each, below normal priority, on an otherwise idle machine). Graders `scw_gate.py` (R1 to R4), `scw_r5.py` (R5), `scw_q2.py` (H, D, T), `scw_trace.py`, `scw_sta.py` and `scw_coll.py`, outside the repository. No departure from section 3. Two readings stated before the numbers that use them: after a new hold generation, a leg's P-C against its null is the estimator's phase-visible clock in the leg minus the null's held one, so it is not expected to read +0.5 m (a float consumer refloats on the generation); and R5's clause is applied as frozen, a difference on a satellite whose clock the guard tripped statistically in the null without the keys being the designed response.

### 5.1 Card SCR's route on the field legs (question 3)

| leg | R1 | R2 median e_k, epochs 1 to 10 / 11 to 45 | R3 median c_k, 1 to 10 | R4 generation leg / null | verdict |
| --- | --- | --- | --- | --- | --- |
| `c` G01 | pass | 0.0005 / 0.0004 | 0.0005 | 2 / 1 on L1C, L1W, L2W | pass |
| `c` E02 | pass | 0.0002 / 0.0001 | 0.0002 | 2 / 1 on L1C, L1X, L5Q, L5X | pass |
| `g` G01 | pass | 0.0004 / 0.0003 | 0.0004 | 2 / 1 | pass |
| `g` E02 | pass | 0.0002 / 0.0001 | 0.0002 | 2 / 1 | pass |
| `c` E34, 1.51 h | pass | 0.016 / **0.234** | 0.016 | 2 / 1 on L1C, L1X, L5X | **R2 fails** |

| null against the null without the keys | records that differ | satellites | each preceded by a statistical trip in the null without the keys | R5 |
| --- | --- | --- | --- | --- |
| `scw_c_r_n` / `slv_c_n95` | 105 P | 1 (E07 from tow 553620) | yes | pass |
| `scw_g_r_n` / `slv_g_n95` | 6 P | 1 (E07 from 554610) | yes | pass |
| `scw_c_r182_n` / `slv_c_n182` | 2229 P | 23 (E18 from 524610; 22 GPS satellites from 524700) | yes | pass |

The C, B, E and every other record are identical in all three pairs.

- **The 0.75 fraction refuses nothing; the classification is as predicted.** G01 is classified in the L1W + L2W pair (step 0.504 m, dispersion 0.010 m, 6 of 6 non-reference samples) and keeps its arcs at those 6 stations and at its 8 reference stations (14 keep events); the 3 flags of the L1C + L2W pair, under `if_phase_td_sat_clock_min_stations` 4 in their own pair, refloat (DJIG00DJI, MRC100USA, NYA200NOR). E02 is classified in both pairs (10 of 10 and 14 of 14, steps 0.508 and 0.504 m) and keeps all 26 arcs. E34 is classified in L1X + L5X (6 of 6) and refloats its one L1C + L5Q flag (MRC100USA). Every classified satellite is decoupled with reason `slip_clk` in the step epoch, and its keys open generation 2 with cause `trip`. The `g` legs agree with the `c` legs to 0.1 mm.
- **The wire follows to half a millimetre.** G01 emits dC -0.502 m at the step epoch and -0.500 m from epoch 1 to 45; E02 -0.501 m, then -0.497 to -0.500 m. The code side reads B-C +0.497 to +0.502 m on every code signal, the ionosphere-free code residual of the satellite stays within 0.004 m of the null, its slant ionosphere moves at most 0.036 m by epoch 45 (against +0.54 m without the route), and no other satellite's clock moves more than 0.006 m. After generation 2, P-C reads +0.43 to +0.46 m on G01 and +0.43 to +0.50 m on E02.
- **A second generation on G01.** At epoch 16 (tow 553620) the guard trips G01 statistically (`e_phi` -0.138 m, w 6.89) on both seeds, after its re-coupling at epoch 5, and the keys open generation 3; the emitted clock stays at -0.500 m. The trip has no counterpart in the null.
- **E34 at 1.51 h follows for 24 epochs, then loses the step.** Epochs 0 to 23: dC -0.502 to -0.540 m. At epoch 24 (tow 524610) the constellation median of the Galileo phase statistic reads +0.50 m in the leg against +0.21 m in the null, 9 Galileo clocks trip `phi` in the leg (1 in the null) and E34's own clock moves +0.36 m against its prior (`w_clk` 11.6; `w_phi` 5.7, under k). At epoch 25 the gross-LOS screen declares E34's C1C row at PALM00ATA (8.05 m), the station that took E34's L1C phase datum at the step epoch, in the leg only. At epoch 27 a Galileo clock-pivot handover (E02 to E03, in the null as well) retires the Galileo frame, and E34's de-commoned clock returns to the null's (+0.007 m at epoch 30) before reading -0.27 m from epoch 39. Card SLV's E34 legs at the same snapshot show a clock event at the same time: +0.99 m at epoch 24 without centring, +0.24 m at epoch 29 with it. The frame-level move is not attributed by this card.
- **R5 shows the cost card SCR named.** In the 1.51 h null, a Galileo pivot handover with statistical trips on 22 GPS clocks (present without the keys) opens 61 key generations at one epoch and 62 at another: the trip generation acts on every statistical trip, not only on the classified event (card SCR section 6, item 1).

### 5.2 The centred E02 overshoot (question 2)

- **D, the deferral, removes the overshoot.** The guard trips E02 at the step epoch exactly as in the control (`phi`, w 9.73) and defers the correction (`cta_resolve_deferred`, "exact coupling rows reference the clock's own column"). The step epoch emits the first solve's clock (dC -0.195 m); from epoch 1 E02 folds LOOSE and emits -0.497 m, -0.503 m at epoch 2 and -0.515 m from epoch 13 to 45. Median e_k is 0.012 m over epochs 1 to 10 (control 0.383 m) and 0.015 m over epochs 11 to 45; the ionosphere-free code residual is +0.207 m at the step epoch and -0.010 to -0.023 m after it (control -0.22 to -0.27 m); the other satellites' clocks move at most 0.017 m at the step epoch and 0.005 m at epoch 45 (control 0.010 and 0.069 m). The prediction holds: the 0.38 m is created by the guard's same-epoch withdraw-and-re-solve after the exact coupling rows were built on the anchored clock (card ARS), here 76 % of the step on the field golden with the arcs carried, against 17 % in card ARS's unit fixture and a 24-fold move (12.19 m) in card SCR's sim leg.
- **H, the estimator's phase biases, refutes the prediction.** With the hold off the C, B and E records are byte-identical to the hold-on legs (leg and null), as predicted. E02's P-C is not +0.5 m: it reads +0.880 m at the step epoch and +0.86 to +0.92 m through epoch 45 on all four phase signals. The estimator's phase biases did not move in the re-solve (d(P) 0.001 m at the step epoch, within 0.05 m of zero through epoch 45); the clock alone moved -0.879 m, so the estimator's phase-visible clock over-follows by the same 0.38 m as the code side. The phase rows absorb the difference in the carried ambiguities (ionosphere-free combination -0.25 m, median over E02's stations) and the station states.
- **What the phase-clock hold does with it.** Under `network.emit_pb_hold_phase_clock` the held phase bias follows the de-commoned clock, so the wire's P-C stays at +0.001 m: the hold hides the over-follow on the phase side and the true step with it, and the code side carries all of the clock's move (B-C +0.879 m). With no trip generation, the consumer is told nothing.
- **T, the trip generation, puts the overshoot on the phase side as well.** Generation 2 with cause `trip` starts on E02's four keys at the step epoch; C, B and E are byte-identical to the control. Leg against leg, the emitted phase biases equal the estimator's (leg H) exactly at the step epoch and stay within 0.015 m through epoch 12. The prediction as written (P-C of T within 0.01 m of H's, each against its own null) fails on L1C (0.04 to 0.05 m) and L5Q (0.07 to 0.09 m): T's null holds phase biases that drifted from that null's estimator values over the 10 epochs since the leg's first epoch, which H's null does not. On the wire P-C reads +0.81 to +0.88 m: the generation tells a float consumer to refloat, which absorbs a phase-side offset at that epoch; the 0.38 m on the code side stays.

### 5.3 Station states without centring (question 1, read after the pre-registration from card SLV's legs)

Freeing every arc of one satellite moves a code-only direction at every station, seeing the satellite or not. On the G01 leg the GPS receiver DCB moves +0.15 m (median over 56 stations; +0.147 m at the 18 that see G01, +0.159 m at the 38 that do not) at the step epoch, +0.24 m at epoch 10 and +0.27 to +0.35 m at epoch 45; the receiver C1C inter-signal bias -0.25, -0.38 and -0.41 to -0.54 m; and the slant ionosphere of every other GPS line of sight +0.24, +0.38 and +0.53 m (median over 426 to 444 lines). The Galileo receiver DCBs move 0.000 to 0.008 m. The E02 leg moves the Galileo receiver DCB +0.04 m at the step epoch and up to +0.10 m by epoch 45 and leaves GPS untouched. The satellite clocks and code biases do not carry it (d(B) = 0.000), so it does not reach the wire: it is the receiver DCB against the per-line ionospheres, a direction the priors alone set. Its network-wide reach is not attributed here. With card SCR's route the satellite's own slant ionosphere moves 0.036 m by epoch 45.

### 5.4 Verdicts and the trace

| gate | `c` G01 | `c` E02 | `g` G01 | `g` E02 | `c` E34 |
| --- | --- | --- | --- | --- | --- |
| R1 | pass | pass | pass | pass | pass |
| R2 | pass | pass | pass | pass | **fail** (epochs 11 to 45) |
| R3 | pass | pass | pass | pass | pass |
| R4 | pass | pass | pass | pass | pass |
| R5 (its null) | pass | pass | pass | pass | pass |

Read-outs: D as predicted; H refuted (the estimator's phase-visible clock over-follows too); T as predicted leg against leg, the null-referenced 0.01 m clause failed.

**Why the golden's wire does not follow a satellite clock step.** Three mechanisms act in series. (1) The slip layer reads the step as a slip at every station: the IF-TD pool flags every non-reference sample and the reference gate resets the satellite where it is the chain's reference, so every arc is freed and the phase step lands in the arcs and the satellite's internal phase bias. (2) The anchor holds the clock with a 3.2 mm per epoch prior against the ionosphere-free code, which still reads +0.38 to +0.48 m after 45 epochs while the clock moves about 1 mm per epoch, and the phase-space guard cannot see a step whose phase was freed. (3) The phase-clock hold transfers every clock move into the held phase bias and declares no generation, so the wire's phase side never carries a clock move. When the arcs are kept (centring), the guard trips and its same-epoch re-solve puts 0.38 m too much into the clock, which the anchor then holds; the deferral removes that. Card SCR's route acts on (1) and (3) and avoids the re-solve of (2): on the four 9.56 h legs the wire carries the step to half a millimetre on both seeds.

Nothing is armed. Open for the author: arming `network.cycle_slip.if_phase_td_sat_clock_event` and `network.emit_pb_hold_trip_generation` (UNVALIDATED in the ledger; one of five legs fails R2 on a frame event at 1.51 h, and the trip generation acts on every statistical trip, 61 keys at one epoch of the 1.51 h null); arming `network.clock_anchor_defer_coupled` (card ARS), which removes the field overshoot when the guard trips a clock whose arcs are carried; the Galileo frame move at E34's epoch 24; the network-wide receiver DCB move of section 5.3.

## 6. The current golden and the trip generation (2026-11-09, after the close)

The author asked, after the close: "Yes, if it is an improvement, then turn it on!" (for the keys of section 5.4), under the 2026-11-07 arming rule (arm unless measured harmful; one 6 h health check).

**Correction to section 5.4.** Its last paragraph lists `network.cycle_slip.if_phase_td_sat_clock_event` and `network.clock_anchor_defer_coupled` as open for the author. Both were already ARMED in every golden on `main` since 2026-11-07 (`f8513e4`, card RF0HV18B section 6); the seeds of sections 3 to 5 were written by card SLV's exe from before that commit. Of the three keys, only `network.emit_pb_hold_trip_generation` was unarmed (ledger UNVALIDATED).

### 6.1 Event legs on the current golden

Exe `D:/GnssProcessorSupport/gates/scwt/x64-Release/gnss_processor_scwt.exe` (sha256[:16] `64543d2d89994cae`, the exe of baseline `_rf0hv18`). Seed `evseed_scwt_f129`: the current field golden on DOY 129, snapshot `ep0242`; a 0.5 m `sat_clock_step` on G13, E09 or G22 at the leg's tenth epoch (tow 266760), 60 epochs, each leg against its own null; arm `scwt_l0` (the golden) and `scwt_lt` (the golden plus `network.emit_pb_hold_trip_generation`). Legs `D:/GnssProcessorSupport/runs_nom/evleg_scwt_{l0,lt}_{G13,E09,G22,n}`; grader `D:/GnssProcessorSupport/gates/scwt/legs_grade.py` (the gates of section 4).

| leg | R1 | R2 median e_k, epochs 1-10 / 11-45 (m) | R3 | R4 (generation at the step, leg / null) | in-loop consumer, peak 3D error leg minus null |
| --- | --- | --- | --- | --- | --- |
| G13 golden | pass (`slip_clk`) | 0.0023 / 0.0020 | pass | **fail** (1 / 1) | +8.9 mm (epoch 8) |
| G13 + trip generation | pass | 0.0023 / 0.0020 | pass | pass (2 / 1) | -2.5 mm |
| E09 golden | pass | 0.0007 / 0.0018 | pass | **fail** (1 / 1) | +16.3 mm (epoch 31) |
| E09 + trip generation | pass | 0.0007 / 0.0018 | pass | pass (2 / 1) | +5.5 mm |
| G22 golden | pass | 0.0009 / 0.0016 | pass | **fail** (1 / 1) | +5.3 mm (epoch 8) |
| G22 + trip generation | pass | 0.0009 / 0.0016 | pass | pass (2 / 1) | -2.0 mm |

On the current golden the clock and code biases follow the step to 2 mm (the satellite clock event is armed); the phase side does not. Without the trip generation the held phase bias absorbs the clock move (P-C unchanged against the null), no generation is declared, and the in-loop consumer reinflates the satellite (`reinflate_isb`, ambiguity variance 1e6) on phase innovations of -6 to -16 m (G13) and -66 to -115 m (E09) one epoch after the step. With it, generation 2 starts on every key of the satellite at the step epoch, the consumer re-bases (`reset_hold_gen`, phase innovation 0, ambiguity variance about 0.06 m2) and its peak error falls on all three satellites. P-C moves -0.43, +0.24 and +3.05 m at the generation (the held value re-based to the estimator's phase bias, which carries the freed arcs); a float consumer absorbs that.

### 6.2 Health check (6 h, same exe)

Arms are the three goldens plus only `network.emit_pb_hold_trip_generation`: `_rap_scwt_f6`, `_rap_scwt_s6` and `_rap_scwt_b6` against the baseline `_rap_srn_{f6,s6,b6}`; DOY 132 (the field golden on `D:/GnssProcessorSupport/data_mday/2024-132`) is `_rap_scwt_g6` against its own control `_rap_scwt_g6c`. The censuses are `cscwtf` against `c18a`, `s18t` against `s18a`, and `cscwtg` against `cscwtgc`. Every run exits 0 with 720 of 720 epochs emitted. Grader: `D:/GnssProcessorSupport/gates/scwt/grade_scwt.py`.

| bed | trip generations (keys, epochs, most keys in one epoch) | in-loop consumer, worst settled hour arm minus control | census median, hours 2-6 | worst census site (mean, worst hour) |
| --- | --- | --- | --- | --- |
| field DOY 129 | 10, 2, 6 | +3.8 mm (hour 2) | +0.00 mm | WTZZ +0.67 (+3.8); ANMG -7.6 |
| field DOY 132 | 168, 9, 33 | +4.5 mm (hour 3) | +2.25 mm (14 of 17 sites over +0.19 mm) | **HARB +50.6 (+129.6)**, SCRZ +16.4, CEDU +11.4 |
| sim | 170, 19, 32 | **+13.4 mm (hour 3)** | -0.18 mm | **s60 +18.1 (+28.7)**, s37 +8.9 (+42.5); s57 -17.8 |
| BeiDou lineage | 6, 1, 6 | +0.35 mm | not run | |

Every quiet-day trip that opened a generation is a phase-statistic trip of the anchor guard (`cta_decouple` with `reason=phi`): a constellation-wide decoupling with a common `e_phi` (for example eight Galileo satellites at -0.26 m, or eleven GPS satellites at +1.16 m, on DOY 132), never `slip_clk`. In the control those trips leave the wire's generations alone and the consumers do better; with the key each one restarts every key of up to 33 satellites at once, and every consumer refloats them.

### 6.3 Verdict

`network.emit_pb_hold_trip_generation` improves the response to a satellite clock step and is measured harmful on quiet days (DOY 132 census, the sim's hour 3 and census site s60). Kept off: ledger row KEEP_OFF, with a look-here note. The goldens are unchanged (`make_golden_config.py --verify` passes).

The data point to a narrower trigger: every injected step decoupled with `reason=slip_clk`, every quiet-day trip with `reason=phi`. A generation started only on the declared clock events (`slip_clk`, `slip_clk_late`; the two insertion sites into `ClockAnchorState::stepped` in `src/core/network/solve/ClockAnchor.cpp`, read only by the phase-bias hold) would keep section 6.1's response and leave these three quiet days without a trip generation. It is not built; it needs the event legs and the 6 h health check again before it can be armed.

## 7. A trip generation on clock events only (pre-registered 2026-11-09, before any run of this section)

The author approved building section 6.3's narrower trigger ("Yes, build it"). New key `network.emit_pb_hold_trip_generation_trigger` (`any_trip`, the default and the rule as built; `clock_event`). The guard's per-epoch set `ClockAnchorState::stepped` now carries each satellite's cause (`ClockStepCause::TestTrip` for `phi`, `nis` and `abs`; `ClockStepCause::ClockEvent` for `slip_clk` and `slip_clk_late`); under `clock_event` the phase-bias hold opens a generation only on a clock event and counts the held keys of a test-tripped satellite (`n_pbh_trip_test_skipped`, on the run-end PBH line). Default off and byte-identical when off: only the hold reads the set, and only with `network.emit_pb_hold_trip_generation` armed. An unknown trigger is refused by the loader. Unit tests: four new cases in `tests/network/test_pb_hold_trip_generation.cpp`.

Arm `scwt_lc`: the golden plus `network.emit_pb_hold_trip_generation` true and the trigger `clock_event`, on the exe built from this branch. The event legs repeat section 6.1 (seed `evseed_scwt_f129`, snapshot `ep0242`, step at the tenth epoch, 60 epochs; G13, E09, G22 and a null). That arm's legs and null contain four `phi` decouplings each (14 trip-generation keys in the null), so `clock_event` is tested against test trips inside the legs as well.

Gates, frozen here:

- **E1.** Each `scwt_lc` step leg passes R1 to R4 of section 4. Its trip generations are exactly the stepped satellite's keys at the step epoch; its null has none.
- **E2.** On each of G13, E09 and G22 the peak in-loop consumer error, leg minus its own null, is below the golden's (section 6.1: +8.9, +16.3, +5.3 mm).
- **E3.** The `scwt_lc` null's wire (C, B and P records) equals the golden null's (`evleg_scwt_l0_n`) record for record. A difference is reported with its size: the exe differs, so the gate also tests build reproducibility.
- **H1.** The 6 h arms on the field (DOY 129), the sim, the BeiDou lineage and DOY 132, each the golden plus the two keys, exit 0 with 720 of 720 epochs emitted.
- **H2.** None of the four opens a trip generation, and the run-end PBH line reports the test-trip keys held (non-zero on DOY 132 and the sim, where section 6.2 counted 168 and 170).
- **H3.** Each arm's wire equals its control's record for record (`_rap_srn_{f6,s6,b6}` and `_rap_scwt_g6c`, the exe of baseline `_rf0hv18`). If a build difference breaks equality, same-exe controls and the three censuses of section 6.2 run, with section 6.2's +10 mm bars on the settled in-loop hours and the worst census site.

Verdict rule: E1 to E3 and H1 to H3 pass, then both keys are armed in the goldens under the 2026-11-07 rule (H3 means no quiet-day output moves, so the `_rf0hv18` numbers stand). A failed gate is reported and nothing is armed.

### 7.1 Results (2026-11-10)

Exe `D:/GnssProcessorSupport/gates/scwt/lc/x64-Release/gnss_processor_scwlc.exe` (sha256[:16] `d360a0ae65085b7f`), built from this branch at `d02a4c8`. Legs `D:/GnssProcessorSupport/runs_nom/evleg_scwt_lc_{G13,E09,G22,n}`; arms `_rap_scwlc_{f6,s6,b6,g6}`. Graders `D:/GnssProcessorSupport/gates/scwt/lc/legs_grade_lc.py` and `compare.py`.

| gate | result |
| --- | --- |
| E1 | pass. R1 to R4 pass on G13, E09 and G22 (median e_k 0.0023 / 0.0020, 0.0007 / 0.0018, 0.0009 / 0.0016 m; generation 2 on every key at the step). Trip generations: the stepped satellite's keys at tow 266760 only (3, 4 and 3 keys); the null opens none; 14 test-trip keys held in each leg and in the null (the four `phi` decouplings that opened generations in `scwt_lt`). |
| E2 | pass. Peak in-loop consumer error, leg minus null: G13 -2.5 mm (golden +8.9), E09 +5.5 mm (+16.3), G22 -2.0 mm (+5.3); the same peaks as `scwt_lt`, whose test-trip generations fell on leg and null alike. |
| E3 | pass. The null's products (29,483 lines) and its in-loop consumer trace equal `evleg_scwt_l0_n` line for line: the new build reproduces the baseline exe. |
| H1 | pass. All four arms exit 0; 720 of 720 epochs. |
| H2 | pass. No trip generation on any arm. Test-trip keys held: field DOY 129 10, sim 170, BeiDou 6, DOY 132 168 (section 6.2's counts exactly). |
| H3 | pass. Products and in-loop consumer traces equal their controls line for line: field 348,247 lines against `_rap_srn_f6`, sim 241,030 against `_rap_srn_s6`, BeiDou 511,690 against `_rap_srn_b6`, DOY 132 348,058 against `_rap_scwt_g6c`. No census is needed: the consumers replay identical wires. |

Unit tests: the nine `PbHoldTripGeneration` cases and the `ClockAnchorSlipClk`, coupled re-solve and satellite clock event suites pass on the Release build (35 cases); the full suite is reported in the commit that arms the keys.

**Verdict and arming.** Every gate passes. Under the 2026-11-07 rule both keys are pinned on the field and sim goldens (so on the BeiDou lineage): `network.emit_pb_hold_trip_generation` true and `network.emit_pb_hold_trip_generation_trigger` `clock_event`, ledger rows ARMED with look-here notes; the `any_trip` behaviour stays as measured in section 6. The regenerated goldens equal the tested arms except for their run paths, and H3 shows no quiet-day output moves, so baseline `_rf0hv18` stands unchanged. What the arming adds is section 6.1's event response: on a satellite clock step the IF-TD pools read, the wire declares a new phase-bias generation and the consumer re-bases instead of reinflating. A clock step below the IF-TD floor (0.107 m), which only the guard's statistic reads, still opens no generation.
