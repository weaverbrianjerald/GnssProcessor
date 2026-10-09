# L5R: GPS L5 return plan step R-1 on the exact-update golden

**STATUS**

- **id:** L5R
- **topic:** network-vehicle
- **status:** open
- **summary:** L5 SEQUENCED. R-1 on the exact-update golden: every posture passes the sim and WTZZ00DEU's hour 1 is 0.13 m (1.47 m on 2026-09-21), but all four fail the field census and the clock wire. The field cost is the receiver's band-5 code delay, which the network solve does not estimate (station-constant band-5 offsets up to 29 m when exposed). New step R-1b (estimate it) ahead of R-2.
- **source:** the author's decision of 2026-10-08 ("run L5 alongside" the Galileo re-attempt, card [GAL2](GAL2_galileo_on_the_exact_update_2026-10-08.md)); [L5P](L5P_l5_phase_posture_2026-09-21.md) section 4 (the return plan R-1 to R-4); [TBI](TBI_three_band_ignition_2026-09-20.md) section 4 (the L5 adoption bars); [B5D](B5D_band5_code_datum_2026-09-20.md); [AB2](AB2_l5_third_frequency_2026-09-20.md) sections 2, 10 and 11; [MSC](MSC_multi_site_consumer_2026-09-21.md) (the census); [EXL](EXL_exact_update_lah_promotion_2026-10-01.md) (the posture)
- **knobs / posture:** the goldens plus `processing.frequency_mode` triple and the GPS band-5 types appended to `processing.signal_priorities.GPS` (C5Q, C5X, C5I, L5Q, L5X, L5I and their D and S types, the list of `runs/_rap_lS_tri`); B5D's `network.sat_gf_extra_band_free` and L5P's `network.extra_band_code_sigma_scale` per arm. Nothing is armed by this card.
- **dates in the text:** 2026-10-08
- **hashes / run dirs:** exe `gnss_processor_gal2.exe` (card GAL2's build, clean worktree at main `a4cda77`); dual control `runs/_rap_gal2_ctl_{f,s}6` (shared with card GAL2); arms `runs/_rap_l5r_*`; census tags `_rap_l5c_*`
- **gates (frozen at pre-registration):** P0 to P3 and B1 to B4 in section 3

**Outcome.** Question: on the current posture, which three-band posture holds the dual control? None on the field census (10 to 14 of 16 sites worse in hours 2 to 6), though all pass the sim and two pass WTZZ00DEU. The remaining cost is one unmodelled quantity: the receiver's band-5 code delay, dropped from the network solve, which leaves station-constant band-5 offsets of metres. With band-5 code out of the network the wire is clean for dual consumers, and triple consumers still lose (R-2). L5 SEQUENCED: R-1b, then R-2 (section 4).

## 1. The question

Every L5 number in the record belongs to the posture of 2026-09-21 (`runs/_rf0hv5_*`). The defects blamed for L5's losses since then are armed (the gap re-entry hole, the first-epoch latch) or built and off (the band-5 code datum B5D, the band-5 code deweight L5P). The base has moved about nine times (RXP, EV7, UPO and SCC, the MIZU00JPN drop, GRK, RF-3, OWD and PBF, WUP, SFR, EXL with LAH), and the current dual baseline at WTZZ00DEU (657 of 720 epochs below a decimetre, `runs/_rf0hv14_field6`) beats every three-band arm on record (best 597, `runs/_rap_p2_sig10`). R-1 was sequenced after the reader fix and the code half of the latch rule; both were settled on 2026-09-22.

The question: on the current posture, which of the four three-band postures holds the dual control, and where does the remaining cost sit (the band-5 code rows, the wire, the consumer)?

Pre-registered reading of the base change: card GAL section 6.1 found every satellite clock level frozen at ignition, the slant ionosphere absorbing 71 % of that error per line of sight and the code rows keeping the rest. With three bands the ionosphere is over-determined, so the residue cannot be absorbed and lands on the code rows; L5P located the remaining cost on exactly the band-5 code rows. If the frozen level was that cost, the assertion arm `t0` should now hold the dual control, or come much closer to it than on 2026-09-21.

A second lead, from the code (2026-10-08, not measured): with `network.estimate_sat_code_bias` on, every receiver `*_IFB*` state (the band-5 receiver code delay among them) is dropped from the network's local columns (`ApplicationNetworkAssembly.cpp` around lines 161 to 176) and held at its first-epoch seed. AB2 section 11 states that the band-5 receiver bias is estimated; the code does not. On two bands the dropped delay is inseparable from the station's ionosphere level and costs nothing. With three bands and the delay frozen, band-5 code determines the station's ionosphere level, and the zero-mean receiver code-bias datum may then fight band-5 code data, on exactly the band-5 code rows.

## 2. The arms

One exe and one tool (`scripts/rapid_arm.py make --base config/golden/golden_<bed>_refound.json`); 6 h; sim arms started at 00:30:15 (`network.start_offset_hours` 0.504167). The sim fixture carries L5 on all 32 GPS satellites at every station (the field on 17); the record's sim three-band arms used the same coverage.

| tag | bed | change against the golden | the posture |
| --- | --- | --- | --- |
| `gal2_ctl_{f,s}6` | field, sim | none | the dual control |
| `l5r_t0_{f,s}6` | field, sim | `frequency_mode` triple, band-5 GPS types | the assertion in place (TBI H1) |
| `l5r_t1_{f,s}6` | field, sim | `t0` + `network.sat_gf_extra_band_free` | B5D, the band-5 code datum free |
| `l5r_t2_{f,s}6` | field, sim | `t1` + `network.extra_band_code_sigma_scale` 10 | B5D with the code scale |
| `l5r_t3_{f,s}6` | field, sim | `t0` with C5 code stripped from every network station (`strip;types=C5?`; the consumer keeps it) | no band-5 code at the network (TBI's `tB_nocode5_net`) |

Replays: a dual consumer on each field three-band wire (the wire's bands 1 and 2), and the census (B4).

## 3. Gates and bars (frozen)

Pre-flight, every three-band arm (read before any consumer number):

- **P0.** Exit 0; 720 epochs emitted; the S-basis's declared dimension equal to its count below the cutoff at every epoch at `rank_gate_tol` 1e-11 (the tolerance read on the configuration that uses it), 0 completions, 0 withheld epochs.
- **P1.** The band-5 pin and assertion counters (`n_sat_gf_isc_zero`, the extra-band counters) consistent with the posture: assertions on `t0` and `t3`, none on `t1` and `t2`.
- **P2 (the latch, invariant 6 for a RINEX edit).** On `t3`, every station's band-1 and band-2 signals in `logs/network_pf_rows.csv` at the first epoch equal those of `t0` (card CPA's trap: a RINEX edit moved the first-epoch latch).
- **P3 (the lead, report).** On `t0` and `t1`: per station, the mean band-5 code post-fit residual over hours 2 to 6 with its standard error; the count of stations with |mean| above three standard errors; the same for bands 1 and 2. A station-constant band-5 offset at many stations, absent on bands 1 and 2, is the signature of the frozen receiver band-5 delay.

Bars (TBI section 4's form, re-frozen against the same-exe dual control; the absolute numbers of 2026-09-20 and the 12 h reference `runs/_rap_lD_dual12` are retired):

- **B1 (field, WTZZ00DEU).** 3D p50 every hour within 0.03 m of `gal2_ctl_f6` or better.
- **B2 (field, the clock wire).** Within-arc rms of the wire against COD (`scripts/wire_vs_code_census.py`), hours 2 to 6, on the consumer-tracked satellites and over all satellites: each at or below 1.10 times the dual control's (TBI's 0.0075 / 0.0140 m stood at 1.09 times the dual pair of its day).
- **B3 (sim).** SZZ000SIM 3D p50 every hour within 0.03 m of `gal2_ctl_s6` or better.
- **B4 (census, field).** The 16 sites of `runs/_msc/site_list.json`, 6 h replays built from each wire's own `arm.json`: triple consumers at the L5 sites (dual consumers elsewhere) on the three-band wire against dual consumers on the dual control; the paired change of each site's mean hourly p50 over hours 2 to 6 (1 cm threshold): a median at or below 0 and no more sites worse than better; hour 1 reported. Also dual consumers on the three-band wire against the control (the wire's bands 1 and 2): a median at or below +0.005 m.

Attribution (frozen): `t0` passing B1 to B3 means the base change removed the band-5 code-row cost (the frozen level was it). `t0` failing with `t1` or `t2` passing names the datum or the deweight. Only `t3` passing means the cost is still the band-5 code at the network, and P3 then decides whether the frozen receiver band-5 delay is the next suspect.

Whatever passes, L5 is not adopted by this card: R-2 (the consumer's half: withhold the band-5 code bias, or carry a quality the consumer weights by) is not built, the ledger has no rows for the two knobs, and under `tri` the ledger row of `network.estimate_iono_sh` enters regime and needs a documented golden exception. A missed bar is reported as L5 SEQUENCED with the next step of the return plan named.

<!-- Results are appended below this line under dated headings. -->

## 4. 2026-10-08: result (every number read by the manager from `runs/`)

Exe `gnss_processor_gal2.exe` (sha256 `df936f396109e367`, card GAL2's build). The dual controls reproduce the baseline pair (card GAL2 section 6). Census keys: `l5c_t<k>` (the wire's own consumers: triple at all 16 sites, every census site tracks L5) and `l5c_t<k>d` (dual consumers built from `gal2_ctl_f6/arm.json` reading the three-band wire), both against `g2c_ctl`. Grader correction as in card GAL2 section 6: the clock wire is `scripts/ftc_step_attrib.py <run> --r3`, row `e_clk` (the pre-registered `wire_vs_code_census.py` cannot form it); on the current posture the phase row is exactly 0 on every arm.

**Pre-flight.** P0: every arm 720 of 720 emitted, 0 withheld at `rank_gate_tol` 1e-11 on both beds; S-basis completions 0, except `l5r_t3_f6` (1 direction on one epoch). P1: zero assertions on band 5, 22 999 (`t0` field) and 22 530 (`t0` sim), none on `t1`, `t2` and `t3` (`t3` has no band-5 code at the network, so nothing to assert; the pre-registered expectation of assertions there was a wording error). P2: the first-epoch band-1 and band-2 signals of `t3` equal `t0`'s at 57 of 58 field stations and all 60 sim stations; BOGT00COL has no rows at `t3`'s first epoch (admission, not a latch move).

**P3, the lead.** Per station, the mean GPS code post-fit residual over hours 2 to 6 (full-weight rows; standard error from 10-minute block means):

| arm | band 1, median abs mean / stations above 0.10 m | band 2 | band 5 | largest band-5 offsets (m) |
| --- | --- | --- | --- | --- |
| `gal2_ctl_f6` (dual) | 0.017 / 0 | 0.013 / 0 | | |
| `l5r_t0_f6` | 0.086 / 28 | 0.137 / 40 | 0.220 / 37 | MAJU00MHL +1.74, LCK400IND -1.60, MBAR00UGA -0.90 |
| `l5r_t1_f6` | 0.052 / 17 | 0.067 / 23 | 0.091 / 27 | LCK400IND -1.51, MAJU00MHL +1.09 |
| `l5r_t2_f6` (band-5 code at sigma times 10) | 0.055 / 12 | 0.045 / 10 | 0.856 / 55 | LCK400IND -29.0, MAJU00MHL +21.7, PALM00ATA -5.0, HKSL00HKG +4.8 |

With the band-5 rows deweighted they stop pulling the solve and show their full misfit: station-constant offsets of metres to tens of metres. At full weight the same misfit spreads into bands 1 and 2 at the station. `logs/network_rx_states.csv` carries `GPS_DCB` and the inter-signal terms but no band-5 receiver term: the receiver's band-5 code delay is not a state of the network solve (`ApplicationNetworkAssembly.cpp` drops every `*_IFB*` column when the satellite code biases are estimated), against AB2 section 11's statement that it is estimated. On two bands that delay is inseparable from the station's ionosphere level and costs nothing; on three bands it is a station-constant error on every band-5 code row. The sim draws receiver band-5 biases of tens of centimetres, and passes B3.

| bar | `t0` assertion | `t1` B5D | `t2` B5D + scale 10 | `t3` no band-5 code at the network |
| --- | --- | --- | --- | --- |
| B1 WTZZ00DEU, 3D p50 by hour (control 0.108 0.024 0.016 0.020 0.030 0.026 m) | 0.129 0.028 0.021 0.036 0.063 0.056: FAIL (hour 5 +0.033) | 0.158 0.026 0.019 0.036 0.059 0.050: FAIL (hour 1 +0.050) | 0.132 0.028 0.020 0.036 0.058 0.039: PASS | 0.129 0.030 0.022 0.035 0.057 0.045: PASS |
| below 0.1 m of 720 (control 657) | 638 | 629 | 635 | 634 |
| B2 clock wire, all / tracked (control 0.0216 / 0.0186 m; bar 1.10 times) | 0.1136 / 0.0695: FAIL | 0.0905 / 0.0452: FAIL | 0.1330 / 0.0700: FAIL | 0.0313 / 0.0189 (1.45 / 1.02): FAIL on all, PASS on tracked |
| clock anchor events (`CLK_ANCHOR`, control 130) | 430 | 547 | 277 | 145 |
| B3 sim, 3D p50 by hour (control 0.227 0.098 0.081 0.019 0.020 0.026 m) | 0.209 0.071 0.071 0.023 0.023 0.027: PASS | 0.148 0.101 0.080 0.019 0.020 0.025: PASS | 0.154 0.100 0.080 0.019 0.020 0.024: PASS | 0.224 0.098 0.080 0.019 0.021 0.025: PASS |
| B4 census, triple consumers, hours 2 to 6 paired (median, better / worse) | +0.0298, 1 / 14: FAIL | +0.0279, 1 / 14: FAIL | +0.0346, 2 / 14: FAIL | +0.0198, 1 / 10: FAIL |
| B4 hour 1 paired | +0.281, 3 / 12 | +0.182, 4 / 12 | +0.088, 4 / 11 | +0.005, 6 / 8 |
| B4 dual consumers on the wire, hours 2 to 6 (bar +0.005 m) | +0.0126, 3 / 10: FAIL | +0.0105, 2 / 8: FAIL | +0.0059, 0 / 7: FAIL | +0.0004, 1 / 2: PASS |

The slip tiers stay about doubled on every three-band field arm (`if_slip` 183 against 70, of which 97 on band 5; `T0_band_scope` 707 against 411; 463 on `t3`).

**Reading.** The base change did most of what the pre-registration predicted: on 2026-09-21 the assertion arm's WTZZ00DEU hour 1 was 1.47 m with 314 epochs below a decimetre; it is now 0.129 m with 638, and every posture passes on the sim. What remains on the field is not the band-5 code rows as such but one unmodelled quantity behind them: the receiver's band-5 code delay, held out of the network solve. By the attribution rule, only `t3` (no band-5 code at the network) clears the wire (dual consumers neutral, clock wire on the tracked satellites at the control's, anchor events at the control's), and P3 names the frozen receiver band-5 delay as the suspect. Even on `t3` the triple consumers lose (10 of 16 sites worse): the consumer's half, R-2, is a second and separate gap, now without any wire-side excuse.

**L5 is SEQUENCED, not closed.** In L5's favour: hour 1 and the sim on every posture, and a wire whose bands 1 and 2 are clean once band-5 code leaves the network solve. The return plan gains one step ahead of R-2: **R-1b**, the receiver band-5 code delay as a carried state of the network solve under `frequency_mode` triple (one per station and constellation, with its datum stated against the satellite band-5 code biases of B5D, default off, an event at both edges), graded first by P3 (the station-constant band-5 offsets must vanish) and then by B1 to B4 as frozen here. Then R-2 (the consumer's band-5 code: withhold, or a quality the consumer weights by), R-3 and R-4 as in card L5P section 4.
