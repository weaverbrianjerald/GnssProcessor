# ICL: the GPS-to-Galileo level of a broadcast-referenced network, held for receivers as configured

**STATUS**

- **id:** ICL
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered and closed 2026-11-03. Each constellation's phase-visible clock level is its clock pivot's a priori error (under the tracked a priori a random walk), passed to the user by the smoothed hold; the clock frame hold keeps it off the code side. A held frame on the emitted phase-visible clock (default off) cuts the GPS-to-Galileo level from 188 and 193 to 9 and 32 mm per hour; receivers as configured then do as free-offset receivers (paired -0.0002 and +0.0044 m). G0, G2, G3 pass; G1, G4 (DOY 129), G5 fail. Nothing armed.
- **source:** [BRD](BRD_broadcast_referenced_level_2026-11-02.md) sections 6.8 and 8 (the failure and the open item); [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (the hold whose passed motion carries the level); [LAH](LAH_level_aware_hold_2026-09-29.md) (the phase-clock hold of the goldens); [PFQ](PFQ_pivot_frame_process_noise_2026-10-08.md) (the pivot-frame process noise that refuses the sum-to-zero datum); `docs/SPEC_clock_emit_rebase.md` (the clock frame hold, `network.emit_clock_frame_hold`)
- **knobs / posture:** new, default off: `network.emit_phase_frame_hold` with `network.phase_frame_leash_m` (default 1.0 m); graded on card BRD's GPS + Galileo broadcast posture with both of its knobs (`network.cycle_slip.if_phase_td_sat_centre`, `network.apriori_clock_track`), receivers as the GPS + Galileo golden configures them (`filter.process_noise.system_time_offset_q` 1e-12)
- **dates in the text:** 2026-11-03
- **hashes / run dirs:** base exe `gnss_processor_icl0.exe` = card BRD's `gnss_processor_brd4.exe` (`8084428d737f95ed`, branch `brdc/independence` at `1e8a543`); `gnss_processor_icl1.exe` (`2cef22f4d967f078`, the frame), Release from the worktree `D:/GnssProcessorSupport/worktrees/icl`; arms `D:/GnssProcessorSupport/runs_nom/_rap_icl_*` (`icl_p{129,132}_6` graded, `icl_r{129,132}_6` and `icl_rp{129,132}_6` the regime on CODE without and with the frame, `icl_c129_2`, `icl_s_2` G0, `icl_cp129_2` G1); census replays `_rap_<key><doy>_<site>`, keys `iclp`, `iclr`, `iclrp`; measurement and grading scripts in `D:/GnssProcessorSupport/worktrees/icl_tools/` (outside the repository); card BRD's runs `_rap_brd_{c,ngb,ngt}{129,132}_6` and its census keys `brdc`, `brdngt`, `brdngtI` the references

**Outcome.** Card BRD's GPS + Galileo broadcast posture cost receivers with a frozen inter-system offset +0.14 m at the census. Each constellation's level is its clock pivot's a priori error (a random walk under the tracked a priori), which the smoothed hold passes to the phase side. A held frame on the emitted phase-visible clock (default off, outputs only) cuts the GPS-to-Galileo level from 188 and 193 to 9 and 32 mm per hour, the census to +0.011 and +0.017 m: receivers as configured then do as free-offset receivers. G4 (DOY 129) and G5 fail; nothing armed.

## 1. The question

Card BRD (section 6.8) found the GPS + Galileo broadcast-referenced posture failing at the user with the receivers as the golden
configures them (census +0.206 and +0.116 m with both knobs, +0.032 and +0.043 m without), and passing on DOY 129 once the receiver
re-estimates its GPS-to-Galileo offset every epoch (a receiver setting). It measured the receiver's offset moving 1.1 mm per epoch
on the golden, 3.1 mm on the broadcast posture and 5.7 mm with both knobs, and left open why the network's two constellation
levels move apart and where a network-side fix belongs. The question: what moves the GPS-to-Galileo level that a receiver sees,
and what is the smallest network-side mechanism that holds it, so that receivers configured as today work?

## 2. Measurement before building (2026-11-03)

Every statistic below is computed on card BRD's 6 h runs (exe `brd3`), hours 2 to 6, against the CODE final clock of the day
(one time scale for both constellations), with the scripts of `icl_tools/`. For each emitted epoch and satellite the delivered
clock error is e = full - COD (full = brdc(iode) + C_wire on the broadcast a priori, COD + C on the CODE a priori; code-visible)
and pv = e - P_IF (phase-visible, the ionosphere-free phase bias of the modelled pair). A constellation's level moves by the
median one-epoch increment over its satellites present at both epochs (pv: in the same phase-bias generation); D is the
cumulated GPS-to-Galileo difference. Pivot handover epochs (signalled datum changes, at which receivers re-inflate the offset)
are left out. Lags are in epochs of 30 s; the root mean square of D(t + lag) - D(t) is quoted.

### 2.1 The level a receiver sees (`icl_dcode.py`)

| day | arm | code-visible D, lags 1 / 10 / 40 / 120 (mm) | phase-visible D, lags 1 / 10 / 40 / 120 (mm) |
| --- | --- | --- | --- |
| 129 | GPS + Galileo golden (`brd_c129_6`) | 0.1 / 0.4 / 1.3 / 2.5 | 0.1 / 0.4 / 1.3 / 2.5 |
| 129 | broadcast posture (`brd_ngb129_6`) | 1.3 / 3.5 / 5.8 / 10.6 | 11.1 / 34.0 / 45.5 / 69.2 |
| 129 | both knobs (`brd_ngt129_6`) | 1.7 / 4.9 / 10.3 / 22.6 | 3.3 / 20.7 / 71.4 / 188.5 |
| 132 | GPS + Galileo golden (`brd_c132_6`) | 0.1 / 0.4 / 1.1 / 2.4 | 0.1 / 0.4 / 1.1 / 2.4 |
| 132 | broadcast posture (`brd_ngb132_6`) | 1.5 / 4.6 / 12.7 / 33.3 | 10.9 / 38.3 / 48.9 / 85.0 |
| 132 | both knobs (`brd_ngt132_6`) | 1.9 / 6.0 / 12.2 / 28.3 | 3.6 / 23.1 / 77.1 / 193.4 |

The closed-loop receiver's ISB (`logs/closed_loop_isb.csv`, WTZZ00DEU, handover epochs out, `icl_rxisb.py`) moves 0.80 and 0.85
mm per epoch on the golden, 3.15 and 3.84 mm on the broadcast posture, 4.20 and 3.60 mm with both knobs (DOY 129 and 132). Its
increments correlate with the network's dD at only +0.09 to +0.28: the receiver's frozen offset is pulled by the level's
accumulated motion, not epoch by epoch. Card BRD's 1.1, 3.1 and 5.7 mm (DOY 129) include the handover epochs.

### 2.2 Each constellation's phase-visible level is its pivot's a priori error (`icl_pivot.py`)

The S-basis pins one satellite clock per constellation every epoch (`NetworkSbasis::buildSharedDatum`), so the solve's level
of a constellation is its pivot's a priori error, a_p + U_p - COD_p (U the tracked a priori's offset, 0 without the knob; a + U
is `brdc_m + offset_m` of the broadcast-offset trace). Lag 1 / lag 120 (mm), the sticky pivots G02 and E02 (E03 for 112 epochs):

| day | arm | GPS level | G02's a priori error | difference | Galileo level | E02's a priori error | difference |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 129 | broadcast posture | 11.1 / 64.4 | 19.7 / 73.8 | 12.1 / 20.5 | 1.6 / 40.5 | 1.7 / 50.9 | 1.8 / 75.1 |
| 129 | both knobs | 2.8 / 88.9 | 4.4 / 85.0 | 2.8 / 7.5 | 2.1 / 167.1 | 3.0 / 182.9 | 2.7 / 49.2 |
| 132 | broadcast posture | 10.9 / 65.4 | 18.8 / 57.4 | 11.6 / 24.2 | 1.6 / 41.1 | 1.7 / 19.4 | 1.9 / 47.6 |
| 132 | both knobs | 3.1 / 132.5 | 4.7 / 124.1 | 3.0 / 11.6 | 1.8 / 25.2 | 2.7 / 118.9 | 2.9 / 102.3 |

Without the track the pivot's error is its broadcast misfit: G02 moves 19.7 and 18.8 mm per epoch around its polynomial, among
the noisiest GPS clocks of the day (median satellite 2.9 and 3.1 mm; G05 44.9, G12 31.0, G21 26.6 mm). With the track every
satellite's a priori a + U random-walks (median satellite 4.0 mm per epoch and 106 mm at lag 120 for GPS, 2.8 and 78 mm for
Galileo, DOY 129), because U sums the IF-TD column's per-epoch errors; for every other satellite the data correct the walk each
epoch, for the pivot nothing does, since its a priori is the datum. This answers card BRD's open point (why the tracked a priori
raises the jitter): the pivot's track carries the column's summed error into the whole constellation. The pivot's record
cut-overs are not the cause: the level-minus-pivot increments at G02's and E02's record switches sum to -2.6 and +1.0 mm (DOY
129, knobs) against -157 mm for Galileo between them.

### 2.3 The code side does not inherit it, the phase side does

The clock frame hold (`network.emit_clock_frame_hold`, armed in both goldens) removes each constellation's median innovation of
the emitted clock C every epoch (`NetworkClockEmitRebase`), so the code-visible level follows the median of the satellites' a
priori increments and the pin's motion cancels (section 2.1: 23 and 28 mm at lag 120 with both knobs). The phase bias is written
after it by the phase-bias hold. Under card LAH's phase-clock hold (the goldens) the phase-visible clock is the generation-start
value plus the constellation's common-removed emitted-clock step, so it follows the a priori, which on CODE is the truth. Under
card SMH's smoothed hold (the broadcast posture) the hold writes LAH's transfer minus the clone's passed motion
Phi = c(t|t) - c(t0|t), the solve's own clock motion in the solve's pinned frame. Per constellation the median of (dpv - dcv)
equals the frame hold's accumulator F = eps_p - median eps (`icl_rev.py`): with both knobs, lag 120, GPS 98.6 against 97.8 mm
(DOY 129) and 130.5 against 130.0 mm (DOY 132), Galileo 122.1 against 121.1 mm and 51.0 against 101.0 mm. What remains is the
clones' constellation-common revision (GPS 9.4 and 8.3 mm, Galileo 25.2 and 80.4 mm at lag 120).

### 2.4 The network's own station ISBs (`icl_isb.py`, `icl_isbfloor.py`)

The median over stations of the one-epoch change of the network's estimated Galileo ISB follows the phase-visible dD with slope
+1.00 and +1.01 (correlation +0.86 with both knobs, +0.99 on the broadcast posture; both days alike). It is not a usable
reference for the level: on the golden, where the delivered D stays within 2.5 mm over 120 epochs, the same median wanders 1.9
/ 8.9 / 27.9 / 66.0 mm (DOY 129) and 1.6 / 9.0 / 28.5 / 69.4 mm (DOY 132) over lags 1 to 120. The solve's own GPS-to-Galileo
phase level moves under every a priori; card LAH's hold discards it on the goldens.

### 2.5 The candidates, predicted offline (`icl_predict.py`, `icl_p2.py`, `icl_p2raw.py`)

The delivered phase-visible level under a different datum is the run's level with the pins' error replaced by the candidate's
own level-defining error. Phase-visible D, lag 1 / lag 120 (mm), the both-knobs runs:

| candidate | DOY 129 | DOY 132 |
| --- | --- | --- |
| as run (pivot pinned on the tracked a priori) | 3.3 / 188.5 | 3.6 / 193.4 |
| (a) each pivot left out of the track (raw broadcast a priori) | 19.5 / 63.0 | 19.2 / 99.4 |
| (a') the quietest satellite of each constellation as the untracked pivot (hindsight bound) | 4.3 / 53.0 | 4.7 / 44.2 |
| (c) time-linked sum to zero, median of the increments, tracked a priori | 4.0 / 39.6 | 4.4 / 77.2 |
| (c) the same on the raw broadcast a priori | 3.8 / 33.0 | 4.1 / 70.0 |
| (b) the station ISBs held at their window-start values | 2.6 / 35.6 | 6.3 / 50.3 |
| a held frame on the phase-visible clock, vote on the correction against a + U | 1.6 / 20.0 | 1.9 / 30.4 |
| the same, vote on the correction against the raw broadcast a priori a | **1.1 / 8.6** | **1.3 / 28.9** |
| (reference) the GPS + Galileo golden | 0.1 / 2.5 | 0.1 / 2.4 |

On the broadcast posture without knobs the held frame predicts 1.2 / 7.4 and 1.4 / 31.0 mm (from 11.1 / 69.2 and 10.9 / 85.0);
on the golden it is an exact no-op. Every datum candidate stops near the same floor (33 to 99 mm at lag 120): the phase side
passes the solve's own constellation-common motion besides the pin's (section 2.3), and no clock datum removes that.

(c), the refusal: `network.clock_pivot_frame_q` adds the pinned pivot's walk r_p dt to every other carried clock, the
S-transform of the absolute walk to the basis in which the pivot is zero (Snow 15.9, `network/carry/PivotFrameQ.hpp`). The
sum-to-zero datum has no pivot; its basis needs its own transform (S = I - 11^T/N per constellation), which the code does not
have. The refusal guards that premise; it is not a physical incompatibility. Lifting it means a second process-noise formula at
the three sites that use the pivot one (the fold decision, the dense fold, the exact coupling update) and either the plain sum's
re-reference at every membership change or a time-linked variant; its user-side gain is bounded by row (c) above. Not pursued.

### 2.6 Where the fix belongs

At the emission stage, in the frame of the phase-visible clock: the receiver's one GPS-to-Galileo offset serves code and phase,
the code side is already held per constellation, and the phase side carries the solve's pinned frame. A held frame on the
emitted phase-visible clock removes the pin's motion, the tracked pivot's walk and the clones' common revision together, leaves
every between-satellite difference and every phase-bias combination across signals unchanged, and touches no estimate.

## 3. The mechanism

`network.emit_phase_frame_hold` (default off; requires `network.emit_pb_hold`; outputs only, the solve byte-identical; not
fingerprinted), `network/emit/PhaseFrameHold.{hpp,cpp}`, called in `networkEmitStore` after the phase-bias hold (the last writer
of the phase biases) and before the broadcast wire. Per constellation, every satellite with an emitted clock and both phase
biases of a modelled pair (GPS L1W or L1C with L2W, Galileo L1C with L5Q or L1X with L5X, the first present in that order) gives
the phase-visible correction against the raw broadcast a priori, v = C - P_IF + U (U the tracked a priori's offset, 0 without
it; on the CODE a priori, v = C - P_IF). A satellite votes when it has a value of the same pair and the same generation of both
phase biases at the previous emitted epoch (within the clock frame hold's 600 s horizon), and neither a datum change of its
constellation nor a retro bump of its own is pending this epoch. With at
least 4 voters (the clock frame hold's `min_common`) the constellation's frame G moves by the median of their one-epoch
innovations; otherwise it stays. Every phase bias of every satellite of the constellation is emitted as P + G, so the
phase-visible clock moves by -G and every between-satellite difference, the wide lane and every geometry-free combination of
the phase biases are unchanged.

Both edges are events (standing invariant 7). The decoupling edge: `PHASE_FRAME` `frame_start` at a constellation's first vote;
every later step is counted. The re-coupling edge: when |G| exceeds `network.phase_frame_leash_m` the constellation re-anchors (G
= 0) with a datum mark (`markDatumChange`), event `PHASE_FRAME` `reanchor`; a warm resume's first armed epoch after emitted
products re-anchors the same way (`resume_seam`), since the frame restarts cold. Counters and the run-end line
(`networkPhaseFrameHoldRunEnd`): per constellation the epochs voted and paused, voters, the |step| ladder, the summed |step|
drained, the largest |G|, re-anchors, datum-change epochs left out, and the phase biases shifted; "0 voted is inert wiring".
Trace `network_phase_frame.csv` (epoch, constellation, voters, step, G, event). Tests:
`tests/network/test_network_phase_frame_hold.cpp`.

## 4. Gates (frozen 2026-11-03, before any run of the mechanism)

The graded posture is card BRD's GPS + Galileo broadcast posture with both of its knobs (`_rap_brd_ngt{129,132}_6`) plus
`network.emit_phase_frame_hold`; references are card BRD's runs on the same days. Census: `brd_census.py` (gcensus replays of
WTZZ00DEU and the 16 sites) and `brd_pool.py`, paired per site, the mean of the site's hourly p50 over hours 2 to 6.

| gate | bar |
| --- | --- |
| U0 | Debug `gnss_tests`: no new failure, the new suite passes; `make_golden_config.py --verify` PASS; `docs_index_check.py`, `nom_epoch_lint.py` pass. |
| G0, off | Knob off: the 2 h field and sim goldens byte-identical to the base exe's (`products.txt` without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`). |
| G1, golden | The knob on the GPS + Galileo golden (CODE a priori, card LAH's hold), 2 h, DOY 129: WTZZ00DEU within 2 mm in hours 1 and 2; largest |G| at most 5 mm. |
| G2, live | The graded posture, 2 h, DOY 129 and 132: both constellations voted on at least 99 % of the emitted epochs after the first (datum-change epochs aside); the run-end line present; exit 0; phase-visible D against CODE at lag 1 at most 2.5 mm and at lag 40 at most 20 mm (hour 2). |
| G3, mechanism | The graded posture, 6 h, each day: phase-visible D against CODE at lag 120 at most 35 mm (from 188.5 and 193.4; `icl_dcode.py`); the closed-loop receiver's ISB one-epoch rms at most 2.0 mm (from 4.20 and 3.60, golden 0.80 and 0.85; `icl_rxisb.py`). |
| G4, receivers as configured reach the free-offset receivers | The graded posture, 6 h, receivers as the golden configures them, against the GPS + Galileo golden (`brd_c129_6`, `brd_c132_6`): census paired median at most the free-offset arm's against the same golden plus 0.003 m, each day (card BRD section 6.8: +0.0034 and +0.0205 m, so at most +0.0064 and +0.0235 m). |
| G5, the user bars | The same arms: the bars of card BRD's G2 (WTZZ00DEU hours 2 to 6 mean change at most +0.005 m each day; census paired median at most +0.005 m each day; pooled over the 32 site-days at most 20 % worse by more than 0.02 m). |

Reported without a bar: hour 1; the code-visible D; the frame's |G| range and re-anchors; the posture's regime on the CODE a
priori with GPS + Galileo (`_rap_icl_r{129,132}_6`, card SMH's hold with the golden's a priori), with and without the knob, as the
attribution of what remains. G5 is expected to fail on DOY 132, where card BRD's GPS-only arms already fail (+0.0101 m); G4 is the
gate of this card's question.

## 5. Results

### 5.1 The build and the off path: U0 and G0 (2026-11-03)

Exe `gnss_processor_icl1.exe` (sha256[:16] `2cef22f4d967f078`), Release from the worktree
`D:/GnssProcessorSupport/worktrees/icl`. The committed source differs from that exe's only in comment text (card citations
removed from four comments); rebuilt as `gnss_processor_icl2.exe` (`6eaa68964d3ff620`), it reproduces the graded exe's 10-epoch
smoke of the graded posture byte for byte (`icl_p129_s2` against `icl_p129_s`: `products.txt`, `network_phase_frame.csv`,
`closed_loop_trace.csv`, `network_epoch_status.csv`).

U0: Debug `gnss_tests` 3 873 tests in 528 suites: 3 840 passed, 33 skipped (fixture-gated, as on the base), 0 failed (the 15
tests of the new suite `PhaseFrameHold` among them; the base had 3 858 in 527); `gnss_simulator_selftest` 99 of 99;
`make_golden_config.py --verify` PASS; `config_posture_check.py --verify-loader` 0 gaps; `docs_index_check.py` passes;
`nom_epoch_lint.py` PASS (V12: 96 writer functions, 96 `kEntityWriters` rows). `nom_n4_verify.py` V7 flags the contract-test
lines of card BRD's commit, as it did on card BRD; this card does not touch that file. **U0 PASS.**

G0: knob off, the 2 h field golden (`icl_c129_2` against `brd_c129_2`) and sim golden (`icl_s_2` against `brd_s0_2`):
`products.txt` without comment lines, `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical; of 34 trace files
three differ, in wall-clock columns only (`ms`, the run time). **G0 PASS.**

### 5.2 The golden with the knob: G1

`icl_cp129_2` against `icl_c129_2` (GPS + Galileo golden, 2 h, DOY 129): the C records identical; WTZZ00DEU 0.056 / 0.012 m
against 0.059 / 0.012 m in hours 1 / 2; largest |G| 35 mm (GPS) and 51 mm (Galileo), no re-anchor. **G1 FAIL** on both bars
(hour 1 moves 3 mm; |G| above 5 mm). The premise of the bar was wrong: under card LAH's phase-clock hold the phase-visible
correction is not constant within a generation. It carries M, the constellation's accumulated common step of the frame-held
emitted clocks, which LAH keeps on code and phase alike by design (`network/emit/PbHold.cpp`: a constellation-common move stays
on the clock, where a receiver's clock and offset absorb it on both). The frame removes M from the phase side only (|step| below
0.5 mm in 214 and 221 of 239 epochs, a few steps of 2 to 10 mm in hour 1), so on the golden it opens a code-to-phase level
difference of up to 5 cm per constellation, which the receiver hardly sees (hour 1 better by 3 mm, hour 2 equal). The knob
is a mechanism for the smoothed hold, where the phase side carries the solve's frame; on the phase-clock hold it is not a no-op.

### 5.3 The graded posture: G2 and G3

`icl_p129_6`, `icl_p132_6` against card BRD's `brd_ngt129_6`, `brd_ngt132_6` (the same arms without the frame):

| day | frame moved, epochs without a pending datum change (G / E) | D hour 2, lags 1 / 40 (mm) | phase-visible D hours 2 to 6, lags 1 / 10 / 40 / 120 (mm) | receiver ISB rms (mm) | largest |G|, GPS / Galileo |
| --- | --- | --- | --- | --- | --- |
| 129 | 719 of 719 / 718 of 718 | 1.3 / 5.8 | **1.1 / 3.1 / 5.7 / 9.4** (without: 3.3 / 20.7 / 71.4 / 188.5) | **1.03** (without: 4.20) | 0.239 / 0.474 m |
| 132 | 719 of 719 / 718 of 718 | 1.4 / 5.5 | **1.4 / 5.0 / 12.6 / 32.4** (without: 3.6 / 23.1 / 77.1 / 193.4) | **1.26** (without: 3.60) | 0.489 / 0.371 m |

The golden reads 0.1 / 0.4 / 1.3 / 2.5 mm and 0.80 mm on DOY 129 (2.4 mm and 0.85 on DOY 132); the predictions of section 2.5
were 8.6 and 28.9 mm at lag 120. The code-visible D is unchanged (22.6 and 28.3 mm at lag 120), as are every C record (37 763
and 37 788) and `network_epoch_status.csv`: outputs only. Run-end line (DOY 129): Galileo voted 718, paused 1, datum-change
epochs 1, 21.9 voters per voted epoch, |step| below 0.5 / 1 / 2 / 5 / 10 mm and above 192 / 154 / 227 / 138 / 6 / 1, drained
0.933 m; GPS voted 719, 30.4 voters, drained 1.346 m; 120 665 phase-bias values shifted; no re-anchor, no resume seam. G at the
end of hours 1 to 6 (m): DOY 129 GPS -0.017 -0.128 -0.207 -0.202 -0.204 -0.034, Galileo -0.047 +0.152 +0.336 +0.399 +0.424
+0.466; DOY 132 GPS -0.050 -0.265 -0.446 -0.434 -0.426 -0.313, Galileo +0.094 +0.238 +0.327 +0.348 +0.360 +0.322. The frame
absorbs 0.1 to 0.2 m per hour in hours 2 and 3 (the tracked pivots' walk and the solve's own phase motion) and less than
0.05 m per hour after, except GPS in hour 6 of DOY 129 (+0.17 m); the largest |G|, 0.49 m, stays at half the leash. **G2
PASS. G3 PASS** (DOY 132's lag 120 within 3 mm of its bar).

### 5.4 The user, receivers as configured: G4 and G5

Against the GPS + Galileo golden, census keys `iclp129`, `iclp132` (17 replays each, exit 0):

| day | arm | WTZZ00DEU by hour (m) | WTZZ hours 2 to 6 | census median by hour | census paired, hours 2 to 6 | worse by > 2 cm | hour 1 paired |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 129 | GPS + Galileo golden (`brd_c129_6`) | 0.059 0.012 0.023 0.012 0.017 0.012 | 0.0152 | 0.142 0.050 0.036 0.029 0.032 0.030 | | | |
| 129 | both knobs (`brd_ngt129_6`) | 0.107 0.168 0.221 0.176 0.130 0.169 | 0.1729 | 0.286 0.144 0.396 0.428 0.201 0.176 | +0.2058 | 14 (88 %) | +0.0872 |
| 129 | both knobs, free offset (card BRD, `brdngtI129`) | 0.066 0.010 0.023 0.007 0.011 0.012 | 0.0129 | 0.226 0.048 0.033 0.036 0.040 0.039 | +0.0034 | 4 (25 %) | +0.0359 |
| 129 | both knobs and the frame (`icl_p129_6`) | 0.059 0.026 0.016 0.013 0.015 0.010 | **0.0160** | 0.189 0.051 0.042 0.038 0.047 0.039 | **+0.0114** (3 better, 8 worse) | 5 (31 %) | +0.0279 |
| 132 | GPS + Galileo golden (`brd_c132_6`) | 0.072 0.019 0.014 0.017 0.019 0.009 | 0.0156 | 0.118 0.068 0.051 0.036 0.035 0.028 | | | |
| 132 | both knobs (`brd_ngt132_6`) | 0.213 0.239 0.100 0.084 0.048 0.140 | 0.1220 | 0.312 0.248 0.226 0.133 0.107 0.071 | +0.1158 | 15 (94 %) | +0.0788 |
| 132 | both knobs, free offset (`brdngtI132`) | 0.076 0.062 0.069 0.030 0.023 0.018 | 0.0406 | 0.152 0.088 0.060 0.050 0.054 0.036 | +0.0205 | 8 (50 %) | +0.0084 |
| 132 | both knobs and the frame (`icl_p132_6`) | 0.062 0.046 0.045 0.040 0.022 0.029 | **0.0363** | 0.237 0.092 0.070 0.057 0.049 0.043 | **+0.0173** (2 better, 11 worse) | 7 (44 %) | +0.0271 |
| pooled | both knobs and the frame | | | | +0.0144 | 12 of 32 (38 %) | +0.0272 |

G4: DOY 129 +0.0114 m against its bar of +0.0064 m, DOY 132 +0.0173 m against +0.0235 m. **G4 FAIL** (DOY 129). G5: WTZZ00DEU
+0.0007 m (passes) and +0.0207 m (fails); census +0.0114 and +0.0173 m (both fail); 38 % of the site-days worse by 2 cm (fails).
**G5 FAIL**, as section 4 expected on DOY 132.

Read beside the gates, not as bars. Paired site by site with the free-offset arm, the frame arm reads -0.0002 m on DOY 129 (1
better, 3 worse at 1 cm; WTZZ00DEU +0.0031 m) and +0.0044 m on DOY 132 (4 better, 4 worse; WTZZ00DEU -0.0043 m): the receivers
configured as today, given the frame, do as receivers that re-estimate their offset every epoch do without it. Against both
knobs alone, -0.1838 and -0.1100 m (13 better and 1 worse; 15 better and 0 worse). The G4 statistic compares two medians against
the golden, which do not subtract: the two arms scatter by 2 to 9 cm per site in both directions (DAV100ATA +0.033 against
-0.056 m on DOY 129 and -0.024 against +0.062 m on DOY 132, ARHT00ATA +0.046 against +0.652 m on DOY 132), and on DOY 129 the
scatter moves the median by 8 mm. On the golden itself the free offset moves every site by less than 1 cm except ARHT00ATA
(-0.071 and +0.022 m).

### 5.5 Attribution: the regime on the CODE a priori

The posture's regime with GPS + Galileo on the golden's CODE a priori (card SMH's smoothed hold, `network.clock_anchor_q` 5e-6,
B1, B2, the keep, X2 with X2b: card BRD's set without its three broadcast keys), receivers as configured, census keys `iclr129`,
`iclr132`:

| day | arm | WTZZ00DEU by hour (m) | WTZZ hours 2 to 6 | census paired, hours 2 to 6 | worse by > 2 cm | phase-visible D, lags 1 / 120 (mm) | receiver ISB rms (mm) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 129 | the regime on CODE (`icl_r129_6`) | 0.066 0.032 0.073 0.067 0.056 0.065 | 0.0586 | +0.0069 | 3 (19 %) | 1.6 / 25.0 | 0.98 |
| 132 | the regime on CODE (`icl_r132_6`) | 0.051 0.022 0.041 0.037 0.018 0.041 | 0.0316 | +0.0087 | 4 (25 %) | 2.2 / 22.0 | 1.12 |
| 129 | the regime on CODE and the frame (`icl_rp129_6`) | 0.063 0.021 0.025 0.024 0.016 0.021 | 0.0213 | +0.0016 | 0 (0 %) | 0.0 / 0.2 | 0.80 |
| 132 | the regime on CODE and the frame (`icl_rp132_6`) | 0.056 0.022 0.034 0.031 0.015 0.039 | 0.0283 | +0.0077 | 2 (12 %) | 0.2 / 2.2 | 0.89 |

The smoothed hold passes the solve's own constellation-common phase motion on the CODE a priori too: 25.0 and 22.0 mm over 120
epochs against the golden's 2.5 and 2.4, and the receivers lose +0.043 and +0.016 m at WTZZ00DEU, +0.007 and +0.009 m at the
census, before any broadcast clock enters. The frame removes it: the level then moves as the golden's (0.2 and 2.2 mm at lag
120), the receiver's offset as the golden's (0.80 and 0.89 mm per epoch), and paired with the regime without the frame the
census reads -0.0043 and -0.0015 m (6 sites better and 1 worse at 1 cm over the 32 site-days, none worse by 2 cm), WTZZ00DEU
-0.0373 and -0.0034 m. Census median by hour with the frame: 0.167 0.044 0.036 0.041 0.042 0.038 (DOY 129), 0.109 0.058 0.055
0.041 0.029 0.035 (DOY 132).

The chain, census paired median over hours 2 to 6 against the golden (pooled share of site-days worse by more than 0.02 m),
receivers as configured:

| step | DOY 129 | DOY 132 | pooled | WTZZ00DEU 129 / 132 |
| --- | --- | --- | --- | --- |
| the regime on CODE, no frame | +0.0069 | +0.0087 | +0.0069 (22 %) | +0.0434 / +0.0160 |
| the regime on CODE, the frame | +0.0016 | +0.0077 | +0.0033 (6 %) | +0.0061 / +0.0126 |
| both knobs on the broadcast a priori, no frame | +0.2058 | +0.1158 | +0.1430 (91 %) | +0.1576 / +0.1064 |
| both knobs on the broadcast a priori, the frame | +0.0114 | +0.0173 | +0.0144 (38 %) | +0.0007 / +0.0207 |
| the broadcast clock given the frame (knobs and frame against the regime on CODE and the frame, paired) | +0.0050 | +0.0127 | +0.0108 | -0.0053 / +0.0081 |

With the frame on both, what the broadcast clock costs on DOY 132 (+0.0127 m) is card BRD's GPS-only remainder of that day
(+0.0160 m, its section 6.7: the clock-anchor decouplings and the smoothed hold's bridge transfers); on DOY 129 the GPS-only
posture cost nothing against the regime (-0.0019 m) and GPS + Galileo costs +0.0050 m, of the size of the 9.4 mm per 120
epochs that the frame leaves in the GPS-to-Galileo level against its 0.2 mm on the CODE a priori.

## 6. Verdict

**The GPS-to-Galileo level is explained, and a network-side mechanism holds it: with the frame, receivers configured as today do
as receivers that re-estimate their offset every epoch. The pre-registered user gates G4 (on DOY 129) and G5 fail, and so does
G1.** Measured before building: the S-basis pins one satellite clock per constellation every epoch, so each constellation's level
in the solve is its pivot's a priori error. Under the tracked a priori the pivot's track sums the IF-TD column's per-epoch errors
and nothing corrects the datum satellite's, a walk of 85 to 183 mm per 120 epochs; without the track it is the pivot's broadcast
misfit (G02: 19.7 mm per epoch). The clock frame hold keeps that motion off the code side. Card SMH's smoothed hold passes it to
the phase side through the clone's motion in the solve's frame, together with the solve's own constellation-common phase motion
(25.0 and 22.0 mm per 120 epochs on the CODE a priori). Neither datum candidate reaches it: the pivot left out of the track, a
quiet pivot, a time-linked sum to zero and the station ISBs all stop at 33 to 99 mm per 120 epochs offline, and the refusal of
the sum-to-zero datum under the pivot-frame process noise guards a formula's premise, not a physical incompatibility.

The phase-visible frame hold (`network.emit_phase_frame_hold`) removes each constellation's common innovation of the emitted
phase-visible correction against the raw broadcast a priori. Outputs only: every clock record and the epoch status are
unchanged. The GPS-to-Galileo phase-visible level against CODE falls from 188.5 and 193.4 to 9.4 and 32.4 mm per 120 epochs, the
held-out receiver's offset moves 1.03 and 1.26 mm per epoch (4.20 and 3.60 without the frame; golden 0.80 and 0.85), and at the
user (golden's receivers, hours 2 to 6, against the golden) WTZZ00DEU reads +0.0007 and +0.0207 m (from +0.1576 and +0.1064 m),
the census +0.0114 and +0.0173 m (from +0.2058 and +0.1158 m). Paired site by site with card BRD's free-offset receivers the
frame arm reads -0.0002 and +0.0044 m: once the frame holds the level, re-estimating the offset gains nothing. G4, which compares
the two arms' medians against the golden, fails on DOY 129 by 5 mm through the per-site scatter of the two arms; G5 fails on
both days. On the CODE a priori the same frame cures the regime's own GPS + Galileo cost (census +0.0069 to +0.0033 m pooled,
WTZZ00DEU 0.0586 to 0.0213 m on DOY 129, the level as the golden's). Given the frame on both, what the broadcast clock still costs
is +0.0050 m (DOY 129) and +0.0127 m (DOY 132), the latter card BRD's GPS-only remainder of that day. On card LAH's phase-clock
hold the frame is not a no-op (G1: |G| up to 51 mm over 2 h, the receiver within 3 mm): it is a mechanism for the smoothed hold.

Nothing is armed; arming, any golden change and the merge are the author's decisions.

## 7. What remains

- **The frame over a day.** |G| reached 0.49 m in 6 h (the leash is 1.0 m) and moves 0.1 to 0.2 m per hour while the tracked
  pivots walk; a 24 h run, and the leash's re-anchors (each a datum mark) over a day, are owed. Leaving each pivot out of the
  track, or electing a quiet pivot, would shrink the walk the frame drains (section 2.5, rows (a) and (a')).
- **One level for code and phase.** The frame takes the phase side to the median of the raw broadcast a priori increments, the
  clock frame hold the code side to the median of the tracked ones (22.6 and 28.3 mm per 120 epochs). Untested: the clock frame
  hold voting on C + U under the track, or a phase frame voting on the phase biases' common mode (consistent with the code side
  by construction and an exact no-op under card LAH's hold).
- **G4's statistic.** The bar compared two medians against the golden; a site-paired comparison with the free-offset arm reads
  the card's question directly (section 5.4).
- **The broadcast clock's remainder.** +0.0127 m on DOY 132 (card BRD section 8: the clock-anchor decouplings and the smoothed
  hold's bridge transfers) and +0.0050 m on DOY 129; DOY 130 and 131 and a real-time run are owed before any adoption.
