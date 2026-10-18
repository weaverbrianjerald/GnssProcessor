# BBE: the block Baarda test on the float vehicle, and its estimable form

**STATUS**

- **id:** BBE
- **topic:** raim-qc
- **status:** open
- **summary:** BUILT 2026-10-18, default off, never armed: `network.block_baarda_estimable` tests the part of each (station, satellite) line-of-sight block that the data can test. The whole-block test refused every block on the float vehicle (field 60279 of 60302, sim all) because the line of sight's own slant ionosphere is a free column that absorbs one direction of the block exactly. Knob off is identical to a main control on the 10-epoch smokes (field and sim). The 2 h check on both beds is owed.
- **source:** `docs/status/OPEN_ITEMS.md` (block_baarda is blind on the float vehicle); [Rider B](../cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) section 12.3 and deviation 3 (a pseudo-inverse was refused for its unknown degrees of freedom); [REDUNDANCY-CENSUS](../cards/raim-qc/REDUNDANCY-CENSUS.md)
- **knobs / posture:** `network.block_baarda_estimable` (ledger UNVALIDATED), `network.block_baarda_estimable_eps` (PARAM, 0.05); both goldens keep `block_baarda` armed and do not set either key, so both are unchanged. Measure-only: nothing in the solve reads the census.
- **dates in the text:** 2026-10-18
- **hashes / run dirs:** branch `fix/baarda` from main `30ca49e`; control exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_baarda_ctl.exe` (main), exe under test `gnss_processor_baarda.exe` (same directory); arms `D:/GnssProcessorSupport/runs_nom/_rap_baarda_{f,s}10_{ctl,off,on}` (10 epochs) and `_rap_baarda_{f,s}1h_est` (1 h); reference run `runs/_rf0hv15_field6`; dumps and comparisons `D:/GnssProcessorSupport/gates/baarda/`; reader `scripts/block_baarda_join.py`

**Outcome.** Question: why does the armed block test find every line-of-sight block singular on the float vehicle, and can it be made to work? The slant ionosphere of a line of sight is a local column that only that line of sight's rows touch and nothing constrains, so it absorbs one direction of the block exactly: in 8000 of 8000 sampled warm blocks the smallest redundancy number of the whitened block cofactor is below 1e-11 and its eigenvector lies in the span of that column. The estimable form keeps the other directions (3 of 4, 4 of 5) with their count as the degrees of freedom. Sim and field 1 h arms: 0 and 16 of 11562 and 10741 blocks refused. Cost and the 2 h grade are owed.

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

- The 2 h check above, on both beds.
- A positive control for the block tier: a fault of the kind it can see (opposite-sign cycle slips, code multipath), injected with `rapid_arm.py` ops, graded against the row tier, so the block-only fraction can be read against a known answer.
- The ignition population: the first sweep flags most of the sim (623 of 891). A sweep is read from the epoch at which sigma0 is warm; a guard that withholds events before then is an output change and is not part of this card.
