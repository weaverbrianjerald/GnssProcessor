# ICL4: why one slip scatters a constellation's clocks (the epoch after an IF-TD flag)

**STATUS**

- **id:** ICL4
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered and closed 2026-11-06, from card ICL3 section 7. The scatter at 23:40:30 on DOY 129 is not the slip handover's: a second, real slip (L5X of E26 at YKRO00CIV, about -294 cycles, no LLI) arrives in the epoch after an IF-TD flag, when that test has no double difference and the MW signal is dropped. The arc's ionosphere-free level is already stiff, so the receiver's Galileo offset takes +10.8 m and the scatter walks the Galileo graph; `network.clock_anchor_q` sets its reach. `network.cycle_slip.if_phase_td_reseed_on_flag` removes it on both postures and on DOY 132. All gates pass. Nothing armed.
- **source:** card [ICL3](ICL3_frame_quorum_2026-11-05.md) section 7 (the owed item) and section 2.2 (the event); card [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) section 14 (`network.cycle_slip.if_phase_td_reseed_on_flag`, rung S7); card [SCL](SCL_self_contained_clock_level_2026-10-03.md) (`network.clock_anchor_q` 5e-6 on the broadcast a priori); standing invariant 1 of `CLAUDE.md`
- **knobs / posture:** read, not changed in any default: `network.cycle_slip.if_phase_td_reseed_on_flag`, `network.clock_anchor_q`. Graded on card BRD's GPS + Galileo broadcast posture with card ICL's frame (base `icl_p{129,132}_6`) and on the golden of the same exe (base `brd_c{129,132}_6`).
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `icl4/post-flag-slip` from `icl3/frame-quorum` (`569482c`), worktree `D:/GnssProcessorSupport/worktrees/icl2`; exe `gnss_processor_icl3q1.exe` (sha256[:16] `92743f1fb3c747fb`); arms `D:/GnssProcessorSupport/runs_nom/_rap_icl4_*`; the 24 h arms read are `icl2_{p,c}{129,132}_24` of card ICL2; scripts `D:/GnssProcessorSupport/worktrees/icl3_tools/icl4_*` (outside the repository)
- **gates (frozen at pre-registration):** section 3

**Outcome.** The scatter comes from a second, real slip one epoch after an IF-TD flag, when no pre-solve test answers it; the handover is a coincidence. The misclosure spreads through the receiver's Galileo offset, and the anchor's process noise sets how far: 18 decouplings at 5e-6, 2 at the golden's 2.9e-8. The existing re-seed key flags the slip at 94.1 m and removes the event on both postures, and a second natural event on DOY 132. All gates pass. Receivers are not graded here. Nothing armed.

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

### 4.1 The gates (2026-11-06)

Short arms from one exe (sha256[:16] `92743f1fb3c747fb`). `e_phi` is the anchor's first pass, relative to the constellation's median; `dC` is the emitted clock's increment relative to the median, from `products.txt`; the offset is the row's station's `ISB:Galileo`. Scripts `icl4_grade.py`, `icl4_rx.py`.

DOY 129, epoch 344430, E26 at YKRO00CIV:

| arm | change | IF-TD at the epoch | decouplings G / E | T4 arcs | s0 phase | E26 `e_phi` (m) | YKRO offset move (m) | E `dC` range (m) | E beyond 0.12 m |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `icl4_b0` | broadcast posture, control | none | 1 / 18 | 102 | 6.01 | -4.417 | +10.278 | -5.147 to +1.029 | 18 |
| `icl4_b1` | re-seed key | `post_flag`, 94.127 m | 0 / 0 | 2 | 0.29 | -0.001 | -0.004 | -0.006 to +0.007 | 0 |
| `icl4_b2` | q 2.9e-8 | none | 0 / 2 | 57 | 1.15 | -0.289 | +10.554 | -5.342 to +0.817 | 3 |
| `icl4_b4` | clone and shared history off (beside the gates) | none | 2 / 16 | 99 | 4.90 | -3.800 | +17.451 | -4.201 to +0.588 | 17 |
| `icl4_g0` | golden, control | none | 0 / 2 | 41 | 0.66 | -0.285 | +10.164 | -1.670 to +0.365 | 2 |
| `icl4_g1` | q 5e-6 | none | 0 / 13 | 58 | 2.61 | -2.122 | +4.818 | -2.318 to +0.510 | 13 |
| `icl4_g2` | re-seed key | `post_flag`, 94.131 m | 0 / 0 | 0 | 0.24 | +0.000 | -0.004 | -0.003 to +0.001 | 0 |

DOY 132, epoch 529770, E19 at SYOG00ATA (the first flag at 529740, 95.791 m, `cb_kept=L5X`):

| arm | change | IF-TD at the epoch | decouplings G / E | T4 arcs | s0 phase | E19 `e_phi` (m) | SYOG offset move (m) | E `dC` range (m) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `icl4_r0` | broadcast posture, control | none | 4 / 7 | 43 | 2.33 | -0.895 | +0.438 | -1.054 to +0.477 |
| `icl4_r1` | re-seed key | `post_flag`, 95.817 m | 0 / 0 | 0 | 0.31 | +0.003 | -0.003 | -0.013 to +0.016 |

- **C0 PASS.** `icl4_b0`: the second slip unanswered, 18 Galileo decouplings. `icl4_g0`: 2. The short arm starting 2 h before the event reproduces the 24 h arm's response (18 against 18; -4.42 against -4.46 m for E26), so it is a bed for this event, at about a tenth of the run time.
- **H1 PASS, every clause on both postures.** The key flags the second slip before the solve, at the size the accounting of section 2.2 predicts (94.1 m against 94.5 m), and the event is gone: no decoupling, at most 2 T4 arcs, s0 phase at its normal level, the YKRO offset still.
- **H2 PASS in both directions, with an unequal share.** The broadcast posture at the golden's q answers exactly as the golden does (E26 -0.289 against -0.285 m, 2 decouplings against 2), so at low q the rest of the posture adds nothing at this event. The golden at q 5e-6 crosses every bar (13 decouplings, -2.122 m) but reaches about half of the posture's amplitude, uniformly over the satellites (E33 +0.503 against +1.021 m). That factor of 2 is not attributed. The clock clone with its shared history is not it (`icl4_b4`: -3.800 m, 16 Galileo decouplings).
- **H3 PASS.** `icl4_b0` has E26's anchors at STHL00GBR on both bands and no slip handover of E26 at all, and decouples 18 satellites. `icl4_g1` has them at MBAR00UGA and decouples 13. The handover in the 24 h arm is a coincidence of where the anchor stood.
- **R PASS.** `icl4_r0` reproduces the 24 h arm's footprint exactly (s0 phase 2.33, 43 arcs, 11 retro bumps); the key removes it (0 decouplings against 11).
- **Measure only.** `icl4_b3`'s products are byte-identical to `icl4_b0`'s. Its detector log at 344430: `MWDEC sta=YKRO00CIV E26_L1X_L5X dev=+219.4446 m (+292.041 WL) thr=2.2416 m n=93 flag=1`, then `MW outlier on locked E26 ... absorbing into Welford`, then `WL lock demoted ... Signaling unknown-integer slip`. So the MW detector does report the slip at its own epoch, with an unknown integer, and the PASS A admission drops it: the integer rule needs a known integer, and the evidence rule refuses a bias-corrected lane (`bia=yes` here). The day's run-end line reads `legacy 482, evidence 0 [knob on; would-admit 0]; dropped loose 240 / unknown 5710` on `icl2_p129_24`, and evidence 0 on the golden's arm too (`legacy 526`, `unknown 6034`).

### 4.2 The raw data of the event

RINEX of YKRO00CIV, E26 (`icl4_rnx.py`): S1X falls 45.25, 42.75, 37.75, 30.25 dB-Hz and S5X 35.25, 28.00, 26.00, 21.50 dB-Hz over 23:38:30 to 23:40:00, and both recover at once at 23:40:30 (45.25, 35.75). The L5X increment at 23:40:30 is about 294 cycles short of its neighbours, the L5X record is absent at 23:41:00 and no LLI is set on any of these records. A deep fade at an equatorial station in the local evening, followed by a re-acquisition about 294 cycles off with no mark from the receiver. The fade is measured; scintillation as its cause is an interpretation, not tested here. E12 at the same station holds steady over these epochs; G06 dips from 39.25 to 31.75 dB-Hz at 23:40:00.

### 4.3 What a post-flag step needs in order to scatter (a correction to section 2.4)

Section 2.4 closes with "the footprint needs a first flag small enough to leave the refloated arc in the solve". That is too narrow: DOY 132's first flag is 95.8 m. Over both days, per (station, satellite), every |dGF| of at least 3 m in the epoch after an IF-TD flag of that arc (`icl4_postflag.py`):

| what met the second step | arcs (DOY 129 + 132) | largest s0 phase |
| --- | --- | --- |
| an LLI or MW refloat at its own epoch | 17 + 28 | 0.62 |
| T4 had dropped the arc at the flag epoch | 2 + 1 | 0.38 |
| nothing: the arc stood in the solve | 3 + 8 | 5.85 |

The third row is the population. All 11: E26 at YKRO (s0 phase 5.85, 100 arcs, 19 decouplings), E19 at SYOG (2.33, 43, 11), G16 at SYOG (1.20, 14, 6), G14 with G22 at SYOG (1.16, 11, 3), G08 at SYOG (0.83, 8, 0), G19 at SYOG (0.82, 1, 0), HKSL E04 (0.81, 2, 0), G23 at SYOG (0.71, 8, 4), G13 at SYOG (0.55, 0, 1), KIT3 G21 (0.40, 0, 0). The size of the step matters less than which row it falls in: SYOG G03 at -124 m in the second row and DJIG E07 at +60.5 m in the first decouple nothing.

Epochs the IF-TD test cannot see at all are about 0.9 % of adjacent samples (DOY 129: 2 300 733 tested, 20 670 blind, of which 1 051 post-flag; `icl4_blind.py`). A tested row can also be vacuous. At WIND00NAM at 311100 the phases step at once on every satellite of the pools (E30's code stays smooth in the RINEX), and E30's double difference of 6.81 m is the median of its pool of 5 rows (`abs_dev_m` 0.000), so it cannot flag; G31 is the same among 8. The flags then erase the others' gradients, which leaves a pool of one row per chain at 311130. That epoch leaves no footprint, but the coverage after a mass flag is thin; the key keeps those satellites in the pool. The other blind classes hold no comparable footprint on these days: the large jumps there are on pairs outside the solve (LMMF00MTQ E27 L1X + L8X) or in the first minutes of a run (BSHM00ISR G07). The epochs 304110 of DOY 129 (CEBR00ESP, no raw step at or above 1 m) and 604560 to 604770 of DOY 132 (a smooth rise and fall of phase residuals over the whole network, up to 7.6 cm, no retro bump) are other mechanisms, not traced here.

### 4.4 What the key costs and what the receiver sees

Beside the gates; no adoption claim. Paired against each control after hour 1 (`icl4_pairs.py`):

| pair | IF-TD flags | T4 arcs | anchor decouplings | WTZZ00DEU 3D, median of the pair (mm) | paired change (mm) |
| --- | --- | --- | --- | --- | --- |
| `icl4_b1` against `icl4_b0`, 1.4 h | 19 to 22 | 107 to 2 | 20 to 1 | 56.0, 52.9 | -2.40 |
| `icl4_g2` against `icl4_g0`, 1.4 h | 17 to 19 | 48 to 2 | 3 to 1 | 36.3, 36.0 | -0.20 |
| `icl4_r1` against `icl4_r0`, 2.25 h | 81 to 99 | 73 to 13 | 30 to 19 | 45.0, 46.6 | +1.60 |

The extra flags sit on arcs already in trouble at equatorial or polar stations (MBAR00UGA, YKRO00CIV, DJIG00DJI; STHL00GBR G05 on DOY 132), mostly in hour 1; card ARL's 6 h arm reads the same way (111 to 149 flags, census 0.0 mm). One receiver on short arms does not grade the key: the three paired changes have both signs, and the first half hour of `icl4_r1` moves +17 mm. At the event itself the receiver's error rises and decays without the key and does not with it: `icl2_p129_24` 11 mm before, then 33, 34, 27, 20, 20, 17 mm; `icl4_r0` 39 mm before, then 53, 48, 46 mm, against `icl4_r1` 41 mm before, then 41, 40, 41 mm. The receiver's Galileo offset moves -31 mm at the event in `icl4_b0` and -1 mm in `icl4_b1`, and the frame votes on 4 satellites in `icl4_b0` (step -0.085 m) and on 22 in `icl4_b1` (step -0.001 m), so this event no longer reaches card ICL3's quorum.

### 4.5 The golden pays too

The hole is not the broadcast posture's. On the golden the same slip costs 41 arcs refloated by T4, 2 retro bumps and an emitted E26 clock of -1.67 m for one epoch, all gone with the key (`icl4_g2`: 0, 0, -0.003 m). The broadcast posture adds the reach, not the cause.

## 5. Verdict

**The scatter is not the slip handover's. It is a second, real slip that no pre-solve test answers, spread through the receiver's Galileo offset, and the broadcast posture's anchor lets it reach the whole constellation.** In order:

1. At 23:40:00 a deep signal fade at YKRO00CIV gives E26 a small step, and the IF-TD test flags and refloats the arc, erasing the satellite's gradient.
2. At 23:40:30 L5X returns about 294 cycles off (-75 m) with no LLI. IF-TD has no double difference. The MW detector reports an unknown integer and the admission drops it.
3. The one-epoch-old arc is loose only along the ionosphere; its ionosphere-free level is already stiff, so a misclosure of 94.5 m meets stiff rows. YKRO's per-epoch Galileo offset takes +10.8 m and pulls the stiff arcs of its other four Galileo satellites.
4. Those clocks and E26's pull the offset of every station that shares them, so the scatter walks through the Galileo graph. GPS is shielded by the offset.
5. The anchor's process noise sets how far the walk reaches. At q 5e-6 the anchor decouples 18 of 22 Galileo satellites; at the golden's 2.9e-8 it decouples 2, with the same slip, on the same posture.

Everything downstream follows from that: 19 decouplings and retro bumps, 100 arcs refloated by T4, clocks scattered by -5 to +1 m for one epoch, and the frame's minority vote of card ICL3. The within-epoch tier stays silent: its robust sigma0 reads 1.00 against the bar of 3.0 (the post-fit 1.09, the phase family alone 5.85), since one arc is a small share of 3 800 rows, although the attribution table names E26 at once (prominence 81, `acted=0`).

`network.cycle_slip.if_phase_td_reseed_on_flag`, built by card ARL for this hole on the sim bed's rung S7 and left off because its 6 h field arm held no such event, removes the event entirely on both postures and on DOY 132's natural event. C0, H1, H2, H3 and R pass. This is standing invariant 1 on a response's own re-coupling edge: the refloat's fresh value is stiff again after one epoch, and the test that would guard it is switched off for exactly that epoch.

Nothing is armed. Card ICL3's quorum stays useful as a guard on the frame, but its evidence, this one event, is removed at the source by the key. Arming, any golden change and the merge are the author's decisions.

## 6. What remains

- **The key over a day.** A 24 h grade of `network.cycle_slip.if_phase_td_reseed_on_flag` on DOY 129 and 132 with the 16-site census, on the golden and on the broadcast posture. The short arms here remove the events and do not grade the receivers. Card ARL's unit-test debt for the key's run-end report falls due with any arming.
- **The factor of 2 at q 5e-6.** The golden at the broadcast posture's q reaches half of its amplitude; the clone is not it.
- **MW at a locked arc.** A flag of 292 wide-lane cycles is treated as a noise outlier for its own epoch, and the unknown-integer signal that follows is dropped. `network.cycle_slip.mw_evidence_refloat`, armed in the goldens, admitted 0 flags on the arms read, because the rule refuses bias-corrected lanes (standing invariant 4). A second pre-solve answer is possible here; it is not needed for this event once the key is on.
- **The within-epoch tier and one arc.** Its robust sigma0 cannot trip on one stiff arc; the attribution table names the arc and does not act. Whether a large single-arc attribution should re-solve within the epoch is a question for the response doctrine (adapt and emit within the detected epoch).
- **Other footprints.** The epoch 304110 of DOY 129 at CEBR00ESP and the last minutes of DOY 132 are other mechanisms.

## 7. The owed grade (2026-11-06)

The first item of section 6 is answered by card [ICL5](ICL5_reseed_key_over_a_day_2026-11-06.md): 24 h on DOY 129 and 132, on both postures, with the 16-site census. The other items of section 6 stay open.
