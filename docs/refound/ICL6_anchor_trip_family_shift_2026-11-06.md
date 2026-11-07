# ICL6: why the clock anchor trips alone: a slip's stale level, a signal family's common mode, a mixed statistic

**STATUS**

- **id:** ICL6
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered 2026-11-06 and closed 2026-11-07, asked for by the author after card ICL5, whose gate R3 failed on the golden at DOY 132 through two lone Galileo anchor decouplings. The lone trips read a shift of one signal family, invisible to every row, through a statistic that mixes families (14 of 15 Galileo trips on a mixed pair, a kind of pair held about 2.7 % of the time). A freed arc's pre-slip level pushes a receiver's DCB by metres and is the source. The key only softens the family's prior: `phi` trips are 35 in the two golden controls and 36 with it. M0, H4 and H5 pass. H1 is unread at three trips (the monitor has no row there). H2 fails on the step count at DOY 132 (12 against 6.7, 9 at one station) and at WIND00NAM (+0.132 against 0.1 m). H3 fails on L5Q (22.7 and 13.6 against 10 mm), because the floor also keeps the family's prior soft. The 100 m2 floor used as the instrument removes every `phi` trip at no census cost. Nothing armed.
- **source:** card [ICL5](ICL5_reseed_key_over_a_day_2026-11-06.md) sections 4.3 and 6 (the failed clause, the owed trace); card [RF-1](RF-1_float_ambiguities_design.md) section 3.4 (the drop keeps `value_abs`); card [RF-3](RF-3_code_row_qc_2026-09-22.md) sections 1 and 5.1 (the walk on the arc itself, the drop floor); card [PVN](PVN_phase_visible_nees_2026-09-29.md) (the phase-visible monitor)
- **knobs / posture:** no code change and no new key. Existing keys in one-change arms: `network.phase_visible_monitor` (measure-only), `network.amb_float_drop_var_m2` (KEEP_OFF, used as an instrument). Bases: `icl2_c{129,132}_24` (the GPS + Galileo golden), `icl5_g{129,132}_24` (the same with `network.cycle_slip.if_phase_td_reseed_on_flag`).
- **dates in the text:** 2026-11-06, 2026-11-07
- **hashes / run dirs:** branch `icl6/anchor-trip-family` from `icl5/reseed-key-day` (`0d40d22`), worktree `D:/GnssProcessorSupport/worktrees/icl2`; exe `gnss_processor_icl3q1.exe` (sha256[:16] `92743f1fb3c747fb`), census `gnss_processor_icl2c.exe`; arms `D:/GnssProcessorSupport/runs_nom/_rap_icl6_{mc,mg,fg}{129,132}`; census `_rap_i6fg{129,132}_*`; scripts `D:/GnssProcessorSupport/worktrees/icl3_tools/icl6_*` (outside the repository)
- **gates (frozen at pre-registration):** section 3

**Outcome.** The lone anchor trips read no clock event. A slip's freed arc keeps its pre-slip level at birth variance; that prior pushes a receiver's DCB by metres and shifts one signal family's phase biases, which no row sees; a statistic that mixes families reads the shift as a clock event. The key only softens the family's prior: 35 `phi` trips in the two golden controls, 36 with it. A 100 m2 floor removes all of them at no census cost. H1, H2 and H3 each fail clauses, which stay failed. Nothing armed.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

Card ICL5 found that with the re-seed key three Galileo satellites decouple alone at epochs where the control does not decouple them: E08 at DOY 129 311100, E26 at DOY 132 550410, E07 at DOY 132 554610 (E26 also on the broadcast posture). The IF-TD flags at those epochs are the same in both runs and the receivers do not register the trips on the golden. Why does the test read more on one satellite, and does the fault lie with the key?

## 2. Measured before the gates (2026-11-06)

From the existing 24 h arms of cards ICL2 and ICL5. `w` is the ionosphere-free weight: 2.2606 on E1, -1.2606 on E5a; gamma = (f1 / f5)^2 = 1.7933.

### 2.1 A family-wide shift no row sees

At each trip the phase-bias columns of one signal family move together, in the ionosphere's ratio, and the family's arcs at non-anchor stations move the opposite way. The median over the satellites, from the PRIORRES rows, control then arm, mm:

| epoch | family | band 1 | band 5 | band 5 / band 1 | example arc |
| --- | --- | --- | --- | --- | --- |
| DOY 132 550410 | {L1C, L5Q} | +21.3, +54.0 | +38.5, +96.9 | 1.81, 1.79 | CHUR00CAN E19 L5Q -42.9, -101.0 |
| DOY 132 554610 | {L1C, L5Q} | +30.0, +74.3 | +54.5, +133.9 | 1.82, 1.80 | HKSL00HKG E05 L5Q -57.5, -136.2 |
| DOY 129 311100 | {L1X, L5X} | +192.4, +192.3 | +345.2, +344.9 | 1.79, 1.79 | SAVO00BRA E30 L5X -363.3, -363.0 |

The direction is a null of the rows: a phase bias plus an arc is unchanged in every phase row, and at an anchor station the pinned arc passes the shift to the slant ionosphere and the receiver DCB, which leave both the phase and the code rows unchanged. The tripped satellite's postfit phase rows agree within 1 mm between the runs at those epochs while its arcs differ by decimetres, and its raw phase is smooth (the IF-TD double difference on the MRC100USA E07 and NIUM00NIU E26 arcs reads 2 to 16 mm, against a bar of 109 to 150 mm).

### 2.2 The statistic reads the shift

The anchor's phase statistic per satellite (`clockAnchorPhaseStatistic`) takes one phase-bias signal per band, the smallest folded variance, band by band. The code comment says any such choice is blind to the ionosphere direction. That holds for two signals of one family, which move together; for one signal of each family, the moved column is not cancelled. Satellites E07 and E26 carry L1C, L1X and L5X columns. The prediction, `w` x the family's band shift, against the guard's statistic:

| trip | run | predicted, m | statistic, m | bar, m |
| --- | --- | --- | --- | --- |
| E26, 550410 | golden control | 2.2606 x 0.0213 = 0.048 | 0.052 | 0.12 |
| E26, 550410 | golden arm | 2.2606 x 0.0540 = 0.122 | 0.125, trips | 0.12 |
| E07, 554610 | golden control | 2.2606 x 0.0300 = 0.068 | 0.067 | 0.12 |
| E07, 554610 | golden arm | 2.2606 x 0.0743 = 0.168 | 0.166, trips | 0.12 |
| E08, 311100 | golden arm | 1.2606 x 0.3449 = 0.435 | 0.405, trips | 0.12 |
| E08, 311100 | golden control | same-family pair predicted: 0 | 0.000 | 0.12 |

The family's dense columns are quiet at the same epochs: E26 L1X +4.0 mm, and E07's L1X and L5X are not reported (|t| below 4). On E08 the shift is the same in both runs and only the statistic differs, which points to a change of the pair between runs. The trace does not hold the pair: H1 reads it. The satellite's solved clock moves across the epoch by at most 3.8 mm on the golden.

### 2.3 The push: a freed arc's stale level moves the receiver's DCB gauge

At those epochs one station's arcs of that constellation all move in the ionosphere's ratio, with its DCB: DARW00AUS at 554610 (L1C -3.59 m, L5Q -6.43 m on 8 satellites; `Galileo_DCB` 0.04 to 2.85 m), MAJU00MHL at 550410 (L1C -1.95 m, L5Q -3.49 m; 0.04 to 1.55 m, then 1.94 m), WIND00NAM at 311100 (0.01 to 3.00 m). Each step is a slip: the raw MW and GF jumps give DARW E24 L1C -67.05 m, L5Q -0.46 m (a true L1-only slip, LLI set), MAJU E19 L1C -0.66 m, L5Q -54.05 m (a true L5-only slip).

The cause is the float vehicle's response. A drop keeps the arc's level (`ambFloatDrop`: "Keeping value_abs costs little, because a prior at birth variance is dominated by the epoch's data", card RF-1 section 3.4) and the arc folds at birth variance around its pre-slip value. No data at all bear on the station's common mode (slant ionosphere, band 1 arcs, band 2 arcs and DCB together), so even a weak prior moves it until other priors balance it. With `de` the freed arc's level change, `Vb` its birth variance, `c` = 1 on band 1 and gamma on band 2, and `V` the variance of the station's other band 1 arcs the epoch before:

    common mode delta = -V x sum(c x de / Vb),   DCB step = (gamma - 1) x delta

DARW at 554610: `de` -62.98 m, `Vb` 0.478 m2, `V` 0.0274 m2 give delta +3.61 m against the observed +3.59 m, with no free parameter. Over whole days, every DCB step of at least 0.3 m after hour 1 with a freed arc at its epoch (`icl6_force.py`):

| run | DCB steps | with a freed arc | slope, observed on predicted | r | same sign |
| --- | --- | --- | --- | --- | --- |
| `icl2_c132_24` | 101 | 67 | 0.988 | 0.998 | 65 of 66 |
| `icl5_g132_24` | 101 | 67 | 0.988 | 0.998 | 65 of 66 |
| `icl2_c129_24` | 129 | 97 | 0.804 | 0.930 | 92 of 94 |
| `icl5_g129_24` | 130 | 101 | 0.805 | 0.939 | 96 of 98 |
| `icl2_p132_24` | 215 | 76 | 0.987 | 0.969 | 73 of 75 |

DARW's `Galileo_DCB` takes 10 steps on DOY 132 and ends at 6.06 m, the level walk of standing invariant 1 in the station's gauge. Card RF-3 saw the walk on the arc itself; this is its reach. The steps without a freed arc at their epoch (34 of 101 on `icl2_c132_24`) are not classified here.

### 2.4 The amplification: why the arm reads 2.5 times more

The family's shift is a regression on its prior. At anchor stations the family's common mode and the DCB are one mode, so their DCB variance reads it. On the golden at DOY 132, arm over control, per family-wide epoch, Galileo L5Q (`icl6_ratio.py`):

| epoch | control, mm | arm, mm | shift ratio | anchors' DCB variance ratio |
| --- | --- | --- | --- | --- |
| 524610 | +1210.1 | +1210.1 | 1.00 | 1.00 |
| 537870 | +4.4 | +11.5 | 2.62 | 2.32 |
| 543150 | +5.2 | +14.9 | 2.83 | 2.43 |
| 549270 | -20.2 | -51.3 | 2.54 | 2.42 |
| 550410 | +38.5 | +96.9 | 2.51 | 2.39 |
| 554610 | +54.5 | +133.9 | 2.46 | 2.32 |
| 568770 | -7.1 | -16.1 | 2.26 | 2.17 |

Over 14 epochs the median shift ratio is 2.48 and the median variance ratio 2.32. The two parts at one place. In the control T4 fires at PALM00ATA at 525870 and names E30, E36, G04, G11 and G20. In the arm the key flags G11 and G20 at PALM at 525840 (`post_flag`, 0.57 and 1.03 m) and T4 does not fire: the mechanism of card ICL5 section 4.5, one flag standing in for several arcs. After that T4 the Galileo DCB deviation at PALM falls from 0.19 m to 0.134 m at 525900 and 0.048 m by 527700; at SEYG, NYA2, COCO, GUAM and JPLM with it (variance ratios 6 to 10 at 526500). In the arm it stays at 0.19 m until PALM's E34 flag at 527700, then falls to 0.048 m by 533910, and stays looser by 2 to 2.8 in variance until about hour 15.

On the broadcast pair the same chain appears (PALM variance ratio 7.34 at 526500), and the ratio later turns: at 554580 about 0.55 at JPLM and GUAM. E07's statistic at 554610 goes 0.049 to 0.025 m there, a ratio of 0.52.

Why a drop at an anchor station tightens a mode no row sees is not traced here; since no row sees it, the variance can only fall through the way priors are folded.

### 2.5 Ruled out

The phase anchors: identical in both runs at all 2 880 epochs (Galileo L5Q and L1C on DOY 132, L1X on DOY 129). A real clock event: the solved clock moves by millimetres. A real phase step on the tripped satellite: section 2.1.

## 3. Hypotheses and gates (frozen 2026-11-06, before any arm below is read)

Arms, each 24 h, each one change on its base, `gnss_processor_icl3q1.exe`:

- `icl6_mc129`, `icl6_mg129`, `icl6_mc132`, `icl6_mg132`: copies of `icl2_c129_24`, `icl5_g129_24`, `icl2_c132_24`, `icl5_g132_24` with `network.phase_visible_monitor` true (measure-only).
- `icl6_fg129`, `icl6_fg132`: copies of `icl5_g129_24`, `icl5_g132_24` with `network.amb_float_drop_var_m2` 100. The floor is an instrument: predicted from section 2.3, it shrinks each pull by the ratio of the birth variance to 100 m2 or more. It is KEEP_OFF in the ledger (card RF-3) and is not proposed for arming here.

| gate | bar |
| --- | --- |
| M0, identity | Each `icl6_m*` arm: `products.txt` (without comment lines), `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical to its base. |
| H1, the reader | `sigs` in `network_phase_visible.csv` at the epoch: E26 at 550410 and E07 at 554610 in `icl6_mc132` and `icl6_mg132`, and E08 at 311100 in `icl6_mg129`, pair a band 1 signal of one family with a band 5 signal of the other ({L1C, L5Q} against {L1X, L5X}). E08 at 311100 in `icl6_mc129` is a same-family pair. |
| H2, the source | In each floor arm: DCB steps of at least 0.3 m after hour 1 with a freed arc at their epoch at most 10 % of the base's (67 and 101). `Galileo_DCB` at DARW00AUS at 554610 and MAJU00MHL at 550410 (`icl6_fg132`), and at WIND00NAM at 311100 (`icl6_fg129`): each step at most 0.1 m in size (from +2.78, +1.52 and +3.00 m). |
| H3, the family | In the floor arms, the median size of the PRIORRES phase-bias rows: Galileo L5Q at 550410 and 554610 at most 10 mm, Galileo L5X at 311100 at most 30 mm; or fewer than 5 rows reported. |
| H4, the trips | In the floor arms: E26 at 550410, E07 at 554610 and E08 at 311100 do not decouple, and each one's statistic is at most 0.03 m. |
| H5, the amplification, out of sample | `icl6_ratio.py` on the pairs not tabulated above, `icl2_c129_24` with `icl5_g129_24` and `icl2_p132_24` with `icl5_b132_24`, Galileo L5Q, after hour 1, at least 10 rows in both runs and a control shift of at least 2 mm: the median shift ratio within 25 % of the median variance ratio. (Two broadcast epochs have been read, section 2.4.) |

Reported without a bar: the share of Galileo and GPS `phi` trips on a mixed pair over each day; how often a satellite's pair changes; E08's pair around 311100; in the floor arms, decouplings, T4 arcs and IF-TD flags against the base, the frozen flag sets (the detectors read raw data), and the 16-site census against the base by hour (card RF-3 reports an hour 1 cost for this floor).

A failed clause stays failed; no bar moves after this section is committed. Card ICL5's failed clause stays failed whatever this card finds. Nothing is armed by this card: arming, any golden change and the merge are the author's decisions.

## 4. Results

Graded 2026-11-07. Six 24 h arms on `gnss_processor_icl3q1.exe`, all exit 0; the census of the two floor arms on `gnss_processor_icl2c.exe`, 34 replays, all exit 0. "Control" is `icl2_c*`, "base" is `icl5_g*` (the control with the re-seed key), "floor arm" is `icl6_fg*` (the base with `network.amb_float_drop_var_m2` 100).

### 4.1 The gates

| gate | clause | read | verdict |
| --- | --- | --- | --- |
| M0 | `products.txt`, `closed_loop_trace.csv`, `network_epoch_status.csv` byte-identical to the base | identical on all four monitor arms | PASS |
| H1 | E26 at 550410 in `icl6_mc132`, mixed | L1C+L5X | PASS |
| H1 | E07 at 554610 in `icl6_mc132`, mixed | L1C+L5X | PASS |
| H1 | E08 at 311100 in `icl6_mc129`, same family | L1C+L5Q | PASS |
| H1 | E26 at 550410 in `icl6_mg132`, mixed | no row | FAIL, unread |
| H1 | E07 at 554610 in `icl6_mg132`, mixed | no row | FAIL, unread |
| H1 | E08 at 311100 in `icl6_mg129`, mixed | no row | FAIL, unread |
| H2 | steps with a freed arc in `icl6_fg132`, at most 6.7 (10 % of 67) | 12 of 327 | FAIL |
| H2 | steps with a freed arc in `icl6_fg129`, at most 10.1 (10 % of 101) | 4 of 8 | PASS |
| H2 | DARW00AUS at 554610, at most 0.1 m | +0.063 m (base +2.780) | PASS |
| H2 | MAJU00MHL at 550410, at most 0.1 m | +0.071 m (base +1.517) | PASS |
| H2 | WIND00NAM at 311100, at most 0.1 m | +0.132 m (base +2.998) | FAIL |
| H3 | L5Q at 550410, at most 10 mm | 22.7 mm, 20 rows (base 96.9) | FAIL |
| H3 | L5Q at 554610, at most 10 mm | 13.6 mm, 21 rows (base 133.9) | FAIL |
| H3 | L5X at 311100, at most 30 mm | 20.5 mm, 23 rows (base 344.9) | PASS |
| H4 | E26 at 550410, no decoupling, statistic at most 0.03 m | no decoupling, 0.0000 m | PASS |
| H4 | E07 at 554610, same | no decoupling, 0.0004 m | PASS |
| H4 | E08 at 311100, same | no decoupling, 0.0008 m | PASS |
| H5 | `icl2_c129_24` with `icl5_g129_24`, median shift ratio within 25 % of the median variance ratio | 1.00 and 1.00, 26 epochs | PASS |
| H5 | `icl2_p132_24` with `icl5_b132_24`, same | 1.00 and 1.00, 14 epochs | PASS |

The failed clauses stay failed. Sections 4.2, 4.4 and 4.5 read them; they move no bar.

### 4.2 H1: the monitor has no row at a trip

The monitor reads the epoch's last state. A tripped satellite's clock key is removed before the same-epoch re-solve (`ClockAnchor.cpp` lines 972 to 988), and the statistic skips a satellite with no clock key (line 135). The Galileo row count shows it: 24, 23 at the trip, 24 again (E08's epoch: 23, 22, 23). The pre-registration did not foresee this, so three clauses are unread and stay failed. Read beside them, not as a gate:

| trip | arm | pair, epoch before | pair, epoch after | statistic, m | predicted on the pair before, m |
| --- | --- | --- | --- | --- | --- |
| E26, 550410 | `icl6_mg132` | L1C+L5X | L1C+L5X | +0.125 | 2.2606 x 0.0540 = +0.122 |
| E07, 554610 | `icl6_mg132` | L1C+L5X | L1C+L5X | +0.166 | 2.2606 x 0.0743 = +0.168 |
| E08, 311100 | `icl6_mg129` | L1X+L5Q | L1X+L5X | +0.405 | 2.2606 x 0.1923 = +0.435 |

A same-family pair reads about 0: the two weights cancel the ionosphere ratio. Correction to section 2.2: its E08 row is written with the band 5 weight, 1.2606 x 0.3449. The two forms are equal in size by that ratio, but on a pair holding L5X and not L1X the statistic has the opposite sign, so the observed +0.405 m belongs to L1X.

E08's history explains why only the arm trips with the same shift in both runs. Both runs hold L1X+L5X from 294690. The arm moves to L1X+L5Q at 310830 and stays until 311130. The control moves to L1X+L5Q at 310740 and on to L1C+L5Q at 311040, leaving the mixed pair 60 s before the push at 311100. The difference between the runs is the timing of one pair change.

### 4.3 The reader over the day

Reported without a bar. The pair is read at the epoch before where the monitor has no row at the trip.

| arm | `phi` trips after hour 1, in epochs | Galileo, on a mixed pair | GPS | Galileo satellite-epochs on a mixed pair | pair changes, Galileo, GPS |
| --- | --- | --- | --- | --- | --- |
| `icl6_mc129` | 6, 5 | 3, 2 | 3 | 1 755 of 64 868 (2.7 %) | 313, 188 |
| `icl6_mg129` | 5, 5 | 2, 2 | 3 | 1 640 of 64 869 (2.5 %) | 220, 190 |
| `icl6_mc132` | 29, 8 | 4, 4 | 25 | 1 712 of 64 941 (2.6 %) | 171, 150 |
| `icl6_mg132` | 31, 10 | 6, 6 | 25 | 1 751 of 64 939 (2.7 %) | 176, 155 |

14 of 15 Galileo trips sit on a mixed pair (9 of 10 distinct trips, the arms share most of them), on a kind of pair held about 2.7 % of the time. The exception is E26 at 344430 in the control: L1X+L5X the epoch before, statistic -0.287 m, not traced, its pair at the epoch unread.

The key changes which satellites trip, not how many: 35 `phi` trips in the two golden controls, 36 with the key.

GPS pairs are L1C+L2W and L1W+L2W (9 and 16 on DOY 132, 1 and 2 on DOY 129, the same in both runs). They share the L2W column, so the two-family reading does not carry over, and the GPS trips are not decomposed here. On DOY 132 they come in groups at 524700, 524970 and 527730, with signs and sizes differing by satellite on the same pair. They share the source (section 4.7): all of them go under the floor, and the largest GPS DCB step at those three epochs falls from 0.295, 0.286 and 0.279 m to 0.009, 0.007 and 0.004 m.

### 4.4 The source

At the three named epochs the floor shrinks the step 44, 21 and 23 times (DARW00AUS, MAJU00MHL, WIND00NAM). WIND00NAM's +0.132 m misses the 0.1 m bar; 10 Galileo arcs on 5 satellites are freed at that epoch.

The count on `icl6_fg132` misses its bar by one station's steps. Of the 12, 9 are at BSHM00ISR, 2 at WARK and 1 at SAVO. Five of BSHM's 9 have arcs dropped but no fold at that epoch and lie on the station's slow fall from a step that no freed arc explains: at 586320 its GPS DCB steps +335 m in the base and +422 m in the floor arm, then falls at first about 0.75 m per epoch under the floor. BSHM holds 322 of the arm's 327 steps of at least 0.3 m, nearly all on that fall. The step itself is not traced here.

The force balance of section 2.3 holds at a prior of 100 m2 against about 1 m2, with no parameter changed (observed, predicted, m):

| run | step | observed | predicted |
| --- | --- | --- | --- |
| `icl6_fg132` | BSHM 586140 | -8.749 | -8.831 |
| `icl6_fg132` | BSHM 585330 | -1.101 | -1.110 |
| `icl6_fg132` | WARK 523290 | +0.795 | +0.806 |
| `icl6_fg132` | WARK 544170 | +0.374 | +0.383 |
| `icl6_fg129` | SYOG 299100 | +0.349 | +0.351 |
| `icl6_fg129` | SYOG 299130 | -0.344 | -0.346 |
| `icl6_fg129` | BSHM 335670 | -0.326 | -0.328 |

Regression, as in section 2.3: slope 0.951, r 0.995 over 11 steps (9 of the same sign; the other 2 are on BSHM's fall) and slope 0.994, r 1.000 over 3 steps.

Section 2.3 left the steps without a freed arc unclassified. A T4 drop at their epoch accounts for few of them: 5 of 34 on `icl5_g132_24`, 7 of 29 on `icl5_g129_24`.

The walk's reach, at the end of the day:

| | `icl5_g129_24` | `icl6_fg129` | `icl5_g132_24` | `icl6_fg132` |
| --- | --- | --- | --- | --- |
| median size of the Galileo DCB, m | 0.681 | 0.029 | 0.635 | 0.032 |
| stations over 2 m, Galileo | 7 | 0 | 8 | 1 |
| median size of the GPS DCB, m | 0.910 | 0.031 | 1.803 | 0.146 |
| stations over 2 m, GPS | 17 | 0 | 25 | 1 (BSHM) |
| largest Galileo DCB, m | YKRO +4.32 | KIT3 -0.94 | SGOC +21.76 | SGOC +2.13 |

DARW00AUS's Galileo DCB ends at +5.97 m in the base and +0.158 m in the floor arm.

### 4.5 The family and the amplification

H3's two failures have a cause section 3 left out. Its prediction shrinks the pull only. The floor also removes the tightening, so the family's prior stays soft and the same push moves it further. Base against floor arm (`icl6_push.py`, composite = push ratio x variance ratio):

| epoch, signal | shift, base, floor arm, mm | shift ratio | anchors' DCB variance ratio | largest push, base, floor arm, m | push ratio | composite |
| --- | --- | --- | --- | --- | --- | --- |
| 550410, L5Q | +96.9, +22.7 | 0.23 | 4.82 | MAJU +1.517, +0.071 | 0.05 | 0.22 |
| 554610, L5Q | +133.9, +13.6 | 0.10 | 4.57 | DARW +2.780, +0.063 | 0.02 | 0.10 |
| 311100, L5X | +344.9, +20.5 | 0.06 | 1.33 | WIND +2.998, +0.132 | 0.04 | 0.06 |

The composite was written after H5's exceptions (below) and before the floor arms were read; it is not a frozen gate. Epochs with a shift ratio inside 25 % of each reading, `icl6_push.py`, at least 10 rows in both runs and a control shift of at least 2 mm:

| pair | epochs | by the variance ratio alone | by the composite |
| --- | --- | --- | --- |
| `icl2_c129_24` with `icl5_g129_24`, L5Q | 26 | 24 | 26 |
| `icl2_p132_24` with `icl5_b132_24`, L5Q | 14 | 12 | 12 |
| `icl2_c132_24` with `icl5_g132_24`, L5Q (in sample, this tool's count) | 15 | 14 | 14 |
| `icl5_g132_24` with `icl6_fg132`, L5Q | 3 | 0 | 3 |
| `icl5_g129_24` with `icl6_fg129`, L5X | 10 | 3 | 9 |

H5's medians alone are a weak test, because DOY 129 is nearly all ratio 1 and the broadcast pair crosses 1. The per-epoch tracking on the broadcast pair is the evidence, shift ratio against variance ratio: 535620 2.03, 2.09; 536970 1.78, 2.09; 543000 2.28, 2.14; 544470 2.88, 2.67; 549270 2.99, 2.81; 550410 3.01, 2.77; 551220 0.65, 0.58; 552630 0.77, 0.72; 554610 0.51, 0.59; 556650 0.62, 0.58. The exceptions to the variance ratio are each a different push (the run's key changes which arcs T4 or a flag names, and so how far the station moves):

| epoch | what differs in the arm | largest push, control, arm, m | push ratio | variance ratio | composite | shift ratio |
| --- | --- | --- | --- | --- | --- | --- |
| 296190, golden DOY 129 | T4 also names MAJU E12 | MAJU +1.397, +2.696 | 1.93 | 1.03 | 1.98 | 1.93 |
| 325950, golden DOY 129 | the key's `post_flag` G07 at MBAR, extra drops | DJIG -0.083, -0.232 | 2.79 | 1.01 | 2.81 | 2.83 |
| 565110, broadcast DOY 132 | `post_flag` SOLO E12, T4 also names E10 and E11 | SOLO +0.308, +0.592 | 1.92 | 0.53 | 1.02 | 0.98 |
| 565860, broadcast DOY 132 | `post_flag` SOLO E12 | SOLO +0.264, +0.366 | 1.39 | 0.53 | 0.73 | 0.72 |

The composite misses where no station gives a real push: broadcast 543000 (shift 2.28, composite 4.21; the "largest" step is MRC1 -0.009 to -0.017 m, an anchor carrying the shift) and 536970 (1.78, 2.29), golden DOY 132 571080 (1.29, 2.44).

The anchors also close on the family: anchor DCB step = -(gamma - 1) / gamma x band 5 family shift. Median step against prediction, mm: golden DOY 132 524610 -535.2, -535.3; broadcast 550410 -17.8, -17.7 and -53.5, -53.3; base at 311100 -152.4, -152.5 and floor arm -9.0, -9.1. A visible miss: golden DOY 132 523080 -57.6, -67.7.

### 4.6 The tightening, the link section 2.4 left open

The floor arm answers the first half directly. PALM00ATA's Galileo DCB precision (1 / std^2, per m2) across E34's two drops at 527700 and 527730, with the same flags in both runs:

| run | 527670 | 527700 | 527730 | 527760 | 527790 | 527820 | 527850 | 528000 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `icl5_g132_24` | 32.7 | 75.5 | 115.3 | 145.5 | 169.3 | 188.5 | 204.4 | 255.3 |
| `icl6_fg132` | 30.4 | 30.6 | 30.8 | 30.9 | 30.9 | 31.0 | 31.1 | 31.6 |

Under the floor the precision keeps the +0.1 per epoch it had before the drop. So the climb goes with the size of the prior, and the drop as such does not produce it.

A progress note during this trace read the long climb as a time update no longer exact after a drop. That reading is withdrawn. Decaying increments over hundreds of epochs are what an exact recursion gives when a fresh prior reaches the DCB through noisy rows. With `x` the arc's geometry-free coordinate (band 1 + x, band 2 + gamma x), `V_x = 1 / (1 / Vb1 + gamma^2 / Vb2)` the prior on it, `R` the rows' variance on it, from the arc's own first fold (`1 / R = 1 / var_post - 1 / V_x`), and `n` the epochs since the drop:

    P_n = P_0 + 1 / ((gamma - 1)^2 (V_x + R / n))

No parameter is fitted. E34 at PALM00ATA, `V_x` 0.00371 m2, `R` 0.0405 m2, limit from the prior alone 428.8:

| tow | 527700 | 527730 | 527760 | 527850 | 528000 | 528120 |
| --- | --- | --- | --- | --- | --- | --- |
| observed | 75.5 | 115.3 | 145.5 | 204.4 | 255.3 | 279.6 |
| form | 68.5 | 104.7 | 135.2 | 203.7 | 274.1 | 310.0 |

The form is not a general law. The first epoch's observed increment over the form's, median, Galileo, for a station epoch with one line of sight dropped on both bands by anything but T4, no T4 row at the station the epoch before (a T4 row takes effect in the next epoch's fold) and no other drop there in the 10 epochs before (`icl6_info3.py`):

| DCB precision before | `icl5_g132_24`, rows | ratio | `icl2_c132_24`, rows | ratio | `icl5_g129_24`, rows | ratio |
| --- | --- | --- | --- | --- | --- | --- |
| under 30 | 40 | 1.06 | 40 | 1.04 | 37 | 1.08 |
| 30 to 100 | 48 | 0.50 | 47 | 0.50 | 42 | 0.53 |
| 100 to 300 | 46 | 0.31 | 46 | 0.31 | 66 | 0.056 |
| 300 to 1 000 | 83 | 0.037 | 75 | 0.039 | 89 | 0.011 |
| 1 000 to 3 000 | 0 | | 9 | 0.045 | 0 | |
| over 3 000 | 4 | 0.38 | 4 | 0.42 | 0 | |

GPS on `icl5_g132_24` reads 1.43, 0.81, 0.55, 0.057 and 0.071 over the first five bins. The form describes the onset at a soft DCB, in the median and with wide scatter (quartiles about 0.7 to 1.5 under 30). It fails as the DCB stiffens (NIUM00NIU E31 at 561090, precision 675: +43.6 against +1.3; WARK E18 at DOY 129 314490, precision 368: +61.0 against +0.2), and it misses at PALM00ATA's T4 at 525870 in the control (E30 and E36, about +70 from the band 5 fold, against +28.9). How one station's prior tightens the DCB at every anchor together (section 2.4) is not derived here. What is measured is that the tightening goes with the prior's size.

PALM00ATA over DOY 132, the same precision: control 24.7 at hour 2, 689.6 at hour 3, 1 613 at hour 9, 6 552 at hour 15; base 24.7, 352.1, 613, 5 556; floor arm 22.8 at hour 2, 47.7 at hour 4, 133.1 at hour 10. Control over base is 1.96 at hour 3 and 2.63 at hour 9, then 1.18 once both jump near hour 15: section 2.4's 2 to 2.8 "until about hour 15". The floor slows the tightening and does not stop it: the median Galileo DCB deviation over the anchors reads 0.0757 and 0.1534 m at hour 8, 0.0376 and 0.0502 m at hour 22 (base, floor arm).

### 4.7 The floor arms against their bases

Reported without a bar. After hour 1 except the flag sets, which cover the whole day.

| | `icl5_g129_24` | `icl6_fg129` | `icl5_g132_24` | `icl6_fg132` |
| --- | --- | --- | --- | --- |
| IF-TD flags in the frozen set | 1 593 | 1 593, none differ | 1 957 | 1 957, none differ |
| of which `post_flag` | 467 | 467, none differ | 678 | 678, none differ |
| IF-TD flags after hour 1 | 1 501 | 1 501 | 1 888 | 1 888 |
| T4 arcs | 422 | 80 | 549 | 176 |
| decouplings | 60 | 55 | 84 | 55 |
| of which `phi`, Galileo, GPS | 2, 3 | 0, 0 | 6, 25 | 0, 0 |
| of which `datum`, in epochs | 55, 2 | 55, 2 | 53, 2 | 55, 2 |
| anchor slip handovers | 79 | 79 | 115 | 95 |
| epochs with s0 phase of at least 1.0 | 0 | 0 | 9 | 9 |
| median s0 phase | 0.319 | 0.317 | 0.327 | 0.327 |
| DCB steps of at least 0.3 m | 130 | 8 | 101 | 327 (5 without BSHM) |

The detectors read raw data, and the flag sets confirm it. The two mass `datum` decouplings stay, a different mechanism (`icl6_fg132`'s 2 extra are E18 and E34 at 524700, which the base had already decoupled by `phi` at 524610).

Every `phi` trip goes. Large statistics remain but do not trip: 16 and 8 guard rows of at least 0.12 m after hour 1, all `ok`, with the guard's own predicted size at least 0.106 m at those rows against its 0.02 m floor. In the floor arms no Galileo statistic exceeds 2.2 mm at 550410, 1.1 mm at 554610 and 2.6 mm at 311100. The floor arms carry no monitor, so their pairs are unread; a 22.7 mm shift on E26's mixed pair would read about 0.029 m.

The census against the base (`icl5_pool.py`; its PASS line is card ICL5's bar and does not apply here):

| | DOY 129 | DOY 132 |
| --- | --- | --- |
| paired median, hours 2 to 24 | -0.2 mm | -0.3 mm |
| WTZZ00DEU, hours 2 to 24 | +0.3 mm (0.0149 to 0.0152 m) | -1.0 mm (0.0172 to 0.0162 m) |
| sites worse, better by more than 1 cm | 0, 0 | 0, 0 |
| site range | BADG -0.8 to WUH2 +1.1 mm | ARHT -5.2 to ANMG +1.3 mm |
| worst hour of the census median | hour 11, +0.7 mm | hour 10, +1.7 mm |
| hour 1, census median | 0.142 to 0.142 m | 0.118 to 0.107 m |
| hour 1, WTZZ00DEU | 0.059 to 0.061 m | 0.074 to 0.063 m |

Card RF-3's hour 1 cost for this floor (+0.09 m at WTZZ00DEU, +0.034 m on the census) does not appear on this posture on these two days. Card RF-3's bars were not re-run, and the ledger's verdict stands.

## 5. Verdict

**M0, H4 and H5 pass. H1, H2 and H3 each fail clauses, and those stay failed.** H1 is unread at three trips because the monitor has no row there. H2 misses on one station's steps (12 against 6.7, 9 of them at BSHM00ISR) and on WIND00NAM (+0.132 against 0.1 m). H3 misses on L5Q (22.7 and 13.6 against 10 mm) because section 3's prediction left out the family's softer prior.

**The chain, each link with its evidence.**

1. A slip frees an arc, and the arc keeps its pre-slip level at birth variance. That prior pushes the station's common mode and its DCB by metres (force balance, r 0.998 on DOY 132, and within 2.4 % at the seven steps of section 4.4 at a prior of 100 m2).
2. At anchor stations that DCB and the signal family's phase biases are one mode, so the push spreads over the network as a shift of one family, in the ionosphere's ratio, invisible to every row. Its size is the push times the family's variance (composite within 25 % at 26 of 26, 12 of 14, 3 of 3 and 9 of 10 epochs).
3. The anchor statistic picks one phase bias per band independently. On a mixed pair, held about 2.7 % of the time, it reads the shift as a clock event (14 of 15 Galileo trips; the predicted statistic within 30 mm at the three trips, where the solved clock moves at most 3.8 mm).
4. The same fold tightens the family's prior, and the size of the prior sets how far (PALM00ATA +42.8 against +0.2 at the same drop).

**Does the fault lie with the key?** No. The trips read a defect that exists without the key. The key moves the family's variance through which T4 drops it removes, in either direction by epoch (0.53 to 2.8), and changes which satellites cross the bar: E26 at 550410 from 0.052 to 0.125 m and E07 at 554610 from 0.067 to 0.166 m on a softer prior, E08 by the timing of a pair change. `phi` trips are 35 in the two golden controls and 36 with the key. Card ICL5's clause stays failed (82 to 84); this is the reading of it that its section 5 asks an arming decision to weigh.

**The floor.** Used as an instrument, it removes all 36 `phi` trips on the key's two golden days, cuts T4 arcs 81 and 68 %, cuts the largest DCB values at the end of the day about ten times, and moves no census site by more than 5.2 mm over hours 2 to 24. That is more than this card set out to measure. It is not a recommendation to arm: see section 6.

Nothing is armed. Arming, any golden change and the merge are the author's decisions.

## 6. Limits and what remains

- **No fix is built.** Two routes follow from the chain, each default off. The reader: a same-family pair in `clockAnchorPhaseStatistic`, or the pair of least functional variance from `cov`; it would silence Galileo's mixed reads and leave the shift in place. The source: a response to a slip that does not keep the stale level at birth variance, either the floor or a prior re-centred by the measured per-band slip (`dL1 = dMW - f5 dGF / (f1 - f5)`, `dL5 = dL1 - dGF`), the route card RF-3 section 6 points to.
- **The floor is graded here only as an instrument.** Two field days at 24 h, with the key on. No sim bed, no 6 h baseline, none of card RF-3's bars, no double-difference or integer grade, no resume seam. At the 24 guard rows of section 4.7 the test's own predicted size is large, so a real clock event there would pass.
- **GPS trips are not decomposed.** 28 of the 36 on the golden arms. They go under the floor; why they read as they do on pairs sharing L2W is open.
- **Not traced:** E26 at 344430 on a same-family pair; BSHM00ISR's GPS DCB step at 586320; the 34 and 29 DCB steps with no freed arc and mostly no T4 drop; how a prior at one station tightens every anchor's DCB (section 4.6); the form's misses at a stiff DCB.
- **Two days, one network.** DOY 129 and 132, on the golden and, for H5 only, the broadcast posture.
