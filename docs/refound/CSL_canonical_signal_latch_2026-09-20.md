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

## 4. 2026-09-20: CSL-1 result: built; the rule reproduces the emulation with no file edited; the mechanism is half read

Build at main `df691cd`, exe `gnss_processor_csl.exe`: the pick is the pure function
`PppModel::decideCanonicalPicks`; the closed-loop consumer is pinned to `count`
(`deriveClosedLoopConsumerConfig`), so the knob moves the network stations only; one
log line per station states the latch and its runner-up with counts; suite
`CanonicalSignalLatchRule` (15 cases), `gnss_tests` 2332 passed with the one known skip.
Every number below was re-read from the run directories by the manager. No arm of
this section edits an observation file.

**G0 PASS** on three controls at the default (products and event ledgers). **G1 PASS**:
with `priority` the field latch is C1C 41 / C1W 18 and L1C 41 / L1W 18 (from
`network_pf_rows.csv`; the log agrees), the six stations added to L1W are the six of
section 1; band 2 is C2W / L2W everywhere under both rules.

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- |
| `gD_dual` (`count`) | 0.53 0.11 0.10 0.11 0.12 0.11 | 162 | 92 | 0.0090 / 0.0128 |
| **`lD_dual`** (`priority`) | 0.43 0.07 0.06 0.08 0.08 0.07 | **603** | 126 | **0.0069** / 0.0128 |
| `d12_grs` (`count`, 12 h) | 0.53 0.11 0.10 0.11 0.12 0.11 0.16 0.16 0.14 0.11 0.17 0.12 | 275 | 370 | 0.0148 / 0.0251 |
| **`lD_dual12`** (`priority`, 12 h) | 0.43 0.07 0.06 0.08 0.08 0.07 0.08 0.06 0.06 0.14 0.19 0.17 | **994** | 340 | 0.0128 / 0.0250 |
| `gF_tri` / **`lF_tri`** (triple field) | 0.69 0.31 0.15 0.14 0.12 0.17 / 1.47 0.21 0.09 0.11 0.09 0.03 | 6 / **314** | 219 / 242 | 0.0095 / 0.0073 tracked |
| `lS_dual`, `lS_tri` (sim) | byte-identical to their bases | | | |

**B1 PASS** (every hour within 0.01 m of the reference, 603 epochs, 0.0069 m). **B3
PASS** (314). **B4 PASS** (byte identity). **B2 split**: 994 of 1440 epochs passes;
the second clause fails in hour 12 only (0.17 against 0.12 m of `d12_grs`, 0.05 m),
as the emulation arm it was set from does (`d12_prim`), so the clause was unattainable
as written; hours 3 to 11 are within 0.03 m or better. The build reproduces the
emulation to the digit: the latch is the lever, and this time nothing else moved.

**The mechanism read (trace-only, `gD_dual` against `lD_dual`, hours 2 to 6).**
H1, slip and refloat traffic at the six stations: refuted (MW detections 3 to 1, IF-TD
9 to 5, band scope 5 to 5, gaps 36 to 32, post-fit refloats 16 to 26; and the claim
that L1W is the more coherent partner of L2W inverts: cells with band 2 only are
0.16 % on L1C with L2W and 7.6 % on L1W with L2W at those stations). H3, post-fit
residuals: refuted, they get louder (band 1 at the 41 unchanged stations 4.0 to 5.6
mm; band 2 at the six 4.2 to 8.4 mm). H2, the phase-bias columns the consumer uses:
supported, not established. Stations per (satellite, L1W) column 4.58 to 5.94, cold
re-realizations of L1W columns 2 to 1, anchor-slip handovers 5 to 3, and the clock
wire improves only on the consumer's satellites. The counts of this paragraph are the
worker's; two were spot-checked by the manager with a slightly different window
(stations per column 4.59 to 6.00, post-fit refloats at the six 17 to 26).

**Two coverage arms decide between support count and a station subset.** The
first-epoch margins of the six are 10 of 11 (WIND00NAM, KOKV00USA, STHL00GBR), 9 of 10
(ULAB00MNG), 8 of 10 (BOGT00COL) and 10 of 13 (URUM00CHN), so the coverage floor
selects subsets with no file edited.

| arm | L1W stations | which of the six | consumer 3D p50 by hour (m) | sub-dm | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| `gD_dual` | 12 | none | 0.53 0.11 0.10 0.11 0.12 0.11 | 162 | 0.0090 / 0.0128 |
| `lE_cov085` (floor 0.85) | 16 | KOKV, STHL, ULAB, WIND | 0.37 0.07 0.05 0.09 0.08 0.05 | 610 | 0.0096 / 0.0126 |
| `lG_cov078` (floor 0.78) | 17 | and BOGT | 0.40 0.07 0.06 0.07 0.08 0.05 | 615 | 0.0080 / 0.0129 |
| `lD_dual` (floor 0.5) | 18 | all six | 0.43 0.07 0.06 0.08 0.08 0.07 | 603 | 0.0069 / 0.0128 |

The consumer's gain is complete with four stations and does not grow with the fifth
and sixth, while the clock wire on its satellites keeps improving with them (0.0096,
0.0080, 0.0069 m). The consumer effect is therefore a station subset, not a support
count, and the two instruments part company here: what four L1W stations give a
consumer at one site is not what the wire as a whole gains. Which of the four, and
why, is not read; margins are equal for three of them, so the next split needs a
per-station override, not a floor.

**Standing of the rule.** Independent of the unread part, `count` is an artefact: one
satellite that lacks the semi-codeless pair in a station's first processed epoch
decides that station's band-1 signal for the run, and `priority` is what a priority
list is for. It is byte-identical on the sim, holds over 12 h, helps both postures on
the field, and leaves the consumer's own latch alone. Arming it in the working bases
is the author's decision; the card's own condition (the mechanism read before a
recommendation) is met only in part, and that is stated to the author with the
recommendation.

## 5. 2026-09-20: both knobs armed in the working bases; the baseline pair is re-set

At the author's instruction `processing.canonical_signal_rule: "priority"` and
`network.cycle_slip.gap_reentry_per_signal: true` are armed in
`config/refound/{field,sim}_gps_dual_rf0hv.json` (the two knobs and a dated `_comment`
block, nothing else). The new baseline pair is `runs/_rf0hv5_{field,sim}6`, exe
`gnss_processor_csl.exe`, exit 0, 720 consumer epochs; the arm configs equal the
validated arms key for key and the wires are byte-identical to them (`_rap_lD_dual`
sha256 `72b5324bf12761d6...`, `_rap_lS_dual` `46449eac18d3ca0b...`). Every later arm is a
delta against this pair. The day's three field pairs:

| field pair | armed | consumer 3D p50 by hour (m) | sub-dm of 720 | T4_attrib | CLK_ANCHOR | clock wire against COD, tracked / all (m) |
| --- | --- | --- | --- | --- | --- | --- |
| `_rf0hv3_field6` | the hold | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 457 | 598 | 0.0274 / 0.0281 |
| `_rf0hv4_field6` | and the row-level gap re-entry response | 0.53 0.11 0.09 0.10 0.14 0.12 | 203 | 228 | 424 | 0.0107 / 0.0173 |
| **`_rf0hv5_field6`** | and the per-signal rider and the priority latch | **0.43 0.07 0.06 0.08 0.08 0.07** | **603** | **126** | **268** | **0.0069 / 0.0128** |

The sim pair is unchanged through all three (0.60 0.10 0.10 0.06 0.06 0.05 m, 448
epochs, no post-fit refloat, clock wire against truth 0.0030 / 0.0040 m). The 12 h
field arm of the armed posture exists already (`runs/_rap_lD_dual12`, section 4: 994
of 1440 epochs below a decimetre, one hour, the twelfth, worse than the rider-only
arm by more than 0.03 m).

Open with the arming: the latch effect is a station subset whose cause is not read
(section 4); one consumer at one site has graded it; the first hour of the triple
posture under the priority latch is slow (1.47 m); hours 10 to 12 are soft on every
12 h arm. The promotion-ledger rows of the two knobs, and the KEEP_OFF row of
`network.canonical_pair_admission`, are being written in step.

## 6. 2026-09-21: CSL-2 pre-registration: the four-station split (a per-station scope of the rule)

Section 4 left the consumer's gain complete with four of the six stations that move
to L1W (KOKV00USA, STHL00GBR, ULAB00MNG, WIND00NAM; `lE_cov085`, 610 epochs below a
decimetre against 162 on the count latch), with equal first-epoch margins at three of
them, so a coverage floor cannot split further.

**Build.** `processing.canonical_signal_priority_stations` (array of station names,
default empty). Empty: the rule of `processing.canonical_signal_rule` applies to every
station, as today. Non-empty: `priority` applies at the listed stations only and
`count` at the others. Byte-identical when empty; the consumer stays on `count`.

**G1.** The four stations listed, rule `priority`, floor 0.5: the latch equals
`lE_cov085`'s at every station and the wire is byte-identical to it.

**Arms (dual field base, 6 h):** each of the four alone (4 arms) and each leave-one-out
of the four (4 arms). Controls on disk: `gD_dual` (none, 162), `lE_cov085` (the four,
610), `lD_dual` (all six, 603).

**Reading (frozen).** The gain is 162 to 610 epochs. A station CARRIES it if it alone
reaches 311 or more, or if leaving it out of the four falls below 461. One carrier: a
station-local cause, read next at that station (its band-1 phase, the two signals'
arcs, its share of the L1W phase-bias columns). Several partial carriers: a support
effect after all, and the count of L1W stations per satellite column is read against
the consumer's satellites. Every arm is also replayed at the sites of card MSC when
that list exists, because a split graded by one consumer at one site is what this
section is trying to get beyond.

## 7. 2026-09-21: CSL-3 pre-registration: the code half of the rule (found by the multi-site census)

[MSC](MSC_multi_site_consumer_2026-09-21.md) section 4: under `priority` the code
follows the phase's attribute, so 16 stations that track L1C phase with C1W code
moved their code to C1C; the wire's C1W code biases fell from a median of 30
satellites per epoch to 24 (satellite-epochs with a clock and no C1W code bias 1293
to 5323), and consumers on C1W code away from the European cluster log code-bias
dropouts that did not exist before (QUIN00USA 0 to 13, with a divergence of tens of
metres; KOKB00USA 0 to 10; POHN00FSM 0 to 7; WUH200CHN 0 to 2). The same section
refutes the supported reading of section 4 (both consumer families gain).

**Build.** `processing.canonical_signal_code_follows_phase` (bool, default true, the
behaviour built in CSL-1). False: under `priority` the code is decided by the same
first-in-the-list-with-coverage rule as the phase, on its own list, and does not
follow the phase. Byte-identical at the default; the consumer stays on `count`.

**Gates and bars (frozen).**

- **G0** byte identity at the default on the dual field base and the dual sim base.
- **G1** with the knob false the field latch keeps L1W at 18 stations and L1C at 41, and
  the code is C1W at every station that has it; the wire's C1W code biases return to a
  median of 29 satellites or more per epoch and 1500 satellite-epochs or fewer without
  one.
- **B1 WTZZ00DEU, 6 h:** never worse than the baseline pair by more than 0.03 m in any
  hour, 570 or more epochs below a decimetre (603 today).
- **B2 the census sites (replays on the new wire):** no code-bias freshness flip at
  QUIN00USA, KOKB00USA, POHN00FSM and WUH200CHN beyond their count on `_rap_gD_dual`
  (zero); no epoch above 5 m at QUIN00USA after the first hour; the across-site
  median count at or above 145 and the paired change of the mean hourly p50 over
  hours 2 to 6 at or below +0.01 m at the median.
- **B3 sim** byte-identical (one signal per band).
- A pass makes a recommendation to replace the armed value in the working bases; the
  author decides.
