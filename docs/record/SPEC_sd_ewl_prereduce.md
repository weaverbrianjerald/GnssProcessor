# SPEC — SD pre-reduce EWL constraint (`network.sd_ewl.*`), increments 1 + 2

**Status.** **Increment 1 (float pseudo-observation): BUILT, wired, rows building, graded — DOES NOT CLEAR GATE 4. Stays default OFF.** At an honest row σ the float layer buys ev1 **+0.51 %** (real — 12/12 epochs, |mean|/se = 14.6 — but below the 0.6 % the DD-chord fold bought, and below this SPEC's own "≫0.6 %" bar); the +6.78 % first measured was over-weighting. **§7.4 is the verdict.** **Increment 2a + 2b (Route-B integer decision + fixed fold): BUILT 2026-07-29, default OFF (`network.sd_ewl.fix_enable`); gate 3 (the field verdict) STILL OWED.** **Increment 2c (zero-q hold): DEFERRED** — §8.3 keeps its five non-negotiable design rules intact. One verdict, not two: **the value of this card is in the INTEGER, not the float layer** (§8.1 argues it, §11.2 measures it).
Absorbed 2026-08-15: `docs/SPEC_sd_ewl_integer_hold.md` (increment 2 — now §8, §9, §15) and the sd_ewl-mechanism halves of `docs/SPEC_lane_spectrometer.md` (its §6 amendments A1–A4 and §12.1/§12.3/§14.1–§14.3 implementation record — now §10 and §11). **§10 is the AUTHORITATIVE amendment site**; the spectrometer keeps the diagnostic instrument and its campaign verdict. The absorbed integer-hold document now lives at `docs/attic/SPEC_sd_ewl_integer_hold.md`.

> **Provenance.** Every `runs/<name>` path below (`runs/golden_0729/*`, `runs/sdewl_on30*`, `runs/sdewl_byteid_off`, `runs/spectro_*`) resolves under `D:\GnssProcessor_archive\2026-08-12\runs\` (archived off C: 2026-08-12).

Author trail: increment 1 measured 2026-07-29 in `runs/golden_0729/`; the first ON run built zero rows for three separate reasons — see **§12** (renumbered from §10), the part to read first if the counters ever go quiet again.

## 1. Why

The EWL/UWL decision tiers fix ~4970–8792 integers per 31 epochs at mean `R = 0.0001 cyc²` (100 % FFR-fixed) and contribute **nothing** to the solve. Two measurements bound the problem: folding those integers on the existing **DD chord basis** (`mw_wl.ewl_fold_rows`) lifts the soft end of the reduced shared spectrum by **0.6 %** (ev1 `1.4135e-1 → 1.4220e-1`), leaving `mass_pb` 0.928 and `span_pb` 23 unchanged; and the F6 dump measured **80.7 % of each fundamental cycle's arcs are eq34 tree pins** held at absolute 0, so a DD-chord constraint pins the ONE free chord ambiguity per cycle and never reaches the per-(satellite, signal) phase biases.

Those phase biases are exactly what the network is worst at determining. Every epoch, the two softest **estimable** directions of `reduced_N_ss` are third-band Galileo phase/code biases (`network_estimability.csv`), e.g.

```
ev 1.244e-2   PB|E09|L5X 0.1186  CB|E09|C5X 0.1184  PB|E09|L8X 0.1155  CB|E09|C8X 0.1154
ev 1.292e-2   PB|E08|L5X 0.1059  PB|E08|L8X 0.1052  PB|E14|L5X 0.1049  PB|E13|L5X 0.1037
```

against `ev_max ≈ 4.8e5`. Those directions mix **PB and CB of the same satellite across third bands** — precisely the combination an EWL constraint carries (§3). Excellent integers, wrong basis.

## 2. Estimability — why SD and never undifferenced

`NetworkChainDatum` gauges **per (constellation, signal) connected component**: one datum-receiver RPB pin + `(n_c + m_c − 1)` tree-edge ambiguity pins = `n_c + m_c`, exactly the gauge dimension. L2 and L5 are **separate components with independent gauges**, so the satellite phase bias carries

```
b_σ^s  →  b_σ^s + λ_σ·k_σ^s + c_σ         (per-satellite integer shift + component constant)
```

- **Undifferenced** `(b_b1^s − b_b2^s)`: freedom `λ_b1 k^s − λ_b2 k^s + (c_b1 − c_b2)`. Both free ⇒ **NOT estimable.** An undifferenced cross-signal constraint is meaningless.
- **Between-satellite SD** `∇(b_b1 − b_b2)`: the `c_σ` cancel, leaving `λ_b1·Δk_b1 − λ_b2·Δk_b2` — exactly the lattice `{λ_b1 m − λ_b2 n}`, which **is the EWL ambiguity**. Estimable modulo that lattice, and fixing the SD EWL integer is precisely the act that pins the lattice member.

⇒ The constraint MUST be between-satellite. A correctness requirement, not a preference.

## 3. The row

Per station `r`, per satellite `s ≠ p` (`p` = the station's SD pivot satellite for that (constellation, signal-pair)), one row on the EWL pair `(b1, b2)`: `z_SD = arcZdMean(s,b1,b2).mean_m − arcZdMean(p,b1,b2).mean_m` [metres]. The receiver EWL bias is common to both arcs at the same station and cancels identically. Model:

```
z_SD = λ_EWL·[N_EWL(r,s) − N_EWL(r,p)]  +  [B_EWL^s − B_EWL^p]
B_EWL^s = (f1·PB_b1^s − f2·PB_b2^s)/(f1−f2)  −  (f1·CB_b1^s + f2·CB_b2^s)/(f1+f2)
```

with the ambiguity states held in **metres** (`N_b = amb_b/λ_b`) and `B_EWL^s` the satellite bias lump of the MW(b1,b2) observable, `MW = φ_WL − P_NL`.

**Local coefficients** (`A_r`, 4 columns) and **shared coefficients** (`A_s`, up to 8 columns) — the shared signs follow `MW = φ_WL − P_NL`, and the satellite code bias enters the pseudorange with `+`:

| column | coeff | | column | coeff |
|---|---|---|---|---|
| `amb_b1(r,s)` | `+λ_EWL/λ_b1` | | `PB\|s\|b1` | `+f1/(f1−f2)` |
| `amb_b2(r,s)` | `−λ_EWL/λ_b2` | | `PB\|s\|b2` | `−f2/(f1−f2)` |
| `amb_b1(r,p)` | `−λ_EWL/λ_b1` | | `CB\|s\|b1` | `−f1/(f1+f2)` |
| `amb_b2(r,p)` | `+λ_EWL/λ_b2` | | `CB\|s\|b2` | `−f2/(f1+f2)` |
| | | | same four for `p` | negated |

This is the whole point: the row reaches the shared PB **and** CB columns of both satellites — the measured soft directions — instead of a mostly-pinned DD chord.

**Which columns, exactly.** The PB columns are keyed on the two PHASE signals of the elected pair. The CB columns are keyed on the **CODE** observables the MW actually consumed (`CB|s|C2W`, not `CB|s|L2W` — the OSB layout is per code signal), and those are *served by the arc*, via `MwArcZdMean::code1/code2`. They are not derived from the phase attribute: `MwSlipDetector::collectUncombinedPairs` picks the code by `matchChannel`, which prefers the same tracking attribute but **falls back to the first code in the band**, so `L2L → C2L` is wrong whenever a station publishes `L2L` without `C2L`. A mis-named OSB column is an undetectable modelling error, so the arc reports its own channels and the row skips (counted) if it cannot.

⚠️ **THE THROW TRAP (paid for once — do not re-learn).** `NetworkEstimator::phaseBiasColumn` / `codeBiasColumn` / `clockColumn` are `index_.at()` and **throw** `std::out_of_range` on an unregistered key — they do not return −1. A bare call inside the parallel produce phase escapes into the produce loop's `catch`, which marks the station `threw` and **drops its entire block** — good phase rows included. Cost when it happened: **961 station-epoch drops on a run whose counters read "0 rows built"** — it looked like a clean inert control (§12). **Both lookups must be wrapped**, and never `register*` here (produce is parallel; the layout is frozen after `beginEpoch()`).

## 3b. Frame — the row is a CORRECTION, not an absolute value

The station's whole block is in the **correction frame**: `networkSplitStation_` fills `y` from `em.innovations = z − h(x_lin)` with `x_lin = st.sm->getStateVector()`. A pseudo-observation must obey the same convention, so the folded value is

```
y = z_SD − H·x_lin = [MW̄(r,s) − MW̄(r,p)] − [pred(s) − pred(p)]
pred(a) = λ_EWL/λ_b1·x_lin(amb_b1) − λ_EWL/λ_b2·x_lin(amb_b2)
        + f1/(f1−f2)·x_lin(PB_b1) − f2/(f1−f2)·x_lin(PB_b2)
        − f1/(f1+f2)·x_lin(CB_b1) − f2/(f1+f2)·x_lin(CB_b2)
```

Not a refinement. The raw SD MW difference carries `λ_EWL·N_EWL` with `N_EWL` in the **thousands of cycles** — measured mean `|z_SD|` **203 m**, max **4162 m**, against a row σ of 0.13 m. Folding it unreduced is a colossal wrong constraint: the arm that did so produced repeated `solveSharedConstrained: constrained shared block is not positive-definite` and RAIM re-solve failures within eleven epochs (`runs/sdewl_on30_RAWFRAME_INVALID/`). `NetworkMwWlLayer` subtracts the same term as its `z_lin` (`z_corr = z_mw − z_lin`) — that is the precedent, not an option.

Which columns carry an `x_lin` term: **local ambiguities** (seeded from code-minus-phase and reseeded on slips, so large and load-bearing) and **shared satellite PB/CB** (allocated at 0 and never written back from the solve, so identically zero *today*; the term is **computed anyway** rather than asserted, so a future seeding change cannot silently skew the frame). `max_abs_z_m` gates this **correction-frame residual**, the quantity the cold-clock `x_lin` pathology inflates (a 256 km `x_lin` makes `pred` absurd while the raw statistic looks ordinary). Increment 1's residuals: median 1.02 m, p90 3.50, p95 5.04, **p99 1349 m, max 1.85e6 m** — a 2.3 % pathological tail. Keep `max_abs_z_m` armed and `cold_clock_seed` on in the lineage.

⚠️ **Open exposure — BIA frame.** Measured on the 50-station fixture, **every** EWL arc came back `bia_corrected = false` (**4138/4138**): `computeSatMwBias` could not form the MW bias for the `(b2,b5)` pair, so the arc's mean carries the *total* satellite bias. If the estimator applied an a-priori OSB the detector did not, `y` absorbs that difference as a per-satellite constant and the row pulls PB/CB toward it. `skip_mixed_frame` only ensures the two arcs of a row agree with *each other*, not with the estimator. The instrument is the trace's `resid_m`: frames agree iff it sits at the MW code-noise scale with no per-satellite offset. **Measure it before trusting a verdict from gate 4.** Route B (§8.2) is immune to this; Route A is not.

## 4. Where it goes

**Pre-reduce, in the station's own block**, appended during assembly exactly like the existing `networkAppendShIonoRows_` / `networkAppendGimIonoRows_`. That path folds into `N_rr / N_sr / N_ss` **before** `reduce()`, so it **composes EXACTLY** with the post-reduce Card-4 DD fold — see `NetworkNormalEquations.hpp:395`, explicit that two POST-reduce Woodbury folds over coupled local columns drop the cross term `C_L N_rr⁻¹ C_L,gauge^T` and are therefore mutually exclusive, while pre-reduce rows "compose EXACTLY with this post-reduce DD fold (it reads the already-pinned factor)". ⚠️ `addStationPrior` is **DIAGONAL ONLY** (`N_rr(c,c) += Λ`) and cannot express this cross-column row; the vehicle is the station's `A_s/A_r/w/y` block, which reaches `addStationObservations`. Seam: `Application::networkAppendSdEwlRows_` in `ApplicationNetworkAssembly.cpp`, called from the per-station produce loop next to the existing iono-row appenders.

## 5. Correlated R and whitening

All SD rows at a station share the pivot arc's noise: `R = diag(var_s) + var_p·11ᵀ`, structurally identical to the GIM common-mode R. Whiten with `L⁻¹` (`LLᵀ = R`) into unit-information rows (`w = 1`), verbatim the `networkAppendGimIonoRows_` pattern, so the rows stay in the station's diagonal-weight block and contribute exactly `Hᵀ R⁻¹ H` / `Hᵀ R⁻¹ z`. `arcZdMean::var_of_mean_m2` is documented a CONSERVATIVE upper bound for a reconstructed SD arc — using it as served is safe (over-weights nothing). ⚠ True per-arc, FALSE per-CADENCE: see A1 (§10.1), which is why the sigma floor is 1.0 m at per-sample scale, and why the fixed path needs TWO floors (§10.5).

## 5b. Which signal pair — the candidate search

The MW arc key is `sat_<phase1>_<phase2>`, and **which tracking channel the detector holds an arc on is its choice, not ours** (`collectUncombinedPairs` prefers the KF-chosen channel, else the canonical attribute priority). Under `channel_mode: all` a band carries several estimated channels, so electing one channel by rule and querying only that pair reports "no arc" whenever the detector picked the other. So the appender **searches**: every candidate `(band1 phase, band2 phase)` pair that has BOTH ambiguities in this station's local block, in ascending `(sig1, sig2)` order (a strict total order — byte-identity), taking the first with a usable arc. A one-channel band reduces this to the direct lookup. Candidates come from `StateManager`'s own ambiguity key map, never from a reconstructed key string: the key format is the state layer's business and a key that fails to round-trip makes every arc look absent.

**THE CHANNEL-SEARCH MEASUREMENT (50-station fixture) — stated once, referenced throughout:** **41 % of resolved arcs had two candidate pairs**, and both `L7X/L5X` (1242) and `L7Q/L5Q` (1034) won — at different satellites. Likewise `L2W/L5X` (959) and `L2L/L5Q` (844). A fixed lexicographically-smallest election would have missed the `X`-attribute half outright. *(This is the fact §8.2's co-visibility figure, §10.7's channel-identity precondition and §11.1's second defect all rest on.)*

## 6. Guards (each needs a counter — standing invariant #4)

| guard | counter | reason |
|---|---|---|
| `st.mw_slip == nullptr` | `skip_no_detector` | the detector is built only for NON-pivot stations, and only under `phase_leveling && (mw_detect \|\| turbo_edit)`. The **pivot station contributes no rows** — count it, don't hide it. |
| no candidate pair has both local amb columns | `skip_missing_amb_col` | the second EWL band is not ESTIMATED for this satellite. Dominant and CORRECT on GPS: only the L5 fleet qualifies (measured: 17 of 32 GPS satellites built rows). |
| `!exists` on every candidate | `skip_no_arc` | the detector never formed the pair. **Distinct from immature** — §12. |
| `exists && count ≥ 2 && !valid` | `skip_frame_recon` | the arc is SD (`bia_corrected`) but its group MW pivot arc is absent, so `arcZdMean` cannot rebuild the ZD frame. |
| `count < min_arc_epochs` (incl. `count_lag < 2`) | `skip_immature` | maturity, mirrors the WL-1 gate. |
| `bia_corrected` mismatch | `skip_mixed_frame` | satellite-bias cancellation needs ONE frame per row (same rule as `NetworkMwWlLayer::skip_mixed_frame`). |
| any shared PB/CB column absent | `skip_missing_shared_col` | use the **const** `phaseBiasColumn` / `codeBiasColumn`, WRAPPED (they throw — §3); NEVER `register*` here (produce is parallel — registration would race and the layout is frozen after `beginEpoch()`). |
| arc reports no code channels | `skip_missing_code` | the OSB column cannot be named without guessing (§3). |
| non-finite / non-positive `λ`, or `λ_WL` disagreeing with the tier's nominal `c/(f1−f2)` | `skip_bad_lambda` | arithmetic safety + mis-tiering: the detector's `λ_WL` comes from the ACTUAL frequencies, so a mismatch means the elected pair is not the tier the coefficients assume. |
| `\|z_SD\| > max_abs_z_m` | `skip_big_z` | the station-1 cold-clock `x_lin` pathology class. |
| fewer than 2 resolved arcs at the station | `skip_no_pivot` | nothing to difference against. |

`sat_seen` counts every `(station, epoch, satellite)` visit on an EWL-tier constellation — the denominator, without which the skip tallies cannot be read. **A loop that aborts truncates its own counters; always emit a denominator** (§12). Pivot satellite election: highest lagged `count`, tie-break by ascending satellite name — deterministic, required for byte-identity.

Trace: `network.sd_ewl.trace` → `logs/network_sd_ewl.csv`, one row per satellite visit with the elected pair, its code channels, the candidate count, the arc count, and a `status` naming the outcome in the vocabulary above. Rows are buffered per station in `StationAssembly::sd_ewl_trace` (the produce phase must not touch a shared stream) and drained by PASS B's serial consume, so the file is thread-count independent. Flushed per epoch — this stream exists to diagnose runs that get killed mid-flight.

## 7. Gates

1. **OFF byte-identical.** Default `enable=false` ⇒ zero rows appended, artifacts identical to the current golden run. Same-build only; clear `C:/GnssProcessor/logs` before each arm (several service CSVs are APPEND-mode and `products.txt` differs in its `G` generation-timestamp record, so a stale block reads as a difference).
2. `gnss_tests` green, new suite in a NEW file (suite names are one-per-file). ⚠ The figure originally written here — "904 cases / 138 suites" — is a **dated historical record of the suite when this card landed**, not a gate. The suite is now **1360 cases across 196 suites in 89 files** (CLAUDE.md, re-counted 2026-08-12); grade against the current suite, green with the one fixture-gated skip.
3. Unit tests: coefficient algebra against a hand-worked GPS L2/L5 case; the `R = diag + var_p·11ᵀ` whitening reproducing `Hᵀ R⁻¹ H`; every guard counted; pivot election determinism.
4. **The bar, from §1:** must move ev1 in `network_sh_spectrum.csv` from **0.142 by ≫0.6 %** on the 50-sta GPS+Gal 30-epoch fixture, and must not degrade `clock_std_m` or D5 churn. Below that it is not worth the ordering risk and should stay off.

### 7.4 verdict — MEASURED 2026-07-29, gate 4 NOT cleared

Baseline `runs/golden_0729/net50_wl1off` (ev1 **0.139037**, mean of last 10 epochs). Three ON arms, same build, threads matched at 8, one knob apart:

| arm | σ floor | \|z\| cap | rows | ev1 | vs base | clock_std_m | worst_dev_m | D5/run |
|---|---|---|---|---|---|---|---|---|
| `net50_wl1off` | — | — | — | 0.139037 | — | 8.520 | 16.309 | 1279 |
| `sdewl_on30` | 0.05 m | off | 7692 | 0.148465 | **+6.78 %** | 8.724 | 17.090 | 2003 |
| `sdewl_on30_guard` | 0.05 m | 10 m | 7425 | 0.147108 | +5.81 % | 8.796 | 17.454 | 1895 |
| `sdewl_on30_guard_sig` | 1.0 m | 10 m | 7425 | 0.139748 | **+0.51 %** | 8.475 | 16.157 | 1345 |

Read in that order: (1) **the lift is not the pathological tail** — capping `|z|` at 10 m drops 267 rows (3.5 %) and keeps 86 % of the lift (+5.81 %), but `clock_std_m` did not recover and `worst_dev_m` got worse, so the secondary damage lives in the **bulk** rows; (2) **the lift was the optimistic σ** — at `sigma_floor_m = 1.0` (the measured residual spread; realized σ_eff 1.414 m through `R = diag + var_p·11ᵀ`) the lift collapses to **+0.51 %** and every secondary metric returns to baseline or slightly better (`clock_std_m` 8.475 vs 8.520; `worst_dev_m` 16.157 vs 16.309; D5 1345 vs 1279), and `sigma0_pf_code`'s apparent improvement (4.109 → 3.909) also reverts to 4.068 ⇒ **the float layer's entire apparent value was over-weighting, ~13×**; (3) **+0.51 % is real but small** — paired per-epoch over the last 12 epochs, mean +0.51 %, sd 0.12 %, **|mean|/se = 14.6, positive in 12/12**.

**Why that is the theoretically expected answer:** §8.1's rank argument. A float MW row adds **zero new rank**; +0.51 % is the sliver of arc temporal constancy the carries do not already transmit. The **integer** is the part that is nowhere in the normal equations, and increment 1 discards it.

**Disposition:** keep `enable` default false. Do not arm on the strength of the +6.78 % number — it does not survive an honest σ. If ever armed for diagnostics, arm `max_abs_z_m` (**10 m**) and `sigma_floor_m` (**1.0 m**) **together**, or it over-weights by ~13×.

---

# 8. Increment 2 — the integer (2a + 2b BUILT 2026-07-29; 2c DEFERRED)

*§8–§9 and §15 were `docs/SPEC_sd_ewl_integer_hold.md` (now `docs/attic/`), absorbed 2026-08-15. Its own section labels are carried as aliases so an "integer_hold §N" grep still lands — **integer_hold §1/§2 → §8.1, §3 → §8.2, §4 → §8.3, §5 → §8.4, §6 → §9.1, §7 → §9.2, §8 → §9.3, §9 (traps) → §3/§3b/§5b/§8.5/§12, §10 → §15.*

Once the SD EWL float rows converge, round and fix (`∇N_EWL` integer-hard). That is the AR step and needs its own validation — **the network producer is float-only today by design, and a wrongly-fixed integer poisons the emitted product for every consumer.** Increment 2 thereby converts each row from a soft constraint on a *sum* into a **direct observation of the shared satellite bias combination**: `[B_EWL^s − B_EWL^p] = z_SD − λ_EWL·∇N_EWL^fixed`. Three sub-increments, in this order, no reordering: **2a obtain → 2b fix → 2c hold.**

## 8.1 The rank argument (integer_hold §2) — why the float layer is not enough

`MW = φ_WL − P_NL` is a linear combination of `φ1, φ2, P1, P2` — observables the epoch's normal equations already hold. **A float MW row therefore adds ZERO new rank at that epoch.** Its only genuinely new content is (a) **integrality**, which increment 1 discards, and (b) the arc's **temporal constancy**, which the Card-4 DD carry and the shared bias carry already partly transmit with no cross-covariance in the fold. With `∇N_EWL` floating, the row's information splits between a station-local nuisance with effectively no prior and the shared bias — so **the nuisance absorbs nearly all of it**. Fixing the integer REMOVES the local degree of freedom rather than estimating it, and hands the whole row weight to the bias. That is the mechanism; everything below is about earning the right to do it. **Measured and CONFIRMED in §11.2.**

## 8.2 Increment 2a — obtain the integer (THE PRECONDITION)  *(integer_hold §3)*

Rounding `z_SD/λ_EWL` requires `B^s − B^p` removed first, which is what you are solving for. Circular. Two ways out.

**Route A — round against the a-priori satellite bias.** Needs no co-visibility. **BLOCKED, measured:** all **4138 EWL arcs** on the 50-station fixture returned `bia_corrected = false` — `MwSlipDetector::computeSatMwBias` cannot form the MW bias on the `(b2,b5)` pair, so the integer and the bias sit conflated in one number. Unblocking means extending BIA MW-bias formation to the third band. Inherited risk: **an integer rounded against an a-priori is poisoned by errors in that a-priori, PERMANENTLY, for every downstream consumer.**

**Route B — between-station DD of the SD EWL (PREFERRED, and the one BUILT).**

```
[MW̄(A,s) − MW̄(A,p)] − [MW̄(B,s) − MW̄(B,p)] = λ_EWL·[(N_As−N_Ap) − (N_Bs−N_Bp)]
```

`B^s − B^p` cancels **EXACTLY** (same satellite pair; each station's receiver term already cancelled inside its own SD). What remains is a pure integer combination × λ_EWL — **5.861 m GPS (2,5), 9.768 m Galileo (7,5), 19.537 m Galileo (8,5)** — against decimetre averaged MW noise. Roundable with **no bias knowledge at all**, which is why it is preferred: an integer that uses no a-priori cannot be poisoned by one.

Cost: pairwise co-visibility. Measured on the fixture — abundant: **67 distinct GPS SD pairs, 80 Galileo, ONE connected component each** over 17 GPS / 21 Galileo satellites. ⚠ **That figure is OPTIMISTIC under channel matching** — Route B's cancellation additionally requires both stations' arcs on the SAME tracking-channel pair (§5b's 41 %-two-candidate measurement), and the channel-consensus rule that enforces it excludes **3222 (station, satellite) arcs vs 2727 admitted — more than half** (§10.7).

**Decision criterion:** build Route B; fall back to A only if B's measured fix rate on the 50-station fixture is below the FFR gate, **and say so in the report**. *(Route B was built; Route A's precondition is unchanged and untouched.)* **Validation before 2b:** FFR (not ADOP) on the rounded DD, per the standing AR criterion. The existing decision tiers already fix at 100 % FFR with mean `R = 1e-4 cyc²` — that is the bar to reproduce, on the DD-of-SD combination rather than the chord basis.

## 8.3 Increment 2b — fold the fixed integer (the four rules)  *(integer_hold §4)*

In `Application::networkAppendSdEwlRows_`, when the row's `∇N_EWL` is fixed: subtract `λ_EWL·∇N_EWL^fixed` from the row's RHS; **zero the four local ambiguity coefficients** — the local DOF is REMOVED, not constrained, so **do NOT leave them in with a tight weight** (that is a stiff prior on a gauge-adjacent local column, not a rank reduction); keep the eight shared PB/CB coefficients unchanged (§3); keep the correction frame (§3b) — `pred_m` must drop its ambiguity terms IN STEP with the design, or the row is folded in a mixed frame (**both live in one place for this reason; keep them there**). Rows whose integer is not fixed stay on the increment-1 float path — **the two coexist PER ROW, not per run** (and, per §10.8, in SEPARATE whitened groups).

## 8.4 Increment 2c — hold at zero process noise (DEFERRED)  *(integer_hold §5)*

An ambiguity is exactly constant along an unbroken arc, so `q = 0` is the physically correct process noise, and holding it while the biases keep their `q_pb`/`q_cb` is what makes every later epoch a fresh bias observation. This is also **standing invariant #1** territory, which this codebase has paid for twice: the phase-leveling zero-q carry needed a slip detector retrofitted; the Card-4 invariant DD carry's slip hole was a **10.9 cyc/ep FIELD FAIL**, fixed to 2.4 only by the slip re-tap. The failure mode is a **slow non-recovering warp, not a crash** — cf. `q_cb = 1e-10` giving a **49 m step with τ = 3.79 h**.

**Five NON-NEGOTIABLE design rules (they survive the deferral):** (1) **HOLD ZD, DIFFERENCE AT USE** — hold `N_EWL(r,s)` per (station, satellite) *undifferenced* and form the between-satellite difference at row-build time; holding an SD value referenced to the station's pivot would make a **pivot re-election** a state event and drag the card into `NetworkChainDatum`'s ordering contract (increment 1's pivot election is per-epoch and stateless — keep it that way), and it halves the event surface (two arcs per held value, not four plus an election). (2) **Slip response wired in the SAME increment**, never a follow-up: a slip on either contributing arc drops the held integer (`resetSatelliteSignal` already gives per-(satellite, signal) granularity; PASS-A's slip funnel `carry_slipped_ambs` is the recording site, the same place the Iono-2 SH-dev erase hooks in). (3) **Gap / re-acquisition ⇒ DROP, not carry** — reset-over-repair. (4) **Every event class gets a counter and a run-end line** — a zero-q carry with no event counters is inert-and-invisible, which is how BOTH prior instances shipped broken. (5) **Grade on datum-invariant / SD quantities only.**

⚠️ **Invariant #1 already bites WITHOUT 2c** — and it has a wired response; see §10.9.

## 8.5 The basis-scope trap (integer_hold §9 — do not mis-cite)

The previously **FALSIFIED** MW/WL integer tiers and WL-1 float layer were measured on the **DD CHORD basis**, where 80.7 % of each cycle's arcs are eq34 tree pins at absolute 0. **This card CHANGES the basis. Do not cite that verdict as covering this one — and do not assume it doesn't, either. Measure.**

## 9. Increment-2 code locations, knobs, counters, and gates still owed

### 9.1 Where the code goes  *(integer_hold §6)*

| concern | location |
|---|---|
| row build (float + fixed) | `Application::networkAppendSdEwlRows_`, `src/core/app/ApplicationNetworkAssembly.cpp` |
| declaration + Doxygen | `include/app/Application.hpp` |
| DD-of-SD integer decision (2a) | NEW seam `filter/NetworkSdEwlAr.{hpp,cpp}` — free function over `(NetworkRunContext&, const GnssConfig&, log, NetworkRunTraces&, …)`, called from the epoch loop; **add to `CMakeLists.txt` BY HAND** (explicit source list, no globbing) |
| held integers + event counters | `app/NetworkRunContext` (`NetworkCarryState` / `NetworkServiceCounters`) — **never a function-local** |
| config knobs | `SdEwlConfig` in `include/config/GnssConfigTypes.hpp`; parse + clamp + ON-summary in `src/core/config/GnssConfigLoadNetwork.cpp` |
| per-station tallies | `NetStation::SdEwlCounters`, `include/filter/NetworkRunTypes.hpp` |
| run-end readout | `src/core/filter/NetworkTeardown.cpp` |
| trace | `NetworkRunTraces::sd_ewl_trace` → `logs/network_sd_ewl.csv`; rows buffer in `StationAssembly::sd_ewl_trace`, drained **SERIALLY** in `networkPassB_` (produce is parallel — never touch a shared stream there) |
| statistic source | `MwSlipDetector::arcZdMean` — serves `exists`, `code1`, `code2`, `bia_corrected`, `count` (lagged) |
| tests | NEW file, new suite names (one suite per file); `tests/test_sd_ewl_prereduce.cpp` owns `SdEwl{Algebra,Whitening,PivotElection,Config,ArcSource,ChannelSearch}Test` |

New knobs (default OFF ⇒ byte-identical): `sd_ewl.fix_enable`, `sd_ewl.fix_min_ffr`, `sd_ewl.fix_max_frac_cyc`, `sd_ewl.hold_enable`, `sd_ewl.hold_max_gap_s`, and — from A1 — `sd_ewl.fix_sigma_floor_m` (default **1.0 m**, §10.5).

### 9.2 Counters (invariant #4 — or it didn't happen)  *(integer_hold §7)*

`dd_pairs_seen`, `dd_fix_attempts`, `dd_fixed`, `dd_reject_frac`, `dd_reject_ffr`, `rows_fixed`, `rows_float`, `held_alive`, `held_dropped_slip`, `held_dropped_gap`, `held_dropped_pivotless`, `held_reused` — plus (from the build) `dd_flip`, `sd_ref_reelected`, `rows_fixed_folded`, `fix_datum_only`, `fix_pair_mismatch`, `fix_lone_arc`. **The run-end line must name the dominant cause when `rows_fixed == 0`**, as the increment-1 line does.

### 9.3 Gates (integer_hold §8) — gate 3 STILL OWED

1. **OFF byte-identical.** All new knobs default false ⇒ artifacts identical to `runs/sdewl_on30/` (increment-1 ON) and to `runs/sdewl_byteid_off/` with `sd_ewl` off. Same-build only; clear `logs/` per arm (§7 gate 1's APPEND-mode caveat).
2. `gnss_tests` green. Unit gates: the DD-of-SD cancellation of `B^s − B^p` (exact); the fixed-row design having **zero** local ambiguity coefficients; the correction frame still vanishing at the linearization point with the ambiguity terms dropped; each event class dropping the held value.
3. ⚠️ **STILL OWED — field, 50-sta GPS+Gal 30-ep fixture**, baseline `runs/golden_0729/net50_wl1off` (`ev1` = 0.1390, mean of last 10 epochs). Reference scale: the DD-chord fold bought **+0.6 %** (`singletop_base` 0.14135 → `singletop_fold` 0.14220). PRIMARY: `ev1` lift ≫ 0.6 %, **surviving** the tail guard and an honest `sigma_floor_m`. SECONDARY, where increment 1 fails today: must NOT degrade `clock_std_m` (**8.52 m**), `worst_dev_m` (**16.31 m**), or D5 churn (**1279 flags/run**). Fixed rows must show `resid_m` at the MW code-noise scale with **no per-satellite offset** — also the read on whether the §3b BIA frame skew is real.
4. **Convergence-speed claims are NOT gradeable on this fixture**: 31 epochs at 30 s with `min_arc_epochs = 10` leaves a 21-epoch row window. Use a longer lineage or the UWL cold-fix path (whose λ floor deliberately excludes GPS's 5.861 m EWL).

## 10. AUTHORITATIVE amendments A1–A4 and their build-time refinements

*Relocated 2026-08-15 from `docs/SPEC_lane_spectrometer.md` §6 (titled there "AMENDMENTS to SPEC_sd_ewl_integer_hold.md (authoritative)") and §12.3. **This is now the amendment site**; the labels A1–A4 are preserved verbatim. Where these conflict with §8/§9, THESE WIN.*

### 10.1 A1 — fold-cadence noise model  *(spectrometer §6 A1)*

`MwSlipDetector::arcZdMean` is **arc-CUMULATIVE**: consecutive-epoch rows share n−1 of n samples (**ρ ≈ 1 − 1/2n**). Refolding `var_of_mean` as white every epoch **over-claims information ~√k**, and the over-claim GROWS exactly as fixed-row residuals drop to code-noise scale (the convicted σ-overconfidence pattern: SH pseudo-obs refolded white; D2/local-carry double marginal). For ANY repeated fold (a live fold and a future 2c): **keep `sigma_floor_m` at PER-SAMPLE scale (1.0 m — the increment-1 ladder's measured honest point)**; folding the current mean at per-sample weight accretes ≈1 sample of information per epoch, asymptotically honest with **zero detector change**. Production v2: expose the per-epoch MW sample in `MwArcZdMean` (same bookkeeping class as `exists`/`code1`/`code2`) and fold the sample — the zero-q hold then lets the filter do the averaging optimally.

### 10.2 A2 — the DD→SD integer datum (the 2a→2b construction)  *(spectrometer §6 A2)*

Route B yields DD integers. Per connected component (measured: one per constellation), elect a reference station **r0** deterministically (most resolved arcs, tie-break ascending name), define **N_SD(r0) ≡ 0**, and propagate spanning-tree DD fixes to every station's SD integer; count `dd_flip` on any re-decision that changes a propagated value. The bias combination the network then observes is the true `B^s−B^p` **plus λ·N_SD_true(r0)** — a per-(sat-pair, component) constant re-datum, i.e. the classical WL-FCB modulo-λ definition. Harmless in-run and to any consumer integer decision; ⚠️ **PRODUCTION ARMING MUST ROUTE FIX-ONSET THROUGH THE PER-λ DISC CONTRACT (PB gate v2)** or the consumer wire sees a PB step.

### 10.3 A3 — grading additions  *(spectrometer §6 A3)*

(i) **Consumer replay (~33 s/arm) as a secondary gate** — the constrained `B_EWL` combinations are exactly what the consumer WL cascade consumes; its WL fix rate is the truest "convergence" read for a PPP-RTK service. (ii) A **cross-station fixed-row residual watchdog**: per (sat-pair), each station's fixed-row residual vs the multi-station bias estimate. Majority redundancy convicts the constant offsets that per-arc Welford σ structurally cannot. **Calibrate its ROC on EWL rows** (truth near-certain) — **REQUIRED before any WL(1,2) hold is ever considered** (2–4 % systematic wrong-by-one floor; half-cycle hazards).

### 10.4 A4 — scope  *(spectrometer §6 A4)*

The campaign uses **EWL rungs only: G(2,5) and E(7,5)**. WL(1,2) is decision / population-diagnostic only. The hold (2c) stays deferred until the campaign verdict; if the verdict is iono-representation, 2c competes with Iono-2 for the next build slot on measured payoff, not on promise.

### 10.5 A1 is implemented as TWO FLOORS, because it is two quantities  *(spectrometer §12.3)*

`sd_ewl.fix_sigma_floor_m` (default **1.0 m**) is used as **(i)** a variance-**OF-THE-MEAN** floor `σ²/n` for the FFR **DECISION** — the served `var_of_mean_m2` shrinks as 1/n and measures **13–60× optimistic** against the rows' own residual spread, and an optimistic σ inflates FFR into buying wrong fixes; and **(ii)** a **flat PER-SAMPLE** floor for the fixed **ROW**, which is A1's actual prescription. **Conflating them would either reject every mature arc or over-weight every fixed row.**

### 10.6 A2 gains a frame-extension rule the amendment did not state

`N_SD(r0) = 0` plus spanning-tree propagation only ever relays satellites the *parent already holds*, and at a 50-station global roster **r0 sees roughly a THIRD of a constellation** — that would forfeit two thirds of the arcs. A satellite is therefore **framed (value 0, counted as a datum, ungated) at the FIRST station that reaches it**. Legitimate, and it preserves A2 exactly: the frame constant is per **satellite**, so wherever it is defined the residual offset in any SD difference stays one constant per (satellite pair), identical at every station reading it. `SdEwlDatumTest.PropagationIsTransitiveAlongAChain` is the test that found the gap.

### 10.7 A NEW exactness precondition, not in A1–A4: CHANNEL IDENTITY

`B_EWL^s` is a **per-signal** combination of the satellite phase and code biases, so Route B's `B^s − B^p` cancellation holds **only if BOTH stations' MW arcs sit on the same two tracking channels**. They need not (§5b: 41 % two candidates; `L7X/L5X` and `L7Q/L5Q` each winning at different satellites). 2a therefore elects a **network-common channel pair per (constellation, satellite) by station consensus** (tie-break ascending pair name) and excludes stations on any other pair. **Measured cost: 3222 (station, satellite) arcs excluded vs 2727 admitted — more than half.** That is the price of exactness, and why §8.2's co-visibility headline is optimistic. The row builder counts `fix_pair_mismatch` if it settles elsewhere (**measured 0**).

### 10.8 Fixed and float rows coexist per ROW but in SEPARATE WHITENED GROUPS

§8.3 asks for per-row coexistence; a single group cannot deliver it. `R = diag + var_p·11ᵀ` is whitened by a **dense `L^-1`**, which would re-populate a fixed row's deliberately zeroed local columns out of its float siblings — and the pivot arc's ambiguity columns appear in **EVERY** row of a group, so one mixed group would simultaneously remove and estimate the pivot ambiguity. **Two groups**, each with its own pivot and its own common-mode term, keeps both halves exact and makes "a fixed row carries no local ambiguity coefficients" true of the *whitened block*, not just of `H`. A constellation with exactly ONE fixed arc has nothing to difference it against; it is demoted to the float group and counted (`fix_lone_arc`).

### 10.9 Invariant #1 has a wired response even without 2c — `sdEwlFlipCheck`

2c is not built and no decision is ever reused (integers re-derived every epoch). That does NOT by itself escape invariant #1: **a fixed row writes a λ-quantized value (5.861 / 9.768 m) into satellite PB/CB columns running at `q = 1e-12` / `1e-10`, whose measured response to a step is τ ≈ 3.8 h with the discontinuity counter SILENT throughout.** So an integer that CHANGES between epochs injects exactly that failure.

The response, in the same increment: `sdEwlFlipCheck` classifies each arc against the previous epoch as **Fresh** (folds — which is what keeps the S2 single-epoch fold from being byte-identical); **Stable**; **Restart** (the detector's lagged sample count FELL — slip, gap or re-acquisition, so the true integer legitimately changed: memory replaced, **no flip charged**); **Flip** (a different integer on a continuous arc: counted as `dd_flip` and the fix is **WITHHELD for that arc that epoch**, because on a flip epoch neither value is known to be right). `r0` is kept **STICKY** with re-election counted separately (`sd_ref_reelected`): only r0 sets the SD frame — the spanning tree and each station's link satellite cancel in the between-satellite difference — so tree churn needs no defending and a reference change does.

### 10.10 Build manifest (what landed, 2026-07-29)  *(spectrometer §12.1)*

| piece | where |
|---|---|
| 2a integer decision | NEW `filter/NetworkSdEwlAr.{hpp,cpp}` + `CMakeLists` entry; called from the epoch loop **inside the existing `SB_PREASM` bracket** |
| 2a pure core | `sdEwlBands`, `sdEwlRoundingFfr`, `sdEwlFlipCheck`, `sdEwlPropagate` — extracted so the datum algebra is unit-testable **without a detector** |
| 2b fixed fold | `Application::networkAppendSdEwlRows_` |
| carried state / counters | `ctx.carry.sd_ewl_{fix,r0,prev}`, `ctx.counters.sdewl_*`, `NetStation::SdEwlCounters::{rows_fixed,rows_float,groups_fixed,fix_pair_mismatch,fix_lone_arc}` |
| run-end readout | `NetworkTeardown.cpp` — two lines (2a decision, 2b fold) + a dominant-cause line when `rows_fixed == 0` |
| traces | `network_sd_ewl_ar.csv` (new; armed only with `trace && fix_enable`); `network_sd_ewl.csv` gains `n_fix_cyc,fixed,grp_fixed` **only** when `fix_enable` — armed-only header, so an increment-1 artifact set is unchanged |
| tests | NEW `tests/test_sd_ewl_integer.cpp` — `SdEwl{Datum,Ffr,Flip,FixedRow,BandTable,FixConfig}Test`, **22 cases** |

## 11. Measured behaviour of 2a + 2b

### 11.0 First smoke (10 epochs, 50 sta GPS+Gal, `min_arc_epochs = 2`)

**2727 arcs admitted, 1092 DD roundings, 1079 usable integers**, 34 rejected on |frac| and 493 on FFR, **49 flips counted and vetoed**, **0 reference re-elections**; **174 fixed rows in 118 all-fixed whitened groups** beside 2353 float rows, **0 channel mismatches**. The card is alive; the gate-3 field verdict is still owed.

### 11.1 The two code defects the validity arm caught (+ one about the arm)  *(spectrometer §14.1)*

*Relocated from `SPEC_lane_spectrometer.md` §14.1. The validity arm is the four-leg `ctl` → `off` → `on` / `inert` resume matrix; `off` vs `inert` came out **24 artifacts byte-identical, 0 DIFFER** (the only differences are the two sd_ewl TRACE files, armed-only by construction), and the datum was identical between `off` and `on` at the graded epoch, so the Δx below is a redistribution in a COMMON frame (invariant #5).*

1. **DATUM-VALUED INTEGERS ARE UNGATED BY CONSTRUCTION.** The reference station's own satellites, each station's link satellite, and a satellite framed on first sighting carry **no rounding decision**, so **no FFR or |frac| setting can reject them** — and a group built only from those asserts a hard constraint with ZERO integer information while still removing a real local DOF. Fix: `SdEwlFix::measured` marks them and a fixed group requires **at least one MEASURED integer** (`fix_datum_only` counts the demotions). A **MIXED group is correct and wanted** — the datum value is the reference the measurement is relative to.
2. **THE INTEGER MUST NOT BE ALLOWED TO CHOOSE THE CHANNEL PAIR.** The first version tried the decision's pair FIRST in the row builder's candidate search, on the correct reasoning that a fixed row must fold on the pair its integer was decided on. But that let the integer path change which pair the FLOAT path elects, and therefore **change the solve even when nothing was fixed**. The dependency now runs the other way: the row elects its pair exactly as increment 1 does (§5b) and the fix is used only if it was decided on THAT pair (`fix_pair_mismatch`, measured 0 — 2a searches the same candidates in the same ascending order).
3. **An FFR gate CANNOT express "impossible".** At `fix_min_ffr = 0.99999999` the arm still folded **6 fixed rows from 26 measured integers**, because a long arc's variance-of-the-mean makes its rounding arbitrarily confident. **The inert arm must use `fix_max_frac_cyc = 1e-9`.**

### 11.2 The Δx-per-class table — §8.1's mechanism, MEASURED  *(spectrometer §14.2)*

*Relocated from `SPEC_lane_spectrometer.md` §14.2. `runs/spectro_vf6/`, four legs resumed from the S1 snapshot (week 2313, tow 270060, fingerprint 68a662a415533831), 7 epochs each, `min_arc_epochs = 2`, graded at the first epoch with a folded fixed row (tow 270150).*

De-medianed per-constellation |Δ| medians at the graded epoch (the datum-invariant column; a whole-constellation common mode is a frame shift the product absorbs):

| state class | FLOAT layer (ctl→off) | **THE INTEGER (off→on)** |
|---|---|---|
| satellite PB | 0.0243 m | **0.0234 m** |
| satellite CB | 0.0102 m | **0.0014 m** |
| **PB / CB ratio** | **2.4×** | **16.7×** |
| satellite clock | 0.558 m | 0.241 m |
| SH-VTEC | 0.053 TECU | 0.018 TECU |
| ZWD | 1.22 m | 0.084 m |

The row algebra predicts the phase-bias columns move first: **per-satellite PB leverage +24.00 / −23.00 (GPS) and +39.33 / −38.33 (Galileo) against ±0.51 / 0.49 on the code-bias columns.** Measured: the FLOAT layer spreads its effect almost evenly between PB and CB (**2.4×**), while THE INTEGER concentrates it on PB by **16.7×** and moves CB **7× LESS than the float layer does** (0.0014 vs 0.0102 m). Exactly §8.1's mechanism, measured rather than argued.

Counters at the graded epoch: `on` folded **7 fixed rows of 306 built** (45 measured FIXED, 61 LINK, 13 R0, 175 NO_LINK, 0 FLIP); `off` folded **0 of 368**. Post-fit σ̂0 over stations: phase **0.085 (ctl) → 0.393 (float) → 0.323 (fixed)** — the float layer degrades phase σ̂0 4.6× and the integer fold recovers ~18 % of that; code **29.10 → 27.90 → 29.77**.

### 11.3 What this does NOT license  *(spectrometer §14.3)*

**None of it licenses arming `sd_ewl.fix_enable` in production.** Gate 3 (the 50-sta 30-epoch ev1 field verdict, §9.3) is still OWED; the A2 re-datum must route fix-onset through the per-λ disc contract (PB gate v2) before any consumer sees it; and the campaign measured **7 fixed rows per epoch out of ~300 built** — a yield question the channel-consensus rule (over half the arcs excluded, §10.7) now owns.

## 12. Increment-1 field record — why the first ON run built ZERO rows *(was §10)*

The first ON run (`runs/golden_0729/sdewl_on`, 50 sta / 31 ep) reported `0 rows built`, `5041 immature arc`, `4274 missing ambiguity column`, and **zero** in every other tally. Three separate defects, and the counter shape was itself misleading:

1. **The loop was ABORTING, not skipping.** `phaseBiasColumn` / `codeBiasColumn` throw (§3). The first satellite per station-epoch that got past the earlier gates threw, the exception left the appender, and the produce loop's `catch` set `R.threw` → **961 station-epoch drops** (`"assembly failed (invalid map<K, T> key)"`, absent from the OFF control). So the feature was not merely inert: **it silently removed stations from the solve, and every artifact of that run is invalid.** It also truncated the tallies — ~300 of ~831 satellite visits per epoch were counted at all, which is why `sat_seen` now exists.
2. **The channel election was wrong**, not the arc. `arcZdMean(sat, "L2W", "L5X")` found nothing because the appender elected the lexicographically smallest channel while the detector held the arc on another (§5b). The earlier reading — "`MwSlipDetector` only forms L1-referenced pairs, so the `(band2, band5)` pair is never created" — is **FALSE**: `collectUncombinedPairs` enumerates every band pair present, and the WL-2 EWL lane in the very same run decided 112 GPS `(L2,L5)` and 1292 Galileo `(E5b,E5a)` rows off those arcs. `SdEwlArcSourceTest` now pins this directly. (Only **106 of 8308** visits had no EWL arc at all.)
3. **`immature` was the wrong bucket** for a pair that does not exist, because `arcZdMean` returned one `valid=false` for four different reasons. Hence `exists`, `skip_no_arc`, and `skip_frame_recon`.

Diagnostic smoke after the fix (`_sdewl_smoke10.json`, 50 sta / 10 ep, `min_arc_epochs=2`): **3358 rows built** from 8308 satellite visits, mean SD σ 0.127 m. Remaining skips, all understood:

| bucket | n | reading |
|---|---|---|
| `NO_AMB_COL` | 2955 (2658 GPS) | the GPS L5 fleet gap — all 32 GPS satellites appear, exactly the 17 L5-capable ones build rows. Physical, not a defect. |
| `FRAME_RECON` | 1026 | SD arc whose group MW pivot arc is missing on that channel pair. The largest recoverable loss; a candidate that reconstructs is preferred already, so this is "no candidate reconstructed". |
| `NO_ARC` | 106 (1.3 %) | the pair genuinely absent — the direct answer to item 2. |
| `NO_SHARED_COL` | 69 | PB/CB column unregistered (thin/gated keys). |
| `NO_PIVOT` | 14 | <2 resolved arcs at the station. |

## 13. Detector changes this card required (SHARED PATH) *(was §9b)*

`MwSlipDetector` gained three things, all bookkeeping — the MW value arrives pre-computed at `processMwSample`, so none of it can change a detection: `MwArcZdMean::exists` (the arc KEY is present in the accumulator — strictly weaker than `valid`, and the only way to separate "never built" from "built but unusable"); `MwArcStats::code1/code2` + two trailing `processMwSample` parameters + the served `MwArcZdMean::code1/code2` (the CODE channels the arc's MW consumed, §3); and `arcZdMeanCold`, which serves the same three for contract uniformity. Because this is a shared path, **standing invariant #6 applies**: the OFF arm must be byte-identical to the pre-change golden, compared **same-build**. `_sdewl_byteid_off.json` is that arm; unit coverage is `SdEwlArcSourceTest` (which drives a real detector).

## 14. Known risk, unresolved *(was §9)*

`NetworkChainDatum.hpp:31` carries a strict ORDERING CONTRACT and the sticky tree re-elects often (**47 tree swaps / 31 epochs** measured at 50 stations). These rows are built from `h2local` + the const shared lookups each epoch and hold no cross-epoch state, so they should be re-reference-agnostic by construction — **but that is an argument, not a measurement.** Watch `eq34_reref` against the SD-EWL row count per epoch.

## 15. Run recipe  *(integer_hold §10)*

```
build/x64-Release/gnss_processor.exe config/test_suite/<arm>.json
```

Release only (Debug is 50–100× slower — compile checks and `gnss_tests` only; see CLAUDE.md's Build section and `scripts/vs2026_build.ps1` for the toolchain). Concurrent sessions are normal: if `LNK1104` hits, relink a renamed exe rather than killing another session's run, and pre-kill name-scoped WER zombies before each launch. Verify exe mtime > source mtime before trusting any run (the stale-Release trap). Arms derive from `_golden0729_net50_30ep_wl1off.json`; keep `eigen_threads` / `parallel_max_threads` at the baseline's **8** so the spectrum comparison stays single-treatment.
