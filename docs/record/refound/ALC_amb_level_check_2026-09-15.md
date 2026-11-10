# ALC: consumer ambiguity level check against its own code (2026-09-15)

**STATUS**

- **id:** ALC
- **topic:** consumer
- **status:** superseded
- **summary:** The consumer level check (`network.closed_loop.amb_level_check`) BUILT, default OFF: field bar PASS, sim bar FAIL by 0.0096 m; adopted 2026-09-19, then OFF under the phase-bias hold.
- **dates in the text:** 2026-09-15; corrections appended 2026-09-16 and 2026-09-17
- **knobs / posture:** `network.closed_loop.amb_level_check` (+ `_window_ep` 20, `_k` 4.0, `_mode` `act` | `observe`), default OFF; arms are the PBT pair plus the one key
- **hashes named in the text:** exe `gnss_processor_alc.exe` `87923C21E731DDC8` (source main `852f02e` + the card's diff); OFF smoke record stream `333DCB0871E1F5BC`; `products.txt` field `EDAF96CEDB1262B2`, sim `2149CABD1192E610`, byte-identical to the PBT arms
- **run dirs named in the text:** `runs/_alc_field6`, `runs/_alc_sim6`, `runs/_alc_smoke_off`; references `runs/_pbt_{field,sim}6`, `runs/_rfpdfix_{field,sim}6_both`, `runs/_cst_field6_both`
- **source:** [PBT](PBT_consumer_pb_translate_2026-09-15.md) sections 7 and 8 (a carried or translated ambiguity is continuous, not correct)

**Outcome.** The card asks whether re-validating the consumer's float ambiguities against its own wide-lane and ionosphere-free code levels repairs the hours that card PBT's translation lost. With the wire byte-identical, field hours 4 and 5 go 1.58 / 1.90 to 0.49 / 0.68 m; sim hour 3 is +0.0596 m against a 0.05 bar. C1 and C3 PASS, C2 FAILS, nothing promoted. [ALC-ARC](ALC-ARC_reset_generation_2026-09-16.md) corrected the window restart and [ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md) found the 4 sigma bar uncalibrated; [ALC2](ALC2_restart_policy_adoption_2026-09-19.md) adopted the check into the working bases, and [PBH](PBH_phase_bias_hold_2026-09-19.md) turned it OFF as harmful under the phase-bias hold.

**Status as recorded (2026-09-15):** open — **one knob BUILT (`network.closed_loop.amb_level_check` + three tuning knobs), default OFF, two runs spent, no golden and no ledger touched, nothing promoted.**
PBT §7 named the one thing cards CC and PBT had in common and neither fixed: **a carried or translated ambiguity is CONTINUOUS, not CORRECT**, and
invariant #7's re-coupling edge has no re-validation.  It measured the cost exactly — over TOW 270900–273000 the reset-only baseline moved its
ambiguities **3.2416 m** (max cumulative |ΔAMB|) through mass resets at 271050 / 271200 and came out NEARER the truth, while the translated arm
moved **0.4489 m** and came out **+0.53 / +1.45 m worse** at hours 4 / 5.  Those resets were re-levelling accumulated ambiguity error FROM CODE,
by accident.  **This card does it deliberately.**  Per satellite tracked on two frequencies the consumer keeps a 20-epoch window of two levels
built from its OWN corrected observations — the Melbourne–Wübbena wide-lane and the ionosphere-free combination of the PRE-fit innovations, both
geometry-, clock-, troposphere- and first-order-ionosphere-free — and a float state that disagrees with its level by more than `k·σ_level`
REFLOATS both of that satellite's ambiguities through the existing reset path.  **The wire never moves**: `products.txt` is **byte-identical**
to the PBT arm on BOTH beds, whole file included, and so are `network_epoch_status.csv`, `network_dd_fractional.csv` and the entire `rf2d_grade`
wire read row for row.  **FIELD goes `0.48 / 0.19 / 0.29 / 1.58 / 1.90 / 1.09 → 0.43 / 0.16 / 0.31 / 0.49 / 0.68 / 0.49 m`** — hour 4 **−1.09 m**,
hour 5 **−1.22 m**, hour 6 **−0.60 m**, five of six hours better than the best prior arm and the sixth (+0.023 m) inside the noise; the event
window goes **1.288 → 0.669 m** (baseline 3.561) and the ramp window's max-rise **+0.027 → +0.004 m**.  **SIM is flat to better on five hours and
+0.060 m on hour 3**, with epochs under 0.10 m **53 → 81** and WL at its own true integer unchanged at **99.1 %**.  The instrument fired and says
so: FIELD **106 trips (27 wl / 79 if) → 106 refloats** over **1183 full-window checks**, `|state − level|/σ` p50 **5.58** (min 4.01, max 15.99),
and the CST cross-check **212 = 2 × 106 cell-epochs carry `refloat_level`, AGREE**.  **Verdict: C1 PASS, C3 PASS, C2 FAILS on ONE clause by
0.0096 m** (sim hour 3, +0.0596 against a ≤ 0.05 bar).  Nothing is promoted.

## 1. The fix, as built

### 1.1 `network.closed_loop.amb_level_check` (+ `_window_ep` 20, `_k` 4.0, `_mode` `act` | `observe`)

| piece | file:line |
|---|---|
| **THE LEVELS** — the two combinations, derived and documented | `include/realtime/AmbLevelCheck.hpp:191` `wlLevelMinusState`, `:202` `ifLevelMinusState` |
| **THE LEVELS, at the consumer** — the four pre-fit innovations harvested per (sat, signal) | `src/core/realtime/RealTimeEngineEstimate.cpp:447-461` (the `ctx.meas_update` sweep), formed at `:520-523` |
| the point at which the model has applied clock, code bias and phase bias | `src/core/measurement/MeasurementModel.cpp:1640-1643` (`predicted = modeled + … + ssr_bias_term`), with `ssr_bias_term` at `:1606-1630` |
| **THE TEST** — window, moments, floors, WL-then-IF | `src/core/realtime/AmbLevelCheck.cpp:36` `moments`, `:54` `push` (`:81-88` the two comparisons) |
| **THE REFLOAT PATH** — the EXISTING reset, both bands | `src/core/realtime/RealTimeEngineEstimate.cpp:554-555` `state_manager_->resetAmbiguity(amb, reset_std)`; the primitive is `src/core/filter/StateManager.cpp:1095`, the same call and the same `ambiguity_std` `reset_disc` used before card CC (`RealTimeEngineSsrBreaks.cpp:139`) |
| **THE WINDOW-RESTART GUARANTEE** — the rule | `src/core/realtime/RealTimeEngineEstimate.cpp:505-518`; its two halves are stated in the seam banner `:380-401` |
| … half (1), the EXPLICIT marks | `RealTimeEngineEstimate.cpp:416` `noteAmbLevelDisturbed_`, called from `RealTimeEngineSsrBreaks.cpp:141` (reset_disc), `:160` (reset_flip), `:237` (card-CC carry), `:349` (card-PBT translate) and `RealTimeEngineEstimate.cpp:557` (this seam's own refloat) |
| … half (2), the STRUCTURAL variance test | `RealTimeEngineEstimate.cpp:512-514` (`getStateVariance(i) >= ambiguity_std²`) |
| the call site (after update / constraints / AR, before the CST harvest) | `src/core/realtime/RealTimeEngine.cpp:555` |
| per-epoch transport cleared at the epoch top | `src/core/realtime/RealTimeEngine.cpp:409-413` |
| events + counters drained into `network_events.csv` | `src/core/filter/NetworkEmitStore.cpp:845-865`; counters `include/app/NetworkRunContext.hpp:3064-3067` |
| run-end readout beside the PBT closure line | `src/core/filter/NetworkTeardown.cpp:5728`; engine-side line `RealTimeEngineEstimate.cpp:571` `logAmbLevelCheckSummary_` |
| knobs + Doxygen | `include/config/GnssConfig.hpp:656` / `:660` / `:664` / `:669`, members `:7727-7730`; `include/config/GnssConfigTypes.hpp:224` (`ClosedLoopConfig::amb_level_check` + the three) |
| base-config parse (a STANDALONE RT consumer) | `src/core/config/GnssConfigLoad.cpp:619-659` |
| harness parse + derivation onto the consumer | `src/core/config/GnssConfigLoadNetwork.cpp:11383-11386` (parse) and `:11680-11688` (`deriveClosedLoopConsumerConfig`) |
| gtest (`AmbLevelCheckTest`, 12 cases) | `tests/test_amb_level_check.cpp` |
| new trace-only grader | `scripts/alc_level_read.py` |

### 1.2 The two levels, and why they are levels of the ambiguity error and of nothing else

The consumer's rows are `predicted_phase = modeled + geometry + clock + tropo − iono + ssr_bias_term + amb` and
`predicted_code = modeled + geometry + clock + tropo + iono + ssr_bias_term` (`MeasurementModel.cpp:1640-1643`, `amb` in METRES).  The PRE-fit
innovation of each row is therefore the consumer's own CORRECTED observation — satellite clock, satellite code bias and satellite phase bias all
applied, by the model's own read — referenced to the current state.  Two classical combinations of the four innovations of a dual-frequency
satellite (`ip1, ip2` phase; `ic1, ic2` code, all metres) are:

```
WL, cycles :  [ (f1·ip1 − f2·ip2)/(f1−f2)  −  (f1·ic1 + f2·ic2)/(f1+f2) ] / λ_WL
IF, metres :  ( f1²·(ip1 − ic1) − f2²·(ip2 − ic2) ) / (f1² − f2²)
```

The WL phase coefficients `(f1, −f2)/(f1−f2)` and the NL code coefficients `(f1, f2)/(f1+f2)` each sum to 1 on the geometry and cancel the
first-order ionosphere against each other; the IF coefficients do both on their own.  **So the receiver clock, the receiver PHASE clock, the
troposphere and every slant-iono state cancel exactly, and no receiver-common constant is left for the test to trip on** — which is the whole
reason the levels are taken at the innovation and not by re-forming φ and P.  What survives is exactly `level − state`: the WL statistic is
`N_WL(code) − (N̂₁ − N̂₂)` in cycles and the IF statistic is the IF level minus the λ-weighted IF combination of the float ambiguities in metres.
Pinned by `AmbLevelCheckTest.BothCombinationsKillGeometryAndFirstOrderIonosphere` (17 m of common mode and 6 m of slant iono give both
statistics EXACTLY zero, to 1e-9).  **Nothing re-reads a product anywhere in this seam.**

Each sample is a PAIRED per-epoch difference, so the window's own sample σ is the noise of precisely the quantity tested:
`σ_level = max( s/√n , floor )`, floors `0.02 cyc` (WL) and `0.05 m` (IF).  The floors exist only so that an abnormally quiet window cannot
manufacture an arbitrarily tight bar; both sit ~4× below what a 20-epoch window of 0.5 m code noise actually produces (WL 0.092 cyc, IF 0.33 m),
so on real data the DATA sets the bar — confirmed on the runs, where the median trip's σ_WL is 0.02–0.05 cyc and σ_IF 0.05–0.27 m.

⚠ **The two channels have very different sensitivity and the card states it rather than hiding it.**  At code σ 0.5 m and n = 20, the WL
channel's 4σ bar is **0.37 cycles** and the IF channel's is **1.33 m**.  An equal-in-metres 1.0 m ambiguity error is 1.0 m of IF level AND 1.16 WL
cycles, so the WL channel catches it first; the IF channel is what catches a drift that is band-common in CYCLES, which leaves `N̂₁ − N̂₂`
exactly unchanged and is invisible to WL.  Both clauses are pinned
(`OneMetreDriftTripsAfterTheWindowFillsAtKFour`, `TheIfTestCarriesAWlSilentLevelDriftOnItsOwn`).

### 1.3 THE WINDOW-RESTART GUARANTEE — "a window containing a translation cannot trip"

Two independent halves, either sufficient on its own:

1. **EXPLICIT.**  Every seam that WRITES an ambiguity value or variance calls `noteAmbLevelDisturbed_(sat)`: the reset path
   (`RealTimeEngineSsrBreaks.cpp:141` / `:160`), the card-CC carry (`:237`, on any non-`None` action), the card-PBT translation (`:349`, on any
   action but `None` / `SkippedEps` / `RefusedUndeclared` — the three that write nothing, which is what keeps an EMPTY declaration from costing
   a whole window), and this seam's own refloat (`RealTimeEngineEstimate.cpp:557`).  **All of those run strictly BEFORE this seam in
   `processEpoch`**: the three SSR reactions are pre-update (`RealTimeEngine.cpp:524`) and this seam is post-AR (`:555`), so the set is complete
   for the epoch by the time it is read.  The window is dropped BEFORE this epoch's sample is pushed, so a translated epoch becomes sample 1 of
   a NEW window and cannot be tested for another `window_ep` epochs.
2. **STRUCTURAL**, and it covers writers this card never touched (the slip seam's `resetAmbiguity`, a first sighting, a re-admission).
   `StateManager::resetAmbiguity` sets the ambiguity variance to exactly `ambiguity_std²` and marks the key pending re-seed; `addAmbiguity`
   births it at the same value.  So a key whose variance is `>= ambiguity_std²` has NOT been updated since it was reset or born, and the seam
   restarts on that test alone.  **It is an equality against the same constant the writer uses, not a tuned threshold.**

Consequence kept deliberately: after a refloat, half (2) holds the window shut until the state has re-converged, so the check cannot enter a
refloat limit-cycle on one satellite.  Pinned by `AmbLevelCheckTest.ATranslationEpochRestartsTheWindow` (19 samples, a restart, then 19 grossly
wrong samples that are all UNTESTABLE, and the first legal test lands exactly `window_ep` epochs after the step).

**Measured cost of the guarantee**: FIELD **1159 window restarts against 5799 samples**, so only **1183 of 5799 satellite-epochs (20 %) ever
reach a full window**.  The check is live on a fifth of the consumer's sky and still moves hours 4–5 by more than a metre.  That is a finding,
not a caveat — see §6 item 2.

### 1.4 What `observe` does

`observe` emits the `trip` rows and counts, and writes nothing to any state (`RealTimeEngineEstimate.cpp:547`).  A trip clears the window in
BOTH modes, so the `observe` census counts distinct trip EVENTS rather than one per epoch until the drift ages out; in `act` that clear is
redundant with the refloat's variance re-inflation.  **Per the run budget the `observe` arms were NOT run** — the trip census `act` produced is
the observatory this card reports (§4.3), and it is the SAME census `observe` would produce up to the refloats' own feedback.

## 2. Build, exe and identity

Exe `build/x64-Release/gnss_processor_alc.exe` sha256[:16] **`87923C21E731DDC8`** (a copy of `gnss_processor.exe`, re-hashed after the copy;
source main `852f02e` + this card's diff).  Both arms exit 0 and emit **720 / 720**.

Debug `gnss_tests.exe`, verbatim:

```
[==========] 2265 tests from 298 test suites ran. (85057 ms total)
[  PASSED  ] 2264 tests.
[  SKIPPED ] 1 test, listed below:
[  SKIPPED ] FixtureSmokeTest.Sp3HeaderTimeConversionMatches
```

(2253 / 297 before this card + 12 `AmbLevelCheckTest` = 2265 / 298.)

**OFF smoke** `runs/_alc_smoke_off` (= `runs/_rf2d_smoke_off/smoke.json`, the two run paths only — a verified 2-line diff): `^#`-stripped record
stream **`333DCB0871E1F5BC`** over **6 596 record lines** — the canonical value, IDENTICAL.  Whole-file sha256[:16] **`B05F26B1CD0540DB`**, which
is **byte-for-byte the PBT card's own OFF smoke**: this card adds nothing to `products.txt`, not even a legend token.  (The stored
`_rf2d_smoke_off` reads `9ADD1D3AB9D70657`; the whole difference is the `# S,` legend line JT + MS added on main before this card, PBT §2.)

**Identity gates — the strongest form, on both beds.**

| gate | measured | |
|---|---|---|
| FIELD `products.txt` vs `_pbt_field6` | whole file **`EDAF96CEDB1262B2` → `EDAF96CEDB1262B2`**, record stream `44B50A8A85312ABC` (172 476 lines) — **BYTE-IDENTICAL, `#` lines and `R` records included** | PASS |
| SIM `products.txt` vs `_pbt_sim6` | whole file **`2149CABD1192E610` → `2149CABD1192E610`**, record stream `60242D44DCCE0A85` (136 246 lines) — **BYTE-IDENTICAL** | PASS |
| both beds `logs/network_epoch_status.csv` | `939727D60970F6B5` (field) / `E4FC04F44D51F1EE` (sim) — IDENTICAL | PASS |
| both beds `logs/network_dd_fractional.csv` | `805D8B21F3A7F9C4` / `8E3F6AA14565E52D` — IDENTICAL | PASS |
| FIELD whole `rf2d_grade` wire read | IF dispersion p50/p90/max `0.055 / 0.201 / 0.68` m, worst G26 +0.68 / G27 −0.63 / G17 −0.31; declarations cont 0/139, cold 12/0; `CLK_STEP` 0; `cta_decouple` 189; handover-mode table identical row for row (`all-cold 7`, `all-cont 91`, `mixed 2`, `none 22107` with 6 above 1.0 m) | PASS |
| the PBT ledger itself | `cells 11618 (declared 390) → declared 326 / undeclared 1358; Σ|Δ| 100.927 m; skipped_eps 9894; closure HOLDS` — **identical to `_pbt_field6` field by field**; SIM likewise `10800 / 715 / 1989 / 575.557 m / unknown_key 4` | PASS |

The consumer artefacts DIFFER by design (`closed_loop_trace.csv` `5D66D5FCCA0BD240 → EE254B2E01B932A8` field, `916E1595EE0F7A72 → 9CC3F566BED54963`
sim; `closed_loop_sat_trace.csv` `19CB96F4944ED5BE → C84647CA904A960B` and `FC3537B64932CFF8 → 60ECF98805E7392F`): that is the fix working.

**The knob armed** — the derived consumer config is not echoed, so it is confirmed from the consumer's own run-end telemetry:

```
FIELD  amb level check (ALC) : mode 'act' window 20 ep k 4.00; sats 20 / samples 5799 / window restarts 1159
       -> n_amb_level_checks 1183; n_amb_level_trips_wl 27 / n_amb_level_trips_if 79; n_amb_level_refloats 106;
       drained 1183/27/79/106 -> AGREE
SIM    amb level check (ALC) : mode 'act' window 20 ep k 4.00; sats 19 / samples 5387 / window restarts 1508
       -> n_amb_level_checks 1626; n_amb_level_trips_wl 2 / n_amb_level_trips_if 24; n_amb_level_refloats 26;
       drained 1626/2/24/26 -> AGREE
```

`AGREE` is the cross-check between the ENGINE's own ledger and what the harness drained into `network_events.csv` — two different places, so a
mismatch would mean an epoch's events were lost between them.

## 3. The arms

| arm | from | to | keys added |
|---|---|---|---|
| OFF smoke | `runs/_rf2d_smoke_off/smoke.json` | `runs/_alc_smoke_off/arm.json` | none (2 paths) |
| field | `runs/_pbt_field6/arm.json` | `runs/_alc_field6` | `closed_loop.amb_level_check: true` |
| sim | `runs/_pbt_sim6/arm.json` | `runs/_alc_sim6` | same |

Each diff was verified in full before launch: **two hunks** for the field and sim arms (the run path and the one key) and **two path lines** for
the smoke.  Both arms already carried `pb_step_translate: true` and `trace_consumer_sat: true`; the three tuning knobs are at their defaults.

## 4. Bars C1–C3

### 4.1 C1 — FIELD — **PASS** (7 of 7 clauses)

| clause | bar | measured (`_alc_field6`) | |
|---|---|---|---|
| hour 4 improves from 1.58 by > 0.30 m | < 1.28 (target ≤ 1.05) | **0.4892** (−1.093) | PASS, and it beats the pre-translate target by 0.56 |
| hour 5 improves from 1.90 by > 0.30 m | < 1.60 (target ≤ 0.45) | **0.6806** (−1.215) | PASS on the bar; **0.23 short of the parenthetical 0.45 target** |
| hour 1 no worse than 0.48 by > 0.10 | ≤ 0.58 | **0.4312** (−0.051) | PASS |
| hour 2 no worse than 0.19 by > 0.10 | ≤ 0.29 | **0.1629** (−0.030) | PASS |
| hour 3 no worse than 0.29 by > 0.10 | ≤ 0.39 | **0.3097** (+0.023) | PASS |
| hour 6 no worse than 1.09 by > 0.10 | ≤ 1.19 | **0.4857** (−0.600) | PASS |
| emitted | 720/720 | **720 / 720** | PASS |

Windows, all three arms:

| window | baseline `_rfpdfix_field6_both` | translate `_pbt_field6` | translate+check `_alc_field6` |
|---|---|---|---|
| event, TOW 276810–278400 (n 54) — p50 | 3.561 | 1.288 | **0.669** |
| … first / last / max | 0.383 / 3.731 / 4.012 | 1.813 / 1.109 / 1.813 | **0.635 / 0.594 / 1.143** |
| … per-5-min p50 | 2.89 3.20 3.75 3.54 3.75 3.75 | 1.60 1.46 1.33 1.24 1.14 1.10 | **0.61 0.65 0.70 0.69 0.67 0.63** |
| … max-rise | +0.863 | −0.033 | **+0.092** |
| ramp, TOW 279240–280800 (n 52) — first / last / max | 1.081 / 4.683 / 5.636 | 1.084 / 0.908 / 1.084 | **0.464 / 0.385 / 0.502** |
| … per-5-min p50 | 1.30 3.03 4.77 5.57 5.19 4.73 | 1.05 0.93 0.91 0.88 0.88 0.91 | **0.47 0.45 0.45 0.39 0.38 0.39** |
| … max-rise | **+4.265** | +0.027 | **+0.004** |

The event window is now **flat at 0.67 m** rather than the tail of a decay, and the ramp window is flat at 0.4 m.

### 4.2 C2 — SIM — **FAIL on one clause of nine, by 0.0096 m**

| clause | bar | measured (`_alc_sim6`) | |
|---|---|---|---|
| hour 1 no worse by > 0.05 | ≤ 0.5100 | **0.4600** (+0.0000) | PASS |
| hour 2 | ≤ 0.3282 | **0.2789** (+0.0007) | PASS |
| **hour 3** | **≤ 0.2367** (= 0.1867 + 0.05; ≤ 0.24 on the bar's own 2-dp reference) | **0.2463 (+0.0596)** | **FAIL by 0.0096 m** (0.0063 on the 2-dp reading) |
| hour 4 | ≤ 0.2239 | **0.1513** (−0.0226) | PASS |
| hour 5 | ≤ 0.2100 | **0.1603** (+0.0003) | PASS |
| hour 6 | ≤ 0.1719 | **0.1366** (+0.0147) | PASS |
| epochs < 0.10 m | ≥ 53 | **81** | PASS |
| WL(1,2) ref-free, last 25 %, `round(e)=0` | ≥ 98 % | **99.1 %** (n = 48 816 — identical to the PBT arm, the DD trace being byte-identical) | PASS |
| emitted | 720/720 | **720 / 720** | PASS |

**Hour 3 is graded literally and it FAILS.**  It is +0.0596 m on a ≤ 0.05 bar, i.e. the bar is missed by **9.6 mm** on a bed whose hour-3 number
is 0.19 m; the same hour carries **4 of the sim's 26 trips**.  The card does not argue it away: one clause of nine fails, so C2 fails.

### 4.3 C3 — telemetry — **PASS (reported)**

Per hour and per satellite (`scripts/alc_level_read.py`):

| bed | h1 | h2 | h3 | h4 | h5 | h6 | total |
|---|---|---|---|---|---|---|---|
| FIELD trips (wl / if) | 3 (0/3) | 25 (8/17) | 19 (4/15) | 27 (8/19) | 19 (4/15) | 13 (3/10) | **106 (27/79)** |
| FIELD refloats | 3 | 25 | 19 | 27 | 19 | 13 | **106** |
| SIM trips (wl / if) | 0 | 1 (1/0) | 4 (0/4) | 5 (1/4) | 8 (0/8) | 8 (0/8) | **26 (2/24)** |
| SIM refloats | 0 | 1 | 4 | 5 | 8 | 8 | **26** |

FIELD per satellite (trips, wl/if): `G05 5(1/4) G06 4(2/2) G10 1(1/0) G11 3(1/2) G12 4(2/2) G13 6(3/3) G14 4(3/1) G15 15(3/12) G17 10(2/8)
G19 16(2/14) G22 6(1/5) G23 10(2/8) G24 14(3/11) G25 4(0/4) G28 1(0/1) G30 2(0/2) G32 1(1/0)` — 17 satellites, spread, no single sat dominating.
SIM: `G10 8(1/7) G13 1(1/0) G15 2(0/2) G23 2(0/2) G24 4(0/4) G25 3(0/3) G29 6(0/6)`.

**The distribution of `|state − level| / σ_level` at the trips** — the reading that says whether the check is skimming its own bar or catching
gross excursions:

| bed | n | min | p25 | p50 | p90 | max |
|---|---|---|---|---|---|---|
| FIELD | 106 | 4.01 | 4.25 | **5.58** | 11.17 | **15.99** |
| SIM | 26 | 4.02 | 4.19 | **4.25** | 4.96 | 5.19 |

and in physical units: FIELD WL trips `p50 0.1418 / p90 0.2418 / max 0.3565` cycles, IF trips `p50 0.5824 / p90 1.5130 / max 2.3228` m; SIM WL
`p50 0.3718 / max 0.5151` cyc, IF `p50 1.1114 / p90 1.8513 / max 2.8041` m.  **The field's median trip is 5.6 σ and its worst is 16 σ** — these
are not marginal.  The sim's are tighter (p50 4.25 σ) and its IF magnitudes LARGER (1.11 m vs 0.58 m), i.e. the sim's own level channel is
noisier, which is consistent with its single FAILing hour.

**The CST cross-check**: `ev_pb` gains kind `refloat_level` at exactly **212 cell-epochs on the field (= 2 × 106) and 52 on the sim (= 2 × 26)**,
AGREE on both.  The full per-cell census, with the PBT arm beside it:

| arm | cells | none | translate_undeclared | translate_disc | reset_unknown_key | **refloat_level** |
|---|---|---|---|---|---|---|
| `_pbt_field6` | 11 618 | 9 934 | 1 358 | 326 | 0 | — |
| **`_alc_field6`** | 11 618 | 9 722 | 1 358 | 326 | 0 | **212** |
| `_pbt_sim6` | 10 800 | 8 092 | 1 989 | 715 | 4 | — |
| **`_alc_sim6`** | 10 800 | 8 040 | 1 989 | 715 | 4 | **52** |

The translate populations are **unchanged cell for cell**, which is its own identity check: the refloats do not perturb what the PBT ledger sees.

**Does the check fire where the accidental resets used to?  NO, and that is the finding.**  C3 asked specifically about TOW 271050 / 271200 —
the baseline's two THERAPEUTIC mass resets.  In `[271000, 271400]` the ALC arm has **ZERO trips**.  What it has instead is **16 trips spread
across the whole hour-4 window** `[270900, 273000]` (14 IF, 2 WL), at 270930 (G15), 271620 (G15 / G23 / G24), 271680 (G19 / G17), 271920 (G22),
272250 (G15 / G24), 272310 (G19), 272430 (G13 wl), 272460 (G12 wl), 272580 (G17), 272880 (G15 / G24) and 272940 (G19), at ratios 4.05–13.49 σ.
**The check does not reproduce the baseline's two mass events; it re-levels continuously, a few satellites at a time, and gets a better result
than either.**

## 5. The per-hour and per-30-min tables, both beds

**FIELD** (`pos_err_3d_m` p50 / p90 by hour):

| arm | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| RF-0hv baseline `_rfpdfix_field6_both` | 5.02 / 5.46 | 2.11 / 3.61 | 0.97 / 1.27 | 1.05 / 2.39 | 0.45 / 0.94 | 3.65 / 5.35 |
| translate `_pbt_field6` | 0.48 / 3.11 | 0.19 / 0.32 | 0.29 / 0.35 | 1.58 / 1.78 | 1.90 / 1.93 | 1.09 / 1.32 |
| **translate + check `_alc_field6`** | **0.43 / 3.11** | **0.16 / 0.28** | 0.31 / 0.39 | **0.49 / 0.61** | **0.68 / 0.71** | **0.49 / 0.69** |
| *4 dp* | *0.4312* | *0.1629* | *0.3097* | *0.4892* | *0.6806* | *0.4857* |

Per-30-min p50 (12 bins from the consumer's own first epoch):

| arm | bins 1–12 |
|---|---|
| baseline | 3.76 5.10 2.31 1.89 1.01 0.91 2.02 0.97 0.44 0.45 3.56 4.31 |
| translate | 1.30 0.29 0.17 0.20 0.32 0.28 1.14 1.74 1.90 1.89 1.15 0.91 |
| **translate + check** | **1.30 0.28 0.16 0.17 0.25 0.33 0.43 0.51 0.67 0.70 0.66 0.45** |

Bins 7–12 are the whole story: `1.14 1.74 1.90 1.89 1.15 0.91` becomes `0.43 0.51 0.67 0.70 0.66 0.45`.  Hour 1's p90 is **unchanged at 3.11 m**
and its max is still **20.79 m at TOW 259230** — the epoch-2 ignition transient PBT §7 item 5 left open is untouched by this card (the window
cannot be full at epoch 2).

**SIM** (p50 / p90 by hour):

| arm | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| RF-0hv baseline `_rfpdfix_sim6_both` | 1.81 / 5.41 | 0.79 / 1.23 | 0.40 / 0.48 | 0.26 / 0.30 | 0.19 / 0.62 | 0.46 / 0.53 |
| translate `_pbt_sim6` | 0.46 / 3.19 | 0.28 / 0.32 | 0.19 / 0.26 | 0.17 / 0.39 | 0.16 / 0.37 | 0.12 / 0.17 |
| **translate + check `_alc_sim6`** | 0.46 / 3.19 | 0.28 / 0.32 | **0.25 / 0.28** | **0.15 / 0.21** | 0.16 / 0.23 | 0.14 / 0.20 |
| *4 dp* | *0.4600* | *0.2789* | *0.2463* | *0.1513* | *0.1603* | *0.1366* |

Per-30-min p50:

| arm | bins 1–12 |
|---|---|
| baseline | 4.46 1.36 1.15 0.56 0.44 0.34 0.26 0.26 0.17 0.47 0.42 0.48 |
| translate | 1.60 0.13 0.23 0.30 0.23 0.14 0.15 0.35 0.30 0.12 0.15 0.10 |
| **translate + check** | **1.60 0.13 0.23 0.30 0.26 0.19 0.15 0.15 0.16 0.16 0.12 0.16** |

Note the p90 column: the check tightens the sim's UPPER tail on hours 4, 5 and 6 (`0.39 / 0.37 / 0.17 → 0.21 / 0.23 / 0.20`) and its epochs under
0.10 m go **53 → 81**, while its p50 on hour 3 moves the wrong way.  Overall p50 over the whole run is **0.1837 → 0.1858 m** — a 2 mm difference
on 720 epochs.

## 6. The hour-4 attribution, and what the mechanism actually did

PBT §7's table, extended with this arm.  `cst_consumer_read --section pair`, max cumulative de-commoned |ΔAMB| over TOW 270900–273000:

| arm | max cum ΔAMB | worst cell | hour-4 p50 | hour-5 p50 |
|---|---|---|---|---|
| baseline (reset-only) `_cst_field6_both` | **+3.2416 m** | G10 L2W | 1.05 | 0.45 |
| translate `_pbt_field6` | −0.4489 m | G12 L2W | 1.58 | 1.90 |
| **translate + check `_alc_field6`** | **+0.6043 m** | G24 L2W | **0.49** | **0.68** |

⚠ **This does NOT reproduce the baseline's 3.24 m, and the card will not pretend it was supposed to.**  The ALC arm's ambiguities move **0.60 m**
in that window — 35 % more than the translated arm and **5.4× LESS than the baseline** — and its position is **2.1× better than the baseline's**
and **3.2× better than the translated arm's**.  The reading that fits all three rows:

- the baseline's 3.24 m was a mass reset **dumping and re-seeding 17–18 cells at once** from one epoch of code, which is both the therapy and,
  per invariant #1's re-coupling clause and `SPEC_slip_relevel_walk`, the mechanism that seeds a fresh zero-q value with station-common iono
  error.  It got hours 4–5 right and hour 6 catastrophically wrong (3.65 m, max-rise +4.265);
- the translated arm preserved everything and paid for the preserved error at hours 4–5;
- **the check re-levels a few satellites at a time, only when the satellite's OWN code level refuses the state by more than 4 σ, and only on the
  20 % of satellite-epochs whose window is intact.**  The total state motion it needs is therefore small — 0.60 m — because it is targeted.
  Hours 4–5 come back AND hour 6's ramp stays dead (max-rise +0.004 m, against the baseline's +4.265).

**The second-order reading, which is the honest limit of what these two runs show.**  79 of the field's 106 trips are IF trips and 24 of the
sim's 26 are, with IF magnitudes p50 0.58 m (field) and 1.11 m (sim).  The IF channel measures the ambiguity's **level in metres** — exactly the
quantity the LEVEL vehicle and `SPEC_level_information_budget` are about — so what this card demonstrates is that **the consumer's own code can
supply a per-satellite level authority the producer's wire does not carry**, at a 4 σ bar, with the wire byte-identical.  Whether that is the
right authority (as against the per-epoch data-derived one §15 of the level card asks for) is NOT established here.

## 7. Verdict

**The pair `pb_step_translate` + `amb_level_check` does NOT join RF-0hv on the literal bar set, because C2 fails.**  It fails on **one clause of
nine, by 0.0096 m**, on the bed where the card's own mechanism has the least to do (26 trips against the field's 106).  Both knobs stay
**default OFF**; no golden, no ledger row, no loader default, `config/refound/*_rf0hv.json` untouched, `config/golden/` untouched.

What the runs establish anyway, and it is the largest consumer movement this program has measured on the field bed:

- **The re-coupling edge was the whole of PBT's residual.**  PBT §8 said "what blocks promotion is not the mechanism; it is that a preserved
  ambiguity is never re-validated", and named this as the next card.  Adding the re-validation and changing NOTHING else takes hours 4–5 from
  **1.58 / 1.90 → 0.49 / 0.68 m**, which is better than the pre-translate baseline on hour 4 (1.05) and within 0.23 m of it on hour 5 (0.45) —
  while KEEPING the hour-1/2/6 wins the translation bought.  **Five of six field hours are the best this program has measured on this bed, and
  the sixth is +0.023 m.**
- **A consumer can re-level its own ambiguities from its own observations, with the producer's wire byte-identical.**  Not one byte of
  `products.txt` moves on either bed — `#` lines and `R` records included — and every solve-side artefact and the whole `rf2d_grade` wire read
  are unchanged. The entire effect is inside the consumer.
- **The instrument is honest at both edges and cross-checks itself three ways**: the engine ledger against the harness drain (`AGREE`), the
  refloat count against the CST `refloat_level` cells (212 = 2 × 106, 52 = 2 × 26), and the PBT ledger unchanged cell for cell.
- **The response is TARGETED, not therapeutic-by-accident.**  It does not fire where the baseline's mass resets were; it fires 106 times across
  17 satellites at a median 5.6 σ, moving 0.60 m of state where the baseline moved 3.24 m, and gets a better answer than either.

Open items:

1. **SIM hour 3 (+0.0596 m) is unattributed.**  It carries 4 of 26 trips and its own 30-min bins move `0.23 0.30 → 0.23 0.30` (bins 5–6 are
   `0.23 0.14 → 0.26 0.19`), so the loss is in the second half of the hour.  The cheapest next read is the sim arm in `observe` mode: it isolates
   whether the loss is the refloats' own cost or the window restarts' side effects, and it is one run.
2. **The check is live on only 20 % of satellite-epochs (1183 checks of 5799 samples, 1159 restarts).**  The restart rule is deliberately
   conservative — ANY ambiguity write and ANY variance at or above `ambiguity_std²` drops the window — and on a lineage that translates 1684
   cells over 720 epochs that is most of the sky most of the time.  **A band-SCOPED restart** (restart only the band whose ambiguity was written,
   and keep a one-band window) is the obvious next increment and is NOT built.  Note the direction of the finding: the check is already worth
   >1 m at hours 4–5 on a fifth of the sky.
3. **`observe` was not run on either bed** (budget).  §4.3's census is the `act` arm's own, which includes the refloats' feedback; a pure
   observatory would differ.
4. **The `k = 4.0` and `window = 20` defaults have had ONE arm each.**  Nothing here calibrates them — the trips sit at a median 5.6 σ (field)
   and 4.25 σ (sim), so the field population would survive a considerably higher k and the sim's would not, which is itself a hint about where
   the sim's hour-3 cost comes from.  **No knob was moved to make a bar.**
5. **The IF channel carries 74–92 % of the trips and it is the weaker of the two.**  At n = 20 and code σ 0.5 m its minimum detectable IF level
   is 1.33 m against the WL channel's 0.37 cycles.  A longer IF window (or a separate window per channel) would lower that bar at no cost to WL;
   not built, not pre-registered.
6. **The epoch-2 ignition transient (20.79 m field) is untouched**, as predicted — the window cannot be full at epoch 2.  PBT §7 item 5's
   first-arc guard is still the shape and is still not built.
7. **Neither ledger is in the RT snapshot** (v1 format unchanged).  After a warm restore every window starts empty, so the first `window_ep`
   epochs past a seam carry no check at all — the same property cards CC and PBT have, and the same one a seam card would have to close.
8. **The check is inert on a BSSD consumer** by construction (the differenced rows are labelled `<sat_i>-<sat_p>` and are skipped rather than
   attributed to one satellite), and inert on an NL/WL basis (no per-(sat, signal) ambiguity key).  Stated, not fixed.
9. **`scripts/rt_seam_verify.py` is unchanged at 6 FAILs** — `reactToGenerationChange`, `resetOnPhaseBiasDisc`, `reinflateOnCodeBiasDisc`,
   `reinflateOnClockDisc` (CST's inserted event-recorder lines) and `resolveAmbiguities`, `initAmbiguityResolver` (pre-existing).  This card adds
   two lines to `resetOnPhaseBiasDisc`, which was already among them, so it contributes **0 new FAILs**.  Still the known hygiene item.

## 8. Runs and artefacts

- `runs/_alc_smoke_off` — OFF smoke, record stream `333DCB0871E1F5BC` (canonical), whole file `B05F26B1CD0540DB`.
- `runs/_alc_field6` / `runs/_alc_sim6` — the two arms, 720/720 each, exit 0, wall 274 s for the pair.
- `runs/_pbt_field6` / `runs/_pbt_sim6` (the translate baseline), `runs/_rfpdfix_field6_both` / `_rfpdfix_sim6_both` (RF-0hv baseline),
  `runs/_cst_field6_both` (the reset-only instrument bed) — read only, not re-run, left in place.
- Exe `build/x64-Release/gnss_processor_alc.exe` `87923C21E731DDC8`.
- New gtest: `tests/test_amb_level_check.cpp` (`AmbLevelCheckTest`, 12 cases).
- New grader: `scripts/alc_level_read.py` (trace-only — the per-hour / per-satellite / per-σ breakdown the run-end line cannot carry, plus the
  CST `refloat_level` cross-check).  `scripts/cc_arm_grade.py`, `scripts/cst_consumer_read.py`, `scripts/rf2d_grade.py` and
  `scripts/simnet_amb_integer.py` cover everything else and are unchanged.

## 9. 2026-09-16 — correction to the window-restart guarantee

The variance fallback described in §1 is insufficient: a same-epoch measurement
update can contract the reset variance before this post-update check runs. Some
slip and KF recovery paths also bypassed the explicit notification. Per-epoch
notifications could be lost during epochs without usable samples.

[ALC-ARC](ALC-ARC_reset_generation_2026-09-16.md) closes those gaps with persistent
ambiguity generations and immediate window clearing on explicit notifications.
Eight new engine regressions exercise actual reseeding and Kalman updates; all
82 focused tests pass. ALC-OFF products are byte-identical. The existing results
above remain historical results of the original implementation; six-hour accuracy
campaigns on the corrected implementation are deferred, and nothing is promoted.

## 10. 2026-09-17 — correction to the statistical interpretation

[ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md) audits the statistical claims
above without changing k, window length, floors or runtime behavior. The scale
is an estimated standard error: with iid Gaussian noise and inactive floors,
the single-channel ratio follows Student t(19), not a standard normal. The
two-sided probability at 4 is 0.07662%, approximately twelve times the Gaussian
tail. Production-detector experiments give 0.1504% either-channel rejections
per independent noise-only window; stationary code correlation phi=0.5 raises
that to 7.5084% with the same marginal noise and correct ambiguities.

Persistent code bias can give exactly the same two statistics as ambiguity
error. A phase-only receiver clock also survives the combinations. Therefore
the historic "4 sigma" ratios do not establish ambiguity correctness, identify
the defect's cause, or give a calibrated false-refloat probability. The quoted
"minimum detectable" levels are nominal thresholds without a specified power.
Overlapping windows, trip clears, engine restarts and act feedback also prevent
reading historical event counts as independent false-alarm trials.

The historical position results above remain unchanged. Four new production
statistical tests pass; the full focused selection is 40 passing tests. Saved
PBT traces show a more persistent upper correlation tail in field than sim,
but lack the monitor population and truth labels for field calibration. The
next step is observation-only telemetry and null/power calibration of the
complete event policy, before any threshold/window tuning or promotion.

## 11. 2026-09-17 — reconsider the band-scoped-window proposal

[ALC-BAND](ALC-BAND_window_scope_review_2026-09-17.md) withdraws section 7's
claim that keeping a one-band window is the obvious next increment. Both
existing WL and IF samples depend on both selected ambiguity arcs; a reset
of either invalidates both existing histories for the new pair. Independently
averaging raw band contributions over different times also breaks the common
geometry/ionosphere cancellation. Retain the paired-generation safeguard.

A per-ambiguity direction can be constructed from its own phase and both
simultaneous codes, or projected from the existing paired samples. That is a
viable different observable, with strongly amplified and correlated code
noise; it needs explicit dependencies, covariance, null/power calibration and
a separately justified response. It is not a bookkeeping-only optimization.
Narrowing satellite-wide notifications for events outside the selected pair
is a separate potential change with no measured benefit on the current bed.

Four new numerical witnesses pass, and the focused selection totals 44 passing
tests. No runtime windows or settings changed. Complete the statistical
calibration work first; the historical low coverage does not by itself justify
more checks or band-scoped refloats.
