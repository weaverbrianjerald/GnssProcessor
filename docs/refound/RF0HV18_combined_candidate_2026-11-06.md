# RF0HV18: the end-of-week combined golden candidate

**STATUS**

- **id:** RF0HV18
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered 2026-11-06, before any 6 h run; graded 2026-11-07, C fails four clauses (section 4.3). Question: can the built-off keys of the week be armed together as the next golden baseline `_rf0hv18`, and what does each add to or cost the combination? Candidate C is the field golden plus the three signal decisions of card N3c (D1 `network.n3c_bia_pin_pair`, D6 `network.n3c_wire_per_signal`, D7 `network.n3c_ledger_lifecycle` with `network.roster_tracker` and `network.ledger_reentry`) and the refloat maturity gate (`network.raim_refloat_maturity_gate`, 120 epochs). One Release exe runs every arm: control, C, four leave-one-out arms and three add-one arms (pivot election, IF-TD post-flag re-seed, full-equation seeds) on DOY 129, control, C and C plus re-seed on DOY 132, control and C on the sim bed. Nothing is armed by this card.
- **source:** `scripts/nom3_designs/N3b_decisions.md` sections 9 and 10 (D1 to D7 and their 6 h grades); cards [NOM5](NOM5_station_parameter_set_tail_2026-10-30.md) (the recommendation bar, G3), card NSP (census noise floor, quoted in CLAUDE.md), [PIV](PIV_pivot_election_coverage_2026-10-17.md), [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) (re-seed key), card ICL5 (`docs/refound/ICL5_reseed_key_over_a_day_2026-11-06.md` on branch `icl5/reseed-key-day`, unmerged), [MDAY](MDAY_multiday_candidate_2026-10-19.md) (the DOY 132 setup), [BOF](BOF_built_off_follow_through_2026-10-28.md) (the baseline `_rf0hv17`)
- **knobs / posture:** see section 2. No golden, manifest or ledger row is edited.
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `golden/rf0hv18` from main `284ec29`, worktree `D:/GnssProcessorSupport/worktrees/rf0hv18`; run root `D:/GnssProcessorSupport/runs_rf0hv18` (`RAPID_RUNS_ROOT`); scratch, arm scripts and graders `D:/GnssProcessorSupport/gates/rf0hv18/`; exe and sha256 recorded in section 4
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: can the built-off keys of the week be armed together as `_rf0hv18`? On one Release exe the control reproduces baseline `_rf0hv17` byte for byte (R0), every arm exits 0 with 720 of 720 epochs (R1), and candidate C fails four clauses: the DOY 129 census median (+0.2 mm against +0.1 mm), DOY 132 DAV1 (+27.5 mm, caused by the D7 keys), and on the sim the consumer hour 2 (+1.75 mm, first-epoch bins) and census site s05 (+12.7 mm), both caused by the maturity gate. D6 is clean, D1 is mixed, and the three add-ons (pivot election, re-seed, full-equation seeds) pass against C where run. Nothing armed; the decision is the author's.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

The cards of the week left several default-off keys graded neutral or better on their own, each against the golden. Arming them one at a time re-bases the goldens once per key. The author's directive is that built work is activated rather than left in open cards. This card grades the combination on one exe, so that one re-base can replace several, and grades each member by removing it from the combination. The add-on keys (pivot election, post-flag re-seed, full-equation seeds) are graded on top of C. The decision to arm anything is the author's.

## 2. Arms

All arms are copies of a golden with only the listed keys changed (`scripts/rapid_arm.py make`, tags `rf18_<bed>_<arm>`), 6 h, one Release exe from the branch build. Keys were checked against the loader (`src/core/config/GnssConfigLoadNetwork.cpp`) and the cards:

- D1 `network.n3c_bia_pin_pair`: pins the bias product's clock-reference pair (GPS C1W with C2W). No companion.
- D6 `network.n3c_wire_per_signal`: one wire bias per modelled signal. Armed in the BeiDou golden; not in the GPS + Galileo goldens.
- D7 `network.n3c_ledger_lifecycle`: arms `network.ledger_screens` and `network.ledger_anchor` itself (loader). Its third site, gap re-entry, is `network.ledger_reentry`, which needs `network.roster_tracker` and `network.cycle_slip.gap_reentry_relevel` (already true in the field golden). As graded (`N3b_decisions.md` section 9, "the arm sets them"), D7 is the three keys `n3c_ledger_lifecycle`, `roster_tracker`, `ledger_reentry`.
- Gate: `network.raim_refloat_maturity_gate` true with `network.raim_refloat_maturity_epochs` 120. The ledger row states it was never A/B tested.

| arm | field DOY 129 (bed f) | field DOY 132 (bed g) | sim (bed s) |
| --- | --- | --- | --- |
| ctrl | golden unchanged | day golden unchanged | golden unchanged |
| c | D1 + D6 + D7 + gate | same | same |
| noD1, noD6, noD7, nogate | c minus that member | | |
| cpiv | c + `network.pivot_election` `coverage` | | |
| crs | c + `network.cycle_slip.if_phase_td_reseed_on_flag` | same | |
| csfe | c + `estimation.seed_full_equation` | | |

The DOY 132 golden is derived by `scripts/mday_golden_config.py` (observations from `D:/GnssProcessorSupport/data_mday`, 56 stations, as card MDAY). The sim bed starts at the fixture's start (the golden carries no start offset; the baseline `_rf0hv17_sim6` ran the same way) and is graded from its first epoch (00:30:15).

**Code check of the add-on keys against main.** The re-seed key was graded on branches `icl4/post-flag-slip` and `icl5/reseed-key-day`. The diff of that branch against main (`git diff main...icl5/reseed-key-day`) adds default-off code (frame hold, quorum, apriori clock track, two-way centring sink, tallies) and leaves the key's own path (`SlipQc.cpp` re-seed lines, `IfPhaseSlipDetector`, loader) as on main. Main therefore carries the graded behaviour of the key; the branch's other additions are not part of this card. Pivot election (card PIV) and `estimation.seed_full_equation` (card NOM5) are on main.

## 3. Gates (frozen 2026-11-06, before any 6 h run of this card)

Existing bars quoted: the recommendation bar of card NOM5 G3 (decision D6): "hours 1 to 6 not worse at the held-out consumers by more than 1 mm in any hour, the census median site not worse, and no census site worse by more than 1 cm (card NSP: a neutral change moves no site by more than 0.19 mm, so a site past 1 cm is a response)". Standing grading rules of CLAUDE.md: hour 1 is ignition and settled claims come from hours 2 to 6; a delta is against the same-exe control; never adopt on one instrument. This card applies the bar to hours 2 to 6 and reports hour 1; "census median not worse" is read at the tool's printed resolution, a paired median change of at most +0.1 mm. No bar moves after this section is committed. A failed clause stays failed.

Instruments: held-out consumer WTZZ00DEU, hourly p50 of `pos_err_3d_m` in `logs/closed_loop_trace.csv` (`rapid_arm.py score`); the 16-site census by `scripts/clock_level/gcensus.py` (GPS + Galileo consumers, each replay uses the arm's own `arm.json`), paired change of each site's mean hourly p50 over hours 2 to 6 against the control of the same exe; on the sim bed the held-out consumer SZZ000SIM by hour and the census of `simcensus6h.py` with `SIMCENSUS_BASE` set to the control's arm.

| gate | bar |
| --- | --- |
| R0, reproduction | The field DOY 129 control against the baseline `_rf0hv17_field6` (exe `gnss_processor_bofarm.exe`): each of the six WTZZ00DEU hourly p50 within 1 mm. Byte identity of `products.txt` is reported, not a bar (main moved since the baseline; the exe differs). Same for the sim control against `_rf0hv17_sim6`. |
| R1, health | Every arm exits 0 and emits as many epochs as its control; epochs withheld at most the control's. |
| R2, held-out consumer | Each of hours 2 to 6 of WTZZ00DEU (SZZ000SIM on the sim) at most +1 mm against the control (candidate and leave-one-out arms) or against C (add-one arms). |
| R3, census median | Paired median change over hours 2 to 6 at most +0.1 mm. |
| R4, census sites | No site worse by more than 1 cm over hours 2 to 6. Sites worse by more than 0.19 mm are listed (a move past the noise floor). |
| R5, ON path | Each key's run-end counters are read; 0 driven means inert wiring and the key's verdict is "inert", whatever the grade. Reported: IF-TD refloats, T4 arcs, decouplings, withheld epochs and slip handovers per arm. |

**Candidate verdict.** C passes when R1 to R4 pass on DOY 129 (against ctrl), on DOY 132 and on the sim. The candidate may become `_rf0hv18` only if C passes; the author decides.

**Key verdicts.** Leave-one-out, from `c` against `c minus X` on the field bed: X is *contributing* when C is better than `c minus X` by more than 1 mm in a settled WTZZ00DEU hour or by more than 0.19 mm at the census median or any site; *harmful* when C is worse by the same margins (then `c minus X` is the candidate to read); *neutral* when the products differ and no margin is exceeded; *inert* when the products are byte-identical or the key's counters read 0 driven. Add-one keys are passed or failed by R1 to R4 against C (DOY 129; the re-seed key also on DOY 132) and then read as for the leave-one-out arms.

Reported without a bar: WTZZ00DEU hour 1 and the mean of hours 2 to 6; the census by hour; the full per-site table; key counters.

Nothing is armed by this card. Arming, any golden, manifest or ledger change and the merge are the author's decisions.

---

## 4. Results (2026-11-07)

*Exe `build/x64-Release/gnss_processor_rf18.exe` of branch `golden/rf0hv18` (source of main `284ec29`, no source edit), sha256 `6e510cae8c234d6991c767329c1f7f70c4c6ef3a7852b7d956c8a63f68c2991f` (sha256[:16] `6e510cae8c234d69`), built by `scripts/wt_build.ps1` (Release). One exe for every arm and census replay. Run root `D:/GnssProcessorSupport/runs_rf0hv18` (`_rap_rf18_<bed>_<arm>`: bed f = field DOY 129, g = field DOY 132, s = sim). Graders, tables and logs `D:/GnssProcessorSupport/gates/rf0hv18/`: `hourly_all.txt`, `precise.py`, `counters_all.txt`, `grade_vs_ctrl.txt`, `grade_vs_c.txt`, `grade_g_vs_ctrl.txt`, `grade_g_vs_c.txt`, `simcensus.log`. Smokes (11 epochs, 14 arms) and the 2 h development check of C (`_rap_rf18_f_c_2h` against `_rap_rf18_f_ctrl_2h`: hour 2 +0.6 mm, every key driven) all exit 0. Wall time is not graded (the machine carried other sessions).*

### 4.1 R0, reproduction (PASS)

The field control `_rap_rf18_f_ctrl` against baseline `C:/GnssProcessor/runs/_rf0hv17_field6`: WTZZ00DEU 58.8 12.3 23.1 12.2 16.5 12.0 mm by hour in both, and `products.txt` byte-identical with the comment lines removed. The sim control `_rap_rf18_s_ctrl` against `_rf0hv17_sim6`: 136.8 72.3 30.8 23.9 25.7 18.2 mm, `products.txt` byte-identical. The control of main `284ec29` is the baseline `_rf0hv17`.

### 4.2 R1, health (PASS)

All 18 six-hour arms (15 planned, plus `rf18_s_nogate`, `rf18_g_noD7`, `rf18_g_noD1`) exit 0, emit 720 of 720 epochs and withhold 0 (observability gate and sigma0 ceiling); the rank gate emits every epoch.

### 4.3 Candidate C against its control, by bed

WTZZ00DEU (SZZ000SIM on the sim) hourly p50 change in mm, hours 1 to 6, from `precise.py`; census paired change of the site mean hourly p50 over hours 2 to 6 from `gcensus.py` (on the sim `simcensus6h.py`, run as the copy `simcensus_rf18.py` with its run root and 2 jobs set).

| bed | hour 1 (reported) | hours 2 to 6 | R2 (settled hours at most +1 mm) | census median (R3, at most +0.1 mm) | worst site (R4, below +10 mm) | sites worse than +0.19 mm |
| --- | --- | --- | --- | --- | --- | --- |
| field DOY 129 | -1.45 | +0.65 -2.70 -0.65 +0.95 +0.55 | PASS (+0.95) | **+0.2 mm: FAIL** | BAKE +4.9 mm: PASS (ARHT -215.1 mm) | 8 of 16 |
| field DOY 132 | -14.50 | -4.30 +0.50 +0.15 +0.10 +0.20 | PASS (+0.50) | -0.1 mm: PASS | **DAV1 +27.5 mm: FAIL** (HARB +7.5, ARHT -50.5, QUIN -10.6) | 6 of 16 |
| sim, first-epoch hours | +30.95 | +1.75 -1.35 -0.35 -0.10 +0.45 | **hour 2 +1.75 mm: FAIL** | -0.1 mm: PASS | **s05 +12.7 mm: FAIL** (s09 +3.5) | 5 of 16 |
| sim, bins from 00:30:15 (`simbins.py`) | -4.5 | +0.9 -2.8 -0.2 -0.8 +0.4 | PASS (+0.9) | not recomputed | the census bins are absolute hours | |

Section 2 of the pre-registration names the sim start "its first epoch (00:30:15)"; the first epoch is 00:00:30 and the baseline was graded in bins from it. Both bins are shown. The sim R2 fails on the first-epoch bins and passes on the 00:30:15 bins; R4 fails on the census either way.

**C fails.** DOY 129 R3 (+0.2 mm against +0.1 mm; the move sits at the card NSP noise floor of 0.19 mm, and the clause stands failed), DOY 132 R4 (DAV1 +27.5 mm), sim R2 on the first-epoch bins and sim R4 (s05 +12.7 mm). C does not become `_rf0hv18` as built. No bar moved.

### 4.4 Leave-one-out, DOY 129 field, against C (key verdicts)

Site columns are changes of the site mean hourly p50, hours 2 to 6, of `C minus X` against C, so a positive number means X helps that site. Sources: `hourly_all.txt`, `grade_vs_c.txt`.

| X | WTZZ00DEU, C minus X against C (mm), hour 1 then hours 2 to 6 | census median, sites over 0.19 mm | largest site moves (mm) | counters of C | verdict |
| --- | --- | --- | --- | --- | --- |
| D1 `n3c_bia_pin_pair` | +1.60; -0.95 +3.10 +0.45 -1.00 -0.45 | -0.1 mm, 7 | BAKE -5.7, CEDU +4.4, QUIN +3.9, SCRZ -2.8, STPM -2.8 | code-bias datum `cbpin=[G:C1W-C2W]` against `[-]` | mixed: contributes at WTZZ00DEU (hour 3 +3.1 mm), CEDU and QUIN; costs BAKE 5.7, SCRZ 2.8, STPM 2.8 mm; the source of C's census median +0.2 mm (without D1: -0.05 mm) |
| D6 `n3c_wire_per_signal` | 0.00 in every hour (wire only) | 0.0 mm, 2 | ARHT +105.1, QUIN +2.9, WUH2 -1.4 | wire records differ (`products.txt` differ) | contributing (ARHT00ATA -105 mm); no clause touched |
| D7 `n3c_ledger_lifecycle` + `roster_tracker` + `ledger_reentry` | -0.15; 0.00 -0.20 +0.40 -0.40 -0.20 | 0.0 mm, 5 | DAV1 +6.0, CHTI -1.7, BAKE +0.9 | 435 ledger re-entry arcs; lifecycle gate kept 1 676 column-epochs | contributing at DAV1 (-6.0 mm) on this day; harmful on DOY 132 (section 4.6) |
| gate `raim_refloat_maturity_gate`, 120 epochs | +0.25; -0.05 -0.30 0.00 +0.05 0.00 | 0.0 mm, 3 | DAV1 -0.5, BAKE +0.2 | 53 isolated-arc flags: 27 held, 26 passed; 27 `arc_held` events | neutral on the field (every move within 0.5 mm); harmful on the sim (section 4.5) |

### 4.5 The gate on the sim bed

`_rap_rf18_s_nogate` (C without the gate) has `products.txt` byte-identical to the sim control: D1, D6 and D7 are inert on the sim (D7's counters read 30 re-entry arcs and 0 gate keeps, and no product byte moves). The gate alone causes the sim failures. All 22 isolated-arc flags of the run fall inside the 120-epoch horizon, so 22 are held and 0 pass; the consumer error in hour 1 is 167.8 mm against 136.8 mm, hour 2 rises by 1.75 mm and census site s05 by 12.7 mm.

### 4.6 DOY 132: the failed site clause is D7

Two diagnostic arms were added after C's DOY 132 result, with no bar of their own: `_rap_rf18_g_noD7` and `_rap_rf18_g_noD1` (C minus that member, same day golden). Census against the control, hours 2 to 6 (`grade_g_vs_ctrl.txt`): DAV1 +27.5 mm in C, +26.1 mm without D1, +0.6 mm without D7; HARB +7.5, +8.1 and +0.2 mm. The D7 keys therefore move DAV1 by 26.9 mm and HARB by 7.3 mm on this day, against DAV1 -6.0 mm on DOY 129. D1 produces the hour-1 gain of WTZZ00DEU on this day (C -14.5 mm; without D1 -0.7 mm; hour 2 -4.3 and -0.4 mm). The response of DAV1 and HARB to the D7 wire is attributed by the leave-one-out arm and not traced to a mechanism.

### 4.7 Add-one arms against C

R1 to R4 against C; counters from each arm's own run end (`counters_all.txt`).

| arm (bed) | WTZZ00DEU against C (mm), hour 1; hours 2 to 6 | R2 | census median, worst site, sites over 0.19 mm | counters (control, C, arm) | clauses | verdict |
| --- | --- | --- | --- | --- | --- | --- |
| C + `pivot_election` coverage (DOY 129) | -2.05; -0.60 -0.15 +0.35 -0.40 -0.90 | PASS (+0.35) | +0.1 mm, CEDU +3.0, 5 | clock decouplings E 25 G 33, E 25 G 34, G 3 (E none); elections G22 and one for E; T4 arcs 55, 53, 53 | R1 to R4 PASS | neutral at the census, better in hour 1 at WTZZ00DEU; card PIV's DOY 130 failure (ARHT00ATA +40.6 mm) was not re-run |
| C + `if_phase_td_reseed_on_flag` (DOY 129) | +0.30; 0.00 -0.10 0.00 0.00 0.00 | PASS (0.00) | 0.0 mm, +0.2, 2 | IF-TD refloats 107, 107, 143; T4 arcs 55, 53, 30; 149 flags kept their gradient, 138 post-flag evaluations, 51 repeated | R1 to R4 PASS | neutral, driven |
| C + `if_phase_td_reseed_on_flag` (DOY 132) | +1.40; +0.95 -0.10 -0.05 0.00 -0.05 | PASS (+0.95) | 0.0 mm, +0.1, 0 | IF-TD refloats 165, 165, 201; T4 arcs 61, 58, 37; 212 flags kept | R1 to R4 PASS (hour 1 +1.4 mm reported) | neutral, driven; against its own control hour 1 is -13.1 mm, as for C |
| C + `estimation.seed_full_equation` (DOY 129) | 0.00 in every hour | PASS (0.00) | 0.0 mm, 0.0, 0 | producer `products.txt` byte-identical to C | R1 to R4 PASS | inert for the producer by design; consumers move by under 0.05 mm |

The branch check of section 2 holds: the re-seed key behaves on main as the branches that graded it report (T4 arcs and post-flag events fall, WTZZ00DEU and the census do not move). Card ICL5's 24 h clauses (decouplings, hour clause) were not repeated; these arms are 6 h.

### 4.8 Not done and not verified

- The add-one arms ran on DOY 129 only, except the re-seed key (DOY 132 also). Pivot election on DOY 130 (its failing day in card PIV) and DOY 132 was not run.
- No leave-one-out arms on the sim beyond the gate, and none on DOY 132 beyond D1 and D7.
- The DAV1 and HARB response to the D7 keys on DOY 132 has no traced mechanism.
- The sim census consumers are in-network stations; the field censuses are GPS + Galileo consumers on each arm's own configuration.
- Wall time was not graded. Nothing is armed; no golden, manifest or ledger row changed.

### 4.9 Reading for the arming decision

Findings, not recommendations. D6 is clean (ARHT00ATA -105 mm, WTZZ00DEU unmoved, inert on the sim) and touches no clause on its own as far as these arms show. D1 helps WTZZ00DEU by 3 mm at hour 3 of DOY 129 and by 14 mm in hour 1 of DOY 132, but costs BAKE 5.7 mm and carries the DOY 129 census median past its bar. D7 helps one site on DOY 129 and costs 27 mm at one site on DOY 132, which fails the 1 cm bar. The gate is neutral on the field and costs the sim 31 mm in hour 1 and 12.7 mm at one site. The three add-ons pass R1 to R4 against C where they were run. A re-graded combination without D7 and the gate (D1 + D6, with or without the add-ons) was not run here.
