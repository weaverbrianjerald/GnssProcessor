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
