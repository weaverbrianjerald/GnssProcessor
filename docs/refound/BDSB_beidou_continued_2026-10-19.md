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
