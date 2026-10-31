# BBE: the block Baarda test on the float vehicle, and its estimable form

**STATUS**

- **id:** BBE
- **topic:** raim-qc
- **status:** closed
- **summary:** CLOSED 2026-10-31. ARMED 2026-10-28 at the author's decision (section 10, card BOF; landed as `2b9af4a`): `network.block_baarda_estimable` is pinned in both goldens beside `block_baarda` (ledger TRACE). BUILT 2026-10-18: the key tests the part of each (station, satellite) line-of-sight block that the data can test. The whole-block test refused every block on the float vehicle (field 60279 of 60302, sim all) because the line of sight's own slant ionosphere is a free column that absorbs one direction of the block exactly. Knob off is identical to a main control on the 10-epoch smokes, the 2 h arms and every same-build gate (sections 4 and 8), and again in an independent rebuild (section 11); knob on moves nothing but the census outputs, and no flagged block is block-only. Cost: 0.7 to 0.9 times the refusing form on loaded runs (section 8.3), 1.05 at equal load (section 11.4). 2026-10-28 (section 9): the positive control passes; the estimable form flags 1.5 m code multipath at 24 times the control's rate and no other armed check reports it. The first-sweep guard is a reader filter, `--skip-raw-formal` of `scripts/block_baarda_join.py` (section 12); a response to code multipath and the station clustering of flags are new work.
- **source:** `docs/status/OPEN_ITEMS.md` (block_baarda is blind on the float vehicle); [Rider B](../cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) section 12.3 and deviation 3 (a pseudo-inverse was refused for its unknown degrees of freedom); [REDUNDANCY-CENSUS](../cards/raim-qc/REDUNDANCY-CENSUS.md)
- **knobs / posture:** `network.block_baarda_estimable` (ledger TRACE, pinned in both goldens since 2026-10-28, section 10), `network.block_baarda_estimable_eps` (PARAM, 0.05). Measure-only: nothing in the solve reads the census.
- **dates in the text:** 2026-10-18, 2026-10-28, 2026-10-31
- **hashes / run dirs:** branch `fix/baarda` from main `30ca49e`; control exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_baarda_ctl.exe` (main), exe under test `gnss_processor_baarda.exe` (same directory); arms `D:/GnssProcessorSupport/runs_nom/_rap_baarda_{f,s}10_{ctl,off,on}` (10 epochs) and `_rap_baarda_{f,s}1h_est` (1 h); reference run `runs/_rf0hv15_field6`; dumps and comparisons `D:/GnssProcessorSupport/gates/baarda/`; reader `scripts/block_baarda_join.py`; independent check (section 11): branch `chk/baarda` (`9a47d3f`), arms `D:/GnssProcessorSupport/runs_nom/_rap_bbc_{f2,s2}_{ctl,off,on}` and `_rap_bbc_s2n_on`; reader-filter outputs (section 12) `D:/GnssProcessorSupport/gates/docsclose/bbe/`

**Outcome.** Question: why does the armed block test find every line-of-sight block singular on the float vehicle, and can it be made to work? Result: each line of sight's free slant ionosphere absorbs one direction of its block exactly (smallest redundancy number below 1e-11 in 8000 of 8000 warm blocks); the estimable form tests the other directions and refuses 4 of 9984 field and 0 of 10584 sim blocks in hour 2, at 1.05 times the refusing sweep at equal load (0.7 to 0.9 on loaded runs). Verdict: the positive control passes (1.5 m code multipath flagged at 24 times the control's rate); no flag is block-only. Standing: closed; armed 2026-10-28 (section 10); the unwarmed first sweep is left out by a reader filter (section 12).

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

## 9. Positive control, 2026-10-28

Gates written before the runs (`D:/GnssProcessorSupport/runs_nom/bof_reports/preregistration.md`, section BBE). Exe
`build/x64-Release/gnss_processor_bof.exe` (the Release build of main at `ef9f75d`, sha256[:16] 2d84a326fd00fd04), run
root `D:/GnssProcessorSupport/runs_nom`. Sim golden, 2 h, `network.block_baarda_estimable` true in every arm; the faults
are `rapid_arm.py` ops on GPS at the first six stations (S01000SIM to S06000SIM): `_rap_bof_bbe_s2_ctl` (none),
`_mp05` and `_mp15` (`drift;types=C1?,C2?;amp_m=0.5|1.5;period_s=900;per=los;seed=11`, a code sinusoid per line of
sight), `_slip` (`slip;types=L1?;rate=0.5;max=3;seed=21` and the same on L2? with seed 22, independent integer slips on
the two bands). Warm sweeps only (from 00:30:15, the start artifact of the fixture). A flag is block-only when the
largest testable row statistic of the same block (`row_t`) is below the event threshold.

| arm | flagged, injected GPS | flagged, other GPS | flagged, injected Galileo | block-only | blocks tested (G / E) | refused |
|---|---:|---:|---:|---:|---|---:|
| ctl | 9 | 68 | 8 | 0 | 8833 / 7114 | 0 |
| mp05 | 33 | 60 | 8 | 0 | 8833 / 7114 | 0 |
| mp15 | 218 | 34 | 8 | 0 | 8833 / 7114 | 0 |
| slip | 10 | 70 | 7 | 0 | 8833 / 7114 | 0 |

Events of every tier at the injected GPS cells, same window:

| arm | BLOCKBAARDA | MDB | PRIORRES | T1_if_slip | T0_band_scope | T0_mw_detect | RAIM, REDUNDANCY |
|---|---:|---:|---:|---:|---:|---:|---:|
| ctl | 9 | 44 | 241 | 0 | 2 | 0 | 0 |
| mp05 | 33 | 44 | 242 | 0 | 1 | 0 | 0 |
| mp15 | 218 | 44 | 261 | 0 | 4 | 1 | 0 |
| slip | 10 | 44 | 578 | 80 | 54 | 1 | 0 |

The golden as armed (whole-block form, `runs/_rf0hv16_sim6`, same exe): 16815 blocks in the same window, 16815
refused, 0 reported, no BLOCKBAARDA event in 6 h.

Reading.
- B1 passes: the estimable form sees code multipath of 1.5 m (218 flags against 9 at the same stations, ratio 24) and
  of 0.5 m (ratio 3.7). B3 passes: no block refused. It does not see independent slips (10 against 9); the IF-TD
  detector takes them (80 flags).
- B2 is 0 on every arm: no flag is block-only. The pre-registered reading of B2 = 0 was "the tier adds nothing the rows
  do not; drop the census". That reading assumed a row tier outside the census, and at these cells there is none: RAIM
  and the census cross-flag emit nothing, MDB is flat (44 in every arm), the prior-residual census moves 241 to 261.
  The row statistic that sees the multipath is computed inside the block sweep. Dropping the census therefore removes
  the only instrument that reports this fault, and the pre-registered reading is not followed.
- Arming the estimable form is the decision the evidence supports: it turns a census that tests nothing into the one
  tier that reports code multipath at these sizes, measure-only, at 0.7 to 0.9 times the wall of the refusing form
  (section 8.3). The guard for the unwarmed first sweep stays owed and does not block it (the first sweep is an
  event-only population). The decision is the author's.

## 10. Armed, 2026-10-28

Armed at the author's decision with card [BOF](BOF_built_off_follow_through_2026-10-28.md) (section 6, the gates and the baseline pair `runs/_rf0hv17_{field,sim}6`): `network.block_baarda_estimable` is pinned in both goldens beside `block_baarda` (ledger TRACE, golden). On the new field baseline the census tests 60209 of 60302 blocks (93 refused) and reports 4155 (6.9 %), none block-only. The guard for the unwarmed first sweep stays owed.

## 11. Independent check of 2026-10-18 (gate w3baarda)

*Text of the independent check held on the unmerged branch `chk/baarda` (commit `9a47d3f`, 2026-10-18, docs only), appended here with its headings and internal references renumbered: its section 9 is section 11 here and its 9.1 to 9.6 are 11.1 to 11.6. Its references to sections 1 to 8 are unchanged (the body of those sections is identical on main). Its closing "still owed" items were answered later: the positive control by section 9, the arming by section 10, the first-sweep guard by section 12.*

A second check of sections 5 to 8 by a separate rebuild and separate arms, run after section 8 was written. Executable under test: Release build of `fix/baarda` at `de0bd2c` in worktree `baarda_chk` (the delta from `a0b8e16`, the commit section 8 was run on, is six documentation files and nothing under `src`, `include`, `tests`, `scripts` or `CMakeLists.txt`), sha256 `b0923a18b03e4151`, run as `gnss_processor_w3baarda.exe` (gate side) and `gnss_processor_baarda_chk.exe` (arms). The hash differs from section 8's `aa0fa357871242fe` (a build in another worktree path; both files are 21331456 bytes); the products agree byte for byte (section 11.2). Control: main `30ca49e`, `gnss_processor_w3base.exe`, sha256 `50ca2ea5fbbc540c`. The gate outputs written under the label `w3baarda` by the first check (G5 arms, user-side arms, real-time captures, summary) were copied or moved to `D:/GnssProcessorSupport/gates/baarda_chk/builder_bak/` before the gates were repeated, so none of them is read here.

Arms `D:/GnssProcessorSupport/runs_nom/_rap_bbc_{f2,s2}_{ctl,off,on}` and `_rap_bbc_s2n_on`: golden copies, 2 h, `ctl` the control executable, `off` the executable under test with the key absent, `on` with `network.block_baarda_estimable` true. The `s2` arms set `network.start_offset_hours` 0.504167 (the sim golden does not carry it; the G5 `s2` arm sets it the same way), so their window is 00:30:15 to 02:30:15 and their 25th sweep closes the run in a third hour bin. `s2n_on` has no offset and is the sim arm of section 8. The `off`, `on` and `s2n_on` arms ran in one launch (one machine load); the `ctl` arms ran afterwards at a heavier load, so their wall figures are not comparable. Readers: `bbe_grade.py` of the first check (hour table) and `D:/GnssProcessorSupport/gates/baarda_chk/bbc_events.py` (events, row tier at equal false-alarm probability, same-epoch RAIM).

### 11.1 Why the whole-block test is blind (confirmed)

The run-end spectrum line of the `on` arms prints the mean redundancy numbers of the whitened block cofactor, smallest first. Field, 4 rows: -3.1e-12, 0.593, 0.926, 0.982; 5 rows: -1.3e-11, 0.609, 0.926, 0.982, 0.992. Sim, 4 rows: -4.8e-13, 0.611, 0.927, 0.986. A mean of zero to rounding over thousands of blocks means every block has the null direction, in every size class, as section 1 found by a dump. The layout accounts for it. The goldens set `network.estimate_iono_sh` false and `network.w6_iono_constraint` none (the run-start line reads "free per-LOS slant ionosphere"), so the slant ionosphere of a line of sight is a column that only that line of sight's rows touch and nothing constrains. A parameter private to a block and without a prior reproduces exactly one direction of the block's rows, which leaves that direction a redundancy number of 0 whatever the data. The ambiguities keep their carry priors outside the block, so the other directions stay testable. Re-blocking over the estimable columns cannot remove the direction (the column spans part of the block's rows by construction); the options are to test the complement, as built, or to constrain the ionosphere, which changes the posture. Key off, the census refuses 19902 of 19904 field blocks and 22023 of 22023 sim blocks.

### 11.2 Knob off identical (gate)

- **G5, 23 arms against `w3base`:** 24 IDENTICAL lines (23 arms and the snapshot bytes), detail in `D:/GnssProcessorSupport/gates/g5/gate_w3baarda/`. The four arms that exit 1 (`zero`, `joint`, `a4`, `gate`) exit 1 on both sides and compare identical.
- **User side (`uom_gate.py`, quick set, 16 arms against `w3base`):** 16 of 16 IDENTICAL.
- **Real time against `w3base`:** every artifact identical in the dr arms (`sbf`, `sbf_bssd`, `sbf_te`) and the cl arm except wall-clock text. Dr: the "Real-time session ended" line of `console.log` and `main_log.log` (ms wall). Cl: four wall-clock lines of `console.log` and `main_log.log` (RINEX ingestion, epoch-loop wall, the product round-trip self-check, the arc-ledger cost line), the `ms_per_epoch_window` column of `network_service_health.csv`, and in `network_run_manifest.json` the written time, the build stamp and `loop_ms`, which the comparator masks. Decoded and compared line by line, no line differs in any other field.
- **2 h golden arms, main control against the executable under test with the key absent, same arm configuration, both beds:** `compare_network_run.py` lists differences only in `arm.json` (the product path), `exitcode.txt` (wall time), `network_datum_graph.csv` (the `ms` column alone), `network_run_manifest.json` (config path and its hash), `rapid.json` (tag) and `stdout.txt`. In `stdout.txt` the lines pair up once digits are masked, except the column widths of the section-timer tables; the block-Baarda run-end line differs in its `ms` alone.
- **Reproduction of section 8.** The field `on` arm and the sim `s2n_on` arm reproduce section 8's `network_block_baarda.csv`, `network_events.csv`, `network_congruence.csv` and `network_solution.csv` byte for byte (the field `network_raim.csv` too), from a different executable file. The field `off` arm's `products.txt` equals the main control's.
- **Key on against key off, same executable, both beds:** only `network_block_baarda.csv`, `network_congruence.csv` (the `block_flag` column alone: 648 rows field, 336 sim), `network_events.csv` (the BLOCKBAARDA rows, 1195 field and 378 sim, and the CONGRUENCE rows whose detail reads the block tier, 62 and 14), the run-end text and the declared wall columns differ. `products.txt` is byte-identical key on against key off on both beds, so the wire, the solution and the held-out consumer do not move.
- **Unit tests, Debug build of the same tree:** 3352 tests in 450 suites, 3322 passed, 30 skipped (fixture-gated), 0 failed; `BlockBaardaEstimableTest` 14 of 14 and `BlockBaardaTest` 8 of 8 run on their own. `nom_epoch_lint.py` and `nom_n4_verify.py --whole` PASS, `make_golden_config.py --verify` PASS, `config_posture_check.py --verify-loader` 0 coverage gaps, `ledger_render.py` view matches the JSON, `docs_index_check.py` OK before this section was added.

### 11.3 Blocks tested, refused and flagged, key on (hours counted from the arm start)

| | field hour 1 | field hour 2 | sim hour 1 | sim hour 2 |
|---|---|---|---|---|
| blocks | 9920 | 9984 | 10708 | 10444 |
| refused (no direction above the floor) | 16 | 4 | 0 | 0 |
| flagged (t_B above 4) | 746 (7.52 %) | 706 (7.07 %) | 198 (1.85 %) | 174 (1.67 %) |
| flagged and block-only | 0 | 0 | 0 | 0 |
| flagged, raw-flat / raw-stepped / no evidence | 709 / 33 / 4 | 689 / 15 / 2 | 140 / 3 / 55 | 169 / 3 / 2 |
| flagged per sweep, range | 0.1 to 20.1 % | 5.9 to 9.0 % | 0 to 5.9 % | 1.3 to 2.7 % |
| key off: refused | 99.98 % | 100 % | 100 % | 100 % |

Sim: the closing 25th sweep adds 871 blocks, 13 flagged (1.5 %), none refused. Whole runs: field 19904 blocks over 24 sweeps, 20 refused (0.10 %: 10 of one row, 10 of two), 1452 flagged, 1195 events; sim 22023 blocks over 25 sweeps, 0 refused, 385 flagged, 378 events. After the first two sweeps the field refuses 8 of 18249 blocks (0.04 %). Directions kept: field 4-row blocks 3 in 11155 of 11691 (2 in 21, 1 in 515), 5-row blocks 4 in 7663 of 8035; sim 3 in 21081 of 22023 (2 in 21, 1 in 921). The sigma0 at the end of the run is 0.390 (G) and 0.397 (E) in the field and 0.840 and 0.857 in the sim. The sim `s2n_on` arm reproduces section 8 (hour 2: 10584 blocks, 0 refused, 123 flagged, 1.16 %).

The flagged fraction depends on the scale estimate as well as on the threshold. From the third sweep the sim flags 1.64 % in the arm that starts at 00:30:15 (sigma0 0.84) and 0.81 % in the arm that starts with its data (sigma0 0.884), against 0.74 % for chi-square with 3 degrees of freedom at 4: two windows of one bed differ by 0.044 in sigma0 and by a factor of 2 in the flagged share. The field flags 7.0 % from the third sweep with the same estimator, so the field tail is heavier than the scale floor accounts for. The ladder of the run-end lines, not a count at 4, is the quantity to read across runs.

### 11.4 Wall cost

Census sweep time of the run-end line and run wall, arms of one launch (same load):

| arm | sweep ms | run wall s | sweep share of wall |
|---|---|---|---|
| field off / on | 32910 / 34585 | 1074 / 1081 | 3.1 % / 3.2 % |
| sim off / on | 22824 / 23945 | 712 / 710 | 3.2 % / 3.4 % |
| sim s2n on | 23365 | 728 | 3.2 % |

Key on against key off: 1.05 on both beds, 0.16 % of the run wall in the field (1.7 s of 1074 s). The ratio of 0.68 (field) in section 8.3 is not reproduced at equal load: the estimable form adds an eigen-decomposition of a 4 by 4 or 5 by 5 matrix per block to the cofactor solve that both forms perform. The `ctl` arms (28640 ms sim, 50115 ms field, run wall 1023 s and 1862 s) ran at a heavier load and are not comparable.

### 11.5 What it flags (hour 2 on, key on)

Field, 575 events (rate-limited per line of sight): 17 have a RAIM spike at the same cell within 90 s (3.0 %, control 2, lift 8.5), 27 an MDB exceedance (4.7 %, control 15), 99 a prior-residual event (17.2 %, control 68), 6 a gap or arc resume (control 2), 1 and 2 and 2 a MW, IF-phase or attribution event, 7 a band-scope event, 0 a census cross-flag (the REDUNDANCY tier holds no event in either run), 0 a gross-LOS, admission, readmission or hold event; 445 (77 %) have none of these. The RAIM tier spiked 80 rows on 14 epochs in the 2 h run, 2 of them sweep epochs; of the 18 RAIM-spiked cells at those 2 epochs, 15 are block events of the same epoch (events are rate-limited, so 15 is a lower bound on the flagged ones). 36 epoch and satellite groups of 3 or more stations hold 121 of the 575 events. 8 of 49 flagged stations hold 234 events (41 %: CPVG00CPV 40, DARW00AUS 37, MAJU00MHL 33, SAVO00BRA 29, LCK400IND 26, KOKV00USA 25, POVE00BRA 22, KIT300UZB 22); seven of the eight have median t_B 5.1 to 6.7 and median row_t 12 to 20, so their flags are marginal on both tiers and not a hidden population; KIT300UZB has median t_B 45. 560 of 575 are raw-flat.

Sim, hour 2 and the closing sweep, 186 events: the RAIM tier never spiked in the arm (0 rows), the census cross-flag tier is empty, 6 have an MDB exceedance (control 3), 14 a prior-residual event (control 16), 1 a band-scope event; 168 (90 %) have none of these; one group of 3 stations; no station holds more than 7. 182 are raw-flat.

**Block-only is zero at a threshold that favours the row tier.** t_B is a mean of m squared standardized components, so a flag at 4 has a false-alarm probability of 0.0074 for m = 3 and 0.0030 for m = 4, while a single row flagged at 4 has 0.0455. The ratio t_B / row_t of the flagged blocks has quartiles 0.34, 0.38 and 0.46 in the field (maximum 0.90 for m = 3 and 0.65 for m = 4; 1.0 only for the 5 blocks with m = 1): a loud single row crosses the row threshold at about a third of the loudness the block needs. Row_t of the flagged blocks lies in (4, 8] for 41, (8, 16] for 299 and above 16 for 235 of the 575 field events, none at or below 4. With the row tier held to the block test's false-alarm probability (row threshold 7.18 for m = 3 and 8.80 for m = 4 per test, 9.70 and 11.76 with a union bound over the block's rows), 25 of 575 field flags (4 %) and 23 of 186 sim flags (12 %) would be block-only per test, and 91 (16 %) and 67 (36 %) per block. The reading is approximate (the rows of a block are correlated and the union bound is conservative), and it does not show that the block tier sees a pattern no row shows. It shows that "no flagged block is block-only" is a statement at the census's own threshold, not at equal power, so the positive control of section 7 is what settles whether the block form adds detections.

### 11.6 Reading

- Knob off is identical on every gate (G5, user side, real time, 2 h golden arms on both beds, Debug suite and linters); key on moves only the census outputs and the two things that read them, the `block_flag` column and the CONGRUENCE events. Section 8's figures reproduce exactly where the window is the same (field entire, sim `s2n_on`).
- The open item is exact, and the estimable form removes the blindness: 99.90 % (field) and 100 % (sim) of the blocks are tested, at 1.05 times the sweep cost of the refusing form.
- What it flags is mostly not a slip (97 % raw-flat), is concentrated in stations in the field and spread in the sim, and has companions in RAIM in 3 % of the field events. Nothing it flags is block-only at the census threshold; at equal false-alarm probability 4 to 16 % (field) and 12 to 36 % (sim) would be.
- Differences from section 8: the cost ratio (11.4); the sim window, whose proper start is 00:30:15 and whose hour-2 flagged share is then 1.7 % and not 1.2 % (11.3); the equal-false-alarm reading of block-only (11.5).
- The "Sim start" bullet of `CLAUDE.md` says the sim golden carries `start_offset_hours` 0.504167. The golden file carries no such key; every arm that grades from 00:30:15 sets it (`--set network.start_offset_hours=0.504167`). Not edited here.
- Still owed: the positive control and the guard for the unwarmed first sweep (section 7). Whether the goldens keep the census armed, arm the estimable form or disarm the block tier is the author's decision. No golden was edited and nothing was armed.

## 12. 2026-10-31: closed; the first-sweep guard, the cost at equal load, the follow-ups

| owed item | where it landed |
| --- | --- |
| section 7: the 2 h check on both beds | section 8 (2026-10-18), repeated by an independent rebuild in section 11 |
| section 7: a positive control for the block tier | section 9 (2026-10-28): 1.5 m code multipath 218 flags against 9 at the same stations (24 times), 0.5 m 33 (3.7 times), independent slips not seen (10 against 9; the IF-TD detector takes them, 80 flags), no flag block-only |
| sections 6, 8.5 and 9: arm the estimable form, or disarm the census | section 10 (2026-10-28): armed in both goldens at the author's decision |
| sections 7, 8.5, 9 and 11.6: the guard for the unwarmed first sweep | the reader filter below; a guard that withholds events in the producer is an output change and stays out of this card (section 7) |

**The first-sweep guard as a reader filter.** Every BLOCKBAARDA event already says whether its statistic was scaled with sigma0 = 1 because the family had not warmed up: `raw_formal=1` in the event detail (`src/core/network/qc/BlockBaarda.cpp`). `scripts/block_baarda_join.py` takes `--skip-raw-formal`, which leaves those events out of the join and prints how many; without the flag its output is as before. Run once on each key-on 2 h arm of section 8 (outputs `D:/GnssProcessorSupport/gates/docsclose/bbe/join_{s2,f2}_{plain,skip}.txt`):

| arm | events | left out | read | epoch and satellite groups of 3 or more stations (cells in them) | events with none of the other tiers at the cell |
| --- | --- | --- | --- | --- | --- |
| sim `_rap_bbe_s2_on` | 764 | 623 | 141 | 56 (623 of 764) to 0 | 536 of 764 to 122 of 141 |
| field `_rap_bbe_f2_on` | 1195 | 1 | 1194 | 92 (345) unchanged | 924 of 1195 to 923 of 1194 |

On the sim bed the 623 events left out are the 623 blocks that the first sweep flags of 891 (section 8.2), and the 56 satellite-wide groups of the unfiltered join are exactly those cells: with the unwarmed sweep out, no epoch and satellite group of three stations remains and 141 events are read (the MDB tier has 39 hits before the filter and 7 after; the prior-residual tier 14 and 14). On the field bed the filter changes one event, so the field's flags are not an artifact of the unwarmed scale (its formal variances are conservative by a factor of 2.6 in sigma, section 5). The guard needs no producer change.

**The cost, stated at equal load.** The estimable form costs 1.05 times the sweep of the refusing form on both beds when both run at one machine load (section 11.4; the sweep is 3.1 to 3.4 % of the run wall, and the form adds 1.7 s to the 1074 s of the field run). The 0.68 (field) and 0.90 (sim) of section 8.3 and the 0.7 to 0.9 quoted in section 9 compare arms that ran at different loads, which section 8.3 itself says, and section 11.4 does not reproduce them. The figure to use is 1.05.

**Follow-ups (new work; not owed by this card).** (a) A response to code multipath: the estimable form reports it and no response exists on either form (card BOF section 4); nothing in the solve reads the census, which is measure-only. (b) The station clustering of flags: 8 of 49 flagged stations hold 41 % of the field events in hour 2 (CPVG00CPV, DARW00AUS, MAJU00MHL, SAVO00BRA, LCK400IND and KOKV00USA among them; sections 8.4 and 11.5), a station-level pattern that the run does not explain (the stations' noise against their formal variances is the candidate, untested). Each starts as a card of its own; both are filed as register lines with the close.

Nothing else is owed on this card.
