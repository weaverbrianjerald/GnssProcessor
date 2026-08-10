# SPEC — redundancy / testability census (the statistical parked-garbage monitor)

**Status**: SPEC'd 2026-08-01 (Fable session). Build in a fresh session
(assigned: Opus, after or alongside `SPEC_prefit_census_observability.md` —
INC-0/INC-1 of that card are prerequisites here).
**Memory context**: `project_ft63_fast_tier` (era-2), 
`reference_network_fault_qc_literature` (Baarda MDB/MIB — the "unported" DIA
piece this card starts porting), `project_network_ssystem_dia_assessment`.
**Scope discipline**: default-OFF, OFF byte-identical, NOT fingerprinted.

---

## 1. Why redundancy, not magnitude (the design argument)

Era-2's parked cell had a steady 2.6·10⁵σ common-removed prefit innovation —
but magnitude alone is an ambiguous discriminator, and the obvious hard
responses are wrong:

- **Raw magnitude is legitimate**: receiver clock offsets/steps put ms-scale
  (10⁵ m) innovations on whole stations (URUM/INEG take clean 1 ms steps in
  the era-2 leg window).  The census card handles this with common-mode
  removal, but centering only cleans the CLOCK direction — it cannot say
  whether a per-cell excursion is a fault or an extreme.
- **Excluding single-frequency cells as a class is wrong** (user directive:
  prefer fallback over drop).  Single-freq LOS are valuable data; their
  problem is not their band count, it is that under certain topologies they
  become UNCHECKABLE.

The principled discriminator is **testability**.  Per row, the local
redundancy number

    r_i = 1 − w_i · (H Σ Hᵀ)_ii            (r_i ∈ [0, 1])

says what fraction of that row's information is cross-checked by the rest of
the network.  Baarda: the minimal detectable bias scales as
MDB_i = δ₀·σ_i/√r_i → **∞ as r_i → 0**.  A row with r_i ≈ 0 is a pure
state-feeder: its misfit is INVISIBLE to every residual-based test — postfit
RAIM, σ̂₀, robust weighting — no matter how large.  That is exactly what the
(BSHM, G07) code row became when the deweight gate cut the station's SH ties:
one observation feeding one otherwise-unconstrained slant state, r ≈ 0, free
to park 188 km without a single residual anywhere moving.

**The alarm condition is the CROSS-FLAG:**

    uncheckable (r_i < ε)  ×  large centered innovation (s_i > S)

Neither half alone suffices: healthy single-freq risers are uncheckable with
small s (fine — keep them, per the fallback directive); storm extremes have
large s with healthy r (fine — the redundant network absorbs and localizes
them).  The intersection is parked garbage with essentially zero
false-positive surface, and it fires DURING the park (era-2: 17 epochs of
warning), not at the discharge.

## 2. Design — two censuses and the cross-flag

### INC-1: row-redundancy census (`network.redundancy_census_stride`, int, default 0 = off)

Every Nth processed epoch (and ALWAYS on flight-recorder trip epochs, §4):

- Per station, per data class: n_rows, Σr_i (the redundancy mass; network
  total must satisfy Σr = n_rows − n_effective_states — cheap closure check,
  invariant-#4 style), n_uncheckable = #{r_i < `redundancy_eps`} (double,
  default 0.05), and among uncheckable rows the max centered s_i (from the
  census card's INC-1 metric) with its (station, sat, class) identity.
- Emit: per-epoch scalars into `network_epoch_status.csv` (n_uncheckable_rows,
  uncheckable_max_s network-wide); identities via `network_events.csv` tier
  REDUNDANCY, event `uncheckable_hot_cell`, when the cross-flag
  (r < ε AND s > `redundancy_hot_sigma`, double, default 1000) fires —
  rate-limited like the PREFIT events.
- Computation: `NetworkEstimator::rowRedundancy` already exists (used by the
  RAIM attribution path) over the datum-frame shared covariance
  (`datumFrameSharedCov`) + per-station reconstruct.  The implementing
  session re-uses it verbatim; no new algebra.

### INC-2: unobservable-local census (same stride, no extra knob)

The state-side complement: per station, count local columns whose N_rr LDLT
pivot is below `redundancy_pivot_eps` (double, default 1e-8 relative to the
station's median pivot) — states carrying NO information this epoch (fully
orphaned; the one-obs-one-state case shows in INC-1 instead).  Needs one
small kernel accessor (per-station `vectorD` of the cached LDLT — read-only,
the factor exists).  Identities via `h2local` labels; event
`unobservable_local` on first appearance per (station, key) per era.

### The formal MDB extension (deferred, pre-scoped)

Reporting per-class MDB percentiles (δ₀·σ_i/√r_i at fixed α, β) is the full
Baarda port the DIA catalog lists as open.  INC-1's r_i census is its
computational substrate — the extension is a formatting/statistics layer, one
later increment, NOT this card's gate.

## 3. Cost model and the two-tier escalation (measured anchors)

D0 timers on the era-2 leg (63 sta, ~450 shared cols): cov-consumers 158 ms /
697 calls ≈ 0.2 ms per covariance access; reconstruct + per-station GEMM for
a full row-redundancy sweep estimated 100–300 ms/epoch — NOT free at the
1.6–1.8 s/ep budget (would cost 5–20%).  Hence:

- **Stride default 10** (one sweep per 5 min at 30 s cadence) — the park
  signature persists for many epochs (era-2: 17+), so stride-10 detection
  latency is minutes, acceptable for an alarm.
- **Escalation trigger** (free→costly, targeted): when the census card's
  INC-1 (free, every epoch) reports any class max_s > `prefit_event_sigma`,
  run THIS card's sweep for the flagged STATION only at the next epoch
  (single-station cost ~2-5 ms).  The free census asks "is something big?";
  redundancy answers "is it checkable?" — exactly where and when it matters.
- Always run a full sweep on flight-recorder trip epochs (the attribution
  dump and the redundancy census describe the same epoch).

## 4. Wiring sites

- `NetworkEstimator::rowRedundancy` (+ `datumFrameSharedCov`) — existing.
- Kernel: per-station LDLT `vectorD` accessor (new, read-only, ~5 lines next
  to `stationLocalNormals`).
- Sweep seam: a new `filter/NetworkRedundancyCensus.cpp` (+ manual CMake
  entry), called from the driver's SB_FRAMEDIAG block next to
  `networkPostfitSigma0` / `networkRhsAttribution` — same ingredient list
  (stations, est, asm_res, est_index_of); reuses the hoisted classifier
  (census card INC-0).
- Escalation channel: a ctx.carry flag set by the census walk (station index
  + one-epoch latch), consumed here next epoch.
- Config: `NetworkConfig` fields near `rhs_attrib_tow`; loader near the
  rhs_attrib block with arming lines + an INERT-guard WARN if the stride is
  armed without `trace_prefit_census` (the cross-flag needs the s metric).
- Counters + run-end line: `NetworkServiceCounters` / `NetworkTeardown.cpp` —
  sweeps run, cells flagged, escalations taken, closure worst-case.

## 5. Unit tests

New file `tests/test_redundancy_census.cpp` (one suite): (1) r_i on a
hand-built 2-station system matches 1 − w·(HΣHᵀ)_ii; (2) one-obs-one-free-
state cell reports r ≈ 0; adding a second coupled row raises r; (3) Σr closure
= n_rows − n_states on the toy system; (4) pivot census flags an orphaned
local column; (5) cross-flag requires BOTH halves; (6) stride/escalation
gating; (7) OFF byte-identity (structural).

## 6. Field validation — KNOWN-ANSWER gates (standing harness)

Controls: `runs/ft63_leg19_rhsat` (+ its rows/attribution dumps).  Distinct
run dir (`ft63_leg19_redund`); deweight-hygiene knobs OFF (interaction
discipline as in the census card §12).

1. Smoke + OFF-identity as usual.
2. **The park is visible**: from ~328020 through 329130, (BSHM00ISR, G07,
   obs_code) reports r < 0.05 with centered s ≈ 2.6e5 → `uncheckable_hot_cell`
   fires (rate-limited), naming the cell **~17 epochs before the break** on a
   stride-1 diagnostic leg; on stride-10 it fires within 2 sweeps.
   (BSHM, G16) likewise.
3. **The valve is visible**: at 329160 (tie rows re-folded) the same cells'
   r_i RISES (the slant re-couples to the field) — the census records the
   uncheckable→checkable transition at the discharge epoch, corroborating the
   topology-ledger event from the census card.
4. **No fallback-violation**: healthy single-frequency risers elsewhere in
   the window appear as uncheckable-with-small-s (counted, NOT evented, never
   weight-touched) — enumerate a few and confirm zero hot-cell events for
   them.
5. **Escalation path**: with stride 0 (sweep off) + escalation armed, the
   BSHM hot cell still gets named within one epoch of the first PREFIT event.
6. Solve byte-identical to control in every armed leg (measure-only card).
7. Product claims on total_IF only.

## 7. Deferral triage (pre-declared)

- Full MDB/α/β reporting + solution-separation integration: later increment
  on top of INC-1's r_i (the catalog's open port; this card is its
  substrate).
- Consumer/RT engine port: after producer validation.
- Any automatic weight/exclusion response to the cross-flag: NOT this card
  (measure-only).  The optional fuse composition lives in the census card
  (`prefit_fuse_require_uncheckable`) and arms only after both cards'
  independent validation.

---

## 8. BUILD RECORD (Opus session, 2026-08-01) — implemented, smoke-gated

Code: `include/filter/NetworkRedundancyCensus.hpp`,
`src/core/filter/NetworkRedundancyCensus.cpp` (seam, CMake entry added),
`NetworkNormalEquations::stationLocalPivots` (the INC-2 kernel accessor),
6 `network.redundancy_*` knobs + loader guards, `NetworkKnobs::redun_census_on`,
carry/counter blocks in `NetworkRunContext`, 4 epoch-status columns, run-end
telemetry in `NetworkTeardown.cpp`, `tests/test_redundancy_census.cpp`
(10 cases, one suite).  INC-0 was consumed from the concurrent census session's
`NetworkRowClassify` hoist — one classifier, one truth, as specified.

### Deviations from this spec, all deliberate

1. **The centred metric is computed HERE, not consumed from the census card.**
   That card had not landed its INC-1 when this was built.  Same definition,
   same hoisted classifier; the cost is one extra O(rows) pass when both are
   armed.  Consequence: **no INERT-guard WARN is needed** (§4) and the
   escalation channel is live on its own.  A later increment can collapse the
   duplicate walk by having the prefit census publish per-station `max_s`.
2. **Escalation threshold is its own knob** (`redundancy_escalate_sigma`, 0 =
   off) rather than borrowing `prefit_event_sigma` — same reason, and it keeps
   the two cards independently armable.
3. **The closure check is an INEQUALITY, not the equality of §2.**
   `Σr = n_rows − n_states` holds only for an unprior'd, fully-swept,
   unclamped system.  The producer folds prior information (carries, DD, Kaula,
   datum) and the kernel clamps `r` to [0,1], so the defensible invariant is
   `trace(H) = Σ(1−r) ≤ n_free`, which survives restriction to any subset of
   rows and is therefore also valid on a targeted single-station sweep.
   `n_free` is measured as the non-zero columns of the datum-frame covariance
   (so datum pins AND ensemble phase gauges are excluded without this file
   knowing which mechanism zeroed them) plus the swept stations' local
   dimensions.  Reported as the worst slack; negative ⇒ WARN + "untrusted".
4. **A 4th epoch-status column, `n_redun_rows`** (the denominator).  A raw
   uncheckable count is uninterpretable without it.
5. **INC-2's firable window is bounded from below by the kernel.**
   `addStation` already rejects a station whose smallest local pivot is under
   1e-12 × block scale, so INC-2 can only fire in [1e-12, `pivot_eps`) × median
   — four decades at the 1e-8 default, EMPTY at or below 1e-12.  The loader
   warns about the latter; a quiet INC-2 must be read against the knob.

### Smoke gate (FT63, 63 stations, 10 epochs, COLD, Release)

Arms `runs/ft63_redund_{off,on,escal,diag}`, configs
`config/test_suite/_ft63_redund_*.json`.

- **OFF-identity PASS.** `network_solution.csv` byte-identical OFF vs ON, as
  are all 36 other solve/QC artifacts; `network_epoch_status.csv` has **zero**
  non-census column differences.  The 7 differing files are the 4 census
  columns, the REDUNDANCY events, the two config/telemetry logs, the manifest,
  and two wall-clock fields (`network_run_summary` ms, `service_health` ms).
- **Live wiring.** 11 passes / 11 sweeps / 692 station-sweeps; 35 948 data rows;
  events carry full identities `(station, sat, class, r, eps, y, w, row)`.
- **Closure holds**: worst slack +84.5 over 11 sweeps.
- **Trip-forced sweep works** (`_diag` arm, `rhs_attrib_tow` armed): "1 forced
  by a flight-recorder trip"; the census and the attribution dump the same
  epoch.  The predicate also reads the concurrent card's INC-5
  `rhs_trip_pending`.
- **Two-tier escalation works and is the headline result** (`_escal`, stride 0):
  the free tier raised 17 latches, 15 became targeted sweeps, and the arm found
  **the same worst cell** (URUM00CHN/G22/obs_phase, s = 3.95e6) using **15
  station-sweeps / 194 ms instead of 692 / 4126 ms — 21× cheaper**.
- **Cost**: 6.1 ms per station-sweep ⇒ ~380 ms/epoch for a full 63-station
  sweep.  The §3 estimate was 100–300 ms; the stride is doing real work.
- **INC-2 fired 0** over 26 466 columns examined — expected, see deviation 5.
  The denominators are in the run-end line, so this reads as "ran, found
  nothing", not as "never ran".

### What the cold smoke does NOT establish, and why

Both halves of the cross-flag are structurally inflated during convergence and
a reader must not mistake this for a defect (documented at length in the seam
header):

- **43 % of data rows read uncheckable** on a cold start — a FREE ambiguity
  absorbs its own arc's phase misfit exactly, the same property that makes the
  post-fit phase pool identically zero.
- **A freshly seeded arc cross-flags**: its seed makes the arc's first phase
  innovation ~0, but at a station whose receiver clock is not yet absorbed into
  the linearization point the rest of that station's phase rows sit AT the
  clock offset (measured here: FFMJ00DEU 259 km, URUM00CHN 42 km — timing-lab
  receivers — against a 0.23 m median across the other 61 stations).  The fresh
  arc is then the outlier against its own station's median, at r = 0.  Measured
  40 hot cells / 11 cold epochs = 0.11 % of data rows, concentrated on exactly
  this signature.

Both statements are TRUE of the system, so neither is suppressed.  The
procedural consequence: **the §6 known-answer gates must run on the converged
warm leg-19 window, not on a cold smoke.**  §6 gates 1 and 5 are met; gates
2, 3, 4, 6, 7 (the leg-19 park/valve/fallback/product claims) are OWED and
require the `runs/ft63_leg19_rhsat` control lineage.

Incidental find, handed to the companion card: the exactly-zero phase rows on a
newly risen arc at a clock-carrying station are the **seed-birth signature**
that card's INC-3 exists to locate — one station in 63 at the sampled epoch.
