# SPEC — redundancy / testability census (the statistical parked-garbage monitor) + its three riders — SPLIT

`docs/SPEC_redundancy_census.md` was split on 2026-09-14 — the original is [archive/SPEC_redundancy_census.md](archive/SPEC_redundancy_census.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/raim-qc.md](topics/raim-qc.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 1. Why redundancy, not magnitude (the design argument) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 2. Design — two censuses and the cross-flag | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| INC-1: row-redundancy census (`network.redundancy_census_stride`, int, default 0 = off) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| INC-2: unobservable-local census (same stride, no extra knob) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| The formal MDB extension (no longer deferred) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 3. Cost model and the two-tier escalation (measured anchors) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 4. Wiring sites | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 5. Unit tests | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 6. Field validation — KNOWN-ANSWER gates (standing harness) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 7. Deferral triage (pre-declared) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 8. BUILD RECORD (Opus session, 2026-08-01) — implemented, smoke-gated | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| Deviations from this spec, all deliberate | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| Smoke gate (FT63, 63 stations, 10 epochs, COLD, Release) | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 9. ★ CONVERGENCE HYGIENE — the one statement governing the host and all three riders | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 10. ★ GOVERNANCE — scope discipline, standing-invariant pre-flight, what these cards do NOT do | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 10.1–10.7 Standing-invariant pre-flight | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 10.8 What these cards explicitly do NOT do | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 11. RIDER A — Baarda reliability numbers (MDB) on the network path (`network.mdb`) | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.1 Why | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.2 Design | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.3 Cost | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.4 Config | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.5 Unit tests | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.6 Field validation | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.7 Deferral triage | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 11.10 BUILD RECORD (Opus session, 2026-08-04) — implemented, smoke-gated | Rider A | [cards/raim-qc/MDB-RIDER-A.md](cards/raim-qc/MDB-RIDER-A.md) |
| 12. RIDER B — BlockFormal: (station, sat) LOS block outlier census (`network.block_baarda`) | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.1 Why — the smearing gap between row tests and station polish | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.2 Binding precedent — the prior-residual census's D5 rules apply unchanged | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.3 The statistic | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.4 Design | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.5 Cost | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.6 Config | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.7 Unit tests | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.8 Field validation — **ALL OWED** | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.9 Deferral triage | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 12.12 BUILD RECORD (Opus session, 2026-08-04) — BB-INC-0..3 implemented, unit-gated | Rider B | [cards/raim-qc/BLOCK-BAARDA-RIDER-B.md](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) |
| 13. RIDER C — station congruence screen (`network.congruence_screen`) | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.1 Why — the one fault class no armed tier can see | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.2 The statistic | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.3 Design | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.4 Cost | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.5 Config | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.6 Unit tests | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.7 Field validation | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.8 Deferral triage (invariant #3) | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 13.11 BUILD RECORD — CG-INC-0..3, 2026-08-04 | Rider C | [cards/raim-qc/CONGRUENCE-RIDER-C.md](cards/raim-qc/CONGRUENCE-RIDER-C.md) |
| 14. ★ CROSS-RIDER RECONCILIATIONS — the shared objects, stated once | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 14.1 ∞ (arithmetic) vs uncheckable (policy) — a strict SUBSET, never an equality | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 14.2 The two `NetworkCarryVce` corrections, inherited by riders B and C | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 14.3 The shared kernel and who owns it | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 14.4 The shared δ₀ convention | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 14.5 The one budget restatement the measurements forced | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 14.6 The shared stride and escalation | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 14.7 Every knob in the family, one table | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
| 15. Consolidated OWED / gate ledger | redundancy census | [cards/raim-qc/REDUNDANCY-CENSUS.md](cards/raim-qc/REDUNDANCY-CENSUS.md) |
