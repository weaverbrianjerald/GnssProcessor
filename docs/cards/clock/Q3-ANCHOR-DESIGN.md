# Q3 design — the guarded temporal clock anchor — the measured basis, stage A/B design, the empirical derivation and the pre-registered gates

**STATUS**

- **id:** Q3 design
- **topic:** clock
- **status:** closed
- **summary:** the guarded temporal clock anchor — the measured basis, stage A/B design, the empirical derivation and the pre-registered gates
- **dates in the text:** 2026-08-10 → 2026-08-15
- **knobs / posture:** `network.clock_model`, `network.clock_temporal_anchor`, `network.clock_anchor_gauge_free`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/clkr_12h_hold`, `runs/cta_3h_`
- **source:** `docs/archive/SPEC_clock_temporal_anchor.md` — (preamble), §1, 1.1, §2, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, §3, §4, §4.1

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_clock_temporal_anchor.md -->
# SPEC — Card Q3: the guarded temporal clock anchor (CTA)
### (give the per-satellite clock ESTIMABLE CONTENT temporal stiffness — the estimator-side half that CLK-R, C1 and FC-1 all named as their prerequisite)

2026-08-10.  Owns the `per-sat clock VALUE` row of
`docs/SPEC_sbasis_realization_contract.md` §C — the one row whose "estimator-side
continuity" cell reads **missing**.  Successor to `docs/SPEC_clock_emit_rebase.md` (CLK-R:
the emission-side half — the common-mode gauge is HELD, the per-sat walk explicitly *not
mediatable at emission*, its §2 falsification), to the NO-GO pair
`docs/SPEC_c1_clock_rate_validation.md` (which also carries the `network.clock_model` NO-GO,
§10 there), and to `docs/SPEC_fast_clock_split_cadence.md` §12–§16 (FC-1; the REPORT that
carried those results was absorbed into it 2026-08-15 and lives at
`docs/attic/REPORT_fc1_split_cadence.md`) — all blocked on this card by measurement.

---

## §1 Measured basis — what the walk actually is

All numbers from `runs/clkr_12h_hold/products.txt` (the 12 h denominator for the internal
golden63 posture; `emit_clock_frame_hold` armed, so the common-mode gauge is held and the
per-sat channel is clean).  Reproduced by `python scripts/product_step_census.py
<products.txt>` (dev channel) — 1439 steps × 32 satellites = 45 271 rows.

| statistic | value |
|---|---|
| per-sat common-removed step \|Δc0 − med\| — **median** | **6.23 cm** |
| p90 / p99 / max | 21.11 cm / 67.69 cm / 10.03 m |
| robust σ_w = 1.4826·MAD over all rows | **9.23 cm / 30 s** |
| **per-satellite** robust σ_w, min … median … max over 32 sats | **7.93 … 9.07 … 10.87 cm** |
| physical change over 30 s (FC-1, carrier-phase measured) | **1.7 – 2.0 cm**, corr ≈ 0 |

**The armed carry does not bind.**  The lineage runs
`filter.process_noise.sat_clock_q = 1e-3 m²/s` (the Jul-31 sweep's adopted value), so the
carry's one-epoch spread is √(1e-3·30) = **17.3 cm**, nearly twice the realized 9.2 cm.  A
random-walk prior constrains only when its spread is BELOW the data's per-epoch spread;
above it the prior is a bystander and the clock is re-drawn from each epoch's data.  **The
walk is the satellite clock's per-epoch ESTIMATION noise, published at 30 s cadence.**

**The per-satellite uniformity is the proof.**  Physical stability spans orders of magnitude
across the GPS fleet (Rb vs Cs, IIR/IIF/III); measured σ_w spans **1.37×** (7.93–10.87 cm,
median 9.07).  No physical process is that uniform across that fleet; an estimator's
per-epoch noise under common geometry and a common weight model is.  (FC-1 reached the same
verdict from the other side: corr(producer Δc0, phase-measured) ≈ 0 at every hour.)

**Two exact identities used throughout** (scalar KF, prior N(μ, P⁻), data information 1/R,
posterior P⁺ = (1/P⁻ + 1/R)⁻¹):

1. `Var(x̂ − μ) = P⁻ − P⁺`, **exactly** — the variance of the guard statistic (§2.3), and
   precisely the `disp` the prior-residual census's folded branch already computes
   (`NetworkPriorResidual::addRow`, `disp = r·fold_var == fold_var − post_var`).  It is
   Teunissen's *generalized predicted residual* for a predictable function — the
   GKF-conformant test, and the same B3 data-incest-free form `NetworkClockQc` already uses
   for the two-state model.
2. Once the carry binds (q·Δt ≪ R), `Var(x̂_k − x̂_{k−1}) → q·Δt`, so the emitted walk is
   **σ_w = √(q·Δt)** and the ladder reads off the armed q: 1e-4 → 5.5 cm, 3e-5 → 3.0 cm,
   1e-5 → 1.7 cm.  The physical floor (2 cm) sits at q ≈ 1.3e-5 m²/s.

### 1.1 Why this is NOT the Jul-31 `sat_clock_q` sweep repeated

That sweep (memory `project_clock_model_hadamard_calib`) walked 1e-2 → 3e-4, was monotone
better on every metric through the bracket, and stopped for one stated reason: *"a
stiffer-than-measured carry is exactly the invariant-#1 class"*.  **That is this card.**  Q3
is not a q value; it is the guard that makes a binding q admissible, plus the per-satellite
elapsed-time bookkeeping without which a binding q is actively dangerous (§2.2).  The q
ladder is the last increment, not the first.

---

## §2 Design

One new seam, `filter/NetworkClockAnchor.{hpp,cpp}`, plus a variance decision inside the
existing D2 carry fold.  **Default OFF; OFF is structurally byte-identical** (no new fold,
state or trace row).

### 2.1 Stage A — the anchor (what changes in the fold)

`network.clock_temporal_anchor` (bool, default **false**) turns the satellite clock's D2
carry variance from `var_eff = cp.var + sat_clock_q · dt_epoch` into

```
var_eff = max( cp.var + clock_anchor_q · Δt_sat ,  clock_anchor_var_floor_m2 )
```

applied **in both D2 branches** (the diagonal fold and the C1 dense fold's `cov_eff`
diagonal): the marginal prior on any one state must be identical either way, which is what
makes the guard statistic path-independent (`networkInjectD2CarryPrior`'s own comment).

| knob (`network.*`, inert unless armed) | meaning |
|---|---|
| `clock_anchor_q` | the anchor's random-walk rate, m²/s.  Emitted walk = √(q·Δt). |
| `clock_anchor_var_floor_m2` | **the deviation-#1 consistency floor**.  Caps folded prior information at 1/floor so a diagonal-or-dense independence-assuming carry cannot accumulate unbounded information against a time-correlated error floor.  Clock analogue of the B4 `pb_var_floor` / `cb_var_floor` (whose comment says *"clock is out of scope (own two-state model / monitor)"* — that exemption ends here), and ranked-plan item (3) of `project_network_ssystem_dia_assessment`, "var-floor default for stiff carries". |
| `clock_anchor_max_gap_s` | beyond this per-satellite elapsed time the carried clock is STALE: not folded, satellite DECOUPLED (§2.4). |

**Covariance treatment, decided consciously.**  The anchor rides the EXISTING D2 fold rather
than adding a second prior.  Under `shared_carry_full_cov` (armed here) the clock stays in
the C1 dense block, so its off-diagonals are retained — the anchor does not re-create the
independence assumption the C1 card removed, on the very state it makes stiffest.  Under
`carry_invariant` (also armed) the shared PhaseBias carry is not stashed at all, so
deviation #1's named worst case (clock↔PB) is not in the block; what the dense path retains
is **clock↔CB**, exactly the pair the DOY-129 ratchet ran through — the safer choice, not
the neutral one.  The residual inconsistency of ANY stiff carry (information accumulating ~n
against a time-correlated floor) is bounded by the variance floor regardless of branch.  A
refloat (§2.4) is a **marginalization** — drop the key's row/col and re-invert — which
`addSharedPriorDense` documents as exact for a departed key ("Dropping rows of Λ would
instead condition on the departed key — wrong statistics for every survivor").

**✔ BUILT 2026-08-10 as `network.clock_anchor_gauge_free` (default OFF, inside the anchor's
fingerprint).**  Original open question: the anchor as built constrains the RAW x̃ˢ, not only
its datum-invariant part.  The per-constellation clock common mode is a nuisance direction
fixed by the pivot pin whose measured emitted step (~8.5 cm) is above every rung of the
ladder, so a binding raw-state anchor resists it and the resistance lands on the pivot
satellite's rows rather than on the estimable between-satellite differences (a diagonal prior
with near-equal variances separates exactly into common ⊕ estimable, so the differences stay
unbiased to first order).  The clean fix folds the prior **gauge-free**: one rank-1 downdate
of the folded information along the constellation's common direction u,
`Λ ← Λ − Λu(uᵀΛu)⁻¹uᵀΛ`, putting ZERO prior information on the gauge, identical on the
diagonal and dense paths — the GKF *"carry on FUNCTIONS, not states"* form, datum-invariant
at the MECHANISM level rather than only at grading level.  Deliberately NOT built at the time
(a second unmeasured mechanism inside the first breaks one-knob-per-A/B); pre-registered in
G1/G2 with an exact signature — σ̂₀ code and the within-epoch RAIM trip rate rising vs
control, concentrated on the pivot's rows.  **Measured: it does not express** — σ̂₀ code
4.2707 → 4.2698 at 20 ep and 5.0213 → 5.0886 at 1 h, RAIM 0 trips in every arm (§5).
Re-check at 12 h and 305 stations before treating that as settled.

**MEASURED (3 h pair `runs/cta_3h_{ctl,on}`): the defect is real and quantified.**  The
raw-state anchor cut CLK-R's drained common mode **Σ|μ| 49.43 m → 27.77 m** and the pivot's
dev row (which IS the drained common mode — §4.1) **16.79 → 10.03 cm** — two independent
instruments, same 0.56–0.60 ratio: **the carry was doing ~44 % of the datum's work.**

**⚠ CORRECTED EXPECTATION for the gauge-free arm — the obvious prediction has the sign
backwards.**  Removing the prior's information along the gauge hands the common mode back to
the data alone, so **Σ|μ| and the pivot dev row must RISE toward the control's level
(49 m / 16.8 cm)**, not fall.  That rise is the mechanism working, and it is harmless on the
wire because CLK-R's frame hold drains that channel at emission.  What must IMPROVE if the
fold is worth arming is the **estimable** side: ex-pivot dev ceiling and floor, σ̂₀_code, and
the consumer.  Pre-registered as such before the arm was run.

### 2.2 Per-satellite elapsed time — the latent defect a binding q would detonate

`dt` in the driver is time since the PREVIOUS EPOCH, and every skip path (`gate_withheld`,
σ̂₀ withhold, solve failure) advances `prev_tow` while the D2 carry survives untouched — so a
carried clock can be N epochs old while `dt` still reads 30 s.  At the armed 1e-3 this is
invisible (2× under-inflation of a prior that never binds).  At an anchor q it is the
invariant-#1 catastrophe verbatim: the DOY-129 SH gate withheld **278 consecutive epochs**
(95+ min), after which the carry would fold as if 30 s old.  Fix (same class as the clock
model's own P0-3 / R11 fix, `osc.t_last`): a per-satellite `t_last` stamped at every FOLD,
`Δt_sat = pl_now_s − t_last(sat)`, and `Δt_sat > clock_anchor_max_gap_s` ⇒ DECOUPLE (§2.4)
rather than fold a stale value.

### 2.3 Stage B — the guard (the statistic)

Per satellite `s` with a clock prior folded this epoch, on the solved `x̂`:

```
e_s  = x̂_s − μ_s                                  (posterior − folded prior value)
ê_s  = e_s − median_{t : c(t)=c(s)} e_t            (per-constellation common mode)
S_s  = max( var_eff,s − var_post,s , σ_floor² )    (= P⁻ − P⁺, exact under H0)
w_s  = ê_s / √S_s                                  ~ N(0,1) under H0
```

- The **median removal is mandatory**: the per-constellation clock common mode is a free
  gauge (Odijk Eq. 41 class, `q_rx_clock = 1000`), so `e` carries the epoch's whole gauge
  re-realization on every satellite (invariant #5).  It is also what `NetworkClockQc`'s NIS
  path and CLK-R's census statistic both do.
- `S = var_eff − var_post` is exact (§1 identity 1).  The floor `σ_floor²`
  (`clock_anchor_sigma_floor_m`, default the measurement-noise scale) prevents the 0/0
  blow-up on a weak-data epoch — same reasoning as the B3 fix's `max(..., clk_model_r)`.
- Trip when `|w_s| > clock_anchor_k` **or** `|ê_s| > clock_anchor_abs_m`; the absolute arm is
  a floor-independent backstop that fires on a metre-class step even if `S` were inflated by
  a mis-set floor.

**Both thresholds are derived from measured data, never from the stochastic model** (§3).

### 2.4 Stage B — the response (adapt-and-EMIT, both edges marked)

```
ANCHORED  ──trip / stale gap / resume seam / datum change / external refloat──▶  LOOSE(0)
LOOSE(n)  ──|w| ≤ k for n ≥ clock_anchor_recouple_epochs consecutive epochs──▶  ANCHORED
```

- **LOOSE folds at the legacy `sat_clock_q`** — today's behaviour, which discharges
  invariant #7's *"the DECOUPLING edge must bound what the decoupled states can accumulate"*
  by construction: a decoupled satellite sits on the pre-card carry, already bounded by the
  existing `clock_runaway_check` monitor (`clk_mon_abs`, `clk_mon_jump`).
- **In-epoch response (the adapt-and-EMIT half).**  The tripped satellites' clock priors are
  **withdrawn from the shared system and the epoch is re-solved**, so the EMITTED value for
  the trip epoch is the data's, not the stale anchor's.  The re-solve reuses the kernel's
  already-reduced normals (a shared-border information delta carries through the Schur
  reduction unchanged — `NetworkNormalEquations::addSharedInfo`'s reduction-preserving fast
  path), costing one datum + factorization of the K×K reduced system: **no re-reduction, no
  re-assembly**.  Capped at `kCtaMaxPasses = 3` (a pass can change the median and expose a
  second satellite).
- **Placement: immediately after the primary solve, BEFORE the within-epoch RAIM
  adaptation.**  Load-bearing: a stiff prior fighting a real step dumps tension into
  residuals, raising σ̂₀, and within-epoch RAIM would then attribute a PRIOR fault to a
  satellite's or station's DATA and drop it.  The `cap_priors` / `cap_dense_*` replay channel
  is updated in step, so a subsequent RAIM re-solve cannot silently re-inject the withdrawn
  prior.
- **Never deweight** (no measurement's weight is touched at either edge) and **never
  withhold** (the epoch always emits; a withhold is graded as an adaptation failure).
- **The decoupling edge is MARKED on the wire**: the tripped satellite is queued into a
  per-sat discontinuity pending set drained by `networkSolPrep` →
  `emitter.markSatDiscontinuity` → R record + per-sat phase/code/clock counters — the B1
  retro-bump chain Q4 proved end-to-end at the consumer, which must not integrate across a
  satellite whose clock just changed temporal regime AND value.  Marks land in the SAME epoch
  as the refloat (SolPrep runs before EmitStore).
- **The re-coupling edge is an EVENT with identity and a re-seed, and is NOT wire-marked.**
  Re-engaging a prior at the carried value adds information without stepping the emitted
  value (the prior's mean IS the last posterior), so there is nothing for a consumer to
  reset.  Invariant #7's re-validation is the `recouple_epochs` in-band run; the re-seed is
  that the first anchored fold after LOOSE uses `cp.var + q_a·Δt` with `cp.var` the LOOSE
  posterior — the larger honest variance, not the steady-state tight one.  Event
  `cta_recouple` carries the satellite, the consecutive-in-band count and the realized
  dispersion.

### 2.5 Telemetry (standing invariant #4)

Armed-only run-end line via `networkTeardown` plus `CLK_ANCHOR`-tier `trace_events` rows at
every edge: `epochs armed · keys folded at the anchor · keys folded LOOSE · trips (by reason:
nis / abs / gap / resume / datum / external) · in-epoch re-solves · recouples · loose-epoch
mass · max |w| and its identity · realized dev-channel σ_w`.
**0 folded = the mechanism is inert.**  Counters, not the absence of crashes, are the
ON-path evidence.

### 2.6 Fingerprint class

**Solve-affecting ⇒ FINGERPRINTED ⇒ fresh-convergence arms only.**  A snapshot written by an
anchored run carries variances from a different temporal model; a resume across the flip
would mix them inside one lineage.  Registered in the snapshot fingerprint alongside the
other solve-affecting knobs.

---

## §3 The empirical derivation (two-pass, pre-registered)

Never from the stochastic model — the standing directive; the Jul-31 sweep already showed
the "measured WFM q1" was itself churn-contaminated because it was measured under an unbound
carry.

**INC-0 instrument — the `clk` family of the prior-residual census.**  The census already
standardizes the statistic this card needs (`disp = fold_var − post_var`), keeps a per-family
σ̂₀ and a quantile LADDER of |t|, and reports the max row's identity; its clock family was
excluded by one line (*"Clock / VtecSh out of scope (own models)"*).  Widening it is
trace-only and byte-identical.  The clk family feeds the **common-mode-removed** ê (§2.3),
unlike pb/cb — the clock is the one family with a free per-epoch gauge, and standardizing the
raw `e` would report the gauge on every satellite.

**Pass 1 — locate the binding knee with σ̂₀_clk.**  Non-binding, the realized dispersion is
data-set, so σ̂₀ = √((σ_true² + R)/(q·Δt)) < 1 and falls as q rises; once binding,
σ̂₀ → √(1 + σ_true²/R) ≈ 1, **independent of q**.  So **σ̂₀_clk = 1 marks the binding knee
exactly**, as a measurement.  From §1 the knee is expected near q·Δt ≈ σ_w² = 8.5e-3 ⇒
q ≈ 2.8e-4 — to be MEASURED, not assumed.

**Pass 2 — the ladder below the knee.**  σ̂₀ saturates below the knee, so the descent is
graded on the wire and the consumer: `q_anchor ∈ {knee, knee/10, 1.3e-5}` (the last = the
FC-1 physical floor).  **k** is read off the measured |t| ladder at the armed q with the
guard disarmed (`clock_anchor_k` beyond the top rung) so the distribution is the un-truncated
null.

**⚠ The trip-rate target stated in v1 (“≤ ~1–2 network-wide trips per hour”) was MIS-SCOPED
and is withdrawn.**  It was imported from CLK-R, whose marked population was ~98 % estimator
churn, and applied to a population that is not churn at all.  Measured on the 3 h arm:
**22 trips/h** through the DOY-129 hour-2–3 run-up, with the live consumer BETTER in every
hour of that window.  **The rate is a property of the DATA, not a tuning target; tuning k to
hit a quiet-hour number would tune the detector to under-report activity on exactly the days
the card exists for.**  What replaces it:

- **Grade the trip population, never its count.**  A trip is healthy when it (a) resolves
  in-epoch (`cta_resolves` tracks `trips`), (b) lands a wire mark tying out event ↔ R record
  ↔ counter, (c) leaves the consumer unchanged-or-better over the window containing it.
- **A rate floor is still meaningful in the other direction**: ~0 trips over a window with
  known clock activity means the guard is not firing when it should — the invariant-#1
  failure G4 exists to exclude.
- k stays derived from the ladder for CALIBRATION (so |w| means the same thing across arms),
  not from a target rate.

**`clock_anchor_abs_m`** likewise from the census tail: the dev channel's p99 is 0.68 m and
max 10.0 m over 12 h, so a backstop in the 1.5–3 m band isolates genuine steps without
touching the body of the distribution.

---

## §4 Pre-registered gates

- **G0 — OFF-path byte-identity.**  `_golden63_gps_12h_internal.json` (and the 20-ep smoke)
  re-run unchanged ⇒ `products.txt` record stream byte-identical to the stored run; unit
  suite green.  Doubles as the same-build control for every arm below.
- **G1 — 20-epoch ON smoke.**  Counters live (folded > 0); folded variance is the armed one;
  trips ≈ 0; no withholds; console shows the armed line.  `folded = 0` FAILS regardless of
  how clean the log is.
- **G2 — 1 h ON.**  σ̂₀_clk from the census (the knee read); dev-channel σ_w vs the 9.23 cm
  control; no new withholds; `total_IF` no-regression; trip population healthy per §3
  (resolve + mark + tie-out), NOT a trip count.
- **⚠ The per-satellite σ_w SPREAD prediction is withdrawn.**  v1 pre-registered that the
  spread should SHRINK as the anchor binds.  It does not, and the reason is structural: **the
  population bifurcates BY DESIGN.**  An anchored quiet satellite is driven toward the process
  floor √(q·Δt) (measured floor sat: 2.37 cm ≈ FC-1's ~2 cm physical), while a tripped
  satellite is deliberately handed back to the LEGACY carry for its loose window and marked,
  keeping the pre-card dispersion on purpose because that is what bounds a decoupled state
  (invariant #7).  A statistic mixing the two must widen when the mechanism works.  Measured
  3 h: spread 3.11× → 4.23× including the pivot, 2.61× → 2.95× excluding it.  **Grade the
  FLOOR and the CEILING SEPARATELY (§4.1), never the ratio.**

### §4.1 Read the dev census PIVOT-AWARE — the ceiling row is a gauge

With `emit_clock_frame_hold` armed (the golden posture) the pivot satellite's raw clock is
pinned to 0, so its EMITTED value is exactly `−A_c`, the frame accumulator, and its
dev-channel row is **`Δ(−A_c) = −μ`, the per-epoch drained common mode** — a gauge quantity,
the only row in the census that is.  Verified on the 3 h pair: pivot G01 is the ceiling in
BOTH arms (16.79 / 10.03 cm vs a fleet median of 8.64 / 4.33), and its σ ratio across the
arms (0.60) matches CLK-R's independent Σ|μ| counter ratio (27.77 / 49.43 = 0.56).

1. **Quote the ex-pivot ceiling.**  Including the pivot row puts a gauge into a statistic the
   card grades as estimable content — an invariant-#5 contamination of the GRADER.  Ex-pivot
   ceiling on the 3 h pair: 14.08 → 6.98 cm (−50 %), against 16.79 → 10.03 (−40 %) with it.
2. **The pivot row is the direct read of §2.1** — how much of the gauge's work the carry is
   doing.  The raw-state anchor cut the drained common mode 49.43 m → 27.77 m over 3 h: **the
   carry was doing 44 % of the datum's job**.

`scripts/product_step_census.py` reports the split (PIVOT / EX-PIVOT lines), reading the
pivot identity off the product's own S-basis contract line.

- **G3 — 12 h A/B (the verdict).**  ON vs the G0 control, same build, same config chain,
  fresh convergence both arms, runs never overlapping.  Headline: dev-channel |step| median
  **6.23 cm → ≪, toward 2 cm**; robust σ_w 9.23 cm → √(q·Δt).  Plus `scripts/fc1_grade.py`
  **shadow hour table** — the FC-1 memory's standing instruction is that this table IS the
  score for Q3 work (producer Δc0 vs phase-measured per hour with the correlation; a
  successful anchor moves producer Δc0 from ~68–83 cm/30 s toward the ~1.8 cm physical AND
  lifts the correlation off zero); consumer live WTZZ closed loop + replay vs
  `runs/clkr_12h_hold` (hourly LEVEL bands, never slopes — the warm-resume/ignition trap);
  `total_IF` hourly bands (datum-invariant; never `rms_clock_m`); AR fix rate against the
  0/1440 baseline — **hoped-for, NOT promised**; counters tie out (trips = events = R records
  = consumer reactions).
- **G4 — the event-response validation (invariant #1's actual bar).**  A DOY-129 event-window
  replay of the 20:47 class must show the guard FIRING with marks at the decoupling edge, a
  `cta_recouple` at the re-coupling edge, and the epoch EMITTING (adapt-and-emit, not
  withhold).  **Quiet-day cleanliness alone does not pass this gate.**  Event expression
  depends on ACCUMULATED FILTER STATE, so the arm must be a seamless lineage into the event
  window, not a warm resume onto it (a 19:00-style seam measurably erases the event).

---

