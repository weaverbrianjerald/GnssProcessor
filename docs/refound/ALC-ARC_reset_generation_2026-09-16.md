# ALC-ARC: ambiguity-window reset generations

**STATUS**

- **id:** ALC-ARC
- **topic:** consumer
- **status:** closed
- **summary:** Reset-gap fix landed: the level-check window restarts on explicit ambiguity generations instead of a post-update variance heuristic; 82 focused tests pass, ALC-OFF products byte-identical, no accuracy claim.
- **source:** [ALC](ALC_amb_level_check_2026-09-15.md), September 16 code review
- **knobs / posture:** existing `processing.amb_level_check`; no new tuning or promotion
- **dates in the text:** 2026-09-16
- **hashes / run dirs:** `runs/_alc_arc_20260916/`; OFF products SHA256 `b05f26b1cd0540dbdb3a2601a843e9f88f123aa90281515bc57ddfc89725e090`

**Outcome.** The level check runs after the measurement update, so a reset whose variance the update had already shrunk could leave old-arc samples in the window. Generations owned by `StateManager` now restart the window on every lifecycle event; eight new regressions pass and the ALC-OFF 600 s smoke is byte-identical. The full Debug suite and the six-hour campaigns were deferred; [ALC2](ALC2_restart_policy_adoption_2026-09-19.md) section 3 closed the six-hour validation as a null result (field: 38 fewer checks, all non-trips, position untouched). Since 2026-09-20 the level check is off in the working bases ([PBH](PBH_phase_bias_hold_2026-09-19.md) section 8).

## Scope and pre-registered checks

The ALC check runs after the measurement update. A reset before that update can
have its variance reduced below `ambiguity_std²` before ALC inspects it. Slip and
KF outlier reset paths do not all notify ALC explicitly. A per-epoch disturbance
flag can also disappear during an epoch with no committed update or no usable
dual-frequency pair. The existing arithmetic test calls `restart()` directly and
does not exercise these engine paths.

Track nonzero, monotonically assigned ambiguity generations in `StateManager`.
Birth, reset/reseed request, explicit discontinuity, restore and rename receive a
new generation; removing and recreating a key cannot reuse its old generation.
Ordinary filter updates preserve it. ALC associates its samples with both selected
signal generations and restarts before sampling when either changes. Existing
explicit satellite notifications clear the window immediately, including when the
epoch cannot produce a sample. This remains a whole-satellite, two-frequency check.

The generation metadata changes neither state values nor covariance. Direct KF
recovery writes and explicit slip repairs must notify the metadata owner. The
existing ALC variance check may remain only as a conservative readiness guard;
it cannot establish that an arc is unchanged or that a state has converged.
ALC windows remain cold across process restart; snapshot format is unchanged.

Validation:

1. Exercise the actual engine sampling seam after reset, measurement-model reseed,
   and a KF update that reduces variance below the old threshold. No old-window
   check may occur; the first legal check needs a full window of new-arc samples.
2. Cover reset across a missing/deferred sample, remove/rebirth, selected-signal
   changes, ordinary-update continuity and explicit translation notifications.
3. Run focused regressions and the Debug unit suite; build Release with VS 2026.
4. Compare ALC-OFF outputs before/after on matched Release smoke configurations.
5. Run the corrected ALC sim arm, then field arm on the existing six-hour posture.
   Require identical producer products; report consumer changes and restart/check
   counts without changing thresholds to recover a score. No golden promotion.

## Results

Following the user's usage-limit report, this implementation uses a narrower validation pass.
Validation is narrowed to the focused Release regressions, documentation/config
checks and a matched ALC-OFF smoke. The full Debug suite and six-hour sim/field
campaigns above are deferred; no positioning-accuracy or promotion claim will be
made from the focused tests.

- VS 2026 Release build: PASS (310 s).
- Focused Release tests: 82/82 PASS across eight suites, including eight new
  `AmbLevelArcTest` regressions. The main regression uses the real measurement
  builder, pending reseed and Kalman update, verifies variance is below the old
  reset threshold, then verifies no test can mix old/new arc samples.
- Covered: delayed sampling, missing paired code, pending reseed without variance
  inflation, remove/rebirth, selected-signal change, ordinary-update continuity,
  immediate explicit restart, nonzero versus zero translation.
- Documentation index and golden-config verification: PASS; `git diff --check`
  clean. No loader default, golden configuration or promotion ledger changed.
- Frozen-source seam verifier: 22 matches / 8 failures. Six are the previously
  documented failures. The two additional diffs are intentional metadata wiring
  in `detectAndRepairSlips` and history clearing in `initStateManagerAndApriori`;
  this verifier tests equality with the original seam extraction, not behavior.
- ALC-OFF 600 s smoke comparison: PASS. Both runs completed; `products.txt` is
  byte-identical (289,991 bytes, SHA256 above). Both saved snapshot files differ
  only in the wall-clock write timestamp on line 2; all estimated state records
  and the configuration fingerprint are identical. Comparison details are in
  `off_comparison.json`; raw snapshot hashes intentionally retain the timestamp.

Files: generation ownership is in `StateManager`; direct recovery notifications
are in `KalmanFilter`, `RealTimeEngineSlipQc` and `SoftLliBridge`. ALC compares both
selected generations in `RealTimeEngineEstimate`. Reinitializing an engine clears
its ALC history together with replacing the state manager. No snapshot-format
change is made. Tests are in `tests/test_amb_level_arc.cpp`.

Validation artifacts: `runs/_alc_arc_20260916/`.

The reset-gap implementation is closed. Full Debug and six-hour sim/field accuracy
validation remain deferred; the focused evidence does not establish an accuracy
gain or authorize a configuration promotion.
