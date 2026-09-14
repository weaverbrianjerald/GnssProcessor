# redundancy census — the row-redundancy and unobservable-local censuses — design, cost model, wiring, build record, convergence hygiene and governance

**STATUS**

- **id:** redundancy census
- **topic:** raim-qc
- **status:** closed
- **summary:** the row-redundancy and unobservable-local censuses — design, cost model, wiring, build record, convergence hygiene and governance
- **dates in the text:** 2026-08-01 → 2026-08-15
- **knobs / posture:** `network.mdb`, `network.block_baarda`, `network.congruence_screen`, `network.redundancy_census_stride`, `network.redundancy_eps`, `network.redundancy_pivot_eps`, `network.redundancy_hot_sigma`, `network.redundancy_escalate_sigma`, `network.mdb_event_m`, `network.mdb_event_holdoff_ep` … (+4 more in the text)
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/ft63_leg19_rhsat`, `runs/ft63_redund_`
- **source:** `docs/archive/SPEC_redundancy_census.md` — (preamble), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10.1, 10.8, 14, 14.1, 14.2, 14.3, 14.4, 14.5, 14.6, 14.7, 15

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_redundancy_census.md -->
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
