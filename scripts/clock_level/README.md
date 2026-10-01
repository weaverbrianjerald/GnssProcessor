# scripts/clock_level/

Graders and census drivers for the clock-level cards in `docs/refound/` (CLV, CTU2, LAH, PVN,
SMH, SMD, QPV). The cards cite these scripts by bare file name as "session scripts". They were
moved here from session scratchpads on 2026-09-30. Only paths changed: the repository root now
comes from the file's own location, and `ctugrade.py` loads `clklevel.py` from this directory.
On existing runs, every script's output is byte-identical to the scratchpad copy.

| script | cards | purpose |
| --- | --- | --- |
| `census6h.py` | LAH, SMH, SMD, QPV | The 16-site field consumer census on a 6 h wire. `make` writes and runs consumer-only replays (tags `msc6_<site>_<key>`, PBF on). `grade` gives the per-hour median of the site p50 and the paired change against a reference key, split by the site's latched band-1 code (C1W / C1C). `CENSUS_SITES=a,b` limits the sites and drops WTZZ. |
| `simcensus6h.py` | SMH, SMD, QPV | The same census on the sim bed: 16 in-network stations (S01, S05, ..., S57, S60), tags `smc6_<station>_<key>`. Absolute values are optimistic. The paired change between two wires of one solve is the quantity read. |
| `ctugrade.py` | CTU2, QPV | One-screen grade of an arm. On the sim: the datum-free level rms across satellites by epoch, and the pivot-frame normalized level error. On both beds: the IF(C1C, C2W) post-fit code spread (hour 2 and the last hour), anchor trips, floor share, RAIM re-floats, S-basis completions, the emitted count and the consumer p50 by hour. |
| `clklevel.py` | CLV, CLV2, HLT (helper of `ctugrade.py`) | Sim clock-level grade from the wire's C records. `--datumfree` removes the product OSB datum share (`datum_share()`), which `ctugrade.py` imports. |
| `pvnees.py` | PVN, QPV | From `network_phase_visible.csv`: the truth-free NIS by hour and the within-arc NEES. Arcs break at gaps and at phase-bias frame events. |
| `smh_g0.py` | SMH, SMD | Record-by-record comparison of two runs' `products.txt` (identical record counts by type, P records differing in value only, the S line) and a byte comparison of `closed_loop_trace.csv`. This is the G0 gate for an emit-only mode. |
| `smd_split.py` | SMD | The block clones' passed motion, from the `b_*` columns of `network_pb_hold_clone.csv`: the level part \|Phi_c - Phi_D\|, the phase-visible part \|Phi_D\| and mean r_D, by clone age and by hour of the day. |
| `smd_ctlwire.py` | SMD, QPV | Rebuilds a wire from the phase-clock hold's wire: P = P_LAH - alpha (frozen + Phi) on every held key. Column `phi_m` with alpha 1 reproduces the block member's wire, and the script checks it against that run's P records. An alpha below 1 gives the QPV shrinkage wires. |
| `qpv_ladder.py` | QPV | The anchor-q ladder rule: the mean r over clone ages 1 to 3 h per rung and bed, the field excess x(q) = r_field - r_sim, and the q where x = 1 (log-linear between the bracketing rungs). The rung tags are fixed in `RUNGS`. |

Inputs outside the repository, all under the git-ignored `runs/`:

- `runs/_msc/site_list.json`: card MSC's 16-site selection, read by `census6h.py`.
- `runs/_rf0hv10_field6/arm.json` and `runs/_rf0hv11_sim6/arm.json`: the base arms of the two
  censuses.
- `runs/simnet60_quiet_wu93_24h/S01000SIM/truth_bias.csv`: the datum share in `clklevel.py`.
  Without it, the sim level rows of `ctugrade.py` come out empty.

The census `make` step calls `scripts/rapid_arm.py` with the executable it is given. That is a
renamed copy of the Release build, never the Debug tree. Typical use:

```bash
python scripts/clock_level/census6h.py make <key> <wire run dir under runs/> <exe>
python scripts/clock_level/census6h.py grade <reference key> <key> [<key> ...]
```

Other scripts the same cards cite were not moved in this pass. They remain in the scratchpad of
the session that wrote them: `census2h.py`, `ident2h.py`, `ctu_g1.py`, `levelnees.py`,
`levelpath.py`, `guardladder.py`, `iflevel_code2.py`, `ifwin.py`, `ign.py`, `hold_rewrite.py`,
`mkarm.py`, `p50.py`, `pcmp.py`, `g3_walk.py`, `g3grade.py`, `g3pred.py`, `g3track.py`,
`lahphi.py`, `pkeydiff.py`, `smh_rewrite.py`, `splicekey.py`, `hash3.py`. `wires4.py` (card LAH)
is not in any scratchpad.
