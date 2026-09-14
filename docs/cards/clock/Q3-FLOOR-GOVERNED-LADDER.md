# Q3 ladder — the floor-governed ladder — floors derived not tuned, the guard's denominator bisected, and the descent 3e-5 → 1e-7

**STATUS**

- **id:** Q3 ladder
- **topic:** clock
- **status:** closed
- **summary:** the floor-governed ladder — floors derived not tuned, the guard's denominator bisected, and the descent 3e-5 → 1e-7
- **dates in the text:** 2026-08-10
- **knobs / posture:** `network.clock_anchor_gauge_free_stat`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/cta4_`, `runs/cta3_ctl`, `runs/cta_20ep_ctl`, `runs/cta3_on`
- **source:** `docs/archive/SPEC_clock_temporal_anchor.md` — §8, §8.1, §8.2, §8.3, §8.4, §8.5, §8.6, §8.7, §8.8, §8.9, §8.10

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_clock_temporal_anchor.md -->
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

