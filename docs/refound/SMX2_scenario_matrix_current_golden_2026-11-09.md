# SMX2: the fault-scenario matrix on the goldens as armed on 2026-11-09, the floor at the seam, and the trip generation's trigger

**STATUS**

- **id:** SMX2
- **topic:** harness
- **status:** closed
- **summary:** PRE-REGISTERED and CLOSED 2026-11-09. The fault-scenario matrix on the goldens as armed today passes 23 of 55 cells (card SMX's `smx1` and card CRS2's `crs2b`: 22): the satellite clock step passes on the golden posture for the first time, through the clock-event rule armed on 2026-11-07; with `network.emit_pb_hold_trip_generation` 24 of 55 (the step also passes on the native posture; the self-contained regime's consumer misses by 43 mm; the storm's native consumer is 4.5 times worse); the storm fails on every posture in every label. Stage B: card SLV's seam cost of the 100 m2 floor was the broken leg consumer (S1 passes at 0.1 mm), and the floor's guard misses a 3 m clock step on E34 that the control catches, resumed or not (K1 fails). Stage C not built: card SCW section 7 took up the narrowing. Stage D: the floor removes the three-band event of card L5B and passes card ALD's gates (4) and (5). Nothing armed; no C++ change.
- **source:** card [SMX](SMX_scenario_matrix_2026-10-24.md) (the matrix, label `smx1`, 22 of 55); card [CRS2](CRS2_resume_hold_and_latches_2026-11-06.md) sections 5.5, 5.6 and 6 (label `crs2b`, the resume keys, the consumer channels read on a converged leg consumer); card [SCR](SCR_sat_clock_step_response_2026-10-26.md) sections 6 and 7 (the two keys of the satellite clock step and the trigger of the trip generation, item 1); card [ARS](ARS_anchor_resolve_coupling_2026-10-26.md) (the guard's deferral); card [RF0HV18B](RF0HV18B_reduced_candidate_2026-11-07.md) section 5.1 (the keys armed on 2026-11-07); card SLV (`docs/refound/SLV_freed_arc_stale_level_2026-11-07.md` on the unmerged branch `slv/stale-level-response`, sections 5, 6.6, 6.8 and 7); card SCW (`docs/refound/SCW_sat_clock_step_wire_2026-11-09.md` on the unmerged branch `scw/sat-clock-wire`, section 5.1, the E34 leg at 1.51 h)
- **knobs / posture:** stage A: postures `g`, `c`, `n`, `gx` of card SMX built by `scripts/scenario_matrix.py` from today's sim golden; label `smx2t` adds `network.emit_pb_hold_trip_generation` through `make --extra`. Stage B: the DOY 132 field golden derived from today's field golden (`scripts/mday_golden_config.py`, obs root `D:/GnssProcessorSupport/data_mday`), control as armed, arm plus `network.amb_float_drop_var_m2` 100. Stage C: not built (taken up by card SCW section 7, key `network.emit_pb_hold_trip_generation_trigger`). Stage D: today's field golden with the `t1on` keys of card ALD, with and without the floor. Attribution labels `smx2x` and `smx2xt` with `network.raim_prior_channel` false. Nothing armed, no golden, manifest, ledger or loader default edited.
- **dates in the text:** 2026-11-09, 2026-11-10 (the last runs after midnight)
- **hashes / run dirs:** branch `smx2/fault-response` from main `27d6ece`, worktree `D:/GnssProcessorSupport/worktrees/smx2`; exe `gnss_processor_smx2.exe`, a renamed copy of that worktree's Release build (AVX2, the default; the only change on the branch at that build is the seed-family option of `scripts/scenario_matrix.py`, no C++); seeds `evseed_smx2_{g,c,n,gx}` (sim) and `evseed_smx2_f132`, `evseed_smx2_f132A` (field) under `D:/GnssProcessorSupport/runs_nom`; legs `evleg_smx_smx2*_*` (matrix and attribution labels) and `evleg_smx2_*` (stage B); arms `_rap_smx2_d_*` (stage D) and `_rap_smx2_kf_*` (the K1 addendum); gate outputs `D:/GnssProcessorSupport/gates/smx/smx2*/`; helper scripts `D:/GnssProcessorSupport/worktrees/smx2_tools/` (outside the repository; copies of cards SLV's and SCW's graders)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Question: how many fault scenarios does the network producer survive on today's goldens, does the 100 m2 floor cost the seam consumer once the leg consumer resumes correctly, and does the floor blind the clock guard? Decisive result: 23 of 55 cells pass as armed and 24 with the trip generation; the satellite clock step now passes on the golden and the storm still fails everywhere. The floor's seam cost was the harness's, but its guard misses a 3 m step the control catches. On the three-band event the floor passes both of card ALD's gates. Verdict: scoreboard up by one; nothing armed. Standing: section 6.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

Label `smx1` (card SMX, 2026-10-24) scored 22 of 55 cells; label `crs2b` (card CRS2, 2026-11-06) re-read every consumer channel on a leg consumer that resumes at its seed's convergence and found the same 22. Both predate the keys armed on 2026-11-07 (card RF0HV18B section 5.1), among them `network.cycle_slip.if_phase_td_sat_clock_event`, `network.clock_anchor_defer_coupled`, `network.cycle_slip.if_phase_td_reseed_on_flag`, `network.raim_prior_channel` and `estimation.seed_full_equation` (fingerprinted). Both ran on SSE2 builds. Today's scoreboard is therefore not known, and the two rows that failed on every posture (the satellite clock step, the slip storm) have not been read under the armed response.

Two open items of other cards ride on the same seeds and harness. Card SLV found that the floor `network.amb_float_drop_var_m2` at 100 m2 with the re-seed key passes every field gate and fails clause S1 (the seam consumer, +0.10 to +0.12 m), read through a leg consumer whose own warm start was broken (card CRS2's keys were not on that branch), and it left open whether the floor lets through a clock step that the control's guard catches (its section 7 found only one satellite where the control's guard tripped). Card SCR section 6 item 1 left open the trigger of `network.emit_pb_hold_trip_generation`, which opens a hold generation on every statistical trip of the clock anchor (61 keys at one epoch of a null leg in card SCW section 5.1).

## 2. Stage A: the matrix today

**Matrix.** `scripts/smx/matrix.json` version 1, unchanged (rows, channels, factors, floors, the PLANT reference, the drift row; sha256 recorded in each plan). Every leg resumes the seed's snapshot index 8 (4 h after the 00:30:15 start), 60 epochs (the storm 240), the event at `at_ep` 10, targets resolved by `make` from each seed (`G25` unless it is a GPS clock pivot at the snapshot; the `L2W` anchor station of the satellite at the snapshot epoch for `lol`). Version 2 items of cards SMX and CRS2 (the anchor resolved at the event epoch, the `pb` channel's de-commoning, a truth reference for the slant ionosphere) are not taken up here, so that the verdicts stay comparable with `smx1` and `crs2b`.

**Seeds.** Fresh, from the postures as armed on main `27d6ece`: `SMX_SEED_TAG=smx2 python scripts/scenario_matrix.py seed <p> --hours 4.5 --exe <exe> --launch` for `p` in `g`, `c`, `n`, `gx`, each after a 10-epoch smoke of the same posture. `event_leg.py write-seed` arms card CRS2's captures (`network.state_snapshot_hold_capture`, `realtime.state_snapshot_latch_capture`) on every seed and `make-leg` / `null` arm the restores on every leg. 4.5 h is enough for the snapshot at 4 h; the legs do not read the seed past it. The seeds `evseed_smx_*` of 2026-11-06 are not resumed: they predate the 2026-11-07 keys, and `estimation.seed_full_equation` is in the snapshot fingerprint. The seed family is a new option of `scripts/scenario_matrix.py` (`SMX_SEED_TAG`, default `smx`, recorded in `plan.json` when set; with it unset every name and plan is as before).

**Labels.** `smx2a`: the matrix on the armed postures, nothing added. `smx2t`: the same plus `network.emit_pb_hold_trip_generation` true on every leg (`make --extra`; the key is not fingerprinted and acts only at a guard trip, card SCR section 4). `smx2actl`: rows `slip100`, `sta_out`, `step1m` on `g` and `c`, the same exe, its own nulls. Runs at two legs at a time.

**Readings.** The verdict table of every cell for `smx1`, `crs2b`, `smx2a` and `smx2t`, with every verdict change explained from the legs (the channel that moved, and what in the event log moved it). The earlier labels ran on SSE2 builds and earlier code: verdicts and channel magnitudes are compared, bytes are not. Rows `sat_clk` and `storm` are read channel by channel on every posture, with the responses that act in them: satellite clock events and kept arcs, `slip_clk` decouplings, statistical trips and deferrals, refloats, hold generations by cause, consumer refloats.

## 3. Stage B: the floor at the seam and against a step the guard can see

**Seeds.** The DOY 132 field golden derived from today's field golden by `scripts/mday_golden_config.py` (56 stations; MAC100AUS and LCK400IND have no RINEX that day): `evseed_smx2_f132` (the golden as armed, which includes `network.cycle_slip.if_phase_td_reseed_on_flag`, so it is card SLV's control `g`) and `evseed_smx2_f132A` (plus `network.amb_float_drop_var_m2` 100, SLV's arm `Ag`). `event_leg.py write-seed`, 10 h, a snapshot every 60 emitted epochs, card CRS2's captures armed, each after a 10-epoch smoke. Card SLV's `c` pair (re-seed key off) is not re-run: the golden now arms the key.

**S1 and S2 re-graded.** One null leg of 60 epochs per seed from the snapshot nearest emitted epoch 360 (hour 3, card SLV section 6.6), card CRS2's restores armed, against the seed's own continuation, by card SLV's `slv_seam.py`.

**The guard question.** Every leg below, nulls included, adds `network.cycle_slip.if_phase_td_sat_centre` (card BRD), as in card SLV section 7: the pooled IF-TD test thresholds the residual of a station by satellite median polish, so a satellite's common step is not read as slips, the armed clock-event rule has no flags to classify, the arcs stay carried, and the step reaches the guard's statistic. Steps (`sat_clock_step`, every code and phase of the satellite at every station):

| snapshot | `at_ep` | satellite | steps |
| --- | --- | --- | --- |
| nearest emitted epoch 1148 (9.56 h) | 10 | G01 | 0.5 m, 1.0 m |
| same | 10 | E02 | 0.5 m, 1.0 m |
| nearest emitted epoch 182 (1.51 h) | 1 | E34 | 0.5 m, 1.5 m, 3.0 m |

One null leg per seed and snapshot. The satellites are card SLV's (G01 and E02 tie at the floor of `sqrtS_phi` at 9.56 h; E34 at 1.51 h is where the floor's predicted size was largest against the control's), so the legs repeat card SLV section 7 on main's code and add the larger steps that make the control's guard trip where it did not at 0.5 m (E34's control statistic was +0.575 m at a predicted size of 0.401 m, a bound of 2.4 m). The legs and the null legs carry the CRS2 restores. If a satellite named here is not anchored or not observed at the snapshot on a seed, the leg is reported as unread for that satellite; no substitute is chosen after the seeds are read.

## 4. Gates (frozen 2026-11-09, before any seed or leg below ran)

A failed clause stays failed; no bar moves after this section is committed.

**Stage A**

| gate | bar |
| --- | --- |
| A0, seeds | Each of the four 10-epoch smokes and the four 4.5 h seeds exits 0; each seed has at least 9 history snapshots; the snapshot at index 8 carries `PH` rows (the hold capture) and its consumer snapshot carries the four `I` rows (the latch capture). |
| A1, the matrix as armed | Label `smx2a`: 65 legs, each exits 0, every cell graded by `scenario_matrix.py grade` against the version-1 bars; the verdict table against `smx1` and `crs2b` reported with every verdict change explained. A FAIL cell is a finding, not a defect of the gate. |
| A2, the matrix with the trip generation | Label `smx2t`: the same, its verdicts against `smx2a`, every change explained. |
| A3, determinism | Label `smx2actl` reads IDENTICAL to `smx2a` (`report --control`), every measured value and spread equal in the 6 shared cells. |
| A4, the trip generation in the null legs | Every null leg of `smx2t` against its `smx2a` twin: `products.txt` identical, or every differing record follows a `PB_HOLD gen_start` with cause `trip` on that satellite at a statistical trip that the `smx2a` twin also has (the designed response, card SCR section 5.4). Any other difference fails A4. |

Predictions (not bars), written before any leg ran:

- P-A1. `sat_clk` passes every producer channel (`clk`, `pb`, `zwd`, `iono`, `emit`) on `g`, `c` and `n` in both labels, because the armed clock-event rule classifies the step and folds the clock LOOSE (card SCR section 5.2: `clk` 0.5, 0.2, 0.2 mm). In `smx2a` (no trip generation) the hold keeps `G25` in its first generation and the consumer takes the step as a phase innovation, so the cell fails a consumer channel on `c` and on `n`. In `smx2t` the row reads as card CRS2's `crs2bscr`: PASS on `g`, a consumer near miss on `c`, the `n` consumer about 15 cm off.
- P-A2. The storm fails every posture in both labels; in `smx2t` its `pb` channel and its `n` consumer are worse than in `smx2a` (card CRS2 section 5.6).
- P-A3. Outside `sat_clk`, no cell changes verdict between `crs2b` and `smx2a`, except that the slip rows (`slip100`, `slip2sig`, `sta_out`, `storm`) may move on `zwd` and `iono` through the re-seed key and the RAIM prior channel. `smx2a` passes at least 22 cells.
- P-A4. A4 holds; at least one null leg of `smx2t` differs from its twin (card SCR found one on `n`).

**Stage B**

| gate | bar |
| --- | --- |
| B0, seeds | Both 10-epoch smokes and both 10 h seeds exit 0, with history snapshots near emitted epochs 182, 360 and 1148. |
| S1, the consumer at the seam (card SLV's bar, unchanged) | Over leg epochs 6 to 60 after the resume, median of the held-out receiver's position difference leg minus seed at WTZZ00DEU: the arm at most the control's + 0.005 m. |
| S2, the wire at the seam (card SLV's bar, unchanged) | Over the same epochs, median absolute clock difference leg minus seed after the per-epoch, per-constellation median: the arm at most the control's + 0.002 m. |
| K1, the guard (card SLV section 7, sharpened) | For every satellite and step at which the control's guard responds to the stepped satellite within 2 epochs of the step (a `cta_decouple` naming it with any reason, or a deferred decoupling `cta_resolve_deferred` naming it), the arm's guard responds within 2 epochs too. A satellite and step where the control does not respond is unread and says so; the question is answered only if at least one satellite and step is read. |
| K2, the wire (card SLV's bar) | Per satellite and step s: with d_k the stepped satellite's emitted clock in the leg minus in its null, minus the per-epoch median of the same over the other satellites of its constellation, and e_k = \|\|d_k\| - s\|, the median of e_k over epochs 1 to 10 after the step at most the control's + 0.005 m. |

Reported without a bar: S1 and S2 of the 1.51 h and 9.56 h null legs; the control's own S1 next to card SLV's 0.459 m (the CRS2 restores are expected to bring it to centimetres); the guard row of each stepped satellite at the step epoch (`e_phi`, `sqrtS_phi`, the decision), the IF-TD flags and clock events naming it, and d_k by epoch.

Predictions: S1 and S2 pass (the +0.10 m was the broken consumer warm start meeting the floor's 100 m2 seam drops; with the hold and latches restored the leg consumer starts at its seed's convergence in both arms). K1 passes where read, and is read at least on E02 at 0.5 m (as in card SLV) and on the larger steps. K2 fails on E34, where the floor's predicted size is largest (card SLV: 52 mm slower).

**Stage C (built only if, after stages A and B, no other session has taken up the narrowing)**

The narrowing: one new default-off key on the hold's side, read only where `network.emit_pb_hold_trip_generation` is armed: a trip opens a generation only when the guard's reason is the classified satellite clock event (`slip_clk`, and its backstop `slip_clk_late`), so the generation then depends on `network.cycle_slip.if_phase_td_sat_clock_event`. No change to the guard's trip decision (the sibling card on branch `brd3/clock-share` edits that).

| gate | bar |
| --- | --- |
| C0, identity off | The new key absent: `products.txt`, `closed_loop_trace.csv`, `network_solution.csv`, `network_epoch_status.csv` and `network_events.csv` hash-equal against `gnss_processor_smx2.exe` on the 2 h sim golden (from 00:30:15) and the 2 h field golden. |
| C1, tests | New unit tests of the trigger pass; the Debug `gnss_tests.exe` reports 0 failed; `python scripts/nom_epoch_lint.py` and `python scripts/nom_n4_verify.py --whole` pass. |
| C2, the row | Row `sat_clk` on `g`, `c`, `n` with the trip generation and the new key: every channel within 0.1 mm of label `smx2t`'s cell, and every key of the stepped satellite opens a generation with cause `trip` at the step epoch. |
| C3, inert outside the event | Row `storm` and every 60-epoch null leg with the trip generation and the new key: `products.txt` byte-identical to the label `smx2a` twin (no generation on a statistical trip). |
| C4, the quiet-day cost | 2 h field golden with the trip generation and the new key against the 2 h field golden as armed: no `PB_HOLD gen_start` with cause `trip`, and `products.txt` identical (card RF0HV18B found no classified clock event on any bed). |
| C5, card SCW's E34 leg | On `evseed_smx2_f132`, the snapshot nearest emitted epoch 182, `at_ep` 1, a 0.5 m step on E34 with the clock-event rule, the trip generation and the new key, against its null with the same keys: card SCW's R1, R3 and R4 pass; R2 is reported with the Galileo frame event at epochs 24 to 27 traced first (which records move, at which epoch, in the leg and the null). |

## 5. Results

**Stage D added (2026-11-09, before any leg ran).** By the author's ruling of 2026-11-09 ("Close ALD, and test one thing before choosing"), one three-band event-window test of `network.amb_float_drop_var_m2` 100 runs after stage B on this card's exe. It is pre-registered, with its frozen bar (card ALD section 5, gate 4), in [L5B](L5B_band5_receiver_delay_2026-10-10.md) section 15, and its result is appended there.

**Departure (2026-11-09, recorded before any matrix reading).** The first run of label `smx2a` was aborted and deleted unread: a driver stopped earlier in the session had left its own child process alive, so after the seeds two drivers made and ran the label into the same leg directories at once (four of this card's processes, two writers per leg). Every `smx2a` leg, its events and its plan were deleted, the label was made again and run by one driver; every number below comes from that second run. No seed was affected (each seed was written by one process).

### 5.1 Seeds (gates A0 and B0)

**A0 passes.** The four 10-epoch smokes exit 0 (`evseed_smx2_{g,c,n,gx}_smoke`, 74 to 165 s). The four 4.5 h seeds exit 0 (1196 to 1462 s), each with 9 history snapshots; the snapshot at index 8 is `ep0485` (TOW 275580) on every posture, as in cards SMX and CRS2, and carries 114, 64, 62 and 108 `PH` rows on `g`, `c`, `n` and `gx` (card CRS2 read 114, 64, 62 and 108), its consumer snapshot four `I` rows. **B0 passes.** Both field smokes and both 10 h seeds exit 0 (4145 and 4163 s), 1201 emitted epochs each, with snapshots `ep0182`, `ep0363` and `ep1148`, the suffixes card SLV used.

### 5.2 Stage B: the floor at the seam and against a step the guard can see

Exe `gnss_processor_smx2.exe` (sha256[:16] `1335d935c990529c`); 20 legs, every one exits 0 (87 to 112 s). Graders: card SLV's `slv_seam.py`, unchanged, and `smx2_k.py` (card SLV's `slv_k.py` with the sharpened K1 response and the step size as the reference of e_k), in `D:/GnssProcessorSupport/worktrees/smx2_tools/`.

**S1 and S2 pass.** Null legs from `ep0363` against each seed's own continuation, leg epochs 6 to 60:

| | control `f132` | floor `f132A` | bar | verdict |
| --- | --- | --- | --- | --- |
| S1, median position difference at WTZZ00DEU (m) | 0.0001 (max 0.0013) | 0.0001 (max 0.0003) | control + 0.005 | pass |
| S2, median clock difference, per-constellation median removed (m) | 0.0092 (p90 0.044) | 0.0060 (p90 0.025) | control + 0.002 | pass |
| drops in the first 5 leg epochs (on arcs folded before); variance after the drop, p50 | 50 (36); 0.109 m2 | 51 (37); 100 m2 | | |

Card SLV read S1 at 0.459 m (control) and 0.560 m (floor) through a leg consumer that resumed 0.31 m off its seed. With card CRS2's hold and latch restores the leg consumer continues its seed's to a tenth of a millimetre in both arms, and the floor's +0.10 m is gone: it was the broken warm start meeting the floor's seam drops, not a cost of the floor. The floor still sets every seam drop to 100 m2 (51 of 51), as card SLV found. Reported without a bar: the 1.51 h nulls read S1 0.0005 m (control) and 0.0000 m (floor), S2 0.015 and 0.006 m; the 9.56 h nulls read S1 0.100 and 0.102 m, but every leg of the guard test carries the IF-TD centring, which the seeds do not, so those two nulls are not seam measurements (at 1.51 h the centring changes nothing the consumer sees, at 9.56 h it moves the consumer 0.10 m in both arms alike).

**The guard: K1 read on 4 satellite-steps, 3 pass and 1 FAILS.**

| satellite, snapshot, step | control: guard at the step epoch (`e_phi`, `sqrtS_phi`, decision) | floor: guard at the step epoch | K1 | K2 median e_k, control / floor (m) | K2 |
| --- | --- | --- | --- | --- | --- |
| G01, 9.56 h, 0.5 m | -0.086, 0.020, ok | -0.075, 0.020, ok | unread | 0.336 / 0.341 | **fail** |
| G01, 9.56 h, 1.0 m | -0.170, 0.020, trip (deferred) | -0.148, 0.020, trip (deferred) | pass | 0.050 / 0.069 | **fail** |
| E02, 9.56 h, 0.5 m | -0.207, 0.020, trip (deferred) | -0.202, 0.020, trip (deferred) | pass | 0.029 / 0.021 | pass |
| E02, 9.56 h, 1.0 m | -0.414, 0.020, trip (deferred) | -0.405, 0.020, trip (deferred) | pass | 0.060 / 0.040 | pass |
| E34, 1.51 h, 0.5 m | +0.579, 0.423, ok | -0.065, 0.125, ok | unread | 0.405 / 0.442 | **fail** |
| E34, 1.51 h, 1.5 m | +1.375, 0.423, ok | -0.145, 0.125, ok | unread | 1.243 / 1.329 | **fail** |
| E34, 1.51 h, 3.0 m | +2.568, 0.423, trip (deferred) | -0.264, 0.125, ok | **fail** | 0.344 / 2.737 | **fail** |

No null leg has a guard response on the stepped satellite; no leg has a satellite clock event (the centring leaves the classification nothing to classify); the IF-TD flags name G01 at 3 stations, E02 at none and E34 at 1, equal in both arms.

- **Where the guard has the statistic at its floor (9.56 h), the floor does not desensitise it.** G01 at 1.0 m and E02 at both sizes trip in both arms at the step epoch, the trip deferred to the next epoch (card ARS's key, armed since 2026-11-07), and the wire then carries 0.92 to 1.06 times the step. The floor's statistic reads 87 to 98 % of the control's.
- **Where the floor's state differs (E34 at 1.51 h), the floor lets through a step the control catches.** The control's statistic grows with the step (+0.58, +1.38, +2.57 m for 0.5, 1.5 and 3.0 m: 0.86 m per metre) and trips at 3.0 m against its bound of 2.54 m; the wire then carries 2.72 m of the 3.0 m by epoch 10. Under the floor the statistic moves the other way and ten times less (-0.065, -0.145, -0.264 m: about -0.09 m per metre) against a bound of 0.75 m, so the guard never trips and the wire has carried 0.47 m of the 3.0 m by epoch 10. Card SLV measured a five-fold loss at 0.5 m and could not read K1 because its control did not trip; this card's larger steps read it, and it fails.
- **What the miss is not.** E34's 14 arcs carry the same variances in both legs at the epoch before the step (p50 0.125 m2, largest 0.436 m2; none at the floor). At the step epoch both legs free the same five E34 arcs (one IF-TD flag at MRC100USA, four post-solve attribution drops at SAVO00BRA and BOGT00COL); the control keeps their variances (1.9e-4 to 1.0e-3 m2), the floor sets them to 100 m2. The step lands on the second epoch after the resume, when the floor's 51 seam drops of other arcs are also at 100 m2; whether a run that never resumed misses it the same way is not measured by these legs (addendum below).
- **K2.** The floor follows a step worse wherever the control follows it at all, except on E02, where it follows 8 to 20 mm better.

**Addendum, pre-registered 2026-11-09 before any run below (the seam caveat of K1).** Two fresh runs of the DOY 132 configuration of section 3, from its start to TOW 524190 (10 epochs past the step), each with `network.cycle_slip.if_phase_td_sat_centre` and a 3.0 m `sat_clock_step` on E34 at TOW 523890 through `network.inject`: `_rap_smx2_kf_c` (the golden as armed) and `_rap_smx2_kf_A` (plus the floor), and their two null runs without the step (`_rap_smx2_kf_cn`, `_rap_smx2_kf_An`), same exe. Bar K1-fresh: if the control's guard responds to E34 within 2 epochs of the step, the floor's does too. K2-fresh as K2, against the fresh nulls. If the control does not respond, K1-fresh is unread and the seam caveat stands.

### 5.3 Stage D: the freed arc's variance floor at the three-band event (card L5B section 15)

Windows `_rap_smx2_d_t1on_ev` and `_rap_smx2_d_t1onA_ev` (today's field golden with the `t1on` keys, 33 epochs, TOW 259200 to 260160), both exit 0 with 33 emitted (213 and 216 s). Grader `smx2_d.py`, which reproduces card ALD's control and H1 readings to the digit on ALD's own windows (`_rap_ald_t1on_ev`: -7.02, +17.66, +13.13 m, 21 decouples). **Gate (4) passes**: MAJU00MHL moves -0.10, +0.26 and +0.15 m (GPS_DCB, GPS_IFB5, GPS_ISC_C1W) against the control's -3.54, +9.12 and +6.22 m, with 0 decouples at TOW 260040 against 14. GPS code-bias steps above 0.5 m from 260010 to 260040:

| key | control (`t1on`) | `t1on` plus the floor |
| --- | --- | --- |
| GPS_DCB | 11 stations: LCK400IND +4.17, MAJU00MHL -3.54, CAS100ATA +1.63, DARW00AUS +1.62, SCTB00ATA +1.58, WARK00NZL +1.25, NYA200NOR -1.17, GCGO00USA -1.08, SVTL00RUS -1.07, MBAR00UGA -0.83, JPLM00USA -0.68 | none |
| GPS_IFB5 | 16 stations: LCK400IND -10.92, MAJU00MHL +9.12, CAS100ATA -4.52, DARW00AUS -4.53, SCTB00ATA -4.40, WARK00NZL -3.53, NYA200NOR +3.37, SVTL00RUS +3.02, GCGO00USA +2.97, MBAR00UGA +2.35, JPLM00USA +1.85, YKRO00CIV -1.16, BSHM00ISR -0.92, MAC100AUS +0.81, BOGT00COL +0.70, STHL00GBR +0.53 | LCK400IND -1.21 |
| GPS_ISC_C1C | 4 stations: SVTL00RUS +1.86, GCGO00USA +1.58, MBAR00UGA +1.45, YKRO00CIV -0.50 | none |
| GPS_ISC_C1W | 6 stations: MAJU00MHL +6.22, DARW00AUS -2.45, NYA200NOR +1.66, JPLM00USA +1.15, BSHM00ISR -0.83, MAC100AUS +0.60 | none |

The control of today's golden already carries about half of ALD's event (the keys armed on 2026-11-07 are the difference; not isolated here). The four 2 h arms of gate (5) (`_rap_smx2_d_{ctl,A,t1on,t1onA}_f2`) are queued after label `smx2t`.

**Attribution label (written 2026-11-09 before it runs; read without a bar).** Label `smx2x`: rows `slip100`, `slip2sig` and `anc_cold` on `c` and `n`, with `network.raim_prior_channel` false (not fingerprinted), to attribute the consumer moves of those cells between `crs2b` and `smx2a` (section 5.4).

### 5.4 Stage A: the matrix today

Labels `smx2a` (the postures as armed) and `smx2t` (plus `network.emit_pb_hold_trip_generation`), exe `gnss_processor_smx2.exe` (sha256[:16] `1335d935c990529c`), matrix version 1 (sha256[:16] `bfd6c00e0f0b7dec`, unchanged), seeds `evseed_smx2_*` from `ep0485`. Targets as in cards SMX and CRS2: `G25` on every posture (no GPS clock pivot at the snapshot); the `L2W` anchor of `G25` is `S24000SIM` on `g` and `c` and `S14000SIM` on `n` and `gx`; the storm draws the same 646 slips. Gate outputs `D:/GnssProcessorSupport/gates/smx/{smx2a,smx2t,smx2actl}/` (`plan.json`, `grades.json`, `report.md`; `smx2t/table_4labels.md`).

- **A1 passes.** `smx2a`: 65 legs, all exit 0 (56 to 661 s), every cell graded: **23 of 55 cells pass**.
- **A2 passes.** `smx2t`: 65 legs, all exit 0, every cell graded: **24 of 55 cells pass**.
- **A3 passes.** `smx2actl` (rows `slip100`, `sta_out`, `step1m` on `g` and `c`, own nulls) against `smx2a`: IDENTICAL, every measured value and spread equal in the 6 shared cells. The AVX2 build with the parallel stages is deterministic on these legs.
- **A4 passes.** Five of the seven null legs of `smx2t` write a `products.txt` identical to their `smx2a` twin (`null60_g`, `null60_c`, `null60_gx`, `null240_g`, `null240_c`). On `n` only G05's phase-bias records differ (36 in `null60_n`, 396 in `null240_n`), each after a `PB_HOLD gen_start` with cause `trip` at a `phi` trip (TOW 276900, and 282120 in the 240-epoch null) that the `smx2a` twin has as well: the designed response, and the trip card SCR found in its `n` null.

**The verdict table.** Verdict and failing channels per cell; `smx1` and `crs2b` ran on SSE2 builds of earlier code, so verdicts and magnitudes are compared, not bytes.

| row | posture | smx1 | crs2b | smx2a | smx2t | change |
|---|---|---|---|---|---|---|
| slip1 | g | PASS | PASS | PASS | PASS |  |
| slip1 | c | PASS | PASS | PASS | PASS |  |
| slip1 | n | PASS | PASS | PASS | PASS |  |
| slip100 | g | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| slip100 | c | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| slip100 | n | FAIL zwd,iono | FAIL zwd,iono | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono |  |
| slip2sig | g | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| slip2sig | c | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| slip2sig | n | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| storm | g | FAIL clk,pb,cons_peak,cons_plat,zwd,iono | FAIL clk,pb,zwd,iono | FAIL clk,pb,zwd,iono | FAIL clk,pb,zwd,iono |  |
| storm | c | FAIL clk,pb,cons_peak,cons_plat,zwd,iono | FAIL clk,pb,cons_peak,cons_plat,zwd,iono | FAIL clk,pb,cons_peak,cons_plat,zwd,iono | FAIL clk,pb,cons_peak,cons_plat,zwd,iono |  |
| storm | n | FAIL clk,pb,cons_peak,cons_plat,zwd,iono | FAIL clk,pb,cons_peak,cons_plat,zwd,iono | FAIL clk,pb,cons_peak,cons_plat,zwd,iono | FAIL clk,cons_peak,cons_plat,zwd,iono |  |
| lol | g | PASS | PASS | PASS | PASS |  |
| lol | c | PASS | PASS | PASS | PASS |  |
| lol | n | PASS (no effect) | PASS (no effect) | PASS (no effect) | PASS (no effect) |  |
| sat_out | g | PASS | PASS | PASS | PASS |  |
| sat_out | c | PASS | PASS | PASS | PASS |  |
| sat_out | n | PASS | PASS | PASS | PASS |  |
| sta_out | g | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| sta_out | c | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| sta_out | n | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| sat_clk | g | FAIL clk,cons_peak,cons_plat,zwd,iono | FAIL clk,zwd,iono | PASS | PASS | crs2b->smx2a PASS |
| sat_clk | c | FAIL clk,cons_peak,cons_plat,zwd,iono | FAIL clk,cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat | FAIL cons_peak,cons_plat |  |
| sat_clk | n | FAIL clk,cons_peak,cons_plat,zwd,iono | FAIL clk,cons_peak,cons_plat,zwd,iono | FAIL pb,cons_peak,cons_plat | PASS | smx2a->smx2t PASS |
| rx_clk1m | g | PASS | PASS | PASS | PASS |  |
| rx_clk1m | c | PASS | PASS | PASS | PASS |  |
| rx_clk1m | n | PASS | PASS | PASS | PASS |  |
| rx_clk1ms | g | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| rx_clk1ms | c | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| rx_clk1ms | n | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| anc_cont | g | FAIL iono | FAIL iono | FAIL iono | FAIL iono |  |
| anc_cont | c | FAIL iono | FAIL iono | FAIL iono | FAIL iono |  |
| anc_cont | n | FAIL zwd | FAIL zwd | FAIL zwd | FAIL zwd |  |
| anc_cold | g | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| anc_cold | c | FAIL zwd,iono | FAIL zwd,iono | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono |  |
| anc_cold | n | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono |  |
| arc_erase | g | PASS | PASS | PASS | PASS |  |
| arc_erase | c | PASS | PASS | PASS | PASS |  |
| arc_erase | n | PASS | PASS | PASS | PASS |  |
| health | g | PASS (no effect) | PASS (no effect) | PASS (no effect) | PASS (no effect) |  |
| health | c | PASS (no effect) | PASS (no effect) | PASS (no effect) | PASS (no effect) |  |
| health | n | PASS (no effect) | PASS (no effect) | PASS (no effect) | PASS (no effect) |  |
| health | gx | FAIL cons_peak,cons_plat,zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| step1cm | g | FAIL zwd | FAIL zwd | FAIL zwd | FAIL zwd |  |
| step1cm | c | PASS | PASS | PASS | PASS |  |
| step1cm | n | PASS | PASS | PASS | PASS |  |
| step10cm | g | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| step10cm | c | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| step10cm | n | FAIL zwd | FAIL zwd | FAIL zwd | FAIL zwd |  |
| step1m | g | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| step1m | c | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono | FAIL cons_peak,cons_plat,zwd,iono |  |
| step1m | n | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono | FAIL zwd,iono |  |
| clkdrift | g | FAIL cons_p50 | FAIL cons_p50 | FAIL cons_p50 | FAIL cons_p50 |  |
| clkdrift | c | PASS | PASS | PASS | PASS |  |
| clkdrift | n | PASS | PASS | PASS | PASS |  |

**Every verdict change, explained.**

1. **`sat_clk` on `g`, FAIL (`crs2b`) to PASS (`smx2a`).** The clock-event rule, armed since 2026-11-07, classifies `G25` at the step epoch (one `sat_clock_event`, 15 of 15 non-reference flags kept, the reference reset kept at one station), the fold takes the anchored clock LOOSE and the guard declares `slip_clk`; no `G25` arc is refloated, so the station states are not disturbed. Wire clock 0.6 mm (`crs2b` 28.4), ZWD 13.2 mm (57.4; bar 16.1), slant ionosphere 19.1 mm (658.5), consumer peak 12.9 mm and plateau 0.3 mm (bar 18.3). The producer channels reproduce card SCR's fold-side route (`sckb1`: 0.5, 12.1 and 17.8 mm) within 1.3 mm. The row passes on the golden as armed, without the trip generation: on the phase-clock posture the consumer meets the bar although `G25`'s hold keeps its first generation.
2. **`sat_clk` on `n`, FAIL (`smx2a`) to PASS (`smx2t`).** In `smx2a` every producer channel but `pb` passes (wire clock 0.2 mm, ZWD 1.0, ionosphere 1.5; `pb` 88.9 against 87.7 mm, the smoothed hold's bridge withdrawn twice more than in the null), and the consumer sits 1.05 to 1.25 m off for the whole leg (peak 1247.8 mm, plateau 1221.4): the step is on the clock, the hold moves `G25`'s phase biases with it, no generation is declared, and the consumer takes 0.5 m as a phase innovation. With the trip generation, generation 2 opens on `G25`'s two keys at the `slip_clk` trip, the consumer resets both (`HOLD generation 2 -> 3: reset G25 L1C`, `L2W`), and its difference from the null peaks at 19.7 mm at +2 and +9 (plateau 14.5; bar 28.2). Card CRS2's `crs2bscr` read 157.8 and 152.6 mm on this cell; the improvement is the 2026-11-07 armed set, the review builds or the build, not isolated here.

**Moves inside cells whose verdict did not change** (measured / bar in mm; each cell already failed on another channel):

- `sat_clk` on `c`: `smx2a` passes every producer channel (wire clock 0.3, ZWD 1.6, ionosphere 0.9) and misses the consumer by a metre (1283.4 / 1257.5 against 26.7), as on `n`. `smx2t` brings it to 69.6 / 65.6: the consumer resets `G25` at the step and stays within 10 to 22 mm for three epochs, then drifts about 1.3 mm per epoch to 69 mm at +45. Card CRS2's `crs2bscr` peaked at 30.6 mm at +2 and decayed (plateau 7.4). Not attributed here; a one-change label (`smx2xt`, the prior channel off) is queued (section 5.5).
- `slip100` on `n`: the consumer channels now fail (72.8 / 68.6 against 28.2; `crs2b` 14.8 / 8.9). `G25`'s clock moves about 3 cm in both labels; in `smx2a` the RAIM prior channel (`network.raim_prior_channel`, armed 2026-11-07) releases `G10` and `G25` at S48000SIM at the event epoch and `G25` again at +1, where `crs2b` released `G10` and `G25` once each through the post-solve attribution, and the consumer drifts linearly to 73 mm. `slip100` on `c` (23.7 / 22.0 against 26.7; `crs2b` 3.4), `slip2sig` on `n` (23.2; `crs2b` 3.6), `anc_cold` on `c` (35.1 / 32.1, now over 26.7; `crs2b` 24.3) and on `n` (78.6; `crs2b` 61.6) move the same way. Label `smx2x` (section 5.5) arms the prior channel off on those cells.
- The event station's ZWD transient on `g` is larger: `slip100` 274.1 (`crs2b` 190.1), `slip2sig` 67.0 (46.4), `sta_out` 156.8 (137.6); the ZWD bar on `g` rose with the null's spread (14.7 to 16.1).
- `storm`: section 5.4.1.

#### 5.4.1 The two rows that failed on every posture

**`sat_clk`** (+0.5 m on every code and phase of `G25` at every station, TOW 275910). Measured / bar in mm, X over the bar:

| posture | label | clk | pb | cons_peak | cons_plat | zwd | iono | emit | cell |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| g | crs2b | 28.4 / 5.0 X | 94.6 / 135.8 | 12.8 / 18.2 | 0.3 / 18.2 | 57.4 / 14.7 X | 658.5 / 234.5 X | 0 | FAIL |
| g | crs2bscr | 0.5 / 5.0 | 0.3 / 135.8 | 4.6 / 18.2 | 1.0 / 18.2 | 12.1 / 14.7 | 17.8 / 234.5 | 0 | PASS |
| g | smx2a | 0.6 / 5.0 | 67.5 / 135.7 | 12.9 / 18.3 | 0.3 / 18.3 | 13.2 / 16.1 | 19.1 / 234.3 | 0 | **PASS** |
| g | smx2t | 0.6 / 5.0 | 0.3 / 135.7 | 4.7 / 18.3 | 1.0 / 18.3 | 13.2 / 16.1 | 19.1 / 234.3 | 0 | **PASS** |
| c | crs2b | 35.1 / 5.0 X | 122.5 / 142.9 | 57.3 / 26.4 X | 32.0 / 26.4 X | 121.1 / 18.6 X | 642.4 / 230.2 X | 0 | FAIL |
| c | crs2bscr | 0.2 / 5.0 | 0.3 / 142.9 | 30.6 / 26.4 X | 7.4 / 26.4 | 1.1 / 18.6 | 1.0 / 230.2 | 0 | FAIL |
| c | smx2a | 0.3 / 5.5 | 89.8 / 143.7 | 1283.4 / 26.7 X | 1257.5 / 26.7 X | 1.6 / 20.6 | 0.9 / 230.1 | 0 | FAIL |
| c | smx2t | 0.3 / 5.5 | 0.2 / 143.7 | 69.6 / 26.7 X | 65.6 / 26.7 X | 1.6 / 20.6 | 0.9 / 230.1 | 0 | FAIL |
| n | crs2b | 83.0 / 37.1 X | 16.3 / 84.9 | 1139.0 / 28.3 X | 1110.8 / 28.3 X | 138.8 / 19.0 X | 814.9 / 222.4 X | 0 | FAIL |
| n | crs2bscr | 0.2 / 37.1 | 0.1 / 85.2 | 157.8 / 28.3 X | 152.6 / 28.3 X | 0.8 / 19.0 | 0.9 / 222.4 | 0 | FAIL |
| n | smx2a | 0.2 / 37.2 | 88.9 / 87.7 X | 1247.8 / 28.2 X | 1221.4 / 28.2 X | 1.0 / 21.0 | 1.5 / 222.2 | 0 | FAIL |
| n | smx2t | 0.2 / 37.2 | 0.1 / 88.0 | 19.7 / 28.2 | 14.5 / 28.2 | 1.0 / 21.0 | 1.5 / 222.2 | 0 | **PASS** |

The response, event leg minus null, is the same on the three postures and in both labels: one `sat_clock_event`, 15 `sat_clock_keep`, one `refsat_clock_keep`, one `cta_decouple` with reason `slip_clk` at the step, one re-coupling; no refloat of `G25`, no handover. The wire carries the step on the clock to 0.2 to 0.6 mm (the de-commoned clock difference reads -0.496 to -0.504 m from the step epoch on). What separates the postures is the consumer: on `g` it meets the bar with or without a generation; on `c` and `n` it needs the generation to be told (a metre without it), and with it `n` passes and `c` drifts to 69 mm. The trip generation opens two generations (`G25` L1C and L2W) in every `sat_clk` leg and none at a statistical trip there.

**`storm`** (646 GPS slips at all 60 stations, 240 epochs). Measured / bar in mm:

| posture | label | clk | pb | cons_peak | cons_plat | zwd | iono | keys regenerated |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| g | crs2b | 782.8 / 11.2 X | 996.1 / 96.3 X | 14.8 / 20.0 | 5.2 / 20.0 | 4276.2 / 9.3 X | 24083.6 / 221.5 X | |
| g | smx2a | 179.8 / 11.2 X | 1224.2 / 96.1 X | 13.6 / 20.0 | 2.9 / 20.0 | 3277.7 / 9.8 X | 20330.4 / 221.6 X | 0 |
| g | smx2t | 179.8 / 11.2 X | 1293.4 / 96.1 X | 12.7 / 20.0 | 1.9 / 20.0 | 3277.7 / 9.8 X | 20330.4 / 221.6 X | 12 |
| c | crs2b | 299.2 / 9.0 X | 1553.5 / 85.5 X | 500.8 / 32.9 X | 380.6 / 32.9 X | 6052.7 / 12.4 X | 21506.7 / 262.5 X | |
| c | smx2a | 425.5 / 9.4 X | 1734.5 / 86.9 X | 546.0 / 33.0 X | 326.6 / 33.0 X | 5584.3 / 13.1 X | 20466.1 / 262.5 X | 0 |
| c | smx2t | 425.5 / 9.4 X | 3335.4 / 86.9 X | 376.5 / 33.0 X | 299.9 / 33.0 X | 5584.3 / 13.1 X | 20466.1 / 262.5 X | 48 |
| n | crs2b | 490.7 / 36.4 X | 962.4 / 111.0 X | 369.6 / 36.8 X | 123.3 / 36.8 X | 15713.0 / 13.3 X | 47449.2 / 261.1 X | |
| n | smx2a | 680.1 / 36.5 X | 965.8 / 112.7 X | 404.8 / 36.8 X | 294.6 / 36.8 X | 22390.2 / 13.9 X | 37272.9 / 261.1 X | 0 |
| n | smx2t | 680.1 / 36.5 X | 103.6 / 112.6 | 1829.1 / 36.8 X | 372.3 / 36.8 X | 22390.2 / 13.9 X | 37272.9 / 261.1 X | 58 |

Responses in `smx2a`, event minus null over the leg: the IF-TD pool flags 540 to 544 samples and resets 83 to 84 reference chains, and classifies no satellite clock event; the RAIM prior channel releases 714 to 777 arcs and the post-solve attribution 139 to 178; the gross-LOS screen declares 49 to 58 rows and marks 617 to 853 emission discontinuities; the guard trips 18 clocks on `phi` on `g` (all 18 deferred by card ARS's key, the reason the `g` wire clock is 180 mm here against 783 in `crs2b`), 74 on `c` and 62 on `n` (none deferred). The trip generation opens 36 (`g`), 148 (`c`) and 124 (`n`) key generations at those statistical trips, and the consumer resets 2, 24 and 24 keys. It costs the `n` consumer 4.5 times (404.8 to 1829.1 mm peak) and the `c` phase-bias channel twice, and helps the `c` consumer (546.0 to 376.5 mm); on `n` the `pb` channel passes only because 58 keys left the same-generation comparison. The storm fails on the producer channels on every posture in every label: wire clock 180 to 680 mm, phase bias, station ZWD and slant ionosphere by metres. Its `g` consumer has passed since card CRS2.

#### 5.4.2 Predictions against results

- **P-A1.** Producer channels passing on every posture in both labels: held, except `pb` on `n` in `smx2a` (88.9 against 87.7 mm). The `c` and `n` consumers failing in `smx2a`: held (a metre each). `smx2t` reading as `crs2bscr`: held on `g`; refuted on `c` (69.6 mm against `crs2bscr`'s 30.6, still a fail) and on `n` (19.7 mm, a pass, against `crs2bscr`'s 157.8).
- **P-A2.** The storm failing everywhere: held. With the trip generation the `n` consumer worse: held (4.5 times); the `pb` channel worse: held on `g` and `c`, refuted on `n` (its regenerated keys leave the comparison).
- **P-A3.** No verdict change outside `sat_clk` between `crs2b` and `smx2a`: held; at least 22 cells pass: held (23). The slip rows moved on `zwd` as predicted, and also on the consumer channels on `c` and `n`, which was not predicted.
- **P-A4.** Held (G05 on `n`).

### 5.6 Stage D: gate (5), the 2 h arms (2026-11-09)

`_rap_smx2_d_{ctl,A,t1on,t1onA}_f2`, today's field golden from its start, 240 of 240 epochs each, exit 0 (197 to 248 s). Grader `smx2_d2.py` (card ALD's tools: `scripts/ftc_step_attrib.py --r3 --systems G,E`, row `e_clk`, hour 2; `scripts/clock_level/band5_offsets.py`, hour 2), which reproduces ALD's dual control on its own run (`_rap_ald_ctl_f2`, clock wire 0.0233 m).

| arm | WTZZ00DEU, hours 1 and 2 (m) | GPS clock wire, all / tracked (m) | GPS band 1 / 2 / 5 offsets, stations (median \|mean\|, m) | decouples (at 260040) | MAJU00MHL, 260010 to 260040: GPS_DCB / GPS_IFB5 (m) |
| --- | --- | --- | --- | --- | --- |
| dual control | 0.058 0.013 | 0.0372 / 0.0089 | 1 (0.014) / 0 (0.013) / - | 3 (2) | -0.17 / - |
| dual plus the floor | 0.057 0.012 | 0.0382 / 0.0087 | 1 (0.015) / 0 (0.014) / - | 0 (0) | -0.10 / - |
| `t1on` | 0.075 0.039 | 0.0435 / 0.0088 | 2 (0.023) / 2 (0.017) / 4 (0.028) | 17 (14) | -3.54 / +9.12 |
| `t1on` plus the floor | 0.079 0.038 | 0.0166 / 0.0094 | 1 (0.017) / 0 (0.016) / 5 (0.025) | 0 (0) | -0.10 / +0.26 |

The dual bar passes (WTZZ00DEU within 0.001 m, clock wire 1.03 and 0.98 times). The three-band bars of card R1D (B), each three-band arm against the dual control: `t1on` fails (clock wire 1.17 times, band 2 offsets +2 stations; WTZZ00DEU +0.017 and +0.026 m within 0.03); `t1on` plus the floor passes (WTZZ00DEU +0.021 and +0.025 m, clock wire 0.45 and 1.06 times, band 1 and 2 offsets +0 and +0). **Stage D: the floor passes gates (4) and (5) as frozen in card L5B section 15.** The hypothesis that the three-band event is the freed arc's stale level holds on this day's event and its 2 h arms. Not read here: hours 3 to 6, a census, a second day, the sim, and the floor's other measured costs (card SLV: H4 and DOY 132's H5 without the re-seed key, R2 at the 00:00 sim start, the walk metric; this card section 5.2: the guard's miss on E34 at 1.51 h).

### 5.7 The K1 addendum: the same step in runs that never resumed (2026-11-09)

The four fresh runs of the addendum of section 5.2 (`_rap_smx2_kf_{c,A,cn,An}`, the DOY 132 configuration from its start with the IF-TD centring, 194 epochs to TOW 524190, exit 0, 152 to 157 s each); the step runs write one `INJECT` row each.

| run | guard on E34 at the step epoch (`e_phi`, `sqrtS_phi`, decision) | response within 2 epochs | d_k, epochs 0 to 10 (m) | K2 median e_k (m) |
| --- | --- | --- | --- | --- |
| control `kf_c` | +8.817, 1.115, trip (bound 6.69) | `cta_decouple` `phi`, deferred | -0.135, -0.226, ... -0.308 | 2.717 |
| floor `kf_A` | +2.510, 0.870, ok (bound 5.22) | none | -0.069, -0.088, ... -0.113 | 2.899 |
| nulls `kf_cn`, `kf_An` | -0.003 and -0.004, ok | none | | |

**K1-fresh FAILS and K2-fresh FAILS.** In a run that never resumed, the control's guard trips on the 3.0 m step and the floor's does not: its statistic reads 28 % of the control's at 78 % of the control's predicted size. The seam caveat of section 5.2 is removed: the floor desensitises the guard to a step on E34 at 1.51 h whether or not the run resumed there. K2-fresh: neither wire follows the step in these runs (the control carries 0.31 m of the 3.0 m by epoch 10 although its guard tripped and deferred the correction, the floor 0.11 m); the floor is 0.18 m worse. Why the control's wire follows 2.72 m on the resumed leg and 0.31 m here, after the same deferred trip, is not read: the two runs differ by 1.5 h of centring before the step.

**Answer to card SLV's guard question.** Where the guard's predicted size sits at its floor (9.56 h, G01 and E02), the floor trips wherever the control trips. On E34 at 1.51 h, where the two states differ, the control's guard catches a 3.0 m step that the floor's guard lets through, on a resumed leg and in a continuous run alike. The floor can make the anchor guard blind to a satellite clock step of metres on a satellite whose phase statistic it has damped. On the golden, which does not arm the centring, a step that the IF-TD pool flags at most of the satellite's stations is routed to the clock by the armed clock-event rule before the guard's statistic is needed (section 5.4.1; card SCW section 5.1 for E34 on the field); the exposure is a step the rule does not classify (too few stations in a signal pair, or a step below the IF-TD thresholds).

### 5.5 Attribution labels (2026-11-10, read without a bar)

- **`smx2x`** (rows `slip100`, `slip2sig`, `anc_cold` on `c` and `n`, `network.raim_prior_channel` false; 8 legs, all exit 0): every channel of all six cells equals `smx2a`'s, and on `slip100` on `n` the event leg's `products.txt` and consumer trace are byte-identical to `smx2a`'s. With the prior channel off the post-solve attribution releases `G10` and `G25` at the event epoch, the release the prior channel made there, and the prior channel's second release of `G25` at +1 does not occur; neither changes a record. The prior channel is exonerated: the consumer moves of these cells between `crs2b` and `smx2a` (section 5.4) come from another part of what changed between the two labels (the other keys armed on 2026-11-07 and in card RF0HV18B section 6, the code between main `284ec29` and `27d6ece`, the AVX2 build), not isolated.
- **`smx2xt`** (row `sat_clk` on `c` with the trip generation and the prior channel off; 2 legs): the event leg and its null are byte-identical to `smx2t`'s (`products.txt` and consumer trace). The `c` consumer's drift after the generation (69.6 mm) is not the prior channel's either.

## 6. Standing (2026-11-10)

**The scoreboard today.** 23 of 55 cells pass on the postures as armed (label `smx2a`), against 22 for `smx1` (2026-10-24) and `crs2b` (2026-11-06); 24 of 55 with `network.emit_pb_hold_trip_generation` (label `smx2t`). The one verdict gained as armed is the satellite clock step on the golden posture (the clock-event rule and the guard's deferral, armed 2026-11-07); the trip generation adds the same row on the native posture. Every producer channel of the clock-step row now passes on every posture (the wire carries the step to 0.6 mm or better); what remains is the consumer on the self-contained regime (69.6 mm with the generation, a metre without). The slip storm fails every channel class on every posture: wire clock 180 to 680 mm, station states by metres; the deferral cut the golden's wire clock from 783 to 180 mm. The other 27 failing cells are as in card SMX: transients of the faulted station's ZWD and slant ionosphere (single slips of 100 cycles, the two-signal slip, the station outage, the receiver clock steering jump, the anchor handovers, the coordinate steps, the health exclusion), the clock drift on the golden, the 1 m step and the cold handover on the consumer of the self-contained regimes.

**Recommendations for the author** (the decisions are the author's):

1. `network.emit_pb_hold_trip_generation` as built: not for arming. On the matrix it buys one cell (`sat_clk` on `n`) and costs the storm consumer on the native posture 4.5 times; card SCW section 6 measured it harmful on quiet days (DOY 132 census, sim hour 3). The narrowed trigger of card SCW section 7 (`network.emit_pb_hold_trip_generation_trigger` `clock_event`) keeps every generation of this card's `sat_clk` legs (each opened at a `slip_clk` trip) and none of the storm's or the nulls' (each at a `phi` trip); grading it on this card's `sat_clk` and `storm` rows on `g`, `c` and `n` against `smx2t` and `smx2a` is the matrix check that card does not include.
2. The 100 m2 floor (`network.amb_float_drop_var_m2`): the seam objection of card SLV is withdrawn by S1 here, and the floor removes card L5B's three-band event (stage D). Against it stand this card's K1 (a guard blind to a 3 m step on E34 at 1.51 h, resumed or not) and card SLV's remaining failures (H4 and DOY 132's H5 without the re-seed key, R2 at the 00:00 sim start, the walk metric). Arming it is a choice between those; step R-1d of the L5 plan waits on that choice.
3. The trip generation, the floor, and any response to the storm: no further build is proposed by this card.

**Owed.** (a) The consumer moves on the self-contained regimes between `crs2b` and `smx2a` (slip rows, the cold handover, the clock step's drift on `c`) are not attributed; the prior channel is excluded, the other changed keys are not. (b) Stage D read one day's event and its 2 h arms only (no hours 3 to 6, census, second day or sim). (c) Why the control's wire follows the 3 m E34 step 2.72 m on the resumed leg and 0.31 m in the continuous run, after the same deferred trip. (d) Card SMX's version-2 items (the anchor resolved at the event epoch, the `pb` channel's de-commoning, a truth reference for the slant ionosphere) are unchanged. (e) Card SCW section 7's narrowed trigger on this matrix (recommendation 1).
