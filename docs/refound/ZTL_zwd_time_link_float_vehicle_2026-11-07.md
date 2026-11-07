# ZTL: a time link for the station wet delay on the float ambiguity vehicle

**STATUS**

- **id:** ZTL
- **topic:** network-vehicle
- **status:** open
- **summary:** Build B2 of the scientific review (card SRN, finding F2). On the float vehicle the station wet delay is a local column born at zero every epoch with no time link; the only vehicle the record did not falsify (`network.carry_zwd_in_block`) was disabled by a loader interlock that tests the F3 knob, which the float vehicle forces off. The card makes the interlock vehicle-aware, folds the wet delay as a column of the station's whitened block, adds the declared event response that standing invariant 1 requires, and grades the time link on the sim truth and the field. Pre-registered 2026-11-07; results below.
- **source:** card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, finding F2, section 3.5, build B2); the second-model verdicts `D:/GnssProcessorSupport/srn/wave2_verdicts.json` (D2-10, D5-02, D5-03, D5-10); `docs/cards/sim-bed/ZWD-SINK-CENSUS.md` (the diagonal carry falsified).
- **dates in the text:** 2026-11-07
- **knobs / posture:** `network.carry_zwd` + `network.carry_zwd_in_block` (both default false, both off on the goldens), `network.zwd_carry_q` (bracket 1e-8, 5e-8, 2e-7 m^2/s); the float vehicle of both goldens (`network.ambiguity_vehicle float_kf`).
- **hashes / run dirs:** branch of this card from main `284ec29`; review executable `D:/GnssProcessorSupport/worktrees/scirev/build/x64-Release/gnss_processor_srn.exe` (main `adef08b`); arms under `D:/GnssProcessorSupport/runs_nom/_rap_ztl_*`; renamed executable `gnss_processor_ztl.exe`.
- **gates (frozen at pre-registration):** section 3

**Outcome.** Pre-registered; no result yet.

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
