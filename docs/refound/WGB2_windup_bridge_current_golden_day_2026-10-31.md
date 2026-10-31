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
