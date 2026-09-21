# MSC: the multi-site consumer census

**STATUS**

- **id:** MSC
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-09-21. Every adoption of 2026-09-20 (the gap re-entry response, the per-signal rider, the priority canonical-signal latch) and every L5 verdict was graded by one cold consumer at one site (WTZZ00DEU). The consumer replay reproduces a closed loop to 0.002 m per half hour in about 12 s, and 251 stations with observations on the day are outside the 59-station roster. The card replays independent consumers at a frozen list of sites on the recorded wires of the day, and freezes the site rule, the statistics and the reading before any replay is graded.
- **source:** [TBI](TBI_three_band_ignition_2026-09-20.md) section 5 (the replay controls); [CSL](CSL_canonical_signal_latch_2026-09-20.md) sections 4 and 5; [B5D](B5D_band5_code_datum_2026-09-20.md) sections 3 and 4
- **knobs / posture:** no new knob; `network.closed_loop.replay_products_path` with `network.closed_loop.station` moved to the site; recorded wires only, no producer run
- **dates in the text:** 2026-09-21
- **hashes / run dirs:** wires `runs/_rf0hv3_field6`, `runs/_rf0hv4_field6`, `runs/_rap_gD_dual`, `runs/_rf0hv5_field6`, `runs/_rap_lF_tri`, `runs/_rap_b5_tri`, `runs/_rap_tB_nocode5_net`; exe `build/x64-Release/gnss_processor_b5d.exe`

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
