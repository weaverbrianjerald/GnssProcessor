# ICL5: the IF-TD post-flag re-seed over a day, on both postures

**STATUS**

- **id:** ICL5
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered and closed 2026-11-06, asked for by the author after card ICL4: `network.cycle_slip.if_phase_td_reseed_on_flag` over 24 h on DOY 129 and 132, on the golden and on the broadcast posture, with the 16-site census. R0, R1, R2, R4, R6 and S pass on every arm. R3 fails on the golden at DOY 132 (decouplings 82 to 84: two lone Galileo trips, receivers unmoved, cause not traced). R5 fails on the broadcast posture at DOY 132 on its hour clause alone (hour 5 +11.7 mm, hour 14 +5.2 mm; paired median -1.5 mm, WTZZ00DEU -1.6 mm). Card ICL2's D2 on DOY 129 passes with the key alone (58.3 to 15.7 mm, 9.49 to 1.94 mm). Nothing armed.
- **source:** card [ICL4](ICL4_post_flag_slip_scatter_2026-11-06.md) sections 4 to 6 (the cause and the owed item); card [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) sections 13 and 14 (the key, its 6 h field arm); card [ICL2](ICL2_frame_day_one_level_2026-11-03.md) (the controls and D2's bars); card [ICL3](ICL3_frame_quorum_2026-11-05.md) (the exe's off path)
- **knobs / posture:** the key under grade, unchanged in every default: `network.cycle_slip.if_phase_td_reseed_on_flag`. Arms are copies of `icl2_p{129,132}_24` (card BRD's GPS + Galileo broadcast posture with card ICL's frame) and `icl2_c{129,132}_24` (the GPS + Galileo golden), one key changed.
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `icl5/reseed-key-day` from `icl4/post-flag-slip` (`5771660`), worktree `D:/GnssProcessorSupport/worktrees/icl2`; exe `gnss_processor_icl3q1.exe` (sha256[:16] `92743f1fb3c747fb`), census `gnss_processor_icl2c.exe`; arms `D:/GnssProcessorSupport/runs_nom/_rap_icl5_{b,g}{129,132}_24` and `_rap_icl5_s_2`; census keys `i5b{129,132}`, `i5g{129,132}` against `i2p{129,132}`, `i2c{129,132}`; scripts `D:/GnssProcessorSupport/worktrees/icl3_tools/icl5_*` (outside the repository)
- **gates (frozen at pre-registration):** section 3

**Outcome.** Over 24 h on DOY 129 and 132 the key removes the post-flag events on both postures: one flag before the solve stands in for several arcs dropped after it, so T4 arcs halve and refloats fall 2 to 6 % net. On the broadcast posture the level meets card ICL2's bars on both days without the quorum. Golden receivers do not move; broadcast receivers stay flat over the day. Two clauses fail: 2 extra lone decouplings on the golden at DOY 132, and two hours' census median on the broadcast posture at DOY 132. Nothing armed.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

Card ICL4 traced the largest natural events on DOY 129 and 132 to a slip in the epoch after an IF-TD flag, and showed on short arms that `network.cycle_slip.if_phase_td_reseed_on_flag` removes them on both postures. It did not grade receivers: one receiver on three short arms moved -2.40, -0.20 and +1.60 mm. Card ARL's 6 h grade reads consumer-neutral but holds none of these events. The question for an arming decision: over whole days, on the golden and on the broadcast posture, what does the key gain and what does it cost?

## 2. The controls, measured before the gates (2026-11-06)

The existing 24 h arms, read with `icl4_postflag.py` and `icl5_totals.py`. After hour 1:

| control | IF-TD refloats | T4 arcs | decouplings | epochs s0 phase >= 1.0 | largest decouplings in one epoch | slip handovers |
| --- | --- | --- | --- | --- | --- | --- |
| `icl2_p129_24`, broadcast | 1 071 | 786 | 32 | 3 | 344430: 19 | 113 |
| `icl2_p132_24`, broadcast | 1 349 | 1 044 | 48 | 12 (9 in the last minutes) | 529770: 11 | 144 |
| `icl2_c129_24`, golden | 1 227 | 802 | 61 | 0 | 276150: 31, 271230: 24 | 79 |
| `icl2_c132_24`, golden | 1 439 | 1 059 | 82 | 9 (all in the last minutes) | 529950: 31, 524700: 30 | 115 |

Post-flag arcs with |dGF| of at least 3 m that nothing answered at their own epoch: 3 and 8 on the broadcast posture, 3 and 6 on the golden. On the golden they are mild: the largest footprints are 344430 (s0 phase 0.73, 37 arcs, 2 decouplings) and 529770 (0.41, 6 arcs, 0).

The controls ran on the exe `gnss_processor_icl2d.exe`. Card ICL3 showed `gnss_processor_icl3q1.exe` reproduces them with its own key off: `icl3_q129_24` byte-identical to `icl2_p129_24` up to its first action, `icl3_q132_6` to `icl_p132_6`, and the 2 h golden arms (Q0, Q1). R0 below tests the prefix of each arm again. The golden's mass decouplings at 271230, 276150, 524700 and 529950 belong to other events; they are read, not claimed.

## 3. Gates (frozen 2026-11-06, before any run of this card)

Arms: `icl5_b129_24`, `icl5_b132_24` (broadcast posture), `icl5_g129_24`, `icl5_g132_24` (golden), each a copy of its control with the key true; `icl5_s_2`, a copy of `icl3_s_2` (the sim golden, 2 h, same exe) with the key true. "After hour 1" is from the day's second hour; DOY 132 comparisons of s0 phase stop at tow 604500, before the end-of-day ramp that both arms share and that card ICL4 does not claim.

| gate | bar |
| --- | --- |
| R0, identity | Each arm's `products.txt` (without comment lines) byte-identical to its control up to the first epoch at which the IF-TD flag set differs from the control's. |
| R1, the events | Broadcast posture at 344430, 529770, 581790, 592890; golden at 344430, 529770: the arc flagged by IF-TD at its own epoch with the detail `post_flag`, and at that epoch s0 phase below 1.0, decouplings at most 2, T4 arcs at most 10. |
| R2, the population | Post-flag arcs with |dGF| of at least 3 m that nothing answered at their own epoch (`icl4_postflag.py`): at most 1 per arm, each named. |
| R3, the day's footprint | After hour 1: 2 880 of 2 880 epochs emitted; epochs with s0 phase >= 1.0, T4 arcs and decouplings each at most the control's; no epoch of at least 5 decouplings that the control lacks. |
| R4, the cost in arcs | After hour 1: IF-TD refloats plus T4 arcs at most the control's sum + 25 %. |
| R5, the receivers | Census against the control's, the site's mean hourly p50, over hours 2 to 24 of each arm: paired median at most +0.001 m; WTZZ00DEU's change at most +0.003 m; at most 1 of the 16 sites worse by more than 0.01 m; the census median of any hour from 2 to 24 at most +0.005 m against the control's. |
| R6, the level (broadcast posture) | Card ICL2's D2 bars over hours 2 to 24 on each day: phase-visible D against CODE at lag 120 at most 35 mm; the closed-loop receiver's offset one-epoch rms at most 2.0 mm. |
| S, the sim bed | `icl5_s_2`: `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical to `icl3_s_2`. If they differ, the IF-TD flags that differ are listed and the difference is graded against truth with `rapid_arm.py score`. |

Reported without a bar: WTZZ00DEU and the census median by hour; the receivers over the 10 minutes after each event of R1; the key's run-end report; the extra flags by station and by size; slip handovers; what the frame's vote does at DOY 129 344430 with no quorum.

Each clause is reported for each arm. A failed clause stays failed; no bar moves after this section is committed. Nothing is armed by this card: arming, any golden change and the merge are the author's decisions.

## 4. Results

*2026-11-06. All five arms exit 0 on `gnss_processor_icl3q1.exe` (the four day arms 8 791, 8 713, 5 954 and 4 160 s, under a load other than the controls', so no timing claim); 17 of 17 census replays exit 0 on each of the four keys. Every producer total below was read twice, by `icl5_grade.py` and by a separate count over `network_events.csv` and `network_epoch_status.csv`; the two agree.*

### 4.1 The gates

R0, R1, R2, R4, R6 and S pass on every arm. Two clauses fail and stay failed: R3's decouplings on `icl5_g132_24`, and R5's hour clause on `icl5_b132_24`.

| gate | `icl5_b129_24` (broadcast) | `icl5_b132_24` (broadcast) | `icl5_g129_24` (golden) | `icl5_g132_24` (golden) |
| --- | --- | --- | --- | --- |
| R0: first differing line, first differing flag set | 259260, 259260: PASS | 518460, 518460: PASS | 259380, 259260: PASS | 518580, 518460: PASS |
| R1, the events | PASS (table below) | PASS, all three | PASS | PASS |
| R2: unanswered post-flag arcs (control's count) | 0 (3): PASS | 0 (8): PASS | 0 (3): PASS | 0 (6): PASS |
| R3: epochs emitted | 2 880 of 2 880 | 2 880 of 2 880 | 2 880 of 2 880 | 2 880 of 2 880 |
| R3: epochs of s0 phase >= 1.0 | 3 to 2 | 4 to 0 | 0 to 0 | 0 to 0 |
| R3: T4 arcs | 786 to 415 | 1 044 to 489 | 802 to 422 | 1 059 to 549 |
| R3: decouplings | 32 to 13 | 48 to 24 | 61 to 60 | **82 to 84: FAIL** |
| R3: epochs of >= 5 decouplings new in the arm | none: PASS | none: PASS | none: PASS | none |
| R4: IF-TD refloats + T4 arcs, bar +25 % | 1 857 to 1 746 (-6.0 %): PASS | 2 393 to 2 269 (-5.2 %): PASS | 2 029 to 1 923 (-5.2 %): PASS | 2 498 to 2 437 (-2.4 %): PASS |
| R5: census paired median, bar +0.001 m | +0.0002 | -0.0015 | 0.0000 | 0.0000 |
| R5: WTZZ00DEU, bar +0.003 m | +0.0011 (0.0147 to 0.0158) | -0.0016 (0.0269 to 0.0253) | 0.0000 (0.0149) | 0.0000 (0.0172) |
| R5: sites worse by > 0.01 m, bar 1 | 0 | 0 | 0 | 0 |
| R5: worst hour, bar +0.005 m | hour 2 +0.0046: PASS | **hour 5 +0.0117, hour 14 +0.0052: FAIL** | hour 24 +0.0003: PASS | hour 5 +0.0006: PASS |
| R6: phase-visible D at lag 120, bar 35 mm (control) | 15.7 mm (58.3): PASS | 17.1 mm (18.9): PASS | not gated | not gated |
| R6: receiver's offset one-epoch rms, bar 2.0 mm (control) | 1.94 mm (9.49): PASS | 0.84 mm (0.87): PASS | not gated | not gated |

R1, control to arm at the epoch of each event:

| event | arm | IF-TD flag, `post_flag` | s0 phase | decouplings | T4 arcs | retro bumps |
| --- | --- | --- | --- | --- | --- | --- |
| DOY 129 344430, YKRO00CIV E26 | `icl5_b129_24` | 94.127 m | 5.85 to 0.31 | 19 to 0 | 100 to 1 | 19 to 0 |
| DOY 132 529770, SYOG00ATA E19 | `icl5_b132_24` | 95.817 m | 2.33 to 0.31 | 11 to 0 | 43 to 0 | 11 to 0 |
| DOY 132 581790, SYOG00ATA G16 | `icl5_b132_24` | 17.034 m | 1.20 to 0.36 | 6 to 0 | 14 to 1 | 6 to 0 |
| DOY 132 592890, SYOG00ATA G14 | `icl5_b132_24` | 16.219 m | 1.16 to 0.42 | 3 to 0 | 11 to 2 | 3 to 0 |
| DOY 129 344430, YKRO00CIV E26 | `icl5_g129_24` | 94.131 m | 0.73 to 0.28 | 2 to 0 | 37 to 1 | 2 to 0 |
| DOY 132 529770, SYOG00ATA E19 | `icl5_g132_24` | 95.819 m | 0.41 to 0.32 | 0 to 0 | 6 to 0 | 0 to 0 |

R2: the population with |dGF| of at least 3 m in a post-flag epoch and no IF-TD flag at that epoch falls from 22, 37, 22 and 40 arcs in the controls to 1, 0, 0 and 0. The one is answered at its own epoch by LLI (`icl5_b129_24`, 259800, MAC100AUS E26, dGF +3.14 m, s0 phase 0.31). None stands in the solve.

S: `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv` of `icl5_s_2` are byte-identical to `icl3_s_2`. Both hold 0 IF-TD flags, and the key's run-end line reads 0 kept and 0 evaluated, so S is a same-build no-op check and does not exercise the key.

### 4.2 Two corrections to the reading

- **R0 on `icl5_b132_24`.** `icl5_grade.py` first printed FAIL. It dated a line by the last `E` record, and an epoch's `H` record comes before its `E` record. The first differing line is the `H` record of the epoch 518460 (its own time field), and the first differing flag is at the same epoch (G06 at SAVO00BRA, `post_flag`, 2.948 m). The tool now dates lines by `H` or `E`; the bar is unchanged and R0 passes on the raw file.
- **Section 2, `icl2_p132_24`.** "12 (9 in the last minutes)" reads 12, of which 4 before tow 604500 and 8 after. The gate uses 4. The golden's "9 (all in the last minutes)" stands.

### 4.3 The failed clause in R3: lone decouplings

On `icl5_g132_24` all 9 of the control's decoupling epochs recur with identical sets (82 decouplings), and two epochs are added, each with one Galileo satellite. `icl5_g129_24` loses the 2 at 344430 and adds one in the same way, and `icl5_b132_24` adds the first of the two at the same epoch. The IF-TD flags at these epochs are the same in both runs and, apart from E08's own flag at WIND00NAM at its epoch, no flag on the satellite lies within the 20 minutes before. The anchor's phase-visible statistic on that one satellite is larger in the arm, on the guard's first pass, against the bar of 0.12 m (6 x the 0.02 m floor). On the golden arms the other Galileo statistics at those epochs agree within 1 mm. The statistic is not quiet in the controls: at 550410, E15 and E26 already read -0.045 and +0.052 m on the golden and -0.058 and +0.053 m on the broadcast posture, where the arm's E15 reads +0.001 m.

| epoch (what both runs hold there) | arm | statistic on the satellite, control to arm | S, 10 minutes after, m | paired median | WTZZ00DEU | most worse, most better |
| --- | --- | --- | --- | --- | --- | --- |
| DOY 129 311100, E08 (12 arcs flagged at WIND00NAM) | `icl5_g129_24` | 0.000 to 0.405 m | 0.0344 to 0.0344 | 0.0000 | 0.0000 | QUIN +0.0003, ARHT -0.0064 |
| DOY 132 550410, E26 (E19 at MAJU00MHL, 67.1 m) | `icl5_g132_24` | 0.052 to 0.125 m | 0.0204 to 0.0204 | 0.0000 | +0.0001 | BADG +0.0002, ARHT -0.0013 |
| DOY 132 554610, E07 (E24 re-entry at DARW00AUS) | `icl5_g132_24` | 0.067 to 0.166 m | 0.0240 to 0.0241 | 0.0000 | +0.0001 | HARB +0.0004, ARHT -0.0003 |
| DOY 132 550410, E26 | `icl5_b132_24` | 0.053 to 0.161 m | 0.0405 to 0.0393 | -0.0013 | +0.0009 | DAV100ATA +0.0244, WUH200CHN -0.0272 |

On the golden arms the postfit phase rows of the satellite at those epochs agree within 1 mm between the runs, the satellite's solved clock moves across the epoch by at most 3.8 mm in the arm and 2.7 mm in the control, and the receivers do not register the trips. The cause of the larger statistic is not traced: the trace does not record which signal per band the statistic took, nor the first pass's solved values.

### 4.4 The failed clause in R5: hours 5 and 14 on the broadcast posture at DOY 132

The day-level clauses pass: paired median -1.5 mm, WTZZ00DEU -1.6 mm, no site worse by more than 2.4 mm (UTQI), and the largest improvements CEDU -6.6 and ARHT -6.4 mm. By hour, the census median over hours 2 to 24 changes by -0.4 mm on average, from -8.4 mm (hour 21) to +11.7 mm (hour 5); hour 14 is +5.2 mm.

Hour 5 goes from 0.049 to 0.061 m, and the 16 sites are bimodal in that hour. No site of the control lies between 0.041 and 0.058 m, so the median falls across a gap (ranks 8 and 9: DAV1 0.041 and HARB 0.058 in the control, BAKE and QUIN 0.061 in the arm). ARHT crossing it (0.032 to 0.091 m) moves rank 8. Site by site, in mm: ARHT +59.2, POHN +47.0, HARB +21.9, CEDU +8.9; SCRZ -9.4, DAV1 -9.3, ASCG -8.3, BADG -4.9, WUH2 -3.0, STPM -2.4; WTZZ00DEU -3.3; the other six within 1.6. Hour 14: POHN +13.2, UTQI +9.7, KOKB +7.4, ARHT +6.7, DAV1 +6.6; QUIN -6.0, CEDU -4.8, CHTI -4.3.

ARHT, POHN and CEDU stay within 20 mm of the control from the start of hour 2 up to 529770, the event the key removes, and differ by 20 mm or more at it (ARHT and POHN by 50 mm or more). HARB had parted earlier (20 mm from the start of hour 2, 50 mm at 525840), so its change is not this event's alone. After the event ARHT's 5-minute median error is larger in the arm by 1 to 10 cm until about tow 538800, from a standing error of about 0.14 m in both runs before it. The golden, whose control does not decouple at that epoch, moves hour 5 by +0.6 mm. This reads as the control's scatter happening to pull some receivers' standing errors down and others up, and the arm losing both. It is a reading, not an isolated cause: no arm separates the removed event from the key's other changes before hour 5.

### 4.5 Reported without a bar

**The receivers over the 10 minutes after each event of R1** (`icl3_window.py`; S is the median over the sites of the site's median 3D error):

| event | posture | S, m | WTZZ00DEU, m | paired median, m | sites worse / better by > 2 cm | most worse, most better |
| --- | --- | --- | --- | --- | --- | --- |
| DOY 129 23:40:30 | broadcast | 0.0467 to 0.0472 | 0.0081 to 0.0075 | -0.0024 | 0 / 2 | HARB +0.0166, KOKB -0.0624 |
| DOY 129 23:40:30 | golden | 0.0256 to 0.0256 | 0.0299 to 0.0299 | 0.0000 | 0 / 0 | ARHT +0.0053, DAV1 -0.0026 |
| DOY 132 03:09:30 | broadcast | 0.0625 to 0.0614 | 0.0486 to 0.0459 | +0.0001 | 1 / 1 | ARHT +0.0226, DAV1 -0.0232 |
| DOY 132 03:09:30 | golden | 0.0307 to 0.0304 | 0.0195 to 0.0194 | 0.0000 | 0 / 0 | QUIN +0.0005, ARHT -0.0172 |
| DOY 132 17:36:30 | broadcast | 0.0398 to 0.0365 | 0.0086 to 0.0063 | -0.0007 | 0 / 2 | KOKB +0.0148, ARHT -0.0381 |
| DOY 132 20:41:30 | broadcast | 0.0493 to 0.0433 | 0.0175 to 0.0143 | -0.0058 | 0 / 3 | BADG +0.0077, HARB -0.0569 |

**The census median and WTZZ00DEU by hour**, m, hours 1 to 24, control then arm:

```
DOY 129 broadcast, census  0.192 0.051 0.042 0.038 0.051 0.039 0.035 0.040 0.043 0.033 0.034 0.034 0.039 0.040 0.043 0.046 0.061 0.056 0.042 0.052 0.054 0.048 0.045 0.051
                           0.197 0.056 0.044 0.038 0.050 0.039 0.035 0.039 0.044 0.034 0.033 0.034 0.038 0.039 0.043 0.047 0.061 0.056 0.041 0.052 0.055 0.048 0.045 0.049
DOY 129 broadcast, WTZZ    0.059 0.026 0.016 0.013 0.015 0.010 0.006 0.008 0.008 0.007 0.010 0.013 0.023 0.015 0.014 0.044 0.029 0.011 0.011 0.009 0.012 0.012 0.012 0.011
                           0.059 0.033 0.025 0.019 0.017 0.014 0.007 0.007 0.008 0.007 0.009 0.011 0.022 0.015 0.015 0.044 0.030 0.012 0.010 0.009 0.012 0.013 0.014 0.011
DOY 132 broadcast, census  0.237 0.092 0.071 0.057 0.049 0.043 0.044 0.048 0.042 0.046 0.040 0.036 0.038 0.042 0.043 0.039 0.040 0.035 0.033 0.037 0.046 0.048 0.034 0.032
                           0.243 0.087 0.069 0.052 0.061 0.047 0.046 0.043 0.047 0.040 0.034 0.030 0.035 0.047 0.044 0.038 0.041 0.038 0.034 0.039 0.038 0.043 0.035 0.036
DOY 132 broadcast, WTZZ    0.062 0.046 0.045 0.040 0.022 0.029 0.044 0.047 0.046 0.026 0.023 0.024 0.015 0.015 0.019 0.016 0.021 0.010 0.028 0.016 0.031 0.024 0.014 0.017
                           0.063 0.049 0.046 0.038 0.019 0.026 0.042 0.047 0.049 0.027 0.024 0.021 0.014 0.013 0.019 0.015 0.021 0.010 0.024 0.013 0.019 0.019 0.013 0.014
DOY 129 golden, census     0.143 0.050 0.036 0.029 0.032 0.030 0.023 0.027 0.026 0.023 0.027 0.024 0.030 0.028 0.038 0.032 0.026 0.033 0.031 0.027 0.027 0.024 0.029 0.028
                           0.142 0.049 0.036 0.029 0.032 0.030 0.023 0.027 0.026 0.023 0.027 0.024 0.030 0.028 0.038 0.032 0.026 0.033 0.031 0.027 0.027 0.024 0.029 0.028
DOY 129 golden, WTZZ       0.059 0.012 0.023 0.012 0.017 0.012 0.016 0.009 0.008 0.011 0.010 0.008 0.012 0.012 0.008 0.011 0.010 0.017 0.012 0.013 0.016 0.029 0.035 0.030
                           0.059 0.012 0.023 0.012 0.017 0.012 0.016 0.009 0.008 0.011 0.010 0.008 0.012 0.012 0.008 0.011 0.010 0.017 0.012 0.013 0.017 0.029 0.035 0.030
DOY 132 golden, census     0.117 0.068 0.051 0.036 0.034 0.027 0.023 0.024 0.023 0.022 0.029 0.024 0.028 0.028 0.028 0.026 0.025 0.025 0.032 0.023 0.026 0.030 0.028 0.026
                           0.118 0.068 0.051 0.036 0.035 0.028 0.023 0.024 0.024 0.022 0.029 0.024 0.028 0.028 0.027 0.026 0.025 0.025 0.032 0.023 0.026 0.030 0.028 0.026
DOY 132 golden, WTZZ       0.070 0.018 0.015 0.017 0.019 0.009 0.011 0.019 0.026 0.012 0.015 0.017 0.015 0.029 0.024 0.016 0.012 0.017 0.013 0.019 0.024 0.016 0.019 0.013
                           0.074 0.019 0.014 0.017 0.019 0.009 0.011 0.019 0.026 0.012 0.015 0.017 0.015 0.029 0.024 0.016 0.012 0.017 0.013 0.019 0.024 0.016 0.019 0.013
```

Site changes over hours 2 to 24, mm. Broadcast DOY 129: ARHT -3.6 to BAKE +1.1. Broadcast DOY 132: CEDU -6.6 to UTQI +2.4. Golden DOY 129: ARHT -1.0 to QUIN +0.2. Golden DOY 132: STPM -0.2 to ARHT +0.3. On the broadcast posture at DOY 129, WTZZ00DEU over hours 2 to 6 is +2 to +9 mm and the census median of hour 2 is +4.6 mm, with no event in those hours in either run; the cause is not traced.

**The extra flags and the T4 arcs.** After hour 1, IF-TD flags go 1 071 to 1 331, 1 349 to 1 780, 1 227 to 1 501 and 1 439 to 1 888 (+22 to +32 %), all refloats, and T4 arcs fall by more than the flags rise on every arm. The two sit at the same stations:

| arm | IF-TD flags, control to arm (T4 arcs, control to arm) |
| --- | --- |
| `icl5_b129_24` | MBAR 258 to 357 (189 to 57); SOLO 122 to 182 (142 to 32); MAC1 335 to 366 (24 to 28); DJIG 61 to 78 (12 to 8) |
| `icl5_b132_24` | SOLO 303 to 438 (221 to 44); NIUM 182 to 269 (138 to 39); DARW 205 to 283 (216 to 84); REYK 177 to 221 (21 to 21) |
| `icl5_g129_24` | MBAR 289 to 392 (250 to 60); SOLO 154 to 223 (131 to 22); MAC1 444 to 481 (21 to 21); DJIG 66 to 83 (16 to 12) |
| `icl5_g132_24` | SOLO 312 to 458 (276 to 67); DARW 255 to 349 (270 to 107); NIUM 202 to 280 (174 to 73); REYK 242 to 303 (18 to 20) |

Arc by arc (`icl5_swap2.py`), an "extra" flag being one the control does not hold on that arc at that epoch, most T4 arcs that the arm no longer drops are not the flagged arc itself:

| arm | T4 arcs gone (new) | at that epoch in the arm: same arc has an extra flag | another arc of the station has one | another station has one | no extra flag | epochs with T4 arcs | epochs of >= 10 T4 arcs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `icl5_b129_24` | 391 (20) | 59 (15 %) | 222 (57 %) | 99 (25 %) | 11 (3 %) | 322 to 276 | 3 to 1 |
| `icl5_b132_24` | 598 (43) | 139 (23 %) | 313 (52 %) | 112 (19 %) | 34 (6 %) | 419 to 305 | 8 to 4 |
| `icl5_g129_24` | 394 (14) | 73 (19 %) | 267 (68 %) | 32 (8 %) | 22 (6 %) | 322 to 267 | 2 to 0 |
| `icl5_g132_24` | 560 (50) | 163 (29 %) | 365 (65 %) | 0 (0 %) | 32 (6 %) | 443 to 328 | 4 to 3 |

So one flag before the solve stands in for several arcs T4 dropped after it, mostly at the same station: 94 to 97 % of the arcs gone fall at an epoch with an extra flag. The larger share at another station on the broadcast posture (25 and 19 % against 8 and 0 %) is consistent with the scatter of card ICL4.

Flags with the detail `post_flag` after hour 1: 402, 631, 430 and 655, of median size 0.505, 0.476, 0.589 and 0.589 m; at least 1 m: 116, 200, 145 and 234; at least 10 m: 12, 26, 14 and 29. They concentrate: MBAR, SOLO and MAC1 hold 306 of 402 on `icl5_b129_24`; SOLO, NIUM and DARW hold 458 of 631 on `icl5_b132_24`.

A new shape: runs of consecutive flags on one arc. A flag leaves no gradient in the controls, so every control run has length 1 (by construction). In the arms:

| arm | runs of length 1 | length 2 | length 3 or more | longest | flags in runs of 3 or more, by station |
| --- | --- | --- | --- | --- | --- |
| `icl5_b129_24` | 748 | 100 | 92 | 11 | MBAR 149, SOLO 117, MAC1 36 |
| `icl5_b132_24` | 818 | 205 | 126 | 15 | SOLO 217, NIUM 111, DARW 105, REYK 45 |
| `icl5_g129_24` | 877 | 121 | 93 | 11 | MBAR 136, SOLO 123, MAC1 55 |
| `icl5_g132_24` | 904 | 186 | 143 | 20 | SOLO 229, DARW 127, NIUM 115, REYK 79 |

**The key's run-end report** (whole run): flags that kept their flag-epoch gradient 1 410, 1 833, 1 589 and 1 955; had none to keep 0, 0, 4 and 2; post-flag evaluations 1 314, 1 747, 1 490 and 1 861; post-flag flags 427, 651, 467 and 678.

**Slip handovers:** 113 to 115, 144 to 146, 79 to 79, 115 to 115.

**The frame's vote at DOY 129 344430, with no quorum.** Control: Galileo 4 voters, 18 forced, step +0.4035 m. Arm: 22 voters, 0 forced, step -0.0001 m. Over the day on that arm the largest Galileo |G| goes 0.976 to 0.716 m and the step marks above 1.5 m go 1 to 0. The one re-anchor stays on GPS in hour 24, marked (23:25:00, |G| 1.003 m, to 23:28:30, 1.000 m). The anchor's phase-statistic ladder at |w| >= 6 goes 31 to 14 on DOY 129 and 62 to 41 on DOY 132, and 9 to 8 and 54 to 56 on the golden.

### 4.6 What the key leaves

The arms keep these with identical decoupling sets, so they belong to other mechanisms: DOY 129 304110 on the broadcast posture (3 decouplings, s0 phase 2.24, then 1.41); on the broadcast posture at DOY 132, 524970 (7), 524700 (6) and 527730 (5); on the golden, 276150 (31) and 271230 (24) at DOY 129, 529950 (31), 524700 (30), 524970 (8) and 527730 (7) at DOY 132. The end-of-day ramp of DOY 132 is unchanged: 8 epochs from 604560 on the broadcast posture and 9 from 604530 on the golden, at most 4.26 and 5.36 in s0 phase, equal between the runs within 0.03.

## 5. Verdict

**Two clauses fail; the rest pass.** R3's decouplings fail on the golden at DOY 132 by 2 lone Galileo trips, which the receivers do not register and whose cause is not traced. R5's hour clause fails on the broadcast posture at DOY 132, in hour 5 by a median falling across a gap and in hour 14 by 0.2 mm, while that arm's day-level clauses pass with an improvement.

**What the key does over a day.** It removes the post-flag events it was built for on both postures, each by the frozen bars of R1, and leaves no unanswered step of 3 m or more. One flag before the solve stands in for several arcs T4 dropped after it: T4 arcs halve, 94 to 97 % of those gone fall at an epoch with an extra flag, and the sum of refloats falls 2.4 to 6.0 % while the flags rise 22 to 32 %. The cost is a new shape: an arc on a noisy receiver can now refloat at every epoch, for up to 10 minutes.

**What it means for each posture.** On the golden, receivers do not move: no site by more than 1.0 mm over the day and no hour by more than 0.6 mm, WTZZ00DEU unchanged. No measured receiver gain and no measured receiver cost; the gain is in the producer's events and arcs. On the broadcast posture the gain is the level: card ICL2's D2 failure on DOY 129 passes with the key alone (phase-visible D 58.3 to 15.7 mm, offset rms 9.49 to 1.94 mm), where card ICL3's quorum reached 15.8 and 2.10 mm; the key removes the event at its source. Receivers there stay flat over the day (+0.2 and -1.5 mm).

Nothing is armed. Arming, any golden change and the merge are the author's decisions, and a decision to arm would set aside the two failed clauses above with this section's reading of them.

## 6. Limits and what remains

- **Two days, one network.** DOY 129 and 132 only. The sim bed holds no IF-TD flag in its 2 h, so it says nothing about the key.
- **The larger anchor statistic.** Why three satellites read 2 to several times more on the guard's first pass with the same flags and rows within 1 mm. The trace lacks the signal choice per band and the first pass's solved values.
- **Hour 5.** No arm separates the removed event at 529770 from the key's other changes. Section 4.4's reading of it is not proven.
- **Hours 2 to 6 at DOY 129 on the broadcast posture.** WTZZ00DEU +2 to +9 mm with no event; cause not traced.
- **Runs of flags.** Whether an arc refloated at every epoch for up to 10 minutes costs anything the census does not read.
- **The key with the quorum.** Not run together here; card ICL3's guard still covers an anchor decoupling from any other cause.
- **From card ICL4 section 6, still open:** the factor of 2 at q 5e-6; MW at a locked arc; the within-epoch tier and one arc; the epoch 304110 of DOY 129 and the last minutes of DOY 132. Card ARL's unit-test debt for the key's run-end report falls due with arming.
