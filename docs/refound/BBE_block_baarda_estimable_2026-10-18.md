# BBE: the block Baarda test on the float vehicle, and its estimable form

**STATUS**

- **id:** BBE
- **topic:** raim-qc
- **status:** open
- **summary:** BUILT 2026-10-18, default off, never armed: `network.block_baarda_estimable` tests the part of each (station, satellite) line-of-sight block that the data can test. The whole-block test refused every block on the float vehicle (field 60279 of 60302, sim all) because the line of sight's own slant ionosphere is a free column that absorbs one direction of the block exactly. Knob off is identical to a main control on the 10-epoch smokes (field and sim). Checked 2026-10-18 on both beds with the same-build gates (section 8): knob off identical, knob on moves nothing but the census outputs, no flagged block is block-only.
- **source:** `docs/status/OPEN_ITEMS.md` (block_baarda is blind on the float vehicle); [Rider B](../cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) section 12.3 and deviation 3 (a pseudo-inverse was refused for its unknown degrees of freedom); [REDUNDANCY-CENSUS](../cards/raim-qc/REDUNDANCY-CENSUS.md)
- **knobs / posture:** `network.block_baarda_estimable` (ledger UNVALIDATED), `network.block_baarda_estimable_eps` (PARAM, 0.05); both goldens keep `block_baarda` armed and do not set either key, so both are unchanged. Measure-only: nothing in the solve reads the census.
- **dates in the text:** 2026-10-18
- **hashes / run dirs:** branch `fix/baarda` from main `30ca49e`; control exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_baarda_ctl.exe` (main), exe under test `gnss_processor_baarda.exe` (same directory); arms `D:/GnssProcessorSupport/runs_nom/_rap_baarda_{f,s}10_{ctl,off,on}` (10 epochs) and `_rap_baarda_{f,s}1h_est` (1 h); reference run `runs/_rf0hv15_field6`; dumps and comparisons `D:/GnssProcessorSupport/gates/baarda/`; reader `scripts/block_baarda_join.py`

**Outcome.** Question: why does the armed block test find every line-of-sight block singular on the float vehicle, and can it be made to work? The slant ionosphere of a line of sight is a local column that only that line of sight's rows touch and nothing constrains, so it absorbs one direction of the block exactly: in 8000 of 8000 sampled warm blocks the smallest redundancy number of the whitened block cofactor is below 1e-11 and its eigenvector lies in the span of that column. The estimable form keeps the other directions (3 of 4, 4 of 5) with their count as the degrees of freedom. Sim and field 1 h arms: 0 and 16 of 11562 and 10741 blocks refused. The 2 h grade (section 8): 4 of 9984 field blocks refused in hour 2, 7.1 % flagged, none block-only; sim 0 of 10584 refused, 1.2 % flagged, none block-only; cost within 0.7 to 0.9 times the refusing form.

## 1. Why every block is singular

The reference run (`_rf0hv15_field6`, field golden, 6 h) swept 60302 blocks in 4176 station sweeps and refused 60279 of them as singular, 0 as non-finite. The blocks were 36241 of 4 rows (two code and two phase rows) and 23535 of 5 rows (three code and two phase rows, a second code on one band), 526 smaller. The sweep took 69.5 s of a 1585 s run. The row census of the same run reads the same rows as mostly checkable: mean redundancy number 0.69, 1.03 % of data rows below 0.05. The singularity is therefore not a row that is absorbed. It is a combination of rows.

The cofactor of a block B is Q = P^-1 - L_BB, and the statistic needs its inverse. Whitened with the row weights it is I - H_BB, whose eigenvalues are redundancy numbers in [0, 1] on one scale for code and phase. A direction of the block that a column private to the block can reproduce has redundancy number 0. On the float vehicle the line of sight has such a column: the slant ionosphere (the golden has no SH tie, `estimate_iono_sh` false, and in the dump below no row outside the block touches the column). The ambiguities do not: each carries a coupling row (the leveling carry) that lies outside the block and keeps the phase directions testable.

Measured on warm blocks. Two 1 h arms with the estimable key on, with a diagnostic dump (not committed) of every block swept in the first five sweeps at or after epoch 60: 4000 blocks per bed.

| quantity | field | sim |
|---|---|---|
| largest absolute smallest eigenvalue of the whitened cofactor | 9.6e-12 | 7.2e-12 |
| blocks with the iono column as their only private column | 3990 | 3975 |
| of those, null eigenvector in the span of that column to 1e-6 | 3989 | 3974 |
| blocks with a free ambiguity column besides (young arcs) | 6 | 25 |
| mean eigenvalues of the 4-row block, smallest first | 0, 0.53, 0.88, 0.97 | 0, 0.52, 0.87, 0.97 |
| row redundancy number, code rows, median (p10, p90) | 0.99 (0.98, 1.00) | 0.99 (0.98, 0.995) |
| row redundancy number, phase rows, median (p10, p90) | 0.22 (0.13, 0.53) | 0.22 (0.13, 0.53) |

The one exception per bed among the single-column blocks has a second near-null direction beside the iono one (0.2 % of blocks have two). The cold case is in the 10-epoch field smoke, whose two sweeps (epochs 1 and 11) read, by blocks of 4 rows, 485 keeping 3 directions and 485 keeping 1, and by blocks of 5 rows, 333 keeping 4 and 329 keeping 2: at epoch 1 the ambiguities have no carried prior and are free private columns beside the iono one, so only the code directions remain.

So the whole-block test could not pass a block on this vehicle at any epoch, and the "blind" reading of the open item is exact: 100 % of blocks refused is a property of the column layout, not of the data.

## 2. What was built

All of it is default off and leaves the whole-block form untouched when off.

- `blockEstimableForm` (`network/qc/BlockBaarda.cpp`). Whitens Q with the row weights, eigen-decomposes it (b is at most 5 in practice, 8 by design), keeps the directions with eigenvalue at least `eps` and returns the sum over them of (v' W^1/2 e)^2 / lambda together with their count m. T_B is that sum divided by m, so under the null it is sigma0^2 chi^2_m / m, which is what the pseudo-inverse of Rider B deviation 3 lacked: the degrees of freedom are known. With nothing dropped it is the whole-block statistic.
- `NetworkBlockBaarda::Input::dof` carries m. The sigma0 rescaling of the chi-square-b correction (`blockChi2GeoMean`) is taken at m, so a mixed population of kept counts is not biased. An input that does not set it is the b-row block it always was (unit test).
- The row tier of the block-only comparison (`max_row_T`) reads only the rows whose own redundancy number passes the same floor, the rows a row test can test. An absorbed row would otherwise divide a rounding-level residual by a rounding-level variance.
- `network.block_baarda_estimable` (bool, default false, needs `block_baarda`, hard error otherwise) and `network.block_baarda_estimable_eps` (default 0.05, the row census's `redundancy_eps`; outside (0, 1) restored with a warning). Plumbed through `NetworkKnobs::bb_estimable`, `bb_estimable_eps` into the accumulator.
- Readouts, printed only with the key on: a run-start line, a run-end line with the blocks by size and directions kept, a run-end line with the mean spectrum by block size (smallest first), `dof=` in the BLOCKBAARDA event detail. The existing CSV columns and run-end lines are unchanged.
- `scripts/block_baarda_join.py <run_dir>` reads the BLOCKBAARDA events against the other tiers' events at the same cells (RAIM trips, the slip detectors, the census cross-flag, MDB, prior-residual, gross-LOS, admission, readmission, hold, band scope), against the same cells a fixed time away as a control, and groups the flagged blocks by epoch and satellite and by station.

## 3. What the test cannot see

A fault that lies wholly in an absorbed direction is taken up by the column that makes the direction null. In this layout that is an additive change of the line of sight's slant ionosphere: no test of the block can see it, this one included. Two consequences are in the unit tests. A half-cycle slip of the same sign on both bands moves the phase rows nearly in the iono pattern (1 : 1.65), is mostly absorbed, and reads weak; the same slip with opposite signs is loud. The pre-registered smearing example of Rider B (a same-sign half-cycle slip on both bands) therefore does not transfer to a free-iono vehicle. Gate 2 of Rider B section 12.8 (a positive control) stays owed and must be built from the faults the block can see.

## 4. Gates run

- Unit tests `tests/network/test_block_baarda_estimable.cpp`, suite `BlockBaardaEstimableTest`, 14 cases: the absorbed direction and the whole-block refusal on the toy with a free iono; equality with the whole-block statistic when nothing is absorbed; the null statistic is chi-square with the kept count (18000 draws per case, the prior noise drawn with the data so the residual covariance is the cofactor itself: mean, variance and the 0.001 tail); a slip and a code fault are loud, an iono-pattern fault is invisible; the floor decides the kept count; the row tier reads only testable rows; malformed input is non-finite; the accumulator scales by the kept count; the rank histogram and spectrum; the sweep on a real kernel turns refused blocks into tested ones and names a slip; the knob, its dependency and its clamp.
- Linters: `nom_epoch_lint.py` PASS, `nom_n4_verify.py --whole` PASS, `make_golden_config.py --verify` PASS, `config_posture_check.py --verify-loader` PASS (0 coverage gaps).
- Knob off identical (smoke). Field and sim golden, 10 epochs (two sweeps), main-built control against the new exe with the key absent: `compare_network_run.py` lists differences only in `arm.json`, `rapid.json`, `stdout.txt`, `exitcode.txt` (paths and wall time), `network_run_manifest.json` (config path and its hash) and the `ms` column of `network_datum_graph.csv`. Every solution product, trace and block-Baarda file is identical, and the run-end block lines read 1653 of 1655 (field) and 1773 of 1773 (sim) blocks refused, as on the reference run.
- Knob on, same arms: the same files as knob off differ only in `network_block_baarda.csv`, `network_congruence.csv` (its separability join reads whether the block tier flagged anything at the station) and `network_events.csv`. The solve is untouched.

## 5. Results of the 1 h arms (hour 1, ignition included)

| | field | sim |
|---|---|---|
| station sweeps, blocks, data rows | 754, 10741, 47096 | 780, 11562, 46248 |
| refused (no direction above the floor) | 16 (6 of 1 row, 10 of 2) | 0 |
| 4-row blocks keeping 3 / 2 / 1 direction | 5794 / 11 / 502 | 10353 / 265 / 944 |
| 5-row blocks keeping 4 / 3 / 2 / 1 | 3972 / 15 / 344 / 3 | none |
| flagged (t_B above 4) | 795 (7.4 %) | 665 (5.8 %) |
| flagged and block-only | 0 | 0 |
| flagged per sweep, from the fourth (field) and the second (sim) | 4.6 to 9.4 % | 0.1 to 1.0 % |
| sigma0 at the end (G, E) | 0.375, 0.385 | 0.984, 0.969 |

The sim is calibrated: sigma0 converges to 1 and the later sweeps flag 0.1 to 1.0 % of blocks against 0.7 % expected for chi-square with 3 degrees of freedom at 4. The first sweep (epoch 1, family not warmed up, fixture start artifact) flags 623 of 891. The field formal variances are conservative by a factor of 2.6 in sigma, which is what the empirical sigma0 is for; the field flags 4.6 to 9.4 % per sweep from the fourth sweep on.

No flagged block is block-only: every flagged block also has a row above the same threshold, so on these arms the block tier sees nothing the row tier does not. The statement holds for hour 1 and for faults as they occur in the data; the injected positive control is owed.

Reading the 660 field events (`block_baarda_join.py`, window 90 s, control 1800 s, hour 1 only): RAIM spikes at the cell 12 (control 1), MW/LLI slips 11, IF-phase slips 10, arc resumes 12, slip attribution 7, MDB exceedances 37 (control 14), prior-residual events 108 (control 156), census cross-flags 0; 512 of 660 events have none of these. 234 events sit in 59 groups of three or more stations on one satellite at one epoch, the largest at epoch 11 (G03 13 stations, G10 11, G31 7), the satellite-level convergence of the ignition. Per station: DARW00AUS 41, MAJU00MHL 36, CPVG00CPV 35, KOKV00USA 34. Later in the hour the test names SEYG00SYC E12 (t_B 135 and 86), a line of sight card GAD lists as open (the C5Q rows of E11 and E12). The raw observables corroborate none of the loud blocks (455 of 476 G blocks raw-flat), so they are not slips.

Cost is not measured. The runs shared a machine with several builds, and the whole-block run-end totals are not comparable between loaded runs (field 11.4 s against 7.3 s on the smokes). The added work per block is one eigen-decomposition of a 4 by 4 or 5 by 5 matrix beside the cofactor solve, which sets the cost of the sweep.

## 6. Check owed (frozen before the runs)

Arms, from the golden copies, one key changed, a same-exe control, 2 h each, field and sim (sim from `start_offset_hours` 0.504167, already in the golden):

```
set RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom
python scripts/rapid_arm.py make --base config/golden/golden_field_refound.json --tag bbe_f2_off --hours 2
python scripts/rapid_arm.py make --base config/golden/golden_field_refound.json --tag bbe_f2_on  --hours 2 --set network.block_baarda_estimable=true
python scripts/rapid_arm.py make --base config/golden/golden_sim_refound.json   --tag bbe_s2_off --hours 2
python scripts/rapid_arm.py make --base config/golden/golden_sim_refound.json   --tag bbe_s2_on  --hours 2 --set network.block_baarda_estimable=true
python scripts/rapid_arm.py run --tags bbe_f2_off bbe_f2_on bbe_s2_off bbe_s2_on --exe <Release exe built from fix/baarda>
python scripts/block_baarda_join.py <run_dir>          # on the two _on arms
```

- **B0 (gate).** The `_off` arms against a main-built Release control on the same config (`compare_network_run.py compare`): differences only in paths, wall columns and the config hash.
- **B1 (solve unchanged).** `_on` against `_off`: only `network_block_baarda.csv`, `network_congruence.csv`, `network_events.csv` and the run-end text differ.
- **B2 (wiring).** The run-end rank line prints; blocks refused (kept none) below 1 % of blocks after the first two sweeps.
- **B3 (cost).** The `ms` of the run-end block line, `_on` against `_off`, at the same machine load, within 2 times (Rider B section 12.5); report the ratio.
- **B4 (what it flags).** Report blocks tested, refused, flagged and block-only, from hour 2 on (hour 1 is ignition). Read every flagged block against the RAIM, slip and census events of the same epochs with the join script, and state the share with no companion and the satellite- and station-wide groups. No bar on the shares: they decide whether a response card is worth writing. The block-only fraction near 0 closes the question that Rider B section 12.4 states (the block tier adds nothing here), and does not by itself retire the census.

Arming or disarming the census in the goldens is the author's decision; the goldens are not edited by this card.

## 7. Owed

- The 2 h check above, on both beds. (Done on 2026-10-18; the result is section 8. The two items below remain owed.)
- A positive control for the block tier: a fault of the kind it can see (opposite-sign cycle slips, code multipath), injected with `rapid_arm.py` ops, graded against the row tier, so the block-only fraction can be read against a known answer.
- The ignition population: the first sweep flags most of the sim (623 of 891). A sweep is read from the epoch at which sigma0 is warm; a guard that withholds events before then is an output change and is not part of this card.

## 8. Check of 2026-10-18: the 2 h arms and the same-build gates

Executable under test: Release build of `fix/baarda` at `a0b8e16` (clean tree, full rebuild), sha256 `aa0fa357871242fe`, run as `gnss_processor_w3baarda.exe`. Control: main `30ca49e`, `gnss_processor_w3base.exe`. Arms `D:/GnssProcessorSupport/runs_nom/_rap_bbe_{f,s}2_{ctl,off,on}`: golden copies, 2 h, `ctl` the control executable with the key absent, `off` the executable under test with the key absent, `on` with `network.block_baarda_estimable` true. The goldens set no `network.start_offset_hours` (the card's section 6 says the sim golden carries 0.504167; it does not, and the golden's own note says to grade the sim from 00:30:15), so both beds start at the first epoch of their data, 00:00:00 (field) and 00:00:30 (sim) on the run clock, and the sim's first 30 minutes are the fixture's start artifact. Hours are counted from the arm start; the sim is also read from 00:30:15. The grader and the readers are in `D:/GnssProcessorSupport/gates/w3baarda_chk/` (`bbe_grade.py`, `join_hour.py` over `scripts/block_baarda_join.py`).

### 8.1 Knob off and knob on (bars B0 and B1)

- **B0, key absent, executable under test against the main control, field and sim 2 h.** `compare_network_run.py` lists differences only in `arm.json` (the product output path), `exitcode.txt` (wall time), `network_datum_graph.csv` (the `ms` column alone), `network_run_manifest.json` (config path and hash), `rapid.json` (tag) and `stdout.txt`. In `stdout.txt` the lines match as a multiset once the leading time stamp, the run directory name and the digits are masked, except for the column widths of the section-timer tables; the order of the parallel station set-up lines moves. `products.txt` is byte-identical in both beds.
- **G5, 23 arms against the `w3base` side:** all 23 arms and the snapshot bytes IDENTICAL (24 IDENTICAL lines; detail in `D:/GnssProcessorSupport/gates/g5/gate_w3baarda/`).
- **User side (`uom_gate.py`, quick set, 16 arms against `w3base`):** 16 of 16 IDENTICAL.
- **Real time against `w3base`.** Dr set (`sbf`, `sbf_bssd`, `sbf_te`): every artifact identical, the one difference in each arm is the wall-clock figure of the line "Real-time session ended" in `console.log` (1677, 2263 and 3741 ms against 7129, 7168 and 8365 ms). Cl arm: every CSV, `network_products.txt`, `network_solution.csv`, `network_events.csv` and the other logs identical; the differences are the wall-clock figures of four lines in `console.log` and `main_log.log` (RINEX ingestion 7.765 s against 32.942 s, epoch-loop wall, the product round-trip self-check 2.04 s against 0.44 s, the arc-ledger cost line).
- **Unit tests:** the full Debug suite of the same sources ran 3352 tests in 450 suites, 3322 passed, 30 skipped (fixture-gated), none failed, `BlockBaardaEstimableTest` 14 of 14; `nom_epoch_lint.py`, `nom_n4_verify.py --whole`, `make_golden_config.py --verify` and `config_posture_check.py --verify-loader` pass on the same tree.
- **B1, key on against key off, same executable.** Only `network_block_baarda.csv`, `network_congruence.csv`, `network_events.csv`, the run-end text and the declared wall columns differ. In `network_events.csv` the additions are the BLOCKBAARDA rows (field 1195, sim 764) and 62 and 66 CONGRUENCE rows whose detail reads the block tier (the `block_flag` column of `network_congruence.csv`, 648 and 193 rows). `products.txt` is byte-identical key on against key off in both beds, so the wire, the solution and the held-out consumer do not move.

### 8.2 Blocks tested, refused and flagged (bar B2)

With the key absent the golden's armed census refuses 19902 of 19904 field blocks (99.99 %) and 21263 of 21263 sim blocks (100 %). With the key on:

| | field hour 1 | field hour 2 | sim hour 1 | sim hour 2 |
|---|---|---|---|---|
| blocks | 9920 | 9984 | 10679 | 10584 |
| refused (no direction above the floor) | 16 | 4 | 0 | 0 |
| flagged (t_B above 4) | 746 (7.52 %) | 706 (7.07 %) | 660 (6.18 %) | 123 (1.16 %) |
| flagged and block-only | 0 | 0 | 0 | 0 |
| flagged, raw-flat / raw-stepped / no evidence | 709 / 33 / 4 | 689 / 15 / 2 | 35 / 0 / 625 | 122 / 1 / 0 |
| flagged per sweep, range | 0.1 to 20.1 % | 5.9 to 9.0 % | 0.1 to 69.9 % | 0.5 to 1.6 % |
| sigma0 of the last sweep of the hour (G) | 0.367 | 0.390 | 1.003 | 0.888 |

The sim from 00:30:15, the grading start of that bed (sweeps 7 to 24): 15947 blocks, 0 refused, 149 flagged (0.93 %), 0 block-only (146 raw-flat, 1 raw-stepped, 2 with no evidence). Its first sweep flags 623 of 891 blocks (the ignition inside the start artifact, scale not warm); the other five sweeps of the artifact flag 11 of 4425 (0.25 %).

Whole run: field 19904 blocks over 24 sweeps, 20 refused (0.10 %: 10 of one row, 10 of two), 1452 flagged, 1195 events; the 4-row blocks keep 3 directions in 11155 of 11691, the 5-row blocks keep 4 in 7663 of 8035. Sim 21263 blocks, all of 4 rows, 0 refused, 783 flagged, 764 events; 3 directions kept in 20037, 2 in 265, 1 in 961. After the first two sweeps the refused fraction is 8 of 18249 (0.04 %) in the field and 0 in the sim. The mean smallest redundancy number of the whitened block cofactor over all blocks is -3e-12 (4 rows) and -1.3e-11 (5 rows) in the field and -5.5e-13 in the sim, the next ones 0.59, 0.93 and 0.98: the null direction of section 1 is present in every size class, and the hour-1 numbers of the 1 h arms of section 5 reproduce exactly (the field's 746 flagged plus the 49 of the 13th sweep are the 795).

The sim is not at sigma0 = 1 in hour 2: the G scale falls from 5.1 (second sweep) through 1.03 (eleventh) and 1.003 (twelfth, end of hour 1) to 0.888 at the 24th sweep (0.884 on the run-end line, E 0.886) and is still falling by about 0.004 per sweep, so "calibrated" holds at 1 h (0.984 and 0.969 at the 13th sweep) and not as a fixed point. The field scale rises slowly from 0.31 to 0.39 (G) and 0.33 to 0.40 (E) over the two hours.

### 8.3 Wall cost (bar B3)

The census sweep time of the run-end line, with the wall time of the run. The `off` and `on` arms of one bed ran in one launch on a machine shared with other sessions' builds and runs; the `ctl` arms ran earlier at another load.

| arm | sweep ms | run wall s |
|---|---|---|
| field ctl / off / on | 104457 / 98170 / 67208 | 4197 / 4084 / 4019 |
| sim ctl / off / on | 74029 / 111447 / 99942 | 2100 / 2716 / 2813 |

On against off: 0.68 (field) and 0.90 (sim), inside the 2 times of Rider B section 12.5. Two executables with identical products differ by up to 1.5 times between loads (sim ctl against off), so the ratios say only that the estimable form costs nothing measurable beside the cofactor solve; it replaces the factorisation test of the whole-block form with one eigen-decomposition. The sweep is 2.4 % (field) and 4.1 % (sim) of the run wall with the key off, for a census that refuses every block; this is the cost the open item calls 5 % of the loop wall.

### 8.4 What the test flags (bar B4)

Hour 2 on, key on. No flagged block is block-only on either bed, in either hour: every flagged block also has a row above the same threshold, so the block tier finds nothing here that the row tier does not (Rider B section 12.4 for this vehicle; it does not by itself retire the census). Most of the flagged blocks are not slips: 97.6 % (field) and 99.2 % (sim) of the flagged blocks of hour 2 are raw-flat.

Field, hour 2: 575 events (an event is rate-limited per line of sight to one in 20 epochs, so fewer than the 706 flagged blocks). Companion tiers at the same line of sight within 90 s, against the same cells 1800 s later:

| tier | hits | rate | control | lift |
|---|---|---|---|---|
| RAIM spike | 17 | 3.0 % | 2 | 8.5 |
| MW/LLI, IF-phase, attribution | 1, 2, 2 | 0.2 to 0.3 % | 0, 0, 0 | |
| gap and arc resume | 6 | 1.0 % | 2 | 3.0 |
| census cross-flag | 0 | 0 | 0 | |
| MDB exceedance | 27 | 4.7 % | 15 | 1.8 |
| prior-residual | 99 | 17.2 % | 68 | 1.5 |
| band scope | 7 | 1.2 % | 5 | 1.4 |

445 of 575 (77 %) have none of these. 36 epoch and satellite groups of 3 or more stations hold 121 of the 575 cells (largest 5 stations); over the whole 2 h the groups are 92 with 345 of 1195 cells, the largest at the second sweep (G03 13 stations, G10 11, G31 7). The flags concentrate by station and not by line of sight: CPVG00CPV is flagged in 11 of the 12 sweeps on 13 satellites (40 events), DARW00AUS 37, MAJU00MHL 33, SAVO00BRA 29, LCK400IND 26, KOKV00USA 25, a station-level pattern that the run does not explain (its noise against its formal variances is the obvious candidate, untested here). SEYG00SYC E12 is named at 600 s spacing (the event holdoff) with t_B 86, 70, 42, 30 and 24 (raw-flat), the line of sight card GAD lists as open. Neither is a demonstrated fault: the positive control of section 7 is what separates a fault from a station or signal property.

Sim, hour 2: 112 events, none with a companion in the RAIM, slip, resume, census, gross-LOS or readmission tiers; prior-residual 11 (lift 1.0), MDB 2 (control 4); no epoch and satellite group of 3 stations; 100 of 112 without a companion. S29000SIM has 13 events on 6 satellites in 6 sweeps (t_B median 4.7), the rest are spread (at most 6 per station).

### 8.5 Reading

- The open item is exact: the whole-block form is blind on this vehicle (99.99 % and 100 % refused on the 2 h arms) and the estimable form tests 99.90 % (field) and 100 % (sim) of the blocks (a 4-row block keeps 3 directions, a 5-row block 4 in the typical case).
- It does not change the solve, the wire or any other product (B0, B1, the G5 side, the user side and the real-time arms above).
- On this evidence the block tier is not a source of new detections: nothing it flags is block-only, 77 % of the field flags have no companion tier, 98 % are raw-flat, and the flagged lines of sight cluster by station. Whether the goldens keep the census armed (2.4 to 4.1 % of the run wall, in its refusing form a sweep that tests nothing) or arm the estimable form is the author's decision; no golden is edited by this card.
- Still owed: the positive control (section 7) and the guard for the unwarmed first sweep.
