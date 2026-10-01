# scripts/clock_level/

Graders, census drivers and offline wire tools for the clock-level cards in `docs/refound/`
(CLV, CLV2, HLT, CTU, CTU2, LAH, PVN, SMH, SMD, QPV, PRM). The cards cite these scripts by bare
file name as "session scripts" or "scratchpad graders". On 2026-09-30 they were moved here from
session scratchpads.

Only paths and usage lines changed. The repository root comes from the file's own location.
Helpers (`clklevel.py`, `g3grade.py`) load from this directory, and `iflevel_code2.py` loads
`scripts/wire_vs_code_census.py`. The two replay launchers take their executable from
`$CLOCK_LEVEL_EXE`. Four scripts that had no usage line (`p50.py`, `pkeydiff.py`, `hash3.py`,
`ign.py`) gained one. On existing runs, every script's output is byte-identical to its scratchpad
copy, and that includes the files the writers produce (output location substituted).

## Consumer censuses

| script | cards | purpose |
| --- | --- | --- |
| `census2h.py` | CLV, CLV2, HLT, CTU, LAH, SMH | The 16-site field consumer census on a 2 h wire. `make` writes and runs consumer-only replays (tags `msc_<site>_<key>_d`). `grade` reads hours 1 and 2 against a reference key, split by the site's latched band-1 code. |
| `census6h.py` | LAH, SMH, SMD, QPV | The same census on a 6 h wire (tags `msc6_<site>_<key>`, PBF on), graded per hour with the paired change. `CENSUS_SITES=a,b` limits the sites and drops WTZZ. |
| `simcensus6h.py` | SMH, SMD, QPV | The 6 h census on the sim bed: 16 in-network stations (S01, S05, ..., S57, S60), tags `smc6_<station>_<key>`. Absolute values are optimistic. The paired change between two wires of one solve is the quantity read. |
| `p50.py` | LAH | Consumer 3D p50 by hour from `closed_loop_trace.csv`. |

## Clock level and the anchor

| script | cards | purpose |
| --- | --- | --- |
| `clklevel.py` | CLV, CLV2, HLT | Sim clock-level grade from the wire's C records. `--datumfree` removes the product OSB datum share (`datum_share()`). Imported by `ctugrade.py`, `levelnees.py`, `levelpath.py` and `ign.py`. |
| `ctugrade.py` | CTU2, QPV | One-screen grade of an arm. On the sim: the datum-free level rms by epoch and the pivot-frame normalized level error. On both beds: the IF(C1C, C2W) code spread, anchor trips, floor share, RAIM re-floats, S-basis completions, the emitted count and the consumer p50 by hour. |
| `ctu_g1.py` | CTU | Gate G1 from the run-end lines of `stdout.txt`: anchor folds at the variance floor, trips, RAIM-attrib re-floats, emitted and withheld epochs, the CTU telemetry. |
| `levelnees.py` | CTU | Sim: each satellite's level error in the pivot frame against the monitor's P_L and the code-only bound 1/I_L (`network_level_bound.csv`). |
| `levelpath.py` | CTU | Sim: per-satellite datum-free level error path at chosen epochs, several runs side by side. |
| `ign.py` | CTU2 | Sim ignition: the datum-free level rms over epochs 1..N (`$IGN_N`, default 12), with `-v` per satellite. |
| `ifwin.py` | CTU2 | Truth-free level read: the windowed per-satellite median IF(C1C, C2W) post-fit code residual, rms across satellites per window. |
| `iflevel_code2.py` | CTU2 | Field: the wire's IF code side against CODE per satellite, for a C1C and a C1W user, by hour. |
| `guardladder.py` | CTU2 | The anchor guard's phase ladder from `network_anchor_guard.csv`: the e_phi distribution and would-trips per hour at several sqrt(S) floors. |
| `pvnees.py` | PVN, QPV | From `network_phase_visible.csv`: the truth-free NIS by hour and the within-arc NEES. Arcs break at gaps and at phase-bias frame events. |
| `qpv_ladder.py` | QPV | The anchor-q ladder rule: the mean r over clone ages 1 to 3 h per rung and bed, the field excess x(q) = r_field - r_sim, and the q where x = 1. The rung tags are fixed in `RUNGS`. |

## The phase-bias hold and its wires

| script | cards | purpose |
| --- | --- | --- |
| `hold_rewrite.py` | LAH | Offline hold rules (`hold`, `off`, `dc`, `dcraw`, `if`, `ifraw`) from a hold-on and a hold-off run of one arm. Writes `runs/_rpl_<tag>/` and launches the replay unless `--no-launch` is given. |
| `smh_rewrite.py` | SMH | Offline rules on the smoothed hold's wire (`same`, `emit`, `emit0`, `emitz`, `smdc`). Writes `runs/_rpl_<tag>/` and launches the replay unless `--no-launch` is given. |
| `lahphi.py` | SMH | Preview wire: LAH's P minus the SMH clone's Phi wherever the clone is live, plus a replay arm. |
| `smd_ctlwire.py` | SMD, QPV | Rebuilds a wire from the phase-clock hold's wire: P = P_LAH - alpha (frozen + Phi) on every held key. Column `phi_m` with alpha 1 reproduces the block member's wire, and the script checks it against that run's P records. An alpha below 1 gives the QPV shrinkage wires. |
| `smd_split.py` | SMD | The block clones' passed motion by clone age and hour of day: the level part \|Phi_c - Phi_D\|, the phase-visible part \|Phi_D\| and mean r_D. |
| `splicekey.py` | SMH | Replaces the P records of chosen satellites in one wire with another wire's, and writes a replay arm. |
| `mkarm.py` | LAH | Derives an arm from an existing `_rap_` arm: re-points the run paths and sets knobs. |

## Identity and comparison

| script | cards | purpose |
| --- | --- | --- |
| `ident2h.py` | CLV, CLV2, HLT, CTU, PRM | Windowed identity of two runs on their first N epochs (products records, `closed_loop_trace.csv` without `n_sats`, `network_epoch_status.csv`). |
| `hash3.py` | CLV, PRM | Windowed hashes of one run's products, consumer trace and epoch status. |
| `smh_g0.py` | SMH, SMD | Record-by-record comparison of two runs' `products.txt` and a byte comparison of `closed_loop_trace.csv`. This is the G0 gate for an emit-only mode. |
| `pcmp.py` | LAH | P-record comparison of two product files, plus whether every non-P line is identical. |
| `pkeydiff.py` | SMH | The P keys that differ between two product files, largest first. |

## The injected clock walk (card SMH G3)

| script | cards | purpose |
| --- | --- | --- |
| `g3_walk.py` | SMH | `make` adds a seeded per-satellite random walk to the a priori CLK product and writes the walk. `check` verifies a product against its walk. |
| `g3grade.py` | SMH | The hidden fraction of the injected walk, per generation and per clone, with the clone's predicted rho_D. Imported by `g3pred.py` and `g3track.py`. |
| `g3pred.py` | SMH | rho_D from card PVN's stated variance at the clone's birth, beside the measured hidden fraction. |
| `g3track.py` | SMH | The shares of the walk taken by the wire clock and by the held phase bias. |

## Inputs outside the repository

All of these are under the git-ignored `runs/`:

- `runs/_msc/site_list.json`: card MSC's 16-site selection, read by both field censuses.
- `runs/_rf0hv10_field6/arm.json` and `runs/_rf0hv11_sim6/arm.json`: the censuses' base arms.
- `runs/simnet60_quiet_wu93_24h/S01000SIM/truth_bias.csv`: the sim datum share in `clklevel.py` and
  `g3grade.py`. Without it, the sim level rows come out empty.
- `runs/_g3_data/`: the walked product and `walk_q5e-6.csv` of card SMH G3.

The census `make` steps call `scripts/rapid_arm.py` with the executable they are given. The two
rewrite tools launch `scripts/ev_launch.ps1` with `$CLOCK_LEVEL_EXE`. Each of these is a renamed
copy of the Release build, never the Debug tree. Typical use:

```bash
python scripts/clock_level/census2h.py make <key> <wire run dir under runs/> <exe>
python scripts/clock_level/census2h.py grade <reference key> <key> [<key> ...]
```

## Not moved

Scripts that the older cards CLV, CLV2 and HLT cite (`census_pooled.py`, `userview.py`,
`cstdiff.py`, `armsteps.py`, `ra2_premise.py`, `g3_recovery.py`, `clv_*.py`) and card CTU2's
`wf/` analyses remain in the scratchpads of the sessions that wrote them. `wires4.py` (card LAH)
is not in any scratchpad.
