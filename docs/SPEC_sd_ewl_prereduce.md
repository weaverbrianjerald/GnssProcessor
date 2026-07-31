# SPEC — SD pre-reduce EWL constraint (`network.sd_ewl.*`)

Status: **increment 1 (float pseudo-observation) — built, wired, rows building, graded,
DOES NOT CLEAR GATE 4. Stays OFF.** At an honest row σ the float layer buys ev1 +0.51 %
(real — 12/12 epochs, |mean|/se = 14.6 — but below the 0.6 % the DD-chord fold bought, and
below this SPEC's own "≫0.6 %" bar). The +6.78 % first measured was over-weighting. See
**§7.4 verdict**. The mechanism is harmless at honest weight and the plumbing is sound; the
value is in the integer, which is `docs/SPEC_sd_ewl_integer_hold.md` (increment 2).
Author trail: measured 2026-07-29 in `runs/golden_0729/`; the first ON run built zero rows
for three separate reasons — see **§10**, which is the part of this document to read first
if the counters ever go quiet again.

## 1. Why

The EWL/UWL decision tiers fix ~4970–8792 integers per 31 epochs at mean
`R = 0.0001 cyc²` (100 % FFR-fixed) and contribute **nothing** to the solve. Two
measurements this session bound the problem:

- Folding those integers on the existing **DD chord basis** (`mw_wl.ewl_fold_rows`)
  lifts the soft end of the reduced shared spectrum by **0.6 %** (ev1
  `1.4135e-1 → 1.4220e-1`), leaving `mass_pb` 0.928 and `span_pb` 23 unchanged.
- Cause: the F6 dump measured **80.7 % of each fundamental cycle's arcs are eq34
  tree pins** held at absolute 0. A DD-chord constraint therefore pins the ONE free
  chord ambiguity per cycle and never reaches the per-(satellite, signal) phase
  biases.

Those phase biases are exactly what the network is worst at determining. Every epoch,
the two softest **estimable** directions of `reduced_N_ss` are third-band Galileo
phase/code biases (`network_estimability.csv`), e.g.

```
ev 1.244e-2   PB|E09|L5X 0.1186  CB|E09|C5X 0.1184  PB|E09|L8X 0.1155  CB|E09|C8X 0.1154
ev 1.292e-2   PB|E08|L5X 0.1059  PB|E08|L8X 0.1052  PB|E14|L5X 0.1049  PB|E13|L5X 0.1037
```

against `ev_max ≈ 4.8e5`. Note those directions mix **PB and CB of the same satellite
across third bands** — which is precisely the combination an EWL constraint carries
(§3). Excellent integers, wrong basis.

## 2. Estimability — why SD and never undifferenced

`NetworkChainDatum` gauges **per (constellation, signal) connected component**: one
datum-receiver RPB pin + `(n_c + m_c − 1)` tree-edge ambiguity pins = `n_c + m_c`,
exactly the gauge dimension. L2 and L5 are **separate components with independent
gauges**, so the satellite phase bias carries

```
b_σ^s  →  b_σ^s + λ_σ·k_σ^s + c_σ         (per-satellite integer shift + component constant)
```

- **Undifferenced** `(b_b1^s − b_b2^s)`: freedom `λ_b1 k^s − λ_b2 k^s + (c_b1 − c_b2)`.
  Both free ⇒ **NOT estimable.** An undifferenced cross-signal constraint is meaningless.
- **Between-satellite SD** `∇(b_b1 − b_b2)`: the `c_σ` cancel, leaving
  `λ_b1·Δk_b1 − λ_b2·Δk_b2` — exactly the lattice `{λ_b1 m − λ_b2 n}`, which **is the
  EWL ambiguity**. Estimable modulo that lattice, and fixing the SD EWL integer is
  precisely the act that pins the lattice member.

⇒ The constraint MUST be between-satellite. This is a correctness requirement, not a
preference.

## 3. The row

Per station `r`, per satellite `s ≠ p` (`p` = the station's SD pivot satellite for that
(constellation, signal-pair)), one row on the EWL pair `(b1, b2)`:

```
z_SD = arcZdMean(s,b1,b2).mean_m − arcZdMean(p,b1,b2).mean_m        [metres]
```

The receiver EWL bias is common to both arcs at the same station and cancels
identically. Model:

```
z_SD = λ_EWL·[N_EWL(r,s) − N_EWL(r,p)]  +  [B_EWL^s − B_EWL^p]
```

with the ambiguity states held in **metres** (`N_b = amb_b/λ_b`) and the satellite bias
lump of the MW(b1,b2) observable, `MW = φ_WL − P_NL`:

```
B_EWL^s = (f1·PB_b1^s − f2·PB_b2^s)/(f1−f2)  −  (f1·CB_b1^s + f2·CB_b2^s)/(f1+f2)
```

**Local coefficients** (`A_r`, 4 columns):

| column | coeff |
|---|---|
| `amb_b1(r,s)` | `+λ_EWL/λ_b1` |
| `amb_b2(r,s)` | `−λ_EWL/λ_b2` |
| `amb_b1(r,p)` | `−λ_EWL/λ_b1` |
| `amb_b2(r,p)` | `+λ_EWL/λ_b2` |

**Shared coefficients** (`A_s`, up to 8 columns) — sign follows `MW = φ_WL − P_NL`, and
the satellite code bias enters the pseudorange with `+`:

| column | coeff |
|---|---|
| `PB\|s\|b1` | `+f1/(f1−f2)` |
| `PB\|s\|b2` | `−f2/(f1−f2)` |
| `CB\|s\|b1` | `−f1/(f1+f2)` |
| `CB\|s\|b2` | `−f2/(f1+f2)` |
| same four for `p` | negated |

This is the whole point: the row reaches the shared PB **and** CB columns of both
satellites — the measured soft directions — instead of a mostly-pinned DD chord.

**Which columns, exactly.** The PB columns are keyed on the two PHASE signals of the
elected pair. The CB columns are keyed on the **CODE** observables the MW actually
consumed (`CB|s|C2W`, not `CB|s|L2W` — the OSB layout is per code signal), and those
are *served by the arc*, via `MwArcZdMean::code1/code2`. They are not derived from the
phase attribute: `MwSlipDetector::collectUncombinedPairs` picks the code by
`matchChannel`, which prefers the same tracking attribute but **falls back to the first
code in the band**, so `L2L → C2L` is wrong whenever a station publishes `L2L` without
`C2L`. A mis-named OSB column is an undetectable modelling error, so the arc reports its
own channels and the row skips (counted) if it cannot.

⚠️ `NetworkEstimator::phaseBiasColumn` / `codeBiasColumn` are `index_.at()` and **throw**
`std::out_of_range` on an unregistered key — they do not return −1. Bare calls inside the
parallel produce phase escape into the produce loop's `catch`, which marks the station
`threw` and **drops its entire block** — good phase rows included. Both lookups must be
wrapped (see §10).

## 3b. Frame — the row is a CORRECTION, not an absolute value

The station's whole block is in the **correction frame**: `networkSplitStation_` fills `y`
from `em.innovations = z − h(x_lin)` with `x_lin = st.sm->getStateVector()`. A
pseudo-observation must obey the same convention, so the folded value is

```
y = z_SD − H·x_lin = [MW̄(r,s) − MW̄(r,p)] − [pred(s) − pred(p)]
pred(a) = λ_EWL/λ_b1·x_lin(amb_b1) − λ_EWL/λ_b2·x_lin(amb_b2)
        + f1/(f1−f2)·x_lin(PB_b1) − f2/(f1−f2)·x_lin(PB_b2)
        − f1/(f1+f2)·x_lin(CB_b1) − f2/(f1+f2)·x_lin(CB_b2)
```

This is not a refinement. The raw SD MW difference carries `λ_EWL·N_EWL` with `N_EWL` in
the **thousands of cycles** — measured mean `|z_SD|` **203 m**, max **4162 m**, against a
row σ of 0.13 m. Folding it unreduced is a colossal wrong constraint: the arm that did so
produced repeated `solveSharedConstrained: constrained shared block is not
positive-definite` and RAIM re-solve failures within eleven epochs
(`runs/sdewl_on30_RAWFRAME_INVALID/`). `NetworkMwWlLayer` subtracts the same term as its
`z_lin` (`z_corr = z_mw − z_lin`) — that is the precedent, not an option.

Which columns carry an `x_lin` term:

- **Local ambiguities** — seeded from code-minus-phase and reseeded on slips, so large
  and load-bearing.
- **Shared satellite PB/CB** — allocated at 0 and never written back from the solve
  (`MeasurementModel`: *"Allocated at value 0 … a SHARED state the network solves"*), so
  identically zero today. The term is **computed anyway** rather than asserted, so a
  future seeding change cannot silently skew the frame.

`max_abs_z_m` gates this **correction-frame residual**, which is the quantity the
cold-clock `x_lin` pathology inflates (a 256 km `x_lin` makes `pred` absurd while the raw
statistic looks ordinary).

⚠️ **Open exposure — BIA frame.** Measured on the 50-station fixture, **every** EWL arc
came back `bia_corrected = false` (4138/4138): `computeSatMwBias` could not form the MW
bias for the `(b2,b5)` pair, so the arc's mean carries the *total* satellite bias. If the
estimator applied an a-priori OSB the detector did not, `y` absorbs that difference as a
per-satellite constant and the row pulls PB/CB toward it. `skip_mixed_frame` only ensures
the two arcs of a row agree with *each other*, not with the estimator. The instrument is
the trace's `resid_m`: frames agree iff it sits at the MW code-noise scale with no
per-satellite offset. **Measure it before trusting a verdict from gate 4.**

## 4. Where it goes

**Pre-reduce, in the station's own block**, appended during assembly exactly like the
existing `networkAppendShIonoRows_` / `networkAppendGimIonoRows_`. That path folds into
`N_rr / N_sr / N_ss` **before** `reduce()`, so it **composes EXACTLY** with the
post-reduce Card-4 DD fold — see `NetworkNormalEquations.hpp:395`, which is explicit
that two POST-reduce Woodbury folds over coupled local columns drop the cross term
`C_L N_rr⁻¹ C_L,gauge^T` and are therefore mutually exclusive, while pre-reduce rows
"compose EXACTLY with this post-reduce DD fold (it reads the already-pinned factor)".

⚠️ `addStationPrior` is **DIAGONAL ONLY** (`N_rr(c,c) += Λ`) and cannot express this
cross-column row. The vehicle is the station's `A_s/A_r/w/y` block, which reaches
`addStationObservations`.

New seam: `Application::networkAppendSdEwlRows_` in `ApplicationNetworkAssembly.cpp`,
called from the per-station produce loop next to the existing iono-row appenders. No new
`.cpp` ⇒ no CMakeLists change.

## 5. Correlated R and whitening

All SD rows at a station share the pivot arc's noise:

```
R = diag(var_s) + var_p·11ᵀ
```

structurally identical to the GIM common-mode R. Whiten with `L⁻¹` (`LLᵀ = R`) into
unit-information rows (`w = 1`), verbatim the `networkAppendGimIonoRows_` pattern, so the
rows stay in the station's diagonal-weight block and contribute exactly
`Hᵀ R⁻¹ H` / `Hᵀ R⁻¹ z`.

`arcZdMean::var_of_mean_m2` is documented a CONSERVATIVE upper bound for a reconstructed
SD arc — using it as served is safe (over-weights nothing).

## 5b. Which signal pair — the candidate search

The MW arc key is `sat_<phase1>_<phase2>`, and **which tracking channel the detector
holds an arc on is its choice, not ours** (`collectUncombinedPairs` prefers the KF-chosen
channel, else the canonical attribute priority). Under `channel_mode: all` a band carries
several estimated channels, so electing one channel by rule and querying only that pair
reports "no arc" whenever the detector picked the other.

So the appender **searches**: every candidate `(band1 phase, band2 phase)` pair that has
BOTH ambiguities in this station's local block, in ascending `(sig1, sig2)` order (a
strict total order — byte-identity), taking the first with a usable arc. A one-channel
band reduces this to the direct lookup. Candidates come from `StateManager`'s own
ambiguity key map, never from a reconstructed key string: the key format is the state
layer's business and a key that fails to round-trip makes every arc look absent.

Measured on the 50-station fixture: **41 % of resolved arcs had two candidate pairs**, and
both `L7X/L5X` (1242) and `L7Q/L5Q` (1034) won — at different satellites. Likewise
`L2W/L5X` (959) and `L2L/L5Q` (844). A fixed lexicographically-smallest election would
have missed the `X`-attribute half outright.

## 6. Guards (each needs a counter — standing invariant #4)

| guard | counter | reason |
|---|---|---|
| `st.mw_slip == nullptr` | `skip_no_detector` | the detector is built only for NON-pivot stations, and only under `phase_leveling && (mw_detect \|\| turbo_edit)`. The **pivot station contributes no rows** — count it, don't hide it. |
| no candidate pair has both local amb columns | `skip_missing_amb_col` | the second EWL band is not ESTIMATED for this satellite. Dominant and CORRECT on GPS: only the L5 fleet qualifies (measured: 17 of 32 GPS satellites built rows). |
| `!exists` on every candidate | `skip_no_arc` | the detector never formed the pair. **Distinct from immature** — see §10. |
| `exists && count ≥ 2 && !valid` | `skip_frame_recon` | the arc is SD (`bia_corrected`) but its group MW pivot arc is absent, so `arcZdMean` cannot rebuild the ZD frame. |
| `count < min_arc_epochs` (incl. `count_lag < 2`) | `skip_immature` | maturity, mirrors the WL-1 gate. |
| `bia_corrected` mismatch | `skip_mixed_frame` | satellite-bias cancellation needs ONE frame per row (same rule as `NetworkMwWlLayer::skip_mixed_frame`). |
| any shared PB/CB column absent | `skip_missing_shared_col` | use the **const** `phaseBiasColumn` / `codeBiasColumn`, WRAPPED (they throw — §3); NEVER `register*` here (the produce phase is parallel — registration would race and the layout is frozen after `beginEpoch()`). |
| arc reports no code channels | `skip_missing_code` | the OSB column cannot be named without guessing (§3). |
| non-finite / non-positive `λ`, or `λ_WL` disagreeing with the tier's nominal `c/(f1−f2)` | `skip_bad_lambda` | arithmetic safety + mis-tiering: the detector's `λ_WL` comes from the ACTUAL frequencies, so a mismatch means the elected pair is not the tier the coefficients assume. |
| `\|z_SD\| > max_abs_z_m` | `skip_big_z` | the station-1 cold-clock `x_lin` pathology class. |
| fewer than 2 resolved arcs at the station | `skip_no_pivot` | nothing to difference against. |

`sat_seen` counts every `(station, epoch, satellite)` visit on an EWL-tier constellation —
the denominator, without which the skip tallies cannot be read (a truncated loop looks
like a quiet run; see §10).

Pivot satellite election: highest lagged `count`, tie-break by ascending satellite name.
Deterministic — required for byte-identity.

Trace: `network.sd_ewl.trace` → `logs/network_sd_ewl.csv`, one row per satellite visit
with the elected pair, its code channels, the candidate count, the arc count, and a
`status` naming the outcome in the vocabulary above. Rows are buffered per station in
`StationAssembly::sd_ewl_trace` (the produce phase must not touch a shared stream) and
drained by PASS B's serial consume, so the file is thread-count independent. Flushed per
epoch — this stream exists to diagnose runs that get killed mid-flight.

## 7. Gates

1. **OFF byte-identical.** Default `enable=false` ⇒ zero rows appended, artifacts
   identical to the current golden run.
2. `gnss_tests` green (904 cases / 138 suites, one fixture-gated skip). New suite in a
   NEW file — suite names are one-per-file.
3. Unit tests: coefficient algebra against a hand-worked GPS L2/L5 case; the
   `R = diag + var_p·11ᵀ` whitening reproducing `Hᵀ R⁻¹ H`; every guard counted; pivot
   election determinism.
4. **The bar, from §1:** must move ev1 in `network_sh_spectrum.csv` from **0.142 by ≫0.6 %**
   on the 50-sta GPS+Gal 30-epoch fixture, and must not degrade `clock_std_m` or D5 churn.
   Below that it is not worth the ordering risk and should stay off.

### 7.4 verdict — MEASURED 2026-07-29, gate 4 NOT cleared

Baseline `runs/golden_0729/net50_wl1off` (ev1 0.139037, mean of last 10 epochs). Three ON
arms, same build, threads matched at 8, one knob apart:

| arm | σ floor | \|z\| cap | rows | ev1 | vs base | clock_std_m | worst_dev_m | D5/run |
|---|---|---|---|---|---|---|---|---|
| `net50_wl1off` | — | — | — | 0.139037 | — | 8.520 | 16.309 | 1279 |
| `sdewl_on30` | 0.05 m | off | 7692 | 0.148465 | **+6.78 %** | 8.724 | 17.090 | 2003 |
| `sdewl_on30_guard` | 0.05 m | 10 m | 7425 | 0.147108 | +5.81 % | 8.796 | 17.454 | 1895 |
| `sdewl_on30_guard_sig` | 1.0 m | 10 m | 7425 | 0.139748 | **+0.51 %** | 8.475 | 16.157 | 1345 |

Read in that order:

1. **The lift is not the pathological tail.** Capping `|z|` at 10 m drops 267 rows (3.5 %)
   and keeps 86 % of the lift (+5.81 %). But `clock_std_m` did not recover and
   `worst_dev_m` got worse — the secondary damage lives in the **bulk** rows.
2. **The lift was the optimistic σ.** At `sigma_floor_m = 1.0` (the measured residual
   spread; realized σ_eff 1.414 m through `R = diag + var_p·11ᵀ`) the lift collapses to
   **+0.51 %** — and every secondary metric returns to baseline or slightly better
   (`clock_std_m` 8.475 vs 8.520; `worst_dev_m` 16.157 vs 16.309; D5 1345 vs 1279).
   `sigma0_pf_code`'s apparent improvement (4.109 → 3.909) also reverts to 4.068, so that
   too was over-weighting.
3. **+0.51 % is real but small**: paired per-epoch over the last 12 epochs, mean +0.51 %,
   sd 0.12 %, |mean|/se = 14.6, positive in **12/12**. So the float layer does carry
   information — just about 1/13 of what it was claiming.

**Why that is the theoretically expected answer.** `MW = φ_WL − P_NL` is a linear
combination of `φ1, φ2, P1, P2`, all already in the epoch's normals, so a float MW row adds
**zero new rank**. Its only genuinely new content is the arc's temporal constancy — which
the Card-4 DD carry and the shared bias carry already partly transmit, with no
cross-covariance in the fold. +0.51 % is that sliver. The **integer** is the part that is
nowhere in the normal equations, and increment 1 discards it.

**Disposition:** keep `enable` default false. Do not arm on the strength of the +6.78 %
number — it does not survive an honest σ. If the knob is ever armed for diagnostics, arm
`max_abs_z_m` (10 m) and `sigma_floor_m` (1.0 m) with it, or it over-weights by ~13×.

## 8. Increment 2 — BUILT 2026-07-29 (2a + 2b; 2c deferred)

Once the SD EWL float rows converge, round and fix (`∇N_EWL` integer-hard). That is the
AR step and needs its own validation — the network producer is float-only today by design
and a wrongly-fixed integer poisons the emitted product for every consumer.

**Built:** `network.sd_ewl.fix_enable` (default OFF). Route B (between-station DD of the
SD EWL — no bias a-priori, hence unpoisonable) decides the integer in a new serial seam
`filter/NetworkSdEwlAr`, and the row builder folds it by subtracting `λ_EWL·∇N` and
DROPPING its four local ambiguity coefficients. Fixed and float rows coexist per row, in
separate whitened groups. `docs/SPEC_sd_ewl_integer_hold.md` is the design;
`docs/SPEC_lane_spectrometer.md` §12 is the implementation record and amends both.
Increment 1 stays default OFF and its §7.4 verdict is unchanged — the float layer is still
worth +0.51 % at an honest σ, and that remains below this document's own bar.

## 9b. Detector changes this card required (SHARED PATH)

`MwSlipDetector` gained three things, all bookkeeping — the MW value arrives
pre-computed at `processMwSample`, so none of it can change a detection:

- `MwArcZdMean::exists` — the arc KEY is present in the accumulator. Strictly weaker
  than `valid`, and the only way to separate "never built" from "built but unusable".
- `MwArcStats::code1/code2` + two trailing `processMwSample` parameters + the served
  `MwArcZdMean::code1/code2` — the CODE channels the arc's MW consumed (§3).
- `arcZdMeanCold` serves the same three, for contract uniformity.

Because this is a shared path, standing invariant #6 applies: the OFF arm must be
byte-identical to the pre-change golden, compared **same-build**. `_sdewl_byteid_off.json`
is that arm; unit coverage is `SdEwlArcSourceTest` (which drives a real detector).

## 10. Increment-1 field record — why the first ON run built ZERO rows

The first ON run (`runs/golden_0729/sdewl_on`, 50 sta / 31 ep) reported `0 rows built`,
`5041 immature arc`, `4274 missing ambiguity column`, and **zero** in every other tally.
Three separate defects, and the counter shape was itself misleading:

1. **The loop was ABORTING, not skipping.** `phaseBiasColumn` / `codeBiasColumn` throw
   (§3). The first satellite per station-epoch that got past the earlier gates threw, the
   exception left the appender, and the produce loop's `catch` set `R.threw` → **961
   station-epoch drops** (`"assembly failed (invalid map<K, T> key)"`, absent from the OFF
   control). So the feature was not merely inert: it silently removed stations from the
   solve, and every artifact of that run is invalid. It also truncated the tallies —
   ~300 of ~831 satellite visits per epoch were counted at all, which is why `sat_seen`
   now exists.
2. **The channel election was wrong**, not the arc. `arcZdMean(sat, "L2W", "L5X")` found
   nothing because the appender elected the lexicographically smallest channel while the
   detector held the arc on another (§5b). The earlier reading — "`MwSlipDetector` only
   forms L1-referenced pairs, so the `(band2, band5)` pair is never created" — is
   **false**: `collectUncombinedPairs` enumerates every band pair present, and the WL-2
   EWL lane in the very same run decided 112 GPS `(L2,L5)` and 1292 Galileo `(E5b,E5a)`
   rows off those arcs. `SdEwlArcSourceTest` now pins this directly.
3. **`immature` was the wrong bucket** for a pair that does not exist, because
   `arcZdMean` returned one `valid=false` for four different reasons. Hence `exists`,
   `skip_no_arc`, and `skip_frame_recon`.

Diagnostic smoke after the fix (`_sdewl_smoke10.json`, 50 sta / 10 ep,
`min_arc_epochs=2`): **3358 rows built** from 8308 satellite visits, mean SD σ 0.127 m.
Remaining skips, all understood:

| bucket | n | reading |
|---|---|---|
| `NO_AMB_COL` | 2955 (2658 GPS) | the GPS L5 fleet gap — all 32 GPS satellites appear, exactly the 17 L5-capable ones build rows. Physical, not a defect. |
| `FRAME_RECON` | 1026 | SD arc whose group MW pivot arc is missing on that channel pair. The largest recoverable loss; a candidate that reconstructs is preferred already, so this is "no candidate reconstructed". |
| `NO_ARC` | 106 (1.3 %) | the pair genuinely absent — the direct answer to §10.2. |
| `NO_SHARED_COL` | 69 | PB/CB column unregistered (thin/gated keys). |
| `NO_PIVOT` | 14 | <2 resolved arcs at the station. |

## 9. Known risk, unresolved

`NetworkChainDatum.hpp:31` carries a strict ORDERING CONTRACT and the sticky tree
re-elects often (**47 tree swaps / 31 epochs** measured at 50 stations). These rows are
built from `h2local` + the const shared lookups each epoch and hold no cross-epoch state,
so they should be re-reference-agnostic by construction — but that is an argument, not a
measurement. Watch `eq34_reref` against the SD-EWL row count per epoch.
