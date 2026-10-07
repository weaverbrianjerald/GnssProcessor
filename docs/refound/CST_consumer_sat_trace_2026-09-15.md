# CST: the per-satellite closed-loop consumer trace (2026-09-15)

**STATUS**

- **id:** CST
- **topic:** consumer
- **status:** open
- **summary:** Both hour-6 events are the consumer's own ambiguity reset, and 117 of its 390 declaration-driven resets answer EMPTY declarations; the instrument `network.trace_consumer_sat` was later ADOPTED.
- **dates in the text:** 2026-09-15
- **knobs / posture:** `network.trace_consumer_sat` (built, default OFF, solve byte-identical); pre-registered fixes `network.closed_loop.pb_disc_carry` (section 7.1), `network.disc_mark_scope` (7.2), `network.disc_jump_test_decommon` (7.3); RF-0hv field posture
- **hashes named in the text:** exe `build/x64-Release/gnss_processor_cst.exe` `5D8AA185319293F0` (main `0c1a66e` + this card's diff); OFF smoke `9ADD1D3AB9D70657`; trace `45C18707D70BDBDE`; Debug `gnss_tests.exe` 2228 passed + 1 skipped of 2229
- **run dirs named in the text:** `runs/_cst_field6_both`, `runs/_cst_smoke_off`; baseline `runs/_rfpdfix_field6_both`; reader `scripts/cst_consumer_read.py`; gtest `tests/test_consumer_sat_trace.cpp`
- **source:** [EV6](EV6_hour6_decouple_event_2026-09-15.md) section 9 item 1 (the owed per-satellite consumer instrument)

**Outcome.** The card builds the per-satellite consumer trace EV6 lacked and reads both hour-6 events with it, the solve byte-identical. At TOW 276810 a real declaration resets 11 of 12 cells and the solution falls onto the code; at TOW 279030 a Q3 clock decouple's `R` records reset 10 of 16 cells while the emitted phase bias moved at most 0.011 m, and the ramp after it is the consumer's own ambiguity and slant-iono states drifting together (r = 0.951). The three pre-registered fixes each FAIL their bars ([CC-arm](CC-arm_pb_disc_carry_rf0hv_2026-09-15.md), [PBT](PBT_consumer_pb_translate_2026-09-15.md), [JT](JT_pb_jump_test_decommon_2026-09-15.md)). The instrument was ADOPTED 2026-09-19 ([ALC2](ALC2_restart_policy_adoption_2026-09-19.md)); its seam-verifier hygiene row stays open ([OPEN_ITEMS_A](../status/OPEN_ITEMS_A_current_program.md)).

**Status as recorded (2026-09-15):** open — **one instrument built (`network.trace_consumer_sat`, bool, default OFF), one field run spent, no knob promoted, no golden and no ledger touched.**
EV6 §9 item 1 named the blocker: `logs/closed_loop_trace.csv` carries position, `n_sats` and the AR columns and nothing else, so a consumer-side
float re-convergence could not be separated from the slant-iono half of the RF-0b applied pair and an undeclared wire step could not be seen
landing.  That instrument now exists — `logs/closed_loop_sat_trace.csv`, one row per (satellite, raw phase signal) per consumer epoch carrying
the APPLIED clock / code-bias / phase-bias corrections as the consumer's own gated reads return them, the producer's discontinuity COUNTERS as
the store presents them, **what the consumer DID with each declaration**, the pre-fit innovations, the post-fit residuals, and the ambiguity +
slant-iono STATE values with their variances.  It names both hour-6 events in one pass, and the answer is the same mechanism twice.
**The consumer resets an ambiguity for EVERY declaration it sees — 390 declared cell-epochs, 390 resets, 1:1 — and 117 of those 390 (30.0 %)
are declarations the emitted phase bias did not follow (|de-commoned Δ| < 0.020 m).**  At **TOW 276810** the declaration is REAL: 11 of the
consumer's 12 cells are declared and reset on the run's largest step at the consumer (G12/L2W **−5.000 m** de-commoned), ambiguity variance
re-inflates 2.75–5.56×, the post-fit phase residuals go to **exactly 0.0000**, N and U each jump **+0.49 m** in that epoch and the solution
walks to 3.16 m by 276900 — the consumer threw away six satellites' worth of converged phase and fell back on code.  At **TOW 279030** the
declaration is EMPTY: a **20-satellite Q3 clock-anchor mass decouple** fires, its `R` records bump the PHASE-BIAS counter of every decoupled
satellite, and the consumer resets exactly the five-satellite intersection with its own sky (**{G12, G24, G25, G29, G32}**, 10 of 16 cells)
while the emitted phase bias moved at most **0.011 m**.  The ramp that follows is the consumer's post-reset re-convergence walking the
ambiguity ↔ slant-iono null: over 279240–280260 the wire is silent (0 declarations, 0 resets, max undeclared |de-commoned ΔPB| **0.046 m**)
while the consumer's own ambiguity states travel to **9.71 m** and its slant-iono states to **3.33 m** with Pearson **r = 0.951**, against a
matched quiet-hour control of **0.78 m / 0.85 m at r = 0.226**.  EV6's refutation stands and is sharpened: the decouple's *wire values* are
not the consumer's channel — its `R` RECORD is.

## 1. The bed, the arm, the identity

The RF-0hv FIELD baseline re-run with ONE knob added.  `runs/_cst_field6_both` = `runs/_rfpdfix_field6_both/arm.json` with the run paths
repointed and `network.trace_consumer_sat: true` — verified programmatically to be identical to its source apart from the run-dir name and
that key.  DOY-129, 59 stations, GPS L1/L2, 6 h cold, TOW 259200–280800 at 30 s, 720/720 emitted.

| artefact | `_rfpdfix_field6_both` | `_cst_field6_both` | |
|---|---|---|---|
| `products.txt` | `4F513E762F292337` | `4F513E762F292337` | IDENTICAL |
| `logs/closed_loop_trace.csv` | `F9D7FB7D53298083` | `F9D7FB7D53298083` | IDENTICAL |
| `logs/network_epoch_status.csv` | `939727D60970F6B5` | `939727D60970F6B5` | IDENTICAL |
| `logs/closed_loop_sat_trace.csv` | — | `45C18707D70BDBDE` | NEW, 11 618 rows |

Exe `build/x64-Release/gnss_processor_cst.exe` sha256[:16] **`5D8AA185319293F0`** (source: main `0c1a66e` + this card's diff).  OFF smoke
`runs/_cst_smoke_off` (= `runs/_rf2d_smoke_off/smoke.json`, paths only): `products.txt` sha256[:16] **`9ADD1D3AB9D70657`** — the canonical hash.
Debug `gnss_tests.exe`: `2229 tests from 294 test suites ran`, `[  PASSED  ] 2228 tests.`, `[  SKIPPED ] 1 test` (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`).
Run-end counter (invariant #4): `per-sat consumer trace : 11618 rows over 720 consumer epoch(s)`.

**Instrument:** `python scripts/cst_consumer_read.py <run_dir> [--tow-min T --tow-max T] [--section overview|steps|common|impact|states|resid|drift|events|census|pair] [--sat G23] [--step-m M] [--var-ratio R] [--empty-m M] [--min-reset N] [--top N]`.
Trace-only: it launches nothing, writes nothing but stdout and does not plot.

## 2. The instrument

### 2.1 Schema — `logs/closed_loop_sat_trace.csv`

```
week,tow,sat,sig,code_sig,elev_deg,n_sig,
clk_m,cb_m,pb_m,
clk_disc,cb_disc,pb_disc,
ev_pb,ev_cb,ev_clk,
innov_code_m,innov_phase_m,
res_code_m,res_phase_m,
amb_m,amb_var,iono_m,iono_var
```

**One row per (satellite, RAW PHASE SIGNAL), not per satellite with keyed signal columns** — that is what the consumer's state layout makes
EXACT: the ambiguity key is `<sat>_<sig>` (`StateKeys::makeAmbiguityKey`), the satellite phase and code biases are per-signal in the SSR
store, and the residual rows of `EpochMeasurement` are labelled (`satellite_ids`, `signal_types`) = (sat, RINEX code).  `code_sig` is the
band+attribute-paired CODE signal (`L1W → C1W`), because the satellite code bias and the slant ionosphere are the RF-0b pair a consumer must
apply TOGETHER and they belong on one row.  Per-satellite quantities (clock, slant iono, elevation) repeat across a satellite's rows and
`n_sig` says how many that is.

Three rules the file obeys, each of which the read depends on:

- **Absent is BLANK, never 0.**  An absent phase bias and a zero phase bias are different wires.  Metres at 6 dp (a 0.5 mm emitted move is
  still visible — EV6 graded exactly such a number at the +14.36 m G22 S-transform); variances at 6 significant digits.
- **`ev_pb` / `ev_cb` / `ev_clk` are `none` when nothing happened** — "the consumer did nothing with the wire this epoch" is a measurement.
  Values: `reset_disc`, `reset_flip`, `carry_disc`, `carry_flip`, a named carry refusal, `reset_gen`, `reinflate_disc`, `reinflate_flip`,
  `reinflate_isb`; `;`-joined if a key took more than one.
- **The buffer is cleared at the TOP of `processEpoch`**, before any admission gate, so an epoch the gates deferred writes NO rows rather
  than a stale repeat of the previous one.

**Deliberately absent: the gain-weighted position effect of a row.**  That needs the Kalman gain `K = P⁻HᵀS⁻¹`, and neither `K` nor the prior
`P⁻` of the committed update survives `processEpoch` (only the posterior does), so reconstructing it would be an O(m³) re-solve per epoch
against a covariance that is no longer the one the update used — not cheap and not exact.  The PRE-fit innovation is both, and is what the row
carries instead.  ⚠ It carries the consumer's receiver clock (≈ −98 m on this bed), so it is read de-commoned or not at all; the POST-fit
residual has the clock absorbed and is quoted raw.

### 2.2 Where it lives

| piece | file:line |
|---|---|
| row struct + header constant + formatter contract | `include/realtime/ConsumerSatTrace.hpp` |
| pure CSV formatter (the unit-testable kernel) | `src/core/realtime/ConsumerSatTrace.cpp:33` `formatConsumerSatRow` |
| harvest seam (consumer-side observability) | `src/core/realtime/RealTimeEngineSsrDiag.cpp:162` `RealTimeEngine::traceConsumerSat_` |
| event recorder | `src/core/realtime/RealTimeEngineSsrDiag.cpp:155` `RealTimeEngine::noteConsumerSatEvent_` |
| call site: LAST thing the epoch does (post-update, post-constraints, post-AR) | `src/core/realtime/RealTimeEngine.cpp:537` |
| buffer clear, above every admission gate | `src/core/realtime/RealTimeEngine.cpp:399` |
| reaction sites (global reset / reset / carry / re-inflate) | `src/core/realtime/RealTimeEngineSsrBreaks.cpp:54, 132, 150, 228, 233, 242, 300, 320, 419` |
| stream ownership: open + header, close | `src/core/logging/NetworkRunTraces.cpp:159`, `:2194` |
| writer called from the consume step | `src/core/logging/NetworkRunTraces.cpp:982` `writeConsumerSatRows` |
| hook, beside the `closed_loop_trace.csv` write | `src/core/filter/NetworkEmitStore.cpp:764` |
| arming (before the product-routing branch, so the BASELINE arm is instrumented too) | `src/core/app/ApplicationNetworkSetup.cpp:1314` |
| counters | `include/app/NetworkRunContext.hpp:3055` `n_cst_epochs` / `n_cst_rows` |
| run-end readout | `src/core/filter/NetworkTeardown.cpp:5649` |
| knob + Doxygen | `include/config/GnssConfigTypes.hpp:2369`; parse `src/core/config/GnssConfigLoadNetwork.cpp:2345`; sink check `:11354`; manifest echo `src/core/logging/NetworkRunTraces.cpp:1569` |
| gtest (schema pin, 6 cases, `ConsumerSatTraceSchemaTest`) | `tests/test_consumer_sat_trace.cpp` |

MEASURE-ONLY by construction: every read is a const lookup into state the epoch has already committed, and the three gated artefacts above are
byte-identical to the baseline.

⚠ **`scripts/rt_seam_verify.py` now reports 4 FAILs it did not before**, because this card inserts a guarded one-line `noteConsumerSatEvent_`
call into four RELOCATED seam bodies (`reactToGenerationChange`, `resetOnPhaseBiasDisc`, `reinflateOnCodeBiasDisc`, `reinflateOnClockDisc`).
That is deliberate and the verifier is doing its job: those bodies are no longer character-exact against the frozen pre-cut source.  Two
further FAILs (`resolveAmbiguities`, `initAmbiguityResolver`) are PRE-EXISTING — neither `RealTimeEngineEstimate.cpp` nor
`RealTimeEngineInit.cpp` is in this card's diff.  The frozen goldens are not in the working tree (archived 2026-08-12); they were copied back
from `D:\GnssProcessor_archive\2026-08-12\runs\rtseam_goldens\` to run the check.

### 2.3 Two traps the file itself exposes

1. **`n_sats` in `closed_loop_trace.csv` is NOT the consumer's satellite count.**  It is `cit->second.size()` — the RAW observation records
   handed to the consumer, all constellations, before system filtering and modeling.  It reads **47–51** for the whole run while the
   consumer's GPS-only solution carries **6 / 8 / 10** satellites (min / median / max).  Every consumer-geometry statement made from that
   column is wrong by a factor of six.
2. **`network.emit_clock_disc: true` DOES reach the in-process consumer.**  EV6 §9 item 2 measured every `C` record at 3 fields and asked
   whether the knob is inert; `clk_disc` is populated in this trace (values 0–7) and `ev_clk = reinflate_isb` fires on 114 cell-epochs over 14
   epochs, so the defect is SERIALIZATION-only — a file-replay consumer is blind, the in-process one is not.  ⚠ The reaction is nevertheless
   structurally empty here: `reinflateOnClockDisc` re-inflates ISBs, `systemToIsbName(GPS)` is `""` and the consumer is GPS-only, so
   `present_isb` is empty and nothing is re-inflated.  The trace records that the DECLARATION arrived and that the consumer's only possible
   response fired; on this posture that response is a no-op.

### 2.4 The one piece of algebra (standing invariant #5)

Every epoch-to-epoch per-satellite delta in this card is **de-commoned** by the median change over the cells present at BOTH epochs, because
the network-common part of a correction is absorbed by the consumer's own receiver-clock state and grading it raw confounds benign frame
motion with a real step.  A cell whose availability changes between two epochs is skipped, never differenced.  **Per-(sat,signal) quantities
take the median WITHIN each signal**, not across the epoch: L1W and L2W phase biases do not share a common mode (the band-differential part is
absorbed by the receiver's code-bias / phase-clock states, the band-common part by the receiver clock), and one median over a mixed L1/L2
population leaks a band-differential motion into every cell.  Clock and slant iono are per-SATELLITE and are collapsed to the satellite first,
so a two-band satellite is not weighted twice in the median.

## 3. THE RUN-WIDE CENSUS — the consumer resets 1:1, and 30 % of what it resets for is empty

`--section census`, whole run, 11 578 cell-epoch pairs:

| quantity | value |
|---|---|
| cell-epochs with an ambiguity RESET | **390 (3.37 %)** |
| cell-epochs the producer DECLARED (phase-bias disc counter changed) | **390** |
| declared → reset | **1 : 1, no exceptions** |
| DECLARED \|de-commoned ΔPB\|: p50 / p90 / max | **0.0619 / 0.3273 / 5.0001 m** |
| **of the 390, moved < 0.020 m — an EMPTY declaration** | **117 (30.0 %)** |
| UNDECLARED cell-epochs (11 188): p50 / p90 / max | 0.0007 / 0.0052 / **1.4089 m** |
| `ev_cb reinflate_disc` / `ev_pb reset_disc` / `ev_clk reinflate_isb` | 404 on 48 ep / 390 on 43 ep / 114 on 14 ep |

Two readings, and both matter.

**(a) The consumer has exactly one response to a declaration, and it is total.**  `resetOnPhaseBiasDisc`
(`RealTimeEngineSsrBreaks.cpp:104`) calls `state_manager_->resetAmbiguity(key, config_.getAmbiguityStd())` — the arc's whole accumulated phase
information, discarded.  Card CC's declared-step CARRY (`processing.ssr_phase_bias_disc_carry`, driven by
`network.closed_loop.pb_disc_carry`) is BUILT and default OFF; it is **not armed on this lineage**, and the 390 `reset_disc` events with zero
`carry_disc` are the proof.

**(b) 117 of those 390 resets bought nothing**, because the emitted phase bias the consumer reset for had not moved.  This is EV6 §4's
producer-side finding — "the median reversal at the very next epoch is 91 %, 102 of 130 reverse by ≥ 50 %, and 0 of 130 returns carries a
declaration" — measured at its landing site.

**Where the empty declarations come from, with file:line.**  `markSatDiscontinuity` (`NetworkProductEmitter.cpp:105`) inserts the satellite
into `forced_bump_sats_`, and `satForced(s.sat)` then forces a counter bump on the CLOCK (`:313`), the CODE BIAS (`:362-363`) **and the PHASE
BIAS (`:408-409`) alike**.  One `R` record declares all three channels of a satellite.  So a Q3 clock-anchor decouple — a purely CLOCK event,
`NetworkClockAnchor.cpp:394-402` → `markSatDiscontinuity` — tells every consumer of that product that the satellite's PHASE BIAS re-realized.
**15 of the 21 post-ignition consumer-reset epochs are co-incident with a `cta_decouple` epoch, and 57 of the 109 post-ignition
(satellite, epoch) resets are on a satellite the Q3 anchor decoupled that epoch (52.3 %).**

**The mass-reset impact table** (`--section impact`, post-ignition; `before`/`after` = consumer 3D p50 over the 15 min on each side, `peak` =
max 3D in the 30 min after) shows the reset is a cost, not a verdict — most are survivable:

| tow | cells | reset | max\|ΔPB\| | before | after | peak | cost |
|---|---|---|---|---|---|---|---|
| 262980 | 14 | 12 | 0.7531 | 5.049 | 2.411 | 4.888 | −2.638 |
| 266910 | 18 | 11 | 0.1596 | 1.377 | 1.008 | 1.068 | −0.370 |
| 268860 | 18 | 16 | 0.0340 | 0.626 | 1.007 | 2.461 | +0.381 |
| 269640 | 20 | 11 | 0.2404 | 1.007 | 1.468 | 2.482 | +0.461 |
| 271050 | 18 | 17 | 0.2312 | 2.357 | 1.184 | 2.481 | −1.173 |
| 271200 | 18 | 18 | 0.0062 | 2.364 | 0.982 | 2.301 | −1.382 |
| 275070 | 14 | 14 | 0.0590 | 0.438 | 0.449 | 2.231 | +0.012 |
| 276150 | 12 | 12 | 0.0012 | 0.405 | 0.510 | 4.012 | +0.105 |
| **276810** | **12** | **11** | **4.9472** | **0.449** | **3.313** | **4.012** | **+2.864** |
| 277560 | 14 | 11 | 0.2402 | 3.035 | 3.701 | 3.767 | +0.666 |
| 278520 | 14 | 14 | 0.5619 | 3.701 | 1.364 | 5.636 | −2.337 |
| **279030** | **16** | **10** | **0.0099** | **2.510** | **1.925** | **5.636** | −0.586 |

Every mass reset costs a transient (peak 2.2–5.6 m in the following half hour, even the ones whose `after` is better than their `before`).
What separates the two damaging ones is what happens NEXT — §4 and §5.

## 4. WINDOW A — TOW 276600–277500: the step is a DECLARED mass reset of the whole sky

**The consumer's sky is SIX satellites** (12 cells): `{G06, G12, G19, G24, G25, G32}` × `{L1W, L2W}`.

**The declaration and the response, 276780 → 276810** (`--section steps --step-m 0.0`; ΔPB de-commoned per signal):

| sat | sig | ΔPB (m) | declared | Δamb (m) | amb_var ratio | consumer event |
|---|---|---|---|---|---|---|
| G12 | L2W | **−5.000** | YES | +5.358 | 3.36 | `reset_disc; reinflate_disc; reinflate_isb` |
| G12 | L1W | **−2.746** | YES | +3.235 | 5.09 | `reset_disc; reinflate_disc; reinflate_isb` |
| G25 | L2W | −1.769 | YES | +1.844 | 2.75 | `reset_disc` |
| **G32** | **L1W** | **+1.409** | **NO** | −0.908 | **0.97** | **— (nothing)** |
| G32 | L2W | +0.976 | YES | −0.573 | 1.01 | `reset_disc` |
| G19 | L1W | +0.587 | YES | −0.116 | 5.26 | `reset_disc; reinflate_disc` |
| G19 | L2W | +0.487 | YES | −0.164 | 4.12 | `reset_disc` |
| G25 | L1W | −0.474 | YES | +0.703 | 3.49 | `reset_disc` |
| G06 | L1W | +0.191 | YES | −0.085 | 5.24 | `reset_disc` |
| G24 | L1W | −0.191 | YES | +0.423 | 5.56 | `reset_disc; reinflate_disc; reinflate_isb` |
| G06 | L2W | −0.165 | YES | +0.071 | 4.40 | `reset_disc` |
| G24 | L2W | +0.165 | YES | −0.071 | 3.75 | `reset_disc; reinflate_disc; reinflate_isb` |

**Eleven of twelve cells declared and reset in one epoch.**  The `amb_var` ratio is **2.75–5.56** on every reset cell and **0.97 / 1.01** on
G32 — the variance re-inflation is the instrument's own discriminator between a declared reset and a silent float move, and it separates the
population with no overlap.  The slant-iono variances re-inflate with them (ratio 1.5–2.95).

**The position, and why it moves the way it does.**  The consumer's post-fit phase residuals at 276810 are **exactly 0.0000** (p50 and max
both), and again at 276840 (max 0.0001): a freshly re-inflated ambiguity absorbs the entire phase misfit, so the phase rows stop constraining
and the epoch is solved on the code.  `p50|res_code|` is 0.154 m at 276810 — the code is fine; it is simply now the only thing holding the
solution.  That is the +0.492 m N and +0.485 m U single-epoch jump (276780 N −0.490 / U −0.112 → 276810 N +0.002 / U +0.373), and it is why
3D then runs 0.383 → 0.989 → 2.231 → **3.156 m** by 276900 and 3.953 m at 277500 rather than snapping back: six satellites' worth of
ambiguity is re-converging from scratch with no integer aid (`ar_gate = not_run` all run — this consumer is FLOAT).

**Two satellites of the six were in the Q3 decouple set** ({G12, G13, G24, G10, G23, G26, G28, G31, G29} ∩ sky = {G12, G24}); the other four
declarations came from the producer's own phase-bias emission tests, which is the network-wide phase-bias re-realization EV6 measured
independently (19 satellites > 0.5 m de-commoned IF-phase, max 3.638 m).  **So at 276810 the declaration is REAL and the reset is the correct
response to it.**  The defect is not that the consumer reacted; it is that its only reaction is total.

**The one cell nothing protected, and the reason is a FRAME-BLIND declaration test.**  G32/L1W took a **+1.409 m de-commoned, UNDECLARED**
step while its own L2W partner WAS declared (`pb_disc` 29 → 30) and reset.  Its `amb_var` ratio is 0.97 — no re-inflation — so the metre-scale
step went straight into a live ambiguity.  That is the largest undeclared step anywhere in the run (the 11 188 undeclared cell-epochs have p50
0.0007 m and p90 0.0052 m).  The arithmetic is exact and the emitter behaved to specification:

| | raw ΔPB 276780 → 276810 (m) | per-signal median (m) | de-commoned (m) | `disc_threshold_frac_cycles: 0.25` × λ | declared |
|---|---|---|---|---|---|
| G32 **L1W** | **−0.0421** | −1.4510 | **+1.4089** | 0.25 × 0.1903 = **0.0476** | **NO** (0.0421 < 0.0476) |
| G32 L2W | −0.3436 | −1.3198 | +0.9762 | 0.25 × 0.2442 = 0.0611 | YES |

The whole L1W column re-realized by ≈ **−1.45 m** that epoch (per satellite: G12 −4.197, G25 −1.925, G24 −1.642, G06 −1.260, G19 −0.864,
G32 −0.042), and G32 was the satellite that barely moved IN THAT FRAME — which is precisely why, once the consumer's receiver clock absorbed
the −1.45 m common mode, G32's L1W ambiguity saw **+1.409 m, 29.6× its own declaration bar**.  The emitter's jump test
(`NetworkProductEmitter.cpp:408-409`, threshold `discThresholdM`, `:87-99`) thresholds the RAW emitted value; what lands in a consumer's
ambiguity is the DE-COMMONED one.  Same family as the known rx-CB inject-gate frame-blindness (open defect, 2026-09-13).  §7.3 pre-registers
the fix.

## 5. WINDOW B — TOW 279000–280800: the ramp is a post-reset re-convergence, and its trigger is an EMPTY declaration

### 5.1 The trigger: TOW 279030, a 20-satellite Q3 decouple declaring a phase-bias step that does not exist

`network_events.csv` at 279030 carries **`CLK_ANCHOR cta_decouple` ×20** (17 at pass 0, 5 at pass 1, 2 at pass 2 — the 3-pass cap again) plus
20 `cta_disc_landed`.  The consumer's sky that epoch is `{G06, G11, G12, G19, G24, G25, G29, G32}`; the decoupled set intersects it in
`{G12, G24, G25, G29, G32}`.

**The consumer reset exactly those five satellites, both signals, 10 of 16 cells — and nothing else.**  The emitted phase bias moved, over the
whole epoch, **at most 0.011 m de-commoned** (every cell ≤ 0.010 m on ΔPB, ≤ 0.002 m on ΔCB, ≤ 0.07 m on ΔCLK).  This is the cross-channel
over-declaration of §3(b) caught in the act: a CLOCK guard's withdraw-and-re-solve cost the consumer five satellites' phase memory.

### 5.2 The ramp itself: the wire is silent and the consumer's own states run away

`--section census` over **279030 → 280800** (950 cell-epoch pairs): **0 declared, 0 reset**, max undeclared |de-commoned ΔPB| **0.0461 m**.
Over the ramp onset 279240–279450 specifically, `--section steps --step-m 0.05` returns **zero cell-epochs** on any channel.  There is no wire
event to attribute the ramp to, from the consumer's own reads — which is EV6 §5's conclusion reproduced from the other side of the link.

What DOES move is the consumer (`--section drift` / `--section pair`, cumulative de-commoned over 279240–280260, 34 epochs, consumer
1.08 → 5.64 m), against a matched 34-epoch quiet control in hour 5 (274200–275190, consumer ≈ 0.45 m flat):

| quantity | RAMP | CONTROL h5 | ratio |
|---|---|---|---|
| max cumulative \|ΔPB\| (the WIRE) | 0.250 m | 0.024 m | — (both negligible) |
| max cumulative \|Δambiguity\| (the CONSUMER) | **9.710 m** | **0.782 m** | **12.4×** |
| max cumulative \|Δslant-iono\| (the CONSUMER) | **3.326 m** | **0.845 m** | **3.9×** |
| Pearson r(cum Δamb, cum Δiono) over the cells | **0.951** | **0.226** | — |
| slope | 0.371 m/m | 0.261 m/m | — |
| `p50 res_code`, first → last epoch of the window | **0.262 → 0.701 m** | 0.189 → 0.132 m | — |
| `p50 res_phase`, window median / max | 0.0044 / 0.0057 m | 0.0011 / 0.0032 m | 4× |

The per-satellite drift is coherent and paired — the ambiguity and the slant iono of the SAME satellite move together, in sign and in rank:

| sat | sig | cum ΔPB | cum Δamb | cum Δiono (per sat) |
|---|---|---|---|---|
| G06 | L2W | −0.0034 | **+9.700** | **+3.326** |
| G06 | L1W | −0.0045 | +7.683 | +3.326 |
| G11 | L2W | −0.0886 | +7.448 | +2.013 |
| G11 | L1W | −0.0064 | +5.849 | +2.013 |
| G29 | L2W | −0.0027 | −4.402 | −2.234 |
| G32 | L2W | +0.2502 | −4.314 | −0.954 |
| G25 | L2W | +0.0566 | −4.045 | −1.293 |
| G19 | L2W | −0.0029 | +3.143 | +1.356 |
| G24 | L2W | −0.4534 | +1.305 | +1.045 |
| G12 / G28 | both | ≤ 0.17 | ≤ 0.10 | +0.358 / −0.697 |

**The consumer has no ionospheric authority on this posture, and that is measured, not inferred.**  The arm sets
`network.closed_loop.consume_network_iono: true`, but the supply block is gated on a live SH model (`NetworkEmitStore.cpp:545`:
`cl.consume_network_products && cl.consume_network_iono && sh_model`) and RF-0g runs `estimate_iono_sh: false` with
`w6_iono_constraint: none`.  The run logs **zero** `supplied SH-VTEC slant iono` lines in 720 epochs.  So the per-satellite slant-iono states
are a free random walk whose split against the ambiguity is held only by the code — and after 279030 stripped the phase memory off five of
eight satellites, the split walked, ±3.3 m of slant iono (≈ ±20 TECU) fanning out in 16.5 minutes while the code residuals grew 2.7×.

### 5.3 The discriminator: post-reset fan-out, not reset size

Applying the same pair instrument to the 34 epochs after each post-ignition mass reset separates the survivable ones from the damaging ones
better than the reset's own size does:

| reset epoch | cells reset | max\|ΔPB\| at the reset | max cum Δamb after | max cum Δiono after | r | consumer |
|---|---|---|---|---|---|---|
| 271200 | 18 of 18 | 0.006 (empty) | +1.457 | +0.786 | 0.671 | recovered 2.36 → 0.98 |
| 275070 | 14 of 14 | 0.059 | +0.751 | +0.256 | −0.008 | flat 0.44 → 0.45 |
| **276810** | **11 of 12** | **5.000** | **−5.860** | −1.255 | **0.897** | **0.45 → 3.31** |
| **279030** | **10 of 16** | **0.011 (empty)** | **+9.708** | **+3.159** | **0.947** | 2.51 → 1.93 → **ramp to 5.64** |

A mass reset whose post-reset ambiguity fan-out stays ≤ 1.5 m is survivable; the two that reach 5.9 and 9.7 m are the two hour-6 events.  The
fan-out, not the reset count, is the quantity a guard would have to bound.

## 6. THE VERDICTS

**WINDOW A (TOW 276810).  The consumer's channel is its own DECLARED mass ambiguity reset, in response to a REAL network-wide phase-bias
re-realization.**  Evidence: 11 of 12 cells declared and reset in one epoch; `amb_var` ratio 2.75–5.56 on the reset cells against 0.97 on the
one cell that was not; post-fit phase residuals exactly 0.0000 at the reset epoch and 0.0001 at the next; the N/U jump of +0.49 m each is the
solution falling onto the code at the epoch the phase stopped constraining; the excursion to 3.16 m in three epochs is a six-satellite float
re-convergence with AR never running.  The producer half is EV6's, unchanged: the largest phase-bias re-realization of the run, at the loss of
`PB:G23:L1W`'s last two stations and the G23 anchor off STJO00CAN.  **EV6's refutation is preserved and sharpened**: only 2 of the 6 sky
satellites were in the Q3 decouple set, so the decouple is not the driver here either — but its `R` records are two of the twelve declarations
that fired the reset.  Secondary defect, new and exact: **G32/L1W took +1.409 m UNDECLARED into a live ambiguity because the emitter's jump
test screens the RAW emitted value while the consumer sees the DE-COMMONED one — the epoch's −1.45 m L1W frame move hid a 1.409 m
per-satellite step under a 0.0476 m bar (§4, §7.3).**

**WINDOW B (the ramp from 279270).  The consumer's channel is its own post-reset float re-convergence along the ambiguity ↔ slant-iono null,
triggered by an EMPTY declaration.**  Evidence: the trigger at 279030 is a 20-satellite Q3 clock-anchor decouple whose `R` records declared a
phase-bias discontinuity on five of the consumer's eight satellites while the emitted phase bias moved ≤ 0.011 m; through the ramp there are
zero declarations, zero resets and no de-commoned wire step above 0.046 m; the consumer's own ambiguity states travel 12.4× further than in a
matched quiet window and its slant-iono states 3.9× further, at r = 0.951 against a control r = 0.226; the post-fit CODE residual median grows
monotonically 0.262 → 0.701 m while the phase residual median stays ≤ 0.0057 m; and the ionospheric authority the posture nominally supplies
is INERT (0 supply epochs in 720).  **EV6's open item 1 is CLOSED**: the ramp is not a producer channel `products.txt` does not carry, it is
the consumer's own state.

**What this does NOT claim.**  That the reset is wrong.  A real 5 m phase-bias step must be responded to, and at 276810 it was.  What is wrong
is (i) that the only response is total, and (ii) that 30 % of the responses are to declarations the wire did not follow.

## 7. THE PRE-REGISTERED FIXES (nothing built beyond the instrument)

### 7.1 CONSUMER side — arm the declared-step CARRY instead of the reset

`network.closed_loop.pb_disc_carry: true` (→ `processing.ssr_phase_bias_disc_carry`).  **Already BUILT** (card CC), default OFF, and never run
on a field lineage with an instrument that can see it — CST is that instrument.
- **DECOUPLING edge** — `carryOnPhaseBiasDisc_` (`RealTimeEngineSsrBreaks.cpp:182`) translates the ambiguity by `−Δ` where `Δ` is the change in
  the value the model applies, instead of discarding the arc.  An EMPTY declaration has `Δ ≈ 0` and therefore costs nothing; a real 5 m step is
  absorbed with the convergence intact.  Counters exist (`PbDiscCarryLedger::Counters`, printed at `NetworkTeardown.cpp:5658`).
- **RE-COUPLING edge** — every refusal class (`first_sight`, `basis`, `bssd`, `unknown_key`) falls back to the existing reset and is counted,
  with the closure `carried + resets == events` asserted (`closureHolds()`).  CST makes both edges visible per cell: `ev_pb` reads
  `carry_disc` / `carry_flip` / a named refusal instead of `reset_disc`, and `amb_var` does NOT re-inflate on a carried cell.
- **Bars.**  Consumer p50 over the event window TOW 276810–278400 ≤ **1.0 m** (baseline ≈ 3.2 m); ramp window 279270–280260 no monotone drift
  > **1 m** (baseline +4.56 m); full-run hours 3–5 no worse than 0.97 / 1.05 / 0.45 m by > 0.10 m and hour 6 < 3.65 m; sim twin
  (`runs/_rfpdfix_sim6_both` → an ON copy) 720/720 with WL ≥ 98 %.  CST bars: `reset_disc` cell-epochs **390 → ≤ 40** with `carry_disc` ≥ 350
  and closure HOLDS; on the 117 empty declarations the `amb_var` ratio ≤ 1.1; max post-reset cumulative Δamb over any 34-epoch window
  **9.71 → ≤ 2.0 m**.  ⚠ The carry is CONSUMER-side only, so `products.txt` must be byte-identical to the baseline — a difference there means
  something else moved.

### 7.2 PRODUCER side — scope the `R` record to the CHANNEL that moved

New knob `network.disc_mark_scope` (enum `sat` | `channel`, default `sat` ⇒ byte-identical).  `markSatDiscontinuity`
(`NetworkProductEmitter.cpp:105`) currently drops a satellite into one `forced_bump_sats_` set that `satForced()` reads for the clock
(`:313`), the code bias (`:362-363`) and the phase bias (`:408-409`) alike, so a CLOCK guard declares a PHASE-BIAS discontinuity.  Under
`channel` the mark carries which channels it applies to, and the Q3 clock anchor (`NetworkClockAnchor.cpp:394-402`) marks CLOCK only.
- **DECOUPLING edge**: the mark is raised with its channel set and counted per channel (`n_mark_clk` / `n_mark_cb` / `n_mark_pb`), so "which
  channel was declared and why" is a number rather than an inference.
- **RE-COUPLING edge**: a satellite whose PHASE BIAS genuinely re-realizes while a clock mark is outstanding must still declare the phase
  channel — the emitter's own jump test (`:408-409`, `value_jump`) already covers it and must be shown to fire on the 276810 population
  (19 satellites > 0.5 m) with the scope narrowed, or the fix has traded a false declaration for a missed one.  A `PB_MARK/scoped` event with
  the channel set and the satellite identity is the trace.
- **Bars.**  CST census on the field arm: EMPTY declarations **117 of 390 → ≤ 20**; total reset cell-epochs 390 → ≤ 290; the 276810 epoch's
  declarations UNCHANGED at 11 of 12 (this fix must not touch a real re-realization); post-ignition (sat, epoch) resets co-incident with a
  `cta_decouple` **57 of 109 → ≤ 10**.  Wire bars: `products.txt` differs ONLY in `R` lines and disc-counter fields; consumer hours 3–5 no
  worse by > 0.10 m, hour 6 < 3.65 m; ramp window no monotone drift > 1 m; sim twin 720/720, WL ≥ 98 %.
- ⚠ This is the same wire-contract family as EV6 §8.1 (`clock_anchor_declare_return`) and the two should be graded on ONE arm, not two: 8.1
  declares the RETURN of a step, this one stops declaring a channel that never stepped, and both change the `R` population that §7.1's carry
  then acts on.

### 7.3 PRODUCER side — grade the declaration test in the frame the consumer sees

New knob `network.disc_jump_test_decommon` (bool, default false ⇒ byte-identical).  The phase-bias jump test at
`NetworkProductEmitter.cpp:408-409` compares `|bias − baseline|` against `discThresholdM` (`:87-99`) on the RAW emitted value.  A consumer's
receiver clock absorbs the satellite-common part, so the quantity that actually lands in its ambiguity is the step MINUS the per-signal
network median — and §4 measures a −1.45 m L1W frame move hiding a +1.409 m per-satellite step under a 0.0476 m bar.  Armed, the test forms
the de-commoned step (raw minus the median over the satellites emitted on that signal at BOTH epochs) and thresholds that.
- **DECOUPLING edge**: a satellite absent at the previous epoch contributes no de-commoned step and falls back to the raw test, exactly as the
  existing `staleKey` baseline suppression does; the median's population size is emitted with the epoch so a thin-signal epoch is visible.
- **RE-COUPLING edge**: the frame move itself is NOT declared (the receiver clock absorbs it by construction) — that non-declaration is the
  claim being made, so it is counted: `n_disc_frame_only` (raw over bar, de-commoned under) and `n_disc_frame_hidden` (the converse, the G32
  population).  Both must be non-zero on the 276810 epoch or the mechanism is not what this card says.
- **Bars.**  Field arm: at TOW 276810 declarations **11 of 12 → 12 of 12**; run-wide the UNDECLARED population's max |de-commoned ΔPB|
  **1.4089 → ≤ 0.10 m** and its p90 ≤ 0.0060 m (it must not mint noise); total declared cell-epochs 390 → within +10 %.  Consumer hours 3–5 no
  worse than 0.97 / 1.05 / 0.45 m by > 0.10 m, hour 6 < 3.65 m; sim twin 720/720, WL ≥ 98 %.
- ⚠ **Ordering.**  This fix ADDS declarations, and with today's reset-only consumer every added declaration is an added mass reset — so on its
  own it is predicted to make the consumer WORSE, and grading it alone would falsify a correct fix.  It must be graded on an arm that already
  carries §7.1 (`pb_disc_carry`), where an added declaration costs a translation and not an arc.

## 8. WHAT STAYS OPEN

1. **`network.trace_consumer_sat` has had ONE run.**  No sim twin, no A/B against a `consume_network_products: false` baseline arm (the
   instrument is deliberately armed before that branch so the BASELINE arm is instrumented too — that A/B is the cheapest next read and is not
   spent).
2. **Neither fix is built.**  §7.1 needs no new code at all — it is one config key on an existing, tested mechanism — and is the cheapest
   experiment in this card.  §7.2 is a real change to the wire contract and belongs with EV6 §8.1 on one arm.
3. **The `reinflate_isb` reaction is a no-op on a GPS-only consumer** (§2.3).  114 cell-epochs of declared clock discontinuity reach the
   consumer and nothing happens — correct by construction here (one receiver clock absorbs the common mode) but uninstrumented for a
   multi-constellation consumer, where the same 114 events would re-inflate every non-reference ISB.
4. **`rt_seam_verify.py` is 4 FAILs dirtier** (§2.2) and its frozen goldens live only in the D: archive.  Either the goldens are re-based on
   the current seam bodies or the four inserted lines are moved out of the relocated bodies into a wrapper; neither is done.
5. **The frame-blind declaration test of §7.3 is named but not built**, and the ordering constraint means it cannot be graded before §7.1.
   Its sibling — whether the CODE-bias and CLOCK jump tests (`NetworkProductEmitter.cpp:362-363`, `:313`) have the same blindness — is not
   measured here; this card's instrument carries `cb_disc` and `clk_disc` and can answer it with no new code.
6. **EV6's own open items are untouched** — the Q3 return declaration, the cascade share, and `emit_clock_disc`'s missing serialization
   (narrowed here to file-replay consumers only).

## 9. RUNS AND ARTEFACTS

- `runs/_cst_smoke_off` — OFF smoke, `products.txt` `9ADD1D3AB9D70657` (canonical).
- `runs/_cst_field6_both` — the ON field arm, 720/720, three gated artefacts byte-identical to `runs/_rfpdfix_field6_both`, plus
  `logs/closed_loop_sat_trace.csv` (11 618 rows, `45C18707D70BDBDE`).
- `runs/_rfpdfix_field6_both` — the RF-0hv FIELD baseline, read only, not re-run, left in place.
- New instrument: `scripts/cst_consumer_read.py`.  New gtest: `tests/test_consumer_sat_trace.cpp` (`ConsumerSatTraceSchemaTest`, 6 cases).
- `config/golden/`, `golden_manifest.json` and `docs/CONFIG_promotion_ledger.{md,json}` untouched.  No loader default changed.
