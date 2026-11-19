# 9 · The chain datum and the ring fix

**Goal:** the seams of the chain datum and the ring fix in the order a run reaches them, the
identities they key on, and every rule with its key, its ledger verdict and the function that
applies it, so that a chain or ring-fix line in a log or a trace leads to its code. Page 7 comes
first.

The page describes the code after card CHA's refactor (steps S1 to S11). Both mechanisms are off
by default and in the reference configurations (`config/golden/*_refound.json` set
`network.phase_datum_eq34` false and no `network.ring_fix.*` key). The integer-fixing configuration
of card NRF arms both (its values: card CHA part 2, section 3).

---

## The two mechanisms

**The chain datum** ("eq34", after Eq. 34 of Odijk and de Jonge; `network.phase_datum_eq34`). The
election works per **family**: one constellation's phase signal (a band), or two bands of one
constellation that share one tree. In each connected component of a family's receiver-by-satellite
graph it elects a datum receiver r1, pins r1's receiver phase bias (RPB, a column per receiver and
band) and pins the ambiguities of a spanning tree rooted at r1 at integer targets: n_c + m_c - 1
tree-edge pins and one RPB pin for n_c stations and m_c satellites, the gauge dimension. Every other
arc, a **chord**, is then an integer in the chain's frame. The tree is sticky, and an **event row**
gives each class of edge event its integer rule.

**The ring fix** (`network.ring_fix.enable`). After the solve it decides the integers of the
chords of a pair family by a gate on the chord's two-band float, a damage gate and a persistence
count. A decided chord is **held** from the next epoch by one prior row per band and released at a
slip, a refloat, a gap, a chain event or the residual monitor.

Terms: the **record** is `ChainEpochObs` (`include/network/solve/ChainFrame.hpp`): the
epoch's tree edges, components, chords, cut releases and root handovers, written by every election
and read by the ring fix, the stream and the observatory. The **latch** is
`RelevelEpochState::free_ambs`: the arcs a slip detector or a re-entry response freed this epoch. A
**cut release** releases a tree edge: its child and subtree (the moved side) take an unknown step,
and every carry relating the moved side to the rest is erased.

## 1. One epoch, in order

The seams in the order a run reaches them: the setup, one epoch of `Application::networkProcessEpoch_`
(`src/core/network/driver/ApplicationNetworkEpoch.cpp`), the run end. Paths are under
`src/core/network/`.

| # | place | seam (file) | armed by | reads; writes |
| --- | --- | --- | --- | --- |
| 0 | setup, from `runNetwork` | `networkRingFixArm` (`ar/RingFixSeams.cpp`) | `network.ring_fix.enable` | `ctx.carry.ring_fix` (null when off) |
| 1 | phase datum, after the measurement admission | `networkPhaseAnchorEpoch` (`solve/PhaseAnchorEpoch.cpp`) | always; the chain branch needs `ctx.knobs.phase_datum_prioritized`, true whenever `network.phase_datum_eq34` is | the candidates (`networkBuildPhaseDatumCandidates`), the hygiene (`networkAnchorHygiene`); then 2, or without the chain `networkElectPhaseAnchors` |
| 2 | inside 1 | `networkElectChainDatum` (`solve/ChainDatum.cpp`) | `network.phase_datum_eq34` | candidates, latch, held links, chain memory; pins, `shared_phase_keys`, `events.phase_datum` (the record, `this_epoch_anchor`, `eq34_reref`), carries, chain memory |
| 3 | after 1 | `networkReceiverPhaseBiasAllocation` (`solve/PhaseAnchorEpoch.cpp`) | `network.phase_datum_eq34` | `shared_phase_keys`; the stations' RPB columns |
| 4 | before the drop pass and PASS B | `networkRingFixHoldEpoch` (`ar/RingFixSeams.cpp`; stages `ar/RingFixHold.cpp`) | the ring fix's state | record, latch, carries; held links, hold table, re-opened carries (`networkRingFixReopenCarry`), events |
| 5 | PASS B, parallel | `networkPhaseDatumPinRows` (`solve/PhaseDatum.cpp`, step `phase_datum_pins`) | a station with pins | its pins; its 1e10 pin rows |
| 6 | PASS B, parallel | `networkRingFixPriors` (`ar/RingFixSeams.cpp`, step `ring_hold`) | the leveling carry injects, the table is not empty | the hold table; a prior row (sigma 1e-4 m) per held column off the tree |
| 7 | PASS B, serial | `networkRingFixStationTally` (`ar/RingFixSeams.cpp`); `networkEq34PinAudit` (`solve/ChainDatum.cpp`) | the ring fix's state; the chain and the consume's verdict | row counts; counters, the pin audit, `ArcState::eq34_seed_realized` |
| 8 | the solve | `networkSolveGlue` (`solve/SolveGlue.cpp`) | always; bridges: `network.phase_datum_time_linked` | the pin bridges (`networkInjectPinBridgePrior`, stage 4); `networkRankGate` (stage 8) withholds a mis-pinned datum |
| 9 | after the lattice step | `networkRingFixDecide` (`ar/RingFixSeams.cpp`; stages `ar/RingFixDecide.cpp`) | the ring fix's state, an armed record | solve, assembly, record, lattice; held links, settled flags, `network_ring_fix.csv`, events |
| 10 | solution prep; emission | `ringFixStampSettledFlags` (`ar/RingFix.cpp`, in `networkSolPrep`); `chainDeclaredKeys` (`solve/ChainFrame.cpp`, in `networkEmitStore`) | `network.ring_fix.settled_flags`; `network.disc_step_class_pairs` | settled flags, record; `signal_integer`, `wide_lane_integer`, the released keys declared |
| 11 | leveling harvest | `networkHarvestRxPhaseBias` (`solve/ChainDatum.cpp`) | the chain, `network.phase_datum_eq34_rpb_q_m2s` above 0 | solved RPBs; `StationSignalState::rpb` |
| 12 | observer, RosterTail | `networkRingFixNearNullMonitor` (`ar/RingFixSeams.cpp`) | `network.ring_fix.nn_monitor` | code averages; tallies, events |
| 13 | observer, StashTail | `networkChainFrameObserve` (`solve/ChainFrame.cpp`) | `network.trace_chain_datum` or `network.trace_rpb_states` | record, latch, solve; `network_chain_edges.csv`, `network_chain_frame.csv`, `network_rpb_states.csv`, CHAIN events |
| 14 | run end (`runend/RunEnd.cpp`) | `networkRingFixRunEnd`, `networkChainDatumRunEnd`, `networkChainFrameRunEnd` | the ring fix's state; per section; `network.trace_chain_datum` | counters; the run-end lines |

**Why the election comes first.** It runs after every PASS A slip drain, the signal-integrity
refloats and the measurement admission, so the latch and the slip funnel are complete and a
withheld channel's phase-bias state is pruned first. It precedes the RPB allocation, which
derives each station's RPB columns from `shared_phase_keys`, the set only the election writes on
this path: in the other order a pinned RPB key could name a pruned column and a station new to the
graph would get none (the banner of `include/network/solve/ChainDatum.hpp`).

**Inside the election.**
1. `ringFixGatherKeepEvidence` (keep-carry and the entry check armed) records each held link's
   entry evidence: the gap resumptions, final after PASS A, and the epoch-start near-null signature.
2. A record is created, the pins are cleared, `shared_phase_keys` is set from the candidates' union.
3. `groupChainKeys` groups the keys by band; `formChainFamilies` (`chainFamiliesOf`) forms one family
   per band, or with `network.phase_datum_eq34_family` "pair" one for a constellation with exactly
   two phase signals of different bands (with more, the pairs of
   `network.phase_datum_eq34_family_pairs`).
4. `electFamily`: `buildChainGraph`, `seedIncumbents` (last epoch's live tree edges),
   `planChainFamily` (the components, their datum receivers and forests); per band and component
   `electBandComponent`: r1's RPB pin, the root row, `recordTreeEdge` per edge in attachment order,
   `walkFrameSteps`, `observeTree`; `extendBand` attaches a pair's band-only vertices.
5. `finishChainElection`: `checkChainGauge`, `eraseReferencedCarries`, `releaseCutCarries`,
   `applyCarryShift` (the known steps), `retainGapEdges`, `advanceChainBaselines` (the new memory),
   `closeRestoredLives`.

The datum receiver (`electDatumReceiver`) is the configured one (`network.phase_datum_eq34_receiver`),
else the root most of the component's satellites remember (`eq34_sat_root`), else the band's
`datum_rx`, each only if in the component, else the highest coverage.

**The walk.** `arcgraph::growChainForest` (`ar/ArcGraph.cpp`) attaches the roots (r1, then the
satellites a pin bridge grounds) and repeats until its queues are empty, each turn taking the first
step that applies:
1. `chainLeadStep`, with lead arcs only (`network.ring_fix.cascade_tree`, `network.ring_fix.reattach`):
   rank 1 breadth first, then the lowest higher rank one arc at a time, without the new-edge test.
2. `chainHeldStep`, with held arcs (`network.ring_fix.held_first`) and the incumbent queue empty:
   the first held arc to an unattached vertex that passes the test, or the most durable under
   `network.ring_fix.held_first_by_life`.
3. `chainIncumbentStep`: the front vertex's unattached neighbours through its incumbent arcs.
4. `chainNewEdgeStep`: the front vertex's new edges that pass the test, one at a time when held or
   lead arcs are given.

The test (`chainNewEdgeOk`) refuses a slipped arc and an ineligible host while those refusals are
armed (`chainForestRules`). A second pass, run only when a refusal is armed and a vertex is
unreached, admits every arc from a frontier in the chain's vertex order. That order,
`arcgraph::chainVertexRank`, ranks receivers by the decimal string of their roster index ("10"
before "2"), then satellites by slot; a component's root is always a satellite, so components come
in the string order of their root satellites. The order is part of the datum: the unit case
`ChainVertexRankOrdersRosterIndicesAsStrings` (`tests/network/test_network_arc_graph.cpp`) pins it.

**The rows and the pins.** `recordTreeEdge` drafts each edge as built (`draftPinTarget`: the
rounded carry, the seed round's target or 0; `draftPinStep`), then applies the armed rows in this
order, each leaving alone an edge an earlier row decided: `applyMergeRow`, `applyKeptRow`,
`applyReturningRow`, `applyRehostRow`, `applySlipRow`. The root row runs per component before its
edges (`applyRootRow`). `notePin` writes the pin the fold reads (`NetStation::pinned_amb_targets`);
`traceTreeEdge` adds the edge to the record. A released edge's step is unknown, so `walkFrameSteps`
re-references its moved side's satellites (`eq34_reref`) and drops its receivers' RPB carries.

**The hold epoch, before PASS B.** `networkRingFixHoldEpoch` reads the election's record and runs
before the drop pass (`networkAmbFloatDropEpoch`) and PASS B, whose produce only reads the hold
table. Its stages (order constraints in `include/network/ar/RingFixHold.hpp`):
`networkRingFixHoldTruth`, `networkRingFixHoldCuts`, `networkRingFixHoldThroughCuts`
(`ringFixApplyCuts`: held links re-expressed across a known cut, released across an unknown one),
`networkRingFixHoldToTree`, `networkRingFixHoldRoots`, `networkRingFixHoldReleases`
(`ringFixReleaseStep`), `networkRingFixHoldCascadeReleases`, `networkRingFixHoldInherit`
(`ringFixInherit`), `networkRingFixHoldTargets`, `networkRingFixHoldTable`.

**The decision, after the solve.** `networkRingFixDecide` runs after `networkFixedCycles`, whose
lattice its veto reads, and before `networkSolPrep`, which stamps this decision's settled flags; a
decided chord is held from the next epoch's PASS B. Its stages
(`include/network/ar/RingFixDecide.hpp`): `networkRingFixDecideReports`,
`networkRingFixDecideChain` (`ringFixRings`), `networkRingFixDecideReadings` (`chainChordFloats`),
`networkRingFixDecideAudit`, `networkRingFixDecideLinks` (`ringFixDecideStep`),
`networkRingFixDecideCascades`, `networkRingFixDecideTrace`, `networkRingFixDecideSettled`, and
`networkRingFixDecideNearNull` last: its moves act on the next epoch.

**One owner for a held entry.** Whether a held link entering the tree carries its integer as a
known step is decided by `chainEntryStep` (`ar/RingFix.cpp`) for three askers (`ChainEntryKind`):
the ring fix's cut, the returning row (never known) and keep-carry (`chainHeldInteger`). A refused
keep is released in the election and reported by the same epoch's decision (RINGFIX `keep_refused`).

## 2. The identities

The arc graph is typed (`StationId`, `SatId`, `ArcId`); the election and the ring fix key mostly by
text. "File-local" means in `src/core/network/solve/ChainDatum.cpp`.

| identity | type and home | names |
| --- | --- | --- |
| chain vertex | `ChainVertex`, keys "R:<roster index>" and "S:<satellite>" (`chainVertexKey`, `chainVertexOfKey`, `chainVertexIsStation`; `include/network/solve/ChainVertex.hpp`); a slot in the graph (`arcgraph::stationVertex`, `arcgraph::satVertex`) | a tree's receiver or satellite (record, rings, cuts) |
| band | `ChainGroupKey` (constellation, phase signal), file-local, text "G\|L1C" (`chainBandText`, `chainBandOfText`); `ChainBand` adds the wavelength and the RPB key | the election's group; a cell of `net::ConstellationState::channels` |
| receiver's band cell | `ChainStationBand` (roster index, band), file-local; in `net::StationState::signals` the band's text, or `chainRpbKey` ("GPS_RPB_L1C") | one receiver on one band: tree parent, RPB pin and carry |
| arc | `ArcId` (station, satellite, signal; `include/common/GnssIdentity.hpp`); as text the key "E05_L1X" (`ChainEdgeKeys::arc`) | an arc under its own signal: latch, carry, pin, pin life |
| satellite band | no own type: the key `ChainEdgeKeys::band` ("E05_L1C", the band's identity signal) from `chainEdgeKeys`, stored under `SatSignalId` | a satellite on a band: tree parent, root, tree memories |
| family | `ChainFamily` (one or two `ChainBand`s), file-local, text "E:L1C/L5Q" (`chainFamilyText`) | the unit of the election |
| ring-fix link | `RingFixLinkId` (station, satellite, `sig1`, `sig2`; `include/network/ar/RingFix.hpp`); a cascade link reuses it with the reference and the third signal | a link at one station: the keys of `NetworkRingFix::links` and `cascade_links` |
| pair families | `NetworkRingFix::pairs` from "E:L1C/L5Q" (`pairConsidered`); `chainPairFamilies`, `chainArcFamilies` (`include/network/solve/ChainChordTable.hpp`); `NetworkKnobs::eq34_family_pairs`; `RingFixCascade` ("G:L1C/L2W>L5Q") | the signals that decide a chord |
| phase families | `PhaseFamilyRow` (member, identity), `PhaseFamilyTable` (`include/network/assembly/PhaseFamily.hpp`: `phaseFamilyOf`, `phaseFamilyCodeOf`); in the election `chainBandSignalOf` and its inverse `chainArcSignalsOf` | a member signal (Galileo L1X) sharing its identity's band (L1C) |
| life stamp | `ArcLifeStamp` (`include/network/ar/ArcLife.hpp`: `arcLifeStamp`, `arcPinnedLifeContinues`) | an arc's integer life (arc ledger, carry, screen flag) |

An arc's key and its satellite band's key differ only for a phase-family member that keeps its own
signal (card CHA, defect P5).

The fields the chain keeps across epochs (`include/network/state/Entities.hpp`, persistence from
`Entities.cpp`):

| field | entity | holds | between epochs |
| --- | --- | --- | --- |
| `datum_rx` | `net::ChannelState` | the band's datum receiver | record EQD |
| `anchor_incumbent` | `net::SatSignalState` | the satellite band's tree parent | record PA |
| `eq34_sat_root` | `net::SatSignalState` | its datum receiver at the last election | record EQS |
| `eq34_rx_parent` | `net::StationSignalState` | the receiver band's tree parent | record EQP |
| `eq34_tree_seen` | `SatSignalState`, `StationSignalState` | a vertex of an earlier tree | ColdStart |
| `eq34_tree_root` | `SatSignalState`, `StationSignalState` | the root of its last tree | ColdStart |
| `eq34_root_rpb_target` | `net::StationSignalState` | a root's pinned RPB target | ColdStart |
| `rpb` | `net::StationSignalState` | the RPB carry | ColdStart |
| `eq34_prev_el` | `net::LosState` | last epoch's elevation | record EQE |
| `amb` | `net::ArcState` | the leveling carry | record L |
| `eq34_prev_target` | `net::ArcState` | the pin's last target | record EQT |
| `eq34_seed_realized` | `net::ArcState` | the pin audit's seed-round target | Latch |
| `eq34_pin_life` | `net::ArcState` | the arc's life stamp at its last pin | ColdStart |
| `eq34_retained_edge` | `net::ArcState` | a tree edge kept across a row gap | record EQR |

`advanceChainBaselines` replaces the tree memories at the end of each election. After a warm resume
the ColdStart tree memories start empty unless the snapshot's chain memory section restores them
(`network.state_snapshot_chain_capture`, `network.resume_chain_restore`;
`include/network/state/ChainMemory.hpp`). `NetworkRingFix` declares its 59 members in
`net::kRunFields` as `carry.ring_fix.<member>`.

## 3. The rules

Verdicts are those of `docs/CONFIG_promotion_ledger.json`; files are named by basename.

**The chain's event rows** are the entries of `network.phase_datum_eq34_event_rows` (UNVALIDATED;
empty by default: the as-built rule). `kChainEventSpec` (`ChainFrame.cpp`) states each row's
integer rule. The loader clears the list without the chain, without `network.ambiguity_vehicle`
"float_kf" or with `network.carry_invariant`; it drops `merge_split` without `root`, and `root`
when `network.state_snapshot_path` is set without `network.state_snapshot_chain_capture`.

| entry | what the row does | lives in (`ChainDatum.cpp`) |
| --- | --- | --- |
| `kept` | an incumbent edge whose pinned life continues keeps its previous target, no step of its own | `applyKeptRow` |
| `returning` | a vertex back in its band's tree after an absence takes the cut release | `applyReturningRow` |
| `rehost` | a vertex moving to a new parent edge takes the cut release, unless keep-carry keeps it on a held integer | `applyRehostRow` |
| `root` | a new datum receiver pins its RPB at its carried value, so the frame does not step | `applyRootRow` |
| `slip` | a latched arc leaves the tree at the slip epoch; one no other route replaces takes the cut release | `applySlipRow`, `seedIncumbents` |
| `merge_split` | an edge joining two old trees takes the cut release; a split piece's new root takes the root rule | `applyMergeRow`, `applyRootRow` |

**The ring fix's rules.** Keys are under `network.ring_fix.`; each is inert without `enable`. The
frozen numbers are constants of `include/network/ar/RingFix.hpp` (`kRingFixWindowS` 600 s,
`kRingFixPersistEpochs` 10, `kRingFixHoldSigmaM` 1e-4 m, `kRingFixMonitorBoundM` 0.03 m at
`kRingFixMonitorEpochs` 4).

| rule | what it does | key | verdict | lives in |
| --- | --- | --- | --- | --- |
| arming | builds the state; needs `network.phase_datum_eq34` | `enable` | UNVALIDATED | `networkRingFixArm` (`RingFixSeams.cpp`) |
| decision rule | `gated`, `plain` (every candidate held) or `log` (nothing held) | `rule` | UNVALIDATED | `ringFixDecideStep` (`RingFix.cpp`) |
| order | `ring` (ring by ring from the root) or `certain` (most certain first) | `order` | UNVALIDATED | `ringFixDecideStep`, `ringFixRings` |
| gate | `sigma` (N1 sigma at most 0.10, fraction at most 0.20 cycle) or `odds` (log-odds at least ln 1e6) | `gate` | UNVALIDATED | `ringFixGate` (`RingFix.cpp`) |
| sigma scale | a factor on the odds gate's sigmas | `sigma_scale` | RECOMMEND_SCOPED | `ringFixGate`, `ringFixCascadeGate` |
| wide-lane bound | refuses a wide-lane fraction above the bound | `wl_frac_max` | UNVALIDATED | `ringFixDecideStep` |
| cap | at most N decisions per epoch (order `certain`) | `max_decisions_per_epoch` | RECOMMEND_SCOPED | `ringFixDecideStep` |
| code-event window | no decision within the window after a satellite's code event | `code_event_window_s` | UNVALIDATED | `ringFixCodeEventOpen` (`RingFix.cpp`), `networkSolPrep` |
| pairs | the pair families decided | `pairs` | RECOMMEND_SCOPED | `NetworkRingFix::pairConsidered`, `networkRingFixDecideReadings` |
| configured families | the chord table forms every listed pair an arc tracks | `form_configured_pairs` | UNVALIDATED | `chainArcFamilies` (`ChainChordTable.cpp`) |
| held first | a fragment re-attaches through a held link before a new edge | `held_first` | UNVALIDATED | `chainHeldArcs` (`ChainDatum.cpp`), `chainHeldStep` (`ArcGraph.cpp`) |
| held first by life | the held arc of most remaining life is taken | `held_first_by_life` | UNVALIDATED | `chainHeldStep`, `chainArcLife` |
| keep-carry | a re-host onto a held link whose carry sits on the integer keeps it at step 0 | `rehost_keep_carry` | RECOMMEND_SCOPED | `chainHeldInteger`, `applyRehostRow` (`ChainDatum.cpp`) |
| entry check | a held entry is known only after a clean gap check and, on Galileo E1/E5a, a near-null signature off a step | `entry_revalidate` | RECOMMEND_SCOPED | `chainEntryStep` (`RingFix.cpp`), `networkRingFixHoldCuts` |
| settled flags | `signal_integer` 1 while a satellite is tied to its root through held links | `settled_flags` | RECOMMEND_SCOPED | `networkRingFixDecideSettled`, `ringFixStampSettledFlags` |
| settled rule | `root` or `largest` (the largest tied set) | `settled_rule` | RECOMMEND_SCOPED | `ringFixSettledSats`, `ringFixLargestSettledSet` |
| settled wait | a flag rises after N emissions in the set | `settled_wait_emissions` | RECOMMEND_SCOPED | `ringFixSettledWait` (`RingFix.cpp`) |
| unsettled steps | an unsettled held family's counters step at every emission | `unsettled_steps` | RECOMMEND_SCOPED | `NetworkProductEmitter::emitToStore` |
| unheld steps | likewise for signals of no held family | `unsettled_steps_unheld` | RECOMMEND_SCOPED | as above |
| continuity guard | a flagged level that moves under constant counters lowers its flags | `continuity_guard_cyc` | UNVALIDATED | as above |
| guard's hold and run | the hold (20 emissions) and the run before comparing (4) | `continuity_guard_hold`, `continuity_guard_min_run` | PARAM | `NetworkProductEmitter::setContinuityGuard` |
| re-attachment | moves an untied satellite's tree edge at the next election | `reattach` | KEEP_OFF | `requestUntiedMoves` (`RingFixDecide.cpp`), `untiedLeads` (`ChainDatum.cpp`) |
| near-null guard | a held E1/E5a chord at k near-null steps moves by -k (3, 4) | `nn_guard` | KEEP_OFF | `networkRingFixDecideNearNull` (`RingFixDecide.cpp`) |
| release form | the guard releases instead of moving | `nn_guard_release` | KEEP_OFF | `networkRingFixDecideNearNull` |
| signature parameters | averaging (60 epochs), tolerance (0.12 m), ionosphere-free bound (0.30 m) | `nn_guard_min_epochs`, `nn_guard_tol_gf_m`, `nn_guard_max_if_m` | PARAM | `ringFixNearNullInputs`, `ringFixNearNullVerdicts` |
| near-null monitor | measure-only tally of the signature | `nn_monitor` | TRACE | `networkRingFixNearNullMonitor` (`RingFixSeams.cpp`) |
| cascade | a third signal decided from a held pair | `cascade` | UNVALIDATED | `networkRingFixDecideCascades`, `networkRingFixHoldCascadeReleases` |
| cascade tree | the third band's walk takes the pair's trees first | `cascade_tree` | UNVALIDATED | `chainCascadeLead` (`ChainDatum.cpp`), `chainLeadStep` |
| cascade log | the cascades trace, never hold | `cascade_log` | UNVALIDATED | `networkRingFixDecideCascades` (`RingFixDecide.cpp`) |
| cascade keep-carry | keep-carry for a third band | `cascade_rehost_keep` | UNVALIDATED | `chainCascadeHeldInteger` (`ChainDatum.cpp`) |
| truth audit | simulation only: held integers against truth | `truth_root` | UNVALIDATED | `ringFixArmTruth`, `networkRingFixDecideAudit` |
| oracle phase | simulation only: true integers held for N seconds | `oracle_until_s` | UNVALIDATED | `ringFixDecideStep` |
| audit file | simulation only: held integers against a target file | `audit_frame_file` | UNVALIDATED | `ringFixReadAuditFile` (`RingFix.cpp`) |
| slip mask | simulation only: one arc's slip flags masked | `mask_slip_arc`, `mask_slip_tow`, `mask_slip_epochs` | UNVALIDATED | `passASlipMaskApplyStage` (`PassASlipDrains.cpp`) |

## Where to look next

- Card CHA: [part 1](../refound/CHA_chain_audit_2026-11-18.md) (the map and the identities, at
  `72b7b3b`), [part 2](../refound/CHA_chain_audit_2026-11-18_part2.md) (the rules, the defects P1
  to P18, the plan), [part 3](../refound/CHA_chain_audit_2026-11-18_part3.md) (steps S8 to S10,
  what survives an epoch).
- Card NRF, the ring fix: [the first file](../refound/NRF_network_ring_fixing_2026-11-14.md) (the
  frozen rule), [part 4](../refound/NRF_network_ring_fixing_2026-11-14_part4.md) section 24.3
  (keep-carry), [part 14](../refound/NRF_network_ring_fixing_2026-11-14_part14.md) section 108 (the
  near-null guard), [part 15](../refound/NRF_network_ring_fixing_2026-11-14_part15.md) sections 111
  (the entry check) and 113 (the monitor). Each key's ledger row names the part that decided it.
- [Event legs](event_legs.md): resumed runs, for the chain's memory across a resume.
- [Where new code goes](where_new_code_goes.md): the seam a new chain or ring-fix step belongs to.
