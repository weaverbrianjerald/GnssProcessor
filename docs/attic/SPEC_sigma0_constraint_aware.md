# SPEC — constraint-aware global variance component (the honest σ̂₀²)

**Status**: **IMPLEMENTED 2026-08-04.** Default-OFF, solve byte-identical, 18 unit
cases green (6 new), gate 1 + live wiring PASS on the FT63 raim-live smoke. See
§10 for the build record and the deviations from this spec — including the one the
smoke falsified (§10.2.1: the card's headline is **not measurable on the FT63
census lineage at all**, and the reason is structural).
**Origin**: NGS LASER Guide v2.7 slide 5-74 (PDF p. 386):
`σ̂₀² = (ẽᵀPẽ + ẽ₀ᵀP₀ẽ₀)/(n − m + l)` — prior residuals in the numerator, constraint
count in the redundancy; slide 5-85 for the minimal-vs-non-minimal distinction.
**In-tree precedent**: `KalmanFilter::computeResidualQc_`
(`src/core/filter/KalmanFilter.cpp:1736-1791`) already computes exactly this shape on
the PPP path — `variance_factor = wssr / Σrᵢ` with per-row redundancy — the network
path has no equivalent.
**Memory context**: `project_laser_theory_mapping`, `project_m2_lsvce_carryq` (the
per-family σ̂ this global number must triangulate with).
**Scope discipline**: default-OFF, OFF byte-identical, MEASURE-ONLY, fingerprint-SAFE.

---

## 1. Why

The network's within-epoch σ̂₀ is a **robust median of standardized data-row residuals**
(the `assess` lambda, `src/core/filter/NetworkSolveGlue.cpp:1029`). That is the right
statistic for RAIM attribution and it stays untouched. But it is structurally blind to
tension absorbed by the carried priors — the largest constraint population in the solve
by orders of magnitude — which is the "postfit σ̂₀ launders a parked cell through the
free ambiguities" blind spot the census spec names. LASER 5-74 gives the honest global
scale: the prior residual quadratic form joins the numerator and the constraints join
the redundancy.

Slide 5-85's refinement matters here: for a MINIMAL constraint set,
`n − m + l = n − rk A` — the datum pins need no correction and get none. It is the
**non-minimal** stochastic rows — the carry families, `cb_kaula_prior`'s 389 CB rows —
whose omission biases the global test, and those are exactly the rows the census now
walks.

M2's field verdict is the expectation to confirm: hand floors 30–120× optimistic
(robust), with all families spiking together at the hr-4 cliff while the median-σ̂₀
stayed comparatively quiet. This card turns that laundering into one number per epoch.

## 2. Design — three sums, one ratio, nothing acted on

Per epoch:

```
data side    Σ w·v²   and  Σ r_data      (rows in the RAIM pool)
prior side   Σ w·ê²   and  Σ r_prior     (census rows, FOLDED branch ONLY)

σ̂₀²_global = (Σwv² + Σwê²) / (Σr_data + Σr_prior)
```

plus the two components reported separately — the ratio `Σwê² / Σwv²` ("how much of the
epoch's tension lives in the carry") is the headline column.

- **Prior side is free.** The census's INC-0 already forms `(ê, fold_var⁻¹=w, r)` per
  row. Two running sums, FOLDED rows only — the not-folded (RESPONDED/FRAME) rows are
  NOT constraints in the solve and must not enter; this is the census's `folded` bit
  reused, and mixing them in is the same sign error the census spec §4 documents.
- **Data side rides RAIM.** With `raim_within_epoch` + `raim_redun`, per-row Baarda
  redundancy already exists (B2, `src/core/filter/NetworkRaim.cpp:128-147`) — accumulate
  `w·v²` and `r` in the same loop that standardizes `v/(σ·√r)`. Without `raim_redun`
  there is no honest `Σr_data`: report the column with `r ≡ 1` and a per-epoch
  `approx` flag set (mirroring the census's raw-formal warm-up convention: degraded
  output SAYS SO in-band, never silently).
- **Nothing consumes it.** The A4 `raim_withhold_sigma0` guard, all gates, and all
  deweights stay on the median σ̂₀. v1 wires zero decisions to the new quantity.

Outputs: two `network_epoch_status.csv` columns (`sigma0_global`, `prior_tension_ratio`
+ the approx flag), run-end summary in `NetworkTeardown` (median/p95 of both, split
data/prior contributions per family), counters `s0g_*` in `NetworkServiceCounters`.

## 3. Cost

O(1) per row inside two loops that already run. Budget: unmeasurable; no covariance
access, no sweep, no new pass.

## 4. Config

| knob | type | default | note |
|---|---|---|---|
| `network.prior_residual_global_sigma0` | bool | `false` | requires `prior_residual_census` (hard error otherwise, census-style) |

Loader: next to the census block in `loadNetworkCarrySection_`. WARN when armed without
`raim_redun` (data side degrades to r≡1, flagged). Fingerprint-SAFE — legal on a warm
resume.

## 5. Unit tests

Extend `tests/test_prior_residual_census.cpp`'s suite (same file — the accumulator is
the census's; suite-per-file ownership preserved):

1. **The 5-74 identity.** Hand-built scalar net with one data row and one stochastic
   constraint: `σ̂₀²_global` equals LASER's `(ẽᵀPẽ + ẽ₀ᵀP₀ẽ₀)/(n−m+l)` to 1e-12.
2. **The 5-85 equivalence.** Same net with the constraint's variance → 0 (a minimal
   pin): global form converges to the fixed-constraint `ẽᵀPẽ/(n − rk A)` — pins need no
   correction, and the test proves the two denominators coincide for a minimal set.
3. **Label discipline.** A not-folded row moves neither sum.
4. **Approx flag.** r≡1 fallback sets the flag and changes only Σr_data.
5. OFF byte-identity (structural).

## 6. Field validation

Controls: the golden 3 h/305 lineage + the 12 h window containing the hr-4 cliff.

1. OFF byte-identity; live wiring (non-zero sums both sides; invariant #4).
2. **The triangle.** σ̂₀_global vs median-σ̂₀ vs M2's per-family robust σ̂ on the same
   run: global must sit between the median (data-only, robust) and the M2-implied
   union, and the per-family split must reconcile with M2's floors to ~factor-2. A
   failure to reconcile is a bug in one of the three instruments — find which.
3. **The cliff.** Across the known hr-4 event: `prior_tension_ratio` must spike with
   the M2 family spikes while the median-σ̂₀ stays comparatively flat — quantifying the
   laundering. This is the card's reason to exist as a measurement.
4. Cost unmeasurable; solve byte-identical in every arm.

## 7. Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| Any decision wired to σ̂₀_global (withhold, deweight, VCE feedback) | **INERT** — measure-only by construction; successor card needs §6 data | — |
| Exact Σr_data without `raim_redun` | **INERT** — flagged approx path; the armed-set directive includes RAIM anyway | flag is in-band |
| Per-station / per-constellation partition of the global | **INERT** — the family split covers the diagnostic need in v1 | — |

## 8. Standing-invariant pre-flight

1. No stiff prior gains or loses a response — measure-only. 2. Release-only field runs.
3. Deferrals triaged above. 4. `s0g_*` counters + run-end distinguish "ran, quiet" from
"never ran". 5. The statistic is datum-invariant (quadratic forms of residuals).
6. Default-OFF byte-identical; same-build A/B. 7. No coupling change — nothing is
decoupled or re-coupled by a trace column.

## 9. What this card explicitly does NOT do

- It does not replace the median σ̂₀ anywhere a decision is made.
- It does not modify the census, RAIM, or any gate.
- It does not claim the global σ̂₀ is Gaussian-calibrated — it inherits the same
  heavy-tail reality the census ladders measure; it is a SCALE, not a test statistic,
  in v1.

---

## 10. BUILD RECORD (2026-08-04) — what landed, and every deviation from §1–§9

Code: the rider lives entirely inside the census's accumulator
(`include/filter/NetworkPriorResidual.hpp` — `GlobalSums` / `GlobalFlag` /
`GlobalResult` / `computeGlobal` / `addDataRow` / `recordEpochGlobal` + the
per-family split and the per-epoch history), its emission and run-end readout in
`src/core/filter/NetworkPriorResidual.cpp`, its data-side feed in the post-fit
RAIM Pass-1 row walk (`src/core/filter/NetworkRaim.cpp`), one knob in
`loadNetworkCarries_` with three tripwires at the tail of `loadNetworkQcTiers_`,
three armed-only `network_epoch_status.csv` columns behind their OWN schema flag,
`s0g_*` counters reported by `NetworkTeardown`, and 6 new cases (13–18) appended
to `tests/test_prior_residual_census.cpp`. Arms:
`config/test_suite/_ft63_s0g_{raimlive_off,raimlive,approx}.json`.

### 10.1 Measured on the FT63 raim-live smoke (Release, 63 stations, 20 epochs)

| gate | result |
|---|---|
| **OFF byte-identity** | **PASS.** Same-build A/B, distinct cwd, the only config delta being `prior_residual_global_sigma0`. **Every** solve/QC artifact byte-identical: `network_trace`, `network_solution`, `network_raim`, `network_raim_attrib`, `network_carry_vce`, `network_dd_fractional`, `network_pf_keyed`, `network_vtec_sh*`, `network_slip_raw`, `network_if_slip`, `network_mdb`, `network_flag_rates`, `network_events`, `network_stations`, `products.txt` — **and `network_prior_residual.csv`**, which is the property the census's own gates depend on (the rider may not perturb one ladder count; unit case 17 asserts the same thing). Diffs confined to `network_epoch_status.csv` (= OFF **exactly** once the 3 appended columns are trimmed, 57 → 60) and the wall-clock fields of `network_run_summary.csv` / `network_service_health.csv`. |
| **fingerprint SAFE** | **CONFIRMED, not asserted.** The two arms' `state_snapshot.bin` differ in **exactly 2 bytes** — the ISO wall-clock stamp — and the resume fingerprint is identical (`5d3de43ce235d931`). A warm resume may cross arms. |
| **live wiring** (invariant #4) | **PASS on the raim-live lineage, and the inventory is itself a finding:** 20 300 data rows on the RAIM side and 24 932 folded prior rows on the census side, split `pl` 18 872 / `pb` 2 557 / `cb` 2 484 / `rxcb` 1 019. **This is the first run in which `pl` — the largest carry population by two orders of magnitude, and the census's own single largest open gap (its §13.3) — contributes to a measured statistic.** |
| **cost** | **PASS: unmeasurable, as budgeted.** Repeat-measured (2 reps/arm, interleaved): OFF 3173 / 2439 ms/epoch (mean 2806), ON 2696 / 2839 (mean 2767) — **ON is marginally FASTER on the mean**, i.e. the rider sits entirely under a ±25 % run-to-run noise floor. Arithmetic bound for the ~45 k O(1) row operations is ~0.1 ms/epoch. No stride knob, as specified. |
| **the triangle** (§6.2), **the cliff** (§6.3) | **Not graded** (cold 20-epoch smoke). §6's controls and the census's convergence hygiene both apply: during ignition a free ambiguity absorbs its own arc's misfit exactly, and the census σ̂₀ has not cleared its 200-row warm-up. |

First reads, explicitly **not** gate results. Run-pooled
σ̂₀²_global = (5.426e4 + 3.035e6)/(1.337e3 + 9.906e3) = 274.8 ⇒ **σ̂₀_global =
16.58**, with **prior tension Σwê²/Σwv² = 55.9** — i.e. on this cold window
**98 % of the weighted misfit sits in the carry, not in the data**, and the
robust median σ̂₀ that every gate runs on cannot see any of it. The per-family
split reads `pl` 341.4, `pb` 22.0, `cb` 8.9, `rxcb` 7.8 as
`Σwê²/Σr` variance factors. Convergence hygiene forbids quoting these; what they
DO establish is that the instrument resolves four families across 1.7 decades
rather than returning one undifferentiated number.

### 10.2 Deviations from the spec, and why

**10.2.1 ★ THE DATA SIDE IS STRUCTURALLY DEAD ON THE FT63 CENSUS LINEAGE — the
first smoke falsified the arm plan, not the code.** §2 says "data side rides
RAIM", and the census's own arms carry `network.raim: true`, so the first arm was
built on `_ft63_prires_smoke.json`. It produced **0 data rows over 11 epochs**.
Cause: the driver brackets `networkPostFitRaim` with `raim_on && carry_warm`, and
`carry_warm = phase_leveling && warmup` (`ApplicationNetwork.cpp:1318`) — so on a
lineage that runs `carry_invariant` WITHOUT `phase_leveling`, the post-fit pass is
never called, `network.raim` reads ARMED in every log line, and
`prior_tension_ratio` — **the card's headline** — is undefined for the whole run.
This is precisely the live-counter-over-dead-wiring failure invariant #4 exists
for, and the in-band `NO_DATA` flag caught it on the first run. Three
consequences, all landed:

1. A **load-time WARN** on `prior_residual_global_sigma0 && !phase_leveling`,
   naming the bracket, the file:line, the exact degradation, and the arm to use
   instead. Load time is where this is still a cheap fix; run time is one run too
   late.
2. The graded arms moved to `_ft63_mdb_raimlive.json` — the MDB card's own answer
   to the same problem (63 sta, GPS triple, `phase_leveling` + 2-epoch warmup,
   `raim_redundancy_normalize` on). Both sides are live there, and `pl` with it.
3. The run-end line **refuses to print `nan`**: with no tension samples it WARNs
   that the headline is unmeasured, says it is a property of the LINEAGE and not
   of the data, and names the fix.

**10.2.2 Three exclusions on the prior side, not one.** §2 specifies FOLDED-only.
Two more are needed, and each is counted and flagged rather than silent, because a
row admitted to the NUMERATOR but not the DENOMINATOR biases the ratio with
nothing saying so: `var_nominal` rows (a fold variance the mechanism never used ⇒
`w = 1/fold_var` is a fiction; this is the census's `phase_hold` population), and
`r_negative` rows (`post_var > fold_var` — a (prior, posterior) pair that does not
describe one fold, whose `w·ê²` is real and whose redundancy is not). Measured on
the raim-live smoke: 18 of 18 872 `pl` rows (0.1 %) hit `r_negative`, 192 rows
across `pb`/`rxcb` were unfolded, 0 nominal. The merely **r-FLOORED** row is
deliberately KEPT: `E[w·ê²] = r`, so a vanishing-`r` row contributes ~0 to BOTH
sums and cannot bias the ratio — the reason the `kRedundancyFloor` clamp that
dominates `|t|` barely matters here.

**10.2.3 The flag is a BITMASK, not a bool.** §2 asks for "a per-epoch `approx`
flag". Five states are distinguishable and they demand different readings:
`APPROX` (1, r ≡ 1 — denominator too large, σ̂₀_global reads LOW, a KNOWN
direction), `NO_DATA` (2, prior-only), `NO_PRIOR` (4, data-only — exactly the
quantity the card corrects), `DEGENERATE` (8, Σr ≤ 0 ⇒ the value column is BLANK,
never 0) and `RNEG_DROP` (16). Collapsing them to one bool would have made
10.2.1's finding read as an ordinary approximation. Observed on the raim-live arm:
epoch 0 = 14 (`NO_DATA|NO_PRIOR|DEGENERATE`, both values blank), epoch 1 = 2
(`NO_DATA`, σ̂₀ present, tension blank), 0 thereafter.

**10.2.4 The epoch-status columns get their OWN schema flag**
(`es_s0g_columns_`), not the census's. The census's §9.1 gate is a same-build A/B
of `network_epoch_status.csv`, so a census-only run has to keep byte-for-byte the
schema it had before this rider existed.

**10.2.5 The data-side feed carries its own visited mask**, not the MDB rider's.
Both exist because the RAIM walk is keyed on AMBIGUITY COLUMNS rather than rows
(a row touched by two carried ambiguities would be counted twice, inflating both
sums and leaving the ratio plausible), but MDB requires `raim_redun` to exist at
all while this rider degrades to r ≡ 1, so the two populations are different sets
and one shared mask would silently hand whichever card ran second a subset. The
feed sits **ahead** of the uncheckable-row `continue`, from the arithmetic rather
than from the publication argument MDB uses: `E[w·v²] = r_i`, so an r ≈ 0 row
contributes ~0 to both sums, whereas dropping it would delete a real row from
Σr_data and report the pool as more redundant than it is.

**10.2.6 Two statistics are reported at run end, and the line says which is
which**: the run-POOLED variance component (Σ over every row of the run — the
estimator, and it accumulates for epochs that fed rows but were never drained),
and the per-epoch median / p05 / p95 (a median of ratios, over the DRAINED epochs
only — the population a reader of `network_epoch_status.csv` can check it
against). They are different numbers and conflating them is a category error.

**10.2.7 `_ft63_s0g_approx.json` had to move too.** An APPROX arm on the census
lineage exercises the `NO_DATA` path and tests nothing about APPROX, because the
fallback never fires when there are no data rows. It now extends the raim-live arm
with `raim_redundancy_normalize: false` and was verified to degrade **in the
predicted direction**: Σr_data 1337 (redundancies) → 20 351 (a row count),
σ̂₀_global 16.58 → 9.40.

### 10.3 Open, for whoever grades this next

- **§6.2 (the triangle) and §6.3 (the cliff) need a converged warm leg.** Chain
  `_ft63_s0g_raimlive.json` onto a seed snapshot (legal — §10.1 confirms the
  fingerprint is unchanged) and grade only the resumed leg. Nothing in a cold
  20-epoch smoke is quotable for either.
- The **golden 3 h/305 lineage** named in §6 has not been run with this armed, and
  it must be checked against 10.2.1 first: if it does not carry `phase_leveling`,
  the headline is not measurable there either and the control has to change.
- The §6.2 reconciliation against M2's per-family robust σ̂ is now **possible for
  the first time on `pl`** (10.1) and is the highest-value open measurement.

