# CSL: the canonical-signal latch

**STATUS**

- **id:** CSL
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-09-20. The producer decides each station's canonical signal per band from the satellite count of the first processed epoch, which prefers a civil signal whenever one satellite lacks the semi-codeless pair at that epoch. With every station forced onto its priority-first signal (RINEX op `primaryonly`, nothing else changed) the dual field consumer goes from 162 to 601 epochs below a decimetre of 720 in 6 h and from 275 to 983 of 1440 in 12 h. The mechanism is unread. The card builds the decision rule behind a knob so that it can be graded without editing observation files.
- **source:** [CPA](CPA_canonical_pair_admission_2026-09-20.md) section 5 (where the lever was separated from the admission rule); [GRS](GRS_gap_reentry_per_signal_2026-09-20.md) section 8
- **knobs / posture:** new `processing.canonical_signal_rule` (`count`, the default and today's behaviour, or `priority`) with `processing.canonical_signal_min_coverage` (default 0.5); RF-0hv working bases with the per-signal rider
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** `runs/_rap_{gD_dual,cD_dual_primoff,kA_six,kB_seven,kC_consL1C,hD_tri_noalt,d12_grs,d12_prim}`

## 1. What is known (every number re-read from the run directories)

The decision is `decideConstellationCanonicals` (`src/core/model/PppModel.cpp`, the block
near lines 455 to 490): per band and observation type, the signal present on the
most satellites of the station's first processed epoch, ties to the earlier entry of
the priority list. One satellite without L1W at that epoch is enough to latch L1C for
the run. Dual field base: C1C at 31 stations and C1W at 28, L1C at 47 and L1W at 12,
although 18 stations carry L1W.

| arm (dual field, rider posture) | latch, band 1 | consumer 3D p50 by hour (m) | sub-dm | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- |
| `gD_dual` | C1C 31, L1C 47 | 0.53 0.11 0.10 0.11 0.12 0.11 | 162 | 0.0090 / 0.0128 |
| `cD_dual_primoff` (every station on its priority-first signal) | C1C 41, L1C 41, L1W 18 | 0.44 0.07 0.06 0.08 0.08 0.07 | **601** | **0.0069** / 0.0129 |
| `hH_dual_causal3` (the `holes` emulation of CPA) | C1W 41, L1C 41, L1W 18 | 0.19 0.08 0.06 0.07 0.09 0.07 | 627 | 0.0074 / 0.0137 |
| `kA_six` (the strip at BOGT00COL, KOKV00USA, STHL00GBR, ULAB00MNG, URUM00CHN, WIND00NAM only) | those six on C1W / L1W | 0.69 0.34 0.10 0.06 0.07 0.06 | 411 | |
| `kC_consL1C` (the consumer moved to C1C / L1C, network unchanged) | as `gD_dual` | 0.68 0.27 0.13 0.10 0.13 0.11 | 80 | |

12 h: `d12_grs` 0.53 0.11 0.10 0.11 0.12 0.11 0.16 0.16 0.14 0.11 0.17 0.12 m with 275
epochs below a decimetre of 1440 and a clock wire of 0.0148 / 0.0251 m; `d12_prim` 0.44
0.07 0.06 0.08 0.08 0.07 0.08 0.06 0.06 0.14 0.19 0.17 m with **983** and 0.0128 / 0.0251
m, never worse than the working base by more than 0.02 m from hour 2. Triple field
(`hD_tri_noalt` against `gF_tri`): 6 to 312 epochs, hour 6 at 0.03 m, a slow first hour
(1.47 m).

What the arms say. The band-1 PHASE attribute of six stations carries most of the
gain. The band-1 code attribute does not matter: the two good arms have opposite
code latches (C1C at 41 stations, C1W at 41). `kB_seven` of CPA section 5 is a null
arm by construction (the strip leaves those seven stations' latch where the base has
it) and carries no evidence. Moving the consumer to the majority family does not
help, so the gain is not simply the number of stations that support the consumer's
signal. Why a station that offers both L1W and L1C serves the network better on L1W
is not read: candidates are the coherence of L1W with L2W on a semi-codeless
receiver (they lose lock together, L1C does not), the anchoring of the L1W phase-bias
columns that the consumer uses, and the slip tiers' behaviour on a mixed pair.

## 2. The build (default unchanged, byte-identical at the default)

`processing.canonical_signal_rule: "priority"`: per band and observation type, the
first signal of the priority list whose satellite count at the decision epoch is at
least `canonical_signal_min_coverage` times the largest count of that band and type;
the code follows the phase's tracking attribute when that code exists at the
station. `"count"` is today's rule and the default. The decision and its runner-up
are logged per station (one line), so that a run states its latch. Tests on the pure
decision function. No RINEX edit is involved in any validation arm.

## 3. Gates and bars (frozen)

- **G0** byte identity at the default on the dual field base and the dual sim base.
- **G1** with `priority` the field latch reads L1W at 18 stations and L1C at 41 (the
  emulated latch), stated by the run's own log.
- **B1 field dual, 6 h** (rider posture): consumer p50 within 0.03 m of 0.44 0.07 0.06
  0.08 0.08 0.07 in every hour, 500 or more epochs below a decimetre, clock wire on the
  consumer's satellites at or below 0.0080 m.
- **B2 field dual, 12 h**: 850 or more epochs below a decimetre of 1440, never worse
  than `d12_grs` by more than 0.03 m from hour 3.
- **B3 field triple, 6 h**: 250 or more epochs below a decimetre (312 emulated).
- **B4 sim**: dual and triple within 0.01 m of their bases in every hour (one signal
  per band on the sim, so the rule should change nothing).
- **Not adopted by this card.** The mechanism read (section 1, last paragraph) is owed
  before a recommendation: a lever whose cause is unknown is not armed on the
  strength of one consumer at one site.
