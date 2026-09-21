# GRS: the gap re-entry response per signal

**STATUS**

- **id:** GRS
- **topic:** slip
- **status:** open
- **summary:** Build card, pre-registered 2026-09-20. The only response that covers a return epoch keeps its history per (station, satellite); a loss of lock on the primary-pair phases during which another signal (band 5, a fallback signal) keeps the satellite present is therefore never a re-entry, and the slipped ambiguities are folded. This is how GPS L5 damages the L1/L2 wire on the field (AB2 sections 10 to 12) and it is a defect of the dual posture too. The build keys the re-entry on the signal.
- **source:** [AB2](AB2_l5_third_frequency_2026-09-20.md) sections 10 to 12; `docs/SPEC_slip_qc_hardening.md` card G (the instrument and the satellite-level response); standing invariants 1, 4, 6, 7
- **knobs / posture:** new `network.cycle_slip.gap_reentry_per_signal` (default false), rider of `network.cycle_slip.gap_reentry_relevel`; RF-0hv working bases, dual and triple
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** evidence arms `runs/_rap_{dC_gapre,fM_gapre,dD_holes_gapre,fN_holes_gapre}`; sim fault arms `runs/_rap_{sI_ds_dual_off,sJ_ds_dual_on,sK_ds_tri_off,sL_ds_tri_on}`

## 1. The defect

`networkComputeRawObservables` (`src/core/slip/NetworkRawObservables.cpp:53-65`) builds
one raw row per (station, satellite) from whichever phase signals with a matching
code are present, lowest band first, second signal on the next distinct band.
`networkQualifyRawObservables` (`:318-321`) defines a resumption as that row being
absent for at least one processed epoch, and `networkSlipDrainGapReentry`
(`src/core/slip/NetworkSlipQc.cpp:2026` onward) frees the two signals of the row of a
shifted, flagged or unmeasurable resumption.

A receiver that loses lock on the semi-codeless L1/L2 phases of a satellite keeps
tracking its civil signals. The row then survives the dropout with another pair, the
return epoch has an adjacent predecessor, and any adjacent-epoch step is formed
across a pair change (`:224-230`, flagged `pair_changed`). No resumption is written, no
detector covers the return epoch (the MW tier needs an adjacent predecessor of the
same pair, the IF-TD tier three consecutive epochs), and the carried ambiguities of
the returning phases are folded across the break. Field instance: BSHM00ISR, G14, TOW
268770 to 268860, a 301-cycle L1C step with the receiver flagging L2W only; the
station is wrecked for hours, which is the "gross code" of RF-0 and all of the L5
loss of AB2. With a third band this is the rule: every satellite with L5 stays
present through every L1/L2 dropout.

## 2. The build (default OFF, byte-identical when OFF)

1. **History per signal.** `RawSlipHistory` gains, per (satellite, signal), the last
   processed epoch at which that signal's phase and its matching code were present,
   with the values needed for a level comparison. Filled only when the knob is on.
2. **A per-signal resumption.** A signal whose phase is present now and was last seen
   two or more processed epochs ago, inside the leveling maximum-gap window, on a
   satellite for which the satellite-level resumption did not fire in this epoch (no
   double count).
3. **The level test.** The geometry-free difference against a partner signal that was
   present before the gap and is present now: first choice a partner that tracked
   through the gap, second choice the pre-gap partner of the same pair. The gate is
   the station's existing empirical GF gate. No partner on both sides means
   unmeasurable.
4. **The response.** The verdicts of the satellite-level path (shifted, LLI at the
   return epoch, unmeasurable), applied to the signals that were ABSENT only, through
   the same free-epoch latch and pending-reseed mark. For this tier an unmeasurable
   re-entry is freed by default (`gap_reentry_per_signal_unmeasurable`, default true):
   a partial dropout with no witness is the poison case measured in AB2.
5. **Both edges are events** (invariant 7): `T0_gap sig_resume` with the verdict and
   `T0_gap reentry_relevel` with `scope=signal`; run-end counters in the card-G line
   (per-signal resumptions, shifted, unmeasurable, freed by cause, no carry, state
   absent). Armed without `gap_reentry_relevel` the rider warns and counts itself inert
   (invariant 4).
6. **Tests.** Pure-function cases on the qualify pass: knob off gives today's output;
   a primary-pair dropout with band 5 tracking through gives two per-signal
   resumptions with the geometry-free step against the continuous partner; a
   third-band-only dropout gives one; a full-satellite gap gives the satellite-level
   path only; a gap beyond the window gives none.

## 3. Gates and bars (frozen)

- **G0 byte identity OFF:** same-build products and event ledgers equal to the
  pre-build exe on the dual field base and the dual sim base.
- **G1 the counters drive** on the field triple arm: per-signal resumptions above
  zero, the BSHM00ISR G14 return of TOW 268860 present with `scope=signal` and freed.
- **B1 field triple** (T3 with both knobs): consumer p50 within 0.03 m of the dual
  baseline in each of hours 3 to 6, at least 250 epochs below a decimetre,
  `T4_attrib` below 400, clock wire on the consumer's satellites at or below 0.0150 m.
- **B2 field dual** (baseline with both knobs): `T4_attrib` at or below 228 and the
  clock wire on the consumer's satellites at or below 0.0107 m, which are the values
  of the satellite-level response alone.
- **B3 sim:** the dual and triple sim bases with both knobs within 0.01 m of their
  bases in every hour; the sim fault arm `sL_ds_tri_on` with the rider healthy (within
  0.02 m of the sim's T3 in hours 3 to 6) where it is damaged without it.
- **Not adopted by this card:** arming either knob in the working bases, and the
  triple posture itself, are separate decisions after the bars.

## 4. 2026-09-20: GRS-1 result: built; the sim fault is repaired completely; the field triple arm is short of bar B1

Build at main `9edc0ea`, exe `gnss_processor_grs.exe`. Every number below was re-read
from the run directories by the manager.

**G0 PASS, three controls.** Rider OFF, products and event ledgers byte-identical to
the pre-build exe: the dual field base (`_rap_g0_field` against `_rf0hv3_field6`), the
dual sim base (`_rap_g0_sim` against `_rf0hv3_sim6`), and the armed row-level arm
(`_rap_g0_dc` against `_rap_dC_gapre`), which certifies the relocation of the
responder body. New suite `GapReentryPerSignalQualify`, 9 of 9; `gnss_tests` 2305
passed with the one known skip.

**G1 PASS.** Field triple arm `gF_tri`: 149 per-signal re-entries, 117 shifted, 32
measured and unshifted, none unmeasurable, 31 judged against a second-choice partner;
125 judged slipped, 81 with a protectable carry, 81 freed (74 shifted, 7 LLI), the
closure exact. The BSHM00ISR G14 return of TOW 268860 is there: `sig_resume` and
`reentry_relevel` with `scope=signal` for L1C (GF step -57.86 m against L1L, which
tracked through) and for L2W (shifted and flagged).

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | CLK_ANCHOR | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| dual field, row-level response (the working base) | 0.53 0.11 0.09 0.10 0.14 0.12 | 203 | 228 | 424 | 0.0107 / 0.0173 |
| **`gD_dual`** (dual field, rider) | 0.53 0.11 0.10 0.11 0.12 0.11 | 162 | **92** | **232** | **0.0090 / 0.0128** |
| `fM_gapre` (triple field, row-level) | 0.67 0.30 0.15 0.30 0.35 0.52 | 0 | 716 | 607 | 0.0269 / 0.0340 |
| **`gF_tri`** (triple field, rider) | 0.69 0.31 0.15 0.14 0.12 0.17 | 6 | **219** | 325 | **0.0095** / 0.0175 |
| `fN_holes_gapre` (triple field, the RINEX emulation) | 0.31 0.07 0.14 0.13 0.11 0.07 | 311 | 327 | 454 | 0.0126 / 0.0270 |
| `sJ_ds_dual_on` (sim fault, dual, row-level) | 0.81 0.72 0.11 0.10 0.17 0.16 | 178 | 1940 | 2416 | 0.0670 |
| **`gS_ds_dual`** (the same with the rider) | 0.59 0.11 0.10 0.06 0.05 0.05 | 439 | 61 | 127 | **0.0030** |
| `sL_ds_tri_on` (sim fault, triple, row-level) | 0.50 0.23 0.28 0.24 0.22 0.22 | 1 | 2944 | 2893 | 0.0939 |
| **`gS_ds_tri`** (the same with the rider) | 0.57 0.17 0.08 0.04 0.04 0.04 | 424 | 72 | 133 | **0.0029** |

**B2 PASS** (dual field: `T4_attrib` 92 against a bar of 228, clock wire 0.0090 m
against 0.0107). **B3 PASS**: `gS_dual` and `gS_tri` equal their bases to the printed
digit (no per-signal re-entry occurs on the undisturbed sim); on the sim fault bed
the rider takes both postures back to the level of the undisturbed runs, consumer,
refloats and clock wire against truth (0.0670 to 0.0030 m, 0.0939 to 0.0029 m).

**B1 FAILS on the consumer.** `T4_attrib` 219 (bar 400) and the clock wire 0.0095 m
(bar 0.0150) pass, and hours 4 to 6 go from 0.30 / 0.35 / 0.52 to 0.14 / 0.12 / 0.17
m. Against the dual baseline the arm is still 0.04 to 0.06 m short in hours 3, 4 and
6, its first two hours are those of the unrepaired triple (0.69 / 0.31 m), and it has
6 epochs below a decimetre against a bar of 250. The rider is not the cause (its
control has none). The RINEX emulation of AB2 section 12 is much better than the
rider exactly there (hours 1, 2 and 6 at 0.31 / 0.07 / 0.07 m, 311 epochs), and it
differs from the rider in one respect: it deletes the epochs in which a satellite is
present WITHOUT its primary phases, at the arc edges and inside the dropouts, where
the rider only answers the return. The knob stays default OFF and unarmed.

**GRS-2, pre-registered (RINEX only, on the rider posture, exe `gnss_processor_grs.exe`).**
The `holes` op gains `where=edge|mid|all`. `hA_tri_edge` = `gF_tri` with the partial
epochs at the arc edges removed (34 726 records of the day), `hB_tri_mid` = with those
inside an arc removed (17 071), `hC_tri_all` = both (the control: it must reproduce
the level of `fN_holes_gapre` on the new exe). Reading, frozen: the arm that brings
hours 1 and 2 to within 0.05 m of 0.31 and 0.07 m with 250 or more epochs below a
decimetre names the class of partial-signal epochs that still costs the triple
network its ignition; the fix owed is then a rule for a satellite that is present
without its primary pair (no fallback pair in the solve, or no rows at all), not a
further slip response.

## 5. 2026-09-20: GRS-2 result: the partial-signal epochs at the arc EDGES are what the triple network still pays for

Arms on the rider posture (`gF_tri`), exe `gnss_processor_grs.exe`, exit 0, 720 consumer
epochs; all 59 stations on three bands.

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | CLK_ANCHOR |
| --- | --- | --- | --- | --- |
| dual working base (`_rf0hv4_field6`) | 0.53 0.11 0.09 0.10 0.14 0.12 | 203 | 228 | 424 |
| `gF_tri` (rider) | 0.69 0.31 0.15 0.14 0.12 0.17 | 6 | 219 | 325 |
| **`hA_tri_edge`** (partial epochs at the arc edges removed) | 0.83 **0.08 0.05 0.09 0.11 0.07** | **393** | 267 | 349 |
| `hB_tri_mid` (partial epochs inside an arc removed) | 0.43 0.11 0.16 0.14 0.09 0.12 | 206 | 300 | 439 |
| `hC_tri_all` (both; the control) | 0.31 0.07 0.14 0.12 0.11 0.07 | 324 | 327 | 454 |

The control reproduces the emulation of AB2 section 12 on the new exe (0.31 0.07 0.14
0.13 0.11 0.07, 311). By the letter of the frozen reading only the control meets all
three conditions; the split says why. Removing the edge epochs alone gives the best
field consumer of the program from hour 2 on, better than the dual base in every one
of hours 2 to 6 (0.08 0.05 0.09 0.11 0.07 against 0.11 0.09 0.10 0.14 0.12 m) with
nearly twice its epochs below a decimetre, and a worse first hour (0.83 m). Removing
the mid-arc ones alone brings the triple to the level of the dual base with the best
first hour but one (0.43 m). The settled level is an EDGE matter: a rising or setting
satellite that a receiver tracks on its civil signals before (after) the semi-codeless
pair is present costs the triple network about 0.05 m of consumer accuracy and nearly
all of its epochs below a decimetre; the day holds 34 726 such records, 1.9 % of the
GPS records. The first hour is a MID-ARC matter.

This is not a slip question any more. With the row-level response, the rider and the
edge rule, GPS L5 on all 59 stations is better than two bands on real data.

**GRS-3, pre-registered.** Two arms decide what the rule should be, before it is
built. `hD_tri_noalt` = `gF_tri` with every non-primary observation type of bands 1
and 2 removed from every station's RINEX file (op `primaryonly`, selftest 17/17): no
fallback signal exists, the epochs stay. `hE_dual_edge` = the dual rider arm `gD_dual`
with the edge epochs removed. Reading, frozen: `hD_tri_noalt` at the level of
`hA_tri_edge` from hour 2 (within 0.03 m in each hour, 300 or more epochs below a
decimetre) means the damage is done by the FALLBACK SIGNALS entering the solve at the
edges, and the rule owed is "no fallback walk on the network path" (a canonical
signal per band or nothing); otherwise the rule owed is an admission rule: a
(station, satellite) epoch enters the solve only with its canonical pair on bands 1
and 2. `hE_dual_edge` says whether the dual working base owes the same rule (better
than `gD_dual` by 0.02 m or more in three of hours 2 to 6, or 250 or more epochs below
a decimetre).

## 6. 2026-09-20: GRS-3 result: an admission question; a rule a filter can run is the best network of the program

All arms on the rider posture (`gD_dual`, `gF_tri`), exe `gnss_processor_grs.exe`, exit 0,
720 consumer epochs. "Partial" means a (station, satellite) epoch without the
station's primary phase on band 1 or band 2.

| arm | what is removed | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| dual working base | nothing, no rider | 0.53 0.11 0.09 0.10 0.14 0.12 | 203 | 228 | 0.0107 / 0.0173 |
| `gD_dual` | nothing | 0.53 0.11 0.10 0.11 0.12 0.11 | 162 | 92 | 0.0090 / 0.0128 |
| `hE_dual_edge` | partial epochs at the arc edges | 0.48 0.05 0.09 0.09 0.12 0.10 | 454 | 113 | 0.0086 / 0.0128 |
| **`hH_dual_causal3`** | partial epochs before the first good epoch or more than 3 after the last | **0.19 0.08 0.06 0.07 0.09 0.07** | **627** | 147 | **0.0074** / 0.0137 |
| `hI_dual_causal0` | every partial epoch | 0.19 0.07 0.06 0.07 0.09 0.07 | 624 | 152 | 0.0075 / 0.0136 |
| `gF_tri` | nothing | 0.69 0.31 0.15 0.14 0.12 0.17 | 6 | 219 | 0.0095 / 0.0175 |
| `hA_tri_edge` | edges | 0.83 0.08 0.05 0.09 0.11 0.07 | 393 | 267 | 0.0117 / 0.0217 |
| `hD_tri_noalt` | the non-primary observation TYPES of bands 1 and 2 (epochs kept) | 1.47 0.21 0.09 0.11 0.09 0.03 | 312 | 241 | 0.0073 / 0.0181 |
| **`hG_tri_causal3`** | the causal rule, 3 epochs | 0.83 0.09 0.05 0.10 0.10 0.06 | **461** | 206 | 0.0076 / 0.0185 |
| `hJ_tri_causal0` | every partial epoch | 0.81 0.10 0.10 0.13 0.10 0.06 | 288 | 260 | |

**Readings, by the frozen rules.** `hD_tri_noalt` is not within 0.03 m of `hA_tri_edge` in
hours 2 and 3 (0.21 and 0.09 against 0.08 and 0.05 m), so the rule owed is an
ADMISSION rule, not a no-fallback rule; removing the fallback types alone does help
the settled hours (312 epochs below a decimetre, 0.03 m in hour 6). `hE_dual_edge`
has 454 epochs below a decimetre against a bar of 250: the dual working base owes the
same rule.

**A rule that a filter can run does better than the edge emulation.** The causal
variant needs no look-ahead: a (station, satellite) is not admitted before its
primary pair has been seen, and after the pair is lost it is admitted for at most N
more epochs. On the dual posture it gives the best network of the program: 0.19 m in
the FIRST hour (0.53 before), 0.06 to 0.09 m in hours 2 to 6, 627 of 720 epochs below a
decimetre (203 before), and a clock wire of 0.0074 m on the consumer's satellites
(0.0274 m at the start of the day). N = 3 and N = 0 are equal there. On the triple
posture N = 3 is the best arm (461 epochs) and N = 0 is worse (288): with three bands
a satellite without its band-2 phase is still a two-band satellite, and deleting it
costs information; the first hour of the triple stays at 0.8 m under every variant,
which is a separate ignition matter of the third band. What is removed is 62 451
records of the day for N = 3 (3.4 % of the GPS records), most of it long stretches in
which a receiver tracks a satellite without ever holding the semi-codeless pair.

The rule is the build card [CPA](CPA_canonical_pair_admission_2026-09-20.md).
