# SPEC — FC-1: Split-cadence clocks-only fast pass (network producer)

> ## ★ BUILT INC-0..4 (2026-07-30) — the decisive gate FAILED at 1.00× against a ≥ 5× bar. The card is a NO-GO, and it is NOT to be deleted.
>
> Structural, not a tuning miss: the reference the fast tier must reproduce is **~98 %
> producer frame churn**, which a physically-derived measurement does not contain.  Max
> removable error **2.3 % at 30 s → 0.7 % at 20 min** (§13.2); the gate asks 80 %.  Same
> verdict as C1 on a different mechanism; **FC-1's prerequisite is Card Q3**
> (`docs/SPEC_clock_temporal_anchor.md`), not the reverse.  **Keep it**: correct, cheap,
> default-OFF byte-identical.  **`network.fast_clock.shadow` is a KEEPER instrument** —
> the only way to answer *"how much of the emitted clock is real?"*; re-run after any Q3
> work (§14).  Verdict §14 · as-built §12 · deciding measurement §13 · seven spec defects
> §15 · reproduce §16.

*2026-07-28 (Fable). Implementer: Opus.  Absorbed 2026-08-15:
`docs/REPORT_fc1_split_cadence.md` (now `docs/attic/REPORT_fc1_split_cadence.md`) — its
§0/§1 → §12, §2 → §13, §3/§4 → §14, §5 → §15 (so the REPORT's §5.6 / §5.7 are **§15.6 /
§15.7** here), §6 → §16.*

*Companions:* `docs/SPEC_c1_clock_rate_validation.md` §5.3 (the R11 emission-continuity
lesson this card must not re-create); `docs/SPEC_clock_temporal_anchor.md` (Card Q3, the
prerequisite); `docs/SPEC_product_cbpin_contract.md` (product legend grammar).
⚠ **NOT IN TREE**: ~~`docs/REPORT_timing305_allon_30s_10ep.md`~~ (the §0 measured basis —
numbers survive in memory `project_timing305_allon_baseline`) and
~~`docs/AUDIT_consumer_product_gap.md`~~ (the staleness horizon).
⚠ Every `runs/…` and `val_out/…` path resolves under `D:\GnssProcessor_archive\2026-08-12\`.

---

## 0. Motivation (measured, not conjectured)

Jul-28 all-on 305-station GPS 30 s baseline: **27.72 s/epoch** (0.92× real time at 30 s) =
Card-4 carry 56.8 % / dual recon sweeps 25.1 % / assembly 11.4 % / reduce 4.9 % / **border
solve 0.1 % (41 ms)**.  *(Source report not in tree; run dir `val_out/fable_timing305/`,
bench config `_net305_timing_gps_allon_10ep.json`.)*  The optimization ladder (P1 fusion,
MKL Cholesky, D1) tops out near ~22 s/epoch and C2 proved the dense-K carry cost intrinsic
⇒ **a 1 s product cadence is unreachable by optimizing the per-epoch pipeline at 305
stations.**  What IS cheap is the estimation once the expensive structure is known: 41 ms.
IGS-RTS / commercial SSR exploit exactly that — orbits/biases slow, **clocks measured at
the served rate**.  C1 supplies the mandate: the emitted c0 is a random walk (0.157 m per
30 s), so ZOH is already MMSE *given* a 30 s solve; the only way to serve better 1 s clocks
is to **measure** them at 1 s.  Rates (C1 emission) were measured NET-HARMFUL and stay dead.

FC-1 adds a second tier: the full pipeline runs at its cadence unchanged; between full
solves a **clocks-only fast pass** measures per-satellite clock deltas against the last
emitted full solution and emits them, everything else frozen on ZOH.  One machinery, two
regimes — dev (30 s RINEX, 305 sta, full every N·30 s, tick at 30 s) and the RT target
(1 s stream, full every 5–30 s, tick at 1 s); **nothing in the fast pass knows the tick
interval**, and the dev regime validates the architecture against a same-data full-rate
reference (§7).

---

## 1. Architecture and driver hook

The seam is **T2e epoch decimation** (`src/core/app/ApplicationNetwork.cpp:1349`, anchor
refreshed 2026-08-15; the 2,359-line driver's loop head `for (const auto& se : synced)` is
at `:2350`): epochs decimated out by `network.epoch_decimation_n` hit a bare `continue`
*after* the start-offset / max-duration guards and the brdc_rts nav catch-up, *before*
`++n_loop_epochs`.  Those epochs have fully synced observations and are simply not
processed today.  **The seam CONCEPT is what matters; the line anchors drift.**

With `network.fast_clock.enable=true` a decimated-out epoch runs `networkFastClockTick_(…)`
instead of that `continue`; false (default) leaves it untouched — **byte-identical,
structural**.  Full epochs (`decim_counter % dn == 0`) are untouched either way; `shadow`
mode (§7.1) runs the tick on full epochs immediately before the full pipeline, for grading
only.  Fast ticks are NOT loop epochs: no `n_loop_epochs`, no D0/D0b brackets, no
assembly/solve/carry/QC/emit, and they mutate **nothing** outside their own carry struct
(§5) — the isolation contract that makes the card safe to land incrementally.

---

## 2. Estimation design

### 2.1 Formulation: per-band delta-from-fit, IF-combined, phase-only

Do NOT reconstruct absolute models or touch bias/ambiguity conventions.  The fast
observable is the **change of the fitted phase since the anchor epoch** t₀ (the last
*emitted* full solve; "emitted" is load-bearing, §4.4/§9).

> **⚠ Three defects corrected during build.  As SHIPPED: (a)** the lump is defined on
> **OBSERVED** phase, not fitted phase (§15.3 — the harvest below is unbuildable as
> written); **(b)** lump and tick phase use **a-priori-corrected** phase
> `Φ̃ = Φ + c·δt_ap`, so `ċ` is the RESIDUAL clock rate in the emitted frame (§15.6 — the
> af1 frame bug); **(c)** the emitted quantity is **`c0(t₀) + ċ·Δt + δc`**, not
> `c0(t₀) + δc` (§15.1).

At t₀, per used LOS (station r, sat s, band j), harvest `Φ̂ⱼ(t₀) = Φⱼ_obs(t₀) − vⱼ(t₀)` and
split it as

```
Φ̂ⱼ(t₀) = ρ(t₀) + Mⱼ(t₀)          ρ = geometric range (light-time, station ECEF fixed)
```

`Mⱼ` is the *lump*: everything modeled or estimated that is not instantaneous range —
clocks, tropo (mf·ZWD), iono (μⱼ·I), ambiguity, phase biases, windup, PCO/PCV projections,
tides, shapiro, relativity.  Also harvest the lump **rate** `Ṁⱼ` by finite difference
against the previous emitted full epoch (hence arming only after **two consecutive
emitted** full epochs).  At fast tick t (Δt = t − t₀):

```
yⱼ(t) = Φⱼ_obs(t) − [ ρ(t) + Mⱼ(t₀) + Ṁⱼ·Δt ]      then   y_IF = IF(y_j, y_k)
y_IF(r,s,t) = δτ_r(t) − δc^s(t) + ε
```

ρ(t) is recomputed exactly (SP3 interpolation + light-time + Earth rotation — the ONLY
nonlinear term per tick).  **δc^s** = satellite clock change since t₀ beyond the linear
trend already in Ṁ — the product.  **δτ_r** = receiver clock change (incl. ms jumps),
re-estimated fresh per tick (white), nuisance.  Iono change is **eliminated by IF**
(μⱼΔI cancels); frozen ambiguities/biases/windup/tides/tropo cancel in the delta or are
absorbed to harmless order by Ṁ·Δt (§3).  Every tick measures against the **same** t₀
anchor, so per-tick errors do not random-walk across the window.

Single-band arcs excluded by default (`allow_single_band=false`) — a lone band leaves μⱼΔI
in y; at 1 s RT the knob may admit them with iono-rate-inflated R, at 300 s dev cadence
they are useless.  Phase-only: code deltas add nothing at the cm level and import
code-bias freshness questions.

### 2.2 Robust fit and screen

~2 400 LOS, ~303 + 32 unknowns, bipartite:

1. **Median seed** (robust to any wild LOS / receiver ms jump): per-station median of y_IF
   → δτ_r seed; per-sat median of (y_IF − δτ_r) → δc^s seed — the `medianPolish2Way`
   row/column structure; reuse the helper.  ⚠ **Corrected: it is
   `include/filter/NetworkRobustStats.hpp:66`, NOT `filter/NetworkClockQc`** (which uses
   plain per-constellation `nth_element` medians and contains zero occurrences of the
   symbol).  Precedents `NetworkRaim.cpp:390`, `NetworkSolveGlue.cpp:1306`, and
   `NetworkFastClock.cpp` itself (`:347`, `:584`).  See §15.5.
2. **One WLS pass**, elevation-weighted R (existing stochastic constants × IF noise
   factor), station clocks Schur-eliminated (scalar diagonal blocks) → dense n_sat×n_sat
   SPD solve.  Serial, deterministic, Eigen single-threaded — microseconds; do NOT
   parallelize.
3. **Per-LOS screen** on the post-fit cell residual (both δτ_r and δc^s removed):
   `|res| > k·σ_pred`, `k = fast_clock.screen_k_sigma` (default 5).
   ⚠ **Corrected (§15.4): the σ_pred scale below is the WRONG quantity** —
   `clockOscPredict`'s q2 spread describes motion already removed as the median-polish
   column effect, and thresholds must come from the observable stream, never the
   stochastic model.  **As shipped, σ_pred is the robust spread of the interactions
   themselves, floored by the formal σ.**  (Original: σ_pred from the elevation R plus the
   ClockOsc-scaled prediction spread — `clockOscPredict`,
   `include/filter/NetworkRunTypes.hpp:701`, Allan-derived q2 = 1.5e-7 m²/s³,
   `GnssConfig.hpp` `networkClockModelQ2()` `:4376` / member `:5958`; per-1 s ~3 cm, per Δt
   scale √Δt.)  A screened LOS is excluded from THIS tick (re-tested next tick —
   reset-not-repair, same philosophy as D5), counted, and its arc flagged for the run-end
   readout.  A cell breach on one station only = slip suspect; a whole-row (sat) coherent
   move is NOT screened — that is δc^s.
4. **Estimability floors**: sat needs `min_stations_per_sat` (default 3) unscreened
   dual-band LOS *with live t₀ arcs*; station needs `min_sats_per_station` (default 2).
   Sats below the floor emit nothing new (consumer keeps the last value; age grows — never
   fabricate).

### 2.3 Datum / frame contract (do not deviate)

- Fast datum = the SAME `network.clock_datum_station` pin as the slow solve:
  `δτ_datum := 0`.  **No re-election inside a fast window, ever.**  Datum station
  absent/screened at a tick ⇒ tick **withheld entirely** (counter + log once per window),
  not re-datumed.
- **As SHIPPED the emitted fast clock is `c0^s(t) = c0^s_emitted(t₀) + ċ^s·Δt + δc^s(t)`.**
  ⚠ The delta-on-emitted-only form `c0^s_emitted(t₀) + δc^s(t)` that v1 of this subsection
  stated is **FALSIFIED design intent** — it drops the clock's linear drift, the dominant
  motion over 30–300 s, and would publish something worse than ZOH (§15.1; source
  `src/core/filter/NetworkFastClock.cpp:325` "the emitted value must be anchor + cdot*dt +
  delta", formula `:739`, with `:743` naming `c0_emitted(t0) + delta_c` as what would
  "publish a systematically de-trended clock").  What the frame argument still buys: as a
  delta on the *emitted* value it lives in the emitted frame by construction, and any
  per-era frame constant (re-base, datum, pivot, IF-pin realization) cancels in the delta
  **because re-bases/re-elections/re-levels only ever happen inside the full pipeline**,
  i.e. at era boundaries.  The fast pass must not import, recompute or "correct" any frame
  quantity; if the next full solve re-bases, its emitted record re-anchors everything and
  the fast window ends.
- Anchoring on *emitted* t₀ ⇒ a withheld full epoch (observability gate, A4 ceiling, SH
  gate) does NOT advance the fast anchor: the window keeps running against the last emitted
  era — the frame the consumer holds — until `emit_max_gap_s` expires (§4.4).

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
(`exclude_eclipsing_satellites`).  At the RT tick the frozen-state error is sub-mm — fast
clock quality is limited by phase noise and the frozen-arc assumption, not staleness.  At
the dev cadence the low-elevation lump curvature is visible but graded (§7), and its
common-mode part cancels in SD grading.

---

## 4. Event responses — standing invariant #1 pre-flight

Every stiff/frozen quantity has a wired, tested response BEFORE any ON field run:

1. **Cycle slip inside the fast window** → the §2.2 screen excludes the LOS that tick (a
   slipped arc's delta jumps by λ·ΔN ≫ k·σ).  No repair/refloat in the fast tier — the next
   full solve owns repair.  Counter `fast_screen_slips`; test: inject a λ jump into a cached
   lump → LOS screened, sat still estimated from the rest, counter fires.
2. **Receiver ms clock jump** → absorbed exactly by the fresh per-tick δτ_r (all of a
   station's LOS move together).  Test: +1 ms on one station → δτ_r ≈ 299 792.458 m, zero
   LOS screened, sat clocks unmoved.
3. **Sat rises after t₀** → no cached arc → not fast-estimable → not emitted (consumer ZOH +
   age).  **Sat sets / arc dies** → below floor → stops being refreshed.  Never
   extrapolated.  Counter `fast_sat_below_floor`.
4. **Emission-continuity gate — the R11 lesson** (`SPEC_c1_clock_rate_validation.md` §5.3:
   R11 gated on *solve* continuity while the consumer experiences *emission* continuity;
   gap knob `networkClockModelMaxGapS()` `GnssConfig.hpp:4409` / member `:6042`).  The fast
   gate keys on **emitted** times only: `t − t_last_emitted_full > fast_clock.emit_max_gap_s`
   (default 60 s) ⇒ fast emission STOPS (ticks may still run in shadow).  A withheld full
   era longer than the gap silences the fast tier rather than letting it ride a stale frame
   — degrade-by-silence, never degrade-by-fabrication.  Counter `fast_gap_withheld_ticks`;
   test: force a withhold streak → fast rows stop at the bound.
5. **Datum station lost at a tick** → tick withheld (§2.3).  Counter
   `fast_datum_withheld_ticks`.
6. **Warm resume** (RTB-7a): `FastClockCarry` is deliberately NOT in the snapshot — a
   resumed run arms only after two fresh emitted full epochs.  Zero interaction with
   `NetworkStateSnapshot`.

---

## 5. Where the code goes (anti-regrowth decision tree, CLAUDE.md)

*(All BUILT; this is the as-built layout.)*

- **Seam** `include/filter/NetworkFastClock.{hpp,cpp}` (manual `CMakeLists.txt` entry, full
  Doxygen): free functions over `(NetworkRunContext& ctx, const GnssConfig& config_, log,
  NetworkRunTraces&, …)` with the alias preamble, owning NO state.
  `networkFastClockHarvest` sits at the emit-success path (`NetworkEmitStore`) behind the
  **"kept epoch" gate** — every whole-epoch withhold `continue`s before the emit site,
  which is exactly where the harvest must sit — and caches per-LOS
  `{bands, Φ̃ⱼ, ρ(t₀), Mⱼ, Ṁⱼ}` + `c0^s_emitted(t₀)` + `t₀`, piggybacking an existing
  per-station post-solve sweep (do NOT add a new `reconstructStation` sweep).
  `networkFastClockTick` is the decimation-hook body (§1): geometry recompute, deltas,
  median seed, WLS, screen, trace row, emission handoff.
- **Carried state** → `struct FastClockCarry` in `NetworkCarryState`
  (`NetworkRunContext.hpp:827`, next to `clock_osc` `:813`): harvest maps, arm state
  (2-emitted-epoch counter), `t_last_emitted_full`, per-arc screen flags.  No bare
  function-locals in the driver (INC-12 lesson).
- **Counters** → `NetworkServiceCounters` (`ticks_driven, ticks_emitted, los_used,
  los_screened, fast_screen_slips, fast_sat_below_floor, fast_datum_withheld_ticks,
  fast_gap_withheld_ticks` — 14 total), read out in `filter/NetworkTeardown`.
- **Trace** → `network_fast_clock.csv` owned by `logging/NetworkRunTraces` (per tick: tow,
  Δt, n_los, n_screened, n_sat_est, per-sat δc^s + σ on a stride knob).
- **Config** → `network.fast_clock.{enable, shadow, emit, min_stations_per_sat,
  min_sats_per_station, screen_k_sigma, allow_single_band, emit_max_gap_s}` — POD sub-struct
  in `GnssConfigTypes.hpp`, parsed in `loadNetworkSection_`
  (`GnssConfigLoadNetwork.cpp:1039-1066`) via `jval`.  **All default OFF / conservative**
  (false / false / false / 3 / 2 / 5.0 / false / 60.0); OFF ⇒ byte-identical.
- **Emission** → §8, extending `NetworkSolPrep` / `NetworkEmitStore` + `NetworkProductIO`.
- **Perf timing** → its own `[TIMING]` accumulator (ticks are outside D0); run-end line
  `fast ticks: N, wall X ms, Y ms/tick` + max tick.

Driver delta: ~6 lines at the decimation hook + the harvest call at the emit site.
Everything else lives in the seam.  **Keep it that way.**

---

## 6. Increments

*(INC-0..4 all EXECUTED — results §12.1.  Gate definitions kept because they are the
pre-registration §12–§14 are read against.)*

**INC-0 — plumbing.** Config block, `FastClockCarry`, seam skeleton (no-op), counters,
trace registration, CMake.  *Gate:* default-OFF run of
`_net305_timing_gps_allon_10ep.json` byte-identical (same build) on products.txt, all CSVs,
run-end counters; suite green.

**INC-1 — harvest.** Fill the carry at emit-success; arm after 2 consecutive emitted
epochs.  *Gate:* byte-identity everywhere (harvest reads, never writes) + unit tests for
arm/reset on withhold streaks and resume-cold.

**INC-2 — tick estimation, log-only.** Full §2 fit at decimated epochs + shadow mode; trace
rows; NO emission.  *Gates:* (a) shadow δc^s vs the same epoch's full-solve emitted clock
delta — RMS ≤ 2× the full solve's formal σ_clock, zero screened-LOS storms; (b) unit tests
for the §4 injections; (c) determinism — two identical runs → identical
`network_fast_clock.csv`.

**INC-3 — emission + store.** §8 record + `SsrCorrectionStore` push + replay/tail parity +
the R11-keyed gap gate.  *Gates:* `fast_clock.emit=false` ⇒ INC-2 behavior byte-identical;
ON ⇒ a products.txt with fast rows replays into a store whose slow-era state is
byte-identical to the fast-rows-stripped file (consumer clock values differ ONLY at fast
epochs; biases/PB/iono ages untouched).

**INC-4 — cadence-split A/B (THE DECISIVE GATE).**  Pre-registered conditions as written:
same build, same data — **305-sta / GPS / 30 s, ≥ 2 h after a 1 h warm-up**; staged
cadence, 10-ep smoke first.  REF `epoch_decimation_n=1` (full solve every 30 s, today's
mode); SPLIT **n=10 + `fast_clock` on** (full at 300 s, ticks at 30 s).  Grade emitted
satellite clocks at tick epochs, SPLIT vs REF, **common-mode removed per epoch**
(invariant #5): **`SD-RMS(fast vs REF) ≤ (1/5) · SD-RMS(ZOH-of-300s-era vs REF)`** — beat
the C1-proven ZOH floor by ≥ 5× (ZOH error at 300 s ≈ 0.157·√10 ≈ 0.5 m; target ≪ 10 cm);
no per-sat systematic vs elevation/screening; screen rate < 1 % of LOS in quiet data;
REF-vs-REF noise floor (±5 %) respected, run on a quiet box.
⚠ **Executed at a DIFFERENT geometry and decimation than this pre-registration** — 50 sta /
GPS+Gal / 3 h / `n=2` (§12.3, §14.1).  §13.2's churn ceiling is what makes the substitution
immaterial.

**INC-5 — deferred (triaged field-inert, invariant #3):** RT transport (RTCM/SSR encode),
1 s high-rate data pull, consumer positioning A/B with fast clocks, single-band admission
at 1 s.  None blocks the verdict — INC-4 validates the architecture on existing data and
the default-OFF flag keeps every shipped path untouched.

---

## 7. Validation instruments

**7.1 Shadow mode** (`fast_clock.shadow=true`): run the tick on FULL epochs too,
immediately before the full pipeline, logging fast-vs-full deltas.  Zero downstream effect
(gate: byte-identity of everything but `network_fast_clock.csv`); per-epoch truth grading
on ANY run for free, and a permanent regression instrument (the M2 LS-VCE class:
instrument, never feedback).

**7.2 ZOH benchmark**: inside INC-4's grader (`scripts/fc1_grade.py`) — per tick epoch,
ZOH = last SPLIT full-era emitted clock.  Report honestly if fast ≤ ZOH anywhere (a NO-GO
finding, not a tuning knob).  *(It was — §14.1.)*

**7.3 Perf**: per-tick wall ≤ **50 ms @ 305 sta** (1 s RT budget with 20× headroom;
expected ~5–10 ms: ~2 400 light-time recomputes + a 32×32 solve), measured in the run-end
`[TIMING]` line.  If a tick exceeds budget, profile before threading — the §2.2 solve must
stay serial-deterministic.

---

## 8. Emission / product contract (INC-3)

> **⚠ Two build corrections.  (1) The emitted value is `c0^s(t₀) + ċ^s·Δt + δc^s`** (§15.1)
> — the bare `c0(t₀) + δc` below drops the linear drift and publishes worse than ZOH.
> **(2) "A pre-FC-1 consumer must fail loudly" is UNACHIEVABLE** (§15.2): the legend banner
> is a `#` comment no C++ reader inspects.  **As shipped, marker and payload use private
> `F` / `K` tags**, so an old reader drops the whole record and degrades to its pre-card
> behaviour instead of re-attaching a familiar `C` line to the previous full epoch.  A test
> asserts a fast-stripped file reads identically to a pre-card one.

- Grammar: fast epochs append a compact record to the SAME products file consumed by
  `NetworkProductIO` (`include/products/NetworkProductIO.hpp:34` — the
  `C,<sat>,<clock_c0_m>` line, still current), carrying ONLY satellite-clock lines plus a
  header marker distinguishing it from a full era.  σ per the M4 contract fields (`sigc`),
  from the WLS formal σ.  As shipped: `ProductEpochRecord::is_fast` / `fast_anchor` /
  `fast_clocks` (`include/products/NetworkProductIO.hpp:288-295`) and `pushFastClock`.
- Consumer semantics (`SsrCorrectionStore`): fast rows refresh **clock value and clock age
  only**.  Bias/PB/iono entries and their ages are untouched (the 30 s staleness-horizon fix
  for clocks and ONLY clocks).  Discontinuity counters (`disc`) never advance on fast rows.
- Replay parity: `NetworkProductReader::replay` (`NetworkProductIO.hpp:493`, impl
  `NetworkProductIO.cpp:736`, fast rows `:755-757`) must reproduce live-consume behavior for
  fast rows.  *(The "fail loudly via a legend version bump" requirement was replaced by the
  private-tag degradation above — §15.2.  The pre-decomposition name/anchor
  `NetworkProductReplayer::replay` / `NetworkProductIO.hpp:389` is stale in both respects —
  `:389` is now `writeContract`.)*
- **No C1 emission. No clock smoothing. No interpolation.** (C1: NET-HARMFUL; ZOH between
  fast rows is the consumer's job, now at fast-row granularity.)

---

## 9. Non-goals / closed doors (do not re-open)

- No fast-tier estimation of iono/tropo/biases/ambiguities; no AR; no datum election.
- No C1/rate product (measured NET-HARMFUL, `project_network_clock_rate_c1`).
- No GF/TurboEdit at 30 s dev cadence (rate directive); the §2.2 screen is the fast tier's
  only detector and defers repair to the full pipeline.
- No threading in the fast tick before a measured budget breach.
- No change to full-pipeline behavior with the flag on: full epochs byte-identical whether
  ticks ran or not (gated at every increment).
- Not a replacement for the perf ladder (P1 fusion / MKL / D1) — orthogonal.

---

## 10. Standing-invariants pre-flight (explicit)

1. **#1** stiff/frozen quantities have wired, tested event responses — §4; ON field run
   blocked until the INC-2 unit gates pass.
2. **#2** Release-only for every timed/graded run; renamed exe copy; name-scoped zombie
   pre-kill.
3. **#3** deferrals triaged with data — §6's INC-5 list, each field-inert for this verdict.
4. **#4** run-end counters are the ON-path telemetry — §5 counters + teardown readout;
   "0 `ticks_emitted`" on an armed run = inert wiring, investigate before grading.
5. **#5** grade datum-invariant quantities only — INC-4's common-mode-removed SD grading;
   never raw wire clocks.
6. **#6** default-OFF + structural byte-identity at every increment, same-build golden
   comparisons only.  *(Measured — §12.2.)*

---

## 11. Context pointers

**All line anchors re-grepped 2026-08-15.  Verify by SYMBOL — they drift.**

- Measured basis: source report NOT IN TREE (header); run dir `val_out/fable_timing305/`;
  bench config `_net305_timing_gps_allon_10ep.json`; memory
  `project_timing305_allon_baseline`.
- Clock model + q: `struct ClockOsc` `include/filter/NetworkRunTypes.hpp:655`,
  `clockOscPredict` `:701`; `networkClockModelQ2()` `GnssConfig.hpp:4376` (member `:5958`);
  carried as `clock_osc` at `NetworkRunContext.hpp:813`.
- FC-1's own state: `FastClockCarry fast_clock` `NetworkRunContext.hpp:827`; driver knobs
  `ApplicationNetwork.cpp:321-327`; tick call `:1358` (decimated) / `:1395` (shadow);
  harvest `:2116`.
- R11 mechanism: `docs/SPEC_c1_clock_rate_validation.md` §5.3; gap knob
  `networkClockModelMaxGapS()` `GnssConfig.hpp:4409` (member `:6042`).
- Robust separation precedent: **`include/filter/NetworkRobustStats.hpp:66`**
  (`medianPolish2Way`) — NOT `NetworkClockQc`; §15.5.
- Emit-gating discipline copied for the harvest: the "kept epochs only" rule documented
  through `NetworkRunContext.hpp` (`:682`, `:857`, `:876`).
- Decimation hook `ApplicationNetwork.cpp:1349`; loop head `:2350`.
- Product grammar + fast records: `include/products/NetworkProductIO.hpp:284-295` (`is_fast`
  / `fast_anchor` / `fast_clocks`), `:360`, `:377`; emitter entry fields
  `include/products/NetworkProductEmitter.hpp:81-83` (`has_clock` `:81`, `clock_c0_m` `:82`,
  `clock_c1_m_s` `:83`).
- **Anchor-drift ledger** (pre-decomposition anchors as they appear in older notes and
  commits → current, verified 2026-08-15): `ApplicationNetwork.cpp:1053-1061` → `:1349`;
  `ApplicationNetwork.cpp:1006` → `:2350`; `NetworkRunTypes.hpp:351-390` → `:655`/`:701`;
  `NetworkRunTypes.hpp:365` → `:701`; `GnssConfig.hpp:4708-4710` and `GnssConfig.hpp:4709` →
  `:4376`/`:5958`; `GnssConfig.hpp:4763` → `:4409`/`:6042`; `NetworkRunContext.hpp:284` →
  `:813`/`:827`; `NetworkRunContext.hpp:333-342` → `:682`/`:857`/`:876`;
  `NetworkRaim.cpp:258` → `:390`; `NetworkSolveGlue.cpp:930` → `:1306`;
  `NetworkProductIO.hpp:389` → `:493`.  The old list
  `include/products/NetworkProductIO.hpp:34,294,389,402` (grammar / write / replay / tail
  reader) resolves today as `:34` unchanged, `294` inside the `:284-295` fast-record block,
  `389` `writeContract`, `402` the per-epoch flush.
- Memory: `project_timing305_allon_baseline`, `project_network_clock_rate_c1`,
  `project_fast_clock_card`, `project_consumer_gap_integrity`,
  `feedback_slip_detectors_by_rate`, `feedback_reset_over_repair_biasless`.

---

## 12. As built (2026-07-30) — INC-0..4

### 12.1 Per-increment gate table

| INC | Content | Gate | Result |
|---|---|---|---|
| 0 | config block, `FastClockCarry`, seam, 14 counters, `network_fast_clock.csv`, `SB_FASTCLOCK`, driver hooks | default-OFF byte-identity | **PASS** |
| 1 | anchor harvest at emit-success, arming streak, lump + lump-rate cache | byte-identity + unit tests | **PASS** |
| 2 | tick estimation: ρ recompute, IF delta, median-polish seed, Schur WLS, screen, floors | σ̂₀, screen rate, shadow grade | **PASS** (mechanics) |
| 3 | `F` / `K` wire records, `pushFastClock`, replay parity | round-trip + consumer-contract tests | **PASS** |
| 4 | cadence-split A/B | fast ≤ ZOH/5 | **FAIL — 1.00×** |

### 12.2 The isolation contract, MEASURED

Tier armed in **shadow** (both hooks firing every epoch): `products.txt` and all **23
estimation CSVs are byte-identical** to the tier-off arm.  Only the wall-clock `G`
generation stamp and the two timing CSVs differ.

### 12.3 Runtime health (SPLIT arm, 50 sta, 3 h, 180 ticks)

- **179 / 180 ticks estimated and emitted**, 0 datum-withheld, 0 gap-withheld.
- **131 920 LOS used / 1 095 screened = 0.82 %** (bar: < 1 % on quiet data).
- **σ̂₀ 0.85–0.98**; per-satellite formal σ **3.8 mm**.
- Tick wall **14.2 ms mean / 38.2 ms max at 50 stations** — inside the §7.3 budget.

**⚠ Perf extrapolation and its named UNBUILT fix.**  By station count, **305 stations lands
at ~85 ms mean / ~230 ms max — OVER the 50 ms budget.**  The cost is the per-LOS
`getSatellitePV` call; the documented fix (evaluate PV once per satellite and extrapolate
to each station's emission time by velocity, ~0.1 mm error) **was not needed for the
verdict and was not built.**

---

## 13. The measurement that decides it

Shadow mode computes the fast tick on FULL epochs and discards it, so every epoch yields
both a phase-derived clock change and the producer's own.  50 stations, GPS+Galileo, 3 h,
common mode removed per epoch (invariant #5).

### 13.1 The producer's clock change is not the satellite's clock change

| hour | producer Δc0 | phase-measured Δc | correlation |
|---|---|---|---|
| 0 | 79.4 cm | 1.74 cm | −0.009 |
| 1 | 67.8 cm | 1.73 cm | −0.001 |
| 2 | 83.1 cm | 1.93 cm | +0.018 |

Flat across three hours ⇒ **not a convergence transient**; correlation zero at every hour ⇒
the two quantities are **unrelated**, not merely different in scale.  Three explanations
RULED OUT: **cold-start amplification** (the ratio does not improve with time);
**discontinuity bookkeeping** (5 disc advances in 530 consecutive pairs); **de-trending**
(comparing against the producer's *second difference*, matching the fast observable's own
de-trending, leaves the disagreement unchanged).

### 13.2 The ceiling on any fast tier, at every horizon

| ZOH horizon | ZOH error | √Δt law | physical change | **headroom** |
|---|---|---|---|---|
| 30 s | 76.9 cm | 76.9 cm | 1.80 cm | 2.3 % |
| 60 s | 112.1 cm | 108.8 cm | 2.06 cm | 1.8 % |
| 120 s | 161.6 cm | 153.8 cm | 1.95 cm | 1.2 % |
| 300 s | 243.4 cm | 243.2 cm | 2.05 cm | 0.8 % |
| 600 s | 317.0 cm | 343.9 cm | 2.28 cm | 0.7 % |
| 1200 s | 431.2 cm | 486.4 cm | 2.81 cm | 0.7 % |

1. **The emitted clock follows the √Δt random-walk law almost exactly** — independent
   confirmation of C1's measured `dt^0.581` signature, on different data.
2. **The "physical change" column is FLAT at ~2 cm and does not accumulate.**  Over twenty
   minutes the emitted product random-walks 4.3 m.

So the *maximum* error a perfect fast tier could remove is **2.3 % at 30 s, falling to
0.7 % at 20 minutes**, against a gate asking 80 %.  No cadence, threshold or estimator
improvement reaches it, because the missing 97.7 % is not clock.

> **⚠ SUPERSEDED INTERPRETATION (2026-08-10, `docs/SPEC_clock_temporal_anchor.md` §7).**
> The ~1.74–2.28 cm "physical change" figures are the **SF(30 s) MEASUREMENT FLOOR, not the
> walk** — the column was already flat 30 s → 1200 s, i.e. non-accumulating, which is the
> signature.  The accumulating physics is **q_phys ≈ 2.9e-8 m²/s = 0.94 mm per 30 s**
> (fractional stability ~1e-13), 20× below even the smaller floor.  **Consequence: the FC-1
> correlation gate of §14 is RETIRED, not merely unmet** — 0.9 mm true motion against a
> 2.7 mm measurement floor makes the correlation structurally unliftable at any reachable q.
> The FC-1 numbers stay valid **AS FLOORS**; quote them with this correction attached.

---

## 14. Verdict, consequences, and what to keep

### 14.1 The verdict

| INC-4 (50 sta, GPS+Gal, 3 h, REF `n=1` vs SPLIT `n=2`) | measured |
|---|---|
| SD-RMS(fast vs REF) | **327.7 cm** |
| SD-RMS(ZOH vs REF) — what a consumer gets today | **326.4 cm** |
| improvement factor | **1.00×** (gate requires ≥ 5×) |

> **⚠ That row is SATURATED, and the emit path also had a frame bug — both fixed the same
> day (2026-07-30); the verdict is unchanged.**  The two arms' full solves realize
> independent frame random walks **~318 cm SD-RMS** apart, so both errors floor at that
> divergence (√(318² + 68.5²) ≈ 325) and **the gate could never have shown more than ~1.03×
> at ANY churn level** (§15.7).  De-saturated, the original build graded **0.97× —
> measurably WORSE than ZOH** — because the emitted K double-counted broadcast af1 drift +
> relativistic rate (§15.6).  Post-fix it grades **1.00×: fast 68.494 vs ZOH 68.494 cm.**

The fast tier is **not inaccurate**: it measures the right quantity to a formal σ of 3.8 mm
at σ̂₀ ≈ 0.9.  Its ~0.5 cm of true cross-satellite signal per 30 s tick is simply invisible
against the 68.5 cm churn step — §13.1's finding on the wire.

### 14.2 Consequences beyond this card

- **The producer's clock random walk is its own error budget, now measured against a
  physical reference for the first time.**  C1 established the walk and *inferred* frame
  churn from its magnitude relative to Allan deviation; this measures the physical clock
  directly and finds the walk **~98 % non-physical**.
- **Any consumer-side clock coasting scheme inherits the same ceiling** — high-rate clock
  interpolation, SSR saw-tooth correction, C1 rate extrapolation all predict a walk that is
  producer-internal.
- **The lever that pays is Q3 / producer temporal stiffness**, not faster measurement:
  reducing the 0.77 m per-30 s churn is worth **up to 100× more** than measuring the 1.8 cm
  the fast tier recovers.

### 14.3 What to keep — `network.fast_clock.shadow`

A **standing instrument**, same *"instrument, never feedback"* class as the M2 LS-VCE
carry-variance monitor: **14 ms/epoch at 50 stations**, byte-identical to off, σ̂₀ ≈ 0.9.
**Re-run after any Q3 / temporal-anchor work**; §13.1's table is the direct score, and
INC-4 becomes passable exactly when the ratio closes.  *(Q3 §7 later RETIRED the
CORRELATION half of that score — §13.2 banner — replacing it with Q3's own gate set:
dev-census ex-pivot floor/ceiling, σ̂₀_code flat, consumer unchanged-or-better,
guard-population health, G4 event response.  What survives unchanged: re-run the shadow
instrument, and the SD-RMS body statistic.)*

---

## 15. Spec defects found while building (the seven, verbatim)

1. **§2.1 vs §8 contradict each other on the emitted quantity.** `Ṁ` absorbs the clock's
   linear drift so `δc` is the change *beyond* the trend (§2.1); §8 nevertheless emits
   `c0(t₀) + δc`, dropping the dominant motion over 30–300 s.  **Fixed** by recovering `ċ^s`
   from the same lump rates via the same bipartite separation (`Ṁ = τ̇_r − ċ`, so the
   satellite column effect *is* `−ċ` once the datum row is pinned) and emitting
   **`c0(t₀) + ċ·Δt + δc`**.
2. **§8's "a pre-FC-1 consumer must fail loudly" is not achievable.** The banner is a `#`
   comment no C++ reader has ever inspected; a version bump is invisible to every consumer
   already built.  **Marker and payload therefore use private tags (`F` / `K`)**, so an old
   reader drops the whole record and degrades to its pre-card behaviour.  A test asserts a
   fast-stripped file reads identically to a pre-card one.
3. **§2.1's fitted-phase harvest is unbuildable as written.** It needs the post-fit residual,
   but §5 forbids a new `reconstructStation` sweep and all three existing sweeps run *after*
   the emit site behind trace/RAIM knobs.  **Defining the lump on OBSERVED phase makes the
   observable exact TDCP**, where the ambiguity and both hardware biases cancel *identically*
   on a live arc; cost = one epoch of phase noise as a per-LOS constant (~2 mm, ~0.6 mm on δc
   after averaging).
4. **§2.2's screen scale is the wrong quantity** — it sizes the per-LOS screen with
   `clockOscPredict`'s q2 spread, but that motion is already removed as the median-polish
   column effect, and the standing directive is that **thresholds come from the observable
   stream, never the stochastic model**.  **Implemented as the robust spread of the
   interactions themselves, floored by the formal σ.**
5. **§5 / §11 point at the WRONG FILE for `medianPolish2Way`** — not `NetworkClockQc` (which
   uses plain per-constellation `nth_element` medians) but
   **`include/filter/NetworkRobustStats.hpp:66`**; precedents `NetworkRaim.cpp` and
   `NetworkSolveGlue.cpp`.  *(Re-verified 2026-08-15: `NetworkClockQc.{cpp,hpp}` contain ZERO
   occurrences of the symbol; the precedent anchors are now `NetworkRaim.cpp:390` and
   `NetworkSolveGlue.cpp:1306`, and `NetworkFastClock.cpp` uses it at `:347` and `:584`.)*
6. **Implementation defect (post-report audit, 2026-07-30; FIXED same day): the emitted
   `ċ·Δt` term was in the wrong frame.**  The lump was raw `Φ·λ − ρ` with no a-priori
   satellite clock subtracted (`fastClockGeometricRange` evaluated the clock for light time
   and discarded it), so the median-polished `ċ` was the **FULL physical clock rate** —
   per-sat correlation **+0.93 with broadcast `c·af1`** (G22 agreed to 0.02 mm/s) and only
   **+0.07** with the emitted c0's own slope, plus the periodic relativistic rate on
   eccentric satellites (E14/E18, ±10–15 mm/s).  The consumer's a-priori already advances
   both, so every K **double-counted** them: **15.3 cm RMS spurious drift per 30 s tick, up
   to ~62 cm on E19** (af1 = 20.7 mm/s) against a ~1.8 cm physical signal — and the
   de-saturated grade of the original runs shows fast = √(ZOH² + 15.3²): **70.290 measured
   vs 70.2 predicted**, i.e. WORSE than ZOH (**0.97×**) with E18/E19 in the worst-satellite
   list.  **Fix:** the lump (and the tick's phase) is built on model-consistent corrected
   phase **`Φ̃ = Φ + c·δt_ap`** — the same polynomial + periodic relativistic a-priori the
   full model applies, gated by the same knob — so `ċ` is the RESIDUAL clock rate in the
   emitted frame (`NetworkFastClock.cpp` `@ref fc1_apriori`).  **Post-fix verification**
   (same-build 3 h re-runs, `*_v2` configs): corr(ċ, c·af1) **+0.93 → +0.12**; embedded
   drift **15.3 → 1.48 cm** per 30 s tick; E19's ċ −0.57 mm/s vs af1's +20.65; **K − anchor
   now carries 0.545 cm of cross-satellite structure** under a 9.5 cm per-epoch common mode
   (datum receiver, cancels in SD grading).  The shadow instrument was and is unaffected —
   §13.1/§13.2 reproduce to 0.01 cm (**76.981 → 76.971** SD-RMS, formal σ **0.383 cm**
   unchanged) — and the OFF/shadow byte-identity gate re-passed.  **One prediction from that
   audit did NOT materialize:** `ċ` does **not** correlate with the producer's own 3 h c0
   slope (**−0.04**), and cannot — that slope's RMS (**1.386 mm/s**) is ~97 % the random
   walk's realized endpoint drift (0.77 m·√360 / 10800 s ≈ **1.35 mm/s**), i.e. §13.1's
   churn finding at slope scale.
7. **Grader defect (post-report audit, 2026-07-30; FIXED same day): the INC-4 metric was
   SATURATED BY CONSTRUCTION.**  SPLIT and REF full solves differ by **~318 cm SD-RMS** at
   common epochs (independent producer frame walks), so both errors floored at the inter-arm
   divergence — √(318² + 68.5²) ≈ 325 ≈ the 326–328 cm measured — and **the maximum
   measurable improvement factor was ~1.03× against a 5× gate, at ANY churn level.**
   `grade_split` now estimates the per-satellite arm-frame offset at the bracketing common
   full epoch (SPLIT full − REF full, held across the era), subtracts it from both errors —
   turning the ZOH error into REF's own step across the era and the fast error into the
   tick's miss of that step — and **PRINTS the removed divergence** (`arm-frame divergence:
   SD-RMS 317.989 cm REMOVED`), so saturation can never hide again.  Any future INC-4 re-run
   (post-Q3) inherits the honest metric.

---

## 16. Reproduce

```bash
python scripts/fc1_grade.py shadow --products runs/fc1_shadow_3h/products.txt \
    --trace runs/fc1_shadow_3h/logs/network_fast_clock.csv
python scripts/fc1_grade.py split --products runs/fc1_split_n2_3h/products.txt \
    --ref-products runs/fc1_shadow_3h/products.txt \
    --trace runs/fc1_split_n2_3h/logs/network_fast_clock.csv
```

Configs: `_fc1_byteid_off.json` / `_fc1_byteid_shadow.json` (isolation gate),
`_fc1_shadow_3h.json` (REF + shadow grade), `_fc1_split_n2_3h.json` (SPLIT arm).

**Provenance rule — do not mix the two families.**  `runs/fc1_*` are the **ORIGINAL
(pre-§15.6-fix) build's output**, kept as provenance; `grade_split` on them now reports the
de-saturated **0.97×**.  The post-fix arms live in `runs/fc1_*_v2` with configs
`_fc1_byteid_off_v2.json` / `_fc1_byteid_shadow_v2.json` / `_fc1_shadow_3h_v2.json` /
`_fc1_split_n2_3h_v2.json` (same posture, **fresh output dirs** — append-mode service CSVs
make reusing a run dir unsafe); grading those reports the honest post-fix **1.00×**.  Both
families resolve under `D:\GnssProcessor_archive\2026-08-12\runs\`.
