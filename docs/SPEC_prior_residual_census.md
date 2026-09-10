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

## 17. The RNEG re-partition (2026-09-09)

> Numbering note: the card that ordered this work called it "§15"; §15 (BUILD RECORD B) and §16
> (pointer-drift ledger) were already taken, so it lands here. Cross-references from
> `docs/SPEC_slip_qc_hardening.md` §12 point at **§17**.

### 17.1 What a `post_var > fold_var` row is

`r` is the row's redundancy contribution. It is computed from the harvested posterior against the
folded prior (`include/filter/NetworkPriorResidual.hpp:747-751`), and when the posterior EXCEEDS the
prior it goes negative before the numerical floor:

```cpp
r_negative = r < 0.0;                        // coupling-carry conditional > prior
if (r < kRedundancyFloor) { r = kRedundancyFloor; r_clamped = true; }   // 1e-9, :1207
```

That is not a corrupted row. It is the documented **coupling-carry** case: the harvested posterior is
a CONDITIONAL variance (conditioned on the Card-1 coupling regressors), and a conditional variance
can legitimately exceed the prior it was folded against. What breaks is the STATISTIC — the pair
(prior, posterior) no longer describes ONE fold, so D{ê} is not that fold's dispersion, `r` slams
into `kRedundancyFloor = 1e-9`, and |t| inflates by √((p+v)/(εp)). The constraint-aware σ̂₀_global
rider has always dropped these rows (`kGlobalRnegDrop`, `NetworkPriorResidual.hpp:543`); the ladder
did not, so an artifact sat at the top of every readout.

### 17.2 The measured artifact (field, `pl` family, both leveling-vehicle arms)

Counted directly off the PRIORRES identity rows, `logs/network_events.csv`, with the r-negative test
applied numerically (`post_var > fold_var` parsed from the row's own printed fields, so the count
does not depend on the pre-fix text token):

| arm (12 h, DOY-129, 63 sta) | PRIORRES rows | rows \|t\| > 1e4 | of those, r-negative | worst \|t\| of the run | worst \|t\| EXCLUDING r-negative |
|---|---|---|---|---|---|
| `runs/fldf3_pl2` (converging leveling posture) | 26 855 | **957**, all family `pl` / label `CLEAN` | **927** (96.9 %) | **2.4819e8** at `pl/CLEAN OAFA00ARG/G25/L2X @263460` | **5.925e4** (`JPLM00USA/G32 @276150`) |
| `runs/fldf3_on2` (+F3) | 23 193 | **346**, all `pl` | **306** (88.4 %) | **7.261e7** at `pl/CLEAN BSHM00ISR/G22 @271200` | **7.068e5** (`SGOC00LKA/G13 @294780`) |

Two readings, both load-bearing:

1. **The old headline was an instrument artifact, not a stiff prior.** Four to five orders of
   magnitude of the "worst |t| of the run" came from the floor, not from the data.
2. **On `fldf3_on2` the run's worst |t| is BSHM00ISR/G22 @271200 — the gap-re-entry poison case**
   (`docs/SPEC_slip_qc_hardening.md` §12.1). A real defect was sitting inside a population that the
   readout had already made unreadable.

**The re-partition's effect on the same posture, measured.** `runs/fldf3_pl2_ctl_new` is
`runs/fldf3_pl2`'s configuration re-run on the post-change exe; its `products.txt` is **byte-identical**
(sha256 `be523008c181e487…` both), i.e. the change is readout-only. Run-end lines:

| | `runs/fldf3_pl2` (pre) | `runs/fldf3_pl2_ctl_new` (post) |
|---|---|---|
| `worst \|t\| of the run` line | 2.4819e8 at `pl/CLEAN OAFA00ARG/G25/L2X @263460` | **1.2649e5** at `pl/CLEAN COCO00AUS/G06/L1C @276150` (RNEG excluded) |
| RNEG headline | (did not exist) | **32 177 row(s)**, worst \|ê\| **139.44 m** at `pl/CLEAN INEG00MEX/G08/L2X @299490` |
| `rneg=1` identity rows emitted | 0 (token did not exist) | **3 012** |

The identity stream reports only rows clearing the census's per-epoch hit criterion, so 3 012 emitted
identities against 32 177 census rows is a sample, not a discrepancy.

### 17.3 What was built

| piece | where |
|---|---|
| the class | `include/filter/NetworkPriorResidual.hpp:1155-1156` — `rneg_epoch` / `rneg_run`, one Ladder per label, alongside the existing arrays; the re-partition itself at `:781-784` (`const bool to_rneg = r_negative;`) |
| ordering rule | the class is kept by **\|ê\| in metres**, not \|t\| (`keepsMaxByE`, `:628-631`): inside the class the floor can invert the \|t\| ranking of a loud residual on a stiff prior and a quiet one on a loose prior |
| what it touches | **nothing** in the CLEAN/RESPONDED/FRAME bins, in `sum_log_t2`, or in the worst-\|t\| line; `n_r_negative` in the label ladders is therefore 0 by construction and is retained only so existing readers keep parsing (`:345-351`) |
| CSV | 12 columns **appended at the end** of `network_prior_residual.csv` so every reader keeps parsing by position — `rneg_n_rows, rneg_n_raw_formal, rneg_n_no_sigma0, rneg_max_abs_e_m, rneg_station, rneg_sat, rneg_signal, rneg_mech, rneg_max_abs_t, rneg_fold_var_m2, rneg_post_var_m2, rneg_gap_s` (`include/logging/NetworkRunTraces.hpp:136-152` doc, `:153-166` header; emitted `src/core/filter/NetworkPriorResidual.cpp:193-200`) |
| row survival | a (family, label) whose ONLY rows this epoch were r-negative still emits — the guard is `L.n_rows == 0 && N.n_rows == 0` (`NetworkPriorResidual.cpp:169`), so the re-partition MOVES rows instead of deleting them |
| event token | ` rneg=1`, a SEPARATE greppable token from the pre-existing conditional `r_clamped=1 r_negative=1` text (`NetworkPriorResidual.cpp:236-246`) — the pre-fix text could not mark an r-negative row that did not also clamp, which is why §17.2 counts numerically |
| run-end, per family/label | a per-(family, label) RNEG line: row count, worst \|ê\| with its full identity, the fold_var / post_var / gap that produced it, and the \|t\| it *would* have read — stated so the exclusion is auditable (`NetworkPriorResidual.cpp:375-400`); a label with 0 ordinary rows says how many r-negative rows it did have |
| run-end, global | the worst-\|t\| line now says "(RNEG rows EXCLUDED — see the next line)" and a second headline reports the RNEG class **in metres** (`NetworkPriorResidual.cpp:569-589`), deliberately adjacent: a reader who remembers the 2.5e8 needs to see where it went |
| unit tests (uncommitted with the change; not re-run here) | `tests/test_prior_residual_census.cpp` — `RNegativeRowsAreClassedApartAndMoveNoLadderStatistic`, `RNegClassIdentityFollowsTheLargestResidualNotTheLargestT`, `UnfoldedRowsWithLargePosteriorsStayInTheirLabelLadder` |

**Reading rule (binding).** Quote the RNEG class on `rneg_max_abs_e_m` (metres). Its `rneg_max_abs_t`
is retained for diagnosis only and means nothing as a test statistic. Any "worst |t| of the run"
quoted from a run older than 2026-09-09 on a leveling vehicle is the pre-partition number and must be
re-read against §17.2 before it is compared with anything.

### 17.4 OPEN — the numerator card (not part of this change)

The census's ê is formed as the raw carried-minus-solved difference at the harvest site:

```cpp
const double innov = oc->second.value_abs - value_abs;   // src/core/app/NetworkRunContext.cpp:3279
```

Under the Card-1 conditional coupling the carried prior's RHS is **not** `value_abs` alone: the
coupling rows contribute Σ M_rj·ŝ_prev,j to it —

```cpp
rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j   src/core/filter/NetworkCoupling.cpp:238
```

— so on a coupled posture this ê and the ê the fold actually saw differ by the regressor term. The
DENOMINATOR side of the same mismatch is what produces the r-negative population in the first place
(the harvested posterior is a conditional variance). **Aligning the numerator was deliberately left
out**: it moves every coupled family's ê and therefore its σ̂₀, i.e. it is solve-adjacent measurement
work with its own A/B, not a readout re-partition. Until it is done, an ê on a coupled family is an
UNCONDITIONED residual and the frame it is expressed in is not the fold's — read magnitudes, not
significance, and remember `project_network_xlin_invariance`'s rule that a screen reading a linearization
frame measures an arbitrary quantity.
