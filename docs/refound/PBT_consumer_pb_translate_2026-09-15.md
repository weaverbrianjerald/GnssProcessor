# PBT — the consumer translates EVERY phase-bias product step (`pb_step_translate`) + per-channel discontinuity marks (`disc_mark_scope`), both beds: the hour-6 ramp and the hour-1–3 ignition are both gone, the empty declarations are halved, and the SAME hours 4–5 that beat card CC beat this too (2026-09-15)

**STATUS:** open — **two knobs BUILT (`network.closed_loop.pb_step_translate` + `pb_step_translate_eps_m`, and `network.disc_mark_scope`), both default OFF, four runs spent on both beds, no golden and no ledger touched, nothing promoted.**
CST §7.1 and §7.2 named two halves of one defect: the consumer resets an ambiguity for every declaration it sees, 117 of 390 of those
declarations are EMPTY because `markSatDiscontinuity` bumps all three channel generations together, and EV6 measured 451 of 545 wire steps
> 0.10 m arriving UNDECLARED and landing in live ambiguities.  Card CC translated the DECLARED steps and card JT tried to mint the missing
declarations at the emitter — and JT failed because the emitter's frame is the network's and the consumer's is its own sky's.  **This card
drops the declaration from the decision entirely.**  Fix A: for every tracked (sat, signal) with a phase bias applied at both k−1 and k,
`Δ = PB_applied(k) − PB_applied(k−1)` and the ambiguity is translated by `−Δ` when `|Δ| > 5 mm`, declared or not — because a change in the
applied product carries no information about the receiver's position, and the phase model stays consistent only if the ambiguity absorbs it
exactly.  A satellite-common move is then absorbed uniformly and is exactly consistent, so there is no de-commoning and no wire change.
Fix B: each marking site names the channels it reports and only those generations step.
**The mechanisms do exactly what they were built to do.**  FIELD: `cells 11618 (declared 390) -> n_pb_translate_declared 326 /
n_pb_translate_undeclared 1358; sum_abs_translate_m 100.927 (max |d| 6.320 m); skipped_eps 9894; resets 0; closure HOLDS`, the per-cell
trace goes `{none 11228, reset_disc 390}` → `{none 9934, translate_undeclared 1358, translate_disc 326}` with **0 resets in 11 618 cells**,
and at TOW 276810 **all 12 cells are `translate_*`, G32 L1W included** — the frame-hidden cell JT needed an emitter change to reach, reached
here with the wire untouched.  Fix B suppresses **320 (sat, channel) marks** (clk 0 / cb 158 / pb 162), takes the field's EMPTY declarations
**117 → 44** and **erases CST's TOW 279030 event outright: 10 of 16 cells declared → 0 of 16**.
**And the consumer splits the same way card CC's did, harder.**  FIELD hours go **5.02 / 2.11 / 0.97 / 1.05 / 0.45 / 3.65 → 0.48 / 0.19 /
0.29 / 1.58 / 1.90 / 1.09 m** — hours 1, 2, 3 and 6 are the best this program has measured on this bed (hour 2 is **11×** better than
baseline and **10×** better than the carry arm) and the hour-6 ramp is dead (max-rise **+4.265 → +0.027 m**) — but **hours 4 and 5 are +0.53
and +1.45 m worse** than the best prior arm and the event window is **1.288 m** against a ≤ 1.0 bar.  The cause is the one CC-arm named and
this card measures directly: over TOW 270900–273000 the baseline's max cumulative |ΔAMB| is **3.2416 m** and the translated arm's is
**0.4489 m** — the baseline's mass resets at 271050 / 271200 were moving the ambiguity toward the truth, and a faithful translation, like a
faithful carry, preserves a state the data says is wrong.  **A carried or translated ambiguity is continuous, not correct.**  SIM is better
on **all six hours** (1.81 / 0.79 / 0.40 / 0.26 / 0.19 / 0.46 → **0.46 / 0.28 / 0.19 / 0.17 / 0.16 / 0.12 m**) at 720/720 with WL ref-free
last-25 % **99.1 %** unchanged.  **Verdict: A2 and A3 PASS, A1 and B1 FAIL on their literal numbers, and NEITHER knob joins RF-0hv.**
Nothing is promoted.

## 1. The knobs, as built

### 1.1 Fix A — `network.closed_loop.pb_step_translate` (+ `pb_step_translate_eps_m`, default 0.005 m)

| piece | file:line |
|---|---|
| the ledger: applied-value record, translate-vs-skip-vs-refuse decision, counters | `include/realtime/PbStepTranslateLedger.hpp`; `src/core/realtime/PbStepTranslateLedger.cpp:31` `react` |
| the translation itself (the exact call card CC's carry makes) | `src/core/realtime/PbStepTranslateLedger.cpp:92` `sm.shiftAmbiguity(amb_key, -d.delta_m)` |
| the engine seam (the per-key sweep, the applied-value read, the CST event kinds) | `src/core/realtime/RealTimeEngineSsrBreaks.cpp:291` `RealTimeEngine::translatePhaseBiasStep_` |
| the DISPATCH — the once-only guarantee | `src/core/realtime/RealTimeEngineSsrBreaks.cpp:112` (inside `resetOnPhaseBiasDisc`, whose banner at `:105-111` states it) |
| run-end summary line | `src/core/realtime/RealTimeEngineSsrBreaks.cpp:374` `logPbStepTranslateSummary_`; called `RealTimeEngine.cpp:368` |
| knobs + Doxygen | `include/config/GnssConfig.hpp:628` / `:636` (`ssrPhaseBiasStepTranslate`, `getSsrPhaseBiasStepTranslateEpsM`), members `:7694-7695`; `include/config/GnssConfigTypes.hpp:202` / `:211` (`ClosedLoopConfig::pb_step_translate`, `pb_step_translate_eps_m`) |
| base-config parse (a STANDALONE RT consumer) | `src/core/config/GnssConfigLoad.cpp:585-612` |
| harness parse + derivation onto the consumer | `src/core/config/GnssConfigLoadNetwork.cpp:11377-11378` (parse) and `:11662-11664` (`deriveClosedLoopConsumerConfig`) |
| run-end readout beside the carry's closure line | `src/core/filter/NetworkTeardown.cpp:5702` |
| gtest (`PbStepTranslateTest`, 9 cases) | `tests/test_pb_step_translate.cpp` |

**The rule.**  For each tracked (sat, raw phase signal) at consumer epoch k:

```
Δ = PB_applied(k) − PB_applied(k−1)
|Δ| ≤ eps                        → nothing written   (the filter absorbs it as noise)
|Δ| >  eps                        → amb ← amb − Δ     (value only; the covariance is not touched)
no PB applied at k−1 or at k      → the EXISTING first-sighting / A5-flip logic
```

No de-commoning, no wire change: a move common to every tracked satellite is absorbed uniformly by the same rule and is exactly consistent —
which is why the +1.409 m "de-commoned" G32 L1W step of CST §7.3 needs **no** producer change here.  Its RAW applied step at 276810 is
−0.0421 m, its 11 neighbours' raw steps are their own, and translating each cell by its own Δ preserves the relative geometry exactly.  The
1.409 m only ever existed as a *difference* between a kept ambiguity and eleven re-seeded ones.

**THE ONCE-ONLY GUARANTEE (both knobs armed).**  `resetOnPhaseBiasDisc` branches on `ssrPhaseBiasStepTranslate()` **first and returns**, so
`carryOnPhaseBiasDisc_` is never entered in an epoch where the translation is armed.  Exactly one reaction seam runs per epoch; it visits each
(sat, signal) key exactly once; and `PbStepTranslateLedger::react` makes at most one `shiftAmbiguity` call per cell.  A declared step therefore
cannot be translated twice by construction, not by arithmetic.  The evidence at run end is that the card-CC counters stay at 0 while the PBT
counters carry the whole population, and the loader emits an explicit WARN naming the subsumption when both keys are set
(`GnssConfigLoad.cpp:609-612`).  Locked by `PbStepTranslateTest.DeclaredStepWithBothKnobsIsTranslatedExactlyOnce` (the state moves by −Δ, not
−2Δ, and an engine configured with both reports `pbStepTranslateArmed()` and `pbDiscCarryArmed()` with both counter sets at 0).

**THE APPLIED-VALUE GUARD.**  Δ is formed from the value the measurement model ACTUALLY applies, read with the model's own read —
`src/core/measurement/MeasurementModel.cpp:1622-1624`, `ssr_bias_term`: gated ⇒ `getPhaseBias(sat, code, obs.time, hold_factor × max_age)`,
ungated otherwise, 0 when it yields nothing — reproduced verbatim at `RealTimeEngineSsrBreaks.cpp:318-324`.  The other side of the difference
is this ledger's own RECORD of that same read at the previous epoch (`record()` at `:368`, the tail of each key's iteration, AFTER the
reaction), never a fresh re-read of the product.  So an interpolation change, a provider update landing mid-epoch, or a staleness hold cannot
make the translation disagree with the step the phase row actually took.  ⚠ Consequence to keep: `n_pb_translate_declared` + `undeclared` count
RAW applied steps, not the de-commoned wire steps EV6 and CST report — see §4.3.

**Refusals.**  A DECLARED cell the translation cannot reach falls back to the EXISTING reset with its own counter (first sighting / A5 edge,
NL/WL basis, BSSD consumer, unknown key).  An UNDECLARED one is a counted NO-OP: the pre-card path did nothing there, and a new reset on every
sub-threshold product move would be a regression — on a BSSD consumer it would reset continuously.  ⚠ The ambiguity state of the
undifferenced consumer is in METRES (`MeasurementModel.cpp:1653-1655`, unit Jacobian beside a metres `ssr_bias_term`), so "−Δ/λ cycles" and
"−Δ metres" are the same write; the basis where λ would appear (NL/WL) is one of the refusals.

### 1.2 Fix B — `network.disc_mark_scope` (`sat` | `channel`, default `sat`)

| piece | file:line |
|---|---|
| the channel mask + the mark API | `include/products/NetworkProductEmitter.hpp:254` / `:266` / `:280` (`DiscChannel`, `MarkScopeEvent`, `MarkChannelTally`), `markSatDiscontinuity(sats, channels)` `:374` |
| the mark records its mask | `src/core/products/NetworkProductEmitter.cpp:130` |
| the read: `satForced(sat, channel)` + the suppression census | `src/core/products/NetworkProductEmitter.cpp:341-368` |
| the three channel reads | clock `:418`, code bias `:468`, phase bias `:514` |
| pending-mark state carries the mask (3-field `efs,`; a 2-field legacy line still restores) | `:721` (serialize), `:836` (restore) |
| counters + the `MARK_SCOPE` event drain | `include/app/NetworkRunContext.hpp:3068-3079`; `src/core/filter/NetworkEmitStore.cpp:465-498` |
| run-end readout | `src/core/filter/NetworkTeardown.cpp:5675` |
| knob + Doxygen | `include/config/GnssConfigTypes.hpp:2467`; parse `src/core/config/GnssConfigLoadNetwork.cpp:2385-2409` |
| emitter construction | `src/core/app/ApplicationNetworkSetup.cpp:1103`; contract field `:1230` |
| S contract (`discscope=channel`) | field `include/products/NetworkProductIO.hpp:157`; writer `src/core/products/NetworkProductIO.cpp:552`; parser `:332`; legend `:640`; `checkContract` `:994`; `makeReplayEmitter` `:1044` |
| warm-resume fingerprint (conditional append) | `src/core/app/NetworkRunContext.cpp:967` |
| gtest (`DiscMarkScopeTest`, 9 cases) | `tests/test_disc_mark_scope.cpp` |

**The call-site classification.**  Every `markSatDiscontinuity` call site in the producer, and the channel it reports:

| # | site | what raised the mark | channels under `channel` | why |
|---|---|---|---|---|
| 1 | `NetworkEmitStore.cpp:383` | CLK-R stage-2 emitted-clock step test / held-frame per-satellite marks | **CLK** | the emitted CLOCK value moved; nothing says the biases did |
| 2 | `NetworkSolPrep.cpp:742` | the B1 retro drain — the UNION of URA-retro QC, the reference-sat mark, the datum-station slip re-reference and the RF-2 held-basis anchor handover | **CLK+CB+PB** | the union deliberately loses provenance (the code's own `detail=union`); a contaminated epoch can have moved any channel and a held-basis handover moves the phase bias.  The card narrows sites that KNOW their channel, never a union that does not |
| 3 | `NetworkSolPrep.cpp:781` | eph cutover whose prior version was evicted before Δ_eph could be computed | **CLK+CB+PB** | the step is unknown in magnitude AND in channel |
| 4 | `NetworkSolPrep.cpp:815` | code-bias excursion re-init (`cb_excursion_reinit`) | **CB** | the emitted CODE bias steps; the clock and phase bias do not |
| 5 | `NetworkSolPrep.cpp:852` | **the Q3 clock-anchor decouple** (`clock_anchor_mark`) | **CLK** | THE site CST §7.1 measured: a 20-satellite clock decouple at TOW 279030 declared a phase-bias step of at most 0.011 m and reset five of the consumer's satellites |
| 6 | `NetworkSolPrep.cpp:894` | T3c `clock_signal_basis_hold` RE-LATCH | **CLK+CB+PB** | the re-latch re-splits δtˢ↔dˢ, so all three step — the block's own comment says so |
| 7 | `NetworkSolPrep.cpp:947` | T3c IF-pin RE-SELECTION (hold OFF) | **CLK+CB+PB** | the same δtˢ↔dˢ re-reference by the satellite's inter-channel bias |
| 8 | `NetworkSolPrep.cpp:985` | code-datum time-linked RE-SEED (`code_datum_time_linked`) | **CB+CLK** | the emitted CODE bias re-seeds, and under the IF-pin convention the satellite clock is defined against that same pinned pair, so it is re-referenced with it.  The phase bias has its own datum and is left to its own jump test |
| 9 | `NetworkProductIO.cpp:847` | **product REPLAY of an `R` record** | **CLK+CB+PB** (the default) | ⚠ the `R` record carries satellite ids and NO channel set, so this is the only honest reconstruction — see §7 item 1 |

Four of the nine sites keep the full satellite scope; two are clock-only, one code-only, one code+clock.  The RE-COUPLING edge is unchanged and
is what keeps a real re-realization declared: the emitter's own per-(sat, signal) phase jump test (and the opt-in code one) still fires on a
channel no mark named — pinned by `DiscMarkScopeTest.PhaseJumpTestStillDeclaresUnderAClockOnlyMark` and measured at TOW 276810 in §5.

## 2. Build, exe and identity

Exe `build/x64-Release/gnss_processor_pbt.exe` sha256[:16] **`F6A75B12A8430647`** (a copy of `gnss_processor.exe`, re-hashed after the copy;
source main `cd713ab` + this card's diff).  All four arms exit 0 and emit **720 / 720**.

Debug `gnss_tests.exe`, verbatim:

```
[==========] 2253 tests from 297 test suites ran. (86776 ms total)
[  PASSED  ] 2252 tests.
[  SKIPPED ] 1 test, listed below:
[  SKIPPED ] FixtureSmokeTest.Sp3HeaderTimeConversionMatches
```

(2235 / 295 / 139 before this card + 9 `PbStepTranslateTest` + 9 `DiscMarkScopeTest` = 2253 / 297.)

**OFF smoke** `runs/_pbt_smoke_off` (= `runs/_rf2d_smoke_off/smoke.json`, paths only): `^#`-stripped **record stream
`333DCB0871E1F5BC`** over **6 596 record lines** — the canonical value, IDENTICAL.  Whole-file sha256[:16] `B05F26B1CD0540DB` against the
stored `9ADD1D3AB9D70657`; the ENTIRE diff is line 16, the `# S,` legend, which gains `[,discdecom=1][,discscope=channel]` — the first token
from JT (already on main, `cd713ab`; the stored smoke predates it) and the second from this card.  The legend is grammar-complete by contract
(`NetworkProductIO.cpp:594-618`), so it names every field whether or not the run writes it.

**Identity gates.**

| gate | measured | |
|---|---|---|
| A field: `products.txt` vs `_rfpdfix_field6_both` | whole file `4F513E762F292337` → `EDAF96CEDB1262B2`, **the only differing line is the `# S,` legend**; record stream `44B50A8A85312ABC` (172 476 lines) IDENTICAL | PASS |
| A sim: same vs `_rfpdfix_sim6_both` | whole `D9280279A6BBCCD8` → `2149CABD1192E610`, same single legend line; record stream `60242D44DCCE0A85` (136 246 lines) IDENTICAL | PASS |
| A both beds: `logs/network_epoch_status.csv`, `logs/network_dd_fractional.csv` | `939727D60970F6B5` / `805D8B21F3A7F9C4` (field) and `E4FC04F44D51F1EE` / `8E3F6AA14565E52D` (sim) — IDENTICAL to the baselines | PASS |
| A both beds: the whole `rf2d_grade` wire read | IF dispersion p50/p90/max `0.055 / 0.201 / 0.68` m, worst G26 +0.68 / G27 −0.63 / G17 −0.31; declarations cont 0/139, cold 12/0; `CLK_STEP` 0; `cta_decouple` 189; handover-mode table identical row for row | PASS |
| A+B vs A, both beds: `network_epoch_status.csv` | IDENTICAL (`939727D60970F6B5` / `E4FC04F44D51F1EE`) | PASS |
| A+B vs A, both beds: `products.txt` minus `R` records and `#` lines | `6BC6B04FEDF43488` (172 440) / `109C3C4DFA931F93` (136 236) — IDENTICAL | PASS |
| A+B vs A, both beds: the WHOLE `products.txt` | `EDAF96CEDB1262B2` / `2149CABD1192E610` — **byte-identical, R records included (36/36 and 10/10)** | stronger than the gate |

⚠ **The last row is a warning, not a win.**  `products.txt` cannot show fix B on this lineage: `network.serialize_disc_counters` is off (a v1
header — every `P` record is 5 fields), so the per-record disc counters never reach the file and the `R` records list the same satellites under
either scope.  The wire the IN-PROCESS consumer reads **does** change (the store's per-channel generations do), and only the consumer's own
trace can see it.  This is exactly the blind-instrument trap JT hit with `ev6_event_read.py`, on a different file.  A file-replay consumer of a
`channel`-scoped producer is also blind — see §7 item 1.

The A arms' consumer artefacts DIFFER from the baseline by design (`closed_loop_trace.csv` `F9D7FB7D53298083` → `5D66D5FCCA0BD240` field,
`70E79B2487D3FDAA` → `916E1595EE0F7A72` sim): that is the fix working.

## 3. The arms

| arm | from | to | keys added |
|---|---|---|---|
| A field | `runs/_rfpdfix_field6_both/arm.json` | `runs/_pbt_field6` | `closed_loop.pb_step_translate: true`, `trace_consumer_sat: true` |
| A sim | `runs/_rfpdfix_sim6_both/arm.json` | `runs/_pbt_sim6` | same |
| A+B field | `runs/_pbt_field6/arm.json` | `runs/_pbtms_field6` | `+ disc_mark_scope: "channel"` |
| A+B sim | `runs/_pbt_sim6/arm.json` | `runs/_pbtms_sim6` | same |

Each diff was verified before launch: **three hunks** for the A arms (the run path and the two keys, the same shape CC-arm used) and **two**
for the A+B arms (the run path and the one key).  Every arm keeps `trace_consumer_sat: true`.

## 4. Bars A1–A3

### 4.1 A1 — FIELD — **FAIL** (3 of 7 clauses)

Bar: hours 3–6 each no worse than `min(RF-0hv baseline, carry arm)` = 0.38 / 1.05 / 0.45 / 1.92 m by more than 0.10 m; event-window (TOW
276810–278400) p50 ≤ 1.0 m; ramp window (279240–280800) no monotone rise > 1 m; 720/720.

| clause | bar | measured (`_pbt_field6`) | |
|---|---|---|---|
| hour 3 | ≤ 0.48 | **0.29** (−0.09 on the best prior arm) | PASS |
| hour 4 | ≤ 1.15 | **1.58** (+0.53) | **FAIL** |
| hour 5 | ≤ 0.55 | **1.90** (+1.45) | **FAIL** |
| hour 6 | ≤ 2.02 | **1.09** (−0.83) | PASS |
| event window p50 | ≤ 1.0 m | **1.288** (baseline 3.561, carry 1.404) | **FAIL by 0.288** |
| ramp window | no monotone rise > 1 m | first 1.084 / last 0.908 / max 1.084; per-5-min p50 `1.05 0.93 0.91 0.88 0.88 0.91`; **max-rise +0.027 m** (baseline +4.265, carry −0.016) | PASS |
| emitted | 720/720 | **720 / 720** | PASS |

**Epoch-2 ignition:** `_pbt_field6` reads **20.79 m** at TOW 259230 (baseline 4.12, carry arm 27.82), settling 9.94 → 6.17 over the next two
epochs.  The same class of excursion as the carry arm's and for the same reason — the first tracked epochs carry a large product step that the
reset path used to discard — smaller here only because the eps band drops the sub-5 mm part.  It is 2 of 720 epochs and hour 1 still reads
**0.48 m p50 / 3.11 p90**, ten times better than any prior arm; it is reported, not defended.

### 4.2 A2 — SIM — **PASS**

Bar: 720/720; WL ≥ 98 %; consumer per hour within 0.10 m of the RF-0hv sim baseline or better.

| clause | bar | measured (`_pbt_sim6`) | |
|---|---|---|---|
| emitted | 720/720 | **720 / 720** | PASS |
| WL(1,2) ref-free, last 25 % of epochs, `round(e)=0` | ≥ 98 % | **99.1 %** (n = 48 816; identical to the baseline — the DD trace is byte-identical) | PASS |
| per hour vs baseline | within 0.10 m or better | **better on all six**: 1.81 → 0.46, 0.79 → 0.28, 0.40 → 0.19, 0.26 → 0.17, 0.19 → 0.16, 0.46 → 0.12 m | PASS |

### 4.3 A3 — telemetry — **PASS (reported)**

Run-end line, verbatim (`NetworkTeardown.cpp:5687`):

```
FIELD  pb step translate (PBT) : cells 11618 (declared 390) -> n_pb_translate_declared 326 / n_pb_translate_undeclared 1358;
       sum_abs_translate_m 100.927 (max |d| 6.320 m); n_pb_translate_skipped_eps 9894 (declared-but-EMPTY 64);
       declared resets first_sight 0 / basis 0 / bssd 0 / unknown_key 0; undeclared refused 0; no_pair 40; closure HOLDS
SIM    pb step translate (PBT) : cells 10800 (declared 735) -> n_pb_translate_declared 715 / n_pb_translate_undeclared 1989;
       sum_abs_translate_m 575.557 (max |d| 27.045 m); n_pb_translate_skipped_eps 8054 (declared-but-EMPTY 16);
       declared resets first_sight 0 / basis 0 / bssd 0 / unknown_key 4; undeclared refused 0; no_pair 38; closure HOLDS
```

Closure (invariant #4): FIELD 326 + 1358 + 9894 + 0 + 0 + 40 = **11 618**; SIM 715 + 1989 + 8054 + 4 + 0 + 38 = **10 800**.  Both HOLD.  The
sim's 4 `unknown_key` refusals are the same 4 the carry arm took.  `sum_abs_translate_m` is **identical** on the A and A+B arms of each bed
(100.927 / 575.557 m) — fix B moves the declared/undeclared LABEL, never the applied value, which is its own identity check.

**The 276810 cells — 12 of 12 `translate_*`, G32 L1W included:**

| sat/sig | `ev_pb` | pb_disc |
|---|---|---|
| G19 L1W, G19 L2W, G32 L2W, G12 L1W, G12 L2W, G06 L1W, G06 L2W, G24 L1W, G24 L2W, G25 L1W, G25 L2W | `translate_disc` | stepped |
| **G32 L1W** | **`translate_undeclared`** | 4 → 4 (unchanged) |

The run's largest declaration epoch costs **zero arcs**.  `amb_var` on the 12 cells is 0.017–0.059, i.e. no re-inflation anywhere (the reset
path re-inflated 2.75–5.56×).

**Undeclared translations above 0.10 m: FIELD 0, SIM 0** — and that number needs its explanation, because EV6 counted 451 undeclared wire
steps > 0.10 m.  They are different statistics on purpose.  EV6 and CST report the **de-commoned** step (raw minus a sky/network median) over
every EMITTED satellite; the ledger translates the **raw applied** step over the 6–10 satellites the consumer TRACKS, because that is the
quantity the phase row actually takes.  Measured on the field arm, the undeclared translations are 1358 cells at p50 **0.010** / p90 **0.027** /
max **0.060 m**, while the declared ones are 326 at p50 **0.123** / p90 **0.478** / max **6.320 m** (193 above 0.10 m).  G32 L1W at 276810 is
the worked example: its raw Δ is **−0.0421 m** and it is translated by that; the +1.409 m JT chased is the *difference* between a kept
ambiguity and eleven re-seeded ones, and it never forms when all twelve are translated by their own Δ.  The CST census confirms the wire is
untouched: `UNDECLARED cell-epochs 11188: |de-commoned dPB| p50 0.0007 p90 0.0052 **max 1.4089 m**` on the A arm, the same number CST measured.

## 5. Bar B1 — A+B vs A

| clause | bar | measured | |
|---|---|---|---|
| EMPTY PB declarations, field (`cst_consumer_read --section census`, `--empty-m 0.02`) | 117 → ≤ 20 | **117 → 44** (30.0 % → 14.4 % of declarations); declared cell-epochs 390 → **306** | **FAIL by 24** |
| the same on the ledger's own 5 mm RAW band | — | declared-but-EMPTY **64 → 12** | reported |
| TOW 276810 stays declared | 11/12 | **11 / 12** (`translate_disc` ×11 + G32 L1W `translate_undeclared`), counters stepped exactly as on the A arm | PASS |
| `n_mark_suppressed_by_scope` per channel | reported | FIELD **320** (clk **0** / cb **158** / pb **162**), applied `n_mark_clk 175` / `n_mark_cb 48` / `n_mark_pb 33`; SIM **9** (clk 0 / cb 2 / pb 7), applied 10 / 16 / 6 | PASS |
| consumer per hour vs the A arm | within 0.05 m, both beds | FIELD **identical to 2 dp on all six** (0.48 / 0.19 / 0.29 / 1.58 / 1.90 / 1.09); SIM **identical on all six**.  Event window 1.288 → **1.292**, ramp max-rise +0.027 → **+0.027** | PASS (the predicted NULL) |
| emitted | 720/720 | **720 / 720** both beds | PASS |

**The mechanism, exactly.**  `MARK_SCOPE` rows, field: **158** clock-only marks each suppressing the CB and the PB channel, plus **4** clk+cb
marks suppressing PB — 320 rows, all `marked_channels=clk` or `clk cb`, and **zero** clock suppressions (no site under test reports something
other than the clock while withholding it).  Sample:

```
2313,263670.000,MARK_SCOPE,suppressed,G21,,,,cb,marked_channels=clk
2313,263670.000,MARK_SCOPE,suppressed,G21,,,,pb,marked_channels=clk
2313,262920.000,MARK_SCOPE,suppressed,G20,,,,pb,marked_channels=clk cb
```

**CST's TOW 279030 event is erased.**  The 20-satellite Q3 clock decouple whose `R` records reset the five-satellite intersection with the
consumer's sky goes **10 of 16 cells declared → 0 of 16**; the five cells that did carry a real (sub-decimetre) product move now read
`translate_undeclared` instead of `translate_disc`, and the ambiguity outcome is identical because the translation never asked about the
declaration.  That is the two fixes doing the same job from both ends, and it is why B's consumer delta is null: on an arm that already
translates every step, removing a false declaration removes a *label*, not a reaction.  **On a reset-only consumer the same suppression would
be worth 10 arcs at that single epoch** — which is the reading B is owed on its own, and this card does not have it (see §7 item 3).

The bar misses because the remaining 44 empty declarations are **not** mark-driven: they survive the scope narrowing, so they come from the
emitter's per-(sat, signal) jump test firing on a raw step that the consumer's sky sees as ≈ 0 — JT's frame gap, from the other direction.

## 6. The per-hour and per-30-min tables, both beds

**FIELD** (`pos_err_3d_m` p50 by hour; p90 in the second row of each cell's pair):

| arm | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| RF-0hv baseline `_rfpdfix_field6_both` | 5.02 / 5.46 | 2.11 / 3.61 | 0.97 / 1.27 | 1.05 / 2.39 | 0.45 / 0.94 | 3.65 / 5.35 |
| carry `_ccarm_field6_both` | 3.74 / 4.77 | 1.89 / 3.79 | 0.38 / 0.63 | 1.26 / 1.92 | 0.72 / 0.85 | 1.92 / 2.08 |
| **A `_pbt_field6`** | **0.48 / 3.11** | **0.19 / 0.32** | **0.29 / 0.35** | 1.58 / 1.78 | 1.90 / 1.93 | **1.09 / 1.32** |
| **A+B `_pbtms_field6`** | 0.48 / 3.11 | 0.19 / 0.32 | 0.29 / 0.35 | 1.58 / 1.78 | 1.90 / 1.94 | 1.09 / 1.33 |

Per-30-min p50 (12 bins from the consumer's own first epoch):

| arm | bins 1–12 |
|---|---|
| baseline | 3.76 5.10 2.31 1.89 1.01 0.91 2.02 0.97 0.44 0.45 3.56 4.31 |
| carry | 3.37 4.19 2.91 1.24 0.49 0.36 1.54 1.10 0.79 0.71 1.74 2.04 |
| **A** | 1.30 0.29 0.17 0.20 0.32 0.28 1.14 1.74 1.90 1.89 1.15 0.91 |
| **A+B** | 1.30 0.29 0.17 0.20 0.32 0.28 1.13 1.74 1.91 1.90 1.15 0.91 |

**SIM** (p50 / p90 by hour):

| arm | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| RF-0hv baseline `_rfpdfix_sim6_both` | 1.81 / 5.41 | 0.79 / 1.23 | 0.40 / 0.48 | 0.26 / 0.30 | 0.19 / 0.62 | 0.46 / 0.53 |
| carry `_ccarm_sim6_both` | 1.73 / 4.57 | 1.03 / 1.57 | 0.50 / 0.68 | 0.24 / 0.34 | 0.18 / 0.32 | 0.61 / 0.67 |
| **A `_pbt_sim6`** | **0.46 / 3.19** | **0.28 / 0.32** | **0.19 / 0.26** | **0.17 / 0.39** | **0.16 / 0.37** | **0.12 / 0.17** |
| **A+B `_pbtms_sim6`** | 0.46 / 3.19 | 0.28 / 0.32 | 0.19 / 0.26 | 0.17 / 0.39 | 0.16 / 0.37 | 0.12 / 0.17 |

Per-30-min p50:

| arm | bins 1–12 |
|---|---|
| baseline | 4.46 1.36 1.15 0.56 0.44 0.34 0.26 0.26 0.17 0.47 0.42 0.48 |
| carry | 1.72 1.74 1.32 0.88 0.63 0.36 0.30 0.15 0.14 0.23 0.61 0.60 |
| **A** | 1.60 0.13 0.23 0.30 0.23 0.14 0.15 0.35 0.30 0.12 0.15 0.10 |
| **A+B** | 1.60 0.13 0.23 0.30 0.23 0.14 0.15 0.35 0.30 0.12 0.15 0.10 |

**The two field windows:**

| window | baseline | carry | A | A+B |
|---|---|---|---|---|
| event, TOW 276810–278400 (n 54) — p50 | **3.561** | 1.404 | **1.288** | 1.292 |
| … first / last / max | 0.383 / 3.731 / 4.012 | 0.760 / 1.847 / 1.847 | 1.813 / 1.109 / 1.813 | 1.820 / 1.112 / 1.820 |
| … per-5-min p50 | 2.89 3.20 3.75 3.54 3.75 3.75 | 0.66 0.88 1.37 1.62 1.81 1.82 | 1.60 1.46 1.33 1.24 1.14 1.10 | 1.60 1.46 1.33 1.24 1.14 1.11 |
| ramp, TOW 279240–280800 (n 52) — first / last / max | 1.081 / 4.683 / 5.636 | 2.096 / 1.882 / 2.098 | **1.084 / 0.908 / 1.084** | 1.087 / 0.909 / 1.087 |
| … per-5-min p50 | 1.30 3.03 4.77 5.57 5.19 4.73 | 2.08 2.06 2.04 2.00 1.93 1.89 | 1.05 0.93 0.91 0.88 0.88 0.91 | 1.05 0.93 0.91 0.88 0.88 0.91 |
| … max-rise | **+4.265** | −0.016 | **+0.027** | +0.027 |

Note the event window's SHAPE, not just its p50: the baseline and the carry arm both climb through it (max at the far end), while the
translated arm *decays* across it (max at the first epoch, `end-start −0.494 m`).  The 1.288 m is the tail of a decay, not the head of an
excursion — which the bar, written for a rise, cannot distinguish.

## 7. What the failures are, and what stays open

**Hours 4 and 5 are card CC's C3, with a larger population — and this card measures the cause directly.**  CC-arm attributed its +0.21 /
+0.27 m to two mass resets at TOW 271050 / 271200 that were THERAPEUTIC (CST's impact table scored them −1.173 / −1.382 m).  Over TOW
270900–273000, `cst_consumer_read --section pair` gives max cumulative |ΔAMB| across the consumer's cells:

| arm | max cum ΔAMB | worst cell |
|---|---|---|
| baseline (reset-only) `_cst_field6_both` | **+3.2416 m** | G10 L2W (G10 L1W +2.6068, G23 L2W +1.6735) |
| carry `_ccarm_field6_both` | −0.3868 m | G24 L1W |
| **A `_pbt_field6`** | **−0.4489 m** | G12 L2W |

The reset path moved those ambiguities by METRES in that window and came out better; the translated arm holds them to half a metre and comes
out worse.  **A translated ambiguity is continuous, not correct** — invariant #7's re-coupling edge has no re-validation here, exactly as
CC-arm said, and going from the declared population (390 cells) to every step (1684 cells) preserves proportionally more of whatever error is
sitting in those states.  That is a real open problem and it is the same one, not a new one.

Open items:

1. **A `channel`-scoped producer is not replay-faithful.**  The `R` record carries satellite ids and no channel set, so
   `NetworkProductIO::replay` re-marks with the full satellite scope (`NetworkProductIO.cpp:847`).  The scope travels in the S contract
   (`discscope=channel`) so `checkContract` NAMES the divergence rather than hiding it, and the gtest pins the token and the legend — but the
   gap is real and closing it needs an `R`-record grammar extension (a `chan=` token per mark, or one `R` line per channel set).  Not built.
   ⚠ On this lineage the token never appears in a DATA line either: `serialize_sbasis_contract` is off (a v1 header, zero `S,` lines), so it
   is pinned only by `DiscMarkScopeTest.SLineDeclaresTheScopeAndOffIsByteIdentical` — JT's finding, inherited.
2. **`products.txt` is a BLIND instrument for fix B on this lineage** (§2).  `network.serialize_disc_counters` on a declaration arm is owed —
   JT owes the same thing.
3. **Fix B has never been measured on a reset-only consumer**, which is where its 320 suppressed marks are worth arcs rather than labels.  The
   cheapest missing read is `disc_mark_scope: "channel"` on the RF-0hv baseline itself (no `pb_step_translate`), graded against
   `_rfpdfix_field6_both` and `_cst_field6_both`: predicted ~10 arcs saved at TOW 279030 alone.  Not run.
4. **The 44 surviving EMPTY declarations are jump-test declarations, not marks** — the emitter fires on a raw step the consumer's sky reads as
   ≈ 0.  That is JT's frame gap seen from the suppression side, and JT's own pre-registered fix (declare `c_s` on the wire and let the consumer
   de-common against its OWN sky) is the formulation that would close both.  Not built.
5. **The ignition transient at epoch 2 (20.79 m field) is unattributed.**  It is 2 epochs and hour 1 is still 10× better than any prior arm,
   but the mechanism — the first tracked epochs carrying a large product step the reset used to discard — is the same one hours 4–5 pay for.
   A first-arc guard (translate only once the ambiguity's variance is below a bar) is the obvious shape and is NOT built or pre-registered here.
6. **No re-validation at the re-coupling edge.**  Neither the carry nor the translation ever asks whether the ambiguity it preserved is still
   right.  The RF-1 float vehicle's own covariance is the natural place for that test and this card does not use it.
7. **Neither ledger is in the RT snapshot** (v1 format unchanged).  After a warm restore the first cell per key has no (k−1, k) pair, so a
   declared one takes the reset and an undeclared one is a counted no-op — the same property card CC has, and the same one a seam card would
   have to close.
8. **`scripts/rt_seam_verify.py` is unchanged at 6 FAILs** — 4 from CST's inserted event-recorder lines and 2 pre-existing
   (`resolveAmbiguities`, `initAmbiguityResolver`).  This card adds ONE line to `resetOnPhaseBiasDisc`, which was already among the 4, so it
   contributes **0 new FAILs**.  Still the known hygiene item.

## 8. Verdict

**Neither knob joins RF-0hv.**  `network.closed_loop.pb_step_translate` fails A1 on hours 4 and 5 and on the event window;
`network.disc_mark_scope: "channel"` fails B1's one numeric bar (44 against ≤ 20).  Both stay **default OFF**; no golden, no ledger row, no
loader default, `config/refound/*_rf0hv.json` untouched, `config/golden/` untouched.

What the runs establish anyway:

- **The consumer does not need the producer to declare anything.**  Every result card JT bought with an emitter change — the 276810 cell, the
  frame-hidden step, the declaration population — the consumer reaches on its own with the wire byte-identical, because the quantity that
  matters is the raw applied step and the consumer already has it.  JT's failure (two different frames) cannot occur here: there is only one
  frame, the consumer's own, and it never enters the arithmetic.
- **The hour-6 ramp and the hour-1–3 ignition are both solved**, on both beds, by the same rule — and the sim, which has no therapeutic-reset
  population, is better on all six hours.
- **What blocks promotion is not the mechanism; it is that a preserved ambiguity is never re-validated.**  Hours 4–5 are the measurement of
  that, now with the cause quantified (3.2416 m of corrective ambiguity motion in the baseline against 0.4489 m preserved).  The next card is
  the re-coupling edge, not another emission fix.

## 9. Runs and artefacts

- `runs/_pbt_smoke_off` — OFF smoke, record stream `333DCB0871E1F5BC` (canonical), whole file `B05F26B1CD0540DB`.
- `runs/_pbt_field6` / `runs/_pbt_sim6` — the A arms, 720/720 each.
- `runs/_pbtms_field6` / `runs/_pbtms_sim6` — the A+B arms, 720/720 each.
- `runs/_rfpdfix_field6_both` / `_rfpdfix_sim6_both` (RF-0hv baseline), `runs/_ccarm_field6_both` / `_ccarm_sim6_both` (carry),
  `runs/_cst_field6_both` (the instrument bed) — read only, not re-run, left in place.
- Exe `build/x64-Release/gnss_processor_pbt.exe` `F6A75B12A8430647`.
- New gtests: `tests/test_pb_step_translate.cpp` (`PbStepTranslateTest`, 9 cases), `tests/test_disc_mark_scope.cpp` (`DiscMarkScopeTest`, 9).
- No new grader: `scripts/cc_arm_grade.py`, `scripts/cst_consumer_read.py`, `scripts/rf2d_grade.py` and `scripts/simnet_amb_integer.py` cover
  everything this card reports.

## 10. 2026-09-16 — correction to the universal translation premise

[PBT-AUDIT](PBT-AUDIT_translation_premise_2026-09-16.md) demonstrates that keeping
`phase_bias + ambiguity` unchanged preserves the previous prediction, but does
not necessarily match the current observation. A correctly tracked physical
phase-bias change is cancelled by blanket translation, producing a residual that
was zero without it. Known datum shifts remain a valid use of translation.
Clock/bias redistributions and improved-estimate counterexamples further show
that the applied bias delta and discontinuity bit do not identify the required
state transformation. The blanket necessity claim in §1.1 and §8 is superseded
by this audit; the recorded field improvements remain valid historical results.
Seven new production-ledger premise tests pass. Runtime behavior is unchanged;
a provenance-bearing frame-transfer contract is recommended before replacement
or promotion, and the field contribution of each mechanism remains unmeasured.
