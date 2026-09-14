# global σ̂₀ rider — `network.prior_residual_global_sigma0` — three sums, one ratio, nothing acted on

**STATUS**

- **id:** global σ̂₀ rider
- **topic:** raim-qc
- **status:** closed
- **summary:** `network.prior_residual_global_sigma0` — three sums, one ratio, nothing acted on
- **dates in the text:** 2026-08-04 → 2026-08-15
- **knobs / posture:** `network.prior_residual_global_sigma0`, `network.raim`, `network.raim_carry_invariant`
- **hashes named in the text** (commit / exe / byte-cert): `5d3de43ce235d931`
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_prior_residual_census.md` — 14, 14.1, 14.2, 14.3, 14.4, 14.5, 14.6, 14.7, 14.8, 14.9, 15, 15.1, 15.2, 15.3

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_prior_residual_census.md -->
## 14. THE GLOBAL σ̂₀ RIDER — `network.prior_residual_global_sigma0`

*Absorbed 2026-08-15 from `docs/SPEC_sigma0_constraint_aware.md` §1–§9 (now attic'd).* The rider lives entirely inside this census's accumulator and consumes the same rows. It answers the complementary question: not *which row is stale* (the ladder) but *how much of this epoch's total weighted misfit lives in the carry at all*.

### 14.1 Why

The network's within-epoch σ̂₀ is a **robust median of standardized data-row residuals** (the `assess` lambda, `src/core/filter/NetworkSolveGlue.cpp:1200`) — the right statistic for RAIM attribution, and it stays untouched. But it is structurally blind to tension absorbed by the carried priors, the largest constraint population in the solve by orders of magnitude — the "postfit σ̂₀ launders a parked cell through the free ambiguities" blind spot the prefit census names. LASER 5-74 gives the honest global scale `σ̂₀² = (ẽᵀPẽ + ẽ₀ᵀP₀ẽ₀)/(n − m + l)` — prior residuals in the numerator, constraint count in the redundancy. Slide 5-85's refinement matters: for a MINIMAL constraint set `n − m + l = n − rk A`, so the datum pins need no correction and get none. It is the **non-minimal** stochastic rows — the carry families, `cb_kaula_prior`'s 389 CB rows — whose omission biases the global test, and those are exactly the rows this census walks.

**In-tree precedent, and the gap it names**: `KalmanFilter::computeResidualQc_` (`src/core/filter/KalmanFilter.cpp:1710+`; `meas.variance_factor` assigned `:1797`) already computes this shape on the PPP path — `variance_factor = wssr / Σrᵢ` with per-row redundancy. **The network path had no equivalent.** (The original spec cited `KalmanFilter.cpp:1736-1791`; that arithmetic has since been re-based onto `include/filter/BaardaReliability.hpp` — §16.)

M2's field verdict is the expectation to confirm: hand floors **30–120× optimistic** (robust), all families spiking together at the hr-4 cliff while the median-σ̂₀ stayed comparatively quiet. This rider turns that laundering into one number per epoch.

### 14.2 Design — three sums, one ratio, nothing acted on

*(Cited from `include/filter/NetworkPriorResidual.hpp:830`, `:880`, `src/core/filter/NetworkRaim.cpp:74` and `src/core/filter/NetworkPriorResidual.cpp:501` — all four re-grepped 2026-08-15 and pointing at §14.2 today.)*

```
data side    Σ w·v²   and  Σ r_data      (rows in the RAIM pool)
prior side   Σ w·ê²   and  Σ r_prior     (census rows, FOLDED branch ONLY)
σ̂₀²_global = (Σwv² + Σwê²) / (Σr_data + Σr_prior)
```

plus the two components reported separately — the ratio `Σwê² / Σwv²` ("how much of the epoch's tension lives in the carry") is the **headline column**.

- **Prior side is free.** INC-0 already forms `(ê, fold_var⁻¹ = w, r)` per row. Two running sums, FOLDED rows only — the not-folded (RESPONDED/FRAME) rows are NOT constraints in the solve and must not enter; mixing them in is the same sign error §4 documents. (Landed with **three** exclusions, not one — §15.2 R-2.)
- **Data side rides RAIM.** With `raim_within_epoch` + `raim_redun`, per-row Baarda redundancy already exists (B2, `NetworkRaim.cpp:178-184` forms `row_r`, `:267` standardizes `v/(σ·√r_i)`) — accumulate `w·v²` and `r` in the same loop (`prires.addDataRow`, `:240`; arming bit `s0g_redun_here`, `:209`). Without `raim_redun` there is no honest `Σr_data`: report the column with `r ≡ 1` and a per-epoch `approx` flag, mirroring the census's raw-formal warm-up convention — degraded output SAYS SO in-band, never silently. ⚠ The pool is the RAIM pool, not the whole system (`NetworkPriorResidual.hpp:886+`, and §15.2 R-1 for what happens when it never runs).
- **Nothing consumes it.** The A4 `raim_withhold_sigma0` guard, all gates and all deweights stay on the median σ̂₀. v1 wires zero decisions to the new quantity.

Outputs: three `network_epoch_status.csv` columns (`sigma0_global`, `prior_tension_ratio` + the flag) behind their **own** schema bit (§15.2 R-4), run-end summary in `NetworkTeardown` (median/p95 of both, split data/prior contributions per family), counters `s0g_*` in `NetworkServiceCounters`.

### 14.3 Cost

O(1) per row inside two loops that already run. Budget: **unmeasurable**; no covariance access, no sweep, no new pass. (Measured: it is — §15.1.)

### 14.4 Config

*(Cited from `src/core/config/GnssConfigLoadNetwork.cpp:3879`, re-grepped 2026-08-15.)*

| knob | type | default | note |
|---|---|---|---|
| `network.prior_residual_global_sigma0` | bool | `false` | requires `prior_residual_census` (**hard error** otherwise, census-style) |

Parsed next to the census block in `loadNetworkCarries_` (`GnssConfigLoadNetwork.cpp:2990-2992`; ARMED info line `:3062-3073`). **The `raim_redundancy_normalize` tripwire cannot live there** — `loadNetworkQcTiers_` runs *after* that helper and has not resolved the flag yet — so it sits at the tail of `loadNetworkQcTiers_` (`:3879+`, next to the MDB precondition), per the section's load-order rule.

**FOUR guards, not three** (corrected 2026-08-15; the earlier count in this section was wrong and omitted a real user-facing WARN) — one hard error in `loadNetworkCarries_`, three WARNs at the tail of `loadNetworkQcTiers_`:

| guard | site | fires when |
|---|---|---|
| hard error | `:3008-3013` | `global_sigma0` without `prior_residual_census` |
| WARN | `:3903` | `!phase_leveling` — the §15.2 R-1 WARN; names the bracket, the degradation, and the arm to use instead |
| WARN | `:3913` | `!network.raim` — post-fit row walk never runs, data side sees zero rows, every epoch carries `NO_DATA` |
| WARN (`else if` on the previous) | `:3920` | `!raim_redundancy_normalize` — data side degrades to `r ≡ 1`, `APPROX` flagged; arming it is SOLVE-AFFECTING, so that A/B is not byte-identical |

**Fingerprint-SAFE** — legal on a warm resume, confirmed by snapshot comparison (§15.1).

### 14.5 Unit tests — cases 13–18

Appended to `tests/test_prior_residual_census.cpp`'s suite (same file — the accumulator is the census's; suite-per-file ownership preserved).

13. **The 5-74 identity.** Hand-built scalar net, one data row and one stochastic constraint: `σ̂₀²_global` equals `(ẽᵀPẽ + ẽ₀ᵀP₀ẽ₀)/(n−m+l)` to 1e-12.
14. **The 5-85 equivalence.** Same net with the constraint's variance → 0 (a minimal pin): the global form converges to the fixed-constraint `ẽᵀPẽ/(n − rk A)` — pins need no correction, and the two denominators coincide for a minimal set.
15. **Label / branch discipline.** A not-folded row moves neither sum; three skips, each counted, never silent.
16. **Approx flag.** The r ≡ 1 fallback sets the flag, changes only `Σr_data`, and degrades in ONE direction.
17. **OFF byte-identity (structural)** — and, ON, it changes no census ladder count.
18. **The per-family split closes**, and the per-epoch history is what the quantiles claim (§15.2 R-6's two statistics).

### 14.6 Field validation

Controls: the golden 3 h/305 lineage + the 12 h window containing the hr-4 cliff. §9's convergence hygiene governs — nothing below is gradable on a cold smoke.

1. OFF byte-identity; live wiring (non-zero sums both sides; invariant #4). **PASS, §15.1.**
2. **★ THE TRIANGLE — still OWED.** σ̂₀_global vs median-σ̂₀ vs M2's per-family robust σ̂ on the same run: global must sit **between** the median (data-only, robust) and the M2-implied union, and the per-family split must reconcile with M2's floors to ~factor-2. Failure to reconcile is a bug in one of the three instruments — find which.
3. **★ THE CLIFF — still OWED.** Across the known hr-4 event: `prior_tension_ratio` must spike with the M2 family spikes while the median-σ̂₀ stays comparatively flat, quantifying the laundering. The rider's reason to exist as a measurement.
4. Cost unmeasurable; solve byte-identical in every arm. **PASS, §15.1.**

### 14.7 Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| Any decision wired to σ̂₀_global (withhold, deweight, VCE feedback) | **INERT** — measure-only by construction; successor card needs §14.6 data | — |
| Exact `Σr_data` without `raim_redun` | **INERT** — flagged approx path; the standard armed set includes RAIM anyway | flag is in-band (`APPROX`, §15.2 R-3) |
| Per-station / per-constellation partition of the global | **INERT** — the family split covers the diagnostic need in v1 | — |

### 14.8 Standing-invariant pre-flight — see §11

Stated once for both halves. All seven hold for the rider; #4 (`s0g_*` counters separate "ran, quiet" from "never ran") is the one R-1 exercised on first contact.

### 14.9 What this rider explicitly does NOT do — see §12, final bullet

---

## 15. BUILD RECORD B — the rider (2026-08-04)

Code: the rider lives entirely inside the census's accumulator (`include/filter/NetworkPriorResidual.hpp` — `GlobalSums` / `GlobalFlag` / `GlobalResult` / `computeGlobal` / `addDataRow` / `recordEpochGlobal` + the per-family split and the per-epoch history), emission and run-end readout in `src/core/filter/NetworkPriorResidual.cpp`, data-side feed in the post-fit RAIM Pass-1 row walk (`src/core/filter/NetworkRaim.cpp`), one knob in `loadNetworkCarries_` with three WARN tripwires at the tail of `loadNetworkQcTiers_` (§14.4), three armed-only `network_epoch_status.csv` columns behind their OWN schema flag, `s0g_*` counters reported by `NetworkTeardown`, and 6 new cases (13–18) appended to `tests/test_prior_residual_census.cpp` (18 green at this build; 21 today). Arms: `config/test_suite/_ft63_s0g_{raimlive_off,raimlive,approx}.json`.

### 15.1 Measured on the FT63 raim-live smoke (Release, 63 stations, 20 epochs)

| gate | result |
|---|---|
| **OFF byte-identity** | **PASS.** Same-build A/B, distinct cwd, the only config delta being `prior_residual_global_sigma0`. **Every** solve/QC artifact byte-identical: `network_trace`, `network_solution`, `network_raim`, `network_raim_attrib`, `network_carry_vce`, `network_dd_fractional`, `network_pf_keyed`, `network_vtec_sh*`, `network_slip_raw`, `network_if_slip`, `network_mdb`, `network_flag_rates`, `network_events`, `network_stations`, `products.txt` — **and `network_prior_residual.csv`**, the property the census's own gates depend on (the rider may not perturb one ladder count; unit case 17 asserts the same). Diffs confined to `network_epoch_status.csv` (= OFF **exactly** once the 3 appended columns are trimmed, 57 → 60) and the wall-clock fields of `network_run_summary.csv` / `network_service_health.csv`. |
| **fingerprint SAFE** | **CONFIRMED, not asserted.** The two arms' `state_snapshot.bin` differ in **exactly 2 bytes** — the ISO wall-clock stamp — and the resume fingerprint is identical (`5d3de43ce235d931`). A warm resume may cross arms. |
| **live wiring** (invariant #4) | **PASS on the raim-live lineage, and the inventory is itself a finding:** 20 300 data rows on the RAIM side and 24 932 folded prior rows on the census side, split `pl` 18 872 / `pb` 2 557 / `cb` 2 484 / `rxcb` 1 019. **★ The first run in which `pl` — the largest carry population by two orders of magnitude, and the census's own single largest open gap (§13.3) — contributes to a measured statistic.** |
| **cost** | **PASS: unmeasurable, as budgeted.** Repeat-measured, 2 reps/arm interleaved: OFF 3173 / 2439 ms/epoch (mean 2806), ON 2696 / 2839 (mean 2767) — **ON marginally FASTER on the mean**, i.e. the rider sits entirely under a ±25 % run-to-run noise floor. Arithmetic bound for the ~45 k O(1) row operations is ~0.1 ms/epoch. No stride knob, as specified. |
| **the triangle** (§14.6.2), **the cliff** (§14.6.3) | **Not graded** (cold 20-epoch smoke). §14.6's controls and §9's convergence hygiene both apply: during ignition a free ambiguity absorbs its own arc's misfit exactly, and the census σ̂₀ has not cleared its 200-row warm-up. |

First reads, explicitly **not** gate results. Run-pooled σ̂₀²_global = (5.426e4 + 3.035e6)/(1.337e3 + 9.906e3) = 274.8 ⇒ **σ̂₀_global = 16.58**, with **prior tension Σwê²/Σwv² = 55.9** — on this cold window **98 % of the weighted misfit sits in the carry, not in the data**, and the robust median σ̂₀ every gate runs on cannot see any of it. The per-family split reads `pl` **341.4**, `pb` **22.0**, `cb` **8.9**, `rxcb` **7.8** as `Σwê²/Σr` variance factors. Convergence hygiene forbids quoting these; what they DO establish is that the instrument resolves four families across 1.7 decades rather than returning one undifferentiated number.

### 15.2 Deviations from §14, and why

*(Renumbered R-1..R-7 on absorption; they were §10.2.1..§10.2.7 in the retired doc, and no code cites those numbers. Old numbers given per entry for grep continuity.)*

**R-1 (was 10.2.1) ★ THE DATA SIDE IS STRUCTURALLY DEAD ON THE FT63 CENSUS LINEAGE — the first smoke FALSIFIED the arm plan, not the code.** §14.2 says "data side rides RAIM", and the census's own arms carry `network.raim: true`, so the first arm was built on `_ft63_prires_smoke.json`. It produced **0 data rows over 11 epochs**. Cause: the driver bracketed `networkPostFitRaim` with `raim_on && carry_warm`, and `carry_warm = phase_leveling && warmup` (then `ApplicationNetwork.cpp:1318`) — so on a lineage running `carry_invariant` WITHOUT `phase_leveling` the post-fit pass is never called, `network.raim` reads ARMED in every log line, and `prior_tension_ratio` — **the rider's headline** — is undefined for the whole run. Precisely the live-counter-over-dead-wiring failure invariant #4 exists for; the in-band `NO_DATA` flag caught it on the first run. Three consequences, all landed: (1) a **load-time WARN** on `prior_residual_global_sigma0 && !phase_leveling`, naming the bracket, the file:line, the exact degradation and the arm to use instead — load time is where this is still a cheap fix; (2) the graded arms moved to `_ft63_mdb_raimlive.json`, the MDB card's own answer to the same problem (63 sta, GPS triple, `phase_leveling` + 2-epoch warmup, `raim_redundancy_normalize` on) where both sides are live and `pl` with them; (3) the run-end line **refuses to print `nan`** — with no tension samples it WARNs that the headline is unmeasured, says it is a property of the LINEAGE and not of the data, and names the fix.

⚠ **Partly relieved since, by a different card — verify before planning an arm.** The RAIM pool re-host (`docs/SPEC_raim_pool_rehost.md`) widened the bracket to `if (raim_on && (carry_warm || ctx.knobs.raim_ci_on))` (`src/core/app/ApplicationNetwork.cpp:2158`; `carry_warm` is now defined at `:1428` and additionally admits the T1d convergence latch — both re-grepped 2026-08-15). A `carry_invariant` lineage with `network.raim_carry_invariant` armed CAN now reach the post-fit pass and feed this rider's data side. **The finding stands as the class of defect** ("reports ARMED and screens NOTHING"), and the load-time WARN at `GnssConfigLoadNetwork.cpp:3903` still keys on `phase_leveling` alone, so it can now fire on a lineage where the data side is in fact live — open, and that WARN's own code comment still cites the retired `:1318` bracket. Nothing here has been re-measured under the re-host: treat the relief as available, not as graded.

**R-2 (was 10.2.2) Three exclusions on the prior side, not one.** §14.2 specifies FOLDED-only. Two more are needed, each counted and flagged rather than silent, because a row admitted to the NUMERATOR but not the DENOMINATOR biases the ratio with nothing saying so: `var_nominal` rows (a fold variance the mechanism never used ⇒ `w = 1/fold_var` is a fiction; the census's `phase_hold` population) and `r_negative` rows (`post_var > fold_var`, whose `w·ê²` is real and whose redundancy is not). Measured on the raim-live smoke: **18 of 18 872 `pl` rows (0.1 %) hit `r_negative`, 192 rows across `pb`/`rxcb` were unfolded, 0 nominal.** The merely **r-FLOORED** row is deliberately KEPT: `E[w·ê²] = r`, so a vanishing-`r` row contributes ~0 to BOTH sums and cannot bias the ratio — the reason the `kRedundancyFloor` clamp that dominates `|t|` barely matters here.

**R-3 (was 10.2.3) The flag is a BITMASK, not a bool.** §14.2 asks for "a per-epoch `approx` flag"; five states demand different readings: `APPROX` (**1**, r ≡ 1 — denominator too large, σ̂₀_global reads LOW, a KNOWN direction), `NO_DATA` (**2**, prior-only), `NO_PRIOR` (**4**, data-only — exactly the quantity the rider corrects), `DEGENERATE` (**8**, Σr ≤ 0 ⇒ the value column is BLANK, never 0) and `RNEG_DROP` (**16**). Collapsing them to one bool would have made R-1's finding read as an ordinary approximation. Observed on the raim-live arm: epoch 0 = **14** (`NO_DATA|NO_PRIOR|DEGENERATE`, both values blank), epoch 1 = **2** (`NO_DATA`, σ̂₀ present, tension blank), **0 thereafter**.

**R-4 (was 10.2.4) The epoch-status columns get their OWN schema flag** (`es_s0g_columns_`), not the census's. The census's §9.1 gate is a same-build A/B of `network_epoch_status.csv`, so a census-only run has to keep byte-for-byte the schema it had before this rider existed.

**R-5 (was 10.2.5) The data-side feed carries its own visited mask**, not the MDB rider's. Both exist because the RAIM walk is keyed on AMBIGUITY COLUMNS rather than rows (a row touched by two carried ambiguities would be counted twice, inflating both sums and leaving the ratio plausible), but MDB requires `raim_redun` to exist at all while this rider degrades to r ≡ 1, so the two populations are different sets and one shared mask would silently hand whichever card ran second a subset. The feed sits **ahead** of the uncheckable-row `continue`, from the arithmetic rather than MDB's publication argument: `E[w·v²] = r_i`, so an r ≈ 0 row contributes ~0 to both sums, whereas dropping it would delete a real row from `Σr_data` and report the pool as more redundant than it is.

**R-6 (was 10.2.6) Two statistics at run end, and the line says which is which**: the run-POOLED variance component (Σ over every row of the run — the estimator, and it accumulates for epochs that fed rows but were never drained), and the per-epoch median / p05 / p95 (a median of ratios, over the DRAINED epochs only — the population a reader of `network_epoch_status.csv` can check it against). Different numbers; conflating them is a category error.

**R-7 (was 10.2.7) `_ft63_s0g_approx.json` had to move too.** An APPROX arm on the census lineage exercises the `NO_DATA` path and tests nothing about APPROX, because the fallback never fires when there are no data rows. It now extends the raim-live arm with `raim_redundancy_normalize: false` and was verified to degrade **in the predicted direction**: `Σr_data` **1337** (redundancies) → **20 351** (a row count), σ̂₀_global **16.58 → 9.40**.

### 15.3 Open, for whoever grades the rider next

- **§14.6.2 (the triangle) and §14.6.3 (the cliff) need a converged warm leg.** Chain `_ft63_s0g_raimlive.json` onto a seed snapshot (legal — §15.1 confirms the fingerprint is unchanged) and grade only the resumed leg.
- The **golden 3 h/305 lineage** named in §14.6 has not been run with this armed, and must be checked against R-1 first: if it does not carry `phase_leveling` (or, since the re-host, `raim_carry_invariant`), the headline is not measurable there either and the control has to change.
- The §14.6.2 reconciliation against M2's per-family robust σ̂ is now **possible for the first time on `pl`** (§15.1) and is **the highest-value open measurement** on either half of this card.

---

