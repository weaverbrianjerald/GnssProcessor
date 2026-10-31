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
