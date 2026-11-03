# BRD2: the broadcast-referenced network's DOY-132 remainder, the phase-clock hold on the tracked a priori, DOY 130 and 131

**STATUS**

- **id:** BRD2
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-03: card BRD's three owed items on the GPS broadcast-referenced posture with both of its knobs (the DOY-132 remainder against the same estimator on the CODE a priori, card LAH's phase-clock hold in place of the smoothed hold on the tracked a priori, DOY 130 and 131); measurements on card BRD's runs in section 2, gates in section 4; results owed.
- **source:** card [BRD](BRD_broadcast_referenced_level_2026-11-02.md) sections 6.6, 6.7, 6.9 and 8; card [LAH](LAH_level_aware_hold_2026-09-29.md) (the phase-clock hold); card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (the smoothed hold and its bridge); card [SCR](SCR_sat_clock_step_response_2026-10-26.md) (`network.emit_pb_hold_trip_generation`)
- **knobs / posture:** new, default off: `network.apriori_clock_track_phase_clock_hold` (section 3). Existing keys the arms set: `network.emit_pb_hold_trip_generation` (card SCR, UNVALIDATED), `network.clock_anchor_k`, `network.emit_pb_hold_phase_clock`, `network.emit_pb_hold_smoothed`, `network.clock_anchor_q`. The graded posture is card BRD's T2 posture: the broadcast-referenced posture of card NBP with `network.cycle_slip.if_phase_td_sat_centre` and `network.apriori_clock_track`, GPS
- **dates in the text:** 2026-11-03
- **hashes / run dirs:** branch `brdc/brd2` from `brdc/independence` (`1e8a543`, unmerged), worktree `D:/GnssProcessorSupport/worktrees/brd2`; arms `D:/GnssProcessorSupport/runs_nom/_rap_brd2_*`; census replays `_rap_<key>_<site>` with keys `b2*`; tools `D:/GnssProcessorSupport/worktrees/brd2_tools/` (outside the repository); card BRD's runs `_rap_brd_*` and census keys `brdg`, `brdr`, `brdnt` are the references of DOY 129 and 132
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registration. Three questions left by card BRD: what carries the +0.016 m census remainder of DOY 132 against the same estimator on the CODE a priori, whether card LAH's phase-clock hold can replace the smoothed hold once the a priori is tracked, and how DOY 130 and 131 grade with both knobs. Section 2 measures, on card BRD's own runs, a candidate carrier (the smoothed hold's bridge at the clock anchor's decouplings) and predicts the phase-clock hold's wander from the tracked arm's records (112 and 116 mm per 120 epochs, a predicted failure). Results owed.

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
