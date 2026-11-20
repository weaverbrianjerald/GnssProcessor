# Event legs: resumed runs from one seed

A leg is a short run resumed from a snapshot that a longer run (the seed) wrote. Legs serve two purposes: a behaviour check read as a leg with an injected event minus the same leg without it (the null), and an identical-output check shown by two legs from one seed on two engines. The tool is `scripts/event_leg.py`; the runs go under `RAPID_RUNS_ROOT` (by convention `D:/GnssProcessorSupport/runs_nom`).

## The commands

1. `write-seed --name <n> --base <config> --hours <h> --interval-ep <k> --exe <exe> --run` derives a seed from a configuration and runs it: every capture group of the snapshot is armed, and a history copy of the snapshot is kept every `k` emitted epochs (`evseed_<n>/state_snapshot.txt.epNNNN`). The sim golden's seeds add `--extra '{"network":{"start_offset_hours":0.504167}}'` (the 00:30:15 start). A seed that runs the chain (`network.phase_datum_eq34`) also arms `network.state_snapshot_chain_capture`.
2. `null --seed-run evseed_<n> --snapshot-ep <ep> --epochs <m> --name <leg> --exe <exe> --run` resumes from one history copy for `m` epochs with every restore group armed, and with no event. The leg starts at the epoch after the snapshot (the start offset is read from the snapshot and the fixture's first observation epoch). On a chain seed it arms `network.resume_chain_restore`; `--no-chain-restore` leaves the chain memory out, which gives the matched null of the chain memory's grade. The section carries its own fingerprint of the keys that shape the memory and is refused, with a warning and a counter, when the leg's configuration differs; the warm resume then goes on without it. A leg that resumes without the memory (the null, an absent or a refused section) keeps the `root` event row on an empty memory: a root that stays a root finds no saved target, and a handed-over root still pins from its carry. That one step is stated whenever the null is graded.
3. `make-leg` is the same with one injected event (`--event`, at `--at-ep` epochs into the leg).
4. `rerun <legdir>` restores the leg's own snapshot copy from its pristine record and runs it again.
5. `chain-grade --seed <seed dir> --leg <leg dir> [--null <matched null>] [--leg2 <leg again>] [--null2 <null again>] [--json <file>]` grades a leg against its seed over the leg's epochs: at the seam, the holds the seed carries on, the settled flags, the discontinuity counters and the tree edges with their pin targets; over the leg, the integers of every link both runs hold; the topology ledger's events at the seam and over the leg; the distances of the phase biases, clocks and wet delays from the seed's at chosen epochs; the holds and releases at a different epoch than the seed's. With `--leg2` and `--null2` it also checks that two runs from one seed are identical in full (`nom_td_gate.py --whole --mask-time`).

## What a leg can be compared with

Today's resume does not reproduce the uninterrupted run record for record, even without integer fixing: the snapshot leaves out the per-station filter state (the cycle-slip arcs and detector history, the per-station ambiguity, ionosphere and receiver-bias marks and covariance), so a resumed solve departs from the seed at its first epoch (card NRF section 125, B1: 0.3 mm median phase-bias difference at the first epoch on both references, 7.5 mm on the sim and 35 mm on the field golden over the hour). A leg is therefore compared with its seed on the discrete state the snapshot carries, and an event is read against its null, never against the seed. The first few epochs after the seam are a transient.

The broadcast group-delay latch is taken over the run's configured window, so a leg latches its own unless the chain memory restore puts the seed's back; a seed and a short run from the same start therefore differ from their first epoch (card NRF section 124). Compare legs with legs from the same seed.

## The warm resume: order of operations

`networkTryWarmResume` (`src/core/network/state/WarmResume.cpp`) is an ordered list of restore groups, one file-local function each, called in the order below. The banner of the file holds the same list. A group opens with its arming gate (a group armed by no knob always runs), reads the snapshot and writes the run context. The record tags (V, C, J and so on) are restored through `net::recordFromSnapshot`, whose call order is `kRestoreOrder` (`src/core/network/state/Entities.cpp`), compared with the reader by `python scripts/nom_n4_verify.py`. Every key below is under `network.`.

| # | Group | Armed by | Reads from the snapshot | Writes in the run |
|---|-------|----------|-------------------------|-------------------|
| 1 | `resumeCheckPreconditions` | `state_snapshot_require`, `carry_vce_apply` | nothing (the snapshot path and its file) | nothing; throws when the file is missing under `require`, or when the VCE apply is armed and a snapshot exists |
| 2 | `resumeReadSnapshot` | always | the file | the parsed snapshot; a parse failure ends the resume |
| 3 | `resumeCheckGate` | always | configuration fingerprint, age, format | first live epoch and horizon (`ResumeFlow`); `ctu_switch_on_resume` when the stripped twin of the fingerprint admits the file |
| 4 | `resumeEmitterLayer` | always | emitter section | the emitter's own state, all or nothing; a failure ends the resume |
| 5 | `resumeSharedCarry` | always | D rows; V C J K L Z | `prev_carry`; constellation pivots, satellite records, arc leveling carries, station wet-delay carries |
| 6 | `resumeZwdSeam` | `zwd_seam_reseed` | the Z rows of group 5 (counted) | `zwd_seam_ep_left` |
| 7 | `resumeFrame` | `resume_frame_restore` | PA PT; clock-anchor, held-frame and handover rows | phase-anchor incumbents and pin targets, `clk_anchor`, the clock re-base frame, the handover ledger (primed from the restored leveling rows when the continuous handover is armed: `phase_datum_handover_continuous` on an anchored datum without eq34) |
| 8 | `resumeDatum` | `resume_datum_restore` | EQD EQP EQS EQT EQE EQR; CBP CBS CBI | eq34 tree tables and code-bias previous values, each half only when its rows exist; `ResumeFlow::datum_cb_restored` |
| 9 | `resumeEmitLayer` | `resume_emit_restore` | the emit re-base frame | the phase-bias emit re-base engine |
| 10 | `resumeHold` | `resume_hold_restore`, with `emit_pb_hold` | PH PHM PHC PHF | hold fields of the entity tables; clone births queued again when `emit_pb_hold_smoothed` is armed |
| 11 | `resumeChainMemoryHandoff` | `resume_chain_restore` | the chain memory section | `carry.resume.chain_memory` (staged only) |
| 12 | `resumeClockLevel` | `clock_level_monitor` (and `clock_level_restore_raise`) | LB | the level information, `carry.resume.clm_raise` |
| 13 | `resumeStationLayer` | `resume_station_restore`, sub-switch `resume_station_restore_rxcb` | RC SD | receiver code-bias carries, per-line-of-sight deviation entries |
| 14 | `resumeCanonicalSignals` | `resume_signal_canon_restore` | canonical-signal rows | staged on the stations table |
| 15 | `resumeRxCbLinearization` | `resume_rxcb_lin_restore` | RL rows | staged on the stations table |
| 16 | `resumeCouplingRegressors` | `resume_coupling_restore` | QS QL QZ rows; clock-anchor and pending-cutover lists; QR | coupling regression of the restored L and Z carries; clock re-draw seeds (`tu_reinit`); the refloat set |
| 17 | `resumeDenseBorder` | `resume_coupling_restore` or `resume_border_restore`, with `shared_carry_full_cov` | QK QP; previous pivots; snapshot epoch | `prev_carry_cov`; the exact update's stash stamp and pivots; the pivot-frame pivots |
| 18 | `resumeFirstFoldCensus` | `shared_carry_full_cov` | nothing | the first-fold census latch and count |
| 19 | `resumeSlipWarmup` | `resume_slip_warmup_ep` above 0 | nothing | `resume_slip_warmup_left` |
| 20 | `resumeD5Warmup` | `resume_d5_warmup_ep` above 0 | nothing | `resume_d5_warmup_left` |
| 21 | `resumeSatelliteRecords` | always | Y R A I X; URA estimate | satellite records (including the ephemeris-cutover baseline and pending bumps) |
| 22 | `resumeSeamLatches` | always | nothing | the one-shot latches of the first post-resume epoch; the code-bias "resumed" latch is the negation of `ResumeFlow::datum_cb_restored` |
| 23 | `resumeProducerCounters` | always | counters, generation id, ramp state | the producer counters, the generation id, the warm-up ramp |
| 24 | `resumeCarryBlob` | `state_snapshot_carry` | SV GG; the DD carry blob | vertex and group numbering; the invariant carry (cold-started when it cannot be restored or bound) |
| 25 | `resumeReport` | always | stamps | the summary log line |

After the function returns, the driver calls `networkRestoreCanonicalSignals` (applies group 14 to the station models) and `networkApplyRxCbLinRestore` (applies group 15 to the station parameter sets), and later `networkChainMemoryRestore` (puts back group 11 after the section's own fingerprint check).

### Order dependencies

1. Nothing is restored until the gate passes. The emitter is restored first and all or nothing: the carries without the emitter's baselines would emit wrong discontinuity counters.
2. The shared carry (group 5) precedes every group that reads it: the wet-delay seam counts the Z rows, the frame group primes the handover ledger from the restored leveling rows, the coupling group rebuilds the regression of the L and Z rows and stamps the clock re-draw seeds above their last-seen times, the dense border keeps only the keys of `prev_carry`, and the report counts what was restored.
3. The frame group (7) precedes the datum, emit, hold and station groups: the loader requires `resume_frame_restore` with each of them, because they are expressed in the realization frame it brings back. The datum group needs the satellite side of the eq34 tree that the frame group restores.
4. The datum group's code-bias verdict is read by the seam latches (22).
5. The hold group (10) follows the emit group (9). It queues clone births at `counters.n_loop_epochs`, which is zero before the first epoch.
6. The chain memory is staged only; the driver restores it with the group-delay latch in scope. The loader requires `resume_datum_restore` with `resume_chain_restore`.
7. The canonical-signal and linearization rows are staged because the station models and parameter sets are not in scope. The canonical-signal latch locks on the first processed epoch, and the linearization seed latch closes at the first birth of local states, so both applications run before those points. The loader requires `resume_station_restore` and its code-bias half with `resume_rxcb_lin_restore`.
8. The dense border (17) follows the coupling regressors (16); the first-fold census latch (18) follows both.
9. The carry blob (24) restores SV and GG and binds the blob against them, after the producer counters.
10. The report (25) is last.

### What a seed arms

The seeds `evseed_s7r_gs`, `evseed_s7r_gf`, `evseed_s7b_m1` and `evseed_s7br_smoke` under `D:/GnssProcessorSupport/runs_nom` arm these capture keys: `state_snapshot_frame_capture`, `_coupling_capture`, `_datum_capture`, `_emit_capture`, `_hold_capture`, `_station_capture`, `_signal_capture` and `_rxcb_lin_capture` in all four; `_chain_capture` in `evseed_s7b_m1` and `evseed_s7br_smoke` only. None arms `state_snapshot_carry` (the carry blob and the SV and GG rows) or `clock_level_monitor` (the LB rows). The legs of `scripts/event_leg.py` arm the restore keys of the groups above from the seed (frame, datum, emit, hold, station, signal, coupling, the first-fold census, `resume_d5_warmup_ep` 1, the wet-delay seam, and the chain memory where the seed wrote it), but never `resume_border_restore` and `resume_slip_warmup_ep`, and not `resume_rxcb_lin_restore` where `rx_code_bias_datum` is armed (all four seeds). The groups that no leg of these seeds exercises are therefore 12 (clock level), 15 (code-bias linearization), 19 (slip warm-up), 24 (carry blob), the standalone border restore of group 17, the clone queue of group 10 and, for `evseed_s7r_gs` and `evseed_s7r_gf`, group 11 and the eq34 half of group 8. The refusal and cold-start branches of groups 1 to 4 and the failure branch of group 24 are not reached by a leg that resumes cleanly.
