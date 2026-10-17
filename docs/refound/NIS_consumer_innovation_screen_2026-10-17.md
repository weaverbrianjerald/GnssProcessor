# NIS: the consumer's pre-update innovation screen

**STATUS**

- **id:** NIS
- **topic:** consumer
- **status:** open
- **summary:** BUILT 2026-10-17, default OFF, NOT ARMED: before each measurement update the consumer tests every undifferenced row's innovation against the predicted state (conditional normalized innovation with a model-error floor, flag above 6), holds a flagged row out of that epoch's update as an event with counters, re-tests it every epoch, and re-floats the ambiguity of a phase row flagged on three consecutive epochs. Bars N0 to N5 are frozen below (N2 amended before any graded read, section 8); the check on the field golden and the 16-site census is owed.
- **source:** [PBF](PBF_pb_hold_presence_edges_2026-09-28.md) section 1 item 3 (one 92 m phase row moved the first update 704 m and the peel reset innocent satellites) and section 4 owed item 2 (a pre-update screen of the innovations); the register item "Consumer innovation screen" ([OPEN_ITEMS](../status/OPEN_ITEMS.md)); standing invariant 7 ([CLAUDE.md](../../CLAUDE.md)).
- **knobs / posture:** new `processing.innovation_screen` (bool, default false, byte-identical) with `innovation_screen_k` 6.0, `innovation_screen_mode` `act`, `innovation_screen_rows` `all`, `innovation_screen_max_fraction` 0.25, `innovation_screen_refloat_ep` 3, `innovation_screen_phase_floor_m` 0.3 and `innovation_screen_code_floor_m` 1.0; new `network.closed_loop.innovation_screen` with the same seven parameters (derived into the consumer engine, never reaching the producer's solve or its resume fingerprint); ledger rows UNVALIDATED (the key) and PARAM (the seven); no golden, no loader default and no posture row changed.
- **dates in the text:** 2026-10-17
- **hashes / run dirs:** base main `4318fe3`; branch `feat/innoscreen`; control exe `gnss_processor_innoscreen_ctl.exe` (main `4318fe3`, Release); the check's run dirs are filled in below when it has run.

**Outcome.** Question: can a gross row be stopped before the update spreads it, by testing each innovation against the previous state? The screen is built with its counters and both exclusion edges, unit-tested on synthetic matrices and on a hand-built engine measurement, and default off. A model-error floor was added at build after the formal covariance proved miscalibrated (section 8). The check (N0 to N5) has not run: section 8 is a 2 h build-stage smoke, not a result. On it the screen flags nothing at six edge-free sites, takes the 92 m row at ARHT00ATA through exclusion and re-float, and moves the position by millimetres, because today's consumer already survives that row. Arming is the author's decision; this card never arms.

## 1. The case

Card PBF found that a held phase bias appearing on a row already in use (G32 L1W, 92.409 m at TOW 260130 on the CLV wire, ARHT00ATA) stepped the row by the whole bias. The consumer's first update pass then moved the state 704 m, all 36 standardized residuals exceeded the post-fit threshold, and the peel reset G03, G06, G28 and G19 and excluded G32's code rows; the epoch committed nothing. At TOW 260160 the G32 L1W row entered with a -104.6 m innovation and the update moved the position 490 m and the receiver clock -270 m.

PBF answered that edge where it is declared (`pb_hold_presence_flip`). The generic defect stays: any other unanswered step (an undeclared producer step, a missed slip) meets the same peel, which works on residuals computed after the state has absorbed the row.

## 2. Why the innovations, and why the conditional form

The Kalman update spreads one gross row over every state the row touches, so after one pass every row looks inconsistent and the post-fit statistics cannot tell the culprit. The innovations are formed against the previous epoch's state, before the update: only the gross row's own innovation is wrong. The screen tests them there.

With the design matrix H, the predicted covariance P and the measurement noise R, the innovation covariance is S = H P H^T + R. Row i is tested against what the other rows imply for it:

    w_i = (S^-1 nu)_i / sqrt((S^-1)_ii)

which is the innovation of row i minus its prediction from the other rows, over the standard deviation of that difference. Without a fault w_i is standard normal; a gross error d on row i alone shifts it by d divided by the conditional standard deviation of the row.

The marginal form nu_i / sqrt(S_ii) cannot see this case. The consumer's receiver-clock process noise is 1000 m2/s ([PBF](PBF_pb_hold_presence_edges_2026-09-28.md) section 1), which is 3e4 m2 over a 30 s epoch, so every S_ii carries a clock standard deviation near 173 m and a 92 m step is half a marginal sigma. The conditional form lets the other rows fix the common clock, geometry and troposphere first and tests only the part they do not explain. The unit test `A92MetrePhaseStepIsInvisibleMarginallyAndGrossConditionally` pins this on a toy system with that clock variance: marginal statistic under 1, conditional above 100.

## 3. The decision rule

1. **Eligible rows.** Undifferenced single-satellite code and phase rows (`G32` with `L1W` or `C1W`), of the class `innovation_screen_rows` selects. Every other row (a differenced `<sat>-<pivot>` row, an aiding row, a combination) is dropped from S and from the vector before the test (marginalized out), so a fault in a row the screen may not exclude cannot make an eligible row look wrong. A differenced consumer therefore has no eligible row and is counted as skipped.
2. **Model-error floor.** S carries `innovation_screen_phase_floor_m` squared (0.3 m) on every phase row and `innovation_screen_code_floor_m` squared (1.0 m) on every code row, for the screen only. The filter's formal phase noise is millimetres, while the epoch-to-epoch innovations of a healthy consumer spread over decimetres (section 8): on the formal covariance alone, a healthy ARHT00ATA replay flagged 170 phase rows in 87 of 240 epochs. The floors are the unmodelled error budget (product clock and orbit error, ionosphere, multipath), stated in metres so that the threshold reads physically: with k = 6 and the default floors, a phase row is flagged when it departs from what the other rows imply by about 2 m or more, a code row by about 7 m.
3. **Sequential removal.** The eligible row with the largest |w| is removed when it exceeds k (default 6.0), S^-1 and S^-1 nu are downdated by the rank-one identity for the removed row, and the test repeats until no eligible row exceeds k. This is the usual single-outlier snooping applied one row at a time, on the prior; it stops two gross rows from masking each other. The downdate equals a refactor of the reduced S (unit test `TwoGrossRowsLeaveInOrderWithTheDowndatedStatistic`).
4. **Abstain.** More than `innovation_screen_max_fraction` (0.25) of the epoch's rows flagged means the prior is wrong, not the rows; fewer than six rows would remain; or S is not positive definite. The epoch then goes to the update and the post-fit machinery unscreened, counted with its reason, and every open exclusion is released.
5. **Act.** In `act` mode a flagged row's key (`<sat>_<signal>`) joins the exclusion set the measurement model already reads, and the first update pass is built again without it. In `observe` mode everything is computed and counted and nothing is written.

## 4. The two edges of an exclusion (standing invariant 7)

An exclusion decouples a row's information from the states. The decoupling edge is the event EXCLUDE (key, w, conditional sigma, innovation, streak). Every epoch tests every row again, including those excluded the epoch before, so the re-coupling edge is a re-validation, not a timer:

| edge | event | meaning |
| --- | --- | --- |
| decouple | EXCLUDE | flagged: the row leaves this epoch's update |
| re-couple | READMIT | an excluded key passes again; the row enters the update |
| re-couple | REFLOAT | a phase key flagged on `innovation_screen_refloat_ep` (3) consecutive epochs: the ambiguity is re-floated through the first-sighting reset path (the arcs of the slip detectors are reset with it) and the row enters the update the same epoch with the fresh ambiguity. A persistent step cannot heal while its ambiguity is held out |
| re-couple | LOST | the excluded key left the epoch's row set |
| re-couple | RELEASED | the screen abstained; the open exclusion ended with the row in the update |

A code row has no state to heal and stays excluded for as long as it fails. Closure, printed at run end: episodes opened = readmitted + re-floated + lost + released + still open; the line says HOLDS or BROKEN (unit test `ClosureHoldsOverARandomSequence`).

Identities: every event is an `[RT] Innovation screen:` line in the engine log with the satellite, signal, w, conditional sigma, innovation and streak, and a `screen_*` token (`screen_obs_*` in observe mode) in the `ev_pb` or `ev_cb` column of `closed_loop_sat_trace.csv` when `network.trace_consumer_sat` is armed. The run-end block (`logInnovationScreenSummary`) prints rows screened, flagged rows by class, flagged epochs, abstains by reason, the episode closure, and a histogram of the epoch maximum of |w| (below 2, 2 to 3, 3 to 4, 4 to 5, 5 to 6, 6 to 10, 10 to 100, 100 and above). The histogram is the calibration read for k: in `observe` mode on a healthy replay it shows how close an ordinary epoch comes to the threshold.

## 5. Placement and what is unchanged

`RealTimeEngine::runMeasurementUpdate` calls `screenInnovations_` after the predict, the slip detection and the datum-break reactions have settled the predicted state and before the first update pass. With the knob off the call returns false at its first line and the loop builds the pass exactly as before. With the knob on, the screen builds the first pass itself and the loop starts from it, so an armed epoch that flags nothing performs the same computation as an unarmed one (bar N1 checks this by construction and by comparison). The statistic uses the formal measurement noise R; the filter's own robust R inflations (the persistent-bias and Huber queues) apply later, inside the update, and are not in S. The producer, its wire and its fingerprint are untouched. Not built: a differenced (BSSD) consumer screen, a snapshot of the open episodes (a restored consumer starts with none), the drain of the events into `network_events.csv` (the log and the satellite trace carry them).

Files: `include/realtime/InnovationScreen.hpp` and `src/core/realtime/InnovationScreen.cpp` (statistic, removal, episodes, counters), `src/core/realtime/RealTimeEngineScreen.cpp` (engine side), a call in `RealTimeEngineEstimate.cpp`, the config in `GnssConfig*.{hpp,cpp}`, three run-end call sites (`RealTimeEngine::flush`, `ApplicationConsumerReplay.cpp`, `network/runend/ClosedLoopRunEnd.cpp`: one line each), tests `tests/test_innovation_screen.cpp` and `tests/test_innovation_screen_engine.cpp`.

## 6. The bars (frozen at pre-registration, before any knob-on grade was read)

Run keys and procedures are those of cards PBF and MSC: Release exe only, same-exe controls, the field golden for the held-out consumer, `scripts/clock_level/gcensus.py make` with `--set network.closed_loop.innovation_screen=true` for the 16 sites and WTZZ00DEU, graded against the control key with `gcensus.py grade`.

| gate | reading | bar |
| --- | --- | --- |
| N0 (the GATE) | the new exe with the key absent against the control exe built from main `4318fe3`: field golden consumer 6 h, and the 17 census replays of the wire `_rf0hv15_field6` | `products.txt` records, `closed_loop_trace.csv` and `network_epoch_status.csv` of the golden run byte-identical, and `closed_loop_trace.csv` byte-identical at 17 of 17; the Debug unit suite passes |
| N1 | the key armed with `innovation_screen_k` 1e9 against the key absent, same exe | `closed_loop_trace.csv` byte-identical at 17 of 17 and on the golden run; flagged 0, abstained 0 |
| N2 (the case, amended 2026-10-17 before any graded read, section 8) | the ARHT00ATA replay on the CLV wire (`runs/_rap_clv2_f_b1/products.txt`) with `pb_hold_presence_flip` off, 2 h: screen on against off, same exe | the run-end and the log show G32 L1W excluded at TOW 260130 and re-floated no later than TOW 260190, and no other satellite excluded between TOW 260100 and 260400; at no epoch from TOW 260130 to 260400 is the 3D error worse than the knob-off run by more than 0.05 m. The original bar (hour-2 p50 below 10 m against 803.907 m) is dropped: the knob-off replay of today's exe does not diverge |
| N3 | the field golden, 6 h, held-out consumer WTZZ00DEU, screen on against the same-exe control | by-hour p50 3D error: no hour worse than the control by more than 0.005 m; every flagged row read and attributed (the expected count is 0) |
| N4 | the 16-site census, screen on against the same-exe control (`gcensus.py grade`) | the median over sites of the change in the mean hourly p50, hours 2 to 6, within 0.005 m; no site worse than the control by more than 0.05 m in hour 2 or in the mean of hours 2 to 6; every site with 0 flagged rows byte-identical to the control; QUIN00USA and ARHT00ATA (the sites with edges) reported separately |
| N5 | the counters of every run above, plus the 17 replays in `observe` mode | rows screened, flagged rows by class, flagged epochs, abstains, episode closure HOLDS in every run; the false-flag rate on the edge-free sites read from the histogram and the observe counters, not assumed |

A verdict that misses a bar is reported with the miss. The result is reported and never armed; arming needs a ledger row change and a golden pin by the author's decision (the key's ledger verdict is UNVALIDATED until then).

## 7. Owed by the card

1. The check itself (N0 to N5).
2. The threshold. k = 6.0 is a gross-error bar chosen from the Gaussian tail (two-sided 2e-9 per row, so about 6e-5 false flags over the 29 thousand row tests of a 6 h run at the nominal model); the real tails are heavier and the first hour is optimistic, which is what the histogram of N5 measures. A different k is a new card line, not an edit of the bars above.
3. The deferrals, triaged (standing invariant 3):
   - a differenced (BSSD) consumer is not screened: field-inert, the golden consumers are undifferenced (`consumer_undifferenced` true) and a differenced epoch has no eligible row, which the run-end line counts as skipped calls;
   - the filter's own robust R inflations (persistent-bias EWMA, Huber queue) are not in S: field-inert for the screen's purpose (a gross-error bar sits far above a downweighted row), unmeasured otherwise;
   - the open exclusion episodes are not in the real-time state snapshot: field-inert for the cold 6 h runs of the bars, field-blocking for a warm-resumed consumer, which restarts with no open episode and so loses a streak toward a re-float; the knob is not warm-resume-runnable until the episodes are snapshotted;
   - the events are not drained into `network_events.csv`: field-inert, the engine log and the satellite trace carry the same identities.

## 8. Build-stage readings (2026-10-17; 2 h consumer replays; not graded, not the check)

Exe `gnss_processor_innoscreen.exe` (Release, sha256 prefix `862abec86112b9aa`, main `4318fe3` plus this card's diff) against the control `gnss_processor_innoscreen_ctl.exe` (main `4318fe3`, prefix `626b503eb6c9d00b`). Arms are copies of the current field golden's arm (`runs/_rf0hv15_field6`) with only the consumer station, the replayed product file and `network.trace_consumer_sat` changed, 2 h, run dirs `runs_nom/_rap_isc_*`. The bars of section 6 were not read from these runs; they only fixed the design and one amendment.

1. **Identity.** The new exe with the key absent reproduces the control byte for byte on ARHT00ATA (CLV wire): `closed_loop_trace.csv` and `closed_loop_sat_trace.csv`. In `observe` mode `closed_loop_trace.csv` equals the control's at WTZZ00DEU, QUIN00USA and ARHT00ATA, and the satellite trace equals it except the six `screen_obs_*` tokens in `ev_pb` at ARHT00ATA. The formal N0 and N1 comparisons are owed.
2. **The formal covariance is not calibrated for this consumer (design change before any graded read).** The first build tested S = H P H^T + R with no floor. On the ARHT00ATA replay it flagged 170 phase rows in 87 of 240 epochs in `act` mode (35 re-floats) and 330 in 219 epochs in `observe` mode, with the epoch maximum of |w| at 6 to 10 in 172 of 240 epochs. In the knob-off run from epoch 10 on, the deviation of each row's innovation from the epoch's median over all rows is, for phase rows, 0.058 m at the median, 0.21 m at the 90th percentile and 0.65 m at the 99th (code rows 0.134, 0.40 and 0.82 m), while the formal conditional standard deviation of the flagged phase rows was 4 to 15 mm. The floors of section 3 item 2 were added.
3. **Edge-free sites, `observe`, default floors, k = 6.** WTZZ00DEU, QUIN00USA, DAV100ATA, CEDU00AUS, BAKE00CAN and POHN00FSM, 240 epochs and 84 thousand eligible rows in all: 0 flagged. The epoch maximum of |w| is below 2 in 240 of 240 epochs at WTZZ00DEU, QUIN00USA, CEDU00AUS and BAKE00CAN; POHN00FSM is below 2 in 225 and never above 4; DAV100ATA is below 2 in 176 and reaches the 5 to 6 bucket in 20 epochs, none above 6. The margin to the threshold is therefore thin at DAV100ATA (a site with a known intermittent phase problem, [GAD](GAD_galileo_adoption_2026-10-13.md) section 4) and wide elsewhere; k stays at 6.0 and the 6 h read of N5 decides whether a longer run flags healthy rows there.
4. **The case.** ARHT00ATA on the CLV wire, presence flip off, `act`: G32 L1W is excluded at TOW 260130 (innovation -98.6 m, w -248, conditional sigma 0.37 m) and 260160 and re-floated at 260190. Two more episodes, both re-floated after three epochs: G25 L1W at TOW 262500 (about -65 m) and G04 L1W at TOW 262980 (-13.0, -18.0, -6.2 m). In all, 9 flagged phase rows in 9 epochs, 3 episodes, 3 re-floats, 0 readmitted, closure HOLDS. Against the knob-off run the 3D error differs by at most 0.015 m (mean 0.003 m) and the hourly p50 is 0.238 / 0.293 m against 0.236 / 0.291 m. The knob-off replay of today's exe does not diverge on this wire (PBF read 478.953 / 803.907 m on its 2026-09-28 build and the `_rf0hv10` base arm; the difference is not attributed here): the satellite trace shows the post-fit peel excluding the G32 L1W row and resetting its ambiguity at TOW 260130 (`amb_var` 1e6), so on this replay the screen takes the same action one step earlier, by identity and with counters, at the millimetre level. The 92 m case is therefore not a reproducible failure of the current consumer, and the original N2 bar (hour-2 p50 below 10 m) was replaced by a no-harm and identity bar.
5. **Cost** was not measured: replay wall times on a loaded box are not comparable.
6. **In-process path.** An 18-epoch run of the field golden (58 stations, `--hours 0.15`, `runs_nom/_rap_isc_gold_*`) with the key absent, armed in `act` and armed in `observe` against the control exe: `products.txt`, `closed_loop_trace.csv`, `closed_loop_sat_trace.csv` and `network_epoch_status.csv` are byte-identical in all four runs. The run-end block prints from `networkClosedLoopRunEnd` when armed (18 epochs, 904 eligible rows, 0 flagged, all 18 epochs below 2, closure HOLDS) and is absent otherwise. The 6 h N0 comparison is still owed.
