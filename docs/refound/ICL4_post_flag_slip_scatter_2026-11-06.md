# ICL4: why one slip scatters a constellation's clocks (the epoch after an IF-TD flag)

**STATUS**

- **id:** ICL4
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-06, from card ICL3 section 7: why the slip at YKRO00CIV at 23:40 on DOY 129 scatters 18 of 22 Galileo clocks on the broadcast posture and 2 on the golden. Measured first: the slip handover is not the cause. A second, real slip (L5X of E26 jumps about -75 m) arrives in the epoch after the IF-TD flag, when that test has no double difference. Gates freeze the cause, the amplification and the handover's role before the short arms are read.
- **source:** card [ICL3](ICL3_frame_quorum_2026-11-05.md) section 7 (the owed item) and section 2.2 (the event); card [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) section 14 (`network.cycle_slip.if_phase_td_reseed_on_flag`, rung S7); card [SCL](SCL_self_contained_clock_level_2026-10-03.md) (`network.clock_anchor_q` 5e-6 on the broadcast a priori); standing invariant 1 of `CLAUDE.md`
- **knobs / posture:** read, not changed in any default: `network.cycle_slip.if_phase_td_reseed_on_flag`, `network.clock_anchor_q`. Graded on card BRD's GPS + Galileo broadcast posture with card ICL's frame (base `icl_p{129,132}_6`) and on the golden of the same exe (base `brd_c{129,132}_6`).
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `icl4/post-flag-slip` from `icl3/frame-quorum` (`569482c`), worktree `D:/GnssProcessorSupport/worktrees/icl2`; exe `gnss_processor_icl3q1.exe` (sha256[:16] `92743f1fb3c747fb`); arms `D:/GnssProcessorSupport/runs_nom/_rap_icl4_*`; the 24 h arms read are `icl2_{p,c}{129,132}_24` of card ICL2; scripts `D:/GnssProcessorSupport/worktrees/icl3_tools/icl4_*` (outside the repository)
- **gates (frozen at pre-registration):** section 3

**Outcome.** Open. Measured before the gates: the event's cause is a second slip one epoch after an IF-TD flag, not the handover. The arms are graded in section 4.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

Card ICL3 closed the frame's part of the event at 23:40:30 on DOY 129 and left its cause owed: why does this one slip scatter the code-visible clocks of 18 Galileo satellites by -5.2 to +1.0 m, when 266 other slip handovers over the two days do not, and why does the golden answer the same slip with 2 decouplings? The card's framing, a slip handover that scatters, is tested here and the measurements below replace it.

## 2. Measured before the gates (2026-11-06)

All from the existing 24 h arms `icl2_p129_24` (broadcast posture) and `icl2_c129_24` (golden), tows of week 2313; 344400 is 23:40:00. Scripts `icl4_guard.py`, `icl4_rows.py`, `icl4_station.py`, `icl4_scan.py`, `icl4_cross.py`.

**2.1 The raw data hold two slips, not one.** E26 at YKRO00CIV, pair L1X + L5X:

| tow | MW (m) | GF (m) | L1 - C1 (m) | test at this epoch |
| --- | --- | --- | --- | --- |
| 344370 | -12.02 | -22.29 | 53.47 | IF-TD |dd - med| 0.016 m, clean |
| 344400 | -12.53 | -22.40 | 53.74 | IF-TD 0.346 m against 0.109 m: flag, refloat of both bands |
| 344430 | +206.87 | +52.61 | 52.13 | no IF-TD row |
| 344460 | (L5X absent) | | 51.88 | no IF-TD row |
| 344490 | +202.97 | +51.67 | 51.36 | no IF-TD row (L5X re-entered) |
| 344520 | +203.15 | +51.91 | 50.81 | IF-TD 0.001 m, clean |

GF steps +75.00 m and MW +219.40 m at 344430 and stay: L1 - C1 is smooth, so the L5X phase stepped about -75 m (about -294 cycles), and the step lasts. Neither posture's first answer sees it: no LLI, no IF-TD row, and the MW detector absorbs a flag on a locked arc (the band scope records `mw(L1X+L5X)flag`; no `mw_wl_slip` event follows). The IF-TD row is absent because a flag erases the satellite's gradient, so the next epoch has no double difference. `network.cycle_slip.if_phase_td_reseed_on_flag` exists to close exactly this hole (card ARL, rung S7, repetition 2); it was graded consumer-neutral on 6 h arms that hold no such event, and is off.

**2.2 The accounting at YKRO.** One epoch after a refloat the arc's ionosphere-free ambiguity is already stiff: the fold's variances are 0.0556 and 0.1787 m^2 with a ratio of 3.21, the square of the ionosphere's 1.793 between the bands, so the arc is loose only along the ionosphere direction. The step of -75 m on L5X is an ionosphere-free misclosure of +94.5 m, against a stiff ambiguity, and the solve shares it (broadcast posture, m): the ambiguity's ionosphere-free level +46.6 (L1X +1.82, L5X -33.76), the E26 rows' residuals +32.1 (L1X +10.83, L5X -6.04, each with weight 7.9e4), and a clock-like term of +15.76, of which +10.79 is YKRO's own Galileo offset (`ISB:Galileo` 8.376 to 19.165 to 8.337 m). The ionosphere takes +28.42 m in the same solve. The golden's YKRO offset moves +10.58 m: the first stage is the same on both.

**2.3 The contagion.** YKRO's offset is a per-epoch state common to its five Galileo rows, so +10.8 m pulls the stiff arcs of E12, E19, E27, E33 at YKRO (residuals near -3 m on L1X, +1.7 m on L5X), and those satellites' clocks and E26's pull the offsets of every station sharing them. GPS at YKRO is shielded by the offset (its clock moves +1.03 m, G rows at most 0.26 m). Largest Galileo offset moves and the median over 58 stations, m:

| arm | YKRO | WIND | STHL | SYOG | median |move| |
| --- | --- | --- | --- | --- | --- |
| broadcast posture | +10.79 | -2.65 | -1.75 | -1.20 | 0.66 |
| golden | +10.58 | -0.97 | -0.66 | -0.31 | 0.018 |

The clock innovations (`e_phi`, m) fall with the graph distance from the slipped arc on both arms, at different rates: E26 -4.46 / -0.287, E33 +0.98 / +0.188, E19 +0.43 / +0.041, E27 +0.38 / +0.056, E12 +0.33 / +0.021, the rest of the constellation 0.10 to 0.73 on the broadcast posture and at most 0.06 on the golden, against the anchor's bar of 0.12 m (6 x the 0.02 m floor). The anchor's prior differs between the arms: `network.clock_anchor_q` 5e-6 against 2.9e-8 m^2/s (0.0132 against 0.0018 to 0.0043 m in `sqrtS_clk`), which card SCL set for the broadcast a priori.

**2.4 Not one event.** Over both days, adjacent-epoch |dGF| of at least 1 m classified by what answered it at its own epoch before the solve, and the epochs with the largest phase-family sigma0:

| epoch | jump, first answer | s0 phase | arcs refloated | retro bumps | decouplings G / E |
| --- | --- | --- | --- | --- | --- |
| DOY 129 344430 | YKRO E26 +75.0 m, post-flag | 5.85 | 100 | 19 | 1 / 18 |
| DOY 132 529770 | SYOG E19 +42.4 m, post-flag | 2.33 | 43 | 11 | 4 / 7 |
| DOY 132 581790 | SYOG G16 +10.4 m, post-flag | 1.20 | 14 | 6 | 6 / 0 |
| DOY 132 592890 | SYOG G14 +10.5 m, post-flag (and REYK flagged) | 1.16 | 11 | 3 | 3 / 0 |
| DOY 129 304110 | no raw jump at or above 1 m | 2.24 | 14 | 4 | 2 / 1 |
| DOY 129 299130 | SYOG G03 -124.1 m, post-flag | 0.34 | 4 | 1 | 0 / 0 |
| DOY 132 545370 | SYOG G23 -119.8 m, post-flag | 0.34 | 1 | 1 | 0 / 0 |

The last two have no footprint although they are larger. They are the return of a one-epoch outlier: the outlier's epoch trips T4 (`drop:t4_attrib`), so the arc is not stiff when the data return, and the post-flag epoch moves nothing. So the footprint needs a first flag small enough to leave the refloated arc in the solve and a second step large against the arc's stiff ionosphere-free level. The last minutes of DOY 132 (604560 to 604770, s0 phase up to 4.25, no retro bumps) are not treated here.

## 3. Hypotheses and gates (frozen 2026-11-06, before any arm below was read)

The short arms start at 21:30:15 on DOY 129 and run 2.4 h (`network.start_offset_hours` 21.504167), one change each, from one exe; the same-length controls are `icl4_b0` (broadcast posture) and `icl4_g0` (golden). A 2.4 h arm is not the 24 h arm (card ICL3, section 8 of card ICL2): each is compared with its own control. The bars are on the epoch 344430 unless stated, with `e_phi` read from `network_anchor_guard.csv`.

**C0, reproduction.** `icl4_b0` shows the second slip unanswered and at least 10 Galileo decouplings at 344430; `icl4_g0` at most 4. If C0 fails the short arm is no bed for this event, and the 24 h arm is the bed.

**H1, the cause.** The post-flag slip is the cause, on both postures. With `network.cycle_slip.if_phase_td_reseed_on_flag` (`icl4_b1`, `icl4_g2`): an IF-TD flag of E26 at YKRO at 344430 with the detail `post_flag`; decouplings at most 2 in all (the slipped satellite may remain); arcs refloated by T4 at most 10; phase-family sigma0 below 1.0; |move of YKRO's Galileo offset| at most 0.5 m. Each clause is reported; H1 passes only with all of them on both arms.

**H2, the amplification.** The difference between the postures at this event is the anchor's q alone. `icl4_b2` (broadcast posture at q 2.9e-8) at most 4 Galileo decouplings and |E26 e_phi| at most 1 m; `icl4_g1` (golden at q 5e-6) at least 10 and |E26 e_phi| at least 2 m. A posture that crosses in only one direction fails H2 and is reported as a partial share.

**H3, the handover.** The slip handover is not needed. A mass decoupling (at least 10) with no anchor of E26 at YKRO at 344400 passes H3; on the golden at q 5e-6 the anchors of E26 are at another station (`icl4_g1`), so H2's arm is H3's test too. If no such arm exists, H3 is reported as not tested.

**R, replication.** The same response on a second natural event: DOY 132, the arms `icl4_r0` (control) and `icl4_r1` (the key), broadcast posture, from the start of the day, 3.25 h, event 529770 (E19 at SYOG00ATA). `icl4_r1` flags E19 at SYOG at 529770 (`post_flag`) with decouplings at most 3 at that epoch, against at least 5 in the control. If the control does not reproduce the event, R is not tested.

**Measure only.** The MW detector's branch at 344430 is read from its own log (`icl4_b3`, `logging.enable_mw_slip_detector`, a logger change only: `icl4_b0`'s products must be identical).

Beside the gates, not bars: the key's other flags in the short arms, the receiver's clocks and phase biases around the event, and the epochs 304110 of DOY 129 and 604560 to 604770 of DOY 132, which this mechanism does not claim.

No key is armed and no default moves. A day-long grade of the key is a separate decision.

## 4. Results
