# Rider A — Baarda reliability numbers (MDB) on the network path — `network.mdb`

**STATUS**

- **id:** Rider A
- **topic:** raim-qc
- **status:** closed
- **summary:** Baarda reliability numbers (MDB) on the network path — `network.mdb`
- **dates in the text:** 2026-08-04
- **knobs / posture:** `network.mdb`, `network.mdb_event_m`, `network.mdb_event_holdoff_ep`, `network.trace_postfit_sigma0`, `network.raim_redundancy_normalize`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/ft63_mdb_`, `runs/agu305_mdb`, `runs/agu305_gps_`
- **source:** `docs/archive/SPEC_redundancy_census.md` — 11, 11.1, 11.2, 11.3, 11.4, 11.5, 11.6, 11.7, 11.10

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_redundancy_census.md -->
## 11. RIDER A — Baarda reliability numbers (MDB) on the network path (`network.mdb`)

*Absorbed from `docs/attic/SPEC_network_mdb_reliability.md` (SPEC'd + built 2026-08-04).  Increments
namespaced **MDB-INC-0 / MDB-INC-1 / MDB-INC-2** (absorbed labels: `INC-0` / `INC-1` / `INC-2`).*

**Origin**: NGS LASER Guide v2.7 slide 5-97 (PDF p. 409) — the minimal-detectable-outlier concept,
Snow (2002); Baarda reliability theory.

**In-tree precedent — this was a PORT, and the port is now a SHARE.**
`KalmanFilter::computeResidualQc_` (`src/core/filter/KalmanFilter.cpp:1710`; the SPEC cited the whole
body as `KalmanFilter.cpp:1709-1792`) computes the full quartet on the PPP path: per-row redundancy
rᵢ, w-test, `MDBᵢ = σᵢ·δ₀/√rᵢ`.  As landed (deviation 1) that arithmetic lives in
`include/filter/BaardaReliability.hpp` and **both paths call it**:
`baardaDelta0(config_.dataSnoopingCritical())` at `KalmanFilter.cpp:1737`,
`baardaMinimalDetectableBias(sigma_i, r_i, delta_0)` at `:1772` and `:1789`;
`kBaardaPowerQuantile = 0.8416` at `BaardaReliability.hpp:62`, `kBaardaRedundancyFloor = 1e-6` at
`:68`.  ⚠ The pre-hoist description — δ₀ and the `kInf` convention inlined in `KalmanFilter.cpp` at
`:1728-1731`, `:1765`, `:1782` — is **history, kept only in deviation 1**; those inner pointers no
longer locate the arithmetic they named.  The DIA catalog recorded this machinery as unported
(`project_network_ssystem_dia_assessment` item 5); verified still true 2026-08-04 before the port —
the network path had redundancy (B2) but no MDB anywhere.

### 11.1 Why

The redundancy census counts UNCHECKABLE rows (rᵢ < ε) and RAIM standardizes residuals, but nothing on
the network path answers the service question in fault units: **how large a fault could hide in this
row right now?**  MDB is that number, per row, in metres, at one division wherever rᵢ already exists.
Two honesty rules, both inherited from measured history:

1. **Formal MDB inherits the formal-σ optimism.**  D5 measured the formal σ 10–30× optimistic on this
   data; MDB scales linearly with σ.  Report BOTH `mdb_formal` and `mdb_scaled = mdb_formal × σ̂₀,f`.
   Quoting `mdb_formal` alone to a consumer is repeating D5 v1.  ⚠ The rule ASSUMES σ̂₀ inflates — see
   deviation 5, where it does not.
2. **MDB is a property of the GEOMETRY, not a test.**  v1 wires no decision to it; its value is making
   the census's "uncheckable" binary quantitative and the RAIM thresholds interpretable.

### 11.2 Design

**MDB-INC-0 — the port.**  Compute MDB inside the two loops that already hold rᵢ:

- **RAIM pool** (`raim_redun` armed): `src/core/filter/NetworkRaim.cpp` — B2's `row_r` is in hand
  (`:171-182`), σᵢ = 1/√wᵢ from the row weight; the rider feeds at `:185-228`.  (The SPEC's
  `NetworkRaim.cpp:128-147` is stale.)  Mirror the KalmanFilter formula verbatim including the `kInf`
  convention and the r-floor.
- **Redundancy-census sweep** (`NetworkRedundancyCensus.cpp:212`): rᵢ per swept row already computed;
  MDB is the same division.  Swept-row MDB inherits the host's stride and escalation — no new sweeps.

δ₀ derivation is shared code: reuse the PPP config's `dataSnoopingCritical()` k and the fixed β=0.80
convention (`+0.8416`) so PPP and network MDBs are commensurable by construction.
δ₀ = k + 0.8416 = **4.1316** at the stock k = 3.29.  No new α/β knobs in v1.

**MDB-INC-1 — aggregates and the uncheckable join.**  Per (constellation × row class) per epoch:
median / p95 / max `mdb_formal`, the same for `mdb_scaled` when available, count of rows at ∞, and the
worst finite row's identity.  Rows at ∞ relate to the census's `redun_uncheckable` population — ⚠ **as
a strict SUBSET, not an equality; §14.1.**  Events (tier `MDB`, rate-limited like PREFIT) only for
finite rows above `mdb_event_m` — a reporting threshold, not a gate.

**MDB-INC-2 (deferred, own card) — external reliability.**  `|Δ emitted product|` for an MDB-sized
fault — needs the row→product influence vector.  Named so the omission reads as deliberate; the
leave-one-station-out solution separation (cheap; station contributions to the reduced normals are
additive) belongs to that card too.

### 11.3 Cost

Budget as specified: ≤0.2 ms/epoch at 63 stations; above 1 ms the implementation is wrong (say so, no
stride knob).  **Measured over budget — §11.10, and the restatement in §14.5.**

### 11.4 Config

Three knobs — `network.mdb` (master; **hard error** without `raim_redun` OR `redundancy_census`,
because there is no rᵢ to divide by), `network.mdb_event_m` (event threshold in metres, reporting
only), `network.mdb_event_holdoff_ep` (deviation 4).  **Types, defaults and dependencies: the one
table in §14.7.**  Loader `loadNetworkQcTiers_` (`GnssConfigLoadNetwork.cpp:3559-3580`), next to the
RAIM B2 knobs.  Fingerprint-SAFE.  Invariant-#4 tripwires: WARN at run-end if armed and zero rows were
seen; aggregates must distinguish "ran, all rows checkable" from "never ran"
(`NetworkTeardown.cpp:1070`).  Additional load-time WARN when armed without the post-fit σ̂₀ pass
(deviation 6).

### 11.5 Unit tests

`tests/test_network_mdb.cpp`, one suite, **21 cases** as landed.

1. **Formula parity** — superseded by deviation 1: the helper is checked against an INDEPENDENT closed
   form over a (σ, r, k) grid, the two constants as literals, the stock config's δ₀ = 4.1316, and item
   4's Monte Carlo (the absorbed card's §5.4).  **Two files agreeing on a WRONG δ₀ would pass a file
   comparison and fail all four.**
2. r → 0 ⇒ ∞ (the `kInf` convention), and the uncheckable-join reconciliation.
3. `mdb_scaled = mdb_formal × σ̂₀,f`, and the absent-σ̂₀ flag path.
4. Brute-force Monte Carlo: an injected bias of exactly MDBᵢ is detected with ~80 % power at the k
   threshold (loose tolerance band — pins the δ₀ convention to its **Baarda meaning**, not just to the
   other file).
5. OFF byte-identity (structural).

### 11.6 Field validation

1. OFF byte-identity; live wiring (invariant #4).  **MET, §11.10.**
2. **The known answer — FT63 BSHM**: the admission screen's born-wrong channels were caught at
   |z| ≈ 1.6e5 m, so those rows' MDB must be finite and ≪ that; the healthy single-frequency risers the
   census counts as uncheckable must read ∞.  **OWED — §15 row 2.**
3. **The headline table**: MDB per constellation per signal class on a converged FT63 window, formal
   AND scaled — the number the service quotes, with the per-family formal/scaled gap as the honesty
   margin, expected O(10×) from D5/M2.  **OWED at ≥2 h — §15 row 3.**
4. Cost within budget; solve byte-identical in every arm.  **MET on identity; budget restated, §14.5.**

The arm provenance a §11.6 grader needs is written into the run manifest
(`NetworkRunTraces.cpp:1374`).

### 11.7 Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| External reliability / solution separation | **INERT** — v1 is internal reliability only; successor card named in MDB-INC-2 | — |
| Configurable α/β | **INERT** — fixed conventions keep PPP/network commensurable; revisit only with a consumer requirement in hand (the row `BaardaReliability.hpp:31` cites) | — |
| MDB on prior rows (carry fold "observations") | **INERT** — the prior-residual census's `t`-ladder already screens priors; a prior-row MDB needs the same r that census computes and can be added there in one line if a consumer asks | `docs/SPEC_prior_residual_census.md` |

*(Pre-flight and does-NOT-do: §10.)*

### 11.10 BUILD RECORD (Opus session, 2026-08-04) — implemented, smoke-gated

Code: `include/filter/BaardaReliability.hpp` (the shared δ₀ / MDB / σ-from-weight convention — NEW,
now the ONLY copy in the tree); `include/filter/NetworkMdb.hpp` + `src/core/filter/NetworkMdb.cpp`
(accumulator, per-station feeder, MDB-INC-1 flush, run-end report; CMake entry added); feed sites in
`NetworkRaim.cpp` and `NetworkRedundancyCensus.cpp`; flush call in `ApplicationNetwork.cpp`
(SB_FRAMEDIAG, after `networkPostfitSigma0` and after the census); report call in
`NetworkTeardown.cpp`; 3 `network.mdb*` knobs + loader hard error; `NetworkKnobs::{mdb_on,
mdb_from_raim, mdb_from_census, mdb_event_m, mdb_event_holdoff}`; carry + counter blocks in
`NetworkRunContext`; `network_mdb.csv` + conditional manifest fields in `NetworkRunTraces`;
`tests/test_network_mdb.cpp` (21 cases, one suite).  Unit suite green at the time: **1178 cases / 178
suites**, one pre-existing fixture skip.

#### Deviations from this spec, all deliberate

1. **The port is by SHARING, not copying.**  δ₀, the r-floor and the `kInf` convention were lifted out
   of `KalmanFilter::computeResidualQc_` into `filter/BaardaReliability.hpp` and BOTH paths call it.
   `KalmanFilter.cpp` therefore changed — arithmetic identical (`k + 0.8416`, `σδ₀/√r`, floor 1e-6,
   same order of operations), so PPP is unaffected and PPP/network MDBs are commensurable **by
   construction**.  Consequence for §11.5.1: a file-to-file parity test is now a tautology, and
   building a `KalmanFilter` in a unit test would need a ProductParser + RinexParser + PppModel fixture
   to compare a formula against itself.  The suite pins something strictly stronger instead.
2. **★ The uncheckable JOIN is not an equality against the ∞ count**, and asserting one would have
   hard-coded a bug.  Full statement in **§14.1**.
3. **The event stream is the per-(station × class) WORST finite row, not per row.**  MDB-INC-1's
   literal domain is "finite rows above `mdb_event_m`", which on this data is essentially EVERY code
   row (**1432 of 1432** finite code rows exceed the 1.0 m default in one epoch) — tens of thousands of
   lines per epoch is a second copy of the trace, and it would blow §11.3's budget on string
   construction alone.  The full population still reaches the reader in the trace's `n_over` column,
   and each bin's `worst_*` columns name the network-wide worst exactly.
4. **A third knob, `network.mdb_event_holdoff_ep` (default 20).**  "Rate-limited like PREFIT" needs a
   period; PREFIT's is a knob, so this one is too.
5. **★ `sigma0_src` gained a `*_deflating` tag — found in the first field run, not specified.**  §11.1
   rule 1 assumes σ̂₀ INFLATES.  Measured on FT63: σ̂₀,phase = **0.041–0.107**, which scales the phase MDB
   *down* (median **4.58 m → 0.19 m**) on rows that are **79 % UNCHECKABLE**.  That reads as a TIGHTER
   guarantee and is the exact inverse of the truth — the phase pool has near-zero post-fit dispersion
   precisely BECAUSE each phase row is fitted exactly by its own free ambiguity, the same fact as "no
   redundancy" (§9).  Reported and **never clamped** (the prior-residual census's rule: the deflation
   IS the measurement), but tagged, counted, and called out in a run-end WARN.  `absent_sigma0_zero`
   catches only the exactly-zero case; this catches the dangerous middle, which on the `raimwarm` arm
   reached **σ̂₀ = 5.7e-12 — eleven decades of silent MDB deflation, had it gone untagged.**
6. **σ̂₀,f comes from the P1-4 post-fit pass, per class.**  §11.1 says "the census / M2 estimator"; those
   measure CARRY-PRIOR dispersion, not observation-row dispersion — the wrong instrument for a
   row-level MDB.  `network.trace_postfit_sigma0` supplies `ep_sigma0_pf_code` / `_phase` for the same
   epoch (it runs earlier in SB_FRAMEDIAG).  `obs_other` and `iono_sh_row` have no pool there by
   construction and read `absent_no_pool_for_class`.  The loader WARNs at load time when armed without
   the post-fit pass.
7. **Two source flags, not one master.**  `mdb_from_raim` / `mdb_from_census` are resolved in
   `NetworkKnobs::snapshot` so the run-end lines can say WHICH feed was inert — "the RAIM pool never
   ran warm" and "no sweep completed" demand different fixes.
8. **The epoch accumulator is STAMPED, not driver-cleared.**  The RAIM feed is in SB_RAIM and the
   census feed in SB_FRAMEDIAG; a feed whose TOW differs resets first.  An epoch that fed rows and
   never reached the flush (withhold, failed solve) leaks nothing into the next epoch.

#### ★ FINDING — the RAIM feed site has no domain on a `carry_invariant` lineage

Post-fit RAIM's Pass-1 walk iterates `R.pl_amb`, and `pl_amb` is populated only when `carry_invariant`
is **OFF** (`ApplicationNetworkAssembly.cpp`, `do_amb_carry` — the DD invariant carry supersedes the
phase-leveling ambiguity carry as a solve input).  So on the standard armed set the post-fit RAIM pool
is empty and `network.raim_redundancy_normalize` (B2) is inert with it — a pre-existing property of
the lineage, surfaced by this card's invariant-#4 counter and now named in its run-end WARN.  **On a
`carry_invariant` run the redundancy census is the ONLY rᵢ source**, which is why §11.4's loader check
accepts either.

#### Smoke gate (FT63, 63 stations, DOY-129, COLD, Release)

Arms `runs/ft63_mdb_{off,on,raimwarm,raimlive}`, configs `config/test_suite/_ft63_mdb_*.json`, all
derived from `_ft63_redund_on.json`.

- **OFF-identity PASS** (`off` vs `on`, same build, single-knob delta).  **38 of 44** shared artifacts
  byte-identical, including `network_solution.csv` and every product / QC / epoch-status trace.  The 6
  that differ: the two config and run logs, `network_events.csv` (every delta an `MDB`-tier row),
  `network_run_manifest.json` (exactly the three conditional `network.mdb*` keys + the config
  path/hash), and two wall-clock fields.  One added file: `network_mdb.csv`.  **The solve is
  byte-identical**, i.e. §11.6.4.
- **Live wiring, census feed.**  11 flush passes, 11 carrying rows, **35 948 rows binned, 12 623
  uncheckable (35.1 %)**, 0 skipped for an unusable weight.
- **Live wiring, RAIM feed** (`raimlive`, `carry_invariant` off + phase leveling — the only
  configuration in which the pool has a domain; NOT a byte-identity arm).  **85 569 rows binned,
  20 300 of them from the RAIM pool**, join still reconciles ⇒ the two feeds do not contaminate each
  other's domain.  The `raimwarm` arm (carry_invariant ON) contributed **0** RAIM rows **and said so**
  — the counter is the telemetry, per invariant #4.
- **Headline table**, `raimlive`, one converged-ish epoch (TOW 259500):

  | const/class | rows | ∞ | med formal | p95 | max | σ̂₀,f | med scaled |
  |---|---|---|---|---|---|---|---|
  | G / obs_code | 1436 | 4 | 2.24 m | 6.16 m | 9.03 m | 4.195 | **9.40 m** |
  | G / obs_phase | 2566 | 25 | 0.149 m | 0.541 m | 9.03 m | 0.107 ⚠deflating | 0.016 m |
  | G / obs_other | 9 | 0 | 1.79 m | 4.09 m | 4.09 m | — | absent |
  | ? / iono_sh_row | 354 | 0 | 4.25 m | 4.47 m | 4.59 m | — | absent |

  The **code row is the quotable number**: a fault smaller than ~2.2 m (formal) / **9.4 m (scaled)**
  can hide in the median GPS code row of this network.  The formal/scaled gap is **4.2×** — the
  measured honesty margin on this window, below the O(10×) D5/M2 expectation, and on a COLD 11–20 epoch
  start where σ̂₀ itself is not converged.  **The phase row is NOT quotable**: its σ̂₀ deflates
  (deviation 5).
- **★ Uncheckable fraction is a LINEAGE property, and MDB is the first instrument that says so in fault
  units.**  `carry_invariant` ON: **79.2 %** of phase rows uncheckable.  OFF + phase leveling:
  **10.8 %**.  Same network, same day — the leveled ambiguity carry buys real row redundancy.
- **★ Cost, MEASURED at 63 stations, stride 1** (worst case; recommended stride 10).  Flush 4.0 ms over
  11 epochs = **0.36 ms/epoch**; the per-row rider inside the census sweep is the census-wall delta,
  3559.5 → 3625.5 ms = **6.0 ms/epoch**, i.e. **1.9 % of that seam's own 324 ms/epoch**.  Total
  ≈ 6.4 ms/epoch — **ABOVE §11.3's 0.2 ms budget and above its 1 ms "the implementation is wrong"
  bound.**  §11.3 costed ONE DIVISION; the dominant term is instead the `push_back` of ~3 270
  doubles/epoch into the distribution pools the median and p95 require, which cannot be removed without
  removing the order statistics that ARE the card's product.  Per the spec's own instruction **no
  stride knob was added**: at `redundancy_census_stride: 10` it amortizes to ~0.6 ms/epoch against a
  host seam costing 324 ms/epoch on the same schedule.  §14.5.

#### Scale gate (AGU305, 303 stations, DOY-129, COLD, Release, 2026-08-04)

Arm `runs/agu305_mdb`, config `config/test_suite/_agu305_mdb.json` (extends `_agu305_gps_smoke`):
GPS-only, lane layer OFF (the AGU-26 measured regime), 900 s = **31 epochs, all emitted**, census
stride 5 ⇒ 7 sweeps, `carry_invariant` ON so the census is the only rᵢ source.  **ON arm only — no OFF
control this pass** (user directive), so every OFF-vs-ON claim is inherited from the FT63 gate and the
per-row rider's cost is BOUNDED, not measured.

- **Wiring live at scale, join holds.**  31 flush passes / 7 carrying rows, **105 693 rows binned**, 0
  skipped for an unusable weight; the uncheckable JOIN **RECONCILED on all 7** census epochs.  Census
  closure worst slack **+109.4** (must be ≥ 0).  No new WARN/ERROR class — the only errors are the
  pre-existing 305-path TraceLogger message (present in `runs/agu305_gps_{smoke,1h}` from before this
  card) and the usual uncalibrated-radome / `addStation` rejections.
- **The two thresholds, concretely**: MDB counted **39 645** rows at ∞ (r ≤ 1e-6); the census counted
  **46 773** uncheckable (r < 0.05) over the same rows.  A strict subset — **an equality assertion would
  have FAILED here** (§14.1).
- **Headline, last swept epoch (TOW 260100):**

  | const/class | rows | ∞ | med formal | p95 | max | σ̂₀,f | med scaled |
  |---|---|---|---|---|---|---|---|
  | G / obs_code | 6634 | 16 | 2.82 m | 8.19 m | 22.9 m | 3.074 | **8.67 m** |
  | G / obs_phase | 6642 | 5617 | 5.00 m | 8.08 m | 22.7 m | 7.8e-3 ⚠deflating | 0.039 m |
  | G / obs_other | 19 | 0 | 2.61 m | 7.74 m | 7.74 m | — | absent |
  | ? / iono_sh_row | 1731 | 0 | 4.60 m | 5.07 m | 6.47 m | — | absent |

- **★★ THE RESULT: network size buys almost no per-row reliability.**  Same day, same lineage,
  63 → 303 stations moves the median GPS code row's MDB by ~5 %:

  | | 63 sta (`ft63_mdb_on`) | 303 sta (`agu305_mdb`) |
  |---|---|---|
  | code med MDB, formal | 2.97 m | **2.82 m** |
  | code med MDB, scaled | 9.78 m (σ̂₀ 3.29) | **8.67 m** (σ̂₀ 3.07) |
  | code rows uncheckable | 0.3 % | 0.2 % |
  | phase rows uncheckable | 79.2 % | **84.7 %** |

  Adding 240 stations does not make an individual code row meaningfully more checkable, and it makes
  the phase rows LESS so.  That is what rᵢ being a **LOCAL** property predicts — a row's redundancy is
  set by its own station's geometry and by how many shared parameters its information has to support,
  and both grow together.  **The win from scale is in the SHARED parameters, not in row-level internal
  reliability.**
- **σ̂₀,code is still converging over the window** (2.43 → 3.07 across the 7 sweeps), so the scaled column
  is trending UP, not settling — another reason §11.6.3 is still owed.
- **Cost, isolated WITHOUT an OFF control** by differencing the D0b `frame-diag` section between sweep
  and non-sweep epochs (the census + MDB rider + MDB flush are the only stride-gated occupants):
  sweep epochs **2569 ms** mean vs non-sweep **127 ms** ⇒ **2442 ms per sweep epoch**, i.e. **551
  ms/epoch amortized at stride 5 = 1.24 % of the 44.6 s/epoch loop**.  Within that, the MDB FLUSH is
  13.8 ms over 7 sweeps = **2.0 ms per sweep epoch (0.08 %)**.  The per-row rider is **not separable**
  from the census's own 8.17 ms/station-sweep without an OFF arm; scaling the FT63 measurement (+1.9 %
  of the host sweep) puts it at ~50 ms/sweep-epoch, ~11 ms/epoch amortized — an **EXTRAPOLATION,
  flagged as such**.  The "≤2 % of the host sweep" restatement (§14.5) holds at 303 stations.

#### Outstanding (NOT covered by these smokes) → §15

---

