# GFC: one clock datum per constellation across Galileo's two code families

**STATUS**

- **id:** GFC
- **topic:** network-vehicle
- **status:** open
- **summary:** RESULT 2026-10-10: the family-difference datum is the fix (Galileo clock wire 0.105 to 0.035 m, pilot-family users 0.8-0.9 to 0.16-0.26 m from hour 3, WTZZ00DEU hour-3 hump gone, census neutral against the Galileo control); pair continuity KEEP_OFF (it preserves birth realizations, worse for pilot users). Not armed. Pre-registered text: Card GAL2's field leftovers (the Galileo clock wire 0.106 m against 0.009 m on the sim, HARB00ZAF and DAV100ATA worse on pilot channels, E11 and E12's C5Q gross rows at SEYG00SYC) trace to one root read on 2026-10-10: a Galileo satellite's ionosphere-free code pin falls on whichever code family covers it, the inter-family offset (a constellation constant the data cannot see) is realized at about +1.1 m on pilot-born satellites and about 0 on combined-born ones, and every pair change re-seeds the pin at zero. Two default-off knobs: continuity at a pair change, and a family-difference datum in place of the combined channel's sum-to-zero.
- **source:** [GAL2](GAL2_galileo_on_the_exact_update_2026-10-08.md) section 6 (the owed list); [GAL](GAL_gps_galileo_dual_2026-09-22.md) section 3.2 (the family rows); [OWD](OWD_owed_defects_2026-09-22.md); two code and run reads of 2026-10-10 (no new run; section 1)
- **knobs / posture:** new `network.code_datum_pair_continuity` and `network.code_datum_family_difference` (both default false); the goldens of 2026-10-09 plus card GAL2's four Galileo keys (`processing.active_systems`, `processing.signal_priorities.Galileo`, `estimation.clock_disc_isb_reinflate_std` 10, `model_corrections.enable_receiver_cross_system_atx_fallback`)
- **dates in the text:** 2026-10-10
- **hashes / run dirs:** Galileo controls `runs/_rap_l5g_gal_{f6,s6}` and census key `l5c_gal` (exe `gnss_processor_l5g0.exe`, main `47fb634`); GPS-only controls `runs/_rap_l5g_ctl_{f6,s6}`, census `l5c_ctl`; arms `runs/_rap_gfc_*`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Question: does one clock datum per constellation across Galileo's two code families clear card GAL2's field leftovers? The family-difference datum alone does: the two C1X-minus-C1C groups collapse (median 0.001 m), the Galileo clock wire falls from 0.105 to 0.035 m, pilot-family users see 0.16 to 0.26 m instead of 0.8 to 0.9 m from hour 3, and WTZZ00DEU loses its hour-3 hump, census neutral. Missed: pair continuity is harmful (KEEP_OFF), HARB00ZAF / DAV100ATA and SEYG00SYC's IOV rows are untouched, and the residual R-a error stays (section 6).

## 1. What the reads of 2026-10-10 found (no new run)

- **The pin follows coverage.** For each satellite the ionosphere-free code-bias pin is re-selected every epoch from the code signals registered at two or more stations, sorted by band and name (`NetworkSbasis::selectIfPinPair`). For Galileo the pin is (C1C, C5Q) while the pilot family covers the satellite and falls to (C1C, C5X) or (C1X, C5X) when it does not. On the 6 h field Galileo arm of card GAL2 (`runs/_rap_gal2_g5_f6`), 15 378 satellite-epochs pinned on (C1C, C5Q), 1 826 on (C1X, C5X), 375 on (C1C, C5X); 22 `CLK_BASIS` re-selections, all Galileo (0 on the GPS-only control, 0 on the sim split emulation, whose relabelling at every second station never removes a family from a satellite).
- **A pair change re-seeds the pin.** Under `code_datum_time_linked` the pin's target carries the previous solved combination only when the pair is unchanged (`NetworkSbasis.cpp` around 988 to 1000); otherwise it re-seeds at zero and the emit site declares a full-scope discontinuity. The geometry-free row beside it carries across a pair change whenever both new signals have previous values.
- **The inter-family offset is realized twice.** The two families share no receiver, so the family's common offset is a datum quantity, fixed today by the time-linked sum-to-zero row of the combined channel (E|C1X, seeded at zero over the satellites present at ignition). On the wire, C1X minus C1C (median over hours 2 to 6) is +0.89 to +1.33 m on the 19 satellites first pinned on a pilot or hybrid pair and +0.004, -0.002, +0.003, +0.040 m on E10, E11, E18, E25 (first pinned on the combined pair). COD's product puts C1X minus C1C at mean 0.000 m, sd 0.041 m.
- **What users see.** The wire's ionosphere-free code side against CODE, rms across satellites: combined-family users 0.13 to 0.35 m by hour, pilot-family users 0.81 to 1.44 m, with +1.6 to +2.2 m on the combined-born satellites (E02, E10, E11, E18, E25) and within 0.44 m elsewhere. The producer's pilot-family post-fit code residual on those five satellites is -1.4 to -2.0 m.
- **The owed items it explains.** The Galileo clock wire's steps fall at the pair changes (E36 +1.634 m at TOW 264930, E34 +0.748, E30 +0.560, E02 +0.473, E25 +0.320); arcs cut at the declared discontinuities give 0.027 m instead of 0.106 m. HARB00ZAF and DAV100ATA draw 58 % and 52 % of their Galileo code rows in hours 2 to 4 from the five combined-born satellites (BAKE, POHN and KOKB, near 0 %, gain). E12's and E11's C5Q gross rows all sit at SEYG00SYC in windows that start at the epoch each satellite's pin moved from (C1C, C5X) to (C1C, C5Q) with two or three pilot stations; E19 (the third IOV satellite) is clean and no yaw manoeuvre is near (beta about 23 degrees).
- **Not this card.** Card GAL2's "per-family geometry-free pin" cannot move the graded quantity: a geometry-free row is blind to every ionosphere-free combination (a1 mu1 + a2 mu2 = 0), so it changes only which of C1X and C5X carries the family's ionosphere-free content. The G3 "datum" grade of card GAL2 is the ionosphere-free grade divided by a1 (0.0304 = 0.0689 / 2.266, re-run). The O8 item (GPS clock wire +14 % with Galileo) does not reproduce on the current golden: same exe, `runs/_rap_l5g_gal_f6` 0.0245 / 0.0198 m against the GPS-only `runs/_rap_l5g_ctl_f6` 0.0242 / 0.0198 m (all / consumer-tracked, hours 2 to 6); card GAL2 graded against a control without card ICF's cold-start bound.

## 2. The build

Two default-off knobs in `NetworkSbasis::buildCodeBiasDatumRows`, read only under `network.code_datum_time_linked`:

- **`network.code_datum_pair_continuity`.** At a pin pair change where both signals of the new pair have a previous solved value, the pin's target is the new pair's previous solved ionosphere-free combination (the satellite clock continues), the satellite is not marked for a re-seed, and the emit site does not declare the re-selection discontinuity for it; the change is logged as an event and counted. Without previous values the existing re-seed applies.
- **`network.code_datum_family_difference`.** Per constellation and band, where a secondary-family code channel (attribute X, `NetworkSbasis::channelFamilyOf`) keeps a sum-to-zero row (no other row pins it) and a primary-family channel of the same band exists, that sum-to-zero row is replaced by a family-difference row over the satellites carrying both channels: the sum of (d_X - d_primary) is zero at its seed and carries the previous solved sum over veterans afterwards, with the same re-seed and marking rules as the sum-to-zero's time link. With no satellite carrying both channels, the sum-to-zero row is kept and the epoch counted. This is COD's convention (inter-family mean zero) and puts every satellite's clock on one realization of the family offset, whichever family pins it.

Both knobs are fingerprinted (they change the realized datum), carry an INFO line when armed and an inert warning without `code_datum_time_linked`, and report run-end counters (pair changes continued and re-seeded; family-difference rows built, veterans, re-seeds, fallbacks) printed only when armed.

## 3. The arms

One exe from the GFC worktree (main `47fb634` plus the build), 6 h unless noted, sim arms from 00:30:15.

| tag | bed | change against the golden | role |
| --- | --- | --- | --- |
| `gfc_g0_{f2,s2}` | field, sim | none | G0 against `l5g_g0_{f2,s2}` |
| `gfc_g0g_f2` | field | the Galileo keys | G0 against the first 240 epochs of `l5g_gal_f6` |
| `gfc_pc_f6` | field | the Galileo keys + pair continuity | attribution |
| `gfc_fd_f6` | field | the Galileo keys + family difference | attribution |
| `gfc_both_f6` | field | the Galileo keys + both | the build |
| `gfc_both_s6` | sim | the Galileo keys + both | sim identity or truth |
| `gfc_hx_ctl_s6`, `gfc_hx_both_s6` | sim | the Galileo keys + a hemisphere family split (C1C/L1C/C5Q/L5Q relabelled to the X types, with per-satellite constants N(0, 0.05 m) on C1X and C5X, seeds 71 and 72, at the 31 eastern stations), knobs off / both | the coverage edge on the truth bed |

Census: Galileo consumers at the 16 sites and WTZZ00DEU on `gfc_both_f6` (key `gfc_both`) against `l5c_gal`, and against `l5c_ctl`.

## 4. Gates (frozen)

- **G0.** `gfc_g0_{f2,s2}` identical to `l5g_g0_{f2,s2}` and `gfc_g0g_f2` identical to the first 240 epochs of `l5g_gal_f6` (`scripts/clock_level/ident2h.py`).
- **G1 (mechanism, `gfc_both_f6`).** 720 of 720 emitted, the S-basis's declared dimension equal to its count at every epoch, 0 completions; the knobs' run-end counters non-zero (pair changes continued at least 15, family-difference rows on every epoch with a satellite carrying both channels); C1X minus C1C per satellite (wire B records, median over hours 2 to 6) within 0.30 m of its across-satellite median on every satellite (today a +0.89 to +1.33 m group and a 0 group).
- **G2 (field, `gfc_both_f6` against `l5g_gal_f6`).** (i) The Galileo clock wire (`scripts/ftc_step_attrib.py --r3 --systems E`, row `e_clk`, all satellites, uncut) at or below 0.05 m (0.105 today; the reads predict about 0.03). (ii) The pilot-family code users' wire error (the ionosphere-free code side against CODE, rms across satellites, hours 2 to 6) at or below 0.30 m (0.81 to 0.91 today). (iii) E11 and E12 C5Q gross rows above 3 m: at most 10 (127 today). (iv) The GPS clock wire within 0.0005 m of `l5g_gal_f6`'s. (v) WTZZ00DEU 3D p50 within 0.010 m of `l5g_gal_f6` in hours 2 to 6 or better.
- **G3 (census, key `gfc_both` against `l5c_gal`).** Paired change of each site's mean hourly p50 over hours 2 to 6 (1 cm threshold): median at or below 0 and no more sites worse than better; HARB00ZAF and DAV100ATA each within 0.02 m of their `l5c_ctl` (GPS-only) value. Against `l5c_ctl`: reported (card GAL2's adoption statistic).
- **G4 (sim).** `gfc_both_s6` against `l5g_gal_s6`: the consumer 3D p50 within 0.010 m by hour (no pair change occurs on the uniform fixture, so near-identity is expected). `gfc_hx_ctl_s6` shows the defect on truth (re-selections above 0, a two-group C1X minus C1C) and `gfc_hx_both_s6` removes it; the consumer not worse than `gfc_hx_ctl_s6` by more than 0.010 m in any hour from hour 2.
- **Attribution (read, no bar).** `gfc_pc_f6` and `gfc_fd_f6` against `l5g_gal_f6` on G1 to G2's quantities: the continuity alone should remove the steps but not the two groups; the family difference alone should remove the two groups and shrink the steps to the inter-family scatter (about 0.1 m).

Not armed by this card; Galileo adoption stays the author's decision, now with these two knobs on its list.

<!-- Results are appended below this line under dated headings. -->

## 5. 2026-10-10: amendments before any gate run (the build and its review)

The build (worktree `gfc/build`, three adversarial reviews and a fix round) changed four things against section 2. All are recorded here before the first gate run; the frozen text above stands as written.

- **Entering changes.** A pin pair change has two kinds. A leaving change (a signal of the pinned pair leaves the registered set) can be continued, because the new pair's signals were registered at the last kept epoch and have previous values. An entering change (a lower-ordered signal becomes registered and `selectIfPinPair` prefers it) cannot: the new signal has no previous value. On `runs/_rap_l5g_gal_f6` 12 of the 22 re-selections are entering changes, among them E11 and E12 moving from (C1C, C5X) to (C1C, C5Q), the windows of the SEYG00SYC gross rows. `network.code_datum_pair_continuity` therefore also HOLDS the previous pair while both of its signals stay registered (no change, no re-seed); only a broken pair falls back to the lexical choice, and the continuity rule applies to that leaving change. Holds are counted and their two edges logged (`CLK_BASIS` `held`).
- **Validity condition.** Continuity keeps the clock only when every other row on the satellite's code-bias columns also carries its previous value. It is applied only when no row re-asserts an absolute target on a new pair: `code_bias_gf_datum` off, and `sat_gf_datum` either off or time-linked, and `sat_if_level_datum` off (`NetworkSbasis::pinPairContinuityBlocker`); otherwise the knob is inert and says so. The goldens qualify (`broadcast_tgd`, time-linked).
- **Scope of the family difference.** Restricted to Galileo. On the dual posture only band 1 can carry the row: the RF-0d extra rows already fix C5X against the pinned pair, so E|C5X is suppressed. Where every spanned satellite's X column is already pinned by another row, the family row would be dependent; the sum-to-zero row is then kept (outcome `Dependent`, counted). With no family veteran but a sum-to-zero veteran, the sum-to-zero row carries that epoch (`Bridged`, counted) and the family row continues from it, so the family row re-seeds only where the sum-to-zero row would.
- **Emit site.** The re-selection discontinuity is suppressed only when the last emitted pair equals the pair the builder continued from and the satellite carries no pending re-seed; every other case is declared as before. Counts and events are taken once per kept epoch (`networkCodeDatumFamilyKeptEpoch`), not per main pass. The family row reuses the contribution kind `CbSumToZero`: a new kind would add a line to the contributions table and manifest of every run; the `CB_DATUM` `family_row` events and the run-end line witness it.

Amended gate wording (the bars are unchanged):

- **G1** reads "pair changes continued at least 15" as: re-selections that re-seed while previous values exist for the held or new pair: 0; holds plus continued changes at least 15 of the run's re-selection opportunities. "Family rows on every epoch with a satellite carrying both channels" reads as: on every such kept epoch, a family row, a `Bridged` sum-to-zero row or a `Dependent` sum-to-zero row, with their counts reported.

## 6. 2026-10-10: result (every number read by the lead from `runs/`)

Exe `gnss_processor_gfc1.exe` (sha256 `13fd071e05894df3`, worktree `gfc/build` commit `7f56ae9`: the build, the hold rule and both review rounds). Galileo control `runs/_rap_l5g_gal_f6` (exe `gnss_processor_l5g0.exe`, main `47fb634`), census key `l5c_gal`; GPS-only census `l5c_ctl`. Clock wire: `scripts/ftc_step_attrib.py --r3`, row `e_clk`, hours 2 to 6, all / consumer-tracked satellites. Pilot- and combined-family users' wire error: `scripts/clock_level/iflevel_gal.py` (rms across satellites of the per-satellite hourly mean, by hour).

| gate | measured | verdict |
| --- | --- | --- |
| G0 | `gfc_g0_{f2,s2}` identical to `l5g_g0_{f2,s2}`; `gfc_g0g_f2` identical to the first 240 epochs of `l5g_gal_f6` | PASS |
| G1 structure (`gfc_both_f6`) | 720 of 720 emitted; rank gate declared 2 = observed 2 at every epoch; 0 completions | PASS |
| G1 counters | 20 pair changes continued, 0 re-seeded; 10 holds begun and ended; 719 family rows on veterans, 1 seed | PASS |
| G1 two groups | C1X minus C1C per satellite, median over hours 2 to 6: control +0.89 to +1.33 m on 19 satellites and about 0 on E10, E11, E18, E25; `gfc_fd_f6` median +0.001 m, 24 of 25 within 0.17 m, E24 +0.37 m; `gfc_both_f6` E11 at -3.70 m | FAIL by the letter (one satellite on each arm); the two groups are gone with the family difference alone |
| G2 (i) Galileo clock wire | `gfc_both_f6` 0.0365 / 0.0298 m, `gfc_fd_f6` 0.0352 / 0.0239 m, `gfc_pc_f6` 0.0596 / 0.0339 m, against 0.1054 / 0.0370 m | PASS (bar 0.05 m) |
| G2 (ii) pilot-family users, hours 2 to 6 | `gfc_both_f6` 0.586 0.269 0.811 0.783 0.704 m; `gfc_fd_f6` 0.586 0.264 0.204 0.184 0.161 m; `gfc_pc_f6` 0.742 0.633 1.152 1.131 1.029 m; control 0.887 0.805 0.860 0.914 0.857 m. Combined-family users: `gfc_fd_f6` 0.101 0.092 0.091 0.088 0.080 m against 0.132 0.142 0.201 0.294 0.350 m | FAIL (bar 0.30 m); the family difference alone passes hours 3 to 6 |
| G2 (iii) E11 and E12 C5Q rows above 3 m | 93 (both), 120 (family difference) against 127; all at SEYG00SYC; on the family-difference arm the same station's E11 C1C rows rise to 88 | FAIL: not this card's defect (an SEYG00SYC / IOV issue, open) |
| G2 (iv) GPS clock wire | 0.0232 / 0.0198 m (both), 0.0230 / 0.0196 m (family difference) against 0.0245 / 0.0198 m | PASS |
| G2 (v) WTZZ00DEU, hours 2 to 6 | `gfc_both_f6` 0.012 0.017 0.010 0.015 0.010 m, `gfc_fd_f6` 0.011 0.018 0.010 0.015 0.010 m, against 0.011 0.050 0.029 0.024 0.014 m (card GAL2's hour-3 hump gone) | PASS |
| G3 census against `l5c_gal` | `gfc_both`: hours 2 to 6 median +0.0000 m, 1 better, 2 worse; `gfc_fd`: -0.0004 m, 1 better (HARB00ZAF -0.028), 1 worse (DAV100ATA +0.038). HARB00ZAF and DAV100ATA stay 0.06 to 0.29 m above their GPS-only values in hours 2 to 4 | median clause PASS; HARB / DAV1 clause FAIL |
| G3 against `l5c_ctl` (reported) | `gfc_fd`: hours 2 to 6 median -0.0182 m, 10 better, 2 worse; hour 1 -0.260 m, 13 better | read |
| G4 sim, uniform fixture | `gfc_both_s6` byte-identical to `l5g_gal_s6` (no X channel, no pair change; the knobs say so) | PASS |
| G4 hemisphere split | control: 15 re-selections, all re-seeded; ionosphere-free code error against truth from hour 3 1.60 m. Both knobs: 15 continued, 7 holds, 0 re-seeds; 0.88 m. Consumer 3D p50 by hour 0.665 0.072 0.073 0.022 0.035 0.029 m (control) against 0.490 0.067 0.072 0.017 0.040 0.047 m | FAIL in hour 6 (+0.018 m); the defect is reproduced on truth and halved |

**Reading.** The family-difference datum carries the card: one realization of the inter-family offset (COD's convention) puts every Galileo satellite's code biases in one frame, collapses the two groups, cuts the Galileo clock wire threefold, gives pilot-family users 0.16 to 0.26 m instead of 0.8 to 0.9 m from hour 3 and combined-family users 0.08 to 0.10 m, and removes WTZZ00DEU's hour-3 hump, at a census cost of nothing. Pair continuity removes every re-seed and discontinuity, as built, but preserves each satellite's birth realization through a pair change, and that is worse for the pilot users than the declared re-seed onto the pilot pair (E11 -3.70 m on the both-knob wire). The truth bed agrees: both knobs halve the error, and the remaining 1 to 2.4 m on E03, E05, E08, E15 and E19 (satellites with no pin event at all) is card GAL2's reading R-a, the combined family's stiff code column absorbing clock error that the data then cannot pull back; no datum row can reach it.

**Standing.** `network.code_datum_family_difference`: recommended as a Galileo adoption companion (wire quality, census-neutral against the Galileo control), arming is the author's decision. `network.code_datum_pair_continuity`: KEEP_OFF (harmful to pilot-family users). Open, named: R-a (a soft first-sighting prior tying each X column to its pilot partner, or a separate code-bias q for the secondary family); SEYG00SYC's E11/E12 code rows; HARB00ZAF and DAV100ATA.

## 7. 2026-10-10: step 2 pre-registered, the first-sighting family prior (reading R-a)

**The defect left.** With the family-difference datum the inter-family offset has one realization, but on the truth bed (hemisphere split, `gfc_hx_both_s6`) satellites with no pin event at all (E03, E05, E08, E15, E19) keep 1 to 2.4 m of ionosphere-free code error from hour 3, and on the field the pilot-family users' hour 2 is 0.59 m. This is card GAL2's reading R-a: a secondary-family column born beside an uncertain satellite clock absorbs part of the clock error (in ionosphere-free terms all of it, card CLV's algebra), and with `sat_code_bias_q` 1e-10 it keeps it; the data that would pull it back arrive only through the code of the other family's stations.

**The build.** `network.code_family_birth_prior_sigma_m` (default 0, off): at the first sighting of a Galileo band-1 code column of either family on a satellite whose same-band column of the other family is registered, one soft shared row ties their difference to the constellation's realized family mean, `d_X - d_P = mu` (mu = the mean of the previous solved differences over the satellites carrying both, 0 at the first epoch), with the stated sigma, folded once (a per-epoch soft row on a carried state would accumulate into a hard one). Counted (rows, satellites), an event per fold, armed-only telemetry; inert without `network.code_datum_family_difference`.

**Arms** (same exe, 6 h; the GFC step-1 arms are the controls): `gfc2_fd_f6` (Galileo keys + family difference + prior 0.10 m), `gfc2_hx_s6` (the hemisphere split + family difference + prior 0.10 m) against a new `gfc2_hxfd_s6` (the split + family difference only), and `gfc2_fd_s6` (uniform sim, expected identical to `l5g_gal_s6`: no X channel).

**Gates (frozen).**
- **G0:** prior 0 identical to the step-1 exe on `gfc_fd_f6`'s first 240 epochs.
- **G1 (truth):** `gfc2_hx_s6` ionosphere-free code error from hour 3 at or below 0.30 m (`gfc2_hxfd_s6` reported; step 1's both-knob arm 0.88 m); no satellite above 0.5 m; the consumer not worse than `gfc2_hxfd_s6` by more than 0.010 m in any hour from hour 2.
- **G2 (field, against `gfc_fd_f6`):** pilot-family users' wire error at or below 0.30 m in every hour from hour 2 (0.586 in hour 2 today); combined-family users not worse by more than 0.02 m in any hour; the Galileo clock wire not worse than 0.040 m; WTZZ00DEU within 0.010 m by hour from hour 2.
- **G3 (census, against `gfc_fd`):** paired hours 2 to 6 median at or below 0, no more sites worse than better (1 cm).
