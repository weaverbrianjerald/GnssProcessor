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

## §5 Results

Session binary `gnss_processor_cta.exe` md5 `F5068BA6707F04708CDB9299893D4F6A` (Release,
2026-08-10).  Unit suite **1331 passed / 1 skipped** (the fixture gate) — +14 cases
(`tests/test_clock_anchor.cpp` × 11: OFF-path bit equality, born-LOOSE, per-satellite elapsed
time, staleness fallback, the variance floor, epoch bookkeeping, the in-epoch step recovery,
the common-mode non-trip, the re-coupling run, and the two loader contracts;
`tests/test_network_estimator.cpp` × 2: the withdraw is the exact inverse of the fold, and the
dense re-fold equals a from-scratch marginalized fold, with its restore branch and no-op
cases).

### G0 — OFF-path byte-identity: **PASS, maximally**

`_golden63_20ep_clkr_ctl.json` re-run unchanged (`runs/cta_g0_ctl`) is **whole-file
byte-identical** to the stored pre-card `runs/clkr_20ep_ctl/products.txt` (md5
`C015A68F73393B3A1C0C02DEEC85E9C1`).  Re-verified after the R-record fix below.

### G1 — 20-epoch ON smoke (`runs/cta_20ep_{ctl,on}`): **PASS**

Run-end line, ON arm: *20 epoch(s); 589 clock carry fold(s) — **453 ANCHORED** at
q = 3.000e-05, **136 LOOSE**, 0 at the variance floor.  Trips 1 = 1 nis; **1 in-epoch
re-solve (0 failed)**, **32 re-coupling edges**, 1 wire mark.  Largest |w| 12.55 (k = 6.00) on
G07 at TOW 259440.*  Control console: zero Card-Q3 lines.

- **Not inert** (invariant #4): 453 anchored folds.
- **The anchor is EARNED**: 32 re-coupling edges = one per satellite, all at TOW 259320 — the
  5th epoch, i.e. born LOOSE + 4 in-band epochs, exactly the configured re-validation.
- **Both edges exercised in 20 epochs**: `cta_decouple` G07 (|w| 12.55, ê = −1.527 m,
  med = −0.107, √S = 0.113) → in-epoch withdraw + re-solve → `cta_disc_landed` G07 at the SAME
  epoch → **`R,G07` on the wire**; census reports `wire marks: R records=1` against the
  control's `NONE`.  G07 at TOW 259440 is the same satellite and epoch CLK-R's independent
  stage-2 statistic marked in its own 1 h arm.
- **Ordering check** (why this seam precedes within-epoch RAIM): post-fit σ̂₀ code
  4.2707 → 4.2698, prefit H-rider σ̂₀ 4.3269 → 4.3179, RAIM T3 0 opp / 0 flag both arms, 20/20
  emitted, 0 withheld.  No σ̂₀ inflation.

**Defect found and fixed during G1** — `markSatDiscontinuity` alone bumps only the emitter's
in-process counters; on a lineage without `serialize_disc_counters` the per-record disc fields
are not written, so the **R record is the only carrier a `NetworkProductIO` replay can read**,
and the first ON arm produced a live-visible, replay-INVISIBLE mark.  Fixed by folding the
landed set into `retro_emit_sats` (which also gives those satellites the "forced" treatment
CLK-R's frame-hold median needs — a just-refloated satellite must not vote).  Solve untouched;
dev-channel numbers bit-comparable across the fix.  **⚠ OPEN: the three sibling bump queues in
`networkSolPrep` (`eph_cutover`, `cb_excursion`, `basis_relatch`) have the SAME gap and were
left alone — they are marking a wire their own replay cannot see.**

### G2 — 1 h A/B (`runs/cta_1h_{ctl,on}`, one knob + the anchor block): **PASS**

Wire census (`scripts/product_step_census.py`, dev channel — the graded quantity), 121
epochs, 3820 rows, 32 satellites:

| dev channel | ctl (q = 1e-3) | ON (q = 3e-5) | Δ |
|---|---|---|---|
| \|step\| **median** | 6.00 cm | **3.60 cm** | **−40 %** |
| p90 | 18.34 | **9.74** | −47 % |
| p99 | 52.56 | **31.82** | −39 % |
| **robust σ_w** | 8.89 cm | **5.34 cm** | **−40 %** |
| per-sat σ_w median | 9.08 | **5.70** | −37 % |
| epochs with a per-sat outlier > 0.5 m | 27 | **15** | −44 % |
| PB mid-arc steps > 0.1 m | 62 / 29 ep | 66 / 31 ep | +6 % (noise) |

QC: post-fit σ̂₀ code 5.0213 → 5.0886 (+1.3 %), 121/121 emitted both arms, **0 withholds, 0
skipped epochs, 0 within-epoch RAIM re-solves**.  Counters, ON: 3701 folds — **3537 anchored /
164 loose**, 8 trips (all `nis`), 6 in-epoch re-solves, 0 failed, 39 re-coupling edges, 8
marks → 4 R records.

### §3 pass 1 — the binding knee, MEASURED

The `clk` prior-residual family (Card Q3 INC-0), same 121 epochs and 3700 rows:

| arm | armed q | **σ̂₀_clk** | implied self-consistent q = q·σ̂₀² |
|---|---|---|---|
| ctl | 1e-3 | **0.2617** | 6.8e-5 |
| ON | 3e-5 | **0.6977** | 1.5e-5 |

Both < 1 ⇒ **both carries are still looser than the data supports**, and the iteration
converges downward — the fixed point (σ̂₀ = 1) sits at **q ≈ 1.0–1.5e-5 m²/s**.
**q ≈ 1.3e-5 is independently where FC-1's carrier-phase measurement of the PHYSICAL clock
change puts it** (≈ 2 cm per 30 s ⇒ q = 0.02²/30 = 1.3e-5): two instruments sharing no
machinery on the same process noise, and 20× tighter than the §3 desk estimate of 2.8e-4 —
which is why the card requires the knob to be measured, not modelled.

Ladder (|t| = |s|/σ̂₀, the population k is read from): ctl >4σ̂₀ 8.19 %, >12 0.62 %, >20
0.24 %; ON >4σ̂₀ 14.90 %, >12 **0.27 %**, >20 **0.11 %** — the ON tail is ~2× thinner relative
to its own (tighter) scale while its body is fatter, the signature of a scale still slightly
under-estimated.  ⚠ The ladder and the guard's realized trip count do NOT yet map onto each
other (the >8σ̂₀ rung would predict tens of rows/h against 8 observed); the plausible reason is
that the guard *suppresses its own population* — a tripped satellite goes LOOSE and its
next-epoch residual is tested against the much wider loose null — but that is an inference,
and k needs the 12 h window to settle before being set from the ladder.  The census FRAME
label ties out exactly: **8 `cta_refloat` rows = the 8 guard trips**, held out of the σ̂₀
calibration (their null is undefined — the treatment a held phase bias gets).

### Pre-registered prediction that did NOT verify

§1 argued the per-satellite σ_w SPREAD (1.37× across 32 satellites) is the signature of
estimation noise, and G2 pre-registered it should SHRINK as the anchor binds.  Measured:
**3.70× → 8.35×, it WIDENED** — quiet satellites gained most (min σ_w 4.70 → 1.44 cm) while
the noisiest barely moved (17.38 → 12.05).  Consistent with an anchor that floors each
satellite at its own data-limited rate rather than at a common process floor; not predicted,
not yet explained, and it does not touch the headline (level and median both fell 40 %).
§4's withdrawn-prediction note supersedes the expectation; this is the measurement behind it.

### G3 — the 3-arm 3 h ladder (2026-08-10)

Binary `gnss_processor_ctagf.exe` md5 `974714FB7174A8C9CE6FB239C71AB9EF`; arms
`runs/cta3_{ctl,on,gf}`, configs `_cta3_{ctl,on,gf}.json`, one build, one chain, fresh
convergence each, never overlapping.  Suite 1335 pass / 1 skip.  **Provenance, free**:
`cta3_ctl` and `cta3_on` came out **byte-identical** to the stored pre-gauge-free
`runs/cta_3h_{ctl,on}` — one comparison proving the estimator refactor (`denseFoldTerms_` +
the gauge-free branch) is arithmetically neutral on the OFF and raw-state paths, that FC-1
`fast_clock.shadow` is genuinely measure-only, and that the earlier 3 h numbers transfer.

| 3 h, 63 sta, DOY-129 00:00–03:00 | ctl | anchor | anchor + gauge-free |
|---|---|---|---|
| **dev channel** median / robust σ_w | 5.98 / 8.87 cm | **3.55 / 5.26** | 3.81 / 5.65 |
| dev p99 / outlier epochs > 0.5 m | 73.81 / 78 | 50.92 / 48 | **33.57 / 43** |
| **ex-pivot** floor / median / ceiling | 5.66 / 8.54 / 13.81 | **2.36** / 5.28 / **8.55** | 3.25 / **5.24** / 8.63 |
| **PIVOT row** (= drained common mode, a GAUGE) | 18.03 | 11.27 | **21.53** |
| CLK-R Σ\|μ\| drained (independent counter) | 49.43 m | 27.77 m | **79.53 m** |
| post-fit σ̂₀ code | 4.0460 | 4.0323 | **4.0057** |
| withholds / skipped epochs | 0 | 0 | 0 |
| trips (nis / abs / gap) | — | 65 (63/0/2) | 3 (0/1/2) |
| in-epoch re-solves / marks | — | 26 / 65 | 1 / 3 |
| σ̂₀_clk (census) | 0.2803 | 0.7539 | 4.4806 |
| consumer med / p95 / worst | 6.40 / 13.08 / 13.82 | **5.49** / 11.77 / 12.90 | 6.64 / **9.13** / **9.46** |
| AR fixed | 0 | 0 | 0 |

**The §2.1 mechanism is PROVEN, with the corrected sign.**  Σ|μ| 27.77 → **79.53 m**, above the
control's 49.43 (the control's legacy carry still loads the gauge slightly; the gauge-free fold
loads it exactly zero), census pivot row in lock-step (11.27 → 21.53).  10 922 keys downdated,
**0 inert epochs**.

**The estimable side is a TRADE, not a win** — gauge-free buys the tail and pays in the body
across three instruments: dev p99 50.92 → **33.57** (−34 %) and outlier epochs 48 → 43, but dev
median 3.55 → 3.81 and ex-pivot floor 2.36 → 3.25; consumer worst 12.90 → **9.46** and p95
11.77 → **9.13** (both best of the three arms) but median 5.49 → 6.64 and hour 0 3.00 → 5.42 (a
cold start converges more slowly with strictly less prior information); σ̂₀ code monotone best
(4.0460 → 4.0323 → **4.0057**), the predicted §2.1 signature.

**Trips collapse 65 → 3 without a fatter tail** (the gauge-free p99 is the smallest of the
three, which a merely desensitized guard could not do).  The raw-state arm's 63 `nis` trips have
|ê| median **0.28 m** (max 1.42 m) against a mean √S of **3.3 cm** — sub-metre residuals made
significant by a denominator smaller than the arm's own realized dispersion (5.26 cm) — and
**0 of 63 would have been caught by the 2.0 m absolute backstop**, so the raw-state guard was
substantially firing on the walk's tail, not on metre-class clock events.

**⚠ CONFOUND: the gauge-free arm changed TWO things** — the fold AND the guard's denominator
(§2.3's matched common-removed posterior), which is correct for that fold but independently
desensitizing; this ladder cannot apportion the 65 → 3 collapse.  **Owed: a
`clock_anchor_gauge_free` arm with the statistic forced to the raw-state denominator.**
*(RESOLVED in §8.6, and it INVERTS this reading: the fold alone drives trips 65 → 109; the whole
collapse is the denominator.)*

**⚠ Instrument defect found: `σ̂₀_clk` is not gauge-aware** — 0.2803 → 0.7539 → **4.4806**.  The
census still standardizes by `last_var_eff`, the per-satellite RAW anchor variance, while the
gauge-free fold carries strictly less information than that (by exactly the gauge direction), so
`disp` under-states and σ̂₀ blows up.  The knee readings (§3) stand for the ctl/anchor arms and
**must not be read across the gauge-free arm** until the census's fold_var is gauge-aware.

### The FC-1 shadow hour table — the score, and it says NO

`scripts/q3_fc1_hours.py`, cross-validated against `scripts/fc1_grade.py shadow` (SD-RMS
agrees to 3 decimals: 21.723 / 27.114 cm):

| 3 h, common-removed | ctl | anchor | gauge-free |
|---|---|---|---|
| producer, robust σ (**the body**) | 8.83 cm | **5.25** | 5.60 |
| producer RMS, declared marks removed | 21.72 | 15.83 | **13.93** |
| producer RMS, raw | 21.72 | 27.11 | **19.85** |
| phase-measured (TDCP) | 0.30 | 0.30 | 0.30 |
| **corr(producer, phase)** | −0.004 | **−0.004** | **−0.005** |
| ratio phase / producer | 0.014 | 0.011 | 0.015 |

**Not one arm moves the correlation off zero**, so the pre-registered FC-1 gate (*"a
successful anchor moves producer Δc0 toward the phase column AND lifts the correlation off
zero"*) is **NOT met**.  Q3 at q = 3e-5 makes the emitted clock **quieter, not more real**:
the body falls 41 %, the marks-removed RMS 27–36 %, and the information fraction stays at
~1–1.5 %.  Two reusable notes:

1. **An RMS is the wrong statistic for a guarded arm.**  Raw SD-RMS gets WORSE with the
   anchor (21.72 → 27.11) purely because the guard's 65 marked refloat steps are large
   one-epoch changes BY DESIGN and RMS is tail-dominated — while every satellite in the
   grader's own worst-5 table improved (G22 16.1 → 13.9, G28 14.7 → 11.5, G29 13.6 → 9.6, G18
   12.8 → 9.4).  A declared discontinuity is not an error the consumer integrates; it is one
   it is told to reset at.  `q3_fc1_hours.py` reports the raw RMS, the robust body AND the
   marks-removed RMS — none of the three is quotable alone.
2. **~~The physical floor on this lineage is an OPEN NUMBER~~ — RESOLVED in §7.**  The TDCP
   column measures 0.30 cm per 30 s here, 6× smaller than the 1.7–2.0 cm
   `project_fast_clock_card` records; §7 settles it — **both are MEASUREMENT FLOORS**, and the
   accumulating physics is q_phys ≈ 2.9e-8 m²/s (0.94 mm/30 s).

### Not yet run

- **G4 — the DOY-129 20:47 event-window replay**, still OWED.  The guard has fired on real
  data (65 trips incl. 2 genuine staleness `gap` trips at TOW 262950 / 267750, on satellites
  whose carry had aged past 120 s) but never on a KNOWN event with a known answer.
  **Quiet-day cleanliness is explicitly not a pass.**
- The 12 h leg, and the comparison against the `runs/clkr_12h_hold` denominator: a 3 h window
  cannot settle the LEVEL channel (~2 h).  `total_IF` is likewise not graded here.
- The gauge-free bisection and the gauge-aware `σ̂₀_clk`, both named above.

---

## §6 Relationship to the surrounding cards

- **CLK-R** (`emit_clock_frame_hold`, promoted) holds the clock's *gauge* at emission; Q3 is
  the *content* half.  CLK-R makes the per-sat channel readable, Q3 makes it quiet.  CLK-R
  stage 2's per-sat marks stay OFF; Q3's marks are a different population (guard trips, not
  census steps).
- **C1 / FC-1**: both measured NET-HARMFUL because they predict a producer-internal walk.  Q3
  is the named prerequisite of both; FC-1's shadow instrument is Q3's scoreboard (G3).
- **`network.clock_model`**: stays NO-GO.  Q3 deliberately does NOT build on the two-state
  carry — its data-incest defect (posterior fed back as a measurement with fixed R) is
  unfixed, its RWFM premise falsified (p ≈ 1), and `service_mode` force-arms it while silently
  making `sat_clock_q` a no-op.  Q3 uses the SCALAR carry and only borrows the two-state
  path's B3-corrected innovation VARIANCE, a general identity.
- **Odijk et al. 2016**: Q3 implements the paper's assumed random-walk clock constraint at a
  defensible q — the one row of the realization contract no S-transformation can fix, because
  it is a property of the estimable content, not of the basis.

## §7 (2026-08-10, Fable) THE PHYSICAL FLOOR MEASURED — trap 5 resolved, the FC-1 gate RETIRED, the ladder re-scoped

`scripts/q3_clock_structure_function.py` on `runs/cta3_ctl` (32 sats, 3 h, common-mode
removed).  The per-tick TDCP telescopes — accumulated change over lag L is the endpoint phase
difference, interior noise cancels — so a structure function separates floor from walk:

| lag [s] | SF robust [cm] | SF/√L |
|---|---|---|
| 30 / 60 / 120 | 0.269 / 0.262 / 0.268 | falling — **floor, flat** |
| 300 → 2400 | 0.318 → 0.846 | **0.0163–0.0173, constant — walk** |

**q_phys ≈ 2.9e-8 m²/s → 0.94 mm per 30 s (fractional stability ~1e-13).**  The clock
corrections beyond the COD a-priori are quasi-static at the mm-per-epoch level — the user's
clock-linearity expectation, confirmed quantitatively.  Consequences, each load-bearing:

1. **Trap 5 is RESOLVED, and both prior "physical change" numbers were the FLOOR, not the
   walk.**  FC-1's 1.7–2.0 cm hour table (its horizon column was already flat 30 s → 1200 s =
   non-accumulating) and this lineage's 0.30 cm TDCP column both measured SF(30 s)-class
   floors; the accumulating physics is 20× below even the smaller of them.
2. **The FC-1 correlation gate is RETIRED, not merely unmet.**  0.9 mm true per-step motion
   against a 2.7 mm measurement floor ⇒ structurally unliftable at ANY reachable q rung.  At
   the floor, quiet IS real.  Replacement gate set: dev census ex-pivot floor/ceiling,
   σ̂₀_code flat, consumer live+replay unchanged-or-better, guard-population health (§3), G4's
   event response.  "QUIETER not MORE REAL" stands as a description of q = 3e-5, no longer a
   criticism.
3. **The §3 knee (σ̂₀_clk → 1 at q ≈ 1.0–1.5e-5) is the DATA-INFORMATION knee, not physics** —
   where the carry meets the per-epoch measurement information, ~3 orders above q_phys.  Below
   it σ̂₀ cannot validate; the descent 3e-6 → 3e-7 → 1e-7 is graded on the replacement set.
4. **The variance floor changes role from "armed but inactive" to GOVERNING.**  At q ≤ 3e-7,
   q·Δt ≤ (3 mm)² sits below any honest correlated-error floor, so `clock_anchor_var_floor_m2`
   must be SET FROM THIS MEASUREMENT (the (2.7 mm)² floor + margin ⇒ ~1e-5 m² class) and is
   expected to BIND — deviation #1's consistency cap becoming load-bearing is the design, not
   a warning condition.  Invariant-#1 pressure is maximal at the bottom rung (a near-zero-q
   carry): G4 gates any arming.

**Design principle (user, 2026-08-11), standing for this ladder and beyond: aim to be
PHYSICALLY CORRECT — set stochastic models to measured physics and let model errors express
elsewhere as visible residuals/events, rather than absorbing them into inflated q.**  An
inflated q is how the 9 cm walk stayed invisible for a year: the error hid in the loosest
state instead of showing up as a residual anyone would grade.

## §8 (2026-08-10/11) THE FLOOR-GOVERNED LADDER — floors derived, the guard's denominator bisected, the descent 3e-5 → 1e-7

Build `gnss_processor_cta4.exe` md5 `94EF4327BABE0E0C9140696AA446D384`; suite 1335 pass / 1
fixture skip; configs `_cta4_*.json`; arms `runs/cta4_*`; one build, one chain
(`_golden63_gps_1h_laneoff` direct), fresh convergence each, serialized by
`scripts/q3_ladder_run.ps1` (it refuses to start while any producer is alive).  Graded by
`scripts/q3_ladder_grade.py`.

### §8.1 The floors, DERIVED — not tuned

§7 left the cap a class ("~1e-5 m²").  The derivation, from `runs/cta3_ctl` re-measured on
this session's tree:

```
SF(30 s)            = 2.690 mm      robust σ of the 30 s CHANGE (floor ⊕ walk)
walk(30 s)          = √(q_phys·30)  = 0.938 mm          (q_phys = 2.93e-8 m²/s)
change floor        = √(2.690² − 0.938²) = 2.521 mm     ⇒ 6.36e-6 m²
per-sample floor    = 2.521/√2       = 1.783 mm         (flat SF ⇒ white per sample,
                                                         so the change floor is √2 σ)
```

- **`clock_anchor_var_floor_m2` = 1.0e-5 m²** = (1.25 × 2.521 mm)² = 9.93e-6, rounded.  The
  margin is in σ and is stated rather than fitted: 2.521 mm is a robust point estimate from
  one arm, and the producer's own absolute-clock error floor is *at least* the TDCP
  instrument's differential floor (TDCP cancels the receiver clock and the a-priori; the
  emitted clock does not).  The cap must not sit BELOW the measurement, because under-capping
  is the failure mode it exists to prevent.  Landing on §7's independently-predicted "~1e-5 m²
  class" is a check, not a coincidence.
- **`clock_anchor_sigma_floor_m` = 0.0018 m** = the per-sample floor.  **Not a free knob:
  leaving it at the shipped 0.02 m would have quietly replaced the guard's statistic at the
  bottom rungs.**  `√S` is floored at this value and `S = P⁻ − P⁺ ≤ P⁻`, which the variance
  floor caps at 1.0e-5 m², so at the bottom rungs the ENTIRE achievable denominator is
  ≤ 3.16 mm.  A 20 mm floor sits 6× above that: `max()` would select it on every row, `w`
  would degenerate to `ê / 0.02` — a fixed 2 cm bar wearing a predicted residual's name — and
  every counter would still read healthy.  The two floors are one physical quantity read in
  two places.

| rung q [m²/s] | √(q·Δt) at 30 s | vs the 3.16 mm floor σ |
|---|---|---|
| 3e-5 | 30.0 mm | inert |
| 3e-6 | 9.49 mm | inert |
| 3e-7 | 3.00 mm | **crossover** |
| 1e-7 | 1.73 mm | **the floor GOVERNS** |

### §8.2 The owed bisection — `network.clock_anchor_gauge_free_stat`

`auto` \| `raw` \| `post` \| `proj` selects the guard's (and the census's) DENOMINATOR
independently of which prior the FOLD applied — what §5's confound needed.  The withdraw path
deliberately keeps reading `clock_anchor_gauge_free`: what a refloat un-folds has to be what
was folded.  The numerator is the COMMON-REMOVED residual ê, so both halves of `S = P⁻ − P⁺`
must be variances of **that contrast**:

```
Var(xₛ − mean_t x_t) = C_ss − 2·mean_t C_st + mean_{t,u} C_tu     (clockAnchorGaugeProject)
```

applied to the posterior block for P⁺ and to `diag(var_eff)` for P⁻.  Correct for **both**
folds: under the gauge-free fold the prior is `diag(var_eff) + ∞·11ᵀ` and the contrast is
orthogonal to `1`, so the infinite part drops out exactly; under the raw fold the prior IS
`diag(var_eff)`.  The odd one out is the RAW marginal — the shipped default.  It over-states
P⁺ (the raw marginal still carries the common-mode variance the median just removed) and
therefore UNDER-states S: **the raw statistic over-trips by construction, on the raw fold too,
not only on the gauge-free one** — a correctness claim derived before the arms were run, with
`cta4_bis_raw_proj` as its test.  `auto` reproduces the shipped pairing byte-for-byte (§8.4).

**Residual approximation, stated rather than hidden.**  The numerator removes the
per-constellation MEDIAN (robust, invariant #5) while the projection is the MEAN contrast; for
n satellites they differ only in the O(1/n) term — a median's sampling variance is ≈ π/2 times
a mean's, so the correction sits on ~3 % of the denominator at n = 32, worth ~1.5 % in σ.
Left unmodelled deliberately: modelling it would trade an exact, closed-form, fold-matched
identity for an asymptotic correction an order of magnitude below the effect the projection
itself removes.

### §8.3 The gauge-aware σ̂₀_clk, and the guard's own ladder

- **Census** (`NetworkStashStatus`): the `clk` family now standardizes by the projected pair
  through the SAME helper the guard uses, so census and guard are the same statistic in every
  arm and the census stays a cross-check rather than a second implementation that can drift.
  §5's instrument defect (σ̂₀_clk 0.28 / 0.75 / **4.48**) is closed by construction.
- **Run-end guard ladder** (`NetworkTeardown`): mean √S, the |w| quantile ladder and an
  ABSOLUTE |ê| ladder in metres over anchored rows on the first guard pass.  Its job is §3's
  per-rung k re-derivation: the denominator shrinks as q tightens, so a FIXED k is a different
  bar in METRES at every rung, and the line prints `k · mean √S` so that bar is legible.  The
  |ê| ladder is what the 2.0 m backstop's rail is read off — the number missing when the 3 h
  arm found 0 of 63 nis trips would have been caught by it.  **Caveat printed with the
  ladder**: the population is truncated ACROSS epochs (a tripped satellite goes LOOSE and stops
  contributing), so the upper bins under-report; the un-truncated null needs an arm with
  `clock_anchor_k` beyond the top rung and `clock_anchor_abs_m: 0`.
- **Floor telemetry**: the run-end line reports the floor BINDING fraction and says in place
  that binding is the DESIGN at these rungs (§7.4), not a warning condition.

### §8.4 Pre-registered gates for this leg (written before any 3 h arm was read)

The §7 replacement set, made per-rung and per-arm.  The FC-1 correlation gate stays RETIRED
(§7.2) and is reported for continuity only.

**Provenance gates** decide whether anything below is interpretable.  **G0a OFF
byte-identity**: the 20-epoch control re-run on this build must match stored
`runs/cta_20ep_ctl`.  **G0b ARMED byte-identity**: the 20-epoch RAW and GAUGE-FREE arms must
ALSO match theirs — the gauge-free one proves `auto` reproduces the shipped pairing, which an
OFF-path check cannot certify because an OFF run never enters the anchor.  **G0c the TIE arm**:
`cta4_tie`, knob-for-knob `_cta3_on` at full 3 h, byte-identical to `runs/cta3_on` — every
later comparison chains through it.

**Per-rung gates**, chained so each link differs by ONE knob (or one physically-coupled PAIR):

```
cta3_on (raw,raw,shipped floors) --stat--> bis_raw_proj --floors--> q3e5 --q--> q3e6 --q--> q3e7 --q--> q1e7
cta3_gf (gf,post)                --stat--> bis_gf_raw / bis_gf_proj
```

1. **ex-pivot dev census floor / ceiling** (`product_step_census.py`, PIVOT row excluded —
   §4.1, a GAUGE): floor approaches √(q·Δt) per rung until the variance floor takes over;
   ceiling must not grow.
2. **σ̂₀_code flat** — a rung that raises it pushes carry tension into observation residuals: a
   stop signal.
3. **Consumer unchanged-or-better** on BOTH instruments (live closed-loop hourly LEVEL bands
   and the replay), hourly levels never a slope.
4. **PB / CB channels untouched** — this card owns the clock row only.
5. **Guard population healthy, never a count** (§3): trips resolve in-epoch, marks tie out
   event ↔ R record ↔ counter, consumer unchanged-or-better over the containing window.
6. **k re-derived per rung** from the run-end |w| ladder, in METRES (`k · mean √S`); the 2.0 m
   backstop's realized rate off the |ê| ladder.  k is REPORTED, not re-armed.
7. **Floor binding is evidence, not a fault** at rungs 2–3 (§7.4); watch the census `clk`
   family and the VCE-class monitors instead.

**Stop rule.**  A rung that degrades the consumer or σ̂₀_code is the BOUNDARY and no rung below
it may be called validated; a physically-correct q that measures worse is a FINDING about
unmodelled error becoming visible, not something to tune away by re-inflating q.  **Deliberate
deviation, declared here**: rungs below the boundary are still RUN (7 min each), because
whether the degradation is a cliff or a ramp is itself the diagnostic.  **NO promotion** — G4
(the DOY-129 20:47 event window) remains the invariant-#1 blocker and is a separate card;
loader defaults stay OFF.

### §8.5 Provenance — all four identity gates PASS

| gate | arm | vs stored | result |
|---|---|---|---|
| G0a OFF, 20 ep | `cta4_20ep_ctl` | `cta_20ep_ctl` | **byte-identical** |
| G0b RAW armed, 20 ep | `cta4_20ep_on` | `cta_20ep_on` | **byte-identical** |
| G0b GAUGE-FREE armed, 20 ep | `cta4_20ep_gf` | `cta_20ep_gf` | **byte-identical** |
| G0c RAW armed, **3 h** | `cta4_tie` | `cta3_on` | **byte-identical** (`72FCF8E5E34668EA76106FA5C79FAB5E`) |

Every number below is therefore same-build, same-chain, and the stored `cta3_{ctl,on,gf}` arms
remain valid members of this leg's table without re-running.

**⚠ The 20-epoch smokes gave the WRONG SIGN, and the settled arms reverse them — "never
verdict in ignition" applies to the guard's own null too.**

| raw fold, denominator isolated | mean √S | guard σ̂₀ = √(Σw²/n) | \|w\| ≥ 1 |
|---|---|---|---|
| 20 ep `raw` | 0.0914 m | 0.926 | 10.2 % |
| 20 ep `proj` | 0.2259 m | 0.475 | 2.6 % |
| **3 h `raw`** (`cta4_tie`) | **0.0354 m** | **1.754** | **49.8 %** |
| **3 h `proj`** (`cta4_bis_raw_proj`) | **0.0819 m** | **1.091** | **20.7 %** |

At 20 epochs the carried posterior variance is still the cold-start one and dominates `P⁻` on
both sides, so the σ̂₀ ordering is a convergence artefact.  The settled pair says three things:
**the raw statistic's `mean √S = 3.54 cm` independently reproduces §5's hand-measured 3.3 cm**
(which is what makes the rest of the column trustworthy); **the projection widens the
denominator 2.31× in σ with the numerator standing still** — the |ê| ladders agree within
sampling (≥ 5 mm: 89.0 % vs 89.5 %; ≥ 5 cm: 31.8 % vs 36.2 %), so the whole effect is the
denominator; and **the corrected statistic is CALIBRATED and the shipped one is not** — under
a correctly specified null σ̂₀ → 1, raw reads **1.754** (a denominator too small by ~√3) and
proj reads **1.091**, confirming from data the §8.2 algebra derived before any arm ran.

Quote BOTH σ̂₀s and never one alone: the guard's is an RMS over every anchored row and is
tail-dominated, the census's is the robust calibrated estimator (`n_eff = 2572 of 10879` on
this arm); on a heavy-tailed population they differ by ~2.4×.

### §8.6 The bisection — RESOLVED, and it inverts §5's reading

The 2×2 over (fold, denominator), all q = 3e-5, all shipped floors, one build, one chain.
`cta3_on` / `cta3_gf` are the stored arms; the other three are new.

| trips (nis) | stat `raw` | stat `post` | stat `proj` |
|---|---|---|---|
| **fold raw** | 65 (63) `cta3_on` | — | **12 (10)** `cta4_bis_raw_proj` |
| **fold gauge-free** | **109 (107)** `cta4_bis_gf_raw` | 3 (0) `cta3_gf` | 3 (0) `cta4_bis_gf_proj` |

**The gauge-free FOLD, on its own, makes the guard trip MORE — 65 → 109.**  The whole 65 → 3
collapse, and then some, is the DENOMINATOR: §5 recorded the confound honestly and guessed the
tail evidence argued against blinding, and the apportionment says the fold contributes in the
opposite direction to the guess.

| | fold effect (stat held) | stat effect (fold held) |
|---|---|---|
| Σ\|μ\| drained (the GAUGE) | 26.7 → **81.5** m | 26.7 → 26.7 / 79.5 → 79.5 (**nil**) |
| trips | 65 → 109 (raw stat) | 65 → 12 (raw fold) |
| σ̂₀_code | 4.0519 → 4.0379 | 4.0323 → 4.0519 |
| guard σ̂₀ | 1.754 → 2.096 (raw stat) | 1.754 → **1.091** (raw fold) |

The gauge channel is **purely** the fold (81.5 vs 79.5 across both denominators — the
statistic cannot move it, by construction, and does not); the trip population is
**overwhelmingly** the denominator.  `proj` and `post` produce a **byte-identical product**
under the gauge-free fold — the expected degeneracy, since that fold carries no gauge
information and projecting the prior is only the O(1/n) diagonal correction; they are distinct
arms only on the raw fold.

**⚠ NEW DEFECT — the gauge-free fold LEAKS ESTIMABLE STIFFNESS, and the corrected statistic is
what exposed it.**  Mean √S on the contrast, same q and same data:

| | mean √S | k = 6 as a bar | guard σ̂₀ | clk variance factor |
|---|---|---|---|---|
| raw fold + proj | **0.0819 m** | 0.491 m | 1.091 | **1.21** |
| gauge-free fold + proj | **0.4298 m** | 2.579 m | 0.178 | **0.057** |

A 28× weaker prior VARIANCE on exactly the between-satellite differences the fold exists to
constrain.  The mechanism is mechanical, not statistical: the D2 carry re-reads the **raw
marginal posterior** as `prev_var`, and under a gauge-free prior that marginal is
gauge-inflated (no prior information along the gauge ⇒ the posterior's gauge component is
data-only and large), which inflates the next epoch's `var_eff`, and compounds.  So `cta3_gf`'s
3 trips were never a quiet guard: the prior was too loose to conflict with anything, and `clk`
vf = 0.057 says so directly (a carry ~17× looser than its own model claims).  **Re-read §5's
verdict as "gauge-free is a much WEAKER anchor that happens to be gauge-clean"**, not
"gauge-free buys the tail and pays in the body".  The fix — stash the PROJECTED posterior for
clocks under that fold — is named here and deliberately NOT built in this leg; until it
exists, the gauge-free fold is not a like-for-like arm at any q.

**Wire-mark tie-out (gate 5b): 100 % on every arm** — event ↔ counter ↔ R record agree exactly
(65/65, 12/12, 109/109, 3/3) via `scripts/q3_mark_tieout.py`.  (That script now looks in
`<run>/logs` as well as `<run>`: the arm runner harvests traces there, and an events file read
from the wrong place reports a silent guard — the same false negative the card's trap #1 is
about.)

### §8.7 Rung 0 — the derived floors are a MEASURED no-op at the top rung

`cta4_q3e5` (q = 3e-5, var floor 1.0e-5, σ floor 0.0018) came out **byte-identical to
`cta4_bis_raw_proj`** (same q, shipped floors 4.0e-4 / 0.02).  Expected — at this rung
q·Δt = 9.0e-4 ≫ 1.0e-5 and mean √S = 8.2 cm ≫ 1.8 mm, so neither floor is ever selected — but
proving it costs one arm and buys the chain's key property: **the descent's rungs differ from
rung 0, and from each other, in `q` ALONE.**

### §8.8 The descent — every PRODUCER gate improves monotonically to 1e-7; the boundary is in the CONSUMER

Raw fold, `proj` denominator, derived floors, k = 6 held; rungs differ in `q` ALONE (§8.7).
`cta3_ctl` is the anchor-OFF control on the same chain.

| 3 h, 63 sta | ctl | 3e-5 | 3e-6 | 3e-7 | 1e-7 |
|---|---|---|---|---|---|
| **dev median / p99** (cm) | 5.98 / 73.81 | 3.77 / 26.79 | 2.51 / 13.54 | 1.65 / 12.02 | **1.41 / 12.00** |
| **ex-pivot floor / med / ceiling** (cm) | 5.66 / 8.54 / 13.81 | 3.18 / 5.34 / 8.57 | 1.81 / 3.62 / 8.16 | 0.77 / 2.28 / 5.68 | **0.56 / 1.93 / 4.45** |
| PIVOT row (GAUGE, not a grade) | 18.03 | 9.86 | 5.97 | 5.28 | 5.29 |
| Σ\|μ\| drained (GAUGE) | 49.43 m | 26.74 | 19.25 | 17.80 | 17.70 |
| mean √S / k=6 as a bar (m) | — | .0819 / .491 | .0666 / .399 | .0640 / .384 | .0638 / .383 |
| guard σ̂₀ (RMS) | — | 1.091 | **1.009** | 0.663 | 0.518 |
| census σ̂₀_clk (robust) | 0.2803 | 0.449 | 0.439 | 0.359 | 0.304 |
| **clk variance factor** | 1.66 | 1.21 | **0.99** | 0.62 | 0.51 |
| **cb variance factor** | 9.98 | 10.80 | 11.73 | 12.66 | **13.04** |
| rxcb variance factor | 4.04 | 4.03 | 4.12 | 4.20 | 4.25 |
| **post-fit σ̂₀ code** | 4.046 | 4.052 | 4.110 | 4.126 | **4.212** |
| trips (nis/abs/gap) | — | 12 (10/0/2) | 3 (1/0/2) | 3 (1/0/2) | 3 (1/0/2) |
| variance floor bound | — | 0 | 0 | 0 | **0** |
| consumer settled med / p95 / worst (m) | 8.01 / 13.21 / 13.82 | 7.84 / 11.66 / 12.77 | 6.24 / 11.87 / 12.57 | **6.12 / 9.65 / 10.72** | 4.17 / 7.71 / **34.91** |

**1. The producer side has NO boundary down to 1e-7.**  Every wire statistic improves
monotonically: dev median 5.98 → **1.41 cm** (4.2×), ex-pivot ceiling 13.81 → **4.45**, floor
5.66 → **0.56**.  The gauge channel drains as it should and is not a grade.

**2. The tension went where it can be SEEN, fully attributed** to two channels, both monotone
in q: the **satellite code-bias carry**, vf 9.98 → **13.04** (+31 %), and the **code
residuals**, σ̂₀ 4.046 → **4.212** (+4.1 %).  Neither moved at 3e-5 (cb flat to 2 %); from
3e-6 down the prior binds — `clk` vf = **0.99 at 3e-6**, dead calibrated — and each rung buys
wire quality with code-bias tension.  **The design principle's intended outcome, measured.**

**3. ⚠ The variance floor NEVER BINDS — §7.4's "GOVERNING at the bottom rungs" is structurally
unreachable as built.**  At 3e-7, √(q·Δt) = 3.00 mm sits BELOW the floor's 3.16 mm and the
counter still reads 0:

```
var_eff = prev_var + q·Δt      prev_var ≈ 4.1e-3 m² = (6.4 cm)²
                               floor    = 1.0e-5 m²   ⇒  410× below
```

`prev_var` is the **raw marginal** posterior, gauge-dominated and unable to approach a 3 mm
floor.  **The cap is applied to the wrong quantity** — to be deviation #1's consistency cap it
must bound the CONTRAST variance, the projection §8.2 put in the guard's denominator.  So these
rungs are governed by the gauge-dominated raw carry, and nothing in this leg measures a
floor-governed posture: second structural defect of the leg, same root as the first — **a
per-satellite clock quantity read on the raw state instead of the estimable contrast.**  Named,
not built.  Corroborating: the ex-pivot floor tracks √(q·Δt) at ratio 1.06 → 1.91 → 2.57 →
3.24, and at the bottom rung is still 0.56 cm against a 0.25 cm measurement floor and a
0.094 cm physical walk.

**4. ★ The boundary is REAL, is at rung 3, and is NOT in the producer — a better clock ARMED a
fail-unsafe consumer.**  Worst 3D error 10.72 → **34.91 m** with 4 epochs > 20 m while the BODY
improves sharply (settled median 6.12 → **4.17 m**, p95 9.65 → **7.71**):

| arm | min consumer AR position-σ | epochs ≤ 0.050 m | epochs with a FIX |
|---|---|---|---|
| ctl | 0.0750 m | 0 | 0 |
| 3e-5 | 0.0710 | 0 | 0 |
| 3e-6 | 0.0550 | 0 | 0 |
| 3e-7 | 0.0580 | 0 | 0 |
| **1e-7** | **0.0500** | **3** | **1** |

Consumer position σ falls monotonically as the producer's clock tightens, and **rung 3 is the
first arm ever to reach its AR arming threshold**.  Trace at TOW 265800: `ar_success=1,
num_fixed=5, ratio = inf, gate = fixed`, error 4.64 → **34.91 m**, vertical to −33 m, filter σ
blown to 3.138 m, six-epoch decay back to 4.68 m.  **`ratio = inf` is a degenerate ratio test,
not a confident fix.**  No clock-anchor trip within 40 minutes either side (trips at 262950,
267750, 269310).  Chain: **better clock → smaller consumer position σ → crosses the AR arming
threshold → fail-unsafe ratio test → 5 wrong integers → 35 m** — reproducing
`project_pppar_user_control`'s "FIXES ARE WRONG; fail-unsafe" and
`project_closedloop_consumer_vertical_bug` from an independent direction, and exactly what
`project_ar_validation_criterion` predicts an ADOP/ratio gate does where an FFR gate belongs.

**Verdict under the pre-registered stop rule.**  Rung 3 degrades the consumer, so **nothing
below 3e-7 may be called validated** — but the regression is a NAMED, PRE-EXISTING, DOWNSTREAM
defect that a better product UNMASKED, not the anchor mis-modelling the clock; all producer
gates improve at rung 3.

> **⚠⚠ DO NOT STOP READING HERE — this verdict was DISSOLVED by §8.11.5.**  The rung-3 consumer
> excursion was a **fail-unsafe consumer AR gate** (`ratio = +inf` is a WL-only SENTINEL meaning
> *no LAMBDA ratio test ran*, NOT a degenerate ratio test — §8.11.1).  With the innovation
> screen `wl_only_max_innov_cyc` armed, **rung 3 is BETTER than rung 2 on every consumer
> statistic** (4.84 / 7.65 / 7.75 vs 6.12 / 9.78 / 10.72).  What does NOT change: the
> producer-side cost (products byte-identical, σ̂₀ code 4.126 → 4.212, cb variance factor
> 12.66 → 13.04) and the standing **G4 invariant-#1 blocker**.

**Rung 2 (q = 3e-7) is the last rung unchanged-or-better on every pre-registered gate**, and
the best arm measured on this lineage for both the wire (dev median 1.65 cm vs the control's
5.98) and the consumer (6.12 / 9.65 / 10.72 vs 8.01 / 13.21 / 13.82 — 24 % / 27 % / 22 %
better).  It still costs +2.0 % σ̂₀_code and +27 % cb tension.  **A MEASUREMENT, not a
promotion**: G4 remains the invariant-#1 blocker and the loader defaults stay OFF.

### §8.9 k re-derived per rung, on the UN-TRUNCATED null — and the 2 m rail is INERT

`cta4_q3e5_kcal` / `cta4_q1e7_kcal`: the graded rungs with `clock_anchor_k: 1e9` and
`clock_anchor_abs_m: 0`, so no satellite is decoupled by the statistic and the |w| population
is the un-truncated null §3 asks for.  (Staleness `gap` trips still fire — not statistical, and
disarming them would hide a real defect: 2 in each arm.)  **Truncation is real but modest**:
armed vs disarmed at rung 0, |w| ≥ 4 reads 0.605 % vs **1.02 %**, ≥ 6 reads 0.0745 % vs
**0.139 %** — the armed ladder under-reports its own upper tail by 1.7–4×.  At rung 3 the two
agree to within one row (only 3 trips to truncate).

| un-truncated \|w\| exceedance | ≥1 | ≥2 | ≥3 | ≥4 | ≥6 | ≥8 |
|---|---|---|---|---|---|---|
| rung 0 (q = 3e-5) | 21.7 % | 6.11 % | 2.43 % | 1.02 % | 0.139 % | 0.037 % |
| rung 3 (q = 1e-7) | 6.88 % | 0.362 % | 0.046 % | 0.028 % | 0.0185 % | 0.0185 % |

**Both halves of the leg's own expectation ("the denominator shrinks with q, so a fixed k
tightens in metres") come out wrong, in opposite directions.**  **The denominator barely
moves** — q spans **300×** (√(q·Δt) spans 17.3×) but mean √S spans **1.28×**, 81.9 → 63.8 mm,
because S tracks the gauge-dominated `prev_var`, not q (§8.8 defect 3), so a fixed k = 6 is
0.491 → 0.383 m across the whole ladder (28 %, not 17×).  And **the RESIDUALS tighten far
faster than the denominator**, so a fixed k gets progressively MORE conservative: exceedance
falls 0.139 % → 0.0185 %, a 7.5× rarer trigger, and guard σ̂₀ falls 1.256 → 0.536.

**Rate-matched k, if |w| is to mean the same thing at every rung** (§3's calibration purpose,
never a target rate), log-interpolating each rung's un-truncated ladder:

| target exceedance | rung 0 (q = 3e-5) | rung 3 (q = 1e-7) |
|---|---|---|
| 0.5 % | k = 4.72 → 0.384 m | k = 1.89 → 0.121 m |
| 0.1 % | k = **6.50** → 0.530 m | k = **2.63** → **0.168 m** |

A **3.2× tightening in metres**, not the 17× the process model suggests; the shipped k = 6
sits almost exactly on the 0.1 % rate at rung 0 while being ~2.5× too conservative by rung 3.
**Not re-armed in this leg** — changing k mid-ladder would make the rungs more than one knob
apart, the discipline that made §8.8 readable.

**⚠ The 2.0 m absolute backstop is INERT at every rung under the corrected statistic, and gets
more so as q tightens.**  Un-truncated |ê| exceedance:

| | ≥ 0.3 m | ≥ 1 m | ≥ 2 m |
|---|---|---|---|
| rung 0 | 0.565 % | 0.0278 % | 0.0185 % (2 rows / 3 h) |
| rung 3 | 0.0556 % | 0.00927 % | **0 % (empty)** |

The `nis` test at k = 6 is a 0.38–0.49 m bar, 4–5× MORE sensitive than the backstop
everywhere; for the backstop to be a rail rather than a formality it would have to sit at
**~0.3–0.5 m**.  Left at 2.0 m deliberately (one knob per rung), and recorded as the third face
of the same root cause: **√S is computed from a raw-state prior variance, so it does not shrink
with q, so nothing downstream of it scales the way the process model says it should.**

### §8.10 The replay instrument — the boundary is a CLIFF, and both consumers are sitting on it

`scripts/q3_make_replay.py` + `q3_ladder_queue2.ps1`: every graded arm's emitted stream
replayed through the held-out consumer (12 s per arm — the producer never runs).  The SECOND
consumer instrument the gate set asks for, and it matters because it reads the wire's own
discontinuity marks rather than the in-process counters.  Settled (hour ≥ 1) 3D error, median /
p95 / worst, metres:

| arm | LIVE | > 20 m | fix | REPLAY | > 20 m | fix |
|---|---|---|---|---|---|---|
| ctl | 8.01 / 13.39 / 13.82 | 0 | 0 | 8.24 / 14.87 / 17.07 | 0 | 0 |
| 3e-5 | 7.84 / 11.68 / 12.77 | 0 | 0 | 6.86 / 8.67 / 8.79 | 0 | 0 |
| 3e-6 | 6.24 / 12.03 / 12.57 | 0 | 0 | **6.33 / 7.70 / 7.84** | 0 | 0 |
| 3e-7 | 6.12 / 9.78 / 10.72 | 0 | 0 | 7.92 / 10.43 / 11.29 | 0 | 0 |
| **1e-7** | 4.17 / 7.72 / **34.91** | **4** | **1** | **5.52 / 7.30 / 7.50** | **0** | **0** |
| gf (post ≡ proj) | 7.44 / 9.15 / 9.46 | 0 | 0 | 5.46 / 6.99 / 7.70 | 0 | 0 |
| gf + raw | 5.61 / 7.16 / 8.69 | 0 | 0 | 4.90 / 6.35 / 7.60 | 0 | 0 |

**The rung-3 excursion does NOT appear in the replay** — a 14 % margin, not a different
mechanism:

| | min consumer AR position-σ | epochs ≤ 0.050 m | fixes |
|---|---|---|---|
| ctl replay | 0.0750 m | 0 | 0 |
| 3e-7 replay | 0.0590 | 0 | 0 |
| 1e-7 **replay** | **0.0570** | 0 | 0 |
| 1e-7 **live** | **0.0500** | 3 | 1 |

Both instruments show the SAME monotone fall in the consumer's own σ and agree on the cause;
they differ only in which side of the **0.050 m arming threshold** they land on — the live path
hits it exactly.  **So the boundary is a CLIFF the ladder walked the consumer up to, not an
artefact of one harness**: at rung 3 a 14 % difference in the consumer's covariance decides
between a 7.5 m and a 34.9 m worst case, and any further clock improvement pushes BOTH
instruments over.

**Verdict, final for this leg.**  Rung 2 (q = 3e-7) remains the last rung unchanged-or-better
on the pre-registered set.  Below it the producer keeps improving and the CONSUMER's
fail-unsafe AR gate is the binding constraint — `ratio = inf` accepted as a fix is the defect,
and it must be fixed (FFR-style gate, per `project_ar_validation_criterion`) before any q below
3e-7 is used end-to-end.

**⚠ Do not over-rank the rungs on this statistic.**  The replay ladder is NOT monotone (3e-7
replay is worse than 3e-6 replay, 7.92 vs 6.33 median) and live/replay orderings disagree for
several arms.  One station over two settled hours does not resolve ~1.5 m differences in a 3D
median; this consumer gate has the power to catch a 35 m cliff and confirm a direction, not to
rank neighbouring rungs.  The wire census (11 280 rows, 32 satellites) is the statistic with
resolving power, and it IS monotone.

## §8.11 The consumer AR blunder — DIAGNOSED and FIXED, and the first two diagnoses were wrong

Binary `gnss_processor_ffr2.exe` md5 `1760ABD54E83D1D68BFF6EB8804F0A10`; arms
`runs/cta4_q1e7_{ffr,ffr2}`; suite **1345 pass / 1 skip** (+10 new).

### §8.11.1 What `ratio = inf` actually is — §8.8's reading was WRONG

§8.8 called it "a degenerate ratio test, not a confident fix".  It is neither.
`AmbiguityResolver.cpp` sets

```cpp
result.ratio = std::numeric_limits<double>::infinity();  // sentinel: no LAMBDA ratio test ran
```

on the **WL-only** path, which the closed-loop consumer runs (`AR mode 1, wl_only on`).  That
path asserts MW-derived DD widelane integers directly as Kalman pseudo-observations at
`wl_only_pseudo_var = 1e-4 cyc²` (σ ≈ 0.01 cycle); there is no LAMBDA search, so **there was
never a ratio test to be degenerate**.  An `+inf` in a confidence column reads as a spectacular
ratio — that is how the blunder passed review, including mine, twice.  The column is now
honest: `p_succ` is emitted alongside, and `ar_gate` reports `wl_only` rather than `fixed`.

### §8.11.2 The FFR gate — necessary, correct, and it does NOT catch this

The path's only acceptance gate was `wl_max_frac`, a POINT test on each arc's running residual;
the LAMBDA tier's bootstrap FFR gate (`min_success_rate = 0.999`) never ran here — the standing
decision that "fixed failure rate is the acceptance gate at BOTH tiers" was simply not
implemented on this path.  Built: `MwSlipDetector::getResolvedWlQuality()` — per-arc σ of the
WL arc mean **and its sample count** (a 2-sample arc whose samples agree reports σ ≈ 0, i.e.
success ≈ 1 on one degree of freedom); the product taken over **DISTINCT ARCS**, not DD
constraints (many DDs share the pivot arc, so a wrong pivot makes all of them wrong together
and a per-DD product would count the shared failure once per DD); the identical
`erf(1/(2√2·σ))` term the LAMBDA tier uses (pinned by a unit test against
`ils::computeBootstrapSuccessRate`) so both tiers report ONE comparable number; **fail-safe** —
a missing entry, an immature arc or a non-finite σ contributes **0**, never 1; and backward
elimination of the weakest arc, mirroring `selectFixableSubset`.

**Armed, it changed nothing: `p_succ = 1.000000` and the 35 m excursion stood.**  Not a defect
in the gate — the gate being *right* about the wrong question:

> **A bootstrap success rate bounds the RANDOM failure of rounding an arc.  It is computed from
> that arc's own dispersion, and nothing in an arc's dispersion knows which FRAME the filter is
> in.  It is therefore structurally blind to a SYSTEMATIC mis-framing.**  Every contributing arc
> was mature with σ → 0, so `p_succ = 1.0` was the correct answer: those widelanes are precisely
> determined.  They were also in the wrong frame.

### §8.11.3 The mechanism, by elimination

| candidate | ruled out by |
|---|---|
| degenerate ratio test | the sentinel above |
| low-confidence integers | `p_succ = 1.000000`, all arcs mature |
| ambiguity covariance inflation (`kAmbResetVar`) | `previous_dd_wl_` is necessarily EMPTY at the first epoch the σ-gate admits, so no reset can fire |
| `holdAmbiguities` | returns early unless `ar_mode == 3`; the consumer is mode 1 |

What remains fits every observation: the MW widelane lives in the **.BIA frame** while a
network-bias consumer's ambiguities live in the **consumed phase-bias frame** — the exact
conflict `setArIgnoreMwWl` exists to avoid.  Asserting across that gap at σ = 0.01 cycle dragged
the position 33 m in one epoch; the next epoch's observations no longer fit, were rejected, and
the covariance grew on process noise alone (σ 0.050 → 3.138 m).  **The covariance INCREASING
across a "fix" is the tell** — an information-adding update cannot do that, so the growth is a
consequence, not the cause.

### §8.11.4 The gate that does catch it — the innovation screen

`wl_only_max_innov_cyc` (default **0.5 cycle**): the direct analogue of the LAMBDA tier's
per-entry `nl_max_per_pair_frac`, which this path had no equivalent of.  A constraint whose
asserted integer disagrees with the predicted DD widelane by more than half a widelane is **no
longer a rounding question**, and is dropped rather than allowed to drag the state across a
frame gap.

| q = 1e-7, settled | med | p95 | **worst** | > 20 m | fixes |
|---|---|---|---|---|---|
| no guards (`cta4_q1e7`) | 4.17 | 7.72 | **34.91** | 4 | 1 (`ratio=inf`) |
| + FFR only (`_ffr`) | 4.17 | 7.72 | **34.91** | 4 | 1 (`p_succ=1.0`) |
| **+ FFR + innovation (`_ffr2`)** | 4.84 | **7.65** | **7.75** | **0** | **0** |

`products.txt` is **byte-identical** across all three — the blast radius is grading-only on the
network path, which makes these three arms a clean one-knob A/B of the CONSUMER alone.  The
replay instrument is unchanged (5.52 / 7.30 / 7.50, 0 fixes): its σ never reached the arming
threshold, so the guards are correctly inert where there is nothing to guard.

### §8.11.5 ⇒ §8.8's boundary is DISSOLVED

With the consumer defect fixed, rung 3 is **better than rung 2 on every consumer statistic**
(4.84 / 7.65 / 7.75 vs 6.12 / 9.78 / 10.72), so §8.8's "boundary at rung 3" no longer stands —
it was a consumer defect the ladder exposed, exactly as §8.8 attributed it.  **What does NOT
change**: the producer-side cost is unaffected (products byte-identical), so σ̂₀_code still rises
4.126 → 4.212 and cb's variance factor 12.66 → 13.04 from rung 2 to rung 3.  **Still not a
promotion** — G4 remains the invariant-#1 blocker.

### §8.11.6 Remaining gaps, named not hidden

1. ~~**`applyMwWlConstraintsNlWl_` has BOTH holes**~~ — **CLOSED, §8.11.7.**
2. ~~**`wl_only on` may be the wrong posture entirely**~~ — **CLOSED, §8.11.9.**  Measured: it
   was, and not only in the tail — the guarded WL-only posture is ~1 m worse in the MEDIAN than
   not constraining at all.
3. ~~The consumer's `ambiguity_resolver` logger does not route to disk~~ — **FALSE, and the
   claim was mine.  See §8.11.10.**

### §8.11.7 The NlWl sibling — both guards applied

`applyMwWlConstraintsNlWl_` is the per-band sibling: it also asserts MW integers with no LAMBDA
search (same `+inf` sentinel) and had the same two holes.  Both guards now stand behind it in the
same order and semantics — **FFR first** (drop arcs whose failure rate cannot be bounded),
**innovation second** (drop assertions the float contradicts).  The order is not cosmetic: the
measured blunder passed the first with `p_succ = 1.0` and was caught only by the second, because
the two answer different questions — RANDOM rounding failure vs SYSTEMATIC frame disagreement.

**One trap this path has that `resolveWlOnly` does not.**  Its constraints are built per ARC from
the MW map, keyed by the detector's own `stats_key`, with `sig_hi` / `sig_lo` re-ordered by
FREQUENCY at construction:

```cpp
if (f_a > f_b) { c.sig_hi = sig_a; c.sig_lo = sig_b; }
else           { c.sig_hi = sig_b; c.sig_lo = sig_a; }   // <-- swapped vs the key
```

So `sat + "_" + sig_hi + "_" + sig_lo` is NOT the stats key whenever `f_a < f_b`; reconstructing
it would miss the quality map on exactly those arcs, and a miss is scored 0 by the fail-safe —
looking like a working guard while silently refusing every affected fix (or, had the fail-safe
gone the other way, silently bypassing the gate).  **The exact `stats_key` is therefore carried
verbatim on `ArcConstraint` and propagated to the DD constraint**, never rebuilt.

**Validation status: FIELD-EXERCISED as of §8.11.8.**  At ship time no config in the tree selected
`estimation.state_ambiguity_basis: nl_wl`, so the guards had no field evidence; what was
demonstrated then is that the wl_only consumer arm re-run on the binary carrying these edits
reproduces `runs/cta4_q1e7_ffr2` exactly (the consumer short-circuits to `resolveWlOnly` before
reaching the sibling), i.e. the edit provably did not reach the path this card exercises.
**Pinned in the unit suite** (`tests/test_wl_only_ffr.cpp`, 12 cases): the default-ON contract for
both guards and the FFR helper's fail-safe behaviour — flipping those defaults restores a 35 m
blunder with nothing in the logs to say so.

### §8.11.8 The NlWl guards, FIELD-EXERCISED — and the path was destroying the solution

**The vehicle.**  `_pppar_algo_static_gps_l1l2.json` — single-station static PPP on ALGO00CAN,
uncombined GPS L1/L2, external COD MGX FIN products, graded against the ITRF2020 SSC monument by
a grader independent of the engine.  `uncombined_enabled: true` is exactly NlWl's prerequisite;
adding `estimation.state_ambiguity_basis: nl_wl` routes AR through `applyMwWlConstraintsNlWl_`,
so the guards are ONE KNOB PAIR apart (`_nlwl_guarded` / `_nlwl_unguarded`), ~45 s per arm.

| 24 h static PPP, settled (hour ≥ 1) | median | p95 | worst | final |
|---|---|---|---|---|
| per-band control (basis unchanged) | **2.75 m** | 2.79 | 2.79 | 2.79 |
| NlWl, guards DISARMED (pre-fix behaviour) | **1026.71 m** | 2574.50 | 5000.37 | 2482.89 |
| NlWl, guards ARMED (shipped default) | **5.46 m** | 22.58 | 94.80 | 5.34 |

**The unguarded NlWl path does not merely risk a blunder — it DESTROYS the solution**, by three
orders of magnitude, on ordinary data with no event in it.  It had never been run.

**Why the FFR gate contributes nothing here — field-confirmed, not inferred.**  With the
diagnostics measured unconditionally, the DISARMED arm reports what the gate WOULD have seen:

| | applied sets, `P_s` | `max\|innov\|` seen | innov drops / all-rejects |
|---|---|---|---|
| disarmed | med **1.000000**, min **0.999913** | med 0.158, **max 17.13 cyc** | 0 / 0 |
| armed | med 1.000000, min 0.999969 | med 2.57, max 24.70 cyc | **372 / 142** |

**The bootstrap gate would have passed every single constraint set** — minimum over the whole 24 h
is 0.999913, above the 0.999 bar — while assertions off by up to **17.13 widelanes** were applied
at `wl_only_pseudo_var` σ ≈ 0.01 cycle.  §8.11.2's structural claim is now demonstrated twice on
independent data (the closed-loop consumer at 35 m, this at 1 km), not argued.

**⚠ SAFE ≠ GOOD.**  The guarded arm is still 2× worse than the per-band control in the body (5.46
vs 2.75 m) and 34× worse in the tail (94.80 vs 2.79) — consistent with what the path IS: it
applies MW widelane constraints and stops (`resolve()`'s own comment says the full LAMBDA NL search
with WL pre-constrained is future work).  **The guards make the NlWl posture safe to run; they do
not make it a working AR posture, and it must not be used as one.**

**Telemetry fix this A/B forced.**  `P_s` and `max|innov|` were computed INSIDE their guard blocks,
so a disarmed run reported `P_s=0.000000, max|innov|=0.0000` — zeros reading as "nothing to see"
when the truth was 17 cycles of disagreement going through unchallenged.  Same class of lie as
`ratio = +inf` reading as a perfect ratio, in code written to fix exactly that lie.  Both are now
MEASURED unconditionally and only ACTED on when armed, with the max labelled PRE-screen so it
cannot be misread as the applied set's residual.

### §8.11.9 The consumer POSTURE — measured, and the WL-only choice was a drag all along

§8.11.6 gap 2 asked whether `wl_only on` is the right posture for a network-bias consumer at all.
Measured on the q = 1e-7 arm, three postures, producer knobs identical throughout (`products.txt`
**byte-identical** in every arm — the consumer is held out and cannot reach the producer, so this
is purely a change of grading instrument):

| consumer posture | med | p95 | worst | > 20 m | fix epochs |
|---|---|---|---|---|---|
| BSSD + `ar_wl_only`, guards OFF | 4.17 | 7.72 | **34.91** | 4 | 1 (wrong) |
| BSSD + `ar_wl_only`, guards ON | 4.84 | 7.65 | 7.75 | 0 | 0 |
| **`consumer_undifferenced: true`** | **3.88** | **6.35** | **6.95** | **0** | 0 |

**The undifferenced consumer is best on all three statistics — and it never fixes**: all 350
settled epochs sit at `sigma_gate`, AR never fires, so the gain is **not asserting MW widelanes at
all**.  Middle row vs bottom: the GUARDED WL-only posture is still ~1 m worse in the median than
simply not constraining.  **The WL-only posture was a net drag on the BODY of the distribution the
whole time, not only a tail risk.**  It was also the wrong instrument on its own terms: the closed
loop exists to answer *"can a held-out station recover integers from the network's emitted phase
biases?"*, and under `ar_wl_only` the consumer never runs a narrowlane search — it asserts MW
widelanes from the **.BIA** product frame and stops, validating substantially the MW detector in a
different frame from the product under test.  `consumer_undifferenced` runs direct per-band LAMBDA
on the network-bias-corrected float, which IS the quantity the product must make integer, and
suppresses the MW feed as a consequence (`ar_wl_only_` left at default ⇒ `setArIgnoreMwWl(true)`).

**Adopted in the chain.**  `_agu63.json` now sets
`network.closed_loop.consumer_undifferenced: true`.  Verified by inheritance: the UNMODIFIED
`_cta4_q1e7_ffr2` config on the new chain reproduces the explicit undifferenced arm's
`closed_loop_trace.csv` **byte-identically**, `products.txt` still byte-identical to every other
arm.  `ar_wl_only` stays in the config, inert under the undifferenced path
(`deriveClosedLoopConsumerConfig` copies it only for a BSSD consumer), recording what the chain
used to run.  **⚠ SHARED-CHAIN EDIT** — every config extending `_agu63` inherits it.  **No product
lineage is invalidated** (products byte-identical, proven on this arm) but **stored
`closed_loop_trace.csv` numbers for other cards will not reproduce**: the grading instrument
changed, so a card re-reading old consumer numbers must re-run or note the posture change.
`_cta4_q1e7_bssd_ref.json` keeps the old posture reachable for that comparison.

**Attribution defect found and fixed in passing.**  The guarded BSSD arm showed 27 `mlambda_fail`
rows on paths where **MLAMBDA never runs** — the `ratio = +inf` mis-attribution class again.
`gate_reason = 5` → `wl_guard` is now set at all four guard-rejection returns in both trace
writers; on the re-run reference arm the 27 split **19 `wl_guard` + 8 `mlambda_fail`**, so the
single label hid two outcomes and "the 27 were guard rejections" (asserted before the arm ran) was
too strong — the residual 8 are genuine, the WL path returning before the guards when no
constraint is formed at all.

**⚠ Process failure worth recording.**  The first label check came back NEGATIVE (27
`mlambda_fail`, no `wl_guard`) though the code was correct: the arm ran `gnss_processor_ffr4.exe`
copied BEFORE the edit (`8CA08AEB…`) while the fix was in `88107F4C…`.  **A renamed exe — the
concurrent-build-lock workaround — is a SNAPSHOT: `cmake --build` updates `gnss_processor.exe` and
leaves the copy untouched**, so the arm silently tested old code with a clean run and no warning.
**Re-copy after every rebuild and hash BOTH.**  It surfaced only because the label was checked on
DATA rather than trusted from the diff.

### §8.11.10 The "broken consumer logger" — claim WITHDRAWN, cause NOT established

§8.11.6 gap 3 asserted the consumer's `ambiguity_resolver` logger does not reach disk even with
`enable_ambiguity_resolver: true`.  **Withdrawn — never established**, and so is the replacement
explanation (an inherited `warn` level).

- **Established:** the PPP arm `runs/nlwl_guarded` writes **181 KB** of resolver log while network
  arms write **0**; the network path DOES write per-logger files (same run: `gnss_config.log`
  44 KB, `main_log.log` 575 KB), so the file-sink machinery is not at fault; `_ft63_24h_conv`,
  four links up the chain, pins `levels.ambiguity_resolver: "warn"` and five other loggers with
  it; `logging.levels` IS parsed into `LoggingConfig::per_logger_levels`
  (`GnssConfigLoad.cpp:3118`) in a block with no enclosing guard.
- **NOT established, asserted prematurely twice:** that the logger is broken (original claim);
  that the inherited `warn` explains the empty file (the correction) — against which the 3 h
  diagnostic arm DID fix once, so it should have emitted one "applied N constraints" line and
  emitted none.
- **A THIRD sufficient cause invalidated the first test:** the 20-epoch check `runs/logchk` came
  back empty, but its consumer AR spent all 20 epochs in `min_epochs` (10) and `sigma_gate` (10)
  and **never reached `resolveWlOnly`**.  "Empty log file" has at least THREE sufficient causes —
  logger disabled, level pinned, code path never reached — and the first two were asserted before
  the third was eliminated.
- **The ergonomic fix is landed but UNVERIFIED — do not treat it as working.**
  `Application::loadConfiguration` emits a one-line ENABLED-BUT-SILENCED census naming every logger
  whose level sits above `info`.  Log-only (suite green at 1354 / 1 skip), but **it did not fire in
  the one run attempted** and that is unexplained: the string is in the binary and the emitting
  block has no guard, so either `per_logger_levels` was empty for that config (⇒ `logging.levels`
  does NOT deep-merge across the inheritance chain, contradicting the effective-config reading
  above) or something else.

**OWED — the decisive test** (needs a free machine; a concurrent session held the producer through
two attempts, and the shared `logs/` harvest makes overlapping runs mutually corrupting): run a
**3 h BSSD + `ar_wl_only`** arm — the one posture where the resolver PROVABLY reaches its logging
sites (19 `wl_guard` + 8 `mlambda_fail` measured in `runs/cta4_q1e7_bssd_ref`) — with **both**
`logging.enable_ambiguity_resolver: true` **and** `logging.levels.ambiguity_resolver: "info"`.
Config written: `_logchk3h.json`.  Log fills ⇒ the level was the cause and the census is the right
fix.  Stays empty ⇒ there IS a defect and the original claim was right for the wrong reason.

**This section's real finding is methodological.**  Five diagnoses here were wrong on first reading
and corrected only by measurement: `ratio = +inf` as a degenerate ratio (a sentinel), the FFR gate
as the fix (it passes at `p_succ = 1.0`), covariance inflation as the cause (a consequence), the
consumer logger as broken, then the level as its cause — plus a stale renamed exe making a correct
fix read as not-applied.  **On this card the plausible reading of a symptom was wrong essentially
every time**, and three of those readings reached a SPEC before they were caught.

---

## §9 (2026-08-31) G4's SIM HALF EXECUTED — the anchor truth-graded on the 12 h quiet flagship, and the displaced cb tension is REAL

**Bed:** `runs/camp60wu_quiet12h_r2q3` vs `runs/camp60wu_quiet12h_r2` — single
mechanism (the full Q3 arm at the CALIBRATED rung: q 3e-6, derived floors, proj
denominator, k 6, guard+resolve), over the R2 sticky-handover flagship on the
truth-graded synthetic network (SPEC_synthetic_minimum_network §17).  1440/1440.

**Mechanism read:** 71 983 anchored folds; **ZERO fault trips (0 nis + 0 abs)**
over 12 h — at q 3e-6 on a quiet field the guard never fires, which is §7's "at
the floor QUIET IS REAL" reading one decade above the floor.  214 decouples are
all MECHANICAL (201 datum + 13 gap), each wire-marked; 266 re-coupling edges.
Guard σ̂₀ 0.443 (conservative side, matching the ladder's descent), mean |ê|
3.6 mm against the 9.5 mm √(q·Δt) allowance.

**Truth A/B (settled half), the first ABSOLUTE grade any Q3 arm has had:**

| | R2 | R2+Q3 | Δ |
|---|---|---|---|
| consumer 3D rms (m) | 0.265 | **0.204** | **−23 %** |
| ZWD rms (m) / vf | 0.073 / 123 | **0.054 / 82** | −25 % / −33 % |
| clock contrast rms (m) | 1.940 | 1.922 | −1 % |
| clock residual rms (m) | 2.000 | 2.058 | +2.9 % (level) |
| clock vf | 33 358 | **216 567** | ×6.5 |
| **code bias rms / p50 (m)** | 0.538 / 0.0248 | **0.609 / 0.0277** | **+13 % / +11 %** |
| code bias vf | 4 841 | 6 460 | +34 % |

- **The §8.8 open question is ANSWERED: the tension the descent displaced into
  the code-bias carry is REAL ERROR, not a formal artifact** — cb truth rms
  +13 % under the anchor, the sim's absolute grade confirming what the field
  ladder could only see as vf.  The anchor buys wire/consumer quality partly by
  spending the cb channel — the LV-6 level-authority successor card inherits
  this bill.
- Consumer −23 % and ZWD −25 % are the anchor working as designed (temporal
  stiffness where the physics is quiet); per-sat clock contrast is flat (the
  truth error there is the level/exchange wander, not epoch noise — Q3 was
  never its cure).
- **Clock vf ×6.5**: the anchor tightens the published σ against a level error
  it cannot see — the carry-aware σ frontier, sharpened not created.  ⚠ Do not
  read Q3's clock vf as calibration on a truth-graded arm; the ladder's
  "vf = 0.99 at 3e-6" was the INTERNAL census against the carry, a different
  statistic.

**Telemetry fix (same session):** the run-end "N trips but ZERO in-epoch
re-solves — every trip shipped an anchor-warped epoch" warning counted the
COMPOSITE trip figure; gap/datum/ext decouples are not resolve candidates (the
prior is withdrawn BECAUSE the frame or visibility moved).  On this arm it
cried 214 warped epochs against a true fault-trip count of ZERO.  The warn now
fires on `nis + abs` only (`NetworkTeardown.cpp`) — the §8.11 ratio=inf lesson,
recurring in the code written after that lesson.

**Status:** G4's sim half is EXECUTED with the anchor NET-POSITIVE on the
product channels at the calibrated rung.  The FIELD half (the DOY-129 event
window — does the guard catch a real clock event without the withdraw path
regressing?) remains the arming blocker for field lineages; this arm never
exercised a fault trip, so the resolve path is still field-unvalidated.

### §9.1 (2026-09-01) THE SIM q-LADDER — 3e-6 confirmed a LOCAL OPTIMUM on truth, from both sides

Single-knob rungs around the calibrated point on the same bed
(`runs/camp60wu_quiet12h_r2q35` q 3e-5, `_r2q37` q 3e-7, vs the `_r2q3`
flagship at 3e-6 and `_r2` without the anchor; one build, fresh convergence
each, 1440/1440 all).  Settled-half truth grades:

| q [m²/s] | consumer 3D rms / p50 (m) | ZWD rms / p50 (m) | cb p50 (m) | clk vf | quiet trips |
|---|---|---|---|---|---|
| — (R2)   | 0.265 / 0.177 | 0.073 / 0.0160 | 0.0248 | 33 358 | — |
| 3e-5     | 0.209 / 0.143 | 0.062 / 0.0118 | 0.0275 | 112 736 | 0 nis |
| **3e-6** | **0.204 / 0.128** | **0.054 / 0.0116** | 0.0277 | 216 567 | 0 nis |
| 3e-7     | 0.240 / 0.171 | 0.101 / 0.0112 | 0.0272 | 406 038 | **5 nis** |

- **The §8.1-derived rung is the consumer optimum from BOTH sides**: loosening
  a decade gives back a third of the ZWD win and +12 % consumer p50; stiffening
  a decade costs +18 % consumer rms and DOUBLES ZWD rms on tails (p50 flat —
  the 5 quiet-bed nis trips' re-realization shocks, landing exactly at the
  §8.1 floor-crossover rung, are the tail source).  The sim independently
  confirms the field ladder's calibration with an absolute grade.
- **The cb bill is STIFFNESS-INDEPENDENT** across the two-decade window
  (p50 0.0272-0.0277 at every rung vs 0.0248 unanchored): the +11 % is the
  price of ARMING the anchor at all, not of its q — retuning q cannot buy it
  back; only the LV-6-class level authority addresses it.
- **Clock-σ overconfidence grows monotonically with stiffness** (vf 33 k →
  113 k → 217 k → 406 k): each decade of q tightens the published σ against
  the same level error it cannot see.  Consistent with §9's "sharpened, not
  created" reading; a stiffer-than-calibrated rung is strictly worse on
  honesty with no consumer payoff.
- Event census: identical mechanical decouples at 3e-5/3e-6 (214); 3e-7 adds
  its 5 nis trips (219 total).  Handovers, slips, refsat resets byte-flat
  across rungs.

**Status: the q choice is CLOSED on this bed** — 3e-6 stands in the flagship
and no rung retune is a live candidate.  The G4 FIELD half (guard catching a
real event) remains the field-lineage blocker, unchanged by this ladder.
