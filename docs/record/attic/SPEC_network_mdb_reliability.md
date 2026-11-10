# SPEC — Baarda reliability numbers (MDB) on the network path

**Status**: SPEC'd 2026-08-04. Build in a fresh session. Independent of the other
LASER cards (composes with the redundancy census if present, requires only RAIM B2).
**Origin**: NGS LASER Guide v2.7 slide 5-97 (PDF p. 409) — the minimal-detectable-
outlier concept, Snow (2002); Baarda reliability theory.
**In-tree precedent — this is a PORT, not a build**: `KalmanFilter::computeResidualQc_`
(`src/core/filter/KalmanFilter.cpp:1709-1792`) already computes the full quartet on the
PPP path: per-row redundancy rᵢ, w-test, `MDBᵢ = σᵢ·δ₀/√rᵢ` with
`δ₀ = config_.dataSnoopingCritical() + 0.8416` (≈4.13 at the default k=3.29, β=0.80),
`kInf` when rᵢ < 1e-6 (`:1728-1731`, `:1765`, `:1782`). The DIA catalog recorded this
machinery as unported (`project_network_ssystem_dia_assessment` item 5); verified still
true 2026-08-04 — the network path has redundancy (B2) but no MDB anywhere.
**Memory context**: `project_laser_theory_mapping`, `project_m1_card4_d5_innovation`
(the formal-σ-optimism lesson that shapes the empirical variant), `project_ft63_fast_tier`
(the admission-screen known answer), `docs/SPEC_redundancy_census.md`.
**Scope discipline**: default-OFF, OFF byte-identical, MEASURE-ONLY, fingerprint-SAFE.

---

## 1. Why

The redundancy census counts UNCHECKABLE rows (rᵢ < ε) and RAIM standardizes residuals,
but nothing on the network path answers the service question in fault units: **how large
a fault could hide in this row right now?** MDB is that number, per row, in metres, and
it costs one division wherever rᵢ already exists.

Two honesty rules, both inherited from measured history:

1. **Formal MDB inherits the formal-σ optimism.** D5 measured the formal σ 10–30×
   optimistic on this data; MDB scales linearly with σ, so formal MDB is optimistic by
   the same factor. Report BOTH `mdb_formal` and `mdb_scaled = mdb_formal × σ̂₀,f`
   (the per-family empirical inflation from the census / M2 estimator, when armed;
   flagged absent otherwise). Anyone quoting `mdb_formal` alone to a consumer is
   repeating D5 v1.
2. **MDB is a property of the GEOMETRY, not a test.** v1 wires no decision to it. Its
   value is making the census's "uncheckable" binary quantitative and the RAIM
   thresholds interpretable.

## 2. Design

### INC-0: the port

Compute MDB inside the two loops that already hold rᵢ:

- **RAIM pool** (`raim_redun` armed): `src/core/filter/NetworkRaim.cpp:128-147` — B2's
  `row_r` is in hand, σᵢ = 1/√wᵢ from the row weight. Mirror the KalmanFilter formula
  verbatim including the `kInf` convention and the r-floor.
- **Redundancy-census sweep** (`NetworkRedundancyCensus.cpp`): rᵢ per swept row already
  computed; MDB is the same division. Swept-row MDB inherits the census's stride and
  escalation — no new sweeps.

δ₀ derivation is shared code (one helper, header-local): reuse the PPP config's
`dataSnoopingCritical()` k and the fixed β=0.80 convention (`+0.8416`) so PPP and
network MDBs are commensurable by construction. No new α/β knobs in v1.

### INC-1: aggregates and the uncheckable join

Per (constellation × row class) per epoch: median / p95 / max `mdb_formal`, the same for
`mdb_scaled` when available, count of rows at ∞, and the worst finite row's identity.
Rows at ∞ ARE the census's `redun_uncheckable` population — assert the counts reconcile
(one number, two instruments; a mismatch is a bug in one of them). Events (tier
`MDB`, rate-limited like PREFIT) only for finite rows above `mdb_event_m` — a reporting
threshold, not a gate.

### INC-2 (deferred, own card): external reliability

`|Δ emitted product|` for an MDB-sized fault — needs the row→product influence vector.
Named here so the omission reads as deliberate; the leave-one-station-out solution
separation (cheap; station contributions to the reduced normals are additive) belongs to
that card too.

## 3. Cost

One division + counter bumps per row in loops that already run. Budget: ≤0.2 ms/epoch
at 63 stations; if above 1 ms the implementation is wrong (say so, no stride knob).

## 4. Config

| knob | type | default | note |
|---|---|---|---|
| `network.mdb` | bool | `false` | master; requires `raim_redun` OR `redundancy_census` (hard error when neither — there is no rᵢ to divide by) |
| `network.mdb_event_m` | double | `1.0` | event threshold, metres; reporting only |

Loader: `loadNetworkQcSection_` next to the RAIM B2 knobs. Invariant-#4 tripwires: WARN
at run-end if armed and zero rows were seen; the aggregates must distinguish "ran, all
rows checkable" from "never ran".

## 5. Unit tests

New file `tests/test_network_mdb.cpp`, one suite.

1. **Formula parity.** On a toy net, the network helper reproduces the KalmanFilter
   path's MDB for identical (σ, r, k) to 1e-12 — one convention, two paths.
2. r → 0 ⇒ ∞ (the `kInf` convention), and the uncheckable-join reconciliation.
3. `mdb_scaled = mdb_formal × σ̂₀,f`, and the absent-σ̂₀ flag path.
4. Brute-force check: on a 2-parameter LS toy, an injected bias of exactly MDBᵢ is
   detected with ~80 % power at the k threshold by Monte Carlo (loose tolerance band —
   this pins the δ₀ convention to its Baarda meaning, not just to the other file).
5. OFF byte-identity (structural).

## 6. Field validation

1. OFF byte-identity; live wiring (invariant #4).
2. **The known answer — FT63 BSHM.** The admission screen's born-wrong channels were
   caught at |z| ≈ 1.6e5 m; those rows' MDB must be finite and ≪ the caught magnitude
   (the screen was operating far above threshold — MDB says by how much). Conversely
   the healthy single-frequency risers the census counts as uncheckable must read ∞.
3. **The headline table**: MDB distribution per constellation per signal class on a
   converged FT63 window, formal AND scaled. This is the number the service quotes; the
   formal/scaled gap per family is the honesty margin, expected O(10×) from D5/M2.
4. Cost within budget; solve byte-identical in every arm.

## 7. Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| External reliability / solution separation | **INERT** — v1 is internal reliability only; successor card named in INC-2 | — |
| Configurable α/β | **INERT** — fixed conventions keep PPP/network commensurable; revisit only with a consumer requirement in hand | — |
| MDB on prior rows (carry fold "observations") | **INERT** — the census's `t`-ladder already screens priors; a prior-row MDB needs the same r the census computes and can be added there in one line if a consumer asks | census §4 |

## 8. Standing-invariant pre-flight

1. No response added or removed — measure-only. 2. Release-only field runs. 3. Deferrals
above. 4. Aggregates + run-end counters are the telemetry; zero-row vs never-ran
distinguished. 5. MDB is datum-invariant (row-level, weight-and-geometry only).
6. Default-OFF byte-identical; same-build A/B. 7. No coupling change.

## 9. What this card explicitly does NOT do

- No gate, no deweight, no admission decision consumes MDB in v1.
- It does not replace the census's empirical ladders — MDB is the formal complement,
  and the scaled variant exists precisely because formal-alone was measured wrong.
- It does not claim detection power beyond what test 4 pins (single-fault, Gaussian).

---

## 10. BUILD RECORD (Opus session, 2026-08-04) — implemented, smoke-gated

Code: `include/filter/BaardaReliability.hpp` (the shared δ₀ / MDB / σ-from-weight
convention — NEW, and now the ONLY copy in the tree),
`include/filter/NetworkMdb.hpp` + `src/core/filter/NetworkMdb.cpp` (accumulator,
per-station feeder, INC-1 flush, run-end report; CMake entry added), feed sites in
`NetworkRaim.cpp` and `NetworkRedundancyCensus.cpp`, flush call in
`ApplicationNetwork.cpp` (SB_FRAMEDIAG, after `networkPostfitSigma0` and after the
census), report call in `NetworkTeardown.cpp`, 3 `network.mdb*` knobs + loader hard
error, `NetworkKnobs::{mdb_on, mdb_from_raim, mdb_from_census, mdb_event_m,
mdb_event_holdoff}`, carry + counter blocks in `NetworkRunContext`,
`network_mdb.csv` + conditional manifest fields in `NetworkRunTraces`,
`tests/test_network_mdb.cpp` (21 cases, one suite).  Full unit suite green:
1178 cases / 178 suites, one pre-existing fixture skip.

### Deviations from this spec, all deliberate

1. **The port is by SHARING, not copying.**  §"In-tree precedent" says PORT; the
   cheapest correct port was to lift δ₀, the r-floor and the `kInf` convention out
   of `KalmanFilter::computeResidualQc_` into `filter/BaardaReliability.hpp` and
   have BOTH paths call it.  `KalmanFilter.cpp` therefore changed — arithmetic
   identical (`k + 0.8416`, `σδ₀/√r`, floor 1e-6, same order of operations), so PPP
   is unaffected.  Consequence for §5.1: a file-to-file parity test is now a
   tautology, and building a `KalmanFilter` in a unit test would need a
   ProductParser + RinexParser + PppModel fixture to compare a formula against
   itself.  The suite pins something strictly stronger instead — the helper against
   an INDEPENDENT closed form over a (σ, r, k) grid, the two constants as literals,
   the stock config's δ₀ = 4.1316, and §5.4's Monte Carlo.  Two files agreeing on a
   WRONG δ₀ would pass a file comparison and fail all four.
2. **The uncheckable JOIN is not an equality against the ∞ count**, and asserting
   one would have hard-coded a bug.  MDB is ∞ at `r ≤ 1e-6` (the ARITHMETIC floor);
   the census calls a row uncheckable at `r < network.redundancy_eps` (0.05, a
   POLICY floor) — four decades apart, so ∞ is a strict SUBSET.  What the flush
   actually reconciles is its own independent recount of DATA rows and rows-below-eps
   against the census's `ep_redun_rows` / `ep_uncheckable_rows` for the same epoch;
   the ∞ count is reported separately.  Measured: RECONCILED on 11/11, 20/20 and
   31/31 census epochs across the arms.
3. **The event stream is the per-(station × class) WORST finite row, not per row.**
   §INC-1's literal domain is "finite rows above `mdb_event_m`", which on this data
   is essentially EVERY code row (measured: 1432 of 1432 finite code rows exceed the
   1.0 m default in one epoch) — tens of thousands of lines per epoch is a second
   copy of the trace, not a reporting channel, and it would blow §3's budget on
   string construction alone.  The full population still reaches the reader in the
   trace's `n_over` column, and each bin's `worst_*` columns name the network-wide
   worst exactly.  Same rate-limit shape as PREFIT.
4. **A third knob, `network.mdb_event_holdoff_ep` (default 20).**  "Rate-limited
   like PREFIT" needs a period; PREFIT's is a knob, so this one is too.
5. **`sigma0_src` gained a `*_deflating` tag — found in the first field run, not
   specified.**  §1 rule 1 assumes σ̂₀ INFLATES.  Measured on FT63: σ̂₀,phase =
   0.041–0.107, which scales the phase MDB *down* (median 4.58 m → 0.19 m) on rows
   that are 79 % UNCHECKABLE.  That reads as a TIGHTER guarantee and is the exact
   inverse of the truth — the phase pool has near-zero post-fit dispersion precisely
   BECAUSE each phase row is fitted exactly by its own free ambiguity, which is the
   same fact as "no redundancy".  Reported and never clamped (the prior-residual
   census's rule: the deflation IS the measurement), but tagged, counted, and
   called out in a run-end WARN.  `absent_sigma0_zero` catches only the exactly-zero
   case; this catches the dangerous middle, which on the `raimwarm` arm reached
   **σ̂₀ = 5.7e-12** — eleven decades of silent MDB deflation, had it gone untagged.
6. **σ̂₀,f comes from the P1-4 post-fit pass, per class.**  §1 says "the census / M2
   estimator"; those measure CARRY-PRIOR dispersion, not observation-row dispersion,
   so they are the wrong instrument for a row-level MDB.  `network.trace_postfit_sigma0`
   supplies `ep_sigma0_pf_code` / `_phase` for the same epoch (it runs earlier in
   SB_FRAMEDIAG).  `obs_other` and `iono_sh_row` have no pool there by construction
   and read `absent_no_pool_for_class`.  The loader WARNs at load time when the card
   is armed without the post-fit pass.
7. **Two source flags, not one master.**  `mdb_from_raim` / `mdb_from_census` are
   resolved in `NetworkKnobs::snapshot` so the run-end lines can say WHICH feed was
   inert — "the RAIM pool never ran warm" and "no sweep completed" demand different
   fixes.
8. **The epoch accumulator is STAMPED, not driver-cleared.**  The RAIM feed is in
   SB_RAIM and the census feed in SB_FRAMEDIAG; a feed whose TOW differs resets
   first.  An epoch that fed rows and never reached the flush (withhold, failed
   solve) therefore leaks nothing into the next epoch's aggregates.

### FINDING — the RAIM feed site has no domain on a `carry_invariant` lineage

Post-fit RAIM's Pass-1 walk iterates `R.pl_amb`, and `pl_amb` is populated only when
`carry_invariant` is **OFF** (`ApplicationNetworkAssembly.cpp`, `do_amb_carry` —
the DD invariant carry supersedes the phase-leveling ambiguity carry as a solve
input).  So on the standard armed set the post-fit RAIM pool is empty and
`network.raim_redundancy_normalize` (B2) is inert with it — a pre-existing property
of the lineage, surfaced by this card's invariant-#4 counter and now named in its
run-end WARN.  **On a `carry_invariant` run the redundancy census is the ONLY r_i
source**, which is why §4's loader check accepts either.

### Smoke gate (FT63, 63 stations, DOY-129, COLD, Release)

Arms `runs/ft63_mdb_{off,on,raimwarm,raimlive}`, configs
`config/test_suite/_ft63_mdb_*.json`, all derived from `_ft63_redund_on.json`.

- **§6.1 OFF-identity PASS (`off` vs `on`, same build, single-knob delta).**
  38 of 44 shared artifacts byte-identical, including `network_solution.csv` and
  every product / QC / epoch-status trace.  The 6 that differ are: the two config
  and run logs, `network_events.csv` (non-MDB rows diff-identical — every delta is
  an `MDB`-tier row), `network_run_manifest.json` (exactly the three conditional
  `network.mdb*` keys + the config path/hash, which necessarily differ between two
  files), and two wall-clock fields (`network_run_summary` ms, `service_health` ms).
  One added file: `network_mdb.csv`.  **The solve is byte-identical**, i.e. §6.4.
- **§6.1 live wiring, census feed.**  11 flush passes, 11 carrying rows, 35 948 rows
  binned, 12 623 uncheckable (35.1 %), 0 skipped for an unusable weight.
- **§6.1 live wiring, RAIM feed** (`raimlive`, `carry_invariant` off + phase
  leveling — the only configuration in which the pool has a domain; NOT a
  byte-identity arm).  85 569 rows binned, **20 300 of them from the RAIM pool**,
  join still reconciles ⇒ the two feeds do not contaminate each other's domain.
  The `raimwarm` arm (carry_invariant ON) contributed **0** RAIM rows and said so —
  the counter is the telemetry, per invariant #4.
- **§6.3 headline table**, `raimlive`, one converged-ish epoch (TOW 259500):

  | const/class | rows | ∞ | med formal | p95 | max | σ̂₀,f | med scaled |
  |---|---|---|---|---|---|---|---|
  | G / obs_code | 1436 | 4 | 2.24 m | 6.16 m | 9.03 m | 4.195 | **9.40 m** |
  | G / obs_phase | 2566 | 25 | 0.149 m | 0.541 m | 9.03 m | 0.107 ⚠deflating | 0.016 m |
  | G / obs_other | 9 | 0 | 1.79 m | 4.09 m | 4.09 m | — | absent |
  | ? / iono_sh_row | 354 | 0 | 4.25 m | 4.47 m | 4.59 m | — | absent |

  The **code row is the quotable number**: a fault smaller than ~2.2 m (formal) /
  **9.4 m (scaled)** can hide in the median GPS code row of this network.  The
  formal/scaled gap is **4.2×** — the measured honesty margin on this window, below
  the O(10×) D5/M2 expectation, and on a COLD 11–20 epoch start where σ̂₀ itself is
  not converged.  The phase row is NOT quotable: its σ̂₀ deflates (see deviation 5).
- **Uncheckable fraction is a lineage property, and MDB measures it.**
  `carry_invariant` ON: 79.2 % of phase rows uncheckable.  OFF + phase leveling:
  10.8 %.  Same network, same day — the leveled ambiguity carry buys real row
  redundancy, and this is the first instrument that says so in fault units.
- **§3 cost, MEASURED at 63 stations, stride 1 (the worst case; recommended stride
  is 10).**  Flush 4.0 ms over 11 epochs = **0.36 ms/epoch**; the per-row rider
  inside the census sweep is the census-wall delta, 3559.5 → 3625.5 ms = **6.0
  ms/epoch**, i.e. **1.9 % of that seam's own 324 ms/epoch**.  Total ≈ 6.4
  ms/epoch — ABOVE §3's 0.2 ms budget and above its 1 ms "the implementation is
  wrong" bound.  §3 costed ONE DIVISION; the dominant term is instead the
  `push_back` of ~3 270 doubles/epoch into the distribution pools that the median
  and p95 require, which the spec did not budget for and which cannot be removed
  without removing the order statistics that are the card's product.  Per the
  spec's own instruction **no stride knob was added**: the rider inherits the
  census's stride, so at the recommended `redundancy_census_stride: 10` it
  amortizes to ~0.6 ms/epoch against a host seam that costs 324 ms/epoch on the
  same schedule.  **Recommendation: restate §3's budget as "≤2 % of the host
  sweep", which is what it measures and what it can hold.**

### Scale gate (AGU305, 303 stations, DOY-129, COLD, Release, 2026-08-04)

Arm `runs/agu305_mdb`, config `config/test_suite/_agu305_mdb.json` (extends
`_agu305_gps_smoke`): GPS-only, lane layer OFF (the AGU-26 measured regime), 900 s
= **31 epochs, all emitted**, census stride 5 ⇒ 7 sweeps, `carry_invariant` ON so the
census is the only rᵢ source.  **ON arm only — no OFF control this pass** (user
directive), so every OFF-vs-ON claim below is inherited from the FT63 gate, and the
per-row rider's cost is BOUNDED, not measured.

- **Wiring live at scale, join holds.** 31 flush passes / 7 carrying rows, **105 693
  rows binned**, 0 skipped for an unusable weight; the uncheckable JOIN **RECONCILED
  on all 7** census epochs.  Census closure worst slack **+109.4** (must be ≥ 0).
  No new WARN/ERROR class — the only errors are the pre-existing 305-path TraceLogger
  message (present in `runs/agu305_gps_{smoke,1h}` from before this card) and the
  usual uncalibrated-radome / `addStation` rejections.
- **The two thresholds, concretely.** MDB counted **39 645** rows at ∞ (r ≤ 1e-6);
  the census counted **46 773** uncheckable (r < 0.05) over the same rows.  A strict
  subset, exactly as deviation 2 says — an equality assertion would have failed here.
- **Headline, last swept epoch (TOW 260100):**

  | const/class | rows | ∞ | med formal | p95 | max | σ̂₀,f | med scaled |
  |---|---|---|---|---|---|---|---|
  | G / obs_code | 6634 | 16 | 2.82 m | 8.19 m | 22.9 m | 3.074 | **8.67 m** |
  | G / obs_phase | 6642 | 5617 | 5.00 m | 8.08 m | 22.7 m | 7.8e-3 ⚠deflating | 0.039 m |
  | G / obs_other | 19 | 0 | 2.61 m | 7.74 m | 7.74 m | — | absent |
  | ? / iono_sh_row | 1731 | 0 | 4.60 m | 5.07 m | 6.47 m | — | absent |

- **THE RESULT: network size buys almost no per-row reliability.**  Same day, same
  lineage, 63 → 303 stations moves the median GPS code row's MDB by ~5 %:

  | | 63 sta (`ft63_mdb_on`) | 303 sta (`agu305_mdb`) |
  |---|---|---|
  | code med MDB, formal | 2.97 m | **2.82 m** |
  | code med MDB, scaled | 9.78 m (σ̂₀ 3.29) | **8.67 m** (σ̂₀ 3.07) |
  | code rows uncheckable | 0.3 % | 0.2 % |
  | phase rows uncheckable | 79.2 % | **84.7 %** |

  Adding 240 stations does not make an individual code row meaningfully more
  checkable, and it makes the phase rows LESS so.  That is what rᵢ being a LOCAL
  property predicts — a row's redundancy is set by its own station's geometry and by
  how many shared parameters its information has to support, and both grow together.
  Anyone expecting "a bigger network catches smaller faults per row" should read
  this table first; the win from scale is in the SHARED parameters, not in row-level
  internal reliability.
- **σ̂₀,code is still converging over the window** (2.43 → 3.07 across the 7 sweeps),
  so the scaled column is trending UP with time, not settling — another reason the
  converged-window gate below is still owed.
- **Cost, isolated WITHOUT an OFF control** by differencing the D0b `frame-diag`
  section between sweep and non-sweep epochs (the census + MDB rider + MDB flush are
  the only stride-gated occupants of that block):
  sweep epochs **2569 ms** mean vs non-sweep **127 ms** ⇒ **2442 ms per sweep epoch**,
  i.e. **551 ms/epoch amortized at stride 5 = 1.24 % of the 44.6 s/epoch loop**.
  Within that, the MDB FLUSH is 13.8 ms over 7 sweeps = **2.0 ms per sweep epoch
  (0.08 % of the sweep)**.  The per-row rider is not separable from the census's own
  8.17 ms/station-sweep without an OFF arm; scaling the FT63 measurement (+1.9 % of
  the host sweep) puts it at ~50 ms/sweep-epoch, ~11 ms/epoch amortized — an
  EXTRAPOLATION, flagged as such.  The "≤2 % of the host sweep" restatement proposed
  above holds at 303 stations.

### Outstanding (NOT covered by this smoke)

- **§6.2, the FT63 BSHM known answer.**  Needs the leg-19 converged window where the
  admission screen caught born-wrong channels at |z| ≈ 1.6e5 m; this gate is a COLD
  DOY-129 start from 00:00 and does not contain that event.  The two halves of §6.2
  are independently checkable there: those rows' MDB finite and ≪ 1.6e5 m, and the
  healthy uncheckable risers at ∞.
- **A converged (≥2 h) headline table.**  Every number above is inside ignition; the
  census header's own warning applies (43 % of DATA rows read uncheckable on a
  10-epoch cold start by construction, because a FREE ambiguity absorbs its own
  arc's phase misfit exactly).  Do not quote the cold σ̂₀ or the cold uncheckable
  fractions as the service answer.
