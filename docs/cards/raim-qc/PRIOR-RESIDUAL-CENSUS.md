# prires census: the prior-residual census of carried priors (INC-0 to INC-3)

**STATUS**

- **id:** prires census
- **topic:** raim-qc
- **status:** closed
- **summary:** Measure-only per-row screen of carried priors (LASER slide 5-74): built solve byte-identical, smoke gates PASS, power gates 3 to 6 not graded; armed on the goldens as an instrument.
- **dates in the text:** 2026-08-01 to 2026-08-15
- **knobs / posture:** `network.prior_residual_census`, `network.prior_residual_families`, `network.prior_residual_window_epochs`, `network.prior_residual_min_rows`, `network.prior_residual_report_sigma`, `network.prior_residual_corroborate`, `network.prior_residual_event_holdoff_ep`, `network.prior_residual_global_sigma0`, `network.cycle_slip.gap_instrument` (auto-armed by INC-3); census, corroboration and rider are true, families `pl,pb,cb,zwd,rxcb`, in `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json`
- **hashes named in the text** (commit / exe / byte-cert): resume fingerprint `084ba4980fec500b`
- **run dirs named in the text:** none (arms `config/test_suite/_ft63_prires_{off,on,corrob,smoke,smoke_off}.json`, `_agu305_prires_smoke.json`)
- **source:** `docs/archive/SPEC_prior_residual_census.md` preamble, sections 1 to 13 (13.1 to 13.5) and 16; the rider (sections 14 and 15) is [global σ̂₀ rider](../raim-qc/GLOBAL-SIGMA0-RIDER.md), section 17 is [RNEG](../raim-qc/RNEG-REPARTITION.md)

**Outcome.** The question: which carried priors hold a stale value, read per row as LASER's residual of prior information. Gates 1, 2 and 7 PASS at 63 stations, 2 and 7 again at 305 (resume fingerprint unchanged, 0.14 ms/epoch); the first cold read put `cb`/CLEAN at 1.61 % beyond 4σ̂₀ against a Gaussian 0.0063 %, the D5 signature. The INC-3 join saturated at 305 stations; its same-day consensus fix is unproven on satellite-level families. Gates 3 to 6 are not recorded as graded. [RNEG](../raim-qc/RNEG-REPARTITION.md) later moved the r-negative rows, whose |t| the 1e-9 floor inflates, out of the ladders.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_prior_residual_census.md -->
# SPEC — prior-residual census (the stale-carry test, generalized off D5)
## + its constraint-aware global σ̂₀ rider (`network.prior_residual_global_sigma0`)

**Status** — IMPLEMENTED 2026-08-04. Both halves default-OFF, solve **byte-identical**, fingerprint-**SAFE** (snapshot-confirmed, not asserted — §13.1, §15.1), 21 unit cases in one suite. Census INC-0..3: gates 1/2/7 PASS at 63 sta, 2/7 re-PASS at 305; the INC-3 join was found SATURATING on the shared families at 305 and fixed the same day (§13.5) — established for receiver-level families, UNPROVEN for satellite-level ones. Rider: gate 1 + live wiring PASS on the FT63 raim-live smoke, its headline **structurally unmeasurable on a `carry_invariant`-without-leveling lineage** (§15.2 R-1, partly relieved by the RAIM pool re-host — read it before planning an arm). Build records §13 / §15; pointer-drift ledger §16. Build: CLAUDE.md's Build section + `scripts/vs2026_build.ps1`.
**Origin** — NGS LASER Guide v2.7 slide 5-74 (`\\depot\cce_projects\gcap\Software\LASER\`, PDF p. 386): the LESS-within-GMM-with-stochastic-constraints *residual of prior information* and its dispersion (the census, §1) and, on the same slide, the constraint-aware global variance component (the rider, §14.1); slide 5-85 for the minimal-vs-non-minimal distinction. Snow (2024, ch. 6); Snow & Smith (2019) is the adjacent fixed+stochastic extension (slides 5-75/76), not this statistic's source.
**Memory** — `project_m1_card4_d5_innovation` (the binding precedent — read §2 before writing code), `project_m2_lsvce_carryq`, `project_network_phase_leveling` (origin of invariant #1), `project_ifphase_mw_slip_agreement`, `project_laser_theory_mapping`, `project_sigma0_constraint_aware`.
**Scope discipline** — default-OFF, OFF byte-identical, MEASURE-ONLY (no response path).

> **Doc history.** `docs/SPEC_sigma0_constraint_aware.md` ("SPEC — constraint-aware global variance component (the honest σ̂₀²)") was absorbed here 2026-08-15 and moved to `docs/attic/`. Old → new, for anyone chasing a pre-merge citation: §1–§9 → §14.1–§14.9, with §6.2 (the triangle) → §14.6.2 and §6.3 (the cliff) → §14.6.3; §10 → §15, with §10.1 / §10.2 / §10.3 → §15.1 / §15.2 / §15.3 and deviations §10.2.1–§10.2.7 → §15.2 R-1..R-7. **The code sweep has run** (re-grepped 2026-08-15): no citation of the retired name survives in `src/`, `include/`, `tests/` or `config/`, and its five sites now read `docs/SPEC_prior_residual_census.md §14.2` / `§14.4` (they are listed in §14.2 and §14.4). Neither doc cites a `runs/<name>` path — arms are named by their `config/test_suite/*.json` — so the 2026-08-12 relocation to `D:\GnssProcessor_archive\2026-08-12\runs\` costs nothing here.

---

## 1. Why — one statistic for a problem we currently meet with a zoo

Standing invariant #1 — every zero-q / stiff time-linked quantity needs a wired, tested slip/jump/event response before an ON field run — exists because we keep paying for the gaps in field time (phase-leveling zero-q carry; Card-4 invariant DD carry → 10.9 cyc/ep FAIL, Jul 2026). The response layer today is **per-mechanism**: MW / LLI / IF-TD / GF detectors on the observable side, D5 on the DD chords, jump tests on the code bias, `eq34_reref` on the pin displacement — each built after its own field failure. Nothing tells us which of the *remaining* stiff priors is carrying a stale value now.

LASER slide 5-74 gives the general statistic. In the GMM with stochastic constraints `z₀ = Kξ + e₀`, the residual of prior information and its dispersion are

```
ẽ₀ = z₀ − K ξ̂ ,      D̂{ẽ₀} = σ̂₀² P₀⁻¹ (I_l + P₀ K N⁻¹ Kᵀ)⁻¹
```

Every carried prior we fold **is** a stochastic constraint row. Specialize to one scalar constraint — prior variance `p = 1/w`, data-only variance `d = KN⁻¹Kᵀ`, posterior variance `v = pd/(p+d)`:

```
D{ê} = p²/(p+d) = r · fold_var ,     r = 1 − w·v
```

**That is what `NetworkCarryVce::addRow` already forms** from its `(post_var_m2, fold_var_m2)` arguments (`include/filter/NetworkCarryVce.hpp:266` doc-comment, `:297` the arithmetic, `r = 1 − w·post_var`). LASER's dispersion and our partial redundancy are the same object — no new algebra, no new solve, no covariance sweep. The hook exists, is proven solve-neutral, and currently **pools** these numbers into a variance component instead of **testing** them one by one. This card turns the pooling into a per-row screen and measures its power; §14 pools the *same* rows the other way, into 5-74's global variance component.

## 2. THE BINDING PRECEDENT — read this before designing anything

D5 (`project_m1_card4_d5_innovation`) is this exact statistic, already built, for exactly one family (the invariant DD chords). Every lesson below is a *measured* one.

| D5 stage | What was learned |
|---|---|
| v1, raw formal `Cov(v)` | Formal σ is **10–30× optimistic**; the gate needed a hand-tuned `k=10`. |
| v2, LS-VCE calibrated σ̂₀ | Empirical σ̂₀ **auto-discovered** v1's sweet spot (em10 σ̂₀ = 2.49 ⇒ effective k = 10.0). Field PASS, em10 8.33 → 2.35 cyc/ep. |
| the correction, Jul-30 | **D5 is not firing on slips.** 2.1 % of resets show a raw MW/GF step vs **95 %** on the same run's known-discontinuity control. Scale calibration fixes the typical scale but **cannot flatten a non-Gaussian tail**: a 4·σ̂₀ gate still flags ~17 %/epoch where clean 4σ Gaussian gives 0.006 %. |
| v3, corroboration | 97.3 % of trips suppressed by requiring positive raw-observable evidence — matching the independently measured 97.0 % `raw_stepped = 0` rate. |

Three consequences, adopted as hard design rules:

1. **The critical value is EMPIRICAL, never tabular.** LASER's `t(α/2, n−q)` / `F(α,1,r)` assume Gaussian errors; on this data that is measured false by three orders of magnitude in the tail. Report the **empirical exceedance ladder** per family — "α" is a quantile position, not a table lookup. Anyone who reads LASER 5-100/5-102 and reaches for a t-table here rebuilds D5 v1.
2. **Corroboration is measured in cycle ONE, not cycle four.** D5 spent three field iterations before joining flags to raw-observable evidence, and that join revealed the population was persistent model error rather than slips. The contingency table is a v1 deliverable for every family.
3. **The response question is a SEPARATE card.** D5's response (reset-not-repair) was right and its *screen* was wrong for two cycles. Measure-only v1 is the only ordering that does not repeat that.

This card does **not** touch D5. It mirrors D5's statistic onto the families with no screen at all, and makes the families comparable on one axis for the first time.

## 3. Family inventory — who has a screen today

| family | what is folded | monitor | per-row test | this card |
|---|---|---|---|---|
| `dd` | invariant DD chord priors | LS-VCE inflation | **D5** | reference only — statistic mirrored for comparability, **no behavior change** |
| `pl` | phase-leveling ambiguity carry | `NetworkCarryVce` "pl" | **none** | ✅ INC-1 |
| `pb` | satellite phase-bias carry | `NetworkCarryVce` "pb" | **none** | ✅ INC-1 |
| `cb` | satellite code-bias carry | `NetworkCarryVce` "cb" | **none** | ✅ INC-1 |
| `zwd` | ZWD carry (B9) | none | none | ✅ INC-2 (new hook) |
| `rxcb` | receiver code-bias carry | none | none | ✅ INC-2 (new hook) |
| `clk` | satellite-clock carry | none | none | ➕ added LATER by **Card Q3 INC-0**, not by this card — note below |
| — | SH ties, Kaula prior, broadcast anchors, per-LOS iono priors | — | — | ❌ out of scope, §10 |

`pl` is the origin of invariant #1 and the largest population by two orders of magnitude. `cb` is where the DOY-129 pivot-CB ratchet lived (q_cb = 1e-10 ⇒ 49 m step, τ = 3.79 h, `project_day129_pivot_cb_ratchet`): a stiff prior carrying a stale value with no screen on it, which is precisely what this statistic is for.

⚠ **`clk` is a sixth family this spec did not write.** Card Q3 INC-0 added it after this spec landed: `kKnownFamilies` is now `{"pl","pb","cb","zwd","rxcb","clk"}` (`GnssConfigLoadNetwork.cpp:3019`), arming bit `NetworkKnobs::prires_clk` (`include/app/NetworkRunContext.hpp:333`). Not an ordinary census family — its `ê` is COMMON-MODE REMOVED per constellation before standardizing, because the clock is the one carry with a free per-epoch gauge and the raw `ê` would report that gauge on every satellite (invariant #5). Trace-only; design, calibration and gates belong to the Q3 card. Recorded so this inventory does not read as complete when it is not.

## 4. Design

### INC-0: the shared statistic (one definition, one file)

`include/filter/NetworkPriorResidual.hpp` — a leaf accumulator class, header-only, in the `NetworkCarryVce` / `NeqAccumulator` mould (pure numeric, no `ApplicationNetwork` coupling, driven from its unit test). Per row it takes `(family, sat, station, ê, fold_var, post_var, gap_s, folded, label)` and forms the dispersion by the `folded` branch — the one bit INC-2's widened population makes load-bearing:

```
folded (the inject predicate held; the prior entered this epoch's solve):
    D{ê} = r·fold_var = fold_var − post_var           residual of prior information
    r    = clamp(1 − fold_var⁻¹·post_var, [ε, 1])     partial redundancy (LASER 5-74 D̂{ẽ₀})
not folded (RESPONDED refloat / FRAME reref — the inject predicate skipped the row):
    D{ê} = fold_var + post_var                        predicted residual (the D5 form)

s  = ê / √D{ê}                                        formally standardized residual
t  = s / σ̂₀,f                                         EMPIRICALLY standardized residual
```

The sign flip is exact, not a convention: with the fold in, `Cov(z₀, ξ̂) = post_var`, so `D{z₀ − ξ̂} = fold_var + post_var − 2·post_var = fold_var − post_var`, identical to LASER's `r·fold_var` under any network coupling; with the fold out, prior and solve are independent and the variances ADD. Applying the folded form to an unfolded row is anti-conservative by orders of magnitude — a refloated ambiguity has `post_var ≫ fold_var`, `r` slams into the floor, and `t` inflates by √((fold_var + post_var)/(ε·fold_var)): **×3.2e6** for a 1e-4 m² carry against a 1 m² refloat at this card's `kRedundancyFloor = 1e-9`, and ×3000 at `NetworkCarryVce`'s ε = 1e-3 (`NetworkCarryVce.hpp:465`). §13.2(1) corrected the figure; unit test 10 asserts the larger one. Otherwise every RESPONDED row lands in the top ladder bin by construction and §9 gate 3 passes vacuously. The `folded` bit is free at both feed sites — it is the branch the existing skip predicate already takes.

`σ̂₀,f` is the per-family robust inflation, accumulated **exactly as D5 v2 does it**: a `NetworkCarryVce` instance fed the standardized value with unit fold variance, so `Estimate::robust_floor` reads back as σ̂₀². Verbatim precedent, `src/core/filter/NetworkInvariantCarryDriver.cpp:1453-1455`:
`inflation_->addRow(fam, sat, v(i)/√var, /*gap_s=*/0.0, /*post_var=*/0.0, /*fold_var=*/1.0)`.

Ordering contracts inherited from D5 v2, all load-bearing. **Predict-then-update**: standardize against the *through-previous-epoch* σ̂₀ and feed this epoch's rows after, or a row calibrates against itself. **Never censor the calibration input**: every row feeds σ̂₀ whether or not it trips the report threshold — censoring biases σ̂₀ low, and under misspecification the flags ARE the calibration signal. **σ̂₀ warm-up**: below `prior_residual_min_rows` (default 200, D5's value) the family reports `t = s` (raw formal) and **says so in the output column**, rather than silently standardizing by a seed.

### INC-1: the exceedance ladder (the actual deliverable)

Per family per epoch, counts of `|t|` exceeding a fixed ladder `1, 2, 3, 4, 6, 8, 12, 20, 50, 100, 200`, plus `n_rows`, the robust geo-mean, and the worst row's full identity. **This ladder is the card's headline product**: it answers the question D5 took three cycles to ask — *what would a k·σ̂₀ gate on this family actually cost, before anyone wires a response to it?* A family whose 4σ̂₀ bin holds 17 %/epoch is telling you what D5's did. O(1) per row, forgetting-compatible, no row storage, no quantile sketch.

### INC-2: the label — three populations, never pooled

The existing VCE feed deliberately **skips** contaminated rows (pending-reseed, `eq34_reref`, `inflate_q`, `phase_hold`, `handover_affects`). Correct for estimating a variance component and **exactly backwards for a detector**: those skips remove precisely the rows a screen exists to find. So the census runs on **every** folded row and labels it:

- **`RESPONDED`** — an independent mechanism already fired on this row this epoch (pending-reseed / slip refloat / D5 / RAIM exclusion). The **positive control**: a statistic with power must show a shifted `|t|` distribution here. If not, the statistic is blind on that family — and that is the card's finding.
- **`CLEAN`** — nothing fired. High `|t|` here is a **miss**, the invariant-#1 gap.
- **`FRAME`** — datum handover / `eq34_reref` / `inflate_q`. A frame step, not noise; reported separately because these are invariant-#7 edges and their `|t|` distribution measures how much a re-coupling edge actually moves.

Two widened populations fold (or decline to fold) with a stochastic model the feed's nominal recompute does not reproduce. **`inflate_q`** (cb): the nominal-q recompute (`var_prev + q_cb·dt`) reproduces neither an inflated-q fold nor a dropped prior — standardize with what the mechanism actually did, and if that is unrecoverable report the row raw-formal-flagged rather than silently under the nominal q (landed differently: §13.2(3)). **`phase_hold`** (pb): the hold pins the posterior, so `ê` measures carry-vs-hold disagreement and neither branch's null applies — label `FRAME`, count it, exclude it from σ̂₀; the one admissible censoring, because the row's null dispersion is undefined, not because its value is large.

Ladders are kept **per (family × label)**. Pooling them is the one thing that would make the output uninterpretable, and it is the current monitor's behavior.

### INC-3: corroboration join (D5 v3's method, from cycle one)

With `prior_residual_corroborate` on, join each row above the report threshold to the raw-observable verdict the slip-QC pass already computed (`raw_stepped` / measured-flat / no-evidence), and emit the **contingency table** per family:

```
                 raw-stepped   raw-flat   no-evidence
  |t| > report        a            b           c
  |t| ≤ report        d            e           f
```

Auto-arm `network.cycle_slip.gap_instrument` when no slip-QC consumer would otherwise compute the verdicts — the same invariant-#4 tripwire D5 v3's loader carries, for the same reason (`gap_instrument` is instrumentation-only and verified byte-identical; **not** `mw_lli_corroborate`, a decision card that would perturb what is being measured).

The b-cell is the number that matters; on the DD family it measured 97 %. ⚠ The v1 "did ANY member of the domain step?" resolver SATURATES on satellite-level families and was replaced the same day — §13.5.

## 5. Cost

The hook already runs; the added work is a division, a `log` and two counter bumps per folded row — no covariance access, no reconstruct, no sweep. Contrast the redundancy census's **6.1 ms per station-sweep**, which needs `rowRedundancy` + `datumFrameSharedCov`; this card needs neither, because `post_var` and `fold_var` are already in hand at both call sites.

Budget: ≤ 1 ms/epoch at 63 stations. **Above 5 ms/epoch, something is wrong with the implementation, not with the estimate** — say so rather than adding a stride. No stride knob in v1.

## 6. Wiring sites (all verified against the tree at 2026-08-04)

*(Line numbers RE-verified 2026-08-15 and current; the 2026-08-04 originals are in §16's drift ledger. Prefer the symbol names — the numbers move.)*

- **`src/core/app/NetworkRunContext.cpp` — the `pl` feed** (`prires_want` gate `:2361`, `prires_est.addRow` `:2413`). The census needs the *same* `(oc->second.value_abs − value_abs, gapv, var, fold_var)` tuple but **without** the pending-reseed / `eq34_reref` exclusions, which become the `RESPONDED` / `FRAME` labels **and the row's `folded` bit**. Identity at the site: `i` (station index), `pa.sat`, `pa.signal`, `pa.amb_key`. ⚠ This spec's original instruction — "hoist the `carry_vce` qualification block" — is **superseded design intent**: it landed as an independent re-evaluation at each site, precisely because a hoist admits mirror drift (§13.2(4)).
- **`src/core/filter/NetworkStashStatus.cpp` — the `pb`/`cb` feed** (§6 anchor comment `:127`; `prires_shared_prior` gathered `:156-267`, drained `:318+`). `vce_shared_prior` (`:108`) is already the pre-clear prior snapshot, so widening its population to the skipped kinds is the whole change. Identity: `cp.kind`, `cp.sat`, `cp.signal`.
- **INC-2's new hooks**, mirroring the `pl` shape: the ZWD harvest (`NetworkRunContext.cpp:2468-2488`) and the `rx_cb` carry harvest (`:2622-2658`).
- **`src/core/filter/NetworkShHealth.cpp` — `beginEpoch` ordering**: `prires_est.beginEpoch` (`:407`) advances alongside `carry_vce_est.beginEpoch(pl_now_s, !datum_changed)` (`:390`). **The census passes `clean = true` unconditionally** — a datum-changed epoch is exactly a `FRAME` population to measure, not one to discard. `beginEpoch` FREEZES the σ̂₀ the epoch standardizes against, which is what makes predict-then-update enforceable.
- **Seam**: `src/core/filter/NetworkPriorResidual.cpp` (+ manual `CMakeLists.txt` entry) for trace/event/aggregate emission; the accumulator itself is header-only.
- **Traces**: `NetworkRunTraces` owns `prior_residual_trace` → `network_prior_residual.csv` (opened `NetworkRunTraces.cpp:551`). Per-epoch aggregates as new `network_epoch_status.csv` columns; per-row identities via `network_events.csv` tier `PRIORRES`, rate-limited like the PREFIT events. ⚠ Durability: only `health_trace` flushes mid-run (`rotateServiceCsvs` `:1001` / `writeHealthRow` `:1177`) — pair any long armed run with `health_interval_s`; the census seam additionally flushes its own stream on the content-hour roll (`prires_last_flush_hour`).
- **Counters + run-end**: `prires_*` in `NetworkServiceCounters` (`include/app/NetworkRunContext.hpp:2088+`, next to the `redun_*` block), reported by `src/core/filter/NetworkTeardown.cpp:932`. Invariant #4: the ladder itself is the telemetry — a family with `n_rows = 0` must read as *"ran, saw nothing"*, distinct from *"never ran"* (`prires_epochs > 0 && prires_rows == 0`), and the run-end line must make that explicit.

## 7. Config

| knob | type | default | note |
|---|---|---|---|
| `network.prior_residual_census` | bool | `false` | master arm |
| `network.prior_residual_families` | string | `"pl,pb,cb"` | csv; `NetworkCarryVce::parseComponents` reused verbatim. Known hooks: `pl,pb,cb,zwd,rxcb,clk` (`clk` is Card Q3's, §3) |
| `network.prior_residual_window_epochs` | int | `60` | σ̂₀ forgetting half-life; mirrors `carry_vce_window_epochs`. `< 1` ⇒ WARN + whole-run accumulation |
| `network.prior_residual_min_rows` | int | `200` | σ̂₀ warm-up (D5 v2's value); below ⇒ raw formal, flagged |
| `network.prior_residual_report_sigma` | double | `4.0` | per-row trace/event threshold in σ̂₀ units. **A reporting threshold, not a gate.** `≤ 0` ⇒ WARN + restored to 4.0 |
| `network.prior_residual_corroborate` | bool | `false` | INC-3; auto-arms `cycle_slip.gap_instrument` with a WARN. **Hard error** without `prior_residual_census` |
| `network.prior_residual_event_holdoff_ep` | int | `20` | added by §13.2(6); per-cell PRIORRES event rate limit, in census epochs |
| `network.prior_residual_global_sigma0` | bool | `false` | the §14 rider; **hard error** without `prior_residual_census` (§14.4) |

Loader placement: `loadNetworkCarries_`, next to the `carry_vce` block (`src/core/config/GnssConfigLoadNetwork.cpp:2954-3075`; this spec's original "`loadNetworkCarrySection_` / `:2458-2490`" is the pre-split name and range — §16). Two further invariant-#4 tripwires: WARN if `families` names a family with no hook in this build; WARN at run-end if any armed family accumulated zero rows.

Fingerprint status: **SAFE** (measure-only, no solve effect, no snapshot field) ⇒ legal on a warm resume. The σ̂₀ accumulator is deliberately **not** snapshotted in v1 — a resumed run restarts it empty and reports raw formal for the first window, acceptable only because the output says so per row. `NetworkCarryVce::seedRobustFloor` (the RTB-7(d) RE-1 precedent) is the v2 path if a resumed leg ever needs the ladder from epoch 1.

## 8. Unit tests

One file, one suite: `tests/test_prior_residual_census.cpp`, `PriorResidualCensusTest` (suite-name ownership is one-file-one-suite — keep it that way; the zero-cross-file-overlap property was verified at **174 suites** on 2026-08-03 and has NOT been re-verified since the suite grew to 196 — CLAUDE.md "Known state"). **21 cases today**: 1–12 this card's, 13–18 the §14 rider's (§14.5), 19–21 the §13.5 consensus fix's.

1. **The dispersion identity** — hand-built scalar fold with known `p`, `d`: `D{ê}` equals `p²/(p+d)` equals `r·fold_var`, §1 and LASER 5-74 agreeing to 1e-12. If this fails nothing else means anything.
2. Synthetic Gaussian rows of known truth ⇒ ladder occupancies match the normal tail (0.0063 % at 4σ) once σ̂₀ has converged — unbiased *when the assumption holds*, the falsifier for §2's claim that field data is not.
3. Heavy-tail input (Student-t, ν = 3) ⇒ ladder grossly over-occupied at 4σ while the geo-mean σ̂₀ stays near 1. **The D5 failure mode reproduced in a unit test**, so a reader cannot mistake it for a bug.
4. Predict-then-update: a single huge row does not standardize itself away.
5. Censoring: withholding above-threshold rows from σ̂₀ gives a *lower* σ̂₀ than the uncensored run — D5 v2's note stated mechanically.
6. Label partition: `RESPONDED`/`CLEAN`/`FRAME` ladders sum to the total; no row double-counted, none dropped.
7. Warm-up: below `min_rows` the reported `t` equals `s` and the raw-formal flag is set.
8. `r` clamp: `post_var > fold_var` (the coupling-carry case, `NetworkCarryVce.hpp:266` doc-comment, floor `:298`, cap at 1.0 `:299`) clamps rather than producing a negative variance.
9. OFF byte-identity (structural).
10. **The branch test** — one `(ê, fold_var, post_var)` row fed folded then not-folded yields `fold_var − post_var` and `fold_var + post_var`; and a refloat-scale unfolded row (`post_var = 10⁴·fold_var`) pushed through the folded branch reproduces the clamp inflation, kept as a NEGATIVE example the way test 3 keeps D5's.
11. INC-3 — the contingency table (the b-cell is the number that matters).
12. The bounded identity buffer (`kMaxHitsPerEpoch`) never costs a ladder count.

## 9. Field validation — known-answer gates

Controls: the golden 3 h 305-sta lineage (`project_golden0729_3h_validation`) plus the FT63 leg-19 window. Distinct run dir; Release only (invariant #2). Arms `config/test_suite/_ft63_prires_{off,on,corrob}.json` + `_smoke`.

1. **OFF byte-identity** — every solve/QC artifact identical OFF vs ON; only census columns, PRIORRES events, manifest and wall-clock fields may differ. Same-build (invariant #6).
2. **Live wiring** (invariant #4) — non-zero `n_rows` for every armed family, run-end line distinguishing "ran, saw nothing" from "never ran".
3. **★ THE POSITIVE CONTROL.** On `RESPONDED` rows the `|t|` distribution must be materially shifted against `CLEAN`. Quantify as the ladder ratio at 4σ̂₀ (meaningful only on INC-0's `+` branch — under the folded form the shift is the ε-clamp artifact). **A null result here is a publishable finding, not a failed gate**: same class as D5's 2.1 %, to be reported rather than tuned away.
4. **The `cb` known answer.** Re-run the DOY-129 pivot-CB ratchet window (q_cb = 1e-10, 49 m step, τ = 3.79 h). `cb`'s `|t|` must rise during the ratchet, **before** the 49 m step lands.
5. **The `pl` ladder.** 4σ̂₀ occupancy per epoch. Near D5's ~17 %/epoch ⇒ `pl` shares the DD family's persistent-model-error signature and the response card must be corroboration-first; near the Gaussian tail ⇒ `pl` is screenable with a plain threshold. **Either answer is the deliverable.**
6. **Contingency table** (INC-3 arm) — the b-cell per family. D5's was 97 %.
7. **Cost** — measured ms/epoch against the §5 budget.
8. Product claims on `total_IF` only (invariant #5); solve byte-identical in every arm.

⚠ Convergence hygiene, learned from the redundancy census's cold smoke: **both the statistic and its calibration are structurally distorted during convergence** (43 % of data rows read uncheckable cold; a free ambiguity absorbs its own arc's misfit exactly). Gates 3–6 run on a **converged warm window**; the cold smoke establishes gates 1, 2 and 7 only, and the report must say so. The same rule governs §14.6.

## 10. Deferral triage (pre-declared, per standing invariant #3)

| deferred | field-blocking? | evidence |
|---|---|---|
| Any response / gate wired to `t` | **INERT** — the card wires nothing; `report_sigma` is a trace threshold. The response is a successor card whose design input is §9 gates 3–6. | measure-only by construction |
| SH ties, Kaula prior, broadcast anchors, per-LOS iono priors | **INERT** for v1 | none has a single hook exposing the `(prior mean, prior var, posterior var)` triple — the SH tie is a block constraint, not a scalar row, so §1's scalar specialization does not apply and the block form (`D̂{ẽ₀}` full `l×l`) is genuinely new algebra. Named so a successor does not mistake the omission for an oversight. |
| σ̂₀ snapshot/restore on warm resume | **INERT** — the raw-formal fallback is flagged per row | `seedRobustFloor` precedent exists; wire it if a resumed leg needs epoch-1 ladders |
| Block (3D-analogue) Baarda test | **INERT** | LASER `BlockFormal`, Snow & Schaffrin (2003) eqs. (13)–(18). The natural block here is a (station, sat) LOS across bands. Own card — since built, `SPEC_redundancy_census`. |
| Consumer / RT-engine port | **INERT** | after producer validation |

The rider's three deferral rows are §14.7; both sets are INERT for the same reason (nothing consumes either statistic).

## 11. Standing-invariant pre-flight

*(Covers BOTH halves — census and §14 rider. §14.8 does not repeat it.)*

1. **Slip/jump response on stiff priors** — the card's whole subject. Neither half adds a response or removes one; they measure which stiff priors currently lack one.
2. **Release-only field runs** — §9, §14.6.
3. **Deferral triage with data for "inert"** — §10, §14.7.
4. **Run-end counters are the ON-path telemetry** — §6; zero-row families must be distinguishable from unarmed ones, and the ladder is the counter. The rider's `s0g_*` counters must likewise separate "ran, quiet" from "never ran" — §15.2 R-1 is where that distinction earned its keep on the first run.
5. **Grade datum-invariant quantities only** — §9.8; product claims on `total_IF`. The global σ̂₀ is datum-invariant by construction (quadratic forms of residuals).
6. **Default-OFF with structural byte-identity; same-build A/B** — §9.1, §15.1.
7. **Both edges of every decoupling** — the `FRAME` label *is* invariant #7 applied to the carry layer: a handover / re-reference decouples a carried prior from its history, and its `|t|` on return measures what the re-coupling edge actually moved. This card is the first instrument that reads that edge on the carry families rather than on the topology. The rider couples nothing.

## 12. What this card explicitly does NOT do

*(Both halves; §14.9 does not repeat it.)*

- It does **not** modify D5, its statistic, its calibration or its response.
- It does **not** add a gate, a deweight, an exclusion or a refloat. Nothing in the solve changes; that is the acceptance criterion, not a limitation.
- It does **not** import LASER's `t`/`F` critical values. §2.1.
- It does **not** replace the observable-side slip detectors. Invariant #1's coarse layer (MW / LLI / IF-TD) is untouched.
- It does **not** claim the statistic has power on any family. Establishing that, per family, is the deliverable — a null result on some family is valid, expected and reportable.
- **The rider**: it does not replace the median σ̂₀ anywhere a decision is made; it does not modify the census, RAIM, or any gate; it does not claim the global σ̂₀ is Gaussian-calibrated. **It is a SCALE, not a test statistic, in v1.**

---

## 13. BUILD RECORD (2026-08-04) — what landed, and every deviation from §1–§12

Code: `include/filter/NetworkPriorResidual.hpp` (header-only accumulator), `src/core/filter/NetworkPriorResidual.cpp` (emission seam + run-end readout), `tests/test_prior_residual_census.cpp` (12 cases at this build; 21 today, §8). Feeds at the five §6 sites; knobs in `loadNetworkCarries_`; ladder to `logs/network_prior_residual.csv`; `PRIORRES` events; five armed-only `network_epoch_status.csv` columns; `prires_*` counters reported by `NetworkTeardown`. Arms: `config/test_suite/_ft63_prires_{off,on,corrob,smoke,smoke_off}.json`.

### 13.1 Measured on the FT63 0.08 h smoke (Release, 63 stations, 11 epochs)

| gate | result |
|---|---|
| **1 — OFF byte-identity** | **PASS.** Same-build A/B, distinct cwd. Every solve/QC artifact byte-identical (`network_trace`, `network_solution`, `network_raim`, `network_dd_fractional`, `network_carry_vce`, `network_d5_flags`, `network_pf_keyed`, `network_vtec_sh*`, `network_slip_raw`, `network_wl_lane_trace`, `network_flag_rates`, `products.txt`). Diffs confined to: `network_prior_residual.csv` (ON only); `network_events.csv` = OFF **exactly** once the 20 PRIORRES rows are removed; `network_epoch_status.csv` = OFF **exactly** once the 5 appended columns are trimmed (54 → 59); wall-clock fields in `network_run_summary.csv` / `network_service_health.csv`. |
| **fingerprint SAFE** | **CONFIRMED, not asserted.** The two arms' `state_snapshot.bin` differ in **exactly one byte range** — the ISO wall-clock stamp — and the resume **fingerprint is identical** (`084ba4980fec500b`). A warm resume may cross arms. |
| **2 — live wiring** | **PASS,** and the inventory is itself the deliverable: `cb` 1305 rows (σ̂₀ = **2.21**), `rxcb` 629 rows (σ̂₀ = **0.56**), `pl` / `pb` / `zwd` **0 rows, each named by the "ran, saw nothing" WARN**. `pl` and `zwd` are structurally inert on this lineage (`carry_warm = phase_leveling && …`, and FT63 runs `carry_invariant` without leveling); `pb` is absent from `prev_carry` under `carry_invariant`. |
| **7 — cost** | **PASS with two decades of margin.** 1.5 ms total over 11 epochs = **0.14 ms/epoch** at 63 stations, against the ≤ 1 ms budget and the 5 ms line. No stride knob, as specified. |
| 3–6 | **Not graded** (cold smoke; §9's convergence hygiene). First reads only. |

First reads, explicitly **not** gate results: `cb`/CLEAN 4σ̂₀ occupancy **1.61 %** against a Gaussian 0.0063 %, with σ̂₀ = 2.21 — the formal σ is ~2.2× optimistic on this family and the residual tail is still ~250× over-occupied *after* calibration. That is the D5 signature, on a family that never had a screen. The INC-3 b-cell (flagged rows with positive evidence the raw observable did **not** move) read **57 % on `cb`** and **62 % on `rxcb`** against D5's 97 % — but §13.5 retires the resolver that produced those two.

### 13.2 Deviations from the spec, and why

1. **`kRedundancyFloor` is 1e-9, NOT `NetworkCarryVce`'s 1e-3.** §4 says reuse the monitor's clamp; that is wrong here and unit test 1 caught it. `r = p/(p+d)` goes to zero exactly when the prior is much **tighter** than the data — the stiffest, least checkable carries, i.e. the entire invariant-#1 population. An ordinary p = 1e-4 m² carry against d = 1 m² of data has r = 1e-4, an order of magnitude under the monitor's floor; clamping inflates `D{ê}` tenfold and shrinks `|t|` by √10, hiding a stale stiff carry behind a clamp. 1e-9 is a floating-point limit (relative precision of `1 − v/p` is ~ε/r: measured **6e-11 at r = 1e-6, 7e-9 at r = 1e-8**), not a modelling choice. **Consequence for §4**: the folded-form-on-unfolded-row artifact is ~**3.2e6**, not ×3000; test 10 asserts the larger figure and records ×3000 as its value at the monitor's ε. (Tree: `NetworkPriorResidual.hpp:1086` = 1e-9, `NetworkCarryVce.hpp:465` = 1e-3.)
2. **`folded` is derived from each family's ACTUAL inject predicate, not from the skip branch §6 assumes.** True for the shared `pb` carry (`eq34_reref` really does marginalize it) but **not for `pl`**: the phase-leveling injector has no `eq34_reref` skip, so a re-referenced arc *did* fold. Label and folded-bit are decided independently — a FRAME row can be folded. Truth wins over symmetry, because the bit selects the branch.
3. **`inflate_q` rows are standardized correctly, not reported raw-formal.** INC-2's raw-formal fallback is not needed: `clk_mon_reinit_var` is in `NetworkKnobs` and `cp.inflate_q` is on the pre-clear snapshot, so the fold variance the mechanism *actually used* is reconstructible at the site — those rows fold with the reinit variance and are labelled FRAME. `phase_hold` censoring is implemented as specified (FRAME, `calibrate = false`, raw-formal).
4. **No mirroring at any feed site.** Every `folded` bit is either ground truth already recorded by the assembly (`R.pl_zwd_injected`) or the injector's own predicate **re-evaluated** from values still in scope and un-mutated at the harvest (`rxCbInjectGate` is called again with identical arguments; the `pl` and `pb`/`cb` chains are reproduced clause for clause). This removes a whole class of mirror drift that §6's "hoist the qualification block" wording would have admitted. Two `pl` clauses are not reproducible and are documented at the site as assumed-held (the gauge-pin exclusion and the `h2local` lookup — both preconditions for the row reaching `R.pl_amb` at all).
5. **σ̂₀ is deliberately NOT clamped to `[1, cap]`** the way D5's gate clamps it. D5 may only *raise* its threshold because it acts; a census that clamped would report a calibrated-optimistic family (σ̂₀ < 1 — measured: `rxcb` at 0.56) as calibrated, and that is the measurement.
6. **Two knobs and one counter beyond §7**, all invariant-#4 driven: `network.prior_residual_event_holdoff_ep` (§6 says "rate-limited like the PREFIT events" but names no knob); a bounded per-epoch identity buffer (`kMaxHitsPerEpoch = 512`, `NetworkPriorResidual.hpp:474`) with `prires_hits_dropped`, a one-shot WARN and a test, so a truncated event stream can never read as a quiet epoch **and never thins a ladder**; and `n_r_negative`, splitting `post_var > fold_var` (a prior/posterior pair that does not describe one fold) out of the merely-stiff floor hits.
7. **The INC-3 `gap_instrument` auto-arm lives in `loadNetworkSlipDetectors_`,** not next to the knob parse: that helper runs *after* `loadNetworkCarries_` and would otherwise overwrite the flag — the section's load-order rule. Same rule, same reason, as the rider's `raim_redundancy_normalize` tripwire (§14.4).
8. **`_ft63_prires_smoke_off.json` added.** §9 names three arms plus a smoke, but gate 1 needs a same-duration control, and a byte-identity break should be caught at smoke scale rather than after a 3 h pair.
9. **The positive control emits a branch-mismatch WARN.** Measured here: `rxcb`/RESPONDED is 100 % unfolded and `rxcb`/CLEAN 100 % folded, so the §9.3 ladder ratio compares two different nulls and is **not** a power measurement. The run-end line says so and tells the reader to report the two ladders side by side instead of their ratio.
10. **The emission seam is called from `networkStashStatus`, not the driver** — it must run after the last feed and before the epoch-status row, written inside that seam. Same rationale as `networkStashCarry`'s call site.

### 13.4 305-station scaling smoke (2026-08-04, `_agu305_prires_smoke.json`)

303 stations, GPS-only, `lane_carry.enable = false`, 900 s = 31 epochs, census + corroboration armed, no OFF control paired (gate 1 was graded on the 63-station pair; the OFF contract is structural, not roster-dependent). Epoch-loop wall 1 386 365 ms over 31 epochs = **44.7 s/epoch**, 20 threads.

| | 63 sta | 305 sta | note |
|---|---|---|---|
| `cb` rows/epoch | 131 | 131 | satellite-level ⇒ **roster-independent**, as it must be |
| `rxcb` rows/epoch | 53 | 291 | receiver-level ⇒ scales with the roster (5.5× for 4.8× stations) |
| emission seam | 0.14 ms/ep | **0.17 ms/ep** | 5.4 ms over 31 epochs = **0.0004 % of the epoch-loop wall** |
| `cb` σ̂₀ | 2.21 | 1.62 | |
| `rxcb` σ̂₀ | 0.56 | 0.51 | |
| identities dropped to the buffer cap | 0 | 0 | 487 above-threshold rows, 182 events after holdoff |

**Gate 7 — PASS with three decades of margin**; §5's 5 ms/epoch line is not approached. One honesty caveat, recorded on the counter: `prires_wall_ms` times the **emission seam only**. The per-row `addRow` cost rides inside the carry harvests and is not separately timed — a divide, a `log` and two counter bumps over 422 rows/epoch, so microseconds, but **unmeasured**, and 0.17 ms/epoch must not be quoted as the card's total cost. **Gate 2 — PASS**: `cb` 4069 rows, `rxcb` 9021 rows, `pl` / `pb` / `zwd` at zero rows each named by the "ran, saw nothing" WARN.

Ladders are **not gradable** (31 cold epochs; §9's convergence hygiene). Four properties of the instrument, worth recording anyway:

- `geo(t²)` reads 1.15 (`cb`) and 1.93 (`rxcb`/CLEAN) — above 1, i.e. the σ̂₀ frozen from earlier epochs under-estimates the current spread. The expected signature of a distribution still moving under a predict-then-update calibration, and precisely why a cold ladder is not quotable.
- The run's worst row is a REAL event, correctly caught and labelled: CKIS00COK `GPS_DCB`, |t| = 81.8, ê = −4.11 m, mech `innov_withhold` — the rx-CB innovation gate fired on it, so the row is RESPONDED and sits on the `+` branch. CKIS00COK also owns the worst CLEAN row (|t| = 25.2). One station dominating both populations is itself the finding.
- The §9.3 branch-mismatch guard earned its place on first contact: `rxcb`'s positive-control ratio came out 0.18 with RESPONDED 100 % unfolded against CLEAN 100 % folded, and the WARN refused to let that read as a power measurement.
- `n_r_clamped` = 0 across both families and all 31 epochs, so the numerical floor never bound at this roster either. The `cb` worst row sat at r = 0.021 — a stiff prior barely checked by the data, two decades under the 1e-3 clamp §13.2(1) removed.

### 13.5 The INC-3 join saturated on SHARED families — FOUND 2026-08-04, FIXED same day

**The defect.** The join resolved a row's raw-observable verdict by asking "did ANY member of the domain step?". For a SHARED (satellite-level) prior — `pb`, `cb` — the domain is every station that measured that satellite, so at 303 receivers `P(any of N) = 1-(1-p)^N → 1` and the join stopped discriminating. Measured, same code, same data, same day:

| under the v1 OR | 63 sta | 305 sta |
|---|---|---|
| `cb` rows reading `raw_stepped` | 29 % | **70 %** |
| `cb` b-cell | 57 % | **21 %** |
| `rxcb` b-cell | 62 % | 77 % |

The `cb` b-cell did not fall because the physics changed. A statistic whose value depends on how many receivers happen to be in the roster is not measuring the satellite.

**The fix — a vote, not a threshold** (`priorResidualConsensus`, unit cases 19–21). The discontinuities these priors can suffer are COMMON-MODE over their domain by construction: a satellite bias jump enters MW (which carries the satellite code *and* phase bias) and GF at **every** station tracking it; a receiver reset moves **every** satellite at that station. A single member stepping is evidence about that ARC, and integrating it over an ever-growing domain is what saturated. So:

```
n_measured == 0  -> NoEvidence
n_measured == 1  -> the member's own verdict          (pl: bit-for-bit unchanged)
n_measured >= 2  -> Stepped iff 2*n_stepped > n_measured   (STRICT majority)
```

No threshold, no tuning, roster-independent in expectation, degenerating exactly to v1 on a one-member domain. The rule is a pure function in the leaf header; the driver's resolver gathers only DOMAIN COUNTS, which makes the roster-independence claim checkable without a network (case 19 holds the per-arc step rate at 20 % and grows the domain 10 → 1000: the OR saturates to Stepped at every size, the consensus stays Flat).

**Measured after the fix** (same two arms, same build):

| | 63 sta | 305 sta | verdict |
|---|---|---|---|
| `cb` mean domain | 17.8 | 81.6 | grows with roster, as it must |
| `cb` b-cell | **100 %** | **100 %** | **roster-independent** (was 57 % → 21 %) |
| `rxcb`/CLEAN b-cell | **100 %** | **100 %** | (was 62 % → 77 %) |
| `rxcb`/RESPONDED b-cell | 0 of 0 | 82.4 % | 3 of 17 flagged rows DO coincide with a receiver-common step |
| emission seam | 0.17 ms/ep | **0.14 ms/ep** | the per-epoch domain cache is a small WIN over v1's per-row 303-station scan, and removes an O(stations × rows) term |

**⚠ The fix trades one failure mode for its mirror image on `cb`, and the run says so.** The consensus fired **0 times in 4069 `cb` rows**, and "0 Stepped" reads identically whether no common-mode event occurred or the rule could never have seen one. Hence the three audit columns (`corrob_domain`, `corrob_or_step`, `corrob_step` — the consensus can only move rows Stepped → Flat, i.e. only RAISE the b-cell, so quoting a b-cell without them is circular) and the SENSITIVITY column `corrob_max_frac`, the highest stepped fraction any domain reached against the rule's 0.5 bar:

- `cb` — max fraction **0.134** (305) / **0.182** (63). The bar was **never approached**, so `cb`'s 100 % b-cell is trivially true and says nothing about whether a real satellite-side jump would be caught. The run-end line states this rather than presenting the zero as a finding.
- `rxcb` — max fraction **1.0000** (305) / **0.571** (63). The bar sits INSIDE the observed distribution and the consensus fired 4 (CLEAN) and 11 (RESPONDED) times, so that family's counts mean what they appear to mean.

So the fix is **established for the receiver-level families and unproven for the satellite-level ones**. `pl` is unaffected by construction (one-member domain, asserted in case 19) and untested in the field on this lineage, which carries no `pl` rows at all.

**The named successor, if `cb` sensitivity ever has to be closed:** replace the fixed 0.5 bar with a robust common-mode removal in the codebase's own idiom — compare a satellite's stepped fraction against the MEDIAN satellite's this epoch, scaled by 1.4826·MAD, exactly as the prefit census centres on the station median. That adapts to the epoch's background rate and needs no constant. NOT done here because it cannot be calibrated on a window with no known satellite-side event, and inventing a second uncalibrated mechanism to rescue the first is how D5 spent three cycles.

### 13.3 Open, for whoever grades this next

- Gates 3–6 need a **converged warm leg**: chain `_ft63_prires_on.json` onto a seed snapshot (legal — §13.1 confirms the fingerprint is unchanged) and grade only the resumed leg.
- `pl` is the largest population by two orders of magnitude and is **unmeasured** by this card's own arms: it needs a `phase_leveling` lineage. §9.5's "either answer is the deliverable" is entirely open. ⚠ Partly superseded — the rider's raim-live arm DID measure `pl` (§15.1), the first `pl` rows in the tree, so the lineage question is answered even though the ladder gate is not.
- §13.5's join saturation is FIXED, but the consensus rule is **unproven on the satellite-level families**: on `cb` the strict-majority bar was never approached (max stepped fraction 0.134 at 305), so its 100 % b-cell is trivially true. Grading it needs a window with a KNOWN satellite-side discontinuity — the DOY-129 pivot-CB ratchet is the obvious candidate, and the same window gate 4 wants.
- Gate 4 (the `cb` known answer) needs that specific window. The family is live and calibrated, so the measurement is now available to make.

---

## 16. Pointer-drift ledger (audited 2026-08-15)

Recorded rather than silently rewritten, because a reader chasing an older revision needs the old coordinate. Every pointer in §1–§15 is the **current** one, re-grepped 2026-08-15.

| what | as written 2026-08-04 | in the tree 2026-08-15 |
|---|---|---|
| census knob loader | `loadNetworkCarrySection_`, `GnssConfigLoadNetwork.cpp:2458-2490` | `loadNetworkCarries_`, `:2954-3075` (helper renamed in the 2026-08-01 config split) |
| `pl` feed (+ VCE skip predicate) | `NetworkRunContext.cpp:1777-1798` (predicate `NetworkRunContext.cpp:1784-1793`) | gate `:2361`, `addRow` `:2413` |
| ZWD / rx-CB hooks | `NetworkRunContext.cpp:1815+` | `:2468-2488` (zwd), `:2622-2658` (rxcb) |
| `pb`/`cb` feed | `NetworkStashStatus.cpp:127-147` (+ `NetworkStashStatus.cpp:100-115`) | anchor comment still `:127`; gather `:156-267`, drain `:318+`, `vce_shared_prior` `:108` |
| nominal-q recompute (`var_prev + q_cb·dt`, cited inline by §4 INC-2) | `NetworkStashStatus.cpp:140` | `:295` (`fold_var_hand = var_prev + q·max(0, dt)`, floored `:296`) |
| `beginEpoch` ordering | `NetworkShHealth.cpp:388` | `carry_vce_est` `:390`, `prires_est` `:407` |
| `prires_*` counters | `NetworkRunContext.hpp:1362+` | `:2088+` (per-epoch L1 scalars `:1497+`) |
| trace durability | `NetworkRunTraces.cpp:896` | opened `:551`; the only mid-run flushes are `rotateServiceCsvs` `:1001` and `writeHealthRow` `:1177` |
| `NetworkCarryVce` r-clamp | `NetworkCarryVce.hpp:245` / `NetworkCarryVce.hpp:267` / `NetworkCarryVce.hpp:430` | doc-comment `:266`, arithmetic `:297-299`, `kRedundancyFloor = 1e-3` `:465`, `kChi2GeoMean` `:473` |
| PPP precedent | `KalmanFilter.cpp:1736-1791` | `computeResidualQc_` `:1710+`, `variance_factor` `:1797`; δ₀ / r-floor / kInf hoisted to `include/filter/BaardaReliability.hpp`, called `:1737` / `:1772` / `:1789` |
| `assess` lambda | `NetworkSolveGlue.cpp:1029` | `:1200` |
| RAIM B2 redundancy | `NetworkRaim.cpp:128-147` | `row_r` `:178-184`, `addDataRow` `:240`, `s0g_redun_here` `:209`, B2 standardization `:267` |
| post-fit RAIM bracket | `ApplicationNetwork.cpp:1318` (`raim_on && carry_warm`) | `:2158` (`raim_on && (carry_warm \|\| ctx.knobs.raim_ci_on)`); `carry_warm` defined `:1428` — §15.2 R-1 |
| `NetworkInvariantCarryDriver` σ̂₀ precedent | `:1453-1455` | unchanged |

Unchanged and still exact: `NetworkPriorResidual.hpp` `kMaxHitsPerEpoch = 512` (`:474`) and `kRedundancyFloor = 1e-9` (`:1086`); `NetworkTeardown.cpp:932` (the census run-end line); the rider's loader guards `:3008-3013` / `:3903` / `:3913` / `:3920` (§14.4).

---

