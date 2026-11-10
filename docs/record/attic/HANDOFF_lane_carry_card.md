# HANDOFF — Lane-coordinate carry (M5 card WL-3)

**Implements** `docs/SPEC_lane_carry_card.md` increments 2–4 (Fable, Jul-16 2026;
increment 1 = config contract landed same-day).  Default-OFF, byte-identical
(bit-exact CSV hashes vs the pre-WL-3 exe re-verified on the final build).
**Status: MECHANISM COMPLETE + quick-gated.  Acceptance §6.4 MET and exceeding
the WL-2 baseline.  Increment 5 (dual-freq grading, graded run) awaits command.**

## Result summary (15-ep 50-sta GPS `_wlver` harness, Release, same-build A/B)

| metric | WL-2 green shape (`_wlinj`) | WL-3 lane shape (`_lane`) |
|---|---|---|
| D5 flags | 358 | **373** (control family; storms are 649–1628) |
| constrained circR (pooled last-5) | 0.343 | **0.471** |
| constrained med\|f\| | 0.137 | **0.061** |
| P(\|f\|<0.15) | 0.519 | **0.651** |
| per-epoch wobble | med\|f\| 0.01↔0.30 oscillation | **≤0.064 for 11 consecutive epochs** (3–13) |
| injects | 24 | **0** (retirement telemetry, §3.3) |
| lane increments | — | 1005 MW + 112 bridge rows |
| FFR decisions | 50 (holds+injects) | 41 → 9 moved + 32 re-pinned |

10-ep smoke D5 218 (control 225).  §7.3 force-fire re-run (`_lane_ff`): EWL
144 fixed = the WL-2 ff baseline (evaporate → re-fix), 13 moved fixes, exactly
1 fallback inject (|Δ|=2.01 — the post-slip catch), no storm (D5 440), no
errors/NaN.  Full gtest suite 761 run / 760 pass / 1 pre-existing skip,
including 7 new WL-3 driver gates (equivalence, lane prior/q hand values, lane
KF hand value, fix semantics, union refloat, pair inject exemption, pair
parallel-install bit-identity).

## What landed where

- **Driver** (`NetworkInvariantCarryDriver`): pair groups (`GroupInput.pair` +
  per-arc `local_col2`/`slipped2`), joint [band-1; band-2] carried storage with
  per-band W projection, spanning-forest completion for pair components
  (`pair_tree_promotions`), lane-basis fresh prior + lane q (`laneRotation`),
  split-vs-joint emission (`BlockMeta.pair_band` 1/2 = §6.1 equivalence halves,
  3 = joint 2K lane block), `applyLaneMeasurement` (scalar KF fold of a
  per-epoch increment), `applyLaneFix` (idempotent lane-coordinate SET, see
  deviations), `carriedPairValue`/`injectCarriedChordPair`, pair handling in
  both installPosterior paths, fold-on-change memory staged per build /
  committed at install (RAIM re-solve safe).
- **Detector** (`MwSlipDetector`): `arcZdLast` / `arcBridgeLast` — LAGGED raw
  per-epoch samples + per-sample Welford variance (the once-only increment
  sources; `prev_mw_lag`/`prev_gf_lag` snapshots ride every frame shift), and
  `setBridgePreferredChannels` (see deviations).
- **ApplicationNetwork**: WL machinery hoisted above the group build; census-
  preferred primary pair under `lane_carry`; pair-group routing (residual
  minority arcs stay per-band); MW-increment walk + bridge-increment/fix walk;
  pair inject fallback; pair lane-trace + D5-funnel (band-2 keys via
  `carry_arc_amb_key2`); WL-3 run-end telemetry line.
- **Configs**: `_wlver_50sta_{10ep,15ep}_lane.json`, `_lane_lt` (trace),
  `_lane_equiv` / `_lane_equiv_ctl` (§6.1 leg), `_lane_ff` (force-fire).
  Exe: `build/x64-Release/gnss_processor_wl3_run.exe`.

## Spec deviations — each measured, none speculative

1. **§3.1 fresh lane prior (σ_wl_fresh = 3 cyc) FALSIFIED — default now 0
   (diffuse birth).**  The correction-frame origin is NOT the code-seed lane: a
   cycle's ambiguities seed at different epochs, so per-station clock error at
   seed time does not cancel in the lane (× (f₁−f₂) ⇒ O(10²) cycles of
   mis-centering).  Measured: D5 691 (prior-only) vs 125 (q-only) at 10 ep.
   The honest code-seed-scale info enters via the FIRST MW increment instead
   (correctly centered).  σ>0 remains as a loud-warned study shape.
2. **§3.1 q_wl = 10³×q_nl FALSIFIED at the lineage qz — default now 0.01.**
   The numerology assumed q_nl≈0; at qz=1e-4 m²/s it adds ~12 WL cyc of lane
   variance per 30 s epoch, swamping every lane measurement and destroying
   each fix within one epoch.  0.01×qz ≈ (0.04 cyc)²/epoch keeps √N
   accumulation + fix persistence (circR 0.471 vs 0.251 at scale 1).  Loader
   clamp relaxed to ≥0.
3. **§3.3 fold-on-change → decide-per-epoch + idempotent lane-coordinate SET.**
   A one-shot fold leaves the decided lane to drift back to MW quality under
   q + noisy increments (measured med|f| 0.25 vs 0.06).  The fix is now an
   information-idempotent SET (recenter + recollapse + zero cross-cov — WL-2's
   inject semantics on the lane coordinate, NL preserved by construction),
   re-applied every decided epoch.  The F-A once-only discipline holds: the
   set does not accumulate; `applyLaneFix` returns moved-vs-re-pinned for the
   telemetry.
4. **§2 pair-group membership needed two additions the spec didn't
   anticipate:** (a) a census-preferred primary pair per constellation — the
   lexicographic tie-break fragments pairs across stations (L2L-vs-L2W),
   splitting the pair graph and forfeiting ~1/3 of the primary cycle space
   (measured); only the majority pair joins the pair group, minority arcs stay
   fully per-band; and (b) driver-side spanning-forest completion — the pair
   subgraph has no upstream election, and band-1's tree tags need not span it
   (27 completions / 15 ep after (a); 903 before).
5. **§6.1 strict ≤1e-9 field equivalence is unattainable by design** whenever
   the two bands' election trees differ or arc churn interacts with different
   fundamental-cycle paths (carried/born decisions are cycle-path-dependent;
   divergence starts at 0 at epoch 1 and grows with churn — measured, gate-off,
   both slip-semantics variants).  The strict gate lives in the UNIT test
   (exact through noise + per-band slip + tree change).  The field leg
   validates STRUCTURAL parity instead: carried dimension 904-vs-912, births
   1286-vs-1271, refloats 321-vs-312, 0 whole-block flicker, D5 310-vs-348,
   boundary quantified by the completion counter.
6. **Bridge arcs re-keyed to the census channels** (`setBridgePreferredChannels`):
   the detector's min-name per-band selection starved the WL decision path to
   9-of-29 cycles at (rx,sat)s whose channels differ from the census pair.
7. **Union slip refloat (§3.4) is LANE-shape-only**; the equivalence shape uses
   per-band slip semantics (matching the two per-band groups it reproduces).
   In practice the PASS-A funnel is band-symmetric, so this distinction is
   latent.

## Deferrals (triaged)

- **Cross-boundary cycles between the majority pair group and minority-channel
  residual groups are not carried** (~8 of 912 DDs ≈ 1% on this fixture —
  field-inert at this scale, DATA above).  A v2 quotient carry could recover
  them.
- **Dual-freq (no-bridge) lane maturity** (§7.5): control-population circR
  0.050 vs 0.019 at 15 ep — the √N accumulation is visible but the census law
  (σ(n)≈0.5–0.7/√n) needs 25–50 ep for FFR grade.  The 15-ep harness cannot
  reach it; increment 5's graded run (on command) is the test.
- **Late-window trace epochs 14–15** (med|f| 0.18/0.31 on n=9): end-of-run
  slips re-tap constrained chords; the §3.4 self-heal needs ~2–3 epochs the
  run doesn't have.  Not a mechanism defect (eps 3–13 clean); the longer
  graded run measures steady state.

## Verification checklist (final build)

- OFF byte-identity: `network_solution.csv` + `network_dd_fractional.csv`
  hash-identical vs `gnss_processor_fable_ab.exe` (pre-WL-3) — after ALL
  shared-path edits (machinery hoist, detector lag fields, install refactor).
- WL-2 regression: `_wlinj` on this build reproduces D5 358 and the lane-trace
  baseline circR 0.343 exactly.
- Suite: 761/760/1 (7 new WL-3 gates).
- Quick-only per the staged cadence; nothing committed.
