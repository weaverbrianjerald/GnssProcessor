# DOSSIER — Consumer coasting, staleness horizons, and the C1 clock-rate path (DOY-129 SH-gate era)

**Scope note.** Read-only audit of `C:\GnssProcessor` while `gnss_processor_p2chain.exe` was live in `runs/day129_guards/leg01/`. Every claim below carries an anchor read by at least one agent; claims a skeptic corrected are marked and carry the correction, not the original.

**Premise correction up front.** The orchestrator's briefing asserted that `emit_clock_rate` / `clock_rate_est` / `T5_clock_rate` appear nowhere in tree or history. **That is false and was independently refuted by five agents.** The C1 clock-rate producer is implemented, committed (introduced in `89fe588`, Jul-16 2026; present at HEAD `dee2824`), compiled into `gnss_core`, snapshot-serialized, and unit-tested. See §E.

---

## A. C1 today — what the consumer would do with a nonzero c1

### A.1 The arithmetic (exact)

```
SsrCorrectionStore.cpp:653   dC_m = c0 + c1*dt + c2*dt*dt + hr_clock;   // metres
SsrCorrectionStore.cpp:654   return dC_m / Constants::SPEED_OF_LIGHT;   // -> seconds
```
- Units: `c0` m, `c1` m/s, `c2` m/s² (`include/products/SsrCorrectionStore.hpp:126,127,128`); divisor `299792458.0` (`include/common/Constants.hpp:31`). Metres in, seconds out.
- Return contract: "Clock correction in seconds; ADD to broadcast clock bias" (`include/products/SsrCorrectionStore.hpp:461`; declaration `:463`).
- **Exactly one production caller**: `src/core/corrections/GnssCorrections.cpp:343-344` `clock_corr += SsrCorrectionStore::applyClockToBias(ssr->c0, ssr->c1, ssr->c2, ssr->hr_clock, dt);`. The other three callers are tests (`tests/test_realtime_seams.cpp:233`, `tests/test_rtcm.cpp:528,535`). A second `ssr->c0,c1,c2,hr_clock` occurrence at `GnssCorrections.cpp:264` is a trace-format argument list inside a `should_log(trace)` guard — it formats, it does not apply.

### A.2 dt base and sign

- `GnssCorrections.cpp:341-342`: `dt = continuousGpsSeconds(time) - continuousGpsSeconds(ssr->epoch_time)` — **positive forward** from the SSR *content* epoch. `time` is the function parameter (`GnssCorrections.cpp:135`), never reassigned.
- The dt base is per-branch in `getOrbitClock`: combined branch sets `out.epoch_time = out.orbit_epoch_time = combined->epoch_time` (`SsrCorrectionStore.cpp:158,159`); separate branch sets `out.epoch_time = rec_copy.clock->epoch_time` (`:180`) with the in-code justification at `:177-179` that the clock polynomial must use the CLOCK epoch "its dt is c1/c2-sensitive". Branches are mutually exclusive (`else` at `:163`).
- **Query time is EMISSION time, not obs epoch**: `src/core/model/ModelGeometry.cpp:495` (seed) and `:552` (final) pass `emission_time`, built at `:487-488` as `obs.time` minus time-of-flight (~0.07 s), refined at `:525-526`. So dt is ~70 ms smaller than the obs-epoch age. Test pins: `tests/test_rtcm.cpp:527-529` (`dt=5.0`, expected `(0.03 + 1e-3*dt)/c`), `tests/test_realtime_seams.cpp:233` (`/*dt=*/0.0`, "query epoch == SSR epoch").
- **Not the same dt the URA/agek machinery uses**, which is obs-epoch-based: `src/core/measurement/MeasurementModel.cpp:1906-1908` and `:1949-1952` use `continuousGpsSeconds(obs.time) - continuousGpsSeconds(*ep)`.

### A.3 Sign consistency end to end — no trap

- Producer: `src/core/filter/NetworkEstimator.cpp:664` `s.clock_c0_m = x_s(c);` (raw state, unnegated) → `NetworkProductEmitter.cpp:240` `rec.c0 = s.clock_c0_m;` → writer `NetworkProductIO.cpp:543` → reader `:141`.
- Producer model: `src/core/measurement/MeasurementModel.cpp:1530` `jacobian(sc_idx) = -1.0;` and `:1531` `sat_clock_term = -x(sc_idx);`, summed additively at `:1587`.
- Consumer model: `src/core/model/PppModel.cpp:1478-1479` `return geom.range - (geom.sat_clock * Constants::SPEED_OF_LIGHT) - ...`.
- So `+c0` on the wire → `+c0/c` seconds → `−c0` metres in modeled range, algebraically identical to the producer's `modeled − x`. **No sign flip required for a coasting extension; a rate must be m/s with `c1>0` meaning the correction grows more positive forward in time.**
- Do NOT generalize the sign: orbit deltas are *subtracted* (`SsrCorrectionStore.cpp:647-648` `return brdc_pos - delta_xyz;`), clock is added. RTCM convention.

### A.4 The gate ladder — four conditions, not three (skeptic-corrected)

`GnssCorrections::getSatelliteClockCorrection` spans `GnssCorrections.cpp:135-388`, returns at `:148, :152, :382, :387`. The polynomial at `:343` is reached only under **four** conditions:

1. **Freshness.** `apply_window = clockHoldWindow(ssr_max_age, clk_hold)` (`:182-186`) → `getOrbitClock(satellite, time, apply_window)`; the clock branch is gated by `freshEnough(rec_copy.clock->received_time, query_time, max_age_s)` (`SsrCorrectionStore.cpp:173`, predicate at `:26-41`, `return age <= max_age_s` at `:40`).
2. **`ssr->has_clock`** — the outer guard at `GnssCorrections.cpp:230`. A fresh orbit-only record passes freshness and never reaches the polynomial.
3. **`std::isfinite(clock_corr) && echo_ok`** (`:339`).
4. **`iodeMatches(brdc_iode, ssr->clock_iode)`** (`:340`); `iodeMatches` is permissive when either IODE `< 0` (`include/products/SsrCorrectionStore.hpp:700-703`).

Failure of the AND falls to a **trace-only** log at `:367-371` — no state change, no counter.

### A.5 Where c1 can be silently defeated

| Defeat point | Anchor | Effect |
|---|---|---|
| Absent field 4 on the wire → `c1` keeps its `{0.0}` member init, **no flag, no counter** | `NetworkProductIO.cpp:149-153`; `include/products/NetworkProductEmitter.hpp:83` | absent, explicit-0, and producer-suppressed rate are indistinguishable to C++ |
| Positional scan stops at first `=` field | `NetworkProductIO.cpp:142-153` | key=value tokens never mis-parse as c1 |
| `c2` hard-zeroed on the network path | `NetworkProductEmitter.cpp:242` | `c2*dt²` is structurally 0 |
| `hr_clock` only ever from an RTCM high-rate record | `SsrCorrectionStore.cpp:114, 209-210` | never set by product replay |
| Producer `clock_model` wins over `emit_clock_rate` | `NetworkRunContext.cpp:106`; `GnssConfigLoadNetwork.cpp:1698-1701` | emit_clock_rate silently inert |
| Producer cadence guard | `GnssConfigLoadNetwork.cpp:1707-1723` | if `gnss_rate_s × decimation > clock_model_max_gap_s`, every epoch ZOHs; one info line only |
| R11 staleness ZOH at emit | `NetworkSolPrep.cpp:213-223, 233-241` | c1 forced to 0 past `clk_model_max_gap`; `T5_clock_rate`/`c1_zoh_suppress` events |
| **The freshness gate itself** | `GnssCorrections.cpp:186-196` | past 30 s the record is rejected wholesale — c1 is never evaluated |

`has_c1` presence **does** survive on the Python side (`pyviz/gnssviz/product_file.py:218-219, 240-241, 248`; counter at `pyviz/gnssviz/viewers/network_product_file.py:255-258`; stale-C1 honesty check at `pyviz/gnssviz/viewers/network_discontinuities.py:305-313, 563`). The C++ estimator discards the bit at the reader. Note: under `emit_clock_disc` the writer emits c1 unconditionally (`NetworkProductIO.cpp:544-546`), collapsing "suppressed rate" and "no rate channel" to a literal `,0` even for pyviz.

### A.6 Verdict: **the c1 path is structurally live and operationally dead.**

- Structurally complete: `NetworkProductEmitter.cpp:241` → writer `NetworkProductIO.cpp:545/547` → reader `:149-151` → replay `emitToStore` at `:661` (and RTA-4 re-emit `:710`) → `SsrCorrectionStore.cpp:175` → `applyClockToBias :653`. Live closed loop uses the same emitter (`NetworkEmitStore.cpp:290`).
- Operationally dead tonight: `network.clock_model` default false (`include/config/GnssConfig.hpp:4707`), `network.emit_clock_rate` default false (`:4764`, loader `GnssConfigLoadNetwork.cpp:1696-1697`), neither present in `runs/day129_guards/leg01/leg.json`. **Measured: 100 % of C records are 3-field** (§F).
- **Even armed, c1 does nothing across a withhold era**, because the record is rejected on AGE at 30 s before the polynomial runs (`GnssCorrections.cpp:182-196`). The default coasting horizon is one epoch. A rate term is not the lever for a multi-hour gap; the freshness window is.

---

## B. The staleness horizon

`freshEnough` (`src/core/products/SsrCorrectionStore.cpp:26-41`) is three-region: record-zero → false (`:27`); **query-zero → true, unbounded bypass** (`:28`; `GnssTime` defaults `{0,0.0}`, `include/math/TimeUtils.hpp:14-16`); far-future `age < -max_age` → false (`:38`); mild negative → true (`:39`); else `age <= max_age_s` (`:40`, **inclusive**). It gates on `received_time`, not `epoch_time` — for network products the two coincide (`NetworkProductEmitter.cpp:243-244`). A second, independently-implemented ladder exists: `classifyClockAge` (`include/products/SsrCorrectionStore.hpp:522-529`), used at `GnssCorrections.cpp:351` and `MeasurementModel.cpp:1876, 1975`.

| Channel | Age-checked? | Knob(s) | Default | At expiry | Observable? |
|---|---|---|---|---|---|
| **SSR clock** | Yes, always | `processing.ssr_max_age_s` × `clockHoldWindow(·, ssr_clock_stale_hold_factor)` (`GnssCorrections.cpp:182-186`; `SsrCorrectionStore.hpp:502-505`) | 30.0 s (`GnssConfig.hpp:4566`, `GnssConfigLoad.cpp:407-408`); factor 0.0 = no widening (`GnssConfig.hpp:4571`, `GnssConfigLoad.cpp:454-455`) → **window = 30 s** | `getOrbitClock` returns nullopt (`SsrCorrectionStore.cpp:213`); falls back to precise-CLK/broadcast baseline. **Not a drop** if a CLK covers the epoch | Per-sat per-epoch `log->warn` "REJECTED stale … falling back to broadcast-grade" (`GnssCorrections.cpp:191-194`) — only if a header epoch exists (`SsrCorrectionStore.cpp:473-480`) |
| **SSR orbit overlay** | Yes | raw `getSsrMaxAge()` — **never widened** (`ModelGeometry.cpp:939, 1278`) | 30 s | `ssr->has_orbit` guard fails (`:947, :1279`); precise SP3 or bare broadcast position survives | **Silent** — no reject log on either orbit site |
| **Combined (1060/1066)** | Yes | same window | 30 s | else-branch → separate orbit/clock (`SsrCorrectionStore.cpp:137/163`) | via clock warn only |
| **High-rate clock** | Yes, own test outside the if/else | same window | 30 s | `hr_clock` term absent (additive, not dt-scaled) (`SsrCorrectionStore.cpp:209-210`) | none |
| **URA** | Yes, same window as clock (deliberate, P1-3) | `clockHoldWindow` (`MeasurementModel.cpp:1943-1944`), lookup `:1958` | 30 s | `add_var == 0` — R untouched | none |
| **Code bias** | **Conditional** — un-aged overload when gate off (`SsrCorrectionStore.cpp:217-235`) | `processing.enable_ssr_bias_max_age`; limit = `ssr_bias_max_age_s × ssr_bias_stale_hold_factor` (`MeasurementModel.cpp:199-202, 246, 1563-1564`) | gate `false` (`GnssConfig.hpp:4568`, `GnssConfigLoad.cpp:414-415`); 30.0 × 2.0 = **60 s** when armed | armed: bias not applied. unarmed: **never expires** | RT freshness-flip info log (`RealTimeEngine.cpp:1790-1794`) only when gate armed |
| **Phase bias** | Same conditional (`SsrCorrectionStore.hpp:224`; aged twin `:293-311`) | same | same | armed: 60 s. unarmed: unbounded | RT flip info + ambiguity reset (`RealTimeEngine.cpp:1726-1737`) |
| **Clock disc counter** | **No aged overload exists** | — | — | served from a frozen record forever | none |
| **Code-bias disc counter** | Both overloads exist (`SsrCorrectionStore.hpp:303`) but RT reads the un-aged one (`RealTimeEngine.cpp:1767, 1843`) | — | — | unbounded | none |
| **Slant iono** (in-process only) | Yes, hard-coded | none — `iono_max_age = (dt>0) ? 0.5*dt : 30.0` (`RealTimeEngine.cpp:2097`, consumed `:2112`) | 0.5 × obs spacing | not applied | none |
| **`getSsrEpoch` / `getPhaseBiasTime` / `getSsrIod` / `censusSummary`** | **No** (`SsrCorrectionStore.cpp:473, 482, 502, 587`) | — | — | keep reporting pre-gap state forever | n/a |

**No eviction exists.** `clear()` (`SsrCorrectionStore.cpp:610-617`, removal at `:612`) is the only removal path; a whole-file grep for erase/prune/evict/ttl/expire hits only `:612, :615, :616`. Its only non-test callers touch `LiveEphemerisStore`, not this store. **Records are frozen-but-live indefinitely.**

### Asymmetries that matter

1. **Clock 30 s vs bias 60 s (armed).** Both stamped with the same `received_time` (`NetworkProductEmitter.cpp:243, 296, 305, 361`), so ages advance in lockstep — producing a deterministic 30-second window in which the producer's phase/code-bias realization is applied at **full weight** (`MeasurementModel.cpp:1560`) on top of a clock from a *different* frame. Bias and clock are two halves of one S-basis realization; splitting them is a frame violation.
2. **Clock 30 s vs bias ∞ (unarmed).** In a hand-written config that sets `realtime.network_products_path` but leaves `processing.network_consumer.enable` false, `ApplicationRealtime.cpp:370` injects the store anyway and `:383-386` defaults `consume_bias = true` — biases consumed with the gate never armed. Unbounded stale biases across the entire era. The codebase names this failure: `GnssConfigLoadNetwork.cpp:3533-3536` "silently applied unboundedly stale biases across producer withhold gaps".
3. **Clock widenable, orbit not.** Only the clock passes `clockHoldWindow`; `ModelGeometry.cpp:939, 1278` pass the raw 30 s. Arming `ssr_clock_stale_hold_factor` alone gives a held clock over an expired orbit. **Moot on the network stream** (no orbit records are ever pushed — see §C), latent the moment an orbit-bearing product exists.
4. **R inflation dies with the record, not before it.** `MeasurementModel.cpp:1935` gates on `(networkConsumeUra() || clk_hold > 1.0)`; `network.consume_ura` defaults false (`GnssConfigLoadNetwork.cpp:1786-1787`) and `clk_hold` defaults 0.0 → **the entire inflation block is inert for the whole run**. There is no "stale clock + inflated R" state by default. The RTA-9(b) StaleHeld rung (`GnssCorrections.cpp:350-358`; `MeasurementModel.cpp:1974-1983`, fallback `q² = 1.5e-7 m²/s³` at `:1979`) is built and unit-tested (`tests/test_realtime_seams.cpp:637-640, 647-650`) but unreachable at factor 0. Note the brake is violent when reachable: `1.5e-7 × 7200³/3 ≈ 2.9e4 m²`, σ ≈ 170 m — annihilation, not graceful deweighting.
5. **Two consumers of the same file, different horizons.** Replay/closed-loop arms the bias gate (`GnssConfigLoadNetwork.cpp:3541-3543`, used by `ApplicationConsumerReplay.cpp:216` and `ApplicationNetworkSetup.cpp:1093`); standalone RT profile arms it too (`:3577-3585`, sole caller `ApplicationRealtime.cpp:343` under `ncc.enable && !path.empty()`, `:337`). A hand-written config that skips both does not. 60 s vs ∞ on the same products.txt.
6. **The one age-proportional weighting term is dead on this stream.** `agek` (`MeasurementModel.cpp:1903`) comes from the S contract; tonight's file has no S line (§F), so `agek = 0` and a 29-s-old correction weighs the same as a 0-s-old one.

---

## C. Coasting across a 2+ hour withheld era — epoch by epoch, DEFAULT config

### C.0 What the producer does

`NetworkEmitStore.cpp:120-139`: `if (nc.sh_health_gate && ctx.carry.sh_gate_engaged) { ++ctx.counters.n_sh_gate_withheld; traces.emitEvent(SH_GATE/withhold …); [H heartbeat]; return; }` — the bare `return;` at `:139` precedes `emitToStore` (`:290`), the RTA-5 S re-broadcast (`:321`), and `product_writer->write(sol,…)` (`:354`), the only site that emits E/C/B/P/U/R/G/I (`NetworkProductIO.cpp:503, 517, 525, 543, 581, 587, 606`). Upstream, `NetworkSolPrep.cpp:140-141` returns an empty `NetworkSolPrepResult{}` before `est.toSolutionEpoch` at `:144`, so no solution object is built. Contract stated at `NetworkEmitStore.cpp:113-119`: "no store push, no product write, no closed-loop consume/iono supply, no n_emitted advance… The carry stash and every QC tier still run — emission-only by design."

**Empirically: 0 E/C/B/P/U records under any withheld epoch** (partition of the full file on H-state; §F).

### C.1 The network stream is clock-only

`emitToStore` (`NetworkProductEmitter.cpp:139-459`) pushes exactly four things: `pushClock :287`, `pushUra :298`, `pushCodeBias :355`, `pushPhaseBias :399`. `pushOrbit`/`pushCombined` have **no** non-test caller outside the RTCM decode path (`RealTimeEngine.cpp:513, 518, 519`). Corroborated in-tree: `tests/test_network_emit.cpp:81` and `tests/test_realtime_seams.cpp:180` both `EXPECT_FALSE(oc->has_orbit)` — "emitter pushes clock only (orbit from IGS)". So the **separate** branch of `getOrbitClock` is the only one taken on a file replay, and the dt base is always the clock record's content epoch.

Caveat (skeptic): the store is a process singleton (`RealTimeEngine.cpp:504`). In RT with an external RTCM stream under the default `corrections_accept = "all"` (`GnssConfigLoad.cpp:2519`; `include/realtime/RealTimeEngine.hpp:97`), 1060/1066 combined records enter the same store and the combined branch at `SsrCorrectionStore.cpp:137` **wins** over the injected network clock — which is precisely why `OrbitOnly` exists (`RealTimeEngine.cpp:500-503`). Not a property of the post-mode replay, but not a property of the binary either.

### C.2 The epoch-by-epoch story (30 s cadence, defaults, precise CLK loaded)

| Age | Clock | Code/phase bias (gate armed) | Bias (gate off) | Orbit | R | Net state |
|---|---|---|---|---|---|---|
| 0 s (last emit) | network c0 applied | applied | applied | SP3/precise | nominal | full network frame |
| **30 s** (1 missed epoch) | **still applied** — `age <= max_age` is inclusive (`SsrCorrectionStore.cpp:40`), and the query is at emission time ~70 ms earlier (`ModelGeometry.cpp:487-488`), giving ~29.93 s | applied | applied | SP3 | nominal | full frame, boundary |
| **60 s** (2 missed) | **REJECTED** → precise-CLK baseline (`GnssCorrections.cpp:160`), warn at `:191-194` | **still applied** (60 s limit inclusive) | applied | SP3 | **unchanged** | **MIXED: network bias on a CLK-frame clock, full weight** |
| **90 s → end of era** | precise-CLK | dropped | **still applied, forever** | SP3 | unchanged | SP3+CLK, or SP3+CLK+stale-bias |
| On resume | network c0 restored, silent step | reapplied | — | — | — | see C.4 |

### C.3 What "expiry" actually does — not a drop (skeptic-corrected)

`GnssCorrections.cpp:378` `*broadcast_grade = used_broadcast && !ssr_clock_applied;` and `used_broadcast` is set only at `:219-222` when `|clock_corr| < 1e-12` after `interpolateClock` (`:160`, MAX_WINDOW 3600 s at `:398`). Three outcomes:

- **(a) Precise CLK covers the epoch** — the DOY-129 post-mode case. `used_broadcast=false` ⇒ `broadcast_grade=false` ⇒ the phase gate at `PppModel.cpp:721-736` never fires (erases every `L*` observable when it does) and `RealTimeEngine.cpp:1223-1232` never fires (`drop_broadcast_grade` default false, `GnssConfigLoad.cpp:1218-1219`, `GnssConfigTypes.hpp:1354`). **The satellite keeps code AND phase at full weight and silently swaps clock datum from the network frame to the CLK-product frame.** No flag, no deweighting, no reset. This is the operative failure mode.
- **(b) No precise CLK, empty live store (post-processing)** — `clock_corr` stays 0.0, passes the `:375` validity test, returns "valid" with `broadcast_grade=false`, and `ModelGeometry.cpp:496-508` / `:553-559` convert `|sat_clk| < 1e-12` into `drop_reason = DropReason::NoProduct; return false;`. **Return 0.0 is the drop signal.**
- **(c) Real-time, broadcast baseline** — `broadcast_grade=true` ⇒ carrier phase stripped per satellite (`PppModel.cpp:721-729`), full drop only under `drop_broadcast_grade` (true in `config/realtime_ppp_rtk.json:150`, `config/realtime_igs_ntrip.json:195`).

Which one applies to DOY-129 is settled: the closed-loop consumer is `app_mode: "post"` (config/test_suite/_day129_guards_conv.json), `deriveClosedLoopConsumerConfig` copies the producer config verbatim (`GnssConfigLoadNetwork.cpp:3520-3521`), and `realtimeUsesLiveOrbitClockSource()` is false whenever `app_mode != RealTime` (`include/config/GnssConfig.hpp:4259-4263`), so `RealTimeEngine.cpp:171-173` **does** load precise SP3/CLK. ⇒ **case (a)**. (A comment at `ApplicationConsumerReplay.cpp:289-291` asserting "every satellite dropped" is wrong for this setup; it holds only under `brdc_rts` or true RealTime mode.)

### C.4 Mixed-state failure modes, enumerated

1. **Bias-without-clock (30–60 s, gate armed).** The most precise defect. Network phase/code biases applied at full weight against the a-priori CLK clock. The state-allocation coverage predicates use the same 60 s product (`MeasurementModel.cpp:199-203, 227-232`), so the consumer also keeps suppressing its own sat-bias states on the strength of a bias whose clock partner is gone.
2. **Bias-forever (gate off).** Same violation, unbounded. Reachable via `ApplicationRealtime.cpp:370, 383-386`.
3. **Clock-without-orbit.** Structurally impossible on the network stream (no orbit records). Latent for orbit-bearing streams the moment `ssr_clock_stale_hold_factor` is armed. The detector that would catch it — Card-7 orbit echo, `GnssCorrections.cpp:269-331`, verdict `NoAppliedOrbit` at `:311` → `echo_ok=false` at `:316` → clock withheld at `:339` — requires a stamped echo (`:285`) and `orbit_clock_max_skew_s > 0` (default 0.0, `GnssConfigLoad.cpp:745-746`). **Inert.**
4. **Resume step, unreset.** No clock-side analogue of the A5 bias freshness-flip detector exists. The only flip maps in the tree are `include/realtime/RealTimeEngine.hpp:468` (`ssr_phase_bias_applied_`) and `:475` (`ssr_code_bias_applied_`); the clock block (`RealTimeEngine.cpp:1876-1929`) reacts only to the producer-signalled `getClockDiscontinuity` (`SsrCorrectionStore.cpp:263-268`), which has no aged overload. `ssr_clock_applied` is a per-call local (`GnssCorrections.cpp:229`, consumed once at `:378`) — nothing persists epoch-to-epoch, so no flip can be detected. On a precise-CLK baseline the applied clock steps by the full SSR delta with phase retained and float ambiguities absorbing it.
   - Scope: all of this machinery is RealTimeEngine-only. `noteSsrDiscontinuity` has zero occurrences in `src/core/app`, so the *post-processing* consumer has neither the bias flip detector nor the clock disc reaction — a shared absence, not an asymmetry.
   - Cost of a gap, corrected: the phase-bias flip test requires `it != ssr_phase_bias_applied_.end()` (`RealTimeEngine.cpp:1730`), so a gap with no *prior* applied bias costs **one** reset storm, not two. The code-bias re-inflation half (`:1783-1796` → `:1820-1828`) fires only under `estimate_ifb` (guard `:1754`, default false at `GnssConfigLoad.cpp:786-787`), armed by the standalone profile (`GnssConfigLoadNetwork.cpp:3565-3566`) but **not** by the closed-loop derivation.
5. **RTA-3 generation-id datum break** (`RealTimeEngine.cpp:1636-1655`): resets every ambiguity and re-inflates every code-bias variance, age-independent. **Inert tonight** — `network.emit_generation_id` defaults false (`GnssConfig.hpp:4757`, `GnssConfigLoadNetwork.cpp:2924-2925`), the store returns nullopt when unstamped (`SsrCorrectionStore.cpp:514-517`), and the live file has zero I records and no S line. A producer restart would go unsignalled.
6. **Cold-start famine (this leg's dominant era).** The 239-epoch leading era precedes any emission — the store is *empty*, not stale. Nothing to coast from; every satellite is on the a-priori clock trivially. Different requirement class: hold-off / do-not-navigate, not coasting.

### C.5 One-sentence summary

> Across a withheld era at defaults, the consumer loses the network clock after one missed epoch, keeps the network biases for one more (or forever if the gate is unarmed), never inflates R, never drops a satellite, never resets a state, and never learns why — it silently changes correction frame and continues at full weight.

---

## D. The `withheld_sh` H-state

### D.1 What is emitted

Grammar: `H,<week>,<tow>,<status>,<sigma0>,<n_sats_published>,<withheld>` — **7 positional fields, no key=value, no optionals**. Header doc `include/products/NetworkProductIO.hpp:66-67` (positional semantics `:69-79`); writer is a single unconditional statement, `src/core/products/NetworkProductIO.cpp:448-452`, inside `writeEpochStatus` (`:439-453`, only guard `if (!out_) return;` at `:440`). Contrast the S-line writer immediately above it (`:411-430`), which is a stack of conditional appends — the H writer has none. Legend line `:490`. **Empirically: 535 H lines, field-count histogram `{7: 535}`, zero key=value tokens.**

**Four producer write sites, five states, not one-per-class:**
- `NetworkEmitStore.cpp:136` → `WithheldShGate` (state `:134`, `withheld="*"` `:135`), guard `:130` `if (emit_epoch_status && product_writer)`
- `NetworkEmitStore.cpp:351` → **Ok or Partial**, ternary `:337-339`, `sigma0 = sigma0_adapt` `:340`, `n_sats_published` `:341`; guard is a bare `if (emit_epoch_status)` at `:333` nested inside `if (product_writer)` at `:325`
- `NetworkSolveGlue.cpp:566` → `WithheldGate` (state `:564`)
- `NetworkSolveGlue.cpp:1220` → `WithheldSigma0` (state `:1217`, `sigma0` set `:1218`)

State enum: `include/products/SsrCorrectionStore.hpp:49` — six values incl. `Unknown`, whose default member init is `State state{State::Unknown}` at `:52`. `stateName` (`SsrCorrectionStore.cpp:531-541`) serializes **six** tokens including `"unknown"` at `:540`; it is the **writer's** formatter (called at `NetworkProductIO.cpp:449` and `RealTimeEngine.cpp:1675,1677`), not a reader fallback. The reader fallback is `return State::Unknown;` at `SsrCorrectionStore.cpp:528`. **So `unknown` is emittable by construction** — any future caller of the public `writeEpochStatus` (`NetworkProductIO.hpp:315`) that forgets `.state` emits it silently. All four current sites assign explicitly, so five tokens appear in practice.

**Dispositions with NO H line at all** (silent wire holes, byte-indistinguishable from producer death): `sync_skip` (`ApplicationNetworkAssembly.cpp:528, 537, 1090, 1759`), `datum_failed` (`NetworkSolveGlue.cpp:300-307` → SkipEpoch, no write), `solve_failed` (`:681-691`). Four of eight trace dispositions get an H.

**Information content of a withheld H:** one enum token (3-way cause discrimination: `withheld_sh` / `withheld_gate` / `withheld_sigma0`), `sigma0=0.0000`, `n_sats=0`, `"*"`. `sigma0` is 0 by **omission**, not unavailability — `sigma0_adapt` is a live in-scope parameter at `NetworkEmitStore.cpp:52` used 210 lines later at `:340`, and the SH_GATE trace event three lines earlier (`:122-126`) carries `sh_gate_last_a00`, `sh_gate_last_rms`, `sh_gate_oob_run` — none of which reaches the wire. The grammar overloads 0 as "unavailable" (`NetworkProductIO.cpp:444-445`; `SsrCorrectionStore.hpp:53`), so a consumer cannot distinguish "not measured" from "not written".

### D.2 What is parsed

- Reader branch `NetworkProductIO.cpp:212` (`else if (tag == "H")`), arity guard `:213` (`f.size() < 7` → `ok = false`), flushes the epoch under assembly `:220-225`, builds the status `:226-232`, sets `latest_status = st; pending_h = st; pending_h_valid = true;` `:233-235`.
- Attach to an E record only on **exact** week/tow match (`:127-135`, guard `:131-132`, assignment `:133`, `pending_h_valid = false` `:134`). A standalone (withheld) H attaches to nothing, by construction and by documentation (`NetworkProductIO.hpp:244-248`: "A standalone H … attaches to NO record … H lines never feed `replay`"). Unit-pinned at `tests/test_network_product_io.cpp:2519`.
- Token map: `SsrCorrectionStore.cpp:527` `if (token == "withheld_sh") return State::WithheldShGate;`.
- Tail: `NetworkProductTail::lastStatus()` (`NetworkProductIO.cpp:916-918`); `poll()` `:920-993`, `pollInto` `:995-1012`; `healthy()` `:914` stays true (producer always writes 7 fields).

### D.3 What is acted upon — **nothing.** Name the dead end.

**Exactly one non-test reader of the status exists in the whole tree.** Repo-wide grep for `getProducerStatus|setProducerStatus|producer_status_|NetworkProducerStatus|lastStatus`:
- store surface: setter `SsrCorrectionStore.cpp:543-546`, getter `:548-551`, member `include/products/SsrCorrectionStore.hpp:721`, reset in `clear()` `SsrCorrectionStore.cpp:614`
- one producer of the value: `ApplicationRealtime.cpp:640-646`, push at `:644`, keyed on heartbeat time so each H pushes once
- **one consumer: `RealTimeEngine.cpp:1671-1688`** — edge-triggered, `log->info` (Ok, `:1679`) / `log->warn` (non-Ok, `:1682-1685`), then `last_producer_status_ = pst->state;` (`:1686`). The block closes at `:1688` and `pst` goes out of scope. Nothing touches `R`, `state_manager_`, measurement usage, or any solution flag.

Three separate doc comments scope the gap out explicitly:
- `include/products/SsrCorrectionStore.hpp:43-44` — "Deeper integrity wiring (gating measurement usage on producer status) is out of scope here — RTF-9."
- `include/realtime/RealTimeEngine.hpp:490-491` — same sentence.
- `RealTimeEngine.cpp:1669-1670` — "Deeper integrity wiring — gating measurement usage or R-inflation on the producer status — is deliberately out of scope here (RTF-9)."

**A second, deader copy:** `NetworkProductRecord::epoch_status` (`NetworkProductIO.hpp:248`) is written at `NetworkProductIO.cpp:133` and read at **zero** sites outside tests (`tests/test_network_product_io.cpp:2511-2522, 2652`).

**Three compounding dead ends:**
1. **The batch/replay path never receives the status at all.** `ApplicationConsumerReplay.cpp` uses `NetworkProductReader::read` (`:152`) and `::replay` (`:317`), never `NetworkProductTail::lastStatus`; `read` returns only `{records, contract, ok}` (`NetworkProductIO.cpp:637-639`) and its `LineAssembler` is function-local (`:629`), so `latest_status` is destroyed at `:640`. `setProducerStatus` has no call site in `src/core/app` other than `ApplicationRealtime.cpp:644`. **The offline grading path — the one that would grade tonight's stream — is status-blind.** Its only signal is a run-end scalar `++n_no_products` (`ApplicationConsumerReplay.cpp:321`, reported `:408-414`), which cannot separate withhold from producer death.
2. **Default config emits no H at all.** `network_emit_epoch_status_record_{false}` (`GnssConfig.hpp:4698`; loader `GnssConfigLoadNetwork.cpp:1766-1768`), force-armed only by the service-mode bundle (`:3041-3042`). Tonight's leg arms it explicitly (`config/test_suite/_day129_guards_conv.json:2931`; `scripts/p2_make_guard_configs.py:90`).
3. **Latent: a v2-contract run that opens inside a withhold era writes NOTHING.** Under `network.serialize_sbasis_contract`, `ApplicationNetworkSetup.cpp:1059-1060` sets `contract_pending = true` and constructs **no** writer (and removes any stale file, `:1065-1071`); the deferred open is at `NetworkSolPrep.cpp:810-811, 823`, **670 lines after** the SH-gate early return at `:140-141`. All four H sites require a non-null `product_writer`. Tonight escapes only because `serialize_sbasis_contract` defaults false (`GnssConfigTypes.hpp:729`) and leg.json omits it — proven by `products.txt:1` = `# gnss_network_product v1` and by the eager construction at `ApplicationNetworkSetup.cpp:1075`. **In the configuration a real consumer needs (frame=/cbpin=/agek=), a leg that opens with a 239-epoch withhold produces an empty directory for two hours** — exactly the property RTA-10 exists to provide, lost precisely where it is needed. This leg's opening era has that shape.

### D.4 The missing link, precisely

There is no function of type `State → {reject | inflate R | hold ambiguities | flag solution}`, and no call to any such function from `MeasurementModel`, `GnssCorrections`, or `ModelGeometry`. Neither exists.

**Minimum viable link:** have the age-based reject path at `GnssCorrections.cpp:187-196` consult `ssrStore().getProducerStatus()` and, when the state is any `Withheld*` with a heartbeat **newer** than the stale record, emit "producer withholding (`withheld_sh`) since tow X" instead of "SSR stream stalled / falling back to broadcast-grade", plus a per-epoch solution-level integrity flag. Second (independent) fix: thread `LineAssembler::latest_status` out of `NetworkProductReader::read` into `setProducerStatus` so the replay path stops being blind — a few lines, no grammar change.

### D.5 The message that says the opposite of the truth (skeptic-narrowed)

`RealTimeEngine.cpp:861-892`: with `processing.enable_obs_ssr_sync` armed **and** a live orbit/clock source, the SSR horizon freezes (`:863-866`), obs are deferred (`:885`), and after `getObsSsrSyncMaxHoldS` the run latches `sync_outage_` (`:888`) and logs "obs↔SSR hold expired after {}s (**SSR stream stalled**)" (`:891-892`) while the producer is alive and heartbeating. That block never reads `getProducerStatus()`.

**But the claim as originally stated is over-scoped.** `enable_obs_ssr_sync` defaults **false** (`GnssConfig.hpp:4573`; `GnssConfigLoad.cpp:484-485`; armed only in `config/realtime_ppp_rtk.json:38` and `config/realtime_igs_ntrip.json:49`), and `realtimeUsesLiveOrbitClockSource()` is false for the closed-loop/product-replay consumer (`GnssConfig.hpp:4259-4263`; comment `RealTimeEngine.cpp:1136`) — so the symptom cannot occur on the path that would replay tonight's file. Where it *can* occur with H armed, the RTA-10 warn at `:1682-1685` naming `withheld_sh` fires **earlier** in the log (held epochs return at `:885` and never reach `:1672`; the heartbeat lands within one ~200 ms tail poll). Residual real defect: the `:891` wording should be conditioned on `getProducerStatus()`, and in the narrow case `emit_epoch_status_record=false` the misleading message is the *only* signal.

---

## E. Producer C1 status

### E.1 It exists. Nothing is gone.

`git grep` at HEAD `dee2824` finds the symbol family in **18–19 tracked files**; the introducing commit is `89fe588` "Network enhancements, WLs, parallelizations." (Thu Jul 16 16:44:57 2026), whose file set is code+config+snapshot+test together (`config/test_suite/_emit_c1_{on,off}.json`, `docs/HANDOFF_c1_clock_rate_producer.md`, `include/config/GnssConfig.hpp`, `include/products/NetworkStateSnapshot.hpp`, `src/core/app/ApplicationNetwork.cpp`, `src/core/config/GnssConfigLoad.cpp`, `src/core/products/NetworkStateSnapshot.cpp`, `tests/test_codebias_network_config.cpp`). `git merge-base --is-ancestor 89fe588 HEAD` succeeds.

| Piece | Anchor |
|---|---|
| Getter | `include/config/GnssConfig.hpp:3424` `bool networkEmitClockRate() const` (doc `:3404-3423`) |
| Member (default OFF) | `include/config/GnssConfig.hpp:4764` `bool network_emit_clock_rate_{false};` |
| Parse | `src/core/config/GnssConfigLoadNetwork.cpp:1696-1697`, default `false` |
| clock_model-wins log | `:1698-1701`; on-log `:1702-1706` |
| Inert-at-cadence guard | `:1707-1723` (`known_spacing_s = gnss_rate_s_ * epoch_decimation_n` `:1713-1714`; compare `:1715`) |
| Knob (mutual exclusion) | `src/core/app/NetworkRunContext.cpp:106` `k.emit_clock_rate_on = config_.networkEmitClockRate() && !k.clk_model_on;` |
| Warm gate | `src/core/filter/NetworkSolveGlue.cpp:580` `emit_rate_warm = emit_clock_rate_on && (n_emitted >= pl_warmup)`; published `:1333`; aliased `ApplicationNetwork.cpp:1499`, passed `:1587` |
| Carry state | `include/app/NetworkRunContext.hpp:130` (knob), `:285-289` `std::map<std::string, ClockOsc> clock_rate_est;` |
| Estimator (decoupled FD) | `src/core/filter/NetworkClockQc.cpp:257` (banner), `:267` `if (emit_rate_warm)`, datum-wipe `:270-279`, `:283` `z = x_s(col)`, `:285` `ClockOsc& r = clock_rate_est[sat];`, `:288-292` `r.x1 = (sat_dt>0 && sat_dt<=clk_model_max_gap) ? (z-r.x0)/sat_dt : 0.0; r.x0=z; r.t_last=pl_now_s; r.init=true;` |
| Emit stamp | `src/core/filter/NetworkSolPrep.cpp:229` `else if (emit_clock_rate_on)`, `:236` `s.clock_c1_m_s = rit->second.x1;`, `:240-241` `emitEvent("T5_clock_rate","c1_zoh_suppress",…)` |
| Warm-resume record | `include/products/NetworkStateSnapshot.hpp:65` `O,<sat>,x0,x1,p00,p01,p11,t_last,init`; field `:165`; writer `NetworkStateSnapshot.cpp:136-138`; parser `:314-323`; plumbing `NetworkRunContext.cpp:479-485` (restore) / `:746-751` (save) |
| Manifest echo | `src/core/logging/NetworkRunTraces.cpp:865`; verification integer `NetworkRunContext.cpp:208` |
| Tests | `tests/test_codebias_network_config.cpp:770` `TEST(NetworkEmitClockRate, TogglesAndDefaults)`; default-off `:701, :777`; on-parse `:783`; **service_mode does NOT arm it** `:786-792` |
| A/B configs | `config/test_suite/_emit_c1_on.json:2908` (true) / `_emit_c1_off.json:2908` (false) |
| Build | `CMakeLists.txt:57, 150, 167, 169, 181` — every host TU in the explicit source list |

`docs/HANDOFF_c1_clock_rate_producer.md:3` reads "Status: IMPLEMENTED, default-OFF, **UNBUILT**" — the doc never claimed the code was absent, only uncompiled, and the CMake entries refute even that. **The rebuild the orchestrator scoped does not need to happen.**

### E.2 Two C1 sources, mutually exclusive

Exactly two producer writes of `clock_c1_m_s`: `NetworkSolPrep.cpp:214` under `if (clk_model_on)` (source `ctx.carry.clock_osc`, the two-state oscillator) and `:236` under `else if (emit_clock_rate_on)` (source `ctx.carry.clock_rate_est`, decoupled FD). Exclusion is belt-and-braces: the `else if` plus `NetworkRunContext.cpp:106`. (A third *assignment* exists at `NetworkProductIO.cpp:151` — the reader deserializing — but it originates no value.)

**Service-mode trap.** `armBool(network_clock_model_, …)` at `GnssConfigLoadNetwork.cpp:3026-3027` arms `clock_model` under `service_mode` (`:3000`); `emit_clock_rate` is **not** in the bundle (asserted `tests/test_codebias_network_config.cpp:786-792`). So `service_mode: true` silently routes C1 through the documented-NO-GO two-state carry ("the estimable is white-FM, not RWFM… risks a NIS storm", parse-site comment `:1692-1695`), which injects a prior (`NetworkRunContext.cpp:1226-1236`) and drives refloats (`NetworkClockQc.cpp:195-200`). **Worse: because the interaction log at `:1698-1706` evaluates `network_clock_model_` at `:1698` BEFORE the arming at `:3026`, a config with `emit_clock_rate: true` + `service_mode: true` logs "network.emit_clock_rate on — C1 = finite-differenced solved clock … solution byte-identical" while the realized knob is `emit_clock_rate_on == FALSE`. The load log affirmatively misreports the active mechanism.**

### E.3 Seam placement — already correct, one slot vacant

Against the anti-regrowth decision tree in CLAUDE.md:

| Concern | Rule | Realized | Status |
|---|---|---|---|
| (a) per-epoch posterior-clock FD | post-fit QC → `NetworkClockQc` | `NetworkClockQc.cpp:267-294`, invoked `ApplicationNetwork.cpp:1584-1589` (SB_CLOCKUPD) | ✅ |
| (b) carried per-sat rate state | carry → `NetworkRunContext` | `NetworkRunContext.hpp:289` + knob `:130`, set `NetworkRunContext.cpp:106`; no bare function-local anywhere | ✅ |
| (c) emit-time stamp | emit prep → `NetworkSolPrep` | `NetworkSolPrep.cpp:229-243`, invoked `ApplicationNetwork.cpp:1534-1535` (SB_SOLPREP), before SB_EMITSTORE `:1546` | ✅ |
| (d) run-end telemetry | → `NetworkTeardown` | **VACANT** | ❌ |
| (e) warm-resume record | → `NetworkStateSnapshot` | `.hpp:65,165`; `.cpp:136-138, 314-323`; plumbing `NetworkRunContext.cpp:479-485, 746-751` | ✅ |

Driver residue: only two mentions remain in `ApplicationNetwork.cpp` — alias `:315` `auto& clock_rate_est = ctx.carry.clock_rate_est;` and comment `:359`. Relocation lineage: `774fdba` (INC-1 config split, "network section loader cut verbatim into GnssConfigLoadNetwork.cpp") → `b644da5` (NetworkRunContext) → `a9301c6` (seams) → `dee2824` (HEAD).

**(d) is a live invariant-#4 gap.** Grep for `clock_rate|c1_zoh|emit_rate|clock_c1` over `src/core/filter/NetworkTeardown.cpp` returns **zero** hits. `NetworkServiceCounters` (`include/app/NetworkRunContext.hpp:567-684`) has no clock-rate field; the run-end line at `NetworkTeardown.cpp:341-344` reports `n_emitted / n_skipped / n_gate_withheld / n_sigma0_withheld` and the SH gate gets its own toggle-gated block at `:775-793`. Only config **echo** exists (`NetworkRunTraces.cpp:865`, `NetworkRunContext.cpp:208`) — never whether a single c1 was produced.
- Skeptic's narrowing: liveness *is* readable out of band — a nonzero c1 on the wire (`NetworkProductIO.cpp:545-548`) and `T5_clock_rate`/`c1_zoh_suppress` trace rows (`NetworkSolPrep.cpp:239-241`). The precise statement is: **liveness is not readable from the run-end summary**, so an A/B needs post-hoc parsing of products.txt or a trace-armed run.
- Placement precedent: `NetworkTeardown.cpp:775` gates its whole SH readout on the toggle — the same pattern preserves default log byte-identity.

### E.4 The emitted-vs-solved design fork (heavily disputed — read carefully)

**Observation both sides agree on:** the FD state advances on **solved** epochs (`networkClockQc` is called unguarded at `ApplicationNetwork.cpp:1585-1588`; the SH gate's early returns live inside `NetworkEmitStore.cpp:120→139` and `NetworkSolPrep.cpp:140`, both upstream), while it *arms* on **emitted** epochs (`n_emitted >= pl_warmup`, `NetworkSolveGlue.cpp:580`, sole `++` at `NetworkEmitStore.cpp:367`).

**Skeptic's structural correction:** this is **one gate, not a fork**. The advance is lexically nested inside `if (emit_rate_warm)` (`NetworkClockQc.cpp:267`), and `n_emitted` is a **monotone latch** (increment `NetworkEmitStore.cpp:367`; restored-never-decremented `NetworkRunContext.cpp:521`). Once armed it never un-arms, so the two sources cannot disagree post-warmup. What exists is a **missing emission-continuity guard on the R11 staleness test**, not two halves decided inconsistently.

**The actual defect (invariant #1, not #5):** because `r.t_last` advances every 30 s straight through a withheld era, the R11 gate at `NetworkSolPrep.cpp:233-235` reads `sat_dt = 30 s` instead of the true ~2 h emission gap, **passes**, and stamps `s.clock_c1_m_s` with a rate finite-differenced between the last two *withheld* epochs — published, ZOH-valid, the instant the gate releases. R11 gates **solve** continuity; the consumer experiences **emission** continuity. `docs/SPEC_c1_clock_rate_validation.md:331-339` states this verbatim and classifies it as standing invariant #1; `:341` grades it latent-not-live; `:344-347` prescribes a separate `t_last_emitted` advanced only on actual emission (more general than gating on `sh_gate_engaged`, since it also covers `WithheldGate`/`WithheldSigma0`). **Caution: the same doc contradicts itself at `:304-308`, asserting the card's handling is "already correct" on a 900 s-bound theory that §5.3 then demolishes. Do not stop reading at §5.2.**

**Two claims about this fork were refuted:**
- "Solved-epoch advance is the operationally BETTER half, delivering a valid rate on the first post-release epoch" — **inverted.** It is precisely the invariant-#1 bypass above.
- "It would have been inert for the first ~2.1 h of tonight's run because n_emitted stays 0" — **false for this leg.** leg01 is a **warm resume** with `n_emitted = 720` restored at `NetworkRunContext.cpp:521` (`stdout.log:1030` "WARM RESUME … n_emitted 720"; `chain.log:18` "seed rc=0 → tow=327569.999988317 n_emitted=720"). Had C1 been armed, `emit_rate_warm` would have been **true from epoch 1**, the FD would have run every 30 s through the whole 239-epoch withheld era, and nonzero C1 would have appeared on the **first** emitted epoch (TOW 334770; stamp needs only `sat_dt = 30 s ≤ clk_model_max_gap = 900 s`, since leg.json sets `phase_leveling_max_gap_s = 900.0` and `clock_model_max_gap_s` defaults to it per `GnssConfigLoadNetwork.cpp:1672-1674`). The hazard inverts from "silent dead zone" to "corrupt rate published at release".
- Chronology sub-claim **confirmed**: `emit_rate_warm` exists at HEAD (`NetworkSolveGlue.cpp:578`); `sh_gate_engaged` has **zero** occurrences at HEAD and lives only in the uncommitted working tree. The warmup gate predates the SH gate. Whether the split was "a copy, not a choice" is authorial intent and **UNVERIFIED**.

### E.5 Cadence traps before arming

- Inert when `clock_model` is on (`NetworkRunContext.cpp:106`).
- Inert when `gnss_rate_s × epoch_decimation_n > clock_model_max_gap_s` (`GnssConfigLoadNetwork.cpp:1707-1723`); skipped entirely when `gnss_rate_s = 0`, which is tonight's case (no `gnss_rate_s` key in leg.json / `_day129_guards_conv.json`). At 30 s vs 900 s the guard does **not** fire.
- Snapshot tag space, if a second rate record is ever added: taken tags are `{A,C,D,GG,I,J,K,L,M,N,O,R,SV,U,V,W,X,Y,Z}` (`NetworkStateSnapshot.cpp:118,163,214,224,231`; `NetworkStateSnapshot.hpp:45`). Free and unambiguous: **F, G, P, Q, T**. Avoid B, H, S — they read as CARRY-blob tags.
- Tonight's `runs/day129_guards/leg01/snap.txt` contains **zero `O,` records and zero `K,` records** — both C1 mechanisms dormant, so a leg02 warm resume inherits no rate state.

---

## F. Empirical ground truth — tonight's numbers

*All figures read-only from the live, still-growing `runs/day129_guards/leg01/products.txt`. Multiple agents scanned at different times; where counts differ, the file grew. `withheld_sh` was **frozen at 278** across every scan while `ok`/`E` advanced in lockstep — the strongest available consistency check.*

### F.1 Record census (representative snapshots)

| Scan | `#` | H | E | C | B | P | U | S | I |
|---|---|---|---|---|---|---|---|---|---|
| early | 5 | 505 | 227 | 7100 | 46675 | 47023 | 6872 | 0 | 0 |
| mid | 5 | 533 | 255 | 7986 | 52531 | 52931 | 7730 | 0 | 0 |
| late | 5 | 545 | 267 | 8373 | 55070 | 55503 | 8105 | 0 | 0 |

- **`#E == #ok-H` in every scan.** Every `ok` H is immediately followed by an E with a string-exact matching tow; **zero** `withheld_sh` H is followed by an E. Withheld epochs produce exactly one line: the H.
- **No S contract record at all** — consistent with the v1 writer branch (`ApplicationNetworkSetup.cpp:1075`), not the deferred-v2 branch (`:1059-1071`). Hence `agek = 0`, no `frame=`/`cbpin=`.
- **No I records** ⇒ `getGenerationId()` nullopt ⇒ RTA-3 datum-break path inert.
- Header line 1 = `# gnss_network_product v1`; legend line 3 = `# C,sat,clock_c0_m` (the **old 5-line legend**, 4 grammar lines).

### F.2 Withhold structure — five eras, not one

Run-length decode of the H state token (reproduced identically by three agents, and by `logs/network_epoch_status.csv` disposition column, and cross-checked against `logs/network_events.csv`):

| Era | State | tow first → last | Epochs | Span |
|---|---|---|---|---|
| 1 | `withheld_sh` | 327599.99998664856 → 334740.0000035763 | **239** | **7140 s = 1.983 h** |
| — | ok | 334770 → 338100 | 112 | |
| 2 | `withheld_sh` | 338130 → 338520 | 14 | 420 s |
| — | ok | 338550 → 338820 | 10 | |
| 3 | `withheld_sh` | 338850 → 339390 | 19 | 570 s |
| — | ok | 339420 → 340290 | 30 | |
| 4 | `withheld_sh` | 340320 → 340350 | 2 | 60 s |
| — | ok | 340380 → 340830 | 16 | |
| 5 | `withheld_sh` | 340860 → 340950 | 4 | 120 s |
| — | ok | 340980 → … | 59→99 (growing) | |

Total withheld = 239+14+19+2+4 = **278**. Corroborated by `logs/network_events.csv`: 5 `SH_GATE engage` + 5 `release` + 278 `withhold` rows, release details `withheld_total = 239/253/272/274/278`. Producer counter agrees: `stdout.log` "Run totals: 278 out-of-band epoch(s), 5 engagement(s), 278 emission(s) withheld."

Modal H spacing **29.999998 s** (503/527 intervals in one scan; `Counter({30: 545})` in another). Zero missing heartbeats.

**Era 1 is a COLD-START famine, not a mid-stream gap.** `products.txt:6` is the first record and is already `H,2313,327599.99998664856,withheld_sh,0.0000,0,*`; lines 6–244 are all withheld; the first `ok` H is `:245` and the first E is `:246` = `E,2313,334770.00000190735,720,0,0,0`. No C/B/P/U exists before line 246. The consumer's store is **empty** through that era — nothing to coast from.

**Consumer-facing seam gap.** `runs/day129_guards/seed/products.txt:343861` last E = `E,2313,327569.9999883175,719,0,0,0`; leg01 first E = `:246`. Delta = **7200.0000136 s = 2.0000 h**, 240 epoch intervals, iod_ssr 719→720. Producer-side `logs/network_service_health.csv` peaks at `last_emit_age_s = 6960.000` at tow 334530 (measured from the *seed's* emit — independent proof the two files are one logical stream; sampled peak undershoots 7200 s because health sampling alternates 300/330 s).

Genuine mid-stream gaps (E-to-E): **600, 450, 150, 90 s** — the 19/14/4/2-epoch eras.

### F.3 c1 on the wire: identically absent

- leg01: **8244–8469 C records** (scan-dependent), field-count histogram **`{3: N}`**, zero key=value tokens, zero `iode=`/`sigcc=`/`rho=`. Sample: `C,G02,-92.21509729227567`.
- `val_out/night305_day129_products.txt` (0-gap reference): **68020 C records, `{3: 68020}`**, zero c1.
- `runs/day129_guards/seed/products.txt`: **22495/22495** 3-field.
- Config: leg.json has zero occurrences of `clock_model`, `emit_clock_rate`, `serialize_disc_counters`, `combined_sigma`, `emit_clock_disc`; it sets `sh_health_gate: true`, `emit_ura: true`, `clock_runaway_check: true`, `phase_leveling_max_gap_s: 900.0`, `start_offset_hours: 18.995833330088193`, `state_snapshot_carry_diagonal: true`.
- Consequence: `dC = c0` exactly (c1=0 absent, c2 hard-zeroed at `NetworkProductEmitter.cpp:242`, hr_clock has no record type on this wire).

### F.4 The measurement that decides the C1 question

Over the completed 0-gap reference (`val_out/night305_day129_products.txt`, 2160 contiguous 30 s epochs, tow 280800→345570, zero gaps, 68020 C records), per-epoch median c0 removed as common-mode datum (invariant #5):

Datum-reduced |d(c0)/dt| over 30 s (n = 67,977): **p50 0.004932, p90 0.021452, p99 ≈0.1312, mean 0.024641 m/s** — confirms the handoff's ~cm/s residual prediction.

**ZOH vs linear (c1 = previous-30 s finite difference — which is literally the shipped estimator, `NetworkClockQc.cpp:288-291`), p90 |error| in metres:**

| dt | ZOH | linear | ratio |
|---|---|---|---|
| 30 s | 0.641 | 1.029 | **1.61× worse** |
| 450 s | 2.640 | 10.587 | 4.01× |
| 600 s | 3.162 | 14.113 | 4.46× |
| 1800 s | 6.902 | 40.599 | 5.88× |
| 3600 s | 10.945 | 79.289 | 7.24× |
| 7200 s | 15.453 | 157.837 | **10.21× worse** |

Smoothing helps monotonically but **never reaches ZOH**: at dt=7200 s, p90 = 157.8 (B=30 s), 50.9 (B=300), 37.4 (B=900), 32.0 (B=1800) vs ZOH 15.5. Proper LS slopes (a stronger estimator than the shipped one) still bottom out at 26.7 m vs ZOH 15.4 m, with optimal shrinkage α* = 0.00 at every window.

**Diagnostic:** ZOH p90 error grows as **dt^0.581** (`log(15.453/0.641)/log(240)`) — the dt^0.5 signature of a random-walk/mean-reverting process, not the dt^1.0 of a deterministic ramp. For such a process the conditional expectation of the future value **is** the current value, so ZOH is the MMSE predictor and any nonzero c1 strictly adds variance. Robust re-derivation under a difference-domain datum (integrating the median of per-sat differences over common satellites) gives the same verdict (exponent 0.657, α*=0.00 at 2 h, linear worse at every horizon), so the result is not a datum artifact.

Error budget for context: a raw 1e-9 s/s oscillator drift is 0.2998 m/s → 1079 m at 1 h, 2158 m at 2 h; the measured SSR *residual* is 60× smaller (mean 0.0246 m/s) because c0 is a residual against SP3.

**Verdict: linear extrapolation is not meaningful at any horizon this producer faces, including one 30 s epoch. Arming `network.emit_clock_rate` would make coasting WORSE. The right fix for a multi-hour gap is a staleness/validity contract on the wire, not a rate term.**
- *Unmeasurable from this data:* dt < 30 s (product cadence is 30 s). The C1 card's stated target — the SSR saw-tooth between user and product epochs — lives below 30 s. The dt=30 s result (1.61× worse) is evidence against, but does not strictly cover sub-30 s interpolation.

### F.5 SH gate: firing on a real pathology, but the trigger is not one thing

- `logs/network_vtec_sh_health.csv`: `a00_in_band` counts `{0: 278, 1: 230}` — **1:1 with the withhold count**. `a00_tecu` spans **+1410.83 → −1147.22 TECU**. Physically absurd ⇒ frame/datum ratchet, not ionosphere.
- **The threshold is NOT adaptive** (a claim that was refuted). It is the static default band `[2.0, 150.0]` TECU (`include/config/GnssConfigTypes.hpp:1712-1713`; loaded `GnssConfigLoad.cpp:1325-1328`; leg.json overrides neither). The `threshold` column reports which **edge** was crossed (`NetworkSolveGlue.cpp:1290-1294`). Era 1 is an **upper**-edge blowup (a00 = 1410.831 vs 150.0); eras 2–5 are **lower**-edge collapses (0.503, −0.199, −8.673, 1.102 vs 2.0 — one negative). The RMS ceiling clause was inert (`sh_gate_rms_max_tecu` default 0.0 = OFF, `GnssConfigTypes.hpp:639`; all five engage rows show `rms_oob=0`).
- Gate logic: `NetworkSolveGlue.cpp:1238` (`if (nc.sh_health_gate && sh_model)`), dual-layer a00 total `:1247-1260`, band/rms test `:1266-1271`, engage `:1278` (`oob_run >= sh_gate_min_epochs`, default **1** — `GnssConfigTypes.hpp:633`, no warmup exemption), publish `:1320`. Computed **in-epoch, post-solve** (after `est.solve` `:673` and RAIM re-solves `:970/:1061/:1064`), consumed in the same epoch by two early returns.
- Gate verdict consumers, complete: `NetworkSolPrep.cpp:140` and `NetworkEmitStore.cpp:120` (the two early returns), `NetworkStashStatus.cpp:224-226` (disposition stamp `sh_gate_withheld` vs `emitted`) and `:229-231` (heartbeat-log suppression), `NetworkTeardown.cpp:787` (run-end readout). **No QC seam is skipped.**
- Edge case: when `sh_model` is null or a `vtecCoeffColumn` lookup throws, the code deliberately leaves the gate state unchanged rather than fabricating a release (`NetworkSolveGlue.cpp:1322-1324`) — so on such an epoch the early returns fire on a *stale prior-epoch* verdict.

### F.6 The pre-gate counterfactual

The completed reference run (guards off) shipped the same pathology window **unflagged**: 2160 contiguous epochs, zero gaps, per-epoch median c0 reaching **−4575.6 m at tow 334020** — inside leg01's era 1 — with `iod_ssr` incrementing normally and `datum_changed = 0`. Four epochs exceed |median c0| > 500 m (tow 334020–334110). **The gate converts a silent-corruption failure into a loud-outage failure.** Whether that is a net win depends entirely on consumer coasting behaviour, which is what §C says is undefended.

### F.7 Wire metadata is continuity-blind (skeptic-narrowed)

- leg01 E lines: **iod_ssr delta set = `{1}`** across every gap, including the 2 h seam (719→720) and all four internal gaps; `datum_changed = '0'` on **all** E lines; field count 7 on all (no optional field-8 `datum_consts`).
- Mechanism: `NetworkSolPrep.cpp:140-141` early-returns **before** the `iod++` at `:144`, so `iod_ssr` counts **emitted** epochs; the counter persists across the warm-resume seam (`NetworkRunContext.hpp:570`, save `:708` / restore `:518`).
- **But the wire is not blind in this file**: 239 `withheld_sh` H heartbeats span exactly the seam gap, and withheld H lines fill each internal gap. The blindness is confined to (i) any DEFAULT-config run, since `emit_epoch_status_record` defaults false and the 0-gap reference indeed contains **zero H lines**; and (ii) any **replay-path** consumer even with H armed, because `NetworkProductIO.hpp:75-76` documents that H never feeds `replay`. For those, `+1` across a 239-epoch gap reads as a normal 30 s step.
- `datum_changed` is not inert: the reference stream carries values `{'0','1'}`. Its meaning is narrow by design — `NetworkProductIO.hpp:209` "Clock-pivot / S-basis re-declaration this epoch" — so leg01's all-zeros is an accurate report that no pivot handover occurred among emitted epochs, not a false continuity assertion. **The real gap: no E-line field encodes "emission was withheld."**

### F.8 Telemetry coverage (scope-corrected)

`logs/network_service_health.csv` shows `n_gate_withheld = n_skipped = n_sigma0_withheld = 0` on **every** row while `n_emitted` froze at 720 for 22 rows and `last_emit_age_s` climbed 300→6960 s. Mechanism confirmed: the SH gate increments `ctx.counters.n_sh_gate_withheld` (`include/app/NetworkRunContext.hpp:594`, `NetworkEmitStore.cpp:121`), while the CSV publishes `n_gate_withheld` — the **rank/null-space** gate's counter (`NetworkRunContext.hpp:578`, `NetworkSolveGlue.cpp:519`, written `NetworkRunTraces.cpp:648`, header `include/logging/NetworkRunTraces.hpp:68-71`).

**This is a documented, intentional coverage boundary, not an invariant-#4 defect** (skeptic correction). The run-end counter exists and fires: `NetworkTeardown.cpp:775-789` (value at `:780`), block comment `:771-774` naming invariant #4, armed-but-never-fired WARN `:790-793`. `docs/rt_service_operations.md:44-48` states the gap in advance; `:109-114` tabulates the SH gate's health-CSV coverage as "**nothing**"; `:151-165` gives the operator the substitute signature. The outage IS visible in three other surfaces of this same leg: `network_epoch_status.csv` disposition (278 vs 255), `network_events.csv` SH_GATE rows, and throttled main-log WARNs every 20 engaged epochs (`NetworkSolveGlue.cpp:1310-1318`). Also, `n_emitted` freezing is an equally correct in-file signal — not just `last_emit_age_s`.

**Surviving real defects:** (a) any alarm keyed on withhold counters alone is blind to the SH tier; (b) **chain-leg continuity** — `grep sh_gate_ src/core/products/NetworkStateSnapshot.cpp` returns **zero matches**, so neither `n_sh_gate_withheld` NOR the gate's engaged/oob-run state crosses a leg boundary, while `n_gate_withheld` does (`NetworkStateSnapshot.hpp:50,117`; `.cpp:122,261`).

### F.9 No consumer grading exists for this run

`leg.json` has **zero** occurrences of `closed_loop` or `consumer` anywhere in the file (top keys: app_mode, data_source, processing, files, estimation, filter, model_corrections, network, data_download, logging; the `network` object's 78 keys contain no `closed_loop`). `ClosedLoopConfig::enable{false}` (`GnssConfigTypes.hpp:133-134`) and `GnssConfigLoadNetwork.cpp:3330-3331` reset-then-conditionally-overwrite make absence deterministic.

Affirmative proof: the consumer grading column is `pos_err_3d_m`, written into `closed_loop_trace.csv` by exactly two sites with an identical header string (`NetworkRunTraces.cpp:118-119` under `if (cl_trace_armed)` `:114`; `ApplicationConsumerReplay.cpp:257-258`). **That file is absent from `leg01/logs/`.** All 13 CSVs there have no `pos_err`/`pos3d`/`east_m` column. `gnss_config.log` (21,662 B): zero `closed`, zero `pos_err`, zero `truth`.

**Coasting cannot be graded from tonight's run.** It requires a separate consumer-replay pass over `leg01/products.txt` via `network.closed_loop.replay_products_path` (`GnssConfigLoadNetwork.cpp:3361`, log text `:3373-3377`; `include/app/Application.hpp:479-485`), which short-circuits `runNetwork` so the joint producer does not run. `scripts/q5_make_replay_arm.py` already encodes a held-out consumer (NRMG00NCL, DOY-129, 30 s, ITRF2020 truth ECEF `[-5743538.2169, 1380503.7952, -2397895.8332]`) and exposes `--products`. Safe against a partial file: `NetworkProductIO.cpp:300, 308` flag-and-continue on short/malformed lines (a torn final line degrades to a sticky `ok=false`, not a crash). **Nothing in this audit launched a binary or touched the run.**

### F.10 Run progress at last read

Last emitted epoch tow **342960.0** (iod_ssr 954) in one scan, tow 343950 (iod 987) in a later one; `chain.log:56` `[390 min] snapshot_tow=342449.999986649`. Day-end target tow 345570.

---

## G. Contradictions and residual uncertainty

### G.1 Where the skeptic overturned something material

| Original claim | Correction | Anchor |
|---|---|---|
| "Every failure mode leaves the satellite on the existing baseline rather than dropping it" | **False.** Stages 1, 2, 7 `return 0.0` (`GnssCorrections.cpp:148, 152, 387`), and **0.0 IS the drop signal** — `ModelGeometry.cpp:496-508, 553-559` → `DropReason::NoProduct`. Silent-coast is conditional on a precise CLK covering the epoch (`interpolateClock` MAX_WINDOW 3600 s, `:398`) | see §C.3 |
| "Gate is a 3-term AND" | **Four** conditions — `has_clock` at `GnssCorrections.cpp:230` is an unenumerated outer guard | §A.4 |
| "Clock/orbit window asymmetry bites tonight" | **Unreachable on this stream** — no orbit records are ever pushed, so `has_orbit` is always false and the ModelGeometry guards never fire. Latent only | §B asym. 3 |
| "The withheld gap degenerates to a single shape: pure clock-frame swap" | **Three shapes** — clock+bias / bias-only (30–60 s) / neither, plus a fourth family (slant iono, `RealTimeEngine.cpp:2097`) with a third expiry law | §C.2 |
| "The consumer never has stale clock + inflated R" | **False.** RTA-9(b) StaleHeld is built and unit-tested (`GnssCorrections.cpp:350-358`; `MeasurementModel.cpp:1974-1983`; `tests/test_realtime_seams.cpp:637-650`). It is unreachable **at default config**, not structurally. Also: the R block is inert for a different reason — `consume_ura` defaults false, so the `:1935` guard is false all run | §B asym. 4 |
| "Nothing reacts when the clock flips fresh↔stale, with no reset and no log" | Right on **reset**, wrong on **log** (`GnssCorrections.cpp:191-194, 367-371`). Also over-scoped: the A5 machinery is RealTimeEngine-only; post-processing has neither side | §C.4.4 |
| "ZOH is silent to every consumer surface" | **pyviz keeps `has_c1`** and two surfaces act on it. The C++ estimator discards the bit; the genuinely-undetectable case narrows to `emit_clock_disc` writing a literal `,0` | §A.5 |
| "One producer gap costs TWO reset storms; tonight's 600 s gap" | Neither. First-sighting guard (`RealTimeEngine.cpp:1730`) ⇒ leading gap costs **one**; inclusive 60 s ⇒ 2-epoch gaps cost **zero**; there is no single 600 s gap — five eras, ~7 storms total | §C.4.4, §F.2 |
| "One contiguous 8340 s (2.317 h) block" | **Five eras**; the leading one is 7140 s = 1.98 h. 8340 came from `278 × 30` instead of a tow subtraction | §F.2 |
| "The gate threshold is adaptive" | **Static band [2.0, 150.0] TECU**; the `threshold` column reports the crossed **edge**. Era 1 is an upper blowup, eras 2–5 lower collapses — opposite failure modes | §F.5 |
| "leg01 resumed into an already-pathological state from the seed" | **False.** Seed emitted 720/720 `ok`, zero SH_GATE events in 12,312 rows, final a00 = 34.837 TECU in-band. `seed/snap.txt` contains **no SH/VTEC state at all** (record census + case-insensitive `vtec|sh|a00|iono` regex ⇒ 0 matches), so leg01 restarts the SH field **cold** and the gate captures a cold-start transient it then clears | §F.5 |
| "Had emit_clock_rate been armed, it would have been inert for ~2.1 h" | **Inverted.** Warm resume restored `n_emitted = 720` (`NetworkRunContext.cpp:521`; `stdout.log:1030`; `chain.log:18`), so the FD would have been armed from epoch 1 and published a corrupt rate at the first release epoch | §E.4 |
| "Horn B (solved-epoch advance) is the operationally better half" | It is the invariant-**#1** staleness-guard bypass; and the fork framing is wrong — one monotone latch, one missing guard | §E.4 |
| "Standing-invariant #4 telemetry defect (health CSV)" | Documented coverage boundary; the run-end counter exists and fires (`NetworkTeardown.cpp:775-793`) and `docs/rt_service_operations.md:44-48, 109-114, 151-165` states the gap in advance | §F.8 |
| "iod_ssr +1 across the gap ⇒ consumer-blind metadata" | Blind only for DEFAULT-config runs (no H) and for replay-path consumers (H never feeds replay). leg01 itself carries 239 explicit `withheld_sh` heartbeats | §F.7 |
| "During a withhold the producer writes exactly one line" | True **only** with RTA-10 armed. Default `emit_epoch_status_record = false` ⇒ **zero** lines. Latent v2-contract case ⇒ zero lines and no file at all | §D.1, §D.3 |
| "Writer can emit exactly five state tokens" | Six are emittable; `Unknown` is the struct default (`SsrCorrectionStore.hpp:52`) and `writeEpochStatus` applies no filter. Five appear today because all four call sites assign explicitly | §D.1 |
| "The C1 wire path being complete means nothing blocks coasting" | The 30 s age gate blocks it before the polynomial is reached. Two changes needed, not one | §A.6 |
| "'SSR stream stalled' is what a consumer sees during a withhold" | Only under `enable_obs_ssr_sync` (default false) with a live orbit/clock source — false for the closed-loop replay consumer; and with H armed the `withheld_sh` warn fires first | §D.5 |

### G.2 Where two agents disagreed with each other

1. **Whether era 1 is "2.0000 h" or "1.98 h".** Both are right about different quantities: the **consumer-facing emission gap** across the seed→leg01 seam is exactly 7200.0000136 s (2.0000 h, last E in seed → first E in leg01); the **withheld-epoch span within leg01** is 7140 s (1.983 h, 239 epochs). Use the first when reasoning about consumers, the second when reasoning about the gate.
2. **Whether era 1 is a "gap" at all.** One agent treated it as the dominant coasting gap; another established it precedes any emission *in leg01*. Both true — but the seed file *does* provide a prior product, so a consumer replaying seed+leg01 concatenated has something to coast from, while a consumer attaching at leg01 start does not. Which applies depends on how the chain legs are presented to the consumer. **Unresolved in operational terms.**
3. **Severity framing of the C1 telemetry gap.** One agent called it "live"; the verifier narrowed it to "latent-but-armable" (default OFF, absent from tonight's leg, and the `_emit_c1_on.json` arm is prepared but unrun). Both correct at different scopes.
4. **`freshEnough` as "the one predicate."** One agent asserted it; the skeptic surfaced `classifyClockAge` as a second, independently-implemented ladder and whole un-gated channels. The corrected statement is in §B.

### G.3 Still UNVERIFIED — with what would settle each

**Binary provenance (affects nothing in §A–§E, everything in "does the tree describe the running producer"):**
- The running `gnss_processor_p2chain.exe` predates the working tree: `products.txt:3` is the old short legend while `NetworkProductIO.cpp:483` writes the grammar-complete one (block `:480-494`, completeness assertion `:463-474`, test `HeaderLegendDeclaresEveryEmittedField`). **Settle:** compare exe build timestamp against `NetworkProductIO.cpp` mtime, or grep the exe for the long legend string. *Data-line conclusions are safe — the data grammar is unchanged.*
- Consequence already visible: the live file emits 545 H and 8105 U lines that its own legend does not declare, and per `NetworkProductIO.hpp:76-79` an unknown tag flips a pre-RTA-10 reader's sticky `healthy()` to false.

**Consumer configuration (decides which §C.3 branch actually fires):**
- Whether any eventual DOY-129 consumer arm loads a precise CLK. Evidence strongly implies yes (post mode ⇒ `RealTimeEngine.cpp:171-173` loads SP3/CLK), but no consumer config for this leg exists yet — `_day129_guards_conv.json` has no `network_consumer`/`closed_loop`/`ssr_*`/`trace_ssr_consumer` keys. **Settle:** read the replay arm's JSON `processing.ssr_max_age_s` / `ssr_clock_stale_hold_factor` / `enable_ssr_bias_max_age` / `network_consumer.*`, and the "SSR coverage / clk-applied" line at `RealTimeEngine.cpp:1127` in its log.
- Whether the batch consumer-replay driver applies the same 30 s gate. `GnssConfig.hpp:4566` annotates `ssr_max_age_s_` as "RT-only", which if literal would mean an offline replay applies a 2-hour-stale frozen clock with **no brake**. **Settle:** trace every `getOrbitClock` caller and the max_age each passes in post-processing mode.
- Whether a tail consumer that sets `realtime.network_products_path` but leaves `network_consumer.enable` false really gets unbounded biases. Only 3 writers of `enable_ssr_bias_max_age_` exist (`GnssConfigLoad.cpp:414`, `GnssConfigLoadNetwork.cpp:3543, 3578`), which strongly suggests yes. **Settle:** confirm no fourth arming path.

**Physics / magnitudes:**
- Whether the ±60–137 m per-satellite spread of emitted C0 about its epoch median (σ ≈ 32 m) is expected for this datum realization or is itself a frame-guard symptom. This decides how bad the §C.3(a) silent frame swap is. **Settle:** read the S-basis/datum seam (`NetworkSbasis`/`NetworkChainDatum`) and compare emitted C0 against the a-priori CLK for one satellite over one epoch.
- Whether withheld-era posterior clocks actually carry frame drift (the premise of the invariant-#1 C1 corruption). **Settle, read-only:** per-sat clock columns from `leg01/logs/network_solution.csv` across withheld epochs, de-meaned per constellation (the gauge removal already at `NetworkClockQc.cpp:340-355`); if raw-minus-demeaned ≈ 0.1 m per 30 s (DOY-129 ratchet scale, 49 m / 3.79 h ≈ 3.6e-3 m/s) the contamination is established; if ≈ 3e-4 m/s it is hygiene.
- Whether dt^0.58 holds below 30 s. **Settle:** a 1–5 s producer run or an external 5 s clock product.
- Whether eras 2–5 (lower-edge, 0.5 to −8.7 TECU) share a cause with era 1 (upper-edge, +1410 TECU). **Settle:** `a00_std_tecu` column in `network_vtec_sh_health.csv` at those epochs.
- Whether era 1 is a genuine SH excursion or a cold-start artifact. Strong evidence for **cold-start**: the snapshot carries no SH state and the gate has no warmup exemption (`sh_gate_min_epochs` default 1). **Settle:** confirm no SH/VTEC block in the snapshot writer.

**Code paths not fully read:**
- What `RtcmParser` sets `SsrClockCorrection::received_time` to (wall-clock vs header epoch) — decides whether freshness and the dt base can diverge on the RT path as they cannot on the product path. **Settle:** read the SSR clock decode site in `src/core/rtcm/RtcmParser.cpp`.
- What `RealTimeEngine.cpp:1711, 1767, 1843, 1887` do with the values from the un-aged bias/disc overloads. **Settle:** read `RealTimeEngine.cpp:1700-1900`.
- Whether any consumer reads `NetworkProductRecord::epoch_status`. Greps found only tests, but call sites of `pollInto`/`read` were not exhaustively enumerated. **Settle:** grep every `pollInto`/`read` call site for `.epoch_status`.
- Whether arming `network.serialize_disc_counters` perturbs replay: `NetworkProductIO.cpp:900-905, 987` indicate a tail consumer **adopts** wire counters instead of recomputing when armed. Adoption branch unread. **Settle:** read `NetworkProductIO.cpp:960-1000` + `NetworkProductIO.hpp:379-397`.
- Whether `ApplicationNetwork.cpp:1546-1584` contains an early `continue` that would make `networkClockQc` skippable on a withheld epoch. Verified unguarded at the call site (`:1584-1589`) and by the `NetworkEmitStore.cpp:119` contract, but the intervening lines were not read in full.
- `networkUraStalenessQ2()`'s default (only the hard-coded fallback `1.5e-7` at `MeasurementModel.cpp:1979` was read; `GnssConfig.hpp:4775` documents it as loader-defaulted to `network_clock_model_q2_`).
- Whether the per-satellite "SSR REJECTED stale" warn actually reaches disk at the configured log level during a real gap. **Settle:** grep a consumer run's `ppp_model` log during a withheld stretch.
- **Authorial intent** on whether `emit_rate_warm`'s use of `n_emitted` was ever reviewed against the SH gate. The comment at `NetworkSolveGlue.cpp:578-579` gives a substantive warmup rationale, so the gate itself is deliberate; what is undocumented is any consideration of withhold eras. **Settle:** `docs/SPEC_day129_frame_guards.md` or the Phase-2.2 review notes.
- **Nothing here was built or run.** All GoogleTest assertions cited are verified as *source*, not as passing executions. `runs/day129_guards/` was read only; no process was touched.

### G.4 The three findings that should drive the next action

1. **The staleness horizon, not the rate term, is the coasting lever.** §F.4 measures linear extrapolation as strictly worse than ZOH at every horizon including 30 s, and §A.6 shows c1 is never evaluated past 30 s anyway. Do not arm `network.emit_clock_rate` for coasting.
2. **The 30–60 s bias-without-clock window is a real frame violation with no guard, no flag, and no reset**, and it becomes unbounded on any config that consumes biases without arming `enable_ssr_bias_max_age`. That is the defect with a concrete fix.
3. **The producer-status channel is a complete dead end on the path that would grade tonight's stream.** One log line in RealTimeEngine, nothing in the replay driver, RTF-9 scoped out in three places. Threading `latest_status` out of `NetworkProductReader::read` is a few lines and converts a silent frame swap into an attributable event.