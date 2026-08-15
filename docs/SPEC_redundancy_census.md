# SPEC — redundancy / testability census (the statistical parked-garbage monitor) + its three riders

**Status**: SPEC'd 2026-08-01; host built + smoke-gated 2026-08-01 (§8).  **Three riders absorbed
2026-08-15** — they share this sweep's scheduler, kernel and conventions and were four files
describing one mechanism.  The absorbed docs now live in `docs/attic/`.

| rider | knob | was | landed | field state |
|---|---|---|---|---|
| **A — Baarda MDB reliability** | `network.mdb` | `docs/attic/SPEC_network_mdb_reliability.md` | §11.10, 2026-08-04 | 63-sta + 303-sta smoke PASS; converged table OWED |
| **B — BlockFormal LOS-block census** | `network.block_baarda` | `docs/attic/SPEC_block_baarda.md` | §12.12, 2026-08-04 | unit-gated only, **NO FIELD RUN** |
| **C — station congruence screen** | `network.congruence_screen` | `docs/attic/SPEC_station_congruence.md` | §13.11, 2026-08-04 | wiring smoke PASS; every field gate OWED |

**Label namespacing**: each card used `INC-0..3` independently, so the riders' increments are
prefixed — `MDB-INC-0/1/2`, `BB-INC-0..3`, `CG-INC-0..3`.  **The host's own `INC-1` / `INC-2` keep
their bare names** (cited bare from `NetworkRunTypes.hpp:1332`, `NetworkNormalEquations.hpp:1049`,
`NetworkRunTraces.cpp:296`, `GnssConfigTypes.hpp:1143`).  Re-pointer map: MDB §N → §11.N (absorbed
§5.4 = §11.5 item 4), block-Baarda §N → §12.N (absorbed §8.4 = §12.8 gate 4), congruence §N → §13.N;
all three cards' standing-invariant pre-flight and does-NOT-do blocks → §10 (block-Baarda's `§10.4`
still resolves, same item).

**Memory context**: `project_ft63_fast_tier` (era-2), `reference_network_fault_qc_literature` (Baarda
MDB/MIB — the "unported" DIA piece this card started porting),
`project_network_ssystem_dia_assessment`, `project_block_baarda_card` (rider B's kernel contracts —
**NO diagonal clamp, no carry_aware**, both inherited by rider C; §14.3),
`project_laser_theory_mapping`, `project_m1_card4_d5_innovation` (formal-σ optimism),
`project_network_if_td_slip` (the 78-station mass refloat),
`project_antenna_eccentricity_arp_frame` (JPLM 6.10 cm), `project_why_non_integer_rootcause`
(D1: K_eff(30) ≈ 2).  **Scope discipline**: §10, one statement for host and all three riders.
**Provenance**: every `runs/<name>` path resolves under `D:\GnssProcessor_archive\2026-08-12\runs\`.

---

## 1. Why redundancy, not magnitude (the design argument)

Era-2's parked cell had a steady 2.6·10⁵σ common-removed prefit innovation.  Magnitude alone is an
ambiguous discriminator and the obvious hard responses are wrong:

- **Raw magnitude is legitimate**: receiver clock offsets/steps put ms-scale (10⁵ m) innovations on
  whole stations (URUM/INEG take clean 1 ms steps in the era-2 leg window).  Centring cleans only the
  CLOCK direction; it cannot say whether a per-cell excursion is a fault or an extreme.
- **Excluding single-frequency cells as a class is wrong** (prefer fallback over drop).  Their problem
  is not band count; it is that under certain topologies they become UNCHECKABLE.

The principled discriminator is **testability**: `r_i = 1 − w_i · (H Σ Hᵀ)_ii`, `r_i ∈ [0, 1]`, the
fraction of that row's information cross-checked by the rest of the network.  Baarda:
MDB_i = δ₀·σ_i/√r_i → **∞ as r_i → 0**.  A row with r_i ≈ 0 is a pure state-feeder — its misfit is
INVISIBLE to every residual-based test (postfit RAIM, σ̂₀, robust weighting) no matter how large.  That
is what the (BSHM, G07) code row became when the deweight gate cut the station's SH ties: one
observation feeding one otherwise-unconstrained slant state, r ≈ 0, free to park 188 km without a
single residual moving.

**The alarm condition is the CROSS-FLAG**: `uncheckable (r_i < ε) × large centered innovation
(s_i > S)`.  Neither half alone suffices — healthy single-freq risers are uncheckable with small s
(keep them); storm extremes have large s with healthy r (the network absorbs and localizes them).
The intersection is parked garbage with essentially zero false-positive surface, and it fires DURING
the park (era-2: 17 epochs of warning), not at the discharge.

## 2. Design — two censuses and the cross-flag

### INC-1: row-redundancy census (`network.redundancy_census_stride`, int, default 0 = off)

Every Nth processed epoch, and ALWAYS on flight-recorder trip epochs (§4):

- Per station, per data class: n_rows; Σr_i (redundancy mass; network total must satisfy
  Σr = n_rows − n_effective_states — a cheap closure check, invariant-#4 style; **§8 deviation 3 — as
  landed this is an INEQUALITY**); n_uncheckable = #{r_i < `redundancy_eps`} (double, default 0.05);
  and among uncheckable rows the max centred s_i with its (station, sat, class) identity.
- Emit per-epoch scalars into `network_epoch_status.csv` (n_uncheckable_rows, uncheckable_max_s
  network-wide); identities via `network_events.csv` tier REDUNDANCY, event `uncheckable_hot_cell`,
  when the cross-flag (r < ε AND s > `redundancy_hot_sigma`, double, default 1000) fires —
  rate-limited like the PREFIT events.
- `NetworkEstimator::rowRedundancy` already exists (RAIM attribution path) over the datum-frame shared
  covariance (`datumFrameSharedCov`) + per-station reconstruct.  Re-used verbatim; no new algebra.

### INC-2: unobservable-local census (same stride, no extra knob)

State-side complement: per station, count local columns whose N_rr LDLT pivot is below
`redundancy_pivot_eps` (double, default 1e-8 relative to the station's median pivot) — states carrying
NO information this epoch (fully orphaned; the one-obs-one-state case shows in INC-1 instead).  Needs
one kernel accessor (per-station `vectorD` of the cached LDLT — read-only, the factor exists).
Identities via `h2local` labels; event `unobservable_local` on first appearance per (station, key) per
era.  **§8 deviation 5 bounds when this can fire at all.**

### The formal MDB extension (no longer deferred)

Per-class MDB percentiles (δ₀·σ_i/√r_i at fixed α, β) are the full Baarda port the DIA catalog lists
as open; INC-1's r_i census is its substrate.  **Built as RIDER A, §11.**

## 3. Cost model and the two-tier escalation (measured anchors)

D0 timers on the era-2 leg (63 sta, ~450 shared cols): cov-consumers 158 ms / 697 calls ≈ 0.2 ms per
covariance access; a full row-redundancy sweep estimated 100–300 ms/epoch — NOT free at the
1.6–1.8 s/ep budget (5–20 %).  Hence:

- **Stride default 10** (one sweep per 5 min at 30 s cadence) — the park signature persists many
  epochs (era-2: 17+), so stride-10 latency is minutes, acceptable for an alarm.
- **Escalation trigger** (free→costly, targeted): when the prefit census's free per-epoch metric
  reports any class max_s > threshold, sweep the flagged STATION only at the next epoch
  (~2-5 ms).  The free census asks "is something big?"; redundancy answers "is it checkable?".
- Always sweep fully on flight-recorder trip epochs (attribution dump and census describe one epoch).

Measured: **6.1 ms per station-sweep ⇒ ~380 ms/epoch for a full 63-station sweep** (§8) — the estimate
was low and the stride is doing real work.  Rider budgets are all fractions of THIS sweep (§11.3,
§12.5, §13.4); §14.5 holds the one restatement the measurements forced.

## 4. Wiring sites

- `NetworkEstimator::rowRedundancy` (`NetworkEstimator.cpp:438`) + `datumFrameSharedCov` (`:360`) —
  existing.
- Kernel: per-station LDLT pivot accessor — as landed `NetworkNormalEquations::stationLocalPivots`
  (new, read-only, `NetworkNormalEquations.hpp:1064`).  The SPEC's placement locator was "~5 lines
  next to `stationLocalNormals`" (a distinct live symbol, `NetworkNormalEquations.hpp:1045`).
- Sweep seam: `filter/NetworkRedundancyCensus.{hpp,cpp}` (+ manual CMake entry), called from the
  driver's SB_FRAMEDIAG block next to `networkPostfitSigma0` / `networkRhsAttribution` — same
  ingredients (stations, est, asm_res, est_index_of); reuses the hoisted classifier
  (`NetworkRowClassify`, the prefit card's INC-0).
- Escalation channel: a ctx.carry flag set by the census walk (station index + one-epoch latch),
  consumed next epoch.
- Config: `NetworkConfig` fields near `rhs_attrib_tow`; loader at
  `GnssConfigLoadNetwork.cpp:1324-1354` with arming lines **+ an INERT-guard WARN if the stride is
  armed without `trace_prefit_census` (the cross-flag needs the s metric)** — the dependency is real
  in code (`GnssConfigLoadNetwork.cpp:1459`, `:1486` gate on `network_config_.trace_prefit_census`).
  ⚠ As landed that WARN is unnecessary: §8 deviation 1.
- Counters + run-end line: `NetworkServiceCounters` / `NetworkTeardown.cpp:940` — sweeps run, cells
  flagged, escalations taken, closure worst-case.

## 5. Unit tests

`tests/test_redundancy_census.cpp` (one suite, 10 cases as landed): (1) r_i on a hand-built 2-station
system matches 1 − w·(HΣHᵀ)_ii; (2) one-obs-one-free-state cell reports r ≈ 0, a second coupled row
raises r; (3) Σr closure **= n_rows − n_states** on the toy system (as landed the shipped invariant is
the inequality of §8 deviation 3); (4) pivot census flags an orphaned local column; (5) cross-flag
requires BOTH halves; (6) stride/escalation gating; (7) OFF byte-identity (structural).

## 6. Field validation — KNOWN-ANSWER gates (standing harness)

Controls: `runs/ft63_leg19_rhsat` (+ its rows/attribution dumps).  Distinct run dir
(`ft63_leg19_redund`); deweight-hygiene knobs OFF.  **All gates run on the converged warm leg-19
window — §9.**

1. Smoke + OFF-identity.  **MET (§8).**
2. **The park is visible**: from ~328020 through 329130, (BSHM00ISR, G07, obs_code) reports r < 0.05
   with centred s ≈ 2.6e5 → `uncheckable_hot_cell` fires (rate-limited), naming the cell **~17 epochs
   before the break** on a stride-1 leg; on stride-10 within 2 sweeps.  (BSHM, G16) likewise.  **OWED.**
3. **The valve is visible**: at 329160 (tie rows re-folded) the same cells' r_i RISES (the slant
   re-couples) — the census records the uncheckable→checkable transition at the discharge epoch,
   corroborating the topology-ledger event.  **OWED.**
4. **No fallback-violation**: healthy single-frequency risers elsewhere appear as
   uncheckable-with-small-s (counted, NOT evented, never weight-touched) — enumerate a few and confirm
   zero hot-cell events.  **OWED.**
5. **Escalation path**: stride 0 + escalation armed, the BSHM hot cell still named within one epoch of
   the first PREFIT event.  **MET (§8).**
6. Solve byte-identical to control in every armed leg (measure-only).  **OWED on leg-19** (met cold).
7. Product claims on total_IF only.  **OWED.**

## 7. Deferral triage (pre-declared)

- Full MDB/α/β reporting: **no longer deferred — RIDER A (§11)**.  Solution-separation integration
  remains deferred (§11.7, §12.9, §13.8).
- Consumer/RT engine port: after producer validation.
- Any automatic weight/exclusion response to the cross-flag: NOT this card (measure-only).  The
  optional fuse composition lives in the prefit census card (`prefit_fuse_require_uncheckable`).  ⚠ As
  landed that knob is parsed and **force-disabled with a WARN** — this census is POST-solve and the
  fuse acts PRE-`addStation`, so the cross-flag cannot reach it
  (`docs/SPEC_prefit_census_observability.md`).

---

## 8. BUILD RECORD (Opus session, 2026-08-01) — implemented, smoke-gated

Code: `include/filter/NetworkRedundancyCensus.hpp`, `src/core/filter/NetworkRedundancyCensus.cpp`
(seam, CMake entry added), `NetworkNormalEquations::stationLocalPivots` (INC-2 kernel accessor), 6
`network.redundancy_*` knobs + loader guards, `NetworkKnobs::redun_census_on`, carry/counter blocks in
`NetworkRunContext`, 4 epoch-status columns, run-end telemetry in `NetworkTeardown.cpp`,
`tests/test_redundancy_census.cpp` (10 cases, one suite).  INC-0 consumed from the concurrent
prefit-census session's `NetworkRowClassify` hoist — one classifier, one truth.

### Deviations from this spec, all deliberate

1. **The centred metric is computed HERE, not consumed from the prefit census card** (that card had
   not landed its INC-1 yet).  Same definition, same hoisted classifier; cost is one extra O(rows)
   pass when both are armed.  Consequence: **no INERT-guard WARN is needed** (§4) and the escalation
   channel is live on its own.  ⚠ **Still open as a CODE item (re-verified 2026-08-15)**: both cards
   have landed their INC-1 and the duplicate walk is still there
   (`NetworkRedundancyCensus.cpp:97-106`).  A later increment collapses it by having the prefit census
   publish per-station `max_s`.
2. **Escalation threshold is its own knob** (`redundancy_escalate_sigma`, 0 = off) rather than
   borrowing `prefit_event_sigma` — keeps the two cards independently armable.
3. **The closure check is an INEQUALITY, not the equality of §2.**  `Σr = n_rows − n_states` holds only
   for an unprior'd, fully-swept, unclamped system.  The producer folds prior information (carries, DD,
   Kaula, datum) and the kernel clamps `r` to [0,1], so the defensible invariant is
   `trace(H) = Σ(1−r) ≤ n_free` — it survives restriction to any row subset and is therefore valid on a
   targeted single-station sweep.  `n_free` = non-zero columns of the datum-frame covariance (datum
   pins AND ensemble phase gauges excluded without this file knowing which mechanism zeroed them) plus
   the swept stations' local dimensions.  Reported as worst slack; negative ⇒ WARN + "untrusted".
4. **A 4th epoch-status column, `n_redun_rows`** (the denominator) — a raw uncheckable count is
   uninterpretable without it.
5. **INC-2's firable window is bounded from below by the kernel.**  `addStation` already rejects a
   station whose smallest local pivot is under 1e-12 × block scale, so INC-2 can only fire in
   [1e-12, `pivot_eps`) × median — four decades at the 1e-8 default, EMPTY at or below 1e-12.  The
   loader warns; a quiet INC-2 must be read against the knob.

### Smoke gate (FT63, 63 stations, 10 epochs, COLD, Release)

Arms `runs/ft63_redund_{off,on,escal,diag}`, configs `config/test_suite/_ft63_redund_*.json`.

- **OFF-identity PASS.**  `network_solution.csv` byte-identical OFF vs ON, as are all 36 other
  solve/QC artifacts; `network_epoch_status.csv` has **zero** non-census column differences.  The 7
  differing files: the 4 census columns, the REDUNDANCY events, the two config/telemetry logs, the
  manifest, and two wall-clock fields (`network_run_summary` ms, `service_health` ms).
- **Live wiring.**  11 passes / 11 sweeps / 692 station-sweeps; 35 948 data rows; events carry full
  identities `(station, sat, class, r, eps, y, w, row)`.  **Closure holds**: worst slack +84.5 over 11
  sweeps.
- **Trip-forced sweep works** (`_diag` arm, `rhs_attrib_tow` armed): "1 forced by a flight-recorder
  trip"; census and attribution dump the same epoch.  The predicate also reads the prefit card's INC-5
  `rhs_trip_pending`.
- **Two-tier escalation works and is the headline result** (`_escal`, stride 0): the free tier raised
  17 latches, 15 became targeted sweeps, and the arm found **the same worst cell**
  (URUM00CHN/G22/obs_phase, s = 3.95e6) using **15 station-sweeps / 194 ms instead of 692 / 4126 ms —
  21× cheaper**.
- **Cost**: 6.1 ms per station-sweep ⇒ ~380 ms/epoch for a full 63-station sweep (§3 estimated
  100–300 ms).
- **INC-2 fired 0** over 26 466 columns examined — expected, deviation 5.  The denominators are in the
  run-end line, so this reads as "ran, found nothing", not "never ran".

**What the cold smoke does NOT establish** is stated once for host and all three riders in **§9**.  §6
gates 1 and 5 are met; gates 2, 3, 4, 6, 7 are OWED and require the `runs/ft63_leg19_rhsat` lineage.

Incidental find, handed to the prefit census card: the exactly-zero phase rows on a newly risen arc at
a clock-carrying station are the **seed-birth signature** that card's INC-3 exists to locate — one
station in 63 at the sampled epoch.

---

## 9. ★ CONVERGENCE HYGIENE — the one statement governing the host and all three riders

*Previously written four times, once per card.  This is the canonical copy; the riders' field-gate
sections point here.*

Both halves of the cross-flag, every rider's uncheckable population, and every block/station statistic
built on post-fit residuals are **structurally inflated during convergence**:

- **A free ambiguity absorbs its own arc's phase misfit exactly.**  Same property that makes the
  post-fit phase pool identically zero, and why **43 % of data rows read uncheckable on a cold start**
  (measured, host smoke).  Also why block-Baarda's UNCHECKABLE-block population and congruence's
  rank-deficient blocks are large and STRUCTURAL cold, and why MDB's phase σ̂₀ deflates (§11.10
  deviation 5).
- **A freshly seeded arc cross-flags.**  Its seed makes the arc's first phase innovation ~0, but at a
  station whose receiver clock is not yet absorbed into the linearization point the rest of that
  station's phase rows sit AT the clock offset (measured: FFMJ00DEU 259 km, URUM00CHN 42 km —
  timing-lab receivers — against a 0.23 m median across the other 61 stations).  The fresh arc is then
  the outlier against its own station's median, at r = 0.  Measured **40 hot cells / 11 cold epochs =
  0.11 % of data rows**, concentrated on exactly this signature.

Both statements are TRUE of the system, so neither is suppressed.  **Procedural consequence: every
known-answer / service-deliverable gate in this document — §6.2/6.3/6.4/6.6/6.7, §11.6, §12.8, §13.7 —
runs on a CONVERGED WARM WINDOW, not on a cold smoke.**  A cold arm establishes wiring, OFF
byte-identity and cost, and nothing else.  A clean-looking MDB table on a cold arm would be the
ALARMING outcome (§13.11).  Corollary for MDB: **do not quote a cold σ̂₀ or a cold uncheckable fraction
as the service answer** (§11.10).

## 10. ★ GOVERNANCE — scope discipline, standing-invariant pre-flight, what these cards do NOT do

*One block covering host and all three riders.  Previously four near-identical copies.  Block-Baarda's
cited `§10.4` resolves to §10.4 below, same item.*

**Scope discipline, all four**: default-OFF, OFF byte-identical, MEASURE-ONLY, fingerprint-SAFE (legal
to arm on a warm resume).  The host was originally declared "NOT fingerprinted"; the riders'
declaration is the same property said the other way round.  Same-build A/B for every identity claim
(verification integers are build-config-sensitive — never compare Debug vs Release).

### 10.1–10.7 Standing-invariant pre-flight

1. **No response added or removed.**  All four measure-only; nothing gates, deweights, refloats or
   excludes on these statistics.  Riders B and C measure tiers standing invariant #1 currently lacks
   (the LOS block, and the held station coordinate).
2. **Release-only field runs.**  Debug perf numbers are artifacts; CLAUDE.md §Build and
   `scripts/vs2026_build.ps1`.
3. **Every deferral triaged field-blocking vs field-inert, with evidence** — §7 (host), §11.7 (A),
   §12.9 (B), §13.8 (C).  All rows are INERT.
4. **Run-end counters are the ON-path telemetry.**  Every card distinguishes "ran, found nothing" from
   "never ran", DENOMINATORS printed first.  MDB's aggregates separate "ran, all rows checkable" from
   "never ran" (`NetworkTeardown.cpp:1070`); block-Baarda reports the **block-size histogram before any
   occupancy** and BB-INC-3's fraction last with its denominator, because "no smearing found" and
   "there was never a block to smear across" produce identical ladders (`NetworkTeardown.cpp:1079`);
   congruence prints the sweep denominators and its per-station ladders + `cg_*` counters
   (`ctx.counters.cg` `NetworkCongruence.cpp:229`, `ctx.carry.cg_hits` `:244`, `ctx.knobs.cg_on`
   `ApplicationNetwork.cpp:534`) and then THE MDB TABLE, because "every held coordinate is right" and
   "the screen never swept a station" produce identical silence (`NetworkTeardown.cpp:1089`).
5. **Datum invariance.**  MDB is row-level (weight and geometry only).  Block residual quadratic forms
   are datum-invariant.  Congruence residuals are unique under the minimal pin datum (LASER 5-65);
   FRAME epochs are still split out.
6. **Default-OFF byte-identical; same-build A/B.**  Congruence's §13.7 perturbed arm is declared
   diagnostic-only (solve-affecting by intent).
7. **No coupling change.**  FRAME blocks/epochs are measured and split, not discarded.

### 10.8 What these cards explicitly do NOT do

- **No gate, no deweight, no admission decision consumes any of these statistics.**  Not MDB, not `t_B`
  — including the loud blocks — not `T_i`.
- MDB does not replace the prior-residual census's empirical ladders: it is the formal complement, and
  `mdb_scaled` exists precisely because formal-alone was measured wrong (§11.1 rule 1).  MDB claims no
  detection power beyond what its Monte Carlo pins (single-fault, Gaussian).
- Block-Baarda does not modify D5, RAIM, median polish, or the prior-residual census; does not import
  F(α, b, r) tables (§12.2 rule 1); and does **not** claim the block tier has power here — §12.8 gate 4
  measures whether it does, and a null is a valid, reportable outcome.
- Congruence does not move, deweight, or re-estimate any station coordinate; does not claim cm-class
  detection on a `carry_invariant` lineage (the MDB table states the actual power, code-grade is the
  expected v1 answer); does not replace block-Baarda or median polish (CG-INC-3 measures the
  partition); does not perform campaign-to-campaign deformation analysis.

---

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

## 12. RIDER B — BlockFormal: (station, sat) LOS block outlier census (`network.block_baarda`)

*Absorbed from `docs/attic/SPEC_block_baarda.md` (SPEC'd + built 2026-08-04).  Increments namespaced
**BB-INC-0..3** (absorbed labels: that card's `INC-0..3`).*

**Origin**: NGS LASER Guide v2.7 slide 5-101 (PDF p. 413): Baarda data snooping at the 3D block level,
`OutlierDetectionMethod = BlockFormal`, formulas Snow & Schaffrin (2003), *GPS Solutions*
7(2):130–139, eqs. (13)–(18).  Slide 5-98 states the motivation: a single outlier is SMEARED across
residuals ("not necessarily a one-to-one correspondence between large residuals and outliers").

### 12.1 Why — the smearing gap between row tests and station polish

One physical fault (one LOS: multipath, a wet radome lobe, an unflagged slip) projects onto 2–8 rows —
code+phase × bands — each individually sub-threshold while the block is jointly loud.  Today's tiers
straddle exactly that unit: per-row w-tests below it, station-level median polish above it.  The
**78-station mass-refloat incident** was the textbook cost: the bad-STATION path was DISABLED rather
than confirmed because nothing could say *which* LOS carried the fault.  LASER's BlockFormal is the
middle tier, and its natural block here is the (station, sat) LOS across bands.

### 12.2 Binding precedent — the prior-residual census's D5 rules apply unchanged

1. **Empirical critical values, never tabular.**  Snow & Schaffrin's F(α, b, r) assumes Gaussian
   errors; D5 measured that false by orders of magnitude in the tail.  The deliverable is the empirical
   exceedance ladder of the block statistic per family.
2. **Corroboration in cycle one.**  Contingency table vs the slip-QC raw-observable verdicts ships in
   v1 (`gap_instrument` auto-arm).  ⚠ The SPEC cited the prior-residual census's loader tripwire at
   `GnssConfigLoadNetwork.cpp:3543-3548`; **that pointer is stale** (now an unrelated
   `ura_staleness_q2` clamp).  Current: the prior-residual census's tail tripwire is at
   `GnssConfigLoadNetwork.cpp:3296-3313`, and this card's own auto-arm sits INLINE with its knobs at
   `:3715-3733` (`loadNetworkQcTiers_` step 153, after the `gap_instrument` jval in step 152) —
   deviation 8.
3. **Response is a separate card.**  v1 wires nothing.

*(These are the rules of `docs/SPEC_prior_residual_census.md` §2 — "the census" in this rider always
means that card, never this host.)*

### 12.3 The statistic

For block B = the data rows of one (station, sat) this epoch, b = |B|:

```
T_B = ẽ_Bᵀ (Q_ẽ,B)⁻¹ ẽ_B / b          normalized block quadratic form
t_B = T_B / σ̂₀,f²                      empirically re-scaled (per family f)
```

`Q_ẽ,B` is the b×b block of the residual cofactor — the diagonal of which is what `rowRedundancy`
already returns.  BB-INC-0 extends the kernel: a sibling
`NetworkNormalEquations::blockResidualCofactor(station, rows)` returning the b×b from the same inputs
`rowRedundancy` takes (declared `NetworkNormalEquations.hpp:997`, defined
`NetworkNormalEquations.cpp:1743` — the SPEC's `NetworkNormalEquations.cpp:1690` has drifted;
station-local A_s/A_r rows + `cov_free` = `datumFrameSharedCov`, `NetworkEstimator.cpp:360`/`:438`).
Same math, off-diagonals kept, **b ≤ 8**.  Degenerate guard: if `Q_ẽ,B` is singular at the working
precision (a fully-leveraged block — the block analogue of r < ε), the block is UNCHECKABLE: counted,
identity kept, never inverted through (**pinv or drop — count either way; the census's
prefer-fallback-over-drop discipline**).

σ̂₀,f is a `NetworkCarryVce` instance fed `T_B` per family — the D5 v2 pattern
(`NetworkInvariantCarryDriver.cpp:1452-1455` precedent; the SPEC wrote
`NetworkInvariantCarryDriver.cpp:1453-1455`, and `:1452` is the `inflation_->addRow(` call head), same
predict-then-update and never-censor contracts.  ⚠ **"Verbatim" is WRONG for b > 1 — §14.2**, which
states both `NetworkCarryVce` corrections once for all three riders.

### 12.4 Design

- **BB-INC-0**: `blockResidualCofactor` kernel + unit tests against brute-force dense GLS.
- **BB-INC-1**: the sweep.  Blocks enumerated for SWEPT stations only — this rider rides the host's
  stride/targeted-escalation machinery (`NetworkRedundancyCensus.cpp:217`, `:443`) rather than adding a
  scheduler.  Per (family × label) exceedance ladders over `t_B` (census bins), n_blocks, worst-block
  identity.  Labels RESPONDED / CLEAN / FRAME exactly as the prior-residual census defines them; a
  block inherits the union of its rows' labels (any RESPONDED row ⇒ RESPONDED block; else any FRAME ⇒
  FRAME).
- **BB-INC-2**: corroboration join per block (slip-QC verdict on that (station, sat)), b-cell reported
  per family.
- **BB-INC-3**: the smearing measurement — **the card's unique deliverable**: among blocks with `t_B`
  above the report threshold, the fraction whose individual rows were ALL below the per-row threshold
  (`block_only` count).  That fraction IS the population invisible to every existing tier; **if it is
  ~0 the block tier adds nothing and that finding CLOSES the card honestly.**

### 12.5 Cost

Per swept station: b×b builds and inverts for its tracked sats, b ≤ 8 — bounded by the host's own
`rowRedundancy` cost class (6.1 ms/station-sweep measured).  Budget: **≤ 2× the census sweep it
rides**; report measured cost per station-sweep (`counters.bb.wall_ms`).  Stride inherited; no new
knob.  **UNMEASURED — no field run (§12.12).**

### 12.6 Config

`network.block_baarda` (master; **hard error** without `redundancy_census`, the sweep host),
`network.block_baarda_report_sigma` (trace/event threshold on `t_B`, reporting only),
`network.block_baarda_corroborate` (BB-INC-2; auto-arms `cycle_slip.gap_instrument` with a WARN).
**Types, defaults and dependencies: the one table in §14.7.**  **Seven `network.block_baarda*` knobs
landed in total.**  Loader `GnssConfigLoadNetwork.cpp:3643-3733`, next to the redundancy-census block.
Fingerprint-SAFE.  Invariant-#4 tripwires as in the host (zero-block families distinguishable from
unarmed).

### 12.7 Unit tests

`tests/test_block_baarda.cpp` — **24 cases across TWO suites**, the one file in the family that owns
two suite names: `BlockBaardaTest` (8, the BB-INC-0 kernel) and `BlockBaardaSweepTest` (16, the
accumulator + loader).  Full suite at the time: **1197 / 179 suites**, one pre-existing fixture-gated
skip.

Kernel cases: the dense-GLS oracle (`blockResidualCofactor` = brute-force dense
`(P⁻¹ − A N⁻ Aᵀ)_BB` to 1e-10, including a rank-deficient-datum case), sub-block consistency, datum
invariance, and **the smearing demonstration** — a fault split evenly across 4 rows of one block, each
row at 0.6× the per-row threshold: every w-test silent, `T_B` loud; and the converse, 4 independent
faults in 4 different blocks do NOT pool into any single loud block.  Sweep cases: the χ²_b rescaling
(`Sigma0RecoversTheScaleAtEveryBlockSize`, `Sigma0IsUnbiasedOnAMixedBlockSizePopulation` — §14.2), the
Gaussian-null ladder against independently computed tail probabilities, Student-t (ν = 3)
over-occupancy with a converged scale (the D5 failure mode preserved at block level), label-partition
sums, predict-then-update, the UNCHECKABLE guard, BB-INC-3's `block_only` accounting, BB-INC-2's
b-cell, and the loader tripwires.

### 12.8 Field validation — **ALL OWED**

Converged warm window (§9: a cold smoke establishes wiring and cost only).

1. OFF byte-identity; live wiring; cost within budget.
2. **Positive control**: RESPONDED-block ladder shifted vs CLEAN (same meaning and caveats as the
   prior-residual census's gate 3; **a null result is a publishable finding, not a tuning licence**).
3. **Contingency b-cell** per family (D5's was 97 % — expect the block statistic to do BETTER on
   code+phase joint evidence; **measure, don't assume**).
4. **The smearing fraction** (BB-INC-3): report `block_only / blocks_flagged`.  **This single number
   decides whether a response card is ever worth writing.**
5. Product claims on total_IF only; solve byte-identical in every arm.

### 12.9 Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| Any response (deweight/refloat/exclude per block) | **INERT** — measure-only; the response card's design input is §12.8 gate 4 (the absorbed card's §8.4) | — |
| Leave-one-station-out solution separation (would re-arm the bad-STATION path) | **INERT** — own card; additive normals make it cheap but it is solve-adjacent | DIA catalog item 5 |
| Cross-band prior-row blocks (carry families) | **INERT** — the prior-residual census's scalar ladder covers priors in v1; block form needs the same `Q_ẽ,B` extension on fold rows | `docs/SPEC_prior_residual_census.md` |
| Snow & Schaffrin's exact F reference distribution | **INERT** — empirical ladder by rule §12.2.1 | D5 history |

*(Pre-flight and does-NOT-do: §10 — this card's cited `§10.4` is §10.4 there.)*

### 12.12 BUILD RECORD (Opus session, 2026-08-04) — BB-INC-0..3 implemented, unit-gated

**Code.** `NetworkNormalEquations::blockResidualCofactor` (BB-INC-0 kernel);
`include/filter/NetworkBlockBaarda.hpp` (header-only accumulator: ladder, σ̂₀, labels, contingency, size
histogram) + `src/core/filter/NetworkBlockBaarda.cpp` (per-station sweep, UNCHECKABLE guard, emission
seam, run-end readout), CMake entry added; the FEED rides `NetworkRedundancyCensus.cpp`'s swept branch
(§12.4's "no new scheduler"); 7 `network.block_baarda*` knobs + loader tripwires; `NetworkKnobs::bb_*`,
`NetworkCarryState::bb/bb_hits/bb_event_last`, `NetworkServiceCounters::bb`;
`network_block_baarda.csv` + `BLOCKBAARDA` events; run-end line in `NetworkTeardown.cpp:1079`;
`tests/test_block_baarda.cpp` (24 cases, two suites).  **The two corrections to §12.3's recipe are in
§14.2** — inherited by rider C and stated once there.

#### Deviations from this spec, all deliberate

1. **`t_B` is a VARIANCE RATIO, not a σ**, and the geo column is not what §12.4 implies.  The ladder
   rungs are shared with the prior-residual census ("census bins") so one reader handles both files,
   but that card's `|t|` is in σ units and this one's is not.  Worse, the *geometric* mean of `t_B`
   under H₀ is `2e^ψ(b/2)/b` — b-dependent, so a raw geo-mean of a mixed-LOS population is unreadable.
   The emitted `geo_cal` column is therefore the **calibrated χ²₁-equivalent** geometric mean (1.0 =
   converged, b-invariant), matching `NetworkPriorResidual::Ladder::geoT2`.  Stated in the CSV header,
   the run-end line and the loader's arming line.
2. **The family partition is the CONSTELLATION**, not the prior-residual census's family enum — there
   is none (families are runtime strings), and its five names are CARRY families that mean nothing for
   a line of sight.  Constellation is the only axis that can label a WHOLE block, because a block spans
   code AND phase rows by construction.  Reuses `netMdbConstIndex`'s partition.
3. **UNCHECKABLE blocks are DROPPED-with-count, not pinv'd.**  §12.3 licenses either.  A pinv'd `T_B`
   has an unknown effective dof, so its ladder rung would mean nothing *and* it would pollute σ̂₀ for
   every honest block in its family.
4. **★ `RESPONDED` is an ESTIMATOR-side signal** (a pending-reseed ambiguity on that
   (station, satellite)), deliberately NOT the slip-QC raw verdict BB-INC-2 corroborates against —
   sharing a source would make §12.8 gate 2's positive control **circular**.  The seam order is
   LOAD-BEARING: SB_LEVELING clears the pending-reseed set, SB_RAIM marks this epoch's refloats, and
   only then does SB_FRAMEDIAG read it.
5. **`networkRedundancyCensus` gained an `x_s` parameter.**  `T_B` needs POST-FIT residuals and
   `StationAssembly::y` is the PREFIT innovation; nothing in the tree stores a post-fit residual (all
   three sites that form one are loop-local temporaries).  An empty `x_s` **DISARMS** the rider rather
   than censusing the wrong quantity.
6. **No `network_epoch_status.csv` columns**, unlike the three sibling censuses.
   `network_block_baarda.csv` already carries n_blocks / n_reported / max_t per (family × label) per
   epoch — strictly more than pooled scalars would say.
7. **★ The size histogram is reported BEFORE any ladder.**  A run whose blocks were all b = 1 produces
   a ladder identical to the scalar w-test and a BB-INC-3 fraction of 0 **BY CONSTRUCTION**; without
   the histogram that reads as "no smearing found" rather than "there was never a block to smear
   across".
8. **§12.2's cited loader line was stale** — recorded there; the pointer is fixed in place above.  The
   pattern followed is the prior-residual census's auto-arm, but this card's own auto-arm sits INLINE
   with its knobs rather than split to the tail of the slip helper.

#### What is NOT established, and is OWED

**No field run.**  Every §12.8 gate is outstanding, on a **converged warm window** (§9) — enumerated
once in §15 row 4.

#### ★★ THE PRE-REGISTERED PREDICTION, recorded before the field run

A design sweep over a physical **3-station × 4-satellite toy** (code+phase × 2 bands, iono/ambiguity
priors at 1 cm / 5 mm) measured the block tier's gain over the row tier, `T_B / max_i w_i²`, as
**FAULT-CLASS DEPENDENT**: **≈ 1.0** for single-row faults **AND** for LOS-common-mode faults (b = 4
pooling loses to the χ²₄ threshold); **≈ 6×** for faults that violate the block's internal correlation
— e.g. an unflagged half-cycle slip on both bands, where **every w-test in the network reads ≤ 2.66
against a 3.29 critical value while `T_B` = 43.7 against χ²₄(0.001) = 18.47**.  So §12.8 gate 4's
`block_only` fraction should be driven by **SLIP-like** faults; if the swept population is dominated by
code multipath, expect ≈ 0 and the honest close §10.8 already licenses.

---

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

## 14. ★ CROSS-RIDER RECONCILIATIONS — the shared objects, stated once

*These existed only as scattered deviations because the four cards were separate files.  MDB's
deviation 2 in particular existed purely to reconcile its ∞ threshold against this host's `eps`.*

### 14.1 ∞ (arithmetic) vs uncheckable (policy) — a strict SUBSET, never an equality

| instrument | predicate | kind of floor |
|---|---|---|
| MDB = ∞ | `r ≤ 1e-6` (`kBaardaRedundancyFloor`, `BaardaReliability.hpp:68`) | **ARITHMETIC** |
| census uncheckable | `r < network.redundancy_eps` (default **0.05**) | **POLICY** |

**Four decades apart, so ∞ is a strict SUBSET of uncheckable.**  Asserting an equality would have
hard-coded a bug.  What the MDB flush actually reconciles is its own independent recount of DATA rows
and rows-below-eps against the census's `ep_redun_rows` / `ep_uncheckable_rows` for the same epoch; the
∞ count is reported separately.  **RECONCILED on 11/11, 20/20, 31/31 and 7/7 census epochs** across the
arms.  At 303 stations: **39 645 rows at ∞ vs 46 773 uncheckable** — an equality assertion would have
**FAILED** here.

### 14.2 The two `NetworkCarryVce` corrections, inherited by riders B and C

1. **★★ CORRECTION 1 — "the D5 v2 pattern verbatim" is WRONG for b > 1.**  `NetworkCarryVce` inverts
   the geometric mean of a **χ²₁** variate (`kChi2GeoMean = 0.2807297418` = 2e^ψ(1/2), hard-coded at
   `include/filter/NetworkCarryVce.hpp:473` — the cards were written when it sat at
   `NetworkCarryVce.hpp:435`; re-grep by the symbol, not the line), but `T_B·b ~ σ₀²χ²_b`.  Since
   `2e^ψ(b/2)/b` runs **0.281 / 0.561 / 0.763 / 0.877 at b = 1 / 2 / 4 / 8**, feeding `√T_B` biases σ̂₀
   by up to **1.8× in σ (3× in variance)** — **and the bias MOVES with the constellation's band plan.**
   The implementation feeds the χ²₁-equivalent `u_B = √(T_B·b·2e^ψ(1/2)/2e^ψ(b/2))` via
   `blockChi2GeoMean` (`include/filter/NetworkBlockBaarda.hpp:151-153`,
   `kBlockChi2GeoMean1 = 0.2807297418`), **exact for any mix of b and byte-identical at b = 1**.
   Unit-gated by `Sigma0RecoversTheScaleAtEveryBlockSize` and
   `Sigma0IsUnbiasedOnAMixedBlockSizePopulation`.  Rider C consumes the same helper at **b = 3**
   (χ²₃ → χ²₁).
2. **★★ CORRECTION 2 — `seed_floor_m2` MUST be passed 1.0 explicitly.**  The constructor default of 0.0
   is silently replaced by **1e-6**, which would make an unwarmed family report **σ̂₀ = 10⁻³** and
   inflate every `t_B` in it by **10⁶**.

Both inherited **verbatim** by rider C (`blockChi2GeoMean` at b = 3, `seed_floor_m2 = 1.0` explicit).
Also inherited from D5 and unchanged in all three riders: **predict-then-update ordering**, and **NEVER
censor the calibration input**.

### 14.3 The shared kernel and who owns it

`NetworkNormalEquations::blockResidualCofactor` (declared `NetworkNormalEquations.hpp:997`, defined
`NetworkNormalEquations.cpp:1743`) is **BB-INC-0**, and rider C consumes it unchanged for a much larger
row set (b = 30–80 vs ≤ 8).  Its diagonal is what `NetworkEstimator::rowRedundancy`
(`NetworkEstimator.cpp:438`) already returns over `datumFrameSharedCov` (`:360`); the block form keeps
the off-diagonals.  Rider C also inherits rider B's `x_s` threading (deviation 5) and its single
`reconstructStation` per swept station — **one reconstruct serves the host sweep and both post-fit
riders.**

**Two kernel contracts travel with it, from rider B to rider C: NO diagonal clamp, and no
`carry_aware`** (`project_block_baarda_card`).  The cofactor is used raw — an `r`-style clamp to [0,1]
would silently change the quadratic form's dof, and the carry-aware covariance variant is a different
system from the one that produced ẽ.

### 14.4 The shared δ₀ convention

`include/filter/BaardaReliability.hpp` is the ONLY copy in the tree of δ₀, the MDB formula, the r-floor
and the `kInf` convention.  `KalmanFilter` (PPP), rider A (network rows) and rider C (coordinate
directions) all call it, so their reliability numbers are commensurable **by construction**.
`kBaardaPowerQuantile = 0.8416` (`:62`), `kBaardaRedundancyFloor = 1e-6` (`:68`),
δ₀ = k + 0.8416 = 4.1316 at the stock k = 3.29.  ⚠ `BaardaReliability.hpp:31` cites the α/β deferral,
now §11.7.

### 14.5 The one budget restatement the measurements forced

Rider A's §11.3 budget ("≤0.2 ms/epoch; above 1 ms the implementation is wrong") costed **one
division**.  The dominant term is the order-statistic pools the median and p95 require.  Measured
6.4 ms/epoch at 63 sta / stride 1, 1.9 % of the host seam; ~1.24 % of the loop at 303 sta / stride 5.
**RECOMMENDATION, standing for all three riders: restate the budget as "≤ 2 % of the host sweep"** —
that is what it measures and what it can hold.  Riders B and C were specified against "≤ 2× the census
sweep"; rider C measured 0.28–0.39×.

### 14.6 The shared stride and escalation

None of the three riders adds a scheduler or a stride knob.  All three are called in sequence from the
host sweep — `NetworkRedundancyCensus.cpp:212` (MDB-INC-0), `:217` and `:443` (BB-INC-1), `:225` and
`:499` (CG-INC-1) — and inherit the host's stride, its targeted escalation and its trip-forced sweeps.
A quiet rider must therefore always be read against the host's stride first.

### 14.7 Every knob in the family, one table

**All eighteen are default-OFF, measure-only and fingerprint-SAFE** (legal to arm on a warm resume) —
the 14 named below plus the four further `network.block_baarda*` of §12.6.  None has a loader-default
flip; arming goes into a config template (no-promotion directive).

| knob | type | default | hard dependency |
|---|---|---|---|
| `network.redundancy_census_stride` | int | **0** (= off) | — |
| `network.redundancy_eps` | double | **0.05** | loader clamps to 0.05 if ≤0 or ≥1 |
| `network.redundancy_pivot_eps` | double | **1e-8** | INC-2 is EMPTY at or below 1e-12 (§8 deviation 5) |
| `network.redundancy_hot_sigma` | double | **1000** | the cross-flag's s threshold |
| `network.redundancy_escalate_sigma` | double | **0** (= off) | its own knob, not `prefit_event_sigma` (§8 deviation 2) |
| `network.mdb` | bool | **false** | **HARD ERROR** without `raim_redun` OR `redundancy_census`; WARNs without `trace_postfit_sigma0` |
| `network.mdb_event_m` | double | **1.0** | reporting only |
| `network.mdb_event_holdoff_ep` | int | **20** | reporting only |
| `network.block_baarda` | bool | **false** | **HARD ERROR** without `redundancy_census` (the sweep host) |
| `network.block_baarda_report_sigma` | double | **4.0** | reporting only |
| `network.block_baarda_corroborate` | bool | **false** | auto-arms `cycle_slip.gap_instrument` with a WARN |
| *(+4 further `network.block_baarda*`)* | — | — | seven landed in total (§12.6) |
| `network.congruence_screen` | bool | **false** | **HARD ERROR** without `redundancy_census` |
| `network.congruence_report_sigma` | double | **4.0** | reporting only |
| `network.congruence_event_holdoff_ep` | int | **20** | reporting only |

---

## 15. Consolidated OWED / gate ledger

**Prerequisite for every row below: a CONVERGED WARM WINDOW (§9).**

| # | owed | owner | blocker / note |
|---|---|---|---|
| 1 | Host §6 gates 2, 3, 4, 6, 7 — the leg-19 park / valve / fallback / product claims | host | needs the `runs/ft63_leg19_rhsat` control lineage; gates 1 and 5 are MET |
| 2 | **§11.6.2** — the FT63 BSHM known answer: those rows' MDB finite and ≪ 1.6e5 m, and the healthy uncheckable risers at ∞ | rider A | the smoke is a COLD DOY-129 00:00 start and does not contain the event; needs the leg-19 converged window |
| 3 | **§11.6.3** — a converged (≥2 h) MDB headline table, formal AND scaled | rider A | every §11.10 number is inside ignition; **do not quote the cold σ̂₀ or the cold uncheckable fractions as the service answer** |
| 4 | **All of §12.8** — rider B has **NO FIELD RUN**: OFF byte-identity on a real lineage, live wiring, cost vs the ≤2× budget (`counters.bb.wall_ms`), the RESPONDED-vs-CLEAN positive control, the contingency b-cell, BB-INC-3's smearing fraction | rider B | pre-registered prediction in §12.12 says the `block_only` fraction should be SLIP-driven; ≈ 0 on a multipath-dominated population is the honest close |
| 5 | **§13.7.2** — the +5 cm East perturbed arm (the KNOWN ANSWER) | rider C | diagnostic lineage only; run at the same stride as the unperturbed control |
| 6 | **§13.7.3** — the per-station ENU coordinate-MDB table on a converged warm window, BOTH lineage regimes | rider C | the card's service deliverable |
| 7 | **§13.7.4** — CG-INC-3 co-occurrence | rider C | needs `network.block_baarda` armed alongside; the smoke ran with it off and the run-end line correctly reports the join as **UNMEASURED** rather than as agreement — read it against the epoch's satellite count (kS² finding) |
| 8 | **§13.7.5** — solve byte-identity on a graded lineage, not an 11-epoch smoke | rider C | |
| 9 | **MDB-INC-2** — external reliability (`\|Δ emitted product\|` for an MDB-sized fault) + leave-one-station-out solution separation | own card | DIA catalog item 5; named in §11.2 / §12.9 / §13.8 so the omission reads as deliberate |
| 10 | **Code item** — collapse the duplicate O(rows) centred-metric walk by having the prefit census publish per-station `max_s` (§8 deviation 1); both cards have landed their INC-1 and the duplicate is still live at `NetworkRedundancyCensus.cpp:97-106` | code, not docs | |

**Promotion status**: none of the four knobs has a `docs/CONFIG_promotion_ledger` row recommending it;
rider A has a ledger entry recording its interlock.  Per the no-promotion directive, arming goes into a
config template, never into a loader default.
