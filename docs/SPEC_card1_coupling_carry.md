# CARD 1 — Coupling-row carry: border TIME UPDATE (1d) + gauge restore (1e) + JOINT whitening (1b) — SPLIT

`docs/SPEC_card1_coupling_carry.md` was split on 2026-09-14 — the original is [archive/SPEC_card1_coupling_carry.md](archive/SPEC_card1_coupling_carry.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/network-vehicle.md](topics/network-vehicle.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 1. Status banner — the two Card-1e structural discoveries | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 2. Card 1d — the defect (verified 4 ways, file:line) | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 3. Magnitude in the T7 lineage, and why the symmetric toy hid it | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 4. Batch-exact validation, and the known bounded conservatism | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 5. Build | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| v1 — scalar per-column κ (the shipped form) | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| v2 — matrix-exact (optional, after v1 field validation) | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 6. Tests and the field falsifier | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 7. Known-open and sequencing | Card 1d | [cards/network-vehicle/CARD-1D-COUPLING-CARRY.md](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) |
| 8. Card 1b — coupling-row JOINT whitening: the mechanism and its pre-registered gate | Card 1b | [cards/network-vehicle/CARD-1B-JOINT-WHITENING.md](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) |
| 8.1 ⚠ SUPERSEDED — the 2026-07-10 late ADDENDUM ("ARMED BUT INERT + 10× SLOWDOWN") | Card 1b | [cards/network-vehicle/CARD-1B-JOINT-WHITENING.md](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) |
| 9. Card 1b — RESULT (2026-07-10, all four addendum steps executed): PREMISE FALSIFIED; toggle… | Card 1b | [cards/network-vehicle/CARD-1B-JOINT-WHITENING.md](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) |
| 10. The accidental `trace_section_timers` byte-identity A/B (Card 1b step 4) | Card 1b | [cards/network-vehicle/CARD-1B-JOINT-WHITENING.md](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) |
| 11. What the flat σ actually is — and the verdict on `carry_coupling_joint_whiten` | Card 1b | [cards/network-vehicle/CARD-1B-JOINT-WHITENING.md](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) |
| 12. Artifacts and configs | Card 1b | [cards/network-vehicle/CARD-1B-JOINT-WHITENING.md](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) |
| 13. Card 1f / 1g — the TRUNCATED-ROW gauge defect, and the reinit-source widening | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.1 The defect (1f) | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.2 Why "annihilate the family sum" is the WRONG fix | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.3 What 1f actually does | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.4 The 1g half | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.5 First armed run — WIRING PROVEN, EFFECT UNGRADED | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.6 Defects found by the post-implementation review (all fixed) | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.7 POSITIVE CONTROL — simnet24 at TOW 305220 (2026-08-15).  **PASS.** | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.8 THE VALIDATION LADDER — 24 (sim) / 50 (field) / 150 (field).  **PASS at every point.** | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.8.1 FIELD ARM detail — 50 real IGS stations, 1 h, DOY-129 | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 13.9 THE 2×2 ISOLATION — 1f and 1g each carry a DIFFERENT half of the win (2026-08-17) | Cards 1f + 1g | [cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) |
| 14. FIXED — the eq34 D4 S-transform now re-bases `coupling[].s_prev` (filed 2026-08-15, fixed… | eq34 D4 | [cards/network-vehicle/EQ34-D4-S-TRANSFORM-FIX.md](cards/network-vehicle/EQ34-D4-S-TRANSFORM-FIX.md) |
