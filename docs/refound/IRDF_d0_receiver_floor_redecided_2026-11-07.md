# IRDF: decision D0 re-decided with the event rows armed

**STATUS**

- **id:** IRDF
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-07. Card IRD's decision D0 (the receiver phase-bias floor) re-decided at the author's word, on the arm going forward (every event row armed), after card IRDE section 9 traced the floor as the cause of A1's wet-delay state. The frozen rule and its bars are applied as written; the readings that bear on them are frozen here before they are graded.
- **source:** [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 10 (D0) and C3; [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md) section 3.5 (the first verdict); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 7.3 (the hold that waits on D0); [IRDE](IRDE_integer_recovering_datum_slip_2026-11-06.md) section 9 (the mechanism)
- **knobs / posture:** `network.phase_datum_eq34_rpb_prior_sigma_m`, `network.phase_datum_eq34_rpb_prior_birth_only`; the base arm A1krhRsm of `scripts/ird_make_arms.py`. No golden change
- **dates in the text:** 2026-11-07
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird4ms.exe` (sha256[:16] `4547066185234026`); arms `_rap_ird4z_*`, replays `_rap_d0c_*`, under `D:/GnssProcessorSupport/runs_nom/`
- **gates (frozen at pre-registration):** section 2; the bars are card IRD's, unchanged

**Outcome.** Question: with every event row armed, does the receiver floor stay, go off or change? Standing: pre-registered; no result yet.

## 1. Why

Asked after card IRDE section 9, "Should D0 (the receiver floor) be re-decided before arm C?", the author answered: "Yes, re-decide D0". The first verdict (IRDB section 3.5) was measured before the event rows and kept the floor because neither switch passed. IRDE section 9 then traced A1's metre-level wet-delay errors to the floor, and showed that under A1 a row's I-b reads that state.

## 2. The rule and what is measured

The rule, card IRD section 10, verbatim: "The floor goes off if A0f, on r3c and the golden bed, shows a DCB sd <= 3x the golden's (0.17 m), W5 withholds no higher than A0's, and float-consumer p50 within +10 % of A0's in hours 1 and 2. Otherwise build birth_only and re-measure. Otherwise keep the floor and record the GF-datum defect, which blocks the ionosphere follow-up."

One reading of the rule is declared: the reference is A1krhRsm with the floor at its default in place of A0, because that is the arm going forward (A1 reproduces A0 where both bands track).

**Candidates.** The rule's two: floor off (sigma 0) and birth only (sigma 1). Readings outside the rule's list: sigma 3 m and 100 m.

**Bars.** As quoted, per candidate on r3c and the golden sim. A clause that fails is reported as failed. No bar moves without the author's ruling.

**Readings, frozen before grading.** Each per candidate, against the floor-on arm and against the no-chain reference R1 on the same exe, on the same two beds:

1. **Multi-site replay.** 16 in-network consumers per wire (S01, S05, ..., S57 and S60), each replayed from the wire's first epoch. The statistic is each site's p50 in hour 1 and hour 2, then the paired change against the floor-on wire: median and sign counts. A replay of the held-out consumer reproduces the in-loop trace (240 of 240 rows identical on `ird4ms_a1krhRsm_r3c`).
2. **DCB against truth.** The sd of (estimate less truth) across receivers, in the sign the microscope shows, using each fixture's `truth_rx_bias.csv`.
3. **Integer instruments.** Reference-free double differences, the search's would-be fixes against truth, the stop rule's event-free chords.
4. **The wet delay against truth,** after hour 1.
5. **Rank.** Station positive-definite rejections and emitted epochs with the floor off.
6. **The row fixtures.** I-b per row with the floor off, from the arms already run (`ird_t1`, `ird_tr`, `ird_ms`, `ird_msr` and the microscope twins), and the rows' consumer clause.

**Seen before this pre-registration.** The bars at sigma 1, 3 and 0 on both beds (IRDE section 9.7), the row fixtures at sigma 0 and 3 (IRDE sections 8.4 and 9.5), hour 1 of the held-out consumer by 5 min bin on those arms, and the fixture's true DCB spread (sd 0.538 m). Not seen: the replays, birth only and sigma 100 m on these beds, the DCB error table on all arms, and the integer instruments on these beds.

**What the verdict will say.** Whether each candidate passes the rule as written. If none does, the rule's last sentence is what stands, and any evidence that the rule should be read otherwise goes to the author as a question, with the recommendation stated and nothing armed.
