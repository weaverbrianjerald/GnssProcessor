# SPEC — Lane-coordinate carry (M5 card "WL-3")

**Status:** IMPLEMENTED (Jul-16 2026, Fable) — increments 1–4 landed + quick-gated;
see `docs/HANDOFF_lane_carry_card.md` for the A/B results (constrained circR 0.471
vs WL-2's 0.343, wobble gone, injects retired) and the MEASURED spec deviations
(§3.1 fresh-prior + q numerology falsified → diffuse birth + q_wl_scale 0.01;
§3.3 fold-on-change → idempotent per-decided-epoch lane SET; census-preferred pair
+ spanning-forest completion; §6.1 strict field equivalence reframed as structural
parity — the ≤1e-9 gate lives in the unit test).  Increment 5 (dual-freq grading)
awaits command.  Default-OFF, byte-identical when off (bit-exact re-verified).

**One sentence:** carry each primary-pair chord's TWO band DDs as one jointly-
covarianced block — stiff along the phase-determined (NL) direction, honestly loose
along the WL lane — and feed the lane per-epoch MW innovations (increments, never
re-folded levels), so the carry itself becomes the code-coherence accumulator and
the WL tightens continuously instead of by discrete injects.

**Relation:** the durable form of F6-fix option (B) (probe-endorsed,
`mw-wl-f6-fix-directions`), scheduled now that WL-2 is green because the F-A
attribution MEASURED the failure class this card removes (a re-folded statistic
against an over-stiff carry elevates D5 network-wide through the shared border),
and the F-B lane trace measured the residual roughness of discrete injects
(constrained-cycle med|f| oscillating 0.01↔0.30 across epochs at 15 ep).  WL-2's
statistics (MW, bridge, iono de-bias), FFR gates, closure check, quarantine, force-
fire harness, and the lane-trace grader carry over UNCHANGED as this card's
measurement sources and acceptance instruments.

---

## 1. Motivation (measured facts, do not relitigate)

- F6: per-band stiff carries pin the WL lane at unearned σ≈0.085 at code-seed
  garbage; rows lose (float) or churn (stiff).  Probe: authority transfers values
  but stiff-vs-stiff sprays D5 (+9%); loose-lane is the durable direction.
- F-A: 147 σ0.01 EWL rows re-folded per epoch elevated D5 ~2× NETWORK-WIDE
  (border-mediated; 24% of flags on untouched signal groups).  Cured by not
  folding — proving "statistic re-folded as independent measurement" is a real,
  measured failure class.  WL-2 v1 therefore acts by discrete injects only.
- F-B trace: injected/held cycles reach circR 0.34 vs control 0.019 — the lift is
  real but ROUGH (re-taps knock lanes off between injects; med|f| oscillates
  0.01↔0.30 per epoch).  A carried loose lane receiving per-epoch increments
  tightens as √N and re-tightens itself after re-taps without waiting for the
  next FFR/disagree trigger.
- σ̂₀ honesty: the WL-1 §11 Q4 tripwire (LS-VCE ≫ 1 under the loose bolt-on)
  fired at 1.86 vs 1.42 control in the fold-on shape — the pre-registered
  criterion for going tight.

## 2. Representation (the core design decision)

**One carry group per (constellation, primary signal PAIR)** replaces the two
per-band groups for those signals.  Within a pair group:

- The arc graph is the SAME (rx, sat) simple graph both bands share (v1 requires
  band-2 presence for membership; a band-1-only arc stays in a residual per-band
  group as today — no information lost, no multigraph).
- Each graph chord carries a **2-vector**: z = (z₁, z₂), the two bands' cycle DDs
  (correction frame, meters — exactly today's per-band z's), with a joint 2K×2K
  per-component covariance Pᶻ (K chords).  Equivalently, the (NL, WL) rotation —
  the LANE basis — is applied only to the PROCESS MODEL and the measurement
  update, never to storage: storage stays per-band (zero-migration reconcile, the
  W projection applies per band unchanged, all lifecycle algebra intact).
- `buildDdPriors` emits per component ONE DdPriorBlock with 2K rows — the K
  band-1 cycles then the K band-2 cycles (both on their own columns; Terms
  machinery already supports it) — with the joint 2K×2K Pᶻ.  The fold is already
  rank-general; `installPosterior` reads the joint posterior back.  D5 gates per
  row exactly as today (a per-band innovation IS still per-band).

Why this shape: it changes NO cycle algebra (tree, reconcile, W, birth/death all
per band as today, just co-located), adds no states, and the ONLY new math is the
covariance coupling + the lane-aware process noise + the lane measurement rows.

## 3. Mechanism

### 3.1 Lane-aware process noise (the honesty fix)

Per chord, per epoch, in the lane basis: q applied as
Q = Rᵀ·diag(q_nl, q_wl)·R with R the (unit-normalized) NL/WL rotation for the
pair's (λ₁, λ₂).  Defaults: `q_nl` = today's `carry_invariant_qz` (phase owns
NL); `q_wl` LARGE at birth-equivalent scale... no — q is a RATE; the honest
statement is the PRIOR: a fresh chord's Pᶻ gets the lane-basis prior
diag(σ_nl_fresh², σ_wl_fresh²) with σ_wl_fresh ≈ the code-seed lane error
(~cycles·λ) instead of the isotropic diffuse prior; thereafter `q_wl` a modest
rate (default: 10³ × q_nl) so the lane never re-earns stiffness it wasn't
measured into.  Both defaults config knobs (§5).

### 3.2 Lane measurement rows (increments, never levels)

Per epoch, per eligible cycle: ONE lane row per pair-group component block —
the WL functional row (+c/λ₁, −c/λ₂ on the cycle's columns... realized on the
CARRIED block, not the estimator columns) with
**z = THIS EPOCH'S MW(1,2) cycle DD** (the detector's per-epoch value, NOT the
Welford mean) and R = the per-epoch MW noise (from the detector's running
variance, floored).  Folded INTO the carry block prior at buildDdPriors time
(a KF measurement update on (z, Pᶻ) before projection — driver-internal, closed
form, K-small).  Each epoch's code information enters ONCE; the Welford
accumulation now happens INSIDE the carried covariance — the estimator owns the
accumulator (the WL-1 §11 Q4 "tight" form, scoped to the carry).
- The BRIDGE (EWL-fixed cycles) supplies a second, phase-precision lane
  measurement the same way (per-epoch GF_b increment, iono-de-biased per WL-2
  §3.4 v2).  Same once-only discipline.
- The detector Welford means remain DECISION-side only (FFR integer fixing).

### 3.3 What replaces the WL-2 inject/holds

Nothing to fight ⇒ nothing to exempt: with an honestly loose lane, the lane
measurements pull the carried lane to the integers continuously; FFR-fixing
(statistic-gated, unchanged) then collapses the lane variance to
`fix_weight_sigma_cyc` via ONE prior edit per integer decision change
(fold-on-change — the F-A lesson, now structural).  `injectCarriedChord` and the
D5 exemption remain as the slip/re-tap fallback path but should fire ~never in
steady state (telemetry proves it).  WL-2's inject machinery stays compiled and
default-armed under `wl_inject_enable` for the non-lane-carry shape (A/B
comparability); `lane_carry.enable` supersedes it at runtime (mutex-warned).

### 3.4 Lifecycle

Per-band algebra unchanged (reconcile/W per band; merge/split/swap co-located).
A slip on EITHER band's arc refloats the chord's BOTH bands (they share the
graph edge; today's funnel already keys per ambiguity — union them).  A fresh
chord re-enters with the §3.1 lane-aware prior — the post-re-tap lane loosens
honestly and the next epochs' lane measurements re-tighten it: the F-B wobble
self-heals without a trigger.

## 4. Estimability / invariants

Same closed-cycle class throughout (both band rows are today's carried DDs; the
lane rotation is a regular linear reparametrization of the SAME invariant pair).
No datum touched; FIREWALL gate re-run mechanically.  Standing invariant #1: the
stiff NL direction keeps today's slip funnel; the WL direction is never stiff
without measurement mass — the invariant's precondition dissolves by design and
the force-fire battery (§6) proves the response anyway.

## 5. Config (`network.lane_carry.*`)

| key | default | meaning |
|---|---|---|
| `enable` | false | Master.  Requires `carry_invariant` + `mw_wl.ewl_enable` (statistics source).  OFF ⇒ byte-identical.  **Landed as a parsed + fail-loud-inert contract (Jul-16); mechanism NOT yet implemented.** |
| `q_wl_scale` | 1000.0 | q_wl = scale × carry_invariant_qz along the lane. |
| `sigma_wl_fresh_cyc` | 3.0 | Fresh-chord lane prior σ (cycles) — the honest code-seed scale. |
| `lane_meas` | true | Per-epoch MW(1,2) lane increments (§3.2). |
| `bridge_meas` | true | Per-epoch bridge lane increments on EWL-fixed cycles. |

## 6. Validation (quick-only until commanded otherwise)

1. **Equivalence gate (unit + 15-ep):** pair-group with lane features OFF
   (q_wl_scale=1, isotropic fresh prior, no lane rows) must reproduce the two
   per-band groups' carried values/counters to ≤1e-9 (not byte — summation order
   changes); D5 totals identical.  THE structural safety net for the regroup.
2. Unit battery: lane rotation round-trip; lane-aware prior/q; per-epoch lane
   update hand value; fold-on-change fixing; slip → both bands refloat.
3. §7.3 force-fire re-run (harness exists) — evaporate → re-warm → re-tighten
   WITHOUT inject; no wrong integer (closure + census truth).
4. 15-ep A/B vs the WL-2 green shape (`_wlver_50sta_15ep_wlinj.json`): PASS =
   D5 ≤ 358; lane-trace constrained circR ≥ 0.34 with the per-epoch WOBBLE GONE
   (med|f| stable ≤0.1 after fix onset); injects ≈ 0 (superseded by continuous
   tightening); dual-freq cycles (no bridge) begin visibly tightening by ep ~15
   (the √N accumulation the per-epoch producer never had).
5. Perf: blocks double in K per pair component (2K rows; Minv (2K)²·K cost) —
   measure with D0/P0b; the C2 impossibility analysis bounds expectations.

## 7. Increments

1. **1 (landed): config contract** — parsed, clamped, mutex-checked, loud
   NOT-implemented warn (the WL-1 Phase-B precedent).
2. **2: pair-group foundation + equivalence gate** — regroup, joint blocks,
   block-diagonal Pᶻ, §6.1 proof.  NO lane features.  The risky increment;
   everything after it is small.
3. **3: lane-aware prior/q + per-epoch MW lane rows** (§3.1–3.2) + A/B.
4. **4: bridge rows + fold-on-change fixing + inject retirement telemetry** + the
   full §6 battery.
5. 5: dual-freq grading (the card's unique win: WL accumulation with NO third
   band) + graded run on command.

## 8. Open questions for the implementer

1. Residual per-band group for band-1-only arcs: confirm the driver tolerates a
   station appearing in both the pair group and the residual group (disjoint
   ambiguities ⇒ disjoint columns — expected yes).
2. The lane KF update inside buildDdPriors vs a driver pre-step: pick the seam
   that keeps `beginEpoch` q-application and the update ORDER deterministic
   under parallel_carry (serial seam like the WL-2 statistics reads).
3. Per-epoch MW value accessor: the detector keeps the last raw MW per arc
   (`prev_mw`) — expose a lagged per-epoch value + its epoch noise honestly
   (NOT the Welford mean; watch the F1-lag discipline).
4. D5 per-lane: with 2K rows the flag stays per band-row; decide whether a
   lane-basis flag (NL vs WL innovations separately) reads cleaner — diagnostic
   first, gate change only if measured.
