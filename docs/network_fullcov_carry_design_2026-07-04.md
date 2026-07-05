# C1 — Full shared-block covariance carry (`network.shared_carry_full_cov`)

2026-07-04.  Design for the last structural card of the network enhancement
queue: replace the DIAGONAL cross-epoch carry of the shared border (per-key
value + variance, `CarryPrior` / `prev_carry`) with a FULL dense covariance
carry, converting the epoch loop into an information-form Kalman filter on the
shared border.  Root motivation: the diagonal stash discards every
cross-covariance the solve produced — most damagingly the strong
clock↔phase-bias off-diagonals (the "data wants ~80 m" split) — which makes
stiff carries statistically inconsistent (information accumulates ~n against a
correlated error floor → formal variance → 0 → lock-in; the QC zoo exists to
compensate).  See `project_network_ssystem_dia_assessment` finding #1.

## Scope

- Dense block covers the D2-carried kinds only: **Clock (scalar-carry mode),
  PhaseBias, CodeBias**.  SH-VTEC stays on its own diagonal pre-gate fold
  (separate physics: Kaula/climatology-blended, folded before the rank gate);
  under `network.clock_model` the clock stays on the two-state `clock_osc`
  path and is excluded from the dense block exactly as it is excluded from
  the diagonal stash today.
- Kernel (`NetworkNormalEquations`) untouched: `addSharedInfo` already accepts
  a full dense symmetric increment and carries it through the Schur reduction
  exactly, including the post-reduce in-place fast path (reduce-once invariant
  preserved).
- Datum, QC tiers, emitter, product, replay: untouched.
- Default OFF; byte-identical when OFF (legacy fold/stash code paths kept
  textually intact behind the branch).

## Math

Stash (emit epochs, after the final solve): `P = est.sharedCovariance()`
restricted to the carried, non-datum-pinned columns, aligned with the
`prev_carry` entries it belongs to.  Fold (next epoch, post-gate, pre-solve):

```
P_eff = P[S, S]                    S = surviving keys (see marginalization)
P_eff(i,i) := var_eff_i            var_eff exactly as the diagonal path:
                                   inflate_q → clk_mon_reinit_var (row/col
                                   decorrelated), else var + q_kind·dt then
                                   B4-floored — identical per-key scalars
Λ = P_eff⁻¹        (LDLT in correlation scaling, ridge retry, PD-guarded)
η = Λ · v_S        v = carried values (eph-cutover re-level already applied)
```

folded as one dense `addSharedInfo` block.  Since the diagonal of the stashed
`P` equals `cp.var` by construction, "set diag to var_eff" IS the Q-addition +
floor of the legacy path; off-diagonals carry unchanged (independent process
noise — the standard RW assumption).

**Marginalization, not conditioning.**  Keys absent from the new epoch's
layout, datum-pinned this epoch, `handover_affects`-retired, or clk-model
clocks are removed by deleting their ROWS/COLS OF P before inversion — exact
marginalization.  Deleting rows of Λ instead would condition on the departed
keys and bias every survivor.  This is the load-bearing reason the estimator
API takes a covariance, not an information matrix.

**Exactness.**  With an unchanged layout and q = 0 the carried recursion is
exact recursive least squares: epoch-2 posterior == the batch solve stacking
both epochs' station data (station locals distinct per epoch, shared border
common).  Holds INCLUDING identical-value datum pins: the pin's Dirichlet
elimination moves `N_fp·v_pin` to the RHS each epoch and the carried
`η = Λ·x1_f = u1_f − N1_fp·v_pin` reproduces the batch RHS term-for-term.
Unit test `DenseCarryRecursiveEqualsBatch` asserts this; the diagonal carry
provably fails it (off-diagonal information discarded).

## Estimator API (the one new seam)

```cpp
std::size_t NetworkEstimator::addSharedPriorDense(
    const std::vector<SharedPrior>& keys,     // identity + mean; .info IGNORED
    const Eigen::MatrixXd& cov,               // k×k prior covariance over keys
    Eigen::VectorXd* lambda_diag = nullptr);  // out: folded Λ(c,c) per input key
```

Semantics: map keys with the same `key_`/`makeVtecShKey` construction as
`addSharedPrior`; drop absent keys by subsetting `cov` (marginalization);
symmetrize; invert in correlation scaling (`D⁻¹ C⁻¹ D⁻¹`, `D = √diag`) via
LDLT with a relative-pivot guard and one `+1e-9·I` ridge retry on the
correlation matrix; symmetrize Λ; scatter Λ and `Λ·v` into the n×n increment;
forward to `neq_.addSharedInfo`.  Throws `std::runtime_error` on a rejected
block — the DRIVER owns the fallback policy.  Guarded `epoch_open_ &&
!solved_` (new API only; the legacy `addSharedPrior` deliberately keeps no
guard).  `lambda_diag` (aligned with the INPUT key vector, 0 for dropped keys)
feeds the B7 anchor-info trace.

**Conditioning guards (adversarial-review hardening).**  Because this factor
is INVERTED and folded as information, an admitted condition number becomes
information amplification (`Λ_aa = 1/(P_aa(1−R²_a))`).  Two guards bound it,
both routing to the driver's diagonal fallback via `runtime_error`:
(1) the correlation factor's relative pivot must satisfy `dmin ≥ 1e-6·dmax`
(NOT the kernel's forgiving 1e-12) even after the ridge — capping relative
amplification at ~1e6; (2) an absolute folded-information budget
`max diag(Λ) ≤ 1e13 1/m²` keeps the downstream constrained solve's own
relative-pivot floor (`1e-12·dmax`) at or below ~10 m⁻², under the weakest
genuine reduced pivots.  Without these, a near-gauge carried direction (the
finite 1e3-weight gauge rows leave data-free directions whose info the carry
re-imports while the epoch re-adds the rows) could push the solve to throw —
and a solve-throw skips the epoch BEFORE the stash, so the identical block
would re-fold every epoch (persistent skip loop; confirmed-major review
finding).  The rejected-epoch fallback folds the same keys diagonally (legacy
magnitudes, known-safe) and the next emit re-stashes a fresh block —
self-healing.  With the B4 floors armed (the recommended C1 operating point;
the card's "re-baseline B4 after" note) the budget is unreachable.

## Driver changes (`ApplicationNetwork.cpp`)

- `struct SharedCarryCov { std::map<std::string,size_t> row; Eigen::MatrixXd P; }`
  beside `CarryPrior`; instance `prev_carry_cov` beside `prev_carry`.
  `prev_carry` stays the single source of truth for values/flags — every
  existing mutation site (eph-cutover re-level, warm-resume restore) works
  unmodified; the matrix is a passive identity-keyed side-car.
- **Fold site** (post-gate, same location): `if (!fullcov_on) { legacy block
  verbatim } else { … }`.  ON path walks `prev_carry` with the IDENTICAL skip
  rules and per-key `var_eff` computation, splitting entries into (a) the
  dense set — has a `prev_carry_cov` row — and (b) leftovers (no matrix row,
  e.g. first fold after warm resume) folded through the legacy diagonal
  `addSharedPrior` with `info = 1/var_eff`.  Dense set: assemble `cov_eff`
  (subset P; decorrelate + reinit-diag the `inflate_q` rows; diag :=
  `var_eff`), then `try { addSharedPriorDense } catch { fold the same keys as
  diagonal priors with 1/var_eff — the legacy semantics — plus a warn +
  `D2_DENSE fallback_diagonal` event }`.  Self-healing: the next emit
  re-stashes a fresh P.
- **RAIM re-solve replay**: capture what was ACTUALLY folded — dense success →
  `cap_dense_keys/cov` replayed via the same call (deterministic, layout
  frozen across `beginEpoch()` reopens); fallback/leftover priors → appended
  to `cap_priors` as today.
- **Stash site**: legacy stash loop unchanged (fills `prev_carry`); when ON the
  stash lambda additionally records `(key, column)` for non-SH kinds, and one
  O(k²) block copy of `est.sharedCovariance()` builds `prev_carry_cov` after
  the loops.  Whole-block finite screen: any non-finite → clear the matrix +
  warn (next fold degrades to the leftover/diagonal path; never poisons the
  solve).
- **B7 trace**: dense keys log `Λ(c,c)` from `lambda_diag`; leftovers/fallback
  log `1/var_eff` (unchanged meaning: carry info on the column).

## Interactions considered

- **Sum-to-zero clock datum**: no clock pins → all clock columns stash with
  the gauge-direction variance the constraint rows imposed; re-folding adds
  bounded gauge-direction stiffness (fixed-point, since q_clock·dt floors the
  prior variance each epoch) and the carried mean satisfies the re-imposed
  constraint.  Benign; watched in the ON validation runs (datum drift metric).
- **Warm resume / snapshot**: format stays v1 (diagonal `D` records).  After a
  resume the matrix is empty → first fold runs the leftover path (== legacy
  diagonal), then dense resumes at the next emit.  Because carried values and
  variances mean the SAME thing in both modes, the toggle is deliberately NOT
  added to the warm-resume config fingerprint — a flip degrades gracefully
  instead of forcing a cold start.
- **Withheld epochs**: stash is emit-only, `dt` spans the last interval only —
  pre-existing under-inflation replicated for parity (not "fixed" here).
- **Quarantine / carve-out**: both prune `sol.sats` only; the dense stash, like
  the diagonal one, stashes everything registered (parity in fault scenarios).
- **pdp anchor handovers**: a re-leveled (sat,signal) fights its carried prior
  exactly as it does under the diagonal carry; the emitter's 0.05 m jump
  detector still signals.  Dense adds no new class of coupling (the S1
  coverage-threshold card remains the fix for thin-pool re-levels).

## Follow-ups (out of scope, tracked on the queue)

- Re-baseline B4 floor sizing with the dense carry active (card note).
- C2 handover S-transform on the carried block (dense makes the exact
  old→new datum map applicable to the covariance too).
- C5 carry-manager unification; Schmidt-consider states now meaningful.
- Snapshot v2 dense-block record if warm-resume fidelity ever matters more
  than the one-epoch diagonal degrade.
