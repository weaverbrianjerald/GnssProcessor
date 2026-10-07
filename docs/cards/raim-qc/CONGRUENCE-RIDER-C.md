# Rider C: the station congruence screen (`network.congruence_screen`)

**STATUS**

- **id:** Rider C
- **topic:** raim-qc
- **status:** closed
- **summary:** BUILT 2026-08-04, measure-only; the wiring smoke PASSED OFF byte-identity, the other field gates are owed, and later runs show it DETECTS held-coordinate errors but cannot MEASURE them.
- **dates in the text:** 2026-08-04 to 2026-08-15
- **knobs / posture:** `network.congruence_screen`, `network.congruence_report_sigma`, `network.congruence_event_holdoff_ep`, `network.block_baarda`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_redundancy_census.md`: 13, 13.1, 13.2, 13.3, 13.4, 13.5, 13.6, 13.7, 13.8, 13.11

**Outcome.** The question: can a station's own post-fit residuals reveal a wrong held coordinate, a fault no other armed tier sees? The 63-station smoke met §13.7.1 only (11 solve traces identical, 1.86 ms per station-sweep, inside the 2× budget); the +5 cm known-answer arm, the MDB table and the co-occurrence table stay owed ([OWED ledger](../raim-qc/REDUNDANCY-CENSUS.md) §15, rows 5 to 8). Armed in the re-founding's runs, it ranked the roster's real coordinate errors and flagged MIZU00JPN at 22 to 29 against a median of 0.9, unread; on the float vehicle its estimate does not carry the error's size ([UPO](../../refound/UPO_consumer_up_offset_2026-09-22.md) §6.2, [SCU](../../refound/SCU_station_coordinates_unknowns_2026-09-22.md) §4).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_redundancy_census.md -->
## 13. RIDER C — station congruence screen (`network.congruence_screen`)

*Absorbed from `docs/attic/SPEC_station_congruence.md` (SPEC'd + built 2026-08-04).  Increments
namespaced **CG-INC-0..3** (absorbed labels: that card's `INC-0..3`, `INC-1..3`).*

**Origin**: deformation-analysis canon in the NGS LASER Guide's own reference list (Caspary 2000,
*Concepts of Network and Deformation Analysis*) composed with the guide's Baarda material
(5-97..5-101): test a station's coordinates against its residuals — the congruence question "has this
point moved relative to its held position", asked per station, per sweep.  Multi-dimensional outlier
form per Snow & Schaffrin (2003), same family as BlockFormal.

### 13.1 Why — the one fault class no armed tier can see

This card IS the DIA-catalog blind-spot item `project_network_ssystem_dia_assessment` names verbatim:
*"fixed station coordinates are un-QC'd; median polish removes station effects ⇒ coordinate faults are
INVISIBLE"*.

Station coordinates are HELD in the network producer.  A wrong held coordinate (survey blunder,
ARP/eccentricity mistake, monument motion) projects into every row of that station through the LOS
geometry — and **every armed tier is blind to it by construction**: median polish removes station
common effects, per-row w-tests see only the post-absorption crumbs, the (station, sat) block test
pools ONE LOS while a coordinate error is spread across ALL LOS, and the admission screen tests
observations, not geometry.  **JPLM's 6.10 cm ARP error is the measured precedent** — found by hand,
not by QC; the ARP-frame card fixed that instance, not the class.

The screen closes the class: estimate, from the station's own post-fit residuals, the 3D coordinate
correction that would best explain them, and test it against zero — with its formal detectability (a
per-station, per-direction coordinate MDB) reported alongside, because on some lineages the honest
answer is "this screen cannot see below X cm in U", and **that number is a deliverable, not an
embarrassment.**

### 13.2 The statistic

For station i on a swept epoch, let B = its `obs_code` + `obs_phase` rows (`NetworkRowClassify` classes
0–1; SH tie rows have no position dependence), ẽ_B the post-fit residuals, P_B = diag(w), Q_ẽ,B the
b×b residual cofactor (`NetworkNormalEquations::blockResidualCofactor`,
`NetworkNormalEquations.hpp:997` — takes an arbitrary row set of ONE station; **b here is 30–80, not
the LOS card's ≤ 8**), and H_B the b×3 position Jacobian in ENU.

```
M   = H_Bᵀ P_B Q_ẽ,B P_B H_B                        3×3 information of the offset
δ̂  = M⁻¹ H_Bᵀ P_B ẽ_B                               BLUE of a station coordinate offset
T_i = δ̂ᵀ M δ̂ / (q · σ̂₀,f²)                          per-dof, empirically re-scaled
MDB_k = δ₀ · σ̂₀,f · √((M⁻¹)_kk)                      per-direction coordinate MDB (m)
```

- **M is eigen-guarded, never blind-inverted.**  With per-epoch station clock and ZWD estimated, the U
  direction's information can collapse (clock/tropo absorb vertical); a near-null eigenvalue reports
  that direction UNOBSERVABLE with MDB = ∞ (the `BaardaReliability.hpp` `kInf` convention) and δ̂/T are
  formed in the observable subspace.  **The absorption is thereby measured, not worked around.**
- **Phase rows need no special casing.**  Under `carry_invariant` their Q_ẽ rows are ≈ 0 (free
  ambiguities), so they contribute ≈ nothing to M and the screen is honestly CODE-grade (expect
  dm-class MDB); on a leveled lineage the phase tier engages and MDB drops toward cm-class.  **The MDB
  table says which regime the run was in.**
- σ̂₀,f: a `NetworkCarryVce` instance fed the χ²₃-equivalent of T_i via `blockChi2GeoMean` at b = 3, with
  `seed_floor_m2 = 1.0` passed explicitly — both corrections inherited verbatim from rider B, stated
  once in **§14.2**.  δ₀ from `BaardaReliability.hpp` — one convention across PPP, MDB and this card.
- ⚠ **T_i divides by `q`, the rank, not by 3** — deviation 2.  **σ̂₀ is ONE NETWORK-WIDE FAMILY** —
  §13.11.

### 13.3 Design

- **CG-INC-0: per-row LOS retention.**  `StationAssembly` (`include/filter/NetworkRunTypes.hpp:432`;
  the SPEC wrote `NetworkRunTypes.hpp:362`) carried no per-row geometry.  The SPEC asked for a member
  `row_los` (b×3 unit LOS, ENU) + per-row satellite id, populated at assembly where geometry is in hand
  (`ApplicationNetworkAssembly.cpp:1922`, `:2209` — re-grepped 2026-08-15; round 1 wrote
  `ApplicationNetworkAssembly.cpp:1921` / `:2181`), **armed-only** (empty otherwise — the byte-identity
  A/B must show OFF unchanged), ~24 B/row when armed, with **sign conventions per row type unit-tested
  against the measurement model, not assumed**.  ⚠ As landed the member is
  `StationAssembly::row_pos_enu` (`NetworkRunTypes.hpp:558`; round 1 wrote `:551`, which is inside its
  doxygen block) and it is the JACOBIAN lifted out of `em.H`, not a re-derived LOS — deviation 3 — so
  the per-row-type sign unit test was dropped along with the re-derivation it would have checked.
  ⚠ These two files are under concurrent edit; re-grep by symbol, never by line.
- **CG-INC-1: the sweep rider.**  Enumerate swept stations inside `NetworkRedundancyCensus.cpp`
  (`:225`, `:499` — stride/escalation inherited; rider B already threaded `x_s` for post-fit
  residuals).  Per station: build B, call `blockResidualCofactor`, contract to M, form δ̂/T/MDB.
  Accumulator in `include/filter/NetworkCongruence.hpp`, emission + run-end in
  `src/core/filter/NetworkCongruence.cpp` (+ CMake).
- **CG-INC-2: accumulation across epochs — honestly.**  Per station keep the running **MEDIAN and
  MAD** of δ̂ components over swept epochs, plus the T ladder (census bins) split CLEAN vs FRAME
  (datum-changed epochs counted separately, invariant #7).  **No √N averaging claims anywhere** — D1
  measured K_eff(30) ≈ 2 on this network; the median/MAD pair is the D-family robust convention and
  **the per-epoch scatter IS the reported uncertainty**.
- **CG-INC-3: separability, measured from day one.**  Report, per flagged station-epoch, whether
  block-Baarda flagged any single LOS of the same station (a coordinate fault is ALL-LOS coherent; a
  single-LOS fault should excite the block tier, not this one).  The 2×2 co-occurrence table is this
  card's contingency deliverable.

### 13.4 Cost

Per swept station: one b×b cofactor + a 3-column contraction + a 3×3 eigensolve.  Budget: **≤ 2× the
redundancy sweep it rides** (rider B's bound), measured per station-sweep; stride inherited; no new
scheduler and no stride knob.  **MEASURED INSIDE BUDGET — §13.11.**

### 13.5 Config

`network.congruence_screen` (master; **hard error** without `redundancy_census`, the sweep host),
`network.congruence_report_sigma` (T-ladder trace/event threshold, reporting only),
`network.congruence_event_holdoff_ep` (the PREFIT/MDB rate-limit convention).  **Types, defaults and
dependencies: the one table in §14.7.**  Loader `GnssConfigLoadNetwork.cpp:3789-3819`, next to the
redundancy-census/block-Baarda block.  Fingerprint-SAFE.  Invariant-#4 tripwires: WARN if armed and
zero stations swept; run-end distinguishes "swept, all quiet" from "never ran"
(`NetworkTeardown.cpp:1089`).

### 13.6 Unit tests

`tests/test_station_congruence.cpp`, one suite (`StationCongruenceTest`), **14 cases** as landed.

1. **Recovery.**  Toy network, +5 cm East injected into one station's held coordinates: δ̂_E ≈ +0.05 m
   (sign convention pinned), T loud, every other station quiet — detection AND localization in one test.
2. **Absorption.**  Same toy with station clock + ZWD estimated: M's U eigenvalue collapses,
   MDB_U ≫ MDB_E/N, U reported unobservable rather than inverted through.
3. **Separability.**  A single-LOS fault (rider B's smearing scenario) produces small T_i while the
   block statistic is loud; the coordinate offset produces the converse.  The negative control is
   deliberately a **phase-only (slip-shaped)** LOS fault — instrument finding 3.
4. Kernel contraction equals brute-force dense GLS on the toy to 1e-10.
5. χ²₃ null ladder once σ̂₀ converged; Student-t over-occupancy (D5 negative preserved).
6. CLEAN/FRAME partition; median/MAD accumulation; OFF byte-identity (structural).

### 13.7 Field validation

1. **13.7.1** OFF byte-identity (same-build A/B, single-knob delta); live wiring; cost within budget.
   **MET, §13.11.**
2. **13.7.2 ★ THE KNOWN ANSWER — the perturbed arm.  OWED.**  A dedicated diagnostic config offsets ONE
   station's held coordinates by **+5 cm East** (the input edit recorded in the arm's README;
   **diagnostic lineage only** — solve-affecting by intent, so nothing from that arm is graded except
   the screen).  The screen must localize: the perturbed station's run-median δ̂_E ≈ +5 cm with T
   persistent, all other stations' ladders unshifted vs the unperturbed control.  Run BOTH at the same
   stride.
3. **13.7.3 The MDB table.  OWED.**  Converged warm window, unperturbed: per-station ENU coordinate
   MDB, split by lineage regime (`carry_invariant` code-grade vs leveled).  **This is the card's
   service deliverable** — the smallest coordinate fault this network can see, per station, per
   direction — and for U on most stations the honest number may be "large": **report it, do not tune it
   away.**  (The anchor `NetworkTeardown.cpp:1089` cites.)
4. **13.7.4 Separability co-occurrence** (CG-INC-3) on the same window.  **OWED** — needs
   `network.block_baarda` armed alongside.
5. **13.7.5** Solve byte-identical in every non-perturbed arm; product claims: none (measure-only).
   **OWED on a graded lineage.**

### 13.8 Deferral triage (invariant #3)

| deferred | field-blocking? | evidence |
|---|---|---|
| Any response (deweight/exclude/re-coordinate) | **INERT** — measure-only; a response needs §13.7.2/13.7.3 first, and **re-coordinating a station is a DATUM act** (invariant #7 both edges) belonging to its own card | — |
| Formal multi-epoch congruence test (time-correlated δ̂ accounting) | **INERT** — median/MAD reports the same physics without a false √N claim | D1, K_eff ≈ 2 |
| Leave-one-station-out solution separation | **INERT** — own card (additive normals make it cheap); this screen is the cheaper first tier | DIA catalog item 5 |
| Velocity / epoch-to-epoch deformation (true Caspary congruence between campaigns) | **INERT** — needs two adjudicated epochs; the within-run screen is the prerequisite instrument | — |

*(Pre-flight and does-NOT-do: §10.)*

### 13.11 BUILD RECORD — CG-INC-0..3, 2026-08-04

**Unit-gated + wiring smoke.  Every §13.7 field gate except 13.7.1 is OWED.**  `gnss_tests` **1248
cases / 182 suites** green at the time (1 pre-existing fixture-gated skip); 14 new cases in one new
suite, `StationCongruenceTest`.

| piece | file |
|---|---|
| CG-INC-0 row geometry | `StationAssembly::row_pos_enu` (`include/filter/NetworkRunTypes.hpp:558`), populated armed-only in `src/core/app/ApplicationNetworkAssembly.cpp` right after `networkSplitStation_` |
| statistic + accumulator | `include/filter/NetworkCongruence.hpp` (header-only numeric leaf) |
| sweep / emission / run-end | `src/core/filter/NetworkCongruence.cpp` (+ CMake) |
| feed site | the SWEPT branch of `src/core/filter/NetworkRedundancyCensus.cpp`, sharing rider B's one `reconstructStation` |
| knobs | `network.congruence_screen` / `_report_sigma` / `_event_holdoff_ep`, parsed next to the block census in `GnssConfigLoadNetwork.cpp` |
| trace | `network_congruence.csv`, one row per SWEPT STATION per epoch |
| unit gate | `tests/test_station_congruence.cpp` |

#### Three deviations from §13.2–§13.3's literal recipe, all load-bearing

1. **★ `Q_ẽ,B` is NEVER inverted, so there is NO UNCHECKABLE population.**  The contraction
   `H_BᵀP Q P H_B` is FORWARD.  A rank-deficient block — the structural state of a cold start, where
   every free ambiguity zeroes its own arc's phase residuals (§9) — costs nothing here and needs no
   special case.  **A genuine simplification over rider B, whose `T_B` needs `Q⁻¹`.**  The degeneracy
   that DOES matter is pushed into the 3×3 eigenproblem, where it is named per direction.
2. **★ `T_i` divides by `q`, the rank, not by 3.**  §13.2 wrote `3·σ̂₀²`.  Full-rank geometry makes those
   coincide; otherwise dividing a rank-2 station's statistic by 3 would read it **a third too quiet
   FOREVER**.  `CongruenceSolve::dof` carries `q` so a reader can always tell which regime a row is in.
3. **`row_pos_enu` is the JACOBIAN lifted verbatim out of `em.H`, not a re-derived LOS with "sign per
   row type"** (§13.3's `row_los`).  `em.H` and `em.innovations` are ONE linearization, so `y ≈ H δx`
   holds row by row for every observable the measurement model can produce.  Re-deriving a sign per row
   class would be a second source of truth waiting to disagree.  The sweep negates it once, to put δ̂ in
   the held-coordinate-error convention §13.6.1/§13.7.2 assert.

Plus one convention the SPEC left implicit and the code pins: **σ̂₀ is ONE network-wide family.**  A
per-station σ̂₀ would let the faulty station calibrate its own fault away; a per-constellation split is
unavailable because the statistic spans every constellation the station tracks.

#### ★ Three things the toy network measured that the SPEC did not anticipate

Properties of the **INSTRUMENT**, found while building the unit gate; they bound what §13.7 can show.

- **1 — Localization needs an over-determined satellite-clock block.**  At 3 stations a **5 cm East**
  offset leaked **37 % / 50 %** of itself onto the two neighbours, with a **LARGER `T` at the
  UNPERTURBED stations** than at the perturbed one — the shared clocks absorb the fault and re-emit it
  everywhere.  **The leakage floor is ≈ 1/N.**  63- and 305-station lineages are fine; **a
  small-network reading is NOT.**
- **2 — The two tiers' separability scales as kS².**  The 3-D coordinate subspace occupies 3 of a
  station's kS line-of-sight directions, so a coordinate fault's share of any one block falls as 1/kS
  while a single-LOS fault's projection onto the coordinate subspace falls as 3/kS.  Measured at
  kS = 6: a single-LOS slip put **59 %** of its residual energy in the coordinate subspace.  **§13.7.4's
  co-occurrence table MUST be read against the epoch's satellite count.**
- **3 — A single-satellite RANGE bias is genuinely confounded with a coordinate error** at small kS —
  real physics, not a weakness of either tier, and what M's eigen structure and the MDB exist to
  report.  Hence the unit gate's negative control is a phase-only (slip-shaped) LOS fault, rider B's
  own discriminating class.

#### First wiring smoke — RUN, 2026-08-04 (§13.7.1 only)

`config/test_suite/_congruence_smoke_{on,off}.json`, a single-knob delta over `_ft63_redund_on.json`
(63 stations, GPS, 300 s = 11 epochs, census stride 1, block census OFF), same Release exe both arms.

- **Live wiring.**  11 emission passes, 11 with a swept station, **692 station-sweeps over 31 674 data
  rows** (15 808 code / 15 866 phase); 0 rows without usable geometry, 0 stations with no post-fit
  residual, 0 with no geometric row, 0 whose 3×3 information yielded no statistic.  366 reported, 63
  events.  `network_congruence.csv`: 692 rows + header.
- **★ Cost — INSIDE §13.4's budget.**  **1.86 ms per station-sweep** against the redundancy census's
  6.62 ms (which *includes* this rider; the host alone is ≈ 4.8 ms) = **0.28–0.39×** the sweep it
  rides, against a bound of 2×.
- **§13.7.1 OFF byte-identity: PASS.**  Same build, single-knob delta, **11 solve-derived traces
  SHA-256 identical** (`network_solution`, `dd_fractional`, `pf_keyed`, `vtec_sh`, `epoch_status`,
  `prior_residual`, `if_slip`, `slip_raw`, `wl_lane_trace`, `d5_flags`, `stations`).  The OFF arm
  emitted zero congruence log lines and never opened the trace stream.
- **⚠ NOTHING ELSE IN THIS RUN IS A FINDING.**  An 11-epoch COLD start on a `carry_invariant` lineage:
  run-median δ̂ ran to **hundreds of metres**, σ̂₀ = **34.75**, MDB **1.3–46 m**.  That is the instrument
  reporting an unconverged solution honestly, which is exactly why §13.7.3 says "converged warm
  window".  **A clean-looking MDB table here would have been the ALARMING outcome.**
- One reading worth carrying into §13.7.3: **0 of 692 station-epochs were degenerate** — the vertical is
  weak (MDB_U **8–46 m** against MDB_E **1.3–5.8 m**, `null_dir` ≈ (0, −0.13, 0.99), i.e. **Up as
  predicted**) but never exactly collapsed on this geometry.  The absorption shows up as a large FINITE
  MDB rather than as an unobservable direction.

**No `docs/CONFIG_promotion_ledger` row and no `logNetworkPosture_` row**: the knob is UNVALIDATED, and
the ledger recommends only what has been measured in a named regime.

---

