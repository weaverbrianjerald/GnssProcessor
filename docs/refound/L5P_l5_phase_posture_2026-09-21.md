# L5P: the L5 phase posture (the wide-lane pair scope and the extra-band code weight)

**STATUS**

- **id:** L5P
- **topic:** network-vehicle
- **status:** open
- **summary:** Build card, pre-registered 2026-09-21. With the band-5 code datum cured (B5D) the triple field arm has 390 epochs below a decimetre; with the band-5 code removed at the stations it has 535. The emulation removed two things at once: the band-5 code rows of the solve and the band-5 pairs of the wide-lane slip detector (55 detections, 28 in the first hour). Two default-OFF knobs separate them, and together they define a posture in which the network uses the L5 phase and keeps the band-5 code only for its own bias. Gates, arms, attribution rule and bars frozen here.
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
