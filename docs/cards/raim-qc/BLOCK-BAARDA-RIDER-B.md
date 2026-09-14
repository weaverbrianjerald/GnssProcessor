# Rider B — BlockFormal — the (station, sat) LOS block outlier census, `network.block_baarda`

**STATUS**

- **id:** Rider B
- **topic:** raim-qc
- **status:** closed
- **summary:** BlockFormal — the (station, sat) LOS block outlier census, `network.block_baarda`
- **dates in the text:** 2026-08-04
- **knobs / posture:** `network.block_baarda`, `network.block_baarda_report_sigma`, `network.block_baarda_corroborate`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_redundancy_census.md` — 12, 12.1, 12.2, 12.3, 12.4, 12.5, 12.6, 12.7, 12.8, 12.9, 12.12

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_redundancy_census.md -->
## 12. RIDER B — BlockFormal: (station, sat) LOS block outlier census (`network.block_baarda`)

*Absorbed from `docs/attic/SPEC_block_baarda.md` (SPEC'd + built 2026-08-04).  Increments namespaced
**BB-INC-0..3** (absorbed labels: that card's `INC-0..3`).*

**Origin**: NGS LASER Guide v2.7 slide 5-101 (PDF p. 413): Baarda data snooping at the 3D block level,
`OutlierDetectionMethod = BlockFormal`, formulas Snow & Schaffrin (2003), *GPS Solutions*
7(2):130–139, eqs. (13)–(18).  Slide 5-98 states the motivation: a single outlier is SMEARED across
residuals ("not necessarily a one-to-one correspondence between large residuals and outliers").

### 12.1 Why — the smearing gap between row tests and station polish

One physical fault (one LOS: multipath, a wet radome lobe, an unflagged slip) projects onto 2–8 rows —
code+phase × bands — each individually sub-threshold while the block is jointly loud.  Today's tiers
straddle exactly that unit: per-row w-tests below it, station-level median polish above it.  The
**78-station mass-refloat incident** was the textbook cost: the bad-STATION path was DISABLED rather
than confirmed because nothing could say *which* LOS carried the fault.  LASER's BlockFormal is the
middle tier, and its natural block here is the (station, sat) LOS across bands.

### 12.2 Binding precedent — the prior-residual census's D5 rules apply unchanged

1. **Empirical critical values, never tabular.**  Snow & Schaffrin's F(α, b, r) assumes Gaussian
   errors; D5 measured that false by orders of magnitude in the tail.  The deliverable is the empirical
   exceedance ladder of the block statistic per family.
2. **Corroboration in cycle one.**  Contingency table vs the slip-QC raw-observable verdicts ships in
   v1 (`gap_instrument` auto-arm).  ⚠ The SPEC cited the prior-residual census's loader tripwire at
   `GnssConfigLoadNetwork.cpp:3543-3548`; **that pointer is stale** (now an unrelated
   `ura_staleness_q2` clamp).  Current: the prior-residual census's tail tripwire is at
   `GnssConfigLoadNetwork.cpp:3296-3313`, and this card's own auto-arm sits INLINE with its knobs at
   `:3715-3733` (`loadNetworkQcTiers_` step 153, after the `gap_instrument` jval in step 152) —
   deviation 8.
3. **Response is a separate card.**  v1 wires nothing.

*(These are the rules of `docs/SPEC_prior_residual_census.md` §2 — "the census" in this rider always
means that card, never this host.)*

### 12.3 The statistic

For block B = the data rows of one (station, sat) this epoch, b = |B|:

```
T_B = ẽ_Bᵀ (Q_ẽ,B)⁻¹ ẽ_B / b          normalized block quadratic form
t_B = T_B / σ̂₀,f²                      empirically re-scaled (per family f)
```

`Q_ẽ,B` is the b×b block of the residual cofactor — the diagonal of which is what `rowRedundancy`
already returns.  BB-INC-0 extends the kernel: a sibling
`NetworkNormalEquations::blockResidualCofactor(station, rows)` returning the b×b from the same inputs
`rowRedundancy` takes (declared `NetworkNormalEquations.hpp:997`, defined
`NetworkNormalEquations.cpp:1743` — the SPEC's `NetworkNormalEquations.cpp:1690` has drifted;
station-local A_s/A_r rows + `cov_free` = `datumFrameSharedCov`, `NetworkEstimator.cpp:360`/`:438`).
Same math, off-diagonals kept, **b ≤ 8**.  Degenerate guard: if `Q_ẽ,B` is singular at the working
precision (a fully-leveraged block — the block analogue of r < ε), the block is UNCHECKABLE: counted,
identity kept, never inverted through (**pinv or drop — count either way; the census's
prefer-fallback-over-drop discipline**).

σ̂₀,f is a `NetworkCarryVce` instance fed `T_B` per family — the D5 v2 pattern
(`NetworkInvariantCarryDriver.cpp:1452-1455` precedent; the SPEC wrote
`NetworkInvariantCarryDriver.cpp:1453-1455`, and `:1452` is the `inflation_->addRow(` call head), same
predict-then-update and never-censor contracts.  ⚠ **"Verbatim" is WRONG for b > 1 — §14.2**, which
states both `NetworkCarryVce` corrections once for all three riders.

### 12.4 Design

- **BB-INC-0**: `blockResidualCofactor` kernel + unit tests against brute-force dense GLS.
- **BB-INC-1**: the sweep.  Blocks enumerated for SWEPT stations only — this rider rides the host's
  stride/targeted-escalation machinery (`NetworkRedundancyCensus.cpp:217`, `:443`) rather than adding a
  scheduler.  Per (family × label) exceedance ladders over `t_B` (census bins), n_blocks, worst-block
  identity.  Labels RESPONDED / CLEAN / FRAME exactly as the prior-residual census defines them; a
  block inherits the union of its rows' labels (any RESPONDED row ⇒ RESPONDED block; else any FRAME ⇒
  FRAME).
- **BB-INC-2**: corroboration join per block (slip-QC verdict on that (station, sat)), b-cell reported
  per family.
- **BB-INC-3**: the smearing measurement — **the card's unique deliverable**: among blocks with `t_B`
  above the report threshold, the fraction whose individual rows were ALL below the per-row threshold
  (`block_only` count).  That fraction IS the population invisible to every existing tier; **if it is
  ~0 the block tier adds nothing and that finding CLOSES the card honestly.**

### 12.5 Cost

Per swept station: b×b builds and inverts for its tracked sats, b ≤ 8 — bounded by the host's own
`rowRedundancy` cost class (6.1 ms/station-sweep measured).  Budget: **≤ 2× the census sweep it
rides**; report measured cost per station-sweep (`counters.bb.wall_ms`).  Stride inherited; no new
knob.  **UNMEASURED — no field run (§12.12).**

### 12.6 Config

`network.block_baarda` (master; **hard error** without `redundancy_census`, the sweep host),
`network.block_baarda_report_sigma` (trace/event threshold on `t_B`, reporting only),
`network.block_baarda_corroborate` (BB-INC-2; auto-arms `cycle_slip.gap_instrument` with a WARN).
**Types, defaults and dependencies: the one table in §14.7.**  **Seven `network.block_baarda*` knobs
landed in total.**  Loader `GnssConfigLoadNetwork.cpp:3643-3733`, next to the redundancy-census block.
Fingerprint-SAFE.  Invariant-#4 tripwires as in the host (zero-block families distinguishable from
unarmed).

### 12.7 Unit tests

`tests/test_block_baarda.cpp` — **24 cases across TWO suites**, the one file in the family that owns
two suite names: `BlockBaardaTest` (8, the BB-INC-0 kernel) and `BlockBaardaSweepTest` (16, the
accumulator + loader).  Full suite at the time: **1197 / 179 suites**, one pre-existing fixture-gated
skip.

Kernel cases: the dense-GLS oracle (`blockResidualCofactor` = brute-force dense
`(P⁻¹ − A N⁻ Aᵀ)_BB` to 1e-10, including a rank-deficient-datum case), sub-block consistency, datum
invariance, and **the smearing demonstration** — a fault split evenly across 4 rows of one block, each
row at 0.6× the per-row threshold: every w-test silent, `T_B` loud; and the converse, 4 independent
faults in 4 different blocks do NOT pool into any single loud block.  Sweep cases: the χ²_b rescaling
(`Sigma0RecoversTheScaleAtEveryBlockSize`, `Sigma0IsUnbiasedOnAMixedBlockSizePopulation` — §14.2), the
Gaussian-null ladder against independently computed tail probabilities, Student-t (ν = 3)
over-occupancy with a converged scale (the D5 failure mode preserved at block level), label-partition
sums, predict-then-update, the UNCHECKABLE guard, BB-INC-3's `block_only` accounting, BB-INC-2's
b-cell, and the loader tripwires.

### 12.8 Field validation — **ALL OWED**

Converged warm window (§9: a cold smoke establishes wiring and cost only).

1. OFF byte-identity; live wiring; cost within budget.
2. **Positive control**: RESPONDED-block ladder shifted vs CLEAN (same meaning and caveats as the
   prior-residual census's gate 3; **a null result is a publishable finding, not a tuning licence**).
3. **Contingency b-cell** per family (D5's was 97 % — expect the block statistic to do BETTER on
   code+phase joint evidence; **measure, don't assume**).
4. **The smearing fraction** (BB-INC-3): report `block_only / blocks_flagged`.  **This single number
   decides whether a response card is ever worth writing.**
5. Product claims on total_IF only; solve byte-identical in every arm.

### 12.9 Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| Any response (deweight/refloat/exclude per block) | **INERT** — measure-only; the response card's design input is §12.8 gate 4 (the absorbed card's §8.4) | — |
| Leave-one-station-out solution separation (would re-arm the bad-STATION path) | **INERT** — own card; additive normals make it cheap but it is solve-adjacent | DIA catalog item 5 |
| Cross-band prior-row blocks (carry families) | **INERT** — the prior-residual census's scalar ladder covers priors in v1; block form needs the same `Q_ẽ,B` extension on fold rows | `docs/SPEC_prior_residual_census.md` |
| Snow & Schaffrin's exact F reference distribution | **INERT** — empirical ladder by rule §12.2.1 | D5 history |

*(Pre-flight and does-NOT-do: §10 — this card's cited `§10.4` is §10.4 there.)*

### 12.12 BUILD RECORD (Opus session, 2026-08-04) — BB-INC-0..3 implemented, unit-gated

**Code.** `NetworkNormalEquations::blockResidualCofactor` (BB-INC-0 kernel);
`include/filter/NetworkBlockBaarda.hpp` (header-only accumulator: ladder, σ̂₀, labels, contingency, size
histogram) + `src/core/filter/NetworkBlockBaarda.cpp` (per-station sweep, UNCHECKABLE guard, emission
seam, run-end readout), CMake entry added; the FEED rides `NetworkRedundancyCensus.cpp`'s swept branch
(§12.4's "no new scheduler"); 7 `network.block_baarda*` knobs + loader tripwires; `NetworkKnobs::bb_*`,
`NetworkCarryState::bb/bb_hits/bb_event_last`, `NetworkServiceCounters::bb`;
`network_block_baarda.csv` + `BLOCKBAARDA` events; run-end line in `NetworkTeardown.cpp:1079`;
`tests/test_block_baarda.cpp` (24 cases, two suites).  **The two corrections to §12.3's recipe are in
§14.2** — inherited by rider C and stated once there.

#### Deviations from this spec, all deliberate

1. **`t_B` is a VARIANCE RATIO, not a σ**, and the geo column is not what §12.4 implies.  The ladder
   rungs are shared with the prior-residual census ("census bins") so one reader handles both files,
   but that card's `|t|` is in σ units and this one's is not.  Worse, the *geometric* mean of `t_B`
   under H₀ is `2e^ψ(b/2)/b` — b-dependent, so a raw geo-mean of a mixed-LOS population is unreadable.
   The emitted `geo_cal` column is therefore the **calibrated χ²₁-equivalent** geometric mean (1.0 =
   converged, b-invariant), matching `NetworkPriorResidual::Ladder::geoT2`.  Stated in the CSV header,
   the run-end line and the loader's arming line.
2. **The family partition is the CONSTELLATION**, not the prior-residual census's family enum — there
   is none (families are runtime strings), and its five names are CARRY families that mean nothing for
   a line of sight.  Constellation is the only axis that can label a WHOLE block, because a block spans
   code AND phase rows by construction.  Reuses `netMdbConstIndex`'s partition.
3. **UNCHECKABLE blocks are DROPPED-with-count, not pinv'd.**  §12.3 licenses either.  A pinv'd `T_B`
   has an unknown effective dof, so its ladder rung would mean nothing *and* it would pollute σ̂₀ for
   every honest block in its family.
4. **★ `RESPONDED` is an ESTIMATOR-side signal** (a pending-reseed ambiguity on that
   (station, satellite)), deliberately NOT the slip-QC raw verdict BB-INC-2 corroborates against —
   sharing a source would make §12.8 gate 2's positive control **circular**.  The seam order is
   LOAD-BEARING: SB_LEVELING clears the pending-reseed set, SB_RAIM marks this epoch's refloats, and
   only then does SB_FRAMEDIAG read it.
5. **`networkRedundancyCensus` gained an `x_s` parameter.**  `T_B` needs POST-FIT residuals and
   `StationAssembly::y` is the PREFIT innovation; nothing in the tree stores a post-fit residual (all
   three sites that form one are loop-local temporaries).  An empty `x_s` **DISARMS** the rider rather
   than censusing the wrong quantity.
6. **No `network_epoch_status.csv` columns**, unlike the three sibling censuses.
   `network_block_baarda.csv` already carries n_blocks / n_reported / max_t per (family × label) per
   epoch — strictly more than pooled scalars would say.
7. **★ The size histogram is reported BEFORE any ladder.**  A run whose blocks were all b = 1 produces
   a ladder identical to the scalar w-test and a BB-INC-3 fraction of 0 **BY CONSTRUCTION**; without
   the histogram that reads as "no smearing found" rather than "there was never a block to smear
   across".
8. **§12.2's cited loader line was stale** — recorded there; the pointer is fixed in place above.  The
   pattern followed is the prior-residual census's auto-arm, but this card's own auto-arm sits INLINE
   with its knobs rather than split to the tail of the slip helper.

#### What is NOT established, and is OWED

**No field run.**  Every §12.8 gate is outstanding, on a **converged warm window** (§9) — enumerated
once in §15 row 4.

#### ★★ THE PRE-REGISTERED PREDICTION, recorded before the field run

A design sweep over a physical **3-station × 4-satellite toy** (code+phase × 2 bands, iono/ambiguity
priors at 1 cm / 5 mm) measured the block tier's gain over the row tier, `T_B / max_i w_i²`, as
**FAULT-CLASS DEPENDENT**: **≈ 1.0** for single-row faults **AND** for LOS-common-mode faults (b = 4
pooling loses to the χ²₄ threshold); **≈ 6×** for faults that violate the block's internal correlation
— e.g. an unflagged half-cycle slip on both bands, where **every w-test in the network reads ≤ 2.66
against a 3.29 critical value while `T_B` = 43.7 against χ²₄(0.001) = 18.47**.  So §12.8 gate 4's
`block_only` fraction should be driven by **SLIP-like** faults; if the swept population is dominated by
code multipath, expect ≈ 0 and the honest close §10.8 already licenses.

---

