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

| state | write key | read key |
|---|---|---|
| shared carries, satellite records | always | always |
| clock and phase-anchor frame | `state_snapshot_frame_capture` | `resume_frame_restore` |
| coupling and dense border | `state_snapshot_coupling_capture` | `resume_coupling_restore` |
| eq34 tree, code-bias datum | `state_snapshot_datum_capture` | `resume_datum_restore` |
| emit re-base | `state_snapshot_emit_capture` | `resume_emit_restore` |
| station layer, canonical signals | `state_snapshot_station_capture`, `state_snapshot_signal_capture` | `resume_station_restore`, `resume_signal_canon_restore` |
| chain and ring fix memory | `state_snapshot_chain_capture` | `resume_chain_restore` |
| wind-up accumulators, GF arc-jump arcs, slip epoch counter | `state_snapshot_station_memory_capture` | `resume_station_memory_restore` |
| parked carries of departed satellites | `state_snapshot_park_capture` | `resume_park_restore` |
| built-in user (consumer filter, seed latches) | `closed_loop.state_snapshot`, `realtime.state_snapshot_latch_capture` | the leg's `realtime.state_snapshot_path`, `realtime.state_snapshot_latch_restore` |

What does not cross: the wide-lane detector's arc statistics (the leg arms `network.resume_slip_warmup_ep` against their cold burst), the IF triple-difference chains, the raw slip history, the per-station covariance block and column order, and some network-level memories on the solve path (receiver phase-bias carry, phase colour, gross line-of-sight state, admission records, arc ledger). The built-in fixing user of the reference files cannot be saved: the consumer snapshot refuses its state layout, so the leg's built-in user starts cold. A test of a user-side response from a warm start uses a replay user on the leg's products.

## What a leg can be compared with

A leg is not the uninterrupted run (card WRS measures how far it is and for how long). Compare a leg with the keys under test against a leg without them from the same seed (two legs from one seed are repeatable: identical products and traces, card NRF section 125.5, B4), and read the hours that matter to the test after the seam's transient.
