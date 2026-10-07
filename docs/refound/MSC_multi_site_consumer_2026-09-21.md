# MSC: the multi-site consumer census

**STATUS**

- **id:** MSC
- **topic:** network-vehicle
- **status:** open
- **summary:** WTZZ00DEU is unrepresentative (603 sub-decimetre epochs, site median 145); by the frozen count rule no 2026-09-20 adoption holds across 16 sites; the census FAILS its own standing-instrument bar.
- **source:** [TBI](TBI_three_band_ignition_2026-09-20.md) section 5 (the replay controls); [CSL](CSL_canonical_signal_latch_2026-09-20.md) sections 4 and 5; [B5D](B5D_band5_code_datum_2026-09-20.md) sections 3 and 4
- **knobs / posture:** no new knob; `network.closed_loop.replay_products_path` with `network.closed_loop.station` moved to the site; recorded wires only, no producer run
- **dates in the text:** 2026-09-21
- **hashes / run dirs:** wires `runs/_rf0hv3_field6`, `runs/_rf0hv4_field6`, `runs/_rap_gD_dual`, `runs/_rf0hv5_field6`, `runs/_rap_lF_tri`, `runs/_rap_b5_tri`, `runs/_rap_tB_nocode5_net`; exe `build/x64-Release/gnss_processor_b5d.exe`

**Outcome.** Question: do WTZZ00DEU verdicts hold for independent consumers replayed on the recorded wires (a replay matches its closed loop to 0.002 m per half hour). Two adoptions read SITE-SPECIFIC and the latch MIXED; together the three improve 13 sites by a median 0.19 m (post hoc). CSL's support reading is refuted: both consumer families gain. The latch thinned the wire's C1W code biases ([SCC](SCC_second_code_channel_2026-09-21.md) restored coverage); a consumer diverges 70 m on the dropouts. Owed: more sites with a pre-frozen paired statistic, and unread KOKB00USA and UTQI00USA; [RXP](RXP_rinex3_blank_prn_2026-09-21.md) explained ANMG00MYS.

## 1. The instrument and the sites (frozen before any replay is graded)

A replay arm is a copy of the wire's own arm config with the replay key set and the
closed-loop station moved to the site (name, observation file, grading coordinate).
The grading coordinate is the ITRF2020 position at the day's epoch from
`scripts/resolve_itrf_ecef.py`; the consumer seeds itself from the RINEX header, as at
WTZZ00DEU. The consumer's canonical-signal rule stays `count`.

Site rule: a station with an observation file for the day under
`data/2024-129/obs/`, not in the 59-station roster and not WTZZ00DEU; an ITRF2020
coordinate resolvable; GPS code and phase on bands 1 and 2 at 90 % or more of the 720
epochs of the window. From the stations that qualify, 16 are taken by a
farthest-point rule on the sphere that starts at WTZZ00DEU (deterministic, no result
consulted), with two constraints applied while selecting: at least six sites whose
consumer latch is L1W and at least six whose latch is L1C, and at least eight that
carry L5. The list, with receiver type, latch, L5 family and distance to the nearest
roster station, is written to a file before the first graded replay. A site that
fails to run or converges nowhere is reported, not replaced.

Controls first: WTZZ00DEU replayed on each wire must reproduce that run's closed loop
(hourly p50 to the printed digit, the same count of epochs below a decimetre).

## 2. The wires and the questions

| wire | what it carries | the question at the sites |
| --- | --- | --- |
| `_rf0hv3_field6` | the hold | the starting point of 2026-09-20 |
| `_rf0hv4_field6` | and the row-level gap re-entry response | does the response hold across sites |
| `_rap_gD_dual` | and the per-signal rider | does the rider hold across sites |
| `_rf0hv5_field6` | and the priority latch (the baseline pair) | does the latch hold across sites, and for which consumer family |
| `_rap_lF_tri` | three bands, the assertion in place | dual consumer at every site, triple consumer at the L5 sites |
| `_rap_b5_tri` | three bands, the band-5 datum free | the same |
| `_rap_tB_nocode5_net` | three bands, band-5 code removed at the stations | the same |

Per site and wire: consumer 3D p50 by hour, epochs below a decimetre of 720, the first
epoch below a decimetre. Across sites: median and quartiles by hour, the median count,
and the number of sites that improve or worsen by more than 36 epochs (5 %) between two
wires.

## 3. The reading (frozen)

- **R1** an adoption HOLDS ACROSS SITES if the count of epochs below a decimetre improves
  at two thirds or more of the sites and the across-site median improves; it is
  SITE-SPECIFIC if fewer than half the sites improve; otherwise MIXED. A loss at more
  than a third of the sites is reported as a finding against the adoption, with the
  sites named.
- **R2** the latch by consumer family: CSL's supported reading (the support of the L1W
  phase-bias columns) predicts that L1W consumers gain from `_rap_gD_dual` to
  `_rf0hv5_field6` and that L1C consumers do not gain or lose. The two families are
  reported apart.
- **R3** L5: whether the dual pair's lead by count over every three-band wire, measured at
  WTZZ00DEU, is general; the L5 adoption bars of TBI section 4 are restated on the
  across-site median, not replaced.
- **R4** the census becomes the standing consumer instrument if its controls pass and its
  spread across sites is smaller than the differences it is asked to grade; it is
  reported with its own noise (the spread across sites on one wire).

## 4. 2026-09-21: MSC-1 result: WTZZ00DEU is not representative; no adoption passes the frozen count rule; the priority latch thinned the C1W code biases of the wire

Replays by an Opus worker (170 run directories, `runs/_rap_msc_*`, exe
`gnss_processor_b5d.exe`, no build, 0.12 GB); tables in `runs/_msc/`. The manager
recomputed every count, every hourly median and every paired comparison from the
replay traces with an independent script (`msc_regrade.py`, session scratchpad): all
equal to the worker's. The site list was written at 08:00:23, the first site replay
at 08:03:16.

**Controls.** Seven of eight reproduce their closed loop exactly (hourly p50 and the
count); `_rf0hv4_field6` differs by one epoch of 720 (202 against 203, largest
difference 0.0011 m, an epoch astride 0.100 m; that wire was produced by an older
exe). The resolver's coordinate for WTZZ00DEU equals the arm's `position_ecef`, and
the replay driver reduces the marker to the antenna reference point from the RINEX
header (the worker's reading of `ApplicationConsumerReplay.cpp`).

**Sites.** 251 candidates, 7 rejected by the rule (no file, no ITRF2020 coordinate,
completeness), 244 left: 39 with an L1W latch, 205 with L1C, 230 with L5. The L1W
quota bound at steps 13 to 16. The realized latch equals the pre-scan at all 16 sites.
Two sites are next to a roster station (KOKB00USA 0.0 km from KOKV00USA, ARHT00ATA
3.1 km from SCTB00ATA): the rule maximized the distance between chosen sites and did
not exclude roster proximity, so those two are independent receivers, not
independent geography.

Epochs below a decimetre of 720 (dual consumer on the seven wires; triple consumer on
the three-band wires in the last three columns):

| site | receiver, latch | hv3 | hv4 | gD | hv5 | lF | b5 | tB | lF t | b5 t | tB t |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CHTI00NZL | Trimble, L1C | 164 | 169 | 420 | 484 | 311 | 305 | 282 | 422 | 530 | 544 |
| SCRZ00BOL | Leica, L1C | 231 | 310 | 164 | 236 | 246 | 131 | 137 | 4 | 101 | 101 |
| ANMG00MYS | Trimble, L1C | 0 | 0 | 0 | 0 | 0 | 0 | 12 | 0 | 0 | 22 |
| QUIN00USA | Javad, L1W | 0 | 0 | 0 | 106 | 100 | 116 | 4 | 84 | 135 | 2 |
| HARB00ZAF | Septentrio, L1C with C1W | 0 | 3 | 194 | 286 | 165 | 184 | 132 | 246 | 378 | 277 |
| POHN00FSM | Septentrio, L1C with C1W | 116 | 77 | 64 | 60 | 73 | 93 | 90 | 99 | 64 | 85 |
| DAV100ATA | Septentrio, L1C with C1W | 296 | 288 | 137 | 144 | 62 | 67 | 62 | 200 | 102 | 307 |
| BADG00RUS | Javad, L1W | 0 | 102 | 116 | 143 | 136 | 146 | 244 | 66 | 116 | 169 |
| STPM00SPM | Trimble, L1C | 108 | 106 | 163 | 406 | 340 | 347 | 239 | 493 | 501 | 458 |
| ASCG00SHN | Trimble, L1C | 0 | 49 | 0 | 146 | 85 | 311 | 150 | 129 | 304 | 217 |
| CEDU00AUS | Trimble, L1C | 47 | 58 | 359 | 269 | 154 | 249 | 220 | 272 | 341 | 477 |
| KOKB00USA | Septentrio, L1C with C1W | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 38 |
| UTQI00USA | Javad, L1W | 0 | 0 | 0 | 0 | 6 | 9 | 0 | 0 | 0 | 2 |
| ARHT00ATA | Javad, L1W | 0 | 0 | 43 | 376 | 34 | 460 | 0 | 313 | 280 | 336 |
| WUH200CHN | Javad, L1W | 38 | 23 | 0 | 21 | 57 | 57 | 37 | 40 | 35 | 43 |
| BAKE00CAN | Septentrio, L1W | 222 | 398 | 442 | 331 | 351 | 252 | 108 | 404 | 373 | 266 |
| WTZZ00DEU (control) | Javad, L1W | 167 | 202 | 162 | 603 | 226 | 349 | 505 | 314 | 390 | 535 |

Across the 16 sites, the median of the hourly 3D p50 (m) and the count (first
quartile / median / third quartile):

| wire, consumer | h1 | h2 | h3 | h4 | h5 | h6 | count |
| --- | --- | --- | --- | --- | --- | --- | --- |
| hv3, dual | 0.913 | 0.403 | 0.227 | 0.224 | 0.232 | 0.245 | 0 / 19 / 164 |
| hv4, dual | 0.956 | 0.416 | 0.245 | 0.215 | 0.174 | 0.228 | 0 / 53.5 / 169 |
| gD, dual | 0.968 | 0.353 | 0.238 | 0.160 | 0.139 | 0.158 | 0 / 90 / 194 |
| **hv5, dual (the baseline pair)** | 0.769 | 0.291 | 0.139 | 0.156 | 0.115 | 0.129 | 60 / 145 / 331 |
| lF, dual / triple | 1.167 / 1.304 | 0.336 / 0.362 | 0.217 / 0.164 | 0.219 / 0.135 | 0.115 / 0.100 | 0.150 / 0.102 | 92.5 / 114 (medians) |
| b5, dual / triple | 1.118 / 0.886 | 0.341 / 0.336 | 0.169 / 0.158 | 0.200 / 0.139 | 0.112 / 0.109 | 0.155 / 0.096 | 138.5 / 125.5 |
| tB, dual / triple | 0.885 / 0.809 | 0.299 / 0.252 | 0.232 / 0.117 | 0.224 / 0.138 | 0.131 / 0.123 | 0.160 / 0.108 | 99 / 193 |

**WTZZ00DEU is not representative.** On the baseline wire it has 603 epochs below a
decimetre and 0.43 0.07 0.06 0.08 0.08 0.07 m; the median independent site has 145 and
0.77 0.29 0.14 0.16 0.12 0.13 m, and no site reaches WTZZ's count on any wire. Three
sites never reach a decimetre on a dual wire (ANMG00MYS, KOKB00USA, UTQI00USA), with
no coordinate or antenna cause found; their cause is unread.

**R1, applied literally (the count, strict improvement).**

| adoption | median count | sites that improve / worsen | more than +36 / -36 epochs | median change | verdict |
| --- | --- | --- | --- | --- | --- |
| `gap_reentry_relevel` (hv3 to hv4) | 19 to 53.5 | 7 / 4 of 16 | 4 / 1 | 0 | SITE-SPECIFIC |
| `gap_reentry_per_signal` (hv4 to gD) | 53.5 to 90 | 7 / 5 | 6 / 3 | 0 | SITE-SPECIFIC |
| `canonical_signal_rule: priority` (gD to hv5) | 90 to 145 | 10 / 3 | 7 / 2 | +24 | MIXED (one site short of two thirds) |

Losses of more than 36 epochs: POHN00FSM on the first; SCRZ00BOL (-146), DAV100ATA
(-151) and ASCG00SHN (-49) on the rider; CEDU00AUS (-90) and BAKE00CAN (-111) on the
latch. No adoption loses at more than a third of the sites.

**A supplementary statistic, computed after the result and labelled as such.** The
count cannot move at a site that stays above a decimetre on both wires (five to nine
sites are at zero on the early wires), so the paired change of each site's mean
hourly p50 over hours 2 to 6 was also computed by the manager. It does not replace
the frozen verdicts above.

| step | sites better by more than 0.01 m / worse | median change (m) |
| --- | --- | --- |
| `gap_reentry_relevel` | 10 / 0 | -0.023 |
| `gap_reentry_per_signal` | 7 / 6 | -0.005 |
| `canonical_signal_rule: priority` | 10 / 4 | -0.045 |
| the three together (hv3 to hv5) | 13 / 2 | -0.187 (count: +107.5 at the median, 10 sites improve, 3 worsen) |
| the dual pair to tB, triple consumer | 7 / 6 | -0.005 (count: 11 improve, 5 worsen, +23.5) |
| the dual pair to b5, triple consumer | 5 / 9 | +0.013 (count: 9 improve, 4 worsen) |
| the dual pair to lF, triple consumer | 3 / 11 | +0.020 |
| lF to b5 (the B5D knob), triple consumer | 7 / 4 | -0.008 (count: 7 sites gain more than 36, 1 loses) |

**R2 refuted.** On the latch step the six L1W consumers go from a median of 22 to 124
(4 improve, 1 worsens) and the ten L1C consumers from 150 to 191 (6 improve, 2
worsen). Both families gain, so the reading that the gain sits in the support of the
L1W phase-bias columns that an L1W consumer uses does not explain it.

**R3.** Restated on the across-site median, every three-band arm fails the L5 adoption
bars, on the first hour. The ordering seen at WTZZ00DEU does not transfer: across
sites the triple consumer on the band-5-code-removed wire has the best median count
of any arm (193 against 145) and the best hours 3 and 6, the cured-datum wire is level
with the dual pair by count, and the wire with the assertion is worse at 11 of 16
sites by the mean hourly p50 of hours 2 to 6; reading a three-band wire with a DUAL consumer costs (tB: nine sites lose more
than 36 epochs).

**R4 fails as written.** On the baseline wire the count has an interquartile range of
about 270 epochs and the hourly p50 one of 0.15 to 0.43 m, against graded effects of
tens of epochs and 0.02 to 0.09 m. Sixteen sites grade a sum of adoptions, not one
adoption. The census stays as an instrument for large effects and for defects (the
next two paragraphs); as a bar it needs more sites and a paired statistic, frozen
before use.

**The priority latch thinned the wire's C1W code biases (manager's read of the wires).**
Under `priority` the code follows the phase's attribute, so the 16 roster stations
that track L1C phase with C1W code moved their code to C1C (TBI section 1). Counted in
`products.txt`: satellites with a C1W code bias per epoch, median 30 of 31 on
`_rap_gD_dual` and 24 of 31 on `_rf0hv5_field6`; satellite-epochs with a clock and no
C1W code bias 1293 against 5323. The consumer logs a code-bias freshness flip when a
satellite's code bias appears or disappears: QUIN00USA 0 on the three earlier wires
and 13 on the latch wires, KOKB00USA 0 and 10, POHN00FSM 0 and 7, WUH200CHN 0 and 2
(ARHT00ATA 13 to 9, DAV100ATA 10 to 8, HARB00ZAF 3 to 1, STPM00SPM 1 to 0; the other
sites none). At QUIN00USA the solution leaves by up to 70 m for 2.3 h between two such
flips (281 epochs above 5 m after the first hour, TOW 269400 to 277800). Two defects, neither visible at WTZZ00DEU: the
rule's code half costs C1W coverage where C1W stations are sparse, and the consumer's
response to a vanished code bias (re-inflating its receiver code-bias states) has an
unbounded decoupling edge (invariant #7). The first is pre-registered as CSL section
7; the second is recorded in the open items for the wire-contract work.

## 5. 2026-09-21: follow-ups from the reader fix (card RXP) and the code half (CSL section 9)

- **ANMG00MYS is explained.** Its observation file pads satellite numbers with a blank;
  the old reader dropped every satellite below G10 and the consumer solved on a median
  of five satellites. Replayed by the fixed exe on the unchanged wire it has 256
  epochs below a decimetre (0 before); STPM00SPM, the other blank-padded site, 406 to
  485. The 14 other sites reproduce bit for bit. KOKB00USA and UTQI00USA remain
  unread. With the fixed exe the census median on the old wire is 191, not 145.
- **The census as a tool.** `runs/_msc/scripts/msc_add_wire.py` adds a wire (16 replays
  in about two minutes) and `msc_pair.py` prints the paired comparison; both are
  outside the repository and are owed a place in `scripts/` with the site list when
  the census is re-frozen with more sites.
- **A caution the census earned.** The scale of card L5P won every hour at WTZZ00DEU
  and lost across sites; the code half of CSL won across sites and lost at
  WTZZ00DEU. One instrument is not enough in either direction, and the frozen bars
  that named both did their work.
