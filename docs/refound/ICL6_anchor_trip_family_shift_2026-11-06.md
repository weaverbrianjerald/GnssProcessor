# ICL6: why the clock anchor trips alone: a slip's stale level, a signal family's common mode, a mixed statistic

**STATUS**

- **id:** ICL6
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-06, asked for by the author after card ICL5, whose gate R3 failed on the golden at DOY 132 through two lone Galileo anchor decouplings with the same flags in both runs and a larger statistic on one satellite. Measured first: the anchor statistic of those satellites mixes two signal families and reads a common shift of one family, invisible to every row, as a clock event; the shift comes from a freed arc's pre-slip level pushing a receiver's DCB gauge by metres; and it is about 2.5 times larger in the arm because the key removes a T4 drop that had tightened the family's prior hours earlier. Gates freeze the reader, the source and the amplification before the arms are read.
- **source:** card [ICL5](ICL5_reseed_key_over_a_day_2026-11-06.md) sections 4.3 and 6 (the failed clause, the owed trace); card [RF-1](RF-1_float_ambiguities_design.md) section 3.4 (the drop keeps `value_abs`); card [RF-3](RF-3_code_row_qc_2026-09-22.md) sections 1 and 5.1 (the walk on the arc itself, the drop floor); card [PVN](PVN_phase_visible_nees_2026-09-29.md) (the phase-visible monitor)
- **knobs / posture:** no code change and no new key. Existing keys in one-change arms: `network.phase_visible_monitor` (measure-only), `network.amb_float_drop_var_m2` (KEEP_OFF, used as an instrument). Bases: `icl2_c{129,132}_24` (the GPS + Galileo golden), `icl5_g{129,132}_24` (the same with `network.cycle_slip.if_phase_td_reseed_on_flag`).
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `icl6/anchor-trip-family` from `icl5/reseed-key-day` (`0d40d22`), worktree `D:/GnssProcessorSupport/worktrees/icl2`; exe `gnss_processor_icl3q1.exe` (sha256[:16] `92743f1fb3c747fb`), census `gnss_processor_icl2c.exe`; arms `D:/GnssProcessorSupport/runs_nom/_rap_icl6_{mc,mg,fg}{129,132}`; scripts `D:/GnssProcessorSupport/worktrees/icl3_tools/icl6_*` (outside the repository)
- **gates (frozen at pre-registration):** section 3

**Outcome.** Open. Measured before the gates: the lone anchor trips read a phase-invisible shift of one signal family through a statistic that mixes families; a freed arc's stale level is the push. The arms are graded in section 4.

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
