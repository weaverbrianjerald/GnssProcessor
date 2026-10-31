# WGB2: the wind-up gap bridge on the current golden and over a day

**STATUS**

- **id:** WGB2
- **topic:** network-vehicle
- **status:** open
- **summary:** Continuation of [WGB](WGB_windup_gap_bridge_2026-10-17.md), which is at the size bar. Pre-registered 2026-10-31 (WGB section 13) and in progress: the bridge against same-exe controls on the field golden of card BOF, 6 h and 24 h of DOY 129, bars G1 to G3 of WGB section 6 unchanged, and an independent recount of the whole-cycle counts of every attempt.
- **source:** [WGB](WGB_windup_gap_bridge_2026-10-17.md) sections 6, 8, 9 and 12; [BOF](BOF_built_off_follow_through_2026-10-28.md)
- **knobs / posture:** `estimation.phase_windup_gap_bridge` (ledger row UNVALIDATED, default false), read, not changed; nothing is armed by this card
- **dates in the text:** 2026-10-31
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_wgbclose.exe` (a copy of `gnss_processor_bofarm.exe`, sha256[:16] `91143c81de9a9547`); run root `D:/GnssProcessorSupport/runs_nom`, arms `_rap_wgbc_{ctl,on}_f{6,24}`, smokes `_rap_wgbc_s_{ctl,on}`; scratch and reports `D:/GnssProcessorSupport/gates/wgbclose/`; checker `scripts/windup_gap_independent_check.py`

**Outcome.** Pending. The question, arms, bars and verdict rule are WGB section 13.

## 1. Amendment A1 to the pre-registration (written before the day pair was read)

The independent check was validated on two single-receiver runs of the project before the day pair finished (the gap run of WGB section 7, and a run with three holes placed over cut crossings, section 5). In the second, one gap (E26, TOW 266280) differs from the check by one cycle: the epoch's raw angle is +3.133 rad in the check and -3.140 rad in the project's trace, on either side of the +-pi cut, and the two bridged angles (raw angle plus 2 pi N) agree to 0.0102 rad, the usual model difference of the raw angle (median 9e-4 cycle, largest 1.0e-2 cycle against the project's trace, WGB section 13). The count N is defined against the epoch's raw angle, so it is representation-dependent within that difference.

Amendment. I1 counts a bridged attempt as agreeing when the counts are equal, or differ by the flip of the cut: the check's raw angle at the epoch lies within 0.07 rad (above the largest difference seen, 0.065 rad) of +pi and the project's count is the check's plus 1, or lies within it of -pi and the project's count is the check's minus 1. I2 compares the run-end count of nonzero bridged gaps with the interval the same allowance gives (an attempt near the cut may carry either count). Both readings are reported: literal equality, which is the pre-registration, and the amended one. The 6 h pair had been read (48 of 48 equal counts, no attempt within 0.07 rad of the cut), so the amendment does not touch it; the day pair had not.

## 2. Runs and identity of the twin

Exe `gnss_processor_wgbclose.exe` for every run (WGB section 13). Wall times are not timings: the box was shared with other sessions for the whole stage.

| arm | tag | length | exit | wall s | epochs emitted |
| --- | --- | --- | --- | --- | --- |
| C6 | `_rap_wgbc_ctl_f6` | 6 h | 0 | 3592.6 | 720 |
| W6 | `_rap_wgbc_on_f6` | 6 h | 0 | 3590.7 | 720 |

The twin C6 equals the baseline `runs/_rf0hv17_field6` (exe `gnss_processor_bofarm.exe`, no census key) byte for byte in `products.txt` (11,345,974 bytes, sha256[:16] `bf16e9474305e2de`) and in `logs/closed_loop_trace.csv` (57,047 bytes, `eb2eca92f04e9906`), so the census key changes nothing and C6 stands for the baseline (`gates/wgbclose/identity_c6_vs_baseline.txt`). Smokes of 10 epochs, `_rap_wgbc_s_{ctl,on}`: exit 0, products identical, the knob-on smoke prints the run-end bridge line.

## 3. G1, the on path

| | C6 | W6 |
| --- | --- | --- |
| forward transitions; in horizon | 600755; 600616 | the same |
| out-of-horizon transitions (between the horizons, beyond both) | 139 (61, 78) | 139 (61, 78) |
| accumulator restarts | 139 | 91 |
| transitions stepped by 0.9 cycle or more (max) | 2 (0.994) | 0 (0.832) |
| restart events answered; arcs refloated | 60; 116 | 12; 23 |
| bridge attempts (stations) | | 61 (33) |
| bridged / refused | | 48 / 13 (umbra 13; no state, noon turn, step, closure 0) |
| bridged over a nonzero count | | 2 |
| virtual epochs evaluated | | 1869 |

Source: `gates/wgbclose/g1_w6.txt`, read from the run-end lines of `_rap_wgbc_{ctl,on}_f6/stdout.txt`. The attempts (61) equal bridged plus refused and equal the gaps the census counts between the horizons in both arms (61); the restarts fall by the bridged count (139 to 91, minus 48); the answered events and the refloated arcs fall with them; every refusal is the umbra. **G1 holds for the 6 h pair.** The numbers equal those of WGB sections 9, 10 and 12 on the golden of 2026-10-17, count for count: the MW re-flag skip and card BOF move none of them.

## 4. G2 and G3

**G2, WTZZ00DEU 3D p50 by hour (m)**, `gates/wgbclose/g2_w6.txt` (`p50.py`: `p50_6h.txt`):

| hour | 1 | 2 | 3 | 4 | 5 | 6 | mean 2 to 6 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C6 | 0.0588 | 0.0123 | 0.0231 | 0.0123 | 0.0165 | 0.0120 | 0.01522 |
| W6 | 0.0588 | 0.0122 | 0.0232 | 0.0123 | 0.0165 | 0.0119 | 0.01522 |

The worst hour of 2 to 6 moves by +0.1 mm (bar +10 mm). **G2 holds for the 6 h pair.**

**G3, the 16-site census**, `gcensus.py make wgbcc_{ctl,on}6`, 17 replays each, all exit 0, graded by `gates/wgbclose/g3_report.py` (`g3_6h.txt`, `census_grade_6h.txt`): paired change of the mean hourly p50 over hours 2 to 6, median +0.00 mm, mean -0.01 mm, best -0.17 mm (DAV1), worst +0.09 mm (ARHT00ATA); 0 sites better or worse at 1 cm, 0 worse by more than 2 cm (bar: median at most +5 mm, at most 3 sites worse by 2 cm). Median site p50 by hour 0.142 0.050 0.036 0.029 0.032 0.030 m for C6 and 0.142 0.050 0.036 0.029 0.032 0.029 m for W6. **G3 holds for the 6 h pair.**
