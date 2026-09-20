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
