# Warm start: a network run resumed at a chosen time

A test of a response to a disturbance (an outage, a satellite clock step, a code-bias event) otherwise starts from a cold network one to three hours before the disturbance, because the network needs that long to converge and to fix its integers. A warm start resumes the network from a state saved just before the chosen time and runs from there. The tool is `scripts/warm_start.py`; it uses the seed and leg mechanics of `scripts/event_leg.py` ([Event legs](event_legs.md)). The measurements behind this page are card [WRS](../cards/harness/WRS-WARM-RESUME-REFERENCE.md).

## The command

```
set RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom
python scripts/warm_start.py run --base <configuration> --start HH:MM --end HH:MM --name <name> --exe <Release exe> --gate
```

- `--base`: the configuration, a reference file (`config/golden/golden_field_refound.json`) or an arm's `arm.json`.
- `--start`, `--end`: times of day (GPS time, as the observation and product files) of the first resumed epoch and of the last epoch, on the day of the configuration's observations.
- `--name`: names the seed `evseed_<name>` and the leg `evleg_<name>` under the runs root.
- `--gate`: every engine run goes through the machine's run gate (`D:/GnssProcessorSupport/gates/run_queue/slots.py`).
- `--leg-extra '<json>'`: the keys under test, merged into the leg's configuration (the same keys go into a second leg without them for the comparison).

What it does, in order:
1. The seed. When `evseed_<name>` holds no snapshot at the epoch before `--start`, it writes one: the configuration from its own start to `--start` plus `--margin-min` minutes (default 10), with every capture key armed and one snapshot copy at the epoch before `--start`. The seed is written once and reused by every later leg of the same name.
2. The leg. It resumes from that copy with every restore key armed and runs to `--end`.
3. The checks (below). `python scripts/warm_start.py check --seed <seed dir> --leg <leg dir>` runs them alone.

## The checks that say the start is sound

| check | passes when |
|---|---|
| C1 | the leg exited 0 and resumed warm (a "warm resume restored" line, no refusal, no cold start) |
| C2 | every saved section the leg arms was restored and none refused or absent: the chain memory, the station memory, the built-in user's filter where the leg arms it |
| C3 | at the leg's first epoch the held links equal the seed's at the same epoch, with the same integers |
| C4 | at the leg's first epoch no topology event that the seed does not have: a station or satellite seen before the seam reads as present, never as a first sighting |
| C5 | reported: over the seed's margin, the between-satellite differences of clocks, code biases and phase biases (wrapped to the wavelength) against the seed, median and largest |

## What crosses the seam

The capture and restore keys the tool arms (every key under `network.` unless named otherwise):

Since 2026-11-22 (the author's adoption of the warm start) `scripts/event_leg.py` and `scripts/warm_start.py` arm the four memory keys by default, each where its prerequisite holds: `state_snapshot_station_memory_capture` where the seed accumulates its wind-up, `state_snapshot_park_capture` where `readmit_seed_horizon_s` is above zero, and each restore where the seed wrote its section. `--no-station-memory-restore` and `--no-park-restore` of `scripts/event_leg.py` give the null legs. The four keys stay off in the loader and in the reference files.

| state | write key | read key |
|---|---|---|
| shared carries, satellite records | always | always |
| clock and phase-anchor frame | `state_snapshot_frame_capture` | `resume_frame_restore` |
| coupling and dense border | `state_snapshot_coupling_capture` | `resume_coupling_restore` |
| eq34 tree, code-bias datum | `state_snapshot_datum_capture` | `resume_datum_restore` |
| emit re-base | `state_snapshot_emit_capture` | `resume_emit_restore` |
| station layer, canonical signals | `state_snapshot_station_capture`, `state_snapshot_signal_capture` | `resume_station_restore`, `resume_signal_canon_restore` |
| chain and ring fix memory | `state_snapshot_chain_capture` | `resume_chain_restore` |
| wind-up accumulators, slip epoch counter, GF arc-jump arcs, raw slip history, gross line-of-sight screen | `state_snapshot_station_memory_capture` | `resume_station_memory_restore` |
| parked carries of departed satellites | `state_snapshot_park_capture` | `resume_park_restore` |
| built-in user (consumer filter, seed latches) | `closed_loop.state_snapshot`, `realtime.state_snapshot_latch_capture` | the leg's `realtime.state_snapshot_path`, `realtime.state_snapshot_latch_restore` |

What does not cross: the wide-lane detector's arc statistics (the leg arms `network.resume_slip_warmup_ep` against their cold burst), the IF triple-difference chains, the per-station covariance block and column order, and some network-level memories on the solve path (receiver phase-bias carry, phase colour, admission records, arc ledger). Through the covariance block the float solution departs slowly from the uninterrupted run's: on DOY 129 the clocks sit 5 to 7 mm median from it after an hour, on the simulator under 1 mm (card WRS). The built-in fixing user of the reference files cannot be saved: the consumer snapshot refuses its state layout, so the leg's built-in user starts cold. A test of a user-side response from a warm start uses a replay user on the leg's products.

A seed belongs to the engine that wrote it: the station memory section carries its format version and its own fingerprint, and a leg of an engine that reads another version refuses the section (check C2 fails). After an engine change the seed is written again (`--name` with a new name, or the old seed folder removed).

## What a leg can be compared with

**A seed is not a long run of another length in its biases.** The broadcast group delay that pins each satellite's geometry-free code bias is latched once per run over that run's processing window, and Galileo's broadcast value changes within a day: E04 on 26 May 2026 carries -5.5879 ns until 05:20, -4.1910 ns from 05:30 to 17:10 and -5.3551 ns after, so a day-long run latches -4.1910 ns (first C1C code bias -1.2564 m) and a 3.5 h seed -5.5879 ns (-1.6752 m). On that day 88 of 216 code-bias keys of the seed differ from the day-long run's by a constant above 1 cm (up to 0.876 m) and the phase biases by the opposite constant, while the clocks agree to 0.1 mm. Two rules follow:
- A user, an ionosphere file or a comparison stays inside one seed and its legs: a leg's products are never mixed with those of a run of another length (its user state, ionosphere corrections or code biases).
- A leg compared with a long run is read in clocks as they are, and in code and phase biases only with each key's constant removed.


A leg is not the uninterrupted run (card WRS measures how far it is and for how long). Compare a leg with the keys under test against a leg without them from the same seed (two legs from one seed are repeatable: identical products and traces, card NRF section 125.5, B4), and read the hours that matter to the test after the seam's transient.
