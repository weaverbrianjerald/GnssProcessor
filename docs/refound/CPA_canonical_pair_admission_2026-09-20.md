# CPA: canonical-pair admission

**STATUS**

- **id:** CPA
- **topic:** network-vehicle
- **status:** open
- **summary:** Build card, pre-registered 2026-09-20. A (station, satellite) epoch without the station's canonical phase and code on bands 1 and 2 is harmful to the network solve on both postures: a rising satellite tracked on its civil signals, a long stretch without the semi-codeless pair, the tail of a setting arc. Removed from the RINEX files by a rule a filter can run, the dual field network goes from 203 to 627 epochs below a decimetre of 720, its first hour from 0.53 to 0.19 m, and the triple network with L5 at all 59 stations from 6 to 461. The build moves the rule into the producer.
- **source:** [GRS](GRS_gap_reentry_per_signal_2026-09-20.md) sections 5 and 6; [AB2](AB2_l5_third_frequency_2026-09-20.md) sections 11 and 12; standing invariants 4, 6, 7
- **knobs / posture:** new `network.canonical_pair_admission` (default false) and `network.canonical_pair_keep_epochs` (default 3); RF-0hv working bases with `gap_reentry_relevel` armed, dual and triple
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** evidence arms `runs/_rap_{hH_dual_causal3,hI_dual_causal0,hG_tri_causal3,hJ_tri_causal0,hE_dual_edge,hA_tri_edge,hD_tri_noalt}`

## 1. The rule

For every (station, satellite): the canonical pair is the station's canonical phase
and code signal on band 1 and on band 2, as the producer's own canonical-signal
latch decides them. An epoch is GOOD when all four observables are present. The
satellite's observation at that station is admitted to the network path (raw
observables, slip tiers, assembly) when the epoch is good, or when the last good
epoch is at most `canonical_pair_keep_epochs` processed epochs back. It is withheld
before the first good epoch and beyond that tolerance, until the pair returns. A
withheld observation is absent for every downstream consumer, so that the row-level
gap re-entry tier sees the return as a re-entry, which is what the emulation relied
on.

## 2. The build (default OFF, byte-identical when OFF)

1. The state is per (station, satellite): the index of the last good processed
   epoch. It lives with the run's carried state, not in a function local.
2. The rule is applied once per epoch before the raw observables are computed, in a
   seam of its own; the driver gets a call.
3. Both edges are events and counters (invariants 4 and 7): `ADMISSION`
   `canonical_pair` with `withhold` and `readmit`, the reason (`before_first`,
   `beyond_keep`), and run-end counters per reason plus the number of distinct
   (station, satellite) pairs affected. Armed and nothing withheld is reported as
   inert.
4. Tests on the pure state machine: never good; good from the first epoch; a dropout
   shorter and longer than the tolerance; a return; tolerance zero.
5. The closed-loop consumer is not touched by this card.

## 3. Gates and bars (frozen)

- **G0 byte identity OFF** on the dual field base and the dual sim base (products and
  event ledgers against the pre-build exe).
- **G1 the counters drive** on the field, and the withheld count on the 6 h window is
  of the order of the emulation's (about 15 000 records for a tolerance of 3).
- **B1 field dual** (the working base with the rider and the rule): consumer p50
  within 0.02 m of 0.19 0.08 0.06 0.07 0.09 0.07 in every hour, 550 or more epochs below
  a decimetre, clock wire on the consumer's satellites at or below 0.0085 m.
- **B2 field triple** (T3 with the response, the rider and the rule): 400 or more
  epochs below a decimetre, hours 2 to 6 within 0.03 m of 0.09 0.05 0.10 0.10 0.06.
- **B3 sim:** the undisturbed dual and triple sim arms within 0.01 m of their bases in
  every hour (the sim has no partial epochs, so nothing should be withheld); the two
  sim fault arms (`sJ_ds_dual_on`, `sL_ds_tri_on`) with the rider and the rule at the
  level of the undisturbed runs, for a tolerance of 3 and of 0.
- **Not adopted by this card:** arming anything in the working bases, and the triple
  posture, are separate decisions. A 12 h arm of the dual field posture is owed
  before a recommendation (the lesson of AB2 section 14).

## 4. 2026-09-20: the 12 h emulation arm (before the build)

The rule emulated in the RINEX files (`holes;where=causal;keep=3`) on the dual field
posture with the rider, 12 h, exe `gnss_processor_grs.exe`.

| arm | consumer 3D p50 by hour, hours 1 to 12 (m) | sub-dm of 1440 | T4_attrib | CLK_ANCHOR | clock wire over 12 h, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| `d12_off` (no re-entry response) | 0.57 0.17 0.11 0.10 0.13 0.11 0.11 0.13 0.17 0.16 0.18 0.40 | 223 | 1069 | 1354 | 0.0406 / 0.0440 |
| `d12_on` (the working base) | 0.53 0.11 0.09 0.10 0.14 0.12 0.14 0.15 0.13 0.12 0.17 0.35 | 321 | 702 | 1066 | 0.0203 / 0.0303 |
| `d12_grs` (with the per-signal rider) | 0.53 0.11 0.10 0.11 0.12 0.11 0.16 0.16 0.14 0.11 0.17 0.12 | 275 | 370 | 628 | 0.0148 / 0.0251 |
| `d12_cpa` (rider and the causal admission emulation, 3 epochs) | 0.19 0.08 0.06 0.07 0.09 0.07 0.09 0.07 0.06 0.13 0.19 0.17 | 962 | 351 | 511 | 0.0138 / 0.0327 |

With the rule 962 of 1440 epochs are below a decimetre (321 on the working base, 223
at the start of the day), the first hour is 0.19 m, hours 2 to 9 are at 0.06 to 0.09
m, and no hour from 3 to 12 is worse than the working base by more than 0.02 m
(hours 10 and 11). The clock wire on the consumer's satellites is 0.0138 m over the
12 h (0.0203); on all satellites it is 8 % worse than the working base and 30 % worse
than the rider alone (0.0327 against 0.0303 and 0.0251), which is the one number that
does not improve and is owed a read once the build exists. Hours 10 to 12 are softer than hours 2 to 9 on every arm.
The long-window condition that the row-level response had to meet is met by the
emulation; the build's own 12 h arm is still owed.
