# SMX: the standing fault-scenario matrix of the network producer

**STATUS**

- **id:** SMX
- **topic:** harness
- **status:** open
- **summary:** PRE-REGISTERED and RUN 2026-10-24 (version 1, label `smx1`): 22 of 55 cells pass. Slips on one signal, the single-signal loss of lock, a satellite outage, an arc erase and a 1 m receiver clock step pass on every posture; the satellite clock step and the slip storm fail on every posture (consumer 0.15 to 2.4 m and 0.08 to 0.92 m); the clock drift fails on the golden only; a 1 m station step breaks the consumer on the self-contained regime only; a health exclusion costs the consumer 8 cm; most other failures are transients of the faulted station's own ZWD and slant ionosphere. Gates G0 (identity), G1 (unit suite), G2 (seeds) and G4 (control, IDENTICAL) pass. Every fault scenario the network producer must survive becomes a row of a standing matrix: one injected event on a converged seed of the current GPS + Galileo sim golden (60 epochs; 240 for the storm), graded event minus a matched null leg on datum-invariant quantities against a bar frozen before the first run, on each posture of the goldens' clock level (the golden as armed; the self-contained regime of card SCL2B; the native posture of card NBP), runnable on any Release exe the way the G5 identity gate runs. Four injector shapes are added default off (a signal-scoped outage, a whole-station outage, a broadcast health flip, a held-coordinate step). Nothing is armed.
- **source:** the author's statement of 2026-10-24 that every fault scenario (stations moving, signals dropping, slips on one or more signals at any rate, satellites unhealthy) should already be tested rapidly in simulation, after the triple-frequency defect of cards [L5B](L5B_band5_receiver_delay_2026-10-10.md) and [R1D](../record/refound/R1D_station_level_datum_2026-10-23.md) (a single-signal loss of lock at a phase-datum anchor station) reached the field first; the event harness ([HARNESS](../cards/harness/HARNESS-REF.md), [seeds](../cards/harness/HARNESS-SEEDS.md)); [PLANT](../record/refound/PLANT_planted_coordinate_offsets_2026-10-19.md) (the coordinate response curve, the reference of the step rows); [SCB](../record/refound/SCB_sim_clock_error_bed_2026-10-24.md) (the clock-error fixture, its grader and the posture key sets)
- **knobs / posture:** no knob armed, no golden or ledger edited. Built default off: `network.inject.events[]` kinds `sat_health` and `station_step`, and three `outage` scopes (`signal` list, `sat` "all", the re-acquisition `delta_cycles` and `flag_lli`). Postures (card SCB's key sets): `g` = `config/golden/golden_sim_refound.json` as armed; `c` = GPS only plus the regime of card SCL2B; `n` = `c` plus the native broadcast-clock a priori of card NBP; `gx` = `g` plus `network.sat_health` "exclude" (the health row only); `f` = `config/golden/golden_field_refound.json` (seed only in version 1). Every sim posture starts at 00:30:15 (`network.start_offset_hours` 0.504167).
- **dates in the text:** 2026-10-24
- **hashes / run dirs:** source branch `claude/sweet-poincare-454cec` on main `7bdcebc` plus card SCB's `77a2154`; exes `gnss_processor_smx_main.exe` (main `7bdcebc`, sha256[:16] `10426f27f2e231b8`) and `gnss_processor_smx1.exe` (this card's kinds, sha256[:16] `cac2bbdea1d05965`) in `D:/GnssProcessorSupport/worktrees/smx/build/x64-Release/`; seeds `evseed_smx_{g,c,n,gx,f}` and legs `evleg_smx_<label>_*` under `D:/GnssProcessorSupport/runs_nom`; gate outputs `D:/GnssProcessorSupport/gates/smx/<label>/`; matrix `scripts/smx/matrix.json` version 1; tools `scripts/scenario_matrix.py`, `scripts/event_leg.py`
- **gates (frozen at pre-registration):** section 7

**Outcome.** Question: does the network producer, on each posture of its clock level, absorb every fault scenario in the matrix within a bar set by its own null-leg spread, so that a scenario is tested on the sim before it reaches the field? Decisive result (version 1, 65 legs, about 45 min, deterministic): 22 of 55 cells pass. A satellite clock step and a slip storm are not survived on any posture; the clock drift only on the self-contained regime and the native posture; a 1 m station step breaks the consumer on the self-contained regime. Verdict: the matrix works as a standing gate and names defects before the field does; two of its predictions were refuted. Standing: section 9.4; nothing armed, the findings and the bars of the station-local channels are the author's decisions.

## 1. The question

A fault that reaches the field before the sim has tested it costs a field campaign to find. The defect of cards L5B and R1D was such a fault: one signal of one satellite lost lock and came back with its partner at a station that anchors the phase datum, and the station's level moved the whole network's clocks. The tools to stage such a fault in the sim existed in three places (the injector of the event harness, the RINEX operations of `scripts/rapid_arm.py`, the simulator's own scripts), but none ran as a standing gate: the harness defaulted to a retired golden, its newest seeds were GPS-only from 2026-10-02 (before card GAD), its `outage` dropped a whole satellite, and no health flip or coordinate step existed. The only standing gate, G5, is byte identity between two exes, with no behaviour bar under a fault.

The question of this card: on the current GPS + Galileo sim golden and on each posture of its clock level, does the producer absorb each scenario of a fixed list within a bar frozen before the run, and which scenario on which posture does not?

## 2. The matrix

Version 1 (`scripts/smx/matrix.json`, frozen 2026-10-24). Every leg resumes the ninth history snapshot of its posture's seed (the seeds write one every 1800 s from the first emitted epoch, so this is 4 h after the 00:30:15 start, TOW 275580; its emitted-epoch suffix is `ep0485` on `c` and `n`), runs 60 epochs (the storm 240) and carries its event at `at_ep` 10, past the resume transient. The local events act at `S48000SIM`, the roster station nearest the held-out consumer `SZZ000SIM` (579 km; card PLANT's station). The satellite is `G25` (elevation 66 to 75 degrees at `S48000SIM` and 67 at the consumer through the 60-epoch window, at least 52 and 47 through the storm), replaced by `G12` on a posture where `G25` is a GPS clock pivot at the snapshot epoch (the rule is applied by `make` from the seed's S line and PIVOT events, and recorded in the plan). GPS satellites only, so the same events run on the GPS-only postures.

| row | event (all at the event tow) | why |
|---|---|---|
| `slip1` | slip, `L1C`, 1 cycle, no LLI | small slip on one signal |
| `slip100` | slip, `L2W`, 100 cycles, no LLI | large slip on one signal |
| `slip2sig` | slips `L1C` +9 and `L2W` +7 together | both signals of one satellite; the geometry-free-blind pair |
| `storm` | GPS slips on all 60 stations, Poisson at 6.8 per station-hour (the campaign rate), size uniform 12 to 125 cycles, LLI with probability 0.506, elevation at least 15 degrees, 240 epochs, schedule seed 1 | a slip storm, the `scripts/ev_dose_legs.py` form on the current bands |
| `lol` | outage of `L2W` (its code, Doppler and strength with it) for 60 s while `L1C` tracks, back with +3 cycles and LLI, at the station anchoring the satellite's `L2W` family at the snapshot epoch | single-signal loss of lock with return at a phase-datum anchor station: card L5B's shape |
| `sat_out` | outage of the satellite at the station, 600 s | whole-satellite outage with return |
| `sta_out` | outage of every satellite at the station, 600 s | whole-station outage with return |
| `sat_clk` | satellite clock step +0.5 m | the wire must carry -0.5 m on that satellite; the expectation is subtracted from the clock channel |
| `rx_clk1m` | receiver clock step +1 m at the station | receiver clock jump |
| `rx_clk1ms` | receiver clock step of 1 ms (299792.458 m) | the clock-steering jump of a receiver |
| `anc_cont` | anchor handover, continuous, every family of the satellite | the datum's own handover |
| `anc_cold` | anchor handover, cold, every family | a re-realized gauge |
| `arc_erase` | arc erase (the production RAIM erase) on one healthy arc | an unnecessary erase |
| `health` | the satellite's broadcast health flagged unhealthy for 300 s | satellite unhealthy; read only by `network.sat_health` (posture `gx`), unread on `g`, `c` and `n` |
| `step1cm` | the station's held coordinate stepped 1 cm East, permanent | a station moves |
| `step10cm` | 10 cm East | |
| `step1m` | 1 m East | |
| `clkdrift` | the clock fixture `D:/GnssProcessorSupport/fixtures/simnet60_clk_col_24h` against the quiet fixture, fresh 2 h arms from 00:30:15 | a drifting satellite clock (card SCB's bed and grader) |

Each row runs on `g`, `c` and `n`; `health` also on `gx`. Field rows are not part of version 1: the field seed is written (section 4) so that a field posture can be added as a version 2.

## 3. The new event kinds

`network.inject.events[]` (`include/network/carry/Inject.hpp`, `src/core/network/carry/Inject.cpp`, the loader in `src/core/config/GnssConfigLoadNetwork.cpp`), all at the existing injection site ahead of `computeModels`, every slip detector and assembly:

- **`outage` scopes.** A `signal` list (`"L1C;L2W"`) drops only those signals, each phase code with the code, Doppler and strength observables of its band and tracking attribute, while the satellite's other signals track; a record left with no code or phase leaves as a missing record. `sat` "all" drops every satellite of the station. An optional `delta_cycles` is added to every dropped phase observable from the end of the window on (the re-acquisition's new integer) and `flag_lli` raises LLI 0x1 on them at the first epoch back. The plain outage keeps its old behaviour and row text.
- **`sat_health`.** The satellite's broadcast health is flagged unhealthy from `tow` for `duration_s` (0: to the run end). The span is written into the run's health series at arm time, where the readers of `network.sat_health` "pivot" and `network.pivot_election` query it. The "exclude" door runs at setup on the broadcast as parsed, so under "exclude" the injector removes the satellite's observations at every station (the consumer included) while the flag holds. With no reader armed the flip fires, nothing acts on it, and the arming line says so (UNREAD).
- **`station_step`.** The station's held coordinate moves by (`east_m`, `north_m`, `up_m`) in its local frame at the first epoch at or after `tow`; with a positive `duration_s` it moves back, restored to the bit. The coordinate is pinned in the solve, so the step reaches the rows through the three copies the epoch reads (the station coordinate, the model state `computeModels` evaluates at, the position linearization points), written together. The observations are unchanged: a step of +d is the antenna moving by -d.

Each is an event at both edges: an `INJECT` row with action `injected` at onset and one with action `released` at the closing edge (the outage's return, the flag's clear, the coordinate's return), and a run-end clause counting both. An armed event that never acts is the existing hard run-end error. With `network.inject` absent nothing is reachable (the existing byte-identity contract; gate G0).

The leg generator `scripts/event_leg.py` was brought to the current goldens: `DEFAULT_GOLDEN` is `golden_sim_refound.json`; the base's `start_offset_hours` is kept by `write-seed`; seeds and legs go under `--runs-root` or `RAPID_RUNS_ROOT`; `--event` takes a list (one scenario of several events); and `resume_rxcb_lin_restore` is armed only where `rx_code_bias_datum` is off (the loader refuses the pair, and both refound goldens arm the datum).

## 4. The seeds

`python scripts/scenario_matrix.py seed <posture>` writes `evseed_smx_<posture>` under `D:/GnssProcessorSupport/runs_nom` through `event_leg.py write-seed` on the posture's golden and key set, 6 h, a snapshot every 60 emitted epochs, with the exe `gnss_processor_smx_main.exe` (main). Each passed a 10-epoch smoke first. Seeds are bound to posture and fixture; a fingerprinted knob (here `network.sat_health`) needs its own seed, which is why `gx` exists. The record is in [HARNESS-SEEDS](../cards/harness/HARNESS-SEEDS.md) under the 2026-10-24 heading.

## 5. The grade

Every cell is the event leg minus its null leg, both resumed from the same snapshot with the same config except `network.inject`, over the post-event window (from the event tow to the leg's end). The producer is deterministic, so the two legs agree exactly before the event (the pre-event difference is recorded per cell). Channels:

| channel | measured E (event minus null) | factor | floor |
|---|---|---|---|
| `clk` | wire satellite clock, de-commoned per epoch and constellation (median), max over post-event epochs of the RMS over satellites (mm) | 3 | 5 mm |
| `pb` | wire phase bias, de-commoned per epoch, constellation and signal, on the (satellite, signal) pairs whose hold generation `gen` is the same in both legs (a new generation is a declared re-realization and is counted, not graded); max of the per-epoch RMS (mm) | 3 | 5 mm |
| `cons_peak` | held-out consumer `SZZ000SIM`: max over post-event epochs of the vector difference of its ENU errors (mm) | 3 | 10 mm |
| `cons_plat` | the consumer's mean vector difference over the last 20 epochs (mm) | 3 | 5 mm |
| `zwd` | station ZWD error growth against truth: max over stations and epochs of the event's absolute error minus the null's (mm) | 3 | 5 mm |
| `iono` | slant ionosphere `iono_total_m`, de-commoned per station and epoch, max absolute difference over lines of sight (mm) | 3 | 20 mm |
| `emit` | post-event epochs the null emitted and the event leg did not (a withhold is an adaptation failure) | | 0 |

**The bar.** For each channel, S is the null leg's own spread: the white-noise equivalent sqrt(mean(d^2)/2) of the null's first differences over the same window, of the same per-epoch quantity (the de-commoned null clock and phase bias per satellite and signal, the null ZWD per station, the de-commoned null slant ionosphere per line of sight, the null consumer's ENU error vector). The bar is max(factor x S, floor), plus for the step rows card PLANT's response at the step size: 49.8 mm per metre on `clk` (1.5 times PLANT's 33.2 mm per metre of East in hour 1), 82.4 on `pb` (1.5 times 54.9), 10.2 on `cons_peak` and 5.1 on `cons_plat` (3 and 1.5 times PLANT's 3.40 mm per metre consumer shift). A step therefore passes when the warm step costs no more than the same error planted cold, with margin. The floors are the sizes below which no user notices (a few millimetres of wire, a centimetre of position); above them the bar grows with the wire's own variability.

**The drift row** is two fresh 2 h arms per posture (the clock fixture and the quiet one, same posture, from 00:30:15), graded in hour 2 with card SCB's `scripts/simnet_clock_truth_grade.py`: `clk_truth` is the event's wire clock level against truth minus the null's (bar max(1 x S, 5 mm), S the null's own hour-2 level); `cons_p50` is the event's consumer 3D p50 minus the null's (bar max(3 x S, 10 mm), S the null consumer's hour-2 first-difference spread).

**A cell passes** when both legs exit 0, the event leg wrote at least one `INJECT injected` row, and every channel is at or below its bar. The report names, per cell, the channel nearest its bar with its measured value and bar.

## 6. The recipe

```
set RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom
python scripts/scenario_matrix.py seed g --exe <exe> --launch     # once per posture and fixture (also c, n, gx)
python scripts/scenario_matrix.py make --label <label> --exe <Release exe>
python scripts/scenario_matrix.py run --label <label> --jobs 3
python scripts/scenario_matrix.py grade --label <label>
python scripts/scenario_matrix.py report --label <label> [--control <label2>]
```

`make` resolves the targets per posture, writes every leg through `event_leg.py` (nulls shared by every 60-epoch row of a posture) and the drift arms, and records the plan with the exe's sha256 (`run` refuses an exe that changed since `make`). `run` launches nulls first, at most three at a time. A control is the same matrix made under a second label with the same exe; `report --control` compares every measured value and spread of the shared cells and must read IDENTICAL.

## 7. Gates (frozen before any leg ran)

- **G0, identity.** With `network.inject` absent the new exe is byte-identical to main's on the 2 h sim golden (from 00:30:15) and the 2 h field golden: `products.txt`, `closed_loop_trace.csv`, `network_solution.csv` and `network_epoch_status.csv` hash-equal between `gnss_processor_smx_main.exe` and `gnss_processor_smx1.exe`.
- **G1, unit suite.** `tests/network/test_network_inject_scenarios.cpp` passes (new kinds parse and refuse by name; each acts on exactly its scope and window; both edges counted; the coordinate returns to the bit; a step on an unknown station fails the run end), and the existing injector suite `test_network_inject.cpp` passes unchanged.
- **G2, seeds.** Each seed exits 0 over 6 h with its snapshots, and its fingerprint is recorded.
- **G3, the matrix.** One full run of version 1 on the new exe (which equals main whenever nothing is injected, gate G0), every cell graded and reported in section 9. A FAIL cell is a finding of this card, not a defect of the gate: the gate's purpose is to show it before the field does.
- **G4, control.** The same exe on a second label (a subset of rows and postures, named in section 9) reads IDENTICAL.

Predictions (written before the run): `clkdrift` fails on `g` and passes on `c` and `n` (card SCB: the golden's consumer takes the a priori's error through the phase-clock hold); `health` is UNREAD on `g`, `c` and `n` (no reader is armed) and measures zero there; `sat_clk` passes on `g` (the 2026-09-10 validation: the clock absorbed 100.4 % of the step in one epoch); `step1cm` and `step10cm` pass under the PLANT reference.

## 8. Not in version 1

- Field rows (no truth: only the wire, the consumer and the emit channels apply). The field seed exists for version 2.
- An orbit-error kind and a troposphere step (no injector kind; the simulator can stage both in a new fixture).
- Rows at a GPS clock pivot or at the clock datum station (`S30000SIM`), and Galileo targets on the golden posture.
- Bars on the detection side (which tier acted, how fast): the counters are in each leg's `network_events.csv` and run-end clauses, read by hand.

## 9. Results

### 9.1 Gates G0 to G2 and G4 (2026-10-24)

- **G0, identity: PASS.** `network.inject` absent, 2 h each, `gnss_processor_smx_main.exe` against `gnss_processor_smx1.exe` (sha256[:16] `cac2bbdea1d05965`): the sim golden from 00:30:15 (241 emitted) and the field golden (240 emitted) hash-equal in `products.txt`, `closed_loop_trace.csv`, `network_solution.csv`, `network_epoch_status.csv` and `network_events.csv` (arms `_rap_smx_g0_{sim,fld}_{main,new}`, 382 to 384 s each).
- **G1, unit suite: PASS.** The injector suites 52 of 52 (the 10 new cases and the parametrized refusals of `test_network_inject_scenarios.cpp`, and `test_network_inject.cpp` unchanged); the Release `gnss_tests.exe` 3588 passed, 0 failed, 33 fixture-gated skips. `scripts/nom_epoch_lint.py` and `scripts/nom_n4_verify.py --whole` pass. A 15-epoch smoke leg with all four shapes at once (`evleg_smx_smoke_kinds_c`) fired each, wrote both edges and the run-end clause.
- **G2, seeds: PASS.** Five seeds, each 720 emitted, exit 0, 12 snapshots; fingerprints in [HARNESS-SEEDS](../cards/harness/HARNESS-SEEDS.md).
- **G4, control: PASS.** Label `smx1ctl` (rows `slip100`, `sta_out`, `step1m` on `g` and `c`, its own nulls), same exe: `report --control smx1` reads IDENTICAL, every measured value and spread equal in the 6 shared cells.

### 9.2 G3: the matrix on the new exe (label `smx1`)

65 legs, all exit 0, 5133 leg-seconds (a 60-epoch leg 61 s at the median and 94 s at most; a 240-epoch leg or a 2 h drift arm 110 to 235 s): about 45 min of wall at two at a time. Targets: `G25` on every posture (no GPS pivot at the snapshot); the `L2W` anchor of `G25` was `S24000SIM` on `g` and `c` and `S14000SIM` on `n` and `gx`; the storm drew 646 slips. Outputs `D:/GnssProcessorSupport/gates/smx/smx1/` (`plan.json`, `grades.json`, `report.md`).

Cell: verdict, the channel nearest its bar, measured / bar (mm; emit in epochs).

| row | g | c | n | gx |
|---|---|---|---|---|
| slip1 | PASS zwd 7.8/14.7 | PASS iono 91.9/230.2 | PASS zwd 6.9/19.0 |  |
| slip100 | FAIL zwd 190.1/14.7 | FAIL zwd 254.3/18.6 | FAIL iono 5146.6/222.4 |  |
| slip2sig | FAIL zwd 46.4/14.7 | FAIL zwd 58.1/18.6 | FAIL zwd 88.5/19.0 |  |
| storm | FAIL zwd 4276.2/9.3 | FAIL zwd 6052.7/12.4 | FAIL zwd 15533.6/13.3 |  |
| lol | PASS iono 127.6/234.5 | PASS iono 129.2/230.2 | PASS NO-EFFECT emit 0.0/0.0 |  |
| sat_out | PASS iono 152.5/234.5 | PASS iono 178.9/230.2 | PASS iono 88.5/222.4 |  |
| sta_out | FAIL zwd 137.6/14.7 | FAIL zwd 195.7/18.6 | FAIL zwd 120.6/19.0 |  |
| sat_clk | FAIL cons_peak 147.1/21.1 | FAIL cons_peak 2354.4/61.4 | FAIL cons_peak 1186.5/29.4 |  |
| rx_clk1m | PASS zwd 0.0/14.7 | PASS zwd 0.1/18.6 | PASS cons_peak 0.1/29.4 |  |
| rx_clk1ms | FAIL zwd 74.9/14.7 | FAIL iono 607.6/230.2 | FAIL zwd 209.1/19.0 |  |
| anc_cont | FAIL iono 308.4/234.5 | FAIL iono 296.5/230.2 | FAIL zwd 79.2/19.0 |  |
| anc_cold | FAIL iono 520.8/234.5 | FAIL zwd 116.0/18.6 | FAIL zwd 100.4/19.0 |  |
| arc_erase | PASS iono 169.8/234.5 | PASS iono 197.0/230.2 | PASS iono 88.2/222.4 |  |
| health | PASS UNREAD emit 0.0/0.0 | PASS UNREAD emit 0.0/0.0 | PASS UNREAD emit 0.0/0.0 | FAIL iono 621.1/221.5 |
| step1cm | FAIL zwd 15.7/14.7 | PASS zwd 11.5/18.6 | PASS zwd 7.5/19.0 |  |
| step10cm | FAIL zwd 156.8/14.7 | FAIL zwd 115.1/18.6 | FAIL zwd 74.9/19.0 |  |
| step1m | FAIL zwd 1496.8/14.7 | FAIL zwd 1735.1/18.6 | FAIL zwd 107.9/19.0 |  |
| clkdrift | FAIL cons_p50 168.6/30.2 | PASS clk_truth -0.1/26.6 | PASS cons_p50 20.8/38.9 |  |

22 of 55 cells pass.

Channels (measured / bar, mm; X marks a channel over its bar):

| row | posture | clk | pb | cons_peak | cons_plat | zwd | iono | emit | clk_truth | cons_p50 | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| slip1 | g | 0.3/5.0 | 0.3/192.7 | 0.1/21.1 | 0.0/21.1 | 7.8/14.7 | 89.5/234.5 | 0.0/0.0 |  |  | zwd S48000SIM |
| slip1 | c | 0.5/5.0 | 0.5/257.9 | 3.0/61.4 | 1.9/61.4 | 5.7/18.6 | 91.9/230.2 | 0.0/0.0 |  |  | zwd S48000SIM |
| slip1 | n | 0.2/37.1 | 0.2/300.7 | 1.2/29.4 | 1.0/29.4 | 6.9/19.0 | 80.7/222.4 | 0.0/0.0 |  |  | zwd S48000SIM |
| slip100 | g | 1.0/5.0 | 1.0/192.7 | 0.1/21.1 | 0.1/21.1 | 190.1/14.7 X | 2697.4/234.5 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S24000SIM/E02 |
| slip100 | c | 1.5/5.0 | 1.5/257.9 | 7.3/61.4 | 4.0/61.4 | 254.3/18.6 X | 1411.5/230.2 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G25 |
| slip100 | n | 7.1/37.1 | 6.8/300.7 | 15.0/29.4 | 8.2/29.4 | 398.9/19.0 X | 5146.6/222.4 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G25 |
| slip2sig | g | 0.4/5.0 | 0.4/192.7 | 0.1/21.1 | 0.0/21.1 | 46.4/14.7 X | 527.3/234.5 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S24000SIM/E02 |
| slip2sig | c | 0.6/5.0 | 0.6/257.9 | 2.7/61.4 | 1.5/61.4 | 58.1/18.6 X | 234.7/230.2 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G25 |
| slip2sig | n | 0.9/37.1 | 0.9/300.7 | 3.7/29.4 | 2.5/29.4 | 88.5/19.0 X | 859.9/222.4 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G25 |
| storm | g | 803.2/11.2 X | 1022.4/222.1 X | 80.8/25.1 X | 68.6/25.1 X | 4276.2/9.3 X | 24107.0/221.5 X | 0.0/0.0 |  |  | zwd S08000SIM; iono S24000SIM/G23 |
| storm | c | 296.9/9.0 X | 1546.8/192.8 X | 923.1/48.2 X | 783.6/48.2 X | 6052.7/12.4 X | 21540.6/262.5 X | 0.0/0.0 |  |  | zwd S29000SIM; iono S40000SIM/G25 |
| storm | n | 490.5/36.4 X | 949.2/200.2 X | 294.3/38.4 X | 187.0/38.4 X | 15533.6/13.3 X | 22322.0/261.1 X | 0.0/0.0 |  |  | zwd S34000SIM; iono S34000SIM/G21 |
| lol | g | 0.7/5.0 | 0.7/192.7 | 0.1/21.1 | 0.0/21.1 | 6.9/14.7 | 127.6/234.5 | 0.0/0.0 |  |  | zwd S24000SIM |
| lol | c | 0.4/5.0 | 0.4/257.9 | 5.2/61.4 | 2.0/61.4 | 5.8/18.6 | 129.2/230.2 | 0.0/0.0 |  |  | zwd S24000SIM |
| lol | n | 0.0/37.1 | 0.0/300.7 | 0.0/29.4 | 0.0/29.4 | 0.0/19.0 | 0.0/222.4 | 0.0/0.0 |  |  | zwd S33000SIM; NO EFFECT on any channel: the target may be out of the solve |
| sat_out | g | 0.8/5.0 | 0.8/192.7 | 0.2/21.1 | 0.1/21.1 | 9.5/14.7 | 152.5/234.5 | 0.0/0.0 |  |  | zwd S48000SIM |
| sat_out | c | 0.5/5.0 | 0.4/257.9 | 6.0/61.4 | 1.9/61.4 | 4.4/18.6 | 178.9/230.2 | 0.0/0.0 |  |  | zwd S48000SIM |
| sat_out | n | 0.5/37.1 | 0.5/300.7 | 4.3/29.4 | 1.1/29.4 | 4.2/19.0 | 88.5/222.4 | 0.0/0.0 |  |  | zwd S48000SIM |
| sta_out | g | 1.2/5.0 | 1.2/192.7 | 0.2/21.1 | 0.1/21.1 | 137.6/14.7 X | 490.6/234.5 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| sta_out | c | 2.2/5.0 | 2.0/257.9 | 11.7/61.4 | 1.9/61.4 | 195.7/18.6 X | 578.7/230.2 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| sta_out | n | 1.5/37.1 | 1.5/300.7 | 3.9/29.4 | 1.1/29.4 | 120.6/19.0 X | 465.1/222.4 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| sat_clk | g | 28.4/5.0 X | 94.6/192.7 | 147.1/21.1 X | 105.1/21.1 X | 57.4/14.7 X | 658.5/234.5 X | 0.0/0.0 |  |  | zwd S24000SIM; iono S37000SIM/G25 |
| sat_clk | c | 35.1/5.0 X | 122.5/257.9 | 2354.4/61.4 X | 692.4/61.4 X | 121.1/18.6 X | 642.4/230.2 X | 0.0/0.0 |  |  | zwd S19000SIM; iono S37000SIM/G25 |
| sat_clk | n | 83.0/37.1 X | 16.3/300.7 | 1186.5/29.4 X | 1133.0/29.4 X | 138.8/19.0 X | 814.9/222.4 X | 0.0/0.0 |  |  | zwd S14000SIM; iono S51000SIM/G25 |
| rx_clk1m | g | 0.0/5.0 | 0.0/192.7 | 0.0/21.1 | 0.0/21.1 | 0.0/14.7 | 0.1/234.5 | 0.0/0.0 |  |  | zwd S48000SIM |
| rx_clk1m | c | 0.0/5.0 | 0.0/257.9 | 0.1/61.4 | 0.0/61.4 | 0.1/18.6 | 0.1/230.2 | 0.0/0.0 |  |  | zwd S48000SIM |
| rx_clk1m | n | 0.0/37.1 | 0.0/300.7 | 0.1/29.4 | 0.0/29.4 | 0.0/19.0 | 0.1/222.4 | 0.0/0.0 |  |  | zwd S48000SIM |
| rx_clk1ms | g | 2.0/5.0 | 2.0/192.7 | 0.1/21.1 | 0.0/21.1 | 74.9/14.7 X | 559.1/234.5 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| rx_clk1ms | c | 2.1/5.0 | 2.4/257.9 | 10.5/61.4 | 9.4/61.4 | 24.5/18.6 X | 607.6/230.2 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| rx_clk1ms | n | 4.0/37.1 | 2.5/300.7 | 20.7/29.4 | 17.7/29.4 | 209.1/19.0 X | 639.8/222.4 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| anc_cont | g | 2.7/5.0 | 2.7/192.7 | 0.2/21.1 | 0.0/21.1 | 13.7/14.7 | 308.4/234.5 X | 0.0/0.0 |  |  | zwd S24000SIM; iono S24000SIM/G25 |
| anc_cont | c | 1.5/5.0 | 1.9/257.9 | 9.2/61.4 | 8.1/61.4 | 9.4/18.6 | 296.5/230.2 X | 0.0/0.0 |  |  | zwd S50000SIM; iono S24000SIM/G25 |
| anc_cont | n | 1.6/37.1 | 1.5/300.7 | 14.9/29.4 | 13.2/29.4 | 79.2/19.0 X | 61.0/222.4 | 0.0/0.0 |  |  | zwd S19000SIM |
| anc_cold | g | 2.8/5.0 | 2.8/192.7 | 6.5/21.1 | 0.5/21.1 | 20.1/14.7 X | 520.8/234.5 X | 0.0/0.0 |  |  | zwd S24000SIM; iono S22000SIM/G25 |
| anc_cold | c | 3.0/5.0 | 2.4/257.9 | 47.5/61.4 | 8.0/61.4 | 116.0/18.6 X | 533.9/230.2 X | 0.0/0.0 |  |  | zwd S24000SIM; iono S22000SIM/G25 |
| anc_cold | n | 4.0/37.1 | 2.2/300.7 | 58.1/29.4 X | 54.5/29.4 X | 100.4/19.0 X | 575.7/222.4 X | 0.0/0.0 |  |  | zwd S14000SIM; iono S22000SIM/G25 |
| arc_erase | g | 0.3/5.0 | 0.3/192.7 | 0.1/21.1 | 0.0/21.1 | 7.8/14.7 | 169.8/234.5 | 0.0/0.0 |  |  | zwd S48000SIM |
| arc_erase | c | 0.5/5.0 | 0.5/257.9 | 3.7/61.4 | 1.6/61.4 | 3.1/18.6 | 197.0/230.2 | 0.0/0.0 |  |  | zwd S27000SIM |
| arc_erase | n | 0.1/37.1 | 0.1/300.7 | 1.0/29.4 | 0.8/29.4 | 3.2/19.0 | 88.2/222.4 | 0.0/0.0 |  |  | zwd S48000SIM |
| health | g | 0.0/5.0 | 0.0/192.7 | 0.0/21.1 | 0.0/21.1 | 0.0/14.7 | 0.0/234.5 | 0.0/0.0 |  |  | zwd S33000SIM; unread; NO EFFECT on any channel: the target may be out of the solve |
| health | c | 0.0/5.0 | 0.0/257.9 | 0.0/61.4 | 0.0/61.4 | 0.0/18.6 | 0.0/230.2 | 0.0/0.0 |  |  | zwd S33000SIM; unread; NO EFFECT on any channel: the target may be out of the solve |
| health | n | 0.0/37.1 | 0.0/300.7 | 0.0/29.4 | 0.0/29.4 | 0.0/19.0 | 0.0/222.4 | 0.0/0.0 |  |  | zwd S33000SIM; unread; NO EFFECT on any channel: the target may be out of the solve |
| health | gx | 1.0/5.0 | 0.4/228.4 | 98.9/55.6 X | 78.7/55.6 X | 26.4/14.5 X | 621.1/221.5 X | 0.0/0.0 |  |  | pb regenerated 2; zwd S40000SIM; iono S51000SIM/G25 |
| step1cm | g | 0.1/5.5 | 0.1/193.5 | 0.1/21.2 | 0.0/21.1 | 15.7/14.7 X | 25.7/234.5 | 0.0/0.0 |  |  | zwd S48000SIM |
| step1cm | c | 0.3/5.5 | 0.1/258.7 | 4.8/61.5 | 1.8/61.4 | 11.5/18.6 | 26.2/230.2 | 0.0/0.0 |  |  | zwd S48000SIM |
| step1cm | n | 0.3/37.6 | 0.1/301.5 | 1.2/29.5 | 0.9/29.4 | 7.5/19.0 | 12.9/222.4 | 0.0/0.0 |  |  | zwd S48000SIM |
| step10cm | g | 1.1/10.0 | 1.1/200.9 | 0.1/22.1 | 0.0/21.6 | 156.8/14.7 X | 256.8/234.5 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| step10cm | c | 3.3/10.0 | 0.6/266.2 | 47.6/62.4 | 17.8/61.9 | 115.1/18.6 X | 262.3/230.2 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| step10cm | n | 3.3/42.1 | 0.8/308.9 | 11.3/30.4 | 9.0/29.9 | 74.9/19.0 X | 128.7/222.4 | 0.0/0.0 |  |  | zwd S48000SIM |
| step1m | g | 7.0/54.8 | 7.0/275.1 | 0.2/31.3 | 0.2/26.2 | 1496.8/14.7 X | 2361.4/234.5 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| step1m | c | 24.5/54.8 | 4.1/340.3 | 333.8/71.6 X | 161.2/66.5 X | 1735.1/18.6 X | 2239.6/230.2 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G29 |
| step1m | n | 5.6/86.9 | 4.3/383.1 | 7.3/39.6 | 5.0/34.5 | 107.9/19.0 X | 597.4/222.4 X | 0.0/0.0 |  |  | zwd S48000SIM; iono S48000SIM/G24 |
| clkdrift | g |  |  |  |  |  |  |  | 3.9/29.1 | 168.6/30.2 X |  |
| clkdrift | c |  |  |  |  |  |  |  | -0.1/26.6 | -1.4/32.5 |  |
| clkdrift | n |  |  |  |  |  |  |  | 0.2/24.2 | 20.8/38.9 |  |

### 9.3 Readings

**22 of 55 cells pass.** The failures fall into four classes; every reading below is event minus null on the same exe and snapshot, and the pre-event difference is exactly zero in every cell.

1. **The wire and the consumer absorb most single faults; the faulted station's own states do not, for a while.** `slip1`, `lol` (on `g` and `c`), `sat_out`, `arc_erase` and `rx_clk1m` pass on every posture. In `slip100`, `slip2sig`, `sta_out`, `rx_clk1ms`, `step10cm` and the anchor handovers the wire's clock and phase bias stay within 1 to 7 mm of the null and the consumer within its bar (except `anc_cold` on `n`, item 4), and the cell fails on `zwd` or `iono` only: the event station's ZWD absorbs the fault and heals (S48000SIM on `g`: `slip100` +190 mm at the event epoch, +40 at +10, +20 at +50; `sta_out` +138 mm at the return, back to 0; `rx_clk1ms` +75 mm at +4, healed by +10), and single lines of sight re-seed their slant ionosphere by 0.2 to 5 m, at the event station or, through a different gate decision, at another one (`slip100` on `g`: `S24000SIM/E02`). The `iono` channel is event minus null, not against truth: absolute slant ionosphere carries per-satellite datum constants of up to 2 m against the simulator's truth (the satellite geometry-free bias is pinned at the broadcast group delay), so a re-seed reads as a change whether or not it is closer to the truth.
2. **The satellite clock step fails on every posture, three different ways.** On `g` the wire carries -0.500 m at the step epoch and then overshoots (-0.601, -0.664, -0.685 m at +1 to +3, still -0.585 at +49) while the consumer's error grows to 0.14 m by +49 (peak 147 mm against a bar of 21). On `c` the wire carries the step correctly at +0 (-0.499 m) yet the consumer jumps 1.5 m at +0 and 2.35 m at +5 (0.56 m at +49). On `n` the wire takes 0.04 m of the 0.5 m at the step and 0.17 m by +49 (the clock anchor holds the native a priori), and the consumer is 1.0 to 1.2 m off throughout. The 2026-09-10 validation (100.4 % absorbed in one epoch, consumer -0.029 m) was on the retired triple-frequency golden; on the current postures the row fails. This is the most direct finding of the matrix: a satellite clock jump, which a real constellation produces, is not survived on any posture.
3. **The slip storm fails catastrophically on every posture.** 646 GPS slips at the campaign rate over 240 epochs: the detectors fire (on `g` 540 IF-TD, 313 MW and 301 LLI detections) and the response is heavy (769 RAIM attribution erases, 692 gross-LOS emit discontinuities, 693 retro bumps, against none in the null); the wire clock moves up to 0.80 m (de-commoned RMS, `g`), 0.30 m (`c`) and 0.49 m (`n`), the consumer up to 81 mm (`g`), 923 mm (`c`) and 294 mm (`n`), and station ZWDs by metres.
4. **Posture-dependent rows.** `clkdrift` fails on `g` only (the consumer's hour-2 p50 is 169 mm worse on the clock fixture; `c` and `n` within 21 mm), as card SCB found and as predicted. `step1m` fails the consumer on `c` only (334 mm peak, 161 mm plateau, against 0.2 mm on `g` and 7 mm on `n`); the station's ZWD takes about 1.5 times an East step on `g` and `c` (1.50 and 1.74 m for 1 m, 157 and 115 mm for 10 cm) and a tenth of it on `n` (108 mm); the wire stays well inside card PLANT's cold response (7.0 mm of clock for 1 m on `g`, against PLANT's 33.2 mm per metre in hour 1). `anc_cold` costs the consumer 58 mm on `n` only. `step1cm` fails on `g` by 1 mm of ZWD (15.7 against 14.7).

**Health flip.** Unread on `g`, `c` and `n` (no reader is armed): the flip fires and no channel moves, as predicted. Read by `network.sat_health` "exclude" (`gx`), `G25` leaves every station for 300 s and returns: the wire is unchanged (1.0 mm), two hold generations restart (declared), and the consumer is 99 mm off at the peak and 79 mm on the plateau (bar 56), so the exclusion and the satellite's return cost the user about 8 cm for the rest of the leg.

**One vacuous cell.** `lol` on `n` moved nothing: in both legs the `L2W` anchor of `G25` handed over from `S14000SIM` to `S45000SIM` at +2 and the arc at `S14000SIM` left the solve after a band-scope flag at the resume, so the outage at the event tow acted on observations the producer no longer used. The report marks such a cell NO EFFECT. The anchor rule resolves the station at the snapshot epoch; version 2 resolves it at the event epoch from the null leg.

**Predictions against results.** `clkdrift` (fail on `g`, pass on `c` and `n`): held. `health` (unread, zero on `g`, `c`, `n`): held. `sat_clk` passes on `g`: refuted (item 2). `step1cm` and `step10cm` pass: refuted on `zwd` (the wire passes the PLANT-referenced bar in every step cell).

### 9.4 Standing

The matrix runs on any Release exe in about 45 minutes and is deterministic (G4). Version 1 shows, before the field: the satellite clock step and the slip storm are not survived on any posture; the clock drift is survived only on the self-contained regime and the native posture; a moved station at 1 m breaks the consumer on the self-contained regime; and a health exclusion costs the consumer 8 cm. Nothing is armed or changed by this card; which findings become cards, and whether the `zwd` and `iono` channels keep their bars (they read station-local transients that do not reach the wire), are the author's decisions. Version 2 items: the anchor resolved at the event epoch, a truth reference for the slant ionosphere with the datum share removed, field rows, and Galileo targets on `g`.
