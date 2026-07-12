# CARD 4 phase C3b — live-driver handoff (Opus builds, Fable gates)

*Fable-specced 2026-07-11, after the phase-C-core gate PASS (committed
`1e675e7`: `foldReducedDdPriors` rank-K per-component Woodbury fold +
`NetworkEstimator::setDdPriors`/`ddPriorPosteriorMean` + gate-4 basis-invariance;
see memory `project-m1-card4-phasec`).  C3b = wire the phase-B z-store
(`NetworkZStore.hpp`) and the phase-C fold into the live ApplicationNetwork
epoch loop.  Parent spec: `docs/HANDOFF_card4_invariant_carry.md` (D1/D2/D6/D7);
adjudicated algebra: `docs/HANDOFF_card4_phaseA_scout.md`.*

## 0. Hard constraints inherited from the phase-C review (non-negotiable)

- **Phase datum under `carry_invariant` = PRE-reduce tree pins via
  `addStationPrior`** (into `N_rr` before the reduce → composes exactly with the
  post-reduce DD fold; proven `FoldReducedDdPriorsComposesWithStationPrior`).
  The sum-to-zero ensemble gauge is SUPERSEDED: `NetworkEstimator::solve`
  THROWS if both `phase_sumzero_gauges_` and `dd_priors_` are set.  The driver
  must not set the ensemble gauge on carry-invariant epochs.
- **The tree-diff/delta driver must be a FACTORED, unit-drivable component**
  (named function/class; friend-shim per docs/test_suite.md if internals are
  needed).  The C3b gate tests must invoke that exact symbol, and the
  ApplicationNetwork epoch loop must call the same one.  A test-local
  reimplementation of the election-diff or the vertex-id mapping fails the gate.
- **Legacy PhaseBias-kind carry bypass**: with `carry_invariant` ON, the D2/C1
  shared PB carry consume and the phase-leveling local carry as SOLVE inputs
  must be gated OFF for phase keys (spec D2/D6) — otherwise the ON path injects
  the same phase information twice.  If deferred, state explicitly that ON is
  not field-runnable and keep all ON-path configs legacy-carry-disabled.

## 1. The Pᶻ posterior — RESOLVED (build this; do not re-derive at build time)

Phase C left only the posterior MEAN readout.  The posterior covariance has a
closed-form kernel readout from quantities already stored per folded block
(`FoldedDdPrior`: G, M, Pz) plus the datum-constrained shared covariance
`Σ_xs` the estimator already computes (`shared_cov_`):

```
Pᶻ⁺ = Pᶻ − Pᶻ M Pᶻ + (Pᶻ M Gᵀ) Σ_xs (G M Pᶻ),      M = (Pᶻ + B)⁻¹
```

Derivation sketch (verified by Fable during the phase-C gate): the (L,L) block
of the augmented-system inverse gives C Σ_L C ᵀ = (B − B M B) + V Σ_xs Vᵀ with
V = C N_LL^aug⁻¹ N_Ls = Pᶻ M Gᵀ, and B − B M B = Pᶻ − Pᶻ M Pᶻ after the
Woodbury identities.  Sanity limits: Σ_xs = 0 ⇒ Pᶻ⁺ = Pᶻ(Pᶻ+B)⁻¹B (the parallel
sum — information addition (Pᶻ⁺)⁻¹ = Pᶻ⁻¹ + B⁻¹, where B = the free DD
covariance conditional on the border); Pᶻ → 0 ⇒ Pᶻ⁺ → 0 (a hard prior stays
hard).  New kernel method, mirrors the mean readout:

```
Eigen::MatrixXd ddPriorPosteriorCov(std::size_t block_index,
                                    const Eigen::MatrixXd& shared_cov) const;
```

**Measured / unmeasured partition (prediction-only chords).**  Per component,
only chords whose arc AND whole fundamental cycle have live ambiguity columns
this epoch are measurable.  Fold ONLY the measured sub-block: marginal
(P_mm, ẑ_m) and C_m.  After the solve, read back ẑ_m⁺ (existing
`ddPriorPosteriorMean`) and P_mm⁺ (`ddPriorPosteriorCov`), then propagate to
the unmeasured chords by Gaussian conditioning through the CARRIED
cross-covariance — exact because this epoch's data touches z only through z_m
(z_u's columns do not exist in this epoch's system):

```
ẑ_u⁺  = ẑ_u  + P_um P_mm⁻¹ (ẑ_m⁺ − ẑ_m)
P_um⁺ = P_um P_mm⁻¹ P_mm⁺
P_uu⁺ = P_uu − P_um P_mm⁻¹ (P_mm − P_mm⁺) P_mm⁻¹ P_mu
```

⚠️ Do NOT build the tempting "effective measurement" route (feed
w = z_free − Gᵀx̂_s with R = B + GᵀΣ_xs G into `ZComponent::update`): x̂_s from
the FOLDED solve is prior-contaminated, so that (w,R) double-counts the prior.
The kernel-readout + conditioning recipe above is the exact one-solve path.

**Datum invariance of (ẑ⁺, Pᶻ⁺) is a GATE ASSERTION** (they are estimable), not
an assumption — assert it in the C3b DatumFreedom test under two datums.

## 2. Driver scope (per epoch, per (const,σ) component)

1. Diff last epoch's z-store tree vs this epoch's eq34 election
   (ApplicationNetwork ~4180–4500 = the F_k selector; election machinery
   unchanged).  Apply in deterministic order: pivots (tree-edge death,
   transform BEFORE graph mutation) → kills → splits (cut-edge death: driver
   re-derives the partition; each successor carries its exact marginal
   sub-block) → merges (block-identity, ascending canonical bridge key) →
   births (tap-off).  Canonical component id = min sat LABEL (per-epoch only);
   ArcKey receiver vertices = STABLE per-run receiver ids, never pool slots.
2. Slip integration: refloat = killChord + re-tap; slip on a tree edge =
   pivot first, then refloat.
3. Build one `DdPriorBlock` per component from the MEASURED chords (marginal
   P_mm, ẑ_m); C entries derived FRESH each epoch from that epoch's h2local —
   no cached local column indices across epochs (the Card-3 collision class).
4. `setDdPriors` → solve (phase datum = addStationPrior tree pins, targets 0)
   → harvest ẑ_m⁺/P_mm⁺ → conditioning update for unmeasured chords → store.
5. Multi-birth epochs (incl. epoch-1 bootstrap: empty store, fold is a
   structural no-op): newborn chords need their MUTUAL cross-covariance
   harvested as a JOINT block (per-chord columns against old z only is wrong
   whenever cycles share tree edges — they always do).  Tap-off values from
   the solve covariance (jointConditionalCovariance requests).
6. GLONASS guard: cons=='R' groups never enter the z-store (guard + one-time
   WARN) — FDMA DDs are λ-heterogeneous; IE-FDMA is v2 (spec OPEN-3).
7. Config (spec D7): `network.carry_invariant` (false; throw if
   !phase_datum_eq34), `carry_invariant_qz` (0.0), `carry_invariant_honesty`
   (false); `carry_invariant_emit_rebase` belongs to phase D.  Warn-INERT:
   pin_round, carry_shift, rpb_q.  Getters + jval + isLoggerEnabled + summary
   line per repo conventions.
8. Counters (D5): z-dim per component, chords born/died, tree swaps, tap-offs,
   prediction-only tally (driver-side), merges/splits — one run-end line.
   Lifecycle unit tests assert EXACT counter values for scripted sequences.
9. Determinism: ordered containers keyed on canonical ids; no new OpenMP in
   driver/fold/harvest.

## 3. The C3b gate (Fable runs it; pre-registered)

- **Production-seam DatumFreedom**: two arms = FULL production runs (assembly →
  reduce → fold → solve → harvest → z-store), byte-identical obs+elections
  through epoch k−1 (stores asserted identical entering k), divergence at k
  induced ONLY via the election-selector input; assert tree_swaps ≥ 1 in the
  divergent arm on an informative store; compare estimable quantities + ẑ⁺/Pᶻ⁺
  through the independent inter-basis T (never from pivot() returns); NO
  setEstimate/zMutable seeding inside arms.
- ẑ⁺/Pᶻ⁺ datum-invariance under two datums (§1).
- Lifecycle unit cases: split, id-flip (verbatim carry), station dropout,
  multi-birth mutual block vs dense reference, bootstrap epoch, RAIM re-solve
  (refold from UNCHANGED predicted z; harvest once, post-acceptance).
- ON-path bypass evidence (PB-carry consume == 0 on an ON epoch) or explicit
  deferral.
- OFF byte-identity (structural review + suite), config-parse regression,
  suite green (664+ baseline).
