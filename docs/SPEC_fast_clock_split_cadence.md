# SPEC — FC-1: Split-cadence clocks-only fast pass (network producer)

*2026-07-28 (Fable). Implementer: Opus. Companion: `docs/REPORT_timing305_allon_30s_10ep.md`
(the measured basis), `docs/SPEC_c1_clock_rate_validation.md` §5.3 (the R11
emission-continuity lesson this card must not re-create), `docs/AUDIT_consumer_product_gap.md`
(staleness horizon), the product legend grammar work (`docs/SPEC_product_cbpin_contract.md`).*

---

## 0. Motivation (measured, not conjectured)

The Jul-28 all-on 305-station GPS 30 s baseline measured **27.72 s/epoch** — 0.92× real
time at 30 s cadence — decomposed as: Card-4 carry pipeline 56.8 %, dual recon sweeps
25.1 %, assembly 11.4 %, reduce 4.9 %, **border solve 0.1 % (41 ms)**. The optimization
ladder (P1 fusion, MKL Cholesky, D1) tops out near ~22 s/epoch; C2 proved the dense-K
carry cost is intrinsic. **A 1 s product cadence is unreachable by optimizing the
per-epoch pipeline at 305 stations.**

What IS cheap is the estimation itself once the expensive structure (ambiguities, iono,
biases, datum, QC) is known: the actual solve is 41 ms. IGS-RTS / commercial SSR
services exploit exactly this: orbits/biases at slow cadence, **clocks measured at the
served rate**. Our own C1 card supplies the theoretical mandate: the emitted c0 is a
random walk (0.157 m per 30 s), so ZOH is already MMSE *given* a 30 s solve — the only
way to serve better 1 s clocks is to **measure** them at 1 s. Rates (C1 emission) were
measured NET-HARMFUL and stay dead.

FC-1 therefore adds a second estimation tier: the full pipeline (solve + carry + QC +
emit) continues at its cadence exactly as today; between full solves, a **clocks-only
fast pass** measures per-satellite clock deltas against the last emitted full solution
and emits them. Everything else is frozen and rides ZOH — which is what it already does
inside a 30 s era today, just without the clock refresh.

**The same machinery serves both regimes:**

| Regime | Data rate | Full solve | Fast tick |
|---|---|---|---|
| Dev/validation (now) | 30 s RINEX, 305 sta | every N·30 s (N=10 ⇒ 300 s) | 30 s |
| RT target (future) | 1 s stream | every 5–30 s | 1 s |

The dev regime is not a toy: it validates the architecture on existing data with a
same-data full-rate reference run as truth (§7). Nothing in the fast pass knows the
tick interval.

---

## 1. Architecture and driver hook

The epoch loop already contains the exact seam: **T2e epoch decimation**
(`src/core/app/ApplicationNetwork.cpp:1053-1061`) — epochs decimated out by
`network.epoch_decimation_n` hit a bare `continue` *after* the start-offset /
max-duration guards and the brdc_rts nav catch-up, *before* `++n_loop_epochs`. Those
epochs have fully synced observations (`se` in `for (const auto& se : synced)`,
`ApplicationNetwork.cpp:1006`) — they are simply not processed today.

FC-1 semantics: with `network.fast_clock.enable=true`, a decimated-out epoch runs
`networkFastClockTick_(...)` instead of the bare `continue`. With the flag false
(default) the `continue` is untouched — **byte-identical, structural**. Full epochs
(`decim_counter % dn == 0`) are untouched in either case; additionally, `shadow` mode
(§7.1) runs the fast tick on full epochs immediately before the full pipeline for
grading, with no effect on any downstream state.

Fast ticks are NOT loop epochs: they don't increment `n_loop_epochs`, don't touch the
D0/D0b brackets, don't enter assembly/solve/carry/QC/emit, and mutate **nothing**
outside their own carry struct (§5). The full pipeline must be provably indifferent to
whether fast ticks ran — that is the core isolation contract, and it is what makes the
card safe to land incrementally.

---

## 2. Estimation design

### 2.1 Formulation: per-band delta-from-fit, IF-combined, phase-only

Do NOT reconstruct absolute models or touch bias/ambiguity conventions. The fast
observable is the **change of the fitted phase since the anchor epoch** t₀ (the last
*emitted* full solve; "emitted" is load-bearing, §4.4/§9).

At t₀, for every used LOS (station r, sat s, band j), harvest the post-fit fitted phase
`Φ̂ⱼ(t₀) = Φⱼ_obs(t₀) − vⱼ(t₀)` (obs minus post-fit residual) and split it as

```
Φ̂ⱼ(t₀) = ρ(t₀) + Mⱼ(t₀)          ρ = geometric range (light-time, station ECEF fixed)
```

`Mⱼ` is the *lump*: everything the full solve modeled or estimated that is not the
instantaneous range — clocks, tropo (mf·ZWD), iono (μⱼ·I), ambiguity, phase biases,
windup, PCO/PCV projections, tides, shapiro, relativity. Also harvest the lump **rate**
`Ṁⱼ` by finite difference against the previous emitted full epoch (this is why the fast
pass arms only after **two consecutive emitted** full epochs).

At fast tick t (Δt = t − t₀), per surviving LOS and band:

```
yⱼ(t) = Φⱼ_obs(t) − [ ρ(t) + Mⱼ(t₀) + Ṁⱼ·Δt ]
```

with ρ(t) recomputed exactly (SP3 interpolation + light-time + Earth rotation — the
ONLY nonlinear term recomputed per tick; everything else rides the linear lump). Then
IF-combine the arc's two cached bands: `y_IF = IF(y_j, y_k)`. What survives in y_IF:

```
y_IF(r,s,t) = δτ_r(t) − δc^s(t) + ε
```

- **δc^s** — satellite clock change since t₀ beyond the linear trend already in Ṁ.
  This is the product.
- **δτ_r** — receiver clock change (incl. ms jumps), re-estimated fresh per tick
  (white); nuisance.
- Iono change: **eliminated by IF** (μⱼΔI cancels in the combination of per-band
  deltas). Frozen ambiguities/biases/windup/tides/tropo: cancel in the delta or are
  absorbed to harmless order by Ṁ·Δt (budget in §3).

Every tick measures against the **same** t₀ anchor — deltas are not chained
tick-to-tick, so per-tick measurement errors do not random-walk across the fast window.

Single-band arcs: excluded by default (`allow_single_band=false`) — a lone band leaves
μⱼΔI in y. At 1 s RT the knob may admit them with iono-rate-inflated R; at 300 s dev
cadence they are useless. (This is data-driven, not a "drop measurements" policy — the
dual-band delta is the fallback structure itself.)

Phase-only. Code deltas add nothing at the cm level and import code-bias freshness
questions; not used.

### 2.2 Robust fit and screen

~2 400 LOS, ~303 + 32 unknowns, bipartite structure:

1. **Median seed** (robust to any single wild LOS / receiver ms jump): per-station
   median of y_IF → δτ_r seed; per-sat median of (y_IF − δτ_r) → δc^s seed. This is the
   `medianPolish2Way` row/column structure already proven in the clock-QC seam
   (`filter/NetworkClockQc`) — reuse the helper, do not reimplement.
2. **One WLS pass**, elevation-weighted R (existing stochastic constants × IF noise
   factor), station clocks Schur-eliminated (scalar diagonal blocks) → dense n_sat×n_sat
   SPD solve. Serial, deterministic, Eigen single-threaded — this is microseconds; do
   NOT parallelize.
3. **Per-LOS screen** on the post-fit cell residual (after removing both δτ_r and
   δc^s): `|res| > k·σ_pred` with `k = fast_clock.screen_k_sigma` (default 5) and
   σ_pred from the elevation R plus the ClockOsc-scaled prediction spread
   (`clockOscPredict`, `include/filter/NetworkRunTypes.hpp:365`, with the shipped
   Allan-derived q2 = 1.5e-7 m²/s³, `GnssConfig.hpp:4709`; per-1 s that is ~3 cm, per
   Δt scale √Δt). A screened LOS is excluded from THIS tick (re-tested next tick —
   fine-slip reset-not-repair discipline, same philosophy as D5), counted, and its arc
   flagged in the fast carry so the run-end readout attributes it. A cell breach on one
   station only = slip suspect. A whole-row (sat) coherent move is NOT screened — it is
   δc^s, the very signal being measured.
4. **Estimability floors**: sat needs `min_stations_per_sat` (default 3) unscreened
   dual-band LOS *with live t₀ arcs* to be fast-estimated this tick; station needs
   `min_sats_per_station` (default 2) to contribute. Sats below the floor this tick emit
   nothing new (consumer keeps the last emitted value; age grows — never fabricate).

### 2.3 Datum / frame contract (do not deviate)

- Fast datum = the SAME `network.clock_datum_station` pin as the slow solve:
  `δτ_datum := 0`. **No re-election inside a fast window, ever.** If the datum station
  is absent/screened at a tick: the tick is **withheld entirely** (counter + log once
  per window), not re-datumed. Cheap, rare, and it preserves the frame exactly.
- The emitted fast clock is `c0^s(t) = c0^s_emitted(t₀) + δc^s(t)` — a delta on the
  *emitted* value, so it lives in the emitted frame by construction. Any per-era frame
  constant (re-base, datum, pivot, IF-pin realization) cancels in the delta **because
  re-bases/re-elections/re-levels only ever happen inside the full pipeline**, i.e., at
  era boundaries. The fast pass must not import, recompute, or "correct" any frame
  quantity. If the next full solve re-bases, its own emitted record re-anchors
  everything — the fast window simply ends.
- Consequence of anchoring on *emitted* t₀: a withheld full epoch (observability gate,
  A4 ceiling, SH gate) does NOT advance the fast anchor. The fast window keeps running
  against the last emitted era — measured against the same frame the consumer holds —
  until `emit_max_gap_s` expires (§4.4).

---

## 3. Staleness budget (what "frozen" costs)

| Term | Handling | Error over Δt ≤ 30 s (RT) | over Δt ≤ 300 s (dev) |
|---|---|---|---|
| Sat position | recomputed (SP3 interp + light-time) | — | — |
| Iono | IF-eliminated | 0 (1st order) | 0 (1st order) |
| Ambiguity, phase bias | exact cancel in delta (live arc) | 0 | 0 |
| Tropo mf·ZWD | in lump + rate | sub-mm | ≲1 cm low-el (in Ṁ residual) |
| Windup, tides, PCO/PCV proj., shapiro | in lump + rate | sub-mm | mm-class |
| Sat clock beyond linear | **measured** (this is δc^s) | — | — |
| Higher-order lump curvature | residual of Ṁ linearization | negligible | absorbed into grading (§7) |

Eclipsing satellites (yaw-maneuver windup curvature) are already excluded by config
(`exclude_eclipsing_satellites`). The budget says: at the RT tick the frozen-state
error is sub-mm — the fast clock quality is limited by phase noise and the frozen-arc
assumption, not by staleness. At the dev cadence the low-elevation lump curvature is
visible but graded (§7), and common-mode components of it cancel in the SD grading.

---

## 4. Event responses — standing invariant #1 pre-flight

Frozen arcs and lumps are stiff time-linked quantities. Every one has a wired, tested
response BEFORE any ON field run:

1. **Cycle slip inside the fast window** → the §2.2 screen excludes the LOS that tick
   (a slipped arc's delta jumps by λ·ΔN ≫ k·σ). No repair, no refloat in the fast tier
   — the next full solve owns repair via the existing slip/carry machinery. Counter:
   `fast_screen_slips`. Test: unit-inject a λ jump into a cached lump → LOS screened,
   sat still estimated from remaining LOS, counter fires.
2. **Receiver ms clock jump** → absorbed exactly by the fresh per-tick δτ_r (all of a
   station's LOS move together; the median seed keeps the fit robust while it happens).
   Test: inject +1 ms on one station's obs → δτ_r ≈ 299 792.458 m, zero LOS screened,
   sat clocks unmoved (assert ≤ noise).
3. **Sat rises after t₀** → no cached arc → not fast-estimable → not emitted (consumer
   ZOH + age). **Sat sets / arc dies** → fewer LOS; below floor → sat stops being
   refreshed. Never extrapolated. Counters: `fast_sat_below_floor`.
4. **Emission-continuity gate — the R11 lesson** (`SPEC_c1_clock_rate_validation.md`
   §5.3: R11 gated on *solve* continuity while the consumer experiences *emission*
   continuity; `GnssConfig.hpp:4763`). The fast gate is keyed on **emitted** times
   only: if `t − t_last_emitted_full > fast_clock.emit_max_gap_s` (default 60 s), fast
   emission STOPS (ticks may still run in shadow for telemetry). A withheld full era
   longer than the gap therefore silences the fast tier rather than letting it ride a
   stale frame — degrade-by-silence, never degrade-by-fabrication. Counter:
   `fast_gap_withheld_ticks`. Test: force a full-epoch withhold streak → fast rows stop
   at the bound.
5. **Datum station lost at a tick** → tick withheld (§2.3). Counter:
   `fast_datum_withheld_ticks`.
6. **Warm resume** (RTB-7a): `FastClockCarry` is deliberately NOT in the snapshot — a
   resumed run arms fast ticks only after two fresh emitted full epochs. Zero
   interaction with `NetworkStateSnapshot`.

---

## 5. Where the code goes (anti-regrowth decision tree, CLAUDE.md)

- **New seam** `include/filter/NetworkFastClock.hpp` + `src/core/filter/NetworkFastClock.cpp`
  (manual `CMakeLists.txt` entry; full Doxygen in the house style). Free functions over
  `(NetworkRunContext& ctx, const GnssConfig& config_, log, NetworkRunTraces&, …)` with
  the alias preamble, owning NO state:
  - `networkFastClockHarvest_(…)` — called from the emit-success path
    (`filter/NetworkEmitStore`; the "kept epoch" gating discipline documented at
    `NetworkRunContext.hpp:333-342` — every whole-epoch withhold `continue`s before the
    emit site, which is exactly the gate the harvest must sit behind). Harvests per-LOS
    `{bands, Φ̂ⱼ, ρ(t₀), Mⱼ, Ṁⱼ}` + `c0^s_emitted(t₀)` + `t₀`. Piggyback an existing
    per-station post-solve sweep for the residual read (one-sweep-many-consumers
    doctrine — coordinate with P1 sweep fusion if it lands first; do NOT add a new
    `reconstructStation` sweep).
  - `networkFastClockTick_(se, ctx, …)` — the decimation-hook body (§1): geometry
    recompute, deltas, median seed, WLS, screen, trace row, emission handoff.
- **Carried state** → `struct FastClockCarry` inside `NetworkCarryState`
  (`include/app/NetworkRunContext.hpp` — next to `clock_osc`, `:284`). Contents: the
  harvest maps, arm state (2-emitted-epoch counter), `t_last_emitted_full`, per-arc
  screen flags. No bare function-locals in the driver (INC-12 lesson).
- **Counters** → `NetworkServiceCounters` (`ticks_driven, ticks_emitted, los_used,
  los_screened, fast_screen_slips, fast_sat_below_floor, fast_datum_withheld_ticks,
  fast_gap_withheld_ticks`), read out in `filter/NetworkTeardown` next to the D0 block
  (run-end counters ARE the ON-path telemetry — invariant #4).
- **Trace** → `network_fast_clock.csv` owned by `logging/NetworkRunTraces` (per tick:
  tow, Δt, n_los, n_screened, n_sat_est, per-sat δc^s + σ on a stride knob). Stream
  registered/closed exactly like the existing 14.
- **Config** → `network.fast_clock.{enable, shadow, emit, min_stations_per_sat,
  min_sats_per_station, screen_k_sigma, allow_single_band, emit_max_gap_s}` — POD
  sub-struct in `include/config/GnssConfigTypes.hpp`, parsed in `loadNetworkSection_`
  (`src/core/config/GnssConfigLoadNetwork.cpp`) via `jval`, getters + Doxygen +
  summary-toggle log line + config-echo, per the CLAUDE.md toggle convention.
- **Emission** → §9, extending `NetworkSolPrep`/`NetworkEmitStore` + `NetworkProductIO`.
- **Perf timing** → its own `[TIMING]` accumulator (ticks are outside D0): run-end line
  `fast ticks: N, wall X ms, Y ms/tick` + max tick.

Driver delta: ~6 lines at the decimation hook + the harvest call at the emit site.
Everything else lives in the seam. Keep it that way.

---

## 6. Increments

**INC-0 — plumbing.** Config block, `FastClockCarry`, seam skeleton (functions no-op),
counters, trace registration, CMake. Gate: default-OFF run of
`_net305_timing_gps_allon_10ep.json` byte-identical (same build) on: products.txt, all
CSVs, run-end counters. Suite green.

**INC-1 — harvest.** Fill the carry at emit-success; arm after 2 consecutive emitted
epochs. Log-only otherwise. Gate: byte-identity everywhere (harvest reads, never
writes, pipeline state); unit tests for arm/reset on withhold streaks and resume-cold.

**INC-2 — tick estimation, log-only.** Full §2 fit at decimated epochs + shadow mode;
trace rows; NO emission. Gates: (a) shadow run on the 10-ep all-on config: shadow
δc^s vs the same epoch's full-solve emitted clock delta — RMS ≤ 2× the full solve's
formal σ_clock, zero screened-LOS storms; (b) unit tests: §4 injections (slip, ms
jump, datum loss, floors); (c) determinism: two identical runs → identical
`network_fast_clock.csv`.

**INC-3 — emission + store.** §9 record + `SsrCorrectionStore` push + replay/tail
parity + the R11-keyed gap gate. Gates: `fast_clock.emit=false` ⇒ INC-2 behavior
byte-identical; ON ⇒ products.txt with fast rows replays into a store whose slow-era
state is byte-identical to the fast-rows-stripped file (consumer clock values differ
ONLY at fast epochs, biases/PB/iono ages untouched by fast rows).

**INC-4 — cadence-split A/B (the decisive gate).** Same build, same data
(305-sta/GPS/30 s, ≥ 2 h after a 1 h warm-up; staged cadence: 10-ep smoke first):
- REF: `epoch_decimation_n=1` (full solve every 30 s) — today's mode.
- SPLIT: `n=10` + fast_clock on (full at 300 s, ticks at 30 s).
Grade emitted satellite clocks at tick epochs, SPLIT vs REF, **common-mode removed
per epoch** (datum-invariant — invariant #5; raw wire steps confound frame drift):
- `SD-RMS(fast vs REF) ≤ (1/5) · SD-RMS(ZOH-of-300s-era vs REF)` — the fast pass must
  beat the C1-proven ZOH floor by ≥ 5× (ZOH error at 300 s ≈ 0.157·√10 ≈ 0.5 m; target
  ≪ 10 cm).
- No per-sat systematic vs elevation/screening; screen rate < 1 % of LOS in quiet data.
- REF-vs-REF noise floor (±5 %) respected: run on a quiet box.

**INC-5 — deferred (triaged field-inert for this card, invariant #3):** RT transport
(RTCM/SSR encode), 1 s high-rate data pull, consumer positioning A/B with fast clocks,
single-band admission at 1 s. Each is a follow-on card; none blocks FC-1's verdict
because the split-cadence A/B (INC-4) validates the architecture on existing data, and
the default-OFF flag keeps every shipped path untouched.

---

## 7. Validation instruments

**7.1 Shadow mode** (`fast_clock.shadow=true`): run the tick computation on FULL epochs
too, immediately before the full pipeline, logging fast-vs-full deltas. Zero downstream
effect (gate: byte-identity of everything but `network_fast_clock.csv`). This gives
per-epoch truth grading on ANY run for free and stays as a permanent regression
instrument (analogous to the M2 LS-VCE monitor: instrument, never feedback).

**7.2 ZOH benchmark**: computed inside INC-4's grader (pyviz — extend
`pyviz/run_compare_n.py` lineage): per tick epoch, ZOH = last SPLIT full-era emitted
clock. The comparison is what justifies the card's existence; report it honestly if
fast ≤ ZOH anywhere (that would be a NO-GO finding, not a tuning knob).

**7.3 Perf**: per-tick wall ≤ **50 ms @ 305 sta** (budget for 1 s RT with 20× headroom
for the stream/decode path; expected ~5–10 ms: ~2 400 light-time recomputes + a 32×32
solve). Measured in the run-end `[TIMING]` line. If a tick exceeds budget, profile
before threading — the §2.2 solve must stay serial-deterministic.

---

## 8. Emission / product contract (INC-3)

- Grammar: fast epochs append a compact record to the SAME products file consumed by
  `NetworkProductIO` (`include/products/NetworkProductIO.hpp:34` — `C,<sat>,<c0_m>`
  lines inside an epoch record; `write()` at `:294`; the RTA-6 tail reader at `:402`
  already streams growing files to RT consumers). The fast record is an epoch record
  carrying ONLY satellite-clock lines plus a header marker distinguishing it from a
  full era (exact token per the legend grammar spec — extend the legend, keep it
  grammar-complete). σ per the M4 contract fields (`sigc`), from the WLS formal σ.
- Consumer semantics (SsrCorrectionStore): fast rows refresh **clock value and clock
  age only**. Bias/PB/iono entries and their ages are untouched (they belong to the
  slow era — this is precisely the 30 s staleness-horizon fix for clocks and ONLY
  clocks). Discontinuity counters (`disc`) never advance on fast rows.
- Replay parity: `NetworkProductReplayer::replay` (`NetworkProductIO.hpp:389`) must
  reproduce live-consume behavior for fast rows; a pre-FC-1 consumer reading a file
  WITH fast rows must fail loudly (legend version bump), not silently misread —
  follow the legend-schema versioning just landed.
- **No C1 emission. No clock smoothing. No interpolation.** (C1 card: NET-HARMFUL;
  ZOH between fast rows is the consumer's job, now at fast-row granularity.)

---

## 9. Non-goals / closed doors (do not re-open)

- No fast-tier estimation of iono/tropo/biases/ambiguities; no AR; no datum election.
- No C1/rate product (measured NET-HARMFUL, `project_network_clock_rate_c1`).
- No GF/TurboEdit at 30 s dev cadence (rate directive); the §2.2 screen is the fast
  tier's only detector, and it defers repair to the full pipeline.
- No threading in the fast tick before a measured budget breach.
- No change to full-pipeline behavior with the flag on: full epochs must be
  byte-identical whether fast ticks ran or not (the isolation contract, gated at every
  increment).
- Not a replacement for the perf ladder (P1 fusion / MKL / D1) — those still pay for
  the slow tier and are orthogonal.

---

## 10. Standing-invariants pre-flight (explicit)

1. Stiff/frozen quantities have wired, tested event responses — §4 (slip screen, ms
   jump absorption, floors, emission-gap gate, datum withhold, resume-cold). ON field
   run blocked until INC-2 unit gates pass.
2. Release-only for every timed/graded run; renamed exe copy; name-scoped zombie
   pre-kill.
3. Deferrals triaged with data — §6 INC-5 list, each field-inert for this card's
   verdict.
4. Run-end counters are the ON-path telemetry — §5 counters + teardown readout;
   "0 ticks_emitted" on an armed run = inert wiring, investigate before grading.
5. Grade datum-invariant quantities only — §6 INC-4 common-mode-removed SD grading;
   never raw wire clocks.
6. Default-OFF + structural byte-identity at every increment, same-build golden
   comparisons only.

---

## 11. Context pointers

- Measured basis: `docs/REPORT_timing305_allon_30s_10ep.md`; run dir
  `val_out/fable_timing305/`; bench config `_net305_timing_gps_allon_10ep.json`.
- Clock model + q values: `include/filter/NetworkRunTypes.hpp:351-390` (`ClockOsc`,
  `clockOscPredict/Update`), `GnssConfig.hpp:4708-4710`; carried at
  `NetworkRunContext.hpp:284`.
- R11 mechanism: `docs/SPEC_c1_clock_rate_validation.md` §5.3; gap knob
  `GnssConfig.hpp:4763`.
- Robust separation precedent: `filter/NetworkClockQc` (`medianPolish2Way`).
- Emit-gating discipline to copy for the harvest: `NetworkRunContext.hpp:333-342`.
- Decimation hook: `ApplicationNetwork.cpp:1053-1061`; loop head `:1006`.
- Product grammar + tail reader: `include/products/NetworkProductIO.hpp:34,294,389,402`;
  entry fields `include/products/NetworkProductEmitter.hpp:81-83`.
- Memory: `project_timing305_allon_baseline`, `project_network_clock_rate_c1`,
  `project_consumer_gap_integrity`, `feedback_slip_detectors_by_rate`,
  `feedback_reset_over_repair_biasless`.
