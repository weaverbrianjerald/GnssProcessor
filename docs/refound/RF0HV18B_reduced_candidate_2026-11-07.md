# RF0HV18B: the reduced golden candidate

**STATUS**

- **id:** RF0HV18B
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-07, before any 6 h run. Continues card RF0HV18, where D1 + D6 + D7 + the refloat maturity gate failed four clauses. Question: do D1 and D6 together (c2), or D6 alone (d6), pass the frozen gates of that card on four beds (field DOY 129, 130 and 132, and the sim), and do the post-flag re-seed (RS) and the coverage pivot election (PIV) pass on top of c2? One Release exe (the exe of card RF0HV18), controls and their censuses reused. Nothing is armed by this card.
- **source:** [RF0HV18](RF0HV18_combined_candidate_2026-11-06.md) (sections 2 to 4: arms, gates, results), `scripts/nom3_designs/N3b_decisions.md` sections 9 and 10, cards [NOM5](NOM5_station_parameter_set_tail_2026-10-30.md), [PIV](PIV_pivot_election_coverage_2026-10-17.md), card ARHT (`docs/refound/ARHT_consumer_mechanism_2026-11-06.md` on main, not on this branch), [MDAY](MDAY_multiday_candidate_2026-10-19.md)
- **knobs / posture:** see section 2. No golden, manifest or ledger row is edited.
- **dates in the text:** 2026-11-07
- **hashes / run dirs:** branch `golden/rf0hv18` (tip `1936252` at the start), worktree `D:/GnssProcessorSupport/worktrees/rf0hv18`; exe `build/x64-Release/gnss_processor_rf18.exe`, sha256[:16] `6e510cae8c234d69`; run root `D:/GnssProcessorSupport/runs_rf0hv18` (`RAPID_RUNS_ROOT`); scratch, arm scripts and graders `D:/GnssProcessorSupport/gates/rf0hv18/reduced/`
- **gates (frozen at pre-registration):** section 3

**Outcome.** Pre-registered; no result yet. The reduced candidate drops the D7 keys (ledger lifecycle, roster tracker, gap re-entry) and the refloat maturity gate, which caused the DOY 132 DAV1 failure and the sim failures of card RF0HV18, and keeps D1 (`n3c_bia_pin_pair`) and D6 (`n3c_wire_per_signal`). Candidate arms c2 (D1 + D6) and the fallback d6 are graded on DOY 129, 130, 132 and the sim; the re-seed and the pivot election are graded on top of c2. The decision to arm anything is the author's.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

Card RF0HV18 graded D1, D6, D7 and the refloat maturity gate together and failed on D7 (DOY 132 DAV1 +27.5 mm) and on the gate (sim hour 2 and site s05), with D1 carrying the DOY 129 census median to +0.2 mm. D6 was clean. The author approved a reduced candidate without D7 and without the gate. This card grades it on the same exe, adds a third field day (DOY 130, the day on which card PIV's pivot election failed at ARHT00ATA), and grades the re-seed key and the pivot election on top of it.

## 2. Arms

All arms are copies of a golden with only the listed keys and the run paths changed (`scripts/rapid_arm.py make`, tags `rf18b_<bed>_<arm>`), 6 h, smoke of 10 epochs first, one Release exe, run root `D:/GnssProcessorSupport/runs_rf0hv18`. Each `arm.json` is compared against its control's `arm.json`: only the listed keys and the run paths may differ.

Definitions: D1 = `network.n3c_bia_pin_pair=true`. D6 = `network.n3c_wire_per_signal=true`. RS = `network.cycle_slip.if_phase_td_reseed_on_flag=true`. PIV = `network.pivot_election="coverage"`. c2 = D1 + D6. No D7 key and no maturity-gate key is set.

| arm | keys | DOY 129 (bed f) | DOY 132 (bed g) | DOY 130 (bed h) | sim (bed s) |
| --- | --- | --- | --- | --- | --- |
| ctrl | golden unchanged | reused `_rap_rf18_f_ctrl` | reused `_rap_rf18_g_ctrl` | new `rf18b_h_ctrl` | reused `_rap_rf18_s_ctrl` |
| c2 | D1 + D6 | yes | yes | yes | yes |
| d6 | D6 | yes | yes | no | no |
| c2rs | c2 + RS | yes | yes | no | yes |
| c2piv | c2 + PIV | yes | yes | yes | yes |

Day goldens: DOY 132 is `gates/rf0hv18/cfg/golden_field_doy132.json` (reused); DOY 130 is derived the same way, by `scripts/mday_golden_config.py <out> 130 --obs-root D:/GnssProcessorSupport/data_mday` from the field golden (58 stations, none dropped), stored as `gates/rf0hv18/cfg/golden_field_doy130.json`. Control runs and their censuses are reused from card RF0HV18 for beds f, g and s (same exe, hash verified before and after); the DOY 130 control and its census are new.

## 3. Gates (frozen 2026-11-07, before any 6 h run of this card)

The gates are those of card RF0HV18 section 3, restated for the reduced arms. No bar moves after this section is committed. A failed clause stays failed. A clause passed or failed by less than 0.05 mm of its bar is called marginal in the results and is not rounded away.

Instruments: held-out consumer WTZZ00DEU (SZZ000SIM on the sim), hourly p50 of `pos_err_3d_m` in `logs/closed_loop_trace.csv`; the 16-site census by `scripts/clock_level/gcensus.py` (`--day` and `--obs-root` for DOY 130 and 132), paired change of each site's mean hourly p50 over hours 2 to 6 against the reference; on the sim the census of `simcensus6h.py` (copy `simcensus_rf18.py`) with `SIMCENSUS_BASE` set to the control's arm.

| gate | bar |
| --- | --- |
| R1, health | Every arm exits 0, emits the same number of epochs as its control, and withholds no more epochs than its control. |
| R2, held-out consumer | Each of hours 2 to 6 of WTZZ00DEU (SZZ000SIM on the sim) at most +1 mm against the reference. |
| R3, census median | Paired median change over hours 2 to 6 at most +0.1 mm. |
| R4, census sites | No site worse than +10 mm over hours 2 to 6. Sites worse than +0.19 mm (the card NSP noise floor) are listed. |
| R5, ON path | Each key's run-end counters are read. 0 driven means the key is inert, whatever the grade. |

**References.** c2 and d6 are graded against the control of their day. c2rs and c2piv are graded against c2 and against the control, both reported, and a pass requires both.

**Sim hour bins.** The bar bins are the hours counted from 00:30:15 of the simulated day (tow 261015 of the fixture's week start 259200), the start the fixture's guidance gives for grading the sim (CLAUDE.md, "Sim start"): hour k is the half-open interval from 00:30:15 + (k - 1) h to 00:30:15 + k h, and hour 6 holds the last half hour of the 6 h run. The wording of card RF0HV18 ("its first epoch (00:30:15)") was wrong in fact: the first epoch of the unchanged golden is 00:00:30 (tow 259230), because the sim golden carries no start offset. Two bin sets are therefore also reported without a bar: bins from the trace's first epoch, as `rapid_arm.py score` computes them (first epoch 00:00:30), and wall-clock hours of the day (the native bins of `simcensus6h.py`, hour 1 from 00:00:00). The census on the sim is computed on the replay traces in each bin set; the bar applies to the 00:30:15 bins. Where the verdict differs between bin sets, the results say so plainly.

**Verdicts.**

- The candidate is c2 if c2 passes R1 to R4 on all of DOY 129, DOY 130, DOY 132 and the sim.
- d6 is the fallback under the same clauses.
- An add-on (RS, PIV) is recommended only if it passes R1 to R4 on every bed it ran (against c2 and against the control).
- Findings are stated as findings. Arming, any golden, manifest or ledger change and the merge are the author's decisions.

**Sim census shortcut.** A sim arm whose `products.txt` is byte-identical to its control's (comment lines removed) has an identical wire, hence an identical census; the census is then skipped and the identity stated.

**Reported without a bar.**

- WTZZ00DEU hour 1 and the mean of hours 2 to 6.
- At ARHT00ATA on each day (129, 130, 132): hourly p50 of ctrl, c2, d6 and c2piv, and whether the DOY 130 pivot-election failure of card PIV (+40.6 mm) persists under D6.
- At BAKE on DOY 129: whether the cost of D1 seen in card RF0HV18 (+4.9 mm in C, +5.7 mm in the leave-one-out arm) remains.
- Run-end counters per arm (IF-TD refloats, T4 arcs, decouplings, withheld epochs, re-seed and election counters).
- Wall time is not graded.

---
