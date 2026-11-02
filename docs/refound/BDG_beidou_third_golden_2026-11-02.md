# BDG: the BeiDou golden (a third lineage, course B of card BDSB)

**STATUS**

- **id:** BDG
- **topic:** network-vehicle
- **status:** open
- **summary:** The author's decision of 2026-11-02 on card BDSB section 5: course B, BeiDou as a separate third golden. `config/golden/golden_field_bds_refound.json` is generated from the field golden's own output plus four keys (`processing.active_systems` with BeiDou, `processing.signal_priorities.BeiDou` `["C2I","L2I","C6I","L6I"]`, `network.second_band_admission`, `network.n3c_wire_per_signal`); the two GPS + Galileo goldens are unchanged. The three BeiDou rows become RECOMMEND_SCOPED under a new regime predicate `has_bds` (BeiDou named in `processing.active_systems`), defined in the loader and the offline checker. Gates pre-registered in section 3; results owed.
- **source:** card [BDSB](BDSB_beidou_continued_2026-10-19.md) section 5 (the three courses and what course B needs); cards [BDS](BDS_beidou_third_constellation_2026-10-18.md), [MDAY](MDAY_multiday_candidate_2026-10-19.md) and [BDS305](BDS305_beidou_305_stations_6h_2026-10-29.md) (the evidence); the author, 2026-11-02: "Yes, BeiDou as a third golden is good."
- **knobs / posture:** the four keys above, pinned on the new lineage only; regime of the three ledger rows: `val("network.ambiguity_vehicle", "level") == "float_kf" and not tri and has_bds`. `network.second_band_scope` (`station`) and `network.second_band_min_sats` (1) stay at their loader defaults, as graded.
- **code:** `src/core/config/GnssConfigNetworkPosture.cpp` (the `has_bds` predicate and three SCOPED rows), `include/config/GnssConfig.hpp` (`PostureRegime::has_bds`), `scripts/config_posture_check.py` (the same predicate), `scripts/make_golden_config.py` (provenance hashes read CRLF as LF), `config/golden/golden_manifest.json` (the third entry), `docs/CONFIG_promotion_ledger.json` (two rows promoted, one added)
- **dates in the text:** 2026-11-02
- **hashes / run dirs:** branch `golden/bds-third` from main `bb9a352`; reference exe `gnss_processor_int1102.exe` (the Release build of `bb9a352`, sha256[:16] `7f4bd4729069be0e`); baseline of the lineage `D:/GnssProcessorSupport/runs_nom/_rap_bdsc_c6` (card BDSB section 4, exe `gnss_processor_bdsclose.exe`, sha256[:16] `91143c81de9a9547`)
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: can BeiDou posture C be made a golden of its own, generated and verified like the other two, without moving them? Built: a third manifest entry whose base is the field golden's output, three RECOMMEND_SCOPED ledger rows under the new `has_bds` regime, and the matching loader posture rows. Results owed (section 3).

> Results are appended TO THIS FILE, below the text.

---

## 1. The decision

Card BDSB section 5 set out three courses after every registered bar of cards BDS and BDSB passed: A, arm posture C on the field golden; B, keep it as a separate third lineage; C, hold. Its writer recommended B now and A only after the mechanism of the ARHT00ATA wire gain is named and the G01 epoch of DOY 128 and 131 is no longer on the wire. The author chose B on 2026-11-02. Under B the GPS + Galileo field golden stays the primary golden, every arm of a later card keeps its cost, and the sim bed, whose fixture holds no BeiDou observations, keeps following the field golden by a knob and not by a constellation.

## 2. What is built

- **The lineage.** `golden_field_bds_refound` in `config/golden/golden_manifest.json`, generated after the field golden with that golden's output as its base and four pins. The field golden's two exceptions (`network.trace_dd_fractional_ref`, `network.serialize_disc_counters`) are carried with the same reasons. Run paths go under `runs/golden_field_bds_refound/`.
- **The regime.** `has_bds` (BeiDou named in `processing.active_systems`) in `GnssConfig::networkPostureRegime_` and in `scripts/config_posture_check.py`. With BeiDou active the nine `gps_gal_only` rows (the satellite code bias, the family-difference datum and its birth prior, `sbasis_complete`, the Galileo inter-system bias re-inflation, the receiver ATX fallback, `processing.active_systems`, the noon-turn attitude and the Galileo priorities) fall out of regime, so on this lineage the loader stops reporting them and the generator stops requiring them; their values are inherited from the field golden unchanged (card BDSB section 5, item a, left untouched under course B).
- **The ledger.** `network.second_band_admission` and `network.n3c_wire_per_signal` move from UNVALIDATED to RECOMMEND_SCOPED; `processing.signal_priorities.BeiDou` gets its first row (RECOMMEND_SCOPED, `armed_when` the B1I / B3I list). All three carry the `has_bds` regime, so the GPS + Galileo goldens are out of it. Generator verify: field 82 armed in regime, sim 78, BeiDou 76 (82 less the nine `gps_gal_only` rows plus the three BeiDou rows); `config_posture_check.py --verify-loader` 0 coverage gaps.
- **Provenance hashes.** `make_golden_config.py` hashed the raw bytes of the base and the ledger, so a CRLF checkout recorded a different hash than an LF one for the same text. It now reads CRLF as LF; the regenerated GPS + Galileo goldens differ from main in the generation date and the ledger hash only.

## 3. Gates (frozen 2026-11-02, before any run of the changed executable)

- **G1, GPS + Galileo goldens unchanged.** Both 2 h goldens on the changed exe against `gnss_processor_int1102.exe`: `products.txt` and every CSV identical (wall-clock columns dropped); no log line differs other than wall-clock timing.
- **G2, user side unchanged.** The 16 quick arms of `scripts/uom_gate.py`: every CSV identical, no residual log difference.
- **G3, smoke.** The BeiDou golden, 10 epochs: exit 0, 10 of 10 epochs emitted, `C##` clock lines on the wire, the three BeiDou rows reported armed in the run-start posture block, no in-regime gap beyond the two documented exceptions.
- **G4, the lineage reproduces its graded arm.** The BeiDou golden, 6 h, on the changed exe against card BDSB's arm C6 (`runs_nom/_rap_bdsc_c6`): the arm files differ only in the three parallel-stage keys of card PES and the run paths; `products.txt` and every CSV identical (wall-clock columns dropped); log differences only the declared text (the posture rows, the PES and NOM5 run-start and run-end lines). An identical result makes C6 the lineage's baseline and the G4 run its twin on the current exe.
- **G5, unit suite.** Debug `gnss_tests` with 0 failures and the skip count of main; `gnss_simulator_selftest` all passed.
- **Close.** G1 to G5 pass: the branch merges into main at the author's word and the card closes. A G4 difference is read to its first differing file and epoch before anything merges.

## 4. Owed by this lineage (register lines)

- The orbit-normal attitude graded for BeiDou before a run on days with a BeiDou IGSO plane within 4 degrees of the Sun (C13, C38 and C08 from about DOY 137); the golden's DOY 129 is clear of it (card BDSB section 5, item e).
- The weak sites of card BDSB section 5 stand: ARHT00ATA as a GPS-only user of the BeiDou wire at the G01 epochs of DOY 128 and 131 (a wire defect common to both lineages), DAV100ATA and WUH200CHN as users without BeiDou, the 01:00 to 01:30 UT transient at WTZZ00DEU, BSHM00ISR's B3I phase.
- Course A (BeiDou on the primary field golden) reopens when the mechanism of the ARHT00ATA wire gain is named and the G01 epoch is off the wire.
