# SPEC — BlockFormal: (station, sat) LOS block outlier census

**Status**: SPEC'd 2026-08-04. Build AFTER the prior-residual census's first field
results — its per-family tails are this card's design input (pre-declared as its §10
successor).
**Origin**: NGS LASER Guide v2.7 slide 5-101 (PDF p. 413): Baarda data snooping at the
3D block level, `OutlierDetectionMethod = BlockFormal`, formulas Snow & Schaffrin
(2003), *GPS Solutions* 7(2):130–139, eqs. (13)–(18). Slide 5-98 states the motivation:
a single outlier is SMEARED across residuals ("not necessarily a one-to-one
correspondence between large residuals and outliers").
**Memory context**: `project_m1_card4_d5_innovation` (§2 of the census spec — the
binding precedent, all three rules apply verbatim), `project_network_if_td_slip` (the
78-station mass refloat = the separability failure this card addresses),
`docs/SPEC_redundancy_census.md` (the sweep this rides), `project_laser_theory_mapping`.
**Scope discipline**: default-OFF, OFF byte-identical, MEASURE-ONLY, fingerprint-SAFE.

---

## 1. Why — the smearing gap between row tests and station polish

One physical fault (one LOS: multipath, a wet radome lobe, an unflagged slip) projects
onto 2–8 rows — code+phase × bands — each individually sub-threshold while the block is
jointly loud. Today's tiers straddle exactly that unit: per-row w-tests below it,
station-level median polish above it. The mass-refloat incident was the textbook cost:
the bad-STATION path was disabled rather than confirmed because nothing could say
*which* LOS carried the fault. LASER's BlockFormal is the middle tier, and its natural
block here is the (station, sat) LOS across bands.

## 2. Binding precedent — the census's D5 rules apply unchanged

1. **Empirical critical values, never tabular.** Snow & Schaffrin's F(α, b, r) assumes
   Gaussian errors; D5 measured that false by orders of magnitude in the tail. The
   deliverable is the empirical exceedance ladder of the block statistic per family.
2. **Corroboration in cycle one.** Contingency table vs the slip-QC raw-observable
   verdicts ships in v1 (`gap_instrument` auto-arm, the same loader tripwire the census
   carries at `GnssConfigLoadNetwork.cpp:3543-3548`).
3. **Response is a separate card.** v1 wires nothing.

## 3. The statistic

For block B = the data rows of one (station, sat) this epoch, b = |B|:

```
T_B = ẽ_Bᵀ (Q_ẽ,B)⁻¹ ẽ_B / b          normalized block quadratic form
t_B = T_B / σ̂₀,f²                      empirically re-scaled (per family f)
```

`Q_ẽ,B` is the b×b block of the residual cofactor — the diagonal of which is what
`rowRedundancy` already returns. INC-0 extends the kernel: a sibling
`NetworkNormalEquations::blockResidualCofactor(station, rows)` returning the b×b from
the same inputs `rowRedundancy` takes (`src/core/filter/NetworkNormalEquations.cpp:1690`;
station-local A_s/A_r rows + `cov_free` = `datumFrameSharedCov`,
`src/core/filter/NetworkEstimator.cpp:360/:438`). Same math, off-diagonals kept, b ≤ 8.
Degenerate guard: if `Q_ẽ,B` is singular at the working precision (a fully-leveraged
block — the block analogue of r < ε), the block is UNCHECKABLE: counted, identity kept,
never inverted through (pinv or drop — count either way; the census's
prefer-fallback-over-drop discipline).

σ̂₀,f is a `NetworkCarryVce` instance fed `T_B` per family with unit variance — the D5
v2 pattern verbatim (`NetworkInvariantCarryDriver.cpp:1453-1455` precedent), same
predict-then-update and never-censor contracts as the census.

## 4. Design

- **INC-0**: `blockResidualCofactor` kernel + unit tests against brute-force dense GLS.
- **INC-1**: the sweep. Blocks are enumerated for SWEPT stations only — this card rides
  the redundancy census's stride/targeted-escalation machinery
  (`src/core/filter/NetworkRedundancyCensus.cpp`) rather than adding a scheduler. Per
  (family × label) exceedance ladders over `t_B` (census bins), n_blocks, worst-block
  identity. Labels: RESPONDED / CLEAN / FRAME exactly as the census defines them, block
  inherits the union of its rows' labels (any RESPONDED row ⇒ RESPONDED block; else any
  FRAME ⇒ FRAME).
- **INC-2**: corroboration join per block (slip-QC verdict on that (station, sat)),
  b-cell reported per family.
- **INC-3**: the smearing measurement — the card's unique deliverable: among blocks with
  `t_B` above the report threshold, the fraction whose individual rows were ALL below
  the per-row threshold (`block_only` count). That fraction is the population invisible
  to every existing tier; if it is ~0, the block tier adds nothing and that finding
  closes the card honestly.

## 5. Cost

Per swept station: b×b builds and inverts for its tracked sats, b ≤ 8 — bounded by the
census's own `rowRedundancy` cost class (6.1 ms/station-sweep measured). Budget: ≤ 2×
the census sweep it rides; report measured cost per station-sweep. Stride inherited; no
new knob.

## 6. Config

| knob | type | default | note |
|---|---|---|---|
| `network.block_baarda` | bool | `false` | master; requires `redundancy_census` (the sweep host) — hard error otherwise |
| `network.block_baarda_report_sigma` | double | `4.0` | trace/event threshold on `t_B`; reporting only |
| `network.block_baarda_corroborate` | bool | `false` | INC-2; auto-arms `cycle_slip.gap_instrument` with a WARN |

Loader: next to the redundancy-census block. Invariant-#4 tripwires as in the census
(zero-block families distinguishable from unarmed).

## 7. Unit tests

New file `tests/test_block_baarda.cpp`, one suite.

1. `blockResidualCofactor` equals the brute-force dense `(P⁻¹ − A N⁻ Aᵀ)_BB` on a toy
   net to 1e-10, including a rank-deficient-datum case.
2. **The smearing test — the reason this card exists, as a unit test.** A fault split
   evenly across 4 rows of one block, each row at 0.6× the per-row threshold: every
   w-test silent, `T_B` loud. And the converse: 4 independent faults in 4 different
   blocks do NOT pool into any single loud block.
3. Gaussian null ⇒ `T_B·b ~ χ²_b` ladder occupancies once σ̂₀ converged; Student-t
   (ν=3) ⇒ gross 4σ over-occupancy with geo-mean near 1 (the D5 failure mode preserved
   at block level).
4. Label partition sums; uncheckable-block counting; predict-then-update ordering.
5. OFF byte-identity (structural).

## 8. Field validation

Converged warm window (census convergence-hygiene rule: cold smoke establishes wiring
and cost only).

1. OFF byte-identity; live wiring; cost within budget.
2. **Positive control**: RESPONDED-block ladder shifted vs CLEAN (same meaning and same
   caveats as census gate 3; null result = publishable finding, not tuning license).
3. **Contingency b-cell** per family (D5's was 97 % — expect the block statistic to do
   BETTER than scalar D5 on code+phase joint evidence; measure, don't assume).
4. **The smearing fraction** (INC-3): report `block_only / blocks_flagged`. This single
   number decides whether a response card is ever worth writing.
5. Product claims on total_IF only; solve byte-identical in every arm.

## 9. Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| Any response (deweight/refloat/exclude per block) | **INERT** — measure-only; response card's design input is §8.4 | — |
| Leave-one-station-out solution separation (would re-arm the bad-STATION path) | **INERT** — own card; additive normals make it cheap but it is solve-adjacent | DIA catalog item 5 |
| Cross-band prior-row blocks (carry families) | **INERT** — the census's scalar ladder covers priors in v1; block form needs the same `Q_ẽ,B` extension on fold rows | census §10 |
| Snow & Schaffrin's exact F reference distribution | **INERT** — empirical ladder by rule §2.1 | D5 history |

## 10. Standing-invariant pre-flight

1. Adds no response, removes none; measures the middle tier invariant #1 lacks.
2. Release-only. 3. Deferrals above with evidence. 4. Ladder + n_blocks are the run-end
telemetry; "ran, saw nothing" ≠ "never ran". 5. Residual quadratic forms are
datum-invariant. 6. Default-OFF byte-identical; same-build A/B. 7. No coupling change;
FRAME blocks measured, not discarded (census convention).

## 11. What this card explicitly does NOT do

- Does not modify D5, RAIM, median polish, or the census.
- Does not import F(α, b, r) tables (§2.1).
- Does not act on any block — including the loud ones (§9 row 1).
- Does not claim the block tier has power here; §8.4 measures whether it does, and a
  null is a valid, reportable outcome.

---

## 12. BUILD RECORD (Opus session, 2026-08-04) — INC-0..3 implemented, unit-gated

**Code.** `NetworkNormalEquations::blockResidualCofactor` (INC-0 kernel);
`include/filter/NetworkBlockBaarda.hpp` (header-only accumulator: ladder, σ̂₀,
labels, contingency, size histogram) + `src/core/filter/NetworkBlockBaarda.cpp`
(the per-station sweep, the UNCHECKABLE guard, the emission seam and the run-end
readout), CMake entry added; the FEED rides
`NetworkRedundancyCensus.cpp`'s swept branch (SPEC §4's "no new scheduler");
7 `network.block_baarda*` knobs + loader tripwires; `NetworkKnobs::bb_*`,
`NetworkCarryState::bb/bb_hits/bb_event_last`, `NetworkServiceCounters::bb`;
`network_block_baarda.csv` + `BLOCKBAARDA` events; run-end line in
`NetworkTeardown.cpp`; `tests/test_block_baarda.cpp` (24 cases, two suites).

### Two corrections to §3's recipe, both load-bearing

1. **§3's "the D5 v2 pattern verbatim" is wrong for b > 1.** `NetworkCarryVce`
   inverts the geometric mean of a **χ²₁** variate (`kChi2GeoMean = 0.2807297418`
   = 2e^ψ(1/2), hard-coded at `NetworkCarryVce.hpp:435`), but `T_B·b ~ σ₀²χ²_b`.
   Since `2e^ψ(b/2)/b` runs 0.281 / 0.561 / 0.763 / 0.877 at b = 1 / 2 / 4 / 8,
   feeding `√T_B` biases σ̂₀ by up to **1.8× in σ (3× in variance)**, and the bias
   MOVES with the constellation's band plan. The implementation feeds the
   χ²₁-equivalent `u_B = √(T_B·b·2e^ψ(1/2)/2e^ψ(b/2))` (`blockChi2GeoMean`), which
   is exact for any mix of b and leaves the b = 1 call byte-identical.
   Unit-gated: `Sigma0RecoversTheScaleAtEveryBlockSize`,
   `Sigma0IsUnbiasedOnAMixedBlockSizePopulation`.
2. **`seed_floor_m2` must be passed 1.0 explicitly.** The constructor default of
   0.0 is silently replaced by 1e-6, which would make an unwarmed family report
   σ̂₀ = 10⁻³ and inflate every `t_B` in it by 10⁶.

### Deviations from this spec, all deliberate

1. **`t_B` is a VARIANCE RATIO, not a σ**, and the geo column is not what §4
   implies. The ladder rungs are shared with the prior-residual census (§4's
   "census bins") so one reader handles both files, but that card's `|t|` is in σ
   units and this one's is not. Worse, the *geometric* mean of `t_B` under H₀ is
   `2e^ψ(b/2)/b` — b-dependent, so a raw geo-mean of a mixed-LOS population is
   unreadable. The emitted `geo_cal` column is therefore the **calibrated
   χ²₁-equivalent** geometric mean (1.0 = converged, b-invariant), matching what
   `NetworkPriorResidual::Ladder::geoT2` means there. Both facts are stated in
   the CSV header, the run-end line and the loader's arming line.
2. **The family partition is the CONSTELLATION**, not the prior-residual census's
   family enum — there is none (families are runtime strings), and its five names
   are CARRY families that mean nothing for a line of sight. Constellation is the
   only axis that can label a WHOLE block, because a block spans code AND phase
   rows by construction. Reuses `netMdbConstIndex`'s partition.
3. **UNCHECKABLE blocks are DROPPED-with-count, not pinv'd.** §3 licenses either.
   A pinv'd `T_B` has an unknown effective dof, so its ladder rung would mean
   nothing *and* it would pollute σ̂₀ for every honest block in its family.
4. **`RESPONDED` is an ESTIMATOR-side signal** (a pending-reseed ambiguity on that
   (station, satellite)), deliberately NOT the slip-QC raw verdict INC-2
   corroborates against — sharing a source would make §8.2's positive control
   circular. The seam order makes this readable and is load-bearing: SB_LEVELING
   clears the pending-reseed set, SB_RAIM then marks this epoch's refloats, and
   only then does SB_FRAMEDIAG read it.
5. **`networkRedundancyCensus` gained an `x_s` parameter.** `T_B` needs POST-FIT
   residuals and `StationAssembly::y` is the PREFIT innovation; nothing in the
   tree stores a post-fit residual (all three sites that form one are loop-local
   temporaries). An empty `x_s` disarms the rider rather than censusing the wrong
   quantity.
6. **No `network_epoch_status.csv` columns**, unlike the three sibling censuses.
   `network_block_baarda.csv` already carries n_blocks / n_reported / max_t per
   (family × label) per epoch — strictly more than pooled scalars would say.
7. **The size histogram is reported before any ladder.** A run whose blocks were
   all b = 1 produces a ladder identical to the scalar w-test and an INC-3
   fraction of 0 BY CONSTRUCTION; without the histogram that reads as "no
   smearing found" rather than "there was never a block to smear across".
8. **§2.2's cited loader line `GnssConfigLoadNetwork.cpp:3543-3548` is stale** —
   that is now an unrelated `ura_staleness_q2` clamp. The pattern followed is the
   prior-residual census's auto-arm. This card's own auto-arm sits INLINE with its
   knobs (it is parsed in `loadNetworkQcTiers_`, step 153, after the
   `gap_instrument` jval in step 152) rather than being split to the tail of the
   slip helper as that card's had to be.

### Unit gate (Debug)

24 cases across `BlockBaardaTest` (8, the INC-0 kernel) and
`BlockBaardaSweepTest` (16, the accumulator + loader). Full suite **1197 / 179
suites, all pass**, one pre-existing fixture-gated skip. Kernel cases include the
dense-GLS oracle, sub-block consistency, datum invariance and the smearing
demonstration; sweep cases include the χ²_b rescaling, the Gaussian-null ladder
against independently computed tail probabilities, the Student-t (ν = 3)
over-occupancy with a converged scale, label-partition sums, predict-then-update,
the UNCHECKABLE guard, INC-3's `block_only` accounting, INC-2's b-cell, and the
loader tripwires.

### What is NOT established, and is OWED

**No field run.** Every §8 gate is outstanding — OFF byte-identity on a real
lineage, live wiring, the cost measurement against §5's "≤ 2× the census sweep"
budget (`counters.bb.wall_ms` is the number to check), the RESPONDED-vs-CLEAN
positive control, the contingency b-cell, and INC-3's smearing fraction. Per §8
and the census's convergence-hygiene rule these must run on a **converged warm
window**: a free ambiguity zeroes its own arc's post-fit phase residuals, so a
cold start reports a large and STRUCTURAL uncheckable-block population that is
not a data finding.

**A prediction worth recording before the field run.** A design sweep over a
physical 3-station × 4-satellite toy (code+phase × 2 bands, iono/ambiguity priors
at 1 cm / 5 mm) measured the block tier's gain over the row tier, `T_B / max_i
w_i²`, as **fault-class dependent**: ≈ 1.0 for single-row faults AND for
LOS-common-mode faults (b = 4 pooling loses to the χ²₄ threshold), but ≈ 6× for
faults that violate the block's internal correlation — e.g. an unflagged
half-cycle slip on both bands, where every w-test in the network reads ≤ 2.66
against a 3.29 critical value while `T_B` = 43.7 against χ²₄(0.001) = 18.47. So
§8.4's `block_only` fraction should be driven by SLIP-like faults; if the swept
population is dominated by code multipath, expect ≈ 0 and the honest close §11
already licenses.
