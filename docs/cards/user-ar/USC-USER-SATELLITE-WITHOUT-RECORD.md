# USC: the user uses a satellite without its stream record

**STATUS**

- **id:** USC
- **topic:** user-ar
- **status:** open
- **summary:** Pre-registered 2026-11-22 after step 0 (measure only, existing traces). A satellite whose clock record the stream does not carry at the epoch (absent, or older than 30 s) stays in the user's solution with the a priori clock and no bias; a refused phase or code bias leaves its row in with a bias of zero. Both observed cases at ASCG00SHN on DOY 129 are this path: E25 back on a 57 min old record (1.59 m) and E24 at its first sighting without a record (55.23 m, sigma 0.69 m), each at an epoch where the filter's complete reset fired. A default-off rule that leaves such a satellite out already exists (card IRDU, ledger UNVALIDATED, never armed).
- **dates:** 2026-11-22 (step 0 measured and the card filed; the build step waits for the author's word).
- **knobs / posture:** none new at filing. Existing, default off: `network.closed_loop.drop_unrecorded` and `network.closed_loop.drop_unrecorded_phase_bias` (consumer `processing.ssr_drop_unrecorded`, `processing.ssr_drop_unrecorded_phase_bias`). Related, armed in the reference files since 2026-11-22: `network.closed_loop.code_bias_gap_response` = full (card CBG).
- **code:** `src/core/realtime/RealTimeEnginePrepare.cpp` (`dropUnrecorded`), `include/realtime/UnrecordedSatGate.hpp`, `src/core/realtime/RealTimeEngineSsrBreaks.cpp` (`resetOnPhaseBiasDisc`, `reinflateOnCodeBiasDisc`), `src/core/corrections/GnssCorrections.cpp` (the clock overlay), `src/core/measurement/ObservationEquation.cpp` (`addSsrBias`), `src/core/filter/PivotManager.cpp` (the reference election), `src/core/filter/KalmanPostFitQc.cpp` and `KalmanOutlierResponse.cpp` (the complete reset).
- **run dirs:** none of its own at filing. Read: `D:\GnssProcessorSupport\runs_irm\_rap_n20_*`, `_rap_n20c_*`; `D:\GnssProcessorSupport\runs_varrate\_rap_vr0*`; `D:\GnssProcessorSupport\runs_nom\_rap_agu3_d10{x,f}_*`, `_rap_agu3_roof10{x,f}`, `_rap_agu_final24_n14*`. Scripts and outputs: `D:\GnssProcessorSupport\gates\user_stale_corrections\` (`scripts\count_unrecorded.py`, `out\`, `REPORT_0.md`).
- **source:** the managing session's order of 2026-11-22 (the two user weaknesses found by card VRE step 0b: `docs/cards/perf/VRE-VARIABLE-RATE-ESTIMATION.md` on the unmerged branch `net/variable-rate-2026-11-21`, sections dated 2026-11-22); card IRDU sections 12.10 to 15 ([part 4](../../record/refound/IRDU_user_integers_on_the_chain_2026-11-14_part4.md), [part 6](../../record/refound/IRDU_user_integers_on_the_chain_2026-11-14_part6.md)); card [CBG](CBG-USER-CODE-BIAS-GAP.md).

**Outcome.** Step 0, existing traces only. Both observed cases are one path: a satellite used with no correction at an epoch whose residuals trip the filter's complete reset (all 9 resets in 295 held-product replays). In the reference streams the path is rare and harmless: one 7 min event in 51 receiver-days of 6 h, five E18 events in 210 poster day-long users, no reset. It is reached when a record is late or the stream stops (5.3 m past the end of the simulator stream). The design arms and completes the existing default-off rule of card IRDU; one simulator event is registered.

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

When the stream carries no usable record for one satellite (the record is late, withheld, or older than the age gate,
or the satellite rises before the stream carries it), does the user keep that satellite in its solution, and at what
cost? Can a default-off response take the satellite out at the epoch its record is refused and bring it back as a
re-coupling at the epoch its record returns, with counters at both edges and identical output when off?

## 2. What the user does today (from the code, main `33c5fef`)

1. **Clock.** `GnssCorrections::getSatelliteClockCorrection` adds the stream's clock record (C) to the a priori clock
   only when `SsrCorrectionStore::getOrbitClock` returns a record within `clockHoldWindow(ssr_max_age_s, 0)` = 30 s of
   the observation. A satellite without one keeps the a priori clock of the run's products (the precise CLK; the
   broadcast clock only where no CLK sample covers the epoch). Its range model is not broadcast-grade, so
   `broadcast_only` stays false, its phase stays in, and `dropBroadcastGrade` does not remove it. A record that exists
   but is too old gives a warning in the corrections log at each epoch ("REJECTED stale"); an absent record gives
   none.
2. **Biases.** The closed-loop user arms the bias age gate (`enable_ssr_bias_max_age`, 30 s times the hold factor 2).
   `addSsrBias` (`ObservationEquation.cpp`) builds a code or phase row whose bias is older than 60 s, or absent, with a
   bias of zero. The row keeps its weight: no term of the user's variance model depends on the bias age, and the URA
   staleness channel (`network.consume_ura`) is KEEP_OFF.
3. **Freshness flips.** `resetOnPhaseBiasDisc` (`RealTimeEngineSsrBreaks.cpp`) resets the ambiguity of a phase signal
   whose bias changes between applied and refused (`reset_flip`), and `reinflateOnCodeBiasDisc` re-inflates the
   receiver code-bias states on a code-bias flip (`reinflate_flip`). The flip memory is kept while a satellite is out
   of view, so a satellite that left with a fresh record and returns with only that record flips at its return
   (fresh to stale) and again when the new record arrives (stale to fresh), with a reset at each.
4. **Held integers.** A hold is a pseudo-observation that leaves the ambiguity at hold variance in the covariance. Any
   reset of that ambiguity releases it; a satellite whose rows stay in keeps its other holds.
5. **Reference satellite.** The differenced user elects its reference satellite per constellation in
   `PivotManager::selectPivotCandidates` from elevation, phase-signal count, score and ephemeris age. No test reads the
   stream records, so a satellite without them can be elected, and every single difference of that constellation then
   carries its missing corrections.
6. **The complete reset.** When an epoch's bad-residual count exceeds `bad_meas_percent` of its rows,
   `KalmanFilter::completeResetBackstop_` calls `resetFilterCovariance`: the covariance is zeroed, the diagonals of
   position, clock, troposphere, inter-system offsets, slant ionosphere and every ambiguity are re-seeded (the
   reference satellite's ambiguity and ionosphere stay pinned at variance 0), and every state value is kept. Every
   hold is released. State families the function does not list (the receiver code-bias, satellite-bias, receiver
   phase-bias and receiver-satellite code-bias states, where present) keep a variance of zero. The epoch's update is
   rebuilt from that covariance.
7. **Seams.** The admission gates (`RealTimeEngineAdmit.cpp`) act on whole epochs; the corrections-ready gate counts
   broadcast-grade satellites at start-up only. The innovation screen (`RealTimeEngineScreen.cpp`) is off in every
   user examined. The only satellite-level response to a missing record is the default-off rule in
   `RealTimeEnginePrepare.cpp` (`dropUnrecorded`, card IRDU 12.11 and 15): a satellite without its clock record
   (and, with the phase-bias key, without a phase-bias record for a used phase signal) is erased before the reference
   satellite is elected and the rows are built; its ambiguity and ionosphere states are pruned as for a satellite out
   of view; at the return of its records they are seeded afresh. Ledger UNVALIDATED; the clock rule is measured on the
   simulator only (12.11); the graded runs of the phase-bias rule (15.1) never ran; armed in no reference file and in
   none of the runs read here.
8. **The code-bias gap response (card CBG, `full` in the reference files since 2026-11-22).** It covers a satellite
   whose clock and phase biases are present and whose code bias is missing: the bias-less code rows leave the update,
   a new fix of the satellite is barred, and its holds are re-tested after the bias returns. A satellite without its
   clock record also lacks its code biases, so `full` would leave its code rows out, but its phase rows stay with no
   clock correction and no phase bias, it can still be elected the reference satellite, and nothing leaves it out of
   the solution. CBG therefore does not cover either observed case. None of the runs read here had CBG armed.

## 3. The two observed cases (ASCG00SHN, DOY 129, held products of card VRE)

- **Case 1** (`runs_varrate/_rap_vr0b_129_ascg_H300`, control `runs_irm/_rap_n20_129_ascg`). E25 is absent from the
  network's products from 264930 to 268320 s. At 268320 s the user takes it back while its only record is that of
  264930 s (3390 s old; the held file gives the new record from 268500 s). Both its phase biases and both its code
  biases flip to stale, its ambiguities reset, and the epoch's update ends in a complete reset (3D sigma 0.02 to
  94.9 m; the position is kept). All 14 held integers are released. From 268380 to 268470 s E25 is the Galileo
  reference satellite (phase rows pinned at amb 0, variance 0) with no correction applied. 3D error 0.024 m before,
  1.589 m at 268350 s, 0.56 m over 268380 to 268470 s, 0.12 m at 2.85 h; control 0.026 m. The never-fixing twins
  (`_rap_vr0bf_129_ascg_H300`, `_L300`) reset at the same epoch (0.53 and 0.57 m within 10 min).
- **Case 2** (`runs_varrate/_rap_vr0_129_ascg_P300`, same control). E24 rises at 261930 s (00:45:30). The held file
  gives it no record before the next block start, and the unheld control has its record at that epoch. The epoch ends
  in a complete reset; one epoch later the error is 55.23 m with a sigma of 0.69 m (control 0.40 m). The same E24
  entry costs HARB00ZAF 2.38 m, SCRZ00BOL 0.79 m and the built-in user WTZZ00DEU 0.89 m without a reset.
- Both cases are one path: a satellite in the solution without its record, at an epoch whose residuals trip the
  complete reset. Step 0 does not trace why the reset of case 2 settles 55 m off with a small sigma; section 6 of the
  design names the candidates.

## 4. Design of the response (default off; identical output when off)

**Starting point.** The rule of card IRDU (`drop_unrecorded_phase_bias`, which includes the clock rule) is the
decoupling edge this card needs, at the right place: before the reference election and before any row is built.

1. **Decoupling edge (the record is refused).** A satellite leaves the solution at the first epoch at which its clock
   record is older than 30 s or absent, or a used phase signal's bias is older than 60 s or absent; the test is the
   apply site's own, so a satellite is out exactly when it would be used uncorrected. Its ambiguity and ionosphere
   states are pruned, so its holds end with it; a reference satellite that leaves hands over as a satellite out of
   view does. Event: one Drop per satellite naming the missing record, its signal, the age of the last record (or
   none) and how the drop began (first sighting, record lost, back in view on a stale record), written to the engine
   log and to the consumer satellite trace (`drop_unrec`). Counters: drops by cause, satellite-epochs out, reference
   satellites lost, and the age of every refused record in four bins (30 to 60 s, 1 to 5 min, 5 to 30 min, longer).
2. **Re-coupling edge (the record returns).** A dropped satellite is readmitted at the first epoch at which every
   record it needs passes the same tests. It is reborn loose: ambiguity and ionosphere seeded afresh from that epoch,
   as a new arc, so nothing estimated against the a priori clock is carried into the stream's frame. Its flip memory
   is set to the returned state, so the return makes no second reset. Event `readmit_unrec` with the gap length;
   counters for readmits and gaps by length. Re-validation instead of rebirth (a parked ambiguity restored when the arc
   and the phase-bias generation are continuous) is not proposed for the first build: rebirth is the safe default, and
   step 0 shows no case where it would cost a hold worth keeping (section 6).
3. **Run-end line.** The existing summary (`logUnrecordedGateSummary`) gains the age bins, the reference losses and
   the satellites still out. The RT state snapshot carries the dropped set, so a warm-resumed user does not readmit a
   satellite silently (the gap card CBG records for its own response).
4. **Key.** Arming is the author's decision through a ledger row. Two choices are offered: (a) arm the existing
   `drop_unrecorded_phase_bias` after its runs of IRDU 15.1 and this card's event, with the additions above behind the
   same key (their output changes only while the rule acts); (b) a new key for the additions. Option (a) keeps one
   owner per concept.
5. **With CBG.** Both on: a satellite in class A or B is out before the code-bias response observes it (`dropUnrecorded`
   runs in the preparation, before the reactions); class C (clock and phase biases present, code bias missing) stays
   CBG's. No overlap in their counters.
6. **The complete reset (not in the first build).** With the rule on, the trigger seen in both cases (a satellite
   entering with metre-level unmodelled terms) is removed. The reset itself keeps every state value, re-seeds a listed
   subset and leaves the rest at variance zero; why case 2 settles 55 m off with a 0.69 m sigma is not traced. Named
   follow-up with its counter (complete resets per run, from the trace's cold-sigma epochs): trace the reset of case 2
   on the injected event and decide between re-seeding every family and re-seeding the position from a code solution.

## 5. One injected event on the simulator (registered before any build or run)

- **Base.** The reference simulator stream `runs_irm/_rap_n20_s_6/products.txt`, user SZZ000SIM replayed on it from the
  network's start for 6 h (the built-in user's configuration), engine of the build, Release, through the run gate.
- **Event.** One Galileo satellite's C, B, P and U records removed for 10 min at about 3 h (`scripts/irdu_record_gap.py`),
  the satellite chosen from the unedited replay's trace as the highest Galileo satellite holding integers at that time,
  so that it is likely the reference satellite. Second variant: the satellite's records removed while it is out of
  the user's view and for 2 min after it returns (the E25 pattern).
- **Arms.** Unedited control; edited stream with the key off; edited stream with the key on; the key on the unedited
  stream (identity: no event, output identical to the control).
- **Readings.** Key off: class-A rows over the window as counted by `count_unrecorded.py`, reference epochs and
  complete resets. Key on: one Drop at the first epoch past 30 s, no row of the satellite in the window, one Readmit at
  the return with a fresh ambiguity variance, no complete reset, every other hold kept, the 3D error within 5 mm of the
  control from 10 min after the return, and 0 wrong held integers against the fixture's truth.
- **Limit.** On the simulator the stream's corrections stand closer to the a priori products than on the field: the
  held simulator replays of card VRE meet four uncorrected returns at no cost (section 6). The event proves the edges,
  counters and identity; the harm and its removal are graded on the field cases `_rap_vr0b_129_ascg_H300` and
  `_rap_vr0_129_ascg_P300` replayed with the key, against their controls.

## 6. 2026-11-22: step 0 results (existing traces, no engine run)

Working report `D:\GnssProcessorSupport\gates\user_stale_corrections\REPORT_0.md`; counter
`scripts\count_unrecorded.py` (per run: the classes of section 2 from `closed_loop_sat_trace.csv`, episodes, record
ages from the replayed product file, reference epochs, 3D error before, at the peak and 10 min after against the
run's control, complete resets as a 3D sigma above 50 m after one below 10 m); outputs in `out\`. Classes: A no clock
correction applied; B clock applied, a used phase signal's bias missing; C clock and phase biases applied, a used code
signal's bias missing. All 653 run configurations read have the unrecorded rule, CBG and the innovation screen off.

| Group of runs | Runs | Satellite-epochs | A: episodes (satellite-epochs) | B | C | A or B as reference (epochs) | Full resets (with an uncorrected satellite) | Past the end of the stream |
|---|---|---|---|---|---|---|---|---|
| Reference streams, 16 receivers and WTZZ00DEU, DOY 129, 130, 132 (`runs_irm/_rap_n20_<doy>_<site>`) | 51 | 533,863 | 2 (28) | 0 | 0 | 0 | 0 | 0 |
| Reference networks, built-in users (`_rap_n20_{f129,f130,f132,s,b}_6`) | 5 | 56,984 | 0 | 0 | 0 | 0 | 0 | 0 |
| Held-product replays, field (`runs_varrate/_rap_vr0*`) | 284 | 2,968,210 | 230 (1,208) | 0 | 0 | 49 | 9 (9) | 0 |
| Held-product replays, simulator (`runs_varrate/_rap_vr0*_s_*`) | 11 | 108,152 | 38 (330) | 0 | 0 | 0 | 0 | 10,373 |
| Poster users, roof pair, 24 h networks (`runs_nom/_rap_agu3_*`, `_rap_agu_final24_n14*`) | 217 | 11,009,405 | 10 (58) | 0 | 38 (1,720) | 0 | 0 | 0 |
| Frozen-lineage controls (`runs_irm/_rap_n20c_*`) | 55 | 574,021 | 2 (28) | 122 (13,700) | 0 | 0 | 0 | 0 |

- **Reference streams.** One event, the same at two receivers: G01's clock record stops at 529980 s on DOY 132
  (03:13:00) for 7 min at ARHT00ATA and DAV100ATA (`_rap_n20_132_{arht,dav1}`); G01 stays in uncorrected for 14
  epochs, not the reference, no hold involved; error 0.049 to 0.047 m and 0.039 to 0.063 m (controls 0.056 and
  0.129 m). Poster users: E18 only (DOY 144 at ARHT and QUIN, the first epoch of DOY 145), no rise. Class C appears
  only on the poster days (card CBG's E14, E18, E32), at most 0.1 cm of rise after the first epochs of a run.
- **Complete resets: 9, all in held replays at ASCG00SHN, each at the entry of a class-A satellite.** E25 on DOY 129
  at 268320 s (fixing H300 and L300: 1.589 and 1.580 m, 14 holds to 0 for 8 epochs while the control keeps 14 to 16;
  never-fixing twins 0.45 and 0.47 m); E24 on DOY 129 at 261930 s (P300: 55.23 m, sigma 0.69 m, above 5 m for 30 min,
  above 0.5 m for 62 min, 188 epochs with the error above 5 sigma; control 0.90 m at its peak); E02 on DOY 132 at
  528210 s (fixing H300 and L300: 0.090 and 0.092 m against 0.081 m, GPS holds 10 to 4 for 4 epochs while the control
  keeps 10; never-fixing twins 0.10 m).
- **Reference satellite.** In 49 epochs of the field held replays the reference satellite of a constellation is a
  class-A satellite (per run: E25 on DOY 129 for 4 or 5 epochs, E31 on DOY 130 for 4, E02 on DOY 132 for 2, G01 on
  DOY 132 for 1).
- **The other 176 class-A episodes** of the field held replays (outside ASCG00SHN, the E24 entry excluded) peak at
  most 3.1 cm above their control (SCRZ00BOL, G12 at 267720 s). The E24 entry without a reset costs HARB00ZAF 2.38 m
  (control 0.57 m).
- **Past the end of the stream.** The simulator replays of card VRE run from 262890 to 284460 s; the stream's last
  epoch is 282630 s. Over the last 60 epochs every satellite is class A: 5.3 m error with a 0.79 m sigma, in the
  unheld control `_rap_vr0_s_U` too. A stopped stream gives no response in the user.
- **Frozen lineage.** Its class B is the Galileo L1C/L5Q phase-bias gap that the single Galileo network closed in the
  reference posture; the reference streams carry no class B.
- **Checks.** Simulator reference run `_rap_n20_s_6`: 0 episodes in 9,773 satellite-epochs; trace and product stream
  agree on every row. The simulator's held replays give positive cases (E25, G12, G25, E02, G20 returning inside hold
  blocks, and the end of the stream), each found with its record age. Trace against stream on every row of every
  run: exact in the reference, held and poster sets; the frozen-lineage and BeiDou-lineage streams stamp epochs with
  a residue of about 20 us, which a 1 us tolerance misread; with 1 ms, 150 of about 1.18 million clock and phase-bias rows still differ, all in
  the frozen-lineage streams (which carry the phase-bias hold), not traced; no reading rests on that lineage. Holds are
  compared with the control at the same epochs (`held_timeline.py`, a 0.01 m^2 variance ceiling): the step 0
  grader's rule reads every ambiguity as held at a reset epoch, and a hold loss at 274590 s on DOY 129 (ASCG, STPM)
  is the GPS reference handover G25 to G15, present in the control as well.
- **Not traced:** why the reset of case 2 settles 55 m off with a 0.69 m sigma (the field trace does not carry the
  user's code-bias states). Section 4, item 6.
