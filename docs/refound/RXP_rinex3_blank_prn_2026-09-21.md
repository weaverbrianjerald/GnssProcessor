# RXP: RINEX 3 satellite numbers padded with a blank are dropped

**STATUS**

- **id:** RXP
- **topic:** build-infra
- **status:** open
- **summary:** FIXED 2026-09-21: the RINEX 3 reader dropped every satellite below G10 in blank-padded files (16 of 310); the corrected field run, the new baseline, is WORSE at WTZZ00DEU.
- **source:** [MSC](MSC_multi_site_consumer_2026-09-21.md) section 4 (ANMG00MYS never reaches a decimetre and solves on a median of five satellites)
- **knobs / posture:** none (a reader defect; no knob)
- **dates in the text:** 2026-09-21
- **hashes / run dirs:** `runs/_rf0hv5_field6`, `runs/_rap_msc_anmg_hv5_d`; build main `23ce39b`, exe `gnss_processor_rxp.exe`; new field baseline `runs/_rap_r6_dual`

**Outcome.** The question: why ANMG00MYS never converged in [MSC](MSC_multi_site_consumer_2026-09-21.md). The RINEX 3 path took a record's first three characters as the identifier, so `G 7` never matched `G07` at four roster stations. The fix passes its controls and G1; ANMG00MYS goes from 0 to 256 epochs below a decimetre. WTZZ00DEU falls from 603 to 533 epochs: a larger burst of the 05:22 UT all-satellite clock-anchor decouple already in the old run, which [EV7](EV7_all_sat_clock_decouple_2026-09-21.md) traced to a coverage-gate defect and fixed (armed 2026-09-22). Owed: nine Python scripts with the same comparison ([OPEN_ITEMS_A](../status/OPEN_ITEMS_A_current_program.md)).

## 1. The defect (read by the manager)

`src/core/rinex/RinexParser.cpp`, the RINEX 3 epoch loop: `std::string sat_id =
line.substr(0, 3);` (line 626 at `b648a7e`). The RINEX 2 path of the same file
normalizes a blank in the identifier to a zero (the `extractSatsFromLine` lambda);
the RINEX 3 path does not. `G 7` is a well-formed identifier for the parser, matches
no orbit and no product record, and the satellite drops out with no warning.

- Files of 2024-129 with blank-padded numbers: 16 of 310 (ANMG00MYS, BRST00FRA,
  CAS100ATA, CMUM00THA, DAE200KOR, DAEJ00KOR, GMSD00JPN, GOPE00CZE, JCTW00ZAF,
  JPRE00ZAF, LCK400IND, LMMF00MTQ, STPM00SPM, ZIM200CHE, ZIM300CHE, ZIMM00CHE).
- In the 59-station roster: CAS100ATA, GOPE00CZE, LCK400IND, LMMF00MTQ. In
  `runs/_rf0hv5_field6` their post-fit rows hold 17, 17, 11 and 16 satellites over 6 h
  and none below G10; JPLM00USA, MAC100AUS and WTZR00DEU hold satellites below G10.
  Nine of the 32 GPS satellites are numbered below 10.
- Census sites: ANMG00MYS and STPM00SPM. At ANMG00MYS the file has 11 GPS satellites
  at the first epoch, six of them below G10; the consumer solves on a median of 5
  satellites (tenth percentile 3) where the other sites use 7 to 10.
- The sim bed has no such file (61 files scanned), so the fix cannot move it.
- The Python tools share the pattern (`scripts/rapid_arm.py` RINEX ops and the raw
  readers compare the first three characters): a `sats=` selector misses these
  records at these stations. Owed separately.

## 2. The fix, the gates and the reading (frozen)

- **Fix.** In the RINEX 3 path a blank second character of the identifier becomes a
  zero, as the RINEX 2 path already does. Unconditional: a reader that drops a valid
  satellite has no correct OFF state. One unit test on a RINEX 3 record with a
  blank-padded number.
- **G0 sim:** the dual sim base is byte-identical (products and event ledger).
- **G1 field:** the four roster stations carry satellites below G10 in the post-fit
  rows; the first-epoch latch line of every OTHER station is unchanged; a changed
  latch at one of the four is reported with its counts.
- **The re-baseline reading.** The dual field base is re-run with the fix. The result
  is the new baseline whichever way it moves, because the old one is a run with data
  missing. It is reported against `runs/_rf0hv5_field6` at WTZZ00DEU by hour and at
  the census sites with the paired statistic (each site's mean hourly p50 over hours
  2 to 6, sign counts and median), ANMG00MYS and STPM00SPM apart because the fix
  changes those consumers directly. A loss of more than 0.03 m in any hour at
  WTZZ00DEU, or a paired median loss across sites, is read (which of the four
  stations, which satellites) before the pair is re-set; it is not tuned away and
  the fix is not reverted.

## 3. 2026-09-21: RXP-1 result: fixed; inert where no file is blank-padded; the corrected field run is WORSE at WTZZ00DEU, and the loss is one episode that two of the four stations amplify together

Build by an Opus worker on main `23ce39b`, exe `gnss_processor_rxp.exe`: the one-line
normalization in the RINEX 3 observation loop (`RinexParser.cpp`), the identical
shape in the navigation reader (`RinexNavParser.cpp`, no file of the day affected),
suite `Rinex3BlankPrnTest` (3 cases); `scripts/rapid_arm.py` compares satellite
identifiers on a normalized form (`selftest` 22 of 22). `gnss_tests` 2379 ran, 2378
passed, the one known skip (the worker's run). `ProductParser::isValidPrn` rejects a
blank-padded number rather than mis-keying it (reported, not changed). Nine other
scripts under `scripts/` compare the first three characters of a record and are
listed in the worker's report; owed. Every number below was re-read by the manager.

**The controls pass (invariant #6).** Sim: `products.txt` and `network_events.csv`
sha256 equal to `runs/_rf0hv5_sim6`. Field with the four blank-padded roster stations
dropped, old reader against new reader: byte-identical (`9896fec0a614843c`,
`877e93442841a195`). Census, the old wire replayed by the new exe (`hv5x`): 14 of 16
sites reproduce their replay exactly, and the two with blank-padded files move:
**ANMG00MYS 0 to 256 epochs below a decimetre** (the first of MSC-1's three
non-converging sites is this defect) and STPM00SPM 406 to 485.

**G1 PASS.** Satellites below G10 in the post-fit rows: CAS100ATA 0 to 8, GOPE00CZE 0 to
3, LCK400IND 0 to 6, LMMF00MTQ 0 to 1; 18 new (station, satellite) arcs. No latch line
changes at any of the 60 stations: the old reader parsed the records and counted
them, it failed to MATCH them downstream.

**The corrected field run (`runs/_rap_r6_dual`, the new field baseline by section 2).**

| arm (fixed reader unless stated) | of the four, present | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| `_rf0hv5_field6` (old reader) | all, without their satellites below G10 | 0.43 0.07 0.06 0.08 0.08 0.07 | 603 | 126 | 0.0069 / 0.0128 |
| **`r6_dual`** | all | **0.48 0.05 0.05 0.08 0.09 0.10** | **533** | 183 | **0.0107 / 0.0184** |
| `r6_drop4_new` | none | 0.59 0.04 0.04 0.08 0.08 0.07 | 599 | 130 | 0.0087 / 0.0137 |
| `r7_no_cas1` | GOPE, LCK4, LMMF | 0.49 0.04 0.05 0.08 0.09 0.08 | 584 | 121 | 0.0063 / 0.0114 |
| `r7_no_lck4` | CAS1, GOPE, LMMF | 0.43 0.07 0.06 0.08 0.08 0.07 | 609 | 115 | 0.0072 / 0.0141 |
| `r7_no_gope` | CAS1, LCK4, LMMF | 0.45 0.05 0.05 0.08 0.09 0.10 | 527 | 183 | |
| `r7_no_lmmf` | CAS1, GOPE, LCK4 | 0.58 0.04 0.05 0.08 0.09 0.10 | 530 | 168 | |
| `r7_only_cas1` | CAS1 | 0.59 0.05 0.06 0.08 0.08 0.07 | 602 | 113 | 0.0070 / 0.0140 |
| `r7_only_gope` | GOPE | 0.52 0.05 0.05 0.08 0.08 0.07 | 601 | 132 | |
| `r7_only_lck4` | LCK4 | 1.41 0.52 0.17 0.03 0.05 0.05 | 396 | 119 | 0.0067 / 0.0124 |
| `r7_only_lmmf` | LMMF | 0.47 0.05 0.04 0.07 0.08 0.05 | 610 | 136 | |

Across the census sites the corrected wire against the old one (both replayed by the
new exe): median count 191 to 206.5, 7 sites improve and 8 worsen (QUIN00USA 106 to 7,
POHN00FSM 60 to 5, BAKE00CAN 331 to 203, CHTI00NZL 484 to 370; CEDU00AUS 269 to 448,
ASCG00SHN 146 to 262, STPM00SPM 485 to 563); across-site median by hour 0.83 0.28 0.10
0.10 0.11 0.17 m against 0.77 0.28 0.13 0.14 0.11 0.13 m: hours 3 and 4 better, hours 1
and 6 worse. At QUIN00USA the code-bias dropout excursion of MSC section 4 grows
(281 to 380 epochs above 5 m, largest 98 m).

**The reading the card asked for (manager's arms `r7_*` and event reads).**

- The loss needs CAS100ATA AND LCK400IND together. Either one absent gives the old
  numbers or better (584, 609, 602, 610, 601); with both present the sixth hour is
  0.10 m (`r6_dual`, `r7_no_gope`, `r7_no_lmmf`) and `T4_attrib` is 168 to 183 against
  113 to 136. GOPE00CZE and LMMF00MTQ carry none of it. LCK400IND alone of the four
  wrecks the first three hours (1.41 0.52 0.17 m) and then has the best hours 4 to 6 of
  any arm, so the first hours of this posture depend on the roster in a way that is
  not read.
- It is not thin support: every satellite-epoch at which one of the four adds
  support had six or more stations already.
- The extra post-fit refloats are one burst of one episode that the old run already
  has. `T4_attrib` by hour: 14 1 21 17 7 66 (old) against 16 4 24 15 4 120 (corrected);
  in the five minutes from 05:20 UT, 21 against 72 (27 without LCK400IND). At TOW
  278520 (05:22:00 UT) the clock temporal anchor decouples for every satellite at once
  (`CLK_ANCHOR cta_decouple`, `reason=nis`, innovations from -0.22 to +0.20 m with both
  signs, the common part 0.0014 m), every clock is marked, the hold declares the step
  not transferred for every satellite, and 26, 28 and 14 arcs are refloated post-fit
  in the next three epochs, mostly at stations of the African, Indian Ocean and
  Antarctic sector on satellites at or above G10 (of the net +57 events, satellites
  below G10 account for +7). The newly admitted arcs are not the ones refloated.
- Two of the 18 new arcs are poor on their own (the worker's read: LCK400IND G04 with a
  code RMS of 1.29 m, CAS100ATA G08 with a phase RMS of 0.010 m); whether they feed the
  episode is not read.

**Standing.** The fix stays (section 2). The field baseline is `runs/_rap_r6_dual` from
here on; the sim pair is unchanged. The 05:22 UT all-satellite decouple is the
dominant open loss of the field bed (it is the sixth-hour loss of every arm since
RF-0hv), more data makes it larger, and it is an event-harness item: one event, 60
epochs, read before any roster or weight is touched.
