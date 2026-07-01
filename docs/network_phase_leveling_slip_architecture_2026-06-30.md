# Network Phase-Leveling + Carrier-Phase Slip / Outlier Architecture

**Status:** scope / implementation plan — NOT yet implemented.
**Author:** drafted 2026-06-30 for B. Weaver, to execute from this doc.
**Companion memory:** `project_ppp_rtk_network.md`, `project_network_perepoch_serial_bottleneck.md`,
`project_global_sh_iono.md`, `project_h1_codebias_discontinuity.md`,
`project_turboedit_mwgf.md`, `project_pivot_handover_flipflop_fix.md`,
`project_ar_validation_criterion.md`.
**Companion docs:** `network_ambiguity_resolution_design.md` (producer AR — the
natural *next* layer once ambiguities are carried), `turbo_edit_lli_bridge_design.md`.

> ⚠️ Line numbers in this doc are *indicative only* — `ApplicationNetwork.cpp`,
> `NetworkNormalEquations.cpp`, and `CycleSlipManager.cpp` are under active
> concurrent edit. Every integration point is anchored on a **symbol**
> (function / struct / method). Re-grep the symbol before editing.

---

## 1. Goal and one-paragraph summary

Move the PPP-RTK **network** producer from a per-epoch **absolute** snapshot
solve to a **phase-leveled** solve that **carries each station's carrier-phase
ambiguities as continuous states across epochs**, so the network finally
extracts carrier-phase *precision* (not just code) into the satellite
clock/phase-bias/code-bias products. Because carried ambiguities make cycle
slips a real cross-epoch state-management problem (today they are not), this doc
also specifies the **slip / outlier architecture**: a strictly tiered,
**repair-first** defense (MW/GF TurboEdit repair → prefit innovation gating →
post-fit cross-station RAIM) whose carried-state **reset** is *batched* — all
flagged ambiguities for an epoch are folded into the single existing solve, not
re-solved one-by-one.

**Two findings reframe the whole effort and must be internalized before
coding:**

1. **Phase-leveling is essentially free on the solve.** Network ambiguities are
   *already* local columns in each station's `N_rr` (they are Schur-eliminated
   every epoch, **not** dropped from the matrix). Carrying them adds a **diagonal
   information increment to columns that already exist** — it does **not** enlarge
   `N_rr`, so `reduce()` cost is unchanged. The earlier "carry grows `n_r` ~50/
   station, 2–3× Schur" estimate is wrong against the tree. *There is no Schur
   cost to amortize; do not build re-solve-skipping machinery for it.*

2. **Today only a PIVOT slip corrupts products.** A non-pivot ambiguity is free
   every epoch (absolute solve), so its slip is auto-absorbed and it contributes
   essentially nothing to the products — which is exactly why the network is a
   "code snapshot with pivot-only phase leveling" today. Phase-leveling is what
   makes *every* station's phase informative, and therefore what makes *every*
   station's slip matter. Slip handling is a **prerequisite of** phase-leveling,
   not an optional add-on.

---

## 2. Current architecture (verified baseline)

The network solve is a **per-epoch absolute normal-equation arrowhead** with
exact Schur reduction onto a shared satellite border, solved **once** per epoch.
There is **no** Kalman filter and **no** cross-epoch prior on local nuisances.

| Concept | Symbol / site | Note |
|---|---|---|
| Per-epoch solve | `NetworkEstimator::solve` → `NetworkNormalEquations::reduce` + `solveSharedConstrained` | one `reduce()` + one LDLᵀ per epoch (idempotency guard in `reduce()`) |
| Shared params | `SharedParamKind {Clock, CodeBias, PhaseBias, VtecSh}` | sat clock (per sat), code/phase bias (per sat,signal), SH-VTEC |
| Local nuisances | per-station `A_r` in `splitStation` (ApplicationNetwork.cpp) | rx clock, ISB, ZWD, slant iono, **ambiguity** — Schur-eliminated each epoch |
| **No local carry** | `ApplicationNetwork.cpp` comment: *"the per-epoch ABSOLUTE network solve carries no Kalman prior on the local nuisances"* | only the SHARED params carry |
| Shared carry (D2) | `prev_carry` stash + `NetworkEstimator::addSharedPrior` → `addSharedInfo` | diagonal prior, value+var, inflated `var_eff = var + q·dt`; folded **after** the rank gate via the in-place fast-path |
| Phase-bias gauge | pivot pin block in PASS-B (`st.is_pivot`), rows in `A_r` at `w = 1e10` | drives pivot ambiguities' **absolute** value → estimable bias absorbs the pivot lump (Odijk Eq. 4) |
| Datum / rank gate | `NetworkSbasis::buildSharedDatum`, `assertNullSpaceRank` | clock-only `declared_dimension`; `SelfAdjointEigenSolver` (eigenvalues-only); withholds the epoch if the reduced shared null-space exceeds the declared gauge |
| Local recovery | `NetworkNormalEquations::reconstructStation` / `reconstructStationCovariance` | returns per-station `x_r` + conditional covariance using the cached `N_rr` LDLᵀ; today used only for tracing/viz |

**Key structural fact for the carry:** `reduce()`'s per-station Schur subtrahend
`N_sr · N_rr⁻¹ · N_rs` depends **only on that station's own blocks**. A prior
added to one station's `N_rr` is therefore **orthogonal** to every other
station's contribution — it conditions only that station's `x_r`. This is what
makes a per-station carried prior safe and cheap.

---

## 3. The carried-ambiguity design (phase-leveling)

### 3.1 Representation

A carried ambiguity **is** a diagonal information contribution on the owning
station's local block:

```
Λ_amb = 1 / var_eff
η_amb = Λ_amb · value            (information-form RHS)
```

injected **only** into `stations_[r].N_rr / u_r` — **never** the shared border.
This is the exact per-station analogue of the existing shared D2 carry
(`prev_carry → addSharedPrior → addSharedInfo`).

v1 ships **diagonal-only** carry. (The reduced full-conditional-covariance form
— for later user-side AR parity — carries off-diagonals and **changes the reset
primitive**; see §3.6 and §9.)

### 3.2 New kernel hook — the only required core addition

```cpp
// NetworkNormalEquations — mirror of addSharedInfo, scoped to one station.
// Folds a diagonal information increment into stations_[station_idx].N_rr / u_r.
// MUST be called BEFORE reduce() (see §4 — there is no post-reduce fast-path
// for a local-block change).
void addStationPrior(int station_idx,
                     const std::vector<int>& local_cols,   // columns within A_r
                     const Eigen::VectorXd&  delta_Nrr_diag, // Λ_amb per column
                     const Eigen::VectorXd&  delta_ur);      // η_amb per column
```

~30 lines, identical symmetrization/caching discipline to `addSharedInfo`, but
it touches a single station's `N_rr` and leaves the shared border untouched.

### 3.3 Stash / inject cycle

- **Stash (post-solve):** harvest `(value, conditional-variance)` for each
  carried ambiguity column from `reconstructStation` /
  `reconstructStationCovariance`. **Diagonal-only:** extract just the diagonal of
  the conditional covariance — do **not** form the full `M·cov·Mᵀ`
  (`reconstructStationCovariance` does a per-station `N_rr⁻¹` = `solve(Identity)`,
  the serial full-inverse flagged in `project_network_perepoch_serial_bottleneck`;
  running it for all ~50–305 stations every epoch is a real new cost — extract
  the diagonal cheaply instead). Store into a new `local_carry` vector keyed by
  the **full identity** `(station_idx, sat, signal, freq)`.
- **Inject (next epoch):** before `reduce()`, loop the carry and call
  `addStationPrior` against the **current** `StateManager` ambiguity index for
  that station; **skip on index miss** (sat absent this epoch — the all-zero-H
  column prune in `splitStation` already removed it; the carry simply has nowhere
  to land and re-injects when the sat returns).
- **Time-update:** `var_eff = var + q_amb · dt` (reuse the D2 inflation pattern).
- **Determinism:** iterate the carry in a **sorted/ordered** container (not an
  unordered map). Since `addStationPrior` folds into `N_rr` before factorization,
  unordered iteration would inject ULP-level non-determinism into `N_rr` even in
  serial. The current default is byte-identical serial≡parallel — preserve it.

### 3.4 `q_amb` lower bound (correctness, not dynamics)

`q_amb` must be **strictly > 0** *and* above the PD floor: a carried-but-this-
epoch-unobserved column is held PD only by its prior, and `addStation`
**hard-throws and drops the whole station** if `N_rr` is not positive-definite.
The relative PD floor is `~1e-12 · d_max` with `d_max ~ 1e10` (the pivot pins),
so require roughly `Λ_amb ≳ 1e-12 · 1e10`, i.e. `var_eff` bounded **above** so the
information stays above the floor. Pick `q_amb` (≈ `1e-6 … 1e-3 m²/s`, one
heuristic constant like `q_clock/q_cb/q_pb`) so a no-observation gap can never
sink a carried column below the floor. Flag for empirical tuning.

### 3.5 Injection ordering and the pivot

- **Hard invariant: inject before `reduce()`** (at the `splitStation` /
  `addStation` assembly site). A local-block change alters the Schur subtrahend,
  which is baked into `reduced_N_ss_` during `reduce()`; there is **no** in-place
  post-gate fast-path for it (unlike `addSharedInfo`, whose shared-border delta
  does not pass through Schur). Injecting after the gate would flip `reduced_` and
  force a **full second `reduce()`** — the serial doubling we must avoid. **Add an
  assertion/test that `reduce()` runs exactly once per epoch with the carry on.**
- **On pivot columns, SKIP the carry injection.** The pivot's ambiguities are
  defined *exactly* by their `w = 1e10` pins; a carried diagonal on the same
  column is redundant and can only add ULP perturbation. Skipping makes "the pin
  wins" exact-by-construction rather than dominance-by-magnitude. Carried priors
  apply to **non-pivot** ambiguity columns only.

### 3.6 Reset primitive (the "batch reset")

Keyed by `(station, sat, signal)`, three actions, all applied to `local_carry`
**before** injection so the single solve absorbs them:

| Action | Operation | Continuity |
|---|---|---|
| `REPAIR dN` | `value += dN·λ`, **variance unchanged** (covariance-preserving additive; identical to `CycleSlipManager` repair arithmetic) | continuous — no re-float |
| `REFLOAT` | inflate `var` (×large) → data re-estimates | discontinuous |
| `DROP` | discard the carry entry (gap age-out / unrepairable) | discontinuous |

Diagonal-only carry is what makes `REFLOAT == inflate one diagonal` sound (no
off-diagonal cross-information to leave stale). **If/when** the reduced full-cov
form is adopted for AR parity, `REFLOAT`/`DROP` must instead Schur-out the
ambiguity's entire row/column of the local information — do **not** scale a single
diagonal entry. This binds the carry-form decision to the reset semantics: it is
a **prerequisite**, not a deferred knob.

### 3.7 Cold-start and silent lock-in

- **Cold-start:** at epoch 1 there is no prior. Estimate ambiguities **freely**
  from data, then carry. The carry **warmup** must be tied to the **clock/SH
  convergence horizon** (not a fixed epoch count) — the network is already
  ill-conditioned at epochs 1–6 (`project_global_sh_iono`); locking a carried
  prior before convergence freezes a not-yet-resolved ambiguity onto a
  contaminated epoch-1 value and propagates it.
- **Fading memory (lock-in guard):** add an optional bounded/fading-memory factor
  on the carry (cap accumulated info, or a small multiplicative forgetting per
  epoch). A sub-cycle/multipath bias that never trips MW/GF (code-blind to
  half-cycle phase errors) would otherwise persist indefinitely and contaminate
  the very products it was meant to sharpen. One scalar per carried entry.

---

## 4. Gate-vs-carry resolution (DECIDED)

**Decision: inject-before-gate + trustworthy carry + a withhold regression
test.** Rationale:

- A carried ambiguity prior **legitimately** makes a marginally-observed
  satellite's phase bias observable (pinning `N` separates `B` from `N`). This is
  *the point* of phase-leveling, not a bug. The rank gate should therefore
  measure **data + trustworthy-carry** observability, not be kept blind to the
  carry.
- The alternative (gate on a carry-free pass, fold carry post-gate like the D2
  shared carry) has **no cheap implementation**: a local-block delta does not pass
  through Schur unchanged, so it would need a Woodbury rank-update of the Schur
  complement or a full second `reduce()`. Rejected for v1.
- The residual risk — a **stale / just-slipped** carry letting the gate pass an
  underdetermined product it should withhold — is controlled **upstream** by carry
  trustworthiness, not by hiding the carry:
  1. **Convergence-gated warmup** (§3.7) — never carry an unconverged ambiguity.
  2. **Drop-on-slip** — any tier flag `REFLOAT`/`DROP`s the carry the same epoch.
  3. **Fading memory** (§3.7).
  4. **Regression test (required before Increment 1 ships):** a marginal-
     observability epoch (single-station satellite, weak geometry) still
     **withholds** with the carry ON. This is the firewall test that proves the
     carry only lifts *trustworthy* gauges.

---

## 5. Slip / outlier architecture — tiered, repair-first

Detection default for **product-emitting** runs is the post-fit cross-station
RAIM (T2 detection); the cheap tiers (T0, T1) are always-on; the only thing that
ever re-solves is T2's **conditional** re-solve, default OFF.

### T0 — MW/GF TurboEdit pre-processing WITH integer repair (front line)

- **Mechanism:** per-station, inside PASS-A/B before `splitStation`, run
  `MwSlipDetector::detectLargeMwResets` + `TurboEditResolver::resolve` on that
  station's observations → per-`(sat,signal)` integer `dN`. Where `dN` is known →
  `REPAIR` (carried `value += dN·λ`, variance unchanged → **continuous**, no
  reset). Where MW/GF flag a slip but `dN` is unresolvable → mark `REFLOAT`.
- **Reuse:** `MwSlipDetector::detectLargeMwResets` **as-is**; `TurboEditResolver`
  **as-is**; additive `dN·λ` repair arithmetic **as-is** (strip the BSSD-pivot
  refusal). The MW/GF arc state is per `(sat,sig1,sig2)` and receiver-agnostic.
- **New:** persistent per-station **arc-state containers on `NetStation`** (MAD
  window, GF TD history). Without persistence each epoch restarts cold and loses
  adaptive noise floors. Keep this state **station-local** (stays in the parallel
  PASS-A region; no reduction).
- **Cost / placement:** ~0.2 ms/station, fully inside the existing PASS-A OpenMP
  region; ~1 ms total at 305 sta, **off** the serial path. Always-on; toggle
  `network.cycle_slip.turbo_edit`.

### T1 — prefit innovation gating (cheap middle net)

- **Mechanism:** during PASS-B scatter, per phase row compute the prefit
  innovation vs the predicted carried-ambiguity + shared prior; if `|innov| >
  k·σ` (k ≈ 4–5), **de-weight** (not hard-drop — per the documented preference to
  avoid measurement loss) so a gross blunder cannot poison `N_rr`/the border this
  epoch. Catches what MW/GF miss (half-cycle, code-correlated, iono-row blunders).
  This tier exists **only because** carried state now gives a prediction (no
  network analogue existed before).
- **Cost / placement:** one comparison + weight scale per row, parallel; <0.1 ms.
  Always-on; toggle `network.cycle_slip.prefit_gate` (+ `k_sigma`).

### T2 — post-fit cross-station RAIM (last tier, conditional re-solve)

- **Mechanism:** after the shared solve + `reconstructStation`, compute per-
  station post-fit residuals (reuse `EpochMeasurement.residual_wtest / _mdb /
  _redundancy`). **Aggregate by satellite across stations:** a slip/outlier T0/T1
  missed shows as a `(sat,signal)` whose standardized residual is large at **many**
  stations at once (network RAIM / coherent-disturbance detector — the redundancy
  a 50–305-station network is uniquely able to exploit). Flag → `REFLOAT` next
  epoch; **only if** the coherent-outlier count exceeds a trigger **and**
  `network.conditional_raim` is ON, perform **one** single-pass re-solve with the
  offending rows de-weighted.
- **Self-confirming-loop fix (required):** the back-substituted `x_r` depends on
  the carried prior, so a biased carry contaminates the residual used to detect
  it. Compute the RAIM residual against a **carry-free** local solve, else the
  detector loses power on exactly the slow lock-in it targets.
- **Cross-station gather:** detection is always-on; the by-satellite aggregation
  over stations is a small **serial** gather (`O(n_sta · n_sat)`) that runs every
  epoch — budget it as non-zero. Keep per-station MW/GF MAD strictly station-local;
  if any **cross-station** MW MAD is added (cf. `feedback_mw_thresholds_empirical`),
  compute it in this T2 gather, not the PASS-A loop.
- **Cost:** residual scan parallel/cheap; a re-solve duplicates
  `reduce()`+LDLᵀ+solve (~+10 ms, ~doubling that epoch's serial budget) — fine if
  **rare**, catastrophic if per-epoch at scale.
- **Do not assume "rare."** The trigger is network-global, so its fire
  probability scales with **network size**, not per-station rate. **Benchmark the
  actual fire rate on a 50-sta 1-hour run with `conditional_raim` ON** before
  trusting it; default the response to **flag-and-reset-next-epoch** and keep the
  re-solve **OFF** for validate runs. Thresholds (`k_sigma`, trigger count) are
  provisional until tuned on dense multi-system data (a gating dependency —
  ORGN/WSRN creds per `project_network_atmosphere_readiness`).

### Batch reset (definition)

Per epoch, every tier appends to a single `ResetSet` of
`(station, sat, signal, action ∈ {REPAIR dN, REFLOAT, DROP})`. The set is applied
to `local_carry` **before** `addStationPrior` injection, so the **one** arrowhead
solve already reflects every reset — **no per-flag re-solve, no second
factorization.** Idempotent and order-independent within an epoch (each key once;
`REFLOAT`/`DROP` dominate `REPAIR` if a key is multiply-flagged). The **only**
thing that can force a second factorization is the T2 conditional re-solve, which
is a distinct, toggled, count-gated event. Keep the two concepts separate:
*batch reset = what the carried prior looks like going INTO the single solve;
conditional re-solve = re-running that solve after seeing post-fit residuals.*

---

## 6. Pivot policy (gauge protection)

Under phase-leveling every station's ambiguity is carried, so the pivot — whose
`w = 1e10` pins **solely** define the sat-phase-bias gauge (`declared_dimension`
is clock-only) — must be **actively** protected.

1. **Detect** a pivot slip via the same T0 pass (pivot is just a station; no
   special detector).
2. **Repair at the pivot requires a STRICTER bar than non-pivot stations.** A
   *wrong* `dN` applied to a pivot ambiguity shifts its absolute value straight
   into the emitted sat phase bias for **all** consumers — replay-deterministic,
   undetectable, and with **no** disc signal (repair emits none). So a
   confidently-wrong repair is the dangerous path. Require **MW ∧ GF agreement**
   **and** an FFR integer-validation **margin** **and** T2 cross-station consensus
   on that satellite before trusting a pivot repair to move the gauge. On repair,
   also shift clean non-slipped peers' carried ambiguities on that sat by `-dN·λ`
   to keep the frame consistent (the `project_pivot_handover_flipflop_fix`
   peer-shift) — note this peer-shift is a **second** discontinuity consumers must
   also see.
3. **Else HAND OVER:** pick a backup pivot from consensus-clean stations seeing
   that satellite (consensus-exclude + retired-pivot backstop), re-define the
   datum on the new pivot's pins, and emit an **explicit per-`(sat,signal)` phase-
   bias discontinuity bump** — signalled like the existing code-bias datum
   re-reference (`project_h1_codebias_discontinuity`: emitter derives the change
   from the epoch, replay-deterministic, no serialization; consumer reinflates).
   Confirm whether the consumer reinflate for a pivot-handover phase-bias disc is
   the **same** per-signal mechanism as H1 or a per-constellation frame shift.

Handover is the exceptional, continuity-breaking path; repair (well-gated) is the
common path for the pivot, reset/repair for everyone else.

---

## 7. User-side stack reuse map

| Piece | Verdict | Note |
|---|---|---|
| `MwSlipDetector::detectLargeMwResets` | **REUSE-AS-IS** | per-arc state, receiver-agnostic |
| `TurboEditResolver::resolve` | **REUSE-AS-IS** | per `(sat,sig1,sig2)` arc state; emits integer `dN` |
| Additive integer repair (`value += dN·λ`) | **REUSE** (extract helper) | strip the BSSD-pivot refusal |
| `SoftLliBridge` snapshot/restore | **REUSE** | per-key `(sat,signal)`; gap-bridge; per-station independent (no auto network-wide release) |
| Baarda w-test / MDB / redundancy | **REUSE** for residuals | but per-station univariate understates network redundancy → the **cross-station** aggregation is the soundness fix |
| `StateManager::resetAmbiguity` arithmetic | **REUSE** (concept) | covariance-preserving delta; the diagonal-reset analogue |
| `PivotManager` handover (`reReferenceAmbiguities`, T·P·Tᵀ) | **REBUILD** | SD-specific; no per-station SD pivot in the network; use the consensus-pick + explicit disc instead |

New per-station persistent state on `NetStation`: carried ambiguity entries +
MW/GF arc windows. Small, but **size-check at the largest test set** — this
codebase already hit a 146 GB→4 GB per-station-duplication cliff
(`project_network_global_zeroemit_perf`); reuse the shared-`ProductCache`
discipline.

---

## 8. Config toggles

Follow the project convention (private member + getter w/ Doxygen + `jval` parse
in the matching `loadXxxSection_` helper; add to `isLoggerEnabled` and the
toggle-summary line). **Every toggle's OFF state must be byte-identical to
today** — this is the bisection anchor for the whole feature.

| Key | Section | Default | Meaning |
|---|---|---|---|
| `network.phase_leveling` | estimation | `off` | `off` \| `on` (drop the `conditional` mode — no Schur cost to amortize per §1) |
| `network.phase_leveling_q_amb` | estimation | heuristic | carry process noise, `> PD floor` (§3.4) |
| `network.phase_leveling_warmup` | estimation | conv-tied | warmup before a carry is trusted (§3.7) |
| `network.cycle_slip.turbo_edit` | estimation | `on` (when leveling on) | T0 MW/GF + repair |
| `network.cycle_slip.prefit_gate` (+`k_sigma`) | estimation | `on` | T1 innovation de-weight |
| `network.conditional_raim` (+ trigger, `k_sigma`) | processing | `off` | T2 re-solve (detection always-on regardless) |

---

## 9. Phased rollout (smoke → 1 h; test on the ~50 globally distributed stations)

Per `feedback_test_cadence_smoke_first`: 10-min smoke catches config/gross; 1-h
catches slow divergence. Check `network_trace.csv` RMS across **all** epochs, not
just the head. Each increment's toggle OFF must be **byte-identical**.

- **Increment 1 — carry mechanism in isolation.** `addStationPrior` + the
  `local_carry` stash/inject loop (diagonal-only harvest, ordered keys,
  `var_eff = var + q_amb·dt`, inject before reduce, skip pivot columns). **No slip
  logic.** Behind `network.phase_leveling=off`. **Gate-vs-carry firewall test
  (§4) is part of this increment.** Smoke (≈50 sta, 10 min): (a) byte-identical
  OFF; (b) carried path runs with no singular `N_rr`; (c) `reduce()` runs exactly
  once/epoch with carry ON; (d) marginal-sat epoch still withholds; (e) sat
  clock/phase-bias RMS does **not** worsen and ideally **sharpens** vs the
  absolute baseline.
- **Increment 2 — T0 repair-only.** MW/GF TurboEdit per-station, `REPAIR` only
  (continuous `dN·λ`); persist `NetStation` arc state; no reflow/handover/pivot
  special-casing. Smoke with an induced non-pivot slip: carried ambiguity repairs
  continuously, products do not spike.
- **Increment 3 — reflow + T1.** `REFLOAT`/`DROP` for unrepairable T0 slips + T1
  prefit de-weighting. Induce an unrepairable slip and a gross blunder; verify
  clean re-estimation and no station drop. **Run the 1-hour cadence here.**
- **Increment 4 — pivot policy.** Repair-with-strict-bar at the pivot; consensus
  backup-pivot handover + explicit phase-bias disc (reuse H1 signalling) +
  peer-shift disc only when unrepairable. Smoke then 1-hour with an induced pivot
  slip: continuity on repair; clean replay-deterministic disc on handover.
- **Increment 5 — T2 RAIM.** Cross-station RAIM detection (always-on, flag-and-
  reset-next-epoch) against carry-free residuals; then the conditional single-pass
  re-solve behind `network.conditional_raim` + trigger. **Benchmark the re-solve
  fire rate** on a 50-sta 1-h run; confirm clean epochs never re-solve and serial
  budget stays within turnaround. Then a longer multi-system run.

---

## 10. Risks / open items

- **Gate firewall (§4):** Increment 1 does not ship until the marginal-
  observability withhold test passes with the carry ON.
- **`reduce()`-once invariant (§3.5):** assert it; a stray post-gate local
  injection silently doubles the serial path.
- **`q_amb` PD floor (§3.4):** too-small `q_amb` drops whole stations.
- **Pivot mis-repair (§6):** the one path that silently biases the gauge for all
  consumers — gate it hardest.
- **T2 fire rate (§5):** measure, don't assume; keep re-solve OFF for validate.
- **Carry form (§3.6):** diagonal-only for v1 binds `REFLOAT` correctness; the
  reduced-cov form for AR parity changes the reset primitive — decide before
  adopting it.
- **Determinism (§3.3):** ordered carry keys; preserve byte-identical
  serial≡parallel.
- **RAIM thresholds (§5):** provisional until tuned on dense multi-system data
  (creds-gated).
- **Natural next layer:** once ambiguities are carried + integer-leveled by T0
  repair, producer-side AR (`network_ambiguity_resolution_design.md`) becomes the
  obvious follow-on — the carried float block + conditional covariance it needs
  are exactly what this doc stands up.

---

## 11. Correctness notes (for the reviewer)

- **Per-station prior is reduction-orthogonal:** the Schur subtrahend
  `N_sr·N_rr⁻¹·N_rs` is a function of one station's blocks only, so a carried
  prior on `N_rr` cannot perturb any other station's contribution or the shared
  null-space — it conditions only that station's `x_r`.
- **Batched reset == sequential reset, exactly:** normal-equation assembly is
  additive and order-independent; folding N resets into `local_carry` before the
  single `reduce()` yields the bit-identical system any sequential ordering would.
  This is **not** the "sequential KF == batch KF" identity (which needs a shared
  linearization point) — it is the stronger order-independence of additive
  information assembly.
- **`REFLOAT` correctness depends on diagonal-only carry:** with no off-diagonal
  information there is nothing cross to leave stale when one diagonal is inflated.
