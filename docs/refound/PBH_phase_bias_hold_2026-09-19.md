# PBH: the phase-bias hold, a wire contract for the phase-bias channel

**STATUS**

- **id:** PBH
- **topic:** consumer
- **status:** open
- **summary:** Three trace reads show the wire's phase-bias movement is harmful error for a float consumer and is not a frame transfer; a config-only dead-band ladder (PBH-0) is pre-registered as the premise test for an emit-side phase-bias hold.
- **source:** [PBT-AUDIT](PBT-AUDIT_translation_premise_2026-09-16.md) section 6; [PBT](PBT_consumer_pb_translate_2026-09-15.md); [ALC2](ALC2_restart_policy_adoption_2026-09-19.md) (the baseline pair)
- **knobs / posture:** `network.closed_loop.pb_step_translate_eps_m` ladder on the RF-0hv working bases (main `ca3666e`); no code change, no golden, no ledger row
- **dates in the text:** 2026-09-19
- **hashes / run dirs:** `runs/_ftc_read_20260919/`; `runs/_pbh0_{field,sim}6_{e0,e1,e5,e20}`; exe `gnss_processor_rf0hv2.exe` sha256 `A3D62A24...`; grader `scripts/ftc_step_attrib.py`

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
