# HANDOFF — Lane-fold efficiency (M5 perf card "LF-1")

**Implements** `docs/SPEC_lane_fold_perf_card.md` (Opus, Jul-17 2026, on user command).
Default-OFF; the OFF path is bit-exact vs the pre-card exe.  **Status: INC-0 / INC-1
/ INC-2 / INC-4 BUILT + unit-gated + 50-sta smoke-gated.  INC-3 STRUCK on user
command (its premise is falsified — see below).  The 150-/305-sta graded/perf legs
are deferred to the staged cadence (on command).**

## Scope decision — INC-3 struck (premise falsified)

The reconnaissance (6-agent map + adversarial critic, all four load-bearing claims
re-verified against source) found INC-3's premise false three independent ways, so
the user chose to **skip it entirely** (no marginalization mechanism, no telemetry
fix):

1. `carried_` is **rebuilt from scratch every epoch** (`carried_.swap(next)`,
   `NetworkInvariantCarryDriver.cpp:1075/1179`).  K per component is *this epoch's*
   live chord count — there is no monotone growth to "cap."
2. `ZComponent::killChord` has **exactly one call site repo-wide** (a unit test,
   `tests/test_network_zstore.cpp:290`); the live driver marginalizes exactly and
   for free via the zero-`W`-column projection (`buildDdPriors`).
3. The "0 marginalized" number came from the always-on line printing the
   *whole-block* counter (`chords_marginalized`, structurally ~0 at 305 sta because
   giant live components always share a vertex).  The per-chord counter exists and
   **INC-C1 already closed this Jul-15 as case (a)** — `SPEC_carry_factored_pz_C2.md:207`.
   The card cited the superseded Jul-14 number.

Marginalization *cannot* shrink K; the only real K-reducer is forest/component
partitioning (an approximation needing a field grade — outside this card's
exact-only scope, and left as a future card).

## Card increment ORDER was inverted — fixed here

The map found the biggest lane-fold win the card only half-anticipated (§7 Q1) and
it is **asymptotic**, not the constant-factor BLAS-3 batch the card led with:

- `applyLaneFix` computed `Pu = blk.Pz * u` as a dense **O(K²)** GEMV when `u` has
  exactly 2 nonzeros, and applied a rank-2 update that only touches rows/cols
  {k, K+k} as a dense (2K)² `+=` — **O(K²) work for an O(K) update**.
- Both lane primitives trailed a full `0.5·(Pz+Pzᵀ).eval()` — a redundant O(K²)
  pass + a (2K)² alloc on **each of the ~10³ rows/epoch**.

**Falsified sub-claim (paid for by the gate):** the per-row re-symmetrize is NOT a
bit-exact no-op.  It holds symmetry only to ULP, and that ULP asymmetry, fed
through the stiff q_z=0 DD carry, **amplifies** across epochs (measured: dropping
it default-ON drifted the lane-on solution ~1e-4 by epoch 4, reproducing the
recorded hashes' failure).  So the symmetrize-hoist is equivalence-class, not
free — it lives behind `fold_blocked`, and the OFF path keeps the exact per-row
symmetrize (bit-exact).

## What landed where

### INC-0 — instrumentation split (log-only, byte-identical)
- `NetworkInvariantCarryDriver::PerfTimers`: `lane_meas_ms` / `lane_fix_ms` +
  `lane_meas_calls` / `lane_fix_calls` (ScopedAccum around the two primitives).
- `Counters::comp_kc_hist[10]` — emitted-component fold-dim distribution (§7.4
  Amdahl census), always-on like the other census counters.
- Two run-end lines under the `Network (D0` prefix (already masked in the dcmp
  gate): `Network (D0/LF-1) lane-fold per-op` + `... K_c histogram`.
- **Measured (50-sta/10-ep uwl):** the lane fold is applyLaneMeasurement-dominated
  (1101 rows) vs applyLaneFix (53 rows at 10 ep — grows to the ~65%-decided
  population at 30+ ep).  K_c histogram showed **many components** (88 in the 64-127
  fold-dim bucket), NOT one giant one ⇒ INC-4 has good parallelism at this scale.

### INC-1 — blocked/sparse lane fold (`network.lane_carry.fold_blocked`, default OFF)
- `applyLaneFix` O(K) sparse path (Pu = 2-column combo; rank-2 update writes only
  cols/rows {k, K+k}, mirrored to stay exactly symmetric); the dense O(K²) path is
  kept verbatim for OFF (bit-exact).
- `finalizeLaneBatch(blocks)` — one re-symmetrize per joint block per epoch,
  replacing the ~10³ per-row ones (skipped under `fold_blocked`).
- **Equivalence class** (op-order change ⇒ ≤1e-9 vs sequential, NOT bit-identical).
  Carries the pcarry usage rule until the §5.3 physically-null gate passes.
- **Perf (50-sta/10-ep):** applyLaneFix **223 ms → 0 ms**; applyLaneMeasurement
  **2667 ms → 1076 ms** (symmetrize hoist).  Field drift OFF-vs-ON dd-frac:
  wrap-corrected **median 0, p95 0.011, max 0.11** (one G04-G03 L5X component);
  no FFR integer flipped (all real drifts < 0.5).  Larger than pcarry's "few 1e-3"
  because it perturbs more (symmetrize-hoist + sparse fix vs pure GEMM
  reassociation); the definitive arbiter is the deferred 150-sta circR/med|f| A/B.

### INC-2 — pinned-cycle increment skip (`network.lane_carry.skip_pinned_increments`, default OFF)
- Skip MW/bridge increments for a chord whose FFR fix is COMMITTED
  (`lane_fix_w_`) and re-decided within `skip_pinned_within_n` epochs (default 3).
  Reads COMMITTED state only (never the per-build staged decision), so every RAIM
  re-solve of the epoch skips the SAME rows — deterministic.
- Any funnel event (slip / D5 flag / re-tap / rebirth) erases the epoch stamp
  (`lane_fix_epoch_`) in lockstep with `lane_fix_w_` ⇒ increments resume the SAME
  epoch (invariant #1 — provably unaffected slip response).
- `laneIncrementSkippable(b, k)` query; app-side `wl_run.lane_pinned_skips` counter
  (invariant #4).  Equivalence class.

### INC-4 — component-parallel MW fold (`network.lane_carry.parallel_components`, default OFF)
- `#pragma omp parallel for` over joint blocks in the MW walk (mirrors the
  `installPosterior` parallel readout pattern).  Each block's folds are independent
  (no shared Pᶻ) and order-preserved within the block ⇒ **BYTE-IDENTICAL to serial**
  (per-block counter deltas sum order-independently; the strong hash gate applies,
  unlike INC-1).
- Eigen re-pinned to 1 inside the region (the pcarry un-pin stays OUTSIDE — the
  dev300s nesting trap); the per-op driver timer silenced for the parallel pass
  (would race a single double; app-side row counts stay exact via per-block deltas).
- Needs `parallel_stations` (n_threads > 1) to fire.

## Config — all four knobs default-OFF, all PERF knobs (NOT lane-shape)

`network.lane_carry.{fold_blocked, skip_pinned_increments, skip_pinned_within_n,
parallel_components}`.  Deliberately kept OUT of the `equivalence_shape` /
`lane_joint` discriminators (they change only HOW the algebra is applied, never
what folds) — see the loader comment at `GnssConfigLoadNetwork.cpp`.

## Verification (this build, Release, same-build A/B vs gnss_processor_lf1_base.exe)

- **OFF byte-id** (`_wlver_50sta_15ep_off` + `_wlver_50sta_10ep_uwl`): `network_solution.csv`
  + `network_dd_fractional.csv` bit-identical vs the pre-card exe, reproducing the
  recorded hashes 90DB1548/DB1A081C + 3A395593/B5A04846.
- **INC-4 byte-id** (`_wlver_50sta_10ep_uwl_par` vs the OFF uwl run): bit-identical
  (the byte-identical claim holds).
- **Suite:** 820/821 (1 pre-existing skip) incl. 4 new gates — the blocked/sparse
  fold vs sequential (≤1e-9/1e-6 + exact symmetry), the pinned-skip lifecycle
  (decided→skip, slip→resume), the grace-window expiry, and the OFF-inert check;
  + the config contract.
- **Deferred (staged cadence, on command):** the 150-sta 30-ep circR/med|f| A/B
  (INC-1/INC-2 quality parity), the `_lane_ff` force-fire re-run (INC-2 post-slip
  latency), and the 305-sta ≤90 s/ep closer (§5.5).

## Fixtures rescued
`_wlver_50sta_15ep_lane_ff.json` + `_ff.json` pointed at a dead-session `%TEMP%`
RINEX (`KRGG_slip.rnx`); copied to `data/_lf1_fixtures/KRGG_slip.rnx` and both
configs re-pointed there (the INC-2 force-fire gate depends on it).

## Renamed exe
`build/x64-Release/gnss_processor_lf1_run.exe` (LF-1); `gnss_processor_lf1_base.exe`
= the frozen pre-card baseline (md5 1E4E3B1C…, do not overwrite).
