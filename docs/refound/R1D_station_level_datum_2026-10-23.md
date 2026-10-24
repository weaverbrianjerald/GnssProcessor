# R1D: the station level under three bands (L5 return plan step R-1d)

**STATUS**

- **id:** R1D
- **topic:** network-vehicle
- **status:** open
- **summary:** The successor of card L5B for step R-1d. First build (2026-10-23): a default-off datum row on the receiver band-5 delay and the second code channel's offset, beside the DCB's. At 10 m it does not hold MAJU00MHL's level and every bar fails; at 1 m it shrinks the move by a fifth but asserts each station's second-channel offset near zero (C1W 0.95 m at the median station) and fails the bars. The datum row is not the fix (as predicted at pre-registration). The coupling read (section 7): the level move is carried by the station's carried ambiguity rows through the phase biases of satellites it anchors. The discriminator (section 9): with MAJU00MHL barred from anchoring the event vanishes (station move 2 %, 0 decouples) and the three-band wire is as clean as dual (clock wire 0.91 times); the anchor loop is the transmission. The station-level projection of the coupling fails twice: it breaks the gauge (section 11), and the weighted, gauge-preserving form (sections 12 and 13) costs the dual network's clock wire 40 % and still fails the event. A re-seed handover rule (section 15) moves the role away and the event still comes: the datum keeps the anchor's realization. Continued in card [ALD](ALD_anchor_level_datum_2026-10-23.md) (an anchor's level held as part of the datum). L5 SEQUENCED.
- **source:** [L5B](L5B_band5_receiver_delay_2026-10-10.md) sections 13 and 14 (the anchored solve, the bisection, the station's parts, the per-signal-off arm); the author's choice of option 1 on 2026-10-23 and of this build the same day
- **knobs / posture:** new `network.rx_code_bias_datum_extra_forms` (bool, default false) and `network.rx_code_bias_datum_extra_sigma_m` (default 10.0); arms on the field golden plus card L5B's `t1on` keys (`processing.frequency_mode` triple, the GPS band-5 types appended to `processing.signal_priorities.GPS`, `network.sat_gf_extra_band_free`, `network.estimate_rx_extra_band_code_delay`)
- **dates in the text:** 2026-10-23
- **hashes / run dirs:** branch `card/r1d` commit `27520ca` (not merged), exe `gnss_processor_r1d.exe` (sha256[:16] `eb67a26758b09b82`); arms `runs/_rap_r1d_{ctl,t1on,x10,x1}_f2`, `runs/_rap_r1d_x10s_s2`, event windows `runs/_rap_r1d_*_{ev,evb}`; loadings run `runs/_rap_l5d_loc_nd` (branch `card/l5b-diag`, `b7f58ba`)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Question: does a zero-mean datum row on the receiver band-5 delay and the second channel's offset hold a three-band station's level at the event of card L5B? No. MAJU00MHL's GPS level loads 2.8 times on the band-5 delay and 1.5 times on the second-channel offset against the DCB, so the DCB-only datum barely holds it; but the row at 10 m changes nothing, and at 1 m it shrinks the move by a fifth while forcing metre-level offsets into the C1W code rows. Every bar missed at both sigmas, as predicted. Sections 7 and 9 then name the transmission: the phase-datum anchor loop (barring MAJU00MHL from anchoring removes the event and leaves a three-band wire as clean as the dual one). The coupling projection (sections 10 to 13) is set aside (gauge break, then a 40 % dual clock-wire cost). A re-seed handover (section 15) does not undo it either. Current standing: L5 SEQUENCED; next, the anchor's level held as part of the phase datum, derived first (section 15).

## 1. The question

Card L5B section 14 found that MAJU00MHL's receiver code states are set by the shared-solution feedback, not by its own data, and that the receiver code-bias datum (`network.rx_code_bias_datum`, armed in both goldens) builds its zero-mean prior for the DCB form only (`RxCodeBiasCarry.cpp`, `cb.form == ParamForm::Dcb`). The estimated band-5 delay (`GPS_IFB5`) and the second code channel's offset (`GPS_ISC_C1W`) get no datum row.

The loadings, read 2026-10-23 with the multi-dimensional null-space readout (branch `card/l5b-diag`, commit `b7f58ba`, run `runs/_rap_l5d_loc_nd`): MAJU00MHL's data-only local normals have two null directions, one per constellation.

| direction | DCB share | receiver code-bias loadings | slant ionospheres, ambiguities (mean loadings) |
| --- | --- | --- | --- |
| GPS level | 0.0074 | GPS_DCB +0.086, GPS_ISC_C1W -0.133, GPS_IFB5 -0.239 | IONO G +0.133; AMB G1 +0.133, G2 +0.220, G5 +0.239 |
| Galileo level | 0.039 | Galileo_DCB -0.197 | IONO E -0.248; AMB E1 -0.248, E5 -0.445 |

(TOW 260010; the anchored 260040 values agree to the second digit.) The DCB share is the squared norm of a unit direction's DCB loadings. At the armed sigma of 10 m the DCB prior holds the unit GPS level direction to about 116 m; the band-5 delay loads 2.8 times and the second channel's offset 1.5 times as much as the DCB. The question: with a datum row on those two forms as well, is MAJU00MHL's level held at the G08 re-acquisition, and does the three-band wire come back to the dual control?

## 2. The build

`network.rx_code_bias_datum_extra_forms` (default false): with `network.rx_code_bias_datum` armed, every estimated receiver code-bias column of the forms `Ifb` (not the GLONASS slot form) and `Isc` also gets the zero-mean prior, at `network.rx_code_bias_datum_extra_sigma_m` (default 10.0 m), through the same channel as the DCB's (the pre-PD fold under `network.station_prior_before_pd`). Off, every output is byte-identical. The run-end report counts the extra rows by form.

## 3. The arms

One exe built from the card's branch, `scripts/rapid_arm.py make --base config/golden/golden_field_refound.json`, 2 h from the golden's start (the event is at TOW 260040, minute 14).

| tag | change against the golden | role |
| --- | --- | --- |
| `r1d_ctl_f2` | none | the dual control |
| `r1d_t1on_f2` | the `t1on` keys | the three-band control |
| `r1d_x10_f2` | `t1on` + `network.rx_code_bias_datum_extra_forms` | the build at 10 m |
| `r1d_x1_f2` | `t1on` + the knob + `network.rx_code_bias_datum_extra_sigma_m` 1.0 | the build at 1 m |
| `r1d_x10_s2` | sim golden + `t1on` keys + the knob, from 00:30:15 | no harm on the sim (against `runs/_rap_l5s_t1on_s2`) |

## 4. Gates, bars and the reading (frozen)

- **G0 (identity).** Knob off, the new exe's `r1d_ctl_f2` and `r1d_t1on_f2` identical over 240 epochs (`scripts/clock_level/ident2h.py`) to the same configurations on the build's base commit without the change.
- **P0 (structure).** Every arm exit 0, 240 of 240 emitted, none withheld, clock-datum dimension 2 declared and realized, no S-basis completion.
- **P1 (the ON telemetry).** The run-end report shows extra datum rows of both forms at every station that estimates them, every epoch.
- **M (the mechanism).** MAJU00MHL's GPS_DCB and GPS_IFB5 change from TOW 260010 to 260040 (`r1d_t1on_f2`: about -7.1 and +17.8 m) and the clock-anchor decouples at 260040 (21).
- **B (the bars, against `r1d_ctl_f2`).** WTZZ00DEU 3D p50 in hours 1 and 2 within 0.03 m; the GPS clock wire (`scripts/ftc_step_attrib.py <run> --r3 --systems G,E`, row `e_clk`, hour 2, all and tracked satellites) at most 1.10 times; GPS bands 1 and 2 code offsets (stations with |mean| above 0.10 m, hour 2) at most one more than the control; the sim arm's SZZ000SIM hourly p50 within 0.01 m of `runs/_rap_l5s_t1on_s2`.

**Prediction, frozen before any run.** Information along a unit level direction from a zero-mean prior is the sum over the prior's columns of the squared loading over sigma squared. At 10 m the row raises it from 0.0074/100 to (0.0074 + 0.0572 + 0.0178)/100, about eleven times, which is still below card L5B section 8's probe (the DCB prior at 1 m, 0.0074) that did not hold the event. At 1 m it is about eleven times that probe. The expected result: `r1d_x10_f2` does not hold MAJU00MHL's level (M about as `r1d_t1on_f2`, the bars fail); `r1d_x1_f2` holds it at least partly.

**Reading.**

- `r1d_x10_f2` passing M and B: the missing datum row was the defect; the row is a candidate (ledger row UNVALIDATED until a 6 h census).
- Only `r1d_x1_f2` passing: the row holds by asserting each station's band-5 delay and second-channel offset near zero at 1 m, which is wrong where those delays are metres (card L5B section 1); read `scripts/clock_level/band5_offsets.py` station by station before calling it a fix. The push itself then comes from elsewhere.
- Neither passing M: the push along the level direction comes from the carried priors that couple a station's ambiguities to the shared columns (data rows cannot move a null direction; the dense border carry fold was 60 % of the move in L5B section 13), and that coupling is option 1's next read; the datum row is not the fix.

Whatever passes, L5 is not adopted by this card. A missed bar is reported as L5 SEQUENCED with the next step named.

<!-- Results are appended below this line under dated headings. -->

## 5. 2026-10-23: result (every number read by the lead from `runs/`)

Build: branch `card/r1d`, commit `27520ca` on main `f45070e`, not merged; exe `gnss_processor_r1d.exe` (sha256[:16] `eb67a26758b09b82`), base exe `gnss_processor_r1d_base.exe`. Unit suite 3577 passed, 33 skipped (fixtures absent in the worktree), 0 failed, including 8 new cases; `nom_epoch_lint`, `nom_n4_verify --whole`, `counters_split_check`, `make_golden_config --verify` (goldens unchanged) and `config_posture_check --verify-loader` pass. The keys are fingerprinted like the DCB datum (token `rxcbdatumx:<sigma>`, only when armed); the ledger rows (UNVALIDATED, PARAM) exist on the branch only.

| gate | measured | verdict |
| --- | --- | --- |
| G0 | the new exe's `r1d_ctl_f2` and `r1d_t1on_f2` identical over 240 epochs to `runs/_rap_l5f_ctl_f2` and `_rap_l5f_t1on_f2` (exe `gnss_processor_l5s0.exe`, main `04517ca`); the 16 min windows identical to the base exe | PASS |
| P0 | every arm exit 0, 240 of 240 emitted (sim 241 of 241), none withheld | PASS |
| P1 | `r1d_x10_f2`: 13 918 rows on the band-5 delay (58 stations) and 9 599 on second-channel offsets (GPS_ISC_C1W at 22 stations, GPS_ISC_C1C at 18) over the run; none on the controls | PASS |

The bars (hour 2 for the wire and the offsets; offsets by `scripts/clock_level/band5_offsets.py`'s statistic: stations with |mean| above 0.10 m, median |mean| in brackets):

| arm | WTZZ00DEU, hours 1 and 2 (m) | GPS clock wire, all / tracked (m) | GPS band 1 / 2 / 5 | decouples (at 260040) | MAJU00MHL, 260010 to 260040: GPS_DCB / GPS_IFB5 (m) |
| --- | --- | --- | --- | --- | --- |
| `r1d_ctl_f2` | 0.058 0.011 | 0.0235 / 0.0208 | 1 (0.015) / 0 (0.015) / - | 2 (1) | -0.16 / - |
| `r1d_t1on_f2` | 0.076 0.042 | 0.0929 / 0.0558 | 8 (0.049) / 5 (0.029) / 5 (0.032) | 25 (21) | -7.06 / +17.78 |
| `r1d_x10_f2` | 0.076 0.042 | 0.1055 / 0.0653 | 10 (0.052) / 5 (0.031) / 6 (0.031) | 24 (20) | -7.14 / +18.00 |
| `r1d_x1_f2` | 0.071 0.031 | 0.0667 / 0.0659 | 46 (0.583) / 13 (0.048) / 11 (0.048) | 38 (20) | -6.27 / +14.63 |

The sim arm `r1d_x10s_s2`: SZZ000SIM 0.099 0.035 m by hour, as `runs/_rap_l5s_t1on_s2` (no harm). On `r1d_x1_f2` the band-1 code offset sits in the second channel: median |mean| per station C1W 0.95 m (C1C 0.112 m), against 0.026 and 0.082 m on `r1d_t1on_f2`; the 1 m row holds each station's second-channel offset near zero where it is about a metre. The 16 min windows of the build agent (`runs/_rap_r1d_*_ev`) also show two decouple bursts before the event on the 1 m arm (TOW 259830, 7 satellites; 259890, 3).

**Reading (the frozen rules).** The prediction holds. At 10 m the row does not hold MAJU00MHL's level (M as `r1d_t1on_f2`) and every bar fails, the clock wire slightly worse. At 1 m the level move shrinks by about a fifth and the wire's all-satellite figure improves, but the row asserts every station's second-channel offset and band-5 delay near zero, which puts 0.95 m into the C1W rows at the median station, adds decouple bursts and fails the offset and wire bars. Neither arm passes M: the datum row is not the fix. A zero-mean prior cannot hold a direction that the network pushes with more information than a physically admissible prior carries.

**Standing.** L5 SEQUENCED. Data rows cannot move a station's null direction (its rows are blind to it), so the push on MAJU00MHL's GPS level comes through the carried priors that couple its ambiguities to shared columns (the dense border carry fold was 60 % of the move in card L5B section 13). Next, on this card: read that coupling at the event (which carried ambiguity priors of MAJU00MHL, coupled to which shared columns, move the level when G08's three ambiguities are re-seeded), then decide the response. The build of this section is not recommended for merging (a knob that fails its card); the branch `card/r1d` keeps it as the record.

## 6. 2026-10-23: pre-registration of the coupling read

**Question** (the author's direction after section 5): which carried ambiguity priors of MAJU00MHL, coupled to which shared columns, move its GPS level at the G08 re-acquisition?

**Instrument** (diagnostics only, on branch `card/l5b-diag`). At each armed attribution dump with a `LOC:` label (and at the anchored pre-withdrawal dump), the station's whole local block is written: every row of `A_r` and `A_s` with its weight, prefit and contribution kind (data rows, the carried ambiguities' coupling rows, the other pseudo-rows), both prior channels, the local column names, the solved shared state `x_s` by label and the solved local state. An offline script reconstructs the local solve at the solved `x_s` (closure against the dumped solution) and decomposes the GPS level coordinate `l = c^T x_r`, `c` the unit GPS level direction of the data-only null space (section 1), exactly: one term per row, `w (g^T a_r) (y - a_s^T x_s)` with `g = N_rr^{-1} c`, and one per prior entry, `g_j eta_j`. Coupling rows are further split into the carried value `w (g^T a_r) y` and one term per shared column `-w (g^T a_r) M_j x_s,j`; groups are (kind, satellite, signal) and, for the shared side, the column's satellite and family. The change from TOW 260010 to the anchored 260040 is split, row by matched row, into the gain change `Delta(w g^T a_r) v` and the residual change `w g^T a_r Delta v`.

**Reading (frozen).** A read, not a gate. Per-column shares of `x_s` depend on the datum and can cancel: only satellite groups and whole-row terms are read, with the closure. If the change of `l` is carried by MAJU00MHL's carried GPS ambiguity rows through their shared terms, the satellites and column families named are the coupling the response must treat at a re-seed; if it is carried by the gain change (G08's rows leaving the block), the response is about the information the re-seed removes from the level; if neither, the read names what does.

## 7. 2026-10-23: the coupling read (result; every number re-run by the lead)

**Instrument.** Branch `card/l5b-diag`, commit `92785d4` (parent `b7f58ba`), exe `gnss_processor_l5blk.exe` (sha256[:16] `6026b236fccb16c1`): beside each `loc_<tow>.csv` the named station's whole local block is written as `blk_<tow>_<station>.csv` (rows of `A_r` and `A_s` with weight, prefit and contribution kind; both prior channels; the shared state `x_s`; the local solution `XR`, an increment around the station's linearization point). Products, consumer and epoch status identical to the instrument's base run; every existing `rhs_attrib` file byte-identical (`runs/_rap_l5d_loc_nd` against `runs/_rap_l5d_loc_blk`); unit suite 3564 passed, 33 skipped (fixtures absent), 0 failed; the two NOM lints pass. The offline read is the session script `blk_read.py`.

**Closures.** The local normals rebuilt from the file agree with the kernel's to 4e-16 (scale-free); the local solution at the solved `x_s` to 2e-7 m on 54 m; the decomposition of `l = c^T x_r` sums to `l` within 4e-11 m; the change split within 1e-14. The GPS level direction `c` reproduces section 1 (DCB share 0.0074, loadings DCB +0.0863, ISC_C1W -0.1334, IFB5 -0.2392). One `c` (from TOW 260010) serves both epochs; at the anchored 260040 the station's own `c` lies 14.4 degrees away (the new G08 columns).

**Where `l` comes from**, MAJU00MHL, TOW 260010 against the anchored 260040 (m): `l` -89.178 to -111.098, change -21.921.

| kind | 260010 | anchored 260040 | change | of which: gain | carried value | shared state | rows entering |
| --- | --- | --- | --- | --- | --- | --- | --- |
| carried ambiguities (`block_ambiguity_carry`) | -61.010 | -81.668 | -20.658 | +13.725 | +4.152 | -14.587 | -23.949 |
| carried receiver code biases (`block_rx_cb_carry`) | -1.130 | -7.930 | -6.800 | +0.026 | -0.090 | -6.735 | 0 |
| phase data | +15.989 | +21.319 | +5.331 | -10.023 | +0.166 | -5.166 | +20.354 |
| ambiguity pins | -43.279 | -43.159 | +0.120 | +0.120 | 0 | 0 | 0 |
| code data (both channels) | +0.253 | +0.340 | +0.087 | | | | |
| receiver DCB datum prior | -0.000009 | -0.000009 | 0 | | | | |

The entering rows are G08's new L1C and L2W ambiguity carries (-23.95), almost cancelled by G08's entering phase rows (+20.35); no row leaves. Over all rows the change is gain +3.84, carried value +4.23, shared state -26.38, entering rows -3.61. The carried ambiguity rows' shared part moves from -46.72 to -70.80 m (-24.08); by the satellite of the shared column: G26 -9.45, G08 -7.02, G16 -5.19, G21 -3.97, G10 -2.75, against G23 +2.29 and G22 +1.85; by family PB -28.50, CLK +4.42, CB +0.00. The receiver code-bias block's shared change (-6.74) is phase biases as well. Per-column and per-family shares are shares of a datum-dependent `x_s` and are read only in aggregate; the per-row split of a whitened block depends on its ordering, so only block totals, the shared part by column and the station's columns grouped by satellite are read.

**MAJU00MHL is a phase-datum anchor.** At both epochs its block holds the phase-datum pin rows of G01 (L1C, L2W), G10 (L1C, L2W, L5Q), G16 (L1C, L2W) and G26 (L1C, L2W, L5Q); it handed G23 to three stations at TOW 259920 and G08's L5Q to NYA200NOR at 259980. Three of the five satellites whose shared columns push the level hardest (G26, G16, G10) are anchored at MAJU00MHL, and G08 was until 259980.

**Reading (the frozen rules).** The receiver DCB datum holds nothing at fixed `x_s` (its term is -9e-6 m). The level coordinate sits on the ambiguity pins, the carried ambiguities and the phase rows, and its change at the event is carried by the carried ambiguity rows (and the carried receiver code biases) through their shared terms, not by the gain change of G08's re-seed: the shared state the station's carries are coupled to moves, mostly on the phase-bias columns of G26, G08, G16, G21 and G10, and the coupling hands that move to the level. That the satellites named are largely the ones MAJU00MHL anchors suggests a loop between the anchor station's level and the phase biases its pins define; a reading, not yet a measurement. Between the anchored and the emitted 260040 the block is identical and only `x_s` differs (up to 1.87 m, mostly clocks): the withdrawal moves `l` by +3.0 m through the shared state alone.

**Standing.** L5 SEQUENCED. Two next steps, both builds. (1) The discriminator: a diagnostic knob that bars a named station from the phase-datum anchor election; the event window and the 2 h field arm with MAJU00MHL barred say whether the anchor loop is the transmission. (2) The response candidate: the coupling rows of a station's carried ambiguities stop handing shared moves into the station's data-null level directions (a station-level analogue of the coupling's gauge projection), so the level evolves only by its own carried information.

## 8. 2026-10-23: pre-registration of the anchor-bar discriminator

**Question** (the author's choice after section 7): is the phase-datum anchor role of MAJU00MHL the path by which the shared phase-bias move reaches its GPS level at the G08 re-acquisition?

**Build.** `network.phase_datum_anchor_bar_stations` (list of station names, default empty): a named station is never elected phase-datum anchor; the election takes the next candidate it would have taken. Diagnostics, default off, byte-identical when empty; a run-end count of the barred candidacies.

**Arms** (one exe; 2 h from the field golden's start; graded like section 5; the controls `r1d_ctl_f2` and `r1d_t1on_f2` of section 5 if the new exe reproduces them, else re-run on it):

| tag | change against the golden | role |
| --- | --- | --- |
| `bar_ctl_f2` | MAJU00MHL barred | the bar's own effect on the dual network |
| `bar_t1on_f2` | `t1on` keys + MAJU00MHL barred | the discriminator |

**Gates.** G0: the empty knob identical to the base (16 min windows and the controls). P0: exit 0, 240 of 240 emitted, none withheld. P1: MAJU00MHL holds no phase-datum pin row at any epoch (`network_events.csv` shows no anchor election or handover to it) and the barred-candidacy count is nonzero.

**Reading (frozen).**

- `bar_t1on_f2` passing M (MAJU00MHL's GPS_DCB and GPS_IFB5 change from TOW 260010 to 260040 within a tenth of `r1d_t1on_f2`'s, and at most 3 decouples at 260040) and the bars of section 4 against `bar_ctl_f2`: the anchor loop is the transmission; the response is a matter of the anchor role (election, or the coupling of an anchor's carries to the phase biases it defines).
- `bar_t1on_f2` passing M but not the bars: the anchor loop carries MAJU00MHL's event, and another path remains; read where before any response.
- `bar_t1on_f2` failing M: the anchor role is not the path; the coupling of a station's carries to the shared columns is, whoever anchors, and the station-level projection of section 7 is next.
- `bar_ctl_f2` must stay within 0.01 m of `r1d_ctl_f2` on WTZZ00DEU by hour and within 1.10 times on the clock wire, or the bar itself is a confound to be read first.

## 9. 2026-10-23: the anchor-bar discriminator (result; every number read by the lead from `runs/`)

**Build.** Branch `card/r1d-anchorbar`, commit `6edfff2` on main `f45070e`, not merged; exe `gnss_processor_anchorbar.exe` (sha256[:16] `f7b10fc695f15661`). `network.phase_datum_anchor_bar_stations` skips a barred station at every site of the prioritized election (sticky incumbent, candidacy screen, re-entry restore, held-basis rank, the coverage sweeps; the slip handover goes through the same election); the loader refuses the key under the eq34 chain or the legacy pivot datum, which cannot honour it. Not a fingerprint term. Unit suite 3586 passed, 33 skipped (fixtures absent), 0 failed (17 new cases); the NOM lints, the ledger verifies and the posture check pass. The pre-registered tag `bar_ctl_f2` ran as `bar_barctl_f2`.

**Gates.** G0: the empty knob identical to the base exe on both 16 min windows (products, consumer, epoch status, and also `network_events.csv`, `network_phase_anchor.csv`, `network_solution.csv`). P0: every run exit 0, 240 of 240 emitted, none withheld. P1: MAJU00MHL holds no pin on either barred run (0 rows in `network_phase_anchor.csv`; 388 and 527 on the unbarred windows); 17 barred candidacies on the three-band arm, 0 incumbents deposed, 0 families left without a candidate; MAJU00MHL's satellites go to GUAM00GUM (G01, G10 L1C and L5Q), GCGO00USA (G10 L2W), DARW00AUS (G16, G26, G02 L1C) and NYA200NOR (G23, G27, G08 L5Q).

| arm | WTZZ00DEU, hours 1 and 2 (m) | GPS clock wire, all / tracked (m) | GPS band 1 / 2 / 5 | decouples (at 260040) | MAJU00MHL, 260010 to 260040: GPS_DCB / GPS_IFB5 (m) |
| --- | --- | --- | --- | --- | --- |
| `r1d_ctl_f2` | 0.058 0.011 | 0.0235 / 0.0208 | 1 (0.015) / 0 (0.015) / - | 2 (1) | -0.16 / - |
| `bar_barctl_f2` | 0.056 0.011 | 0.0223 / 0.0198 | 1 (0.015) / 0 (0.014) / - | 1 (0) | -0.18 / - |
| `r1d_t1on_f2` | 0.076 0.042 | 0.0929 / 0.0558 | 8 (0.049) / 5 (0.029) / 5 (0.032) | 25 (21) | -7.06 / +17.78 |
| `bar_bart1on_f2` | 0.080 0.040 | 0.0202 / 0.0177 | 1 (0.017) / 1 (0.014) / 2 (0.031) | 9 (0) | -0.11 / +0.29 |

**Reading (the frozen rules).** The bar's own effect on the dual network is nil (WTZZ00DEU within 0.002 m, the clock wire 0.95 times). With MAJU00MHL barred, the three-band arm passes M (the station's move is 2 % of `r1d_t1on_f2`'s; no decouple at 260040) and every bar against the barred dual control: WTZZ00DEU +0.024 and +0.029 m (bar 0.03), the clock wire 0.91 and 0.89 times (bar 1.10; better than the dual network), the band 1 and 2 offsets one station each (bar: one more than the control). **The anchor loop is the transmission:** a station that anchors satellites' phase datum ties its weakly held three-band level to the phase biases its pins define, and its carried ambiguities couple back to those biases, so a routine multi-ambiguity re-seed at that station moves the network. Barred, the same re-acquisition at MAJU00MHL is absorbed.

**What remains on the barred arm.** (1) The held-out consumer is still worse than dual in hour 2 (0.040 against 0.011 m) on a three-band wire that is now as clean as the dual one: the consumer's own half, the Block IIF L5 phase of card L5B section 7 (step R-2). (2) A smaller burst of 6 GPS satellites at TOW 262440 (G03, G04, G06, G10, G26, G32), absent on `r1d_t1on_f2`; the pair at 262380 (G05, G11) is on both. Not yet read.

**Standing.** L5 SEQUENCED, with the network-side cause of the field event identified. The bar is a diagnostic, not a response: any station can anchor, and a different day puts the event at a different one. The response is the author's design decision, among: (a) an election rule that keeps the anchor role away from a station whose level is not yet held (an ignition or level-variance criterion); (b) cutting the loop at the coupling, so an anchor station's carried ambiguities do not hand moves of the phase biases its own pins define back into its level; (c) the station-level projection of section 7 (no shared move into any station's data-null level directions), which covers anchors and non-anchors alike. Owed beside it: the burst at 262440, the slip-layer findings of card L5B's last addendum (the common-band scope's missing verdict at a re-acquisition epoch, the IF-TD flag at 259980), and R-2.

## 10. 2026-10-23: pre-registration of the station-level projection of the coupling (option (c))

**The design.** A station's carried local columns that enter through coupling rows (the carried ambiguities and the carried receiver code biases) carry the conditional mean `r_prev + M (s - s_prev)`, `M` the regression on the present shared columns from the previous joint covariance. Let `Z` be the station's data-only null basis this epoch (section 1: one level direction per constellation) and `Z_c` its rows on the coupled carried columns. The projection replaces `M` by `M' = (I - P) M`, `P = Z_c (Z_c^T Z_c)^+ Z_c^T`, before the rows are formed, and uses `M'` in every term that `M` enters (the prefit's shared sum and the departed-variance term). The carried columns still follow the shared moves in every direction the station's own data see; their component along the station's level directions no longer follows the shared state and is held at its carried value. No row is added or removed and nothing else changes.

**Build.** `network.carry_coupling_level_project` (bool, default false); every output byte-identical off. Run-end telemetry: station-epochs projected, the null dimension found (distribution), the norm of `P M` removed.

**Arms** (one exe; 2 h; graded like sections 5 and 9):

| tag | change against the golden | role |
| --- | --- | --- |
| `prj_ctl_f2` | the knob | the projection's own effect on the dual network (against `r1d_ctl_f2`) |
| `prj_t1on_f2` | `t1on` keys + the knob | the response (against `r1d_t1on_f2` and `r1d_ctl_f2`) |
| `prj_t1on_s2` | sim golden + `t1on` keys + the knob, from 00:30:15 | no harm on the sim (against `runs/_rap_l5s_t1on_s2`) |

**Gates (frozen).** G0: knob off identical to the base exe (16 min windows of `ctl` and `t1on`). P0: exit 0, every epoch emitted, none withheld, clock-datum dimension 2, no S-basis completion. P1: the projection acts at every station every epoch on the three-band arm, with a null dimension of 2 at MAJU00MHL. M: MAJU00MHL's GPS_DCB and GPS_IFB5 change from TOW 260010 to 260040 within a tenth of `r1d_t1on_f2`'s and at most 3 decouples at 260040. B: against `r1d_ctl_f2`, WTZZ00DEU within 0.03 m in hours 1 and 2, the GPS clock wire at most 1.10 times (all and tracked), GPS band 1 and 2 offsets at most one station more; `prj_ctl_f2` within 0.01 m of `r1d_ctl_f2` on WTZZ00DEU by hour and at most 1.10 times on the wire; the sim arm within 0.01 m of `runs/_rap_l5s_t1on_s2` by hour.

**Reading (frozen).** M and B passing on both field arms: the projection is the response candidate (6 h and the census next, the ledger row UNVALIDATED until then). M passing and B failing: the loop is cut and another cost remains; read it before any change. M failing: the transmission does not run (only) through the coupled carried columns; the border carry's own shared fold (section 13 of card L5B) and the pins are next. A dual-arm regression beyond its bar makes the projection unacceptable as built, whatever the three-band arm shows.

## 11. 2026-10-23: the projection, result at the window gate (numbers re-read by the lead)

**Build.** Branch `card/r1d-levelproj`, commit `54552c8` on main `f45070e`, not merged; exe `gnss_processor_levelproj.exe` (sha256[:16] `970b2ed2109b8f6c`). The projection runs on the block route (the golden's) at the head of the block build, before the rows and the whitening, so every term of the row (shared coefficients, prefit sum, departed variance, time update, exact update, gauge restore) sees `M'`; the scalar route is covered by unit tests only. Fingerprinted when armed. Unit suite 3580 passed, 33 skipped (fixtures absent), 0 failed (11 new cases); the lints and ledger verifies pass.

**Gates on the 16 min windows.** G0: knob off identical to the base exe on both pairs (PASS). P1: the projection acts at all 58 stations every epoch, null dimension 2 at each (PASS). P0: the dual window emits 33 of 33, but the three-band window emits 29 and withholds 4 (TOW 260070 to 260160: the S-basis null-space gate observes 1 near-zero direction where 2 are declared) (FAIL). M: MAJU00MHL's change from 260010 to 260040 falls to GPS_DCB -0.75 and GPS_IFB5 +1.78 m (from -7.07 and +17.79), but the decouples at 260040 rise to 32 (bar 3) (FAIL). The invariant-7 datum-direction ratio rises from 4e-4 (dual) and 1.3e-3 (three-band) to 1.8 on the Galileo clock family (dual with the projection, 2 epochs, none withheld) and 6.3 on the GPS clock family (three-band with the projection). The 2 h arms were not run: a P0 failure at the window ends the arm.

**Reading.** The projection cuts the transmission into MAJU00MHL's level by about ten times, which supports section 9's mechanism, but as specified it breaks the coupling's gauge invariance. Two defects of the design (this card's section 10, not of the build): (1) `P` is orthogonal, not information-weighted, so the level is held exactly only for equal row weights; (2) `P` mixes coupled columns of different gauge families (receiver code biases, GPS and Galileo ambiguities), which changes each family's coefficient sums that the existing gauge restore relies on, so the rows observe a declared clock-gauge direction and the rank gate withholds. As built, the projection is not acceptable.

**Standing.** L5 SEQUENCED. The station-level projection needs a corrected derivation before another build: a projection weighted by the rows' information and constrained to keep each gauge family's coefficient sums (the projection taken within the gauge-invariant subspace), with the datum-direction ratio as its first gate. The election rule (option (a) of section 9) acts on the anchor role without touching the coupling algebra. Which to pursue is the author's decision.

## 12. 2026-10-23: the weighted, gauge-sum-preserving projection, derived and checked offline

**The invariant to keep** (`CouplingRowBuilder::tuGaugeRestore`, `Coupling.hpp`). Each coupling row observes the datum gauge exactly as much as its local requires: for every gauge family `f` (constellation, kind: CLK, PB, CB) the row's coefficient sum over the family's present shared columns is held at its harvested value, `M 1_f = sigma_f` (about -1 or 0 per row on MAJU00MHL's block, after the time update and the departed-column fold). Section 10's projection changed these sums because `P` mixed rows with different `sigma_f`; the rows then observed a declared clock-gauge direction.

**Derivation.** Write the station's carried columns' coupling prior as `x_c + M x_s = y`, information `W_c` (the inverse of the covariance the block route whitens with), `M` over the present shared columns. Let `Pi_F` be the orthogonal projector onto `span{1_f}` and `R = I - Pi_F` (shared side). Let `Z` be the data-only null basis and `Z_c` its rows on the carried columns. Given the data-visible part `q`, the level coordinates minimize the carried prior, and their direct dependence on `x_s` is `(Z_c^T W_c Z_c)^{-1} Z_c^T W_c M x_s`. Requiring it to vanish for every non-gauge shared move while keeping every family sum, with the smallest change of `M` in the metric of `W_c`:

`M' = M - P_W M R`,   `P_W = Z_c (Z_c^T W_c Z_c)^+ Z_c^T W_c`.

Then `M' 1_f = M 1_f` (since `R 1_f = 0`) and `Z_c^T W_c M' R = 0`. The projection acts on the final present-column coefficients, after the time update, the exact update and the gauge restore, and before the whitening; the prefit's shared sum uses `M'`; the departed terms and `W_c` are untouched (the projection does not touch a departed column), so there is no circularity.

**Offline check on MAJU00MHL's real block** (session script `proj_check.py` on `runs/_rap_l5d_loc_blk/logs/rhs_attrib/blk_260010_MAJU00MHL.csv` and `anchored/blk_260040_MAJU00MHL.csv`; `M` and `W_c` recovered from the whitened coupling rows: 17 and 19 carried columns, 480 shared columns in 6 families, null dimension 2):

| projection | max family-sum change | `max |Z_c^T W_c M' R|` | change of `M` (Frobenius, of 17.3) | level's local response to the 260010 to 260040 shared move, non-gauge part (m) |
| --- | --- | --- | --- | --- |
| none (as built) | 0 | 474 | 0 | -18.42 |
| section 10 (orthogonal, failed at the window) | not kept | | 10.7 | -6.66 |
| this section (weighted, gauge sums kept) | 1.7e-15 | 3.6e-10 | 0.57 | -16.11 |
| full-path variant (below) | 1.9e-15 | | 13.2 | +0.21 |

The derived projection does exactly what it states, and on this block that is a cut of 12 % in the level's local response: most of the response goes through the carried prior's coupling between the level and the data-visible parts of the carried columns, which follow the shared state and pull the level with them; the weighted projection leaves that path intact. A full-path variant zeroes the response through the whole local block at the epoch's operator while keeping every family sum, `M' = M - W_c^{-1} H^T (H W_c^{-1} H^T)^{-1} (H M R + D_0) R` with `H = Z^T N_rr^{-1} E_c W_c` and `D_0` the other rows' transmission, but it changes `M` by three quarters of its norm and needs the station's whole local normals when the coupling rows are formed. The local response is not the closed loop: section 10's projection cut it by 65 % and MAJU00MHL's realized move by 89 %.

**Build (as the author directed), gates and prediction (frozen).** The knob of section 10 keeps its name; on branch `card/r1d-levelproj` the projection becomes the derived one. First gate, the datum-direction ratio of invariant 7 on both 16 min ON windows: at most 0.01 on every family (knob off: 4e-4 dual, 1.3e-3 three-band) with no breach epoch, and no epoch withheld. Then section 10's gates, the window first, the 2 h arms only if the window passes P0. Prediction: the gauge gates pass; M fails (MAJU00MHL's move above a tenth of `r1d_t1on_f2`'s), because the local cut is 12 %. If M fails, the full-path variant is the derivation's next candidate and the election rule the alternative.

## 13. 2026-10-23: the weighted, gauge-sum-preserving projection, result (numbers re-read by the lead)

**Build.** Branch `card/r1d-levelproj`, commit `53941bc` (on `54552c8`), not merged; exe `gnss_processor_levelproj2.exe` (sha256[:16] `846fa2b3f153b0d0`). The projection acts on the finished rows of the block route, after the exact-cross terms and before the whitening (`CouplingRowBuilder::projectRows`), with `W_c` the inverse of the block covariance the rows are whitened with; the scalar route projects its deferred rows together with `W_c = diag(1/sigma^2)`. Post-conditions measured in the build at all 1508 station-epochs of each window: family sums kept to 4.5e-14, `|Z_c^T W_c M' R|` 1.9e-10 to 4.7e-10 (before 1.7e4 to 2.1e5). Unit suite 3589 passed, 33 skipped (fixtures absent), 0 failed; the lints and ledger verifies pass. A log-only line printing the datum-direction ratio by declared direction is added beside the existing one.

**Window gates** (`runs/_rap_prj_*_ev2`). First gate, the datum-direction ratio: at most 1.37e-3 on every family on both ON windows (knob off 4.3e-4 and 1.3e-3), no breach epoch, no withhold (PASS; section 11's projection reached 1.8 and 6.3). G0 identical (PASS). P0 33 of 33 on both ON windows (PASS). M: MAJU00MHL's change from TOW 260010 to 260040 GPS_DCB +1.09 and GPS_IFB5 -2.44 m (control -7.07 and +17.79: 15 % and 14 %), 47 decouples at 260040 on both constellations with a median clock move of 0.18 m (control 21, GPS only, 3.73 m) (FAIL, as predicted in section 12; the realized cut of about 85 % against a local cut of 12 % is the closed loop's amplification).

**The 2 h arms** (P0 passed at the window, so they ran):

| arm | WTZZ00DEU, hours 1 and 2 (m) | GPS clock wire, all / tracked (m) | GPS band 1 / 2 / 5 | decouples (at 260040) |
| --- | --- | --- | --- | --- |
| `r1d_ctl_f2` | 0.058 0.011 | 0.0235 / 0.0208 | 1 (0.015) / 0 (0.015) / - | 2 (1) |
| `prj_pctl_f2` | 0.058 0.013 | 0.0341 / 0.0287 | 1 (0.012) / 0 (0.014) / - | 0 (0) |
| `r1d_t1on_f2` | 0.076 0.042 | 0.0929 / 0.0558 | 8 (0.049) / 5 (0.029) / 5 (0.032) | 25 (21) |
| `prj_pt1on_f2` | 0.074 0.026 | 0.0356 / 0.0294 | 12 (0.047) / 9 (0.039) / 22 (0.072) | 47 (47) |

Sim `prj_pt1ons_s2`: SZZ000SIM 0.100 0.038 m against 0.099 0.035 m (PASS). The datum-direction ratio stays at most 1.37e-3 on the 2 h arms, no breach.

**Reading (the frozen rules of sections 10 and 12).** The dual arm regresses beyond its bar: the GPS clock wire is 1.45 and 1.38 times the dual control's (bar 1.10). By section 10's rule this makes the projection unacceptable as built, whatever the three-band arm shows; the three-band arm also fails M and the offset bars (band 5 at 22 stations). The projection removes the coupling's statistical link between a station's level and the shared state, and on the dual network that link carries information the clock solution uses. The full-path variant of section 12 changes the coupling more (three quarters of its norm) and is not pursued.

**Standing.** L5 SEQUENCED. Option (c) is set aside: two derivations of the station-level projection either break the gauge (section 11) or cost the dual network's clock wire 40 % (this section), and neither holds MAJU00MHL's event. The anchor-bar discriminator (section 9) remains the only intervention that removes the event and leaves the dual network unharmed (clock wire 0.95 times dual; the three-band wire 0.91 times). The next candidate is therefore the election rule, option (a): keep the phase-datum anchor role away from a station when its level is moving, with the criterion still to be chosen (for example, an anchor station hands its families over at an epoch where it re-seeds several ambiguities of one line of sight, the event at MAJU00MHL). The builds of sections 5, 11 and 13 stay on their branches as the record.

## 14. 2026-10-23: pre-registration of the re-seed handover (option (a))

**Question** (the author's direction after section 13): does an election rule that moves the phase-datum anchor role off a station at the epoch it re-seeds several ambiguities of one line of sight remove the event of card L5B, with the dual network unharmed?

**The rule.** `network.phase_datum_anchor_reseed_handover` (bool, default false), with `network.phase_datum_anchor_reseed_min_ambiguities` (default 2) and `network.phase_datum_anchor_reseed_holdoff_epochs` (default 120). At an epoch where a station that anchors at least one phase-datum family re-seeds at least the minimum number of distinct ambiguities of one satellite before the election (the slip responses of the epoch's slip funnel: gap re-entry, wide-lane, IF time-difference and the other pre-solve refloats), the station is barred from the election for the holdoff (the bar of section 9's build, applied for that window), so the existing handover path moves each of its families to the candidate the rule would otherwise take. Every trigger is an event at both edges (invariant 7): a `PHASE_ANCHOR` event names the station, the satellite, the ambiguities and the families handed over, and a second one the bar's expiry; run-end counts of triggers, families handed over, re-elections after the holdoff and families left without a candidate. Off, every output is byte-identical. Expected on this data: the trigger fires at MAJU00MHL on G08 well before TOW 260040 (wide-lane refloats of all three bands at 259230 and 259290, the IF-TD refloat of L1C and L5Q at 259980).

**Arms** (one exe; 2 h; graded like section 9):

| tag | change against the golden | role |
| --- | --- | --- |
| `rsh_ctl_f2` | the rule | its cost on the dual network (against `r1d_ctl_f2`) |
| `rsh_t1on_f2` | `t1on` keys + the rule | the response (against `r1d_t1on_f2` and `r1d_ctl_f2`) |

**Gates (frozen).** G0: the rule off identical to the base exe (16 min windows of `ctl` and `t1on`). P0: exit 0, 240 of 240 emitted, none withheld; the datum-direction ratio at most 0.01, no breach. P1: the triggers by station and epoch; MAJU00MHL barred before TOW 260040; 0 families left without a candidate. M: MAJU00MHL's GPS_DCB and GPS_IFB5 change from 260010 to 260040 within a tenth of `r1d_t1on_f2`'s and at most 3 decouples at 260040. B: against `r1d_ctl_f2`, WTZZ00DEU within 0.03 m in hours 1 and 2, the GPS clock wire at most 1.10 times (all and tracked), GPS band 1 and 2 offsets at most one station more; `rsh_ctl_f2` within 0.01 m of `r1d_ctl_f2` on WTZZ00DEU by hour and at most 1.10 times on the wire.

**Reading (frozen).** M and B passing on both arms: the rule is the response candidate (6 h, the census and a second field day next; the ledger rows UNVALIDATED until then). M passing and the dual bar failing: the handovers themselves cost the network (read the trigger rate and the handover events before any tuning). M failing with MAJU00MHL barred before the event: the handover does not undo what the station's anchoring already moved (the pre-event drift of card L5B section 14), and the bar's whole-run form of section 9 is the bound; the trigger then needs to be earlier or the holdoff longer, which is a new card section, not a tuning of this one.

## 15. 2026-10-23: the re-seed handover, result at the window gate (numbers re-read by the lead)

**Build.** Branch `card/r1d-reseedbar`, commit `c59fd2f` on `6edfff2` (the static bar), not merged; exe `gnss_processor_reseedbar.exe` (sha256[:16] `22e496758eac7473`). The trigger reads the union of the slip funnel's carried slips and the leveling carry's free set `relevel.free_ambs` (where every PASS A drain lands on the goldens: wide-lane, LLI, IF time-difference, GF arc jump, wind-up restart, gap re-entry), as the existing anchor-slip handover does; a refloat recorded only when the carry is warm is not seen, and post-fit refloats act after the election. The temporary bar is a Station entity field (ColdStart), not a fingerprint term. Unit suite 3609 passed, 33 skipped (fixtures absent), 0 failed (23 new cases); the lints and ledger verifies pass.

**Window gates** (`runs/_rap_rsh_*_ev`). G0 identical (PASS). P0: 33 of 33 emitted on both ON windows, datum-direction ratio at most 1.4e-3, no breach (PASS). P1 (FAIL on the three-band window): 3 triggers on the dual window (LCK400IND 259830, MAJU00MHL 259920, SGOC00LKA 260010), 8 on the three-band window (MAC100AUS 259380, KOKV00USA 259410, GCGO00USA 259440, WARK00NZL 259680, LCK400IND 259830, MAJU00MHL and SCTB00ATA 259980, SGOC00LKA 260010); MAJU00MHL barred before the event on both (259920 gap re-seed of G08 L1C and L2W; 259980 IF time-difference re-seed of G08 L1C and L5Q) and every family it anchored re-anchored elsewhere (G01 to NIUM00NIU, G08 L5Q to NYA200NOR, G23 to JPLM00USA and STJO00CAN, and the rest); re-anchorings 14 to 42 on the dual window and 19 to 75 on the three-band window, about half of them cold; one family (G23 L5X) left without a candidate for 25 epochs (both its candidates barred). M (FAIL): MAJU00MHL's change from 260010 to 260040 GPS_DCB -9.33 and GPS_IFB5 +25.24 m (control -7.07, +17.79), 27 decouples at 260040 (control 21). The 2 h arms were not run.

**Reading (the frozen rule of section 14).** MAJU00MHL anchored nothing at TOW 260040 and the event came anyway, larger. A handover is continuous (the families re-anchor with the datum carried across), so the phase biases MAJU00MHL defined during ignition keep its realization after it leaves the role, and its carried ambiguities stay coupled to them; section 9's whole-run bar worked because MAJU00MHL never defined any. The trigger is also noisy during ignition on three bands (8 stations in 16 minutes, half the re-anchorings cold) and can strand a family. The rule is not a response.

**Standing.** L5 SEQUENCED. Three interventions on the anchor loop have now been measured: barring the station for the whole run removes the event (section 9); cutting the coupling either breaks the gauge or costs the dual clock wire (sections 11 and 13); moving the role away after it was held does not undo it (this section). What the anchor contributes to the datum is its own weakly held level, through the phase biases its pins define. The remaining candidate on this line is to make that contribution explicit: while a station anchors, its data-null level directions are held as part of the phase datum (a minimum-constraint completion beside the pins, carried continuously through a handover like the pins' values), so the phase biases it defines cannot inherit a move of its level. Its derivation, against the S-basis and the held basis, comes before any build.
