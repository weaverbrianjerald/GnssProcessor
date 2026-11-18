# CHA: an audit of the chain and the ring fix (2026-11-18)

**STATUS**

- **id:** CHA
- **topic:** network-vehicle
- **status:** open
- **summary:** An audit of the network's chain (`src/core/network/solve/ChainDatum.cpp` and the forest walk of `src/core/network/ar/ArcGraph.cpp`) and of the ring fix (`src/core/network/ar/RingFix.cpp`, `RingFixSeams.cpp`), with no engine code, at branch `chain/audit` = `irdu/user-integers` `72b7b3b`. Section 1 maps every function of more than about twenty lines in epoch order, with what it reads, writes and owns, and where the per-epoch seam contract is kept. Section 2 classifies every string or pair key as naming a station's arc, a satellite band's state or both, and names the typed identities that should stand there, five of them missing. Part 2 holds the rules as one table, the defects in priority order, a refactor plan of eleven steps, and what the plan enables (members of a signal family keeping their own arc names, further constellations, a per-make code term). The 2 h control of the adopted configuration reproduces byte for byte at `2fdfc29` and at `72b7b3b` (part 2, section 7).
- **source:** the author's ruling of 2026-11-18 (the audit of the chain that card NRF section 112.3 (d) set aside is needed anyway); the managing session's brief of 2026-11-18; [NRF part 12](NRF_network_ring_fixing_2026-11-14_part12.md) to [part 16](NRF_network_ring_fixing_2026-11-14_part16.md) (sections 96 to 119.1); [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 11; the hand-over note of the builder session (`D:\GnssProcessorSupport\gates\irdu_handover\IRDU_handover_2026-11-18.md`, outside the repository).

**Outcome.** The chain and the ring fix are about 12 800 lines in twelve files; the election is cut into short functions, but seven functions run 200 to 640 lines. The ambiguity key "G01_L1C" names a station's arc and a satellite band's state in the same code, which is why a signal family's member cannot keep its own arc name. The three known defects trace to rules with several owners: a held entry's step decided in three places, a re-host's pin set through one link, a kept re-host that skips the entry check. The ring fix's input record sits behind a trace key.

## Scope and method

The audit reads the tip `72b7b3b` of `irdu/user-integers` (fast-forwarded on `chain/audit`; the chain code changed in `8241c5a` by the held-first walk by life, the guard's release form, the extra-band delay noise and the codes-shared phase families, all default off). Five read-only maps were made by worker sessions, one per file group, each with every `file:line` read from the files; the claims carried into this card were then checked against the code directly, and one map's claim that did not hold is corrected in part 2 (section 4, P1). "(inferred)" marks a conclusion that no single line shows. No engine code, key or ledger row changed.

Abbreviations: **CD** `src/core/network/solve/ChainDatum.cpp`; **CDh** `include/network/solve/ChainDatum.hpp`; **AG** `src/core/network/ar/ArcGraph.cpp`; **AGh** `include/network/ar/ArcGraph.hpp`; **RF** `src/core/network/ar/RingFix.cpp`; **RFh** `include/network/ar/RingFix.hpp`; **RFS** `src/core/network/ar/RingFixSeams.cpp`; **CF** `src/core/network/solve/ChainFrame.cpp`; **CFh** `include/network/solve/ChainFrame.hpp`; **CCT** `src/core/network/solve/ChainChordTable.cpp`; **LDN** `src/core/config/GnssConfigLoadNetwork.cpp`.

## 1. The map

### 1.1 Files

| file | lines | holds |
| --- | --- | --- |
| CD | 3 389 | the election (one anonymous namespace to line 2852), the pin audit, the receiver phase-bias harvest, the run-end report |
| CDh | 555 | the entry points and `ChainDatumCounters` (356 to 553) |
| AG / AGh | 1 631 / 801 | arc graphs, components, the chain's forest walk; the baseline forests, cycle bases and lattice of producer AR |
| RF / RFh | 1 007 / 1 362 | the ring fix's pure steps and its state `NetworkRingFix` (RFh 731 to 844) |
| RFS | 1 848 | the ring fix's seams (arm, hold, priors, decide, monitor, run end) |
| CF / CFh | 1 194 / 733 | the frame observatory (classification, traces, the t-1 chord table, the event table `kChainEventSpec`) |
| CCT | 132 (+128) | the chord table: a station's chords and their floats |
| `include/network/assembly/PhaseFamily.hpp` | 62 | the phase-family table type |

### 1.2 The epoch, in order

| stage | call site | what runs |
| --- | --- | --- |
| setup | `ApplicationNetwork.cpp:279` | `networkRingFixArm` builds the ring fix; `ApplicationNetworkSetup.cpp:240-282` sets the phase-family renames, which `RinexParser.cpp:379-401` applies to every producer station's header |
| phase datum | `ApplicationNetworkEpoch.cpp:344` | `networkPhaseAnchorEpoch` (`PhaseAnchorEpoch.cpp:52`) calls `networkElectChainDatum` at `PhaseAnchorEpoch.cpp:139`, gated at `:132` by `ctx.knobs.phase_datum_eq34` |
| before PASS B | `ApplicationNetworkEpoch.cpp:388` | `networkRingFixHoldEpoch` turns the chain's record into cuts, releases, re-expressions and the hold table |
| PASS B | `StationBuild.cpp:199-201`, `:78`; `ApplicationNetworkAssembly.cpp:806` | `networkRingFixPriors` (station step `ring_hold`), `networkRingFixStationTally`, `networkEq34PinAudit` |
| solve | `ApplicationNetworkEpoch.cpp:473` | `networkSolveGlue` |
| after the solve | `ApplicationNetworkEpoch.cpp:509` | `networkRingFixDecide`: chord readings, decisions, cascades, the trace row, the settled flags, the near-null guard |
| emission | `SolPrep.cpp:1370-1372`; `EmitStore.cpp:470-482` | `ringFixStampSettledFlags`; the step declaration from the chain's released keys; the phase-family member records; the emitter's counters |
| harvest | `LocalCarryHarvest.cpp:92` | `networkHarvestRxPhaseBias` |
| observers | `EpochObservers.cpp:65-66`, `:95-96` | `networkRingFixNearNullMonitor` (slot RosterTail), `networkChainFrameObserve` (slot StashTail) |
| run end | `RunEnd.cpp:205`, `:209`, `:211` | `networkRingFixRunEnd`, `networkChainDatumRunEnd`, `networkChainFrameRunEnd` |

### 1.3 Functions, in call order

Every function of more than about twenty lines, and every shorter one that writes state across epochs. "Reads" and "writes" name parameters, run state (`ctx`), entity fields (`net::get` and kin) and the election's working set `el` (`ChainElection`, CD 184 to 316, stack-local per election). "State" is what survives the epoch.

**Setup**

| function | file:lines (length) | role | reads | writes | state |
| --- | --- | --- | --- | --- | --- |
| `networkRingFixArm` | RFS:312-544 (233) | builds `NetworkRingFix` from `network.ring_fix.*` and logs every armed option | the configuration, station names, `knobs.pl_max_gap_s` (RFS:331) | `ctx.carry.ring_fix` (RFS:543) | creates the ring fix |
| phase-family intake | `ApplicationNetworkSetup.cpp:240-282` (43) | the parse renames of a member's codes (phase, Doppler, signal strength; codes too with `phase_family_codes`) for every producer station; refuses a streamed run with the table | `network.phase_families`, `network.phase_family_codes` | `sync.setObsCodeRenames` | none |
| header rename | `RinexParser.cpp:379-401` (23) | reads a member's codes under the identity's names, unless the file declares the identity's code too ("kept") | the rename map | the header's code list, `applied_obs_code_renames_` | per file |

**The election** (`networkElectChainDatum`, inside the phase-datum stage)

| function | file:lines (length) | role | reads | writes | state |
| --- | --- | --- | --- | --- | --- |
| `networkElectChainDatum` | CD:2854-2907 (54) | the election: clears the stations' pin maps, groups, elects each family, finishes | `ctx` (knobs, entities, `carry.ring_fix`), `cnd` (usable and union keys, elevations, coverage), `hyg`, `carry_slipped_ambs`, `now_s` | `stations[].pinned_*` (cleared CD:2890-2896, rewritten), `shared_phase_keys` (CD:2901-2902, its only writer on this path), `events.phase_datum` (`this_epoch_anchor`, `eq34_reref`, `eq34_epoch_declared`, `eq34_epoch_bridged`, the record `chain` at CD:2870) | every entity field below, through `finishChainElection` |
| `collectPreviousRoots` | CD:371-377 | the (band, root) pairs of the last election | `SatSignalState::eq34_sat_root` | `el.prev_roots` | none |
| `groupChainKeys` | CD:382-400 | groups the present stations' usable keys by (constellation, phase signal); parses the satellite phase-bias key | `cnd.usable_keys`, `cnd.union_keys` | `ChainGroups` | none |
| `formChainFamilies` | CD:427-457 | one band per group, or a pair of two signals of different digits | the groups, `eq34_pair_family` | the families; counters | none |
| `electFamily` | CD:2457-2483 (27) | one family end to end | all below | `ChannelState::datum_rx` (CD:2478-2480, inside the band loop), `el.elected_bands` | `datum_rx` |
| `buildChainGraph` | CD:484-536 (53) | the family's arc graph: one row per (band, station, satellite) with present, slipped, durability and eligible-host bits | groups, `chainArcSlipped` (CD:331), `chainArcLife` (CD:353), `hyg.eligibleHost` | `ChainGraph` | none |
| `seedIncumbents` | CD:586-629 (44) | last election's live tree edges as the walk's seeds; a slipped incumbent loses its seat; a re-attachment move replaces a satellite's incumbent (`untiedMoveArc`, CD:572-584) | `anchor_incumbent`, `StationSignalState::eq34_rx_parent`, `ring_fix->untied_moves` | `ChainSeeds`, a counter | reads the last tree |
| `arcgraph::legacyChainVertexRank` | AG:423-435 | the station order of components and of the second pass: the decimal string of the roster index ("10" before "2") | the graph | ranks | none |
| `chainCascadeLead` | CD:868-909 (42) | lead ranks of a cascade's third band (`ring_fix.cascade_tree`) | `ring_fix` cascades and links, the latch, the seeds | the rank vector | none |
| `untiedLeads`, `tallyUntiedMoves` | CD:2405-2433, 2438-2455 | turn the ring fix's re-attachment moves into rank-1 lead arcs and record their outcome | `ring_fix->untied_moves` | the same rank vector; `ring_fix->untied_moves` erased (CD:2423, 2453), `untied_outcomes`, ring-fix counters | ring-fix state, written from the election |
| `planChainFamily`, `planChainComponent` | CD:929-950, 723-751 | components, their datum receivers and their forests | `chainForestRules` (CD:776-784), `chainHeldArcs` (CD:790-813), `arcgraph::arcComponents` (AG:366-401) | `ChainFamilyPlan` | none |
| `electDatumReceiver` | CD:639-684 (46) | a component's datum receiver: the configured one, else the vote of the satellites' remembered roots, else `datum_rx`, else coverage (ties by name) | `network.phase_datum_eq34_receiver`, `eq34_sat_root`, `datum_rx`, coverage | none | reads three memories of the last root |
| `arcgraph::growChainForest` | AG:483-691 (209) | the walk (its order in section 1.5) | roots, incumbent arcs, rules, ranks | `SpanningForest` | none |
| `buildRootKeepers` | CD:1704-1731 | per old root, the component that keeps it | `eq34_sat_root` | `el.root_keepers` | none |
| `electBandComponent` | CD:2159-2232 (74) | one component on one band: the datum receiver's pin, the gauge count, the edges, the frame walk | the plan | `pinned_rpb_keys`, `events.phase_datum.eq34_epoch_declared` and `_bridged`, `el.gauge_dim` | none |
| `applyRootRow` | CD:1753-1814 (62) | a root handover: the new root's receiver phase bias pinned at its carried value | `el.prev_roots`, `eq34_root_rpb_target`, the receiver phase-bias carry, `pl_max_gap_s` | `pinned_rpb_targets`, `el.new_root_targets`, `el.root_handed`, the record's roots | none |
| `recordTreeEdge` | CD:1948-1989 (42) | one tree edge: target, step, the event rows in table order, the pin, the record | the rows below | `this_epoch_anchor`, `el.new_sat_root`, `el.new_rx_parent` (through `draftPinStep`) | none |
| `draftPinTarget`, `draftSeedRoundTarget` | CD:1115-1132, 1057-1100 | the pin target: λ·round(the new edge's own carry) (CD:1125), else the seed round (frozen previous target, rounded state, or a deferral), else 0 | `eq34_pin_round`, `ArcState::amb`, `eq34_prev_target` | the draft; `pinned_amb_seed_lambda` (CD:1095) | none |
| `draftPinStep` | CD:1137-1211 (75) | the frame step and the host change; records the tree's direction | `anchor_incumbent`, `eq34_sat_root`, `el.root_handed`, `eq34_rx_parent`, `eq34_prev_target` | `this_epoch_anchor` (CD:1144), `el.new_sat_root` (1158), `el.new_rx_parent` (1166) | none |
| `applyMergeRow` | CD:1275-1302 | releases an edge joining two trees of the last election | `eq34_tree_root` | the draft, counters | none |
| `applyKeptRow` | CD:1327-1379 (53) | holds an incumbent edge at its previous target, step 0 | `eq34_prev_target`, `eq34_pin_life`, the latch | `el.new_pin_life` | none |
| `applyReturningRow` | CD:1447-1471 | releases the edge of a child back after an absence: first-sighting target, step unknown (CD:1469) | `anchor_incumbent`, `eq34_rx_parent`, `datum_rx`, `eq34_tree_seen`, the latch | the draft, counters | none |
| `applyRehostRow` | CD:1572-1615 (44) | releases a re-host (the drafted integer target, step unknown) unless kept on a held integer at step 0 (`chainHeldInteger` CD:1476-1491, `keepOnHeldInteger` CD:1531-1543) | `incumbentEdgeOf` (CD:1232-1242), the latch, `ring_fix->links` | the draft; ring-fix counters (CD:1586-1597) | none |
| `applySlipRow` | CD:1647-1682 (36) | releases an edge on a latched arc that no other route replaces | the latch, `eq34_prev_target` | the draft | none |
| `notePin` | CD:1830-1843 | records the pin the fold reads | the draft | `el.new_prev_target`, `pinned_amb_targets`, `pinned_amb_sigma_scale` | none |
| `traceTreeEdge` | CD:1847-1937 (91) | the record of one edge | the draft, `eq34_prev_target`, `eq34_sat_root` | the record's edges and soft pins | none |
| `walkFrameSteps`, `walkRelease` | CD:2049-2091, 2003-2036 | the frame step pushed down the tree; the moved side of each release | the steps, `shift_on` | `eq34_reref`, the shift maps, `el.releases`, `release_sats`, `release_rx`; the station's receiver phase-bias carry dropped (CD:2080) | the receiver phase-bias carry |
| `observeTree` | CD:2110-2143 | depths, steps and chord cycles for the record (`arcgraph::fundamentalBasis`) | the forest | the record | none |
| `extendBand`, `extendPairTrees` | CD:2292-2323, 2240-2282 | band-only vertices of a pair family | the seeds, the walk | the gauge count, events | none |
| `noteDeseats` | CD:2334-2386 (53) | incumbent edges that left the tree on a slip | the latch and its causes | counters, the record | none |
| `finishChainElection` | CD:2839-2850 | the end, in a fixed order: gauge check, erase, release, shift, retain, advance | | | |
| `eraseReferencedCarries` | CD:2508-2545 | erases every station's carry of a re-referenced satellite band | `eq34_reref`, `release_sats` | `ArcState::amb` | arc carries |
| `releaseCutCarries` | CD:2555-2598 | erases the carries crossing each moved side; tallies releases by row | the releases, `prev_carry` | `ArcState::amb`, the record's releases (CD:2597) | arc carries |
| `applyCarryShift`, `eq34ShiftCarry` | CD:2602-2658, 60-119 | the S-transformation of the carries by known nonzero steps | the shift maps, `rebase_complete` | carries, `prev_carry`, coupling | carries |
| `retainGapEdges` | CD:2677-2749 (73) | under `phase_datum_eq34_extend_only`, keeps the edges of live carries | `anchor_incumbent`, `eq34_rx_parent`, `ArcState::amb` | `this_epoch_anchor` (CD:2709, an epoch event), the `el.new_*` maps | none |
| `advanceChainBaselines` | CD:2759-2833 (75) | replaces the sticky memory wholesale | the `el.new_*` maps | `anchor_incumbent` 2771, `eq34_rx_parent` 2773-2783, `eq34_sat_root` 2785, `eq34_prev_target` 2786, `eq34_pin_life` 2789-2790, `eq34_tree_seen` 2793-2800, `eq34_root_rpb_target` 2803-2809, `eq34_tree_root` 2812-2818, `eq34_retained_edge` 2819-2820, `LosState::eq34_prev_el` 2825-2831 | the chain's entity fields |

**Before PASS B: the hold epoch**

| function | file:lines (length) | role | reads | writes | state |
| --- | --- | --- | --- | --- | --- |
| `networkRingFixHoldEpoch` | RFS:548-934 (387) | in order: cuts from the record's releases (590-689), held links through the cuts (691-780), held links that entered the tree (782-796), kept roots (798-812), the release step (814-848), cascade releases (850-880), inherited old hosts (882-917), previous targets (919-924), the hold table (926-933) | the record (edges, releases, roots), the latch, the slip layer's gap resumptions, `ArcState::amb` | `ring_fix` links, `certified_edges` (793), `prev_targets`, `table`, counters; reopened arc carries; RINGFIX events | the ring fix |
| `ringFixEntryVerdict` | RF:708-718 | whether a cut's held entry is a known step | the evidence (gap measured or shifted, signature) | none | none |
| `ringFixCutStep` | RF:670-678 | a cut's integer step | the held integer, the target | the step | none |
| `ringFixApplyCuts` | RF:736-798 (63) | re-expresses held links across known cuts; releases them across unknown ones | the cuts | links, counters | the ring fix |
| `ringFixReleaseStep` | RF:569-617 (49) | releases held links: chain, slip, refloat, monitor, gap (first cause wins) | per-link facts | links, counters | the ring fix |
| `ringFixInherit` | RF:800-854 (55) | holds last epoch's tree edges that became chords, moved by known steps | candidates, cuts | links, counters | the ring fix |

**PASS B, the solve and after it**

| function | file:lines (length) | role | reads | writes | state |
| --- | --- | --- | --- | --- | --- |
| `networkRingFixPriors` | RFS:938-980 (43) | station step `ring_hold`: one prior row (σ 1e-4 m) per held column that is not a tree edge | `ring_fix->table`, `pinned_amb_targets` | the station's prior rows | none |
| `networkEq34PinAudit` | CD:3252-3317 (66) | the declared, folded and landed pin audit; the seed round's feedback | the assembly | counters; `ArcState::eq34_prev_target` (CD:3292-3293) | a second writer of `eq34_prev_target` |
| `networkRingFixDecide` | RFS:994-1632 (639) | after the solve: trees and groups (1014-1039), chord readings (1041-1166), audit (1168-1201), decisions (1203-1248), cascades (1250-1421), the trace row (1423-1479), the settled flags (1481-1590), the near-null guard (1592-1632) | `x_s`, the assembly, the record, the estimator, the lattice, `code_ref_event_s` | `ring_fix` links, `cascade_links`, `last_tree_s` (1021), `certified_edges`, `settled`, `settled_incumbent`, `settled_run`, the `untied_*` maps, `nn_reopen`; `network_ring_fix.csv`; RINGFIX events | the ring fix |
| `chainStationChordRows`, `chainChordFloats` | CCT:58-82, 93-130 | a station's chords and their two-band floats with the 2×2 covariance, from the station's own columns | the station's columns, the estimator | rows | none |
| `ringFixRings` | RF:234-272 (39) | depth, ring and candidacy of each chord over string vertex keys | the record's tree | none | none |
| `ringFixDecideStep` | RF:348-567 (220) | the oracle phase, continuity, the held-link monitor, the refusal ladder, the hold, the cap | per-chord observations | links, counters | the ring fix |
| `ringFixSettledSats`, `ringFixLargestSettledSet`, `ringFixSettledWait` | RF:43-58, 129-173, 79-87 | the settled set (rule root, rule largest) and the wait on a raise | held links, certified edges | `settled_run` | the ring fix |
| `requestUntiedMoves` | RFS:211-306 (96) | the re-attachment's requests (`ring_fix.reattach`, KEEP_OFF) | links, the record | the `untied_*` maps | the ring fix |
| `ringFixStampSettledFlags` | RF:60-77 | `signal_integer` and `wide_lane_integer` on the phase-bias records | `ring_fix->settled` | the solution | none |
| emission tail | `EmitStore.cpp:470-482`; `EmitStore.cpp:60-77`; `NetworkProductEmitter.cpp:586-696`, `846-853`, `976-990` | the released keys declared (from `chainDeclaredKeys`, CF:1187-1192); member records copied from their identity; the continuity guard, the unsettled steps, the member counters | the record, the table | the solution, the emitter | the emitter's counters |
| `networkHarvestRxPhaseBias` | CD:3323-3385 (63) | stashes each receiver phase bias and prunes old ones | the solve, the assembly | the station's receiver phase-bias carry | the carry |
| `networkRingFixNearNullMonitor` | RFS:1636-1655 | measure-only tally of the code signature | links | `ring_fix->nn_mon`, events | the ring fix |
| `networkChainFrameObserve` | CF:562-986 (425) | the observatory: classification, pin identity, `network_chain_edges.csv`, CHAIN events, the t-1 chord table, the memory | the record, the latch | the CSVs, events, counters, `ChainFrameMemory` (written by `chainRememberEpoch`, CF:512-558) | the memory, in a counters base (`RunContext.hpp:2474`) |
| run-end reports | RFS:1659-1846 (188), CD:2912-3246 (335), CF:1010-1185 (176) | the run-end tables | counters | the log | none |

Shorter helpers of the election, each a few lines and stateless: `chainCoverage` CD:319, `chainArcSlipped` CD:331, `chainArcLife` CD:353 (it also counts slip demotions), `makeChainBand` CD:403, `bridgedSlots` CD:702, `chainBandTreeArcs` CD:818, `tallyCascadeTree` CD:912, `asBuilt` CD:1045, `rxCellKey` CD:1217, `incumbentEdgeOf` CD:1232, `oldTreeRootOf` CD:1249, `childAbsentFromLastTree` CD:1388, `childSeenInEarlierTree` CD:1404, `retakeFirstSightingTarget` CD:1416, `tallySlipCause` CD:1619, `anySatRemembersOtherRoot` CD:1686, `noteTreeVertices` CD:1818, `observeDepths` CD:2096, `checkChainGauge` CD:2487.

### 1.4 The seam contract

| function | contract | where it is not kept |
| --- | --- | --- |
| `networkElectChainDatum` | not kept | takes the context and ten arguments, not `(NetworkRun&, const NetworkEpoch&)`; its gate is the caller's (`PhaseAnchorEpoch.cpp:132`); its two table bindings (CD:2864-2865) are written but not marked `// written`; no row in `tests/network/test_network_epoch_contract.cpp` |
| `networkRingFixHoldEpoch` | signature and gate kept | reads `ctx.counters` and `ctx.knobs` below the bindings (RFS:692, 703-713); writes the ring fix and the traces without a `// written` mark; no contract-test row |
| `networkRingFixDecide` | signature and gate kept | `ctx` and `traces` are writable bindings with no `// written` mark; every write to the ring fix goes through `*ctx.carry.ring_fix`; no contract-test row |
| `networkEq34PinAudit` | kept, with deviations | three aliases follow the end of the bindings (CD:3263-3266) and the gate (CD:3275) follows them; contract row at test line 239 |
| `networkHarvestRxPhaseBias` | kept | the written table `sta_rows` (CD:3334) is not marked; contract row at test line 368 |
| `networkChainFrameObserve` | mostly kept | spells `run.nc` below the bindings (CF:864) and reaches `ctx.carry.ar_lattice` and the traces directly (CF:871); contract row at test line 322 |
| `networkRingFixNearNullMonitor` | kept | an observer row (RosterTail); contract row at test line 308 |
| `networkRingFixPriors` | kept as a station step | the station's rows and counters written without a mark |

The entity-writer table lists `networkRingFixHoldEpoch` (`Entities.cpp:1359`, arcs) and `networkElectChainDatum` (`Entities.cpp:1398`, all stations); `networkRingFixDecide` writes the entity tables only through the ring fix's own maps.

### 1.5 The walk's order (the datum)

`arcgraph::growChainForest` (AG:483-691) attaches the roots (the datum receiver, then bridged satellites of a one-band family) and repeats, until both queues are empty, strictly in this order: (a) lead arcs (rank 1 breadth first, then the lowest higher rank one arc at a time; AG:557-593), which skip the new-edge test; (b) with held arcs given and the incumbent queue empty, the first held arc from an attached vertex to an unattached one that passes the new-edge test, or under `held_first_by_life` the one of greatest durability (AG:608-648); (c) the incumbent queue: every unvisited neighbour of the front vertex through its incumbent arcs in seed order (one at a time when leads are on); (d) without held arcs and leads, the plain breadth-first step (all new edges of the front vertex); (e) otherwise one new edge from the front of the new queue. A second pass, with the refusals off and the frontier in `legacyChainVertexRank` order, runs only when a refusal is armed and a vertex is unreached (AG:595-614). The order of everything that is not an incumbent therefore rests on queue order, slot order (satellites in string order, stations by roster index) and, for components and the second pass, the decimal string of the roster index (AG:427-430).

### 1.6 The recent additions (sections 109 to 118 of card NRF)

| key | state | where it sits | what it touches |
| --- | --- | --- | --- |
| `network.ring_fix.entry_revalidate` | adopted scoped (NRF 111.1) | RFS:596-603, 645-686; RF:708-718 | the known step of a released held entry; refused entries released with the cut (`entry_refused` events) |
| `network.ring_fix.nn_monitor` | trace | RFS:1636-1655; RF:680-706 | its own tallies; shares the guard's moving averages and keys (RFS:56-67) |
| `network.ring_fix.nn_guard_release` | built, not run (NRF 115) | RFS:1604-1617; arm text RFS:428-436 | releases a flagged chord at its first flag by setting `monitor_trip`, the held-link monitor's flag (RFS:1609) |
| `network.ring_fix.held_first_by_life` | built; one bed graded (NRF 116.1) | AG:617-647; CD:198, 521, 782, 2770, 2825; RFS:323; LDN:1638-1642 | the held block of the walk takes the held arc of greatest durability; durabilities and the elevation snapshot `LosState::eq34_prev_el` are computed whenever it is on |
| `network.rx_extra_band_delay_q` | built, not run (NRF 117) | not in the chain or the ring fix: `RxCodeBiasCarry.cpp:101-108` (`rxCbCarryQ`), `Knobs.cpp:187`, `ParamRegistry.cpp:260`, `SnapshotFingerprint.cpp:342` | the random walk of the receiver's extra-band code delay columns only |
| `network.phase_families` | built, not adopted (NRF 112 to 112.6) | none in CD, RF or RFS; intake `ApplicationNetworkSetup.cpp:240-282`, rename `RinexParser.cpp:379-401`, table `SignalRoster.cpp:871-901`, slip pairing `RawObservables.cpp:101-114`, member records `EmitStore.cpp:60-90`, member counters `NetworkProductEmitter.cpp:846-853`, `976-990`, tracking column `RunTraces.cpp:1210-1220`, the run-start table `ApplicationNetworkSetup.cpp:1393-1449`, fingerprint `SnapshotFingerprint.cpp:99-100` | the chain and the ring fix see only identity names; a member is mapped back at the slip layer's code pairing, the stream and the traces |
| `network.phase_family_codes` | work in progress (NRF 118, runs under way) | `ApplicationNetworkSetup.cpp:259-261`, `1585-1586`; `EmitStore.cpp:70-77` | the member's codes read under the identity's names too: one code bias per band |

## 2. Identities

### 2.1 The keys

About 520 key sites were classified across the five maps (counts are the maps' own tallies, with some overlap at their seams): about 120 name a station's arc (A), 85 a satellite band's state (S), 22 both at once (B), 28 a signal family or pair (F), and 130 something else: a band, a receiver's band cell, a chain vertex, a line of sight (O). The spellings:

| key | spelling | built | parsed or compared | names | typed identity |
| --- | --- | --- | --- | --- | --- |
| the ambiguity key | "G01_L1C" (`makeAmbiguityKey`, `StateKeySchema.hpp:117`) | CD:334, 522, 801, 829, 886, 1956, 2209 and the row functions | `net::satSignalKey`, `net::arcKey` | A as (station, key) pairs: the latch `free_ambs` and `carry_slipped_ambs` (`std::set<std::pair<int,std::string>>`), `new_prev_target`, `pinned_amb_targets`; S as `getSig(…, key, …)`: `anchor_incumbent`, `eq34_sat_root`, `eq34_tree_seen`, `this_epoch_anchor`, `new_sat_root`; B in the row functions (section 2.2) | `ArcId` for the arc, a satellite-band identity for the state |
| the satellite phase-bias key | "G01\|L1C\|PB" (`makeSatPhaseBiasKey`, `StateKeySchema.hpp:867`) | the station parameters | CD:391 (`net::satPhaseBiasKey`) | S: the satellite band's column | the satellite-band identity |
| (satellite, signal) string pairs | `std::pair<std::string,std::string>` | CD:2029, 2072-2073; RFh:778; CF:1187-1188 | CD:2533, 2576, 2651; RF:72 | S: `release_sats`, `eq34_reref`, `shift_sat`, `settled`, the declared keys | the satellite-band identity |
| arc tuples | `std::tuple<int,std::string,std::string>`, `net::ArcTuple` | RFS:699, 926-933; RFh:758, 821; CF:513-546 | RF:887, 894 | A: the hold table, previous targets, the latch, truth | `ArcId` |
| (station, satellite) pairs | `std::pair<int,std::string>` | RFh:755; CD:797-806 | RF:493 | a line of sight: `last_tree_s`, the held-arc filter | `LosId` |
| the link | `RingFixLinkId {int station; std::string sat, sig1, sig2}` (RFh:454-465) | RFS throughout | RF:550 (order) | A: a station's pair link; reused for cascade links with `sig1` the reference and `sig2` the third signal (RFh:788, 802) | missing: `LinkId` (a `LosId` and a `SignalPair`); a separate cascade-link identity |
| band and receiver-cell keys | "G\|L1C" (`dr_key`, `rxCellKey`, `ChainGroupKey`), "\|G\|L1C" (`rx_suffix`), "GPS_RPB_L1C" (`rpb_key`), "<name>\|G\|L1C" (receiver parent), (index, letter, signal) (`shift_rxg`) | CD:403-421 (`makeChainBand`), CD:821, 1160-1166, 1217, 2566, 2720 | CD:2722-2728, 2776-2780 (`rfind('\|')`), `Entities.cpp:826-838` | O: a constellation's band, a receiver's cell for it; `StationState::signals` holds two dialects of one cell, "G\|L1C" (CD:1217) and "GPS_RPB_L1C" (CD:1762-1765, 2807) | missing: a band identity (`System`, `SignalId`) and a station-band identity (`StationId`, band) |
| chain vertices | "R:<index>", "S:<satellite>" | CD:1862, 1900, 1908, 2020, 2356-2357; CF:231-232, 819, 832; RFS:49-50 | CF:248-254; RF:251 (`u[0] == 'R'`); RFS:626-627 (`rfind("S:", 0)`, `std::stoi`) | O: a vertex of the tree | missing: a vertex identity (a `StationId` or a `SatId`) |
| family strings | "E:L1X/L5X" (pairs), "E:L1C/L5Q/L7Q" (cascades), "E\|L1C+L5Q" (chord family), "E\|L1C" (group) | RFS:217, 1514, 1527; CCT:70-75; RFS:51; CF:866 | by fixed offsets: LDN:1539-1548, RFS:405-406, 473-477, `NetworkProductEmitter.cpp:109-116` | F | missing: a pair-family identity (`System`, `SignalPair`) with one parse |
| the phase-family row | "E:L1X>L1C" | LDN:1487-1493 (layout by index) | `SignalRoster.cpp:873-880` (typed once, `PhaseFamilyRow`) | F | typed (`PhaseFamilyRow`) |
| raw letter and digit reads | `sat[0]`, `sig[1]` | CD:884; RFS:60-61, 1283, 1556, 1559, 1562; RF:109-110; CCT:38, 47-56; `NetworkProductEmitter.cpp:846-848` | | O and F: a constellation by its letter, a band by its digit | `SatId::sys`, `SignalId::digit`, the constellation's band table |

### 2.2 Where one key names both

Each site below uses one string for a station's arc and for a satellite band's state in the same code. With a member that keeps its own arc name, the arc's signal (L1X) and the band state's signal (L1C) differ, and every site must take the two apart.

| site | the arc use | the satellite-band use |
| --- | --- | --- |
| CD:1956 (`recordTreeEdge`: `amb = makeAmbiguityKey(sat, band.signal)`) | `getArcAmb` and `hasArcAmb` (CD:1665, 1967), the previous target through `draftPinStep` (CD:1194) | `anchor_incumbent` (CD:1142), `this_epoch_anchor[amb]` (CD:1144) |
| CD:1388-1401, 1447-1453 (`childAbsentFromLastTree`, `applyReturningRow`) | the latch `free_ambs.count({ri, amb})` (CD:1453) | `getSig(sig_rows, amb, anchor_incumbent)` (CD:1391) |
| CD:1404-1412 (`childSeenInEarlierTree`) | | `net::satSignalKey(amb)` re-parsed (CD:1408) beside a receiver cell (CD:1410), chosen by a flag |
| CD:1572-1579 (`applyRehostRow`) | the latch (CD:1579) | `incumbentEdgeOf(…, amb)` (CD:1575) |
| CD:1647-1665 (`applySlipRow`) | the latch (CD:1650), `getArcAmb` (CD:1665) | `incumbentEdgeOf` (CD:1664), `eq34_tree_seen` |
| CD:1115-1125 (`draftPinTarget`) | `getArc(arcs, (ri, sat, signal), amb)` (CD:1119) | `getSig(sig_rows, amb, anchor_incumbent)` (CD:1118), the "was lit" gate |
| CD:1232-1242 (`incumbentEdgeOf`), CD:1249-1256 (`oldTreeRootOf`) | | the satellite's anchor or the receiver's cell, chosen by `sat_is_child` |
| CD:594-617 (`seedIncumbents`) | `arcIndex` over (station, satellite) | `anchor_incumbent` and `eq34_rx_parent` read as seed edges |
| CD:2701-2710 (`retainGapEdges`) | `new_retained_edges.emplace(station, amb)` | `amb = net::ambKey(sid)` from a satellite row |
| CD:1689, 1711, 1786 against CD:801 | the latch, keyed by the same text | `getSig(…, makeAmbiguityKey(s, band.signal), eq34_sat_root)` |
| RFh:251, 447 (`ringFixSettledSats` and the largest set) | `std::vector<std::pair<int,std::string>>` links | the returned satellite set |
| RFh:1025-1031, RF:759-760, 809-813 (`RingFixCut`) | the moved side's receivers | the moved side's satellites, queried alike |
| RFh:825-831 (the untied maps) | | `std::pair<std::string,std::string>` whose first element is a family in three maps and a signal in `untied_moves` (RFh:827) |

### 2.3 The typed identities that should stand there

Present in `include/common/GnssIdentity.hpp`: `SatId` (499), `SignalId` (595), `ObservableId` (690), `SignalPair` (745), `SatSignalId` (802), `StationId` (843), `LosId` (881), `ArcId` (917). The arc graph is already typed (`ArcId` is the same type there, AGh:55, AG:36-41); the untyped keys live in the election's working set, the record, the ring fix and the chord table, and cross the graph's boundary at CD:496-531 (into the graph) and CD:2121-2140 (`observeTree` turns `ArcId` terms back into strings). What should stand at each kind of site:

1. **A station's arc: `ArcId`** (station, satellite, the arc's own signal). It keeps the member's signal when a family's member keeps its name. Sets of arcs (the latch, the retained edges) become `std::set<ArcId>`.
2. **A satellite band's state: missing, a satellite-band identity** (`SatId` and the family's identity signal; for example `SatBandId`). It names the satellite phase-bias column, the satellite's anchor, root, tree memory, settled flag and shift. It is built from an arc only through the phase-family map (`phaseFamilyOf`, `PhaseFamily.hpp:46-50`, today without a production caller), so that a member's arc and its identity's arc reach one band state. Today `SatSignalId` with the arc's own signal stands for it, which is exact only while the names are equal (as after the parse-time rename).
3. **A receiver's band cell: missing, a station-band identity** (`StationId`, the band), in place of four spellings and two dialects in `StationState::signals`.
4. **A constellation's band (the chain's group): missing, a band identity** (`System` and the family's identity `SignalId`), in place of "G\|L1C", `ChainGroupKey` and `dr_key`.
5. **A chain vertex: missing, a vertex identity** (a `StationId` or a `SatId`), in place of "R:"/"S:" strings built in four files and parsed in three.
6. **A pair family: missing, a pair-family identity** (`System`, `SignalPair`) with one parse, in place of five offset parses; and **a link: `LinkId`** (`LosId`, the family), with a distinct identity for a cascade's link.
7. **A line of sight: `LosId`**, in place of the (station, satellite) pairs.

Continued in [part 2](CHA_chain_audit_2026-11-18_part2.md): the rules, the defects, the refactor plan and what it enables.

<!-- Results are appended below this line under dated headings. -->
