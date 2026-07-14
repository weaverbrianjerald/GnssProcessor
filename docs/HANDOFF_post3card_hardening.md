# HANDOFF: post-3-card hardening (deweight perf ▸ IF-TD arming ▸ per-λ disc contract)

**Date:** 2026-07-14 (morning session, Fable)
**Executor:** Opus, fresh chat session.
**Scope:** three independent cards (H1 code+perf, H2 config+runs, H3 code+contract).
Implement in order H1 → H2 → H3; they are independent, so a blocked card does not
block the others. LEAN style: single-pass review, no agent panels.

## Precondition — verify before touching anything

The three Jul-13 cards (ATX radome fallback, iono deweight, PB emit filter) plus
Fable's PB unit-test fix (`tests/test_pb_emit_filter.cpp`, Gaussian probe noise —
see §H0) were **uncommitted** in the shared working tree when this handoff was
written, with validation runs in flight. **Start only after that commit ladder has
landed** — `git log --oneline -5` must show the 3-card commits above `02bce7f`.
If the tree is still dirty with those cards, STOP and report; do not build on a
moving tree. Baseline binary for all A/Bs = fresh Release build of HEAD
(same-build A/B only; never compare across builds — verification integers are
build-config-sensitive).

## H0. Context (measured this session — do not re-derive)

- Combined 3-card tree: fresh Release build clean; suite **714 passed / 1 skipped / 0
  failed** after fixing `PbEmitFilter.StepSigmaSelfTunesToNoiseScale` (the test
  probed the σ̂ estimator with an alternating ±s square wave; the estimator's
  `kChi2GeoMean` de-bias assumes Gaussian innovations, so it read 1.89× high *by
  construction*. Filter code untouched; the test now probes with deterministic
  Box–Muller noise, seed 20260714).
- 305-sta dual-sys (GPS+Gal) 3-card 30-min run, steady state **~49 s/ep**:
  assembly 10.3 s | reduce 4.2 s | solve 0.10 s | cov-inv 0.25 s |
  **cov-consumers 10.2 s (1214 calls)** | **other ~24 s**.
  Reference (converged attribution arm, same recipe minus new cards, solo):
  ~20 s/ep with cov-consumers 0.6 s (911 calls), other ~1.9 s.
  ⇒ **adaptive deweight v1 alone adds ~32 s/ep** (see H1).
- Overnight baselines to grade against (established, do not rerun): 305 all-on
  attribution arm CONVERGED — 3h windows 1.36/2.38/1.91/1.48/1.78/2.23 tail 1.97;
  6h event-survival b8 15.02 → 2.04 recovered; 12h GPS-only full-day PASS.
  Run dirs: `build/x64-Release/run_n305_attrib_{3h,6h}`, `run_gpsonly_em25_12h`
  (each has `qc_dashboard.html`).
- Emission-frame wire noise (run-end line, both overnight runs): mean
  |common-fractional δ| ≈ **0.034 m** — i.e. ~0.7× the 0.05 m disc gate (H3's why).

**H0-bis. 30-min combined-run verdicts (graded after this doc was first written):**
- **Pool restoration:** carried pool 8,325 (allon2 class), NOT the 505 collapse —
  and DD ignition returned to the allon2 shape (5.22/5.48/8.71 per 10 min vs
  allon2's 5.07/5.39/8.22). Leading hypothesis: the mystery pool-collapse
  variable was `phase_bias_exclude_unmatched_antenna` × the 12 unmatched-ATX
  stations; the ATX card's always-on radome fallback makes them matched, the
  exclusion goes inert, the pool comes back. Means ALL converged overnight
  baselines rode the 505-pool regime — pool size is a live confound in every
  cross-run comparison; check the carry counter block before comparing anything.
- **Deweight gate-3 (autonomy): NOT met at 60 ep** — 30 stations engaged
  (equatorial-anomaly / Pacific / Antarctic cluster; 0 releases), **UFPR/POAL not
  among them**. Mechanism insight to carry: the pair's poison was diagnosed via
  Galileo DD-chord churn; the gate triggers on SH slant-resid excess — possibly
  orthogonal axes. 60 ep is ignition-only; the decisive read stays the 3h arm.
- **PB filter:** mechanically flawless — counters reconcile exactly (5,769
  innovation resets + 50 preserved ref-path steps + inits ≈ 5,893 disc
  increments; 18,807 smoothed key-epochs silent; mean posterior σ 17 mm).
  Emitter disc rate **~200/ep → 98.2/ep**: halved, NOT single digits — the
  wire step process is heavy-tailed at 305 (the D5-v2 lesson again). The card's
  same-build filter-OFF A/B remains OWED before any disc-floor claim.

**H0-ter. GPS-only 3-card 30-min arm (run_n305_3card_gpsonly_30m, ~26 s/ep):**
- **Pool: 4,826 GPS chords** — the committed-binary "GPS-only pool = 114-142,
  98% of dual pool is Galileo" was a collapsed-pool artifact. True full-pool
  split ≈ 58% Gal / 42% GPS. Every overnight GPS-only baseline rode the
  collapsed regime; DD ignition here 2.45/2.27/4.06 (vs sub-1 collapsed-pool
  probe) — pool regime moves ignition DD-step everywhere.
- **Deweight null-check FAILED — over-fires:** 37 stations engaged (MORE than
  dual's 30; includes PENC00HUN, Hungary), 0 releases. Two suspected causes for
  H-series follow-up: (a) a network-RELATIVE 3σ tail is always populated at
  N=305 under heavy-tailed misfit — relative z needs an absolute-excess AND
  term or engage-count cap; (b) **release may be structurally unreachable**:
  once w_s→0 the station's slants decouple, so the station-vs-field
  disagreement statistic can stay >2σ forever (ratchet). Verify (b) in
  `NetworkIonoDeweightGate` and fix before trusting any 3h deweight arm.
- **PB filter on GPS-only: discs 96.3/ep ≈ dual 98.2 ≈ committed GPS-only
  baseline ~100/ep.** Reset fraction 48% of key-epochs (σ̂ self-tuned to a
  quiet 8.3 mm wire ⇒ 4σ̂ gate ~3 cm ⇒ heavy tail fires it constantly). Reading:
  in dual, the filter removed the ~100/ep Galileo-side noise component and
  preserved a ~96/ep GPS-side step process it (correctly, per its k=4 mandate)
  refuses to smooth. Single-digit disc floors need the TAIL treated (robust-M /
  k recalibration / step-process diagnosis), not just H3's per-band buckets —
  scope H3 expectations accordingly.

---

## H1. Adaptive-deweight perf card — remove the ~32 s/ep serial tax

**Why.** The deweight card's v1 gate update is a SERIAL post-solve loop
(`src/core/app/ApplicationNetwork.cpp:9966`–ff): per present station per epoch it
calls `est.reconstructStation()` (`:9999`) — +303 cov-consumer calls/ep — and
re-derives the standardized slant-residual RMS from per-LOS degree-15 SH
evaluations with per-station map allocations. The loop's own comment (`:9968`)
concedes it recomputes "the SAME resid/σ as the health trace above" (T1a trace,
opened `:1727`).
**⚠️ Corrected sizing (Jul-14, after the 30-min combined run finished):** the
first-look "~32 s/ep" attribution was CONFOUNDED — the combined run's carry pool
restored to 8,325 chords (see H0-bis below), so the right baseline is the
allon2 class (~53 s/ep, cov-consumers 8.0 s @ 910 calls, "other" ~29 s
unattributed), not the 505-pool attribution arm (~20 s/ep). Against the same-pool
baseline the measured deweight tax is **+303 calls ≈ +2.3 s/ep of cov-consumers,
plus an unisolated share of "other"**. The refactor is still right (dedup the
statistic, parallelize, feed D1), but the perf gate below is re-scoped, and the
BIG 305 cost story is the pre-existing allon2 "other 55% unattributed"
instrumentation card — do not conflate the two.

**What (three mechanisms, in order of preference):**
1. **Compute-once/share:** hoist the per-(station, LOS) standardized residual into
   a single site producing both the T1a health-trace row and the gate's
   `observe()` input. The statistic must NOT depend on whether the health-trace
   toggle is on (compute unconditionally when `iono_deweight.adaptive()`).
2. **Parallelize the per-station statistic:** the loop is read-only against the
   solved state `x_s`; only `gate.observe()` mutates. Compute statistics in the
   existing parallel-station machinery (or a 20T loop), then commit `observe()`
   **serially in station order** (determinism — same discipline as the D5 seam;
   recall the GMGᵀ lesson: accumulation order changes integers).
3. **Kill or batch the `reconstructStation` calls:** if mechanism 1 sources
   residuals from the assembly-side quantities, the per-station reconstruction may
   be unnecessary entirely; otherwise batch it (this is the standing D1
   selected-inverse/per-station target — the deweight card just became its
   biggest customer).

**Gates:**
- G1 *(structural)*: suite green (714/1 baseline); new/changed unit coverage for
  the shared-statistic path if you split one out.
- G2 *(numerical identity)*: 10-ep 305 dual-sys A/B old-vs-new gate path, SAME
  build — the per-epoch deweight weights `w_s` (telemetry
  `iono_constraint_deweight_stations` / `_factor`) must be **bit-identical**, and
  the solution stream byte-identical. This is a pure refactor; any drift = FAIL,
  investigate ordering.
- G3 *(perf, re-scoped)*: 60-ep 305 dual-sys, all-3-cards config
  (`_net305_3card_30m.json`): cov-consumer calls return to the ~911-call class
  (deweight's +303 reconstructions eliminated or batched) and the deweight
  stat-loop's share of "other" is measured-then-removed (instrument first, then
  delete — report both numbers). Reference run: `run_n305_3card_30m` @ 53.0 s/ep,
  cov-consumers 10.3 s (1213 calls/ep), other 27.5 s. Do NOT chase the epoch
  total below the allon2-class floor here — that's the separate instrumentation
  card.
- G4 *(OFF byte-identity)*: `iono_constraint_deweight_adaptive=false` run remains
  byte-identical to pre-change binary output (deweight-OFF path untouched).

**Deferral triage:** D1 batching (mechanism 3, batch form) may be deferred
field-inert IF mechanisms 1+2 alone meet G3 — state the measured number.

## H2. Arm the IF-TD coarse slip layer — close the MW (k,k) null space

**Why.** The network coarse slip layer in every lineage config is **MW-only**.
MW's ambiguity is N₁−N₂: equal slips on both signals are structurally invisible,
and today only the D5 estimation-side innovation gate catches them (it does —
59k re-taps over the 6h run — but each catch costs arc continuity). The IF-phase
TD detector sees equal slips at **k·c/(f₁+f₂) ≈ 10.7 cm per cycle** (L1/L2),
is iono-free (rate-robust where GF is not — GF/TurboEdit stays 1s-RT-only per
standing directive), and is **already built and wired**: constructed per station
at `ApplicationNetwork.cpp:1153` when `network.cycle_slip.if_phase_td` is on
(default **false**, `GnssConfigLoad.cpp:3716`), pooled per-pair MAD flagging
(`if_phase_td_k_mad` default 8, `min_pool` 30), detect at PASS-A (`:3821`),
flags feed the C3c refloat funnel. In the network, station positions are pinned,
so the detector's one historical staging concern (pre-convergence position error)
is void by construction. NOTE: the CLAUDE.md / header banner claim that
`IfPhaseSlipDetector` is "STAGED — uncompiled" is **stale for the network path**;
fix the doc line while you're there.

**What:** config + runs only (no code unless a gate fails).
1. 10-ep smoke, 305 dual-sys, attribution recipe + `"cycle_slip": {"if_phase_td": true}`
   in the `network` section: banner/trace confirms detector live
   (`network_if_slip.csv` if trace toggle on; `T1_if_slip` QC rows), no error spam.
2. 60-ep A/B, SAME build, toggle-only diff (do NOT bundle other arms — single
   variable): ON vs OFF.

**Readouts (priority order):**
- `T1_if_slip` flag rate — nonzero and sane (not swamping: if it flags >~5% of
  arcs/ep, the pooled MAD k=8 needs data-driven review — empirical thresholds
  only, MAD+Welford; no hand constants).
- **D5 flag rate delta** — the healthy signature is detections migrating
  upstream: D5 re-taps (OFF baseline ≈ 164/ep at 305 dual) should DROP on the ON
  arm. If D5 drops AND per-30-min DD windows hold or improve ⇒ PASS.
- Carry churn: births + slip-refloat rate vs OFF (baseline ~30%/ep turnover).
- DD windows (10-min buckets at 60 ep) — must not degrade vs OFF.

**Falsifier:** if ON degrades DD windows or D5 rate does not move, the detector
is mis-tuned at 30 s cadence — capture `network_if_slip.csv` and STOP (report;
threshold work is a separate card, not an inline tweak).

## H3. Per-λ disc threshold (contract) + per-band σ̂_step buckets (filter)

**Why.** `NetworkProductEmitter::kDefaultDiscThresholdM = 0.05`
(`include/products/NetworkProductEmitter.hpp:156`) is the R9 producer↔consumer
contract constant, premised on "noise does not trip it" — falsified: wire noise
≈ 0.034 m (H0). The harm model is in CYCLES (consumer ambiguity corruption at
~¼ cycle), so a flat meter gate is 0.26 cyc on L1/E1 but 0.20 cyc on L5/E5a —
tighter (in cycles) exactly on the longer-λ, noisier signals. A per-signal
threshold `frac · λ(sat,signal)` is still a valid FIXED contract (λ is public,
static identity — deterministic on both ends; "fixed" ≠ "scalar"). Elevation /
roster-thinness noise stays OUT of the contract — that's the adaptive layer's
job (PB filter).

**What:**
1. **Emitter:** new config `network.disc_threshold_frac_cycles` (double,
   default 0.0 = legacy 0.05 m behavior ⇒ byte-identity OFF). When > 0 (recommend
   0.25), per-(sat,signal) threshold = frac × λ via `common/SignalBand` +
   `common/Constants` wavelengths (API takes (sat, signal) so GLONASS FDMA slots
   in later; network GLONASS is currently off). Applies to the phase jump test
   and, when `code_bias_disc_jump_test` is on, the code test (same threshold —
   preserve that documented coupling).
2. **Contract plumb-through:** the S-basis replay contract currently carries
   `disc_threshold_m` (see emitter header ~`:156`–`:186` and
   `makeReplayEmitter`) — extend it so producer and replay consumer agree in
   per-λ mode (carry the fraction, or per-signal resolved values; pick the one
   that keeps replay byte-exact and say why).
3. **PB filter follow-on:** refine `NetworkPbEmitFilter` σ̂_step bucketing from
   per-constellation (`bucket(e.sat)` → "G"/"E") to per-(constellation, band),
   hierarchically seeded toward the constellation pool (same kPriorRows trick) so
   thin bands aren't noise-starved. Captures the L5-family's genuinely fatter
   noise without touching wire semantics.

**Numbers for sanity:** 0.25λ ⇒ L1/E1 4.76 cm (≈ legacy — continuity), L2 6.1 cm,
L5/E5a 6.37 cm. Expectation: this equalizes SEMANTICS; it does NOT fix disc churn
(gate still ~1.5–2σ of wire noise) — the filter owns churn. Do not oversell in
the report.

**Gates:**
- G1: unit tests — per-λ threshold selection per signal (incl. a would-be-FDMA
  case via the API), replay agreement (produce → replay byte-exact in per-λ
  mode), legacy default byte-identity.
- G2: OFF-hash — default config output byte-identical pre/post change.
- G3: 10-ep 305 A/B legacy-vs-0.25λ with `emit_pb_filter` ON: disc counter
  deltas per constellation/band (expect L2/L5-family discs to drop, L1 ~flat);
  the >0.5 m synthetic-event preservation logic in
  `tests/test_pb_emit_filter.cpp` gate 5 must still hold 1:1 (anti-launder).

---

## Run hygiene (standing — violations invalidate results)

Release-only for anything graded; run from a RENAMED exe copy
(`gnss_processor_opus_run.exe`) so concurrent builds never hit a locked exe;
pre-kill `gnss_processor*` zombies; 10-ep smoke before any ≥60-ep run; grade
datum-invariant quantities only; counters (not crash-absence) are the ON-path
telemetry — "0 driven / 0 folded" = inert wiring = FAIL; one graded variable per
A/B. Configs: dual-sys base recipe = `config/test_suite/_net305_3card_30m.json`,
GPS-only = `config/test_suite/_net305_3card_gpsonly_30m.json` (durable copies;
derive smoke/60-ep variants by editing `max_duration_s` in scratch). Graders:
`scripts/grade_ddfrac_perhour.py <run_dir> 600|1800`, `scripts/qc_dashboard.py
<run_dir>`.

## Report back (per card)

Gate table (PASS/FAIL + the number), D0 timer lines verbatim (H1), counter blocks
verbatim (H2/H3), run dirs + dashboard paths, deferrals triaged
field-blocking vs field-inert WITH data. Fable grades the verdicts.
