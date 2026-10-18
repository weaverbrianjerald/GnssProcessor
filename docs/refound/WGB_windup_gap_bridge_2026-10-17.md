# WGB: the phase wind-up accumulator through short data gaps

**STATUS**

- **id:** WGB
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-10-17, default off, UNVALIDATED, not armed. `estimation.phase_windup_gap_bridge` carries the accumulated wind-up angle across a data gap of 300 s or more, up to 900 s, by walking it through virtual epochs at most 30 s apart from the orbit products and unwrapping each step, so the accumulator does not restart and the stepped arcs are not refloated. The walk is refused, and the restart happens as before, in the Earth's umbra, in a GPS noon-turn window, at a quarter-cycle half step and when the walk ends a quarter of a cycle or more from the epoch's own angle. The simulator injects wind-up through the same function, so no simulated data can grade it. Build stage passed (section 7): knob off identical to a main control (10-epoch smoke and a 0.5 h arm with 135 real restarts), the bridge reproduces a continuous accumulation exactly in a single-receiver gap run (16 of 16 satellites, 2 of which a restart steps by a cycle), 135 of 135 gaps bridged in the network gap arm. CHECKED 2026-10-18 (section 9): knob off identical to main on 23 G5 arms, 16 user-side arms and 4 real-time arms except the declared Toggles line (`, wu_bridge=off`); field golden 6 h, knob on against off on the same exe: restarts 139 to 91, arcs refloated 116 to 23, 48 of 61 in-range gaps bridged and 13 refused in the umbra (one G20 event), held-out consumer within 0.3 mm in every hour, 16-site census median 0.0 mm with no site beyond 0.2 mm; bars G0 to G3 hold, no benefit shown, still UNVALIDATED and not armed.
- **source:** the queued M-PAGES build 4 ([OPEN_ITEMS](../status/OPEN_ITEMS.md), "Queued after NOM"); card [WUP](WUP_windup_accumulated_promotion_2026-10-01.md) (the 28 in-horizon restarts per 6 h and their refloat); card [NTA](NTA_noon_turn_attitude_2026-10-16.md) (the noon-turn model the guard uses); `docs/archive/SPEC_phase_windup_sign.md` section 10
- **knobs / posture:** `estimation.phase_windup_gap_bridge` (ledger row UNVALIDATED, default false), `estimation.phase_windup_bridge_max_gap_s` (900) and `estimation.phase_windup_bridge_step_s` (30), both PARAM rows; both goldens leave the bridge off
- **dates in the text:** 2026-10-17, 2026-10-18
- **hashes / run dirs:** branch `mpg/windupgap` from main `30ca49e`; control exe `gnss_processor_windupgap_ctl.exe` (main `30ca49e`, built in worktree `windupgap_ctl`); candidate exe `gnss_processor_windupgap.exe` (sha256[:16] `9006af796b89e4e1`, the exe of the commit); arms `D:/GnssProcessorSupport/runs_nom/_rap_wgb_{s,g}_{ctl,off,on}`; single-receiver gap runs and scripts `D:/GnssProcessorSupport/gates/windupgap/` (`ppp_final/`, `ppp_compare.py`, `make_hole.py`, `mk_ppp_cfg.py`); check (section 9): control side `w3base` (main `30ca49e`, exe `gnss_processor_w3base.exe`, sha256[:16] `50ca2ea5fbbc540c`), G5 `D:/GnssProcessorSupport/gates/g5/gate_w3windupgap/`, 6 h arms `D:/GnssProcessorSupport/runs_nom/_rap_wgbchk_{ctl,on}_f`, census `_rap_wgbc_{ctl,on}_<site>`, scripts and logs `D:/GnssProcessorSupport/gates/windupgap_chk/`

**Outcome.** Question: can the wind-up accumulator stay continuous across a gap inside the leveling-carry horizon, so that a restart (an exact integer-cycle step on every signal of the satellite) and the refloat that answers it are avoided? Built as a default-off walk through virtual epochs with refusal guards. Unit gate passes; knob off is identical to a main control in a 10-epoch smoke and in a 0.5 h arm with a 660 s outage of 11 satellites (135 restarts, 242 arcs refloated); with the knob on the same arm bridges all 135 gaps with no restart and no refloat. A single-receiver gap run shows the bridged angle equal to the continuous one for all 16 returning satellites, where the restart is off by a cycle for 2. No simulated bed can grade it. Check (section 9, 2026-10-18): the bars of section 6 hold. On the field golden for 6 h the bridge keeps 48 of 61 in-range restarts (139 to 91; arcs refloated 116 to 23), refusing 13 in the umbra, and moves neither the held-out consumer (0.3 mm at most in any hour) nor the 16-site census (median 0.0 mm). Standing: UNVALIDATED, not armed; no benefit shown.

## 1. What was built

| piece | where | note |
| --- | --- | --- |
| the walk | `WindUpBridge::walk` (`include/corrections/WindUpGapBridge.hpp`, `src/core/corrections/WindUpGapBridge.cpp`) | pure arithmetic on a sampler; outcomes, counters, named events |
| raw angle, one owner | `GnssCorrections::windUpGeometry` | the Wu et al. (1993) dipoles and sign rule moved out of `computePhaseWindUp` unchanged, so the epoch and the virtual epochs share one routine |
| the accumulator branch | `GnssCorrections::computePhaseWindUp`, new trailing argument `gap_bridge` (null by default) | tried only for a gap from `max_gap_s` up to the bridge horizon; a refused walk falls through to the restart |
| satellite state at a virtual epoch | `Geometry::getLookBackState` (`ModelGeometry.cpp`) | the orbit product (SP3, else the live broadcast store with its SSR overlay), one light-time iteration, Earth rotation, the Sun from the model's own `Planetary`; leaves the Sun cache and the APC-overlay record untouched |
| noon-turn predicate | `SatAttitude::inNoonTurnWindow` | the Block IIF turn window (it contains the IIR window), widened by one step of orbit motion; GPS only |
| wiring | `PppModel` constructor installs the provider; `Antenna::getAntennaCorrection` builds the options from the config | nothing is installed or built with the knob off |
| knobs | `estimation.phase_windup_gap_bridge`, `phase_windup_bridge_max_gap_s`, `phase_windup_bridge_step_s` | getters `phaseWindUpGapBridge()`, `phaseWindUpBridgeMaxGapS()`, `phaseWindUpBridgeStepS()`; the toggles log line gains `wu_bridge=` |
| telemetry | `WindUpBridge::Counters` and `WindUpBridge::Event` (the first 512 attempts of a receiver, named by satellite, epoch, gap, outcome and kept cycles); network run-end lines in `networkSlipWindupMwRunEnd` (the counter sum, then the first 100 attempts by station), PPP lines in `reportWindUpCensus_` | printed only with the knob on; the PPP lines go to the `ppp_model` logger, which needs `logging.enable_ppp_model` |
| tests | `tests/test_windup_gap_bridge.cpp` | suites `WindUpBridgeWalk`, `WindUpBridgeCorrections`, `NoonTurnWindowPredicate`, `WindUpBridgeConfig` |

No file of the network producer other than the run-end report is touched, and `MeasurementModel*`, `StateInitializer*` and `StateManager*` are not.

## 2. The walk

The accumulator holds, per satellite, the angle A of the last processed epoch (at time t_prev). At the current epoch t_now the raw `atan2` angle r is known from the epoch's own geometry. A restart sets A to r, which differs from the continuous angle by a whole number of cycles N. The walk finds N.

1. The gap is cut into n = ceil(gap / step) equal steps of at most `phase_windup_bridge_step_s` (30 s).
2. At each virtual epoch the satellite state comes from the provider (orbit product, receiver where it is now) and the raw angle from `windUpGeometry`, the routine the epoch uses.
3. Each step is evaluated at its middle too, and each half step is wrapped to half a cycle either way. A half step of a quarter cycle or more refuses the walk (`StepTooLarge`). A wrapped step cannot be told from its alias, so the limit is half of the half-cycle that the unwrap resolves: a true half step of up to three quarters of a cycle still shows, where a sum test alone would let a half step of 0.55 cycle (wrapped to -0.45) cancel against the next.
4. The sum of the steps is added to A. N is the integer nearest to (A + sum - r) / 2 pi, and the bridged angle is r + 2 pi N. The fraction is therefore always the epoch's own, and an error of the virtual geometry matters only if it moves the sum by a quarter of a cycle or more, which refuses the walk (closure).
5. A refused walk restarts exactly as before: the ledger entry, the census count and the refloat response are those of today.

Refusals, with the counter that records each: no satellite state or a degenerate geometry at a virtual epoch (`refused_no_state`, also a missing provider), the satellite in the Earth's umbra (`refused_umbra`, `Planetary::isSatelliteEclipsing` with `processing.eclipse_shadow_scale`), a GPS noon-turn window (`refused_noon_turn`), a quarter-cycle half step (`refused_step`), closure (`refused_closure`). Gaps at or beyond the horizon are counted as `beyond_horizon` and restart as before.

Residual risks, stated. The closure test sees only a fractional disagreement, so a whole-cycle error inside one half step is invisible to it; only a half step of three quarters of a cycle or more can hide (a rate three times the half-cycle limit, reached only by a pass within about 0.02 degree of the receiver's zenith, where the receiver dipole turns with the azimuth). Midnight and post-shadow recovery manoeuvres are not guarded beyond the umbra test, as in the exclusion of eclipsing satellites; yaw manoeuvres of Galileo and the other blocks are not modelled, and only the half-step test would catch one. Receiver motion is not modelled: the receiver is taken where it is now, which is exact for the static stations of the network and a sub-millimetre error in the integer for a user moving a kilometre in 900 s.

Both edges (standing invariants 1 and 7). The decoupling edge is a refused walk: it is the restart of today, with its response, and each reason is counted, with the identity (station, satellite, epoch, gap) in the named events. The re-coupling edge is a bridged gap: the model stays continuous, so the carried ambiguity and leveling level stay valid across the gap by construction; the bridged and stepped counts (`bridged_stepped`: the gaps over which a restart would have stepped the model by a nonzero whole number of cycles) name the events that matter, and the census shows the model step across the bridged gaps (section 7).

## 3. What the simulator cannot say

The simulator forms its truth wind-up with `computePhaseWindUp` in accumulated mode (`EphemerisProvider::phaseWindupMetres`, the default 300 s restart horizon), with no provider and no bridge: its restarts and its own gap behaviour are the function's, and the processor's model matches them by construction. A sim arm with the bridge on makes the processor bridge a gap that the truth restarted, which is a mismatch the option creates, not a defect it removes. The sim bed therefore cannot grade the option; the unit gate checks the arithmetic against a continuous accumulator, the single-receiver gap run checks the real orbit path, and the field is the only bed for the effect on the solution.

## 4. Unit gate

`tests/test_windup_gap_bridge.cpp`, 24 cases in four suites (section 5). The reference of the integration cases is the accumulator fed every epoch with no gap; seven Sun phases give a restart that steps the model by a whole number of cycles every time, and the bridged value reproduces the continuous one to 1e-9 m with an empty restart ledger and a named event. Also covered: the same epoch's second call reuses the bridged value, a missing or failing provider refuses for want of a state, a gap shorter than the restart threshold and a gap at the horizon are untouched, an umbra at a virtual epoch and a walk that disagrees with the epoch's own angle refuse and restart, the half-step refusal at 0.6 cycle per step (0.3 per half step) and the pass at 0.45, a large half step hidden in the sum, the step count of a gap that continuous seconds carry 2e-7 s above a whole number of steps, the noon-turn predicate (window, margin, non-GPS, degenerate), and the loader (defaults, interlock with accumulated wind-up, unusable limits). `Geometry::getLookBackState` needs orbit products and has no unit case: the single-receiver gap run of section 7 covers it.

## 5. Build

Worktree `D:/GnssProcessorSupport/worktrees/windupgap`; Debug and Release built with `scripts/wt_build.ps1`. Debug `gnss_tests.exe`: section 7. `python scripts/nom_epoch_lint.py` and `python scripts/nom_n4_verify.py --whole` pass; `python scripts/make_golden_config.py --verify`, `python scripts/config_posture_check.py --verify-loader` and `python scripts/docs_index_check.py` pass with the three ledger rows.

Declared change when off: the Toggles log line gains `, wu_bridge=off`. Nothing else moves: no counter, no run-end line and no file is written without the knob. The extraction of the dipole routine from `computePhaseWindUp` changes no output (section 7).

## 6. The check, written before any 6 h arm ran

Owned by a separate stage. Field golden 6 h (`scripts/rapid_arm.py`: a golden copy with only the new knob and the run paths changed), against a same-exe control (`gnss_processor_windupgap.exe` with the knob off), both with `estimation.trace_windup_census`; grading per CLAUDE.md (held-out consumer by hour, hour 1 ignition, settled hours 2 to 6; the 16-site census with `gcensus.py`).

- **G0 (the off path).** Knob off, the field and sim golden 2 h arms are identical to the main control exe `gnss_processor_windupgap_ctl.exe` in every product, trace and log apart from the declared toggles line and wall-clock fields. Section 7 holds the first sign.
- **G1 (the on path).** The run-end line reports attempts equal to bridged plus refused, and attempts equal to the gaps the census counts between the restart horizon and the bridge horizon (`attributed ... between the horizons` when the carry horizon is the later). The arm's restarts (census) fall by the bridged count, its restart events and refloated arcs in the response line fall with them. WUP counted 28 in-horizon restarts per 6 h, of which about 8 held a carry that the restart's refloat alone ended: those are the events the option is meant to keep. A bridged count of 0 on an armed run with in-range gaps is dead wiring. Read the refused counts by reason: umbra and noon turn refusals on a golden window are expected, a closure or step refusal is not.
- **G2 (the consumer).** The held-out consumer WTZZ00DEU by hour against the control: no hour worse by more than 0.01 m (the bar of card WUP).
- **G3 (the census).** Per site, the change of the mean hourly p50 over hours 2 to 6: median change at most +0.005 m and at most 3 sites worse by more than 0.02 m (the bar of card WUP), on the arm's own wire replayed with accumulating consumers.

What follows. G0 fails: the refactor of `computePhaseWindUp` or the new argument moved a shared path. G1 fails: the option does not reach the gaps, back to the build. G2 or G3 fails: recorded, the knob stays off. G2 and G3 pass: the arming decision is the author's, through a ledger row and the generator.

## 7. Results of the build stage (2026-10-18)

Control: main `30ca49e` built in a second worktree with the same wrapper; candidate: branch `mpg/windupgap`. Both Release, run from `D:/GnssProcessorSupport/gates/exe/x64-Release/`. Comparisons by `scripts/compare_network_run.py selfcheck` and `cmp`. The gap arm and the single-receiver runs ran on the build one commit-edit earlier (sha256[:16] `c41055116d4f64ab`), which differs from the committed one only in the text of two loader warnings; the 10-epoch smoke was repeated on the committed exe.

**G0, 10-epoch smoke** (field golden, 58 stations, `estimation.trace_windup_census` on; `_rap_wgb_s_{ctl,off,on}`). Control against candidate with the knob off: `products.txt`, `closed_loop_trace.csv`, `network_epoch_status.csv`, `network_solution.csv`, `network_events.csv`, `network_amb_state.csv` and `network_trace.csv` are byte-identical, and every other file differs only in a path, a tag, a wall time or the `ms` column of `network_datum_graph.csv`. In the log, a sorted line-by-line comparison finds the Toggles line (`wu_bridge=off` added) and timing lines only. Knob on against off: the same, no gap in 10 epochs; the run-end line reads 0 gaps at 0 stations.

**G0 and G1, gap arm** (`_rap_wgb_g_*`: the field golden for 0.5 h with the observations of the 11 Block IIF satellites removed from 00:10:00 to 00:20:00 at all 58 stations and the consumer, so that every returning arc has a gap of 660 s; a synthetic outage that exercises the path, not a field event).

| | control and knob off (identical) | knob on |
| --- | --- | --- |
| gaps in [300, 900) s (census, out-of-horizon transitions) | 135 | 135 |
| accumulator restarts (census) | 135 | 0 |
| transitions stepped by 0.9 cycle or more across the gap | 1 (max 0.969) | 0 (max 0.031) |
| restart events drained by the response | 135: 124 responded, 242 arcs refloated, 11 with no arc | 0 |
| bridged / refused | | 135 / 0 (1 with a nonzero whole-cycle count) |
| virtual epochs evaluated | | 6091 |

Control against knob off with 135 real restarts and the response acting: identical in every product and trace (the same short list of path, tag, wall-time and `ms` differences). Knob on against knob off: the products differ, as they must, since 242 refloats are not taken; this 0.5 h arm is not graded (it is inside ignition and the outage is synthetic).

**Single-receiver gap run** (`ppp_final/`; CODE final products, GCGO 2024-129, GPS and Galileo, 240 epochs of 30 s, accumulated wind-up). A copy of the file without epochs 100 to 119 (a gap of 630 s) is run with the bridge off and on, and the continuous file is the reference. 16 satellites return after the hole. With the bridge off, the accumulated angle after the gap differs from the continuous run by a whole number of cycles for 16 of 16 (zero for 14, one cycle for E13 and E14: the model steps by a cycle, which the network response would refloat). With the bridge on it equals the continuous value for 16 of 16, to the 6 decimals of `windup_trace.csv`, and at every later epoch to the end of the run (worst difference 0). The log lines name the attempts (45 virtual epochs each, E13 and E14 with one whole cycle kept).

**Unit suite.** Debug `gnss_tests.exe` at the last code change but one: 3362 cases in 453 suites, 3332 passed, 30 skipped (the fixture-gated skips), exit 0 (the 24 new cases pass; an earlier build of the branch gave 3360 and 3330 before the two added cases). The last edit changed the text of two loader warnings; the loader cases were rerun on it.

## 8. Owed

- The 6 h field arm and the census of section 6.
- The warm-resume re-derivation of the accumulator (a resumed run seeds each satellite from its raw angle; the walk could re-derive the integer from the snapshot epoch). Not built.
- A satellite-state provider test with orbit products in the unit suite.
- The author's arming decision; none is made here.

## 9. Check (2026-10-18)

Independent stage on `5a66773` of branch `mpg/windupgap`; no source file was edited. Candidate exe `gnss_processor_windupgap.exe` (sha256[:16] `9006af796b89e4e1`; an incremental Release build of the commit reported nothing to do), copied as `gnss_processor_w3windupgap.exe` for the G5 gate. Control side `w3base`: main `30ca49e`, exe `gnss_processor_w3base.exe` (sha256[:16] `50ca2ea5fbbc540c`). Other sessions kept the box at full CPU throughout, so the wall times below are not timings.

### 9.1 G0, the off path

One difference is declared: the Toggles log line ends with `, wu_bridge=off`. A raw comparison therefore reads DIFFERENT, and each was repeated with that line dropped from both sides (`NOM_GATE_DROP_RE='\[gnss_config\] \[info\] Toggles:'`). In every arm of every gate the candidate's Toggles line equals the control's with the suffix removed (scripted, line by line).

| gate | arms | raw | declared line dropped |
| --- | --- | --- | --- |
| G5 (`gate_step.sh`, reference `w3base`; field and sim golden 2 h arms `f2`, `s2` and 21 others) | 23 | 23 DIFFERENT, each in one line of `stdout.txt` and nowhere else: products, every CSV trace, manifest, main log rotation set, `rt_engine.log` identical; exit codes equal to the reference on all 23 (`zero`, `joint`, `a4`, `gate` exit 1 on both sides) | 23 of 23 `RESULT IDENTICAL`, `snapw` snapshot bytes identical: 24 identical lines |
| user side (`uom_gate.py cmp w3base w3windupgap`, quick set, exit 0 on all) | 16 | 0 of 16: `logs/gnss_config.log` and `stdout.txt`, one line each (32 files) | 16 of 16 IDENTICAL |
| real time, dr set (`sbf`, `sbf_bssd`, `sbf_te`) | 3 | the Toggles line in `console.log` and `gnss_config.log`, and the session wall-time line of `console.log`; 93 comparator lines IDENTICAL | none other |
| real time, `cl` arm | 1 | the Toggles line, and wall-clock figures only (RINEX ingestion seconds, epoch-loop wall, round-trip self-check seconds, build and audit milliseconds of the arc ledger) in `console.log` and `main_log.log`; 36 comparator lines IDENTICAL | none other |

G0 holds: knob off moves nothing but the declared line. Records: `D:/GnssProcessorSupport/gates/g5/gate_w3windupgap/` (raw), `gate_w3windupgap_droptoggles/` (dropped), `gates/g7/summary_w3windupgap*.txt`, `C:/GnssProcessor/runs/_uom_w3windupgap`, `C:/GnssProcessor/runs/rtseam_goldens/w3windupgap_{dr,cl}`, logs and scripts in `D:/GnssProcessorSupport/gates/windupgap_chk/`.

### 9.2 The 6 h field arms (G1)

Arms `_rap_wgbchk_{ctl,on}_f` under `D:/GnssProcessorSupport/runs_nom`: the field golden for 6 h with `estimation.trace_windup_census` true in both, and `estimation.phase_windup_gap_bridge` true in the second only (the other two keys at their defaults, 900 and 30). Same exe, made with the worktree `rapid_arm.py` so that the loader check knows the new key. Both exit 0 with 720 epochs emitted (7112 s and 7100 s). The control reproduces the consumer p50 by hour of the baseline run `_rf0hv15_field6` to five digits.

| | control | bridge |
| --- | --- | --- |
| forward transitions, in horizon | 600755, 600616 | the same |
| out-of-horizon transitions (between the horizons, beyond both) | 139 (61, 78) | 139 (61, 78) |
| accumulator restarts | 139 | 91 |
| transitions stepped by 0.9 cycle or more (max) | 2 (0.994) | 0 (0.832) |
| restart events responded | 60 | 12 |
| arcs refloated | 116 | 23 |
| inside the carry horizon with no arc to free / beyond the carry horizon | 1 / 78 | 1 / 78 |
| bridge attempts (stations) | | 61 (33) |
| bridged / refused | | 48 / 13 (umbra 13; no state, noon turn, step and closure 0) |
| bridged over a nonzero whole-cycle count | | 2 |
| virtual epochs evaluated | | 1869 |

The attempts (61) equal the gaps the census counts between the two horizons (61), and attempts equal bridged plus refused. The restarts fall by the bridged count (139 to 91, minus 48), as do the restart events answered (60 to 12); the arcs refloated fall from 116 to 23. By hour of the run the control's answered restarts are 4, 16, 8, 4, 11 and 17; the bridge arm keeps 12, all in hour 2.

What was kept: 28 of the 48 bridged gaps are the Galileo satellites E14 (13, at WARK00NZL and SCTB00ATA, gaps of 300 s to 870 s recurring every 900 s) and E18 (15, at SCTB00ATA, GOPE00CZE and BOR100POL); the rest are 20 GPS and Galileo gaps at 10 stations. Two of them kept a whole cycle (-1): SGOC00LKA G14, gap 570 s at TOW 278100, and KIT300UZB G22, gap 480 s at TOW 279690; they are the two transitions the control stepped by 0.9 cycle or more. The 13 refusals are one event: G20 at TOW 262920 (01:02 UT), a gap of 720 s at 13 stations with the satellite in the umbra at a virtual epoch; it restarts as before and is the 12 answered restarts left in the bridge arm. The expectation of card WUP (28 in-horizon restarts per 6 h, about 8 of them holding a carry) was a GPS-only count; with Galileo armed the golden has 61 restarts between the horizons and 60 answered, and the bridge keeps 48 of the 61.

### 9.3 Consumer and census (G2, G3)

Held-out consumer WTZZ00DEU, 3D error, p50 by hour 1 to 6:

| | h1 | h2 | h3 | h4 | h5 | h6 | mean h2 to h6 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| control | 0.0581 | 0.0113 | 0.0183 | 0.0103 | 0.0155 | 0.0105 | 0.0132 |
| bridge | 0.0581 | 0.0112 | 0.0186 | 0.0103 | 0.0155 | 0.0105 | 0.0132 |
| change (m) | +0.0000 | -0.0001 | +0.0003 | +0.0000 | +0.0000 | -0.0000 | +0.0000 |

688 of 720 epochs below a decimetre in both arms. Epoch by epoch the two 3D errors differ by 0.3 mm at most at the median of an hour and 0.5 mm at most anywhere (hour 3). The mean and the p95 of an hour move by 0.1 mm or less except hour 3 (mean +0.3 mm, p95 +0.4 mm).

Census (`gcensus.py make wgbc_{ctl,on}`, 17 consumers each, replays of the arm's own wire with the arm's own consumer knobs, all exit 0; `grade wgbc_ctl wgbc_on`): paired change of the mean hourly p50 over hours 2 to 6, median -0.0000 m, 0 sites better and 0 worse at 1 cm; the largest single change is ARHT +0.2 mm, the next KOKB and CEDU at +0.1 mm and QUIN, HARB, DAV1 and BAKE at -0.1 mm. Hour 1 paired median +0.0000 m. The median site p50 by hour is 0.136 0.054 0.041 0.036 0.040 0.037 m for the control and 0.136 0.055 0.041 0.036 0.040 0.037 m with the bridge. Per-site rows: `D:/GnssProcessorSupport/gates/windupgap_chk/census_grade.txt`.

Network side, a datum-invariant check: the post-fit phase residual of the bridged (station, satellite) arcs over the 10 minutes after the gap is 1.910 mm rms in the control and 1.912 mm with the bridge (1093 rows, the same rows); the two arcs that kept a whole cycle read 1.92 mm and 1.92 mm (SGOC00LKA G14, 40 rows) and 0.02 mm and 0.19 mm (KIT300UZB G22, 18 rows). The G20 arcs, refused in both, are identical (1.062 mm).

### 9.4 Bars and standing

| bar | result |
| --- | --- |
| G0, off path | holds, apart from the declared Toggles line (9.1) |
| G1, on path | holds: attempts equal bridged plus refused and equal the census gaps between the horizons; restarts 139 to 91; arcs refloated 116 to 23; the only refusals are the umbra ones of one event |
| G2, consumer | holds: no hour worse by more than 0.3 mm (bar 10 mm) |
| G3, census | holds: median change 0.0 mm (bar +5 mm), no site worse by 2 cm (bar 3 sites) |

The option keeps the leveling carry across 48 of the 61 in-range gaps and moves neither the held-out consumer nor the census by more than 0.3 mm. No benefit is shown: the refloats it removes cost the consumer nothing measurable in these 6 h, and the removed events are mostly two recurring Galileo gaps. Standing: UNVALIDATED, not armed. The arming decision is the author's, through a ledger row and the generator.

Limits of this check. One day (DOY 129, 00 to 06 UT) of static stations. The closed-loop consumer shares the producer's wind-up model, so it measures the consistency of the pair, not the model against the sky. The whole-cycle count of the two nonzero bridged gaps has no external reference on this bed: the post-fit residual shows no excess, which a wrong integer would not be guaranteed to show, since the gap re-entry handling armed in the golden (`gap_reentry_relevel`) also acts on arcs that return after a gap. The simulator cannot grade the option (section 3). A day with GPS noon turns and Galileo manoeuvres inside a gap would exercise the refusals that this window did not (noon turn, step, closure: 0 events).
