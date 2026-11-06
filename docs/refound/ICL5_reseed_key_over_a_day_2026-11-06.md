# ICL5: the IF-TD post-flag re-seed over a day, on both postures

**STATUS**

- **id:** ICL5
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-06, asked for by the author after card ICL4: a 24 h grade of `network.cycle_slip.if_phase_td_reseed_on_flag` on DOY 129 and 132, on the golden and on the broadcast posture, with the 16-site census, against card ICL2's 24 h controls. Card ICL4 showed the key removes the largest natural events on short arms and did not grade receivers.
- **source:** card [ICL4](ICL4_post_flag_slip_scatter_2026-11-06.md) sections 4 to 6 (the cause and the owed item); card [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) sections 13 and 14 (the key, its 6 h field arm); card [ICL2](ICL2_frame_day_one_level_2026-11-03.md) (the controls and D2's bars); card [ICL3](ICL3_frame_quorum_2026-11-05.md) (the exe's off path)
- **knobs / posture:** the key under grade, unchanged in every default: `network.cycle_slip.if_phase_td_reseed_on_flag`. Arms are copies of `icl2_p{129,132}_24` (card BRD's GPS + Galileo broadcast posture with card ICL's frame) and `icl2_c{129,132}_24` (the GPS + Galileo golden), one key changed.
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `icl5/reseed-key-day` from `icl4/post-flag-slip` (`5771660`), worktree `D:/GnssProcessorSupport/worktrees/icl2`; exe `gnss_processor_icl3q1.exe` (sha256[:16] `92743f1fb3c747fb`), census `gnss_processor_icl2c.exe`; arms `D:/GnssProcessorSupport/runs_nom/_rap_icl5_{b,g}{129,132}_24` and `_rap_icl5_s_2`; census keys `i5b{129,132}`, `i5g{129,132}` against `i2p{129,132}`, `i2c{129,132}`; scripts `D:/GnssProcessorSupport/worktrees/icl3_tools/icl5_*` (outside the repository)
- **gates (frozen at pre-registration):** section 3

**Outcome.** Open. Gates frozen before any run; results in section 4.

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
