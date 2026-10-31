# BDSB: the BDS card, continued (the independent check of the second-band admission)

**STATUS**

- **id:** BDSB
- **topic:** network-vehicle
- **status:** closed
- **summary:** Card BDS's continuation (that file is at the 60 KB bar after its section 9), from 2026-10-19. The second-band admission (`network.second_band_admission`, default off, ledger row UNVALIDATED, not armed) was re-run from fresh runs on the Release build of branch `fix/single-band` at `f300bda` (exe sha256 `1183ce58…`). Key off: 24 IDENTICAL against the parent side after the two declared run-end census lines (products, every trace, manifest and snapshot bytes identical before any reversal; products and traces also identical against `w3base`). Key on at station scope, 6 h field golden with BeiDou: no station rejected (2,160 in 720 epochs with the rule off), the heal (3,323 columns on 551 epochs) and the S-basis completion (427 directions on 299 epochs) silent, WTZZ00DEU within 0.6 mm of the subtraction arm GEC-s, 16-site census median change against GEC-s 0.0000 m with no site above 6 mm. 2026-10-31 (sections 3 to 5, pre-registered): posture C (GPS + Galileo + BeiDou, the second-band admission, the per-signal wire) on the current golden, 6 h, against the baseline pair on one exe. Every registered gate passes: 720 of 720 epochs, BeiDou clock at most 13.9 m, WTZZ00DEU within +6.7 mm, census -0.0058 m (5 better, 0 worse), users of the C wire without BeiDou -0.0003 and +0.0000 m, ten 24 h clause-days within +0.0000 m. A GPS-only user at ARHT00ATA diverges on the C wire on DOY 128 and 131; cost 2.7 times the epoch loop at 58 stations; four offline reads; every open item triaged. Cards BDS and BDSB closed; nothing armed.
- **source:** [BDS](BDS_beidou_third_constellation_2026-10-18.md) sections 8 and 9 (the rule, its development arms, the first identity gate and the 6 h grade); the request of 2026-10-19 to repeat the identity gate and the 6 h grade from fresh arms; the request of 2026-10-31 to close cards BDS and BDSB with the adoption instrument on the current golden (sections 3 to 5); [MDAY](MDAY_multiday_candidate_2026-10-19.md) and [BDS305](BDS305_beidou_305_stations_6h_2026-10-29.md) (the five days and the 305-station regime)
- **knobs / posture:** `network.second_band_admission` (default false), `network.second_band_scope` (`station` default), `network.second_band_min_sats` (default 1); the arms differ in this knob alone; not armed in either golden; section 3: posture C is `processing.active_systems` with BeiDou, `processing.signal_priorities.BeiDou` `["C2I","L2I","C6I","L6I"]`, `network.second_band_admission` and `network.n3c_wire_per_signal` added to the current field golden; none armed
- **dates in the text:** 2026-10-19, 2026-10-31
- **hashes / run dirs:** exe `gnss_processor_w3sbandv.exe` (copy of the Release build of `f300bda`, sha256 `1183ce584a938bdc6827db126d0b3f2def18839607a2906e80f236dca2d86655`); gate side `w3sbandv` under `gates/g5` and `gates/g7`; 6 h arms `_rap_sbv_{ge6,off6,on6}`, census keys `sbvge`, `sbvoff`, `sbvon` (runs root `D:/GnssProcessorSupport/runs_nom`); readouts in `D:/GnssProcessorSupport/gates/sband/` (`v_*` files); sections 3 to 5: exe `gnss_processor_bdsclose.exe` (copy of `gnss_processor_bofarm.exe`, sha256 `91143c81de9a95479aa08131c08945676d5e542dbd607e9a8ff6d1852d2d73d6`) and, for R2 only, `gnss_processor_mday.exe` (sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`); arms `_rap_bdsc_{c6,c6_sm,ge6r,ctl}`, `_rap_bdsc6tm_{ge,c}`, census keys `bdsc6{ge,c,cge,geg,cg}` and `bdsr2{cge,geg,cg}<doy>`; runs root `D:/GnssProcessorSupport/runs_nom`; tables and scripts `D:/GnssProcessorSupport/gates/bdsclose/`
- **gates (frozen at pre-registration):** card BDS section 4 and section 9.1 (identity gate against the parent side), section 9.2 (counters and consumer by hour), section 9.3 (census); section 3 (pre-registered 2026-10-31, commit `86a4a58`)

**Outcome.** Question: does posture C (GPS + Galileo + BeiDou on B1I + B3I, the second-band admission, the per-signal wire) pass the gates of card BDS on the current golden, and do users of the multi-day wires without BeiDou lose? Every registered gate passes on a 6 h pair of one exe: 720 of 720 epochs, BeiDou clocks at most 13.9 m, census -0.0058 m (5 better, 0 worse), users without BeiDou -0.0003 and +0.0000 m, ten 24 h clause-days within +0.0000 m. One GPS-only user (ARHT00ATA) diverges on the C wire on DOY 128 and 131; the cost is 2.7 times the epoch loop. Closed; arming is the author's (section 5).

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

## 4. 2026-10-31: results

Read after the pre-registration of section 3 (commit `86a4a58`). Exe, arms, keys and bars are those registered; the deviations are listed in 4.1. Tables and scripts: `D:/GnssProcessorSupport/gates/bdsclose/` (`out/`, `tools/`).

### 4.1 Runs, checks and deviations

- **Exe.** `gnss_processor_bdsclose.exe`, sha256 `91143c81de9a95479aa08131c08945676d5e542dbd607e9a8ff6d1852d2d73d6`, equal to `gnss_processor_bofarm.exe`; nothing was built. The R2 replays ran on `gnss_processor_mday.exe` (sha256 `3d6c679e...`) only.
- **Smoke** (`bdsc_c6_sm`, 10 epochs): exit 0 in 99.9 s, 10 of 10 emitted, 58 stations, 38 `C##` satellites with clock, code-bias and phase-bias lines; BeiDou withheld at WARK00NZL, MAJU00MHL and SCTB00ATA for the run and at MAC100AUS in its first epoch.
- **6 h arms**, started together 2026-10-30 22:33 PDT: C6 exit 0 after 6,581.6 s; the control re-run exit 0 after 3,631.2 s (the baseline run took 1,343.5 s). The re-run is byte-identical to the baseline run (`products.txt`, 11,345,974 bytes, sha256 `bf16e947...`, and `closed_loop_trace.csv`; `out/ge6r_identity.txt`): the exe copy reproduces it.
- **B0.** The diff of `_rap_bdsc_c6/arm.json` against the control arm is the five leaves of 3.2 (`out/b0_c6_armdiff.txt`). Each of the 340 census replays (5 sets of 17 on the 6 h arms, 15 in R2) differs from its base in the registered wire, consumer and output paths only, and for DOY 132 in the two duration keys (`out/b0_replays.txt`).
- **Machine.** Shared throughout: the minute samples give a median of 15 other `gnss_processor` processes (2 to 29), a CPU load of 100 % and at least 22 GB free. Wall times are not controlled.
- **Deviations from 3.** (1) This track's replays ran at AboveNormal priority from 23:19 and C6 from its epoch 370 (23:49), since at normal priority C6 advanced 2 to 4 epochs per minute; runs are deterministic, so only wall times depend on it. (2) S6 gained a controlled part, a 60-epoch pair at equal priority (4.6), because the registered pair could not be read as a ratio. (3) The R2 replays ran as three chains of one script at 3 jobs each.

### 4.2 S1, the health of C6 (720 epochs)

| gate | reading | verdict |
| --- | --- | --- |
| H1 | exit 0; 720 of 720 epochs emitted (of 723 synced), 0 skipped, 0 withheld by the observability gate or the A4 ceiling; the rank gate evaluated 720 epochs at 1e-11 with 0 withheld UNDER-specified and 0 OVER-specified, the observed near-null count 3 to 3 against the declared 3; stations in the solve 57 / 58 / 58 (min / median / max), 0 addStation rejections, the S-basis completion and the heal silent (0 directions) | PASS |
| H2 | BeiDou `c0`: median 3.843 m, p99 9.844 m, maximum 13.880 m (C20, epoch 159, TOW 263970, the step of card BDS section 5.1); 0 above 50 m, 0 above 1 km | PASS |
| H3 | GPS and Galileo clocks against the control wire, same 720 epochs: datum-free rms of the change at most 0.0532 m (GPS, median 0.0498) and 0.0824 m (Galileo, median 0.0737); constellation-common shift at most 0.087 m (GPS) and 0.053 m (Galileo); maximum absolute `c0` 2.507 m (GPS) and 4.412 m (Galileo), the control wire 2.583 and 4.424 m | PASS |
| H4 | 38 `C##` satellites at every epoch (min 38) each with a clock line, code-bias lines and phase-bias lines. The second-band admission is not inert: 721 epochs screened 999,978 records in 78,587 station-constellation-epochs, 12,488 records left alone (HKSL00HKG, B1I spelled C1I / L1I), 30,113 records withheld in 2,165 unit-epochs, BeiDou at BSHM00ISR, MAC100AUS, MAJU00MHL, SCTB00ATA and WARK00NZL, 5 withhold and 2 readmit edges. BeiDou event counts (satellite named): 1,745 per-signal resumptions, 248 re-entry re-levels, 391 MDB exceedances, 127 T4 arc attributions, 262 retro bumps, 258 gross-LOS emit discontinuities, 3 clock-anchor decouplings; RAIM rows 366 (242 spiked, 202 refloated) against 102 Galileo and 106 GPS. Zero cases: no `GATE`-tier event; the measurement admission rejected 2 channels at birth and 4 onsets, all GPS at BSHM00ISR (G17, G22, G13, G15, G29), none BeiDou | PASS |

Against the older exe's rule-on arm (`_rap_sbv_on6`, card BDSB section 2.3) the rule's records and edges (30,113; 5 / 2), the maximum clock (13.88 m) and five of the eight BeiDou counts above are equal; the other three differ by one: MDB exceedances 391 against 392, T4 attributions 127 against 126 and RAIM rows 366 against 367 (242 against 243 spiked).

### 4.3 S2, the held-out consumer WTZZ00DEU

3D p50 by hour (m) from 00:00 UT; `out/s2.txt`.

| consumer | h1 | h2 | h3 | h4 | h5 | h6 | below 0.1 m of 720 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| control (GPS + Galileo; the re-run is identical) | 0.059 | 0.012 | 0.023 | 0.012 | 0.017 | 0.012 | 688 |
| C6 in-run, own systems (GPS + Galileo + BeiDou), row 1 | 0.034 | 0.013 | 0.019 | 0.019 | 0.019 | 0.016 | 687 |
| C6 minus control (mm) | -24.5 | +1.1 | -3.7 | +6.7 | +2.5 | +3.9 | |
| GE consumer of the C6 wire (replay `bdsc6cge_wtzz`), row 2 | 0.058 | 0.014 | 0.028 | 0.013 | 0.016 | 0.011 | 683 |
| row 2 minus control (mm) | -1.3 | +1.3 | +4.7 | +0.9 | -0.2 | -0.9 | |

Row 1, C6 minus control, is at most +6.7 mm in hours 2 to 6 (hour 4) against the bar of +10.0 mm: **PASS**. The census replay `bdsc6c_wtzz` equals the in-run consumer. In the half hour 01:00 to 01:30 (the one of card BDS305 and 4.8(b)) the control reads 10.1 mm and C6 17.3 mm (+7.2 mm); in 00:30 to 01:00 C6 is 12.1 mm better and in 01:30 to 02:00 2.3 mm better, so hour 2 reads +1.1 mm.

### 4.4 S4, the census, C6 against the control

Each site's mean hourly p50 over hours 2 to 6, C6 consumers on the C6 wire minus GE consumers on the control wire (`out/s4.txt`; `gcensus.py grade` gives the same medians, counts and rows, `out/s4_gcensus_crosscheck.txt`).

| quantity | value |
| --- | --- |
| paired median, hours 2 to 6 | **-0.0058 m**, 5 better, 0 worse at 1 cm (bar: at most 0 m, more better than worse: **PASS**) |
| sites worse than the control by more than 0.05 m | none; the largest worsening is UTQI00USA +0.0003 m |
| hour 1, paired median | -0.0522 m, 10 better, 1 worse (CEDU00AUS +0.0371 m) |
| median site p50 by hour (m), control / C6 | 0.142 0.050 0.036 0.029 0.032 0.030 / 0.098 0.031 0.032 0.029 0.025 0.021 |
| the ten sites with the B1I + B3I pair | -0.0111 m (5 better, 0 worse); hour 1 -0.0725 m |
| the two with B1I and no B3I (CHTI00NZL, SCRZ00BOL) | -0.0027 m; hour 1 -0.1694 m |
| the four without a usable pair (ANMG00MYS, BADG00RUS, UTQI00USA, BAKE00CAN) | -0.0003 m; hour 1 -0.0026 m |

The gains are ARHT00ATA -0.2021 m (the control averages 0.23 m at that site in hours 2 to 6, C6 0.03 m), DAV100ATA -0.0643, POHN00FSM -0.0264, WUH200CHN -0.0202, CEDU00AUS -0.0134, KOKB00USA -0.0089, HARB00ZAF -0.0074, STPM00SPM -0.0060, QUIN00USA -0.0056 and SCRZ00BOL -0.0047 m. The sites that cannot use BeiDou change by at most 0.0018 m (BAKE00CAN). The consumer WTZZ00DEU is +2.1 mm over hours 2 to 6 (one instrument, S2).

### 4.5 S5, users of the C6 wire

Paired change of each site's mean hourly p50 over hours 2 to 6 (`out/s5a.txt`, `out/s5b.txt`; `gcensus.py grade` gives the same medians and counts, `out/s5a_gcensus_crosscheck.txt`, `out/s5b_gcensus_crosscheck.txt`).

| clause | users of the C6 wire against the control wire | paired median | better / worse | hour 1 median | largest worsening | largest gain | worse than +0.05 m | bar (at most +0.005 m) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | GPS + Galileo (`bdsc6cge` against `bdsc6ge`) | -0.0003 m | 1 / 0 | -0.0049 m | DAV100ATA +0.0032 m | ARHT00ATA -0.2146 m | none | **PASS** |
| 2 | GPS-only (`bdsc6cg` against `bdsc6geg`) | +0.0000 m | 2 / 1 | -0.0004 m | WUH200CHN +0.0194 m | ARHT00ATA -0.5406 m | none | **PASS** |

WTZZ00DEU changes by +1.2 mm (clause 1) and 0.0 mm (clause 2) over hours 2 to 6. The users without BeiDou lose nothing at the median; the one site that is worse by more than 1 cm is WUH200CHN as a GPS-only user (hours 2 to 6 p50 0.074 0.103 0.126 0.098 0.053 m on the control wire, 0.075 0.119 0.149 0.133 0.075 m on the C6 wire).

**What reaches users who do not use BeiDou.** ARHT00ATA, the largest gain of S4, reaches them in full: a GPS + Galileo user of the C6 wire reads 0.044 0.020 0.021 0.017 0.013 0.009 m by hour against 0.301 0.374 0.210 0.194 0.129 0.246 m on the control wire (-0.2146 m over hours 2 to 6, more than the BeiDou user's -0.2021 m), a GPS-only user -0.5406 m. QUIN00USA is mostly a wire effect (BeiDou user -0.0056 m, GPS + Galileo user -0.0043 m, GPS-only user -0.0439 m). The other gains belong to the consumer's own BeiDou; BeiDou user against GPS + Galileo user of the same wire: DAV100ATA -0.0643 against +0.0032 m, POHN00FSM -0.0264 against -0.0006, WUH200CHN -0.0202 against +0.0016, CEDU00AUS -0.0134 against -0.0006, KOKB00USA -0.0089 against +0.0002, STPM00SPM -0.0060 against +0.0005, SCRZ00BOL -0.0047 against -0.0001. The S4 median (-0.0058 m) needs the consumer's BeiDou (-0.0003 m without it).

### 4.6 S6 and S7, cost and the BeiDou wire

**S6.** The registered same-load pair gave no ratio: the load rose from 8 foreign processes in the first 35 minutes to 28 in the last 40, the control re-run ended in 3,631 s with C6 in its third hour of data, and C6 ran at raised priority from epoch 370. Epoch-loop ms per epoch: control re-run 4,922, C6 9,025, baseline run 1,785 (`out/s6.txt`). The controlled pair is read instead: the control arm and C6 for the first 60 epochs, started together on the same exe at the same (AboveNormal) priority (`bdsc6tm_ge`, `bdsc6tm_c`; `out/s6_timing_pair.txt`).

| quantity | control | C6 | C6 / control |
| --- | --- | --- | --- |
| epoch-loop ms per epoch (60 epochs) | 1,768 | 4,811 | 2.72 |
| per-epoch total, mean / median (s), epochs 21 to 60 | 1.77 / 1.53 | 4.84 / 4.12 | 2.73 / 2.69 |
| process wall (s) | 153.8 | 336.3 | 2.19 |
| processor time (s) | 255.5 | 505.6 | 1.98 |

C6 costs 2.7 times the control's epoch loop and 2.0 times its processor time at 58 stations over the first half hour (ignition epochs included). On record: 1.44 times the epoch-loop wall per epoch at 305 stations over 6 h, with about 1 GB more memory (card BDS305); 1.45 to 2.04 times the process wall over five 24 h days at 58 stations (card MDAY). None is a measurement on an idle machine.

**S7.** Datum-free rms of `c0` by hour (m), the deviation from the a priori product and not a truth (`out/s1_c6_wire.txt`):

| constellation | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| BeiDou | 0.85 | 1.48 | 1.52 | 1.32 | 1.18 | 1.07 |
| Galileo | 0.64 | 0.41 | 0.18 | 0.14 | 0.12 | 0.11 |
| GPS | 0.81 | 0.80 | 0.81 | 0.80 | 0.81 | 0.85 |

38 `C##` satellites per epoch (median and minimum). BeiDou-only consumer of the C6 wire at WTZZ00DEU (`bdsc6bds_wtzz`, `processing.active_systems` `["BeiDou"]`; extra, not gated): 0.156 0.072 0.038 0.042 0.040 0.027 m by hour, 620 of 720 epochs below a decimetre (card BDS section 5.2 on the older exe: 0.154 0.075 0.041 0.047 0.042 0.026 m, 621). The BeiDou rows of the run-end tables are the counts of H4 above.

### 4.7 R2, users of the multi-day C wires

Fifteen replay sets of 17 consumers on `gnss_processor_mday.exe`: GPS + Galileo users (R2a, `bdsr2cge<doy>` against the census `mdyge<doy>` of card MDAY) and GPS-only users (R2b, `bdsr2cg<doy>` against `bdsr2geg<doy>`) of the C wire against the same users of the GE wire of the day; paired change of each of the 16 sites' mean hourly p50 over hours 2 to 24 (`out/r2_all.txt`, `out/r2_all_tables.md`).

| DOY | clause | paired median, hours 2 to 24 (m) | better / worse at 1 cm | largest worsening | largest gain | worse than +0.05 m | bar |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | R2a | -0.0001 | 1 / 0 | WUH200CHN +0.0018 | ARHT00ATA -0.0937 | none | pass |
| 128 | R2b | +0.0000 | 2 / 1 | ARHT00ATA +17.2679 | WUH200CHN -0.0873 | ARHT00ATA +17.27 | pass |
| 129 | R2a | -0.0001 | 1 / 0 | DAV100ATA +0.0008 | ARHT00ATA -0.1281 | none | pass |
| 129 | R2b | -0.0000 | 2 / 0 | WUH200CHN +0.0027 | ARHT00ATA -0.2214 | none | pass |
| 130 | R2a | -0.0003 | 1 / 0 | DAV100ATA +0.0063 | ARHT00ATA -0.1182 | none | pass |
| 130 | R2b | +0.0000 | 2 / 0 | WUH200CHN +0.0080 | ARHT00ATA -0.3018 | none | pass |
| 131 | R2a | -0.0000 | 0 / 0 | DAV100ATA +0.0035 | QUIN00USA -0.0013 | none | pass |
| 131 | R2b | +0.0000 | 1 / 1 | ARHT00ATA +14.9114 | QUIN00USA -0.0141 | ARHT00ATA +14.91 | pass |
| 132 | R2a | +0.0000 | 1 / 0 | HARB00ZAF +0.0004 | ARHT00ATA -0.0765 | none | pass |
| 132 | R2b | -0.0000 | 1 / 0 | DAV100ATA +0.0017 | ARHT00ATA -0.2015 | none | pass |

**The registered bar holds on all ten clause-days** (paired median -0.0003 to +0.0000 m against at most +0.005 m). The median site reads lower with the C wire in seven rows, 0.1 mm higher in two (GPS-only users, DOY 128 and 131) and 1.1 mm higher in one (GPS + Galileo users, DOY 131). Sites worse than +0.05 m, named: **ARHT00ATA as a GPS-only user of the C wire, +17.27 m on DOY 128 and +14.91 m on DOY 131.** No other site is worse than +0.0080 m in any row (WUH200CHN +0.0080 m and DAV100ATA +0.0071 m as GPS-only users on DOY 130, DAV100ATA +0.0063 m as a GPS + Galileo user the same day).

**The ARHT00ATA divergence of the GPS-only user** (`bdsr2cg128_arht`, `bdsr2cg131_arht`). On DOY 128 the consumer is 189 m off at 03:36:30 and stays above 1 m for 487 epochs until 07:39:30 (maximum 194 m; hourly p50 129, 98, 89 and 85 m in hours 5 to 8); on DOY 131 it is 176 m off at 03:23:00 and above 1 m for 679 epochs until 09:02:00 (hourly p50 120, 85, 59, 47 and 28 m in hours 4 to 8). On the GE wire the same consumer has no epoch above 1 m after the first hour on DOY 131 and none above 1.3 m on DOY 128 (01:00 to 01:26). Each episode starts at the epoch after one in which both wires carry G01 about 100 m from the product: 03:35:59 on DOY 128 (-110.7 m on the GE wire, -109.7 m on the C wire) and 03:22:30 on DOY 131 (-102.8 and -101.6 m), the first of the three epochs a day on which the wire carries G01 (card MDAY section 9 item 2). The GPS + Galileo user of the C wire and the BeiDou user do not diverge (neither leaves 1 m after the first hour on either day). The wire defect, G01 emitted at three epochs of a day, is common to both postures; this GPS-only consumer survives it on the GE wire and not on the C wire. The two days are those of the ARHT00ATA divergences of card MDR (99.7 m on DOY 128, 166 m on DOY 131, under the earlier GPS-only golden), which card MDAY found absent for the GPS + Galileo and BeiDou users. On the other three days the GPS-only user gains from the C wire (ARHT00ATA -0.2214, -0.3018 and -0.2015 m on DOY 129, 130 and 132), and the GPS + Galileo user on four days (-0.0937, -0.1281, -0.1182 and -0.0765 m on DOY 128, 129, 130 and 132; -0.0004 m on DOY 131).

**DOY 130 after 10:46:00 UT** (the read of 3.4; card MDAY section 6.1). Over 10:46:00 to 24:00 (1,587 epochs per wire) the G03 clock `c0` has median 2.32 m (2.27 to 4.14 m) on the GE wire and -9.79 m (-11.45 to -8.99 m) on the C wire. The users do not see it: GPS + Galileo users, 16-site paired median -0.0005 m, DAV100ATA +0.0144 m the only site worse than +0.01 m, epochs above 0.2 m UTQI00USA 3 (GE wire) and 4 (C wire), ARHT00ATA 163 and 0; GPS-only users, +0.0001 m, no site worse than +0.01 m, epochs above 0.2 m ANMG00MYS 2 and 2, POHN00FSM 2 and 2, DAV100ATA 70 and 96, UTQI00USA 195 and 195, ARHT00ATA 929 and 0; no consumer above 1 m under either wire; WTZZ00DEU 14.9 and 14.3 mm (GPS + Galileo), 23.3 and 18.5 mm (GPS-only). No paired median above +0.005 m and no consumer above 1 m under the C wire alone: no effect of the G03 level on users at this census; DAV100ATA reads worse after 10:46 under both clauses.

### 4.8 Offline reads of section 3.5 (no producer run; tables in `gates/bdsclose/out/`, scripts in `gates/bdsclose/tools/`)

**(a) The BeiDou clock class on the BDS305 wires** (`wire_read.py`; `_rap_bds305_c_f6` against `_rap_bds305_ge_f6`, 305 stations, 6 h; `a_bds305_wire.txt`).

| quantity | GPS | Galileo | BeiDou |
| --- | --- | --- | --- |
| satellites per epoch with clock, code-bias and phase-bias lines (median; min to max) | 31 (30 to 32) | 24 (24 to 25) | 38 (38 to 38) |
| absolute `c0` on the C wire: median / p99 / maximum (m) | 0.688 / 2.176 / 2.309 (G03) | 0.199 / 0.548 / 1.999 (E24) | 3.806 / 4.780 / 5.224 (C11) |
| above 50 m / above 1 km | 0 / 0 | 0 / 0 | 0 / 0 |
| datum-free rms of `c0` by hour (m) | 0.82 0.84 0.84 0.81 0.82 0.85 | 0.14 0.27 0.21 0.17 0.18 0.24 | 0.78 0.77 0.77 0.77 0.78 0.78 |
| change of the clock, C against GE: datum-free rms, maximum / median (m); common shift, maximum (m) | 0.0254 / 0.0223; 0.049 | 0.0341 / 0.0205; 0.045 | |

H2 reads 5.2 m against 50 m and H3 0.025 and 0.034 m against 1.0 m: at 305 stations on the current posture the BeiDou clocks are metre class from the first epoch and the leak into GPS and Galileo is 1 to 3 cm. The 2 h pair of card MDAY section 10 reads the same (BeiDou maximum 5.22 m, changes 0.024 and 0.034 m; `a_bof305_2h_wire.txt`). Failure mode A (BeiDou clocks of 84 km mean and 530 km maximum, GPS and Galileo displaced 3,200 and 6,100 times the baseline) is absent at 58 stations (every 6 h and 24 h run of cards BDS, BDSB, MDAY and 4.2: BeiDou maximum 14.7 m) and at 305 stations. It was recorded for a W6-off configuration of 2026-06-29 that the goldens do not run; that regime is not rerun and the cause was never found.

**(b) The hour-2 transient at 58 stations across DOY 128 to 132** (`half_hour.py`; the ten arms of card MDAY and the 17 consumers of `mdyge<doy>` and `mdyc<doy>`; `b_half_hour.txt`). WTZZ00DEU 3D p50 (mm) in 01:00 to 01:30, with the 16-site paired median change of the same half hour:

| DOY | GE | C | C minus GE | recurs (at least +5.0 mm) | 16 sites, C minus GE (mm); better / worse at 1 cm |
| --- | --- | --- | --- | --- | --- |
| 128 | 26.5 | 12.9 | -13.6 | no | -16.5; 10 / 0 |
| 129 | 8.9 | 16.2 | +7.4 | yes | -20.1; 10 / 1 |
| 130 | 8.8 | 11.0 | +2.1 | no | -4.7; 7 / 2 |
| 131 | 28.7 | 20.0 | -8.7 | no | -4.5; 5 / 1 |
| 132 | 18.8 | 18.6 | -0.2 | no | -11.8; 8 / 0 |

The transient recurs on one of the five days, DOY 129, the day card BDS305 read it on at 305 stations; on the other four C is level with or better than GE at WTZZ00DEU in that half hour, and at the 16 sites C is better in it on every day. The new 6 h draw of DOY 129 on the current golden (4.3) reads the same: control 10.1 mm, C6 17.3 mm, +7.2 mm. The in-run consumer equals its census replay in every half hour of all ten arms.

**(c) BeiDou beta angles, DOY 128 to 132** (`beta_bds.py`, `beta_forward.py`; `c_beta.txt`). Of 546,820 satellite-epochs (38 satellites on 2,880, 2,880, 2,880, 2,880 and 2,870 wire epochs) none has absolute beta below 4 degrees. The closest approach is the IGSO plane nearest the Sun: C13 at 9.0, 8.3, 7.6, 6.9 and 6.2 degrees on DOY 128 to 132, C38 at 9.3 to 6.6, C08 at 9.9 to 7.0 (about 0.7 degree per day toward zero); C10 and C07 are at 21 to 24 degrees, the other IGSO satellites at 30 degrees or more, the 28 MEO satellites at 22 degrees or more (C46, 22.3 to 26.2). Three routes agree: the SP3 route written for this read, `attitude_beta_check.py` on the same SP3 (largest difference 0.001 degree) and the broadcast inclination and node of the navigation file, which needs neither SP3 nor velocity (0.003 degree). Nominal yaw steering is not wrong for any BeiDou satellite on these days. With the plane held fixed (a model: the J2 regression of an IGSO orbit is 0.008 degree per day against 0.986 for the Sun; `c_beta_forward.txt`), C13, C38 and C08 enter the band on DOY 137 to 138 (2024-05-16 to 17) and leave it about DOY 148 to 149; C10 and C07 reach it near DOY 157 to 158.

**(d) BeiDou RAIM load and the BSHM00ISR L6I gaps** (`raim_bds.py`; `_rap_bds_gec6`, `_rap_bds_gecs6`, `_rap_sbv_on6`; `d_raim.txt`).

- **RAIM rows** (rows / spiked / refloated; 6 h, 58 stations): GEC-s and rule on (identical) C 367 / 243 / 202, E 103 / 89 / 21 (102 / 88 / 21 rule on), G 106 / 96 / 13; GEC without the admission C 415 / 251 / 251, E 124 / 110 / 25, G 105 / 96 / 12. The BeiDou rows sit on 35 of 38 satellites (C07 28, C10 27, C20 24, C19 22, C08 21, C06 21, C16 20, C13 18; the top three hold 22 %) and 36 stations (YKRO00CIV 46, every row spiked and none refloated; SGOC00LKA 30, KIT300UZB 28, PARK00AUS 28, GUAM00GUM 23, MAC100AUS 22, BSHM00ISR 18, GAMB00PYF 18).
- **Where the refloats are.** All 202 refloated rows lie in three epochs of DOY 129: 01:14:30 (172 rows, 15 satellites, each spiked at 3 to 7 stations at once: C06 to C10, C13, C16, C19, C20, C36 to C39, C46, C48), 01:18:30 (23 rows: C08, C19, C20) and 01:19:00 (7 rows: C20). The first epoch also carries a `CLK_ANCHOR` decoupling of C20 and a re-entry re-level of C20 at DARW00AUS after a 7-epoch L2I gap with a loss-of-lock flag. The other 165 rows are single-station outliers, not refloated. The 305-station 6 h arm has no epoch with three BeiDou satellites at once; the five C wires of card MDAY have such epochs on every day (DOY 128 at 19:29:30 to 19:31:00; DOY 129 at 01:14:30, 01:18:30 and 23:40:30; DOY 130 at 09:42:00 to 09:44:00 with the G03 event and seven later epochs; DOY 131 at 14:09:00 to 14:11:00; DOY 132 at 20:15:00).
- **BSHM00ISR.** 1,276 of the 1,756 BeiDou per-signal resumptions of the GEC-s run (C6 reads 1,745 in all) are at BSHM00ISR (YKRO00CIV 268, KIT300UZB 41, MAC100AUS 35), 1,271 of them on L6I; 840 follow a one-epoch gap, the median gap is one epoch, the longest 10. The station's RINEX gives the cause: B3I code is tracked where the B3I phase is not (over the satellites with an orbit, B3I phase on 6,717 of 9,804 code satellite-epochs, 0.69; B1I 0.81; B2I 0.93), so L6I is blank between two epochs with L6I 1,775 times in the first 720 epochs on those satellites (2,155 gaps over 31 satellites in all, 1,331 of one epoch). The response is wired and acts: 1,271 resumptions, 154 re-levels and 394 transitions to code-only rows, and the counts agree where the arc is modelled (C10 146 gaps and 146 events, C24 85 and 85, C25 73 and 73, C12 58 and 58; C40 154 and 80, C06 102 and 0). The station carries 18 of the 367 BeiDou RAIM rows. The gaps are a receiver property, not a producer defect.

### 4.9 Triage of the open items (standing invariant 3)

Items open in card BDS section 6, card BDSB section 2.5, card MDAY section 8 and card BDS305, each field-blocking or field-inert with its data, or answered.

| item | class | data |
| --- | --- | --- |
| Warm resume of the second-band admission | field-inert by scope | The withheld memory (`StationState::sba_since`) is ColdStart and not snapshotted; the verdict is recomputed from the data every epoch, so a resume differs only by repeating one decoupling edge per withheld unit (BDS 8.4). The goldens run no snapshot and no resume (the manifest exception for `network.serialize_disc_counters`; the `arm.json` of C6 has no snapshot or resume key). A resume of a snapshot written without the rule into a run with it was not exercised. Register line |
| Satellite scope (`network.second_band_scope` `satellite`) | research-only, not field-runnable | No hysteresis: 1,698 withhold and 1,413 readmit edges in 240 epochs, 20,536 record-epochs withheld, the longest run 200 epochs (BDS 8.3). Posture C uses station scope, the loader default (C6 has no scope key). It needs a tolerance in epochs, the form of the canonical-pair admission. Register line |
| B1C + B2a and B2b | a different configuration | Triple or all mode, which changes GPS and Galileo as well (BDS 2); the BeiDou entry of posture C names B1I + B3I only. Tracked at 21 and 25 of 58 stations against 48, 163,279 and 141,595 pair satellite-epochs against 586,226 (BDS 2) |
| Code-bias admission, Kaula prior, rank-gate placement (failure mode B) | not needed at 58 or 305 stations | The piece rule of BDS section 3 never fired: no epoch withheld by the rank gate on any C arm, 58 stations (C6; the 6 h arms of BDS and BDSB; five 24 h days of MDAY) and 305 stations (BDS305; MDAY section 10), the observed near-null count at the declared 3 throughout. `network.cb_admission` and `network.cb_kaula_prior` stay KEEP_OFF |
| Failure mode A, root cause | not reproducible on the current posture | 4.8(a): BeiDou absolute `c0` at most 5.2 m at 305 stations and 14.7 m at 58, GPS and Galileo displaced by at most 0.13 m. The regime that showed it (W6 off, 305 stations, the older posture, 2026-06-29) is not a golden regime; the cause was never found and is not owed. Register line: unexplained history |
| BeiDou attitude (orbit-normal mode at low beta) | field-inert on the registered days; field-blocking outside them | 0 of 546,820 satellite-epochs below 4 degrees on DOY 128 to 132, closest 6.2 degrees; three IGSO satellites are extrapolated into the band for about 12 days from DOY 137. `processing.enable_orbit_normal_attitude` is off and ungraded for BeiDou. The goldens' window (DOY 129, 00:00 to 06:00 UT) is not affected; a run on days with a plane within 4 degrees of the Sun needs it graded first |
| BeiDou RAIM load; BSHM00ISR L6I gaps | answered | 4.8(d): the load is spread over 35 satellites and 36 stations, the refloats fall in three epochs of DOY 129 at 58 stations and none at 305; the L6I gaps are a receiver property answered by the gap re-entry response |
| The hour-2 transient of BDS305 | answered; no settled cost | 4.8(b): one of five days at 58 stations, C better at the 16 sites in that half hour on all five. The satellite or term that carries it on DOY 129 stays unnamed (single-satellite exclusion replays of the consumer, owed by BDS305). Register line |
| Users of the C wire without BeiDou; G03 on DOY 130 after 10:46 (MDAY 8) | answered | S5 and R2 (4.5, 4.7); the GPS-only divergence at ARHT00ATA on DOY 128 and 131 is a register line (the G01 epochs of the wire) |
| A same-exe 6 h GEC-s; a controlled cost (BDS 6) | answered | C6 and its control are same-exe pairs on the current golden; the controlled cost pair is 4.6 |

### 4.10 Verdicts against the registered bars, and the closing of cards BDS and BDSB

| gate | bar (3.3, 3.4) | reading | verdict |
| --- | --- | --- | --- |
| B0 | the registered keys and paths only | C6: the four keys and the output path; 340 replays: the registered paths; exe sha256 as registered | PASS |
| S1 | H1 to H4 over 720 epochs | 720 of 720 emitted, rank gate 0 UNDER and 0 OVER; BeiDou `c0` at most 13.88 m; GPS and Galileo change at most 0.053 and 0.082 m; 38 `C##` per epoch, admission not inert | PASS |
| S2 | C6 minus control at most +0.010 m in each of hours 2 to 6 | at most +6.7 mm (hour 4) | PASS |
| S4 | paired median at most 0 m, more better than worse, no site worse by 0.05 m unnamed | -0.0058 m, 5 better and 0 worse, no site worse than +0.0003 m | PASS |
| S5 | each clause at most +0.005 m, sites worse by 0.05 m named | -0.0003 m (GPS + Galileo users), +0.0000 m (GPS-only users); none named | PASS (both) |
| S6, S7 | read | 2.7 times the control's epoch loop and 2.0 times its processor time at 58 stations; BeiDou wire metre class; BeiDou-only consumer 0.156 0.072 0.038 0.042 0.040 0.027 m | read |
| R2 | per clause and day at most +0.005 m, sites worse by 0.05 m named | 10 of 10 clause-days (-0.0003 to +0.0000 m); named: ARHT00ATA as a GPS-only user, DOY 128 (+17.27 m) and 131 (+14.91 m) | PASS, two sites named |
| R2 read | DOY 130 after 10:46:00 UT | no paired median above +0.005 m, no consumer above 1 m under the C wire alone; DAV100ATA +0.0144 m | read |

No bar was missed. Two readings against the C wire stand beside the passes: the GPS-only divergence at ARHT00ATA on DOY 128 and 131 (4.7), and WTZZ00DEU's +7.2 mm in the half hour 01:00 to 01:30 of DOY 129 (4.3; hour 2 reads +1.1 mm and the S2 bar holds).

**Closing.** Every open item of card BDS section 6 and card BDSB section 2.5 is answered, returned as a register line or triaged with data (4.9), and the four reads and R2 are done and read. Cards BDS and BDSB are closed. Arming C is not done here; the decision is below.

## 5. 2026-10-31: the arming decision (the author's; evidence, cost, weak sites, prerequisites)

Three courses: **A** arm C on the field golden; **B** keep C as a separate third lineage (the field golden plus the four posture keys, generated from the manifest, the primary golden unchanged); **C** hold (rows stay UNVALIDATED, the evidence stays on file). Nothing is armed by this card.

**Evidence for A or B.** Every registered gate passes on the current golden (4.10). BeiDou users: census -0.0058 m over hours 2 to 6, 5 better and 0 worse, hour 1 -0.0522 m (4.4); five 24 h days on the earlier golden -0.0008 to -0.0038 m (card MDAY). Users without BeiDou lose nothing at the median (-0.0003 and +0.0000 m on the 6 h arm; -0.0003 to +0.0000 m on ten 24 h clause-days), and the largest single gain, ARHT00ATA, reaches them through the wire (-0.21 m for a GPS + Galileo user, -0.54 m for a GPS-only user on the 6 h arm). The BeiDou clocks are metre class at 58 and 305 stations and failure mode A is absent (4.8a).

**Cost on record.** At 58 stations, 2.7 times the control's epoch loop and 2.0 times its processor time over the first 60 epochs at equal priority (4.6); 1.45 to 2.04 times the process wall over five 24 h days (card MDAY); 1.44 times the epoch loop with about 1 GB more memory at 305 stations (card BDS305). All are on a loaded machine. Under A every field arm of every later card costs that factor; the sim bed cannot follow (its fixture holds no BeiDou observations), so the two beds would differ by a constellation and not by a knob, which the two-bed bisection of the standing directive does not expect.

**Weak sites.** (1) ARHT00ATA as a GPS-only user of the C wire diverges on DOY 128 and 131 (4.1 and 5.7 hours, up to 194 m) at the epoch where both wires carry G01 about 100 m off; the GE wire's user survives it. The wire defect is common to both postures and is not C's, but it is exposed here. (2) DAV100ATA reads worse for the users without BeiDou on DOY 130 (+0.0063 and +0.0071 m over hours 2 to 24, +0.0144 m after 10:46 UT) and +0.0032 m in the 6 h read; its gain as a BeiDou user is the second largest (-0.0643 m). (3) WUH200CHN as a GPS-only user, +0.0194 m (6 h) and +0.0080 m (DOY 130). (4) CEDU00AUS in hour 1, +0.0371 m. (5) WTZZ00DEU, +2.1 mm over hours 2 to 6 (+6.7 mm in hour 4) and +7.2 mm in 01:00 to 01:30 of DOY 129, which recurs on one of five days. (6) The producer station BSHM00ISR, whose B3I phase drops out (4.8d). (7) The gain at ARHT00ATA has an unread mechanism: over hours 2 to 6 the control wire gives that station 0.23 m to a GPS + Galileo user and 0.59 m to a GPS-only user, and the C6 wire 0.016 m and 0.052 m, so part of what C buys may be a repair of the GPS + Galileo products near that station that BeiDou observations mask.

**What a yes needs.** (a) The regime predicate `gps_gal_only` excludes BeiDou: its definitions are `src/core/config/GnssConfigNetworkPosture.cpp` (lines 74 to 77), `scripts/config_posture_check.py` (line 213) and the comment at `include/config/GnssConfig.hpp` line 8482, and nine ledger rows carry it (`network.estimate_sat_code_bias`, `network.code_datum_family_difference`, `network.code_family_birth_prior_sigma_m`, `network.sbasis_complete`, `estimation.clock_disc_isb_reinflate_std`, `model_corrections.enable_receiver_cross_system_atx_fallback`, `processing.active_systems`, `processing.enable_noon_turn_attitude`, `processing.signal_priorities.Galileo`); with BeiDou in `active_systems` all nine go out of regime, so the loader stops warning on them and the generator stops requiring them, and the predicate has to admit BeiDou in the three places. (b) A new ledger row for `processing.signal_priorities.BeiDou` (none exists), the `armed_when` of `processing.active_systems` widened, and `network.second_band_admission` and `network.n3c_wire_per_signal` moved from UNVALIDATED to a recommended verdict with a regime. (c) Manifest pins for the four keys on the field golden and the same four as documented exceptions on the sim golden. (d) A fresh baseline pair: the field arm is C6's configuration on the regenerated golden (`runs/_rf0hv18_field6`, same exe), the sim pair is unchanged. (e) `processing.enable_orbit_normal_attitude` graded for BeiDou before a run on days when an IGSO plane is within 4 degrees of the Sun (C13, C38 and C08 from about DOY 137). (f) The golden paragraph of `CLAUDE.md`. Course B needs (b), (c) as a third manifest entry and (e), takes C6 as its baseline, and leaves (a) and (d) untouched.

**Recommendation (the writer's).** Course B now, and course A only after the mechanism of the ARHT00ATA wire gain is named and the G01 epoch of DOY 128 and 131 is no longer on the wire: the gates pass, but the median gain is 6 mm at BeiDou users and 0.3 mm or less at the rest, the largest effect may not need BeiDou, and A would double or more the cost of every field arm and split the two beds. A is supportable on the gates alone if the author wants BeiDou products in the golden now, at the cost and with the prerequisites above.
