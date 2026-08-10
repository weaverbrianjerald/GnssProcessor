# SPEC — receiver code-bias carry (`network.carry_rx_code_bias`)

2026-07-31.  The "hardware biases constrained as we thought they were" card,
receiver half.  Investigation complete; build NOT started (one-card-per-session
rule — this spec pins every structural fact so the build session starts cold).

## 1. Finding — why this is a build, not a config flip

- **Satellite CB is ALREADY hardware-stiff**: the shared border carries sat
  CB via D2 CarryPrior + C1 dense cov (information-form time update,
  `sat_code_bias_q = 1e-10`), measured step response τ ≈ 3.8 h
  (NetworkSdEwlAr.hpp §invariant-#1 note; the day-129 pivot-CB ratchet).
  Half of the user's ask is already true in every run.
- **Receiver CB has NO cross-epoch channel on the network path.**
  `filter.process_noise.code_bias_q` (1e-4) appears NOWHERE in
  src/core/filter/ or src/core/app/ — a phantom knob on this path (the
  ionosphere_q pattern).  The station-local network solve re-estimates rx-CB
  fresh each epoch (initial_std 10 m class); only the station filter's
  linearization point x_lin gives soft continuity.  The v3 de-mean hands the
  per-station iono level lump to the rx-CB states each epoch — with no carry
  they cannot accumulate it.  This card gives that lump its rightful owner.

## 2. Structural facts pinned (verified in source, 2026-07-31)

- Enumeration: `st.sm->getCodeBiasStates()` (used by the GLONASS IFB anchor
  at ApplicationNetworkAssembly.cpp:~2085).
- Local-carry inject region (ambiguity + ZWD-B9) is gated
  `if (carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on))`
  (ApplicationNetworkAssembly.cpp:~2147) — carry_warm = pl_on && … ⇒ the
  whole region is INERT without phase_leveling (the carry_zwd loader warning
  says so for ZWD; same trap).  **The rx-CB block must NOT inherit this
  gate** or it is dead on the carry_invariant/no-leveling lineage.
- Required restructure (byte-safe): hoist
  `const bool legacy_local_inject = carry_warm && (…as today…);`
  change the bracket to `if (legacy_local_inject || rx_cb_on)`, wrap the
  EXISTING interior (amb loop + zwd block) in `if (legacy_local_inject)`,
  add the rx-CB record+inject as a sibling `if (rx_cb_on)`, and leave the
  shared `inj` fold (R.carry_cols/dNrr/dur, ~:2269) untouched.  OFF ⇒
  legacy_local_inject ≡ old predicate ⇒ byte-identical.
- Harvest: mirror the ZWD loop in NetworkRunContext.cpp:~1451
  (`est.reconstructStation` + `conditionalLocalVarianceDiag` /
  `harvestCoupling` under carry_cpl_cond, stash LocalCarry, gap-prune) —
  but in an `if (rx_cb_on)` loop with its own map
  `ctx.carry.rx_cb_carry : map<string(STATION|CBKEY), LocalCarry>`, NOT
  inside any pl gate.  Record struct `PlCb {col, x_lin, key}` vector in
  StationAssembly (mirror PlZwd; plural per station).
- Value/var: value_abs = x_lin(cb_idx) + xr(col); posterior var per column.
  Inject: var_eff = var + q·gap; diagonal (λ, ζ) row into `inj`, or
  cb.buildCplRow under carry_cpl_cond (CouplingRowBuilder already takes q_cb).

## 3. Design decisions (pre-made)

- Knobs: `network.carry_rx_code_bias` (bool, default false — invariant #6)
  + `network.rx_cb_carry_q_m2s` (default 1e-9: σ ≈ 3 mm over 3 h ≈ hardware
  class; the LS-VCE monitor is the eventual calibrator, not sweeps).
- Datum guard: REQUIRE `code_datum_time_linked` (frame continuity is what
  makes a carried rx-CB meaningful across epochs; loader force-off + WARN
  otherwise).  A code-datum re-target/handover must DROP the carries
  (mirror: datum_changed signal) — else the carried lump fights the new frame.
- Invariant #1 response (wired from day one): inject-time innovation gate
  |value_abs − x_lin| > k·√(var_eff + guard²), k = 10 ⇒ drop entry + count
  (`rx_cb_innov_drops`); gap > 900 s ⇒ prune (housekeeping).  Receiver
  resets/firmware jumps are the event class; station-level anomalies are
  additionally covered by the iono-deweight gate on this lineage.
- Quasi-null discipline (the v3 lesson — identify FIRST): rx-CB ↔ iono level
  ↔ shdev per-station mean.  Guard = arm ONLY together with
  `rep_error_demean` (loader WARN otherwise): the de-mean keeps the deviation
  carry out of the level channel, the rx-CB carry accumulates it, the SH
  field takes the sky.  Watch a00 SUM as the safety metric (clean band
  ≤ ~40 TECU) exactly as in the v3/v3k gates.
- Counters + armed-only run-end line (rx_cb: carried n, injected/ep,
  innov drops, gap prunes).  Trace: none new (network_solution.csv already
  carries CB columns for drift grading).

## 4. Gates (the v3k template)

1. Unit: inject-decision helper truth table; gap/innovation gate; harvest
   stash round-trip (pure parts extracted to NetworkRunTypes).
2. Suite green Debug+Release; OFF byte-identity (knob absent vs false) —
   MUST include the gate-restructure check: legacy lineage artifacts
   byte-identical pre/post edit (the 2147 predicate hoist is the risk).
3. ON smoke: counters live; injected rows ≈ n_sta × n_CB-states.
4. Field: 3 h `combined_v3k + carry_rx_code_bias` vs the on-disk
   combined_v3k control.  PASS = a00 SUM in clean band AND per-sat/station
   CB drift (network_solution.csv, de-medianed; total_IF for the invariant
   read) tightens materially vs control.  Then look at formal DD σ / DD
  step / NL circR tail — the point of the whole exercise.

## 5. Explicitly out of scope

- Tightening `filter.process_noise.code_bias_q` (phantom on this path).
- Sat-clock GM treatment (calibration-first: re-measure Hadamard p on the
  quiet substrate AFTER this card; three standing falsifications apply).
- Sat-CB changes (already stiff; excursion/disc responses exist).
