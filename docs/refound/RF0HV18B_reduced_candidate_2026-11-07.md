# RF0HV18B: the reduced golden candidate

**STATUS**

- **id:** RF0HV18B
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered 2026-11-07, before any 6 h run; graded 2026-11-07, c2 fails three clauses (section 4). Continues card RF0HV18, where D1 + D6 + D7 + the refloat maturity gate failed four clauses. Question: do D1 and D6 together (c2), or D6 alone (d6), pass the frozen gates of that card on four beds (field DOY 129, 130 and 132, and the sim), and do the post-flag re-seed (RS) and the coverage pivot election (PIV) pass on top of c2? One Release exe (the exe of card RF0HV18), controls and their censuses reused. Nothing is armed by this card.
- **source:** [RF0HV18](RF0HV18_combined_candidate_2026-11-06.md) (sections 2 to 4: arms, gates, results), `scripts/nom3_designs/N3b_decisions.md` sections 9 and 10, cards [NOM5](NOM5_station_parameter_set_tail_2026-10-30.md), [PIV](PIV_pivot_election_coverage_2026-10-17.md), card ARHT (`docs/refound/ARHT_consumer_mechanism_2026-11-06.md` on main, not on this branch), [MDAY](MDAY_multiday_candidate_2026-10-19.md)
- **knobs / posture:** see section 2. No golden, manifest or ledger row is edited by the card's own grading. Section 5 (2026-11-07, appended) records the arming of ten keys under the author's rule of that date, with a health check.
- **dates in the text:** 2026-11-07
- **hashes / run dirs:** branch `golden/rf0hv18` (tip `1936252` at the start), worktree `D:/GnssProcessorSupport/worktrees/rf0hv18`; exe `build/x64-Release/gnss_processor_rf18.exe`, sha256[:16] `6e510cae8c234d69` (verified before and after); run root `D:/GnssProcessorSupport/runs_rf0hv18` (`RAPID_RUNS_ROOT`); scratch, arm scripts and graders `D:/GnssProcessorSupport/gates/rf0hv18/reduced/`
- **gates (frozen at pre-registration):** section 3

**Outcome.** On one Release exe (hash verified before and after) all 15 six-hour arms exit 0 with 720 of 720 epochs. Candidate c2 (D1 + D6) fails three clauses: census median +0.165 mm on DOY 129 and +0.170 mm on DOY 130, and WTZZ00DEU hour 2 +4.15 mm on DOY 130; it is byte-identical to the control on the sim. The fallback d6 passes every clause on DOY 129, 130 (supplementary arm) and 132 and was not run on the sim. The re-seed is neutral against c2 and inert on the sim; the pivot election fails on the sim and on DOY 132. Nothing armed; the decision is the author's.

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

## 4. Results (2026-11-07)

*Exe `build/x64-Release/gnss_processor_rf18.exe` of branch `golden/rf0hv18`, sha256[:16] `6e510cae8c234d69`, verified before the first arm and after the last census; unchanged. Run root `D:/GnssProcessorSupport/runs_rf0hv18` (`_rap_rf18b_<bed>_<arm>`; censuses `_rap_cfb_*`, `_rap_cgb_*`, `_rap_chb_*`, sim replays `_rap_smc6_<site>_scb_c2piv`). Graders, tables and logs `D:/GnssProcessorSupport/gates/rf0hv18/reduced/`: `grade_all.py`, `grade_all_out.txt`, `grade_s.txt`, `r2_all.txt`, `site_read.txt`, `counters_all.txt`, `make_arms.sh`, `armdiff.py`, `census_pipeline.sh`, `census_list.sh`, `sim_census_pipeline.sh`. Smokes (11 epochs, 14 arms) all exit 0. Wall time is not graded (the machine carried other sessions).*

### 4.1 Setup checks

- Every `arm.json` differs from its control's only by the listed keys and `network.product_output_path` (`armdiff.py`).
- R1 holds for all 15 six-hour arms: exit 0, 720 of 720 epochs, 0 withheld (gate and A4), rank gate 720 emitted, same as every control.
- One arm was added after the c2 result on DOY 130 and is not in the pre-registration: `rf18b_h_d6` (D6 alone on DOY 130). The fallback d6 cannot be judged on DOY 130 without it, and it is the only way to read the DOY 130 pivot question under D6 alone. It is graded under the same clauses and labelled supplementary.
- Census replays use each field arm's own `arm.json`; the sim census uses the control's `arm.json` as consumer base for every arm (as pre-registered). The census `cfb_c2` was interrupted once by a restart of the census queue (10 of 17 replays complete); its other 7 replays were re-run with the same exe and configuration and the set is complete.
- Controls and their censuses reused from card RF0HV18: `_rap_rf18_f_ctrl` / `cf_ctrl`, `_rap_rf18_g_ctrl` / `cg_ctrl`, `_rap_rf18_s_ctrl` / `sc_ctrl`. New: `rf18b_h_ctrl` and `chb_ctrl` (DOY 130).

### 4.2 Sim bins: what was graded

The first epoch of the unchanged sim control is 00:00:30 (tow 259230), not 00:30:15. The bar uses bins from 00:30:15 (section 3). Reported without a bar: bins from the first epoch (`rapid_arm.py score`) and wall-clock hours. Where the verdict differs between bin sets it is stated. For the arms that are byte-identical to the control all three bin sets read 0.000.

### 4.3 Candidate c2 and fallback d6, against the control of the day

Hourly change in mm of WTZZ00DEU (producer closed-loop consumer, `grade_all_out.txt`); census over hours 2 to 6 (`grade_all.py`, full resolution).

| bed | arm | hour 1 (reported) | hours 2 to 6 | R2 | census median (R3, bar +0.1) | worst site (R4, bar +10) | sites over +0.19 mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| DOY 129 | c2 | -1.40 | +0.70 -3.00 -0.25 +0.55 +0.35 | PASS | **+0.165 mm FAIL** | BAKE +5.70 PASS | 8 |
| DOY 129 | d6 | 0.00 | 0.00 in every hour | PASS | +0.000 PASS | WUH2 +1.91 PASS | 1 |
| DOY 132 | c2 | -11.30 | -3.25 +0.60 +0.30 +0.15 +0.25 | PASS | -0.070 PASS | STPM +1.73 PASS | 7 |
| DOY 132 | d6 | 0.00 | 0.00 in every hour | PASS | +0.000 PASS | 0.00 PASS | 0 |
| DOY 130 | c2 | -6.30 | **+4.15 +2.40 +1.85** +0.20 +1.35 | **FAIL** (hours 2, 3, 4, 6 over +1) | **+0.170 mm FAIL** | WUH2 +1.61 PASS | 8 |
| DOY 130 (supplementary) | d6 | 0.00 | 0.00 in every hour | PASS | +0.000 PASS | WUH2 +1.69 PASS | 1 |
| sim | c2 | 0.00 | 0.00 in every hour (all bin sets) | PASS | census skipped: `products.txt` byte-identical to the control | none | none |

**c2 fails.** DOY 129 R3 (+0.165 mm), DOY 130 R2 (+4.15 mm at hour 2; hours 3, 4 and 6 also over +1 mm) and DOY 130 R3 (+0.170 mm). It passes on DOY 132 and, by identity with the control, on the sim. No clause is marginal (R3 fails by 0.065 and 0.070 mm).

**d6 passes** R1 to R4 on DOY 129, 130 and 132 with no producer-side move of the consumer (the wire change moves only census sites: WUH2 +1.91 mm on DOY 129, +1.69 mm on DOY 130; the ARHT00ATA mean of hours 2 to 6 falls 214 mm on DOY 129, 174 mm on DOY 130 and 49 mm on DOY 132). d6 was not run on the sim bed (it was not an arm there). The sim c2 arm is byte-identical to the control, so D1 and D6 together change nothing on the sim; d6 alone on the sim is inferred inert and was not run, so the d6 verdict on the sim is not measured.

By the pre-registered rule the candidate is not c2. The fallback d6 passes where run (DOY 129, 130 supplementary, 132) and is unmeasured on the sim.

### 4.4 Add-ons (R1 to R4 against c2 and against the control; both must pass)

| bed | arm | against | hour 1 | hours 2 to 6 | R2 | census median | worst site | sites over +0.19 mm | verdict on the clause set |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| DOY 129 | c2rs | ctrl | -1.35 | +0.65 -2.95 -0.25 +0.60 +0.35 | PASS | **+0.190 FAIL** | BAKE +5.47 | 8 | FAIL (inherited from c2) |
| DOY 129 | c2rs | c2 | +0.05 | -0.05 +0.05 0.00 +0.05 0.00 | PASS | +0.000 | DAV1 +0.45 | 1 | PASS |
| DOY 132 | c2rs | ctrl | -11.45 | -3.05 +0.40 +0.25 +0.25 +0.25 | PASS | -0.120 | STPM +1.85 | 7 | PASS |
| DOY 132 | c2rs | c2 | -0.15 | +0.20 -0.20 -0.05 +0.10 0.00 | PASS | -0.030 | DAV1 +0.31 | 1 | PASS |
| sim | c2rs | ctrl, c2 | 0.00 | 0.00 | PASS | census skipped: byte-identical | | | PASS, but inert (R5) |
| DOY 129 | c2piv | ctrl | -3.95 | +0.35 -3.85 -0.55 **+1.00** +0.05 | PASS (marginal: hour 5 at the bar) | +0.020 | BAKE +6.16 | 7 | PASS |
| DOY 129 | c2piv | c2 | -2.55 | -0.35 -0.85 -0.30 +0.45 -0.30 | PASS | -0.110 | CEDU +2.43 | 5 | PASS |
| DOY 132 | c2piv | ctrl | -23.60 | -8.05 **+1.60** +1.05 +0.45 +0.30 | **FAIL** (hours 3 and 4) | -0.410 | STPM +4.70 | 6 | FAIL |
| DOY 132 | c2piv | c2 | -12.30 | -4.80 +1.00 +0.75 +0.30 +0.05 | PASS (marginal: hour 3 at the bar) | -0.030 | KOKB +3.91 | 6 | PASS |
| DOY 130 | c2piv | ctrl | -5.90 | **+4.15 +3.40 +2.85** +0.65 -1.30 | **FAIL** (hours 2 to 4) | -0.050 | WUH2 +3.07 | 7 | FAIL (inherited from c2) |
| DOY 130 | c2piv | c2 | +0.40 | 0.00 +1.00 +1.00 +0.45 -2.65 | PASS (marginal: hours 3 and 4 at the bar) | -0.055 | WUH2 +1.46 | 2 | PASS |
| sim, bar bins (00:30:15) | c2piv | ctrl = c2 | -22.70 | **+33.70 +26.75** -7.15 -6.75 +1.70 | **FAIL** | **+1.340 FAIL** | **S60 +19.37 FAIL** (S21 +10.97, S33 +10.75) | 10 | FAIL |
| sim, first-epoch bins (report) | c2piv | ctrl = c2 | +180.45 | -11.80 +32.85 +15.15 -10.30 -1.05 | FAIL | +5.760 FAIL | S21 +20.82 FAIL (6 sites over +10) | 10 | FAIL |
| sim, wall-clock hours (report) | c2piv | ctrl = c2 | +186.90 | -13.70 +32.95 +15.15 -10.15 -1.25 | FAIL | +5.970 FAIL | S21 +20.60 FAIL (6 sites over +10) | 10 | FAIL |

The verdict does not differ between the sim bin sets: c2piv fails R2, R3 and R4 in all three.

**c2rs.** Fails against the control on DOY 129 only because c2 does; against c2 every clause passes on DOY 129 and 132. On the sim it is byte-identical to the control: no IF-TD flag occurs (0 flags, re-seed counters 0 driven), so the key is inert on the sim bed. By the rule (pass on every bed it ran, against both references) c2rs is not recommended on top of c2. It was not run on top of d6.

**c2piv.** Fails against the control on DOY 130 (R2, inherited from c2), DOY 132 (R2, hour 3 +1.60 mm) and the sim (R2, R3, R4); against c2 it passes on the three field beds, with four clauses at the bar (hour 5 of DOY 129 against the control; hour 3 of DOY 132 and hours 3 and 4 of DOY 130 against c2: marginal passes). It fails on the sim against both references, which are the same wire. Not recommended by the rule.

### 4.5 R5, run-end counters (`counters_all.txt`)

| key | bed | reading |
| --- | --- | --- |
| D1 | DOY 129 | code-bias datum `cbpin=[G:C1W-C2W]` in c2 against `[G:C1C-C2W]` in the control and in d6: driven |
| D6 | all field beds | wire records differ from the control (products differ): driven; sim: c2 byte-identical to the control, inert |
| RS | DOY 129, 132 | IF-TD refloats 107 (control and c2), 143 (c2rs) on DOY 129 and 165, 165, 201 on DOY 132; 149 and 212 flags kept their gradient; T4 arc flags 55 to 33 and 61 to 36: driven. Sim: 0 flags, 0 driven: inert |
| PIV | all | `pivot elections: G22` on each field bed and `G20` on the sim; clock decouplings 25 33 to G 3 (E, G) on DOY 129, E 34 G 72 to E 13 G 42 on DOY 132, E 1 G 44 to G 13 on DOY 130, E 69 G 94 to E 39 G 91 on the sim; T4 arc flags 14 to 20 on the sim: driven |

All arms: 720 epochs emitted, 0 withheld by the observability gate or A4, 0 T2 refloats, 0 station exclusions.

### 4.6 Reported without a bar

**ARHT00ATA** (hourly p50 of the census replay in mm, hours 1 to 6, mean of hours 2 to 6 in brackets; `site_read.txt`):

| day | ctrl | c2 | d6 | c2piv |
| --- | --- | --- | --- | --- |
| 129 | 300.6 374.3 209.7 194.0 128.6 245.8 (230.5) | 68.0 21.9 19.0 16.2 10.9 9.4 (15.5) | 50.5 21.3 22.3 18.9 12.6 9.0 (16.8) | 68.8 22.2 19.0 16.2 18.3 10.9 (17.3) |
| 130 | 407.0 297.8 181.4 177.7 135.7 219.6 (202.4) | 268.3 33.4 50.1 49.4 31.6 20.3 (37.0) | 97.2 16.6 30.1 33.6 39.1 23.8 (28.7), supplementary | 265.0 33.2 50.0 49.4 21.9 15.6 (34.0) |
| 132 | 612.7 164.8 173.7 45.5 47.5 50.1 (96.4) | 54.1 17.8 33.0 45.6 71.5 60.0 (45.6) | 55.1 18.1 33.2 42.8 74.1 66.8 (47.0) | 54.3 17.8 32.3 48.2 61.2 52.9 (42.5) |

The DOY 130 pivot-election failure of card PIV (ARHT00ATA +40.6 mm against the control) does not appear: on DOY 130 c2piv reads 34.0 mm against 202.4 mm for the control and against 37.0 mm for c2 (-3.0 mm). The election on top of D6 alone was not run, so the persistence question is answered for D1 + D6 (c2piv) and not for D6 without D1.

**BAKE, DOY 129** (census replay, mm, hours 1 to 6, mean of hours 2 to 6): ctrl 106.1 25.2 22.2 18.3 11.4 7.7 (17.0); c2 107.2 39.7 30.1 22.6 11.8 9.1 (22.7); d6 identical to ctrl (17.0); c2piv 101.0 42.6 31.6 22.4 10.8 8.3 (23.1); c2rs (22.4). D1's cost remains in c2 (+5.70 mm over hours 2 to 6) and in both add-on arms, and is absent in d6.

**Hour 1 of WTZZ00DEU** against the control: c2 -1.40 (DOY 129), -6.30 (130), -11.30 (132); d6 0.00; c2piv -3.95, -5.90, -23.60 and, on the sim, -22.70 in the bar bins.

### 4.7 Not done and not verified

- d6 on the sim was not run (not an arm there); its sim verdict is unmeasured.
- The election on top of D6 alone (the exact combination of card PIV's DOY 130 failure) was not run.
- `rf18b_h_d6` is supplementary and was added after the c2 result; its clauses were frozen with the others but the arm is not in the pre-registered table.
- Census replays are in-network consumers on the sim and GPS + Galileo consumers on the field, one realization each; no multi-day or 24 h grade; no mechanism traced for the DOY 130 R2 failure of c2 (WTZZ00DEU hours 2 to 4 move +4.15, +2.40, +1.85 mm; the counters of c2 and the control differ in one T4 arc flag only).
- Wall time was not graded. Nothing is armed; no golden, manifest or ledger row changed.

### 4.8 Reading for the arming decision

Findings, not recommendations. c2 does not pass the frozen gates: D1 carries the DOY 129 and DOY 130 census medians to +0.165 and +0.170 mm (bar +0.1) and moves WTZZ00DEU on DOY 130 by up to +4.15 mm, while it helps WTZZ00DEU in hour 1 of every field day and in hour 2 of DOY 132. D6 alone passes every clause on the three field days; it lowers the ARHT00ATA mean of hours 2 to 6 by 49 to 214 mm and costs WUH2 1.7 to 1.9 mm. The re-seed key is neutral against c2 and inert on the sim. The coverage pivot election fails on the sim bed (SZZ000SIM hours 2 and 3 +33.7 and +26.8 mm, three census sites over +10 mm) and on DOY 132 against the control. No add-on passes on every bed it ran against both references.


---

## 5. Arming under the 2026-11-07 rule (2026-11-07, appended)

Rule of 2026-11-07: finished keys are armed unless measured harmful; a key whose evidence has an open item carries a "Look here:" note in its ledger row. The health check below is not an adoption gate. Branch `int/2026-11-07`, worktree `D:/GnssProcessorSupport/worktrees/int1107`; golden/rf0hv18 merged as `5715e6c`, arming committed as `47b7563`.

### 5.1 Armed and kept off

Armed (manifest pins on the field and sim goldens, hence also on the BeiDou lineage, which extends the field golden; ledger rows ARMED, except the first):

- `network.n3c_wire_per_signal` (D6). The row stays RECOMMEND_SCOPED under the regime `has_bds`; the pin arms it outside that regime. The regime and the C++ posture-table row are unchanged (a change needs a rebuild). Look here: sim not run alone (inert in candidate C).
- `network.parallel_carry_harvest`, `network.parallel_iono_traces`, `network.parallel_coupling_prepare` (card PES, identical output).
- `network.cycle_slip.if_phase_td_reseed_on_flag`. Look here: 24 h grade failed two clauses (card ICL6); neutral in this card.
- `network.raim_prior_channel`. Look here: refloats +29 % (mostly BSHM00ISR G22); hold and multi-day not graded.
- `network.cycle_slip.if_phase_td_sat_clock_event` and `network.clock_anchor_defer_coupled`. Look here: consumer legs: sat_clk passes on g, misses c by 4 mm, n consumer 15 cm off.
- `estimation.seed_full_equation`. Fingerprinted (fresh convergence only; the golden runs are fresh). Look here: producer-inert on the goldens; acts on new-state seeds only.
- `network.signal_integrity.census`. Look here: armed under the 2026-11-07 rule without its own golden A/B.

Set-level note (not in a ledger row; the arm does not isolate a key): Look here: sim ignition hour of the 2026-11-07 armed set is 50 to 90 mm worse than the control (settled hours unchanged); key not isolated (D6 and the re-seed key were byte-inert on the sim in RF0HV18B).

Kept off (ledger KEEP_OFF, reason appended): `network.n3c_bia_pin_pair` (D1), `network.n3c_ledger_lifecycle` (D7), `network.raim_refloat_maturity_gate`, `network.pivot_election`.

No key was stopped. Each of the three regenerated goldens differs from its 3484ca4 version by these ten keys and the provenance header only.

### 5.2 Method

Exe `D:/GnssProcessorSupport/gates/int1107/x64-Release/gnss_processor_int1107.exe`, sha256[:16] `f333466f62cbc284` (checked before the runs). Release only; 10-epoch smoke on all ten arms (exit 0) before the 6 h runs. Run root `D:/GnssProcessorSupport/runs_int1107` (`_rap_i7_<bed>_<ctrl|arm>`, beds f = DOY 129, h = DOY 130, g = DOY 132, s = sim, b = BeiDou DOY 129). Control = the 3484ca4 goldens (day goldens derived with a copy of `scripts/mday_golden_config.py` pointed at the worktree golden, obs root `D:/GnssProcessorSupport/data_mday`; the control DOY 132 config equals card RF0HV18's), armed = the regenerated goldens. Each `arm.json` pair differs only by the listed keys and the run paths. Censuses: `scripts/clock_level/gcensus.py` (`ci7_<bed>_<ctrl|arm>_<site>`), sim census with the control's `arm.json` as consumer base (`si7_<ctrl|arm>`). Graders, configs and logs: `D:/GnssProcessorSupport/gates/int1107/arming/` (`grade_all.py`, `grade_field.txt`, `grade_sim.txt`, `hourly_all.txt`, `h4.py`). Field and BeiDou hours are counted from the first epoch; the sim hours from 00:30:15 (tow 261015).

### 5.3 Health criteria

| bed | H1 | H2 (settled hours 2 to 6, worst vs control; bar +10 mm) | H3 (census, worst site; bar +10 mm) | census median |
| --- | --- | --- | --- | --- |
| DOY 129 | PASS: 720 of 720, 0 withheld, exit 0, same as control | PASS +0.65 mm | PASS WUH2 +0.76 mm (6 sites over +0.19 mm) | -0.025 mm |
| DOY 130 | PASS (same) | PASS +0.10 mm | PASS WUH2 +1.68 mm | -0.010 mm |
| DOY 132 | PASS (same) | PASS +0.65 mm | PASS DAV1 +7.07 mm (HARB +5.20 mm) | -0.040 mm |
| sim (SZZ000SIM) | PASS (same) | PASS +2.80 mm (from 00:30:15) | PASS S53 +3.78 mm (16-site sim census) | -0.180 mm |
| BeiDou DOY 129 | PASS (same) | PASS +1.30 mm | not run (census was asked per field day only) | not run |

All ten runs exit 0. Reported without a bar: sim ignition (hour 1 from 00:30:15) +75.3 mm; with first-epoch bins hours 1 and 2 read +59.8 and +36.3 mm; the coordinator's half-hour bins read +88.9 then +50.1 mm with settled bins -13.6 to +3.3 mm. The sim census median reads +0.185 and +0.170 mm in first-epoch and wall-clock bins (those bin sets include ignition; the bar bins read -0.180 mm).

WTZZ00DEU hourly p50 of the closed-loop consumer, mm, hours 1 to 6 (control / armed):

| bed | control | armed |
| --- | --- | --- |
| DOY 129 | 58.8 12.3 23.1 12.2 16.5 12.0 | 58.6 11.9 23.7 12.5 16.6 12.2 |
| DOY 130 | 88.4 16.5 24.1 34.3 55.1 51.6 | 88.2 16.2 24.0 34.4 55.2 51.6 |
| DOY 132 | 72.1 18.8 14.5 17.3 19.0 8.5 | 72.2 15.5 15.0 17.8 19.6 8.8 |
| sim SZZ000SIM, bins from 00:30:15 | 100.4 27.4 37.4 21.5 24.7 16.1 | 175.7 30.2 24.9 22.5 26.5 15.9 |
| BeiDou DOY 129 | 34.2 13.4 19.4 19.0 19.0 15.9 | 34.4 14.2 20.5 19.8 20.2 15.9 |

### 5.4 H4, run-end counters of the armed keys (armed arm, `h4.py`)

| key | reading |
| --- | --- |
| PES carry harvest, iono traces, coupling preparation | driven on every bed: harvest 1.14 to 1.85 million columns, traces 40.3 to 43.2 thousand station-epochs, 719 coupling inverses split, 0 declined |
| `n3c_wire_per_signal` | driven on DOY 129, 130, 132 (5128 to 5204 wire records added); inert on the sim (0 added); already pinned in the BeiDou control, so the BeiDou pair does not exercise it |
| `if_phase_td_reseed_on_flag` | driven on every field and BeiDou bed (149, 384, 212 and 264 flags kept their gradient on DOY 129, 130, 132 and BeiDou); 0 on the sim (no IF-TD flag), inert there |
| `raim_prior_channel` | driven on every bed (flags 45, 18, 43, 12 sim, 74 BeiDou) |
| `if_phase_td_sat_clock_event` | 0 satellite clock events on every bed: inert |
| `clock_anchor_defer_coupled` | driven (trips deferred to the next epoch: 3, 13, 52, 85 sim, 2 BeiDou) |
| `signal_integrity.census` | driven (flicker-regain refloats 76, 96, 79, 1412 on BeiDou, 0 on the sim); the log carries warnings of the classifier's re-entry premise (for example 25 re-entries with a surviving ambiguity state on DOY 129) |
| `seed_full_equation` | no run-end counter exists (a producer's births are unchanged by construction); not observable here |

Whole-run T4 arc flags on DOY 129 fall from 55 to 3 and IF-TD refloats rise 107 to 143; on the sim T4 arc flags go 14 to 1 and clock decouplings (E, G) 69 94 to 61 79.

### 5.5 Reading

H1 to H3 pass on every bed run. The largest settled-hour move of the held-out consumer is +2.80 mm (sim hour 3 is -12.5 mm); the largest census site move is DAV1 +7.07 mm on DOY 132, below the bar and above the +0.19 mm noise floor, with HARB +5.20 mm on the same day. The sim ignition hour is worse by 50 to 90 mm, which the settled hours do not carry; the responsible key is not isolated. `if_phase_td_sat_clock_event` is armed and inert on all five beds. The BeiDou bed has no census.
