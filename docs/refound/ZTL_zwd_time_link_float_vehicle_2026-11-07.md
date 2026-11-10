# ZTL: a time link for the station wet delay on the float ambiguity vehicle

**STATUS**

- **id:** ZTL
- **topic:** network-vehicle
- **status:** closed
- **summary:** Build B2 of the scientific review (card SRN, finding F2). On the float vehicle the station wet delay is a local column born at zero every epoch with no time link; the only vehicle the record did not falsify (`network.carry_zwd_in_block`) was disabled by a loader interlock that tests the F3 knob, which the float vehicle forces off. The card makes the interlock vehicle-aware, folds the wet delay as a column of the station's whitened block, adds the declared event response that standing invariant 1 requires, and grades the time link on the sim truth and the field. Built and graded 2026-11-07 (section 4): G1, G2, G5 pass, G4 fails (hours 4 to 6 accuracy and, without the phase colour state, consistency), G6 shows no integer gain. Sections 6 and 7 (2026-11-09): identity rows at the re-coupling edge and the two erase gaps of section 5 built (G2b, G2c, G8, G9 pass); the pair with the phase colour state passes the field (user within 0.4 mm by hour, 16-site census 0.0 mm, wet delay 3.7 times smoother) except one frozen count clause (G7d: `T4_attrib` rows 2 against 3) and fails the sim accuracy bar (G4: hours 4 to 6 5.1 to 5.5 mm against 4; NEES 0.76 to 0.86 from hour 3, 0.61 in hour 2). Recommended for arming as a pair, never the link alone. Armed 2026-11-09 at the author's decision on both goldens and the BeiDou lineage (section 8; branch `ztl/colour-pairing`, not merged): the health check passes on DOY 129, 130, 132, the sim and BeiDou, no key stopped; look here: DOY 130 census hours 2 and 3 (BAKE, DAV1, worst sites +9.9 and +9.8 mm over hours 2 to 6).
- **source:** card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, finding F2, section 3.5, build B2); the second-model verdicts `D:/GnssProcessorSupport/srn/wave2_verdicts.json` (D2-10, D5-02, D5-03, D5-10); `docs/cards/sim-bed/ZWD-SINK-CENSUS.md` (the diagonal carry falsified).
- **dates in the text:** 2026-11-07, 2026-11-09
- **knobs / posture:** `network.carry_zwd` + `network.carry_zwd_in_block` (both default false, both off on the goldens), `network.zwd_carry_q` (bracket 1e-8, 5e-8, 2e-7 m^2/s); `network.phase_colour_state` (card LGM, default false, off on the goldens) for the pair of sections 6 and 7; the float vehicle of both goldens (`network.ambiguity_vehicle float_kf`).
- **hashes / run dirs:** branch `worktree-wf_c49deb5b-2f3-2` from main `284ec29`, code commit `986bd89`; executable `gnss_processor_ztl.exe` (sha256[:16] `c98f567e94a31006`); arms `_rap_ztl_off_{f2,s2}`, `_rap_ztl_q{1e8,5e8,2e7}_s2`, `_rap_ztl_{ctl,on}_{f6,s6}`, `_rap_ztl_q2e7_s6`, `_rap_ztl_pc{ctl,on}_s6`, census keys `ztlc`, `ztla`; review executable `D:/GnssProcessorSupport/worktrees/scirev/build/x64-Release/gnss_processor_srn.exe` (main `adef08b`); arms under `D:/GnssProcessorSupport/runs_nom/_rap_ztl_*`; renamed executable `gnss_processor_ztl.exe`. Sections 6 and 7: branch `ztl/colour-pairing` from main `27d6ece` (worktree `D:/GnssProcessorSupport/worktrees/ztl2`), pre-registration `019eb42`, code `45789af`; executable `gnss_processor_ztl2.exe` (sha256[:16] `bc5cef3a9cca9c7f`), baseline copy `gnss_processor_ztl2base.exe` (`64543d2d89994cae`); arms `_rap_ztl2_{ctl,zl,pc,pz}_{f2,s2,f6,s6}`, `_rap_ztl2_base_s2`, `_rap_ztl2_zlbase_f2`, `_rap_ztl2_ev_f2`, `_rap_ztl2_smoke_{f,s}`; census keys `ztl2c`, `ztl2p`. Section 8 (arming): runs `_rap_ztl2_armg_{f6,s6}`, `_rap_ztl2_{hc,ha}_{h6,g6,b6}`, census keys `ztl2hc`, `ztl2ha`, `ztl2gc`, `ztl2ga`, `zsc`, `zsp`; configs `D:/GnssProcessorSupport/worktrees/ztl2_arming/`.
- **gates (frozen at pre-registration):** section 3; section 6.3 for sections 6 and 7

**Outcome.** Question: does the wet delay as a column of the float vehicle's station block track the sim truth closer and stay neutral on the field? Built, default off: the vehicle-aware interlock, an erase-and-rebirth event response and (section 6) identity rows at the re-coupling edge, with the two erase gaps closed. Alone, the link is smoother but over-confident on the sim (NEES 2.8). Paired with the phase colour state: field user within 0.4 mm, census 0.0 mm; sim error 21 to 30 % below the control, NEES 0.8 from hour 3; the 4 mm sim bar and one count clause fail. Standing: closed; armed as a pair on 2026-11-09 (section 8).

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

The baseline pair (`runs/_rf0hv17_field6`, `_rf0hv17_sim6`, trace `logs/network_zwd.csv`) shows a wet delay that moves 2.6 mm (field) and 3.2 mm (sim) per 30 s against 0.17 mm for a random walk at the table rate (1e-9 m^2/s), a formal sigma of 5.4 and 8.1 mm, a sim error against the fixture truth of 27 mm rms in hour 2 and 7 mm from hour 4, and an event sink: at a slip epoch the white column absorbs the station misfit (HKSL00HKG +0.740 m at TOW 262800, KIT300UZB +0.260, -0.086, -0.169 m at 263670 to 263730, recomputed here from `_rf0hv17_field6`). The `carried` column of `network_zwd.csv` is 0 on every row of both goldens. The question: does the wet delay as a column of the float vehicle's station block, at a physical random-walk rate and with a declared event response, converge faster and track the truth closer on the sim, without moving the user or the census on the field?

## 2. The design

### 2.1 What exists and what blocks it

Every piece of the in-block route is already in the code and is live under the float vehicle except the loader:

- `NetworkKnobs::pl_local_block` is the local-block knob or the vehicle (`pl_blk_from_vehicle`, `src/core/network/driver/Knobs.cpp`), `pl_blk_on = pl_local_block && carry_cpl_cond`, and `zwd_blk_on = zwd_in_block && pl_blk_on`. Under `float_kf` with `carry_zwd_in_block` read true, the route is live.
- PASS B pushes the carried wet delay as one more `LocalBlockCol` (`is_zwd`) of the station block (`networkStationLocalBlock`, `src/core/network/carry/Coupling.cpp`), with its diagonal `var + q * gap` and the harvested cross terms against every other carried column; the diagonal row (`networkZwdCarryPrior`) is skipped for that station-epoch (`R.pl_zwd_in_block`) and runs only as the counted fallback (`pl_blk_zwd_fallback_diag`) when the block does not factor or the station has no block.
- The harvest appends the wet-delay column as the last index of the station's harvest block (`networkHarvestStationBlock`, `src/core/network/carry/LocalCarryHarvest.cpp`), and `networkHarvestZwd` stashes the pure conditional variance, the border regression and the block row in `net::StationState::zwd`.
- The prior-residual census family `zwd` (armed in both goldens, `prior_residual_families` "pl,pb,cb,zwd,rxcb") is fed by that harvest whenever the carry is on, with `fold_var` from `zwdQEff`, the same q the fold used.

The blocker is the interlock in `src/core/config/GnssConfigLoadNetwork.cpp`: `carry_zwd_in_block` is disabled unless `network_phase_leveling_local_block_` is set, and earlier in the same function the float vehicle forces that member false (the F3 refusal). The receiver code-bias in-block interlock tests `!float_kf && !network_phase_leveling_local_block_` and runs on the goldens; the slant-ionosphere and receiver-clock in-block interlocks have the same defect as the wet delay.

### 2.2 The route

1. The three interlocks (wet delay, slant ionosphere, receiver clock) accept the float vehicle as the block, as the receiver code-bias interlock does. The behaviour changes only when the knob is set; all three default false and are off on the goldens. The slant-ionosphere and receiver-clock knobs stay KEEP_OFF and are not graded here on the vehicle (their ledger rows say so).
2. `network.carry_zwd` without `network.carry_zwd_in_block` on the float vehicle is a hard loader error: the diagonal row inside a station whose ambiguities fold as one block is the falsified station-common double count (`ZWD-SINK-CENSUS`), and a silent fallback would measure exactly that.
3. The wet delay joins the block through the receiver code-bias route: no new contribution kind, no new column kind, no new knob. The column is `BlockZwdCarry`; its diagonal is the stashed conditional variance plus `zwd_carry_q * gap`, with no variance floor and no carry variance factor (both are keyed to the ambiguity family).

### 2.3 The process noise

`network.zwd_carry_q` (registry class `station_zwd`, fallback 1e-9 m^2/s, fingerprinted). The fixture's own walk is about 5e-8 m^2/s; the field wet delay at mid latitudes is usually quoted at 1e-8 to 1e-7 m^2/s (3 to 10 mm per sqrt hour). Arms bracket 1e-8, 5e-8 and 2e-7. No new key: the card recommends a value for the ledger row of `network.zwd_carry_q` under this route.

### 2.4 Harvest and stash

Unchanged: value `x_lin + x_r(col)`, the pure conditional variance `(N_rr^-1)_zz` and the regression on the border through `networkHarvestColumn`, the block row through the station's cross-gather, the gap prune against the ambiguity horizon (`network.phase_leveling_max_gap_s`). One addition, measure-only: under the event response the stash that creates an entry (none existed: the first sighting, a gap prune, or an event drop) stamps `relevel_s` on it, so the next fold is recognised as the re-coupling edge. An ordinary re-stash writes a fresh entry with `relevel_s` 0.

### 2.5 The event response (standing invariants 1 and 7)

A stiff time-linked wet delay carried across an event would hand the event epoch's station misfit to the ionosphere and the clock. The response restores the column to its birth state for one epoch. The column has no a priori at birth (it is born at zero with no prior row), so its birth variance is unbounded and the response is an erase-and-rebirth of the station's carried entry: the column folds no prior in the event epoch, the harvest of that epoch re-creates the entry from that epoch's posterior (a value of one epoch of data at its honest conditional variance, not a stiff re-seed), and the next epoch folds it again.

Decoupling edge, two feeds, both station-scoped:

- **Slip latch** (`events.slip.relevel.free_ambs`, read serially after PASS A and before PASS B, in the float vehicle's drop pass `networkAmbFloatDropEpoch`): a station with at least one latched arc whose cause is a slip (MW, LLI, IF-TD, the geometry-free jump, the harness tag) has its wet-delay carry erased before its block is built. An arc latched only by a re-entry response (gap re-entry, wind-up restart) is an arc event, not a station misfit, and is not a feed (the split is the latch's own, `AmbDropLatchSlip` against `AmbDropLatchReentry`).
- **RAIM attribution** (`networkPostFitRaim`, the per-arc response `refloat_sat_at` with `statistical_arc` true: the T4 attribution and the prior channel's line-of-sight flags): the attribution runs after the harvest, so the erase acts on the next epoch, the first fold that can act on it (the producer AR suspect stamp follows the same rule). The satellite-wide calls (a bad satellite refloated at every station, the T3 consensus) are not a feed: a satellite fault is not a station misfit, and erasing every tracking station's wet delay would be a network-wide decoupling.

Re-coupling edge: the first fold of a re-created entry (counted for every creation: first sightings, gap prunes and event drops; the run-end line prints the three populations beside it).

Counters (`ZwdCarryCounters`): `n_zwd_event_drop_slip`, `n_zwd_event_drop_raim`, `n_zwd_recouple`; response-table rows `zwd_slip_erase` and `zwd_raim_erase` (kind `zwd`, event `slip`, policy `erase_and_rebirth`); one run-end line on the local-block report; identities in the events trace (tier `ZWD`, one row per station and edge). The receiver clock's in-block carry was given a jump gate rather than an arc-event erase because a station-wide erase on every arc event would leave the channel unarmed on a slipped bed (the comment in `Raim.cpp`); the wet delay takes the arc-event erase because the field rate is low (on `_rf0hv17_field6`, 456 latched slip drops and 107 RAIM drops over 41,760 station-epochs, so at most about 1.3 % of station-epochs) and the gates count the erases.

### 2.6 What stays off and byte-identical

Every path is gated on `carry_zwd_in_block` (and the response on the float vehicle with the in-block route live). The response-table rows add run-end and run-manifest text on every run (declared, never a CSV); `products.txt` and every pre-existing `logs/*.csv` are unchanged with the knobs off.

## 3. Gates (pre-registered)

- **G1 build and tests.** Debug `gnss_tests.exe` 0 failures with the new suite (the block gains a wet-delay column under the vehicle with the right variance and cross terms; the interlock accepts the pair under the vehicle and refuses the diagonal alone; the event response erases a flagged station's carry and leaves the rest; off is identical); `nom_epoch_lint.py`, `nom_n4_verify.py`, `make_golden_config.py --verify`, `docs_index_check.py` pass.
- **G2 off identity.** 2 h field and sim golden copies on `gnss_processor_ztl.exe` against `_rap_srn_ctl_f2` and `_rap_srn_ctl_s2` (review executable): `products.txt` without `#` lines and every pre-existing `logs/*.csv` identical.
- **G3 sim q bracket (2 h).** Arms with `carry_zwd` + `carry_zwd_in_block` at q 1e-8, 5e-8, 2e-7 and a same-executable control. Each station's `zwd_m` against `truth_tropo.csv` by hour (rms, p50 |e|, NEES with `std_m`); the consumer `logs/closed_loop_trace.csv` `pos_err_3d_m` p50 by hour within 2 mm of the control.
- **G4 sim 6 h** at the chosen q: hour-2 rms under 12 mm (baseline 27 from the run start of the fixture; the 00:30:15 control is reported beside it), hours 4 to 6 under 4 mm (baseline 7), NEES 0.7 to 1.3 by hour, consumer p50 within 2 mm of the control by hour.
- **G5 field 6 h** at the chosen q: the user (WTZZ00DEU) within 2 mm of the control by hour; the wet-delay epoch-to-epoch sd (hours 2 to 6, median over stations) from 2.6 mm toward 1 mm; the HKSL00HKG and KIT300UZB steps gone or declared by the response counter; the 16-site census median within 0.5 mm.
- **G6 integer reading.** `scripts/simnet_amb_integer.py --ref-free --by-age` on the 6 h sim arm and its control: the L1C and L2W reference-free median |e| by chord age.

## 4. Results (2026-11-07)

Executable `gnss_processor_ztl.exe`, the Release build of commit `986bd89` (sha256[:16] `c98f567e94a31006`). Arms under `D:/GnssProcessorSupport/runs_nom/`. Sim arms start at 00:30:15 (`start_offset_hours` 0.504167) and are graded by hour from their start; field arms from 00:00. The wet delay is graded with a script of this card (each station's `zwd_m` of `logs/network_zwd.csv` against `truth_tropo.csv` at the same epoch: rms, median |e|, NEES with `std_m`; the epoch-to-epoch difference sd per station, median over stations).

### 4.1 G1, build and checks

- Debug `gnss_tests.exe`: 3,843 cases in 524 suites, 3,810 passed, 33 skipped (fixture-gated), 0 failed (10 cases and one suite more than CLAUDE.md's count, the new `ZwdTimeLinkFloatVehicleTest`). The 11 cases of `CarryZwdInBlockTest` pass unchanged.
- `nom_epoch_lint.py` PASS (V12: 95 entity writers, 95 rows); `nom_n4_verify.py --whole` PASS; `make_golden_config.py --verify` PASS (sim 78, field 82, BeiDou 76 armed in regime, 0 failures); `ledger_render.py --check` matches; `config_posture_check.py --verify-loader` 0 coverage gaps; `docs_index_check.py` OK.

**G1: PASS.**

### 4.2 G2, off identity

The golden copies on `gnss_processor_ztl.exe` against the review executable's control arms: `products.txt` without `#` lines identical, and all 34 pre-existing `logs/*.csv` identical once the wall-clock columns are blanked (`ms` of `network_datum_graph.csv`, `loop_ms` and `ms_per_epoch` of `network_run_summary.csv`, `ms_per_epoch_window` of `network_service_health.csv`; every other column of those three files is identical). No new trace file.

| pair | products.txt | logs/*.csv |
| --- | --- | --- |
| `_rap_ztl_off_f2` / `_rap_srn_ctl_f2` | identical (3,759,133 bytes) | 34 of 34 |
| `_rap_ztl_off_s2` / `_rap_srn_ctl_s2` | identical (2,614,453 bytes) | 34 of 34 |
| `_rap_ztl_ctl_f6` / `_rap_srn_ctl_f6` | identical (11,344,396 bytes) | 34 of 34 |
| `_rap_ztl_ctl_s6` / `_rap_srn_ctl_s6` | identical (7,814,424 bytes) | 34 of 34 |

The run-end text differs by the two response-table rows (declared). **G2: PASS.**

Smoke (10 epochs, q 5e-8): sim 360 wet-delay block columns, 0 diagonal fallbacks, 0 missing cross terms; field 282 columns, 0 fallbacks, 7 slip-latch erases and 1 RAIM erase.

### 4.3 G3, the sim q bracket (2 h)

| arm | q (m^2/s) | hour-2 rms (mm) | hour-2 median |e| | hour-2 NEES | epoch-to-epoch sd (mm) | hour-2 formal sigma p50 (mm) | consumer 3D p50 h1 / h2 (m) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `_rap_ztl_off_s2` (control) | | 10.07 | 5.98 | 0.70 | 3.34 | 10.99 | 0.0994 / 0.0326 |
| `_rap_ztl_q1e8_s2` | 1e-8 | 8.30 | 5.36 | 1.73 | 0.70 | 6.47 | 0.0997 / 0.0316 |
| `_rap_ztl_q5e8_s2` | 5e-8 | 7.92 | 4.84 | 1.13 | 0.96 | 7.57 | 0.0997 / 0.0313 |
| `_rap_ztl_q2e7_s2` | 2e-7 | 8.44 | 5.23 | 0.95 | 1.38 | 8.51 | 0.0998 / 0.0321 |

Hour 1 is the start transient (rms 173 to 174 mm on every arm); the carry acts on 94 % of the station-epochs of hour 1 and all of hour 2. The consumer moves by at most 1.3 mm. q 1e-8 is over-confident already in hour 2 (NEES 1.73); 5e-8, the fixture's own walk, has the smallest error. **Chosen q: 5e-8.** G3 as a selection gate: PASS (hour-2 rms below 12 mm on every arm, consumer within 2 mm).

### 4.4 G4, the sim at 6 h

| hour | control rms / NEES | q 5e-8 rms / median / NEES | q 2e-7 rms / NEES | consumer 3D p50 control / q 5e-8 (m) |
| --- | --- | --- | --- | --- |
| 1 | 174.2 / 1.02 | 173.1 / 13.3 / 1.27 | 173.2 / 1.16 | 0.0994 / 0.0997 |
| 2 | 10.07 / 0.70 | 7.92 / 4.84 / 1.13 | 8.44 / 0.95 | 0.0326 / 0.0313 |
| 3 | 8.30 / 0.85 | 7.16 / 4.62 / 1.99 | 7.54 / 1.51 | 0.0398 / 0.0392 |
| 4 | 7.43 / 0.79 | 6.36 / 4.11 / 2.33 | 6.75 / 1.67 | 0.0160 / 0.0160 |
| 5 | 7.66 / 0.90 | 6.39 / 4.19 / 2.82 | 6.85 / 1.98 | 0.0180 / 0.0178 |
| 6 | 6.34 / 0.69 | 5.51 / 3.70 / 2.43 | 5.75 / 1.63 | 0.0152 / 0.0152 |

(rms and median in mm; runs `_rap_ztl_ctl_s6`, `_rap_ztl_on_s6`, `_rap_ztl_q2e7_s6`.) The epoch-to-epoch sd falls from 3.24 to 0.81 mm (hours 2 to 6) and the formal sigma from 7.4 to 3.4 mm in hour 6, but the error falls only by 13 to 17 %. Hours 4 to 6, per station: the bias (the station's mean error) is 12 % of the mean square on the control and 14 % on the arm; the rest is a slow wander, sd 6.2 mm about the station mean on the control and 5.4 mm on the arm, while consecutive epochs move 0.7 mm. The error the time link leaves is low-frequency and common to both arms; the carry cannot average it down, and treating the rows as white it shrinks the formal sigma below it.

The phase error of the fixture is coloured by latitude class (card SFR). A diagnostic pair with the per-line-of-sight phase colour state armed (`network.phase_colour_state`, not in the goldens), control `_rap_ztl_pcctl_s6` and arm `_rap_ztl_pcon_s6` (q 5e-8):

| hour | colour control rms / NEES | colour + time link rms / median / NEES | consumer 3D p50 (m) |
| --- | --- | --- | --- |
| 2 | 9.41 / 0.41 | 7.58 / 4.88 / 0.61 | 0.0329 / 0.0320 |
| 3 | 7.56 / 0.44 | 6.36 / 4.21 / 0.77 | 0.0399 / 0.0396 |
| 4 | 6.70 / 0.41 | 5.46 / 3.58 / 0.79 | 0.0163 / 0.0161 |
| 5 | 6.95 / 0.46 | 5.33 / 3.60 / 0.86 | 0.0185 / 0.0183 |
| 6 | 6.06 / 0.40 | 4.99 / 3.36 / 0.84 | 0.0149 / 0.0149 |

With the colour state the time link's NEES is 0.77 to 0.86 from hour 3 and the hours 4 to 6 rms is 5.0 to 5.5 mm (18 to 23 % below its control); the over-confidence of the plain arm is the white-noise model meeting a coloured error, not the carry's algebra.

**G4: FAIL.** Hour-2 rms 7.92 mm passes (below 12 mm; the control at this start is 10.07 mm, the baseline's 27 mm is from the fixture start); hours 4 to 6 rms 6.36, 6.39, 5.51 mm fail the 4 mm bar (5.46, 5.33, 4.99 mm with the colour state); NEES by hour fails from hour 3 (1.99 to 2.82; q 2e-7 1.51 to 1.98; with the colour state 0.77 to 0.86, a pass from hour 3); the consumer is within 1.3 mm by hour (pass).

### 4.5 G5, the field at 6 h

`_rap_ztl_ctl_f6` and `_rap_ztl_on_f6` (q 5e-8).

| hour | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| WTZZ00DEU 3D p50, control (m) | 0.0588 | 0.0123 | 0.0231 | 0.0123 | 0.0165 | 0.0120 |
| WTZZ00DEU 3D p50, arm (m) | 0.0586 | 0.0118 | 0.0241 | 0.0135 | 0.0166 | 0.0113 |
| wet-delay epoch-to-epoch sd, control / arm (mm) | 63.0 / 60.0 | 2.73 / 0.89 | 2.67 / 0.65 | 2.34 / 0.68 | 2.49 / 0.61 | 2.50 / 0.62 |
| formal sigma p50, control / arm (mm) | 18.2 / 15.6 | 7.1 / 5.0 | 5.6 / 3.3 | 5.2 / 2.9 | 5.1 / 2.8 | 4.9 / 2.7 |

- User: largest change +1.2 mm (hour 4). Pass.
- Epoch-to-epoch sd, hours 2 to 6, median over stations: 2.59 to 0.73 mm. Pass.
- Steps above 5 cm in hours 2 to 6: 6 on the control, 0 on the arm. HKSL00HKG: -0.773 / +0.740 m at TOW 262770 / 262800 on the control, -0.050 / +0.042 m on the arm; the station had a slip latch at 262740 (carry erased, 262770 is the re-coupling fold) and a RAIM per-arc attribution (E04) at 262770 (262800 folds no prior). KIT300UZB: +0.260, -0.086, -0.169 m at 263670 to 263730 on the control, +0.011, +0.008, -0.020 m on the arm; slip latch at 263640, RAIM attribution (G21) at 263700. The excursions are gone; the remaining 2 to 5 cm moves sit at declared response epochs. Pass.
- Response load: 241 slip-latch erases and 29 RAIM erases over 41,760 station-epochs (0.65 %); 244 re-coupling folds; 41,082 wet-delay block columns, 0 diagonal fallbacks, 229 missing cross terms. The `zwd` prior-residual family: sigma0 0.46 (field), 0.56 (sim): the folded prior is looser than the epoch-to-epoch increments, while against the sim truth the posterior is tighter than the low-frequency error (section 4.4): the census sees the increments, not the wander.
- 16-site census (`scripts/clock_level/gcensus.py`, keys `ztlc` on the control wire and `ztla` on the arm wire, same executable, 17 replays each, all exit 0): median site p50 by hour 0.142, 0.050, 0.036, 0.029, 0.032, 0.030 m (control) and 0.143, 0.050, 0.036, 0.028, 0.032, 0.030 m (arm); hours 2 to 6 paired median change -0.0000 m, 0 better and 0 worse at 1 cm. Largest site moves: ARHT +3.4 mm, DAV1 +2.5 mm (worse), ANMG -0.5 mm; above the 0.19 mm neutral spread of card NSP, so these two are real small responses. Pass (median within 0.5 mm).

**G5: PASS.**

### 4.6 G6, the integer reading

`scripts/simnet_amb_integer.py --ref-free --by-age` on `_rap_ztl_ctl_s6` and `_rap_ztl_on_s6`:

| | control | arm |
| --- | --- | --- |
| reference-free median |e|, L1C / L2W, whole run (cycle) | 0.103 / 0.138 | 0.103 / 0.138 |
| same, last 25 % of epochs | 0.084 / 0.120 | 0.084 / 0.120 |
| reference-free WL(1,2) median |e| | 0.033 | 0.033 |
| L1C median |e| by chord age [1,10) [10,30) [30,100) [100,300) [300,1000), runs of 200 epochs or more | 1.278 0.508 0.280 0.144 0.121 | 1.268 0.508 0.276 0.142 0.119 |
| L2W, same | 1.566 0.615 0.337 0.186 0.150 | 1.549 0.605 0.335 0.184 0.147 |
| log-log slope L1C / L2W | -0.478 / -0.471 | -0.480 / -0.472 |

The narrow-lane convergence does not improve measurably: 0.002 to 0.017 cycle by age bin, slope unchanged. The carried ambiguities already transmit the time information the wet delay's link adds (the review's increment ratio 0.34). **G6: no improvement (a reading, not a bar).**

### 4.7 Reading and recommendation

The route works on the float vehicle as built: every station-epoch folds the wet delay inside its block, the field excursions at slip epochs are gone, the user and the census do not move, and the wet delay is four times smoother on both beds. The truth grade fails its accuracy bar for hours 4 to 6 and, without the phase colour state, its consistency bar: the error left is a low-frequency wander common to the white and the linked estimate, and a random-walk carry over white-noise rows reports a sigma below it. The card recommends leaving `network.carry_zwd` / `network.carry_zwd_in_block` off on the goldens for now; arming them is the author's decision, and the evidence favours arming them together with `network.phase_colour_state` (NEES 0.77 to 0.86), which needs its own field grade.

Owed: (a) a field 6 h arm and census with the phase colour state and the time link together, and its control; (b) a 24 h sim grade (the wander's time scale against the 6 h window); (c) the slant-ionosphere and receiver-clock in-block carries are now reachable under the float vehicle and ungraded there; (d) the RAIM within-epoch replay withholds carried ambiguity priors but not the wet delay's (not a feed of this response).

## 5. Second-model verification (2026-11-07)

One Opus referee, read-only, reproduced every decision-driving number from the run directories (twelve claims confirmed, one partly): the off identity on all four pairs; the sim truth grade at q 5e-8 (hour 2 rms 7.92 mm against 10.07; hours 4 to 6 6.36, 6.39 and 5.51 mm; NEES to 2.82) and the q bracket; the field user (at most 1.2 mm by hour), the removal of the HKSL00HKG and KIT300UZB excursions and the erase counts; the census (median 0.0 mm, ARHT00ATA +3.4 mm, DAV1 +2.5 mm); the unchanged reference-free integer reading (L1C 0.103, L2W 0.138 cycle); the interlock, the seam, its `kEntityWriters` row, the per-arc RAIM erase, the counters, the ledger rows and the test suite (`ZwdTimeLinkFloatVehicleTest`, ten cases). The decision stands: open, not recommended for arming alone.

The partly confirmed claim is standing invariant 7 at the re-coupling edge. The decoupling edge is wired, counted and identified (241 slip and 29 RAIM rows on the field). The re-coupling edge is counted (`n_zwd_recouple`) but (i) emits no identity row in `network_events.csv`, against section 2.5's "one row per station and edge"; (ii) lumps first sightings and gap prunes with event re-couplings, and the run-end line prints no first-sighting count, so the event re-couplings appear only by subtraction (244 minus 58 on the field); (iii) misses a re-coupling when the first harvest after a re-creation does not inject (the re-stash resets `relevel_s`): a replay of the event trace predicts 248 field re-couplings against 244 logged (the sim matches at 63). Further findings: the RAIM erase sits inside the per-ambiguity loop of `refloat_sat_at`, so a per-arc attribution with no carried ambiguity in the pool leaves the wet delay in place; with `network.raim_prior_channel` armed the wet delay's own prior row is flagged `prior_unwired` with no response (not on the goldens, ungraded); the reborn entry is seeded from the event epoch's own posterior and re-folded without re-validation (HKSL00HKG still steps -5 cm at the re-coupling fold before the RAIM erase); two ledger rows keep "F3-only" text now stale under the float vehicle, with the note appended after it; the sim hours are counted from the first output epoch rather than from 00:30:15 (at most 0.05 mm, no verdict changes). Before any arming: identity rows at the re-coupling edge, the first-sighting count printed, and the two erase gaps closed.

## 6. The pairing with the phase colour state, and the re-coupling edge: pre-registration (2026-11-09)

Two items are owed from sections 4.7 and 5: the field grade of the wet-delay link paired with the phase colour state, and identity rows at the re-coupling edge. Written before any graded run of this section. Main `27d6ece` (the AVX2 build); branch `ztl/colour-pairing`; worktree `D:/GnssProcessorSupport/worktrees/ztl2`.

### 6.1 The build

Default off, inside the existing key (`network.carry_zwd_in_block` under the float vehicle, the response's arming `NetworkKnobs::zwd_event_on`); no new key.

1. **Identity rows at the re-coupling edge** (measure-only). One record per station (`net::StationState::zwd_edge`, `network/carry/ZwdEdge.hpp`, a declared `ColdStart` field) holds the cause and second of the last decoupling edge (slip-latch erase, RAIM per-arc erase, prior-channel erase, gap prune) until the first fold of the entry the harvest re-creates. Each closed edge writes a row of tier `ZWD` in `network_events.csv`: event `recouple` (cause, seconds away, for a RAIM erase the satellite and the drop cause; the statistic is the wet delay's step at the fold, the threshold the folded prior's sigma) or `first_fold` for a station's first entry (a birth, never a re-coupling). A kept entry that folds after more than 1.5 epochs without a stash (a station returning within the gap horizon) is a re-coupling of cause `return_in_horizon`. The gap prune writes a decoupling row `gap_prune`. Counters by cause: re-couplings, supersessions (a re-created entry removed again before its first fold) and records open at the run end; the run-end line prints each decoupling feed beside its outcomes, so every decoupling is accounted for, and the first folds apart. The record stays open until the re-created entry actually folds, which closes the missed re-couplings of section 5 (iii). The stamp `relevel_s` on the wet-delay entry, read by nothing else, is no longer written.
2. **The two erase gaps of section 5** (on-path, key on only). (a) The RAIM per-arc erase of the wet delay runs once per flagged arc, outside the loop over the station's pooled ambiguities, so a flag whose satellite has no ambiguity in the pool also erases. (b) A prior-channel flag on the wet delay's own block row (`block_zwd_carry`) takes the same response (erase, decoupling row `raim_prior`, counter `n_zwd_event_drop_prior`, response site `zwd_raim_erase`) instead of the unwired count. `network.raim_prior_channel` has been armed in all three goldens since 2026-11-07, so (b) acts on every key-on golden arm; section 5 graded it as not on the goldens.

Not in this build: the RAIM within-epoch replay's handling of the wet delay's prior (section 4.7 (d)) and a re-validation test at the re-coupling fold (section 5: the reborn entry is re-folded unchecked); the new rows measure the second.

### 6.2 Arms

Executable `gnss_processor_ztl2.exe`, the Release (AVX2) build of the branch; sha256 recorded with the results. Runs under `D:/GnssProcessorSupport/runs_nom/_rap_ztl2_*`. Every arm is a copy of `config/golden/golden_{field,sim}_refound.json` with only the keys below and the run paths changed; the sim arms add `network.start_offset_hours` 0.504167 (00:30:15) and are graded by hour from that second; the field from 00:00.

| arm | keys |
| --- | --- |
| `ctl` | none (the golden) |
| `zl` (the link alone) | `network.carry_zwd`, `network.carry_zwd_in_block` true, `network.zwd_carry_q` 5e-8 |
| `pc` (the colour state alone) | `network.phase_colour_state` true, `network.trace_phase_colour` true |
| `pz` (the pair) | the union of `zl` and `pc` |

`network.trace_phase_colour` is a trace key (measure-only); it rides on both colour arms for the reading of section 6.6. Development: the four arms at 2 h on each bed (`_rap_ztl2_{arm}_{f2,s2}`), read for mechanics and not graded. Grade: the four arms at 6 h on each bed (`_rap_ztl2_{arm}_{f6,s6}`). A 10-epoch smoke of `pz` on each bed precedes the runs. At most two processes at once.

### 6.3 Gates (frozen here)

- **G2b, off identity.** `_rap_ztl2_ctl_f6` against the baseline `_rap_srn_f6` (executable `gnss_processor_srnint.exe`, the same source as main, the same golden), and `_rap_ztl2_ctl_s2` against `_rap_ztl2_base_s2` (the baseline executable copied as `gnss_processor_ztl2base.exe`, the same arm): `products.txt` without `#` lines and every `logs/*.csv` identical once the wall-clock columns are blanked.
- **G2c, the identity rows are measure-only.** The 2 h field link arm on the baseline executable (`_rap_ztl2_zlbase_f2`) against `_rap_ztl2_zl_f2`: up to the first epoch at which an erase of build item 2 acts, `products.txt` is identical and the only new or changed rows of `network_events.csv` are the declared `ZWD` rows (`recouple`, `first_fold`, `gap_prune`); every later difference starts at such an erase.
- **G4, the sim at 6 h, on the pair** (the bar of section 3 as written): hour-2 rms below 12 mm, hours 4 to 6 rms below 4 mm, NEES 0.7 to 1.3 by hour, the consumer's 3D p50 within 2 mm of `ctl` by hour. The colour arm alone and the link alone are reported beside it.
- **G5, the field at 6 h, on the pair** (the bar of section 3 as written): WTZZ00DEU within 2 mm of `ctl` by hour; the wet delay's epoch-to-epoch sd (hours 2 to 6, median over stations) from 2.6 mm toward 1 mm; the HKSL00HKG and KIT300UZB steps gone or declared by the response counter; the 16-site census median within 0.5 mm.
- **G7, the field bars for the pair** (6 h, `pz` against `ctl` on the same executable):
  - G7a, the held-out consumer by hour: WTZZ00DEU 3D p50 within 2 mm of `ctl` in each of hours 2 to 6; hour 1 (ignition) reported, not graded.
  - G7b, the 16-site census (`scripts/clock_level/gcensus.py`, keys `ztl2c` on the `ctl` wire and `ztl2p` on the `pz` wire, 17 replays each): the paired median change of the site mean p50 over hours 2 to 6 within 0.5 mm, and no site worse by more than 1 cm; every site that moves more than the 0.19 mm neutral spread of card NSP is named.
  - G7c, smoothness: the wet delay's epoch-to-epoch sd, hours 2 to 6, median over stations, at most 1.0 mm (`ctl` about 2.6 mm), and no step above 5 cm in hours 2 to 6 that is not at a declared response epoch.
  - G7d, refloats and slip responses: the counts of `T4_attrib` and `T1_if_slip` rows in `network_events.csv` within 20 % of `ctl`'s; the wet delay's erase load (slip-latch, RAIM per-arc and prior-channel erases) at most 1.5 % of station-epochs; the colour state's RAIM erases reported.
- **G8, the re-coupling edge on every key-on arm** (2 h and 6 h, both beds): for each decoupling cause, decouplings equal re-couplings plus supersessions plus records open at the run end; the `ZWD` `recouple` rows equal `n_zwd_recouple` and the `first_fold` rows the first-sighting count; each `gap_prune` row is followed by the station's `recouple` row of cause `gap_prune` or by an open record.
- **G9, the injected events** (2 h field pair arm `_rap_ztl2_ev_f2`, the field golden plus the `pz` keys, a copy of the pair arm with these station files replaced): (i) CEBR00ESP absent for 600 s (inside the 900 s horizon): a `TOPOLOGY` `sta_readmitted` row and one `ZWD` `recouple` row of cause `return_in_horizon` at the return epoch with seconds away about 630; no `first_fold` row for it; (ii) REYK00ISL absent for 1500 s: one `gap_prune` row about 900 s after its last epoch and one `recouple` row of cause `gap_prune` at the first fold after its return; (iii) STJO00CAN with integer-cycle steps injected on its GPS L1 phase (`slip;types=L1?;stations=STJO00CAN;rate=0.5;max=1;seed=1`): each latched epoch has its `slip_latch` erase row and the next epoch its `recouple` row of cause `slip_latch` with seconds away 30 (or a supersession counted). The outages are made by removing the window's epochs from a copy of the station's RINEX file (all systems), recorded in the arm directory.

Reading rule: G4 and G5 are this card's bars; G7 is the field grade owed by section 4.7; G8 and G9 close section 5's verification. A failed clause is reported as failed.

### 6.4 The question for card LGM

Card LGM section 5.3 records that the colour state is starved (its estimate holds about a fifth to a quarter of the truth's amplitude, G1 ratio 0.87 to 0.91) because the float vehicle's carried ambiguity is not phase-stiff on mature arcs, and that its gates are unreachable until it is. Prediction, written here: the pairing does not change that reading. The wet-delay link removes a station-common white column that the colour state could not absorb anyway (the colour is line-of-sight specific); the carried ambiguity's information loss is untouched. On `_rap_ztl2_pc_s6` and `_rap_ztl2_pz_s6` the LGM G1 ratio (rms of the estimate's error over the truth's rms, arcs older than 10 minutes, by latitude class) stays above 0.8 on both, and differs between them by less than 0.03.

## 7. Results (2026-11-09)

Executable `gnss_processor_ztl2.exe` (sha256[:16] `bc5cef3a9cca9c7f`), the Release (AVX2) build of the sources of commit `45789af` on branch `ztl/colour-pairing` (that commit's `/bigobj` line for one Debug test was added after the Release build and does not touch it). Baseline executable `gnss_processor_ztl2base.exe`, a copy of `gnss_processor_srnint.exe` (sha256[:16] `64543d2d89994cae`, the source of main `27d6ece`). Runs under `D:/GnssProcessorSupport/runs_nom/_rap_ztl2_*`; census keys `ztl2c` (control wire) and `ztl2p` (pair wire). Every number below was read from the run directories with the card's scripts (`scripts/ztl_zwd_grade.py`, `ztl_identity.py`, `ztl_colour_grade.py`) and `scripts/clock_level/gcensus.py`. Sim hours count from 00:30:15, field hours from 00:00. 58 field stations by 720 epochs make 41,760 station-epochs.

### 7.1 Build, tests and off identity (G1, G2b, G2c)

- Debug `gnss_tests.exe`: 4,191 cases in 554 suites, 4,154 passed, 37 skipped (fixture-gated), 0 failed, with the new suite `ZwdCoupleEdgeTest` (14 cases: each cause's re-coupling, the first sighting apart, the record kept open across an epoch that does not fold, the return within the horizon and its non-cases, supersession, the accounting closure, the erase helper, the identity row's text, the prior channel's split). `tests/test_code_datum_family.cpp` takes `/bigobj` (the new station field tipped it over the Debug section limit, C1128). `nom_epoch_lint.py` PASS (V12: 102 entity writers, 102 rows); `nom_n4_verify.py --whole` PASS.
- **G2b, off identity: PASS.** `_rap_ztl2_ctl_f6` against the baseline `_rap_srn_f6`: `products.txt` identical (348,247 lines) and 35 of 35 `logs/*.csv` identical with the wall-clock columns blanked. `_rap_ztl2_ctl_s2` against `_rap_ztl2_base_s2` (baseline executable, same arm): 80,690 lines and 35 of 35 identical.
- **G2c, measure-only: PASS.** `_rap_ztl2_zl_f2` against `_rap_ztl2_zlbase_f2` (the link arm on the baseline executable): `products.txt` identical over the whole 2 h (115,712 lines; no erase of build item 2 acted in that window), 34 of 35 CSVs identical, and the only new rows of `network_events.csv` are 56 `ZWD first_fold` and 66 `ZWD recouple`. The baseline's run-end counter read 121 re-coupling folds for these 122 (56 first folds and 66 re-couplings): the one re-coupling the old stamp missed.
- Smoke (10 epochs, the pair): field 7 slip erases (3 re-coupled, 3 superseded, 1 open), 1 RAIM erase re-coupled, 56 first folds plus 2 superseded first sightings (58 stations); sim 60 first folds.

### 7.2 The sim at 6 h (G4 on the pair)

| hour | control rms / NEES | link alone rms / NEES | colour alone rms / NEES | pair rms / median abs / NEES | consumer 3D p50, control / pair (m) |
| --- | --- | --- | --- | --- | --- |
| 1 | 174.95 / 1.02 | 173.86 / 1.27 | 174.98 / 0.70 | 173.89 / 12.66 / 0.75 | 0.0993 / 0.0993 |
| 2 | 10.11 / 0.70 | 7.97 / 1.13 | 9.45 / 0.41 | 7.61 / 4.90 / 0.61 | 0.0326 / 0.0320 |
| 3 | 8.30 / 0.85 | 7.16 / 1.99 | 7.55 / 0.43 | 6.35 / 4.19 / 0.76 | 0.0400 / 0.0398 |
| 4 | 7.45 / 0.79 | 6.38 / 2.33 | 6.71 / 0.41 | 5.47 / 3.58 / 0.79 | 0.0158 / 0.0159 |
| 5 | 7.67 / 0.89 | 6.42 / 2.81 | 6.99 / 0.46 | 5.38 / 3.64 / 0.86 | 0.0181 / 0.0183 |
| 6 | 6.45 / 0.71 | 5.63 / 2.47 | 6.13 / 0.40 | 5.08 / 3.38 / 0.85 | 0.0155 / 0.0151 |

(rms and median in mm; runs `_rap_ztl2_{ctl,zl,pc,pz}_s6`.) The wet delay's epoch-to-epoch sd, hours 2 to 6, median over stations: 3.24 (control), 0.80 (link), 3.32 (colour), 0.77 mm (pair). The link alone reproduces section 4.4 on the current golden (hours 4 to 6 6.38, 6.42, 5.63 mm; NEES to 2.81). The colour state alone leaves the wet delay's error near the control's and states it too loosely (NEES 0.40 to 0.46); the pair has the smallest error of the four arms in every hour from hour 2 (hours 4 to 6 27 %, 30 % and 21 % below the control) and a NEES of 0.76 to 0.86 from hour 3.

**G4 on the pair: FAIL.** Hour-2 rms 7.61 mm passes (below 12); hours 4 to 6 rms 5.47, 5.38, 5.08 mm fail the 4 mm bar; NEES passes in hours 3 to 6 (0.76 to 0.86) and fails in hour 2 (0.61, below 0.7); the consumer is within 0.6 mm of the control in every hour (pass).

### 7.3 The field at 6 h (G5 and G7 on the pair)

| hour | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| WTZZ00DEU 3D p50, control (m) | 0.0581 | 0.0125 | 0.0207 | 0.0116 | 0.0171 | 0.0132 |
| link alone | 0.0582 | 0.0122 | 0.0215 | 0.0117 | 0.0168 | 0.0129 |
| colour alone | 0.0585 | 0.0133 | 0.0181 | 0.0114 | 0.0172 | 0.0140 |
| pair | 0.0585 | 0.0126 | 0.0205 | 0.0117 | 0.0171 | 0.0132 |
| wet-delay epoch-to-epoch sd, control / pair (mm) | 58.7 / 58.2 | 2.62 / 0.83 | 2.66 / 0.64 | 2.36 / 0.65 | 2.49 / 0.56 | 2.50 / 0.55 |
| wet-delay formal sigma p50, control / pair (mm) | 18.0 / 18.7 | 7.1 / 7.9 | 5.6 / 6.0 | 5.2 / 5.4 | 5.1 / 5.1 | 4.9 / 5.1 |

(runs `_rap_ztl2_{ctl,zl,pc,pz}_f6`.) The control reproduces the baseline `_rf0hv18` by hour.

- **G7a, the held-out consumer: PASS.** The pair against the control: +0.4 mm (hour 1, not graded), +0.1, -0.2, +0.1, 0.0, 0.0 mm in hours 2 to 6. The colour state alone moves hour 3 by -2.6 mm and hour 6 by +0.8 mm; the link alone at most +0.8 mm.
- **G7b, the 16-site census: PASS.** Median site p50 by hour 0.133, 0.044, 0.033, 0.028, 0.031, 0.027 m on the control wire (equal to the baseline key `c18a`) and 0.132, 0.045, 0.033, 0.028, 0.031, 0.027 m on the pair wire; hours 2 to 6 paired median change -0.0000 m, 0 sites better and 0 worse at 1 cm (hour 1: +0.0001 m, 1 better, 4 worse). Sites beyond the 0.19 mm neutral spread of card NSP (the change of the site mean over hours 2 to 6): DAV1 -4.0 mm (better), HARB +2.0 mm (worse), ASCG +0.6, POHN -0.6, STPM -0.5, ANMG -0.4, QUIN +0.4, UTQI +0.4, CHTI +0.3, BADG -0.3 mm; the other six within 0.1 mm. No site is worse by more than 1 cm.
- **G7c, smoothness: PASS.** Epoch-to-epoch sd, hours 2 to 6, median over stations: 2.55 mm (control), 0.72 (link), 2.59 (colour), 0.69 mm (pair). No step above 5 cm in hours 2 to 6 on any of the four arms.
- **G7d, refloats and slip responses: FAIL as frozen, on one clause.** `T1_if_slip` rows 155 on the control and on the pair (0 %); the prior channel's `T4_prior` rows 45 on both; `T4_attrib` rows 3 on the control and 2 on the pair (-33 %, outside the 20 % bar; the link alone keeps 3, the colour state alone has 2, so the row belongs to the colour state). The wet delay's erase load on the pair: 254 slip-latch, 40 RAIM per-arc and 0 prior-channel erases, 0.70 % of station-epochs (bar 1.5 %). The colour state's own erases on the pair: 44 by the RAIM attribution, 5 by the gross line-of-sight screen.
- **G5 on the pair:** the user within 0.4 mm by hour (pass); the wet delay's epoch-to-epoch sd from 2.55 to 0.69 mm (pass); the HKSL00HKG and KIT300UZB steps of section 4.5 no longer occur on the current golden's control (no step above 5 cm in hours 2 to 6), so that clause is moot here and holds on the pair; the census median within 0.5 mm (0.0 mm, pass). **G5 on the pair: PASS.**

### 7.4 The re-coupling edge (G8, G9)

**G8: PASS on every key-on arm** (`zl_f2`, `pz_f2`, `zl_s2`, `pz_s2`, `ev_f2`, `zl_f6`, `pz_f6`, `zl_s6`, `pz_s6`): for each cause, decouplings equal re-couplings plus supersessions plus records open at the run end; the `recouple` rows equal the re-coupling count cause by cause and the `first_fold` rows the first-sighting count. On `_rap_ztl2_pz_f6`:

| feed | decouplings | re-coupled | superseded | open at the end |
| --- | --- | --- | --- | --- |
| slip latch | 254 | 161 (all 30 s later) | 93 | 0 |
| RAIM per-arc | 40 | 23 (all 60 s later) | 17 | 0 |
| prior channel, own row | 0 | 0 | 0 | 0 |
| gap prune | 0 | 0 | 0 | 0 |

plus 2 re-couplings of cause `return_in_horizon` and 56 first folds (2 first sightings superseded). Of the 93 superseded slip erases, 85 are followed by another slip latch of the same station in the next epoch and 8 by a RAIM erase in the same epoch; the 17 superseded RAIM erases are repeated attributions in the next epoch. A third of the slip erases therefore leave the wet delay free for two or more consecutive epochs. After hour 1 the re-coupling folds are small: median step 1.1 mm (slip) and 1.2 mm (RAIM), 90th percentile 4.2 and 4.8 mm, none beyond 3 times the folded sigma; all nine steps above 2 cm fall in hour 1. A natural return is identified on every field arm: SVTL00RUS misses TOW 262770, and at 262800 a `TOPOLOGY sta_readmitted` row (1 epoch absent) and a `ZWD recouple` row of cause `return_in_horizon` (60 s away, step -3.0 mm against a 4.5 mm folded sigma) coincide.

**G9, the injected events (`_rap_ztl2_ev_f2`): PASS.**

| event | rows |
| --- | --- |
| CEBR00ESP absent TOW 263400 to 263970 (20 epochs) | 264000: `TOPOLOGY sta_readmitted` (epochs_absent 20) and `ZWD recouple`, cause `return_in_horizon`, away 630 s, step +23.4 mm against 11.4 mm; no `first_fold` |
| REYK00ISL absent TOW 264600 to 266070 (50 epochs) | 265500: `ZWD gap_prune` (930 s after its last epoch); 266100: `TOPOLOGY sta_readmitted` (50); 266130: `ZWD recouple`, cause `gap_prune`, away 630 s, folded sigma 0.80 m |
| STJO00CAN, integer steps on GPS L1 | the three steps on its modelled L1W phase (G13 at 261600 and 262380, G10 at 266340) each give a `T1_if_slip` row, a `ZWD slip_latch` erase and, 30 s later, a `ZWD recouple` of cause `slip_latch`; the steps on L1C fall on a signal the station does not model, and G14 never enters its modelled set in this run (no row in `network_slip_raw.csv`); the uninjected pair arm has no slip row at STJO00CAN |

The run-end line of the event arm closes: 97 slip erases (63 re-coupled, 33 superseded, 1 open), 5 RAIM (5 re-coupled), 1 gap prune (1 re-coupled), 2 returns, 56 first folds.

### 7.5 Card LGM's reading

| class | colour alone: ratio / correlation / rms x / rms e (mm) | pair: same |
| --- | --- | --- |
| tropical | 0.825 / +0.64 / 3.93 / 11.66 | 0.810 / +0.66 / 4.11 / 11.66 |
| mid | 0.850 / +0.59 / 2.90 / 9.01 | 0.833 / +0.62 / 3.11 / 9.01 |
| high | 0.842 / +0.59 / 2.93 / 8.46 | 0.830 / +0.61 / 3.12 / 8.46 |

(`_rap_ztl2_pc_s6`, `_rap_ztl2_pz_s6`, arcs older than 10 minutes, from 00:30:15; card LGM's 2026-10-01 read was 0.885 / 0.908 / 0.908.) The prediction of section 6.4 holds: the ratio stays above 0.8 on both arms and the pair moves it by 0.012 to 0.017. The pairing does not change card LGM's reading. The colour state still carries a third of the colour's amplitude, correlated with it (+0.6), and its gate G1 (at most 0.7) stays out of reach; the wet delay's link removes a station-common white column, not the carried ambiguity's information loss that caps the colour state (card AIL). What the pairing changes is the wet delay, not the colour: the colour state's per-line-of-sight column takes part of the phase rows' information from the wet delay (the sim's hour-6 formal sigma p50 5.36 mm on the pair against 3.47 mm for the link alone, with errors of 5.08 and 5.63 mm), and that brings the wet delay's NEES from 2.3 to 2.8 down to 0.8.

### 7.6 Reading and recommendation

The re-coupling edge is identified, counted by cause and accounted for (G8, G9), and the two erase gaps are closed; with the key off nothing moves (G2b), and with it on the identity rows are the only new output (G2c). The pair passes the field: the user within 0.4 mm in every hour, the wet delay 3.7 times smoother, no excursion, slip responses unchanged, erase load 0.70 %, the census median unchanged (0.0 mm; DAV1 -4.0 mm, HARB +2.0 mm). It fails one frozen field clause (G7d: `T4_attrib` 2 against 3, a single row, attributable to the colour state) and the sim accuracy bar of G4 (hours 4 to 6 5.1 to 5.5 mm against 4; the low-frequency wander of section 4.4 remains), with the sim NEES consistent from hour 3 and slightly under-confident in hour 2 (0.61). The link alone is measured harmful to the wet delay's consistency on the sim (NEES 2.3 to 2.8) and should not be armed alone. Cost: the colour state adds about 595,000 carried block folds per 6 h field run; wall times on this shared machine (control 2,192 s, pair 1,796 s) do not measure it.

Under the rule of 2026-11-07 the pair is finished and not measured harmful on the field: the card recommends arming `network.carry_zwd`, `network.carry_zwd_in_block` (`network.zwd_carry_q` 5e-8) and `network.phase_colour_state` together, never the link alone, with look-here notes in the ledger rows (the sim accuracy bar, hour-2 NEES, the 24 h sim grade, the RAIM within-epoch replay of section 4.7 (d), the slant-ionosphere and receiver-clock in-block carries still ungraded under the vehicle). Nothing is armed here; the decision is the author's.

## 8. Armed (2026-11-09, the author's decision)

The author decided on 2026-11-09 to arm the pair of section 7. Rule of 2026-11-07: finished keys are armed unless measured harmful; the health check below is not an adoption gate. Branch `ztl/colour-pairing`, not merged and not pushed.

### 8.1 Armed

Manifest pins on the field and sim goldens, and so on the BeiDou lineage, which extends the field golden: `network.carry_zwd` true, `network.carry_zwd_in_block` true, `network.zwd_carry_q` 5e-8, `network.phase_colour_state` true (card LGM's parameters at their defaults). Ledger rows: `network.carry_zwd` and `network.phase_colour_state` ARMED; `network.carry_zwd_in_block` stays RECOMMEND_SCOPED (mandatory wherever `carry_zwd` is on), now in regime and armed; `network.zwd_carry_q` PARAM, pinned. `network.trace_phase_colour` is not armed (a 33 MB trace per 6 h field run). Each of the three regenerated goldens differs from its previous version by these four keys and the provenance lines only; `make_golden_config.py --verify` (sim 80, field 84, BeiDou 77 recommended knobs armed in regime, 0 failures), `config_posture_check.py --verify-loader` (0 coverage gaps), `ledger_render.py --check` and `docs_index_check.py` pass. Solve-affecting; fresh convergence only (`carry_zwd_in_block` and `zwd_carry_q` are fingerprinted).

### 8.2 Health check

Executable `gnss_processor_ztl2.exe` (`bc5cef3a9cca9c7f`), Release only, 10-epoch smokes of the armed field and BeiDou goldens first (exit 0; BeiDou with the colour state on all three constellations). Controls: main's goldens at `27d6ece`; DOY 130 and 132 goldens derived with a copy of `scripts/mday_golden_config.py` pointed at each golden (obs root `D:/GnssProcessorSupport/data_mday`; DOY 132 drops MAC100AUS and LCK400IND, missing files). Every control and armed config pair differs by the four keys only. Configs and the copied scripts: `D:/GnssProcessorSupport/worktrees/ztl2_arming/`. Runs `_rap_ztl2_armg_{f6,s6}` (the regenerated goldens, DOY 129 and the sim from 00:30:15) and `_rap_ztl2_{hc,ha}_{h6,g6,b6}` (control and armed: DOY 130, DOY 132, BeiDou DOY 129). Censuses `ztl2hc` / `ztl2ha` (DOY 130), `ztl2gc` / `ztl2ga` (DOY 132), and the 16-site sim census `zsc` / `zsp` (`simcensus6h.py` run through a copy with two jobs and the runs_nom root, consumer base `_rap_ztl2_ctl_s6/arm.json`; the replays start at 01:01:30, so its hours are counted from 00:30:15 and graded over hours 2 to 6 directly from the traces). Bars as in card RF0HV18B section 5.3: H1 complete (720 of 720 emitted, nothing withheld, exit 0); H2 the held-out user's worst hour over hours 2 to 6 at most +10 mm against the control; H3 the worst census site over hours 2 to 6 at most +10 mm.

The regenerated goldens reproduce the graded pair arms: `_rap_ztl2_armg_f6` against `_rap_ztl2_pz_f6` and `_rap_ztl2_armg_s6` against `_rap_ztl2_pz_s6` have identical `products.txt` (348,249 and 241,008 lines) and 35 of 35 shared CSVs identical (the pair arms carry the colour trace in addition). The BeiDou control is identical to the baseline `_rap_srn_b6` (511,690 lines, 35 of 35 CSVs), so the off identity of section 7.1 holds on the third lineage too.

| bed | H1 | H2: user worst, hours 2 to 6 | H3: census worst site | census paired median |
| --- | --- | --- | --- | --- |
| DOY 129 (section 7) | PASS | PASS +0.10 mm | PASS HARB +2.0 mm | 0.0 mm |
| DOY 130 | PASS | PASS +2.15 mm | PASS BAKE +9.9 mm (DAV1 +9.8 mm) | +0.1 mm |
| DOY 132 | PASS | PASS +0.40 mm | PASS DAV1 +8.7 mm | 0.0 mm |
| sim (SZZ000SIM, from 00:30:15) | PASS | PASS +0.20 mm | PASS S25 +0.39 mm | -0.12 mm |
| BeiDou DOY 129 | PASS | PASS +0.40 mm | not run | not run |

WTZZ00DEU 3D p50 by hour, mm (control / armed): DOY 130 88.0 18.6 26.3 36.1 56.1 52.9 / 87.4 18.8 28.5 37.8 57.4 54.7; DOY 132 83.4 19.5 14.8 17.8 19.6 8.7 / 71.1 16.1 15.2 18.1 19.8 8.6; BeiDou 33.0 15.1 20.2 19.0 20.0 15.1 / 33.8 15.5 18.6 17.6 19.0 15.2. No census site is better or worse by 1 cm on any day. The wet delay's epoch-to-epoch sd, hours 2 to 6, median over stations (control / armed): DOY 130 2.75 / 0.71 mm, DOY 132 2.85 / 0.75 mm, BeiDou 2.08 / 0.68 mm. The edge accounting closes on every armed run (G8): erase load 0.63 % (DOY 130), 0.87 % (DOY 132) and 1.15 % (BeiDou) of station-epochs.

No key was stopped.

### 8.3 Look here

- DOY 130, census hours 2 and 3: the median site p50 is 6 mm worse in each (0.033 to 0.039 m, 0.027 to 0.033 m), led by BAKE (+19 mm in hour 2) and DAV1 (+18 mm), and recovers from hour 4; over hours 2 to 6 the worst sites sit at +9.9 and +9.8 mm, inside the +10 mm bar by 0.1 mm. Not attributed between the link and the colour state (no single-key arm on that day).
- DAV1 moves -4.0, +9.8 and +8.7 mm on DOY 129, 130 and 132, and HARB +2.0, +3.8 and +3.5 mm.
- DOY 130, MAC100AUS: a slip storm (424 LLI and 269 IF-TD flags in hours 2 to 6); its wet delay steps by more than 5 cm 92 times on the control and 90 times armed. The station's data, not the pair.
- The sim accuracy bar of G4, hour-2 NEES 0.61, the 24 h sim grade, the RAIM within-epoch replay of section 4.7 (d), and the slant-ionosphere and receiver-clock in-block carries under the vehicle (sections 4.7 and 7.6).

### 8.4 The baseline after a merge

The goldens changed, so the baseline pair of CLAUDE.md (`_rf0hv18`) describes the previous goldens. Proposed replacement, not applied here (CLAUDE.md is shared with every session and changes with the merge): field `D:/GnssProcessorSupport/runs_nom/_rap_ztl2_armg_f6` (WTZZ00DEU 0.0585 0.0126 0.0205 0.0117 0.0171 0.0132 m by hour; census key `ztl2p`), sim `_rap_ztl2_armg_s6` (SZZ000SIM from 00:30:15 0.0993 0.0320 0.0398 0.0159 0.0183 0.0151 m), BeiDou `_rap_ztl2_ha_b6` (0.0338 0.0155 0.0186 0.0176 0.0190 0.0152 m), executable `gnss_processor_ztl2.exe` (`bc5cef3a9cca9c7f`, branch `ztl/colour-pairing`). A merge onto a main that moved since `27d6ece` needs its own control run before these stand.
