# L5B: the receiver band-5 code delay as a state of the network solve (L5 return plan step R-1b)

**STATUS**

- **id:** L5B
- **topic:** network-vehicle
- **status:** open
- **summary:** L5 SEQUENCED. R-1b (the receiver band-5 code delay estimated, `network.estimate_rx_extra_band_code_delay`) removes the station-constant band-5 offsets (18 to 3 stations); R-1c (`network.cycle_slip.gap_reentry_coresumed_share`) is built; both default off. The field network contamination is one code fault at MAJU00MHL at ignition, which moves MAJU's station level direction whenever band-5 code and phase are both present (whatever the code weight or the delay's treatment); the mechanism is not established and no existing screen catches it. The triple consumer's own cost is the Block IIF L5 phase. The station-local attribution (section 9) shows that MAJU's own rows do not move its states: the move arrives through the shared solution, from the other stations, at the epoch's operator. Next: R-1d, robust ignition of the receiver code states (a design decision for the author), with two owed diagnostics (section 9). On the GPS + Galileo golden (section 10, 2026-10-23) GPS L5 runs stable on the sim at 2 h with B5D and the estimated delay; the assertion posture still leaves band-5 and band-2 offsets. With Galileo E5b added (section 11, every channel the sim tracks) it stays stable with no code change. On the field (section 12, 2 h) Galileo E5b alone is clean, and GPS L5 still brings the MAJU00MHL event unchanged. Section 13: the jump is in the anchored solve (the withdrawal is a consequence); the only real slip is L2W's, correctly refloated; with the per-signal re-entry off the station's jump shrinks tenfold while the 21-satellite anchor burst stays; five R-1d options are laid out.
- **source:** [L5R](L5R_l5_return_on_the_exact_update_2026-10-08.md) section 4 (the root and step R-1b); [L5P](L5P_l5_phase_posture_2026-09-21.md) section 4 (the return plan); [B5D](B5D_band5_code_datum_2026-09-20.md) (the band-5 satellite code datum); [NOM](NOM_network_object_model_2026-10-08.md) step N1 (the receiver IFB drop as a declared rule, treatment `HeldAtSeed`); the author's directive that L5 is never closed
- **knobs / posture:** new `network.estimate_rx_extra_band_code_delay` (default false); the goldens of 2026-10-09 (`03461e4`, card ICF's cold-start bound armed) plus `processing.frequency_mode` triple, the GPS band-5 types appended to `processing.signal_priorities.GPS` (the list of `runs/_rap_l5r_t0_f6`), and per arm `network.sat_gf_extra_band_free`
- **dates in the text:** 2026-10-10, 2026-10-13, 2026-10-23
- **hashes / run dirs:** controls `runs/_rap_l5g_ctl_{f6,s6}` and census key `l5c_ctl` (exe `gnss_processor_l5g0.exe`, main `47fb634`, sha256 `31081ec3b36875a1`; 2 h identity to `runs/_rap_g15_{f2,s2}` verified); arms `runs/_rap_l5b_*`; census keys `l5c_b*`; section 9 smoke `runs/_l5b_loc_smoke` (the `loc_` files in `logs/rhs_attrib/`, station-net tables in `tables/`); sections 10 and 11 (GPS + Galileo sim golden) `runs/_rap_l5s_{ctl,t0,t1,t1on,all}_{sm,s2}`; section 12 (field) `runs/_rap_l5f_{ctl,t1on,e5b,all}_{fsm,f2}`; section 13 branch `card/l5b-diag` (`b75094d`, exe `gnss_processor_l5diag.exe`), event-window arms `runs/_rap_l5d_*_{fsm,dg}` (exe `gnss_processor_l5s0.exe`, main `04517ca`, sha256 `1f41efd915d4aaf0`)
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

## 10. 2026-10-23: GPS L5 on the GPS + Galileo sim golden (2 h development check)

The goldens moved after section 9: Galileo E1/E5a armed (card GAD, 2026-10-13) and the station parameter set owned by the network (card NOM step N6, merged 2026-10-20). This section re-runs the three-band postures of section 3 on the current sim golden, sim only, 2 h from 00:30:15, before any field arm.

Exe `gnss_processor_l5s0.exe` (main `04517ca`, sha256 `1f41efd915d4aaf0`). Arms `runs/_rap_l5s_<arm>_s2`, made from `config/golden/golden_sim_refound.json` with `network.start_offset_hours` 0.504167; the three-band keys are those of section 3 (`processing.frequency_mode` triple, the GPS band-5 types C5Q, C5X, C5I, L5Q, L5X, L5I and their D and S types appended to `processing.signal_priorities.GPS`). Galileo stays on E1/E5a: its priority list has no E5b type, so the triple-mode target E5b is never formed. The fixture tracks C1C, L1C, C2W, L2W, C5Q and L5Q on every GPS satellite. A 12-epoch smoke of each arm (`runs/_rap_l5s_<arm>_sm`) preceded the 2 h runs.

| arm | change against the golden | emitted / withheld | S-basis completions | GPS_IFB5 rows | SZZ000SIM 3D p50, hours 1 and 2 (m) | stations above 0.10 m, bands 1 / 2 / 5 | median abs mean, bands 1 / 2 / 5 (m) | wall (s) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `ctl` | none | 241 / 0 | 0 | 0 | 0.099 0.033 | 0 / 1 / - | 0.023 / 0.018 / - | 262 |
| `t0` | triple (the assertion stays) | 241 / 0 | 0 | 0 | 0.089 0.028 | 11 / 44 / 42 | 0.046 / 0.203 / 0.152 | 411 |
| `t1` | triple + `network.sat_gf_extra_band_free` (B5D) | 241 / 0 | 0 | 0 | 0.099 0.037 | 0 / 0 / 1 | 0.023 / 0.022 / 0.023 | 415 |
| `t1on` | `t1` + `network.estimate_rx_extra_band_code_delay` | 241 / 0 | 0 | 14 460 | 0.099 0.035 | 0 / 0 / 1 | 0.022 / 0.021 / 0.023 | 417 |

Band offsets: `scripts/clock_level/band5_offsets.py`, GPS code rows, hour 2. The clock-datum dimension is 2, declared and realized, on every arm. Post-fit phase median per band (`scripts/rapid_arm.py score --pf`, both constellations pooled): control L1 2.1, L2 1.2, L5 1.2 mm; every three-band arm L1 2.4, L2 0.8, L5 1.2 mm. Warning types new against the control: the estimability listing of the weak GPS L5Q phase-bias directions (above the cutoff) on every three-band arm, and the held-seed warning of the receiver band-5 delay (60 stations) on `t0` and `t1`, where the knob is off.

**Reading.**

- The current golden carries GPS L5 on the sim at every posture: 241 of 241 epochs emitted, none withheld, no S-basis completion. Wall time is 1.6 times the dual control's.
- The assertion posture (`t0`) is wrong on this bed as on the field (cards L5R, B5D): the band-5 satellite code bias pinned at T_GD leaves station-level band-5 offsets at 42 stations and moves them into band 2 (44 stations). Freeing it (`t1`) removes them: bands 1, 2 and 5 sit at the dual control's level.
- The estimated receiver band-5 delay (`t1on`) is neutral on the sim, as section 1 predicts: L5 is tracked on every satellite at every station, so the held seed is never pruned and re-born. Its telemetry is live (GPS_IFB5 at 60 stations, every epoch).
- The consumer moves by at most 0.010 m in either hour on any arm; at 2 h that is inside the ignition spread, not a verdict.

**Standing.** On the sim, GPS L5 is stable on the current golden at posture `t1on` (this card's build). The sim certifies the algebra only (a quiet fixture: no slips, no gaps, L5 on every satellite), so the field event of sections 6 to 9 is not tested here, and R-1d stays the author's design decision. L5 SEQUENCED. Next step named by the author: all tracking channels on the sim (on this fixture, Galileo E5b as a third Galileo band).

## 11. 2026-10-23: all tracking channels on the sim (GPS L1/L2/L5, Galileo E1/E5a/E5b; 2 h development check)

The author's next step after section 10: posture `t1on` plus Galileo E5b, so each constellation tracks three bands under `processing.frequency_mode` triple (its targets: GPS L1, L2, L5; Galileo E1, E5a, E5b). On this fixture that is every tracked channel: C1C, L1C, C2W, L2W, C5Q, L5Q on GPS and C1C, L1C, C5Q, L5Q, C7Q, L7Q on Galileo, one channel per band. The arm `runs/_rap_l5s_all_s2` is `t1on` with C7Q, C7X, L7Q and L7X appended to `processing.signal_priorities.Galileo` (the Q and X pattern of the golden's E1 and E5a entries); same exe as section 10. A 12-epoch smoke (`runs/_rap_l5s_all_sm`) preceded it. The code paths of both knobs are generic over constellation and band (the receiver key `Galileo_IFB7`, the extra-band datum keyed by band digits), and no code changed.

| arm | emitted / withheld | S-basis completions | receiver code-bias keys (rows each) | SZZ000SIM 3D p50, hours 1 and 2 (m) | median sigma0_pf by hour |
| --- | --- | --- | --- | --- | --- |
| `ctl` | 241 / 0 | 0 | GPS_DCB, Galileo_DCB (14 460) | 0.099 0.033 | 0.379 0.429 0.449 |
| `t1on` | 241 / 0 | 0 | + GPS_IFB5 | 0.099 0.035 | 0.368 0.412 0.446 |
| `all` | 241 / 0 | 0 | + GPS_IFB5, Galileo_IFB7 | 0.096 0.034 | 0.354 0.402 0.434 |

Code offsets, hour 2 (stations with |mean| above 0.10 m, median |mean| in m; GPS by `scripts/clock_level/band5_offsets.py`, Galileo by the same computation on Galileo rows):

| arm | GPS band 1 / 2 / 5 | Galileo band 1 / 5 / 7 |
| --- | --- | --- |
| `ctl` | 0 / 1 / -; 0.023 / 0.018 / - | 1 / 2 / -; 0.024 / 0.020 / - |
| `t1on` | 0 / 0 / 1; 0.022 / 0.021 / 0.023 | 1 / 2 / -; 0.024 / 0.021 / - |
| `all` | 0 / 0 / 1; 0.022 / 0.021 / 0.023 | 2 / 2 / 0; 0.026 / 0.025 / 0.017 |

Post-fit phase median per band, both constellations pooled (`scripts/rapid_arm.py score --pf`): `ctl` L1 2.1, L2 1.2, L5 1.2 mm; `t1on` 2.4, 0.8, 1.2 mm; `all` 2.6, 0.8, 1.2 mm and L7 0.9 mm. The clock-datum dimension is 2, declared and realized. The phase-basis S-transform events grow with the number of phase signals (55, 71, 86), one per (satellite, signal), and are otherwise the same events. Warning types new against `t1on`: none at 2 h (the smoke's extra types, the prior-residual census's dispersion-branch note and an estimability listing, also appear on the 2 h `ctl` and `t1on` runs). The prior-residual census (a measurement only, wired to nothing) puts its worst |t| on a satellite code-bias row of the newest extra band on each arm (`t1on` G18 C5Q, `all` E13 C7Q, both at TOW 261150). Wall time is not compared: the section 10 arms ran four at a time and this one ran alone (409 s).

**Reading.**

- All tracking channels on the sim run stable on the current golden: every epoch emitted, none withheld, no completion, `Galileo_IFB7` estimated at 60 stations every epoch.
- Galileo E5b's code rows are clean (no station above 0.10 m, median 0.017 m). The GPS bands are unchanged from `t1on` to the millimetre. Galileo E1 and E5a medians rise by 2 to 4 mm against the control, with 2 stations above 0.10 m on each band (control 1 and 2): within the spread of a 2 h arm, recorded rather than read.
- The pooled band-1 phase residual grows with each added band (2.1, 2.4, 2.6 mm) while sigma0_pf falls; a third band adds redundancy per line of sight, so less of the band-1 phase noise is absorbed by the slant ionosphere and the ambiguities. A reading, not a measurement.
- The consumer moves by at most 0.003 m against `t1on`; at 2 h that is no verdict.

**Standing.** On the sim, the current golden carries every tracked channel (three bands on GPS and on Galileo) at posture `t1on` with no code change. What the sim does not test: the field's band-5 ignition event (sections 6 to 9; R-1d is the author's design decision), several channels per band (C1C with C1W, L2W with L2L, C5Q with C5X: the field's `channel_mode` question, absent from this fixture) and Galileo E5b on real receivers. L5 SEQUENCED.

## 12. 2026-10-23: the field, 2 h (GPS L5, Galileo E5b, and both)

Section 11's posture on `config/golden/golden_field_refound.json`, with one-change arms that separate the two added bands; same exe as sections 10 and 11; a 12-epoch smoke of each (`runs/_rap_l5f_<arm>_fsm`) preceded the 2 h arms `runs/_rap_l5f_<arm>_f2`, run four at a time. Every three-band arm carries `processing.frequency_mode` triple, `network.sat_gf_extra_band_free` and `network.estimate_rx_extra_band_code_delay`.

| arm | added band | emitted / withheld | WTZZ00DEU 3D p50, hours 1 and 2 (m) | GPS clock wire, hour 2, all / tracked (m) | decouples at TOW 260040 | MAJU00MHL GPS_DCB, GPS_IFB5 at 260010 and 260040 (m) | wall (s) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `ctl` | none | 240 / 0 | 0.058 0.011 | 0.0235 / 0.0208 | 1 | -1.03, -1.19; none | 463 |
| `t1on` | GPS L5 | 240 / 0 | 0.076 0.042 | 0.0929 / 0.0558 | 21 | -2.14, -9.21; +8.09, +25.88 | 627 |
| `e5b` | Galileo E5b | 240 / 0 | 0.056 0.011 | 0.0243 / 0.0197 | 1 | -0.99, -1.15; none | 691 |
| `all` | both | 240 / 0 | 0.071 0.038 | 0.0910 / 0.0555 | 21 | -2.03, -9.08; +7.80, +25.52 | 873 |

Clock wire: `scripts/ftc_step_attrib.py <run> --r3 --systems G,E`, row `e_clk`, within-arc RMS p50 (it grades the GPS pair L1W/L2W); the Galileo wire, `--systems E` (pair L1X/L5X), hour 2 p50 / p90: `ctl` 0.0210 / 0.0290 m, `e5b` 0.0215 / 0.0519 m, `all` 0.0249 / 0.0492 m; consumer-tracked p50 0.0127, 0.0106, 0.0083 m. Code offsets, hour 2 (stations with |mean| above 0.10 m; median |mean| in m):

| arm | GPS band 1 / 2 / 5 | Galileo band 1 / 5 / 7 |
| --- | --- | --- |
| `ctl` | 1 / 0 / -; 0.015 / 0.015 / - | 0 / 1 / -; 0.012 / 0.016 / - |
| `t1on` | 8 / 5 / 5; 0.049 / 0.029 / 0.032 | 0 / 1 / -; 0.015 / 0.015 / - |
| `e5b` | 1 / 0 / -; 0.015 / 0.014 / - | 0 / 4 / 3; 0.015 / 0.017 / 0.012 |
| `all` | 8 / 5 / 5; 0.048 / 0.030 / 0.033 | 0 / 4 / 3; 0.015 / 0.018 / 0.013 |

Every arm: clock-datum dimension 2, declared and realized; no S-basis completion. `Galileo_IFB7` is estimated at 57 of 58 stations (13 679 station-epochs), `GPS_IFB5` at 58. The IF time-difference slip tier refloats more with a third band, because it tests every band pair of a line of sight: 69 refloats on `ctl`, 114 on `t1on` (GPS pairs with band 5), 147 on `e5b` (the Galileo pairs with E5b: L1C+L7Q 26, L5Q+L7Q 23, L1X+L7X 12, L5X+L7X 11), 202 on `all`. Whether the added refloats are real slips is not graded here.

**Reading.**

- The section 6 event reproduces on the GPS + Galileo golden, unchanged: wherever GPS L5 enters the network, MAJU00MHL's receiver code states move along the station level direction at the G08 re-acquisition (DCB -2.1 to -9.2 m, band-5 delay +8.1 to +25.9 m, against -2.2 to -9.3 and +8.4 to +26.1 m on the GPS-only golden of section 5), 21 satellites decouple from the clock anchor, GPS bands 1, 2 and 5 carry station offsets (8, 5 and 5 stations), the GPS clock wire is four times the control's and WTZZ00DEU loses 0.018 and 0.031 m. Galileo does not move: its offsets, its wire and its receiver states are those of `ctl` on `t1on`.
- Galileo E5b alone is clean on the field at 2 h: WTZZ00DEU, the GPS wire and the GPS offsets equal the control's; the Galileo wire's median is unchanged (0.0215 against 0.0210 m) and its consumer-tracked median falls (0.0106 against 0.0127 m), with a wider upper tail (p90 0.052 against 0.029 m) and 3 to 4 stations with an E5a or E5b code offset above 0.10 m (control 1). MAJU00MHL's E5b receiver delay sits at -4.10 m, constant, with no event.
- `all` is the sum of the two: the GPS L5 event plus E5b's Galileo pattern.

**Standing.** On the field, Galileo E5b is a candidate (2 h only: its 6 h arm and the 16-site census with three-band Galileo consumers are owed before any ledger row), and GPS L5 is held by the event of sections 6 to 9 exactly as before the Galileo adoption. L5 SEQUENCED: R-1d remains the author's design decision, with section 9's two diagnostics as its first measurements. Several channels per band (`processing.channel_mode`) stay untested.

## 13. 2026-10-23: section 9's two diagnostics, a bisection of the event, and the R-1d options

**The build** (branch `card/l5b-diag`, commit `b75094d`, on main `8302fa1`; not merged; exe `gnss_processor_l5diag.exe`). (a) At an armed attribution epoch where the clock-anchor guard is about to withdraw, the anchored solve is captured and dumped with the existing readout to `logs/rhs_attrib/anchored/`. (b) `xop_<tow>.csv` applies the first armed epoch's operator to a later epoch's right-hand sides (and the reverse), mapped by column identity, with self-checks and per-station parts. Gates: products, consumer, epoch status and every existing `rhs_attrib` file byte-identical to the pre-change exe on the event window (`runs/_rap_l5d_{base,loc}_{fsm,dg}`); the anchored dump fires at TOW 260040 only, closures at most 1.9e-13; the operator self-checks at most 2.75e-8 relative; `nom_epoch_lint`, `nom_n4_verify --whole` and the Debug suite pass (3563 passed, 33 skipped for absent fixtures, 0 failed).

**(a) The anchored solve.** MAJU00MHL's states, TOW 260010, then 260040 anchored (before the withdrawal), then 260040 as emitted:

| state | 260010 | 260040 anchored | 260040 emitted |
| --- | --- | --- | --- |
| GPS_DCB | -4.234 | -12.189 | -11.298 |
| GPS_IFB5 | +8.093 | +27.915 | +25.878 |
| GPS_ISC_C1W | +1.166 | +16.031 | +14.373 |

The jump is in the anchored solve; the withdrawal is a consequence and takes back about a tenth of it. Without the withdrawal's fold (which nearly cancels the dense border carry's in section 9's frame), the change of MAJU00MHL's DCB from 260010 to the anchored 260040 (-7.95 m) splits into the dense border carry fold -4.81 m, MAJU00MHL's own net -2.43 m and the other 57 stations -0.72 m (IFB5 +12.24, +5.91, +1.66 m; ISC_C1W +9.52, +3.84, +1.51 m). These are differences of two decompositions at two operators: they locate the move, they do not separate data from operator.

**(b) The cross-epoch operator readout** computes what it states (self-checks above) but is not interpretable on this window: the 260040 right-hand sides under the 260010 operator give +3652, -8794 and -10285 m, the mirror about 1e8 m, spread over stations whose columns all map (MRO100AUS +2181 m). Each epoch's right-hand side is taken about its own linearization point and carries stiff carried-prior terms of order 1e7 to 1e8 that cancel only under their own operator, so a swap of operators does not isolate a data change.

**The raw data at MAJU00MHL / G08** (`MAJU00MHL_R_20241290000_01D_30S_MO.rnx`, code minus phase per band): L2W and C2W are absent at 259980, L1C and C1C at 259890 and 260010, C5Q and L5Q track throughout. L2W returns at 260040 with a step of about -11 m in code minus phase (a slip of about 45 cycles, LLI 0); code minus phase is continuous on L1C and L5Q across the gap, which excludes a dispersive slip on them but not a step common to their code and phase (see section 14, the IF-TD flag at 259980). The per-signal re-entry's verdicts (L2W shifted and refloated, L1C bridged, L5Q carried) agree with that, and section 6's reading of a common phase jump of L1C and L5Q is not supported. The +5 to +8 m code residuals of every G08 code at MAJU00MHL after 260040 are made by the solution, not by the data. A first, slower move precedes the gap: MAJU00MHL's DCB goes from -0.08 to -2.20 m and IFB5 from +2.40 to +8.25 m between 259650 and 259890 (G08 at 18 to 19 degrees, code residuals within about 1 m).

**The bisection** (16 min event window on the field `t1on` posture, exe `gnss_processor_l5s0.exe`; RINEX edits at MAJU00MHL / G08 only; first-epoch rows unchanged at every station except where the edit removes them):

| arm | the one change | decouples at 260040 | MAJU00MHL DCB, 260010 to 260040 (m) | IFB5 (m) |
| --- | --- | --- | --- | --- |
| `l5d_base_fsm` | none | 21 | -2.14 to -9.21 | +8.09 to +25.88 |
| `l5d_nob5only_fsm`, `_b5phonly_`, `_b5conly_` | band 5, its code or its phase cut at 260010 | 20 | -2.14 to -8.94 | +8.09 to +25.12 |
| `l5d_gapall_fsm` | every G08 signal cut at 259980 and 260010 | 19 | -2.14 to -8.75 | +8.08 to +24.59 |
| `l5d_fresh40_fsm` | G08 cut from the start to 260010 (first seen at 260040) | 0 | 0.00 to 0.00 | +2.15 to +2.14 |
| `l5d_losreentry_fsm` | `network.cycle_slip.gap_reentry_per_signal` false | 21 | -1.48 to -2.32 | +6.27 to +8.15 |

Readings: the band-5-only epoch is not the trigger (a single-band line of sight is not used; the three cuts at 260010 are identical to each other). A common gap of every signal keeps the event. Without G08's earlier arc at MAJU00MHL neither move happens and G08's birth at 260040 is clean (that arm also removes the pre-gap move, so it is not a one-change arm on the re-entry). With the per-signal re-entry off (no gap response; the wide-lane detector refloats L2W and L5Q, the post-fit tier the arc) MAJU00MHL's jump shrinks about tenfold, but the 21-satellite clock-anchor burst stays: the burst and the station's level jump are separable, and the decouple count alone is not the event's grade.

**The R-1d options (the author's decision).**

1. **Find the defect in the per-signal refloat under three bands.** A correct refloat of L2W alone, with L1C and L5Q carried, should leave the line of sight determined; a metres-sized move of the station level points at the refloat's re-coupling edge (standing invariant 1: a re-seed from one epoch of low-elevation code made stiff). First read: MAJU00MHL's local fold and row parts for G08 in the anchored dump, 260040 against 260010. A fix, if found, keeps every carry the raw data say is good.
2. **Restart the whole line of sight at a shifted re-entry when it has three bands** (what the dual posture does by construction). Supported by `l5d_losreentry_fsm`; costs the continuous carries of L1C and L5Q; must keep card GRS's per-signal gain on dual.
3. **A station-level event response** (standing invariant 7): a receiver code state that moves far beyond its formal sigma in one epoch is an event; the epoch is re-solved with the line of sight that changed state refloated whole or held out, and the move is not carried. Generic; does not address the slower pre-gap move.
4. **Band-5 admission maturity:** a line of sight's band-5 rows enter only after N epochs of continuous three-band tracking and once the station's level states are determined. Blunt; costs band 5 during ignition and after every gap.
5. **Sequencing:** Galileo E5b's 6 h arm and census proceed (section 12) while R-1d is designed; GPS L5 stays SEQUENCED.

Owed whatever the choice: the 21-satellite clock-anchor burst at 260040 needs its own read (which satellites, which tension), since it survives option 2's emulation; and any option is graded at 2 h on bands 1 and 2 offsets, the clock wire and WTZZ00DEU, not on the decouple count. The cross-epoch readout (b) is not recommended for merging; the anchored dump (a) is.

## 14. 2026-10-23: option 1, the read of MAJU00MHL's parts and the per-signal-off arm at 2 h

The author chose option 1. Reads of the anchored dump (`runs/_rap_l5d_loc_dg/logs/rhs_attrib/`), the run traces and the code; one 2 h arm.

**What re-enters at 260040.** All three G08 phase rows at MAJU00MHL have a zero prefit at 260040 (the anchored and the emitted solve alike): every G08 phase ambiguity there is re-seeded and the line of sight is code-only at that epoch. L2W is refloated by the gap tier (the real slip). L5Q is refloated by the wide-lane pair detector (`mw_wl_slip`, pair L2W+L5Q), which frees both members of a flagged pair (`SlipQc.cpp`, the `refloat_signal` calls of the pair loop) unless the common-band scope has a narrowed verdict; that verdict (L2W implicated, L1C and L5Q exonerated) is first written at 260070. L5Q's code minus phase is continuous, so its refloat at 260040 is collateral on the wide-lane evidence. L1C is bridged at its prefit. "Refloating L2W alone" is therefore not what happens on this posture.

**Where MAJU00MHL's states come from.** The station's own rows and folds hardly carry its receiver code states, before or during the event; the shared-solution feedback does:

| target, record | 260010 | 260040 anchored |
| --- | --- | --- |
| GPS_DCB: own rows / own folds / feedback / solved | -0.09 / -2.80 / -1.34 / -4.23 | -0.09 / -2.85 / -9.25 / -12.19 |
| GPS_IFB5: own rows / own folds / feedback / solved | -0.01 / +0.43 / +7.67 / +8.09 | -0.02 / +0.21 / +27.73 / +27.91 |

Within MAJU00MHL's net, the G08 line of sight goes from -1.05 to -15.07 m on GPS_DCB (+3.56 to +37.95 m on GPS_IFB5), offset by its other satellites (G26 +4.24, E26 +2.65, G31 +2.56 m on GPS_DCB). The station's data-only block has a null space of dimension 2 (eigenvalues -1.9e-11 and -4.0e-12 of 1.0e7), and the run log names the receiver code-bias datum as the only closure of the station's geometry-free level once the band-5 delay is estimated. That datum's zero-mean prior is built for the DCB form only (`RxCodeBiasCarry.cpp`, `cb.form == ParamForm::Dcb`): the estimated band-5 delay and the second code channel's offset get no datum row. A level direction that loads mainly on GPS_IFB5 is then held by the shared feedback and the in-block carry alone, which is consistent with section 8's probe (the DCB prior tightened to 1 m left the event in place). Not yet established: the loadings of the two null directions, and whether a datum row on the band-5 delay holds the event.

**Excluded.** G08's band-5 satellite code bias is not the path: 12 to 13 stations hold G08 on three bands at every epoch from 259800 to 260130 (MAJU00MHL drops out at 259890, 259980 and 260010 only), and its formal sigma stays at 0.07 m; no extra-band datum switch occurs.

**The per-signal re-entry off at 2 h** (`runs/_rap_l5f_t1onps0_f2`, `t1on` plus `network.cycle_slip.gap_reentry_per_signal` false, same exe):

| arm | WTZZ00DEU, hours 1 and 2 (m) | GPS clock wire, hour 2, all / tracked (m) | GPS stations above 0.10 m, bands 1 / 2 / 5 | decouples (at 260040) |
| --- | --- | --- | --- | --- |
| `ctl` | 0.058 0.011 | 0.0235 / 0.0208 | 1 / 0 / - | 2 (1) |
| `t1on` | 0.076 0.042 | 0.0929 / 0.0558 | 8 / 5 / 5 | 25 (21) |
| `t1onps0` | 0.063 0.020 | 0.0741 / 0.0675 | 5 / 3 / 4 | 37 (21; 12 more at 261030) |

It halves the consumer's hour-2 loss and some of the offsets, not the wire, and adds a second burst; it is not the fix.

**The clock-anchor burst.** At 260040 the dual control decouples G08 alone (phase statistic, e_phi 1.99 m): MAJU00MHL was G08's phase-datum anchor (on `t1on` the G08 L5Q anchor is handed to NYA200NOR at 259980). On `t1on` 21 GPS satellites trip together with a median clock move of 3.73 m against their anchors, on `t1onps0` the same 21 with 0.24 m: a constellation-common GPS clock move that scales with the size of MAJU00MHL's level jump.

**Reading.** On this posture a three-band station's receiver code states are set by the network, not by its own data, along directions only the DCB part of the datum touches; when the operator changes at a re-entry (here a line of sight turned code-only, with a collateral refloat), the feedback moves those states by metres and the network's GPS clock level follows. Two defects are named for the build: (1) the receiver code-bias datum covers the DCB only, so the band-5 delay (and the second channel's offset) has no datum row; (2) the wide-lane pair detector refloats both members of a flagged pair before the common-band scope can exonerate one. Next measurements: the loadings of MAJU00MHL's two null directions; then one default-off build of a datum row on the estimated extra-band delay, graded at 2 h like this section.

**Addendum (same day): the common-band scope at the event.** The wide-lane detector forms every band pair and the common-band scope (`network.cycle_slip.common_band_scope`, armed) frees only the implicated band. On G08 at MAJU00MHL it forms no verdict at TOW 260040 (no `T0_band_scope` row; the scope writes one per verdict), so the L2W+L5Q drain frees both bands; its first verdict is at 260070: L2W implicated, L1C and L5Q exonerated by a silent, mature L1C+L5Q pair (23 samples), which was mature at 260040 as well. Why no verdict was formed at the re-acquisition epoch is not yet read in the code. Earlier, at TOW 259980, with L2W already absent, the IF time-difference test flagged the L1C+L5Q pair by 6.37 m and refloated both (one pair, no isolation possible); code minus phase on both is continuous there, so the step, if real, is common to code and phase. These are two slip-layer findings beside the network's fragility; the bisection's common-gap arm (section 13), where every G08 signal re-enters together and is refloated, keeps the event, so neither is its whole cause.

## 15. 2026-11-09: the freed arc's variance floor at the event (pre-registered; run under card [SMX2](SMX2_scenario_matrix_current_golden_2026-11-09.md), stage D)

The author's ruling of 2026-11-09 ("Close ALD, and test one thing before choosing"): step R-1d is not chosen until this arm is read. **Hypothesis (untested on three bands):** the event at TOW 260040 (G08 re-enters at MAJU00MHL, L2W stepped about -11 m, section 13) is the freed arc's stale level, which cards ICL6 and SLV traced on the dual posture pushing receiver code biases by metres; the floor `network.amb_float_drop_var_m2` 100 removes that push at its source.

**Arms.** `scripts/rapid_arm.py make --base config/golden/golden_field_refound.json --hours 0.27` (33 epochs, TOW 259200 to 260160) on today's golden (main `27d6ece`), exe `gnss_processor_smx2.exe`: `smx2_d_t1on_ev`, the `t1on` keys of card ALD section 6 (the control, re-run: ALD's predates the keys armed on 2026-11-07); `smx2_d_t1onA_ev`, the same plus the floor.

**Bar (frozen; card ALD section 5, gate 4).** MAJU00MHL's GPS_DCB, GPS_IFB5 and GPS_ISC_C1W move from TOW 260010 to 260040 each by at most a tenth of the control's, with at most 3 clock-anchor decouples at 260040. Every station whose GPS code biases step above 0.5 m is listed for both arms; a redistribution to other stations is not a pass. If the floor passes: the 2 h arms of ALD's gate (5) (dual control, dual plus the floor, `t1on`, `t1on` plus the floor), with the dual bar and the three-band bars of card R1D (B). If it fails: reported as failed, no further build. Recorded, not fixed: L5Q is freed collaterally at 260040 (section 14 addendum).

**Result of the windows (2026-11-09): the floor passes gate (4).** Both windows exit 0 with 33 of 33 epochs. MAJU00MHL from TOW 260010 to 260040: control GPS_DCB -3.54 m, GPS_IFB5 +9.12 m, GPS_ISC_C1W +6.22 m, 14 clock-anchor decouples at 260040 (ALD's control, before the 2026-11-07 keys: -7.02, +17.66, +13.13 m, 21); with the floor -0.10, +0.26, +0.15 m (ratios 0.03, 0.03, 0.02; bar 0.10) and 0 decouples (bar 3). No redistribution: on the control the GPS code biases step above 0.5 m at 11 (DCB), 16 (IFB5), 4 (ISC_C1C) and 6 (ISC_C1W) stations; with the floor only LCK400IND's IFB5 does (-1.21 m). The hypothesis holds at the event window; the 2 h arms of gate (5) follow (station lists and the 2 h grade: card SMX2 section 5.3).
