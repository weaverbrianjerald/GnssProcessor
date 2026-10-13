# L5B: the receiver band-5 code delay as a state of the network solve (L5 return plan step R-1b)

**STATUS**

- **id:** L5B
- **topic:** network-vehicle
- **status:** open
- **summary:** L5 SEQUENCED. R-1b (the receiver band-5 code delay estimated, `network.estimate_rx_extra_band_code_delay`) removes the station-constant band-5 offsets (18 to 3 stations); R-1c (`network.cycle_slip.gap_reentry_coresumed_share`) is built; both default off. The field network contamination is one code fault at MAJU00MHL at ignition, which moves MAJU's station level direction whenever band-5 code and phase are both present (whatever the code weight or the delay's treatment); the mechanism is not established and no existing screen catches it. The triple consumer's own cost is the Block IIF L5 phase. The station-local attribution (section 9) shows that MAJU's own rows do not move its states: the move arrives through the shared solution, from the other stations, at the epoch's operator. Next: R-1d, robust ignition of the receiver code states (a design decision for the author), with two owed diagnostics (section 9).
- **source:** [L5R](L5R_l5_return_on_the_exact_update_2026-10-08.md) section 4 (the root and step R-1b); [L5P](L5P_l5_phase_posture_2026-09-21.md) section 4 (the return plan); [B5D](B5D_band5_code_datum_2026-09-20.md) (the band-5 satellite code datum); [NOM](NOM_network_object_model_2026-10-08.md) step N1 (the receiver IFB drop as a declared rule, treatment `HeldAtSeed`); the author's directive that L5 is never closed
- **knobs / posture:** new `network.estimate_rx_extra_band_code_delay` (default false); the goldens of 2026-10-09 (`03461e4`, card ICF's cold-start bound armed) plus `processing.frequency_mode` triple, the GPS band-5 types appended to `processing.signal_priorities.GPS` (the list of `runs/_rap_l5r_t0_f6`), and per arm `network.sat_gf_extra_band_free`
- **dates in the text:** 2026-10-10, 2026-10-13
- **hashes / run dirs:** controls `runs/_rap_l5g_ctl_{f6,s6}` and census key `l5c_ctl` (exe `gnss_processor_l5g0.exe`, main `47fb634`, sha256 `31081ec3b36875a1`; 2 h identity to `runs/_rap_g15_{f2,s2}` verified); arms `runs/_rap_l5b_*`; census keys `l5c_b*`; section 9 smoke `runs/_l5b_loc_smoke` (the `loc_` files in `logs/rhs_attrib/`, station-net tables in `tables/`)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Question: with the receiver band-5 code delay estimated, do the station-constant band-5 offsets vanish and is the three-band wire clean? The offsets vanish (P3 band 5: 3 stations against 18) and the sim passes, but bands 1 and 2 stay contaminated, the clock wire is six times the dual control's and the census fails (section 5). Section 6 traces it to one event: MAJU00MHL re-acquires G08 at ignition, the per-signal re-entry bridges a signal that jumped with its partner, and the weakly held station level absorbs it; without that event the network is clean. R-1c (section 8) answers the re-entry rule and changes nothing; section 9's attribution shows MAJU's own rows carry centimetres and the move comes through the shared solution. L5 SEQUENCED: R-1d is the author's design call.

## 1. The question

Card L5R ran the four three-band postures on the exact-update golden. All passed the sim; none passed the field census. Its P3 read named the cause: per station, the band-5 code post-fit residual has a constant offset of metres (LCK400IND -29.0 m and MAJU00MHL +21.7 m when the band-5 rows are deweighted; 27 stations above 0.10 m on the B5D posture), and at full weight the same misfit spreads into bands 1 and 2 at the station (17 and 23 stations above 0.10 m against 0 on the dual control).

The code read of 2026-10-10 (no run) confirms the mechanism:

- Under `network.estimate_sat_code_bias` the declared rule `RxIfbRule` (`NetworkParamInventory.cpp`, `networkRxIfbRule`) drops every receiver IFB column from the station's local block (`ApplicationNetworkAssembly.cpp`, `networkSplitStation_`). The column stays in the station's model at its first-epoch value. The rule rests on a premise, that a GIM ionosphere weighting leaves the station's ionosphere level free to absorb the delay, which is false on the goldens (`w6_iono_constraint: none`, no ionosphere weighting); a unit test asserts it.
- The held value is a one-epoch median whose divisor treats the band-5 and band-1 samples as one parameter (divisor 1 - kappa_1 = 2.5457 for GPS), so it is not the band-5 receiver delay. When a station has no band-5 code at an epoch the state is pruned and later re-born at 0.0 without a re-seed. The sim has L5 on every satellite at every station and never prunes; the field has L5 on 17 of 32 satellites.
- Held, the column is a constant in the band-5 code prediction. Through the band-5 code rows it fixes the station's geometry-free level direction (ionosphere level, receiver DCB, ambiguities) at a wrong value, which is how a band-5 error reaches bands 1 and 2.
- Every carry, harvest, snapshot and trace path for receiver code biases is generic over the station's code-bias columns that enter the local block, so a kept column is carried by the existing in-block receiver code-bias carry (`network.carry_rx_code_bias_in_block`, armed on the goldens).

The question: with the receiver band-5 code delay estimated and carried, do the station-constant band-5 offsets vanish, are bands 1 and 2 clean again, and how close does the three-band wire then come to L5R's bars?

## 2. The build

`network.estimate_rx_extra_band_code_delay` (default false). Armed, the receiver IFB rule no longer drops the columns: each station's band-5 receiver code delay is an estimated local column, carried as a random walk at `network.rx_cb_carry_q` with the receiver DCB, inside the station's whitened block. Off, every output is byte-identical.

The datum, stated (a single-epoch rank model of the network equations, built by the code reader of 2026-10-10, not a run):

- **A new null direction per (constellation, band-5 code family):** the receivers' band-5 delays against the satellites' band-5 code biases. On the assertion posture (`sat_gf_extra_band_free` off) the per-signal rows of `network.sat_gf_datum` pin every satellite band-5 column and close it; on card B5D's posture the per-(constellation, signal) sum-to-zero row closes it. The GPS field has two band-5 families (C5Q at 29 stations, C5X at 30); the key `GPS_IFB5` is per band, and a station is in one family.
- **The station geometry-free level direction** loses the closure the held column gave it and falls back to its dual-band closure, the zero-mean receiver code-bias prior (`network.rx_code_bias_datum`, sigma 10 m, armed on the goldens). The rank model puts its eigenvalue about 2.7 times below the dual posture's. The rank gate at `rank_gate_tol` 1e-11 decides whether that margin holds (P0).

Owed and not built here: a family-level rule for the in-block step erase at a B5D family re-reference (B5D measured 3 re-references on the field day, all at first appearances); the seed divisor and the re-birth at 0.0 (both exact solve no-ops once the column is estimated, both still active on the default path).

## 3. The arms

One exe built from the l5g worktree (main `47fb634` plus the knob), the tool `scripts/rapid_arm.py make --base config/golden/golden_<bed>_refound.json`, 6 h, sim arms from 00:30:15 (`network.start_offset_hours` 0.504167). The three-band keys are those of card L5R (section 2 there).

| tag | bed | change against the golden | role |
| --- | --- | --- | --- |
| `l5g_ctl_{f6,s6}` | field, sim | none (exe `gnss_processor_l5g0.exe`) | the dual control of every bar |
| `l5b_g0_{f2,s2}` | field, sim | none, new exe | G0 against `l5g_g0_{f2,s2}` |
| `l5b_g0d_f2` | field | the knob on the dual golden | G0: inert on dual |
| `l5b_t1off_{f6,s6}` | field, sim | triple + B5D | the same-exe three-band control (card L5R's `t1` on the current golden) |
| `l5b_t1on_{f6,s6}` | field, sim | triple + B5D + the knob | the build, datum by the family sum-to-zero |
| `l5b_t0on_{f6,s6}` | field, sim | triple + the knob (the assertion stays) | the build, datum by the assertion rows |

Census (B4): consumer-only 6 h replays at WTZZ00DEU and the 16 sites of `runs/_msc/site_list.json` (`scripts/clock_level/gcensus.py`), keys `l5c_b1on` (triple consumers on `l5b_t1on_f6`), `l5c_b1ond` (dual consumers built from `l5g_ctl_f6/arm.json` on the same wire), the same for `t0on`, all against `l5c_ctl`.

## 4. Gates and bars (frozen)

- **G0 (identity).** `l5b_g0_{f2,s2}` against `l5g_g0_{f2,s2}`, and `l5b_g0d_f2` against `l5g_g0_f2`: `scripts/clock_level/ident2h.py` identical (products records, consumer trace, epoch status) over 240 epochs.
- **P0 (structure, every ON arm).** Exit 0; 720 of 720 epochs emitted, 0 withheld; the S-basis's declared dimension equal to its count below the cutoff at every epoch at `rank_gate_tol` 1e-11; 0 completions. A failure is read from `logs/network_estimability.csv` before any consumer number.
- **P1 (the ON telemetry, invariant 4).** The knob's run-end report shows an estimated band-5 receiver column at every field station that tracks band-5 code (at least 55 stations) and non-zero in-block carry injects; `logs/network_rx_states.csv` carries `GPS_IFB5` rows.
- **P3 (the mechanism, primary).** `scripts/clock_level/band5_offsets.py`, hours 2 to 6. On `l5b_t1on_f6`: band 5 at most 3 stations with |mean| above 0.10 m and a median |mean| at or below 0.03 m (card L5R `t1`: 27 and 0.091 m); bands 1 and 2 at most 2 stations above 0.10 m each and a median |mean| within 0.005 m of the dual control's. The same read on `l5b_t1off_f6` reproduces card L5R's `t1` pattern (the attribution control).
- **B1 to B4,** as frozen in card L5R section 3, re-frozen against the same-exe dual control: B1 WTZZ00DEU 3D p50 every hour within 0.03 m of `l5g_ctl_f6` or better; B2 the clock wire (`scripts/ftc_step_attrib.py <run> --r3`, row `e_clk`, hours 2 to 6, all and consumer-tracked satellites) at or below 1.10 times `l5g_ctl_f6`'s; B3 SZZ000SIM 3D p50 every hour within 0.03 m of `l5g_ctl_s6` or better; B4 the census, triple consumers against `l5c_ctl`: paired median change over hours 2 to 6 at or below 0 and no more sites worse than better (1 cm threshold), and dual consumers on the three-band wire: median at or below +0.005 m.

Reading rules, frozen:

- P3 passing with B4's dual-consumer clause passing means the held receiver delay was the field cost of the three-band wire's bands 1 and 2.
- P3 passing with the triple consumers still losing (B4's first clause) names the consumer's half as the remaining gap: R-2 next (a filtered-wire replay grades its withhold variant with no producer run; card L5R section 4), together with the Block IIF inter-frequency clock variation on the L5 phase (card AB2 section 6, card PBH section 8), which no posture of the record has modelled.
- P3 failing names a second station-constant band-5 term the column does not absorb, read per station and satellite family before any other step.
- Whatever passes, L5 is not adopted by this card: R-2 and R-3 are not built, the ledger has no rows for the three L5 knobs, and under triple the ledger row of `network.estimate_iono_sh` enters regime and needs a documented golden exception. A missed bar is reported as L5 SEQUENCED with the next step named.

<!-- Results are appended below this line under dated headings. -->

## 5. 2026-10-10: result of the frozen gates (every number read by the lead from `runs/`)

Exe `gnss_processor_l5b1.exe` (sha256 `85682d27bb9ec4b9`, worktree `l5g/base` commit `30f40dc`: the build after three adversarial reviews and a fix round). The fix round added one rule: an extra-band code row whose satellite code bias is not a shared column (held by fewer than two stations) carries that bias unmodelled and is excluded at zero weight (833 rows on the field arm, 0 on the sim). The three-band control `l5b_t1off_{f6,s6}` (card L5R's `t1` on the current golden) ran on `gnss_processor_l5g0.exe`; a 2 h knob-off three-band arm on the new exe (`l5b_g0t_f2`) is identical to its first 240 epochs.

| gate | measured | verdict |
| --- | --- | --- |
| G0 | `l5b_g0_{f2,s2}` and `l5b_g0d_f2` (the knob on the dual golden) identical to `l5g_g0_{f2,s2}`; the knob is inert on dual and says so; `l5b_g0t_f2` identical to `l5b_t1off_f6` | PASS |
| P0 | every ON arm 720 of 720 emitted; rank gate declared = observed (1 GPS-only) at every epoch at 1e-11; 0 completions | PASS |
| P1 | field `t1on`: GPS_IFB5 estimated on 41 569 station-epochs at 58 stations, 189 unobserved, 0 held at the seed; 41 157 in-block folds; 3 re-couplings. Sim: 43 200 station-epochs at 60 stations, 0 excluded rows | PASS |
| P3 band 5 (`t1on_f6`) | 3 stations above 0.10 m (bar 3), median abs mean 0.033 m (bar 0.03); control `t1off` 18 and 0.054 m; assertion posture `t0on` 36 and 0.133 m | band-5 offsets removed; the median misses its bar by 3 mm |
| P3 bands 1 and 2 | 17 and 12 stations above 0.10 m, medians 0.066 and 0.054 m (dual control 0 and 0, 0.016 and 0.012 m; `t1off` 20 and 14) | FAIL |
| B1 WTZZ00DEU | 0.126 0.030 0.022 0.035 0.058 0.045 m against 0.113 0.023 0.016 0.020 0.030 0.026 m (hour 5 +0.028) | PASS (narrowly) |
| B2 clock wire | 0.1443 / 0.0850 m against 0.0242 / 0.0198 m (`t1off` 0.1071 / 0.0745) | FAIL |
| B3 sim | `t1on_s6` 0.149 0.101 0.080 0.020 0.020 0.024 m against 0.227 0.098 0.081 0.019 0.020 0.026 m; `t0on_s6` 0.210 0.072 0.071 0.023 0.022 0.026 m | PASS |
| B4 census | triple consumers `l5c_b1on`: hours 2 to 6 median +0.0373 m, 2 better, 13 worse (`l5c_b1off` +0.0297); dual consumers on the wire `l5c_b1ond`: +0.0060 m, 1 better, 7 worse (`l5c_b1offd` +0.0132; bar +0.005); hour 1 halves (+0.09 against +0.18 m) | FAIL |

**What the gates say.** The build does what it states: the station-constant band-5 offsets are gone (18 to 3 stations), every structural gate passes, and the dual users of the three-band wire lose half as much. But bands 1 and 2 of the network stay contaminated and the clock wire is six times the dual control's. By the frozen reading rules this is neither the held delay alone nor a second station-constant band-5 term (band 5 is clean). Section 6 finds what it is.

## 6. 2026-10-10: the attribution (one-change 3 h and 6 h arms on `t1on`, same exe)

P3 counts are stations with a band-1 / band-2 code mean above 0.10 m (hours 2 to end); `ref` is `t1on` at 3 h.

| arm | change | bands 1 / 2 | reading |
| --- | --- | --- | --- |
| `l5x_ref_f3` | none | 10 / 6 | the contamination grows over the run (CAS100ATA 5 to 33 cm, WARK00NZL 4 to 36 cm over 6 h) |
| `l5b_t2on_f6`, `l5b_t3on_f6` | band-5 code at sigma times 10 / times 100 | 13 / 13; 38 / 27 | not the band-5 code weight |
| `l5b_t2mwp_f6` | plus `cycle_slip.mw_detect_pairs` primary | 16 / 13 | not the wide-lane detector |
| `l5x_diag_f3` | receiver code-bias carry on the diagonal route | 15 / 10 | not the carry route |
| `l5x_pbq9_f3`, `l5x_pbq7_f3` | satellite phase biases as random walks (3e-9, 3e-7 m2/s) | 9 / 6; 9 / 5 | not the Block IIF inter-frequency clock |
| `l5b_t3s_f6` | band-5 code stripped at every network station | 2 / 1 | clean |
| `l5x_nol5ph_f3` | band-5 phase stripped at every station | 2 / 0 | clean |
| `l5x_noiif_f3` | band 5 stripped on Block IIF satellites | 1 / 0 | clean |
| `l5x_iifnol_f3`, `l5x_iifnoc_f3` | IIF band-5 phase only / IIF band-5 code only stripped | 1 / 0; 5 / 2 | clean; partly |
| `l5x_nomaju_f3` | band 5 stripped at MAJU00MHL alone | 2 / 0 | clean |
| `l5x_g08fresh_f3` | G08 removed at MAJU00MHL for 17 min (its arc restarts fresh) | 2 / 0 | clean |

Every contaminated arm, and none of the clean ones, has a burst of clock-anchor decouples at TOW 260040 (20 to 30 satellites, `e_phi` up to 16 m; 0 to 3 on the clean arms). At that epoch MAJU00MHL re-acquires G08, a Block IIF satellite, after a one- to two-epoch gap on L1C and L2W while L5Q tracks through. Every G08 code residual at MAJU becomes 7 to 9 m; the per-signal re-entry tier (card GRS) judges L2W shifted against the L5Q partner (11.98 m) and refloats it, and judges L1C clean (0.026 m) and BRIDGES its carry. L1C and L5Q had jumped together, which a geometry-free test against L5Q cannot see; in the same epoch MAJU's receiver code states move along the station level direction (DCB -2.2 to -9.3 m, band-5 delay 8.4 to 26.1 m, the second code channel's ISC 1.3 to 14.4 m), the anchored phase biases carry the move into the network clock, and the anchor guard decouples 20 satellites. On the dual posture L1C and L2W return together, are judged together and are both refloated, and the post-fit tier refloats the arc: no trip. With G08's arc restarted fresh, MAJU's band-5 delay stays at 8.2 to 8.4 m all run: the "+32 m" of `t1on` was the event's absorption, not the receiver.

**Reading.** Two defects compound. (a) The per-signal re-entry bridges a co-resumed signal on a geometry-free test that is blind to a common jump of the signal and its partner. (b) With the band-5 delay estimated, the station's level direction is closed only by the zero-mean DCB prior (sigma 10 m), so during ignition a single line-of-sight fault is absorbed into the station's receiver states instead of being exposed to the post-fit test, and the absorption spreads through the anchored phase biases. Neither is a property of L5: (a) is card GRS's rule meeting its first three-band re-acquisition, (b) is the datum this card chose. The Block IIF signature (`noiif` clean) is MAJU's G08 being an IIF satellite.

**Standing.** L5 SEQUENCED, not closed. In L5's favour: the band-5 offsets are gone, the sim passes, the dual users of the wire halve their loss, and one station's one event explains the field's network contamination. Next, step R-1c (being built): co-resumed signals of a line of sight share the re-entry verdict (`network.cycle_slip.gap_reentry_coresumed_share`, default off), graded by P3 and B1 to B4 as frozen. Then, if needed, a firmer closure of the station level direction under the estimated delay. The triple consumers' own loss (WTZZ00DEU's hours 4 to 6 are the same on every network posture, including `t3s`) is R-2's.

## 7. 2026-10-10: the consumer half (replays only, no producer run)

Triple consumers at WTZZ00DEU and the 16 census sites, replayed on the clean network wire `l5b_t3s_f6` (band-5 code stripped at the network, so no network-side contamination; every consumer keeps its own band-5 data), against `l5c_ctl`. The consumer-side RINEX edits act on each site's own file only (`scripts/clock_level/gcensus.py make ... --rinex "...;stations={SITE};consumer=1"`, added for this card).

| key | consumer's own band 5 | hours 2 to 6 paired median (better / worse) | hour 1 | WTZZ00DEU by hour (m) |
| --- | --- | --- | --- | --- |
| `l5c_t3s` | all | +0.0195 m (0 / 10) | +0.035 m | 0.128 0.030 0.022 0.035 0.057 0.045 |
| `l5c_t3sci` | Block IIF band 5 removed | -0.0005 m (1 / 3) | -0.011 m (8 better) | 0.090 0.023 0.017 0.018 0.028 0.024 |
| `l5c_t3scl` | Block IIF L5 phase removed | -0.0004 m (1 / 3) | -0.011 m (9 better) | 0.090 0.023 0.017 0.018 0.028 0.024 |
| `l5c_t3scc` | Block IIF band-5 code removed | +0.0202 m (2 / 10) | +0.046 m | 0.126 0.030 0.022 0.035 0.058 0.055 |

**Reading.** The triple consumer's loss is the Block IIF L5 phase at the consumer, and nothing else measured here: without it the triple consumer is neutral to better against dual (WTZZ00DEU better than dual in every hour). The physical suspect is the Block IIF inter-frequency clock variation (a thermally driven, satellite-specific motion of the L5 signal of order a decimetre, cards AB2 section 6 and PBH section 8): the wire carries a constant L5 phase bias per satellite, so a consumer's IIF L5 phase rows see the variation as a misfit that its float ambiguities cannot absorb. In the network the same variation is absorbed (the station-common post-fit L5 and L2 phase residual of IIF satellites varies by 1.3 mm sd against 0.2 mm for Block III; letting the satellite phase biases random-walk changes nothing in the network). R-2 therefore becomes the IIF L5 phase: either the network estimates a time-varying IIF L5 phase bias and the phase-bias hold passes it to the wire (the principled fix), or the consumer leaves IIF L5 phase out (the stopgap these replays emulate).

## 8. 2026-10-10: step R-1c and the probes of the ignition event

**R-1c built** (`network.cycle_slip.gap_reentry_coresumed_share`, default off; commit `56fb741`, exe `gnss_processor_l5b2.exe`, sha256 `c8d5786e7315873b`): signals of one line of sight that resume from a gap at the same epoch share the per-signal tier's verdict. G0: the goldens identical at 2 h on both beds; armed on the dual field golden, identical at 2 h (12 co-resumed lines of sight, none answered). On the three-band field arm (`l5c1_t1on_f6`) it answers exactly the MAJU00MHL / G08 event (one line of sight, L1C freed by L2W's verdict) and changes nothing else: bands 1 and 2 still 17 and 12 stations above 0.10 m, 20 anchor decouples at 260040, clock wire 0.1445 / 0.0849 m, consumer identical. The bridge was not the driver.

**The mechanism, refined.** At MAJU00MHL after the G08 re-acquisition the dual control's G08 code residuals are 2.3 to 3.3 m, decaying, and MAJU's receiver code states stay put (DCB -0.84 to -1.01 m); on the three-band arm they are 7 to 9 m and the states jump in one epoch (DCB -2.2 to -9.3 m, band-5 delay 8.4 to 26.1 m, C1W channel offset 1.3 to 14.4 m). With two bands the line of sight's own slant ionosphere absorbs the geometry-free part of a single line of sight's code fault exactly (one ionosphere, two bands); with three bands the ionosphere is over-determined, so the geometry-free part of the fault has to go to station-level geometry-free states, and at 14 minutes MAJU's are young (weakly determined, closed only by the zero-mean DCB prior) and then carried stiffly. The arm that removes G08 at MAJU for 17 minutes is clean because when G08 returns the station's states are mature.

| probe (3 h, on `l5c1_t1on_f6`) | bands 1 / 2 above 0.10 m | decouples at 260040 |
| --- | --- | --- |
| reference (`l5x_ref_f3`, before R-1c) | 10 / 6 | 20 |
| `network.gross_los_cold_bound_m` 4 (12 on the goldens) | 8 / 5 | 20 |
| `network.gross_los_k` 4 (8 by default) | 7 / 4 | 20 |
| `network.raim_we_code_iono` (WE-CI): never tripped, pool sigma0 at most 0.43 | 10 / 6 | 20 |
| `network.second_code_channel` off | 17 / 5 | 20 |

No existing screen sees the fault: it is absorbed into the station states before any residual exposes it.

**Standing.** L5 SEQUENCED. The open item is a design decision, stated for the author: robust ignition of the receiver code states under three bands. Candidates: (i) the receiver code-bias carry stays soft until the states are determined (an ignition window, or a variance threshold before the in-block carry becomes stiff), so a fault absorbed early is not locked in; (ii) a station-level geometry-free consistency test on the three-band code (the geometry- and ionosphere-free combination 0.226 P1 - 1.226 P2 + P5 per line of sight, against its arc history) that excludes a line of sight whose combination jumps; (iii) a firmer prior on the station level direction while the band-5 delay is estimated. R-1c is kept (default off) as the right rule for co-resumed signals; whether it is needed once (i) to (iii) exist is for the gates then.

**Addendum (same day): two more probes and the raw data.** A softer receiver code-bias carry (`network.rx_cb_carry_q` 1e-6 instead of 1e-9, `l5x_rxq6_f3`) and a firmer station-level datum (`network.rx_code_bias_datum_sigma_m` 1 m instead of 10, `l5x_rxd1_f3`; neutral on dual, `l5x_rxd1d_f3`) both leave the event in place: MAJU's DCB and band-5 delay still move 7 and 18 m at TOW 260040, 20 decouples, bands 1 and 2 at 9 / 6 and 7 / 5 stations. The moved direction is the station's geometry-free level, which no data pull back once moved and which a prior of one metre cannot hold against several code rows. In the raw RINEX the event is not a single step: G08 is rising at MAJU and its codes are noisy at the metre level around the re-acquisition (C1C minus C5Q swings by 3.8 m from epoch to epoch; the geometry-, clock- and ionosphere-free combination 0.226 C1C - 1.226 C2W + C5Q wanders between -3.3 and +1.8 m with no clean jump). With two bands each line of sight's slant ionosphere absorbs such geometry-free code noise; with three it is over-determined, so the noise has to go somewhere station-level, and during ignition the station level is the weakest direction. So candidate (ii) of section 8 (a jump test on that combination) would not catch this case; what is owed is a weighting or ignition rule for band-5 code rows whose geometry-free content the station cannot yet absorb, or the station level made firm by information rather than by a prior. Recorded for the author's R-1d decision.

**Correction (same day, before any further step): the over-determined-ionosphere reading above is refuted by this card's own arms.** With band-5 code at sigma times 100 (`l5b_t3on_f6`, band-5 code information reduced 10 000-fold) the event is unchanged: MAJU's DCB moves -1.5 to -8.4 m, its band-5 delay 6.2 to 23.8 m and its C1W channel offset 0.2 to 13.1 m at TOW 260040, with 21 decouples; at sigma times 10 (`l5b_t2on_f6`) the same (11 decouples). With no band-5 code at the network (`l5b_t3s_f6`) MAJU's DCB moves only -1.6 to -2.4 m and 1 satellite decouples. So the band-5 code's information is not what moves the station. Nor is the estimated band-5 delay column: the held-delay control `l5b_t1off_f6` has the same event (30 decouples at 260040). What every event arm shares and every clean arm lacks is band-5 code and band-5 phase present together at MAJU00MHL's G08 (stripping either, at all stations, at MAJU, or on the IIF satellites, removes the event), whatever the band-5 code weight and whether the band-5 delay is estimated or held. The handling at the epoch differs between arms (the MW drain refloats L5Q only when band-5 code is present; the per-signal tier bridges L1C against L5Q; a row-level unmeasurable bridge in the arms without L5Q), and the move follows the station level direction (DCB and band-5 delay in the ratio -0.36 expected of it). What the event does to that direction, and why it moves it by about 10 m, is not established; the instrument owed is an attribution of the station's local solution increment at the event epoch by row family (card ICF's instrument covers shared columns only). R-1d stays the author's design decision, now with that instrument named as its first step.

## 9. 2026-10-13: the station-local attribution at the event (the instrument named in section 8)

**The instrument** (commit `5688b61` on `l5g/combined`, diagnostics only). `network.rhs_attrib_columns` accepts `LOC:<station>:<key>` labels. At an armed dump each label decomposes the station's reconstructed increment of that state into the station's own rows, folds and priors (the direct part), the feedback through the shared solution (split back to every station's rows, the shared folds, the DD folds and the datum), and the sum-to-zero and DD-prior corrections. It writes per-station nets (`NETSTA`: a station's whole effect, direct plus through the shared solution), per-(station, satellite) nets, a one-step refinement check and a re-solve check, to `logs/rhs_attrib/loc_<tow>.csv`. Products and the existing `attrib_`, `rows_`, `vec_` and `cols_` files are byte-identical with and without the labels and against the pre-change exe. The decompositions close to 3e-12 m, the split of the feedback by station to 2e-5 to 4e-4 m; removing a group's prefits and re-solving reproduces its attributed effect to 7e-8 m or better for a whole station and 9e-5 m for a single line of sight of 5.6e4 m. Single per-kind, per-satellite and per-row shares reach 1e3 to 1e4 m and cancel within a station, and a single shared column's share depends on the datum (invariant #5), so only station nets are read here.

**The run** (`runs/_l5b_loc_smoke`). The arm `l5c1_t1on_f6` cut to 900 s (exe `gnss_processor_l5bloc4.exe`), dumps at TOW 260010, 260040 and 260070, labels `LOC:MAJU00MHL:GPS_DCB`, `LOC:MAJU00MHL:GPS_IFB5`, `LOC:MAJU00MHL:GPS_ISC_C1W`.

| MAJU00MHL state (m), TOW 260010 to 260040 | solved | MAJU direct | MAJU net | 57 other stations | shared folds, DD folds, datum |
| --- | --- | --- | --- | --- | --- |
| GPS_DCB | -4.33 to -11.34 | -2.87 to -2.93 | -6.80 to -4.57 | +2.77 to -10.16 | -0.31 to +3.38 |
| GPS_IFB5 | +8.36 to +26.08 | +1.94 to +1.76 | +12.62 to +2.45 | -7.88 to +21.33 | +3.62 to +2.30 |
| GPS_ISC_C1W | +1.32 to +14.36 | -0.54 to -0.41 | +5.38 to +0.26 | -4.42 to +20.68 | +0.37 to -6.58 |

**What it shows.**

- MAJU00MHL's own data do not move its receiver code states at the event. MAJU's net moves toward zero on all three states (+2.2, -10.2 and -5.1 m), and its direct part is flat. Its G08 code rows, with post-fit residuals of 6.6 to 9.0 m, have a whole effect of at most 0.023 m.
- The move arrives through the shared solution from the other stations: -12.9, +29.2 and +25.1 m. The stations whose nets change most are the same on all three states: KOKV00USA, WARK00NZL, DARW00AUS, GAMB00PYF, GUAM00GUM, MAC100AUS, then NIUM00NIU, NYA200NOR and GCGO00USA (on GPS_DCB -3.43, -1.93, -1.62, -1.54, -1.45 and -1.21 m for the first six).
- At 260040, and only there, the in-epoch clock-anchor withdrawal of card Q3 appears as a shared fold (`q3_withdraw`: +273, -607 and -531 m on the three states) and nearly cancels the dense border carry's fold (-270, +610 and +524 m); only their sum is read.
- The station's geometry-free level direction (`GFDIR:MAJU00MHL`, loadings DCB +0.647 and IFB5 -1.793, the ratio -0.361 of section 6) moves from -11.93 to -16.82.

**Reading, with its limit.** The attribution is linear in the right-hand side at the epoch's own operator, so a change in a station's net between two epochs mixes a change in that station's data with a change in the operator. Section 6's arms show that the event needs MAJU00MHL's G08 with band-5 code and phase; the instrument shows that MAJU's own rows do not carry the move. Together they point at the operator: what the G08 re-acquisition changes at MAJU (its G08 columns refloated or bridged) and the anchor withdrawal of 20 satellites that follows change how the other stations' information on the shared satellite columns maps into MAJU's level direction. That is a reading, not yet a measurement. The first report of the instrument named MAJU's G08 carried-ambiguity rows and a ranking of phase-bias columns as the drivers; the review withdrew both (the G08 rows' shared effect is cancelled by MAJU's other parts, and the column ranking ranks cancelling, datum-dependent pairs).

**Owed, both diagnostics-only extensions of the same readout:** (a) a dump of the anchored solve before the guard's withdrawal, which says whether MAJU's states have moved before the 20 satellites decouple; (b) the 260040 right-hand side evaluated under the 260010 operator, which separates the data change from the operator change.

**Standing.** L5 SEQUENCED. The instrument narrows R-1d: a rule acting on MAJU's own code rows (a code screen, a code weight, a GFIF jump test) would not act on this event, since those rows carry centimetres; the firmer prior on the station level was probed already (section 8, 1 m: no change). What moves the station is the shared solution's response at the epoch. R-1d stays the author's design decision, with (a) and (b) as its next measurements.
