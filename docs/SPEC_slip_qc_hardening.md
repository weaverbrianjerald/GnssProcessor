# SPEC — Network slip-QC hardening (post IF-TD ↔ MW agreement audit, 2026-07-21) — SPLIT

`docs/SPEC_slip_qc_hardening.md` was split on 2026-09-14 — the original is [archive/SPEC_slip_qc_hardening.md](archive/SPEC_slip_qc_hardening.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/slip.md](topics/slip.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 0. Evidence base (what the audit measured) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 1. Ground rules (binding, project-wide) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 2. Card R — refsat slip-gate hardening (fixes L1) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 3. Card L — LLI corroboration in the MW drain (fixes L2) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 4. Card E — diagnostic MW/LLI event emission when cold (fixes L3) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 5. Card G — gap-coincident slip instrumentation (addresses L4, instrument-first) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 6. Validation harness (shared) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 7. Acceptance summary | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 8. Implementation record, 2026-07-21 (absorbed `slip_qc_hardening_report_2026-07-21.md`) | slip-QC impl | [cards/slip/SLIPQC-IMPL-2026-07-21.md](cards/slip/SLIPQC-IMPL-2026-07-21.md) |
| 8.1 What landed | slip-QC impl | [cards/slip/SLIPQC-IMPL-2026-07-21.md](cards/slip/SLIPQC-IMPL-2026-07-21.md) |
| 8.2 Findings F1–F5 that contradict the spec's premises | slip-QC impl | [cards/slip/SLIPQC-IMPL-2026-07-21.md](cards/slip/SLIPQC-IMPL-2026-07-21.md) |
| 8.3 Card-by-card as built | slip-QC impl | [cards/slip/SLIPQC-IMPL-2026-07-21.md](cards/slip/SLIPQC-IMPL-2026-07-21.md) |
| 8.4 Validation (Release, same-build two-arm gate, 2026-07-21) | slip-QC impl | [cards/slip/SLIPQC-IMPL-2026-07-21.md](cards/slip/SLIPQC-IMPL-2026-07-21.md) |
| 9. Follow-up cards, 2026-07-22 (absorbed `SPEC_slip_qc_followups.md`) | slip-QC follow-ups | [cards/slip/SLIPQC-FOLLOWUPS.md](cards/slip/SLIPQC-FOLLOWUPS.md) |
| 9.0 Evidence base, lean-review notes N1–N5, and why the D5 lead outranks everything else | slip-QC follow-ups | [cards/slip/SLIPQC-FOLLOWUPS.md](cards/slip/SLIPQC-FOLLOWUPS.md) |
| 9.1 Card D5-A — D5 re-tap attribution (instrument-first; land BEFORE Card W) | slip-QC follow-ups | [cards/slip/SLIPQC-FOLLOWUPS.md](cards/slip/SLIPQC-FOLLOWUPS.md) |
| 9.2 Card W — warm validation leg (≥ 1 h) — ⛔ PREPARED 2026-07-22, NEVER LAUNCHED | slip-QC follow-ups | [cards/slip/SLIPQC-FOLLOWUPS.md](cards/slip/SLIPQC-FOLLOWUPS.md) |
| 9.3 Card OP — off-pair / unmeasurable corroboration widening (CONDITIONAL) — trigger never fired | slip-QC follow-ups | [cards/slip/SLIPQC-FOLLOWUPS.md](cards/slip/SLIPQC-FOLLOWUPS.md) |
| 9.4 Card H — housekeeping (fold into the next slip-QC commit; no field gate) | slip-QC follow-ups | [cards/slip/SLIPQC-FOLLOWUPS.md](cards/slip/SLIPQC-FOLLOWUPS.md) |
| 9.5 Acceptance summary + sequencing | slip-QC follow-ups | [cards/slip/SLIPQC-FOLLOWUPS.md](cards/slip/SLIPQC-FOLLOWUPS.md) |
| 10. Card D5-A + H implementation record, 2026-07-22 (absorbed `slip_qc_d5a_report_2026-07-22.… | D5-A | [cards/slip/D5A-RETAP-ATTRIBUTION.md](cards/slip/D5A-RETAP-ATTRIBUTION.md) |
| 10.2 D5-A as built | D5-A | [cards/slip/D5A-RETAP-ATTRIBUTION.md](cards/slip/D5A-RETAP-ATTRIBUTION.md) |
| 10.3 D3 — the cross-experiment protocol (binding, written BEFORE Card W runs) | D5-A | [cards/slip/D5A-RETAP-ATTRIBUTION.md](cards/slip/D5A-RETAP-ATTRIBUTION.md) |
| 10.4 Card H as built | D5-A | [cards/slip/D5A-RETAP-ATTRIBUTION.md](cards/slip/D5A-RETAP-ATTRIBUTION.md) |
| 10.5 Validation (2026-07-22) | D5-A | [cards/slip/D5A-RETAP-ATTRIBUTION.md](cards/slip/D5A-RETAP-ATTRIBUTION.md) |
| 11. Current status and what is still owed (2026-08-14) | slip-QC | [cards/slip/SLIPQC-REF.md](cards/slip/SLIPQC-REF.md) |
| 12. Card G RESPONSE — `network.cycle_slip.gap_reentry_relevel` (2026-09-09) | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 12.1 The defect (Card G's own G3 rule, come back nonzero) | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 12.2 The build | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 12.3 ROUND 1 was INERT — the live-state walk (the lesson) | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 12.4 ROUND 2 — arms and results (`runs/fldf3_pl2_gapfree2/VERDICT.md`) | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 12.5 Verdict | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 12.6 The successor tried once — bridge v3 is FLAT on this posture | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 12.7 Open items this card leaves | Card G response | [cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) |
| 13. Card ORACLE — `network.cycle_slip.oracle_relevel`, the sim-only CEILING of a level-preser… | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
| 13.1 The knobs | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
| 13.2 Where it actuates, and where it deliberately does not | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
| 13.3 The window — a detector-latency bound floored by the carry's BIRTH | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
| 13.4 Counters (standing invariant #4) | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
| 13.5 The four arms — the mechanism is EXACT and the plateau does not move | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
| 13.6 Addendum, 2026-09-10 afternoon — the two levers this card named, both measured | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
| 13.7 The FALSE-ALARM-ONLY corner — `oracle_true_slip`, and what it proves (2026-09-10 evening) | Card ORACLE | [cards/slip/CARD-ORACLE-RELEVEL.md](cards/slip/CARD-ORACLE-RELEVEL.md) |
