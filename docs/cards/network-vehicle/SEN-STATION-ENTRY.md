# SEN: a refused station is kept when it can be, and returns as a cold entry when it cannot

**STATUS**

- **id:** SEN
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-22 on the author's order of that day ("Yes, build it"). On the broadcast clock, day 129 of 2024, PALM00ATA is refused for its first 25 epochs because one local column, the receiver phase bias of E5a, has no row (its two or three E5a satellites have no broadcast clock yet). It then enters a converged network with a wet delay of -23.4 m at 0.19 m, which halves the network's held share for an hour. Two defects, two keys, both default off: (A) the refusal of a whole station for one rowless local column; (B) the return of a refused station as a confident first sighting instead of a re-coupling.
- **dates in the text:** 2026-11-22
- **knobs / posture:** new, default off: `network.station_rowless_column_park` (A), `network.station_entry_cold` (B). No reference file armed.
- **code:** branch `net/station-entry-2026-11-22` from `int/2026-11-19` at `33c5fef`, worktree `D:/GnssProcessorSupport/worktrees/stentry`.
- **run dirs:** `D:/GnssProcessorSupport/runs_nom/_rap_sen_*`; report `D:/GnssProcessorSupport/gates/station_entry/REPORT.md`.
- **source:** the order `D:/GnssProcessorSupport/gates/station_entry/ORDER.md` (the author's words relayed by the managing session); the managing session's scans in `D:/GnssProcessorSupport/gates/broadcast_reference/manager_check/`; runs `runs_nom/_rap_brp_f129_{c1,a1,a3}_2` (engine `gnss_processor_int1120a.exe`).

**Outcome.** Pre-registered. The code read places the refusal at the one drop site of the station loop and the bad return in three places: the station's entry is not seen as a re-coupling (it was never in the solve, so the solve track records no return), the chain's tree kept the station's edges, born while it was out, and re-armed them at its entry as kept pins, and the wet-delay and leveling carries harvested from the first accepted epoch are folded as confident priors from the second epoch on. The build parks a rowless local column (A) and makes the entry a cold one (B); readings S1 to S5 are fixed below.

> Results are appended TO THIS FILE, below the text, under dated headings.

---

## 1. The case (read from the runs, 2026-11-22)

- Arm a3 (`_rap_brp_f129_a3_2`): PALM00ATA is dropped at every epoch from 00:00:00 to 00:12:00 by the addStation positive-definite test (the null vector's family is RPB at every drop). It enters at 00:12:30 (TOW 259950), the epoch E10 and E25 receive their first broadcast clock of the day. In arm c1 (the CODE clock) it is in the solve from the first epoch.
- Its states at the entry epoch, a3 against c1 at the same epoch (`network_rx_states.csv`, `network_zwd.csv`, `network_ionosphere.csv`): receiver clock +36.7 m (σ 0.58 against 0.23 m), Galileo ISB -14.30 m against -2.03 m, wet delay -23.43 m (σ 0.19 m, carried 0) against -0.005 m (carried 1), slant ionosphere of E07 (25° elevation) +27.9 m against -2.0 m. Post-fit code residuals of G03 (17°) 29 m and of G19 (17°) 13 m at that epoch; the gross line-of-sight screen declares both at the entry epoch (cold bound) and keeps them in its hold. In c1 neither is gross.
- At the next epoch the prior-residual census reads the folded wet delay at a standardized residual of -212 and the folded leveling priors of PALM's GPS arcs at -6 to -23, all labelled CLEAN and folded (`network_events.csv`, family `zwd` and `pl`).
- While PALM was out, these kept running on its data: the chain's tree (PALM's edges born at TOW 259200 in `network_chain_edges.csv`, rehost events at TOW 259860), the IF triple-difference slip test (`network_if_slip.csv` from TOW 259230), the raw slip history, the wind-up arcs and the code companions (`SCC companion_open` at TOW 259200).

## 2. Design note

### A. The refusal

- **Where.** `src/core/network/solve/SolveStations.cpp`, the catch of the addStation call (the warn "addStation rejected ... dropping it this epoch" at line 263): any exception from `NetworkNormalEquations::addStation` drops the station for the epoch. The test that throws is the positive-definite test of the station's local block, which fails when one local column has no row and no prior.
- **Why the RPB column is free here.** Under `network.phase_datum_eq34` the receiver phase bias of each tracked phase signal is a local column (`NetworkParamKind::RxPhaseBias`, `src/core/network/assembly/ParamRegistry.cpp`, the block at line 232); with `network.phase_datum_eq34_rpb_in_block` its carry rides in the block, but a station that has not yet entered has no carried value, so the column's only information is its own phase rows. PALM's E5a rows exist only on satellites without a clock, which the epoch drops, so the column is declared and has no row.
- **Fix chosen.** A local column with no data row and no prior row in the epoch is left out of the station's block for that epoch (parked): no estimate, no harvest, its carried value (if any) untouched, its gap clock running. The rest of the station enters. The structural reason: such a column is unobservable this epoch by construction, so the epoch has nothing to say about it, and the project's rule is to keep what is usable. A pin or a weak pseudo-row would put an invented value into the solve. Counter per column family (kept stations, parked columns); one event per park episode (the station, the column, the epochs parked).

### B. The return

- **The entry is not a re-coupling.** `networkSolveStationsTrack` (`src/core/network/solve/SolveStations.cpp`, line 337 on) counts a return only when the station was in the solve before (`ever_in_solve`); a station refused from the first epoch enters as a first sighting with no event, against standing invariant 7 (a station absent while the network runs is decoupled; its entry is a re-coupling).
- **The chain kept the station's edges.** `src/core/network/solve/ChainDatum.cpp`: the election builds its tree over every station of the roster with arcs, not over the stations in the solve, so PALM's edges were born at TOW 259200 and carried as kept edges for 25 epochs while PALM never reached the solve; at its entry they were applied as kept pins (`same_edge`, `Kept`, row armed), with no returning flag on PALM.
- **The first harvest is folded as confident.** The wet delay (`src/core/network/carry/ZwdCarry.cpp`, the harvest at line 364 on, the first-fold edge at line 318) and the phase-leveling carries take the entry epoch's posterior as the carried value and fold it from the next epoch with that epoch's variance plus the process noise, a stiff time link (standing invariant 1) born from one epoch.
- **Which of these makes the -23.4 m** is not proven by the read: the entry solve is pulled by stiff rows that are not the station's data, and both the kept pins and the cold-bound declarations act at that epoch. S2 separates them (section 4).
- **Fix chosen.** A station's entry after an absence while the network ran (including an absence from the first epoch) is a re-coupling event with its identity (refused at, returned at, epochs out, the family that failed). At that entry the station is a cold entry: its carried states (wet delay, leveling, receiver clock, receiver code bias, receiver phase bias, slant ionosphere) are dropped and re-born loose from the entry epoch, and the chain treats the station as returning (its edges are re-elected, not kept). While a station is out nothing of it is harvested or folded (already the case: it is not solved); its edges leave the tree.
- **Deferred, with triage.** The slip detectors and the wind-up arcs keep running on the data of a refused station; that is what the uninterrupted run does too and is field-inert for the entry (the arcs are kept, the phase is continuous). Not deferred: everything listed in the fix.

## 3. The change

- `network.station_rowless_column_park` (A) and `network.station_entry_cold` (B), each a ledger row in the same commit, each identical when off.
- Run-start line per key, run-end counters at both edges, events under `network.trace_topology`.

## 4. Readings, fixed before the first run (2026-11-22)

- **S1 identity.** Both keys off: the two reference files, the frozen lineage `golden_sim_rf0hv18` and the BeiDou lineage, 2 h each, the new engine against its base (`33c5fef`), same build tree: identical `products.txt` and traces apart from wall-time and path fields.
- **S2 the injected event on the simulator** (the reference file with the broadcast clock, start 00:30:15, 2 h): one station whose satellites on one signal are all made unusable for 25 epochs (an outage of that signal at that station, or of the satellites' clocks), placed after the network converged. Keys off: the refusal and the return reproduced (drops counted, the station's wet delay at its return against truth). A on: the station kept, its column parked, counted. B on alone (A off): the return is a re-coupling event, the station's wet delay at the return within three of its first-epoch sigma of truth, never beyond 0.5 m after five epochs. The truth audit's held, right and wrong counts for each arm.
- **S3 the observed case.** Day 129, the run session's arm a3 (2 h, same configuration), both keys on: PALM00ATA's first wet delay within its first-epoch sigma of the control's (c1) value, no epoch beyond 1 m after minute 5, the position check within 0.1 m after minute 30; held share over minutes 60 to 120 against the proof pair's level (day 129 without PALM).
- **S4 no harm where nothing is refused.** Day 129 on the CODE clock (the field reference file), both keys on against off, 2 h: identical, or each difference explained row by row.
- **S5** the full Debug suite, `nom_epoch_lint`, `nom_n4_verify --whole`.
