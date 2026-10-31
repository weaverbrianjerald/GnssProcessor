# BDSB: the BDS card, continued (the independent check of the second-band admission)

**STATUS**

- **id:** BDSB
- **topic:** network-vehicle
- **status:** open
- **summary:** Card BDS's continuation (that file is at the 60 KB bar after its section 9), from 2026-10-19. The second-band admission (`network.second_band_admission`, default off, ledger row UNVALIDATED, not armed) was re-run from fresh runs on the Release build of branch `fix/single-band` at `f300bda` (exe sha256 `1183ce58…`). Key off: 24 IDENTICAL against the parent side after the two declared run-end census lines (products, every trace, manifest and snapshot bytes identical before any reversal; products and traces also identical against `w3base`). Key on at station scope, 6 h field golden with BeiDou: no station rejected (2,160 in 720 epochs with the rule off), the heal (3,323 columns on 551 epochs) and the S-basis completion (427 directions on 299 epochs) silent, WTZZ00DEU within 0.6 mm of the subtraction arm GEC-s, 16-site census median change against GEC-s 0.0000 m with no site above 6 mm.
- **source:** [BDS](BDS_beidou_third_constellation_2026-10-18.md) sections 8 and 9 (the rule, its development arms, the first identity gate and the 6 h grade); the request of 2026-10-19 to repeat the identity gate and the 6 h grade from fresh arms
- **knobs / posture:** `network.second_band_admission` (default false), `network.second_band_scope` (`station` default), `network.second_band_min_sats` (default 1); the arms differ in this knob alone; not armed in either golden
- **dates in the text:** 2026-10-19
- **hashes / run dirs:** exe `gnss_processor_w3sbandv.exe` (copy of the Release build of `f300bda`, sha256 `1183ce584a938bdc6827db126d0b3f2def18839607a2906e80f236dca2d86655`); gate side `w3sbandv` under `gates/g5` and `gates/g7`; 6 h arms `_rap_sbv_{ge6,off6,on6}`, census keys `sbvge`, `sbvoff`, `sbvon` (runs root `D:/GnssProcessorSupport/runs_nom`); readouts in `D:/GnssProcessorSupport/gates/sband/` (`v_*` files)
- **gates (frozen at pre-registration):** card BDS section 4 and section 9.1 (identity gate against the parent side), section 9.2 (counters and consumer by hour), section 9.3 (census)

**Outcome.** Question: do the identity gate and the 6 h grade of card BDS section 9 hold when repeated from fresh runs? They do. With the key off the gate reads 24 IDENTICAL against the parent side (the products and traces are identical against the pre-merge side `w3base` as well), and the user side and the real-time engine do not reach the seam. With the key on at station scope the rule reproduces the subtraction GEC-s in every quantity read: no station is rejected, the heal and the S-basis completion are silent, the consumer is within 0.6 mm and the census median change is 0.0000 m with no site above 6 mm. The ledger row stays UNVALIDATED; the 305-station regime, a second day, the warm resume and the satellite-scope hysteresis were not run.

## 1. What this card continues

Card BDS (filed 2026-10-18) reads BeiDou on B1I and B3I as the third constellation of the field golden and records, in sections 8 and 9, the second-band admission and its first identity gate and 6 h grade. Its file stands at 59.9 KB, so the independent repeat of 2026-10-19 is recorded here. Card BDS keeps the design, the gates and the earlier results.

## 2. 2026-10-19: the independent check

The identity gate and the 6 h grade of card BDS section 9 were repeated from scratch on the Release build of branch `fix/single-band` at `f300bda` (the code is that of `1546d1c`; `a73df8b`, `b4446ee` and `f300bda` change the card and the ledger only). Exe sha256 `1183ce58…`, gate copy `gnss_processor_w3sbandv.exe`. Fresh gate side `w3sbandv` and fresh arms `_rap_sbv_{ge6,off6,on6}` under `D:/GnssProcessorSupport/runs_nom`, so no output of card BDS section 9 was reused; census keys `sbvge`, `sbvoff`, `sbvon`. The arms were made with `rapid_arm.py make` from the `arm.json` of `_rap_bds_ge6` and `_rap_bds_gec6`; a flattened diff shows the output path alone for GE and rule-off, and the output path plus `network.second_band_admission` true for rule-on.

### 2.1 Checks of the tree

- Release build: `ninja: no work to do`, sha256 equal to the gate exe of card BDS section 9.
- Debug suite: 3385 cases, 3355 passed, 30 fixture skips, 0 failed; `SecondBand*` 28 of 28. `nom_epoch_lint` and `nom_n4_verify --whole` PASS; `ledger_render --check`, `make_golden_config --verify` (0 failures), `config_posture_check --verify-loader` (0 coverage gaps) and `docs_index_check` pass.

### 2.2 Key off: identity gate (G5, 23 arms and the snapshot bytes)

| comparison | products, every trace, `rt_engine.log` | manifest | whole logs | snapshot bytes |
| --- | --- | --- | --- | --- |
| against the parent side `w3sbandref` (exe `5fc9c713…`, parent commit `a54155a`), raw | IDENTICAL in 23 of 23 | IDENTICAL in 23 of 23 | 48 to 56 lines differ per arm: the entity census only | IDENTICAL |
| the same, with the two declared census lines reversed (`subs_w3sband.sed`) | IDENTICAL | IDENTICAL | IDENTICAL | IDENTICAL (24 IDENTICAL, `summary_w3sbandv_subs_vs_ref.txt`) |
| against the pre-merge side `w3base` (main `30ca49e`), raw | IDENTICAL in 23 of 23 | 1 line differs in each arm | 49 to 507 lines differ per arm | IDENTICAL |

- The declared difference of the candidate is the run-end entity census (110 declared fields against 109, and the added row `station.sba_since (was ) ColdStart rows max 0 final 0`). The exits of the four arms that end with exit 1 by design (`zero`, `joint`, `a4`, `gate`) equal those of both reference sides.
- The reference of this rule is the parent side, not `w3base`. The manifest line that differs from `w3base` is the `param_responses` record, which gains three declared response sites (`cb_hard_refloat`, `clk_hard_refloat`, `pb_hard_refloat`; armed false, count 0). They come from commit `d84f695` (card [NSD](NSD_network_small_defects_2026-10-17.md)), which is in main `8d15451` and in the parent commit; the feature commit touches none of them, and the candidate record equals the parent-side record. A gate against `w3base` therefore cannot read 24 IDENTICAL for any build of this branch; the products and traces, which it does read identical, are the datum of the identity.
- Same-exe arms against the arms of card BDS section 5.2: `_rap_sbv_ge6` against `_rap_bds_ge6` and `_rap_sbv_off6` against `_rap_bds_gec6` read identical in all 36 product and trace files (the manifest line and the census lines differ as above).
- **User side and real time** are not run: the seam has one call site, in the network epoch driver (`ApplicationNetworkEpoch.cpp`), so neither `Application::run` nor the real-time engine reaches it.

### 2.3 Key on and off, 6 h, one exe

| quantity | GE | GEC, rule off | GEC, rule on | GEC-s `bds_gecs6` (card BDS section 5.2, `w3base` exe) |
| --- | --- | --- | --- | --- |
| exit, wall (s) | 0, 1,697 | 0, 4,638 | 0, 4,508 | 0, 5,226 |
| epochs emitted of 720 | 720 | 720 | 720 | 720 |
| stations in the solve, min / median | 57 / 58 | 54 / 55 | 57 / 58 | 57 / 58 |
| addStation rejections (`n_sta_pd_dropped`) | 0 | 2,160 | 0 | 0 |
| observed near-null count at the gate, min to max (declared) | 2 to 2 (2) | 3 to 21 (3) | 3 to 3 (3) | 3 to 3 (3) |
| S-basis completion, directions on epochs | 0 | 427 on 299 | 0 | 0 |
| phase-anchor heal, columns on epochs | 0 | 3,323 on 551 | 0 | 0 |
| rule edges, withhold / readmit (event rows) | none | none | 5 / 2 | none |
| epoch loop, ms per epoch (shared box, uncontrolled) | 2,262 | 6,335 | 6,161 | not read |

- Rule off reproduces plain GEC counter for counter: three stations rejected whole at every epoch (2,160 in 720), the heal on 551 epochs, the completion on 299. Rule on has none of the three and equals GEC-s in every row.
- **What the rule did.** BeiDou withheld at five stations, 30,113 records in 2,165 of 78,587 station-constellation-epochs (721 screened; the 12,488 records of HKSL00HKG, which carry no modelled band, are left alone): WARK00NZL, MAJU00MHL and SCTB00ATA for the whole run (17, 8 and 16 satellites, none on the pair); MAC100AUS at TOW 259230 (12 satellites, readmitted at 259260 with 5 on the pair); BSHM00ISR at TOW 279540 (15 satellites, readmitted at 279570 with 2). Each edge is one `ADMISSION` / `second_band` event row and one log line naming the station and the constellation. The stations are kept: WARK00NZL, MAJU00MHL and SCTB00ATA hold 5,094, 3,486 and 6,451 GPS rows and 3,510, 2,106 and 3,218 Galileo rows in `network_station_residuals.csv` with the rule on, and none with it off (rejected whole).
- Datum-free per-epoch rms of the change in satellite clock `c0`, rule on against the GEC-s wire: GPS at most 5.4 mm (median 0.1 mm), Galileo 34.6 mm (1.6 mm), BeiDou 77.3 mm (0.5 mm), the maxima in the first four epochs. Rule on against rule off: GPS up to 134 mm (median 51 mm), Galileo up to 79 mm (25 mm), BeiDou up to 131 mm (73 mm).

**WTZZ00DEU, 3D p50 by hour (m), the consumer on the arm's own systems.**

| arm | h1 | h2 | h3 | h4 | h5 | h6 | below 0.1 m of 720 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GE (`sbv_ge6`) | 0.058 | 0.011 | 0.018 | 0.010 | 0.015 | 0.011 | 688 |
| GEC, rule off | 0.033 | 0.017 | 0.020 | 0.017 | 0.018 | 0.014 | 689 |
| GEC, rule on | 0.030 | 0.015 | 0.019 | 0.017 | 0.018 | 0.015 | 687 |
| GEC-s | 0.030 | 0.015 | 0.019 | 0.017 | 0.018 | 0.015 | 689 |

Rule on minus GEC-s: -0.6, 0.0, -0.1, -0.1, 0.0, 0.0 mm by hour; rule on minus rule off: -3.3, -2.2, -0.9, +0.4, -0.2, +1.3 mm; rule on minus GE: -28.1, +3.3, +0.7, +6.9, +2.1, +4.3 mm.

### 2.4 Census, 16 sites

Median site p50 by hour (m): GE 0.136 0.054 0.041 0.036 0.040 0.037; GEC rule off 0.096 0.039 0.033 0.036 0.031 0.031; GEC rule on 0.089 0.036 0.032 0.032 0.030 0.031; GEC-s (`bdsgecs`) 0.092 0.035 0.032 0.032 0.030 0.031. Each key replays the 16 sites on the same exe against the wire's own arm.

| paired change, arm against base | hours 2 to 6, median (m) | better / worse at 1 cm | hour 1, median (m) | better / worse |
| --- | --- | --- | --- | --- |
| rule on against rule off | -0.0010 | 2 / 0 | -0.0103 | 8 / 0 |
| rule on against GE (`sbvge`) | -0.0035 | 5 / 0 | -0.0346 | 11 / 1 |
| rule off against GE | -0.0025 | 5 / 0 | -0.0162 | 10 / 2 |
| GEC-s against GE | -0.0038 | 5 / 0 | -0.0331 | 10 / 2 |
| rule on against the GEC-s census (`bdsgecs`) | 0.0000 | 0 / 0 | -0.0008 | 2 / 1 |

- Rule on against GEC-s, per site, hours 2 to 6: largest changes ARHT00ATA -0.0056 m and DAV100ATA +0.0045 m; no site above 6 mm. Rule on against GE: no site worse by 1 cm (largest ASCG00SHN +0.0043 m); the large gains are ARHT00ATA -0.112 m and DAV100ATA -0.067 m.

### 2.5 Verdict

- Key off identical: PASS. 24 IDENTICAL against the parent side after the two declared census lines (products, every trace, manifest and snapshot bytes identical before any reversal). Against `w3base` the products and traces are identical in all 23 arms, and the remaining differences are the NSD manifest rows and log text, which the parent side shows equally.
- Grade: ran. Every number of card BDS section 9 (counters, consumer by hour, census, withheld records, edge rows) is reproduced from fresh runs to the precision printed there. The rule reproduces the subtraction GEC-s: no station is rejected, the heal and the S-basis completion are silent, the consumer differs from GEC-s by at most 0.6 mm, and the census median change against GEC-s is 0.0000 m with no site above 6 mm.
- The ledger row stays UNVALIDATED and is not armed. Not run (as in card BDS section 9.5): the 305-station regime, a second day, the warm resume, the satellite-scope hysteresis, the users-of-the-wire census, and a same-exe 6 h GEC-s (the comparator `bds_gecs6` ran on the `w3base` exe).

## 3. 2026-10-31: posture C on the current golden (pre-registered)

Committed before the smoke, the 6 h arm, the replays and the offline reads of this section were run or read. A change after a result is read is a dated amendment below this section, never an edit of it.

### 3.1 Question

Cards BDS, BDSB and MDAY graded posture C (GPS + Galileo + BeiDou on B1I + B3I with the second-band admission and the per-signal wire, written C below) before cards MRF and BOF armed the MW re-flag skip, ocean tide loading, the noon-turn attitude, the estimable block-Baarda test and the seed mirrors, and the census sites of card BDS305 are all producers there (its off-roster read is three consumers). The adoption instrument on the golden as it stands is missing. Questions: does C pass the gates of card BDS section 4 against the current golden (baseline pair `runs/_rf0hv17_{field,sim}6`); do the users of the five multi-day C wires who do not use BeiDou lose; and what do the four offline reads of section 3.5 add to the open items of cards BDS, BDSB, MDAY and BDS305. Arming is the author's decision and is not made here.

### 3.2 Exe, control and arm

- **Exe.** `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_bdsclose.exe`, a renamed copy of `C:/GnssProcessor/build/x64-Release/gnss_processor_bofarm.exe`, the exe of the baseline pair; sha256 `91143c81de9a95479aa08131c08945676d5e542dbd607e9a8ff6d1852d2d73d6` for both. Nothing is built in this track. Run root `D:/GnssProcessorSupport/runs_nom` (`RAPID_RUNS_ROOT`).
- **Control (GE).** `runs/_rf0hv17_field6`: exit 0, 1,343.5 s, 720 of 720 epochs emitted, 1,784.57 ms per epoch on the load of its own day. It was copied whole to `runs_nom/_rap_bdsc_ctl` so that replays resolve under the run root (sha256 of `products.txt` `bf16e947...` and of `arm.json` `956226c6...`, equal to the originals). Its `arm.json` equals the current field golden in the comment and the output path only.
- **Arm C6** (tag `bdsc_c6`, 6 h, 58 stations): the control's `arm.json` plus exactly the four posture-C keys of the 305-station C arm of card BDS305 (`_rap_bds305_c_f6` against its GE twin differ in these four and the output path). Flattened leaf diff of `_rap_bdsc_c6/arm.json` against `runs/_rf0hv17_field6/arm.json` (`gates/bdsclose/tools/flatdiff.py`; `rapid.json` beside it records the `--set` list):

  ```
  + network.n3c_wire_per_signal = true
  ~ network.product_output_path: "C:/GnssProcessor/runs/_rap_rf0hv17_field6/products.txt" -> "D:/GnssProcessorSupport/runs_nom/_rap_bdsc_c6/products.txt"
  + network.second_band_admission = true
  ~ processing.active_systems: ["GPS", "Galileo"] -> ["GPS", "Galileo", "BeiDou"]
  + processing.signal_priorities.BeiDou = ["C2I", "L2I", "C6I", "L6I"]
  # 5 differing leaves (446 in A, 449 in B)
  ```

  `network.sparse_station_normals` is in the golden; `network.second_band_scope` (`station`) and `network.second_band_min_sats` (1) stay at their loader defaults.
- **Same-load control** (tag `bdsc_ge6r`, no bar): the control's `arm.json` unchanged, started together with C6 on the same exe. It prices the cost under one load (S6) and checks that the exe copy reproduces the baseline run (`products.txt` byte-identical to `runs/_rf0hv17_field6/products.txt`). Gates use the baseline run as the control; a re-run that differs is reported and changes no gate.
- **Smoke** (tag `bdsc_c6_sm`): C6 with `--hours 0.0833` (10 epochs) before the 6 h arm; expected exit 0, 10 of 10 epochs emitted, `C##` clock lines on the wire. A failed smoke is repaired by a dated amendment before the 6 h arm; no bar moves.
- **Census replays** (`scripts/clock_level/gcensus.py make`, 6 h, WTZZ00DEU and the 16 sites of `scripts/clock_level/site_list.json`, tags `<key>_<site4>`, each key the consumer base `arm.json`, the wire and the exe above):

  | key | consumers | wire | role |
  | --- | --- | --- | --- |
  | `bdsc6ge` | control arm (GPS + Galileo) | control | reference of S4 and of S5 clause 1 |
  | `bdsc6c` | C6 arm (GPS + Galileo + BeiDou) | C6 | S4 |
  | `bdsc6cge` | control arm | C6 | S5 clause 1 |
  | `bdsc6geg` | GPS-only base | control | reference of S5 clause 2 |
  | `bdsc6cg` | GPS-only base | C6 | S5 clause 2 |

  The GPS-only base is the control's `arm.json` with `processing.active_systems` `["GPS"]` and the `processing.signal_priorities.Galileo` entry removed (the form of the G arm of card BDS section 3; `runs_nom/_bdsc_bases/g_c6stage/arm.json`, two differing leaves). Card BDS read S5 clause 2 against `bdsg`, the GPS-only wire of a GPS-only producer arm. The golden is GPS + Galileo and the decision is C against the golden, so the clause is read here against the GPS-only users of the control wire (`bdsc6geg`); no GPS-only producer arm is run.

### 3.3 Gates of the 6 h arm

Gates S1, S2, S4, S5, S6 and S7 of card BDS section 4 apply unchanged to C6 against the control; S3 (GE against G) is on file and is not re-run. Hours are counted from 00:00 UT of DOY 129 (2024); hour 1 is ignition and settled claims come from hours 2 to 6. Better and worse count at 1 cm. Medians are applied unrounded. A bar missed is recorded as missed and not reworded.

| gate | read-out | passes when |
| --- | --- | --- |
| B0 | `rapid.json` and the flattened `arm.json` diff of each arm and replay; the exe hash | C6 differs from the control arm in the four keys and the output path only (printed above); each replay differs from its base in the wire, consumer and output paths only; the exe sha256 is the one of 3.2 |
| S1 | C6 over 720 epochs, gates H1 to H4 of card BDS section 4 | H1: exit 0; 720 of 720 epochs emitted; 0 skipped; 0 withheld by the observability gate, the A4 ceiling or the rank gate (UNDER or OVER-specified). H2: BeiDou `c0` (the `C,C##` lines of `products.txt`) at most 50 m at every epoch; 1 km or more is the signature of failure mode A; a maximum between the two is neither healthy nor mode A and the verdict waits for the author. H3: GPS and Galileo clocks of C6 against the control wire, same epochs, the per-epoch rms of the change after removing the per-epoch median over the constellation at most 1.0 m at every epoch, and the maximum absolute `c0` of both at most 50 m. H4: the median epoch carries at least 10 `C##` satellites on the wire, each with a clock line, code-bias lines and phase-bias lines; the run log's BeiDou counters and the run-end report of the second-band admission are reported with the zero cases named; an admission report reading "ARMED AND INERT" fails H4 |
| S2 | WTZZ00DEU `closed_loop_trace.csv`: 3D p50 by hour and epochs below 0.1 m. Row 1: the in-run consumer of C6 on its own systems (GPS + Galileo + BeiDou). Row 2: the GPS + Galileo consumer of the C6 wire (`bdsc6cge_wtzz`) | Row 1, C6 minus control, no worse than +0.010 m in any of hours 2 to 6. Row 2 is read. One instrument: read, not adopted on |
| S4 | `gcensus.py grade bdsc6ge bdsc6c`: paired change of each site's mean hourly p50 over hours 2 to 6 (16 sites), C6 consumers on the C6 wire against GE consumers on the control wire | median at most 0 m and more sites better than worse; no site worse by more than 0.05 m without being named. Hour 1 reported. Read apart by what the site tracks: the ten sites with the B1I + B3I pair (QUIN00USA, HARB00ZAF, POHN00FSM, DAV100ATA, STPM00SPM, ASCG00SHN, CEDU00AUS, KOKB00USA, ARHT00ATA, WUH200CHN), the two with B1I and no B3I (CHTI00NZL, SCRZ00BOL), the four without a usable pair (ANMG00MYS, BADG00RUS, UTQI00USA, BAKE00CAN) |
| S5 | users of the C6 wire. Clause 1: `gcensus.py grade bdsc6ge bdsc6cge`. Clause 2: `gcensus.py grade bdsc6geg bdsc6cg` | each clause: paired median over hours 2 to 6 at most +0.005 m; sites worse by more than 0.05 m named |
| S6 | cost | read: process wall (`exitcode.txt`) and epoch-loop ms per epoch (the run log's `Network[timing]` line) of C6 and of the same-load control re-run, with the count of other `gnss_processor` processes on the machine sampled each minute |
| S7 | BeiDou wire | read: datum-free rms of `c0` by hour for `C`, `G` and `E` (the deviation from the a priori product, not a truth), the median count of `C##` satellites per epoch, the BeiDou rows of the run-end tables. Extra, not gated: a BeiDou-only consumer of the C6 wire at WTZZ00DEU (`bdsc6bds_wtzz`, `processing.active_systems` `["BeiDou"]`) |

### 3.4 R2: users of the multi-day wires

The wires are `runs_nom/_rap_mday_c_128` to `_rap_mday_c_132` (24 h each; DOY 132 for 86,100 s), card MDAY's arm C. Consumers are replayed on `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_mday.exe` (sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`), the exe that produced the wires and the GE census of card MDAY, and on no other. Per day 17 consumers (WTZZ00DEU and the 16 sites), `gcensus.py make --day <doy> --hours 24`, with `--obs-root D:/GnssProcessorSupport/data_mday` for DOY 128 and 130 to 132 and `--duration-s 86100` for DOY 132, as in card MDAY.

| clause | consumers | wire | reference |
| --- | --- | --- | --- |
| R2a, GPS + Galileo users | the day's GE arm `_rap_mday_ge_<doy>/arm.json`; key `bdsr2cge<doy>` | `_rap_mday_c_<doy>` | `mdyge<doy>` of card MDAY (the same consumers on the GE wire, same exe) |
| R2b, GPS-only users | the GPS-only base of the day (the GE arm with `processing.active_systems` `["GPS"]` and the Galileo priority entry removed; `runs_nom/_bdsc_bases/g_doy<doy>/arm.json`); key `bdsr2cg<doy>` | `_rap_mday_c_<doy>` | the same GPS-only consumers on `_rap_mday_ge_<doy>`, key `bdsr2geg<doy>` (run for this card) |

Bar, per clause and day: the paired median over the 16 sites of the change (users of the C wire minus users of the GE wire) of each site's mean hourly p50 over hours 2 to 24 at most +0.005 m; sites worse by more than 0.05 m named. Each day and clause is reported; a day that misses its bar fails that clause.

Read (no bar), DOY 130 after 10:46 UT (card MDAY section 6.1: G03 about -11 m on the C wire from its return at 10:46:00, against 4 m on the GE wire): for both clauses and for each of the 17 consumers, the 3D p50 and the epochs above 0.2 m and above 1 m over 10:46:00 to the end of the day under each wire; the 16-site paired median of the change, sites worse than +0.01 m named; the C-wire and GE-wire G03 clock over the same window. A paired median above +0.005 m, or a consumer above 1 m under the C wire and not under the GE wire, is recorded as an effect of the G03 level on users.

### 3.5 Offline reads of existing outputs (no producer run; no bar)

- **(a) The BeiDou clock class on the BDS305 wires.** `runs_nom/_rap_bds305_c_f6` and `_rap_bds305_ge_f6`, `products.txt` (305 stations, 6 h, card BDS305): the distribution of absolute `c0` per constellation (median, p99, maximum with satellite and epoch, counts above 50 m and above 1 km); the GPS and Galileo clocks, C against GE, datum-free (per-epoch rms of the change after removing the per-epoch median), by hour, with the constellation-common shift. H2 and H3 of 3.3 are read on this pair as stated there.
- **(b) The hour-2 transient at 58 stations across DOY 128 to 132.** The WTZZ00DEU in-run consumers of the ten arms of card MDAY and the 17 consumers of its census replays (`mdyge<doy>`, `mdyc<doy>`): 3D p50 by half hour over 00:00 to 03:00 UT. The transient is read as recurring on a day when the WTZZ00DEU p50 of 01:00 to 01:30 under C exceeds the GE value by at least 5.0 mm (bar G2 of card BDS305, applied to the half hour in which the excess sat); the 16-site paired median change in that half hour is read beside it. The result is the number of the five days on which it recurs.
- **(c) BeiDou beta angles, DOY 128 to 132.** The BeiDou satellites of the COD MGX final SP3 of each day (38 of them); beta from the SP3 positions (inertial orbit normal, Sun from the low-precision almanac series), computed independently of the producer, with `scripts/attitude_beta_check.py` as the cross-check; the count of satellite-epochs (each 5-minute node standing for ten 30 s epochs) with absolute beta below 4 degrees, by satellite, orbit type and day, and how many of them lie on epochs carried by the C wire of the day. Nominal yaw steering is wrong for an orbit-normal BeiDou satellite below that angle.
- **(d) BeiDou RAIM load and the BSHM00ISR L6I gaps.** `logs/network_raim.csv` of `_rap_bds_gec6`, `_rap_bds_gecs6`, `_rap_sbv_on6` and C6: rows naming a `C##` satellite, by satellite and by station (spiked, refloated, withheld); `logs/network_events.csv`: the per-signal resumptions at BSHM00ISR by satellite and signal; the station's RINEX: the L6I gaps (phase blank while L2I is present) by satellite and length.

### 3.6 Closing rule

Cards BDS and BDSB close when the arm, the replays, R2 and the four reads are done and read, and each item still open in card BDS section 6 and card BDSB section 2.5 is answered, returned as a register line, or triaged field-blocking or field-inert with data (standing invariant 3). Closing records that the question of the cards is answered; it does not depend on every bar passing, and a missed bar stays recorded as missed. The decision on arming C (arm it on the field golden, keep it as a separate lineage, or hold) is returned to the author with its evidence and its cost.
