# PBH: the phase-bias hold, a wire contract for the phase-bias channel

**STATUS**

- **id:** PBH
- **topic:** consumer
- **status:** open
- **summary:** The emit-side phase-bias hold (`network.emit_pb_hold`) is BUILT and ADOPTED: all five PBH-1 bars PASS and the wire's within-arc phase-correction error falls to the clock's (sim 3.0 mm, field 27.4 mm).
- **source:** [PBT-AUDIT](PBT-AUDIT_translation_premise_2026-09-16.md) section 6; [PBT](PBT_consumer_pb_translate_2026-09-15.md); [ALC2](ALC2_restart_policy_adoption_2026-09-19.md) (the baseline pair)
- **knobs / posture:** PBH-0: `network.closed_loop.pb_step_translate_eps_m` ladder on the RF-0hv working bases (main `ca3666e`), config only. PBH-1: `network.emit_pb_hold` (+ `network.emit_pb_hold_max_gap_s`, default 300), default OFF, built on main `28f9735`; adopted 2026-09-20 into `config/refound/{field,sim}_gps_dual_rf0hv.json` with `closed_loop.pb_step_translate: false` and `closed_loop.amb_level_check: false`
- **dates in the text:** 2026-09-19, 2026-09-20
- **hashes / run dirs:** `runs/_ftc_read_20260919/`; `runs/_pbh0_{field,sim}6_{e0,e1,e5,e20}`; exe `gnss_processor_rf0hv2.exe` sha256 `A3D62A24...`; grader `scripts/ftc_step_attrib.py`; PBH-1 arms `runs/_pbh1_{field,sim}6_{H,HA,HP}`, exe `gnss_processor_pbh1.exe` `F1D3BDDB65E554B7`; baseline pair from 2026-09-20 `runs/_rf0hv3_{field,sim}6` (wire `bbaa7fd6...` field, `ea3a646f...` sim)

**Outcome.** The question was whether the wire's phase-bias movement is a frame transfer to translate or error to remove. Only 0.2 % of the translated mass was a known frame move; the zero dead band confirmed the leak hypothesis (PBH-0), and the hold matches that arm to within 0.3 mm per epoch. Under the hold the level check is harmful (field sub-decimetre epochs 167 to 1). ADOPTED 2026-09-20 with the translation and the level check OFF, pinned in the goldens by [GRB](GRB_golden_rebase_2026-09-22.md) and RECOMMEND_SCOPED in the [ledger](../CONFIG_promotion_ledger.md) (A10). Owed: the hold in the state snapshot and the cause of the bias wander ([OPEN_ITEMS_A](../status/OPEN_ITEMS_A_current_program.md)).

## 1. The reads that re-aim the contract (2026-09-19)

All reads are trace-only, on the baseline pair `runs/_rf0hv2_{field,sim}6`
(wire byte-identical to `_alc_{field,sim}6`). Sign convention verified in source:
phase = -clk + pb + A, code = -clk + cb (`MeasurementModel.cpp:1610-1643`); the
consumer applies the same-epoch wire.

**Read 1. What the consumer translation (PBT) translates.** Field, 1684 translated
cells, sum|d_pb| 100.93 m (reproduces the run-end ledger exactly):

| class | cells | sum (m) | share |
| --- | --- | --- | --- |
| known frame move (handover S-transform row for that satellite) | 4 | 0.19 | 0.2 % |
| clock and PB redistribution (phase correction unchanged) | 68 | 1.33 | 1.3 % |
| common to the tracked sky | 363 | 5.16 | 5.1 % |
| phase-correction step at a clock-anchor decouple | 67 | 22.18 | 22.0 % |
| phase-correction step with no producer event | 1150 | 69.69 | 69.0 % |

The no-event class is drift-shaped: 9.7 % of its cells are isolated, the median
run is 8 consecutive epochs, the longest 123. The sim bed has the same shape
(no-event class 77 %, 433 of 445 m in hour 1). The coincidence of translated
steps with frame events is below chance on the field (lift 0.47). Continuous
handovers do not step the wire (median 1.5 mm); cold handovers step it by a
median of 38.9 m, and the `STRANSFORM` event row carries the gauge constant,
not the step. A translator restricted to declared frame moves would cover
0.2 % of the translated mass.

**Read 2. The emit filter is not the source.** `network.emit_pb_filter: true` is
INERT on this posture: it is gated on `carry_invariant`
(`ApplicationNetwork.cpp:919-927`), the loader warns once in `gnss_config.log`,
and no `P` record carries `sigma=`. The wire phase bias is the estimator's own
per-epoch value.

**Read 3. Truth grade.** Ionosphere-free level, de-commoned per epoch with the
difference-median estimator of `wire_vs_code_census.py:304-331`, de-meaned per
arc, hours 2 to 6, satellites the consumer tracks. Within-arc RMS in metres:

| bed | full phase correction (-clk + pb) | clock alone | phase bias alone |
| --- | --- | --- | --- |
| sim (truth) | 0.0199 | 0.0030 | 0.0227 |
| field (COD final clock + OSB) | 0.0693 | 0.0274 | 0.0467 |

The simulator's true phase bias is constant in time (`src/sim/main.cpp:2125-2131`),
so every movement of the wire phase bias is error. The correlation between the
first differences of the phase-bias error and the clock error is +0.006 on both
beds: the phase bias does not compensate the clock, it adds to it. Only 8 %
(field) and 32 % (sim) of the ionosphere-free mass is ionosphere-shaped, so the
consumer's slant-ionosphere state cannot absorb it. On the sim 55 % of the
translated ionosphere-free mass has the equal-cycle shape (d_pb2/d_pb1 = 1.283,
the wide lane untouched), which is the direction code constrains least. A
per-epoch median of levels inflates these numbers up to 20 times through the
rotating satellite set; the difference-median rows above are the trustworthy ones.

## 2. Design consequence

The measured win of the consumer translation is not frame transfer. It is the
consumer ignoring a wandering phase-bias wire while the clock wire is clean
(3 mm on the sim, 27 mm on the field, within arc). The contract that follows is
identity-passing, in the manner of the `iode=` ephemeris cutover: the emitted
phase bias of a (satellite, signal) is HELD at the first value of its generation,
a new generation is declared through the existing discontinuity counter, and the
estimator's own value keeps evolving behind the hold (solve byte-identical).
Handover steps, cold or continuous, never reach the wire, so they need no
signed delta. A delta-passing record is not required for a float consumer.

The cause of the estimator's phase-bias wander is a separate question and is
not needed for the contract. Leads recorded for it: the equal-cycle shape on
the sim; run starts associated with anchor-station slip tiers (lift 4.1) and
phase-anchor events (1.9) on the sim, with no association on the field
(all classes at or below 1.36).

## 3. PBH-0: the pre-registered config-only premise test

For the in-process consumer, translating every change of the applied phase
bias with a zero dead band is mathematically the hold: the effective phase bias
is constant per arc. The shipped dead band is 5 mm, and the baseline ledger
counts 9894 field cell-epochs skipped below it.

**Hypothesis H-LEAK.** The dead band leaks sub-threshold phase-bias drift into
the consumer's ambiguities, and that leak is the accumulated error behind the
hour 4 and hour 5 regression of the translate-only arm (1.58 / 1.90 m), which
the level check later repaired from code.

**Arms.** Both beds, 6 h cold, the adopted RF-0hv bases with
`closed_loop.amb_level_check: false` (a translation restarts the level-check
window, so the check is inert by construction as the dead band goes to zero)
and `closed_loop.pb_step_translate_eps_m` in {0, 0.001, 0.005, 0.02}. The
0.005 arm is the anchor and must reproduce the translate-only pair
`_pbt_{field,sim}6`: field 0.48 / 0.19 / 0.29 / 1.58 / 1.90 / 1.09 m, sim
0.46 / 0.28 / 0.19 / 0.17 / 0.16 / 0.12 m.

**Gates.** G0: `products.txt` of every arm byte-identical to the baseline
wire (consumer-only knob). G1: the 0.005 arm reproduces the anchor numbers to
the centimetre. G2: the run-end translate ledger shows the skipped-below-eps
count falling to the exact-zero population at eps 0.

**Predictions under H-LEAK.** Field hours 4 and 5 ordered
e0 <= e1 < e5 < e20, with e0 below 1.0 m in both hours; sim hours 2 to 6 at e0
no worse than the anchor by more than 0.05 m.

**Falsifier.** Field hours 4 and 5 at e0 within 0.10 m of the anchor: the dead
band is not the accumulated error, and a hold buys the in-process consumer
nothing beyond the translation. The hold then stands only on contract grounds
(a truthful wire for file consumers), and the hour 4 and 5 error needs another
source.

**What a PASS authorizes.** The build card: an emit-side hold
(`network.emit_pb_hold`, default OFF, solve byte-identical, a generation
counter on `SsrPhaseBiasEntry`), graded with the translation OFF against the
baseline pair, with and without the level check (whose window coverage, 20 %
today, is expected to rise once translations stop restarting it).

## 4. 2026-09-19: PBH-0 result, H-LEAK confirmed

Eight arms, 6 h cold, exe `gnss_processor_rf0hv2.exe` (`A3D62A24...`), all exit 0,
720/720 consumer epochs. G0 PASS: `products.txt` byte-identical to the baseline
wire on every arm (`edaf96ce...` field, `2149cabd...` sim). G1 PASS: the 0.005
arm reproduces the translate-only pair to the centimetre on both beds. G2 PASS:
skipped-below-eps 9894 to 0 (field) and 8054 to 0 (sim) at eps 0, with the
translated count rising from 1684 to 11 188 cells (field).

Consumer 3D error, p50 by hour in metres (p90 is nearest-rank):

| arm | field h1..h6 | sub-dm epochs | sim h1..h6 | sub-dm epochs |
| --- | --- | --- | --- | --- |
| eps 0 | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 |
| eps 0.001 | 0.56 0.16 0.19 0.30 0.38 0.41 | 2 | 0.61 0.10 0.10 0.13 0.14 0.13 | 194 |
| eps 0.005 (anchor) | 0.48 0.19 0.29 1.58 1.90 1.09 | 0 | 0.46 0.28 0.19 0.17 0.16 0.12 | 53 |
| eps 0.02 | 4.96 2.62 0.93 1.66 1.83 4.32 | 0 | 2.35 1.91 0.84 0.49 0.25 0.21 | 0 |
| baseline pair (eps 0.005 + level check) | 0.43 0.16 0.31 0.49 0.68 0.49 | 9 | 0.46 0.28 0.25 0.15 0.16 0.14 | 81 |

Field eps 0: p90 by hour 3.31 0.20 0.14 0.12 0.26 0.18; hours 3 to 6 p50 0.113 m,
p90 0.145 m, one excursion to 1.35 m; per 30 min 1.19 0.22 0.19 0.16 0.12 0.07
0.09 0.10 0.12 0.13 0.11 0.11. Sim eps 0: hours 3 to 6 p50 0.059 m, p90 0.115 m,
max 0.213 m.

Against the predictions: field hours 4 and 5 are ordered as predicted
(0.10 <= 0.30 < 1.58 < 1.66; 0.13 <= 0.38 < 1.90, with eps 0.02 at 1.83 in hour 5,
inside 0.07 m of the anchor); eps 0 is below 1.0 m in both hours by a factor of
eight; the sim at eps 0 is better than the anchor in every hour from 2 to 6.
The falsifier did not occur. The 1 mm arm shows the leak is carried by steps
below a millimetre per epoch: the wire phase bias drifts slowly and the dead
band let all of it through.

Two things the ladder does not show. Hour 1 is worse at eps 0 than at the
anchor on both beds (field +0.09 m, sim +0.14 m): in ignition the phase bias
moves with the clock (read 1: field hour 1 slope 1.42, r 0.73), so part of its
movement is signal there. The level check was OFF in every ladder arm, so
these numbers carry no re-validation edge; the level check repaired the leak in
the baseline pair and is not needed for it at eps 0.

## 5. PBH-1: the build, pre-registered 2026-09-19

`network.emit_pb_hold` (bool, default OFF) and `network.emit_pb_hold_max_gap_s`
(default 300). Emit-side only; the solve, the clock records and the code-bias
records are byte-identical with the knob ON.

**Producer.** Per emitted (satellite, signal): the first emitted value of a
generation is held and written to the wire at every later epoch; the
estimator's own value keeps evolving behind it. A new generation starts at a
first sighting, or when the key was not emitted for longer than the gap. The
hold is the last writer of the emitted value, ahead of the emitter's value-jump
test. `SsrPhaseBiasEntry` gains `hold_generation` (0 = not held). Marks from
the existing discontinuity sites are left untouched on the wire. Both edges are
events with counters (invariant 7): `PB_HOLD` rows `gen_start` with cause
`first` or `gap`; run-end counters for keys, generations, and the distribution
of |estimate - held| (what the hold hides); the de-commoned clock step at every
epoch where a mark naming the clock and the phase bias together is ignored
(the cost of not transferring a coupled frame move, measured before any
per-site delta is built).

**Consumer.** For an entry with a nonzero `hold_generation`, a phase-bias
discontinuity counter change is ignored and counted; a generation change
resets that (satellite, signal) ambiguity through the existing reset path.

**Wire text.** A `gen=` token on the phase-bias record and a `pbhold=1` token
on the S line, with the legend extended by the rule at
`NetworkProductIO.cpp:594-618`. If this part grows, it may be deferred, and the
card then states that the hold is field-blocking for file consumers.

**Arms.** Both beds, the RF-0hv bases: H = hold ON, translation OFF, level
check OFF; HA = H with the level check ON; HP = H with the translation ON at
eps 0 (must equal H).

**Bars.** B0 Debug suite green, new suite `PbEmitHoldTest`. B1 knob OFF: the
canonical smoke's `#`-stripped record stream identical before and after. B2
knob ON: `network_epoch_status.csv`, the C and B records of `products.txt`
identical to the baseline arm; every P record constant within a generation.
B3 consumer H: p50 within 0.03 m of the eps 0 arm in hours 2 to 6 on both beds
and within 0.10 m in hour 1; sub-dm epochs at least 150 (field) and 400 (sim);
720/720. B4 wire truth grade (`scripts/ftc_step_attrib.py`, read 3): the full
phase correction's within-arc RMS falls to the clock's (sim 0.0199 to about
0.003 m, field 0.0693 to about 0.027 m). HA is report-only (window coverage,
trips, consumer): the level check is uncalibrated ([ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md)).

Nothing is promoted by this card: no loader default, no golden, no ledger row.
Adoption of the hold, or of eps 0, into the RF-0hv working bases is a separate
decision.

## 6. 2026-09-19: PBH-1 result, the hold BUILT and all five bars PASS

The build is in the tree, default OFF. Exe `build/x64-Release/gnss_processor_pbh1.exe`
sha256[:16] `F1D3BDDB65E554B7` (main `28f9735` plus this card's diff). All six arms exit 0
and emit 720 / 720.

### 6.1 What was built

| piece | file |
| --- | --- |
| the seam, its ordering contract and the coupled-mark census | `include/filter/NetworkPbHold.hpp`; `src/core/filter/NetworkPbHold.cpp` |
| the call site, LAST writer of the emitted phase bias, immediately before `emitToStore` | `src/core/filter/NetworkEmitStore.cpp` |
| the read-only probe of the epoch's pending marks (no card-MS side effects) | `NetworkProductEmitter::pendingMarkChannels`, `src/core/products/NetworkProductEmitter.cpp` |
| the generation on the producer struct and its stamp onto the store entry | `include/products/NetworkProductEmitter.hpp` `NetworkPhaseBias::hold_generation`; `src/core/products/NetworkProductEmitter.cpp` |
| the generation on the consumer struct | `include/rtcm/RtcmTypes.hpp` `SsrPhaseBiasEntry::hold_generation` |
| carried state, counters, knob snapshot | `include/app/NetworkRunContext.hpp` (`NetworkCarryState::pb_hold` / `pb_hold_dev_abs_m` / `pb_hold_coupled_step_m` / `pb_hold_prev_clock_m`, five `n_pbh_*` counters, two `NetworkKnobs` fields); `src/core/app/NetworkRunContext.cpp` |
| knobs, Doxygen and the arming log line | `include/config/GnssConfigTypes.hpp` (`emit_pb_hold`, `emit_pb_hold_max_gap_s`); `src/core/config/GnssConfigLoadNetwork.cpp` |
| the consumer decision, isolated the way cards CC and PBT isolate theirs | `include/realtime/PbHoldLedger.hpp` (header-only) |
| the consumer seam, dispatched first and skipped by the other three | `src/core/realtime/RealTimeEngineSsrBreaks.cpp` `reactToPbHold_`; `RtEpochContext::pb_hold_handled` |
| wire text: `gen=` on P, `pbhold=1` on S, legend, parser, `checkContract` | `src/core/products/NetworkProductIO.cpp` |
| run-end telemetry, producer and consumer | `src/core/filter/NetworkTeardown.cpp` |
| gtest, 9 cases in 1 new suite | `tests/test_pb_emit_hold.cpp` (`PbEmitHoldTest`) |

Three design points worth recording, because each was a choice and not a default.

**The hold sits at the very end of the emit block, not next to the re-base.** That
position buys three things at once: it freezes what the card-4 emission re-base and
the PB emission filter produced rather than being overwritten by them; it is ahead of
the emitter's phase-bias value-jump test, so the declared population screens the value
the consumer receives; and every marking site of the epoch has already run, so the
coupled clock plus phase-bias census sees the complete mark set.

**The consumer keys on the DATA, never on a knob.** A consumer cannot know that the
producer armed `emit_pb_hold`; it can only read a nonzero `hold_generation`. Zero means
an unheld product, which is every RTCM stream, every pre-PBH file and every OFF run, and
the three existing reactions then run unchanged. Keys the hold reaction handles are
recorded in the per-epoch context and skipped downstream, which preserves the once-only
guarantee: exactly one seam reacts per (key, epoch).

**The A5 freshness flip is deliberately not suppressed.** It is a step in the APPLIED
correction, a bias gated out and back in, which the hold does not touch. It is also
unreachable on these postures, where `processing.enable_ssr_bias_max_age` is off.

### 6.2 Bars

| bar | result | numbers |
| --- | --- | --- |
| **B0** Debug suite green with the new suite | **PASS** | `2297 tests from 303 test suites ran`, `2296 passed`, `1 skipped` (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the standing bootstrap-gated skip). That is the 2287-passing baseline plus this card's 9 cases in 1 new suite. |
| **B1** knob OFF, canonical smoke | **PASS** | `runs/_pbh1_smoke_pre` (pre-change exe `A3D62A242C04E545`) and `runs/_pbh1_smoke_off` (new exe): the hash-stripped record stream is **`333DCB0871E1F5BC`** over **6 596** record lines on both, the canonical value. Whole file `B05F26B1CD0540DB` to `6BCE6BAC94063FCF`; the entire difference is TWO legend comment lines (`gen=<n>` on the P grammar, `pbhold=1` on the S grammar), required by the frozen rule in `NetworkProductIO::writeHeader_` and pinned by `HeaderLegendDeclaresEveryEmittedField`. |
| **B2** knob ON, solve and non-phase wire identical | **PASS** | Arm H against `runs/_rf0hv2_*`, both beds. `logs/network_epoch_status.csv` IDENTICAL (`939727D60970F6B5` field, `E4FC04F44D51F1EE` sim). Of `products.txt`: **C records IDENTICAL** (`AC57C356BC666B0E` / `7E4895C5B3E879E3`, 22 586 lines each bed), **B records IDENTICAL** (`956B1F24CD4EE7C7` / `D6292D389CDD194C`), and so are the **E** and **R** records (720 E, 36 field / 10 sim R). Only the P records move, as designed. Every P value is **constant within its generation: max change exactly 0.0, one distinct value per generation**, on both beds. |
| **B3** consumer, arm H | **PASS** | See 6.3. Field and sim H reproduce the eps-0 reference **to 0.0000 m in every hour** (bar: 0.03 m hours 2 to 6, 0.10 m hour 1); sub-dm epochs **167** field (bar 150) and **448** sim (bar 400); **720 / 720** both. HP is **byte-identical to H**, `closed_loop_trace.csv` and `closed_loop_sat_trace.csv` included. |
| **B4** wire truth grade, arm H | **PASS** | See 6.4. Field `e_phi` **0.0693 to 0.0274 m**, exactly the clock alone (expected about 0.027); sim `e_phi` **0.0199 to 0.0030 m**, exactly the clock alone (expected about 0.003). `e_pb` goes to **0.0000 m** on both. |

`python scripts/docs_index_check.py` and `python scripts/make_golden_config.py --verify`
both PASS. `scripts/rt_seam_verify.py` reports **22 verbatim / 8 FAILED**, which is
**unchanged**: the identical eight seams fail on `28f9735` without this diff (measured by
restoring the three touched files and re-running), so this card adds **0 new** ones. The
verifier needs `runs/rtseam_goldens/pre/src/*.frozen`, which the repo-root cleanup moved
to `D:\GnssProcessorSupport\runs\`; it was copied back to run this check.

### 6.3 B3: the consumer, 6 h cold, p50 by hour in metres

The graded column is METRES throughout (`pos_err_3d_m` of `logs/closed_loop_trace.csv`,
median per hour; p90 is nearest-rank).

| arm | field h1..h6 | sub-dm | sim h1..h6 | sub-dm |
| --- | --- | --- | --- | --- |
| **H** (hold ON, translate OFF, level check OFF) | **0.57 0.17 0.11 0.10 0.13 0.11** | **167** | **0.60 0.10 0.10 0.06 0.06 0.05** | **448** |
| **HP** (H plus translate at eps 0) | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 |
| **HA** (H plus level check) | 1.10 0.69 0.51 0.22 0.12 0.16 | 1 | 0.97 0.86 0.40 0.17 0.24 0.18 | 14 |
| eps-0 reference (`_pbh0_*_e0`) | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 |
| baseline pair (`_rf0hv2_*`) | 0.43 0.16 0.31 0.49 0.68 0.49 | 9 | 0.46 0.28 0.25 0.15 0.16 0.14 | 81 |

H p90 by hour: field 3.31 0.20 0.14 0.12 0.26 0.18; sim 3.38 0.16 0.15 0.10 0.09 0.10.
Both reproduce the eps-0 reference digit for digit.

**The emit-side hold and the zero-dead-band consumer translation are the same
operation, and the run proves it numerically rather than by argument.** Matched
epoch by epoch against `_pbh0_*_e0`, the maximum absolute difference in 3D error over
all 720 epochs is **0.0001 m on the field and 0.0003 m on the sim**, with a median
difference of exactly 0. Section 3 predicted this, that translating every change of the
applied phase bias with a zero dead band is mathematically the hold; PBH-1 measures it
at 0.1 to 0.3 mm. The residual is not a discrepancy in the mechanism, it is where the
two differ in principle: the consumer translation cannot act before it has a (k-1, k)
pair and it works on the 6 to 10 satellites the consumer TRACKS, while the producer hold
freezes from the generation's first emitted epoch across all 95 (field) or 64 (sim)
emitted keys.

**HP is the consistency check and it passes in the strongest available form.** With
the hold ON the applied phase bias is constant inside a generation, so the
translation's step is identically zero: the PBT ledger reports
`sum_abs_translate_m 0.000 (max |d| 0.000 m)`, `skipped_eps 0`, `no_pair 0`,
`closure HOLDS`, and both consumer traces are byte-identical to H on both beds. A
translator downstream of a hold is a no-op, which is the arithmetic the card claimed.

**HA is report-only, and it is WORSE, on both beds.** The level check's coverage rises
exactly as section 5 predicted, and the consumer still loses:

| bed | window restarts | checks / samples | coverage | trips (wl / if) | refloats | consumer p50 h1..h6 |
| --- | --- | --- | --- | --- | --- | --- |
| field, baseline `_rf0hv2` | 1163 | 1145 / 5799 | 0.197 | 106 (27 / 79) | 106 | 0.43 0.16 0.31 0.49 0.68 0.49 |
| field, **HA** | **9** | **2424 / 5799** | **0.418** | 158 (35 / 123) | 158 | 1.10 0.69 0.51 0.22 0.12 0.16 |
| sim, baseline `_rf0hv2` | 1508 | 1626 / 5387 | 0.302 | 26 (2 / 24) | 26 | 0.46 0.28 0.25 0.15 0.16 0.14 |
| sim, **HA** | **18** | **3906 / 5387** | **0.725** | 58 (5 / 53) | 58 | 0.97 0.86 0.40 0.17 0.24 0.18 |

Window restarts collapse by two orders of magnitude, because under a hold nothing
writes the ambiguity: there is no translation, no carry and no declared reset to
restart the window with. Coverage roughly doubles on both beds and the check fires 1.5
to 2.2 times as often. The consumer nonetheless goes from 167 sub-dm epochs to 1 on the
field and from 448 to 14 on the sim, and is worse in five of six hours on both beds.
The reading is that a re-validation designed against a MOVING wire is re-levelling from
code an ambiguity that no longer has anything to be re-validated against, so its own
code-level noise becomes the dominant error. The check is uncalibrated
([ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md)); this is a measurement on a
posture it was not tuned for, not a verdict on the check.

### 6.4 B4: the wire against truth

`python scripts/ftc_step_attrib.py <run_dir> --r3`, consumer-tracked satellites,
difference-median de-commoning, within-arc RMS after removing the arc mean, hours 2 to 6,
metres. The sim run takes
`--truth runs/simnet60_quiet_wu93_24h/SZZ000SIM/truth_bias.csv`.

| bed | channel | baseline `_rf0hv2` | arm **H** |
| --- | --- | --- | --- |
| sim (truth) | `e_phi` full phase correction | 0.0199 | **0.0030** |
| sim | `e_clk` clock alone | 0.0030 | 0.0030 |
| sim | `e_pb` phase bias alone | 0.0227 | **0.0000** |
| field (COD final clock plus OSB) | `e_phi` | 0.0693 | **0.0274** |
| field | `e_clk` | 0.0274 | 0.0274 |
| field | `e_pb` | 0.0467 | **0.0000** |

The graded population is unchanged (18 arcs / 4359 points sim, 18 arcs / 4882 points
field), so this is the same satellites over the same epochs. `e_pb` is **exactly**
0.0000 at p50, p90 and max, because within an arc the held bias is a constant and the
per-arc de-mean removes a constant exactly. `e_phi` therefore collapses onto `e_clk` row
for row, including p90 and max: sim 0.0030 / 0.0037 / 0.0042, field 0.0274 / 0.0655 /
0.0865. The correlation of the first differences is undefined because one series is
identically zero. The ALL-hours rows behave the same way (sim `e_phi` 0.0532 to 0.0041,
field 0.0780 to 0.0402).

This is what identity-passing buys, stated at its limit: the phase-bias channel of the
wire now contributes NOTHING to the within-arc error of the phase correction, and the
product's phase quality is the clock's quality. The per-epoch-median rows of the same
read still show large numbers (field `e_phi` 1.548 to 0.859, sim 0.634 at hours 2 to 6),
which is the instrument's own inflation through the rotating satellite set that section 1
already flagged; the difference-median rows are the trustworthy ones.

### 6.5 Telemetry, both edges

Producer, arm H, from the run-end summary:

| quantity | field | sim |
| --- | --- | --- |
| keys held | 95 | 64 |
| generations (first / gap) | 109 (95 / 14) | 74 (64 / 10) |
| held emissions | 60 316 | 45 098 |
| abs(estimate minus held) p50 / p90 / max (m) | 0.7167 / 3.6388 / 74.8122 | 5.8591 / 16.3906 / 93.2241 |
| coupled clock plus phase-bias marks reaching a held key | 201 | 36 |
| de-commoned satellite clock step at those epochs, p50 / p90 / max (m) | 0.0591 / 0.3290 / 1.3337 | 0.0014 / 0.0063 / 1.5531 |

Consumer, arm H: field **11 618 cells over 40 held keys, 40 first sightings, 11 578 held,
0 refloats, 114 discontinuity-counter changes IGNORED**, closure HOLDS; sim **10 800 cells
over 38 keys, 38 first sightings, 10 758 held, 4 refloats, 22 ignored**, closure HOLDS. The
producer holds 95 and 64 keys against the consumer's 40 and 38 because the hold-out station
tracks a subset of the emitted sky.

Two readings.

**What the hold hides is large, and that is the point rather than a caveat.** A median
0.72 m (field) and 5.86 m (sim) of estimator motion is suppressed per emission, with
maxima of 75 and 93 m over a six-hour generation. Section 1 read 3 established that this
motion is error on the sim by construction, and B4 now shows that removing all of it
takes the phase correction's within-arc error down to the clock's exactly. The number is
the decoupling edge's cost, measured on every emission rather than sampled, and it is what
a future card must beat if it wants to transfer any part of that motion instead.

**The one move the hold refuses to transfer is small on the sim and not small on the
field.** 201 field epochs carry a mark naming the clock and the phase bias together on a
held key, and the de-commoned satellite clock step at those epochs runs to 1.33 m with a
p50 of 5.9 cm; the sim's 36 sit at a p50 of 1.4 mm. The consumer nonetheless shows 0 field
refloats and a better trajectory than every prior arm, so on this posture the refusal
costs nothing measurable. The field number is the one to watch if a coupled frame-transfer
contract is ever built.

### 6.6 Deferred and limitations

- **Nothing is deferred from the build.** The wire text was carried: the `gen=` P-record
  token, the `pbhold=1` S-line token, the legend extension by the frozen rule in
  `writeHeader_`, the reader's `gen=` parse into `hold_generation`, and `checkContract`
  naming a `pbhold=` disagreement in both directions. All are pinned by `PbEmitHoldTest`.
- **The `pbhold=1` token never appears on this lineage**, because
  `network.serialize_sbasis_contract` is off and the header is v1, so no S line is
  written at all. This is exactly JT's trap with `discdecom=`: the token is pinned only
  by the gtest, and any arm that wants a file consumer to read the contract must arm the
  contract serializer. The per-record `gen=` token IS present on every P record of both
  arms (60 425 field, 45 172 sim, none missing).
- **Warm resume is out of scope and cold-starts the hold.** The held values and
  generations are not snapshotted, so a resumed producer treats every key as a first
  sighting and restarts at generation 1. A consumer holding generation 2 then sees a
  change and re-floats, which is the conservative direction and the same choice the
  slant-iono carry makes, but it is a seam nobody has graded. The arms here are 6 h cold.
- **The 300 s gap rule is untuned.** It fired on 14 field and 10 sim keys. Those
  re-seedings are not visible in the B4 arc census, whose arc counts are identical to the
  baseline, which suggests they fall at arc boundaries the grader already splits on; that
  was not tested directly.
- **A one-generation-per-arc hold is a ceiling, not a policy.** Nothing here answers when
  a generation SHOULD restart on evidence rather than on a gap. B4 measures the best case
  a restart policy has to beat.

Nothing is promoted by this card: no loader default, no golden, no ledger row,
`config/refound/*_rf0hv.json` untouched. Adoption of `network.emit_pb_hold` into the
RF-0hv working bases is a separate decision, and it is the same decision as adopting
eps 0, since the two are measurably the same operation.

**Runs.** `runs/_pbh1_{field,sim}6_{H,HA,HP}`, `runs/_pbh1_smoke_pre`,
`runs/_pbh1_smoke_off`, `runs/_pbh1_ftc/{field,sim}_H_r3.txt`. Exe
`gnss_processor_pbh1.exe` `F1D3BDDB65E554B7`. Wire sha256[:16] of arm H:
`BBAA7FD66844CB3E` field, `EA3A646F8972AE44` sim.

## 7. 2026-09-20: re-grade from the run directories, and the reading

Re-graded independently from `runs/_pbh1_{field,sim}6_{H,HA,HP}`: the H table of
section 6 reproduces to the digit; `closed_loop_trace.csv` of HP is byte-identical
to H on both beds; `network_epoch_status.csv` of H is byte-identical to the
baseline pair on both beds; the field wire carries 60 425 P records in 109
(key, generation) groups with exactly one distinct value per group.

**The hold is the zero-dead-band translation, moved to the producer.** H equals
the eps 0 arm to 0.1 to 0.3 mm per epoch, so the consumer result of section 4 is
now a property of the wire: any float consumer of `products.txt` gets it with no
translation logic, and the truth-graded phase correction equals the clock
(section 6, B4).

**The level check is harmful under the hold.** With translations gone its window
coverage doubles (field 0.197 to 0.418, sim 0.302 to 0.725), trips rise 106 to 158
and 26 to 58, and the consumer loses in five of six hours on both beds (field
1.10 / 0.69 / 0.51 / 0.22 / 0.12 / 0.16 m, sim 0.97 / 0.86 / 0.40 / 0.17 / 0.24 /
0.18 m). This is the over-firing of the restart-policy arm
([ALC2](ALC2_restart_policy_adoption_2026-09-19.md)) and of the statistics audit
([ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md)) reproduced a third time: the
check earned its place by repairing the dead-band leak, and with the leak closed
at its source it has no population left. Under the hold it stays OFF.

**What the field number is made of.** Arm H, hours 3 to 6, metres:

| bed | east mean / std | north mean / std | up mean / std |
| --- | --- | --- | --- |
| field | +0.054 / 0.033 | +0.017 / 0.028 | -0.068 / 0.104 |
| sim | +0.002 / 0.019 | -0.005 / 0.017 | +0.054 / 0.047 |

The field 3D median of 0.11 m holds a quasi-constant offset of 0.089 m (the
reference coordinate, antenna and frame budget of the hold-out station) next to
a scatter of 3 cm horizontal and 10 cm vertical, on an epoch-wise (white-noise
position) float solution. The consumer bar of the re-founding (first
sub-decimetre epochs, then hours) is met on both beds from hour 3.

**Owed, in order.** (1) The adoption decision for the RF-0hv working bases:
`emit_pb_hold: true` with `pb_step_translate: false` and `amb_level_check: false`;
hour 1 is 0.14 m worse than the present pair on both beds, hours 2 to 6 are 2 to
5 times better. (2) `serialize_sbasis_contract` on a lineage, so the `pbhold=1`
and `discdecom=` S-line tokens exist outside gtest. (3) The hold is not in the
state snapshot: a warm resume cold-starts every generation (conservative,
ungraded). (4) The 201 field marks naming clock and phase bias together reach
held keys with a de-commoned clock step of p50 0.059 / p90 0.329 / max 1.334 m:
with the phase correction now equal to the clock, the clock-anchor decouple
excursion (EV6) is the largest remaining wire term on the field
(clock-alone within-arc RMS 27 mm against 3 mm on the sim). (5) Why the
estimator's phase bias wanders (section 2 leads); it no longer reaches a float
consumer, and it bounds any integer add-back.

## 8. 2026-09-20: the hold ADOPTED into the RF-0hv working bases

At the author's instruction `config/refound/{field,sim}_gps_dual_rf0hv.json` now
carry `network.emit_pb_hold: true`, `closed_loop.pb_step_translate: false` and
`closed_loop.amb_level_check: false` (three keys plus dated comment lines).
Working bases only: no loader default, no golden, no ledger row.

Confirmation arms from the adopted bases with only the product path changed,
exe `gnss_processor_pbh1.exe` (`F1D3BDDB...`), both exit 0 (245 s field, 192 s
sim): `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv` are
byte-identical to the PBH-1 arm H on both beds (wire `bbaa7fd6...` field,
`ea3a646f...` sim).

**The baseline pair from this date: `runs/_rf0hv3_{field,sim}6`.** Consumer 3D
p50 by hour: field 0.57 / 0.17 / 0.11 / 0.10 / 0.13 / 0.11 m (p90 3.31 / 0.20 /
0.14 / 0.12 / 0.26 / 0.18; 167 epochs below 0.10 m), sim 0.60 / 0.10 / 0.10 /
0.06 / 0.06 / 0.05 m (p90 3.38 / 0.16 / 0.15 / 0.10 / 0.09 / 0.10; 448 epochs),
720/720 on both. Every add-back is a delta against this pair.

Pre-flight note for the L5 add-back: the hold is a stiff quantity in the sense
of invariant 1. A constant true phase bias is physically sound on L1 and L2; the
GPS Block IIF L5 signal carries a thermally driven inter-frequency clock
variation of order a decimetre, so on L5 part of the phase-bias movement is
signal, and the hold needs a per-signal scope or a declared slow-variation
channel there.
