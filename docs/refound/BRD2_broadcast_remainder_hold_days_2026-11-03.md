# BRD2: the broadcast-referenced network's DOY-132 remainder, the phase-clock hold on the tracked a priori, DOY 130 and 131

**STATUS**

- **id:** BRD2
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered and closed 2026-11-03: on the GPS broadcast-referenced posture with card BRD's two knobs, DOY 130 and 131 pass the day bars and the four days fail only the pooled share (25 % of site-days worse); the DOY-132 remainder is carried by the clock anchor's statistic decouplings through the smoothed hold's bridge, and with that test off it vanishes and all four days pass (pooled +0.0021 m, 17 %), pending a satellite clock step test; card LAH's phase-clock hold on the tracked a priori fails at either anchor q (112 to 116 mm of phase-visible wander per 120 epochs, users +0.3 to +0.6 m); nothing armed.
- **source:** card [BRD](BRD_broadcast_referenced_level_2026-11-02.md) sections 6.6, 6.7, 6.9 and 8; card [LAH](LAH_level_aware_hold_2026-09-29.md) (the phase-clock hold); card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (the smoothed hold and its bridge); card [SCR](SCR_sat_clock_step_response_2026-10-26.md) (`network.emit_pb_hold_trip_generation`)
- **knobs / posture:** new, default off: `network.apriori_clock_track_phase_clock_hold` (section 3; ledger KEEP_OFF). Existing keys the arms set: `network.emit_pb_hold_trip_generation` (card SCR, UNVALIDATED), `network.clock_anchor_k` (1e6 disarms the statistic test), `network.emit_pb_hold_phase_clock`, `network.emit_pb_hold_smoothed`, `network.clock_anchor_q`, `files.internal_data_root` (card NBP's root without G01). The graded posture is card BRD's T2 posture: the broadcast-referenced posture of card NBP with `network.cycle_slip.if_phase_td_sat_centre` and `network.apriori_clock_track`, GPS
- **dates in the text:** 2026-11-03
- **hashes / run dirs:** branch `brdc/brd2` from `brdc/independence` (`1e8a543`, unmerged), worktree `D:/GnssProcessorSupport/worktrees/brd2`; exes `gnss_processor_b2base.exe` (sha256[:16] `7123ae48e9c41807`, `1e8a543`) and `gnss_processor_b2k.exe` (`e44e043908d07826`, the committed source), Release; arms `D:/GnssProcessorSupport/runs_nom/_rap_brd2_*`; census replays `_rap_<key>_<site>` with keys `b2g130`, `b2g131`, `b2r130`, `b2r131`, `b2t130`, `b2t131`, `b2tj132`, `b2rj132`, `b2tk129` to `b2tk132`, `b2rk129` to `b2rk132`, `b2rn132`, `b2tl129`, `b2tl132`, `b2tlq129`, `b2tlq132`; tools `D:/GnssProcessorSupport/worktrees/brd2_tools/` (outside the repository); card BRD's runs `_rap_brd_*` and census keys `brdg`, `brdr`, `brdnt` are the references of DOY 129 and 132
- **gates (frozen at pre-registration):** section 4, amendments B1 (section 4b) and B2 (section 4c), each committed before its runs

**Outcome.** Question: what carries card BRD's DOY-132 remainder, can card LAH's phase-clock hold replace the smoothed hold on the tracked a priori, and how do DOY 130 and 131 grade? DOY 130 and 131 pass the day bars; over the four days 25 % of site-days are worse by 2 cm (bar 20 %), so gate D fails. The remainder is the clock anchor's statistic decouplings: at each, the smoothed hold's bridge drops centimetres of clock motion. With the test off it vanishes and all four days pass (pooled +0.0021 m, 17 %), pending a satellite clock step test. The phase-clock hold fails at either anchor q. Nothing armed.

> Results are appended TO THIS FILE, below the text.

---

## 1. The questions

Card BRD closed with the GPS broadcast-referenced network (SP3 orbits the only precise product, the satellite clocks
referenced to the broadcast ephemeris) passing its user bars on DOY 129 and failing them on DOY 132: against the same
estimator on the CODE a priori (the "regime on CODE", card BRD section 6.7) the tracked posture costs +0.0160 m at the
16-site census on DOY 132 and -0.0019 m on DOY 129. It left three items (its section 8):

1. **The remainder.** Which decouplings, satellites and epochs carry the DOY-132 cost, and whether the hold or the
   anchor carries it (the anchor's q does not: card BRD section 6.9).
2. **The hold.** With the tracked a priori the a priori follows the real clock to about 3 mm per epoch. Card LAH's
   phase-clock hold is valid with a precise a priori; whether it can replace the smoothed hold on the tracked a priori
   is open, and the loader refuses the combination today.
3. **The days.** DOY 130 and 131 are ungraded with both knobs.

## 2. Measured before pre-registration (card BRD's runs, no new run)

Tools in `brd2_tools/`: `sitediff.py`, `onsets.py`, `decouple_vs_sites.py`, `step_table.py`, `wire_pv_diff.py`,
`pv_predict.py`. Arms of card BRD: GPS golden `brd_g*_6`, regime on CODE `brd_r*_6`, tracked posture `brd_nt*_6`.

### 2.1 The census remainder by site (DOY 132, tracked against the regime on CODE, hours 2 to 6)

ARHT00ATA +0.789, UTQI00USA +0.092, HARB00ZAF +0.067, WUH200CHN +0.043, KOKB00USA +0.024, POHN00FSM +0.023,
BADG00RUS +0.021, DAV100ATA +0.019, CEDU00AUS +0.013, ANMG00MYS +0.009, ASCG00SHN +0.004, WTZZ00DEU +0.002, CHTI00NZL
-0.000, SCRZ00BOL -0.010, STPM00SPM -0.010, BAKE00CAN -0.023, QUIN00USA -0.043 m. The median (+0.016 m) is set by the
sites near 1 to 2 cm (DAV100ATA, CEDU00AUS, BADG00RUS), not by ARHT00ATA, which does not converge on either wire
(regime 0.67 to 0.19 m by hour, tracked 1.66 to 0.66 m) and diverges from 0.42 h, before any decoupling. Card BRD
sections 6.7 and 8 name HARB00ZAF as HARB00USA; the census site is HARB00ZAF.

### 2.2 The decouplings

Clock-anchor decouplings (`CLK_ANCHOR cta_decouple`, all `reason=phi`), DOY 132, satellites (|w|):

| hour | regime on CODE (`brd_r132_6`) | tracked (`brd_nt132_6`) |
| --- | --- | --- |
| 0.642 | | G10 (8.0), G16 (8.0) |
| 0.800 | G05 G14 G22 G30 (6.0 to 6.1) | G05 G14 G22 G30 (6.2 to 6.3) |
| 0.975 | G05 G14 G22 G30 (7.3 to 7.7) | G05 G14 G21 G22 G30 (8.1 to 8.7) |
| 1.750 | G04 G07 G08 G13 G15 G23 G25 G30 (19 to 38) | G06 G08 G14 G25 G30 (7.9 to 13.6) |
| 1.825 | G04 G07 G08 G13 G15 G23 G25 G30 (18 to 37) | G06 G08 G14 G25 G30 (6.7 to 13.5) |
| 2.592 | G04 G06 G07 G13 G15 G23 G24 G30 (6.8 to 23) | G13 G15 G23 G30 (6.2 to 14.1) |
| 3.917 | G12 (15.1) | G12 (15.0) |

Six of the seven epochs are shared by the two regimes; the trips are network events, several satellites at one epoch
with one phase-space residual. At 1.750 and 1.825 h BSHM00ISR re-enters G14 with geometry-free steps of -165.7 and
+166.9 m. DOY 129 has one decoupling on the tracked arm (G11 at 2.900 h) and three on the regime (G07, G08 at 0.233 h,
G11 at 2.900 h). The users' own reaction to the wire's clock marks is an inter-system bias re-inflation
(`RealTimeEngine::reinflateOnClockDisc`), inert for a GPS-only user; the satellite counts of every census consumer are
equal on the two wires epoch by epoch.

Timing: by 10-epoch running means, HARB00ZAF starts to lose at 1.79 h, DAV100ATA at 1.87 h, KOKB00USA at 1.94 h and
BADG00RUS at 2.02 h, each tracking satellites decoupled at 1.750 h on the tracked arm; HARB00ZAF's paired error grows
from +0.027 m at 1.742 h to +0.167 m at 2.242 h. Pooled over the seven epochs the sites that track a decoupled
satellite do not lose more in the next 30 min than the sites that do not (for example +0.039 against +0.094 m at
0.800 h, -0.009 against -0.020 m at 1.750 h), so the epoch census alone does not decide.

### 2.3 The phase-visible clock

The statistic of card BRD section 6.4 (the phase-visible clock minus CODE per satellite and phase-bias generation,
de-commoned, hours 2 to 6, rms of its change over 1, 10, 40 and 120 epochs):

| day | arm | 1 | 10 | 40 | 120 epochs |
| --- | --- | --- | --- | --- | --- |
| 129 | GPS golden (phase-clock hold, CODE by construction) | 0.0 | 0.0 | 0.0 | 0.1 mm |
| 129 | regime on CODE | 2.0 | 3.9 | 5.3 | 6.9 |
| 129 | tracked | 2.2 | 4.0 | 5.3 | 6.8 |
| 132 | GPS golden | 0.0 | 0.0 | 0.1 | 0.1 |
| 132 | regime on CODE | 2.6 | 4.5 | 7.0 | 11.7 |
| 132 | tracked | 2.8 | 4.7 | 7.2 | 11.7 |
| 132 | tracked, anchor q 3e-7 (`brd_ntq132_6`) | 2.8 | 5.2 | 8.4 | 13.2 |

On DOY 132 the tracked wire wanders exactly as the regime on CODE does: the pooled wander is not the remainder. The
code-visible clock (a priori plus C record) against CODE, its time-varying part: 48.5 mm on the tracked wire against
67.5 mm on the regime (61.5 on the golden): the code side is not worse either.

### 2.4 The two wires per satellite: a step at each decoupling

The tracked wire's phase-visible clock minus the regime's (L1W, de-commoned per epoch), its step from the epoch before
a decoupling to ten epochs after (`step_table.py`):

| | n | median | mean | max |
| --- | --- | --- | --- | --- |
| satellites the tracked arm decoupled | 26 | 5.8 mm | 9.4 | 32.4 |
| satellites only the regime decoupled | 11 | 4.2 | 3.9 | 7.2 |
| the other satellites (control) | 125 | 1.7 | 2.9 | 10.5 |

At 1.750 h: G06 +32, G25 +26, G30 +24, G14 +17, G08 +13 mm; G10 -24 mm at 0.642 h, G21 -24 mm at 0.975 h. The hold's
trace (`network_pb_hold_clone.csv`, L1W) gives the mechanism. Before the decoupling the smoothed hold's clone carries a
passed motion Phi of decimetres on the tracked a priori (G06 -0.49 m, G14 -0.21 m after 1.7 h of its generation)
against centimetres on CODE (G06 -0.01 m): the solve's clock correction follows the accumulated error of the track U,
and the clone passes it to the phase-visible clock. At a decoupling the clone dies `Withdrawn` and the key bridges on
card LAH's transfer until a clone is born again at re-engagement four epochs later; on the bridge the phase-visible clock
follows the a priori alone, so the correction's own motion in the bridged epochs reaches the code side only (G06:
-24 mm in the bridge at 1.750 h, -15 mm in the one at 1.825 h). On the CODE a priori that motion is millimetres and the
bridge costs nothing; on the tracked a priori it is the size of the steps above.

### 2.5 The phase-clock hold, predicted from the tracked arm

The hold is emit-side (the solve, the C and the B records do not depend on its mode). Under card LAH's hold the
phase-visible clock is the a priori the solve ran against, plus a constellation-common term and a constant per
generation: on the tracked a priori brdc(iode) + O + U, which `network_brdc_offset.csv` records (its offset column is
O + U). The same statistic on that series (`pv_predict.py ...:lah`; on the regime on CODE the same construction gives
0.0 mm, the golden's construction):

| day | 1 | 10 | 40 | 120 epochs | the smoothed hold's 120 |
| --- | --- | --- | --- | --- | --- |
| 129 | 3.6 | 13.8 | 43.8 | 112.0 mm | 6.8 |
| 132 | 4.0 | 15.0 | 46.8 | 116.1 | 11.7 |

The track is a sum of per-epoch columns whose errors accumulate (card BRD section 6.2: 89 mm over 120 epochs against
CODE). The phase-clock hold would pass that walk to every user; the smoothed hold passes the solve's correction of it.
The prediction is that the phase-clock hold fails on the tracked a priori whatever the anchor's q, which acts on the
code side only under that hold.

## 3. What is built

`network.apriori_clock_track_phase_clock_hold` (default false; `include/config/GnssConfigTypes.hpp`, loader
`src/core/config/GnssConfigLoadNetwork.cpp`). With `network.apriori_source sp3_brdc_clock` and
`network.apriori_clock_track`, the key makes the loader accept `network.emit_pb_hold_phase_clock` in place of the
smoothed hold (and require it); without the track it is refused at load. Nothing else changes in the code paths of the
hold: card LAH's transfer runs on the clocks about to be emitted before the broadcast wire adds U, so the delivered
phase-visible clock within a generation is brdc(iode) + O + U plus the constellation's common term. The run-end line
of the broadcast wire states the hold's a priori, and a new counter sums the track's motion carried into held
phase-visible clocks (the edges of a generation are the existing `PB_HOLD gen_start` rows and counters). Unit tests:
the loader's accept and refuse cases.

## 4. Gates (frozen 2026-11-03, before any run of this card)

Grading as card BRD's T2: 6 h from 00:00, GPS; hour 1 is ignition, hours 2 to 6 the settled read. Census:
`scripts/clock_level/gcensus.py` replays (WTZZ00DEU plus the 16 sites) through `brd_census.py`, paired per site, the
mean of the site's hourly p50 over hours 2 to 6; "worse" means worse by more than 0.02 m. References: the GPS golden
and the regime on CODE of the same day (card BRD's arms on DOY 129 and 132, this card's on DOY 130 and 131). Exes:
Release builds of this branch, renamed, below-normal priority; an arm run before the knob is built uses the branch's
base build (`1e8a543`).

| gate | bar |
| --- | --- |
| U0 | Debug `gnss_tests`: no new failure, the new tests pass; `gnss_simulator_selftest` all pass; `make_golden_config.py --verify` PASS; `docs_index_check.py` passes; `config_posture_check.py --verify-loader` 0 gaps. |
| B0 | The key off: the 2 h field and sim goldens on the final exe byte-identical to card BRD's base-exe runs (`brd_c129_2`, `brd_s0_2`: `products.txt` without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`), and the tracked posture, 2 h, DOY 132, byte-identical to card BRD's `brd_nt132_2`. |
| D, the days | The tracked posture (card BRD's T2 posture) on DOY 130 and 131, each against the GPS golden of the day: WTZZ00DEU mean change over hours 2 to 6 at most +0.005 m; census paired median at most +0.005 m; pooled over the four days 129 to 132 (64 site-days) at most 20 % of site-days worse. The change against the regime on CODE of the day is reported beside it. |
| A1, the hold | DOY 132: the tracked posture and the regime on CODE, each with `network.emit_pb_hold_trip_generation` (a decoupled satellite's keys start a generation, so no bridge carries a step). The remainder (census paired median, tracked against the regime, both with the key) is read as carried by the bridge if it is at most +0.005 m, not carried if above +0.010 m, partly carried between. Each arm's change against its arm without the key is reported. |
| A2, the anchor | DOY 132: the same pair with the anchor's statistic test disarmed (`network.clock_anchor_k` 1e6; the gap, datum and absolute-backstop decouplings stay). The same reading rule, for the decouplings as a whole. |
| L1, the phase-clock hold | The tracked posture with the new key, `network.emit_pb_hold_phase_clock` true and `network.emit_pb_hold_smoothed` false, anchor q 5e-6, DOY 129 and 132: (i) the C and B records of `products.txt` equal the tracked arm's (the hold is emit-side, the premise of section 2.5); (ii) phase-visible wander at 120 epochs at most 1.5 times the smoothed hold's of the same day (10.2 and 17.6 mm); (iii) the bars of D against the GPS golden on each day. The hold replaces the smoothed hold only if (ii) and (iii) pass on both days. |
| L2, the anchor nearer its floor | L1's arm with `network.clock_anchor_q` 2.9e-8 (the golden's), DOY 129 and 132: (ii) and (iii). |
| J, conditional | Only if A1's tracked arm passes the bars of D on DOY 132 against the GPS golden: the tracked posture with `network.emit_pb_hold_trip_generation` on DOY 129, 130 and 131, the bars of D over the four days. |

Reported without a bar: hour 1 per arm; decoupling counts, bridges and generations per arm; the run-end lines; the
per-site rows of every census.

## 4b. Amendment B1: the G01 control (pre-registered 2026-11-03 09:40, before any run of it)

While the section-4 arms ran, a second difference between the tracked wire and the regime on CODE was found on card
BRD's runs: G01 is absent from the broadcast-referenced wire for the whole day (0 clock records on `brd_nt132_6`
against 595 on `brd_r132_6` and the golden). The broadcast navigation file flags G01 unhealthy (health 63 in every
record of DOY 129 to 132), so the broadcast a priori has no clock for it, while the CODE product carries it. Every
census user that tracks G01 therefore loses a satellite against the regime on CODE (ARHT00ATA uses G01 in every
quarter-hour from 0.25 h on the regime's wire and never on the tracked wire). Card NBP built the matching data root
(`runs/_nbpa_cod_nog01`: the CODE clock files of DOY 128 to 132 without G01) and used it only for its clock statistic.

| gate | bar |
| --- | --- |
| B1, the satellite set | DOY 132: the regime on CODE with G01 removed from its a priori (`files.internal_data_root` `C:/GnssProcessor/runs/_nbpa_cod_nog01`, the only change against `brd_r132_6`). The tracked posture against it is the remainder with the satellite sets equal: G01's absence carries the remainder if it falls to at most +0.005 m, does not if it stays above +0.010 m, carries part of it between. The regime without G01 against the regime is reported beside it (G01's own worth to the census). |

## 4c. Amendment B2: the anchor's statistic test off on the other days (pre-registered 2026-11-03, before any run of it)

A2's tracked arm (`brd2_tk132_6`, `network.clock_anchor_k` 1e6) read +0.0008 m at the census and -0.0040 m at WTZZ00DEU
against the GPS golden on DOY 132, inside the bars of D, before its regime twin's census was read. Whether that holds on
the days with few decouplings is measured before any reading of it: the tracked posture and the regime on CODE, each with
`network.clock_anchor_k` 1e6, on DOY 129, 130 and 131 (6 h, the arms of D or card BRD with that one change).

| gate | bar |
| --- | --- |
| B2, the anchor's test off | The tracked posture with the test off: the bars of D against the GPS golden on each of DOY 129 to 131 and over the four days with A2's DOY-132 arm (64 site-days). Reported beside it: each arm against its arm with the test on, and the tracked arm against the regime on CODE with the test off of the same day. |

## 5. Results (2026-11-03)

Exes, Release from the worktree: `gnss_processor_b2base.exe` (`1e8a543`, sha256[:16] `7123ae48e9c41807`) for the arms
of D (DOY 130 and 131), A1 and A2; `gnss_processor_b2k.exe` (`e44e043908d07826`, the committed source) for L1, L2,
B0, B1, B2 and every census replay. Below-normal priority throughout, on a machine shared with other sessions' runs.
Hours from 00:00; "worse" in the census means worse by more than 0.02 m over hours 2 to 6.

### 5.1 U0 and B0

U0: Debug `gnss_tests` 3 863 tests in 528 suites, 3 830 passed, 33 skipped, 0 failed (the five new tests in one new
suite, `AprioriTrackPhaseClockHold`; card BRD's count was 3 858 in 527); `gnss_simulator_selftest` 99 of 99;
`make_golden_config.py --verify` PASS; `docs_index_check.py` passes; `config_posture_check.py --verify-loader` 0
gaps; `nom_epoch_lint.py` PASS. The loader refuses by logging and returning false, so each refusal test is paired with
a control that differs from it only in the condition under test and loads. **U0 PASS.**

B0, the key off, on `gnss_processor_b2k.exe`: the 2 h field golden (`brd2_c129_2` against card BRD's `brd_c129_2`), the
2 h sim golden (`brd2_s0_2` against `brd_s0_2`) and the tracked posture, 2 h, DOY 132 (`brd2_t132_2` against
`brd_nt132_2`): `products.txt` without comment lines, `closed_loop_trace.csv` and `network_epoch_status.csv`
byte-identical in all three; of the other trace files three differ, in wall-clock columns only (`ms`, `loop_ms`,
`ms_per_epoch_window`). **B0 PASS.**

### 5.2 D, the days

The tracked posture (card BRD's T2 posture) against the GPS golden and the regime on CODE of the same day. DOY 129 and
132 are card BRD's arms (section 6.6), repeated for the pooled clause.

| day | arm | WTZZ00DEU by hour (m) | WTZZ hours 2 to 6 | census median by hour | census paired, hours 2 to 6 | worse | hour 1 paired |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 130 | GPS golden (`brd2_g130_6`) | 0.061 0.023 0.026 0.050 0.076 0.070 | 0.0488 | 0.289 0.094 0.054 0.054 0.044 0.045 | | | |
| 130 | regime on CODE (`brd2_r130_6`) | 0.106 0.019 0.025 0.044 0.066 0.073 | 0.0455 | 0.214 0.092 0.071 0.051 0.044 0.051 | -0.0015 (4 better, 4 worse) | 3 (19 %) | -0.0180 |
| 130 | tracked (`brd2_t130_6`) | 0.125 0.021 0.024 0.032 0.057 0.059 | **0.0387** (-0.0101) | 0.208 0.079 0.070 0.071 0.055 0.052 | **+0.0047** (4 better, 7 worse) | 6 (38 %) | -0.0341 |
| 131 | GPS golden (`brd2_g131_6`) | 0.087 0.027 0.017 0.027 0.036 0.028 | 0.0271 | 0.204 0.062 0.052 0.050 0.048 0.039 | | | |
| 131 | regime on CODE (`brd2_r131_6`) | 0.135 0.048 0.013 0.015 0.028 0.039 | 0.0288 | 0.180 0.069 0.052 0.048 0.042 0.054 | +0.0071 (3 better, 6 worse) | 1 (6 %) | +0.0166 |
| 131 | tracked (`brd2_t131_6`) | 0.149 0.050 0.014 0.017 0.023 0.021 | **0.0249** (-0.0022) | 0.196 0.071 0.061 0.045 0.045 0.043 | **+0.0024** (4 better, 5 worse) | 2 (12 %) | +0.0170 |
| pooled, 129 to 132 | tracked against the GPS golden | | | | **+0.0063** (13 better, 25 worse) | **16 of 64 (25 %)** | +0.0000 |

Against the regime on CODE of the same day (the broadcast clock's cost given the estimator): DOY 129 -0.0019, 130
+0.0034, 131 +0.0013, 132 +0.0160 m at the census; WTZZ00DEU -0.0019, -0.0068, -0.0039, +0.0020 m; pooled over the
four days +0.0015 m, 12 of 64 site-days (19 %) worse. The regime's own cost against the golden: +0.0041, -0.0015,
+0.0071, +0.0054 m.

Decouplings (satellite-events) on the golden, the regime and the tracked arm: DOY 129 32, 3, 1; 130 51, 11, 7; 131 37,
5, 2; 132 66, 33, 26. Bridges of the smoothed hold, regime and tracked: 9, 3 (129); 32, 21 (130); 12, 6 (131); 96, 78
(132).

The bars: DOY 130 and 131 pass at WTZZ00DEU and at the census median, each day; the pooled share of site-days worse by
2 cm is 25 % against a bar of 20 %, carried by DOY 130 (6 sites: ARHT00ATA +0.075, BAKE00CAN +0.043, UTQI00USA +0.029,
HARB00ZAF +0.027, ASCG00SHN +0.022, POHN00FSM +0.020 m) and DOY 132 (5). **D FAIL** on the pooled clause; the single
days 129, 130 and 131 pass, 132 fails as in card BRD.

### 5.3 A1, A2 and B1: what carries the DOY-132 remainder

DOY 132, census paired median over hours 2 to 6 (sites worse), WTZZ00DEU hours 2 to 6:

| arm | against the GPS golden | against the regime on CODE | against its arm without the change | WTZZ00DEU |
| --- | --- | --- | --- | --- |
| tracked (`brd_nt132_6`) | +0.0101 (5) | +0.0160 (7) | | 0.0288 |
| regime on CODE (`brd_r132_6`) | +0.0054 (2) | | | 0.0268 |
| A1: tracked, trip generations (`brd2_tj132_6`) | +0.0064 (3) | +0.0019 (2); against the regime with trip generations -0.0037 (1) | -0.0066 (1) | 0.0269 |
| A1: regime, trip generations (`brd2_rj132_6`) | +0.0122 (5) | +0.0056 (3) | +0.0056 (3) | 0.0462 |
| A2: tracked, statistic test off (`brd2_tk132_6`) | **+0.0008 (2)** | -0.0010 (2); against the regime with the test off -0.0007 (2) | **-0.0081 (1)** | **0.0163** |
| A2: regime, statistic test off (`brd2_rk132_6`) | +0.0013 (2) | -0.0018 (0) | -0.0018 (0) | 0.0175 |
| B1: regime without G01 (`brd2_rn132_6`) | -0.0012 (2) | -0.0021 (1) | -0.0021 (1) | 0.0173 |
| tracked against B1 | | +0.0075 (6), against the regime without G01 | | |

Per site, the tracked arm against the regime (t - r), the same with the test off in both (tk - rk), what turning the
test off does to the tracked arm (tk - t), and the G01 pair, m:

| site | t - r | tk - rk | tk - t | t - rn (no G01) | rn - r |
| --- | --- | --- | --- | --- | --- |
| ARHT00ATA | +0.789 | +0.885 | +0.091 | +0.903 | -0.115 |
| UTQI00USA | +0.092 | +0.101 | +0.007 | -0.010 | +0.102 |
| HARB00ZAF | +0.067 | -0.002 | -0.072 | +0.067 | -0.001 |
| WUH200CHN | +0.043 | -0.001 | -0.046 | +0.045 | -0.002 |
| KOKB00USA | +0.024 | +0.002 | -0.024 | +0.033 | -0.009 |
| POHN00FSM | +0.023 | -0.018 | -0.039 | +0.034 | -0.011 |
| BADG00RUS | +0.021 | +0.002 | -0.021 | +0.023 | -0.002 |
| DAV100ATA | +0.019 | -0.003 | -0.018 | +0.006 | +0.013 |
| CEDU00AUS | +0.013 | +0.004 | -0.010 | +0.012 | +0.001 |

The readings of section 4:

- **A1: carried by the bridge.** With trip generations in both arms the remainder is -0.0037 m (bar: at most
  +0.005). A decoupled satellite's keys start a generation instead of bridging (78 and 96 trip generations, 0
  bridges), so the step the bridge would leave is absorbed by the users' re-floated ambiguities. Trip generations help
  the tracked arm (-0.0066 m) and cost the regime on CODE (+0.0056 m at the census, +0.019 m at WTZZ00DEU), where the
  bridge is harmless and the refloats are the whole effect; against the GPS golden the tracked arm with them still
  fails the day bars (+0.0064 m census, +0.0065 m at WTZZ00DEU). Gate J's condition is not met; J was not run.
- **A2: carried by the decouplings.** With the anchor's statistic test disarmed in both arms (0 decouplings, 0
  bridges) the remainder is -0.0007 m, and at the seven sites that set the census median it vanishes site by site
  (tk - rk within 0.004 m, POHN00FSM -0.018 m). Disarming the test costs the regime on CODE nothing (-0.0018 m, no site
  moves by 1 cm) and moves the tracked arm by -0.0081 m (8 better, 1 worse): on DOY 132 the tracked posture with the
  test off passes the bars of D against the GPS golden (+0.0008 m census, -0.0040 m at WTZZ00DEU, 2 of 16 sites
  worse).
- **B1: G01 carries part of it.** Against the regime without G01 the remainder is +0.0075 m (6 of 16 worse), between
  the bars. G01 is worth +0.102 m to UTQI00USA (which never uses it in hours 2 to 6) and +0.013 m to DAV100ATA (which
  uses it 36 % of the time); at most other sites its absence helps the regime (QUIN00USA -0.034, ARHT00ATA -0.115 m),
  and the census median moves by -0.0021 m. G01's absence redistributes site by site; the decouplings are the
  systematic part.
- **ARHT00ATA** diverges on every broadcast-referenced arm (+0.79 to +0.90 m against the regime) with the test on or
  off and whatever the regime's satellite set; neither carrier explains it. On DOY 130 it is again the site most worse
  (+0.075 m). Owed.

The mechanism is that of section 2.4: before a decoupling the clone's passed motion is decimetres on the tracked a
priori, and the bridge passes only the a priori's motion for the five bridged epochs. Measured against CODE, the
dropped motion predicts the step of the phase-visible clock with a correlation of 0.56 over the 26 decoupled
satellite-events (`bridge_drop.py`; rms predicted 14.5 mm, measured 13.0 mm).

Correction to section 2.4: its step table de-commoned the levels per epoch, so a satellite rising or setting moved the
median of satellites with different constants. De-commoning the changes over the window instead (`step_table.py` as now
in the tools folder): decoupled satellites median 7.4 mm, mean 11.6, max 32.7 (26 events); satellites only the regime
decoupled 4.5, 4.7, 9.8 (11); control 0.6, 1.0, 10.4 (125). The conclusion of section 2.4 stands.

### 5.4 L1 and L2: the phase-clock hold on the tracked a priori

| day | arm | E, C and B records against the tracked arm | phase-visible wander 1 / 10 / 40 / 120 epochs (mm) | WTZZ00DEU hours 2 to 6 | census against the GPS golden |
| --- | --- | --- | --- | --- | --- |
| 129 | L1, q 5e-6 (`brd2_tl129_6`) | identical (88 618 records); P records differ | 3.6 / 13.8 / 43.8 / **112.0** | 0.3342 (+0.3115) | +0.4699 (15 of 16 worse) |
| 129 | L2, q 2.9e-8 (`brd2_tlq129_6`) | C records differ | 3.6 / 13.8 / 43.8 / **112.0** | 0.3342 (+0.3115) | +0.4527 (15 worse) |
| 132 | L1, q 5e-6 (`brd2_tl132_6`) | identical (88 397 records); P records differ | 4.0 / 15.0 / 46.8 / **116.1** | 0.6114 (+0.5911) | +0.5182 (16 worse) |
| 132 | L2, q 2.9e-8 (`brd2_tlq132_6`) | C records differ | 4.0 / 15.0 / 46.8 / **116.1** | 0.6119 (+0.5916) | +0.4603 (16 worse) |

(i) passes on both days: the hold is emit-side, and the new exe's solve is the tracked arm's. (ii) fails on both days
(112.0 and 116.1 mm against bars of 10.2 and 17.6 mm), equal to the prediction of section 2.5 to the 0.1 mm. (iii)
fails by an order of magnitude; against the smoothed hold on the same solve the census loses +0.458 m (DOY 129) and
+0.446 m (DOY 132). The anchor's q does not reach the users under this hold: L2 against L1 is +0.0000 m at the census on
both days. **L1 FAIL, L2 FAIL**: the phase-clock hold cannot replace the smoothed hold on the tracked a priori, at either
q. The run-end line states the mode on every L arm (60 910 held emissions moved by the clock transfer on DOY 132).
Section 3 announced a new counter summing the track's motion carried into held phase-visible clocks; it was not built.
Under this hold every increment of the track reaches every held key of its satellite, so the track's own run-end ladder
(its increments by size) is that motion, and the new line states the mode beside the hold's own count of moved
emissions.

### 5.5 B2: the anchor's statistic test off on all four days

The tracked posture and the regime on CODE with `network.clock_anchor_k` 1e6 (`brd2_tk*_6`, `brd2_rk*_6`; DOY 132 is
A2's pair); 0 decouplings and 0 bridges on every arm. Census paired median over hours 2 to 6 (sites worse):

| day | tracked, test off: WTZZ00DEU hours 2 to 6 (against the golden) | census against the GPS golden | against the tracked arm with the test on | the regime, test off against on | tracked against the regime, both off |
| --- | --- | --- | --- | --- | --- |
| 129 | 0.0151 (-0.0077) | +0.0037 (3) | +0.0000 (0) | +0.0000 (0) | -0.0028 (1) |
| 130 | 0.0300 (-0.0188) | +0.0024 (4) | -0.0001 (0); WTZZ00DEU -0.0087 | -0.0001 (0); WTZZ00DEU -0.0153 | -0.0011 (1) |
| 131 | 0.0303 (+0.0032) | +0.0029 (2) | -0.0000 (0); WTZZ00DEU +0.0054 | +0.0000 (0); WTZZ00DEU +0.0010 | -0.0017 (2) |
| 132 | 0.0163 (-0.0040) | +0.0008 (2) | -0.0081 (1); WTZZ00DEU -0.0124 | -0.0018 (0); WTZZ00DEU -0.0093 | -0.0007 (2) |
| pooled | | **+0.0021 (11 of 64, 17 %)**, hour 1 -0.0014 | | | -0.0011 (6 of 64, 9 %) |

Every bar of D holds on every day (WTZZ00DEU at most +0.0032 m, census at most +0.0037 m) and the pooled share is 17 %
against a bar of 20 %. **B2 PASS.** On the days with few decouplings (DOY 129: 1, 130: 7, 131: 2 on the tracked arm)
the test off changes the census by at most 0.0001 m; the regime on CODE is indifferent to it on every day. The regime's
own cost against the golden, pooled over the four days with the test on: +0.0045 m, 9 of 64 site-days (14 %) worse.

## 6. Verdict

**The DOY-132 remainder is the clock anchor's statistic decouplings, passed to users through the smoothed hold's
bridge. Card LAH's phase-clock hold cannot replace the smoothed hold on the tracked a priori. DOY 130 and 131 pass the
day bars; over the four days gate D fails only its pooled share.**

1. **The days.** With card BRD's two knobs, DOY 130 and 131 pass the day bars against the GPS golden (WTZZ00DEU -0.0101
   and -0.0022 m, census +0.0047 and +0.0024 m). Pooled with DOY 129 and 132 the census paired median is +0.0063 m and
   25 % of the 64 site-days are worse by 2 cm (bar 20 %): **D FAIL** on the pooled clause. Against the same estimator
   on the CODE a priori the broadcast clock costs +0.0015 m pooled; only DOY 132 (+0.0160 m) carries a cost.
2. **The remainder.** DOY 132's network trips the anchor's statistic test at seven epochs, station events that both
   regimes share (BSHM00ISR's re-entries among them): 26 satellite-events on the tracked arm. At each, the smoothed
   hold's clone dies and the key bridges for five epochs on the a priori's own motion. On the tracked a priori the clock
   correction moves centimetres in those epochs, so the decoupled satellites' phase-visible clocks step (median 7.4 mm,
   up to 33 mm, against 0.6 mm for the others) and the census sites that track them lose from the event on (HARB00ZAF
   +0.067, WUH200CHN +0.043 m, KOKB00USA, POHN00FSM, BADG00RUS, DAV100ATA and CEDU00AUS +0.013 to +0.024 m). With the
   test disarmed in both arms the remainder is -0.0007 m and vanishes site by site (A2); a generation at each trip also
   removes it (-0.0037 m, A1) at a refloat cost on the CODE regime (+0.0056 m). On the CODE a priori the test and the
   bridge cost nothing, because the bridge passes a precise a priori's motion; the anchor's q does not carry the
   remainder (card BRD section 6.9). G01, flagged unhealthy in the broadcast navigation file and absent from every
   broadcast-referenced wire, moves single sites both ways (UTQI00USA +0.10 m, QUIN00USA -0.034 m) and is not the
   systematic part (+0.0075 m with equal satellite sets, B1). ARHT00ATA's divergence on the broadcast-referenced wires is
   neither and is owed.
3. **The candidate posture.** The tracked posture with the anchor's statistic test off passes every bar of gate D on all
   four days (B2): census against the GPS golden +0.0037, +0.0024, +0.0029 and +0.0008 m, WTZZ00DEU -0.0077, -0.0188,
   +0.0032 and -0.0040 m, pooled +0.0021 m with 17 % of site-days worse, and -0.0011 m against the regime on CODE with
   the test off. It is measured, not field-runnable: with the test off, the anchor's remaining jump responses are the
   gap, datum and absolute-backstop decouplings and the IF-TD satellite clock event, and under the tracked a priori a
   real satellite clock step is followed through the IF-TD column. Standing invariant 1 requires that response tested on
   an injected satellite clock step before any field run: the deferral is field-blocking.
4. **The phase-clock hold on the tracked a priori** (`network.apriori_clock_track_phase_clock_hold`): its phase-visible
   clock is the track, a sum of per-epoch IF-TD columns that walks 112 and 116 mm per 120 epochs against CODE (the
   prediction from the tracked arm's own records, exact to 0.1 mm). Users lose 0.31 to 0.59 m at WTZZ00DEU and 0.45 to
   0.52 m at the census against the GPS golden, at the posture's anchor q and at the golden's. **L1 and L2 FAIL**;
   ledger KEEP_OFF.

Nothing is armed; arming, any golden change and the merge are the author's decisions.

## 7. What remains

- **The satellite clock step on the candidate posture.** An injected satellite clock step (cards SCK and SCR; the clock
  step scenario of card SMX's matrix) on the tracked posture with `network.clock_anchor_k` 1e6, before any field run of
  it; then 24 h runs and DOY 128.
- **A bridge for the tracked a priori**, the alternative to disarming the test: keep the clone across a decoupling or
  pass the correction's own motion on the bridge, measured against B2.
- **Unhealthy broadcast satellites.** The broadcast posture drops a satellite the navigation file flags unhealthy (G01,
  health 63, on every day graded), which the CODE product keeps; whether to admit it on its broadcast record is open.
- **ARHT00ATA** diverges on every broadcast-referenced wire (+0.79 to +0.90 m against the regime on DOY 132, +0.075 m
  on DOY 130) and on no wire of the regime on CODE; its mechanism is owed.
- From card BRD and unchanged here: GPS + Galileo (the inter-constellation level), a real-time run, and the regime's own
  cost against the golden (+0.0045 m pooled over the four days, 14 % of site-days worse).
