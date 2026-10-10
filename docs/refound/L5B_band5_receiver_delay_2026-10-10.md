# L5B: the receiver band-5 code delay as a state of the network solve (L5 return plan step R-1b)

**STATUS**

- **id:** L5B
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered. Card L5R found that the receiver's band-5 code delay (`GPS_IFB5`) is dropped from the network solve and held at a mis-scaled first-epoch seed whenever the satellite code biases are estimated, leaving station-constant band-5 code misfits of metres on the field. This card estimates and carries it behind a default-off knob and grades it by L5R's P3 first, then by L5R's bars against a same-exe control on the current goldens.
- **source:** [L5R](L5R_l5_return_on_the_exact_update_2026-10-08.md) section 4 (the root and step R-1b); [L5P](L5P_l5_phase_posture_2026-09-21.md) section 4 (the return plan); [B5D](B5D_band5_code_datum_2026-09-20.md) (the band-5 satellite code datum); [NOM](NOM_network_object_model_2026-10-08.md) step N1 (the receiver IFB drop as a declared rule, treatment `HeldAtSeed`); the author's directive that L5 is never closed
- **knobs / posture:** new `network.estimate_rx_extra_band_code_delay` (default false); the goldens of 2026-10-09 (`03461e4`, card ICF's cold-start bound armed) plus `processing.frequency_mode` triple, the GPS band-5 types appended to `processing.signal_priorities.GPS` (the list of `runs/_rap_l5r_t0_f6`), and per arm `network.sat_gf_extra_band_free`
- **dates in the text:** 2026-10-10
- **hashes / run dirs:** controls `runs/_rap_l5g_ctl_{f6,s6}` and census key `l5c_ctl` (exe `gnss_processor_l5g0.exe`, main `47fb634`, sha256 `31081ec3b36875a1`; 2 h identity to `runs/_rap_g15_{f2,s2}` verified); arms `runs/_rap_l5b_*`; census keys `l5c_b*`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registered; no result yet.

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
