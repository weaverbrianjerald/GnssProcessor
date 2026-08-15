# SPEC — receiver code-bias carry (`network.carry_rx_code_bias`)

2026-07-31. The "hardware biases constrained as we thought they were" card, receiver half. **Status: BUILT AND FIELD-VALIDATED the same day (after `dc181b2`) — see §6.** (This header previously read "build NOT started"; corrected 2026-08-15.)

## 1. Finding — why this is a build, not a config flip

- **Satellite CB is ALREADY hardware-stiff**: the shared border carries sat CB via D2 CarryPrior + C1 dense cov (information-form time update, `sat_code_bias_q = 1e-10`), measured step response τ ≈ 3.8 h (NetworkSdEwlAr.hpp §invariant-#1 note; the day-129 pivot-CB ratchet). Half of the user's ask is already true in every run.
- **Receiver CB has NO cross-epoch channel on the network path.** `filter.process_noise.code_bias_q` (1e-4) appears NOWHERE in src/core/filter/ or src/core/app/ — a phantom knob on this path (the ionosphere_q pattern). The station-local network solve re-estimates rx-CB fresh each epoch (initial_std 10 m class); only the station filter's linearization point x_lin gives soft continuity. The v3 de-mean hands the per-station iono level lump to the rx-CB states each epoch — with no carry they cannot accumulate it. This card gives that lump its rightful owner.

## 2. Structural facts pinned (verified in source, 2026-07-31)

*(Anchors re-grepped 2026-08-15.)*

- Enumeration: `st.sm->getCodeBiasStates()` — the GLONASS IFB anchor used it at `ApplicationNetworkAssembly.cpp:~2085` when this was written; the live enumeration sites are `:160`, `:2461`, `:2720`.
- ⚠ **There are TWO nested `carry_warm` gates on this path, not one** (corrected 2026-08-15 — missing the second cost a full field run; §6 trap 1). (a) The local-carry inject region (ambiguity + ZWD-B9) is gated `if (carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on))` — `ApplicationNetworkAssembly.cpp:~2147` as originally written, now `legacy_local_inject` at `:2534-2535` after the hoist. (b) The SERIAL CONSUME `if (carry_warm && !R.carry_cols.empty())` (now `:3017`) had to be hoisted the same way. `carry_warm = pl_on && …` ⇒ both are INERT without phase_leveling (the carry_zwd loader warning says so for ZWD; same trap). **The rx-CB block must NOT inherit either gate** or it is dead on the carry_invariant/no-leveling lineage. As built, (b) reads `(carry_warm || rx_cb_on || rx_cb_datum)`.
- Required restructure (byte-safe): hoist `const bool legacy_local_inject = carry_warm && (…as today…);`, change the bracket to `if (legacy_local_inject || rx_cb_on)`, wrap the EXISTING interior (amb loop + zwd block) in `if (legacy_local_inject)`, add the rx-CB record+inject as a sibling `if (rx_cb_on)`, and leave the shared `inj` fold (R.carry_cols/dNrr/dur, ~:2269) untouched. OFF ⇒ legacy_local_inject ≡ old predicate ⇒ byte-identical.
- Harvest: mirror the ZWD loop in `NetworkRunContext.cpp:~1451` (`est.reconstructStation` + `conditionalLocalVarianceDiag` / `harvestCoupling` under carry_cpl_cond, stash LocalCarry, gap-prune) — but in an `if (rx_cb_on)` loop with its own map `ctx.carry.rx_cb_carry : map<string(STATION|CBKEY), LocalCarry>`, NOT inside any pl gate. Record struct `PlCb {col, x_lin, key}` vector in StationAssembly (mirror PlZwd; plural per station).
- Value/var: value_abs = x_lin(cb_idx) + xr(col); posterior var per column. Inject: var_eff = var + q·gap; diagonal (λ, ζ) row into `inj`, or cb.buildCplRow under carry_cpl_cond (CouplingRowBuilder already takes q_cb).

## 3. Design decisions (pre-made)

- Knobs: `network.carry_rx_code_bias` (bool, default false — invariant #6) + **`network.rx_cb_carry_q`** (double, default 1e-9: σ ≈ 3 mm over 3 h ≈ hardware class; the LS-VCE monitor is the eventual calibrator, not sweeps). ⚠ **The shipped key is `rx_cb_carry_q`, NOT `network.rx_cb_carry_q_m2s`** — this spec said `_m2s` until 2026-08-15 and a config copying that name is silently ignored. `include/config/GnssConfig.hpp:5894` (`network_rx_cb_carry_q_{1e-9}`), getter `:2023`, parse `GnssConfigLoadNetwork.cpp:2816`.
- Datum guard: REQUIRE `code_datum_time_linked` (frame continuity is what makes a carried rx-CB meaningful across epochs; loader force-off + WARN otherwise). A code-datum re-target/handover must DROP the carries (mirror: datum_changed signal) — else the carried lump fights the new frame. ⚠ **Loader parse-order trap** (paid for): `code_datum_time_linked` parses at `GnssConfigLoadNetwork.cpp:~2413`, i.e. AFTER this knob, so the datum guard must live in the POST-PARSE validation block (`:7276-7295`). Put in the parse block, the first smoke silently force-disarmed the knob.
- Invariant #1 response (wired from day one): inject-time innovation gate |value_abs − x_lin| > k·√(var_eff + guard²), k = 10 ⇒ drop entry + count (`rx_cb_innov_drops`); gap > 900 s ⇒ prune (housekeeping). Receiver resets/firmware jumps are the event class; station-level anomalies are additionally covered by the iono-deweight gate on this lineage.
- Quasi-null discipline (the v3 lesson — identify FIRST): rx-CB ↔ iono level ↔ shdev per-station mean. Guard = arm ONLY together with `rep_error_demean` (loader WARN otherwise): the de-mean keeps the deviation carry out of the level channel, the rx-CB carry accumulates it, the SH field takes the sky. Watch a00 SUM as the safety metric (clean band ≤ ~40 TECU) exactly as in the v3/v3k gates.
- Counters + armed-only run-end line (rx_cb: carried n, injected/ep, innov drops, gap prunes). Trace: none new (network_solution.csv already carries CB columns for drift grading).

## 4. Gates (the v3k template)

1. Unit: inject-decision helper truth table; gap/innovation gate; harvest stash round-trip (pure parts extracted to NetworkRunTypes).
2. Suite green Debug+Release; OFF byte-identity (knob absent vs false) — MUST include the gate-restructure check: legacy lineage artifacts byte-identical pre/post edit (the 2147 predicate hoist is the risk).
3. ON smoke: counters live; injected rows ≈ n_sta × n_CB-states.
4. Field: 3 h `combined_v3k + carry_rx_code_bias` vs the on-disk combined_v3k control. PASS = a00 SUM in clean band AND per-sat/station CB drift (network_solution.csv, de-medianed; total_IF for the invariant read) tightens materially vs control. Then look at formal DD σ / DD step / NL circR tail — the point of the whole exercise.

## 5. Explicitly out of scope

- Tightening `filter.process_noise.code_bias_q` (phantom on this path).
- Sat-clock GM treatment (calibration-first: re-measure Hadamard p on the quiet substrate AFTER this card; three standing falsifications apply).
- Sat-CB changes (already stiff; excursion/disc responses exist).

## 6. As built + 3 h field verdict (2026-07-31, after `dc181b2`)

Built and field-validated the same day it was specced. Where the code is (re-grepped 2026-08-15):

| concern | site |
|---|---|
| knobs + post-parse validation (datum guard, q ≥ 0, `rep_error_demean` WARN) | `src/core/config/GnssConfigLoadNetwork.cpp:2811-2816` (parse), `:7276-7295` (validate) |
| harvest + stash + prune | `src/core/app/NetworkRunContext.cpp:590` (RXCB-DATUM gauge census `:2542`; conditional fingerprint append `:2563`) |
| inject | `src/core/app/ApplicationNetworkAssembly.cpp:2704` (knob aliases `:1895`, hoisted gate `:2530`, serial consume `:3007`) |
| run-end telemetry | `src/core/filter/NetworkTeardown.cpp:2128,2133` |
| carried state | `include/app/NetworkRunContext.hpp:98,918`; `include/filter/NetworkRunTypes.hpp:496,869` |
| gates | `tests/test_rx_cb_carry.cpp` |

**FIELD VERDICT — 3 h, ALL GATES PASS.** Arm `runs/rxcb_3h` vs control `runs/iono2attr_combined_v3k` (the §4 gate-4 design, run as specced): **a00 SUM 25–33.6 TECU** against the control's **25–37.4** — inside the clean band and slightly tighter; **14,636 priors folded / 17,975 harvests / 50 carried steady**; OFF byte-identical on BOTH lineages. (These runs resolve under `D:\GnssProcessor_archive\2026-08-12\runs\`.)

### Two traps paid for

1. ⚠ **The second `carry_warm` gate (§2b) was missed on the first build, and it cost a full field run.** The first ON arm produced **LIVE counters (14,266 "folded") with artifacts BYTE-IDENTICAL to control** — standing invariant #4's inverted signature: a clean counter over dead wiring. The serial consume `if (carry_warm && !R.carry_cols.empty())` still gated the fold even though the inject region had been hoisted. **Check both edges of the gate, not just the one the spec named.**
2. ⚠ **The loader parse-order trap** (§3): the datum guard in the parse block force-disarmed the knob silently on the first smoke, because `code_datum_time_linked` had not been parsed yet.

Per the no-promotion directive the knob stays **default-OFF** with a SCOPED ledger row.
