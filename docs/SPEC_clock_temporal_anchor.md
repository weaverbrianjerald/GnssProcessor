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

## §7 (2026-08-10, Fable) THE PHYSICAL FLOOR MEASURED — trap 5 resolved, the FC-1 gate RETIRED, the ladder re-scoped

`scripts/q3_clock_structure_function.py` on `runs/cta3_ctl` (32 sats, 3 h,
common-mode removed).  The per-tick TDCP telescopes — accumulated change over
lag L is the endpoint phase difference, interior noise cancels — so a structure
function separates floor from walk:

| lag [s] | SF robust [cm] | SF/√L |
|---|---|---|
| 30 / 60 / 120 | 0.269 / 0.262 / 0.268 | falling — **floor, flat** |
| 300 → 2400 | 0.318 → 0.846 | **0.0163–0.0173, constant — walk** |

**q_phys ≈ 2.9e-8 m²/s → 0.94 mm per 30 s (fractional stability ~1e-13).**
The clock corrections beyond the COD a-priori are quasi-static at the
mm-per-epoch level — the user's clock-linearity expectation, confirmed
quantitatively.  Consequences, each load-bearing:

1. **Trap 5 is RESOLVED, and both prior "physical change" numbers were the
   FLOOR, not the walk.**  FC-1's 1.7–2.0 cm hour table (its own horizon
   column was already flat 30 s → 1200 s = non-accumulating) and this
   lineage's 0.30 cm TDCP column both measured SF(30 s)-class floors.  The
   accumulating physics is 20× below even the smaller of them.
2. **The FC-1 correlation gate is RETIRED, not merely unmet.**  True per-step
   motion 0.9 mm against a 2.7 mm measurement floor ⇒ the correlation is
   structurally unliftable at ANY reachable q rung.  At the floor, quiet IS
   real.  Replacement gate set: dev census ex-pivot floor/ceiling, σ̂₀_code
   flat, consumer live+replay unchanged-or-better, guard-population health
   (§3's rule), and G4's event response.  "QUIETER not MORE REAL" stands as a
   description of q = 3e-5; it is no longer a criticism of the card.
3. **The §3 knee (σ̂₀_clk → 1 at q ≈ 1.0–1.5e-5) is the DATA-INFORMATION knee,
   not physics** — it marks where the carry meets the per-epoch measurement
   information, ~3 orders above q_phys.  Below it σ̂₀ cannot validate (as §3
   already said); the descent 3e-6 → 3e-7 → 1e-7 is graded on the replacement
   gate set.
4. **The variance floor changes role from "armed but inactive" to GOVERNING.**
   At q ≤ 3e-7, q·Δt ≤ (3 mm)² sits below any honest correlated-error floor,
   so `clock_anchor_var_floor_m2` must be SET FROM THIS MEASUREMENT (the
   (2.7 mm)² floor + margin ⇒ ~1e-5 m² class, derivation in the ladder card)
   and is expected to BIND — deviation #1's consistency cap becoming
   load-bearing is the design, not a warning condition.  Invariant-#1 pressure
   is maximal at the bottom rung (a near-zero-q carry): G4 gates any arming.

**Design principle (user, 2026-08-11), standing for this ladder and beyond:
aim to be PHYSICALLY CORRECT — set stochastic models to measured physics and
let model errors express elsewhere as visible residuals/events, rather than
absorbing them into inflated q.**  An inflated q is exactly how the 9 cm walk
stayed invisible for a year: the error hid in the loosest state instead of
showing up as a residual anyone would grade.

## §8 (2026-08-10/11) THE FLOOR-GOVERNED LADDER — floors derived, the guard's denominator bisected, the descent 3e-5 → 1e-7

Build `gnss_processor_cta4.exe` md5 `94EF4327BABE0E0C9140696AA446D384`; suite
1335 pass / 1 fixture skip; configs `_cta4_*.json`; arms `runs/cta4_*`; one
build, one chain (`_golden63_gps_1h_laneoff` direct), fresh convergence each,
serialized by `scripts/q3_ladder_run.ps1` (it refuses to start while any
producer is alive).  Graded by `scripts/q3_ladder_grade.py`.

### §8.1 The floors, DERIVED — not tuned

§7 measured the structure function but left the cap a class ("~1e-5 m²").  The
derivation, from the same run (`runs/cta3_ctl`, re-measured on this session's
tree so the ladder rests on a number this leg produced):

```
SF(30 s)            = 2.690 mm      robust σ of the 30 s CHANGE (floor ⊕ walk)
walk(30 s)          = √(q_phys·30)  = 0.938 mm          (q_phys = 2.93e-8 m²/s)
change floor        = √(2.690² − 0.938²) = 2.521 mm     ⇒ 6.36e-6 m²
per-sample floor    = 2.521/√2       = 1.783 mm         (flat SF ⇒ white per sample,
                                                         so the change floor is √2 σ)
```

- **`clock_anchor_var_floor_m2` = 1.0e-5 m²** = (1.25 × 2.521 mm)² = 9.93e-6,
  rounded.  The margin is in σ and is stated rather than fitted: the 2.521 mm is
  a robust point estimate from one arm, and the producer's own absolute-clock
  error floor is *at least* the TDCP instrument's differential floor (the TDCP
  cancels the receiver clock and the a-priori; the emitted clock does not).  The
  cap must not sit BELOW the measurement, because under-capping is the failure
  mode it exists to prevent.  Landing on §7's independently-predicted "~1e-5 m²
  class" is a check, not a coincidence.
- **`clock_anchor_sigma_floor_m` = 0.0018 m** = the per-sample floor.  **This
  knob is not free, and leaving it at the shipped 0.02 m would have quietly
  replaced the guard's statistic at the bottom rungs.**  `√S` is floored at this
  value, and `S = P⁻ − P⁺ ≤ P⁻`, which the variance floor caps at 1.0e-5 m² —
  so at the bottom rungs the ENTIRE achievable denominator is ≤ 3.16 mm.  A
  20 mm floor sits 6× above that: `max()` would select it on every row, `w`
  would degenerate to `ê / 0.02` — a fixed 2 cm bar wearing a predicted
  residual's name — and every counter would still read healthy.  The two floors
  are one physical quantity read in two places, which is why they move together
  and why neither is a tuning knob.

Where the cap bites, by construction:

| rung q [m²/s] | √(q·Δt) at 30 s | vs the 3.16 mm floor σ |
|---|---|---|
| 3e-5 | 30.0 mm | inert |
| 3e-6 | 9.49 mm | inert |
| 3e-7 | 3.00 mm | **crossover** |
| 1e-7 | 1.73 mm | **the floor GOVERNS** |

### §8.2 The owed bisection — `network.clock_anchor_gauge_free_stat`

`auto` \| `raw` \| `post` \| `proj`.  It selects the guard's (and now the
census's) DENOMINATOR independently of which prior the FOLD applied, which is
what §5's confound needed.  The withdraw path deliberately keeps reading
`clock_anchor_gauge_free`: what a refloat un-folds has to be what was folded.

Reading it required naming the statistic properly.  The numerator is the
COMMON-REMOVED residual ê, so both halves of `S = P⁻ − P⁺` must be variances of
**that contrast**:

```
Var(xₛ − mean_t x_t) = C_ss − 2·mean_t C_st + mean_{t,u} C_tu     (clockAnchorGaugeProject)
```

applied to the posterior block for P⁺, and to `diag(var_eff)` for P⁻.  It is
correct for **both** folds: under the gauge-free fold the prior is
`diag(var_eff) + ∞·11ᵀ` and the contrast is orthogonal to `1`, so the infinite
part drops out exactly; under the raw fold the prior IS `diag(var_eff)`.  So the
odd one out is the RAW marginal — and it was the shipped default.  Using it
over-states P⁺ (the raw marginal still carries the common-mode variance the
median just removed) and therefore UNDER-states S: **the raw statistic
over-trips by construction, on the raw fold too, not only on the gauge-free
one.**  That is a correctness claim, derived before the arms were run, and
`cta4_bis_raw_proj` is its test.

`auto` reproduces the shipped pairing byte-for-byte — proven, not asserted (§8.4).

**Residual approximation, stated rather than hidden.**  The numerator removes the
per-constellation MEDIAN (robust, standing invariant #5) while the projection
above is the MEAN contrast.  For n satellites the two differ only in the O(1/n)
term — a median's sampling variance is ≈ π/2 times a mean's, so the correction
sits on ~3 % of the denominator at n = 32 and is worth ~1.5 % in σ.  It is left
unmodelled deliberately: modelling it would trade an exact, closed-form,
fold-matched identity for an asymptotic robustness correction, and the term it
would fix is an order of magnitude below the effect the projection itself
removes.

### §8.3 The gauge-aware σ̂₀_clk, and the guard's own ladder

- **Census** (`NetworkStashStatus`): the `clk` family now standardizes by the
  projected pair through the SAME helper the guard uses, so census and guard are
  the same statistic in every arm — the census stays a cross-check rather than
  becoming a second implementation that can drift.  §5's instrument defect
  (σ̂₀_clk 0.28 / 0.75 / **4.48**, the last one an artefact of standardizing a
  gauge-free fold by the raw anchor variance) is closed by construction.
- **Run-end guard ladder** (`NetworkTeardown`): mean √S, the |w| quantile ladder
  and an ABSOLUTE |ê| ladder in metres, over anchored rows on the first guard
  pass.  Its job is §3's per-rung k re-derivation: the denominator shrinks as q
  tightens, so a FIXED k is a different bar in METRES at every rung, and the line
  prints `k · mean √S` so that bar is legible.  The |ê| ladder is what the 2.0 m
  absolute backstop's rail is read off — the number that was missing when the 3 h
  arm found 0 of 63 nis trips would have been caught by it.
  **Caveat printed with the ladder**: the population is truncated ACROSS epochs
  (a tripped satellite goes LOOSE and stops contributing), so the upper bins
  under-report; the un-truncated null needs an arm with `clock_anchor_k` beyond
  the top rung and `clock_anchor_abs_m: 0`.
- **Floor telemetry**: the run-end line now reports the floor BINDING fraction,
  and says in place that binding is the DESIGN at these rungs (§7.4), not a
  warning condition.

### §8.4 Pre-registered gates for this leg (written before any 3 h arm was read)

The §7 replacement set, made per-rung and per-arm.  The FC-1 correlation gate
stays RETIRED (§7.2) and is reported for continuity only.

**Provenance gates** — these decide whether anything below is interpretable:

- **G0a OFF byte-identity** — the 20-epoch control re-run on this build must be
  byte-identical to the stored `runs/cta_20ep_ctl`.
- **G0b ARMED byte-identity** — the 20-epoch RAW arm and the 20-epoch
  GAUGE-FREE arm must ALSO come out byte-identical to their stored runs.  The
  gauge-free one is the one that matters: it is what proves `auto` reproduces
  the shipped pairing rather than merely claiming to.  An OFF-path check cannot
  certify this, because an OFF run never enters the anchor.
- **G0c the TIE arm** — `cta4_tie`, knob-for-knob `_cta3_on` at full 3 h length,
  byte-identical to `runs/cta3_on`.  Every later comparison chains through that
  arm, so build-neutrality is proven at the length it is used at.

**Per-rung gates.**  Chained so each link differs by ONE knob (or one
physically-coupled knob PAIR):

```
cta3_on (raw,raw,shipped floors) --stat--> bis_raw_proj --floors--> q3e5 --q--> q3e6 --q--> q3e7 --q--> q1e7
cta3_gf (gf,post)                --stat--> bis_gf_raw / bis_gf_proj
```

1. **ex-pivot dev census floor / ceiling** (`product_step_census.py`, PIVOT row
   excluded — §4.1, it is a GAUGE).  The floor should approach √(q·Δt) per rung
   until the variance floor takes over; the ceiling must not grow.
2. **σ̂₀_code flat.**  A rung that raises it is pushing carry tension into the
   observation residuals — the visible-elsewhere outcome the design principle
   wants, and a stop signal for the descent.
3. **Consumer unchanged-or-better**, on BOTH instruments (live closed-loop
   hourly LEVEL bands, and the replay), hourly levels never a slope.
4. **PB / CB channels untouched** — this card owns the clock row only.
5. **Guard population healthy, never a count** (§3): trips resolve in-epoch,
   marks tie out event ↔ R record ↔ counter, consumer unchanged-or-better over
   the window containing them.
6. **k re-derived per rung** from the run-end |w| ladder and reported in METRES
   (`k · mean √S`); the 2.0 m absolute backstop's realized rate read off the |ê|
   ladder.  k is REPORTED, not re-armed — re-arming it mid-ladder would make the
   rungs more than one knob apart.
7. **Floor binding is evidence, not a fault** at rungs 2–3 (§7.4).  What must be
   watched instead is the census `clk` family and the VCE-class monitors.

**Stop rule.**  If a rung degrades the consumer or σ̂₀_code, that rung is the
BOUNDARY and no rung below it may be described as validated.  A
physically-correct q that measures worse is a FINDING about unmodelled error
becoming visible, which is the outcome the design principle wants surfaced — not
something to tune away by re-inflating q.

**Deliberate deviation from the stop rule, declared here**: the rungs below the
boundary are still RUN (7 min each), because whether the degradation is a cliff
or a ramp is itself the diagnostic, and stopping would leave that unmeasured.
The stop rule is honoured where it matters — in what may be called validated.

**NO promotion.**  G4 (the DOY-129 20:47 event window) remains the standing
invariant-#1 blocker and is a separate card.  Nothing in this leg is a promotion
argument, and the loader defaults stay OFF.

### §8.5 Provenance — all four identity gates PASS

| gate | arm | vs stored | result |
|---|---|---|---|
| G0a OFF, 20 ep | `cta4_20ep_ctl` | `cta_20ep_ctl` | **byte-identical** |
| G0b RAW armed, 20 ep | `cta4_20ep_on` | `cta_20ep_on` | **byte-identical** |
| G0b GAUGE-FREE armed, 20 ep | `cta4_20ep_gf` | `cta_20ep_gf` | **byte-identical** |
| G0c RAW armed, **3 h** | `cta4_tie` | `cta3_on` | **byte-identical** (`72FCF8E5E34668EA76106FA5C79FAB5E`) |

The gauge-free one is the load-bearing check: it proves `auto` reproduces the
shipped pairing rather than merely claiming to.  G0c proves it at the length the
chain is actually used at.  So every number below is same-build, same-chain, and
the stored `cta3_{ctl,on,gf}` arms remain valid members of this leg's table
without re-running them.

**⚠ The 20-epoch smokes gave the WRONG SIGN, and the settled arms reverse them —
"never verdict in ignition" applies to the guard's own null too.**  Recorded
because it nearly became a finding:

| raw fold, denominator isolated | mean √S | guard σ̂₀ = √(Σw²/n) | \|w\| ≥ 1 |
|---|---|---|---|
| 20 ep `raw` | 0.0914 m | 0.926 | 10.2 % |
| 20 ep `proj` | 0.2259 m | 0.475 | 2.6 % |
| **3 h `raw`** (`cta4_tie`) | **0.0354 m** | **1.754** | **49.8 %** |
| **3 h `proj`** (`cta4_bis_raw_proj`) | **0.0819 m** | **1.091** | **20.7 %** |

At 20 epochs the carried posterior variance is still the cold-start one and
dominates `P⁻` on both sides, so the ordering of σ̂₀ is an artefact of
convergence.  Read the settled pair.  It says three things at once:

1. **The raw statistic's `mean √S = 3.54 cm` independently reproduces §5's
   hand-measured 3.3 cm** — the new telemetry and the old manual reading agree,
   which is what makes the rest of the column trustworthy.
2. **The projection widens the denominator 2.31× in σ with the numerator
   standing still** — the |ê| ladders agree to within sampling (≥ 5 mm: 89.0 %
   vs 89.5 %; ≥ 5 cm: 31.8 % vs 36.2 %).  The whole effect is the denominator.
3. **The corrected statistic is CALIBRATED and the shipped one is not.**  Under
   a correctly specified null σ̂₀ → 1.  Raw reads **1.754** — the exact signature
   of a denominator too small by ~√3 — and proj reads **1.091**.  The
   correctness claim of §8.2 was derived from the algebra before any arm ran;
   the guard's own null now confirms it from data.

Quote BOTH σ̂₀s and never one alone: the guard's is an RMS over every anchored
row and is tail-dominated, while the census's is the robust calibrated estimator
(`n_eff = 2572 of 10879` on this arm).  They answer different questions, and on
a heavy-tailed population they differ by ~2.4×.

### §8.6 The bisection — RESOLVED, and it inverts §5's reading

The 2×2 over (fold, denominator), all q = 3e-5, all shipped floors, one build,
one chain.  `cta3_on` and `cta3_gf` are the stored arms; the other three are new.

| trips (nis) | stat `raw` | stat `post` | stat `proj` |
|---|---|---|---|
| **fold raw** | 65 (63) `cta3_on` | — | **12 (10)** `cta4_bis_raw_proj` |
| **fold gauge-free** | **109 (107)** `cta4_bis_gf_raw` | 3 (0) `cta3_gf` | 3 (0) `cta4_bis_gf_proj` |

**The gauge-free FOLD, on its own, makes the guard trip MORE — 65 → 109.**  The
whole 65 → 3 collapse, and then some, is the DENOMINATOR.  §5 recorded the
confound honestly and guessed the tail evidence argued against blinding; the
apportionment says the fold contributes in the opposite direction to the guess.

Everything else separates just as cleanly, and each half stays with its own
cause:

| | fold effect (stat held) | stat effect (fold held) |
|---|---|---|
| Σ\|μ\| drained (the GAUGE) | 26.7 → **81.5** m | 26.7 → 26.7 / 79.5 → 79.5 (**nil**) |
| trips | 65 → 109 (raw stat) | 65 → 12 (raw fold) |
| σ̂₀_code | 4.0519 → 4.0379 | 4.0323 → 4.0519 |
| guard σ̂₀ | 1.754 → 2.096 (raw stat) | 1.754 → **1.091** (raw fold) |

The gauge channel is **purely** the fold (81.5 vs 79.5 across both denominators —
the statistic cannot move it, by construction, and does not).  The trip
population is **overwhelmingly** the denominator.

`proj` and `post` produce a **byte-identical product** under the gauge-free fold,
which is the expected degeneracy: that fold already carries no gauge
information, so projecting the prior is only the O(1/n) diagonal correction and
no trip flips.  They are distinct arms only on the raw fold.

**⚠ NEW DEFECT — the gauge-free fold LEAKS ESTIMABLE STIFFNESS, and the corrected
statistic is what exposed it.**  Mean √S on the contrast, same q and same data:

| | mean √S | k = 6 as a bar | guard σ̂₀ | clk variance factor |
|---|---|---|---|---|
| raw fold + proj | **0.0819 m** | 0.491 m | 1.091 | **1.21** |
| gauge-free fold + proj | **0.4298 m** | 2.579 m | 0.178 | **0.057** |

A 28× weaker prior VARIANCE on exactly the between-satellite differences the
fold exists to constrain.  The mechanism is mechanical, not statistical: the D2
carry re-reads the **raw marginal posterior** as `prev_var`, and under a
gauge-free prior that marginal is gauge-inflated (the prior puts no information
along the gauge, so the posterior's gauge component is data-only and large) —
which then inflates the next epoch's `var_eff`, and compounds.  So `cta3_gf`'s
3 trips were never a quiet guard: the prior was too loose to conflict with
anything, and `clk` vf = 0.057 says so directly (a carry ~17× looser than its
own model claims).

**Consequence for §5's verdict:** "gauge-free buys the tail and pays in the body"
should be re-read as "gauge-free is a much WEAKER anchor that happens to be
gauge-clean".  The fix — stash the PROJECTED posterior for clocks under that
fold, so the carried variance is the contrast's — is named here and deliberately
NOT built in this leg (a second unmeasured mechanism inside the first).  Until
it exists, the gauge-free fold is not a like-for-like arm at any q.

**Wire-mark tie-out (gate 5b): 100 % on every arm.**  event ↔ counter ↔ R record
agree exactly — 65/65, 12/12, 109/109, 3/3 — via `scripts/q3_mark_tieout.py`.
(That script now looks in `<run>/logs` as well as `<run>`: the arm runner
harvests traces there, and an events file read from the wrong place reports a
silent guard, which is the same false negative the card's trap #1 is about.)

### §8.7 Rung 0 — the derived floors are a MEASURED no-op at the top rung

`cta4_q3e5` (q = 3e-5, var floor 1.0e-5, σ floor 0.0018) came out
**byte-identical to `cta4_bis_raw_proj`** (same q, shipped floors 4.0e-4 / 0.02).
Expected — at this rung q·Δt = 9.0e-4 ≫ 1.0e-5 and mean √S = 8.2 cm ≫ 1.8 mm, so
neither floor is ever selected — but proving it costs one arm and buys the
chain's key property: **the descent's rungs differ from rung 0, and from each
other, in `q` ALONE.**

### §8.8 The descent — every PRODUCER gate improves monotonically to 1e-7; the boundary is in the CONSUMER

Raw fold, `proj` denominator, derived floors, k = 6 held.  Rungs differ in `q`
ALONE (§8.7).  `cta3_ctl` is the anchor-OFF control on the same chain.

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

**1. The producer side has NO boundary down to 1e-7.**  Every wire statistic
improves monotonically: dev median 5.98 → **1.41 cm** (4.2×), ex-pivot ceiling
13.81 → **4.45**, floor 5.66 → **0.56**.  The gauge channel drains as it should
and is not a grade.

**2. The tension went where it can be SEEN, and it is fully attributed.**  The
displaced misfit lands in exactly two channels, both monotone in q:

- the **satellite code-bias carry**, vf 9.98 → **13.04** (+31 %);
- the **code residuals**, σ̂₀ 4.046 → **4.212** (+4.1 %).

Neither moved at 3e-5 (§8 earlier: cb flat to 2 %) because the prior was not yet
fighting anything.  From 3e-6 down it binds — `clk` vf = **0.99 at 3e-6**, dead
calibrated — and from there each rung buys wire quality with code-bias tension.
**This is the design principle's intended outcome, measured**: the error did not
vanish and was not absorbed into an inflated q; it moved into the most
diagnosable carry in the system, where the census reads it directly.

**3. ⚠ The variance floor NEVER BINDS — §7.4's "GOVERNING at the bottom rungs"
is structurally unreachable as built.**  At 3e-7, √(q·Δt) = 3.00 mm sits BELOW
the floor's 3.16 mm and the counter still reads 0.  The reason is not the rung:

```
var_eff = prev_var + q·Δt      prev_var ≈ 4.1e-3 m² = (6.4 cm)²
                               floor    = 1.0e-5 m²   ⇒  410× below
```

`prev_var` is the **raw marginal** posterior of the clock, which is
gauge-dominated and cannot approach a 3 mm floor.  **The cap is applied to the
wrong quantity** — for it to be deviation #1's consistency cap it must bound the
CONTRAST variance, the same projection §8.2 put in the guard's denominator.  So
these rungs are governed by the gauge-dominated raw carry, NOT by the floor, and
nothing in this leg measures a floor-governed posture.  Second structural defect
of the leg, same root as the first: **a per-satellite clock quantity read on the
raw state instead of the estimable contrast.**  Named, not built (a third
mechanism inside the ladder would break the one-knob rule).

Corroborating: the ex-pivot floor tracks √(q·Δt) at ratio 1.06 → 1.91 → 2.57 →
3.24 — it descends, but progressively slower, and at the bottom rung it is still
0.56 cm against a 0.25 cm measurement floor and a 0.094 cm physical walk.

**4. ★ The boundary is REAL, is at rung 3, and is NOT in the producer — a better
clock ARMED a fail-unsafe consumer.**  The consumer's worst 3D error goes
10.72 → **34.91 m** with 4 epochs > 20 m, while its BODY improves sharply
(settled median 6.12 → **4.17 m**, p95 9.65 → **7.71**).  The cause is exact:

| arm | min consumer AR position-σ | epochs ≤ 0.050 m | epochs with a FIX |
|---|---|---|---|
| ctl | 0.0750 m | 0 | 0 |
| 3e-5 | 0.0710 | 0 | 0 |
| 3e-6 | 0.0550 | 0 | 0 |
| 3e-7 | 0.0580 | 0 | 0 |
| **1e-7** | **0.0500** | **3** | **1** |

The consumer's position σ falls monotonically as the producer's clock tightens,
and **rung 3 is the first arm ever to reach its AR arming threshold**.  The trace
at TOW 265800: `ar_success=1, num_fixed=5, ratio = inf, gate = fixed`, error
4.64 → **34.91 m**, vertical to −33 m, the filter's own σ blown to 3.138 m, and a
six-epoch decay back to 4.68 m.  **`ratio = inf` is a degenerate ratio test, not
a confident fix** — the second-best candidate had no separation.  No clock-anchor
trip occurs within 40 minutes either side (trips at 262950, 267750, 269310).

So the causal chain is: **better clock → smaller consumer position σ → crosses
the AR arming threshold → fail-unsafe ratio test → 5 wrong integers → 35 m.**
It reproduces `project_pppar_user_control`'s "FIXES ARE WRONG; fail-unsafe" and
`project_closedloop_consumer_vertical_bug` (the vertical is where it lands) from
a completely independent direction, and it is exactly what
`project_ar_validation_criterion` predicts an ADOP/ratio gate does where an FFR
gate belongs.

**Verdict under the pre-registered stop rule.**  Rung 3 degrades the consumer, so
**nothing below 3e-7 may be called validated**.  But the attribution matters more
than the verdict: the regression is a NAMED, PRE-EXISTING, DOWNSTREAM defect that
a better product UNMASKED — not the anchor mis-modelling the clock.  The producer
gates all improve at rung 3.  **The consumer's AR gate is now a blocker for the
bottom of this ladder**, and fixing it is a consumer-side card.

**Rung 2 (q = 3e-7) is the last rung that is unchanged-or-better on every
pre-registered gate**, and it is the best arm measured on this lineage for both
the wire (dev median 1.65 cm vs the control's 5.98) and the consumer
(6.12 / 9.65 / 10.72 vs 8.01 / 13.21 / 13.82 — 24 % / 27 % / 22 % better).
It still costs +2.0 % σ̂₀_code and +27 % cb tension.  **This is a MEASUREMENT, not
a promotion**: G4 remains the invariant-#1 blocker and the loader defaults stay
OFF.

### §8.9 k re-derived per rung, on the UN-TRUNCATED null — and the 2 m rail is INERT

`cta4_q3e5_kcal` / `cta4_q1e7_kcal`: the graded rungs with `clock_anchor_k: 1e9`
and `clock_anchor_abs_m: 0`, so no satellite is ever decoupled by the statistic
and the |w| population is the un-truncated null §3 asks for.  (Staleness `gap`
trips still fire — they are not statistical, and disarming them would hide a
real defect: 2 in each arm, as in every other.)

**Truncation is real but modest.**  Armed vs disarmed at rung 0: |w| ≥ 4 reads
0.605 % vs **1.02 %**, ≥ 6 reads 0.0745 % vs **0.139 %** — the armed ladder
under-reports its own upper tail by 1.7–4×.  At rung 3 the two agree to within
one row (only 3 trips there to truncate with).

| un-truncated |w| exceedance | ≥1 | ≥2 | ≥3 | ≥4 | ≥6 | ≥8 |
|---|---|---|---|---|---|---|
| rung 0 (q = 3e-5) | 21.7 % | 6.11 % | 2.43 % | 1.02 % | 0.139 % | 0.037 % |
| rung 3 (q = 1e-7) | 6.88 % | 0.362 % | 0.046 % | 0.028 % | 0.0185 % | 0.0185 % |

**Both halves of the leg's own expectation come out wrong, in opposite
directions.**  The expectation was "the denominator shrinks with q, so a fixed k
tightens in metres".  Measured:

- **The denominator barely moves.**  q spans **300×** (√(q·Δt) spans 17.3×) but
  mean √S spans **1.28×** — 81.9 → 63.8 mm.  S is tracking the gauge-dominated
  `prev_var`, not q (§8.8 defect 3).  So a fixed k = 6 is 0.491 → 0.383 m across
  the whole ladder: it tightens by 28 %, not by 17×.
- **The RESIDUALS tighten far faster than the denominator**, so a fixed k gets
  progressively MORE conservative, not less: its exceedance falls 0.139 % →
  0.0185 %, a 7.5× rarer trigger, and guard σ̂₀ falls 1.256 → 0.536.

**Rate-matched k, if |w| is to mean the same thing at every rung** (§3's
calibration purpose, never a target rate).  Log-interpolating each rung's
un-truncated ladder to a fixed exceedance:

| target exceedance | rung 0 (q = 3e-5) | rung 3 (q = 1e-7) |
|---|---|---|
| 0.5 % | k = 4.72 → 0.384 m | k = 1.89 → 0.121 m |
| 0.1 % | k = **6.50** → 0.530 m | k = **2.63** → **0.168 m** |

That is the honest per-rung derivation: a **3.2× tightening in metres**, not the
17× the process model would suggest, and the shipped k = 6 happens to sit almost
exactly on the 0.1 % rate at rung 0 while being ~2.5× too conservative by rung 3.
**Not re-armed in this leg** — changing k mid-ladder would make the rungs more
than one knob apart, which is exactly the discipline that made §8.8 readable.

**⚠ The 2.0 m absolute backstop is INERT at every rung under the corrected
statistic, and gets more so as q tightens.**  Un-truncated |ê| exceedance:

| | ≥ 0.3 m | ≥ 1 m | ≥ 2 m |
|---|---|---|---|
| rung 0 | 0.565 % | 0.0278 % | 0.0185 % (2 rows / 3 h) |
| rung 3 | 0.0556 % | 0.00927 % | **0 % (empty)** |

The `nis` test at k = 6 is a 0.38–0.49 m bar, so it is 4–5× MORE sensitive than
the backstop everywhere; the backstop never becomes "the meaningful rail" the
descent anticipated.  For it to be a rail rather than a formality it would have
to sit at **~0.3–0.5 m**, the same stringency as k·√S.  Left at 2.0 m here
deliberately (one knob per rung), and recorded as the third face of the same
root cause: **√S is computed from a raw-state prior variance, so it does not
shrink with q, so nothing downstream of it scales the way the process model
says it should.**

### §8.10 The replay instrument — the boundary is a CLIFF, and both consumers are sitting on it

`scripts/q3_make_replay.py` + `q3_ladder_queue2.ps1`: every graded arm's emitted
stream replayed through the held-out consumer (12 s per arm — the producer never
runs).  This is the SECOND consumer instrument the gate set asks for, and it
matters here because it reads the wire's own discontinuity marks rather than the
in-process counters.

Settled (hour ≥ 1) 3D error, median / p95 / worst, metres:

| arm | LIVE | > 20 m | fix | REPLAY | > 20 m | fix |
|---|---|---|---|---|---|---|
| ctl | 8.01 / 13.39 / 13.82 | 0 | 0 | 8.24 / 14.87 / 17.07 | 0 | 0 |
| 3e-5 | 7.84 / 11.68 / 12.77 | 0 | 0 | 6.86 / 8.67 / 8.79 | 0 | 0 |
| 3e-6 | 6.24 / 12.03 / 12.57 | 0 | 0 | **6.33 / 7.70 / 7.84** | 0 | 0 |
| 3e-7 | 6.12 / 9.78 / 10.72 | 0 | 0 | 7.92 / 10.43 / 11.29 | 0 | 0 |
| **1e-7** | 4.17 / 7.72 / **34.91** | **4** | **1** | **5.52 / 7.30 / 7.50** | **0** | **0** |
| gf (post ≡ proj) | 7.44 / 9.15 / 9.46 | 0 | 0 | 5.46 / 6.99 / 7.70 | 0 | 0 |
| gf + raw | 5.61 / 7.16 / 8.69 | 0 | 0 | 4.90 / 6.35 / 7.60 | 0 | 0 |

**The rung-3 excursion does NOT appear in the replay** — and the reason is a
14 % margin, not a different mechanism:

| | min consumer AR position-σ | epochs ≤ 0.050 m | fixes |
|---|---|---|---|
| ctl replay | 0.0750 m | 0 | 0 |
| 3e-7 replay | 0.0590 | 0 | 0 |
| 1e-7 **replay** | **0.0570** | 0 | 0 |
| 1e-7 **live** | **0.0500** | 3 | 1 |

Both instruments show the SAME monotone fall in the consumer's own σ as the
producer's clock tightens, and they agree on the cause.  They differ only in
which side of the **0.050 m arming threshold** they land on — the live path hits
it exactly.  **So the boundary is a CLIFF the ladder has walked the consumer up
to, not an artefact of one harness.**  That is a stronger conclusion than either
instrument alone: at rung 3 a 14 % difference in the consumer's covariance
decides between a 7.5 m and a 34.9 m worst case, and any further clock
improvement pushes BOTH instruments over.

**Verdict, final for this leg.**  Rung 2 (q = 3e-7) remains the last rung
unchanged-or-better on the pre-registered set.  Below it the producer keeps
improving and the CONSUMER's fail-unsafe AR gate is the binding constraint —
`ratio = inf` accepted as a fix is the defect, and it must be fixed
(FFR-style gate, per `project_ar_validation_criterion`) before any q below 3e-7
is used end-to-end.

**⚠ Do not over-rank the rungs on this statistic.**  The replay ladder is NOT
monotone (3e-7 replay is worse than 3e-6 replay, 7.92 vs 6.33 median) and the
live and replay orderings disagree for several arms.  One station over two
settled hours does not resolve ~1.5 m differences in a 3D median; the consumer
gate here has the power to catch a 35 m cliff and to confirm a direction, not to
rank neighbouring rungs.  The wire census (11 280 rows, 32 satellites) is the
statistic with resolving power, and it IS monotone.

## §8.11 The consumer AR blunder — DIAGNOSED and FIXED, and the first two diagnoses were wrong

Binary `gnss_processor_ffr2.exe` md5 `1760ABD54E83D1D68BFF6EB8804F0A10`; arms
`runs/cta4_q1e7_{ffr,ffr2}`; suite **1345 pass / 1 skip** (+10 new).

### §8.11.1 What `ratio = inf` actually is — §8.8's reading was WRONG

§8.8 called it "a degenerate ratio test, not a confident fix".  It is neither.
`AmbiguityResolver.cpp` sets

```cpp
result.ratio = std::numeric_limits<double>::infinity();  // sentinel: no LAMBDA ratio test ran
```

on the **WL-only** path, which the closed-loop consumer runs (`AR mode 1,
wl_only on`).  That path asserts MW-derived DD widelane integers directly as
Kalman pseudo-observations at `wl_only_pseudo_var = 1e-4 cyc²` (σ ≈ 0.01 cycle).
There is no LAMBDA search, so **there was never a ratio test to be degenerate**.

An `+inf` in a confidence column reads as a spectacular ratio.  That is how the
blunder passed review — including mine, twice.  The column is now honest:
`p_succ` is emitted alongside, and `ar_gate` reports `wl_only` rather than
`fixed`.

### §8.11.2 The FFR gate — necessary, correct, and it does NOT catch this

The path's only acceptance gate was `wl_max_frac`, a POINT test on each arc's
running residual.  The LAMBDA tier's bootstrap FFR gate
(`min_success_rate = 0.999`) never ran here — the standing decision that "fixed
failure rate is the acceptance gate at BOTH tiers" was simply not implemented on
this path.  Built:

- `MwSlipDetector::getResolvedWlQuality()` — per-arc σ of the WL arc mean **and
  its sample count** (a 2-sample arc whose samples agree reports σ ≈ 0, i.e.
  success ≈ 1 on one degree of freedom);
- the product taken over **DISTINCT ARCS**, not DD constraints — many DDs share
  the pivot arc, so a wrong pivot makes all of them wrong together and a per-DD
  product would count the shared failure once per DD;
- the identical `erf(1/(2√2·σ))` term the LAMBDA tier uses (pinned by a unit
  test against `ils::computeBootstrapSuccessRate`) so both tiers report ONE
  comparable number;
- **fail-safe**: a missing entry, an immature arc or a non-finite σ contributes
  **0**, never 1 — "cannot bound this" must not read as "fine";
- backward elimination of the weakest arc, mirroring `selectFixableSubset`.

**Armed, it changed nothing: `p_succ = 1.000000` and the 35 m excursion stood.**

That is not a defect in the gate — it is the gate being *right* about the wrong
question, and it is the finding of this section:

> **A bootstrap success rate bounds the RANDOM failure of rounding an arc.  It
> is computed from that arc's own dispersion, and nothing in an arc's dispersion
> knows which FRAME the filter is in.  It is therefore structurally blind to a
> SYSTEMATIC mis-framing.**  Every contributing arc was mature with σ → 0, so
> `p_succ = 1.0` was the correct answer: those widelanes are precisely
> determined.  They were also in the wrong frame.

### §8.11.3 The mechanism, by elimination

| candidate | ruled out by |
|---|---|
| degenerate ratio test | the sentinel above |
| low-confidence integers | `p_succ = 1.000000`, all arcs mature |
| ambiguity covariance inflation (`kAmbResetVar`) | `previous_dd_wl_` is necessarily EMPTY at the first epoch the σ-gate admits, so no reset can fire |
| `holdAmbiguities` | returns early unless `ar_mode == 3`; the consumer is mode 1 |

What remains fits every observation: the MW widelane lives in the **.BIA frame**
while a network-bias consumer's ambiguities live in the **consumed phase-bias
frame** — the exact conflict `setArIgnoreMwWl` exists to avoid.  Asserting
across that gap at σ = 0.01 cycle dragged the position 33 m in one epoch; the
next epoch's observations no longer fit, were rejected, and the covariance grew
on process noise alone (σ 0.050 → 3.138 m).  **The covariance INCREASING across
a "fix" is the tell** — an information-adding update cannot do that, so the
growth is a consequence, not the cause.

### §8.11.4 The gate that does catch it — the innovation screen

`wl_only_max_innov_cyc` (default **0.5 cycle**): the direct analogue of the
LAMBDA tier's per-entry `nl_max_per_pair_frac`, which this path had no
equivalent of.  A constraint whose asserted integer disagrees with the predicted
DD widelane by more than half a widelane is **no longer a rounding question**,
and is dropped rather than allowed to drag the state across a frame gap.

| q = 1e-7, settled | med | p95 | **worst** | > 20 m | fixes |
|---|---|---|---|---|---|
| no guards (`cta4_q1e7`) | 4.17 | 7.72 | **34.91** | 4 | 1 (`ratio=inf`) |
| + FFR only (`_ffr`) | 4.17 | 7.72 | **34.91** | 4 | 1 (`p_succ=1.0`) |
| **+ FFR + innovation (`_ffr2`)** | 4.84 | **7.65** | **7.75** | **0** | **0** |

`products.txt` is **byte-identical** across all three — the blast radius is
grading-only on the network path, which is what makes these three arms a clean
one-knob A/B of the CONSUMER alone.  The replay instrument is unchanged
(5.52 / 7.30 / 7.50, 0 fixes): its σ never reached the arming threshold, so the
guards are correctly inert where there is nothing to guard.

### §8.11.5 ⇒ §8.8's boundary is DISSOLVED

With the consumer defect fixed, rung 3 is **better than rung 2 on every consumer
statistic** (4.84 / 7.65 / 7.75 vs 6.12 / 9.78 / 10.72), so the "boundary at
rung 3" verdict in §8.8 no longer stands — it was a consumer defect the ladder
exposed, exactly as §8.8 attributed it, and removing the defect removes the
boundary.

**What does NOT change**: the producer-side cost is unaffected (products are
byte-identical), so σ̂₀_code still rises 4.126 → 4.212 and cb's variance factor
12.66 → 13.04 from rung 2 to rung 3.  The descent still buys wire quality with
code-bias tension; only the consumer's objection is gone.  **Still not a
promotion** — G4 remains the invariant-#1 blocker.

### §8.11.6 Remaining gaps, named not hidden

1. **`applyMwWlConstraintsNlWl_` has BOTH holes** (no FFR gate, no innovation
   screen).  Unreachable in the `wl_only` posture, so this consumer is covered;
   a `wl_only: false` user of the NL/WL path is not.  Marked in code.
2. **`wl_only on` for a network-bias consumer may be the wrong posture
   entirely.**  `setArIgnoreMwWl`'s own comment says this harness should
   SUPPRESS the MW feed so AR runs pure MLAMBDA on the consumed-bias float; the
   golden63 chain does the opposite.  These guards make that posture SAFE; they
   do not make it RIGHT, and the config question is open.
3. The consumer's `ambiguity_resolver` logger does not route to
   `logs/ambiguity_resolver.log` even with `enable_ambiguity_resolver: true`
   (the derived consumer config builds its own resolver).  The diagnosis above
   had to be made from the trace and by elimination; that should be fixed before
   the next consumer-side investigation.
