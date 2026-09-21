# L5P: the L5 phase posture (the wide-lane pair scope and the extra-band code weight)

**STATUS**

- **id:** L5P
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-09-21, NOT ARMED, L5 NOT ADOPTED: the gap between the cured datum (390 epochs below a decimetre) and the band-5-code-removed emulation (535) is the band-5 CODE ROWS (scale 10: 597 and every hour better than the dual pair at WTZZ00DEU), not the wide-lane detector pairs (391); the scale fails the clock-wire bar (0.0099 / 0.0217 m) and does not transfer across the 16 census sites (hours 2 to 6 worse at 9, better at 4; hour 6 median 0.24 against 0.13 m). Original summary: Build card, pre-registered 2026-09-21. With the band-5 code datum cured (B5D) the triple field arm has 390 epochs below a decimetre; with the band-5 code removed at the stations it has 535. The emulation removed two things at once: the band-5 code rows of the solve and the band-5 pairs of the wide-lane slip detector (55 detections, 28 in the first hour). Two default-OFF knobs separate them, and together they define a posture in which the network uses the L5 phase and keeps the band-5 code only for its own bias. Gates, arms, attribution rule and bars frozen here.
- **source:** [B5D](B5D_band5_code_datum_2026-09-20.md) sections 3 and 4; [TBI](TBI_three_band_ignition_2026-09-20.md) section 4 (the L5 adoption bars)
- **knobs / posture:** new `network.cycle_slip.mw_detect_pairs` (`all`, the default, or `primary`) and `network.extra_band_code_sigma_scale` (default 1.0, network only); on the RF-0hv working bases with `processing.frequency_mode: triple` and `network.sat_gf_extra_band_free: true`
- **dates in the text:** 2026-09-21
- **hashes / run dirs:** bases `runs/_rap_b5_tri`, `runs/_rap_b5_simtri`, `runs/_rf0hv5_{field,sim}6`; emulation `runs/_rap_tB_nocode5_net`

## 1. What is built (both default to today's behaviour, byte-identical at the default)

- `network.cycle_slip.mw_detect_pairs: primary`: the Melbourne-Wubbena wide-lane
  detector of the network slip seam forms the station's primary pair only (the
  latched band-1 and band-2 signals); `all` forms every pair as today.
- `network.extra_band_code_sigma_scale: s` (s at or above 1): in the NETWORK solve the
  code sigma of a signal whose band is neither band of the pin pair is multiplied by
  s. The closed-loop consumer is pinned to 1. With B5D armed the band-5 code-bias
  column stays determined by the band-5 code rows alone, so the wire keeps a band-5
  code bias while the rows lose their pull on the shared states.

## 2. Gates, arms and the attribution rule (frozen)

- **G0** byte identity at the defaults on the dual field base, the triple field base
  with B5D, and the triple sim base with B5D.
- **G1 mechanism.** With `primary`: no `mw_wl_slip` event names a band-5 signal. With a
  scale s: the weights of the band-5 code rows in `network_pf_rows.csv` fall by s
  squared and no other row's weight changes at the first epoch.
- **G2, invariant #1 with data (sim).** Under `primary` the L5 phase must still have a
  wired slip response. Sim triple with L5-only integer slips injected (AB2's `sE_slip`
  recipe: 5 stations, 2 per arc-hour): the injected class is still detected and
  scoped by the phase-only tiers, and the consumer does not move by more than 0.02 m
  in any hour against the same arm under `all`. A failure makes `primary`
  field-blocking and the card says so.

| tag | bed | the one change against the B5D arm | the question |
| --- | --- | --- | --- |
| `p1_pairs` | field, sim | `mw_detect_pairs: primary` | the detector's share of the gap |
| `p2_sig10` | field, sim | `extra_band_code_sigma_scale: 10` | the code rows' share |
| `p3_both` | field, sim | both | the posture |
| `p4_sig10_nob5d` | field | scale 10 with B5D OFF | does a low weight alone defuse the assertion in the solve |
| `p5_sig3` | field | scale 3 with `primary` | the dose |
| replays | field | a dual consumer on each field wire | the wire's bands 1 and 2 |

- **Attribution (frozen).** The gap is 390 to 535 epochs. An arm RECOVERS it at 487 or
  more. `p1` alone recovers: the detector's response at ignition. `p2` alone: the code
  rows. Only `p3`: both. `p3` below 487: neither, and the emulation differs by
  something this card has not named.
- **B1, the L5 adoption bars of TBI section 4, unchanged,** on the best arm: every hour
  within 0.03 m of the dual pair or better, 570 or more epochs below a decimetre,
  clock wire at or below 0.0075 m tracked and 0.0140 m over all; sim within 0.03 m of
  the sim dual pair in every hour; then a 12 h field arm never worse than
  `runs/_rap_lD_dual12` by more than 0.03 m from hour 3. The across-site reading of
  card MSC is added to the recommendation when it exists.
- **Not armed by this card.**

## 3. 2026-09-21: L5P-1 result: the gap is the band-5 CODE ROWS, not the detector; the deweight wins at WTZZ00DEU, fails the wire bar, and does not transfer across sites

Build by an Opus worker on main `70da07d`, exe `gnss_processor_l5p.exe`: a shared
primitive `filter/NetworkExtraBandScope` (the constellation's pin bands from the
product cache, two pure predicates; an unresolvable pair changes nothing and is
counted); the pair scope acts in the PASS-A slip scan of
`ApplicationNetworkAssembly.cpp` (candidates, the common-band evidence, the datum
station's serial scan); the code scale acts where the row weight is formed
(`Application::networkSplitStation_`, guarded so that 1.0 runs no extra operation)
and the closed-loop consumer is pinned to 1.0; run-end lines in `NetworkTeardown`,
manifest entries when armed. Suites `MwDetectPairScope` (8), `ExtraBandCodeSigma` (10),
`CanonicalSignalStationScope` (8); `gnss_tests` 2367 ran, 2366 passed, the one known
skip (the worker's run). The loader gate and the golden verify pass. Every number
below was re-read from the run directories by the manager.

**G0 PASS** (defaults, new exe, `products.txt` and `network_events.csv` sha256 equal on
the dual field base, the triple field base with B5D and the triple sim base with
B5D). **G1 PASS:** under `primary` the `mw_wl_slip` events go from 66 (55 naming an L5
signal) to 15 with none naming one; under a scale of 10 the 221 band-5 code rows of
the first epoch have a weight ratio of 0.0100 and the other 2111 rows exactly 1. The
LLI path is untouched by the pair scope (the worker's read: the card-L run-end line
is identical, 25 assertions off the primary pair before and after).

**G2 SPLIT.** Sim triple with L5-only slips injected: the phase-only tier detects the
class in full under both scopes (`if_slip` 1635, all on band 5) and the band scope
still fires on it (194 events under `primary`, 794 under `all`, where the 481 band-5
wide-lane detections add theirs); the consumer is
0.67 0.06 0.05 0.03 0.03 0.05 m under `all` and 0.64 0.06 0.05 0.03 0.03 0.05 m under
`primary`, 548 epochs both: the first hour moves by 0.031 m against the 0.02 m bar, in
the favourable direction. By the card's letter `primary` is field-blocking.

| arm | the one change against the B5D arm | consumer 3D p50 by hour (m) | sub-dm | mw_wl_slip / band scope / T4_attrib | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| `_rf0hv5_field6` | (the dual pair) | 0.43 0.07 0.06 0.08 0.08 0.07 | 603 | 15 / 409 / 126 | 0.0069 / 0.0128 |
| `p0_tri` | none (B5D armed) | 0.68 0.09 0.06 0.11 0.09 0.04 | 390 | 66 / 705 / 230 | 0.0072 / 0.0188 |
| `_rap_tB_nocode5_net` | the emulation | 0.27 0.10 0.05 0.04 0.06 0.02 | 535 | 15 / 461 / 158 | 0.0078 / 0.0153 |
| `p1_pairs` | `mw_detect_pairs: primary` | 0.68 0.09 0.06 0.11 0.09 0.04 | 391 | 15 / 462 / 222 | 0.0073 / 0.0187 |
| **`p2_sig10`** | `extra_band_code_sigma_scale: 10` | **0.32 0.06 0.05 0.08 0.08 0.03** | **597** | 66 / 705 / 294 | **0.0099 / 0.0217** |
| `p3_both` | both | 0.32 0.06 0.05 0.08 0.08 0.03 | 597 | 15 / 462 / 287 | 0.0099 / 0.0218 |
| `p4_sig10_nob5d` | scale 10 with B5D OFF | 0.92 0.08 0.05 0.11 0.10 0.05 | 401 | 66 / 705 / 292 | the worker's: 0.0101 / 0.0227 |
| `p5_sig3` | `primary` and scale 3 | 0.77 0.09 0.06 0.11 0.09 0.04 | 389 | 15 / 462 / 299 | the worker's: 0.0102 / 0.0227 |
| sim: `p0_simtri`, `p1s_pairs` | | 0.75 0.06 0.05 0.03 0.03 0.05 (both) | 534 | | |
| sim: `p2s_sig10`, `p3s_both` | | 0.19 0.10 0.05 0.04 0.05 0.05 (both) | 531 | | |

A dual consumer replayed on each wire: `p0_tri` 349, `p1_pairs` 357, `p2_sig10` 476
(0.68 0.08 0.04 0.04 0.10 0.05 m), `p3_both` 475, `p5_sig3` 377, `p4_sig10_nob5d` 461, the
emulation 505.

**Attribution, by the frozen rule (recover at 487 or more).** `p1` 391: the detector
does not recover the gap. `p2` 597: the code rows do. `p3` 597: the pair scope adds
nothing to the scale. The lead of B5D section 3 on the band-5 wide-lane detections is
refuted on this posture. The scale without the datum cure (`p4`, 401) does not
recover either, so the two are needed together, and a scale of 3 is a null.

**B1 at WTZZ00DEU on `p2_sig10`.** Every hour better than the dual pair: PASS. 597
epochs against 570: PASS. Clock wire 0.0099 m tracked against 0.0075 and 0.0217 m over
all against 0.0140: FAIL, and worse than the unweighted arm (0.0072 / 0.0188) and the
emulation. Sim within 0.03 m of the sim dual pair in every hour: PASS. The 12 h arm
is not reached.

**Across the census sites (manager's replays, `runs/_rap_msc_*_p2_{d,t}`, exe
`gnss_processor_l5p.exe`, scripts `runs/_msc/scripts/msc_add_wire.py` and
`msc_pair.py`).** The triple consumer on the `p2_sig10` wire against the dual pair:
across-site median by hour 0.80 0.32 0.15 0.13 0.11 **0.24** m against 0.77 0.29 0.14
0.16 0.12 0.13 m; median count 163.5 against 145 with 8 sites improving and 7
worsening; the mean hourly p50 of hours 2 to 6 is better by more than 0.01 m at 4
sites and worse at 9, median +0.031 m. A dual consumer on the same wire is worse at
11 sites. The emulation's wire stays the best three-band wire across sites (count: 11
improve, 5 worsen, +23.5; first hour better by 0.10 m at the median; hours 2 to 6 a
wash).

**Reading.** The wire bar failed at WTZZ00DEU for a reason the census then showed:
the gain of the deweight is a property of that site. A likely cause, not tested: the
wire still emits a band-5 code bias that the deweighted network determines poorly,
and a consumer pinned at scale 1 trusts it at full weight, where the emulation's
wire emits none and the consumer falls back to its 100-fold variance. L5 is not
adopted and neither knob is recommended. What the card settles: the residual cost of
the third band sits in the band-5 code rows of the network solve and not in the
slip detector; the posture that holds up across sites is the one with no band-5
code in the network and none on the wire.

## 4. 2026-09-21: the standing of L5 (the author's directive) and the return plan

The author's directive: L5 is a goal of the program and is never abandoned; an
add-back that misses its bars is SEQUENCED, not closed. "Not adopted" in section 3 is
a statement about the bars on 2026-09-21, on a base that two defects found the same
day are about to move (card RXP, the reader; CSL section 7, the code half of the
latch rule).

**What the two days established in L5's favour.**

- The field loss of the first attempt was three defects of the network, none a
  property of L5: the gap re-entry hole (AB2; fixed and armed), the first-epoch latch
  (CSL; armed), the zero inter-signal assertion on band 5 (B5D; fixed behind a knob).
- With those out of the way the L5 PHASE is a gain wherever it was measured. At
  WTZZ00DEU every three-band arm is better than the dual pair in hour 6 (0.02 to 0.04
  m against 0.07 m); the two arms in which the band-5 code has no pull are better or
  equal in hours 3 to 6 (`tB_nocode5_net` 0.05 0.04 0.06 0.02, `p2_sig10` 0.05 0.08 0.08
  0.03, against 0.06 0.08 0.08 0.07 m), and `p2_sig10` in every hour. Across the 16
  census sites the three-band wire with no band-5 code has the best median count of
  any arm (193 against 145; 11 sites improve, 5 worsen) and a first hour better by
  0.10 m at the paired median.
- What still costs is the band-5 CODE: in the network solve (section 3) and on the
  wire, as a bias that a consumer trusts.
- The larger payoff of a third frequency lies in the integer layer (the extra-wide
  lane between L2 and L5 has a wavelength of 5.86 m), which the float-only posture of
  the re-founding does not use yet; AB3's readiness read passed on 2026-09-20.

**The return plan**, sequenced after the reader fix and the code half of the latch
rule have re-set the base that every L5 arm is graded against:

- **R-1** the four three-band postures re-run on the corrected base (the assertion in
  place, B5D, B5D with the code scale, no band-5 code), graded at WTZZ00DEU and on the
  census with the paired statistic.
- **R-2** the consumer's half of the posture: a band-5 code bias that the network
  barely determines is either withheld from the wire or carried with a quality the
  consumer weights by, so that the network's weight and the consumer's agree (two
  variants: withhold, or a consumer-side scale that is no longer pinned).
- **R-3** the bars of TBI section 4, on the census median and at WTZZ00DEU, 6 h then
  12 h, with the ledger rows read first.
- **R-4** L5 in the lanes add-back: the wide-lane census of AB3 includes the L2-L5
  pair from the start.
