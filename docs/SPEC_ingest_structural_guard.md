# SPEC — structural-ingest guard (mass-dropout census + single-frequency demotion + arc re-vet)

**Status**: ⛔ **RETRACTED 2026-08-01 before implementation — DO NOT BUILD.** The
motivating "mass-dropout" observation was an artifact of the investigating session's
ad-hoc RINEX census: it string-matched space-padded epoch seconds (`19 26  0`) and
silently missed every zero-padding writer (`19 26 00` — e.g. POTS, full 60-sat epoch
present) at seconds=00 epochs only. The producer's own ingest (`n_pf_rows` 3332→3328
across the break) shows NO row loss — the solve had full data at both engagement
epochs. The era-1/era-2 root cause is REOPENED; see `project_ft63_fast_tier`. Parts of
this spec (per-arc re-vet INC-D, census-as-observability) may be revived by a future
card once a true root cause motivates them — with the census cross-checked against
producer counters, not hand-rolled parsing.

~~SPEC'd 2026-08-01 (FT63 session), implementation OPEN~~ — superseded by the
retraction above.
**Memory context**: `project_ft63_fast_tier` (the resolved root cause + all five
falsified arms + replay mechanics), `project_adaptive_iono_q_network` and
`docs/SPEC_eq34_reattach_hygiene.md` §8 (the two sibling cards whose patterns this card
copies: ctx-side maps, not-in-fingerprint rationale, telemetry shape).
**Scope discipline**: default-OFF knobs under a nested `network.ingest_guard` block
(the `cycle_slip`/`mw_wl` nesting precedent); OFF structurally byte-identical; no
loader-default flips. **Non-goals** (separate cards): real EPB/scintillation response
(deweight/adaptive-q lineage), SH-divergence RESET action, gate-release relevel, RAIM
T2 decouple. External attribution of the dropouts (flex-power schedules etc.) is
diagnostics-only commentary — the guard must work from internal evidence alone
(no-external-data directive).

---

## 1. Problem — the resolved root cause (do NOT re-litigate; five arms already falsified)

DOY-129 FT63 24h (`runs/ft63_24h_allon`): both SH-gate eras are **one-epoch GLOBAL
mass-dropout events in the GPS observations themselves** (signal-in-space
semi-codeless class). RINEX census over the 63 stations (G rows / rows-with-L2W;
stations-to-zero-L2W):

| epoch | before → during → after | L2W | zero-L2W stations |
|---|---|---|---|
| era-1 07:32:00 (TOW 286320) | 655 → **364** → 646 | 616 → 345 → 616 | **26** |
| era-2 19:26:00 (TOW 329160) | 690 → **404** → 689 | 652 → 377 → 649 | **25+** |

Globally distributed (Alaska/Brazil/Japan/Svalbard/Antarctica/Euro cluster) ⇒ common
cause upstream of every receiver. Mechanism into the estimator: for one epoch ~40-44%
of rows vanish and dozens of stations become **L1-only at full weight** with MW / IF-TD
/ LLI-corroboration structurally blind (no dual-freq continuity); the epoch loses its
GF iono anchoring network-wide while the IF-pin CB gauge rows hold ⇒ the solve slides
along the W6 iono↔CB↔clock quasi-null ⇒ a00 +5.7k TECU one-epoch step + km-scale
PB/CLK discharge along pre-existing soft PB directions (NEQ-SVD: σ_min UNCHANGED,
RHS loading along weak-3 swings 83×) ⇒ 48/76-min gate eras; era-2's exit scar (181 m
between-sat total_IF re-shuffle, never heals) = the stiff CB carries (q 1e-10, τ≈3.8h)
latching the slid level.

**Falsified with data — do not retry** (all from `snap_20`, ~5 min/leg):
eq34 cold-reattach causal (hygiene card armed ⇒ break unchanged, 5741 vs 5739);
deweight lag (`rate_limit 1.0`, 9 LOS at ZERO weight in-epoch ⇒ 5738.3); code garbage
(raw code GF ranges ≤2.6 m at the worst stations); satellite-common (ΔGF movers are
station-specific); unvetted-arc trigger (G05/G21/G22/G29 excluded network-wide ⇒
5725.8). The break is invariant to everything except the shared 40% row loss.

**Guard gap this card closes**: no existing mechanism censuses the ingest. RAIM tiers
are residual-scoped (and T2/T3/T4 structurally inert on this lineage), the iono
deweight is pin-scoped, clock-runaway is magnitude-degenerate single-system, the SH
gate is emission-scoped and fires only AFTER the damage.

## 2. Design — four increments, independently knobbed

All under `network.ingest_guard.*`. A shared serial census runs once per epoch; the
responses consume its verdicts. Doctrine: adapt-and-EMIT (respond in-epoch with
weights, never drop the epoch outright; withhold stays the SH gate's job), and
prefer-fallback (R inflation, not hard exclusion) per the standing feedback directive.

### INC-A — ingest census + anomaly declaration (detection first, always buildable alone)
`census: true` arms it. Once per epoch (SERIAL, before PASS-B row consumption), count
per constellation: total obs rows, rows per band-class (L1-only vs dual-freq-capable
per arc — an arc is "dual" this epoch if BOTH bands of its station's preferred pair
are present). Reference = per-quantity MEDIAN over the previous `ref_window_ep`
(default 5) NON-anomalous epochs (an anomaly epoch must not poison its own reference —
latch the window). Declare **ANOMALY** when
`rows_now < (1 − anomaly_drop_frac) · median` (default `anomaly_drop_frac: 0.20`) OR
`dual_now < (1 − anomaly_drop_frac) · median_dual`, with `min_rows` (default 100)
suppressing small-network false fires. Exit when counts recover to within the band
(hysteresis: `exit_frac` default 0.10). Emit events `INGEST/anomaly_enter` (with drop
fractions) and `INGEST/anomaly_exit`; per-epoch census row into a NEW trace
`logs/network_ingest_census.csv` (week,tow,const,rows,dual,l1only,median_ref,anomaly).
Validation hook: on the two known epochs the drop is 41-44% — detection must fire at
exactly TOW 286320 and 329160 and nowhere else on the day (see §6 gate D).

### INC-B — single-frequency demotion (the in-epoch response)
`sf_demote: true`. During an ANOMALY epoch, any arc that was dual-frequency in the
previous epoch but is L1-only THIS epoch gets its rows folded with
σ × `sf_demote_sigma_scale` (default 10.0; applies to that arc's phase AND code rows —
the arc is unmonitorable, not just its iono). Outside anomaly epochs INC-B does
nothing (a solitary arc going single-frequency is INC-D's business). Mechanism
precedent: the eq34-hygiene card's `NetStation::pinned_amb_sigma_scale` map consulted
at fold time — replicate that shape as a per-(sat)→scale map on NetStation for row
folding (station-local, PASS-B produce is parallel).

### INC-C — field protection through the anomaly
`field_protect: true`. During ANOMALY epochs (and `field_protect_tail_ep` after, default
2): (i) SH pin rows fold at σ × `field_protect_sigma_scale` (default 10.0) — composes
multiplicatively with the adaptive deweight at the same site; (ii) SKIP the T5v2
deviation harvest those epochs (a demoted/anomalous epoch must not write levels into
the GM channel); (iii) do NOT advance the emit-side PB/CB discontinuity baselines on
withheld-anomaly epochs (they already only advance at emit — verify, don't change).
Rationale: the field is the softest W6 absorber; starving it of the anomalous epoch's
pull is what prevents the +5.7k slide. NOTE deliberately NOT done: freezing the whole
solve or withholding the epoch — code/geometry content is still good (positions,
clocks); only the iono/bias coupling channel is protected.

### INC-D — per-arc re-vet quarantine (the standing hazard, user-specified)
`arc_revet_ep` (default 0 = off; typical 3). ANY arc transition — re-entry after ≥1
absent epoch, OR band loss/regain (dual→L1-only→dual) — starts a vet counter: until
the arc completes `arc_revet_ep` CONSECUTIVE epochs with its full band set, its rows
fold at σ × `arc_revet_sigma_scale` (default 10.0). The vet counter resets on any
slip-funnel hit for that arc (union feed exactly as the hygiene card built:
`carry_slipped_ambs` ∪ `isAmbiguityPendingReseed`). This is soft quarantine
(prefer-fallback); `arc_revet_sigma_scale ≤ 0` = hard withhold of the arc's rows
(available but not the recommended posture). INC-D alone would have blunted both eras
(every dropout arc re-vets at recovery) but INC-B/C are the in-epoch layer — the eras'
damage happened IN the anomaly epoch itself.

Interplay: INC-B/C need INC-A (loader-enforce: warn+disable if armed without census).
INC-D is independent (its own band-state tracking; shares the ctx map).

## 3. Wiring sites (anchors verified in this tree unless marked LOCATE)

- **Census + band-state map**: serial site after PASS-A produce, before PASS-B — the
  T5v2 aging block neighborhood (`ApplicationNetworkAssembly.cpp` ~:1774-1860) is the
  proven serial-context precedent; per-arc presence/bands from `modeled[i]`
  (per-station `PppObservation` vectors, signal entries enumerable — same source the
  IonoVariabilityMonitor reads). Persistent state (previous band-set map, census
  reference window, anomaly latch, vet counters) → `ctx.carry` (new small POD, the
  anchor-hygiene age-map pattern; NetworkCarryState in `app/NetworkRunContext.hpp`).
- **Row-fold σ scaling (INC-B/D)**: replicate the hygiene card's
  `NetStation::pinned_amb_sigma_scale` consult-at-fold pattern for general arc rows —
  LOCATE the exact fold site in the PASS-B produce (`computeMeasurementUpdate` /
  station block build in `ApplicationNetworkAssembly.cpp`); the hygiene card §8 names
  `foldAmb` — the same neighborhood folds the arc's rows. Keep maps station-local
  (parallel produce contract).
- **SH pin σ scale + harvest skip (INC-C)**: pin rows built in `appendShIonoRows`
  (INC-7 helper, `ApplicationNetworkAssembly.cpp` — takes iono_deweight params; the
  anomaly scale composes there). Deviation harvest: the T5v2 exact joint-posterior
  harvest fn in `app/NetworkRunContext.cpp` (ctx lifecycle "harvest" — skip-flag
  plumbed from the census verdict).
- **Events/trace**: `logging/NetworkRunTraces` owns trace streams — new census CSV
  registers there (NOT in the driver); events via the existing emitEvent path (tier
  string "INGEST").
- **Counters + run-end**: `NetworkServiceCounters` (`include/app/NetworkRunContext.hpp:623`
  region, aiq_*/hygiene precedent): anomaly_epochs, anomaly_max_drop_frac,
  sf_demotions, field_protect_epochs, harvest_skips, revet_transitions, revet_holds,
  revet_slip_resets. Run-end block in `filter/NetworkTeardown.cpp` next to the
  adaptive-iono-q and hygiene blocks (~:402 region), with the invariant-#4 dead-wiring
  warn (armed + zero census passes = wiring never ran).
- **Config**: nested block parse in `GnssConfigLoadNetwork.cpp` near
  sh_health_gate/adaptive_iono_q/hygiene (~:2027-2160); NetworkConfig struct gains an
  `IngestGuardConfig` POD (`GnssConfigTypes.hpp`, nested-struct precedent `mw_wl`);
  arming info lines + INERT-guards (INC-B/C require census; all require the network
  producer path).
- **Fingerprint**: NOT appended — forward-only weight/census policy, stored-state
  meaning unchanged (third precedent: adaptive_iono_q, eq34 hygiene — both recorded at
  the conditional-append site in `networkSnapshotFingerprint`,
  `app/NetworkRunContext.cpp:134-260`). This keeps every §6 shared-snapshot A/B legal.
- **CMake**: if a new seam TU is created (recommended: `filter/NetworkIngestGuard.{hpp,cpp}`
  mirroring NetworkAnchorHygiene), add to CMakeLists.txt manually (no globbing).

## 4. Telemetry contract (invariant #4)

Every response action counted with a denominator; the census trace makes the day's
ingest shape a first-class artifact (the missing observable this whole investigation
had to reconstruct from raw RINEX). Run-end line prints census passes, anomaly
epochs (+ max drop), demotions/protects/vets, and warns explicitly on armed-but-zero
census passes.

## 5. Unit tests

New file `tests/test_ingest_guard.cpp` (one suite, unique name). Cases: census median
window + latch (anomaly epoch excluded from its own reference); detection threshold +
hysteresis exit + min_rows suppression; band-classification (dual vs L1-only from a
synthetic PppObservation set); SF-demotion map only during anomaly; re-vet counter on
re-entry, on band-loss, reset on slip-funnel, completion at K; scale composition with
the deweight at the pin site (multiplicative); OFF ⇒ no maps allocated, no rows scaled
(structural byte-identity).

## 6. Field validation (FT63 session; controls + harness standing)

Replay mechanics (validated, reuse verbatim): seed = `runs/ft63_24h_allon/snap_archive/`
copies; decode generation from the text `M,<week>,<tow>` line (numbering drifts
~30 s/write); `state_snapshot_require: true`; era-2 legs = snap_20 (gen 19:06:30) +
`start_offset_hours: 19.1125`, dur 3600; era-1 legs = snap_08 (gen 07:02) +
offset ≈ 7.0375 (07:02:15), dur 3600; NEQ probe = emitted-count target (2236 pre-break
/ 2237 break) + `sh_health_gate:false` probe leg for the break epoch (gate is
emission-only). Exe: fresh Release build, renamed copy (`gnss_processor_fable_<tag>.exe`).

Baselines to beat (era-2 unless noted): engage at 19:26:00, a00 peak 5739/5741/5738/5726
across arms; 83 oob epochs in the 1h window; across-gap total_IF re-shuffle 181 m
(era-1: 55.8 m, healed); postfit phase σ0 2.95 → 56.7 at the break; exit-scar
excursion 20:42→24:00 worst ~100 m (G07); QC panel: rx-CB withholds ~14.5%, σ̂₀ 1.00,
raw-erases 1308 (24h).

Gates, in order (10-ep smoke first):
A. Smoke: knobs parse, arming lines, OFF twin byte-identical.
B. OFF-identity leg vs `runs/ft63_leg19_off`: a00 5738.6 at 329160 bit-match.
C. **Detection leg (INC-A only)**: `INGEST/anomaly_enter` at EXACTLY 329160 in the
   era-2 leg and 286320 in the era-1 leg; zero false anomalies elsewhere in both 1h
   windows; census CSV shows the 40-44% drop. (Full-day false-positive census happens
   at gate F.)
D. **Response legs** (INC-A+B+C armed; INC-D off first to isolate): NO SH-gate
   engagement at 329160; a00 SUM < 150 through 19:40; postfit phase σ0 at 329160 stays
   O(3); across-gap total_IF span (19:25:30 vs 19:30:00) ≪ 181 m. Then INC-D added
   (K=3): same gates + revet counters live, QC panel flat vs OFF leg (D5 ±few %,
   rx-CB withholds ~14-15%, σ̂₀ 1.00, no refloat storm).
E. NEQ probe pair on the armed config: Δx PB steps collapse to O(σ) (baseline
   PB:G24:L5Q −5470 etc.), weak-3 RHS loading back to O(±33).
F. **24h rerun (~80 min)**: both eras ABSENT (0 gate engagements — the anomaly epochs
   pass as demoted blips) or reduced to ≤2-epoch transients; exit scar GONE
   (between-hour distortion hr20-23 back to the ~17-21 m quiet class; quiescent trend
   verdict NO-DRIFT); anomaly count = exactly 2; full-day false-anomaly count = 0
   (storm-day iono must NOT trip a row-count census — different observable);
   total_IF hourly panels ≤ baseline everywhere else (no collateral). Product claims
   total_IF only.
G. Cold-start sanity: 10-ep cold smoke with all increments — census reference warms
   without firing (min_rows + window latch), re-vet does not blanket-quarantine the
   cold start (first sightings are NOT transitions — only re-entry/band-change vet).

## 7. Deferral triage (pre-declared)

- Real scintillation/EPB response (gradual, sectoral, hours-long) — OUT of scope: this
  census is a step detector; the deweight/adaptive-q lineage owns gradual iono stress.
  Field-inert for this card's verdict (the two events are steps).
- Multi-constellation census tuning: counters are per-constellation by design but
  validated GPS-only (the FT63 substrate); GAL/BDS thresholds deferred until a
  multi-constellation lineage adopts the guard.
- Sub-network (regional) dropouts below the global threshold: the per-constellation
  global census won't see a 5-station regional outage — acceptable (that class lacks
  the network-wide gauge-vs-data tension; single stations going dark are already
  handled by presence pruning). Revisit only with field evidence.
- Provenance of the DOY-129 events (flex-power attribution): diagnostics commentary
  only; nothing in the guard may depend on external schedules.

## 8. Implementation record (fill in when built)

(builder: record deviations here, per the eq34-hygiene §8 precedent — that record's
deviation list materially improved the card; do the same.)
