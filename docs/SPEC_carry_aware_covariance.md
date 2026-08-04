# SPEC — carry-aware (rank-K) covariance readback

**Status**: **BUILT and unit-gated (2026-08-03); NO field arm run yet.** INC-0/1/2 landed
under `network.carry_aware_covariance` (default OFF, NOT fingerprinted — §6); §7's tests
are `tests/test_carry_aware_covariance.cpp` (`CarryAwareCovarianceTest`, 6 cases) plus the
loader gate `CodeBiasNetworkConfig.CarryAwareCovarianceContract`. Unit suite 1136 cases /
175 suites, 1135 pass + the one fixture-gated skip. Implementation record in §11.

**The defect's cost is already measured on existing data, before any arm runs.**
`scripts/grade_ddfrac_sigma_conditioned.py` on `runs/pdfix_v3` (the P1 winner, 3 h): of
4865 DDs in the final hour, **every single one reports σ > 1 cycle, median 6.02 cyc**, and
`med|frac|` is 0.2492 / `P(<0.1)` 0.2016 / `circR` 0.0106 — uniform to three decimals.
At σ = 6 cycles a uniform fractional part is *mathematically forced*, so **P6(b)'s
six-hour null could not have returned anything else**, and the statistic carried no
information about integers at all. That is the case for this card in one line.

This is the follow-on `docs/SPEC_pd_safe_dd_fold.md` §8 names first:

> **Honest covariance readback**: `jointConditionalCovariance` /
> `reconstructStationCovariance*` carry no dd-prior term (only the mean κ path
> exists, NNE :1111 vs :1218) — `dd_sigma_cyc` and the AR probe stay carry-blind
> until the rank-K correction card lands.

**Memory context**: `project_pd_safe_fold_card` (the parent card — its §7 grading
deliberately avoided σ for exactly this reason), `project_network_ddfrac_rootcause_shvtec`
(the DD-fraction trail this unblocks), `project_dd_spillway_rootcause` (the same
Woodbury algebra, one layer down).

**Regime for all arms**: 63 sta, DOY-129, GPS, 30 s, cold, the `_ft63_pdfix_v3.json`
posture (the P1 winner — the current best substrate), so every number is comparable to
`runs/pdfix_v3` and `runs/pdfix_best6h`.

---

## 1. Why this card matters more than "a σ column gets better"

The strongest negative result in the whole DOY-129 campaign is P6(b)
(`docs/RUNQUEUE_pdfix_reask.md`): six hours, a working fold, 55-epoch chord
lifetimes, GPS+Galileo, and `med|frac|` is **dead flat at 0.246–0.254** — the
uniform-distribution value — in every hour and every age bin. It is recorded as
"the DD integer information is NOT PRESENT at this cadence/geometry".

**That conclusion is currently untestable, and this card is what makes it testable.**
A uniform fractional part is the EXPECTED outcome whenever σ_DD ≳ 0.3 cyc, whatever
the underlying truth: wrap a Gaussian of σ = 0.3 cyc around the unit circle and the
fractional part is already indistinguishable from uniform. So `med|frac| = 0.25`
separates "no integer information exists" from "integer information exists but this
estimator's DD uncertainty is too large to expose it" ONLY when read against a valid
σ_DD. The only σ the system emits — `dd_sigma_cyc` — is computed from a covariance
that does not contain the carry. It has sat pinned near ~3.9 cyc for the entire
campaign for exactly that reason, and the parent card's grading rules therefore
forbid using it (`scripts/grade_pdfix_arms.py:21`: "`dd_sigma_cyc` is absent ON
PURPOSE — it is carry-blind").

So the payoff is not cosmetic. It is: **condition `med|frac|` on a σ that is
allowed to be small, and the six-hour null either survives on the σ < 0.25 cyc
subpopulation (a real, quantified negative) or it does not (the whole
integer-information axis re-opens).** Nothing else in the queue can decide that.

## 2. The defect (derived from the code, not inferred)

The producer solves a Schur-reduced system. The carried DD prior enters through
`foldReducedDdPriors` (`src/core/filter/NetworkNormalEquations.cpp:818`) as a
per-block rank-K Woodbury update on the PRISTINE per-station factors:

```
  C_b        the block's DD rows over local ambiguity columns  (K_b × Σ n_local)
  Y_b        = N_rr⁻¹ C_bᵀ                    (per station: st.Nrr_ldlt.solve(CrT), :946)
  B_b        = C_b N_rr⁻¹ C_bᵀ                (:947)
  G_b        = N_sr N_rr⁻¹ C_bᵀ = N_sr Y_b    (:948)
  M_b        = (P_b + B_b)⁻¹                  (:959-961)
  reduced_N_ss += Σ_b G_b M_b G_bᵀ            (:985)
  reduced_u_s  += Σ_b G_b M_b (z_free − ẑ)    (:986)
```

That fold is EXACTLY the Schur complement of

```
  Ã⁻¹ := N_rr⁻¹ − Σ_b Y_b M_b Y_bᵀ           (the fold's implied local inverse)
```

because `N_sr Ã⁻¹ N_rs = N_sr N_rr⁻¹ N_rs − Σ_b G_b M_b G_bᵀ`. The MEAN path already
reads that model back: `reconstructStation` (:1359-1376) subtracts
`N_rr⁻¹ C_bᵀ κ_b` with `κ_b = M_b (z_free − G_bᵀ x_s − ẑ)`, which is
`Ã⁻¹(u_r − N_rs x_s) + Y_b M_b ẑ` written out. **The COVARIANCE path does not.**
All four readbacks use the prior-free `N_rr⁻¹`:

| function | line | what it computes | what it should compute |
|---|---|---|---|
| `reconstructStationCovariance` | :1393 | `N_rr⁻¹ + (N_rr⁻¹N_rs) Σ_ss (·)ᵀ` | `Ã⁻¹ + (Ã⁻¹N_rs) Σ_ss (·)ᵀ` |
| `reconstructStationCovarianceDiag` | :1417 | per-column diagonal of the same | same, corrected |
| `stationRegression` | :1452 | `(N_rr⁻¹)_{rr}` and `N_sr N_rr⁻¹ e_r` | `(Ã⁻¹)_{rr}` and `N_sr Ã⁻¹ e_r` |
| `jointConditionalCovariance` | :1476 | `m_aᵀΣ_ss m_b` + `(N_rr⁻¹)_{ab}` same-station only | + the cross-block term, INCLUDING across stations |

Meanwhile **Σ_ss IS carry-aware**: `NetworkEstimator::solve` takes
`N = neq_.reducedSharedNormals()` at `NetworkEstimator.cpp:259` — AFTER the fold at
:249 — and `shared_cov_` is the inverse of that datum-constrained matrix (:300).

So today's readback is a **hybrid that is the covariance of no estimator at all**:
carry-aware in the shared block, carry-blind in the local block, and carry-blind in
the regression that couples the two. That is the same class of defect as the parent
card's §11 (a readout that cannot distinguish working from silently degraded) —
here the readout is not merely uninformative, it is internally inconsistent.

**Direction and size.** The first term is provably tighter (`Ã⁻¹ ≼ N_rr⁻¹` whenever
the block-diagonal fold is exact — see §3), the second is not sign-definite, so the
net direction is EXPECTED down but not guaranteed. Grade it, do not assume it. The
size on the DD directions is the whole point: the prior variance on a mature chord
is O(1e-4 m²) against a diffuse `fresh_prior_var` of 1e6, so on precisely the
directions the carry is about, the correction is orders of magnitude.

## 3. The precondition, and the thing this card measures for free

`Ã⁻¹` is the exact inverse of `N_rr + Σ_b C_bᵀ P_b⁻¹ C_b` **only when the
cross-block coupling `B_{bb'} = C_b N_rr⁻¹ C_{b'}ᵀ` vanishes**, i.e. when no station
is touched by two blocks. **It does not vanish here, and the violation is structural,
not incidental** — three independent sources, all measured:

1. Blocks are vertex-disjoint only WITHIN one carry group (union-find over
   `(rx_vertex, sat_vertex)`, `NetworkInvariantCarryDriver.cpp:554-576`), and a group
   is one `(constellation, signal)` pair (`NetworkDdPriorBuild.cpp:669`). A station
   tracking two signals is therefore in both groups' blocks —
   `runs/byteid_absent/logs/main_log.log:172-176` shows **5 simultaneous groups** on a
   50-station run.
2. The §6.1 lane EQUIVALENCE shape deliberately emits **two blocks over the same
   arcs** (`NetworkInvariantCarryDriver.cpp:948-973`, whose own comment names the
   dropped `B₁₂`).
3. The WL/EWL/hold layer appends **one extra block per source carry block** into the
   same fold call (`NetworkMwWlGlue.cpp:1017-1020`).

And the precondition is stated too weakly where it is stated at all:
`NetworkMwWlLayer.hpp:49-61` says blocks must not "share local columns", but `N_rr⁻¹`
is dense — the station's clock, ZWD, per-satellite slant iono and every ambiguity are
one block (`ApplicationNetworkAssembly.cpp:193-198`) — so sharing a **station** is
already sufficient to couple them. `SPEC_lane_carry_card.md:176-178` records the
weaker inference as an "expected yes".

The kernel already knows this hazard by name in the OTHER composition —
`NetworkEstimator.cpp:226-242` REFUSES to compose the DD fold with the sum-to-zero
phase gauge because "both are post-reduce Woodbury folds on the same local block;
composing them is inexact… Fail fast rather than bias." DD-vs-DD has no such guard:
`foldReducedDdPriors` validates dimensions only. So `Σ_b G_b M_b G_bᵀ` is a
**block-Jacobi truncation** of the joint middle inverse, running in every network arm
ever graded.

Three consequences, and the third is a genuine deliverable:

1. **The correction must be self-consistent with the fold, not with an idealized
   model.** `Ã⁻¹` is the right target precisely BECAUSE it is what the solve used.
   Reporting the covariance of a model the estimator did not solve would re-create
   the hybrid this card exists to remove. Say it plainly wherever the knob is
   documented: this readback is honest **with respect to the fold that ran**, not with
   respect to the true posterior — otherwise the next reader re-runs this
   investigation.
2. **The truncation must be IDENTICAL on the mean and the covariance.** The mean
   already uses per-block `M_b` (NNE:1370); the correction uses the same `fd.M`. Never
   make one joint and the other per-block — a mean and a covariance from two different
   models is the defect, restated.
3. **`Ã⁻¹` is not guaranteed PSD.** Each block's downdate is individually bounded,
   but the SUM can over-subtract when cross-block coupling is strong relative to the
   priors (`λmax(blockdiag(M)^{1/2} B blockdiag(M)^{1/2}) > 1`). Nothing in the
   system has ever screened that. The correction's own PD guard (§5b) is therefore
   **the first measurement of the fold's cross-block approximation error** — a
   counter that has never existed, on an approximation that has been running in
   every network arm ever graded. Expect it to fire, if anywhere, on stations
   carrying a pair-group block plus a residual per-band block plus WL blocks.

## 4. INC-0 — say which covariance it is (always-on, solve-neutral, lands FIRST)

§11 of the parent card is the lesson: a number published without its state is an
instrument that lies. `dd_sigma_cyc` has been published for the whole campaign with
no field saying which covariance produced it.

- `network_dd_fractional.csv` gains a trailing column `sigma_carry_aware` (0/1),
  APPENDED (readers read by name). Same for the AR probe's log line: it names the
  covariance mode explicitly.
- One run-end line, always: the readback mode, the number of carried DD rows the
  readback did NOT see when blind, and (when armed) the §5b counters.

Gate: solve byte-identity is structural. Trace SCHEMA changes; solve artifacts
(`network_solution.csv`, products, snapshot solve content) must be md5-identical to
the pre-edit exe on `_ft63_pdfix_v3.json` — the parent card's §10 deliberate
reading 2, unchanged.

## 5. INC-1 — the correction (kernel), knob-free at this layer

The kernel gains the correction as an **explicit opt-in argument**, not a global
mode: each of the four readbacks takes a `bool carry_aware = false`, and the
`false` body is the EXISTING expression, verbatim. No knob inside
`NetworkNormalEquations` — the caller decides, so a diagnostic consumer and a
solve-affecting consumer can never be silently coupled (§7).

**(a) The batched rank-K correction.** For a request set `{(station s_i, local col
c_i)}` of size k, on top of the existing `n_i = N_rr⁻¹ e_{c_i}` and
`m_i = N_sr n_i`:

```
  per block b touching any requested station:
      Ycols_b(:, i) = C_{b,s_i} n_i          sparse apply via station_ddprior_
                                             (row, col, coeff) — zero column when
                                             block b does not touch station s_i
      W_b           = M_b · Ycols_b          ONE GEMM, K_b × k
  local term :   cov_local(a,b) −= Σ_b Ycols_b(:,a)ᵀ W_b(:,b)     ← Ycols_bᵀ W_b, k×k
  regression :   m_i            −= Σ_b G_b W_b(:, i)              ← G_b W_b, n_shared×k
  result     :   cov(a,b) = cov_local(a,b) + m_aᵀ Σ_ss m_b
```

`station_ddprior_` (`NetworkNormalEquations.hpp:1054-1055`) already stores exactly
the `(block, row, local_col, coeff)` terms needed for the sparse apply — the same
structure `reconstructStation` walks for the mean.

Two things this makes visible that the current code cannot express:

- **The local term survives ACROSS stations.** Today `jointConditionalCovariance`
  adds a local term only when `sta[a] == sta[b]` (:1509-1511, "same local space
  only"), which is correct for `N_rr⁻¹` (block-diagonal) and WRONG for `Ã⁻¹` (the
  rank-K downdate couples every column in a block, and a DD chord spans two
  stations by construction). This is the term that makes a between-station DD σ
  meaningful at all.
- **A degenerate block (`M_b = 0`, parent card INC-0) contributes exactly zero**, so
  the inert-fold semantics carry through the readback with no special case. Assert
  it rather than rely on it.

**(b) PD guard, counted (§3 consequence 2).** After the correction, any requested
variance ≤ 0 (or non-finite) is REFUSED, not clamped: that request falls back to its
uncorrected value, `covcarry_nonpos_rows` counts it, `covcarry_worst_ratio` records
the worst `corrected/uncorrected`, and the healthy→refusing transition WARNs once
(not per epoch — the parent card's INC-0 rule). Refuse rather than clamp, for the
parent card's §10 reading-1 reason: a clamp makes a broken block look healthy and
removes the very signal the guard exists to produce. For the joint matrix the same
screen runs on the diagonal; a refused diagonal disables the correction for that
request's whole row/column so the returned matrix stays a consistent covariance.

**(c) Magnitude telemetry.** `covcarry_var_ratio_med` — the median
corrected/uncorrected variance over the epoch's requests — so the size of the
previously-hidden information is a NUMBER on the run-end line, not an inference from
a σ histogram. Invariant #4: counters are the ON-path telemetry.

## 6. INC-2 — consumers, knob `network.carry_aware_covariance` (default OFF)

Armed, the knob switches the **diagnostic** consumers to the carry-aware readback:

| consumer | site | what changes |
|---|---|---|
| `dd_sigma_cyc` | `filter/NetworkDdFrac.cpp:255` | the between-station DD σ becomes the σ of the estimator that actually ran |
| AR probe | `filter/NetworkProducerAr.cpp:152` | `Q_cyc`, and therefore the FFR/ADOP/partial-AR ordering, stop being computed from a covariance the solve never used |

Both are READ-ONLY by construction (`NetworkProducerAr.cpp:3` — "log-only";
:64 — "Phase 2 is READ-ONLY (no re-pin / re-solve yet)"), which buys the sharpest
gate available anywhere in this project:

> **ON and OFF must produce BYTE-IDENTICAL solve artifacts.** Not just OFF ≡
> pre-edit — ARMED must be byte-identical too, because the two consumers this knob
> touches feed nothing back. The knob's entire footprint is two trace files. Any
> solve-artifact difference between the `on` and `off` arms is a wiring bug, full
> stop.

Consequences of that scope, all deliberate:

1. **NOT fingerprinted.** No stored state changes meaning, so putting it in the
   resume fingerprint would invalidate every snapshot on disk for nothing —
   `GnssConfigLoadNetwork.cpp:2971` is the precedent ("…and carry are untouched — so
   it is NOT in the resume fingerprint").
2. **Loader**: parsed in the network section, warn-if-set-without `carry_invariant`
   in the CROSS-KNOB VALIDATION PASS (`GnssConfigLoadNetwork.cpp:4737` is the
   pd_safe template — the parse block is the wrong place; the churn card's
   loader-ordering trap), forced false when inert, POSTURE ledger entry, and a line
   in the summary toggle log.
3. **The solve-affecting consumers are OUT of this card** — see §9.

## 7. Tests (new file `tests/test_carry_aware_covariance.cpp`, suite
`CarryAwareCovarianceTest`; one suite per file)

The oracle already exists: `tests/test_network_normal_equations.cpp:788` and :858
build a DENSE AUGMENTED system (`N + Cᵀ Pz⁻¹ C`, inverted) and compare. Same pattern,
own local fixtures (the pd_safe card's precedent — `tests/test_pd_safe_fold.cpp`
carries its own).

1. `LocalCovarianceMatchesDenseAugmentedSolve` — ONE block (where the fold is
   exact), three stations: the corrected `reconstructStationCovariance` /
   `…Diag` / `stationRegression` / `jointConditionalCovariance` all equal the
   corresponding blocks of `(N + CᵀPz⁻¹C)⁻¹` to 1e-9. **And the uncorrected path
   must NOT** — the falsifier: if the carry-blind form ever starts matching, the
   fixture no longer models the defect and must be re-derived (the
   `MixedScaleLanePairStaysPd` precedent).
2. `CrossStationLocalTermIsRecovered` — one chord spanning two stations: the
   corrected joint covariance's cross-station entry equals dense; the OFF path
   returns only the `m_aᵀΣ_ss m_b` term. Names the specific line
   (:1509-1511) this card corrects.
3. `MatureCarryCollapsesDdSigma` — the quantitative statement of the defect: with
   `Pz = 1e-6·I` the corrected DD σ falls to the prior scale while the blind σ stays
   at the epoch-local float value. Assert the RATIO, not an absolute.
4. `OverSubtractionIsRefusedAndCounted` — two blocks sharing a station, coupling
   engineered so `Ã⁻¹` goes indefinite: assert the fallback fires, the counter
   increments, and NO non-positive variance is ever returned. (This is also the
   only place §3's hazard is exercised until the field says otherwise.)
5. `DegenerateBlockContributesNothing` — a block forced non-PD (M=0) leaves the
   corrected readback equal to the uncorrected one, exactly.
6. `BlindPathIsIdenticalFoldOrNot` — stronger than the planned "EXPECT_EQ against the
   verbatim legacy expressions", and it states the defect at the same time: build two
   kernels from identical data, fold the DD priors into ONE of them, and assert all
   four blind readbacks return BIT-identical matrices. Invariant #6's structural
   guarantee and "the covariance could not see the carry even in principle" are the
   same assertion. It also asserts a blind call leaves `CarryCovStats` untouched — an
   instrument that counts work it did not do is the failure this card's parent exists
   to end. Plus the config gate (knob absent ≡ false ≡ inert, and it composes with
   `carry_invariant_pd_safe` without requiring it) in
   `tests/test_codebias_network_config.cpp`, where the other network-config cases live.

## 8. Arms and grading (Release, renamed exe, smoke-first)

| arm | config | purpose |
|---|---|---|
| `cacov_off` | `_ft63_pdfix_v3.json` on the NEW exe | invariant #6: solve artifacts md5-identical to `runs/pdfix_v3` |
| `cacov_on` | + `carry_aware_covariance: true`, 3 h | **the gate: solve artifacts md5-identical to `cacov_off`**, traces differ |
| `cacov_on6h` | the 6 h leg, only if 3 h passes | the P6(b) re-ask on the σ-conditioned subpopulation |

Grading:

1. **THE gate** — `cacov_on` vs `cacov_off`: `products.txt` md5 identical, snapshot
   identical but its ISO write timestamp, fingerprint hash unchanged. A difference
   is a wiring bug, not a result.
2. **σ distribution** — median `dd_sigma_cyc` by chord age bin, ON vs OFF. Prediction:
   the mature bins fall by orders; the freshly-born bins do not move (a diffuse prior
   carries no information, so the correction is ~0 there). If the mature bins do NOT
   fall, either the carry is not being folded on those keys or the correction is
   mis-wired — check `covcarry_var_ratio_med` before believing the σ.
3. **The payoff re-ask** — `med|frac|` CONDITIONED on `dd_sigma_cyc`. Bin the DDs by
   σ (< 0.15, 0.15–0.25, 0.25–0.5, > 0.5 cyc) and report `med|frac|` per bin. The
   question P6(b) could not ask: on the population where σ is small enough for a
   fractional part to be informative, is it still uniform? Deliverable:
   `scripts/grade_ddfrac_sigma_conditioned.py`.
4. **Cross-block approximation error** — `covcarry_nonpos_rows` /
   `covcarry_worst_ratio` over 3 h. This is new information about the FOLD, not
   about this card: a non-zero count means the per-block Woodbury composition is
   materially inexact somewhere, and that is its own card (§9).
5. **Perf** — the correction is `Σ_b K_b²·k` per epoch. At 63 sta the dd-frac tracer
   subsamples to `trace_dd_fractional_max_pairs`, so k is bounded by config; report
   the added ms/epoch against the existing `cov` timer already printed by the T2b
   line (`NetworkDdFrac.cpp:285-289`). Gate: < 25 % on the tracer's own cov stage,
   and no measurable move in total epoch time (it is a tracer).

## 9. Explicitly out of scope (named, with the reason)

- **The solve-affecting consumers.** `conditionalLocalVarianceDiag` at
  `app/NetworkRunContext.cpp:1748` and the coupling harvest at :1864 / :1911 feed the
  **phase-leveling ambiguity carry stash** — the variance that becomes the NEXT
  epoch's prior and is written into the snapshot. Correcting those is a real fix
  (those priors are currently too loose by exactly the information this card
  quantifies), but it changes the solve, changes stored state, and therefore needs
  its own knob, its own FINGERPRINT, and its own field validation. Landing it here
  would make the §8 gate impossible to state and the field A/B two variables at once.
  It is the immediate follow-on, and §8 grade 2's `covcarry_var_ratio_med` is the
  number that sizes it in advance.
- `NetworkIonoDiag.cpp:119`'s use of the same readback — diagnostic, but on the iono
  path; fold it in with the follow-on rather than widening this card's blast radius.
- **`rowRedundancy` takes no `carry_aware` flag, deliberately and permanently at this
  stage.** It is on the ARMED QC path (`NetworkRaim.cpp:145-150`,
  `NetworkSolveGlue.cpp:1069-1073`), and correcting it INVERTS a RAIM direction: the
  carry raises each `r_i`, so fewer rows fall below the exclusion floor and the `√r_i`
  divisor grows, i.e. σ̂₀ falls and **RAIM fires LESS**. Making a fault detector less
  sensitive as a side effect of a σ-reporting card is exactly the kind of coupling the
  standing armed set exists to prevent. It is a legitimate question — the redundancy
  numbers are as carry-blind as everything else here — but it is a QC card with a QC
  gate, not this one.
- **The cross-block Woodbury composition itself.** If §8 grade 4 comes back non-zero,
  the fix is a JOINT fold over blocks that share stations (or a documented refusal,
  as the sum-to-zero gauge already gets at `NetworkEstimator.cpp:236`). That is a
  solve-path card with its own PD story — precisely the parent card's territory, one
  level up.
- Rank-K correction at 305 sta: same `K³`-class boundary as the fold itself
  (INC-C2 impossibility); nothing here moves it.

## 10. Why this order (cont. in §11)

INC-0 first because the parent card's §11 proved that a number without its state is
worse than no number. The kernel correction (INC-1) is knob-free and explicit so the
diagnostic and solve-affecting consumers can never be coupled by accident. The
consumer knob (INC-2) is scoped to the two READ-ONLY consumers precisely so the gate
can be the strongest form available — armed and disarmed must agree bit-for-bit on
the solve — which is the only way to be certain, on a card whose whole subject is a
covariance, that nothing leaked into the estimate.

---

## 11. Implementation record (2026-08-03)

### What landed

| increment | where |
|---|---|
| INC-1a the correction | `NetworkNormalEquations::carryCovCorrection` — ONE sparse pass over `station_ddprior_` builds every touched block's `Y` columns, then one GEMM per block; `local_corr` (k×k) and `m_corr` (n_shared×k) both come back to be SUBTRACTED |
| INC-1 the four readbacks | `carry_aware` flag (default `false`, the pre-card expression verbatim) on `reconstructStationCovariance`, `reconstructStationCovarianceDiag`, `stationRegression`, `jointConditionalCovariance`; pass-through on the four `NetworkEstimator` wrappers |
| INC-1b the cross-station term | `jointConditionalCovariance`'s armed loop adds `local_corr(a,b)` for ALL pairs, not just `sta[a]==sta[b]` |
| §5b guard | refuse-never-clamp: wholesale on the matrix-valued forms, per-entry on the diagonal/scalar forms; `NetworkNormalEquations::CarryCovStats` |
| INC-0 mode visibility | `sigma_carry_aware` column appended to `network_dd_fractional.csv`; the AR probe's log line names `CARRY-AWARE` / `carry-blind`; run-end block in `filter/NetworkTeardown.cpp` on ANY `carry_invariant` run, armed or not |
| knob | `network.carry_aware_covariance` — member+getter `include/config/GnssConfig.hpp`, parse + cross-knob validation `src/core/config/GnssConfigLoadNetwork.cpp`, `ctx.knobs` `app/NetworkRunContext.{hpp,cpp}`, run-manifest echo `logging/NetworkRunTraces.cpp`, POSTURE ledger entry |
| deliverable | `scripts/grade_ddfrac_sigma_conditioned.py`; arms `config/test_suite/_ft63_cacov_{off,on}.json` (+ `_smoke`) |

### Three calls worth recording

1. **The kernel takes a FLAG, not a mode.** `NetworkNormalEquations` holds no
   carry-aware state; each readback is told, per call, which model to report. That is
   what keeps a diagnostic consumer and a solve-affecting one from being coupled by
   a single global switch — the §9 follow-on can arm the phase-leveling stash without
   touching what `dd_sigma_cyc` does, and vice versa.
2. **The guard REFUSES; it never clamps.** Same reading as the parent card's §10-1: a
   clamped variance makes a broken model look healthy and destroys the only signal the
   guard exists to produce. And what it would be laundering here is not this readback's
   defect at all — it is the FOLD's block-Jacobi truncation (§3), which no counter has
   ever measured.
3. **The run-end line prints on OFF runs too.** An armed run knows what it is; the run
   that most needs to be told its σ is carry-blind is the one that did not arm the knob.
   Parent card §11, applied one layer up.

### 300 s smoke A/B — `runs/cacov_{off,on}_smoke`, Release, renamed exe, 10 epochs cold

Run SEQUENTIALLY, not concurrently: two producers share `logs/`, and the first attempt at
running both at once killed the second with `STATUS_STACK_BUFFER_OVERRUN`. That is a
harness fact, not a code fault, but it is the kind that gets misread as one.

**THE gate PASSES at smoke scale.** `products.txt` md5
`233AB84987860D6F1F72CF8EB4D6C0DB` — **identical between the ARMED and DISARMED arms**.
The snapshot differs in exactly ONE of 1704 lines, the ISO write timestamp, and the config
fingerprint `084ba4980fec500b` is unchanged in both (so the knob genuinely is outside the
fingerprint and a snapshot crosses the flip in either direction). That is the strong form
of the gate: not "OFF matches the old exe", but "arming it changes no byte of the solve".

Run-end, ON arm: **11 readback calls over 1180 requested entries; 1180 corrected, 0
REFUSED, 0 wholesale fallbacks. Variance ratio corrected/blind min 0.0344, mean 0.474,
max 1.268.**

| DD σ over the identical 1028-row population | OFF (blind) | ON (carry-aware) |
|---|---|---|
| min | 2.1088 | **0.6627** |
| median | 5.9321 | **3.3809** (−43 %) |
| max | 24.9878 | 24.9875 |
| count with σ < 1 cyc | **0** | **72** |

Four readings, all of which the SPEC predicted and one of which it hedged:

1. **The correction is live and large** — a 29× variance reduction at the tightest end
   (ratio 0.0344 ⇒ σ down 5.4×), on a run only 10 epochs from cold where the carry has
   barely begun to accumulate. Invariant #4 satisfied: the counters move.
2. **The max σ does not move** (24.9878 → 24.9875). A freshly-born chord's prior is
   diffuse, so there is no information to add — §8.2's prediction, confirmed.
3. **`max ratio 1.268 > 1` — a variance GREW.** §2 said the regression term
   `(Ã⁻¹N_rs)Σ_ss(·)ᵀ` is not sign-definite and to grade the direction rather than assume
   it. It is now observed, not merely conceded.
4. **0 refusals.** The §3 cross-block over-subtraction did NOT fire on 63-station GPS-only
   at 10 epochs. Not evidence that it never will — this regime has few groups, and the
   hazard grows with signals per station, the §6.1 lane shape and the WL blocks. The
   guard remains field-unexercised, exactly as INC-1c/INC-2a are for the parent card.

The grader withholds a verdict on both arms — no DD reaches σ < 0.25 cyc in 10 cold
epochs — which is the correct behaviour and the reason the payoff needs the 3 h leg.

### What is OWED

Everything in §8. Nothing above is field evidence for the READBACK — it is a proof from
the code that the defect is real, a measurement (`runs/pdfix_v3`: every DD at σ > 1 cyc,
median 6.02) that it is large, and unit gates that the correction is right. Specifically:

1. `cacov_off` 3 h — md5 identity vs `runs/pdfix_v3` (invariant #6).
2. `cacov_on` 3 h — **solve artifacts md5-identical to `cacov_off`**. This is THE gate and
   it is stronger than the usual OFF-path check: both consumers are read-only, so armed
   and disarmed must agree bit-for-bit on the solve.
3. The σ-by-age distribution, and the payoff re-ask (§8.3).
4. Only then: §9's solve-affecting follow-on, and whatever `covcarry_nonpos_rows` says
   about the fold's cross-block truncation.
