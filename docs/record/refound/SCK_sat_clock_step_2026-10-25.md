# SCK: why a satellite clock step is not survived on any clock-level posture

**STATUS**

- **id:** SCK
- **topic:** clock
- **status:** closed
- **summary:** MEASURED and ATTRIBUTED 2026-10-25 (row `sat_clk` of card SMX: +0.5 m on every code and phase of G25 at every station, consumer included, at TOW 275910). One cause is common to the three postures and one is specific to each. Common: the IF-TD slip layer reads the step as a cycle slip at all 16 stations tracking G25 (statistic 0.45 to 0.54 m against thresholds 0.11 to 0.29 m) and refloats every G25 phase arc, and the satellite's phase datum re-realizes cold by the next epoch, so its clock level is left to the code; and the emit-side phase-bias hold keeps G25 in its first generation, so the wire's phase-visible clock does not carry the step and the consumer's hold reaction refloats nothing. Golden (`g`): the clock anchor trips on its phase statistic (18.9 sigma) and the re-solve emits -0.500 m, but from +1 the level is the code's (-0.714 m at +4), the anchor re-couples on that value at +4 and its q of 2.9e-8 lets the code pull it back only to -0.585 m by +49; the consumer refloats G25 through its own filter and drifts 0.14 m on the code-side error. Self-contained regime (`c`): the same wire, -0.547 m at +49; the consumer accepts a +0.498 m phase innovation on a continuing G25 arc and moves 1.50 m at the step, 2.35 m at +5. Native (`n`): the anchor never decouples; the same-epoch cold re-realization of the L2W family removes the phase statistic its guard decides on, the clock statistic reads 2.4 sigma and the 2.0 m backstop is far, so the anchored clock takes 0.04 m at the step and 0.18 m by +50 and the consumer takes the rest (1.0 to 1.2 m). Confirmation with the hold off (emit-side only): clock and code-bias records byte-identical; the `c` consumer's step-epoch jump falls from 1.504 m to 0.007 m, on a wire whose consumer null is 8 times noisier. A caveat for every consumer number of card SMX: the leg consumer is 0.7 to 1.2 m off in every null leg, against 1 to 7 cm in the seeds over the same window. Nothing built or armed; four decisions for the author (section 10).
- **source:** card [SMX](../../refound/SMX_scenario_matrix_2026-10-24.md) section 9.3 item 2 (the row fails on every posture, three different ways); the brief of the session that ran the matrix: diagnose per posture what moves the wire on `g`, what makes the `c` consumer jump although the wire carries the step, and why the clock step test does not release the anchor on `n`; measurement and attribution only. Background: [SCB](SCB_sim_clock_error_bed_2026-10-24.md) (the posture key sets), [SCL2B](SCL2B_shared_history_with_the_hold_2026-10-07.md) and [NBP](NBP_native_broadcast_clock_posture_2026-10-07.md) (the regimes of `c` and `n`)
- **knobs / posture:** no knob built, armed or changed; no golden or ledger edited. Postures as card SMX: `g` = `config/golden/golden_sim_refound.json` (GPS + Galileo, `clock_anchor_q` 2.9e-8, `emit_pb_hold_phase_clock`, wire `pbhold=2`); `c` = GPS only plus the regime of card SCL2B (`clock_anchor_q` 5e-6, `emit_pb_hold_smoothed`, `pbhold=3`, the clock clone); `n` = `c` plus `network.apriori_source` `sp3_brdc_clock`. All three: `clock_anchor_guard_space` `phase`, `clock_anchor_k` 6.0, `clock_anchor_abs_m` 2.0, `clock_anchor_recouple_epochs` 4, `cycle_slip.if_phase_td` armed. The confirmation legs (section 8) differ from their SMX twins in `network.emit_pb_hold` false (and the hold's mode key false) only.
- **dates in the text:** 2026-10-25
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/worktrees/smx/build/x64-Release/gnss_processor_smx1.exe` (sha256[:16] `cac2bbdea1d05965`, card SMX's); legs under `D:/GnssProcessorSupport/runs_nom`: card SMX's `evleg_smx_smx1_sat_clk_{g,c,n}` and `evleg_smx_smx1_null60_{g,c,n}` (seeds `evseed_smx_{g,c,n}`, snapshot `ep0485`, TOW 275580, 60 epochs, event at `at_ep` 10), and this card's `evleg_sck_hold0_{sat_clk,null60}_{c,g}` (same seeds, snapshot and event; made with `scripts/event_leg.py` of branch `claude/sweet-poincare-454cec`, all four exit 0, 49 to 74 s each); traces read: `products.txt`, `logs/network_events.csv`, `logs/network_anchor_guard.csv`, `logs/network_phase_anchor.csv`, `logs/network_station_residuals.csv`, `logs/closed_loop_trace.csv`, `logs/closed_loop_sat_trace.csv`, `stdout.txt`
- **gates:** none (an attribution card). The predictions for the confirmation legs were written before any of them was read (section 8).

**Outcome.** Question: why is a +0.5 m satellite clock step not survived on the golden, the self-contained regime or the native posture, when the 2026-09-10 harness validation absorbed one in one epoch? Decisive result: on all three the slip layer refloats every arc of the satellite at the step (16 of 16 stations) and the phase datum re-realizes cold within one epoch, so the clock level falls to the code; the hold keeps the wire's phase-visible clock at its pre-step value with no generation edge, so the consumer's phase never receives the step. The postures differ in what holds the level after that: the golden re-couples a stiff anchor on a code value 0.21 m off, and the native posture's guard loses its phase statistic in the same epoch and never decouples. Verdict: three separable mechanisms, each named by rows. Standing: section 10; nothing armed.

## 1. The question

Card SMX's row `sat_clk` steps the clock of G25 by +0.5 m at TOW 275910 (every code and phase of G25 at every station, the held-out consumer SZZ000SIM included; 120 observables). The wire must carry -0.5 m on G25 from that epoch. It fails on every posture: on `g` the wire overshoots (-0.685 m at +3, still -0.585 m at +49) and the consumer drifts to 0.14 m; on `c` the wire carries the step at +0 yet the consumer jumps 1.5 m, 2.35 m at +5; on `n` the wire takes 0.04 m and the consumer is 1.0 to 1.2 m off. The 2026-09-10 harness validation absorbed 100.4 % of a step in one epoch with a 3 cm consumer effect, on the retired triple-frequency golden that predates the re-founding goldens of 2026-09-14; which later change turned the outcome is not separated here.

## 2. Method

Every number is event minus null on the same exe and snapshot; the two legs agree exactly before the event. The wire clock and phase bias are de-commoned per epoch by the GPS median of the clock difference (the median moves by at most 2 mm on `g` and `c`, 6 mm on `n`). "+k" is the k-th 30 s epoch after the step. The consumer is read from `closed_loop_trace.csv` (position error) and `closed_loop_sat_trace.csv` (per satellite and signal: the applied clock, code bias and phase bias, the innovations, the ambiguity and its variance, and what the consumer did with each declaration: `ev_pb`, `ev_cb`, `ev_clk`). The anchor's guard is read row by row from `network_anchor_guard.csv` (`e_clk`, `w_clk`, `e_phi`, `w_phi`, `n_pb`, decision).

## 3. Common to every posture: the step becomes a network-wide refloat

**The slip layer.** At the step epoch the IF-TD tier flags G25 at 15 stations (`T1_if_slip` `if_slip`, action `refloat L1C;L2W`, statistic 0.475 to 0.518 m on `g` and `c`, 0.445 to 0.540 m on `n`, thresholds 0.107 to 0.154 m, to 0.286 m on `n`), and at the sixteenth, S40000SIM, where G25 is the reference satellite of the IF-TD chain, its reference gate resets the arc (`refsat_reset`). Sixteen stations track G25 (the 15 of the residual trace plus the anchor station, which that trace omits): every L1C and L2W arc of the satellite is refloated, on all three postures. The detector differences the IF phase observed minus computed between satellites and between epochs (`include/slip/IfPhaseSlipDetector.hpp`); the computed value subtracts the a priori satellite clock (COD on `g` and `c`, broadcast on `n`), which does not contain the step, so the step sits whole in G25's between-epoch difference at every station, indistinguishable at one station from a slip. The layer tests each station's pooled differences; no test asks whether a flag is common to every station of one satellite with one magnitude, which is the signature of a satellite clock event (the reference gate's coherent-step test is the station-side analogue, and it is what fired at S40000SIM).

**The phase datum.** The anchor station's arcs are among the refloated ones, so the G25 families hand over at the step epoch: on `g` and `c` from S24000SIM to S40000SIM, continuous (`PBGAUGE handover_continuous`, S-transform basis 13.23 m on L1C and 11.39 m on L2W on `g`); on `n` from S45000SIM to S40000SIM, L1C continuous and L2W cold (`handover_cold`, `re_realize`; the run-end clause counts 1 COLD FALLBACK, "the successor had no usable carried value"). At +1, on every posture, both families re-realize cold at S32000SIM (`sts_cold_declared`, basis 0, `marked=1`, a `RETRO retro_bump`). From +1 no phase arc of G25 carries a level from before the step, and no anchor pin does either.

**What is left of the clock level.** With every arc newborn and the datum re-realized, the phase constrains the change of G25's clock from epoch to epoch but not its level; the level comes from the code (and from whatever prior holds the clock). The URA the wire emits for G25 says so on `g`: 0.023 m before the step, 0.037 m at +0, 0.158 m at +1, 0.047 m at +49 (null 0.012 m). The guard rows say it too: at +1 G25 has no phase-bias key in the folded block (`n_pb` 0, `e_phi` empty) on every posture.

**The hold.** On all three postures the emitted phase bias of G25 stays in generation 1 throughout (`gen=1` on every P record of both legs). On `g` (`pbhold=2`) the held bias moves with the de-commoned clock by construction (`p(t0) + [c~(t) - c~(t0)]`, the loader's statement): the phase-bias difference equals the clock difference to 2 mm at every epoch (-0.500, -0.601, -0.714 m at +0, +1, +4), so the phase-visible clock is unchanged. On `c` (`pbhold=3`) the same holds to 2 mm at +0 (clock -0.499, phase bias -0.497 m) and 6 mm at +4; a `PB_HOLD smh_bridge` row withdraws a 0.009 m transfer. On `n` the hold records the move it does not transfer (`PB_HOLD coupled_mark`, -0.041 m, `not_transferred`); the phase-visible clock follows the small clock move there (-0.035 m at +0). On the consumer side a held key is handled by `RealTimeEngine::reactToPbHold_` (`src/core/realtime/RealTimeEngineSsrBreaks.cpp`): a phase-bias discontinuity bump without a generation change is consumed and suppressed by design, and only a generation change refloats. The wire does declare the event (an `R` record naming G25 at +0 and +1, and clock and code-bias discontinuity bumps, which the consumer answers with `reinflate_isb` and `reinflate_disc`), but no declaration reaches the consumer's G25 ambiguity.

## 4. Posture `g`: what moves the wire after the step

| epoch | wire clock G25 (m) | guard: `e_clk` / `w_clk` | guard: `e_phi` / `w_phi` / `n_pb` | anchor | URA (m) |
|---|---|---|---|---|---|
| -1 | 0.000 | -0.0003 / -0.04 | 0.0002 / 0.01 / 2 | anchored | 0.023 |
| +0 | -0.500 | -0.0013 / -0.15 | -0.387 / -18.9 / 2 | trip (`phi`), re-solve | 0.037 |
| +1 | -0.601 | -0.102 / -1.31 | none / 0 | loose | 0.158 |
| +2 | -0.663 | -0.064 / -0.34 | -0.012 / -0.07 / 2 | loose | 0.127 |
| +3 | -0.685 | -0.022 / -0.13 | -0.011 / -0.06 / 2 | loose | 0.114 |
| +4 | -0.714 | -0.031 / -0.18 | 0.021 / 0.12 / 2 | re-couple, re-seed var 1.05e-2 m2 | 0.107 |
| +13 | -0.669 | | | anchored, q 2.9e-8 m2/s | 0.076 |
| +25 | -0.614 | | | | 0.061 |
| +49 | -0.585 | | | | 0.047 |

The sequence: (1) at +0 the guard trips on the phase-visible statistic (`cta_decouple`, reason `phi`, e_phi -0.387 m at sqrt S 0.020 m); the clock-space statistic reads -0.15, because at q 2.9e-8 the anchored posterior moves only 1.3 mm (the run-end maximum of the clock-space |w| is 3.39, on G18), so the phase statistic is the only part of the test that sees the step. The epoch is re-solved without the clock prior (`clock-anchor refloat ... RE-SOLVED before emission`) and the wire carries -0.500 m, exactly, through the continuously handed-over anchor at S40000SIM. (2) At +1 the G25 datum re-realizes cold (section 3) and the clock, LOOSE at the legacy `sat_clock_q` (1e-3 m2/s, about 0.17 m per epoch), takes its level from the code: -0.601 to -0.714 m from +1 to +4, a code-level error of 0.10 to 0.21 m inside the 0.11 to 0.16 m URA the producer emits. (3) After four in-band tests the anchor re-couples at +4 (`cta_recouple`, `reseed_var` 1.05e-2 m2) on the code-determined value; from there the anchor's walk target (0.9 mm per 30 s) lets the code move the level by a few millimetres per epoch: -0.669 m at +13, -0.614 at +25, -0.585 at +49. The wire's phase bias moves with the clock at every epoch (the hold, section 3); the code biases do not move.

So on `g` the overshoot is not the anchor resisting the step (it released at +0 and the re-solve was exact); it is the loss of every phase memory of the level at +0 and +1, after which a stiff anchor is re-seeded on a code value 0.21 m off.

**The consumer on `g`.** At +0 its G25 phase rows are absent from the committed innovations and both G25 ambiguities are reset (variance 1e6) with no declared reaction recorded (`ev_pb` none; the hold suppressed the phase-bias bump). The consumer's raw-observable slip detectors cannot see a clock step, which is common to code and phase on both bands; the reset is read as the filter's post-fit outlier response (`KalmanOutlierResponse.cpp`, whose log is at debug level and not written in these legs). From +1 the G25 phase is consistent (innovation change -0.005 to -0.009 m) and the code carries the wire's overshoot (G25 code residual +0.25 to +0.35 m against the null). The position difference grows from 0.012 m at +1 to 0.036 m at +20 and 0.142 m at +49 (Up +0.127 m), the cell's 147 mm peak.

## 5. Posture `c`: why the consumer jumps although the wire carries the step

The producer runs the same sequence as on `g`: IF-TD refloat at 16 of 16 stations, continuous handover at +0 and cold re-realization at +1, guard trip at +0 on the phase statistic (`w_phi` -15.1; `w_clk` -7.9, since q 5e-6 lets the anchored clock move 0.149 m), re-solve to -0.499 m, LOOSE code-level clock at +1 to +4 (-0.595, -0.634, -0.656, -0.699 m), re-couple at +4 (`reseed_var` 1.49e-2 m2). The looser anchor lets the code pull the level back faster: -0.580 m at +10, -0.547 m at +49.

The consumer's jump is the hold's. At +0 the wire's code correction carries the step (G25 code innovation change +0.001 m on L1C), and the phase-visible clock carries 2 mm of it, so the G25 phase innovation changes by +0.498 m on L1C and on L2W. The consumer's G25 ambiguities continue (variance 0.089 m2 against 0.098 m2 in the null, no reset): the hold reaction suppresses the phase-bias bump, there is no generation change, and the filter does not reject the row. The update absorbs the 0.5 m into position: 1.504 m at +0 (Up -1.451 m), then 1.84 to 2.35 m from +1 to +5 as the code-side overshoot (G25 code innovation change -0.26 to -0.44 m) adds to it, decaying to 0.56 m at +49. On `g` the same phase innovation was rejected; on `c` the consumer has 8 GPS satellites and loose float ambiguities (section 9), and the row is accepted. Whether a converged GPS-only consumer would reject it is not measured.

## 6. Posture `n`: why the clock step test does not release the anchor

| epoch | wire clock G25 (m) | guard: `e_clk` / `w_clk` | guard: `e_phi` / `w_phi` / `n_pb` | decision |
|---|---|---|---|---|
| -1 | 0.000 | 0.0011 / 0.05 | 0.0016 / 0.08 / 2 | ok |
| +0 | -0.038 | -0.042 / -2.40 | none / 0 | ok |
| +1 | -0.047 | -0.011 / -0.62 | none / 0 | ok |
| +2 | -0.053 | -0.013 / -0.65 | -0.020 / -0.52 / 2 | ok |
| +3 | -0.058 | -0.008 / -0.39 | -0.036 / -1.02 / 2 | ok |
| +10 | -0.082 | | | anchored |
| +21 | -0.113 | | | anchored |
| +50 | -0.177 | | | anchored |

The guard runs in phase mode (`clock_anchor_guard_space` `phase`): the phase-visible statistic decides the trip, and a satellite without one is tested on the absolute backstop only (`src/core/network/solve/ClockAnchor.cpp`, the test loop; `clock_anchor_abs_m` 2.0 m). The statistic needs two phase-bias keys of the satellite in the folded dense block (`clockAnchorPhaseStatistic`). On `n` the L2W family re-realized cold in the step epoch itself (section 3), so at +0 G25 has no phase statistic (`n_pb` 0): the one epoch in which the step is visible to the phase test is the one in which the test cannot run. The clock-space statistic reads -2.40 (the anchored clock moved 0.042 m with nothing but code pulling it), under k = 6, and the 0.042 m residual is far under the 2.0 m backstop. At +1 the statistic is still unavailable (both families re-realized cold); from +2 it returns on newborn arcs that carry no memory of the step (`w_phi` -0.52, -1.02). The anchor never decouples G25 in the leg (no `cta_decouple` row for G25; the run's one trip is G05 at 276900, in no relation to the event).

The anchored clock (q 5e-6 m2/s, 1.2 cm per 30 s) then follows the code at the anchor's pace: -0.038 m at +0, -0.082 at +10, -0.177 at +50, with the code still pulling. The phase-visible clock moves with it (the hold does not transfer the coupled mark), so the consumer sees both its code and its phase innovation on G25 change by +0.46 m at +0 (+0.462 and +0.464 m on L1C); with no reset on any declaration it moves 1.04 m (Up -1.03 m) and stays 1.0 to 1.17 m off to the leg's end, as the wire recovers only a third of the step.

On `g` and `c` the same-epoch handover was continuous on both families, the phase statistic existed at +0, and the guard tripped at 18.9 and 15.1. The difference is the cold fallback of one family at its successor S40000SIM.

## 7. The consumer's side in one line per posture

| posture | what reaches the consumer's G25 at +0 | its response | position difference |
|---|---|---|---|
| `g` | code corrected (-0.500 m); phase not (hold) | filter rejects the phase row, resets the ambiguity | 0.006 m at +0, 0.142 m at +49 (code-side overshoot) |
| `c` | code corrected (-0.499 m); phase not (hold, 2 mm) | accepts +0.498 m into position | 1.504 m at +0, 2.354 m at +5, 0.56 m at +49 |
| `n` | neither (wire -0.038 m) | accepts +0.46 m on code and phase | 1.04 m at +0, 1.0 to 1.17 m to the end |

## 8. Confirmation: the step with the hold off

Predictions, written before the four legs were read: (1) the producer's clock and code-bias records are unchanged; (2) without the hold the emitted G25 phase bias follows the estimate, the phase-visible clock carries the step at +0, and the cold re-realization at +1 arrives as a phase-bias discontinuity the consumer honours; (3) on `c` the consumer's phase innovation at +0 falls to the wire's own error and the 1.5 m jump disappears, leaving the code-side overshoot, decimetres at most; (4) on `g` the consumer's drift stays of the same order, being code-side.

Legs `evleg_sck_hold0_{sat_clk,null60}_{c,g}`, identical to card SMX's except `network.emit_pb_hold` false.

| pair | +0 | peak | last 20 epochs | null spread S | SMX bar max(3S, 10 mm) |
|---|---|---|---|---|---|
| `g`, hold (SMX) | 0.006 | 0.147 | 0.105 | 0.0070 | 0.021 |
| `g`, hold off | 0.023 | 0.104 | 0.036 | 0.0271 | 0.081 |
| `c`, hold (SMX) | 1.504 | 2.354 | 0.696 | 0.0205 | 0.061 |
| `c`, hold off | 0.007 | 0.644 | 0.471 | 0.1586 | 0.476 |

(consumer position difference in m; S is the white-noise equivalent of the null consumer's first differences, card SMX section 5)

- (1) holds exactly: the C, B and E records of every hold-off leg are byte-identical to its SMX twin; only P records differ.
- (2) holds: at +0 the G25 phase bias moves by -0.017 / -0.024 m (`c`) and +0.032 / +0.056 m (`g`) against a clock move of -0.5 m, so the phase-visible clock carries the step; at +1 the cold re-realization moves it by +5.49 / +2.79 m (`c`), and the consumer answers with `reset_disc`. The newborn bias keeps stepping its discontinuity counter: the consumer resets its G25 L1C ambiguity at 10 epochs on `c` (last at +14) and 14 on `g` (last at +18).
- (3) holds at the step: the `c` consumer's +0 difference falls from 1.504 m to 0.007 m. The remainder is not decimetres: the peak is 0.644 m late in the leg, on a consumer whose own null wanders 0.23 to 0.87 m (spread S 0.159 m, eight times the held null's 0.021 m); the plateau sits at that null's bar.
- (4) refuted: on `g` the late drift is smaller without the hold (0.036 m against 0.105 m over the last 20 epochs), on a null four times noisier. These legs do not separate the code-side overshoot's share from the held phase-visible clock's.

Reading: the `c` jump is the hold's phase-visible clock meeting an accepted phase innovation, confirmed. Turning the hold off is not a candidate fix: the hold's own record is that the unheld phase bias adds noise to the consumer (the loader's PBH read 3), and these nulls show it (consumer spread 4 to 8 times the held one).

## 9. A caveat on the consumer of the event legs

The consumer the legs resume is not the seed's converged consumer. Over the same 30 min, the null legs' consumer 3D error is 1.07 to 1.14 m on `g`, 0.74 to 1.16 m on `c` and 0.89 to 1.00 m on `n`; the seeds' own consumer reads 0.7 to 3.5 cm, 1.1 to 6.7 cm and 0.9 to 6.2 cm at the same epochs. The jump is present at the first leg epoch (`g`: 1.106 m at TOW 275610, the seed 0.012 m), where the leg's consumer reports a code-bias "datum re-reference" on every satellite and re-seeds its receiver clock (WLS against filter 39 m apart), and the producer's resume seam marks 63 of 64 GPS and 49 of 50 Galileo phase-bias keys (`PB_FRAME resume_seam`, max |dPB| 20.3 and 18.5 m against the restored baselines). The cause is not established here. Event-minus-null differences stay valid as differences, but every consumer channel of card SMX version 1 is read on a consumer 0.7 to 1.2 m from convergence, with loose float ambiguities: a 0.5 m phase innovation is absorbed by such a consumer (section 5) where a converged one might reject it, and the consumer magnitudes of this card and of card SMX are those of that consumer.

## 10. Standing and the decisions for the author

Measurement and attribution are complete for the row. Nothing is armed or changed. The mechanisms name four decisions, each the author's:

1. **The slip layer and satellite clock events.** A flag at every station tracking one satellite, at one magnitude (here 16 of 16, 0.45 to 0.54 m), is the signature of a satellite clock event, and it is read as 16 slips. A coherent satellite-wide set could be routed to the clock (decouple and re-solve, which the anchor already does) with the arcs kept; with the arcs kept, the level at +0 (-0.500 m on `g` and `c`) would survive past +1.
2. **The clock step test on `n` (and on any posture where a family re-realizes in the step epoch).** In phase mode the test depends on a statistic that a same-epoch cold re-realization removes, and the fallback is a 2.0 m backstop. Options: form the statistic before the re-realization drops the keys, or give the fallback a bar the clock-space residual can reach.
3. **The hold and the declaration.** A tripped clock (declared on the wire by an `R` record and a clock discontinuity) opens no hold generation, so the consumer refloats nothing on a held key and the phase-visible clock never carries the step. Options: a generation edge on a tripped satellite, or a transfer of the tripped step to the phase side.
4. **The harness consumer (card SMX version 2).** The resumed consumer is 0.7 to 1.2 m off in every leg (section 9); the consumer channels need a converged consumer before their bars mean what card SMX intends.

**Other observation.** The `n` null leg carries 52 IF-TD reference resets in 60 epochs against none on `g` and `c` (most at S05000SIM on G16, S27000SIM on G29, S48000SIM on G12); the broadcast a priori appears to keep the reference gate firing. Not attributed here.

## Annotation (2026-10-26): card SCR

Card [SCR](../../refound/SCR_sat_clock_step_response_2026-10-26.md) built decisions 1 and 3. Two of its results bear on this card's text, which stands as written:

- The `g` consumer's drift (0.142 m at +49, attributed in sections 4 and 7 to the code-side overshoot) is the same with the wire carrying the step to 0.5 mm (SCR section 5.3: 141 mm at +49 with decision 1 alone). The overshoot is not its cause.
- The same-epoch withdraw-and-re-solve that emitted -0.500 m exactly on `g` in section 4 moved G25 by -12.19 m when G25's arcs were carried (SCR section 5.1).
