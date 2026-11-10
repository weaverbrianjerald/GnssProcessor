# ZTL: a time link for the station wet delay on the float ambiguity vehicle

**STATUS**

- **id:** ZTL
- **topic:** network-vehicle
- **status:** open
- **summary:** Build B2 of the scientific review (card SRN, finding F2). On the float vehicle the station wet delay is a local column born at zero every epoch with no time link; the only vehicle the record did not falsify (`network.carry_zwd_in_block`) was disabled by a loader interlock that tests the F3 knob, which the float vehicle forces off. The card makes the interlock vehicle-aware, folds the wet delay as a column of the station's whitened block, adds the declared event response that standing invariant 1 requires, and grades the time link on the sim truth and the field. Built and graded 2026-11-07 (section 4): G1, G2, G5 pass, G4 fails (hours 4 to 6 accuracy and, without the phase colour state, consistency), G6 shows no integer gain. Open: the pairing with the phase colour state is owed on the field, and identity rows at the re-coupling edge (verification, section 5).
- **source:** card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, finding F2, section 3.5, build B2); the second-model verdicts `D:/GnssProcessorSupport/srn/wave2_verdicts.json` (D2-10, D5-02, D5-03, D5-10); `docs/cards/sim-bed/ZWD-SINK-CENSUS.md` (the diagonal carry falsified).
- **dates in the text:** 2026-11-07
- **knobs / posture:** `network.carry_zwd` + `network.carry_zwd_in_block` (both default false, both off on the goldens), `network.zwd_carry_q` (bracket 1e-8, 5e-8, 2e-7 m^2/s); the float vehicle of both goldens (`network.ambiguity_vehicle float_kf`).
- **hashes / run dirs:** branch `worktree-wf_c49deb5b-2f3-2` from main `284ec29`, code commit `986bd89`; executable `gnss_processor_ztl.exe` (sha256[:16] `c98f567e94a31006`); arms `_rap_ztl_off_{f2,s2}`, `_rap_ztl_q{1e8,5e8,2e7}_s2`, `_rap_ztl_{ctl,on}_{f6,s6}`, `_rap_ztl_q2e7_s6`, `_rap_ztl_pc{ctl,on}_s6`, census keys `ztlc`, `ztla`; review executable `D:/GnssProcessorSupport/worktrees/scirev/build/x64-Release/gnss_processor_srn.exe` (main `adef08b`); arms under `D:/GnssProcessorSupport/runs_nom/_rap_ztl_*`; renamed executable `gnss_processor_ztl.exe`.
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: does the wet delay as a column of the float vehicle's station block track the sim truth closer and stay neutral on the field? Built: the vehicle-aware interlock and an erase-for-one-epoch event response on slip latches and RAIM per-arc attributions, default off and byte-identical off. At q 5e-8 the wet delay is four times smoother (field 2.59 to 0.73 mm per epoch), the field slip-epoch excursions (0.77 m, 0.26 m) are gone, user and census unchanged. The sim error falls only 13 to 17 % (hours 4 to 6 rms 6.4 mm, bar 4) and NEES reaches 2.8; with the phase colour state NEES is 0.77 to 0.86. Standing: open, not recommended for arming alone.

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
