# SMX2: the fault-scenario matrix on the goldens as armed on 2026-11-09, the floor at the seam, and the trip generation's trigger

**STATUS**

- **id:** SMX2
- **topic:** harness
- **status:** open
- **summary:** PRE-REGISTERED 2026-11-09, no leg or seed run yet. Three stages: A, card SMX's version-1 matrix re-run on fresh seeds of the postures as armed on main `27d6ece` (the keys armed on 2026-11-07 included), once as armed and once with `network.emit_pb_hold_trip_generation`, read cell by cell against labels `smx1` and `crs2b`; B, card SLV's seam clause S1 for the 100 m2 floor re-graded on main's code with card CRS2's resume keys, and SLV's guard-sensitivity question answered at snapshots and step sizes where the control's guard trips; C, a default-off narrowing of the trip generation to the classified satellite clock event, built only if no other session has taken it up; D (added 2026-11-09 by the author's ruling), the freed arc's variance floor on the three-band event window of card L5B, pre-registered in card L5B section 15.
- **source:** card [SMX](SMX_scenario_matrix_2026-10-24.md) (the matrix, label `smx1`, 22 of 55); card [CRS2](CRS2_resume_hold_and_latches_2026-11-06.md) sections 5.5, 5.6 and 6 (label `crs2b`, the resume keys, the consumer channels read on a converged leg consumer); card [SCR](SCR_sat_clock_step_response_2026-10-26.md) sections 6 and 7 (the two keys of the satellite clock step and the trigger of the trip generation, item 1); card [ARS](ARS_anchor_resolve_coupling_2026-10-26.md) (the guard's deferral); card [RF0HV18B](RF0HV18B_reduced_candidate_2026-11-07.md) section 5.1 (the keys armed on 2026-11-07); card SLV (`docs/refound/SLV_freed_arc_stale_level_2026-11-07.md` on the unmerged branch `slv/stale-level-response`, sections 5, 6.6, 6.8 and 7); card SCW (`docs/refound/SCW_sat_clock_step_wire_2026-11-09.md` on the unmerged branch `scw/sat-clock-wire`, section 5.1, the E34 leg at 1.51 h)
- **knobs / posture:** stage A: postures `g`, `c`, `n`, `gx` of card SMX built by `scripts/scenario_matrix.py` from today's sim golden; label `smx2t` adds `network.emit_pb_hold_trip_generation` through `make --extra`. Stage B: the DOY 132 field golden derived from today's field golden (`scripts/mday_golden_config.py`, obs root `D:/GnssProcessorSupport/data_mday`), control as armed, arm plus `network.amb_float_drop_var_m2` 100. Stage C (if built): one new default-off key on the hold's side. Nothing armed, no golden or manifest edited.
- **dates in the text:** 2026-11-09
- **hashes / run dirs:** branch `smx2/fault-response` from main `27d6ece`, worktree `D:/GnssProcessorSupport/worktrees/smx2`; exe `gnss_processor_smx2.exe`, a renamed copy of that worktree's Release build (AVX2, the default; the only change on the branch at that build is the seed-family option of `scripts/scenario_matrix.py`, no C++); seeds `evseed_smx2_{g,c,n,gx}` (sim) and `evseed_smx2_f132`, `evseed_smx2_f132A` (field) under `D:/GnssProcessorSupport/runs_nom`; legs `evleg_smx_smx2*_*` (matrix) and `evleg_smx2_*` (stage B); gate outputs `D:/GnssProcessorSupport/gates/smx/smx2*/`; helper scripts `D:/GnssProcessorSupport/worktrees/smx2_tools/` (outside the repository; copies of cards SLV's and SCW's graders)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registered 2026-11-09; no result yet. Question: how many cells of the fault-scenario matrix does the network producer pass on the goldens as armed today, with and without the trip generation; does the 100 m2 floor still cost the seam consumer once the consumer resumes at its seed's convergence; and does the floor let through a clock step the control's guard catches? The verdict table, the two all-fail rows read channel by channel, and the stage C decision follow in section 5.

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
