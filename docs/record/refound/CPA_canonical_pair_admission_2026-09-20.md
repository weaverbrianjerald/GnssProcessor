# CPA: canonical-pair admission

**STATUS**

- **id:** CPA
- **topic:** network-vehicle
- **status:** closed
- **summary:** CLOSED 2026-09-20, KEEP_OFF: canonical-pair admission is built exactly and FALSIFIED as the cause; the emulation's gain belonged to the first-epoch canonical-signal latch (card CSL).
- **source:** [GRS](GRS_gap_reentry_per_signal_2026-09-20.md) sections 5 and 6; [AB2](AB2_l5_third_frequency_2026-09-20.md) sections 11 and 12; standing invariants 4, 6, 7
- **knobs / posture:** new `network.canonical_pair_admission` (default false) and `network.canonical_pair_keep_epochs` (default 3); RF-0hv working bases with `gap_reentry_relevel` armed, dual and triple
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** evidence arms `runs/_rap_{hH_dual_causal3,hI_dual_causal0,hG_tri_causal3,hJ_tri_causal0,hE_dual_edge,hA_tri_edge,hD_tri_noalt}`; exe `gnss_processor_cpa.exe`

**Outcome.** The card asks whether admitting a (station, satellite) epoch only with its canonical band-1 and band-2 pair reproduces a RINEX emulation's gain (dual field 203 to 627 of 720 epochs below a decimetre, triple 6 to 461). The built rule withholds the same records (16 244 against 16 014, 15 986 shared) yet gives 16 epochs against 162 with no rule: B1 and B2 FAIL, B3 passes. The emulation had also moved the first-epoch latch at 13 stations, and that latch alone gives 601. Ledger row KEEP_OFF; the lever became [CSL](CSL_canonical_signal_latch_2026-09-20.md).

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

## 5. 2026-09-20: CPA-1 result: built, the rule is exact, and it is NOT what the emulation measured. KEEP_OFF

Build: seam `filter/NetworkCanonicalPairAdmission` (pure state machine
`canonicalPairStep`; the call sits right after `networkInjectEpoch`, before the raw
observables), state and counters in `NetworkRunContext`, knobs next to `readmit_*`,
suite of 12 cases, `gnss_tests` 2317 passed with the one known skip, exe
`gnss_processor_cpa.exe`. Every number below was re-read from the run directories by
the manager.

**G0 PASS** on three controls (the dual field base, the dual sim base, the rider arm).
**G1 PASS**: field dual arm `cD_dual`, 6 h: 471 076 records screened, 16 244 withheld
(4873 before the first good epoch, 11 371 beyond the tolerance) over 851 pairs, 1258
withhold and 882 readmit edges; the emulation withheld 16 014 in the same window and
15 986 are shared. The built rule IS the emulated rule.

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- |
| `gD_dual` (no rule) | 0.53 0.11 0.10 0.11 0.12 0.11 | 162 | 92 | 0.0090 / 0.0128 |
| `hH_dual_causal3` (the emulation) | 0.19 0.08 0.06 0.07 0.09 0.07 | 627 | 147 | 0.0074 / 0.0137 |
| **`cD_dual`** (the built rule, tolerance 3) | 0.61 0.19 0.12 0.12 0.13 0.12 | **16** | 98 | 0.0090 / 0.0125 |
| `cD_dual_k0` (tolerance 0) | 0.61 0.20 0.11 0.13 0.13 0.10 | 106 | 106 | 0.0092 / 0.0121 |
| `gF_tri` / **`cF_tri`** | 0.69 0.31 0.15 0.14 0.12 0.17 / 0.62 0.29 0.12 0.12 0.11 0.15 | 6 / 60 | 219 / 212 | 0.0095 / 0.0094 tracked |
| sim, undisturbed and fault beds, tolerance 3 and 0 | equal to their bases to the printed digit | | | |

**B1 and B2 FAIL; B3 PASS.** The built rule withholds the emulation's records and does
not reproduce its result: on the dual field bed it is worse than no rule.

**The emulation moved a second variable.** Deleting records from the RINEX files
changes the tally of the first processed epoch, on which the producer decides each
station's canonical signal per band (`decideConstellationCanonicals`, the signal with
the most satellites). Read independently from `network_pf_rows.csv`: the base latches
C1C at 31 stations and L1C at 47; the emulation latches C1W at 41 and L1W at 18; 13
stations differ, six of them in the band-1 PHASE (BOGT00COL, KOKV00USA, STHL00GBR,
ULAB00MNG, URUM00CHN, WIND00NAM: L1C in the base, L1W in the emulation), seven in the
code only. The built rule leaves the latch where the base has it. One-change arms on
the rider posture, same exe:

| arm | the one change | consumer 3D p50 by hour (m) | sub-dm | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- |
| `cD_dual_primoff` | non-primary types of bands 1 and 2 removed at every station, NO rule | 0.44 0.07 0.06 0.08 0.08 0.07 | **601** | 0.0069 / 0.0129 |
| `cD_dual_prim` | the same with the rule | 0.34 0.10 0.08 0.09 0.09 0.07 | 489 | 0.0066 / 0.0128 |
| `kA_six` | the same strip at the six phase-flip stations only | 0.69 0.34 0.10 0.06 0.07 0.06 | 411 | |
| `kB_seven` | the same strip at the seven code-only stations | 0.53 0.11 0.10 0.10 0.12 0.11 | 165 | |
| `kC_consL1C` | the CONSUMER moved to L1C / C1C | 0.68 0.27 0.13 0.10 0.13 0.11 | 80 | |

The gain of sections 4 and of GRS sections 5 and 6 belongs to the canonical-signal
latch, not to admission: forcing every station onto its priority-first signal gives
601 epochs below a decimetre without any rule, six stations carry most of it, the
band-1 code choice carries none, and the rule on top of it costs epochs (601 to 489).
Moving the consumer to the majority family does not help, so the mechanism is not
simply the number of stations that support the consumer's signal; it is not read
yet.

**Verdict.** `network.canonical_pair_admission` is KEEP_OFF on the dual posture
(falsified as the cause; worse than no rule) and buys 6 to 60 epochs on the triple
one. The build stays in the tree, default OFF, as the instrument that separated the
two variables. The card is closed; the lever it uncovered is the canonical-signal
decision (the most satellites at the first processed epoch prefers a civil signal
whenever one satellite lacks the semi-codeless pair at that epoch), which is a new
card. Lesson for the emulation tool: an edit of the observation files can move the
producer's first-epoch decisions; an emulation arm is checked for an unchanged latch
(`network_pf_rows.csv`, signals by station) before its result is attributed.
