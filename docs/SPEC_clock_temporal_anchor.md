# SPEC — Card Q3: the guarded temporal clock anchor (CTA)
### (give the per-satellite clock ESTIMABLE CONTENT temporal stiffness — the estimator-side half that CLK-R, C1 and FC-1 all named as their prerequisite)

2026-08-10.  Owns the `per-sat clock VALUE` row of
`docs/SPEC_sbasis_realization_contract.md` §C — the one row whose
"estimator-side continuity" cell reads **missing**.  Successor to
`docs/SPEC_clock_emit_rebase.md` (CLK-R: the emission-side half — the
common-mode gauge is HELD, the per-sat walk is explicitly *not mediatable at
emission*, §2's falsification) and to the NO-GO pair
`docs/SPEC_c1_clock_rate_validation.md` / `docs/REPORT_fc1_split_cadence.md`
(both blocked on this card by measurement, not by taste).

---

## §1 Measured basis — what the walk actually is

All numbers from `runs/clkr_12h_hold/products.txt` (the post-promotion 12 h
denominator for the internal golden63 posture; `emit_clock_frame_hold` armed, so
the common-mode gauge is already held and the per-sat channel is clean).
Reproduced by `python scripts/product_step_census.py <products.txt>` (dev
channel) — the census's own 1439 steps × 32 satellites = 45 271 rows.

| statistic | value |
|---|---|
| per-sat common-removed step \|Δc0 − med\| — **median** | **6.23 cm** |
| p90 / p99 / max | 21.11 cm / 67.69 cm / 10.03 m |
| robust σ_w = 1.4826·MAD over all rows | **9.23 cm / 30 s** |
| **per-satellite** robust σ_w, min … median … max over 32 sats | **7.93 … 9.07 … 10.87 cm** |
| physical change over 30 s (FC-1, carrier-phase measured) | **1.7 – 2.0 cm**, corr ≈ 0 |

**The armed carry does not bind.**  The lineage runs
`filter.process_noise.sat_clock_q = 1e-3 m²/s` (the Jul-31 sweep's adopted
value), so the carry's own one-epoch spread is √(q·Δt) = √(1e-3·30) =
**17.3 cm** — nearly twice the realized 9.2 cm.  A random-walk prior only
constrains when its spread is BELOW the data's own per-epoch spread; above it
the prior is a bystander and the emitted clock is re-drawn from each epoch's
data.  That is the whole mechanism: **the walk is the satellite clock's
per-epoch ESTIMATION noise, published at 30 s cadence.**

**The per-satellite uniformity is the proof.**  Physical clock stability spans
orders of magnitude across the GPS fleet (Rb vs Cs, block IIR vs IIF vs III).
The measured per-satellite σ_w spans **1.37×** — 7.93 to 10.87 cm — with a
median of 9.07.  A physical process cannot be that uniform across that fleet;
an estimator's per-epoch noise, driven by common geometry and a common weight
model, is exactly that uniform.  (FC-1 reached the same verdict from the other
side: corr(producer Δc0, phase-measured Δc0) ≈ 0 at every hour.)

**Two exact identities used throughout** (scalar KF, prior N(μ, P⁻), data
information 1/R, posterior P⁺ = (1/P⁻ + 1/R)⁻¹):

1. `Var(x̂ − μ) = P⁻ − P⁺` — **exactly**, no approximation.  This is the
   variance of the guard statistic (§2.3) and it is precisely the `disp`
   already computed by the prior-residual census's folded branch
   (`NetworkPriorResidual::addRow`, `disp = r·fold_var == fold_var − post_var`).
   It is Teunissen's *generalized predicted residual* for a predictable
   function — the GKF-conformant test, and the same B3 data-incest-free form
   `NetworkClockQc` already uses for the two-state model.
2. Once the carry binds (q·Δt ≪ R), `Var(x̂_k − x̂_{k−1}) → q·Δt`.  So the
   emitted walk is **σ_w = √(q·Δt)** and the ladder is read directly off the
   armed q: 1e-4 → 5.5 cm, 3e-5 → 3.0 cm, 1e-5 → 1.7 cm.  The physical floor
   (2 cm) sits at q ≈ 1.3e-5 m²/s.

### 1.1 Why this is NOT the Jul-31 `sat_clock_q` sweep repeated

That sweep (memory `project_clock_model_hadamard_calib`) walked 1e-2 → 3e-4 and
was monotone better on every metric through the whole bracket, and stopped
there for one stated reason: *"a stiffer-than-measured carry is exactly the
invariant-#1 class"*.  **That is this card.**  Q3 is not a q value; it is the
guard that makes a binding q admissible, plus the per-satellite elapsed-time
bookkeeping without which a binding q is actively dangerous (§2.2).  The q
ladder is the last increment, not the first.

---

## §2 Design

One new seam, `filter/NetworkClockAnchor.{hpp,cpp}`, plus a variance decision
inside the existing D2 carry fold.  **Default OFF; OFF is structurally
byte-identical** (no new fold, no new state, no new trace row).

### 2.1 Stage A — the anchor (what changes in the fold)

`network.clock_temporal_anchor` (bool, default **false**) turns the satellite
clock's D2 carry variance from

```
var_eff = cp.var + sat_clock_q · dt_epoch
```
into
```
var_eff = max( cp.var + clock_anchor_q · Δt_sat ,  clock_anchor_var_floor_m2 )
```

applied **in both D2 branches** (the diagonal fold and the C1 dense fold's
`cov_eff` diagonal), because the marginal prior on any one state must be
identical either way — the property that makes the guard statistic
path-independent (`networkInjectD2CarryPrior`'s own comment says so).

Knobs (all `network.*`, all inert unless the anchor is armed):

| knob | meaning |
|---|---|
| `clock_anchor_q` | the anchor's random-walk rate, m²/s.  Emitted walk = √(q·Δt). |
| `clock_anchor_var_floor_m2` | **the deviation-#1 consistency floor**.  Caps the folded prior information at 1/floor so a diagonal-or-dense independence-assuming carry cannot accumulate unbounded information against a time-correlated error floor.  The clock analogue of the B4 `pb_var_floor` / `cb_var_floor` (whose own comment says *"clock is out of scope (own two-state model / monitor)"* — that exemption ends here), and ranked-plan item (3) of `project_network_ssystem_dia_assessment`, "var-floor default for stiff carries". |
| `clock_anchor_max_gap_s` | beyond this per-satellite elapsed time the carried clock is STALE: the anchor is not folded and the satellite is DECOUPLED (§2.4). |

**The covariance treatment, decided consciously** (the card's explicit ask).
The anchor rides the EXISTING D2 fold rather than adding a second prior:

- Under `shared_carry_full_cov` (armed on this lineage) the clock stays in the
  C1 dense block, so its off-diagonals are retained — the anchor does **not**
  re-create the independence assumption the C1 card removed, on the very state
  it is about to make the stiffest.
- Under `carry_invariant` (armed on this lineage) the shared PhaseBias carry is
  not stashed at all, so deviation #1's named worst case (clock↔PB) is not in
  the block here; what the dense path retains is **clock↔CB**, which is exactly
  the pair the DOY-129 ratchet ran through — retaining it is the safer choice,
  not the neutral one.
- The residual inconsistency of ANY stiff carry (information accumulating ~n
  against a time-correlated floor) is bounded by the variance floor above,
  which is a hard cap on the folded information regardless of branch.
**✔ BUILT 2026-08-10 as `network.clock_anchor_gauge_free` (default OFF, inside
the anchor's fingerprint).  The paragraph below is the ORIGINAL statement of the
question; the measurement that closed it and the corrected expectation follow.**

**MEASURED (3 h pair `runs/cta_3h_{ctl,on}`): the defect is real and quantified.**
The raw-state anchor cut CLK-R's drained common mode **Σ|μ| 49.43 m → 27.77 m**
and the pivot's dev row (which IS the drained common mode — §4.1) **16.79 →
10.03 cm**.  Two independent instruments, same 0.56–0.60 ratio: **the carry was
doing ~44 % of the datum's work.**

**⚠ CORRECTED EXPECTATION for the gauge-free arm — the obvious prediction has
the sign backwards.**  It is tempting to expect the pivot's dev row to *fall*
toward the fleet once the fold is gauge-free.  It must do the **opposite**:
removing the prior's information along the gauge hands the common mode back to
the data alone, so **Σ|μ| and the pivot dev row must RISE toward the control's
level (49 m / 16.8 cm)**.  That rise is the mechanism working, not a regression
— and it is harmless on the wire, because CLK-R's frame hold drains that
channel at emission by construction.  What must IMPROVE, if the fold is worth
arming, is the **estimable** side: the ex-pivot dev ceiling and floor, σ̂₀_code,
and the consumer.  Pre-registered as such before the arm was run.

**⚠ (historical) OPEN DESIGN QUESTION — the anchor as built constrains the RAW
x̃ˢ, not only its datum-invariant part.**  The per-constellation clock common mode is a
nuisance direction fixed by the pivot pin, and its measured emitted step is
~8.5 cm — above every rung of the ladder — so a binding anchor on the raw state
resists it, and the resistance lands on the pivot satellite's rows rather than
on the estimable between-satellite differences (a diagonal prior with near-equal
variances separates exactly into common ⊕ estimable, so the differences are
unbiased to first order).  The clean fix, if it is needed, is to fold the clock
prior **gauge-free**: one rank-1 downdate of the folded information along the
constellation's common direction u,
Λ ← Λ − Λu(uᵀΛu)⁻¹uᵀΛ, which puts ZERO prior information on the gauge and is
identical on the diagonal and dense paths.  That is the GKF "carry on FUNCTIONS,
not states" form and it would make the anchor datum-invariant at the MECHANISM
level rather than only at the grading level.  It is deliberately NOT built here:
whether the resistance costs anything is an empirical question with an exact
signature (post-fit σ̂₀ code and the within-epoch RAIM trip rate rising vs the
control, concentrated on the pivot's rows), it is pre-registered in G1/G2, and
shipping a second unmeasured mechanism inside the first would break the
one-knob-per-A/B rule.  **Measured so far: it does not express** — σ̂₀ code
4.2707 → 4.2698 at 20 ep and 5.0213 → 5.0886 at 1 h, RAIM 0 trips in every arm
(§5).  Re-check it at 12 h and at 305 stations before treating that as settled.

- A refloat (§2.4) is a **marginalization** of the key from the carried block —
  drop its row/col from the covariance and re-invert — which is the treatment
  `addSharedPriorDense` already documents as exact for a departed key
  ("Dropping rows of Λ would instead condition on the departed key — wrong
  statistics for every survivor").

### 2.2 Per-satellite elapsed time — the latent defect a binding q would detonate

`dt` in the driver is the time since the PREVIOUS EPOCH, and every skip path
(`gate_withheld`, σ̂₀ withhold, solve failure) advances `prev_tow` while the D2
carry survives untouched.  So a carried clock can be N epochs old while `dt`
still reads 30 s.

At the armed 1e-3 this is invisible (a 2× under-inflation of a prior that never
binds).  At an anchor q it is the invariant-#1 catastrophe verbatim: the
DOY-129 SH gate withheld **278 consecutive epochs** (95+ min) on the guards
chain, after which the carry would be folded as if it were 30 s old — a stiff
prior carrying a stale value straight through the event it was withheld for.

Fix (the same class as the clock model's own P0-3 / R11 fix, `osc.t_last`): the
anchor carries a per-satellite `t_last` stamped at every FOLD, and uses
`Δt_sat = pl_now_s − t_last(sat)`.  `Δt_sat > clock_anchor_max_gap_s` ⇒ the
satellite DECOUPLES (§2.4) rather than folding a stale value.

### 2.3 Stage B — the guard (the statistic)

Per satellite `s` that had a clock prior folded this epoch, on the solved
`x̂`:

```
e_s  = x̂_s − μ_s                                  (posterior − folded prior value)
ê_s  = e_s − median_{t : c(t)=c(s)} e_t            (per-constellation common mode)
S_s  = max( var_eff,s − var_post,s , σ_floor² )    (= P⁻ − P⁺, exact under H0)
w_s  = ê_s / √S_s                                  ~ N(0,1) under H0
```

- The **median removal is mandatory**, not cosmetic: the per-constellation
  clock common mode is a free gauge (Odijk Eq. 41 class, `q_rx_clock = 1000`),
  so `e` carries the epoch's whole gauge re-realization on every satellite.
  Standing invariant #5 — grade only datum-invariant quantities.  It is also
  what `NetworkClockQc`'s NIS path and CLK-R's census statistic both do.
- `S = var_eff − var_post` is exact, not a heuristic (§1 identity 1).  The
  floor `σ_floor²` (`clock_anchor_sigma_floor_m`, default the measurement-noise
  scale) prevents the 0/0 blow-up on a weak-data epoch where the data removed
  no information — the same reasoning as the B3 fix's `max(..., clk_model_r)`.
- Trip when `|w_s| > clock_anchor_k` **or** `|ê_s| > clock_anchor_abs_m`.  The
  absolute arm is a floor-independent backstop: it fires on a metre-class step
  even if `S` were inflated by a mis-set floor.

**Both thresholds are derived from measured data, never from the stochastic
model** (§3).

### 2.4 Stage B — the response (adapt-and-EMIT, both edges marked)

The per-satellite anchor is a two-state machine:

```
ANCHORED  ──trip / stale gap / resume seam / datum change / external refloat──▶  LOOSE(0)
LOOSE(n)  ──|w| ≤ k for n ≥ clock_anchor_recouple_epochs consecutive epochs──▶  ANCHORED
```

- **LOOSE folds at the legacy `sat_clock_q`** — i.e. exactly today's behaviour.
  That is the invariant-#7 *"the DECOUPLING edge must bound what the decoupled
  states can accumulate"* requirement discharged by construction: a decoupled
  satellite is not unconstrained, it is on the pre-card carry, which the
  existing `clock_runaway_check` monitor already bounds (`clk_mon_abs`,
  `clk_mon_jump`).
- **In-epoch response (this is the adapt-and-EMIT half).**  A trip does not
  merely flag for next epoch: the tripped satellites' clock priors are
  **withdrawn from the shared system and the epoch is re-solved**, so the
  EMITTED value for the trip epoch is the data's, not the stale anchor's.  The
  re-solve reuses the kernel's already-reduced normals (a shared-border
  information delta carries through the Schur reduction unchanged —
  `NetworkNormalEquations::addSharedInfo`'s reduction-preserving fast path), so
  it costs one datum + factorization of the K×K reduced system: **no
  re-reduction, no re-assembly**.  Capped at `kCtaMaxPasses = 3` re-solves
  (a pass can change the median and expose a second satellite).
- **Placement: immediately after the primary solve, BEFORE the within-epoch
  RAIM adaptation.**  Load-bearing.  A stiff prior fighting a real step dumps
  the tension into residuals, which raises σ̂₀ — and the within-epoch RAIM would
  then attribute a PRIOR fault to a satellite's or a station's DATA and drop
  it.  A prior fault must be resolved before the measurement-fault machinery
  looks.  The `cap_priors` / `cap_dense_*` replay channel is updated in step, so
  a subsequent RAIM re-solve cannot silently re-inject the withdrawn prior.
- **Never deweight.**  No measurement's weight is touched, at either edge.
- **Never withhold.**  The epoch always emits; a withhold would be graded as an
  adaptation failure.
- **The decoupling edge is MARKED on the wire**: the tripped satellite is
  queued into a per-sat discontinuity pending set drained by `networkSolPrep`
  → `emitter.markSatDiscontinuity` → R record + per-sat phase/code/clock
  counters — the B1 retro-bump chain Q4 proved end-to-end at the consumer.  The
  consumer must not integrate across a satellite whose clock just changed
  temporal regime AND value.  Marks land in the SAME epoch as the refloat
  (SolPrep runs before EmitStore).
- **The re-coupling edge is an EVENT with identity and a re-seed, and is NOT
  wire-marked.**  Re-engaging a prior at the carried value adds information; it
  does not step the emitted value (the prior's mean IS the last posterior), so
  there is nothing for a consumer to reset.  What invariant #7 requires there
  is re-validation — supplied by the `recouple_epochs` in-band run — and a
  re-seed: the first anchored fold after LOOSE uses `cp.var + q_a·Δt` where
  `cp.var` is the LOOSE posterior, which is the (larger) honest variance, not
  the steady-state tight one.  Event `cta_recouple` carries the satellite, the
  consecutive-in-band count and the realized dispersion.

### 2.5 Telemetry (standing invariant #4)

Armed-only run-end line via `networkTeardown`, plus `CLK_ANCHOR`-tier
`trace_events` rows at every edge:

`epochs armed · keys folded at the anchor · keys folded LOOSE · trips (by
reason: nis / abs / gap / resume / datum / external) · in-epoch re-solves ·
recouples · loose-epoch mass · max |w| and its identity · realized dev-channel
σ_w`.

**0 folded = the mechanism is inert.**  The counters, not the absence of
crashes, are the ON-path evidence.

### 2.6 Fingerprint class

**Solve-affecting ⇒ FINGERPRINTED ⇒ fresh-convergence arms only.**  A snapshot
written by an anchored run carries a carry whose variances were produced by a
different temporal model; a resume across the flip would mix them inside one
lineage.  Registered in the snapshot fingerprint alongside the other
solve-affecting knobs.

---

## §3 The empirical derivation (two-pass, pre-registered)

Never from the stochastic model — the standing directive, and the Jul-31 sweep
already showed the "measured WFM q1" was itself churn-contaminated because it
was measured under an unbound carry.

**INC-0 instrument — the `clk` family of the prior-residual census.**  The
census already standardizes exactly the statistic this card needs
(`disp = fold_var − post_var`), keeps a per-family σ̂₀ and a quantile LADDER of
|t|, and reports the max row's identity.  Its clock family was excluded by one
line (*"Clock / VtecSh out of scope (own models)"*).  Widening it is
trace-only, byte-identical, and gives the derivation its data.  The clk family
feeds the **common-mode-removed** ê (§2.3), unlike the pb/cb families — the
clock is the one family with a free per-epoch gauge, and standardizing the raw
`e` would report the gauge on every satellite.

**Pass 1 — locate the binding knee with σ̂₀_clk.**  In the non-binding regime
the realized dispersion is data-set, so σ̂₀ = √((σ_true² + R)/(q·Δt)) < 1 and
falls as q rises; once binding, σ̂₀ → √(1 + σ_true²/R) ≈ 1 and is **independent
of q**.  So **σ̂₀_clk = 1 marks the binding knee exactly**, and it is a
measurement, not a model.  From §1, the knee is expected near
q·Δt ≈ σ_w² = 8.5e-3 ⇒ q ≈ 2.8e-4 — to be MEASURED, not assumed.

**Pass 2 — the ladder below the knee.**  σ̂₀ saturates below the knee (it cannot
tell us how far to go), so the descent is graded on the wire and the consumer,
not on the filter's self-assessment: `q_anchor ∈ {knee, knee/10, 1.3e-5}`
(the last = the FC-1 physical floor).

**k (the guard threshold) is read off the measured |t| ladder** at the armed q,
with the guard disarmed (`clock_anchor_k` set beyond the ladder's top rung) so
the distribution is the un-truncated null.

**⚠ The trip-rate target stated in v1 of this spec (“≤ ~1–2 network-wide trips
per hour”) was MIS-SCOPED and is withdrawn.**  It was imported from CLK-R,
whose marked population was ~98 % estimator churn, and applied to a population
that is not churn at all.  Measured on the 3 h arm: **22 trips/h** through the
DOY-129 hour-2–3 run-up, and the live consumer was BETTER in every hour of that
window.  A trip is not a cost to be minimized — it is the guard reporting that
this satellite's clock moved by more than the anchor's own model allows, on a
day when satellite clocks were in fact moving.  **The rate is a property of the
DATA, not a tuning target, and tuning k to hit a quiet-hour number would be
tuning the detector to under-report activity on exactly the days the card
exists for.**

What replaces it, as the standing rule:

- **Grade the trip population, never its count.**  A trip is healthy when it
  (a) resolves in-epoch (`cta_resolves` tracks `trips`), (b) lands a wire mark
  that ties out event ↔ R record ↔ counter, and (c) leaves the consumer
  unchanged-or-better over the window that contains it.
- **A rate floor is still meaningful in the other direction**: ~0 trips over a
  window with known clock activity means the guard is not firing when it should
  — that is the invariant-#1 failure, and it is what G4 exists to exclude.
- k stays derived from the ladder for CALIBRATION (so |w| means the same thing
  across arms), not from a target rate.

**`clock_anchor_abs_m`** likewise from the census tail: the dev channel's p99 is
0.68 m and its max is 10.0 m over 12 h, so a backstop in the 1.5–3 m band
isolates genuine steps without touching the body of the distribution.

---

## §4 Pre-registered gates

- **G0 — OFF-path byte-identity.**  `_golden63_gps_12h_internal.json` (and the
  20-ep smoke) re-run unchanged on the session binary ⇒ `products.txt` record
  stream byte-identical to the stored run; unit suite green.  Doubles as the
  same-build control for every arm below.
- **G1 — 20-epoch ON smoke.**  Counters live (folded > 0); the anchor's folded
  variance is the armed one; trips ≈ 0; no withholds; console shows the armed
  line.  A run with `folded = 0` FAILS regardless of how clean the log is.
- **G2 — 1 h ON.**  σ̂₀_clk from the census (the knee read); dev-channel σ_w
  vs the 9.23 cm control; no new withholds; `total_IF` no-regression; the trip
  population healthy per §3 (resolve + mark + tie-out), NOT a trip count.
- **⚠ The per-satellite σ_w SPREAD prediction is withdrawn.**  v1 pre-registered
  that the spread should SHRINK toward a physical spread as the anchor binds.
  It does not, and the reason is structural rather than a defect: **the
  population bifurcates BY DESIGN.**  An anchored quiet satellite is driven
  toward the process floor √(q·Δt) (measured floor sat: 2.37 cm ≈ FC-1's ~2 cm
  physical), while a satellite the guard trips is deliberately handed back to
  the LEGACY carry for its loose window and marked — so it keeps the pre-card
  dispersion, on purpose, because that is what bounds a decoupled state
  (invariant #7).  A statistic that mixes the two populations must widen when
  the mechanism works.  Measured 3 h: spread 3.11× → 4.23× including the pivot,
  2.61× → 2.95× excluding it.  **Grade the FLOOR (does the quiet population
  reach the physical floor?) and the CEILING SEPARATELY (§4.1), never the
  ratio.**

### §4.1 Read the dev census PIVOT-AWARE — the ceiling row is a gauge

With `emit_clock_frame_hold` armed (the golden posture), the pivot satellite's
raw clock is pinned to 0, so its EMITTED value is exactly `−A_c`, the frame
accumulator.  Its dev-channel row is therefore **`Δ(−A_c) = −μ`, the per-epoch
drained common mode** — a gauge quantity, not estimable content, and the only
row in the census that is.  Verified on the 3 h pair: the pivot G01 is the
ceiling in BOTH arms (16.79 / 10.03 cm vs a fleet median of 8.64 / 4.33), and
its σ ratio across the arms (0.60) matches CLK-R's own independent Σ|μ| counter
ratio (27.77 / 49.43 = 0.56).

Consequences, both load-bearing:

1. **Quote the ex-pivot ceiling.**  Including the pivot row puts a gauge into a
   statistic the card grades as estimable content — a standing-invariant-#5
   contamination of the GRADER.  Ex-pivot ceiling on the 3 h pair: 14.08 →
   6.98 cm (−50 %), against 16.79 → 10.03 (−40 %) with it.
2. **The pivot row is the direct read of §2.1.**  It measures how much of the
   gauge's work the carry is doing.  The raw-state anchor cut the drained
   common mode 49.43 m → 27.77 m over 3 h — i.e. **the carry was doing 44 % of
   the datum's job**, which is the §2.1 defect quantified on the wire.

`scripts/product_step_census.py` reports the split directly (PIVOT / EX-PIVOT
lines), reading the pivot identity off the product's own S-basis contract line.

- **G3 — 12 h A/B (the verdict).**  ON vs the G0 control, same build, same
  config chain, fresh convergence both arms, runs never overlapping.
  - **headline**: dev-channel |step| median **6.23 cm → ≪, toward 2 cm**;
    robust σ_w 9.23 cm → √(q·Δt);
  - `scripts/fc1_grade.py` **shadow hour table** — the FC-1 memory's standing
    instruction is that this table IS the score for Q3 work: producer Δc0 vs
    phase-measured, per hour, with the correlation.  A successful anchor moves
    producer Δc0 from ~68–83 cm/30 s toward the ~1.8 cm physical AND lifts the
    correlation off zero;
  - consumer: live WTZZ closed loop + replay vs `runs/clkr_12h_hold` (hourly
    LEVEL bands, never slopes — the warm-resume/ignition trap);
  - `total_IF` hourly bands (datum-invariant; never `rms_clock_m`);
  - AR fix rate against the 0/1440 baseline — **hoped-for, NOT promised**;
  - counters tie out: trips = events = R records = consumer reactions.
- **G4 — the event-response validation (invariant #1's actual bar).**  A
  DOY-129 event-window replay of the 20:47 class must show the guard FIRING
  with marks at the decoupling edge and a `cta_recouple` at the re-coupling
  edge, and must show the epoch EMITTING (adapt-and-emit, not withhold).
  **Quiet-day cleanliness alone does not pass this gate** — an armed guard that
  has never fired on an event is an untested guard.  The DOY-129 lesson applies
  in full: event expression depends on ACCUMULATED FILTER STATE, so the arm must
  be a seamless lineage into the event window, not a warm resume onto it (a
  19:00-style seam measurably erases the event).

---

## §5 Results

Session binary `gnss_processor_cta.exe` md5 `F5068BA6707F04708CDB9299893D4F6A`
(Release, 2026-08-10).  Unit suite: **1331 passed / 1 skipped** (the fixture
gate) — +14 new cases (`tests/test_clock_anchor.cpp` × 11: OFF-path bit
equality, born-LOOSE, per-satellite elapsed time, staleness fallback, the
variance floor, epoch bookkeeping, the in-epoch step recovery, the
common-mode non-trip, the re-coupling run, and the two loader contracts;
`tests/test_network_estimator.cpp` × 2: the withdraw is the exact inverse of
the fold, and the dense re-fold equals a from-scratch marginalized fold, with
its restore branch and its no-op cases).

### G0 — OFF-path byte-identity: **PASS, maximally**

`_golden63_20ep_clkr_ctl.json` re-run unchanged on the session binary
(`runs/cta_g0_ctl`) is **whole-file byte-identical** to the stored pre-card run
`runs/clkr_20ep_ctl/products.txt` (md5 `C015A68F73393B3A1C0C02DEEC85E9C1`).
Re-verified after the R-record fix below.  Structural byte-identity when OFF is
proven at the wire, not argued.

### G1 — 20-epoch ON smoke (`runs/cta_20ep_{ctl,on}`): **PASS**

Run-end line, ON arm: *20 epoch(s); 589 clock carry fold(s) — **453 ANCHORED**
at q = 3.000e-05, **136 LOOSE**, 0 at the variance floor.  Trips 1 = 1 nis;
**1 in-epoch re-solve (0 failed)**, **32 re-coupling edges**, 1 wire mark.
Largest |w| 12.55 (k = 6.00) on G07 at TOW 259440.*  Control console: zero
Card-Q3 lines.

- **Not inert** (invariant #4): 453 anchored folds.
- **The anchor is EARNED**: 32 re-coupling edges = one per satellite, all at
  TOW 259320 — the 5th epoch, i.e. born LOOSE + 4 in-band epochs, exactly the
  configured re-validation.
- **Both edges exercised in 20 epochs**: `cta_decouple` G07 (|w| 12.55,
  ê = −1.527 m, med = −0.107, √S = 0.113) → in-epoch withdraw + re-solve →
  `cta_disc_landed` G07 at the SAME epoch → **`R,G07` on the wire**.  The
  census now reports `wire marks: R records=1` against the control's `NONE`.
- G07 at TOW 259440 is the same satellite and epoch CLK-R's independent
  stage-2 statistic marked in its own 1 h arm — two unrelated tests naming the
  same ignition step.
- **Ordering check** (the reason this seam sits before the within-epoch RAIM):
  post-fit σ̂₀ code 4.2707 → 4.2698, prefit H-rider σ̂₀ 4.3269 → 4.3179, RAIM
  T3 0 opp / 0 flag both arms, 20/20 emitted, 0 withheld.  No σ̂₀ inflation.

**Defect found and fixed during G1** — `markSatDiscontinuity` alone bumps only
the emitter's in-process counters; on a lineage without
`serialize_disc_counters` the per-record disc fields are not written, so the
**R record is the only carrier a `NetworkProductIO` replay can read**.  The
first ON arm produced a live-visible, replay-INVISIBLE mark.  Fixed by folding
the landed set into `retro_emit_sats` (which also gives those satellites the
"forced" treatment CLK-R's frame-hold median needs — a just-refloated satellite
must not vote).  Solve untouched: the dev-channel numbers are bit-comparable
across the fix.  ⚠ The three sibling bump queues in `networkSolPrep`
(`eph_cutover`, `cb_excursion`, `basis_relatch`) have the SAME gap and were
left alone — out of scope here, but they are marking a wire their own replay
cannot see.

### G2 — 1 h A/B (`runs/cta_1h_{ctl,on}`, one knob + the anchor block): **PASS**

Wire census (`scripts/product_step_census.py`, dev channel — the card's graded
quantity), 121 epochs, 3820 rows, 32 satellites:

| dev channel | ctl (q = 1e-3) | ON (q = 3e-5) | Δ |
|---|---|---|---|
| \|step\| **median** | 6.00 cm | **3.60 cm** | **−40 %** |
| p90 | 18.34 | **9.74** | −47 % |
| p99 | 52.56 | **31.82** | −39 % |
| **robust σ_w** | 8.89 cm | **5.34 cm** | **−40 %** |
| per-sat σ_w median | 9.08 | **5.70** | −37 % |
| epochs with a per-sat outlier > 0.5 m | 27 | **15** | −44 % |
| PB mid-arc steps > 0.1 m | 62 / 29 ep | 66 / 31 ep | +6 % (noise) |

QC: post-fit σ̂₀ code 5.0213 → 5.0886 (+1.3 %), 121/121 emitted both arms, **0
withholds, 0 skipped epochs, 0 within-epoch RAIM re-solves** in either arm.

Counters, ON: 3701 folds — **3537 anchored / 164 loose**, 8 trips (all `nis`),
6 in-epoch re-solves, 0 failed, 39 re-coupling edges, 8 marks → 4 R records.

### §3 pass 1 — the binding knee, MEASURED

The `clk` prior-residual family (Card Q3 INC-0) reports, over the same 121
epochs and 3700 rows:

| arm | armed q | **σ̂₀_clk** | implied self-consistent q = q·σ̂₀² |
|---|---|---|---|
| ctl | 1e-3 | **0.2617** | 6.8e-5 |
| ON | 3e-5 | **0.6977** | 1.5e-5 |

Both readings are < 1, i.e. **both carries are still looser than the data
supports**, and the iteration is converging downward — the fixed point (σ̂₀ = 1,
the binding knee) sits at **q ≈ 1.0–1.5e-5 m²/s**.

That number is the finding.  **q ≈ 1.3e-5 is independently where FC-1's
carrier-phase measurement of the PHYSICAL clock change puts it** (≈ 2 cm per
30 s ⇒ q = 0.02²/30 = 1.3e-5).  Two instruments that share no machinery — the
filter's own prior-residual dispersion and an external TDCP measurement of the
satellite clock — land on the same process noise.  It is also 20× tighter than
my own §3 desk estimate of 2.8e-4, which is exactly why the card requires the
knob to be measured and not modelled.

Ladder (|t| = |s|/σ̂₀, the empirical population k is read from): ctl >4σ̂₀
8.19 %, >12 0.62 %, >20 0.24 %; ON >4σ̂₀ 14.90 %, >12 **0.27 %**, >20 **0.11 %**
— the ON arm's extreme tail is ~2× thinner relative to its own (tighter) scale
while its body is fatter, the signature of a scale that is still slightly
under-estimated.  ⚠ The ladder and the guard's realized trip count do NOT yet
map onto each other (the >8σ̂₀ rung would predict tens of rows/h against 8
observed); the plausible reason is that the guard *suppresses its own
population* — a tripped satellite goes LOOSE and its next-epoch residual is
tested against the much wider loose null — but that is an inference, not a
measurement, and it needs the 12 h window to settle before k is set from the
ladder.

The census's FRAME label ties out exactly: **8 `cta_refloat` rows = the 8 guard
trips**, held out of the σ̂₀ calibration (their null is undefined, the same
treatment a held phase bias gets).

### Pre-registered prediction that did NOT verify

§1 argued the per-satellite σ_w SPREAD (1.37× across 32 satellites) is the
signature of estimation noise, and G2 pre-registered that it should SHRINK
toward a physical spread as the anchor binds.  Measured: **3.70× → 8.35×, it
WIDENED.**  The quiet satellites gained most (min σ_w 4.70 → 1.44 cm) while the
noisiest barely moved (17.38 → 12.05).  That is consistent with an anchor that
floors each satellite at its own data-limited rate rather than at a common
process floor — but it is not what was predicted, and it is not yet explained.
It does not touch the headline (the level and the median both fell 40 %); it is
flagged here so the 12 h arm and the FC-1 shadow table are read against it.

### G3 — the 3-arm 3 h ladder (2026-08-10)

Binary `gnss_processor_ctagf.exe` md5 `974714FB7174A8C9CE6FB239C71AB9EF`; arms
`runs/cta3_{ctl,on,gf}`, configs `_cta3_{ctl,on,gf}.json`, one build, one chain,
fresh convergence each, never overlapping.  Suite 1335 pass / 1 skip.

**Provenance, free**: `cta3_ctl` and `cta3_on` came out **byte-identical** to the
stored pre-gauge-free `runs/cta_3h_{ctl,on}`.  One comparison proving three
things — the estimator refactor (`denseFoldTerms_` + the gauge-free branch) is
arithmetically neutral on the OFF and raw-state paths; FC-1 `fast_clock.shadow`
is genuinely measure-only; and the earlier 3 h numbers transfer to this build.

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

**The §2.1 mechanism is PROVEN, with the corrected sign.**  Σ|μ| 27.77 →
**79.53 m**, i.e. above the control's 49.43 — the control's legacy carry still
loads the gauge slightly, the gauge-free fold loads it exactly zero.  The census
pivot row moves in lock-step (11.27 → 21.53).  10 922 keys downdated, **0 inert
epochs**.  Two independent instruments, same conclusion.

**The estimable side is a TRADE, not a win.**  Gauge-free buys the tail and
pays in the body, consistently across three independent instruments:

- dev p99 50.92 → **33.57** (−34 %), outlier epochs 48 → 43, but dev median
  3.55 → 3.81 and the ex-pivot floor 2.36 → 3.25;
- consumer worst 12.90 → **9.46** and p95 11.77 → **9.13** (both the best of the
  three arms, control included), but the median 5.49 → 6.64 and hour 0 3.00 →
  5.42 — a cold start converges more slowly with strictly less prior information;
- σ̂₀ code is monotone best (4.0460 → 4.0323 → **4.0057**), the predicted §2.1
  signature: the prior stops competing with the pin, so the pivot's rows stop
  carrying the conflict.

**Trips collapse 65 → 3, and the missing trips did NOT leave a fatter tail.**
That is the discriminating evidence: a guard that had merely been desensitized
would show its untreated events as a LARGER dev tail, and the gauge-free arm's
p99 is the smallest of the three.  Supporting: the raw-state arm's 63 `nis`
trips have |ê| median **0.28 m** (max 1.42 m) against a mean √S of **3.3 cm** —
sub-metre residuals made significant by a denominator smaller than the arm's own
realized dispersion (5.26 cm).  **0 of 63 would have been caught by the 2.0 m
absolute backstop.**  So the raw-state guard was substantially firing on the
walk's tail — inflated by the anchor's own conflict with the gauge — not on
metre-class clock events.

**⚠ CONFOUND, stated plainly: the gauge-free arm changed TWO things.**  The fold
AND the guard's denominator (§2.3's matched common-removed posterior).  The
matched denominator is the correct statistic for that fold — an unmatched one
would trip on a widening the fold itself created — but it is also independently
desensitizing, and this ladder cannot apportion the 65 → 3 collapse between
them.  The tail evidence above argues against pure blinding; it does not
substitute for the bisection.  **Owed: a `clock_anchor_gauge_free` arm with the
statistic forced to the raw-state denominator.**  It needs a knob, hence a new
binary, hence its own same-build pair — deliberately not half-done here.

**⚠ Instrument defect found: `σ̂₀_clk` is not gauge-aware.**  0.2803 → 0.7539 →
**4.4806**.  The census still standardizes by `last_var_eff`, the per-satellite
RAW anchor variance, while the gauge-free fold carries strictly less information
than that (by exactly the gauge direction), so `disp` under-states and σ̂₀ blows
up.  The guard's denominator was matched to the fold; the census's was not.  The
knee readings (§3) stand for the ctl/anchor arms and **must not be read across
the gauge-free arm** until the census's fold_var is made gauge-aware.

### The FC-1 shadow hour table — the score, and it says NO

`project_fast_clock_card`'s standing instruction is that this table IS the score
for Q3 work.  `scripts/q3_fc1_hours.py` (cross-validated against the project's
own `scripts/fc1_grade.py shadow` — SD-RMS agrees to 3 decimals: 21.723 /
27.114 cm):

| 3 h, common-removed | ctl | anchor | gauge-free |
|---|---|---|---|
| producer, robust σ (**the body**) | 8.83 cm | **5.25** | 5.60 |
| producer RMS, declared marks removed | 21.72 | 15.83 | **13.93** |
| producer RMS, raw | 21.72 | 27.11 | **19.85** |
| phase-measured (TDCP) | 0.30 | 0.30 | 0.30 |
| **corr(producer, phase)** | −0.004 | **−0.004** | **−0.005** |
| ratio phase / producer | 0.014 | 0.011 | 0.015 |

**Not one arm moves the correlation off zero.**  The card's own pre-registered
FC-1 gate — *"a successful anchor moves producer Δc0 toward the phase column AND
lifts the correlation off zero"* — is **NOT met**.  Q3 at q = 3e-5 makes the
emitted clock **quieter, not more real**: the body falls 41 %, the marks-removed
RMS falls 27–36 %, and the fraction of the emitted motion that is information
stays at ~1–1.5 %.

Two methodological notes that fall out of it, both reusable:

1. **An RMS is the wrong statistic for a guarded arm.**  The raw SD-RMS gets
   WORSE with the anchor (21.72 → 27.11) purely because the guard's 65 marked
   refloat steps are large one-epoch changes BY DESIGN, and RMS is
   tail-dominated — while every satellite in the grader's own worst-5 table
   improved (G22 16.1 → 13.9, G28 14.7 → 11.5, G29 13.6 → 9.6, G18 12.8 → 9.4).
   A declared discontinuity is not an error the consumer integrates; it is one
   it is told to reset at.  `q3_fc1_hours.py` therefore reports the raw RMS, the
   robust body, AND the marks-removed RMS, and none of the three is quotable
   alone.
2. **The physical floor on this lineage is an OPEN NUMBER and it gates
   everything.**  The TDCP column measures 0.30 cm per 30 s here, **6× smaller**
   than the 1.7–2.0 cm `project_fast_clock_card` records (different roster, era
   and possibly statistic).  If 0.30 cm is right, q_phys ≈ 3e-7 and the anchor
   has two orders of magnitude of headroom left, which would reframe the whole q
   ladder.  Resolve this before the next q rung — it is the difference between
   "nearly done" and "barely started".

### Not yet run

- **G4 — the DOY-129 20:47 event-window replay** (invariant #1's actual bar).
  Still OWED.  The guard has now fired on real data (65 trips incl. 2 genuine
  staleness `gap` trips at TOW 262950 / 267750, on satellites whose carry had
  aged past 120 s) but never on a KNOWN event with a known answer.  **Quiet-day
  cleanliness is explicitly not a pass.**
- The 12 h leg, and therefore the comparison against the `runs/clkr_12h_hold`
  denominator: a 3 h window cannot settle the LEVEL channel (~2 h) and is not
  like-for-like against a 12 h run.  `total_IF` is likewise not graded here.
- The gauge-free bisection and the gauge-aware `σ̂₀_clk`, both named above.

---

## §6 Relationship to the surrounding cards

- **CLK-R** (`emit_clock_frame_hold`, promoted): holds the clock's *gauge* at
  emission.  Q3 is the *content* half.  They compose: CLK-R makes the per-sat
  channel readable, Q3 makes it quiet.  CLK-R stage 2's per-sat marks stay OFF;
  Q3's marks are a different population (guard trips, not census steps).
- **C1 / FC-1**: both measured NET-HARMFUL because they predict a walk that is
  producer-internal.  Q3 is the named prerequisite of both; FC-1's shadow
  instrument is Q3's scoreboard (G3).
- **`network.clock_model`**: stays NO-GO.  Q3 deliberately does NOT build on the
  two-state carry — that card's data-incest defect (posterior fed back as a
  measurement with fixed R) is unfixed, its RWFM premise was falsified (p ≈ 1),
  and `service_mode` force-arms it while silently making `sat_clock_q` a no-op.
  Q3 uses the SCALAR carry and only borrows the two-state path's B3-corrected
  innovation VARIANCE, which is a general identity, not that model.
- **Odijk et al. 2016**: Q3 implements the paper's assumed random-walk clock
  constraint at a defensible q — the one row of the realization contract that
  no S-transformation can fix, because it is a property of the estimable
  content and not of the basis.
