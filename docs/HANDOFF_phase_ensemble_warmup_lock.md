# HANDOFF — Fix A ensemble phase-bias datum: initialize-then-FIX (hard S-basis), then the full-B covariance

> ⚠️ SCOPE CORRECTION (2026-07-09 audit, memory `project_opus_ssystem_audit_jul9`): TASK 1's
> GOAL is time-stability / churn elimination ONLY. The original claim that snapshotting all
> b_s together is "the AR-poison cure" is FALSIFIED by the tree's own Eq-34 gate test
> (test_network_estimator.cpp): a hard-locked ensemble reference is constant but STILL
> non-integer (mean-of-integers k/R + per-sat receiver-set differences), so user DD stays
> non-integer under this datum regardless of the lock. Integer recovery needs a CC-R-style
> single-receiver datum (per-(receiver,signal) bias states or de Jonge chain pivoting) —
> separate card. Execute TASK 1 for stability; do NOT grade it on consumer fixes.
> Also note: the DD-fraction blocker's root cause is SH-VTEC iono error absorbed into the
> ambiguities (memory `project_network_ddfrac_rootcause_shvtec`) — no datum work affects it.

> NOTE: an earlier draft of TASK 1 proposed a "soft hold + warmup process-noise". That is
> NOT Odijk/Teunissen-permissible (a soft prior / process noise on a gauge direction lets
> the datum wander and biases the frame). TASK 1 below is the corrected HARD-datum design:
> choose the gauge ONCE after init, fix it as a hard Dirichlet pin, S-transform on any
> later re-realization, never `q` on the gauge. (Filename retains "warmup_lock" for link
> stability.)

## Where this stands (ALL UNCOMMITTED on master; suite 590 pass / 1 skip)

`network.phase_datum_ensemble` ("Fix A") — the anchor-free sum-to-zero-over-receivers
phase-bias datum (`Σ_r zˢ_{r,σ}=0`) — is BUILT and its producer **runaway is CURED**.
The cure is `network.phase_datum_hold_sigma_m` (default 0 = churny/off): SEED `Σz=0`
once per (sat,signal), then HOLD the shared bias `b_s` with a stiff fixed-variance
prior. Validated: 3-sta reproducer clean (121 emit / 0 skip, 0 runaway, phase bias
held <2 mm/epoch, disc=0); byte-identity OFF hash-proven (4/4); suite 590/1; 305-sta
full 60-epoch window (0 runaway/withhold, clock-Δ rms 0.84–1.03 m, **PB disc churn
1716 vs 7138 anchor = −76 %**).

**What is NOT fixed:** the consumer is only COMPARABLE to the anchor (e3d 2.81 vs
2.74 m), still **0 AR fixes**, and settles to a **persistent +1.7 m U-bias**. Root of
the residual: the hold locks **too stiff, too early** — it snapshots a poor epoch-1
FLOAT reference and only crawls toward truth (G21's held bias drifts −86627 → −86575 m
over 30 min and is STILL converging at epoch 60). The estimable (between-station DD)
structure converges naturally via the phase-leveling carry, but the ensemble's
per-(sat,signal) absolute LEVEL is a datum/gauge direction the filter cannot converge
on its own — it must be fixed explicitly, and we fixed it before its estimable
scaffolding had settled.

## Root-cause context (why the hold exists at all)

The un-held ensemble references the emitted bias to `mean_r(ẑ_r)` — a
per-epoch-RE-REALIZED functional of the FLOAT ambiguities whose absolute level is
code/iono-precision-limited — so the emitted bias CHURNS ~1 m/epoch (the 86–130 km
epoch-1 values are the EXACT ensemble value at those sats' conditioning, NOT a bug: the
point solve is a proven per-epoch S-transform, clocks byte-exact to the anchor). The
co-estimated SH-VTEC (carried) absorbs the churn and amplifies it ~3×/epoch → the
exponential runaway (SH-off ⇒ no runaway, proven; the runaway ignites ~epoch 14). No
covariance fix removes the churn. Full audit + derivations: memory
`project_network_phase_ensemble_datum` entry (2), and
`docs/network_phase_ensemble_datum_2026-07-08.md`.

## What is already implemented (this session, uncommitted)

- `NetworkEstimator::zeroPhaseGaugeColumns` (NetworkEstimator.cpp/.hpp) — zeros each
  ensemble gauge's shared column (via new `NetworkNormalEquations::SumZeroGauge::shared_col`,
  set in `buildEnsembleGauges`). Called in the D2 carry harvest AND
  `conditionalLocalCovariance`/`conditionalLocalVarianceDiag`/`rowRedundancy`. (Treats
  the gauge column as an exact datum — the true analogue of the datum-pin zeroing;
  NOTE the handoff's earlier "project out `g`" idea was WRONG: `g = λ·N_ss·e_pb` has
  clock components, so projecting `g` corrupts the correct gauge-INVARIANT clock cov —
  zero the phase-bias COLUMN, not `g`.)
- `network.phase_datum_hold_sigma_m` (GnssConfig.hpp getter+member, GnssConfigLoad.cpp
  parse). In ApplicationNetwork.cpp: `phase_hold_sigma/on/var` locals near the ensemble
  flag; `buildEnsembleGauges` builds a `held` set from `prev_carry` PhaseBias entries
  (var>0) and SKIPS the `Σz=0` fold for held (sat,signal); the D2 stash carries `b_s`
  with the fixed `phase_hold_var` (NOT the contaminated `cov(col,col)`); a dedicated
  PRE-gate injection (right after the SH-VTEC prior) re-applies the held prior every
  epoch (it is a DATUM — must precede the null-space gate, and is orthogonal to the
  clock-pivot `datum_changed`); the two D2 carry-consumption loops SKIP PhaseBias under
  hold (single injection, no double-fold).
- 3-lens adversarial review DONE — all findings addressed (held datum moved pre-gate;
  `var>0` guard aligned; soft-refloat decision documented).

## TASK 1 (do first) — INITIALIZE-THEN-FIX (Odijk/Teunissen-conformant HARD datum)

★ SUPERSEDES the earlier "soft hold + warmup-q" idea, which is NOT S-system-permissible
(user correction, 2026-07-09). A soft prior (finite σ) on a gauge direction is a
pseudo-observation, not a datum: its finite weight lets the reference WANDER (the +1.7 m
drift IS the datum crawling) and a mis-set weight biases the frame; and process noise on
a NON-estimable (gauge) direction is an undamped, unobservable random walk. The
Odijk/Teunissen datum is an EXACT constraint (zero variance), chosen ONCE, after which
estimable variances are datum-invariant. So: **NEVER a soft state / prior / process
noise on the gauge — choose it once and HARD-constrain it.**

The correct design (user's four points → implementation):
1. **INIT (converge, then choose the gauge once).** Run until the estimable structure
   (DD ambiguities, clocks) has converged AND coverage/geometry is adequate (enough
   stations tracking each sat). Use a STABLE datum during init so the SH-VTEC runaway
   can't ignite before the lock — recommended: a **sticky single anchor** (or another
   bounded realization). The re-realized ensemble is used only to READ OFF the reference
   at the lock epoch, not to emit/consume during init.
2. **FIX ONCE (hard Dirichlet pin on the SHARED b_s).** At the lock epoch, snapshot
   `b_s = c_s` for ALL (sat,signal) from the SAME converged `Σz=0` solve — snapshotting
   them together is what preserves the between-satellite consistency (the AR-poison
   cure). Convert the datum to HARD pins: add `(b_s column, c_s)` to `plan.shared_datum`
   — the SAME Dirichlet exact-pin machinery `NetworkEstimator::solve` already applies to
   the clock datum. Stop folding `Σz=0` for pinned sats. Zero variance, NO prior, NO q.
   `b_s` is then EXACTLY constant (the ideal product). **Pin the SHARED b_s, NOT a
   station's ambiguity (as the anchor does):** this is anchor-free (no single-station
   SPOF) AND robust to stations coming/going — the pin is a shared state, so a dropped
   station just removes its data from the estimable DD; it does not invalidate the frame.
   That handles most of the "live network churn" concern for free.
3. **NEW (sat,signal)** = its own gauge → init it individually (own warmup against its
   ensemble), then pin. Emit "converging" / wider-URA until pinned.
4. **RE-REFERENCE (rare, genuine frame change)** → apply a proper S-TRANSFORMATION to
   the carried `b_s` AND their covariance (shift by the frame-change vector) so the
   frame stays consistent, and signal the consumer via that transform (Psychas/
   Khodabandeh 2024 for the DD-integer-carry form), NOT a raw disc.

Implementation notes:
- The hard-pin machinery already exists: `plan.shared_datum` + the Dirichlet elimination
  in `NetworkEstimator::solve` (the clock datum uses it). Reuse it for the b_s columns.
- `buildEnsembleGauges`' `Σz=0` fold becomes INIT-ONLY (it CHOOSES `c_s`); after lock,
  the shared_datum pins carry the frame. The `zeroPhaseGaugeColumns` covariance work
  still applies — a fixed gauge column is exact, so it is zeroed in the carried
  covariance exactly as the clock datum pins are (`conditionalLocalCovariance` etc.).
- Delete `network.phase_datum_hold_sigma_m` (the soft hold) OR repurpose it strictly as
  the INIT-period realization, never as the post-lock datum.
- Genuinely hard (spend the complexity here, not on a soft leash): choosing the lock
  epoch (a convergence/coverage criterion), the per-(sat,signal) init of new gauges, and
  the S-transform on re-reference.

Validation cadence:
- 3-sta: `build/x64-Release/gnss_processor.exe runs/card8_brdc_rts/sp3_twin_ensemble_hold.json`.
  Confirm no runaway, full window, and — the S-conformance check — the emitted `b_s` is
  EXACTLY constant after lock (disc=0, not merely "small drift"), while the estimable
  between-station DD ambiguities are byte-comparable to the anchor (pure S-transform).
- byte-identity OFF stays hash-proven (`runs/card8_brdc_rts/sp3_twin.json` vs
  `runs/card8_brdc_rts/sp3_twin/logs`, SHA on the 4 artifacts). suite 590/1.
- 305 A/B: `runs/net_matrix_2024-129/configs/net305_val30min_ensemble_hold.json` vs the
  anchor baseline `runs/card8_brdc_rts/net305_val30min_stable` (byte-identity-equivalent).
  Grade `python scripts/grade_codeloose.py <run> ...`. SUCCESS = the +1.7 m U-bias
  collapses (the frame is now fixed after convergence, not a crawling epoch-1 float),
  e3d beats the anchor, and — decisive — **does the consumer START FIXING** (baseline
  0/60). Watch closed_loop_trace.csv: the old soft hold spiked to e3d ~12 m at epoch 2
  (poor seed) then settled to a +1.7 m crawl; the hard init-then-fix should remove both.

## TASK 2 (later, per user) — FULL INTER-GAUGE `B⁻¹` COVARIANCE

The multi-gauge fold folds each gauge INDEPENDENTLY:
`reduced_N_ss += Σ_k g_k g_kᵀ/β_k = G diag(β)⁻¹ Gᵀ`
(NetworkNormalEquations.cpp `foldReducedSumZeroGauges`, surfaced into `shared_cov_` by
`solveSharedConstrained`). The exact `w→∞` multi-constraint Woodbury needs the full
inter-gauge coupling: `G B⁻¹ Gᵀ`, `B = C_lᵀ N_ll⁻¹ C_l`,
`B_{jk} = Σ_r (N_rr⁻¹)_{z_r^{(j)}, z_r^{(k)}}` — NON-diagonal, because within one station
the ambiguities to different satellites are correlated through the (near-free,
white-noise) receiver clock.

- The POINT solve is UNAFFECTED (proven — the `diag(β)` vs `B` matrix term cancels
  against the matching RHS term at the datum point). This is a **covariance-only**
  defect: the VTEC/clock/code blocks of `shared_cov_` are systematically distorted.
- It was the crash AMPLIFIER for the runaway — now MOOT with the hold (no churn to
  amplify). It remains a latent defect for anything that consumes `shared_cov_`:
  producer-side network AR seeding, the D2 carry-variance, RAIM redundancy.
- Fix: assemble the K×K `B` (one extra `(N_rr⁻¹)_{z_i,z_j}` entry per within-station
  gauge pair — reuse the already-computed per-gauge `n_r^{(k)} = N_rr⁻¹ e_col`), and
  fold `G B⁻¹ Gᵀ` / `G B⁻¹ γ_vec` for the COVARIANCE path (the point may keep the
  cheaper `diag(β)`). Files: NetworkNormalEquations.cpp `foldReducedSumZeroGauges` +
  `solveSharedConstrained`.
- Validation: the POINT products/estimates stay byte-identical; only `shared_cov_`
  changes. Falsifier / unit test: on a captured 2-gauge epoch, `x_s` from `diag(β)` and
  from `B⁻¹` must match to solve precision while `F⁻¹` differs on the VTEC rows/cols.

## Build / run

```
$v="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$v`" >nul && cmake --build C:\GnssProcessor\build\x64-Release --target gnss_processor 2>&1"
```
(swap `x64-Debug` + `--target gnss_tests` for the suite). Config is `argv[1]` positional.
A running gnss_processor.exe LOCKS the exe — kill it before relinking (LNK1104).

## Key files / symbols / memory

- `src/core/app/ApplicationNetwork.cpp` — `phase_hold_*` locals, `buildEnsembleGauges`
  (held-skip), the D2 stash (hold-carry + `zeroPhaseGaugeColumns`), the PRE-gate held
  injection, the two D2 carry-loop PhaseBias skips.
- `src/core/filter/NetworkEstimator.{hpp,cpp}` — `zeroPhaseGaugeColumns`, the three
  `conditional*`/`rowRedundancy` calls.
- `src/core/filter/NetworkNormalEquations.{hpp,cpp}` — `foldReducedSumZeroGauges`,
  `solveSharedConstrained`, `SumZeroGauge::shared_col` (TASK 2 lives here).
- `include/config/GnssConfig.hpp`, `src/core/config/GnssConfigLoad.cpp` — the toggles.
- `docs/network_phase_ensemble_datum_2026-07-08.md` — design + the 2026-07-08 resolution.
- Memory: `project_network_phase_ensemble_datum` (full detail + entry (2) = the fix +
  definitive 305 result), `project_closedloop_consumer_vertical_bug` (the consumer bug
  this addresses; entry (r) = the Odijk diagnosis).
- Reproducers: `runs/card8_brdc_rts/sp3_twin_ensemble_hold.json` (3-sta, ~44 s),
  `runs/net_matrix_2024-129/configs/net305_val30min_ensemble_hold.json` (305, ~50 min),
  anchor baseline `runs/card8_brdc_rts/net305_val30min_stable`. Grader
  `scripts/grade_codeloose.py`.
