# SPEC — station congruence screen (coordinate-fault detection in position space)

**Status**: SPEC'd 2026-08-04. **INC-0..3 BUILT 2026-08-04, unit-gated only — every
§7 field gate is OWED.** See §11 for the build record, the three deviations from
this document's literal recipe, and what a reader must not conclude from a green
unit suite.
**Origin**: deformation-analysis canon in the NGS LASER Guide's own reference list
(Caspary 2000, *Concepts of Network and Deformation Analysis*) composed with the
guide's Baarda material (5-97..5-101): test a station's coordinates against its
residuals — the congruence question "has this point moved relative to its held
position", asked per station, per sweep. Multi-dimensional outlier form per Snow &
Schaffrin (2003), same family as BlockFormal.
**Memory context**: `project_network_ssystem_dia_assessment` (critic blind spot:
*"fixed station coordinates are un-QC'd; median polish removes station effects ⇒
coordinate faults are INVISIBLE"* — this card is that item),
`project_block_baarda_card` (kernel contracts: NO diagonal clamp, no carry_aware —
both inherited), `project_antenna_eccentricity_arp_frame` (JPLM 6.10 cm — the
historical instance of exactly this fault class), `project_why_non_integer_rootcause`
(D1: correlated errors do not average as white — §5's accumulation rule),
`docs/SPEC_block_baarda.md` §12 (build record this card rides on).
**Scope discipline**: default-OFF, OFF byte-identical, MEASURE-ONLY, fingerprint-SAFE.

---

## 1. Why — the one fault class no armed tier can see

Station coordinates are HELD in the network producer. A wrong held coordinate (survey
blunder, ARP/eccentricity mistake, monument motion) projects into every row of that
station through the LOS geometry — and every armed tier is blind to it by
construction: median polish removes station common effects, per-row w-tests see only
the post-absorption crumbs, the (station, sat) block test pools one LOS (a coordinate
error is spread across ALL LOS), and the admission screen tests observations, not
geometry. JPLM's 6.10 cm ARP error is the measured precedent (found by hand, not by
QC; the ARP-frame card fixed that instance, not the class).

The congruence screen closes the class: estimate, from the station's own post-fit
residuals, the 3D coordinate correction that would best explain them, and test it
against zero — with its formal detectability (a per-station, per-direction coordinate
MDB) reported alongside, because on some lineages the honest answer is "this screen
cannot see below X cm in U", and that number is a deliverable, not an embarrassment.

## 2. The statistic

For station i on a swept epoch, let B = its `obs_code` + `obs_phase` rows
(`NetworkRowClassify` classes 0–1; SH tie rows have no position dependence), ẽ_B the
post-fit residuals, P_B = diag(w), Q_ẽ,B the b×b residual cofactor
(`NetworkNormalEquations::blockResidualCofactor`, `NetworkNormalEquations.hpp:997` —
takes an arbitrary row set of ONE station; b here is 30–80, not the LOS card's ≤ 8),
and H_B the b×3 position Jacobian in ENU (unit LOS per row, sign per row type).

```
M   = H_Bᵀ P_B Q_ẽ,B P_B H_B                        3×3 information of the offset
δ̂  = M⁻¹ H_Bᵀ P_B ẽ_B                               BLUE of a station coordinate offset
T_i = δ̂ᵀ M δ̂ / (3 · σ̂₀,f²)                          per-dof, empirically re-scaled
MDB_k = δ₀ · σ̂₀,f · √((M⁻¹)_kk)                      per-direction coordinate MDB (m)
```

- **M is eigen-guarded, never blind-inverted.** With per-epoch station clock and ZWD
  estimated, the U direction's information can collapse (clock/tropo absorb vertical);
  a near-null eigenvalue reports that direction UNOBSERVABLE with MDB = ∞ — the
  `BaardaReliability.hpp` `kInf` convention — and δ̂/T are formed in the observable
  subspace. The absorption is thereby measured, not worked around.
- **Phase rows need no special casing.** Under `carry_invariant` their Q_ẽ rows are
  ≈ 0 (free ambiguities), so they contribute ≈ nothing to M and the screen is honestly
  CODE-grade (expect dm-class MDB); on a leveled lineage the phase tier engages and
  MDB drops toward cm-class. The MDB table says which regime the run was in.
- σ̂₀,f: a `NetworkCarryVce` instance fed the χ²₃-equivalent of T_i via the block
  card's `blockChi2GeoMean` rescale at b = 3 (that correction exists precisely so
  sibling cards do not repeat the χ²₁ bias), predict-then-update, never censored,
  `seed_floor_m2 = 1.0` passed explicitly (block card correction 2).
- δ₀ from `BaardaReliability.hpp` — one convention across PPP, MDB, and this card.

## 3. Design

- **INC-0: per-row LOS retention.** `StationAssembly` (`NetworkRunTypes.hpp:362`)
  carries no per-row geometry. Add `row_los` (b×3 unit LOS, ENU) + per-row satellite
  id, populated at assembly where geometry is in hand
  (`ApplicationNetworkAssembly.cpp`), **armed-only** (empty vectors otherwise — the
  byte-identity A/B must show OFF unchanged). ~24 B/row when armed. Sign conventions
  per row type are unit-tested against the measurement model, not assumed.
- **INC-1: the sweep rider.** Enumerate swept stations inside
  `NetworkRedundancyCensus.cpp` (stride/escalation inherited; the block card already
  threaded `x_s` for post-fit residuals). Per station: build B, call
  `blockResidualCofactor`, contract to M (implementation free to avoid materializing
  the full b×b if the contraction can reuse the kernel's products — the budget is the
  contract, not the route), form δ̂/T/MDB. Accumulator in
  `include/filter/NetworkCongruence.hpp`, emission + run-end in
  `src/core/filter/NetworkCongruence.cpp` (+ CMake).
- **INC-2: accumulation across epochs — honestly.** Per station keep the running
  MEDIAN and MAD of δ̂ components over swept epochs, plus the T ladder (census bins)
  split CLEAN vs FRAME (datum-changed epochs counted separately, invariant #7). **No
  √N averaging claims anywhere** — D1 measured K_eff(30) ≈ 2 on this network; the
  median/MAD pair is the D-family robust convention and the per-epoch scatter IS the
  reported uncertainty.
- **INC-3: separability, measured from day one.** Report, per flagged station-epoch,
  whether block-Baarda flagged any single LOS of the same station (a coordinate fault
  is ALL-LOS coherent; a single-LOS fault should excite the block tier, not this one).
  The 2×2 co-occurrence table is this card's contingency deliverable.

## 4. Cost

Per swept station: one b×b cofactor + a 3-column contraction + a 3×3 eigensolve.
Budget: **≤ 2× the redundancy sweep it rides** (the block card's own bound), measured
per station-sweep; stride inherited; no new scheduler and no stride knob.

## 5. Config

| knob | type | default | note |
|---|---|---|---|
| `network.congruence_screen` | bool | `false` | master; requires `redundancy_census` (sweep host) — hard error otherwise |
| `network.congruence_report_sigma` | double | `4.0` | T-ladder trace/event threshold; reporting only |
| `network.congruence_event_holdoff_ep` | int | `20` | PREFIT/MDB rate-limit convention |

Loader: next to the redundancy-census/block-Baarda block. Invariant-#4 tripwires: WARN
if armed and zero stations swept; run-end distinguishes "swept, all quiet" from "never
ran". Fingerprint-SAFE.

## 6. Unit tests

New file `tests/test_station_congruence.cpp`, one suite.

1. **Recovery.** Toy network, +5 cm East injected into one station's held coordinates:
   δ̂_E ≈ +0.05 m (sign convention pinned), T loud, every other station quiet —
   detection AND localization in one test.
2. **Absorption.** Same toy with station clock + ZWD estimated: M's U eigenvalue
   collapses, MDB_U ≫ MDB_E/N, U reported unobservable rather than inverted through.
3. **Separability.** A single-LOS fault (block-Baarda's smearing scenario) produces
   small T_i while the block statistic is loud; the coordinate offset produces the
   converse. The two tiers partition the fault classes.
4. Kernel contraction equals brute-force dense GLS on the toy to 1e-10.
5. χ²₃ null ladder once σ̂₀ converged; Student-t over-occupancy (D5 negative preserved).
6. CLEAN/FRAME partition; median/MAD accumulation; OFF byte-identity (structural).

## 7. Field validation

1. OFF byte-identity (same-build A/B, single-knob delta); live wiring; cost within
   budget.
2. **★ THE KNOWN ANSWER — the perturbed arm.** A dedicated diagnostic config offsets
   ONE station's held coordinates by +5 cm East (the input edit recorded in the arm's
   README; diagnostic lineage only — the perturbation is solve-affecting by intent,
   so nothing from this arm is graded except the screen). The screen must localize:
   the perturbed station's run-median δ̂_E ≈ +5 cm with T persistent, all other
   stations' ladders unshifted vs the unperturbed control. Run BOTH at the same
   stride so the comparison is like-for-like.
3. **The MDB table** (converged warm window, unperturbed): per-station ENU coordinate
   MDB, split by lineage regime (`carry_invariant` code-grade vs leveled). This is the
   card's service deliverable: the smallest coordinate fault this network can see,
   per station, per direction — and for U on most stations the honest number may be
   "large"; report it, do not tune it away.
4. **Separability co-occurrence** (INC-3) on the same window.
5. Solve byte-identical in every non-perturbed arm; product claims: none (measure-only).

## 8. Deferral triage (invariant #3)

| deferred | field-blocking? | evidence |
|---|---|---|
| Any response (deweight/exclude/re-coordinate) | **INERT** — measure-only; a response needs §7.2/7.3 first, and re-coordinating a station is a DATUM act (invariant #7 both edges) belonging to its own card | — |
| Formal multi-epoch congruence test (time-correlated δ̂ accounting) | **INERT** — median/MAD reports the same physics without a false √N claim | D1, K_eff ≈ 2 |
| Leave-one-station-out solution separation | **INERT** — own card (additive normals make it cheap); this screen is the cheaper first tier | DIA catalog item 5 |
| Velocity / epoch-to-epoch deformation (true Caspary congruence between campaigns) | **INERT** — needs two adjudicated epochs; the within-run screen is the prerequisite instrument | — |

## 9. Standing-invariant pre-flight

1. Adds no response; measures a fault class with no existing response. 2. Release-only.
3. Deferrals above. 4. Per-station ladders + `cg_*` counters are the telemetry;
"swept, quiet" ≠ "never ran". 5. Residuals are unique under the minimal pin datum
(LASER 5-65), so the statistic is datum-invariant; FRAME epochs are still split out.
6. Default-OFF byte-identical; same-build A/B; the perturbed arm is declared
diagnostic-only. 7. No coupling change; FRAME counted at both edges via the split.

## 10. What this card explicitly does NOT do

- It does not move, deweight, or re-estimate any station coordinate.
- It does not claim cm-class detection on a `carry_invariant` lineage — the MDB table
  states the actual power, and code-grade is the expected v1 answer.
- It does not replace block-Baarda or median polish; INC-3 measures the partition.
- It does not perform campaign-to-campaign deformation analysis (§8, last row).

---

## 11. Build record — INC-0..3, 2026-08-04

**Unit-gated only. Every §7 field gate is OWED.** `gnss_tests` 1248 cases / 182
suites green (1 pre-existing fixture-gated skip); 14 new cases in one new suite,
`StationCongruenceTest`.

### Where it lives

| piece | file |
|---|---|
| INC-0 row geometry | `StationAssembly::row_pos_enu` (`include/filter/NetworkRunTypes.hpp`), populated armed-only in `src/core/app/ApplicationNetworkAssembly.cpp` right after `networkSplitStation_` |
| statistic + accumulator | `include/filter/NetworkCongruence.hpp` (header-only numeric leaf) |
| sweep / emission / run-end | `src/core/filter/NetworkCongruence.cpp` (+ CMake) |
| feed site | the SWEPT branch of `src/core/filter/NetworkRedundancyCensus.cpp`, sharing the block card's one `reconstructStation` |
| knobs | `network.congruence_screen` / `_report_sigma` / `_event_holdoff_ep`, parsed next to the block census in `GnssConfigLoadNetwork.cpp` |
| trace | `network_congruence.csv`, one row per SWEPT STATION per epoch |
| unit gate | `tests/test_station_congruence.cpp` |

### Three deviations from §2–§3's literal recipe, all load-bearing

1. **`Q_ẽ,B` is never inverted, so there is no UNCHECKABLE population.** The
   contraction `H_BᵀP Q P H_B` is FORWARD. A rank-deficient block — the structural
   state of a cold start, where every free ambiguity zeroes its own arc's phase
   residuals — costs nothing here and needs no special case. This is a genuine
   simplification over the block card, whose `T_B` needs `Q⁻¹`. The degeneracy that
   does matter is pushed into the 3×3 eigenproblem, where it is named per direction.
2. **`T_i` divides by `q`, not by 3.** §2 writes `3·σ̂₀²`. When the geometry is
   full-rank those are the same number; when it is not, dividing a rank-2 station's
   statistic by 3 would read it a third too quiet forever. `CongruenceSolve::dof`
   carries `q` so a reader can always tell which regime a row is in.
3. **`row_pos_enu` is the JACOBIAN lifted verbatim out of `em.H`, not a re-derived
   LOS with "sign per row type".** `em.H` and `em.innovations` are ONE
   linearization, so `y ≈ H δx` holds row by row for every observable the
   measurement model can produce. Re-deriving a sign per row class would be a
   second source of truth waiting to disagree. The sweep negates it once, to put
   δ̂ in the held-coordinate-error convention §6.1/§7.2 assert.

Plus one convention this document left implicit and the code pins: **σ̂₀ is ONE
network-wide family.** A per-station σ̂₀ would let the faulty station calibrate its
own fault away; a per-constellation split is not available because the statistic
spans every constellation the station tracks. The block card's two `NetworkCarryVce`
corrections are inherited verbatim (χ²_q → χ²₁ equivalent via `blockChi2GeoMean`,
`seed_floor_m2 = 1.0` passed explicitly).

### Three things the toy network measured that the SPEC did not anticipate

These are properties of the INSTRUMENT, found while building the unit gate, and
they bound what §7 can be expected to show:

- **Localization needs an over-determined satellite-clock block.** At 3 stations a
  5 cm East offset leaked 37 %/50 % of itself onto the two neighbours, with a
  *larger* `T` at the unperturbed stations than at the perturbed one — the shared
  clocks absorb the fault and re-emit it everywhere. The leakage floor is ≈ 1/N.
  63- and 305-station lineages are fine; a small-network reading is not.
- **The two tiers' separability scales as kS².** The 3-D coordinate subspace
  occupies 3 of a station's kS line-of-sight directions, so a coordinate fault's
  share of any one block falls as 1/kS while a single-LOS fault's projection onto
  the coordinate subspace falls as 3/kS. Measured at kS = 6: a single-LOS slip put
  **59 %** of its residual energy in the coordinate subspace. §7.4's co-occurrence
  table must be read against the epoch's satellite count.
- **A single-satellite RANGE bias is genuinely confounded with a coordinate error**
  at small kS — real physics, not a weakness of either tier, and what M's eigen
  structure and the MDB exist to report. The unit gate's negative control is
  therefore a phase-only (slip-shaped) LOS fault, which is the block card's own
  discriminating class.

### First wiring smoke — RUN, 2026-08-04 (§7.1 only)

`config/test_suite/_congruence_smoke_{on,off}.json`, a single-knob delta over
`_ft63_redund_on.json` (63 stations, GPS, 300 s = 11 epochs, census stride 1,
block census OFF), same Release exe for both arms.

- **Live wiring.** 11 emission passes, 11 with a swept station, **692
  station-sweeps over 31 674 data rows** (15 808 code / 15 866 phase); 0 rows
  without usable geometry, 0 stations with no post-fit residual, 0 with no
  geometric row, 0 whose 3×3 information yielded no statistic. 366 reported,
  63 events. `network_congruence.csv`: 692 rows + header.
- **Cost — INSIDE §4's budget.** **1.86 ms per station-sweep** against the
  redundancy census's 6.62 ms (which *includes* this rider; the host alone is
  ≈ 4.8 ms). 0.28–0.39× the sweep it rides, against a bound of 2×.
- **§7.1 OFF byte-identity: PASS.** Same build, single-knob delta, 11
  solve-derived traces SHA-256 identical (`network_solution`, `dd_fractional`,
  `pf_keyed`, `vtec_sh`, `epoch_status`, `prior_residual`, `if_slip`, `slip_raw`,
  `wl_lane_trace`, `d5_flags`, `stations`). The OFF arm emitted zero congruence
  log lines and never opened the trace stream.
- **⚠ Nothing else in this run is a finding.** It is an 11-epoch COLD start on a
  `carry_invariant` lineage: run-median δ̂ ran to hundreds of metres, σ̂₀ = 34.75,
  MDB 1.3–46 m. That is the instrument reporting an unconverged solution honestly,
  which is exactly why §7.3 says "converged warm window". A clean-looking MDB
  table here would have been the alarming outcome.
- One reading worth carrying into §7.3: **0 of 692 station-epochs were degenerate**
  — the vertical is weak (MDB_U 8–46 m against MDB_E 1.3–5.8 m, and `null_dir`
  sits at ≈ (0, −0.13, 0.99), i.e. Up as predicted) but never exactly collapsed on
  this geometry. The absorption shows up as a large finite MDB rather than as an
  unobservable direction.

### What is still OWED

§7.2 (the perturbed arm — the KNOWN ANSWER), §7.3 (the MDB table on a converged
warm window, both lineage regimes), §7.4 (INC-3 co-occurrence — it needs
`network.block_baarda` armed alongside; the smoke ran with it off and the run-end
line correctly reports the join as UNMEASURED rather than as agreement), and §7.5
on a graded lineage rather than an 11-epoch smoke.

No `docs/CONFIG_promotion_ledger` row and no `logNetworkPosture_` row: the knob is
UNVALIDATED, and the ledger recommends only what has been measured in a named
regime.
