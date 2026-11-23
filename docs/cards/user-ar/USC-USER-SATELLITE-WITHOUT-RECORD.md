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

## 7. 2026-11-22: the build and its readings, registered before any build or run (managing session's order 2)

**Decision.** Taken by the managing session in the author's absence, on the author's words of the evening of
2026-11-22: "Proceed with your decisions in my absence." Scope: the build and its runs on branch
`user/stale-corrections-2026-11-22`. Nothing is merged or pushed. Arming is decided by the managing session after the
readings, under the author's arming rule; moving main and pushing stay with the author.

**What is built** (default off; with the keys off the output is identical):

1. **One owner.** The additions go behind the existing keys `network.closed_loop.drop_unrecorded` and
   `network.closed_loop.drop_unrecorded_phase_bias`; no new key. Their output changes only while the rule acts.
2. **Decoupling edge.** Each Drop names its cause (first sighting before any record, record lost while in view, back
   in view on a stale record), the age of the satellite's last record of the missing kind (or none), and whether the
   satellite was the reference satellite of its constellation at the previous epoch. Counters: drops by cause,
   reference satellites lost, and drops by record age (none, up to 60 s, up to 5 min, up to 30 min, longer).
3. **Re-coupling edge.** The Readmit is reborn loose as before (its ambiguity and ionosphere states were pruned while
   it was out and are seeded afresh). Its phase-bias and code-bias flip memories are set to the returned state, so the
   return makes no second reset. Event `readmit_unrec` in the engine log and the consumer satellite trace, with the gap
   in seconds; counters by gap (up to 1 min, up to 5 min, up to 30 min, longer).
4. **Too few satellites left (order item 1).** When fewer satellites keep their records than the solvable floor
   `estimation.defer_until_corrections_ready.min_satellites` (default 5; the floor the corrections-ready gate already
   owns), the epoch has no corrected solution: every satellite is left out of that epoch's update, the filter
   predicts only, the epoch is counted, both edges are events (begin and end, with the number of satellites that
   kept their records), and the closed-loop trace marks the epoch (`ar_gate` = `no_corrected_solution`). Every
   ambiguity and ionosphere state is then pruned as for satellites out of view and is reborn at the stream's return.
   Why not a solution on the a priori products: an uncorrected satellite carries metre-level terms (the clock datum,
   the missing code and phase biases) that the user's variance model has no size for, so a covariance that "says so"
   would be a guess; a predicted state with its own covariance is honest, and a static or slowly moving receiver keeps
   its last corrected position. Per-constellation floors are not added (the global floor is the corrections-ready
   gate's own).
5. **Snapshot.** The RT state snapshot carries the dropped set with the drop epochs and the satellites seen with their
   records (a new record `U`, written only when the rule has content), so a warm-resumed user does not readmit a
   satellite silently. The open no-solution edge is not carried (the first epoch after a resume decides it again).
6. **The complete reset (order item 5)** stays a named follow-up; it is traced, not changed (see the readings).

**Readings and bars.** Engine: a renamed Release copy of this branch with its runtime DLLs beside it; base for the
identity readings: `gates\exe\x64-Release\gnss_processor_int1122.exe` (sha256[:16] a0fadbb1564612b9, built from
`1bb4964`, whose engine code equals `33c5fef`). A 10-epoch smoke first. Gate label `ustale`, priority 6.

- **R1, three injected events on the simulator** (order item 1 adds E3). Stream `runs_irm/_rap_n20_s_6/products.txt`;
  user SZZ000SIM replayed on it (`_rap_n20_s_6` as base, as `runs_varrate/_rap_vr0_s_U`). E1: one Galileo
  satellite's C, B, P and U records removed for 10 min near 3 h into the replay, the satellite chosen from the
  unedited replay's trace as the highest Galileo satellite holding integers then. E2: one satellite's records removed
  while it is out of the user's view and for 2 min after it returns (the E25 pattern). E3: the whole stream withheld
  for 10 min near 3 h (every line from the first epoch line of the window to the last record before its end). Arms:
  each edited stream with the key off and on; the unedited stream with the key off (control) and on (identity); every
  arm also with the code-bias gap response armed (`full`; order item 3). Bars, key on: E1 and E2 one Drop at the first
  epoch past the record's window with its cause, no row of the satellite in the window, one Readmit with a fresh
  ambiguity variance, no complete reset, every other hold kept, the 3D error within 5 mm of the control from 10 min
  after the return, 0 wrong held integers against the fixture's truth; E3 the no-solution edges at the window's
  first epoch past 30 s and at the return, no epoch in the window or the 30 min after it with a 3D error above both
  5 cm and 3 times its 3D sigma (key off fails it: 5.3 m with a 0.79 m sigma past the end of the stream). Key on,
  unedited: identical to the control in every trace. Each bar is read with and without `full`.
- **R2, no harm where nothing happens (order item 4).** Key on against key off, same engine: the built-in users of the
  two reference files (`golden_field_refound`, `golden_sim_refound`), 2 h network runs: identical products and traces.
  The 51 receiver-days of `runs_irm/_rap_n20_<doy>_<site>` replayed with the key on and off: the only differences sit
  at the G01 event of DOY 132 at ARHT00ATA and DAV100ATA, each read.
- **R3, identity off.** This branch's engine with the keys off against the base engine: the 2 h built-in users of R2
  and the real-time seam gate (`scripts/rt_seam_gate.ps1`, four arms, `scripts/rt_seam_compare.py`) identical.
- **R4, the two field cases.** `_rap_vr0b_129_ascg_H300` and `_rap_vr0_129_ascg_P300` replayed with the key on, with
  and without `full`, against the key off on the same engine: no complete reset at 268320 and 261930 s, E25 and E24
  out until their records arrive; the error within 5 cm of the unheld control from 10 min after each return.
- **R5, folded runs of card IRDU section 15.1 (order item 2).** S1 (key off identical to the previous build) is R3. S2
  (key on where every used signal has its record: identical) is R2. S3 (the made case on the retired holds stream
  `_rap_irdu_gpb_r3c`) is dropped as covered by E1 and E2 on the reference simulator stream. O1 (observed data, the
  phase-bias rule against filtered observation files at HARB00ZAF, DAV100ATA, SCRZ00BOL on streams of a retired
  posture and another executable) is replaced by the frozen-lineage DOY 129 users at the same three receivers
  (`runs_irm/_rap_n20c_129_{harb,dav1,scrz}`), whose stream lacks the Galileo L1C/L5Q phase biases (class B 1,369,
  1,446 and 1,715 satellite-epochs at step 0): key on against off; bar: the rule's satellite-epochs left out for a
  missing phase bias equal the class-B satellite-epochs `count_unrecorded.py` counts on the key-off run, or every
  difference is explained; the filtered-file equality is dropped. O2 (the minute from which the 3D error stays below
  10 cm at HARB00ZAF and DAV100ATA) is read on the same pairs.
- **R6, the complete reset traced (order item 5).** The RT state snapshot (keys, values, covariance) of
  `_rap_vr0_129_ascg_P300`, key off, from short replays ending at 261900, 261930 and 261960 s: which states hold a
  variance of zero after the reset and with what value; repeated on an injected event if one of its key-off arms
  resets. Written into this card as a defect that outlives the rule.
- **Tests and gates (order item 6).** Unit tests in files of their own (the gate's causes, ages, gaps and floor; the
  snapshot's `U` record), the full Debug suite, the real-time seam gate, the ledger row of the key in the same commit.

**2026-11-22, the events fixed before any run (an addition of this session).** The code is commit `f8e1384`. The
satellites were chosen from the existing unedited replay of the same stream and user, `runs_varrate/_rap_vr0_s_U`
(replay from 262890 s; this card's own control does not exist before the runs): at 3 h (273690 s, plus or minus 5 min)
E05 (77.6 degrees) is the Galileo reference satellite and E25 (73.7 degrees) is the highest Galileo satellite holding
integers. E1 as registered takes E25; **E1b is added** with the reference satellite E05, so that the edge "reference
satellite lost" is driven (the registered text assumed the two would coincide). E2 uses the simulator's own E25
pattern: the stream carries no E25 record from 264930 to 268320 s, the user sees E25 from 264150 s, loses it at
264930 s and has it back at 268320 s; E2 removes E25's records over 268320 to 268440 s (4 epochs), so E25 returns on its
3390 s old record. Streams in `D:\GnssProcessorSupport\gates\user_stale_corrections\streams\` (`scripts\edit_stream.py`):
`s_e1a_E25.txt` and `s_e1b_E05.txt` (C, B, P, U records of the satellite removed at 273690 to 274290 s, 20 epochs),
`s_e2_E25ret.txt`, `s_e3_all.txt` (every line of the 20 epochs at 273690 to 274290 s removed, epoch lines included).
The replays run 6 h like `_rap_vr0_s_U`; epochs after 282630 s (the end of the stream) are not graded.

**2026-11-23, the held rule of the readings, fixed before any run is read (the managing session's note of
2026-11-22).** A held integer is read from the ambiguity variances of the phase rows of `closed_loop_sat_trace.csv`
in one of two forms, chosen per run by `network.closed_loop.consumer_undifferenced` in its `arm.json`:

- Differenced user (every user of R1, R2 and R4, and the step 0 cases): a row is held when it sits at its group's
  (constellation and signal) smallest positive variance, that value shared by at least two satellites and below
  0.01 m^2, or when its variance is above zero and at most 1e-6 m^2 (a variance of its own at hold level). At these
  users a held row first shares the group's common variance beside the reference rows at 0, then falls under
  1e-6 m^2 with a variance of its own, which the shared-minimum form misses (the managing session's check,
  `runs_nom/_irdu_grades/reads_2026-11-22/brp_tight_rules_check.txt`).
- Undifferenced user (the frozen-lineage users of R5, which do not fix): the second form only; the shared-minimum form
  reads a float group with one common term as held.

Hold losses are read against the control at the same epochs. A limit of both forms: at an epoch whose reference
satellite differs from the previous epoch's, the group's rows carry the reference's variance as a common term
(2.8e-2 m^2 in the control of case 1, whose Galileo reference alternates between E24 and E25 from one epoch to the
next), and neither form reads them as held; a loss read at such an epoch is checked in the rows before it is
reported. Code: `held_flags(..., form=)` and `user_form` in `scripts\count_unrecorded.py` of the gate folder;
recount `scripts\recount_held_two_forms.py`, outputs `out\recount_held_two_forms.txt` and
`out\recount_held_two_forms_extended.txt`.

Step 0's large cases under this rule (section 6 stands as written): case 2 unchanged (no hold on either side, hour 1).
Case 1 unchanged at the run (14 held, 0 from the epoch after the entry, back to 14 at 2,520 s after it under both
readings; the "8 epochs" of section 6 is the counter's window, from the entry to two epochs after the episode's end),
while the control reads 8 to 14 over the first 10 min under both readings (the alternating Galileo reference), not
the 14 to 16 written. E02 on DOY 132 changes: the run's lowest is 10 of 18 (written: 4), equal to the control's lowest
at the same epochs, with 15 held from 180 s after the entry against a control alternating between 10 and 18; the run
is below the control at 54 of 241 epochs to 2 h after the entry (80 under the step 0 reading).

**2026-11-23, order item 6 under the run-queue rule of 2026-11-22 (managing session).** On the branch: the unit suites
of the code the branch touches plus the contract suites that guard its seams, with the gtest filter
`UnrecordedSatGate*:RtStateSnapshot*:ConsumerSatTraceSchemaTest*:ConsumerTraceSchema*:ConsumerDiscCarryTest*:ArFixQuality*:CodeBiasGapTest*:RealtimeDefectTest*:RealtimeReactionRecords*:RealtimeReactionGeneration*:RealtimeReactionLevelEphemeris*:RealtimeEphStalenessTest*:RealtimeSsrFreshnessTest*:RealtimeSsrIodeGateTest*:SsrClockStaleHoldTest*:SsrConsumerSeamTest*:SsrIngestFilterTest*:InnovationScreen*:ConfigAskedVsInEffect*:ConfigMemberDump*:NetworkRunTracesOpenSet*:NetworkEpochContract*`,
the real-time seam gate and the ledger row. The full Debug suite runs once in the integration round on the merged
tree.
