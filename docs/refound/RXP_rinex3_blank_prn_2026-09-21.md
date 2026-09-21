# RXP: RINEX 3 satellite numbers padded with a blank are dropped

**STATUS**

- **id:** RXP
- **topic:** build-infra
- **status:** open
- **summary:** Defect found 2026-09-21 by the multi-site census. The RINEX 3 path of the observation parser takes the first three characters of a record as the satellite identifier and does not normalize a blank, so a file that writes `G 7` for `G07` loses every satellite numbered below 10, silently. 16 of the 310 observation files of 2024-129 are written that way, four of them in the 59-station roster (CAS100ATA, GOPE00CZE, LCK400IND, LMMF00MTQ): those stations have contributed no satellite below G10 to any field run of the re-founding. The fix is unconditional; gates and the re-baseline reading are frozen here.
- **source:** [MSC](MSC_multi_site_consumer_2026-09-21.md) section 4 (ANMG00MYS never reaches a decimetre and solves on a median of five satellites)
- **knobs / posture:** none (a reader defect; no knob)
- **dates in the text:** 2026-09-21
- **hashes / run dirs:** `runs/_rf0hv5_field6`, `runs/_rap_msc_anmg_hv5_d`

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
