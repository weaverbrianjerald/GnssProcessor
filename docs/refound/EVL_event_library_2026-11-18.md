# EVL: a library of injected events on the simulator bed (2026-11-18)

**STATUS**

- **id:** EVL
- **topic:** sim-bed
- **status:** open
- **summary:** Eight injected events are pre-registered as configuration files against one simulator control and one ring-fixing control: a satellite clock step, a non-dispersive satellite code step, a broadcast group-delay change (a check of the run-start latch only), two station outages, one satellite losing band 2 at the network, a 1 ms receiver clock jump and a one-cycle slip on a held link; no run made, ten 2 h runs listed. Several expected responses (events 2 to 5), the prior of event 1 and the smoke rule are corrected in the last section, and no event may run before those corrections are worked into the pre-registrations. The configuration files stay outside git, in `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/EVL_event_library_2026-11-18_parts/_scratch/`, until the author approves the event list and the expected responses are corrected.
- **source:** plan package 23 of the post-reset program 2026-11-18, "A library of injected events" (pre-registrations; no run made); commits read: `7fb0737` (branch chain/audit) for network code, configurations and the record, main `5ddc422` for user-engine code (not traced). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The in-engine injector (`network.inject`) can inject seven of the eight events on the simulator bed, each as one change against a 2 h control from 00:30:15; the broadcast group-delay change acts through a modified navigation file and checks only the run-start latch, which holds one value per satellite per run. The satellite clock step has a full chain of detector, response site, counters and trace rows and passes on record. The station outages and the receiver clock jump fail on record in resumed runs; fresh runs separate the resume seam. The satellite code step meets no detector and no mark (inferred): the time-linked ionosphere-free pin opposes it with no release. A mid-run group-delay step, a dispersive code step, a pin-pair re-election and a receiver time-tag jump cannot be injected.

Read at: network code, configurations and the record on tree 7fb0737 (D:/GnssProcessorSupport/worktrees/read_7fb0737); user-engine code on main 5ddc422 (C:/GnssProcessor), not traced for this package. Fixture: C:/GnssProcessor/runs/simnet60_quiet_col_24h. Line numbers below are tree 7fb0737 unless stated.

Configuration folder, written as `<P>` below:
`D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/EVL_event_library_2026-11-18_parts/_scratch/`

---

## 1. The injection mechanism as it exists

### 1.1 The in-engine injector: `network.inject`

- **Loader.** `loadNetworkInject_`, src/core/config/GnssConfigLoadNetwork.cpp:14021-14284. Ten kinds (:14064-14075): `slip`, `outage`, `anchor_handover`, `arc_erase`, `sat_clock_step`, `rx_clock_step`, `oracle_pin`, `fixed_cycle_plant`, `sat_health`, `station_step`. Fields (:14087-14099): `tow` (required), `station`, `sat`, `signal`, `mode`, `delta_cycles`, `flag_lli` (default false, :14092), `duration_s`, `step_m`, `scope`, `sigma_m`, `east_m` / `north_m` / `up_m`. `enable` defaults to true when the event list is not empty (:14264). An armed injector with `network.trace_events` off logs a warning (:14280-14283). `rx_clock_step` refuses a signal scope (:14252-14257).
- **Seam.** `networkInjectEpoch`, src/core/network/carry/Inject.cpp:372-679. It runs after the start-offset fast-forward and before admission, models, slip detectors and assembly (src/core/network/driver/ApplicationNetworkEpoch.cpp:180-195). It edits the synced observation copy of the epoch.
- **What each kind used here does.**
  - `sat_clock_step`: adds `step_m` to every code observable and `step_m/lambda` cycles to every phase observable of one satellite at every station, persistent from `tow` (Inject.cpp:517-529; `addRangeStep` :244-266). Equivalent to the a priori satellite clock moving by `step_m`.
  - `rx_clock_step`: the same at one station for every satellite (:530-551). Code and phase move alike.
  - `slip`: adds `delta_cycles` (any non-zero real) to one phase code of one (station, satellite) from `tow` on, with an optional LLI bit at onset (:498-516). It counts as fired only when the observable is present in the epoch's measurement map (:503).
  - `outage`: removes whole records (`sat: "all"`) or, with a signal list, every observable of that band and attribute (C, L, D, S) while keeping the record (:92-118, :430-481). A return shift and an LLI bit are optional.
  - `sat_health`: the only kind that touches navigation data; it writes the health series at setup (:310-327). No kind writes a broadcast record otherwise.
- **The held-out user** (SZZ000SIM) reads the same epoch after injection (ApplicationNetworkEpoch.cpp:207-215; src/core/network/emit/EmitStore.cpp:650-652). A satellite-side injection reaches it; a station-side injection at a network station does not.
- **Attribution.** `network_events.csv`, tier `INJECT`, actions `injected` and `released`, with details `slip_onset`, `outage_begin` / `outage_end`, `rx_clock_step_onset`, `clock_step_onset` (Inject.cpp:418-422 and the per-kind rows). Run-end line "Network (fault inject)" (:704-709).
- **Never-fired rule.** An armed event that never fires is a run-end error and the run returns 1 (Inject.cpp:730-740). A wrong station, signal or `tow` therefore fails loudly instead of grading as "no effect".
- **Arming of responses.** Only `arc_erase` changes which responses are armed (src/core/network/assembly/Responses.cpp:257-262). The other kinds change data only.

### 1.2 What the simulator fixture adds

- The simulator can script satellite clock steps and ramps in truth (include/sim/SatClockError.hpp:20-21; src/sim/SimConfig.cpp:857-899), slips (:558-693) and constant receiver code and phase biases (:193, :809).
- [corrected 2026-11-19: see Corrections, item 9] The 24 h fixture has no satellite clock block (its `_child_*.json` files). Satellite code biases come from the OSB product and are constant (C:/GnssProcessor/runs/simnet60_quiet_col_24h/S01000SIM/truth_bias.csv, column `source=product`). There is no group-delay or satellite code-bias step channel in the simulator.
- Truth files used by graders: per-station truth (ZWD, slant ionosphere in `truth_atmo.csv`, `truth_slips.csv`, `truth_bias.csv`), `truth_visibility.csv`. `truth_slips.csv` does not know injected slips (relevant to event 8).
- Fixture types: GPS C1C C2W C5Q (with L1C L2W L5Q), Galileo C1C C5Q C7Q. The reference priorities list no GPS band 5 and no Galileo E5b (config/golden/golden_sim_refound.json:41-128), so each satellite has exactly two registered code channels.

### 1.3 RINEX rewrite operations

[corrected 2026-11-19: see Corrections, item 10] scripts/rapid_arm.py can rewrite observation files, but its `bias` operation is a constant N(0, sigma) draw (:59) and `tow_from` / `tow_to` are refused for every operation except `strip` (:180-181). No rewrite operation makes a step. Not used here.

### 1.4 The tool for short event runs

- scripts/event_leg.py converges a seed once, writes snapshots, then resumes one event for a few epochs and grades it against a matched run without the event resumed from the same snapshot (:4-6, :43-45; usage docs/onboarding/event_legs.md). scripts/scenario_matrix.py builds the frozen scenario matrix on top of it (rows on a seed, 60 epochs, 240 for the storm; bars in scripts/smx/matrix.json).
- A resumed run latches its own group delays at resume (docs/onboarding/event_legs.md:17; NRF section 124), and the resume seam has transients of its own.
- **Choice made here:** every event is a fresh 2 h run from 00:30:15 against a fresh 2 h control on the same executable. Reasons: (a) event 3 cannot be resumed at all (the latch is per run); (b) the station outages and the receiver clock jump fail on record only in resumed runs (section 2), and a fresh run removes the resume seam as an explanation. Events 1, 2 and 4 to 8 could also run as resumed short runs later.

### 1.5 Window and timing (checked by the skeptic)

- First fixture epoch 2024-05-08 00:00:30 = TOW 259230. `start_offset_hours` 0.504167 = 1815.0012 s, so the window opens at TOW 261045.0, halfway between epochs 261030 and 261060 (the half-epoch back-off holds).
- First processed epoch 261060 (src/core/math/TimeUtils.cpp:351-355; src/core/network/driver/EpochAdmission.cpp:45-49). Last processed epoch 268230: the duration cutoff is anchored on the first processed epoch (EpochAdmission.cpp:115-124). The group-delay latch window is [261045, 268245) (src/core/network/driver/ApplicationNetworkSetup.cpp:357-361).
- Hour 1 (ignition) ends at about 264660. Satellite events sit at TOW 264600 (01:30:00), station events at 264660 (01:31:00) except event 5.

### 1.6 Controls

- **Simulator control: `<P>station-and-pin-events/control_sim.json`.** A copy of config/golden/golden_sim_refound.json with only `processing.max_duration_s` and `network.max_duration_s` (21600 to 7200), `network.start_offset_hours` 0.504167 and `network.product_output_path` (D:/GnssProcessorSupport/runs_nom/_rap_evl_control_sim/products.txt) changed. `<P>inject-mechanism-and-satellite-events/sat_control.json` is identical apart from `_comment` and the output path (skeptic, diff.py), so one run serves events 1 to 7; sat_control.json is not run.
- **Ring-fixing control: `<P>station-and-pin-events/control_ringfix.json`** (event 8 only). A second posture, not a one-change copy of the simulator reference (section 2, event 8).
- Every configuration was checked to parse and to differ from its source only in the listed keys (check.py, check_configs.py, critic/diff.py). scripts/loader_keys.py reports `network.min_stations`, `network.redundancy_census` and `_comment` as unread in every file; all three come from the reference file itself, so no unread key was introduced.

### 1.7 Common bars (all events)

Read as event minus control over the post-event epochs, the channels of scripts/smx/matrix.json:19-27 (frozen before the matrix's first run), each against `max(3 S, floor)` where S is the control's own white-noise-equivalent spread:

| channel | quantity | floor |
| --- | --- | --- |
| clk | emitted satellite clock, de-commoned per epoch and constellation (median); max over epochs of the RMS over satellites | 5 mm |
| pb | emitted phase bias, de-commoned per epoch, constellation and signal, on keys with the same hold generation in both runs | 5 mm |
| user peak | held-out user SZZ000SIM, max over epochs of the ENU difference | 10 mm |
| user plateau | held-out user, mean ENU difference over the last 20 epochs | 5 mm |
| zwd | station ZWD error growth against truth | 5 mm |
| iono | slant ionosphere `iono_total_m`, de-commoned per station and epoch | 20 mm |
| emit | epochs the control emitted and the event run did not | 0 |

Plus: the run returns 0, the run-end line reports no never-fired event, and the INJECT `injected` (and for outages `released`) rows are present. A withheld epoch is an adaptation failure (emit bar 0).

---

## 2. The events

### Event 1. Satellite clock step (G22, +0.5 m)

- **File:** `<P>inject-mechanism-and-satellite-events/sat_clock_step.json` (output `_rap_evl_sat_clock_step`).
- **One change:** `network.inject` = one `sat_clock_step`, sat G22, `step_m` 0.5, `tow` 264600.
- **Truth that moves:** every code and phase of G22, at every station and at the held-out user, +0.5 m from 01:30:00, persistent. G22 is tracked at 20 network stations at the event epoch, and by the user for the rest of the window (fixture truth_visibility.csv).
- **Expected response** (all armed in the reference: golden_sim_refound.json:250 `if_phase_td_sat_clock_event`, :255-256 clock discontinuity, :317-327 clock anchor, :953-954 trip generation with trigger `clock_event`):
  1. Detector: the IF-TD pool classifies a satellite clock event (src/core/network/slip/SlipQc.cpp:1381-1440; at least 4 flagged stations, 75 %, coherence 0.25, defaults at GnssConfigLoadNetwork.cpp:6450-6457). Trace row `T1_if_slip` / `sat_clock_event` / G22. Counters `if_td_clock_events`, `if_td_clock_flagged`, `if_td_clock_kept` (run-end line "Network (IF-TD clock event)", SlipQcRunEnd.cpp:950-960). Flagged arcs within the threshold of the step are kept, not refloated.
  2. Response site: the clock-anchor fold takes G22 loose (src/core/network/solve/ClockAnchor.cpp:407) and the guard declares the edge: `CLK_ANCHOR` / `cta_decouple`, detail `reason=slip_clk` (:484-486, :586-596). Counters `cta_trip_slip_clk`, `cta_marks`. `stepped[G22] = ClockEvent`, and a clock discontinuity mark goes on the correction stream at that epoch (src/core/network/emit/SolPrep.cpp:835-845).
  3. Phase-bias hold: one new generation per G22 key, `PB_HOLD` / `gen_start` / `trip`, counter `n_pbh_gen_trip` (src/core/network/emit/PbHold.cpp:185-238).
  4. Re-coupling edge after 4 in-band epochs: `CLK_ANCHOR` / `cta_recouple` (ClockAnchor.cpp:899-918).
- **Falsifier** (any): no `sat_clock_event` for G22 at 264600; G22 arcs refloated at more than one station (flagged minus kept above 1); no `cta_decouple` `slip_clk` at 264600; no trip generation for G22; no `cta_recouple` by 264900.
- **Bars:** common bars, with the expected step subtracted from the clk channel as the matrix does (scripts/smx/matrix.json row `sat_clk`: the emitted G22 clock is expected to move by -0.5 m). Event-specific: G22's de-commoned emitted clock minus control within `max(3 S, 5 mm)` of -0.500 m from 264630 to the end. The first stage proposed raw per-satellite clock bars; the skeptic refuted these (raw clocks carry datum moves; invariant 5 of CLAUDE.md). Only de-commoned values are graded.
- [corrected 2026-11-19: see Corrections, item 7] **Prior on record:** row `sat_clk` passes every channel on the reference posture (docs/record/refound/SMX2_scenario_matrix_current_golden_2026-11-09.md:195 and :232; docs/record/refound/ARS_anchor_resolve_coupling_2026-10-26.md:146). It failed before trip generation was armed (docs/refound/SMX_scenario_matrix_2026-10-24.md:140).
- **Control:** control_sim.json.
- **Runnable today:** yes, after the satellite check of section 4.3.

### Event 2. Satellite code step, non-dispersive (G13, +0.15 m on every code signal)

- **File:** `<P>inject-mechanism-and-satellite-events/sat_code_step.json` (output `_rap_evl_sat_code_step`).
- **One change:** `network.inject` with 67 events at `tow` 264600: one `sat_clock_step` G13 +0.15 m plus 66 `slip` events of -0.15/lambda cycles on L1C, L2W and L5Q at each of the 22 receivers holding G13 after 264600 (21 network stations and SZZ000SIM). One physical change written as 67 events. `flag_lli` stays false (no LLI raised).
- **Truth that moves:** G13's code on every signal at every receiver, +0.15 m; phase unchanged (take-back deltas equal -0.15/lambda to machine precision, residual 0 to 3e-17 m, skeptic). The geometry-free code combination is unchanged, so the time-linked GF pin (golden_sim_refound.json:926-927) does not oppose the step. The skeptic confirmed the 22 receivers are exactly those holding G13 in the RINEX after 264600.
- **Expected response: NONE as an event** (lead from draft NCQ row S2, confirmed on the code by the first stage):
  - IF-TD reads phase only. The anchor guard works in phase space (golden_sim_refound.json:962).
  - MW: the wide-lane shift is 0.174 GPS wide-lane cycle, inferred below its bar.
  - RAIM and gross-LOS: 0.15 m against a 0.3 m zenith code sigma (inferred silent).
  - Code-bias reinit needs clock-refloat flags or `cb_excursion_reinit` (Responses.cpp:281-282); neither fires or is armed.
  - [corrected 2026-11-19: see Corrections, item 1] The emitted code jump test trips on a change above 0.25 cycle between emissions, 4.8 cm on L1 and 6.1 cm on L2 (src/core/products/NetworkProductEmitter.cpp:186-196, 816-838; golden_sim_refound.json:329). Under q 1e-10 m2/s the code column moves well under a millimetre per epoch (inferred), so no mark.
- **Measured (no bar on an unexpected response):** `network_sat_bias_states.csv`, G13 code columns minus control, epochs to 50 % and 90 % of 0.15 m; emitted G13 code biases and counters minus control (products.txt); the user's G13 code residual in `closed_loop_sat_trace.csv`; whether the estimable block-Baarda report names G13.
- **Falsifier of NONE** (any): a G13 code discontinuity mark between 264600 and 264630; a `CbReinit`; RAIM or gross-LOS flags on G13 at half or more of its stations within 10 epochs.
- [corrected 2026-11-19: see Corrections, item 2] **Bars for a future response (expected to FAIL today; the failure documents the gap):** the column takes 90 % of the step within 10 epochs and declares one code discontinuity; user 3D median within 5 mm of control after 10 epochs. Common bars also apply.
- **Grading note:** the 66 INJECT slip rows are the phase take-back. Any grader that scores INJECT slip rows as truth slips must exclude them.
- **Open point (skeptic):** GPS band 5 is not in the priority list. The 22 L5Q slips fire only if the measurement map still holds L5Q at injection time (Inject.cpp:503); the comment at ApplicationNetworkSetup.cpp:324 places priority admission later (inferred). If L5Q is dropped earlier, those 22 events never fire and the run returns 1. They are unnecessary (band 5 is not modelled), so section 6 offers to remove them (45 events instead of 67).
- **Control:** control_sim.json. **Runnable today:** yes, non-dispersive only.

### Event 3. Broadcast group-delay change (G22 T_GD +1.0 ns from the 02:00 record)

- **File:** `<P>inject-mechanism-and-satellite-events/sat_tgd_step.json` (output `_rap_evl_sat_tgd_step`).
- **One change:** `files.navigation_path` = `<P>inject-mechanism-and-satellite-events/nav_tgd_G22/BRDC00IGS_R_20241290000_01D_MN.rnx`, the DOY 129 navigation file with G22's T_GD moved from -11.176 ns to -10.176 ns in its 13 records from toc 2024-05-08 02:00:00. Exactly 13 lines differ from C:/GnssProcessor/data/2024-129/products/BRDC00IGS_R_20241290000_01D_MN.rnx (both parts and the skeptic). An explicit path suppresses discovery (src/core/products/PathResolver.cpp:371).
- **This is not a step in the engine.** The latch reads the navigation file once at setup (ApplicationNetworkSetup.cpp:694-711) and keeps one value per satellite: the low median of healthy records with toc in [261045, 268245) (src/core/network/solve/Sbasis.cpp:155-168). G22's only record in that window is the 02:00 record, so the stepped value applies to the whole run from the first epoch, 1.5 h before it is broadcast. The skeptic moved this event from "runnable steps" to "tests the latch only"; this document agrees.
- **Truth that moves:** nothing in the observations. The true code biases are constant (OSB product). The GF pin target moves by c x 1 ns = 0.2998 m (ApplicationNetworkSetup.cpp:725): a datum change.
- [corrected 2026-11-19: see Corrections, item 3] **Expected response:** the run-start line reports that one satellite differs from the file's first record, G22 +0.2998 m (ApplicationNetworkSetup.cpp:719-751). At 02:00 nothing happens: no event row, no counter, no discontinuity (NONE; draft NCQ row S4).
- **Falsifier** (either): any structure at 266400 in G22's emitted code biases or slant ionosphere minus control; the scaled GF combination of G22's emitted code biases, (b_C2W - b_C1C)/(gamma - 1), minus control not constant at 0.2998 m in magnitude from the first epoch. Correction by the skeptic: the pin row carries -1/(mu2-1) and +1/(mu2-1) on the two code columns with target c T_GD (Sbasis.cpp:1543-1549), so the raw difference b_C2W - b_C1C moves by (gamma - 1) x 0.2998 = 0.194 m. The first stage's "GF combination constant at 0.30 m" named the wrong combination.
- **Bars:** user 3D median within 1 mm of control over 01:30 to 02:30, from the step being a datum change whose estimable part is datum-free (inferred, Sbasis.cpp:1513-1534 as cited by draft NCQ). Common bars apply except the clk and pb channels, which may carry the datum move per satellite and are recorded only.
- [corrected 2026-11-19: see Corrections, item 4] **What it documents:** no in-run reaction to a broadcast change exists, and the change is applied acausally over the whole window. For Galileo (10-minute records) the low median would take a new value only for a negative change (inferred from Sbasis.cpp:165-166).
- **Control:** control_sim.json. **Runnable today:** yes, as a latch check; a mid-run step is a gap (section 3).

### Event 4. Station absent for 10 minutes (S48000SIM)

- **File:** `<P>station-and-pin-events/e04_sta_out_10min.json` (output `_rap_evl_e04_sta_out_10min`).
- **One change:** one `outage`, station S48000SIM (the station of the frozen matrix, scripts/smx/matrix.json:16; not the clock datum station S30000SIM), sat `all`, `tow` 264660, `duration_s` 600, no return shift.
- **Truth that moves:** the station's data is absent from 264660 to 265230; it returns at 265260. Nothing else moves.
- **Expected response:** a re-coupling, never a first sighting (invariant 7).
  - `TOPOLOGY` / `sta_readmitted` with `epochs_absent=20`; counters `sta_readmit_events` +1, `sta_readmit_max_gap_ep` 20 (src/core/network/solve/SolveStations.cpp:356-381).
  - 600 s is inside `network.phase_leveling_max_gap_s` 900 (reference), so no gap cliff: `inject_stale_skips` stays 0, and no `ARC` / `arc_death reason=horizon` for this station (src/core/network/carry/LevelingCarry.cpp:338-356; src/core/network/assembly/SignalRoster.cpp:1381-1409).
  - [corrected 2026-11-19: see Corrections, item 5] Gap re-entry (armed `cycle_slip.gap_reentry_relevel`): each returning arc gets a verdict or an `ARC` / `arc_reentry` row reading "none, bridge, carry=pl" (SlipQc.cpp:2876-2930). `ledger_reentry_bridged` > 0; `gap_reentry_freed_shifted` and `gap_reentry_freed_lli` stay 0 for S48000SIM (SlipQcRunEnd.cpp:560-600).
- **Falsifier:** any S48000SIM arc freed or refloated at the return; a first-sighting treatment (no `sta_readmitted` row, or the carry erased); a withheld epoch.
- **Bars:** common bars.
- **Prior on record:** matrix row `sta_out` (same shape, resumed runs) fails zwd and iono on every posture: zwd 137.6 mm against 14.7 (docs/refound/SMX_scenario_matrix_2026-10-24.md:139, :176), and still in the current matrix (SMX2 :192-194). The record names S48000SIM for both channels. The prediction for this fresh run is therefore not "pass" by default: if zwd and iono fail again without a resume seam, the failure belongs to the outage response itself.
- **Control:** control_sim.json. **Runnable today:** yes.

### Event 5. Station absent for 60 minutes (S48000SIM)

- **File:** `<P>station-and-pin-events/e05_sta_out_60min.json` (output `_rap_evl_e05_sta_out_60min`).
- **One change:** one `outage`, S48000SIM, `all`, `tow` 262860, `duration_s` 3600; return at 266460.
- **Placement compromise:** a 60 min outage cannot both follow ignition and leave recovery inside 2 h. It starts 30 min into the run, which is inside hour 1 (skeptic), and returns 29.5 min before the end. The 2 h file is a mechanics check only. A 3 h validation variant (outage 01:30 to 02:30, recovery to 03:30, with a 3 h control) is proposed and not written (section 6).
- **Truth that moves:** as event 4, with 120 epochs absent.
- **Expected response:** the outage is beyond the carry (every carried leveling entry of the station is past 900 s).
  - Gap cliff at the return (`inject_stale_skips` > 0), then the post-harvest prune: `ARC` / `arc_death reason=horizon src=local_carry`.
  - Gap re-entry ends in "no carry": `gap_reentry_no_carry`, `ledger_reentry_no_carry`; not "freed".
  - `sta_readmitted` with `epochs_absent=120`.
  - 3600 s is inside `readmit_seed_horizon_s` 3900 (reference) and `network.readmit_seed_mirrors` is armed, so a seed-mirror path may engage (inferred; its counter was not identified).
- [corrected 2026-11-19: see Corrections, item 6] **Falsifier:** any carried entry of the station injected after the return (a stale prior carried across 60 min, invariant 1); a first-sighting record; a withheld epoch.
- **Bars:** common bars over the 29.5 min after the return; the plateau bar is read but marked provisional (the window is short).
- **Control:** control_sim.json. **Runnable today:** yes.

### Event 6. One satellite loses band 2 at every network station for 10 minutes (G22)

- **File:** `<P>station-and-pin-events/e06_pin_pair_end.json` (output `_rap_evl_e06_pin_pair_end`; the file name predates the retitle).
- **One change:** 21 `outage` events, sat G22, signal `L2W`, 600 s from 264660, at every network station that holds G22 L2W in 264660 to 265230 (list in `<P>station-and-pin-events/make_configs_record.json`; includes S30000SIM). The outage scope drops C2W and L2W and keeps the record (Inject.cpp:92-118, :463-473). SZZ000SIM is left out on purpose and keeps L2W.
- **What it is, and what it is not.** The first stage aimed at "the end of a satellite's ionosphere-free pin pair". The code shows no pin receiver: the pair is network-wide, the two lowest distinct-frequency code channels registered at the epoch (`selectIfPinPair`, Sbasis.cpp:38-80, called at :1348), and a channel registers when two or more present stations hold it (ApplicationNetworkAssembly.cpp:611-641). Under the reference priorities each satellite has exactly two registered channels, so no re-election to another pair is possible (both parts and the skeptic agree). The event is G22 becoming single-frequency at the network; a true re-election needs a third registered code channel, a posture change.
- **Truth that moves:** G22's band-2 code and phase are absent at the network for 20 epochs. Nothing else moves.
- **Expected response** (`code_datum_time_linked` true, family difference true, no pair-continuity logic):
  - Opening edge: G22 falls to the single-frequency all-ones gauge (Sbasis.cpp:1491-1497) or leaves the stations' models; which one is not settled. No `CLK_BASIS` row at the opening edge, because `this_epoch_if_pin_sel` is recorded only for a dual pair (Sbasis.cpp:1410-1411; SolPrep.cpp:905-931).
  - Return edge, one of two branches: the time link continues the old pair from its previous values (no event; legitimate only if G22's emitted clock and code bias do not step), or the pin re-seeds: `this_epoch_cb_reseed`, a `CB_DATUM` / `reseed_disc_landed` row for G22 and a code-and-clock discontinuity mark (Sbasis.cpp:1481-1488; SolPrep.cpp:966-1004).
- **Falsifier:** G22's emitted clock or code bias steps at either edge (event minus control above the clk bar) without a `CB_DATUM` or `CLK_BASIS` row and a discontinuity counter bump for G22 at that epoch: an unsignalled re-reference.
- **Weakness (skeptic):** the falsifier accepts either branch, so only an unsignalled step fails it. The user keeps tracking G22 L2W while the corrections lose band 2, so the user channels mix a correction gap with the event (inferred). The user channels are recorded, not graded, for this event.
- **Control:** control_sim.json. **Runnable today:** yes, as "G22 band 2 lost at the network".

### Event 7. Receiver clock jump of 1 ms (S48000SIM)

- **File:** `<P>station-and-pin-events/e07_rx_clock_1ms.json` (output `_rap_evl_e07_rx_clock_1ms`).
- **One change:** one `rx_clock_step`, S48000SIM, `step_m` 299792.458, `tow` 264660, persistent.
- **Truth that moves:** the station's c dt_r steps by 1 ms in code and phase alike. Not modelled by the injector: the satellite-motion term of a receiver time-tag jump (up to about 0.8 m per satellite, inferred) and a code-only step (refused, GnssConfigLoadNetwork.cpp:14252-14257).
- **Expected response from the code reading:** no detector action. MW and geometry-free combinations are clock-free; IF-TD single-differences between satellites remove the receiver clock (include/slip/IfPhaseSlipDetector.hpp:80-85); the network receiver clock is not carried (`carry_rx_clock_in_block` default false, GnssConfigLoadNetwork.cpp:5190; src/core/network/assembly/ParamRegistry.cpp:216-229).
- **Disagreement, stated plainly.** The first stage predicted silence. The skeptic marked this unconfirmed: the same injection fails zwd on record (74.9 mm against 14.7, SMX :142; still failing zwd and iono on every posture, SMX2 :201-203), and a 299.8 km step was not traced against the gross-LOS screen (12 m cold bound), the prefit screen report and `cold_clock_seed`. This document pre-registers two outcomes instead of silence:
  - H0: zero slip flags, refloats, `arc_reentry` rows, RAIM actions and gross-LOS actions at S48000SIM, and zwd and iono within the common bars. Then the record's failure was a resume artifact.
  - H1: the record's zwd and iono failure reproduces in a fresh run. Then a path that sees the receiver clock exists, and the first action to name it (from `network_events.csv`) is the finding.
- **Falsifier of H0:** any of the actions above at S48000SIM, or a move of the station's ZWD or slant ionosphere above the bars.
- **Bars:** common bars; the record's failure is the prior.
- **Control:** control_sim.json. **Runnable today:** yes.

### Event 8. One-cycle slip on a held integer link (S48000SIM / G15, L1C)

- **Files:** `<P>station-and-pin-events/e08_held_link_slip.json` (output `_rap_evl_e08_held_link_slip`) against `<P>station-and-pin-events/control_ringfix.json` (output `_rap_evl_control_ringfix`).
- **One change (against control_ringfix):** one `slip`, S48000SIM, G15, L1C, `delta_cycles` +1, no LLI, `tow` 266460 (90 min in). G15 is at 49 to 75 degrees elevation then (truth_atmo.csv). The skeptic confirmed e08 differs from control_ringfix only in the injection and the output path.
- **The control is a second posture.** `network.ring_fix.enable` requires `network.phase_datum_eq34` (GnssConfigLoadNetwork.cpp:11228-11232), which the simulator reference leaves off, so integer fixing cannot be armed by one key. control_ringfix differs from the simulator reference in about 60 keys (skeptic count): eq34 on with 8 eq34 keys, 18 ring_fix keys (rule gated, order certain, odds gate, `wl_frac_max` 0.25, `held_first`, pairs E:L1C/L5Q and G:L1C/L2W, settled flags with rule largest and wait 10, `max_decisions_per_epoch` 12, `entry_revalidate`, `truth_root`), the held basis, prioritized datum, continuous handover, gate-drop erase, the phase-bias hold keys and `phase_anchor_carry_reref` off, `phase_datum_min_stations` 1, GPS priorities with C1C first, `processing.exact_epoch_time`, and 12 slip, carry and trace keys. The keys were taken from runs_nom/_rap_ad_n146/arm.json (the observed-data ring-fix posture) minus data keys; the skeptic did not open that file. The record's simulator run of ring fixing used `kq12x_sim`'s configuration plus the families and settled flags (NRF part 12 section 99, docs/refound/NRF_network_ring_fixing_2026-11-14_part12.md:71-72). Equivalence is unconfirmed (section 6).
- **Truth audit:** `network.ring_fix.truth_root` points at the fixture (simulation-only audit, RingFixSeams.cpp:426-458). The fixture's truth_slips.csv does not hold the injected cycle, so the audit will call a correct re-decision on S48000SIM / G15 wrong; that link is excluded from the `truth_wrong` count.
- **Truth that moves:** the L1C ambiguity of S48000SIM / G15 rises by one cycle from 266460.
- **Expected response:**
  1. Detectors: MW (one wide-lane cycle), IF-TD (about 0.48 m ionosphere-free) and `gf_arc_jump` (0.19 m; armed with k 6 in this posture) flag the arc.
  2. Response site: the free-epoch latch releases the link with cause Slip (include/network/ar/RingFix.hpp:138-144): `RINGFIX` release row with cause `slip` (RingFixSeams.cpp:907-919; RingFix.cpp:36-45), `released[slip]` +1 and `carry_reopened` +2 at the slip epoch.
  3. Later, possibly a `RINGFIX` hold with n1 changed by one and n2 unchanged.
- **Pre-run rule (one amendment allowed, before the event run):** in control_ringfix's `RINGFIX` rows the link S48000SIM / G15 must be held (no release, no `to_tree`) for the 600 s before 266460. If it is not, take the first of G05, G13, G22, G24 at S48000SIM that is, and regenerate e08 with only the satellite changed.
- **Falsifier** (any): no release at the slip epoch or the next; a re-hold at the old integer; `truth_wrong` on any link other than S48000SIM / G15; a re-hold whose n1 differs by anything other than one.
- **Bars:** common bars; `truth_wrong` 0 on every other link.
- **Runnable today:** yes, after the pre-run hold check.

---

## 3. The gaps

### 3.1 Events the mechanism cannot inject

| event | what is missing | where it would go (inferred) |
| --- | --- | --- |
| Mid-run broadcast T_GD step | A time-resolved group delay: the latch is one value per satellite per run (Sbasis.cpp:155-168), read once at setup (ApplicationNetworkSetup.cpp:694-711); no inject kind writes a navigation record. A modified navigation file acts on the whole run or none of it (event 3). | A group-delay series on the setup like the health series (Inject.cpp:310-327), read per epoch by the GF pin, with an event row and counter at the change. |
| Dispersive (per-signal) satellite code step, the truth-side group-delay change | A code-only kind with a signal scope. Composition (event 2) gives only the non-dispersive case; the simulator has no code-bias step channel. | A `sat_code_step` kind with `signal`, or a simulator code-bias schedule. |
| Pin-pair re-election | A third registered code channel per satellite: the reference priorities give exactly two (event 6). | A posture with GPS band 5 or Galileo E5b registered; a posture change, not an injection. |
| Receiver time-tag jump | The satellite-motion term (up to about 0.8 m per satellite, inferred); `rx_clock_step` adds the same range to every satellite. | An `rx_clock_step` mode that re-evaluates geometry at the shifted tag. |
| Code-only receiver step | `rx_clock_step` refuses a signal scope (GnssConfigLoadNetwork.cpp:14252-14257). | A signal scope on `rx_clock_step`. |
| Satellite clock ramp or drift | Not in this library; the matrix runs it as a simulator truth schedule on fresh runs (scenario_matrix.py drift row), not by the injector. | Already covered by the matrix. |

### 3.2 Events whose expected response is NONE

- [corrected 2026-11-19: see Corrections, item 1] **Event 2 (non-dispersive satellite code step).** No detector sees a 0.15 m common code shift with phase unchanged; the code column absorbs it slowly under q 1e-10 m2/s and no discontinuity is declared. This is a gap against invariant 1 (a near-constant quantity whose truth moves without a wired response); the future bars are written and expected to fail.
- **Event 3 (broadcast group-delay change).** No in-run reaction; the latch is acausal over the run window. Expected NONE at the change epoch, with the whole-run offset reported at start.
- Event 7 is pre-registered as NONE under H0 only; the record makes H1 a live alternative.

---

## 4. The batch (none requested)

### 4.1 Runs listed for one night

| # | file | role | length | output under D:/GnssProcessorSupport/runs_nom/ |
| --- | --- | --- | --- | --- |
| 1 | control_sim.json | simulator control | 2 h | _rap_evl_control_sim |
| 2 | sat_clock_step.json | event 1 | 2 h | _rap_evl_sat_clock_step |
| 3 | sat_code_step.json | event 2 | 2 h | _rap_evl_sat_code_step |
| 4 | sat_tgd_step.json | event 3 | 2 h | _rap_evl_sat_tgd_step |
| 5 | e04_sta_out_10min.json | event 4 | 2 h | _rap_evl_e04_sta_out_10min |
| 6 | e05_sta_out_60min.json | event 5 (mechanics) | 2 h | _rap_evl_e05_sta_out_60min |
| 7 | e06_pin_pair_end.json | event 6 | 2 h | _rap_evl_e06_pin_pair_end |
| 8 | e07_rx_clock_1ms.json | event 7 | 2 h | _rap_evl_e07_rx_clock_1ms |
| 9 | control_ringfix.json | ring-fixing control | 2 h | _rap_evl_control_ringfix |
| 10 | e08_held_link_slip.json | event 8 | 2 h | _rap_evl_e08_held_link_slip |

Count: 10 runs of 2 h data (00:30:15 to 02:30:15), simulator bed, GPS plus Galileo. Optional: 2 more runs of 3 h (the event 5 validation variant and its 3 h control) if the author approves that variant. sat_control.json is not run (identical to control_sim.json).

[corrected 2026-11-19: see Corrections, item 8] Engine: one Release executable of the AVX2 build (`build/x64-Release/gnss_processor.exe`, run as a renamed copy so a concurrent rebuild cannot lock it), the same executable for every run and both controls; never Debug. Each run in its own working directory, because traces go to `./logs` of the process working directory. A 10-epoch smoke of each configuration first (it also catches a never-fired event, return code 1, only if the event falls inside the smoke; it does not, so the smoke checks parsing and paths only). Order: controls first; the pre-run checks of 4.3 read the control traces before the event runs start. Wall time was not estimated.

### 4.2 Grading

- Each event run minus its control on the channels of section 1.7 (`max(3 S, floor)`), plus the event's discrete expectations: the named rows present at the named epochs, the counters at the stated values, and each falsifier checked.
- scripts/scenario_matrix.py `grade_cell` computes these channels for a pair of run directories named in a plan (scenario_matrix.py:593-713); reusing it on fresh run pairs through a plan stub is inferred, not checked. Otherwise a small grader is owed (section 7).
- INJECT slip rows of event 2 are excluded from any slip truth count; S48000SIM / G15 is excluded from `truth_wrong` in event 8.
- A verdict needs both: channels within bars and discrete expectations met. A pass on channels with a missing row (for example no `sta_readmitted`) is a fail of the event.
- Same-executable determinism: a control rerun would read identical (the matrix's control rule); not listed, since one control serves all events.

### 4.3 Pre-run checks on the control traces (before the event runs)

1. **Satellite choice (events 1 to 3, 6).** The skeptic noted G22 and G13 were chosen by visibility, not by the matrix rule (G25, else G12 when G25 is a GPS clock pivot; scripts/smx/matrix.json:17), and nobody checked whether either is a GPS clock pivot or the IF-TD reference satellite at the event epoch. If the stepped satellite is the IF-TD reference, the classifier sees the step on every other satellite (SlipQc.cpp:1381-1396). From control_sim's traces: no PIVOT event or pivot line for G22 or G13 around 264600, and neither is the IF-TD reference. Failing that, regenerate with G12 or G15 (both tracked by the user) and only the satellite changed.
2. **Held link (event 8):** the 600 s hold check of event 8.
3. [corrected 2026-11-19: see Corrections, item 8] **Event 2 L5Q presence:** confirm from the smoke that L5Q is present in the injector's measurement map, or remove the 22 L5Q take-back events.

### 4.4 Merge ledger: first stage against the skeptic

| point | first stage | skeptic | this document |
| --- | --- | --- | --- |
| Window | sat part unsure (261030 or 261060) | 261060 to 268230, latch [261045, 268245) | adopted |
| Controls | two files | identical; one run | one run (control_sim) |
| Event 3 | a runnable step | a latch check only | latch check; mid-run step is a gap |
| Event 3 falsifier | GF constant 0.30 m | names the wrong combination; raw difference 0.194 m | corrected |
| Event 1 bars | raw clocks within 5 mm | datum moves; use de-commoned channel | de-commoned only |
| Satellites | G22, G13 by visibility | matrix rule or pivot check | pre-run check (4.3) |
| Event 5 start | 262860 | inside hour 1 | kept as mechanics; 3 h variant proposed |
| Event 6 | pin-pair end | single-frequency satellite; weak falsifier; user mixes a correction gap | retitled; user channels recorded only |
| Event 7 | NONE | unconfirmed; record fails zwd | H0 / H1 |
| Event 8 posture | 55 keys from _rap_ad_n146 | about 60 keys; not checked against kq12x_sim | open decision |
| Unread keys | none | three, all from the reference | three, none introduced |

---

## 5. Runs this design will need later

The ten (optionally twelve) runs of section 4.1, all on the simulator bed. After the gaps of section 3.1 are built: one run per new kind (mid-run T_GD step, dispersive code step) against the same control, then the same events on observed data (DOY 129 reference configuration) once the simulator verdicts exist. None is requested now.

---

## Decisions for the author

1. **Approve the event list** (events 1 to 8, section 2) before any run. Events 3 and 6 are weaker checks than their titles first suggested (a latch check; a single-frequency satellite, not a re-election); keep, rename or drop.
2. **Satellites:** keep G22 / G13 with the pre-run pivot and IF-TD-reference check, or regenerate events 1 to 3 and 6 on the matrix rule (G25, else G12).
3. **Event 2:** remove the 22 L5Q take-back events (unmodelled band, risk of a never-fired exit), leaving 45 events.
4. **Event 5:** add the 3 h validation variant and its 3 h control (2 more runs), or keep only the 2 h mechanics check.
5. **Event 8 posture:** keep control_ringfix as built from the observed-data ring-fix configuration, or rebuild it from `kq12x_sim`'s configuration with the families and settled flags of NRF section 99, so the control matches a run on record.
6. **Event 7:** accept the H0 / H1 pre-registration in place of a prediction of silence.
7. **Gaps:** whether to schedule the missing injections of section 3.1 (a time-resolved group delay with its event, a code-only satellite kind with signal scope) as their own designs; both are default off.
8. **Fresh against resumed:** confirm fresh 2 h runs for this batch; the resumed short-run form (event_leg.py) stays available for events 1, 2 and 4 to 8.

## Not checked

- The user engine's reaction to a hold generation, a clock discontinuity mark or a correction gap (main 5ddc422): not traced.
- Whether L5Q survives to the injector's measurement map (event 2), and hence whether those 22 events fire.
- Whether G22, G13 or G15 is a GPS clock pivot, the IF-TD reference satellite or (G15) a held link at the event epochs: needs the control's traces.
- That MW, RAIM and gross-LOS stay silent at 0.15 m (event 2), and the code column's absorption rate: inferred, not computed.
- The gross-LOS screen, prefit screen report and `cold_clock_seed` under a 299.8 km receiver step (event 7).
- Event 6: whether a satellite reduced to band 1 keeps single-band rows or leaves the model, and whether the persisted pin pair survives a 10 min absence (continue against re-seed).
- Whether an `all` outage leaves the station non-present or present and empty (inferred non-present).
- The counter of the readmit seed mirrors at a station return (event 5).
- The chord orientation, hence the sign of the n1 change, in event 8; runs_nom/_rap_ad_n146/arm.json and the record's `kq12x_sim` configuration were not opened.
- The sign of the expected emitted clock change in event 1 is taken from the matrix, not from the emitter code.
- Whether scenario_matrix.py `grade_cell` can grade fresh run pairs through a plan stub.
- First and last epochs follow from the offset rule and the code, not from a run. Wall time of the batch was not estimated.

## 2026-11-19: corrections from the check against the code

The check read network code, configurations and the record on tree `7fb0737` (B) and user-engine code on main `5ddc422` (M), compared every configuration file with its control and the reference, and parsed the navigation file. Its verdict: usable with corrections; nine claims are wrong, none overturns the Outcome, and the absorption mechanism of event 2 and the falsifier of event 5 are rewritten before any run. Items marked in the body carry the item number below; the original words stand above.

1. **Event 2, expected response, and section 3.2: the code column absorbs the step slowly under q 1e-10 m2/s.** The satellite code-bias columns cannot absorb it: with `code_datum_time_linked` true (`config/golden/golden_sim_refound.json:341`, B) the ionosphere-free pin holds their ionosphere-free combination at the previous epoch's value (`src/core/network/solve/Sbasis.cpp:1454-1466`; `CodeBiasDatum.cpp:343`, B) and the time-linked geometry-free pin holds the other combination, so the step can only reach the satellite clock and phase biases or stay in the code residuals. The NONE outcome stands as "no detector and no mark" (inferred, not run); its reason changes.
2. **Event 2, bars for a future response: "the column takes 90 % of the step within 10 epochs".** Unreachable under the time-linked ionosphere-free pin; a response needs a declared release of that pin. The measured quantities gain G13's emitted clock and phase biases minus control.
3. **Event 3, expected response: the run-start line "reports that one satellite differs from the file's first record, G22 +0.2998 m".** The comparison is an exact comparison of doubles (`src/core/network/driver/ApplicationNetworkSetup.cpp:733-737`, B); in the unmodified file about 21 satellites already differ in the last printed digit. The line lists G22 at +0.2998 m among the satellites that differ from their first record (the control already lists about 21 at 0.0000 m).
4. **Event 3, what it documents: for Galileo the low median "would take a new value only for a negative change".** The low median (`Sbasis.cpp:163-164`, B) takes a new value when the changed records reach it, which depends on their share of the in-window records and on the sign.
5. **Event 4: the `ARC` / `arc_reentry` row reads "none, bridge, carry=pl".** A no-verdict bridge writes `carry=dd`: `GapArcOutcome` defaults `pl_bridged` to false (`src/core/network/slip/SlipQc.cpp:2164`; row at :2898-2902, :2929, B). The row reads `verdict=none`, action `bridge`, `carry=dd`, throttled per station. The counters `gap_reentry_freed_*` and `inject_stale_skips` are run totals: read event minus control.
6. **Event 5, falsifier: "any carried entry of the station injected after the return".** On the float vehicle (`ambiguity_vehicle float_kf`) a past-horizon entry is still injected, at birth variance, by design (`src/core/network/carry/LevelingCarry.cpp:337-356`, B). The falsifier becomes: any carried entry injected after the return with a variance below its birth variance.
7. **Event 1, prior on record: "It failed before trip generation was armed".** On posture g it failed before the IF-TD clock-event rule was armed and passed without the trip generation (`docs/record/refound/SMX2_scenario_matrix_current_golden_2026-11-09.md:232`, B).
8. **Section 4.1, "the smoke checks parsing and paths only", and section 4.3 item 3, L5Q presence confirmed "from the smoke".** By the never-fired rule (`src/core/network/carry/Inject.cpp:730-740`, B) the 10-epoch smoke of every event file exits 1, since its events at TOW 264600 or later never fire; a smoke cannot show the L5Q slips firing. Smoke the controls, and the event files with their events removed or shifted into the smoke.
9. **Section 1.2: satellite code biases from the product, "column `source=product`".** In `truth_bias.csv` GPS C1C, C2W and Galileo C1C come from the product, Galileo C5Q is `product+synth`, and Galileo C7Q and GPS C5Q are mostly `synth`; all are constant per run.
10. **Section 1.3: "No rewrite operation makes a step".** The `slip` operation of `scripts/rapid_arm.py:60` (B) makes integer-cycle phase steps. No rewrite operation makes a timed code step.
11. **Event 2, open point, and decision 3 (unconfirmed, not marked): "If L5Q is dropped earlier, those 22 events never fire".** The parser keeps every code and admission by priority is downstream of the injector (`ApplicationNetworkSetup.cpp:323-324`; no priority filter in `RinexParser.cpp` or `MultiRinexSync.cpp`, B), and L5Q is present at all 22 receivers, so the L5Q slips are expected to fire; not confirmed by a run.
12. **Section 4.1, engine (addition).** The executable is to be built from tree `7fb0737`: main `5ddc422` does not read `network.ring_fix.entry_revalidate`, a key of control_ringfix (the check of unread keys holds on B only).
13. **Event 8 (minor count).** control_ringfix differs from the reference in 60 keys (54 posture keys, `truth_root` and 5 run keys), as stated; the ring_fix keys are 17, not 18.
14. **Section 1.5 (note).** The satellite events at TOW 264600 sit two epochs inside hour 1, which ends at 264660.
15. **Configuration files (outside the document).** The `_comment` of control_sim, control_ringfix and every event file says only the run paths and `network.inject` changed: false for control_sim (no injection; durations and start offset changed) and incomplete for the others; the `sat_*.json` files keep the reference's "GENERATED ... do not hand-edit" header. The comments are to state the duration and start-offset changes and, for control_sim, the absence of an injection.

### Line numbers that moved

All on B; each claim is confirmed.

- Latch window: `ApplicationNetworkSetup.cpp:355-359` (cited :357-361). The latch's single call: :710-711, inside the cited 694-711.
- Held-out user reading the injected epoch: `EmitStore.cpp:650-651` (cited 650-652).
- Signal-scoped outage: `Inject.cpp:443-458` (cited :463-473); outage windows with the 0.25 s guard at `Inject.cpp:36-55`; INJECT rows also at :464, :515, :529, :551, :614.
- `rx_clock_step` refuses `signal` and `sat`: `GnssConfigLoadNetwork.cpp:14244-14257` (cited 14252-14257); receiver clock not carried: :5189-5191 (cited 5190).
- Geometry-free pin row: `Sbasis.cpp:1546-1547`, inside the cited 1543-1549.
- Clock anchor decouple: `ClockAnchor.cpp:484-488` (cited 484-486); golden_sim_refound.json :320.
- Phase-bias hold generation: `PbHold.cpp:231-237`, inside the cited 185-238.
- Station readmission: `SolveStations.cpp:362-375`, inside the cited 356-381.
- Gap cliff and prune: `LevelingCarry.cpp:346`; `SignalRoster.cpp:1403-1406`.
- Ring-fix release row: `RingFixSeams.cpp:793, 911-919` (cited 907-919).
- IF-TD clock event rule: also `SlipQc.cpp:1499`. Emitted code jump test: also golden_sim_refound.json:232. Never-fired rule: also `RunEnd.hpp:56`.

### What stands

- The Outcome stands. The latch statement is correct: one value per satellite per run, so a mid-run broadcast group-delay step cannot be injected. Every event file differs from its control only in the injection and the paths; control_sim differs from the reference only in the two durations, the start offset and the output path; sat_control is identical to control_sim apart from `_comment` and path. The window (first epoch 261060, last 268230, latch [261045, 268245)) and the half-epoch back-off hold.
- Event 1 (detector, response site, counters, trace rows, re-coupling) is confirmed in the code; the record shows it passing on the reference posture.
- Event 2 NONE stands as "no detector and no mark" (inferred, not run), with the reason of item 1: the time-linked ionosphere-free pin opposes the step with no release, which is the invariant-1 gap the document names; the future bar on the code column cannot be met as written (item 2).
- Event 6 (network-wide pin pair, two registered code channels per satellite, no re-election possible) and event 8 (eq34 required, release cause `slip`, `carry_reopened`) are confirmed.

Decisions put to the author, after correction:

1. Approve the event list: stands once corrections 1 to 10 are worked into the pre-registrations.
2. Satellites (G22 / G13 with the pre-run pivot and IF-TD-reference check, or the matrix rule): stands.
3. Removal of the 22 L5Q take-back events: rests on a never-fired risk that the code reading makes unlikely (item 11); removal remains harmless for the network (band 5 is not modelled) and is unchecked for the held-out user.
4. Event 5, the 3 h validation variant: stands.
5. Event 8 posture (control_ringfix from the observed-data ring-fix configuration, or rebuilt from `kq12x_sim`): stands, and additionally needs the executable tree fixed at `7fb0737` (item 12).
6. Event 7, the H0 / H1 pre-registration: stands.
7. Gaps (a time-resolved group delay, a code-only satellite kind with signal scope): stands.
8. Fresh runs against resumed runs: stands.

### Left unconfirmed

- User-engine reactions to a hold generation, a clock discontinuity mark or a correction gap.
- Event 6: single-band behaviour of a satellite reduced to band 1, and the pin pair across a 10 min absence.
- Event 7: the gross-LOS screen, the prefit screen report and `cold_clock_seed` under a 299.8 km step.
- The counter of the readmit seed mirrors (event 5); the ring-fix truth audit (event 8); reuse of `grade_cell` on fresh run pairs.
- `SimConfig.cpp` line numbers; G15 elevation; `runs_nom/_rap_ad_n146/arm.json` (not opened).
- Pivot or IF-TD reference status of G22, G13 and G15 at the event epochs.
- Whether the L5Q slips fire (item 11).

The full errata is at `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/EVL_errata.md`.
