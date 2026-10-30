# MGT: the telemetry of the isolated-arc refloat maturity gate

**STATUS**

- **id:** MGT
- **topic:** raim-qc
- **status:** open
- **summary:** Pre-registered 2026-10-30. Card RCG (branch `claude/laughing-solomon-9a3b51`, commits `68b6a88` and `024a0ac`, unmerged) graded `network.raim_refloat_maturity_gate` at 120 epochs on 6 h arms and found it a silent valve: no arming line, no count of the refloats it holds, no event at the lift, and `network_raim_attrib.csv` marking a held arc `acted=1`. Standing invariants 4 and 7 of `CLAUDE.md` require counters, identities and an event at both edges before any arming. This card builds that telemetry, default off and byte-identical when off. It arms nothing and leaves the key's ledger verdict (UNVALIDATED) as it is.
- **source:** card RCG sections 1.2, 4.3 and 4.5 (the gate, its measured behaviour and the telemetry it owes); `src/core/network/qc/Raim.cpp`, the T4 attribution block of `networkPostFitRaim`
- **knobs / posture:** read, not changed: `network.raim_refloat_maturity_gate` (UNVALIDATED) with `network.raim_refloat_maturity_epochs` (PARAM). No new key. Base: `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json` of main `1527cf9`.
- **dates in the text:** 2026-10-30
- **hashes / run dirs:** recorded in section 5 when the runs are made
- **gates (frozen at pre-registration):** section 3, written before any run of the changed executable

## 1. Question

Can the maturity gate report what it does, without changing what it does? The gate holds an isolated-arc T4 refloat
until the run has emitted max(`raim_refloat_maturity_epochs`, `phase_leveling_warmup_epochs`) epochs and the sigma0
EWMA has received `raim_refloat_maturity_epochs` updates (`networkWarmupRampLifted`). It has two edges (standing
invariant 7): each held flag leaves a stiff carry in the solve that T4 would have dropped, and the lift re-couples
every later flag to the refloat. Card RCG measured both edges from the outside (27 held flags and a lift near 63 min at
120 epochs). The producer reported neither.

## 2. What is built

| piece | where | what it reports |
| --- | --- | --- |
| the gate rules as a pure function | `raimSelectIsolatedArcs` (`network/qc/Raim.hpp`) | the arcs T4 refloats, the flags the maturity gate holds (with station, satellite and statistic) and the persistence streaks to carry; the order of the tests is the T4 block's order, so the unit tests pin the seam |
| the run-scope tally | `RaimMaturityGateTally`, `RaimCounters::maturity_gate` | scans (the opportunity count), flags that reached the gate, flags held (arc-epochs), epochs with a held flag, the first and last held epoch, and the lift (processed epoch, GPS time, `n_emitted`, sigma0 EWMA update count) |
| one event row per held flag | tier `RAIM_MATURITY`, event `arc_held`, action `hold` | station, satellite, the flag statistic (interaction over sigma0), the bar, and the point of the run (`n_emitted`, EWMA updates, the need) |
| one event row at the lift | tier `RAIM_MATURITY`, event `lift`, action `lift` | `n_emitted` as the statistic, the epoch need as the threshold, and the EWMA update count, the held arcs and epochs in the detail; one info line as well |
| a run-end section | `networkRaimMaturityGateRunEnd`, in the RAIM subsystem's report | the counts above, and the four outcomes the denominators tell apart: never consulted, never lifted, held then lifted, open from the first scan (held nothing) |
| an arming line | loader, `validateNetworkCarryPosture_` | ARMED with the rule the gate applies at run time, or INERT (no `network.raim_attribution`), plus a warning when the horizon does not exceed the carry warm-up |
| the `acted` column | `network_raim_attrib.csv`, ARC rows | 1 only when T4 refloated the arc or its satellite |

The held and lift rows carry their own tier, not `T4_attrib`: every grader that counts the `T4_attrib` rows as
refloats (`scripts/rapid_arm.py`, `scripts/ev7_event_read.py` and others) keeps its meaning on a gate-on arm.

The persistence gate (`network.raim_arc_persist_epochs`, K) had the same `acted` defect: an arc over the bar that has
not yet persisted K epochs is not refloated and read `acted=1`. The fix is the same test (membership in the refloat
set), so it is made here. The goldens run K = 1, where the two readings coincide.

## 3. Gates (frozen 2026-10-30, before any run of the changed executable)

The base executable is the Release build of main `1527cf9` that card RCG used
(`D:/GnssProcessorSupport/worktrees/mgsi/build/x64-Release/gnss_processor_rcg.exe`, sha256[:16] `fab88a924e9189fd`).
The changed executable is the Release build of this branch. All arms are golden copies with the one change named, made
with `scripts/rapid_arm.py make` under `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`.

| gate | arms | bar |
| --- | --- | --- |
| G0 unit | `gnss_tests` Debug | the new suites pass; the full suite is green apart from the one fixture-gated skip |
| G1 gate off, field | 2 h field golden, base exe and changed exe | `products.txt`, `logs/closed_loop_trace.csv`, `logs/network_events.csv`, `logs/network_raim_attrib.csv` and the solution trace byte-identical |
| G2 gate off, sim | 2 h sim golden from 00:30:15, same pair | the same four files byte-identical |
| G3 gate on, field | 2 h field golden with `raim_refloat_maturity_gate=true`, `raim_refloat_maturity_epochs=120`, base exe and changed exe | products, solution and closed-loop trace byte-identical; the events file identical apart from the added `RAIM_MATURITY` rows; the attribution trace identical apart from the `acted` values of held arcs; the run-end text gains the gate section |
| G4 reading | the G3 changed-exe arm | the held flags are about 27 in the first hour and the lift falls near 63 min (card RCG section 4.3); the `arc_held` rows equal the held count of the run-end line; the lift row and the run-end line agree; every ARC row with `acted=1` before the lift names a refloat in the events file |

A gate that fails is reported with the difference; no tolerance is applied to a byte comparison.
