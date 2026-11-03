# BRD: a broadcast-referenced network level (independence from the CODE clock product)

**STATUS**

- **id:** BRD
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered 2026-11-02, closed 2026-11-03. The broadcast-referenced posture's cost against the CODE-anchored golden reproduced (+0.021 and +0.027 m at the held-out receiver, GPS), traced to two mechanisms and cut by two default-off knobs (two-way IF-TD centring; the tracked a priori): DOY 129 passes the user bars, DOY 132 fails them; GPS + Galileo fails unless the receiver re-estimates its inter-system offset every epoch. Nothing armed.
- **source:** [NBP](NBP_native_broadcast_clock_posture_2026-10-07.md) sections 10 to 12 (the cost, +0.017 m); [SCL](SCL_self_contained_clock_level_2026-10-03.md), [SCL2](SCL2_level_residue_under_the_clone_2026-10-06.md), [SCL2B](SCL2B_shared_history_with_the_hold_2026-10-07.md) (the posture); [SCK](SCK_sat_clock_step_2026-10-25.md), [SCR](SCR_sat_clock_step_response_2026-10-26.md) (the satellite clock event rule); `docs/cards/raim-qc/LASER-THEORY-STUDY.md` (the one-way IF-TD finding, untested until this card)
- **knobs / posture:** new, default off: `network.cycle_slip.if_phase_td_sat_centre` with `network.cycle_slip.if_phase_td_sat_centre_min_stations` (default 4); `network.apriori_clock_track` (requires `sp3_brdc_clock` and the two-way centring; fingerprinted); Galileo admitted under `network.apriori_source sp3_brdc_clock` (F/NAV records only). The broadcast-referenced posture as card NBP graded it: `network.apriori_source sp3_brdc_clock`, `network.brdc_select ttom`, `network.clock_anchor_q` 5e-6, card SMH's smoothed hold in place of card LAH's phase-clock hold, B1, B2, the keep, X2 at rank 1 with X2b, a data root without CLK files
- **dates in the text:** 2026-11-02, 2026-11-03
- **hashes / run dirs:** base exe `gnss_processor_brd0.exe` (main `284ec29`, sha256[:16] `c32463d86934b2d3`); `gnss_processor_brd1.exe` (`f5405e8afa095409`, the two-way centring), `brd2` (`bcfb8a652be01ed2`, with the Galileo admission), `brd3` (`2622938d0de8270f`, with the tracked a priori), `brd4` (`8084428d737f95ed`, rows per station and constellation: the committed source), all Release from the worktree `D:/GnssProcessorSupport/worktrees/brdc`; arms `D:/GnssProcessorSupport/runs_nom/_rap_brd_*`; census replays `_rap_<key><doy>_<site>` with keys `brdg`, `brdn`, `brdnc`, `brdnt`, `brdr`, `brdc`, `brdcc`, `brdngb`, `brdngt`, `brdcI`, `brdngtI`; grading scripts in `D:/GnssProcessorSupport/worktrees/brdc_tools/` (outside the repository); day configs `D:/GnssProcessorSupport/runs_nom/_brd_cfg/` (`scripts/mday_golden_config.py` on the current golden); data roots `runs/_scl_data_nbp` (DOY 129) and `runs/_nbp_mday_root` (DOY 130 to 132)

**Outcome.** The broadcast-referenced posture lost 0.021 and 0.027 m at the held-out receiver (GPS, hours 2 to 6, DOY 129
and 132). Two mechanisms, measured before building: the IF-TD slip test's one-network-median centring refloated satellites
at every station on real clock motion around the broadcast polynomial, and the clock time update took the broadcast
increment as its prior mean. Two default-off knobs (two-way centring; the tracked a priori) bring the receiver to -0.008
and +0.008 m and the census from +0.013 to +0.007 m pooled: DOY 129 passes, DOY 132 fails. GPS + Galileo fails unless the
receiver re-estimates its inter-system offset every epoch. Nothing armed.

## 1. The question

The network takes the CODE final clock as the a priori satellite clock and holds its phase-clock level to it (card LAH's hold,
valid only with a precise a priori). The self-contained replacement (cards SCL to NBP: SP3 orbits, the broadcast clock as the a
priori, the smoothed hold) costs +0.017 m at the user over hours 2 to 6 on DOY 130 to 132 (card NBP section 10), attributed to "the
broadcast clock's information" (section 12) without a mechanism. The question: where does the cost go, and can the smallest
mechanism in the seam that owns it close it, so that a network referenced to the broadcast ephemeris, with SP3 orbits the only
precise external product, serves the user as well as the CODE-anchored configuration does?

## 2. Reproduction on the current build (2026-11-02)

The native broadcast posture is GPS only on the current code (the setup refuses another constellation), so the like-for-like
pair is the GPS golden (the field golden with `processing.active_systems` GPS) against the same plus card NBP's posture. The
GPS + Galileo golden (the CODE-anchored reference of the poster) is run beside it. DOY 129 and DOY 132 (card NBP's worst day at
the held-out receiver, +0.0265 m), the day configs regenerated from the current golden.

Results: section 6.

## 3. Diagnosis

The network's IF time-differenced slip test (`network.cycle_slip.if_phase_td`, `network/slip/SlipQc.cpp`) forms per station the
between-epoch IF phase double difference dd_i(s) = de_s - de_ref(i) + slip + noise, where de is the increment of the a priori
clock's error, and removes one network median per signal pair. With the CODE clock as the a priori, de is about a millimetre. With
the broadcast clock it is the broadcast polynomial's misfit to the real clock, and a real clock excursion of a few centimetres
reaches every station that sees the satellite. The project's LASER study recorded the hazard (one-way centring) on 2026-10-08 and
left it untested.

Evidence on the existing same-exe pair (`runs_nom/_rap_deck_gg` against `_rap_deck_nbp`, DOY 129, 6 h):

- IF-TD refloats: 67 pool flags and 2 reference-gate resets on the golden; 141 and 327 under the broadcast posture.
- Satellite-coherent flag groups (one satellite, one epoch, at least 3 stations): 0 on the golden, 7 holding 52 flags under the
  broadcast posture (G12 at 04:18:00 at 15 stations, residuals 0.116 to 0.126 m against the 0.107 m floor; the network's clock
  estimate of G12 moves +0.112 m in that epoch, the broadcast a priori is smooth there).
- Reference-gate resets cluster on three satellites (G05, G12, G16: 51 each).
- An offline replay of the pooled test with a two-way (station x satellite) median centring at the run's own thresholds
  (`brdc_tools/iftd_twoway_offline.py`): 141 flags become 70 and every coherent group disappears; the golden's 67 become 72.
- The held-out receiver sits on a 0.10 m plateau from minute 50 to 110 under the broadcast posture (golden 0.02 m from minute 50),
  with 6 to 28 refloats in every 10 minutes all run long against 0 to 5.
- The existing satellite clock event rule (`network.cycle_slip.if_phase_td_sat_clock_event`, cards SCK and SCR) does not act: 0
  events on both days at 2 h, the arms equal the broadcast arms to the last digit, because a broadcast misfit pushes only part of
  a satellite's stations over the threshold and the rule needs 75 %.

## 4. The mechanism

`network.cycle_slip.if_phase_td_sat_centre` (default off; `include/config/GnssConfigTypes.hpp`, loader
`GnssConfigLoadNetwork.cpp`): per epoch and signal pair, the pool's non-reference samples are median-polished as a station x
satellite table (`ifTdSatCentreFit`, `network/slip/SlipQc.hpp`; a satellite's column only with at least
`if_phase_td_sat_centre_min_stations` samples) and each sample enters the pooled test as dd - overall - row - column
(`ifTdSatCentred`), so the centre, the scale, the thresholds, the station-pair EWMA, the bridge pend and the satellite clock rule
read the residual. A reference-gate reset is re-tested with the reference satellite's own column (`ifTdSatCentreRefStep`: the
gate's step less median(columns of the station's satellites) minus the reference's column) against the gate's physical floor:
within it the arcs are kept, beyond it the ordinary refloat runs. The common-band scope's flag preview centres the same way. The
detector exports the gate's step per reset key (`IfPhaseSlipDetector::getLastRefFireMedians`). Events at both edges: a
`sat_centre` row names each satellite whose column reaches half the pair's physical floor (the satellite-common move, with its
size), a `refsat_centre_keep` row each kept reference arc; refloats keep their rows, with a `centre_step` token. Run-end line
(`networkIfTdSatCentreRunEnd`): pools centred, columns fitted and thin, the column-size histogram, flags removed and added
against the one-way centre at the same thresholds, and the reference re-test closure (tested = kept + refloated). Tests:
`tests/network/test_network_iftd_sat_centre.cpp`.

Galileo: `BrdcClockApriori::aprioriRecords` keeps GPS and Galileo F/NAV records (the (E1, E5a) clock, the pair the golden models
and pins); the setup admits GPS and Galileo; the consumer replay stages the same set. Tests: `tests/test_brdc_apriori_galileo.cpp`.

## 5. Gates (frozen 2026-11-02, before any mechanism run)

The broadcast posture is that of section 2 (card NBP's), with `network.cycle_slip.if_phase_td_sat_centre` true where named.
Hours from 00:00; hour 1 is ignition, hours 2 to 6 the settled read. Census: `scripts/clock_level/gcensus.py` replays (WTZZ00DEU
plus the 16 sites), paired per site against the named reference, the mean of the site's hourly p50 over hours 2 to 6.

| gate | bar |
| --- | --- |
| U0 | Debug `gnss_tests`: no new failure, the new suites pass; `make_golden_config.py --verify` PASS; `docs_index_check.py` passes. |
| G0 | Knob off: the 2 h field and sim goldens byte-identical to the base exe's (`products.txt` without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`). |
| G1, live | Broadcast posture with the knob, 2 h, DOY 129 and 132: pools centred > 0 every epoch after the first; IF-TD refloats (pool plus reference) at most 1.5 times the GPS golden's of the same day; satellite-coherent flag groups at most the golden's. |
| G2, the user (GPS) | Broadcast posture with the knob against the GPS golden, 6 h, each day: WTZZ00DEU mean change over hours 2 to 6 at most +0.005 m; census paired median over hours 2 to 6 at most +0.005 m, pooled over the two days (32 site-days) at most 20 % of site-days worse by more than 0.02 m. |
| G3, no harm on COD | The knob on the GPS + Galileo golden (COD a priori), 6 h, DOY 129: WTZZ00DEU within 2 mm in every hour 2 to 6; census paired median within 0.002 m. |
| G4, sim control | The knob on the sim golden (from 00:30:15), 6 h, same exe: SZZ000SIM within 2 mm in every hour 2 to 6; IF-TD refloats not above the knob-off run's. |
| G5, the user (GPS + Galileo) | Only with the Galileo admission built: the broadcast posture with Galileo and the knob against the GPS + Galileo golden, 6 h, each day: the bars of G2. |

Reported without a bar: hour 1 per arm; the refloat counts and the run-end line; the cost left after the mechanism, attributed by
the same one-change arms.

## 5b. Amendment A1: the tracked a priori (pre-registered 2026-11-02 23:10, before any run of it)

Section 6.3 found the slip-test knob live but the held-out receiver's hour-2 plateau unchanged, and section 6.4 measured
the mechanism behind the remaining cost: the clock's prior mean. A second knob is built before the 6 h grade of the first
is read: `network.apriori_clock_track` (default off; requires `sp3_brdc_clock` and the two-way centring, refused at load
otherwise; fingerprinted). Each epoch every satellite's track U moves by minus its IF-TD column (the pool with the most
non-reference samples), every modelled observation of the satellite is lowered by U after the PASS-A slip scans
(`network/assembly/AprioriClockTrack`), so the solve's clock time update starts from a mean that follows the network's own
phase; the wire adds U after the hold (`network/emit/BrdcWire`: C_wire = C_est + O + U), so a user reconstructs
brdc(iode) + C_wire = a + U + C_est. Events: `BRD_TRACK` `track_start` and `track_resume`; run-end line in the broadcast
wire's report; satellite fields `apriori_track_m`, `apriori_track_paused` (ColdStart, fingerprinted). Tests:
`tests/network/test_network_apriori_clock_track.cpp`.

| gate | bar |
| --- | --- |
| T0 | Knob off on the exe that carries it: the 2 h field and sim goldens byte-identical to the base exe's (as G0). |
| T1, live | The broadcast posture with both knobs, 2 h, both days: every epoch after the first moves the tracks (satellite-epochs moved > 0, modelled values moved > 0); every stamped record of a tracked satellite carries U on the wire; no exit other than 0. |
| T2, the user (GPS) | Both knobs against the GPS golden, 6 h, each day: the bars of G2. |
| T3, the user (GPS + Galileo) | Both knobs with Galileo against the GPS + Galileo golden, 6 h, each day: the bars of G2. |
| T4, mechanism | The phase-visible clock against CODE (section 6.4's statistic, DOY 129): its change over 120 epochs at most half the broadcast posture's 22.6 mm. |

## 6. Results

### 6.1 Reproduction on the current build (2026-11-02)

Exe `gnss_processor_brd0.exe` (`c32463d86934b2d3`), 6 h from 00:00, GPS. Held-out receiver WTZZ00DEU, 3D p50 by hour (m),
and the 16-site census (paired per site, the mean of the site's hourly p50 over hours 2 to 6; better or worse at 0.01 m):

| day | arm | WTZZ00DEU by hour | WTZZ hours 2 to 6 | census median by hour | census paired, hours 2 to 6 | hour 1 paired |
| --- | --- | --- | --- | --- | --- | --- |
| 129 | GPS golden (`brd_g129_6`) | 0.115 0.024 0.017 0.018 0.029 0.026 | 0.0227 | 0.451 0.106 0.040 0.052 0.042 0.043 | (reference) | |
| 129 | broadcast posture (`brd_n129_6`) | 0.228 0.095 0.028 0.025 0.031 0.042 | 0.0440 | 0.511 0.111 0.071 0.063 0.054 0.066 | +0.0049 (4 better, 6 worse) | +0.0151 |
| 132 | GPS golden (`brd_g132_6`) | 0.087 0.026 0.021 0.016 0.025 0.014 | 0.0203 | 0.211 0.066 0.050 0.038 0.046 0.031 | (reference) | |
| 132 | broadcast posture (`brd_n132_6`) | 0.129 0.027 0.034 0.079 0.050 0.049 | 0.0476 | 0.521 0.173 0.077 0.083 0.060 0.055 | +0.0233 (3 better, 11 worse) | +0.2687 |

The gap reproduces: at the held-out receiver +0.021 m (DOY 129) and +0.027 m (DOY 132) over hours 2 to 6, on the census
+0.005 and +0.023 m.

### 6.2 Diagnosis, measured before building

- Section 3's evidence repeats on the current build: IF-TD refloats 141 + 327 (pool + reference) against 67 + 2 on DOY 129,
  247 + 253 against 136 + 21 on DOY 132, with 7 and 9 satellite-coherent groups against 0. An offline two-way replay of the
  DOY-132 run: 247 flags become 121 (the golden's 136 become 122), no coherent group.
- Hypothesis b (cut-overs as datum events): refuted. Over 6 h of DOY 129, 104 satellite-epochs switch record; they carry 0
  IF-TD flags, 0 anchor trips, 1 of 332 reference resets and 3 of 106 phase-bias generation starts.
- The hold's generations: the same under both postures (88 to 90 in hour 1, then a handful); the broadcast posture adds 66
  coupled clock-and-bias marks and 18 hold bridges.
- The broadcast misfit (the CODE clock minus the continuous broadcast a priori, de-commoned): its 30 s increment has a root
  mean square of 17.5 mm (p99 67 mm). It is real satellite clock motion the polynomial cannot carry: G12's CODE clock steps
  +0.091 m in one 30 s interval at 04:18:00 against its -0.04 to -0.07 m drift (the 15-station coherent flag of section 3).
- The IF-TD column as a tracker of the misfit (`brdc_tools/column_tracks_misfit.py`): per epoch it is right to 3.0 mm rms
  (p99 10 mm); summed over 120 epochs its error reaches 89 mm (the misfit itself 74 mm), 63 mm against a white 52 mm when only
  samples above 30 degrees are fitted. Hypothesis d in the form of a summed a priori is therefore not viable: the column is a
  per-epoch quantity, which is how amendment A1 uses it.
- The phase-visible clock against CODE (the poster table's statistic, `presentation/agu26/figures/clock_table.py`, imported
  unchanged): STD over hours 2 to 6, per-satellite and per-generation constants removed, 1.97 cm (CODE-anchored 0.0 by
  construction; CODE against IGS final 0.8 cm).

### 6.3 The slip-test knob, G0 and G1 (2026-11-02)

Exe `gnss_processor_brd1.exe` (`f5405e8afa095409`, the two-way centring). G0: knob off, the 2 h field golden
(`brd_c1_129_2` against `brd_c129_2`) and sim golden (`brd_s1_2` against `brd_s0_2`): `products.txt` without comments,
`closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical; of 34 trace files three differ, in wall-clock columns
only (`ms`, the run time). **G0 PASS.**

G1, 2 h:

| day | arm | IF-TD refloats, pool + reference | coherent groups | WTZZ00DEU hours 1 / 2 (m) |
| --- | --- | --- | --- | --- |
| 129 | GPS golden | 41 + 0 | 0 | 0.115 / 0.024 |
| 129 | broadcast posture | 59 + 91 | 1 | 0.228 / 0.095 |
| 129 | broadcast posture, two-way centring (`brd_nc129_2`) | 39 + 5 (88 references kept) | 0 | 0.224 / 0.092 |
| 132 | GPS golden | 55 + 6 | 0 | 0.087 / 0.026 |
| 132 | broadcast posture | 71 + 77 | 1 | 0.129 / 0.027 |
| 132 | broadcast posture, two-way centring (`brd_nc132_2`) | 62 + 9 (75 kept) | 0 | 0.177 / 0.308 |

478 pools centred per 2 h (two GPS pair kinds, every epoch after the first); DOY 129: 11 351 columns fitted, 2 481 thin;
|column| below 5 mm 6 276, 5 to 10 mm 1 762, 10 to 20 mm 1 505, 20 to 50 mm 1 512, 50 to 100 mm 284, at or above 100 mm 12
(largest 0.127 m); 209 flags removed and 1 added against the one-way centre at the same thresholds; references re-tested 89 =
88 kept + 1 refloated. Refloats 1.07 and 1.16 times the golden's, coherent groups 0. **G1 PASS.**

The held-out receiver does not follow: its DOY-129 hour-2 plateau stays (0.092 against 0.095 m), and on DOY 132 the knob arm
loses 0.31 m in hour 2. That loss is one event: at 00:48:00 the clock anchor's phase guard decouples four satellites at once
at |w| 6.03 to 6.26 (threshold 6; the same phase-space residual of about 1.6 m on all four) and the smoothed hold withdraws its
bridge with transfers of 0.10 to 0.32 m; four of the receiver's satellites carry the mark and it refloats them. Without the
knob the guard trips at the same epoch on five other satellites, which the receiver does not track. Both postures trip the
guard on several satellites at the same epochs (the golden more often and harder, |w| 10 to 75 on DOY 132). A single
receiver's 2 h read which satellites such a trip lands on; the census decides.

### 6.4 The mechanism of the remaining cost (2026-11-02)

The phase-visible clock against CODE, its change over a lag within one phase-bias generation, de-commoned, hours 2 to 6, DOY
129 (`brdc_tools/pv_lags.py`, on the poster table's segments):

| arm | 1 epoch | 10 epochs | 40 epochs | 120 epochs |
| --- | --- | --- | --- | --- |
| broadcast posture (`brd_n129_6`) | 4.6 mm | 8.5 | 13.7 | 22.6 |
| the same regime on the CODE a priori (card NBP's `_rap_nbpa_129_c`) | 2.0 | 3.9 | 5.2 | 6.7 |
| the same, G01 removed (`_rap_nbpa_129_c0`) | 2.0 | 3.9 | 5.2 | 6.7 |
| GPS golden (`brd_g129_6`, the phase-clock hold) | 0.0 | 0.0 | 0.0 | 0.1 |

The broadcast posture fits about 2.9 mm of white noise plus a random walk of 2.0 mm per epoch (22 mm per hour). The same
estimator and regime on the CODE a priori wanders 3.4 times less at one hour: the a priori's increments, which the clock's time
update takes as its prior mean, decide it. The broadcast increment is wrong by the misfit (17.5 mm per epoch), the solve
removes most but not all of it each epoch, and the remainder accumulates in the level the smoothed hold passes to the user.

### 6.5 The slip-test knob at 6 h: G2, G3, G4 (2026-11-03)

| day | arm | WTZZ00DEU by hour (m) | WTZZ hours 2 to 6 | census paired, hours 2 to 6 | worse by > 2 cm | hour 1 paired |
| --- | --- | --- | --- | --- | --- | --- |
| 129 | two-way centring (`brd_nc129_6`) | 0.224 0.092 0.025 0.020 0.028 0.029 | 0.0385 (+0.0158) | +0.0013 (4 better, 6 worse) | 5 (31 %) | +0.0138 |
| 132 | two-way centring (`brd_nc132_6`) | 0.177 0.308 0.181 0.161 0.255 0.211 | 0.2231 (+0.2028) | +0.0376 (2 better, 11 worse) | 9 (56 %) | +0.2744 |
| pooled | | | | +0.0131 | 14 of 32 (44 %) | +0.1206 |

Against the GPS golden; refloats 81 + 41 and 164 + 42 (pool + reference) against 141 + 327 and 247 + 253 without the
knob, the golden's 67 + 2 and 136 + 21. **G2 FAIL.** On DOY 132 the receiver loses its float solution at 00:48:00
(section 6.3) and keeps a 0.2 to 0.3 m error for five hours; the census follows (11 of 16 worse). The knob removes the false
refloats and nothing else; with the smoothed hold's bridge transfers at the guard's multi-satellite trips it can make a day
worse.

G3, the knob on the CODE-anchored GPS + Galileo golden, DOY 129, 6 h (`brd_cc129_6` against `brd_c129_6`): WTZZ00DEU equal
to the millimetre in every hour (0.059 0.012 0.023 0.012 0.017 0.012 m); census paired median +0.0000 m, no site moves by
1 cm; refloats 105 + 3 against 107 + 3. **G3 PASS.**

G4, the knob on the sim golden from 00:30:15, 6 h (`brd_sc_6` against `brd_s_6`, same exe): `products.txt`,
`closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical (only the IF-TD trace's centre column and the
wall-clock files differ); SZZ000SIM 0.0994 0.0326 0.0398 0.0160 0.0180 0.0152 m in both; 0 refloats in both. **G4 PASS.**

The slip-free sim with the broadcast a priori (GPS, `brd_sn_6` against `brd_snc_6`): 83 + 356 refloats, every one false,
with 12 satellite-coherent groups, become 0 + 0 (365 reference resets kept).

### 6.6 The tracked a priori (amendment A1): T0, T1, T2, T4 (2026-11-03)

Exe `gnss_processor_brd3.exe` (`2622938d0de8270f`; both knobs and the Galileo admission).

T0: both knobs off, the 2 h field golden (`brd_c3_129_2`) and sim golden (`brd_s3_2`) against the base exe's
(`brd_c129_2`, `brd_s0_2`): `products.txt` without comments, `closed_loop_trace.csv` and `network_epoch_status.csv`
byte-identical; of 34 trace files three differ, in wall-clock columns only. **T0 PASS.**

T1, 2 h (`brd_nt129_2`, `brd_nt132_2`): 240 epochs; DOY 129: 7 206 satellite-epochs moved by a column, 139 held, 31
satellites started, 3 resumptions; |increment| below 5 mm 4 123, 5 to 10 mm 1 054, 10 to 20 mm 922, 20 to 50 mm 939, 50 to
100 mm 162, at or above 100 mm 6; largest |U| 0.560 m; 526 951 modelled values moved; 7 262 stamped records carried U on the
wire, exactly the stamped records of tracked satellites (7 357 stamped, 95 of them before a satellite's first column).
DOY 132: 7 169 moved, 201 held, 509 041 values, 7 225 of 7 225. Exit 0 on both. **T1 PASS.**

T2, 6 h, against the GPS golden (and the posture's two other states):

| day | arm | WTZZ00DEU by hour (m) | WTZZ hours 2 to 6 | census median by hour | census paired, hours 2 to 6 | worse by > 2 cm | hour 1 paired |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 129 | GPS golden | 0.115 0.024 0.017 0.018 0.029 0.026 | 0.0227 | 0.451 0.106 0.040 0.052 0.042 0.043 | | | |
| 129 | broadcast posture | 0.228 0.095 0.028 0.025 0.031 0.042 | 0.0440 | 0.511 0.111 0.071 0.063 0.054 0.066 | +0.0049 | 5 (31 %) | +0.0151 |
| 129 | both knobs (`brd_nt129_6`) | 0.097 0.014 0.015 0.010 0.020 0.017 | **0.0151** | 0.355 0.080 0.054 0.053 0.054 0.053 | +0.0044 (4 better, 4 worse) | 3 (19 %) | -0.0054 |
| 132 | GPS golden | 0.087 0.026 0.021 0.016 0.025 0.014 | 0.0203 | 0.211 0.066 0.050 0.038 0.046 0.031 | | | |
| 132 | broadcast posture | 0.129 0.027 0.034 0.079 0.050 0.049 | 0.0476 | 0.521 0.173 0.077 0.083 0.060 0.055 | +0.0233 | 8 (50 %) | +0.2687 |
| 132 | both knobs (`brd_nt132_6`) | 0.092 0.022 0.024 0.030 0.036 0.032 | **0.0288** | 0.236 0.085 0.065 0.044 0.048 0.039 | +0.0101 (1 better, 9 worse) | 5 (31 %) | +0.0029 |
| pooled | broadcast posture | | | | +0.0134 | 13 of 32 (41 %) | +0.1196 |
| pooled | both knobs | | | | **+0.0068** | **8 of 32 (25 %)** | **-0.0029** |

The bars: WTZZ00DEU at most +0.005 m each day (DOY 129 -0.0076 passes, DOY 132 +0.0084 fails); census median at most +0.005
m each day (+0.0044 passes, +0.0101 fails); at most 20 % of the pooled site-days worse by more than 0.02 m (25 % fails).
**T2 FAIL**, on DOY 132. The tracked a priori halves the pooled cost (+0.0134 to +0.0068 m), cuts the share of site-days
worse by 2 cm from 41 % to 25 %, and removes the hour-1 cost (+0.120 to -0.003 m).

T4, the phase-visible clock against CODE (DOY 129, section 6.4's statistic):

| arm | 1 epoch | 10 epochs | 40 epochs | 120 epochs | phase-visible STD against IGS final (poster table) |
| --- | --- | --- | --- | --- | --- |
| broadcast posture | 4.6 mm | 8.5 | 13.7 | 22.6 | 2.02 cm |
| both knobs (`brd_nt129_6`) | 2.2 | 4.0 | 5.3 | **6.8** | **0.90 cm** |
| the regime on the CODE a priori (`brd_r129_6`) | 2.0 | 3.9 | 5.3 | 6.9 | 0.89 cm |
| GPS golden (phase-clock hold, CODE by construction) | 0.0 | 0.0 | 0.0 | 0.1 | 0.73 cm |

On the sim, against the true clocks: broadcast posture 4.4 / 8.0 / 13.1 / 21.8 mm, with the slip-test knob alone 4.8 / 8.6 /
13.7 / 22.1, with both knobs 1.6 / 3.3 / 4.2 / **4.7**. **T4 PASS** on both beds (6.8 and 4.7 mm against bars of 11.3 and
10.9): with the tracked a priori the broadcast-referenced clock wanders exactly as the same estimator does on the CODE a
priori.

### 6.7 Attribution on the current build (2026-11-03)

The chain golden (g) -> the posture's regime on the CODE a priori (r: q 5e-6, card SMH's smoothed hold, B1, B2, the keep, X2
with X2b; `brd_r129_6`, `brd_r132_6`, the exe of the reproduction) -> the broadcast posture with both knobs (t). Census
paired median over hours 2 to 6 (share of site-days worse by more than 0.02 m), held-out receiver hours 2 to 6:

| step | DOY 129 | DOY 132 | pooled | WTZZ00DEU 129 / 132 |
| --- | --- | --- | --- | --- |
| r - g: the regime | +0.0041 (19 %) | +0.0054 (12 %) | +0.0049 (16 %) | -0.0057 / +0.0065 |
| t - r: the broadcast clock, tracked | -0.0019 (6 %) | +0.0160 (44 %) | +0.0007 (25 %) | -0.0019 / +0.0020 |
| n - r: the broadcast clock, untracked (for comparison) | +0.0010 (25 %) | +0.0166 (38 %) | +0.0110 (31 %) | +0.0270 / +0.0208 |
| t - g | +0.0044 (19 %) | +0.0101 (31 %) | +0.0068 (25 %) | -0.0076 / +0.0084 |

On DOY 129 the tracked broadcast posture equals the same estimator on the CODE a priori (census -0.0019 m, the receiver
-0.0019 m, the phase-visible wander 6.8 against 6.9 mm per hour): the broadcast clock's information cost is gone there, and
what is left against the golden is the regime's (+0.004 m). On DOY 132 a broadcast cost remains at the census (+0.016 m
against the regime on CODE) though not at the held-out receiver (+0.002 m): ARHT00ATA (+0.78 m: the tracked arm diverges
where the golden is already at 0.16 to 0.61 m), UTQI00USA (+0.11 m), and the four C1W sites (HARB00USA +0.077, POHN00FSM
+0.031, KOKB00USA +0.030, DAV100ATA +0.014 m), the family where card SMH's hold already loses on the CODE a priori.
DOY 132 has more clock-anchor decouplings than DOY 129 in every arm (golden 66 against 32, the regime on CODE 33 against 3,
tracked 26 against 1); at each one the smoothed hold withdraws its bridge with a transfer (median 0.22 m on the regime on
CODE, 0.29 m tracked). Which of these carries the DOY-132 remainder is not settled tonight.

Hour 1, pooled: r - g +0.0087, n - r +0.1149, t - r -0.0085 m: the tracked a priori removes the broadcast posture's ignition
cost entirely.

### 6.8 GPS + Galileo: G5 and T3 (2026-11-03)

Galileo admitted under the broadcast a priori (section 4, F/NAV records). Exes: `gnss_processor_brd2.exe`
(`bcfb8a652be01ed2`) for the 2 h arms of the slip-test knob alone, `gnss_processor_brd3.exe` for the 6 h arms; the GPS +
Galileo golden from the base exe (knob off is byte-identical, T0). Receivers as the golden configures them:

| day | arm | WTZZ00DEU by hour (m) | WTZZ hours 2 to 6 | census median by hour | census paired, hours 2 to 6 | worse by > 2 cm | hour 1 paired |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 129 | GPS + Galileo golden (`brd_c129_6`) | 0.059 0.012 0.023 0.012 0.017 0.012 | 0.0152 | 0.142 0.050 0.036 0.029 0.032 0.030 | | | |
| 129 | broadcast posture with Galileo (`brd_ngb129_6`) | 0.301 0.061 0.032 0.039 0.034 0.101 | 0.0535 | 0.340 0.084 0.072 0.057 0.055 0.125 | +0.0319 (0 better, 13 worse) | 12 (75 %) | +0.0825 |
| 129 | both knobs (`brd_ngt129_6`) | 0.107 0.168 0.221 0.176 0.130 0.169 | 0.1729 | 0.286 0.144 0.396 0.428 0.201 0.176 | +0.2058 (1 better, 14 worse) | 14 (88 %) | +0.0872 |
| 132 | GPS + Galileo golden (`brd_c132_6`) | 0.072 0.019 0.014 0.017 0.019 0.009 | 0.0156 | 0.118 0.068 0.051 0.036 0.035 0.028 | | | |
| 132 | broadcast posture with Galileo (`brd_ngb132_6`) | 0.173 0.023 0.051 0.065 0.054 0.019 | 0.0423 | 0.336 0.117 0.071 0.077 0.091 0.079 | +0.0432 (1 better, 13 worse) | 13 (81 %) | +0.1513 |
| 132 | both knobs (`brd_ngt132_6`) | 0.213 0.239 0.100 0.084 0.048 0.140 | 0.1220 | 0.312 0.248 0.226 0.133 0.107 0.071 | +0.1158 (0 better, 15 worse) | 15 (94 %) | +0.0788 |
| pooled | broadcast posture with Galileo | | | | +0.0376 | 25 of 32 (78 %) | +0.0915 |
| pooled | both knobs | | | | +0.1430 | 29 of 32 (91 %) | +0.0872 |

**T3 FAIL** on both days, far beyond the bars. **G5** (the slip-test knob alone with Galileo) ran 2 h only
(`brd_ngc129_2`, `brd_ngc132_2`: WTZZ00DEU hour 2 0.125 and 0.295 m against the golden's 0.012 and 0.019 m); once
amendment A1 made both knobs the graded posture its 6 h arms were not spent. G5 is not graded at 6 h; its 2 h read fails the
receiver bar.

The cost sits at the receiver. The receiver model carries its GPS-to-Galileo time offset with
`filter.process_noise.system_time_offset_q` 1e-12, frozen once converged: right when both constellations' satellite clocks
sit on one time scale, as CODE's do. Under the broadcast a priori each constellation's clock level is held by its own datum
(one pivot satellite per constellation), and the difference between the two levels moves from epoch to epoch. The held-out
receiver's estimated offset, its change over one epoch, DOY 129: rms 1.1 mm (p99 2.8) on the golden, 3.1 mm (p99 14.9) on
the broadcast posture, 5.7 mm (p99 6.6) with both knobs. A frozen offset cannot follow; the misfit goes to the position.
Why the tracked a priori raises the jitter is not measured; a likely reading is that each pivot now carries its own
column's per-epoch error (about 3 mm, section 6.2). The census replayed with the offset re-estimated every epoch
(`filter.process_noise.system_time_offset_q` 1e-3, a receiver setting only; census keys `brdcI`, `brdngtI`):

| day | arm | WTZZ00DEU by hour (m) | WTZZ hours 2 to 6 | census median by hour | census paired, hours 2 to 6 | worse by > 2 cm | hour 1 paired |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 129 | golden, free offset | 0.057 0.013 0.023 0.014 0.019 0.018 | 0.0172 | 0.176 0.058 0.037 0.030 0.033 0.030 | (reference) | | |
| 129 | both knobs, free offset | 0.066 0.010 0.023 0.007 0.011 0.012 | **0.0129** | 0.226 0.048 0.033 0.036 0.040 0.039 | +0.0027 (2 better, 5 worse) | 4 (25 %) | +0.0223 |
| 132 | golden, free offset | 0.063 0.013 0.016 0.018 0.021 0.009 | 0.0153 | 0.137 0.069 0.050 0.036 0.035 0.028 | (reference) | | |
| 132 | both knobs, free offset | 0.076 0.062 0.069 0.030 0.023 0.018 | 0.0406 | 0.152 0.088 0.060 0.050 0.054 0.036 | +0.0194 (1 better, 10 worse) | 8 (50 %) | +0.0470 |
| pooled | both knobs, free offset | | | | +0.0082 | 12 of 32 (38 %) | +0.0283 |

The free offset costs the golden nothing (census pooled +0.0007 m, 1 of 32 site-days worse by 2 cm; WTZZ00DEU +0.0020 and
-0.0003 m). Read against the bars of G2 (a diagnostic, not the pre-registered gate, which keeps the golden's receivers):
DOY 129 passes (receiver -0.0043 m, census +0.0027 m), DOY 132 fails (receiver +0.0253 m, census +0.0194 m), and the
pooled share of site-days worse by 2 cm is 38 %. Against the golden with its own receivers the free-offset arms read
+0.0034 and +0.0205 m at the census, -0.0023 and +0.0250 m at the receiver. A network-side alternative, the
per-constellation clock datum as a sum to zero (`network.clock_datum_sum_to_zero`), is refused at load on the golden's
posture (`network.clock_pivot_frame_q` takes the carried clocks' process noise in the pinned pivot's frame) and was not
pursued.

### 6.9 Supporting checks

- U0: Debug `gnss_tests` 3 858 tests in 527 suites: 3 825 passed, 33 skipped, 0 failed (the 25 new tests in 4 new suites
  among them; the base count was 3 833 in 523); `gnss_simulator_selftest` 99 of 99; `make_golden_config.py --verify`
  PASS; `docs_index_check.py` passes.
- The rows per (station, constellation) of the two-way centring (`ifTdSatCentreRow`, added after `gnss_processor_brd3.exe`
  was built): exe `gnss_processor_brd4.exe` (`8084428d737f95ed`); the GPS + Galileo broadcast posture with both knobs, 2 h
  (`brd_ngt129_2`), against the first two hours of the brd3 run (`brd_ngt129_6`): `products.txt` without comment lines
  (104 922 rows), `closed_loop_trace.csv` and `network_epoch_status.csv` (240 epochs) equal. The graded postures never hold
  two constellations under one pair label (GPS L1C_L2W and L1W_L2W, Galileo L1C_L5Q and L1X_L5X), so the change acts only
  where they would.
- `nom_epoch_lint.py` PASS; `nom_n4_verify.py --entity satellite` V1 to V6 PASS, V7 flags the three lines of the new
  seam's row in `tests/network/test_network_epoch_contract.cpp` that the rules require for a new epoch writer (V7 is the
  entity-move check, which forbids contract changes during a move); `config_posture_check.py --verify-loader` 0 gaps.
- A diagnostic, not a gate: both knobs with the anchor at q 3e-7 (`brd_ntq129_6`, `brd_ntq132_6`), the q the tracked a
  priori's per-epoch error (about 3 mm) would ask for. WTZZ00DEU over hours 2 to 6: 0.0151 m on DOY 129 (unchanged) and
  0.0709 m on DOY 132 (against 0.0288 m at the posture's 5e-6). A stiffer anchor does not carry the DOY-132 remainder; it
  makes that day worse.

## 7. Verdict

**The gap is explained, and most of it is closed by two default-off knobs; the pre-registered user bars hold on DOY 129 and
fail on DOY 132.** The broadcast-referenced posture's cost against the CODE-anchored configuration had two causes, both
measured before any build: the IF-TD slip test centred on one network median, so real satellite clock motion around the
smooth broadcast polynomial refloated satellites at every station; and the clock's time update took the broadcast increment
as its prior mean, so the misfit the solve did not remove each epoch accumulated in the phase-visible clock the smoothed hold
passes to users (22.6 mm per hour against CODE, 6.9 with the CODE a priori). The two-way centring
(`network.cycle_slip.if_phase_td_sat_centre`) removes the false refloats (439 to 0 on the slip-free sim) and is harmless on
the CODE a priori (G3, G4). Alone it does not help the user (G2 FAIL). The tracked a priori
(`network.apriori_clock_track`) takes the centring's per-epoch column as the time update's control input: the
broadcast-referenced clock then wanders as the same estimator does on the CODE clock (6.8 mm per hour; on the sim 4.7 against
21.8 mm against the true clocks), hour 1 costs nothing (pooled +0.120 to -0.003 m), and the held-out receiver over hours 2 to
6 moves from +0.021 to -0.008 m (DOY 129) and from +0.027 to +0.008 m (DOY 132) against the GPS golden. The census halves
(+0.0134 to +0.0068 m pooled; 41 % to 25 % of site-days worse by 2 cm): DOY 129 passes (+0.0044 m, equal to the regime on
CODE), DOY 132 fails (+0.0101 m, carried by ARHT00ATA, UTQI00USA and the four C1W sites). With Galileo (T3) the receivers as
configured today fail on both days (census +0.206 and +0.116 m): their GPS-to-Galileo offset, frozen at q 1e-12, cannot
follow the inter-constellation level that the broadcast-referenced network moves from epoch to epoch (one-epoch rms 1.1 mm on
the golden, 5.7 mm with both knobs). Re-estimated every epoch, a receiver setting that costs the golden nothing, the GPS +
Galileo broadcast posture with both knobs reads +0.003 m at the census and -0.004 m at the held-out receiver on DOY 129, and
+0.019 and +0.025 m on DOY 132.

Nothing is armed; arming, any golden change and the merge are the author's decisions.

## 8. What remains

- **DOY 132's remainder.** Against the same regime on the CODE a priori the tracked posture costs +0.016 m at the census on
  DOY 132 (0 on DOY 129). It sits at ARHT00ATA, UTQI00USA and the C1W sites, on the day with far more clock-anchor
  decouplings (33 against 3 on the regime on CODE, 26 against 1 tracked), at each of which the smoothed hold withdraws a
  bridge with a 0.2 to 0.3 m transfer. To separate: the decouplings' sites and epochs against the census sites' losses,
  then whether the hold carries it (the anchor's q does not, section 6.9).
- **The inter-constellation level.** GPS + Galileo needs either receivers that re-estimate the GPS-to-Galileo offset every
  epoch (a receiver-side setting, measured above) or a network datum that keeps the two constellations' levels together
  without the CODE product (the sum-to-zero datum is refused on the golden's posture). Which is the right design is open;
  whether the pivot's own column should be left out of the track is a cheap first test.
- **The hold itself.** With the tracked a priori the a priori's motion follows the real clock to about 3 mm per epoch, the
  condition card LAH's phase-clock hold was valid under (a precise a priori). Whether the phase-clock hold, with the
  anchor nearer its floor, can replace the smoothed hold on the tracked a priori is the natural next card; the loader refuses
  the combination today.
- **The regime's own cost** against the golden (+0.0049 m pooled, section 6.7) is the CODE-regime golden candidate's open
  item (card NBP section 12), unchanged by this card.
- **Days.** Two days graded (129 and 132); DOY 130 and 131 and longer runs are owed before any adoption, as is a real-time
  run (the column needs four stations per satellite in the epoch).
- **Instruments.** The two-way centring's column is a per-epoch measurement of each satellite's broadcast misfit; a trace
  of it per satellite (today only the counters and the events above half the floor) would serve the grading of the clock.
