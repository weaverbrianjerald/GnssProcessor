# SPEC — prior-residual census (the stale-carry test, generalized off D5) — SPLIT

`docs/SPEC_prior_residual_census.md` was split on 2026-09-14 — the original is [archive/SPEC_prior_residual_census.md](archive/SPEC_prior_residual_census.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/raim-qc.md](topics/raim-qc.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| + its constraint-aware global σ̂₀ rider (`network.prior_residual_global_sigma0`) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 1. Why — one statistic for a problem we currently meet with a zoo | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 2. THE BINDING PRECEDENT — read this before designing anything | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 3. Family inventory — who has a screen today | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 4. Design | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| INC-0: the shared statistic (one definition, one file) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| INC-1: the exceedance ladder (the actual deliverable) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| INC-2: the label — three populations, never pooled | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| INC-3: corroboration join (D5 v3's method, from cycle one) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 5. Cost | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 6. Wiring sites (all verified against the tree at 2026-08-04) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 7. Config | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 8. Unit tests | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 9. Field validation — known-answer gates | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 10. Deferral triage (pre-declared, per standing invariant #3) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 11. Standing-invariant pre-flight | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 12. What this card explicitly does NOT do | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 13. BUILD RECORD (2026-08-04) — what landed, and every deviation from §1–§12 | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 13.1 Measured on the FT63 0.08 h smoke (Release, 63 stations, 11 epochs) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 13.2 Deviations from the spec, and why | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 13.4 305-station scaling smoke (2026-08-04, `_agu305_prires_smoke.json`) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 13.5 The INC-3 join saturated on SHARED families — FOUND 2026-08-04, FIXED same day | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 13.3 Open, for whoever grades this next | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 14. THE GLOBAL σ̂₀ RIDER — `network.prior_residual_global_sigma0` | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.1 Why | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.2 Design — three sums, one ratio, nothing acted on | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.3 Cost | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.4 Config | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.5 Unit tests — cases 13–18 | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.6 Field validation | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.7 Deferral triage | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.8 Standing-invariant pre-flight — see §11 | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 14.9 What this rider explicitly does NOT do — see §12, final bullet | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 15. BUILD RECORD B — the rider (2026-08-04) | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 15.1 Measured on the FT63 raim-live smoke (Release, 63 stations, 20 epochs) | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 15.2 Deviations from §14, and why | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 15.3 Open, for whoever grades the rider next | global σ̂₀ rider | [cards/raim-qc/GLOBAL-SIGMA0-RIDER.md](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) |
| 16. Pointer-drift ledger (audited 2026-08-15) | prires census | [cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) |
| 17. The RNEG re-partition (2026-09-09) | RNEG | [cards/raim-qc/RNEG-REPARTITION.md](cards/raim-qc/RNEG-REPARTITION.md) |
| 17.1 What a `post_var > fold_var` row is | RNEG | [cards/raim-qc/RNEG-REPARTITION.md](cards/raim-qc/RNEG-REPARTITION.md) |
| 17.2 The measured artifact (field, `pl` family, both leveling-vehicle arms) | RNEG | [cards/raim-qc/RNEG-REPARTITION.md](cards/raim-qc/RNEG-REPARTITION.md) |
| 17.3 What was built | RNEG | [cards/raim-qc/RNEG-REPARTITION.md](cards/raim-qc/RNEG-REPARTITION.md) |
| 17.4 OPEN — the numerator card (not part of this change) | RNEG | [cards/raim-qc/RNEG-REPARTITION.md](cards/raim-qc/RNEG-REPARTITION.md) |
