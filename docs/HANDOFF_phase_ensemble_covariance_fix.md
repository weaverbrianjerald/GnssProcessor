# HANDOFF — Fix A (anchor-free ensemble phase-bias datum): fix the covariance-carry blocker

## Goal
Land `network.phase_datum_ensemble` (Fix A): an **anchor-free sum-to-zero-over-receivers**
phase-bias datum (`Σ_r zˢ_{r,σ}=0`) replacing the per-(sat,signal) prioritized-anchor pin,
so the emitted satellite phase bias references the receiver **ensemble** — no pivot, no
handover (kills the moving-datum churn), and every satellite on the SAME receiver reference
(kills the AR-poison that leaves the user's DD ambiguity non-integer). This is the Odijk
S-system datum cure for the closed-loop consumer-vertical bug.

## State (ALL UNCOMMITTED on master; suite 591 = 590 pass / 1 known skip)
BUILT + VERIFIED-IN-ISOLATION, but BLOCKED on one bug (below):
- Kernel: `NetworkNormalEquations::foldReducedSumZeroGauges` folds `Σ_r zˢ=0` as a
  Schur-projected rank-1 correction `Ñ_ss += g gᵀ/β`, `ũ_s += g γ/β`
  (`g=Σ_r N_sr n_r`, `n_r=N_rr⁻¹e_{z_r}`, `β=Σ_r(n_r)_{z_r}`, `γ=Σ_r(N_rr⁻¹u_r)_{z_r}`),
  and corrects `reconstructStation` by `-n_r(γ-gᵀx_s)/β`. `SumZeroGauge` struct +
  `FoldedSumZeroGauge` storage (`terms,g,β,γ`) + `station_sumzero_` index +
  `sumzero_folded_` idempotency guard. Cleared by `reduce()`-recompute / `reset()`.
- Estimator: `NetworkEstimator::setPhaseSumZeroGauges(...)`; folds at `solve()` AND
  `reducedSharedNormals()` (gate). `shared_cov_` filled in `solve()` via
  `neq_.solveSharedConstrained(N,u,&shared_cov_)` = inverse of the folded constrained
  reduced normals; exposed by `est.sharedCovariance()`.
- Driver (`ApplicationNetwork.cpp`): local `phase_datum_prioritized = config || ensemble`
  (ensemble reuses ALL prioritized SPOF-cure machinery, diverges only at the election);
  ensemble election branch (union, no anchor, no pins); `StationAssembly::ens_amb_cols`;
  `buildEnsembleGauges()` lambda (primary + within-epoch RAIM re-solve); S-contract
  `AmbDatum::SumToZeroPerReceiver`; conditional fingerprint tag `phdatum:ens`.
- Config: `network.phase_datum_ensemble` (GnssConfig.hpp getter/member, GnssConfigLoad.cpp
  parse+info-log). Default OFF.
- Proofs: unit test `NetworkEstimatorTest.SumZeroPhaseDatumRecoversEnsembleSTransform`
  (S-transform exact: recovers `b_s+mean_r(M)`, ambiguities Σ=0, between-receiver diffs =
  truth); byte-identity OFF HASH-PROVEN (prioritized config == golden, 4/4 artifacts);
  native null-space-gate pass at 305 sta (epoch 1 emits 417 PB, 0 withheld); 4-lens
  adversarial driver review = 0 confirmed defects.
- Design doc: `docs/network_phase_ensemble_datum_2026-07-08.md`.

## THE BUG (blocker)
On REAL data the producer clocks run away exponentially (~7×/epoch → 5e8 m) and wedge.
Isolation on the fast 3-station reproducer proved: FUNDAMENTAL (not 305-specific), NOT the
phase-leveling carry (diverges with it OFF). **Epoch 1 de-medianed clocks are byte-equal to
the anchor baseline (1e-4 m) — the fold is clock-orthogonal and exact for the POINT SOLVE —
but they diverge starting EPOCH 2**, exactly when the **D2 between-epoch SHARED carry** first
injects a prior (`0.05→0.14→0.69→2→20→145 m ...`).

**ROOT CAUSE:** the anchor datum fixed the gauge with a LOCAL pin (eliminated before the
Schur reduction → no shared-covariance pollution). The ensemble fixes it POST-reduce with the
rank-1 `g gᵀ/β`, and `g=Σ N_sr n_r` has components in the CLOCK + code-bias columns (the
ambiguity couples to them). So the fold injects phase↔clock cross-covariance / an over-tight
gauge variance into `shared_cov_` (= inverse of the folded normals). The point solve stays
EXACT, but the D2 carry (`est.sharedCovariance()` → `addSharedPrior` / `addSharedPriorDense`)
CONSUMES `shared_cov_` as next epoch's prior, mishandles the gauge covariance, and injects
error with gain>1 → the clock (carried by `clock_model`) amplifies it → runaway. The fold is
right for the ESTIMATE; the defect is purely the COVARIANCE the between-epoch loop feeds on.
(The covariance correction was mis-scoped as "conservative-if-omitted" — it is LOAD-BEARING.)

## TASK — implement the covariance-carry consistency fix
**PRIMARY (do first): Option 2 — zero the gauge direction in the covariance the D2 carry
harvests.** Where the driver harvests `est.sharedCovariance()` to build the D2 prior
(grep `sharedCovariance` + `addSharedPrior`/`addSharedPriorDense` in ApplicationNetwork.cpp),
project out the ensemble-gauge direction(s) `g_k` before forming the carry:
`cov_carry = P·shared_cov_·P`, `P = I − Σ_k ĝ_kĝ_kᵀ` (ĝ_k = g_k/|g_k|) — so the carry never
propagates the fold's artificial gauge variance + phase↔clock cross-terms. This MIRRORS the
existing exact-gauge pattern: `conditionalLocalCovariance`/`conditionalLocalVarianceDiag`
already require the caller to ZERO the datum-pinned shared columns of `cov_free_xs`
("datum-pinned rows/cols zeroed → treat as exact"; see NetworkNormalEquations.hpp docs) — the
ensemble gauge is the same kind of exact datum, just a DIRECTION (g_k) not a column. Expose
the stored `g_k` from the kernel (they live in `sumzero_gauges_[].g`) via a small accessor so
the driver can build `P`.

**ALTERNATIVES to investigate if Option 2 is awkward/insufficient:**
- Option 1 — compute `shared_cov_` WITHOUT the rank-1 fold term (the gauge is exact/known, so
  its direction carries no random uncertainty). Cleaner in theory; watch that the phase-bias
  ESTIMABLE variance is still carried (don't accidentally drop the real phase-bias info — only
  the gauge direction is exact).
- Option 3 — mark the fold direction as a pinned/exact gauge the D2 carry SKIPS entirely
  (don't carry the gauge dof; carry only the estimable phase-bias structure).

## VALIDATION CADENCE
1. FAST reproducer (~38 s, diverges at epoch 2 today):
   `build/x64-Release/gnss_processor.exe runs/card8_brdc_rts/sp3_twin_ensemble.json`
   SUCCESS = no "clock runaway", full window emits, and the de-medianed CLK stays matched to
   the anchor baseline `runs/card8_brdc_rts/sp3_twin/logs` (grader below) across ALL epochs.
2. Byte-identity OFF must stay HASH-PROVEN: run the prioritized config `sp3_twin.json`,
   compare `network_solution.csv`+`network_products.txt`+`closed_loop_trace.csv`+
   `network_events.csv` SHA to `runs/card8_brdc_rts/sp3_twin/logs`.
3. Suite: `gnss_tests.exe` = 590 pass / 1 skip (the ensemble unit test must stay green).
4. THE decisive 305-sta A/B: `net305_val30min_ensemble.json` vs the prioritized baseline
   (`net305_val30min_stable.json`). Grade with `python scripts/grade_codeloose.py <run> ...`.
   KEY QUESTIONS: PB disc churn ↓? float converges? **does the consumer START FIXING**
   (baseline 0/55 — the AR-poison test)? Note: fold ≈ doubles per-epoch cost (~50 s/epoch at
   305) — a batching/caching optimization of the per-gauge per-station `n_r=N_rr⁻¹e_col`
   solves is a separate follow-up.

## BUILD (MSVC + vcvars; user shell doesn't source vcvars64)
```
$v="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$v`" >nul && cmake --build C:\GnssProcessor\build\x64-Release --target gnss_processor 2>&1"
```
(swap `x64-Debug` + `--target gnss_tests` for the suite). Config is `argv[1]` positional, NOT
`--config`. Runs write a `logs/` dir in cwd.

## KEY FILES / SYMBOLS
- `include/filter/NetworkNormalEquations.hpp` / `src/core/filter/NetworkNormalEquations.cpp`
  — `foldReducedSumZeroGauges`, `reconstructStation` (fold correction), `SumZeroGauge`,
  `FoldedSumZeroGauge{g,β,γ}`, `sumzero_gauges_`, `reconstructStationCovariance*`/`rowRedundancy`
  (the exact-gauge `cov_free_xs`-zeroing pattern to mirror).
- `include/filter/NetworkEstimator.hpp` / `src/core/filter/NetworkEstimator.cpp`
  — `setPhaseSumZeroGauges`, `phase_sumzero_gauges_`, `solve()` (fills `shared_cov_`),
  `sharedCovariance()`.
- `src/core/app/ApplicationNetwork.cpp` — `phase_datum_ensemble`, `buildEnsembleGauges`,
  `ens_amb_cols`, the D2 carry (`sharedCovariance()` → `addSharedPrior`/`addSharedPriorDense`),
  `clock_model` (the amplifier).
- `include/config/GnssConfig.hpp`, `src/core/config/GnssConfigLoad.cpp` — the toggle.
- `tests/test_network_estimator.cpp` — `SumZeroPhaseDatumRecoversEnsembleSTransform`.
- Memory: `project_network_phase_ensemble_datum` (full detail),
  `project_closedloop_consumer_vertical_bug` entry (r) (the Odijk diagnosis + fix queue),
  `project_network_clock_sum_to_zero` (the clock-side template + the shared-vs-local
  structural difference this fix navigates).
- Reproducer configs: `runs/card8_brdc_rts/sp3_twin_ensemble.json` (3-sta, fast),
  `runs/net_matrix_2024-129/configs/net305_val30min_ensemble.json` (305-sta A/B).

## THEORY CONTEXT (one paragraph)
Odijk et al. 2016 (JoG 90:15-44): the estimable satellite phase bias `δ̃ˢ_σ = [phys] − zˢ_{1,σ}`
absorbs the pivot RECEIVER's ambiguity, so it's time-constant only if that anchor is fixed AND
the same receiver anchors all sats (else the user's between-satellite fractional bias doesn't
cancel → non-integer DD). The prioritized anchor violated both (per-epoch re-election + split
receivers). The ensemble sum-to-zero references the whole receiver ENSEMBLE consistently →
Odijk-conformant datum. (The OTHER remaining Odijk gap is CONTINUITY — RAIM reset-not-repair,
"Fix B" — separate work, not this task.)
```
```
