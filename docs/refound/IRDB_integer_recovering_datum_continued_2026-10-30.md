# IRDB: the integer-recovering datum, continued (IRD-1 onward)

**STATUS**

- **id:** IRDB
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRD's continuation (that file nears the 60 KB bar), from step IRD-1 on; the derivation, the event table, the arms, the knobs, the graders, the bars, the decision rules and the frozen gates stay in card IRD sections 3 to 11, the re-registered control in its section 13. IRD-1 LANDED, measure-only and default off (section 2): the chain's event table, edge and chord traces, CHAIN events, pre-hold and receiver phase-bias traces and the run-end frame census; the consumer's shared AR builder, observe-mode integer search, fix traces and extended gate. Every gate of card IRD section 11 passes, with `rt_seam_verify.py` not run (retired) and the retired lineage graded same-build. The A0 chain on rung 3c sits off the integers: 144 re-hosts in 2 h, 128 of them stepped by a fraction and none certified; chord table 4.1 % determined; receiver phase-bias time-link NEES 2.58. On truth wires the consumer fixes 2 506 settled integers per bed, 0 wrong; the golden wire with the gate off fixes wide lanes 75 % wrong, and the gate admits nothing. IRD-2 RAN (section 3) and the card STOPPED by its static-algebra stop rule: on the microscope, chords event-free for 30 min read p50 0.22 cycle against 0.02, because the producer's double differences under bundle A0 drift off the integers after the first half hour on every bed, where R1's are exact from 20 min. The bisection (section 4) finds the cause in the chain's event handling, not its static algebra: on the microscope A0 is R1 plus the chain (the held machinery and the coverage gate are inert under it, byte-identical), and with the RPB floor off the chain's double differences track R1's until the first satellite return, whose re-admission steps every satellite clock and shocks the carried chords network-wide; cold re-hosts (card IRD C8) add to it, and kept edges re-pin by whole integers when the chord carry their target is read from leaves (card IRDC section 2.3); the floor slows the convergence; the time link is not the cause. The return's source is traced in card [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md): the chain reads a returning satellite as a first sighting with a known zero step, so its old-frame carries survive and the float vehicle folds them at a birth variance as tight as 7.8 mm against a 2.1 m frame step; pruning them by one knob removes the shock and cuts the end-of-run residual seven-fold, and the stop rule still fails (event-free chords 0.090 cycle). The hold is emission-only; D0 keeps the RPB floor (floor-off and birth-only both fail) and records the GF-datum defect (receiver DCB sd 7 to 10 m); seed-round is not adopted; D-N fires (IRD-5b owed); the lattice and would-be certification rules fail (IRD-5 not built; sigma scale 3, still wrong); the W bars are frozen at 1.25 F, above the null; the nine fixtures land on their tree edges. Nothing from IRD-3 on is built until the cause is closed; whether the identified event cause closes it is the author's ruling.
- **source:** [IRD](IRD_integer_recovering_datum_2026-10-13.md) sections 5 (the event table), 7 (the IRD-1 knobs and owners), 8 (the graders), 11 (the IRD-1 gates) and 13 (the re-base onto `a02ccfb`)
- **knobs / posture:** IRD-1 adds `network.trace_chain_datum`, `network.trace_pb_prehold`, `network.trace_rpb_states` and the consumer's `network.closed_loop.ar_observe`, `.trace_ar_fixes` (TRACE rows), `.ar_search`, `.ar_determined_sigma_cyc` (PARAM rows); extends `network.require_signal_integer_for_ar` in the observe search; every one default off, no golden change. IRD-2 adds `network.phase_datum_eq34_rpb_prior_birth_only` (KEEP_OFF after decision D0) and raises the observatory's certification sigma scale to 3 (the frozen certification rule); no golden change
- **dates in the text:** 2026-10-30, 2026-10-31
- **hashes / run dirs:** branch `claude/distracted-germain-d93ac2` (producer `5302bff`, consumer `f703a63` merged at `e35ee4a`, header refactor `a65e4c7`); base exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird1base.exe` (local main `5127429`, sha256[:16] `d0b4ee505807102b`), IRD-1 exe `gnss_processor_ird1f.exe` (`a65e4c7`, sha256[:16] `1cc358ebd6bd3a98`, md5 `f87b1e7caa3737dfaccab47fe44b1f65`); arms under `D:/GnssProcessorSupport/runs_nom/`: base `_rap_ird1b_{gs,gf,r3c,gd2}`, final `_rap_ird1g_{gs,gf,r3c,gd2,a0_r3c,a0q_r3c}`, the producer-half pre-checks `_rap_ird1p_{gs,r3c}` and `_rap_ird1_{a0,a0q}_r3c`, observe `_rap_ird1_obs_gs`, replays `_rap_ird1_tw_{r1c,r2,r3c}` and `_rap_ird1_gw_{on,off}`, truth wires `_ird1_truthwire_{r2,r3c}`; real-time captures `runs/rtseam_goldens/{ird1base,ird1f}`. IRD-2: graders `2ca527f` and `0658848`, fixtures `7f60555` (merged at `99e27bc`), code `928d0c8`; exes `gnss_processor_ird1f.exe` (section 6's arms, D-S, the fixtures, the attribution arms), `gnss_processor_ird2bo.exe` (birth-only, sha256[:16] `f56dc4e8923e8955`) and `gnss_processor_ird2s3.exe` (sigma scale 3, sha256[:16] `5e3487bd2bdc32fd`); arms `_rap_ird2_*` under the same root (section 3.1); fixture trees `runs_nom/simird_{t1,t2,t3,t4,t5,tr}`. The bisection: scripts `1397a8e`, exe `ird1f`, arms `_rap_ird2_bs_{a0hb,r1c,a0m2,a0q0,a0ma,a0lg,a0xo,a0tl}_mic` (section 4.1); the trace of the stop's event causes continues in card IRDC
- **gates (frozen at pre-registration):** card IRD section 11, IRD-1 and IRD-2

**Outcome.** Question: is the as-built eq34 chain on the float vehicle integer when no event touches it, and what do its events and decisions measure? IRD-1 built the observatory, identical with its knobs off (section 2). IRD-2 FAILS the static-algebra stop rule: on the microscope, chords event-free for 30 min read 0.22 cycle against 0.02, because the double differences under bundle A0 drift off the integers after half an hour on every bed while R1's are exact. The bisection (section 4) puts that cause in the chain's event handling, not its static algebra: with the floor off the chain tracks R1 until the first satellite return. That return's shock is traced in card IRDC: the chain reads the satellite as a first sighting with a known zero step, and its old-frame carries fold at a birth variance the chain makes mm-level; pruning them removes the shock, and the stop rule still fails. The would-be certification and lattice rules FAIL; D0 keeps the floor (GF-datum defect); seed-round is not adopted; D-N fires. Standing: STOPPED; the author's ruling on whether the identified cause closes the stop is owed (section 4.6).

## 1. What this card continues

Card IRD (filed 2026-10-13) asks whether the eq34 chain on the float vehicle can be made integer-recovering through events, with a held-out consumer fixing correct integers. Its file reached 58 KB at IRD-0r, so the results from IRD-1 on are recorded here. Card IRD keeps the binding parts: the derivation (section 3), the as-built defects (section 4), the event table (section 5), the arms (section 6), the knobs and owners (section 7), the graders (section 8), the bars (section 9), the decision rules (section 10) and the increments with their frozen gates (section 11). Its sections 12 and 13 hold IRD-0 and IRD-0r.

<!-- Results are appended below this line under dated headings. -->

## 2. 2026-10-30: IRD-1, the observatory (measure-only)

### 2.1 What landed

**Producer: the frame observatory** (`network/solve/ChainFrame.{hpp,cpp}`, default off).

- **The event table.** `kChainEventSpec` holds the nine rows of card IRD section 5 in its order (Kept, Birth, Returning, RehostCertified, RehostUncertified, RootHandover, TreeEdgeSlip, Merge, Split), each with its legacy rule, its integer rule, the step, carry and wire classes of that rule, the entry of `network.phase_datum_eq34_event_rows` that will arm it and its CHAIN event name. No row is armed: the key `network.phase_datum_eq34_event_rows` belongs to IRD-4 and is not read yet, and the election reads nothing the observatory writes.
- **The record at the election.** With `network.trace_chain_datum` armed, `networkElectChainDatum` records each tree edge with its legacy facts (target, previous target, the new host's carry, pin step, known flag, the legacy cause, the incumbent parent edge read before the wholesale replacement, a flip of the arc's direction, the satellite's remembered root, the pin's sigma scale, the arc's life stamp, the child's depth and frame step from the shift walk), each component (root, members, bridged satellites, declared dimension), the legacy shift and re-reference sets, the keys the coverage gate held out, and every chord's fundamental cycle (`arcgraph::fundamentalBasis`, the arc graph's own owner of cycles) with the sum of the tree pins on its path.
- **The observer** (`networkChainFrameObserve`, a StashTail row of `kNetworkEpochObservers`). Each edge takes the first table row that applies (`chainClassifyEpoch`): TreeEdgeSlip when the free-epoch latch names its arc; Kept when its arc was the child's incumbent parent edge, in either direction; a re-host when the child had another incumbent parent edge, certified when the previous epoch's chord table determines the new arc's integer and the lattice, where it has one, agrees; Merge when the child and its parent lay in different components; Returning or Birth when the child was absent at the last observed epoch (seen earlier, or never); RootHandover when the child was a root there. A kept edge whose satellite remembers another root takes RootHandover, or Split in a piece that split off. The component events (root handover, split, merge) are judged against the memory of the last observed epoch (`chainRememberEpoch`).
- **The chord table** (the previous epoch's frame for the next election). After the solve, every arc carried in both bands of a pair family (band-1 signal with the arc's lowest other band) and a chord of both bands' trees: its two floats in cycles, their 2x2 conditional covariance (`blockConditionalCovariances` in the solve's datum), the two-band certification (`ils::ilsFixDecorrelatedPartial` with the producer search's gates, the covariance scaled by 2 in sigma, determined at 0.05 cycle; `chainCertifyChord`) and the lattice's integer of each band's chord (`latticeImplied` of its fundamental cycle, minus the tree pins on the path).
- **Traces.** `network_chain_edges.csv`, `network_chain_frame.csv` (card IRD section 7's columns), CHAIN rows in `network_events.csv` for both edges of every event (a re-host's left and joined parent edges, a root handover's or split's old and new roots; a returning vertex's row names its previous parent and gap; space-separated `key=value` details led by `sig=`). `network.trace_pb_prehold` writes `network_pb_prehold.csv` (the phase biases sampled immediately before `networkPbHold`, written after it with the emitted value and generation, and the satellite's role in the chain). `network.trace_rpb_states` writes `network_rpb_states.csv` (each receiver phase-bias state with its sigma, pin, rank-floor weight, time-link prior, parent satellite, depth, component, event and shift; the station's record is filled in PASS B's pin rows and read nowhere in the solve).
- **The run end** (`networkChainFrameRunEnd`, a row of `kNetworkRunEndReports`): edges by table row with their legacy unknown and fractional steps and the row's integer-rule classes; re-hosts by certification source, conflicts, wrong-certification candidates (a certified integer that is not the nearest integer of the band's own float); non-integer targets, soft pins and bridges; legacy re-references and fractional known shifts; cut releases and declarations (0 while no row is armed); root handovers, merges, splits, returning, departed and flipped vertices, gated keys; components per group (mean, max); the pin identity per epoch (traced pins = declared, folded = declared minus bridged); the chord table's census; the chain shift's moved values by carry site (the open item of card IRD section 13.5, note 3); the receiver phase-bias time-link NEES.
- **Loader honesty** (log text only). The eq34 load line no longer claims that the emitted reference "stays integer-consistent across all satellites": it states that the static algebra is integer-recovering, that the as-built events step the frame by fractions, and that no integer is claimed. The INERT lines of finding C5 already print only under `carry_invariant` (the guard landed before IRD-1). New warnings: the chain traces without eq34 are inert, and `trace_chain_datum` without `trace_events` discards its CHAIN rows.

**Consumer** (branch `ird1-consumer`, `f703a63`, merged; default off).

- **One AR config builder.** `makeAmbiguityResolverConfig` (`filter/AmbiguityResolverSetup`) fills `AmbiguityResolverConfig` for the post-processing, baseline and real-time paths. The baseline builder dropped `min_arc_info` and the real-time builder dropped `min_arc_info` and `enable_partial_ar` (the defect fix); their existing log lines are unchanged, and one line names the fix when a dropped value is set.
- **Observe mode** (`network.closed_loop.ar_observe`, `.ar_search` full or decorrelated_partial, `.ar_determined_sigma_cyc` 0.05). First in `resolveAmbiguities`, the consumer forms the between-satellite single differences of its own float (`D x / lambda`, `D P D^T / lambda^2`), runs `ils::ilsFix` or `ils::ilsFixDecorrelatedPartial` with the gates built as the producer builds them, conditions the position on the fixed combinations, and keeps one row per difference and per wide lane (`realtime/ArObserve`, pure; the seam in `RealTimeEngineArObserve.cpp`). It writes nothing back.
- **Fix traces** (`network.closed_loop.trace_ar_fixes`): `closed_loop_ar_fixes.csv` and `closed_loop_ar_epoch.csv` in the frozen grader's columns, written by the network run and the replay driver; a run-end line of observe totals.
- **The extended gate and the wire's chain declarations.** `network.require_signal_integer_for_ar` admits a (satellite, signal) to the observe search only when its phase bias is integer-grade, the wire's `amb_datum` is 2 and it carries a `cmp=` component; pairs form within equal components. The replay driver reads `amb_datum` from the S line and `cmp=` from the P records; the in-process run reads the producer's contract.

### 2.2 Declared adaptations

1. **Homes.** Card IRD section 5 names `filter/NetworkChainFrame`; the network producer was reorganized into `network/<concern>/` after the card was written, so the table and the observatory live in `network/solve/ChainFrame` beside the chain datum. The fundamental cycles come from the arc graph's existing `arcgraph::fundamentalBasis`; no new tree or cycle code was written outside `network/ar/ArcGraph`. `cutSide` (section 7, Owners) is not built: nothing at IRD-1 releases a cut.
2. **The memory.** The observatory's memory of the last observed epoch (vertices, components, parents, sightings, the chord table) is instrument state that nothing in the solve reads, so it lives in `ChainFrameCounters` (a base of `NetworkServiceCounters`), not in `NetworkCarryState` or an entity. When IRD-4 lets the election read the chord table, the table moves to the arc entity with a `FieldDecl` row.
3. **The pair family.** The chord table pairs every band-1 signal of an arc with its lowest other band (GPS L1C and L1W with L2W; Galileo E1 with E5a) and keeps an arc only where it is a chord of both bands' signal-family trees, so a re-host's new arc has a row only where both bands made it a chord at the last observed epoch. `lattice_int` is one column for two bands, written `i1/i2` with an empty side outside the lattice's span.
4. **Classification rules.** Card IRD section 5 names the rows; the precedence that assigns an edge to one of them (TreeEdgeSlip, Kept, re-host, Merge, Returning or Birth, RootHandover; a kept edge with a remembered root that differs becomes RootHandover or Split) is this card's (section 2.1), and Returning means seen at an earlier observed epoch and absent at the last one. A wrong-certification candidate is a certified integer that is not the nearest integer of the band's own float (no truth is read in the producer; IRD-2 grades certifications against truth).
5. **CHAIN rows are new rows of a pre-existing CSV.** IRD-1 gate 2's "every pre-existing CSV identical" is read as identical once the CHAIN tier rows are removed from `network_events.csv`. `declared_keys` rows are written only for a declaration, so none at IRD-1: the legacy re-references and shifts are not declarations, and writing them as such would excuse their steps in the frame grader.
6. **`rt_seam_verify.py`** (IRD-1 gate 4) was retired on 2026-10-17 (`CLAUDE.md`, real-time decision tree): eight seam bodies were extended after its cut, so against its frozen pre-cut source it can only fail. It is not run; the `rt_seam_gate.ps1` A/B is run on all four arms, base against IRD-1, with the knobs off.
7. **The retired eq34 lineage.** `golden_field_gps_dual.json` has not changed since 2026-09-13 (its LF blob hashes to `8f7b927d`, the sha256 that `_rap_agt_ref_gd2/rapid.json` records), so the arm is the reference's configuration apart from its output and snapshot paths. It is compared with the base exe of this card (same-build identity) and with the archived reference.
8. **Arm plumbing.** `scripts/ird_make_arms.py` writes its arms and bases under `RAPID_RUNS_ROOT` when set (the `rapid_arm.py` rule), and gains arm A0q, A0 without the IRD traces (the traces-off twin of gate 2). Fixtures are read from the repository's `runs/`.
9. **The consumer half** was built on its own branch (`ird1-consumer`, `f703a63`) and merged. Its declared departures: the observe search waits until the consumer's 3D position sigma is under max(`max_position_sigma_m`, 0.20 m), the bar the loader already puts on a direct per-band search (an unconverged float lets the search run without bound); a gated epoch records its float position and is counted. The observe buffer is cleared by the first admission seam of each epoch (`RealTimeEngine.cpp` was left untouched). The run manifest records `closed_loop_ar_observe` when armed. The extended gate applies in the observe search only (`signal_integer`, the S line's `amb_datum` 2 and a `cmp=` component, pairs within equal components); `RowRules::phaseBiasCoveredForAr`, the resolver's per-ambiguity veto, is unchanged. The `cmp=` token is read into `NetworkPhaseBias::chain_component`; nothing writes it until IRD-7, and `SsrPhaseBiasEntry` is untouched.
10. **Record placement.** The epoch holds the chain record through a pointer to a forward-declared type, and the run tallies (`network/solve/ChainFrameCounters.hpp`) hold the memory the same way: held by value in `network/driver/Epoch.hpp` and `network/state/RunContext.hpp`, which every network translation unit includes, they pushed two test objects over the Debug section limit (C1128). `tests/network/test_network_arc_graph_oracles.cpp`, which sits at that limit, takes `/bigobj`.
11. **Two script fixes, no definition changed.** `scripts/consumer_ar_grade.py` read `<run>/products.txt` for the run's first epoch even with `--wire`, and a replay writes none; it reads the wire then. `scripts/rt_seam_compare.py` gains mask 7, the wall-clock sum of the arc ledger's run-end cost clause (`<N> ms total`), which postdates its mask list.

### 2.3 Gates (card IRD section 11, IRD-1)

| gate | what | result |
| --- | --- | --- |
| 1 | knobs off, `scripts/nom_td_gate.py` (G-TD), base exe against the IRD-1 exe | IDENTICAL on the golden sim 2 h from 00:30:15 (`_rap_ird1b_gs` / `_rap_ird1g_gs`), the golden field 2 h (`gf`), ARL r3c with the search (`r3c`: `config/sim/arl/arl_r3c_producer.json` with card FCF's search knobs; `network_producer_ar.csv` md5 `aa6779cc` on both, card AGTB's hash, so the r3c search reference reproduces at `5127429`) and the retired eq34 lineage `golden_field_gps_dual` 2 h (`gd2`): PASS |
| 1, reading | `gd2` against the archived `_rap_agt_ref_gd2` | not reproduced at `5127429`: products and traces differ from the first epoch at the 1e-4 m level and `network_d5_flags.csv` exists in one run only (twenty days of later cards); the gate is the same-build comparison, as card IRD section 13.4 allows |
| 2 | traces on: A0 on r3c against A0q (A0 without the traces), the IRD-1 exe | `products.txt` identical; 36 pre-existing CSVs identical (`network_datum_graph`, `network_run_summary` and `network_service_health` after their wall-clock columns); `network_events.csv` identical with its 292 CHAIN rows removed; manifest and `rt_engine.log` identical; the run end identical apart from the inserted chain-frame block; new CSVs `network_chain_edges` 43 382 rows, `network_chain_frame` 93 178, `network_pb_prehold` 15 062, `network_rpb_states` 28 800; pin identity (traced = declared, folded = declared minus bridged) in 240 of 240 epochs, pin audit COMPLETE in both groups (21 931 pins each): PASS |
| 3a | observe mode bit-identical to `ar_mode` 0 | unit test `ArObserveEngineTest` (state, covariance and ambiguity map bitwise unchanged); 2 h golden arm `_rap_ird1_obs_gs` (observe, decorrelated search, fix traces) against `_rap_ird1g_gs`: products and every pre-existing CSV identical; the run end and `rt_engine.log` identical once the added observe lines (2, 1 and 2) are dropped; the manifest gains `closed_loop_ar_observe`: PASS |
| 3b | the decorrelated branch matches the producer's ils call on synthetic Q | unit test `ArObserveSearchTest` (same kept set, integers, z counts, ratio, success rate, conditioned floats and covariance): PASS |
| 3c | truth-wire replays at SZZ000SIM (extended gate on, decorrelated search) | r1c (IRD-0's wire), r2 and r3c (wires made with `scripts/simnet_truth_wire.py` from `_rap_arl_r2a` and `_rap_ird1b_r3c`): 0 wrong among 2 506 settled determined integers on each (L1C 822, L2W 822, WL 862; Wilson upper 4.7e-3 per band class): PASS. Ceiling below |
| 3d | replay of `_rf0hv14_sim6`, gate on | 9 386 ambiguity states, 0 eligible, 0 accepted (the golden wire declares `amb_datum` 0): PASS. Gate off: no band integer determined; the wide lane kept on 1 387 of 3 212 settled pairs, 678 wrong of the 906 gradeable (75 %, Wilson upper 0.78; 481 ungradeable at the wire's own fraction) |
| 4 | the full Debug suite | 3 723 tests, 3 722 passed, 1 fixture-gated skip, 0 failed (two shards); `nom_epoch_lint.py`, `counters_split_check.py`, `nom_n4_verify.py --whole`, `docs_index_check.py`, `make_golden_config.py --verify` and `config_posture_check.py --verify-loader` pass: PASS |
| 4 | `rt_seam_verify.py` | not run: retired 2026-10-17 (adaptation 6) |
| 4 | `rt_seam_gate.ps1`, knobs off, base against IRD-1, `rt_seam_compare.py` | sbf, sbf_bssd, sbf_te and cl PASS (every CSV byte-identical; the cl arm's logs equal once mask 7 is applied): PASS |

**The ceiling** (the truth-wire replays, settled pairs of arcs at least 30 min old):

| bed | band determined | WL determined | 3D p50 fixed / float, h0 | h1 | first below 0.10 m, fixed / float | sustained 5 cm, fixed / float |
| --- | --- | --- | --- | --- | --- | --- |
| r1c | 0.954 | 1.000 | 0.017 / 0.085 m | 0.009 / 0.011 m | 1 080 / 1 170 s | 1 560 / 2 640 s |
| r2 | 0.954 | 1.000 | 0.017 / 0.086 m | 0.008 / 0.011 m | 1 080 / 1 170 s | 1 560 / 2 640 s |
| r3c | 0.954 | 1.000 | 0.120 / 0.270 m | 0.039 / 0.032 m | 1 170 / 3 000 s | 4 050 / 3 690 s |

The search opens at the same epoch on every bed (the position-sigma bar is formal) and determines the same pairs; the beds differ in the position. On r3c the conditioned position is worse than the float in hour 1 although no integer is wrong (the float and its covariance disagree there; bar K-c is IRD-8's).

### 2.4 What the observatory shows (A0 on rung r3c, 2 h, 240 epochs)

- **The tree.** 43 382 tree edges (one component per band, about 90 tree edges a band and epoch): 43 052 kept, 182 births, 4 returning, 144 re-hosts, no root handover, merge or split; 20 kept edges flipped direction, and 18 kept edges took an unknown legacy step (finding C7's parent-keyed edges).
- **The re-hosts.** All 144 are uncertified: 16 new arcs had no row in the previous chord table, 128 had one but the two-band fix did not determine the band. The legacy rule stepped 128 of them by a fraction (the new edge's float error, finding 1 of card IRD section 4) and 16 by an unknown step (re-referenced).
- **The frame steps.** 46 (satellite, signal) re-references (D4 erase) and 170 known shifts, 84 of them fractional. The chain shift moved, by carry site: `prev_carry` 170, `pb_level_prev` 170, `ambiguity_value` 4 062, `ambiguity_coupling` 130 860, `rx_phase_bias_value` 292, `rx_code_bias_coupling` 9 262 (the per-site counts card IRD section 13.5 note 3 asked for, under `carry_rebase_complete`).
- **The chords.** 93 178 chord rows; the two-band fix determines a band on 3 807 (4.1 %), the lattice implies one on 616, and the two disagree on 31. The as-built frame keeps the chords off the integers, as IRD-0's census found at the null.
- **The receiver phase biases.** Time-link NEES 2.58 over 27 434 terms: above decision D-N's 2, a reading on the IRD-1 build (D-N is taken on IRD-2's arms).
- **Targets.** No non-integer target, no soft pin, no bridge.

In-process, on the golden itself (`_rap_ird1_obs_gs`, gate off), the observe search kept 51 wide lanes and no band in 2 h; 40 were gradeable, none wrong.

### 2.5 Standing

IRD-1 LANDED on branch `claude/distracted-germain-d93ac2` (not merged). Every gate passes with two declared departures (`rt_seam_verify.py` retired; the retired lineage graded same-build). The card proceeds to IRD-2 (section 6's arms on the four sim beds, read with these traces and graders). Two readings bear on it: the RPB time-link NEES of 2.58 on A0 points at decision D-N firing (IRD-5b owed before D1), and the chord table shows the as-built chain far from integer, which the static-algebra stop rule of card IRD section 10 will test on the microscope.

## 3. 2026-10-31: IRD-2, the as-built chain measured (arms only)

### 3.1 What ran

Every arm runs 2 h (240 epochs; the golden sim 241, from 00:30:15) under `D:/GnssProcessorSupport/runs_nom/`, made by `scripts/ird_make_arms.py` on the pinned base `a02ccfb`; GPS L1C/L2W is graded (the golden sim keeps Galileo, unclaimed).

- **Section 6's arms:** R1, A0, A0h and A0f on the microscope (`mic`: `simarl_r1c` with the zero-ionosphere rows), r2, r3c and the golden sim (`gs`), `_rap_ird2_{r1,a0,a0h,a0f}_{mic,r2,r3c,gs}`, exe `gnss_processor_ird1f.exe` (IRD-1). `_rap_ird2_a0_r3b` is the fixtures' parent.
- **D0's re-measure:** A0b, A0 with `network.phase_datum_eq34_rpb_prior_birth_only` (built here, adaptation 1), `_rap_ird2_a0b_{r3c,gs}`, exe `gnss_processor_ird2bo.exe` (sha256[:16] `f56dc4e8923e8955`); its knob-off twin `_rap_ird2_a0x_r3c`.
- **D-S:** A0s, the D0 winner (A0) with `network.phase_datum_eq34_seed_round`, `_rap_ird2_a0s_{mic,r2,r3c,gs}`, exe `ird1f`.
- **The certification rule's re-measure** at sigma scale 3: `_rap_ird2_a0c3_{r2,r3c}`, exe `gnss_processor_ird2s3.exe` (sha256[:16] `5e3487bd2bdc32fd`).
- **The fixtures:** `config/sim/ird/` (six slip fixtures, each with `_2h` and `_producer`; fixture trees `runs_nom/simird_*`) and three gap beds; A0 on each, `_rap_ird2_a0_{t1,t2,t3,t4,t5,tr,rg,rv6,rv16}`.
- **Attribution** (outside section 6): A0 on the microscope with one event-handling knob off, `_rap_ird2_bxns_mic` (`phase_datum_eq34_carry_shift` false) and `_rap_ird2_bxnr_mic` (`carry_rebase_complete` false), exe `ird1f`.
- **Graders:** `scripts/ird_frame_census.py` (new: `stop`, `cert`, `floor`; selftest 59 checks), `scripts/ird_dd_drift.py` (new: `steps`, `drift`; selftest), `scripts/ird_fixture_gate.py` (new: `verify`, `gate`; selftest), `ird_chord_census.py`, `simnet_wire_integer_consistency.py` (`--source prehold` and `products`), `simnet_producer_ar_grade.py`, `simnet_ar_lattice_grade.py` and the chain-frame run end.

### 3.2 Gates (card IRD section 11, IRD-2)

| gate | result |
| --- | --- |
| pin audit COMPLETE | COMPLETE in every group of every section 6 arm, A0b, the twin, the certification arms and the fixture arms; SHORT in both groups of A0s on r2 (seed-round, not adopted) |
| 0 non-integer targets | 0 on every arm; pin identity held in every epoch |
| every counter printed | the chain-frame run end prints every counter on every arm |
| hold emission-only | A0h's `network_amb_state.csv` and `network_rx_states.csv` (and its chain edges and chord table) byte-identical to A0's on all four beds: PASS |
| static-algebra stop | microscope, chords whose tree path saw no event for 30 min (22 908 L1C and 22 744 L2W chord-epochs): p50 \|frac\| 0.223 and 0.217 cycle against 0.02 (A0f 0.210 and 0.215): FAIL. The card stops (section 3.8) |
| D0 | neither switch passes; the floor stays and the GF-datum defect is recorded (section 3.5) |
| D-S | seed-round not adopted (section 3.5) |
| D-N | fires: A0's RPB NEES 2.31 on r2 and 2.58 on r3c; IRD-5b owed before D1 |
| lattice under eq34 | dimension E - V + C in 230 of 230 builds per band on r3c (lattice dimension p50 397), but 3 695 wrong producer fixes: FAIL. IRD-5 is not built; certification is pair-only |
| would-be certification on r2 and r3c | wrong against truth at sigma scale 2 and again at 3: FAIL (section 3.4). The scale is 3 by the rule; the defect is owed before IRD-4 |
| W bars | every bar MISSED, each frozen at 1.25 F (section 3.6) |
| fixtures on their tree edges | nine of nine: PASS (section 3.7) |

### 3.3 The arms by bed

Held-out float consumer (SZZ000SIM) 3D p50 in the run's first and second hours; receiver DCB sd over 60 stations at the end of each hour; RPB time-link NEES; the producer search over the run against truth (DD float \|e\| p50 in cycles; kept fixes and the wrong ones). No arm in this table withholds an epoch (W5).

| bed | arm | consumer h1 / h2 (m) | DCB sd h1 / h2 (m) | NEES | DD \|e\| p50 | fixes kept / wrong |
| --- | --- | --- | --- | --- | --- | --- |
| mic | R1 | 0.121 / 0.043 | 0.492 / 0.494 | | 0.008 | 169 595 / 0 |
| mic | A0 | 0.149 / 0.024 | 1.084 / 1.132 | 1.35 | 0.190 | 115 014 / 6 056 |
| mic | A0h | 1.723 / 3.939 | as A0 | 1.35 | as A0 | as A0 |
| mic | A0f | 0.133 / 0.036 | 0.607 / 0.629 | 1.40 | 0.151 | 121 819 / 5 594 |
| r2 | R1 | 0.094 / 0.011 | 0.018 / 0.014 | | 0.269 | 57 920 / 0 |
| r2 | A0 | 0.224 / 0.034 | 9.099 / 10.105 | 2.31 | 5.611 | 5 190 / 3 629 |
| r2 | A0h | 21.39 / 49.40 | as A0 | 2.31 | as A0 | as A0 |
| r2 | A0f | 0.088 / 0.011 | 0.671 / 3.200 | 1.45 | 1.034 | 15 988 / 2 040 |
| r3c | R1 | 0.289 / 0.032 | 0.024 / 0.014 | | 0.280 | 56 626 / 0 |
| r3c | A0 | 0.417 / 0.042 | 9.454 / 10.135 | 2.58 | 5.561 | 5 056 / 3 695 |
| r3c | A0h | 26.04 / 59.33 | as A0 | 2.58 | as A0 | as A0 |
| r3c | A0f | 0.280 / 0.027 | 0.683 / 3.088 | 1.62 | 1.070 | 15 808 / 2 075 |
| r3c | A0b | 0.369 / 0.042 | 6.740 / 8.265 | 1.51 | 4.734 | 5 894 / 3 821 |
| gs | R1 | 0.099 / 0.033 | 0.073 / 0.054 | | 0.263 | 140 418 / 0 |
| gs | A0 | 0.071 / 0.029 | 7.108 / 9.745 | 1.11 | 3.268 | 17 258 / 12 552 |
| gs | A0h | 3.008 / 1.371 | as A0 | 1.11 | as A0 | as A0 |
| gs | A0f | 0.093 / 0.034 | 0.587 / 4.241 | 1.01 | 0.809 | 50 590 / 9 597 |
| gs | A0b | 0.108 / 0.044 | 5.366 / 8.395 | 1.05 | 2.347 | 20 508 / 13 601 |

Chords and wire in the second hour (A0, R1 in brackets; the size-matched null is 0.250): the chord wide-lane p50, its per-arc circular R over 30 min, the carried wide lane's per-epoch jump (p99 and the share above 0.5 cycle), and the pre-hold wire's E1, E2, WL and NL p50.

| bed | chord WL p50 | circular R p50 | WL jump p99, share > 0.5 | wire E1 / E2 / WL / NL |
| --- | --- | --- | --- | --- |
| mic | 0.193 (0.276) | 0.680 (0.991) | 10.2, 2.9 % (0.15, 0.1 %) | 0.248 / 0.255 / 0.252 / 0.246 |
| r2 | 0.251 (0.257) | 0.385 (0.952) | 12.6, 4.9 % (0.26, 0.4 %) | 0.248 / 0.250 / 0.241 / 0.253 |
| r3c | 0.254 (0.259) | 0.363 (0.948) | 15.3, 4.9 % (0.27, 0.4 %) | 0.248 / 0.252 / 0.252 / 0.249 |
| gs | 0.247 (0.266) | 0.480 (0.959) | 16.8, 5.4 % (0.27, 0.3 %) | 0.250 / 0.249 / 0.248 / 0.252 |

A0h's emitted wire grades exactly as A0's pre-hold wire on every bed: with the holds off the emitted biases are the pre-hold biases. A0's emitted wire (holds on) takes no step at any event epoch (the hold freezes the biases) and stays at the null in value (r3c, second hour: 0.252 / 0.254 / 0.251 / 0.246). With the holds off the float consumer diverges to metres (A0h above): on this chain the holds are what keep the wire consumable.

### 3.4 Events, certifications and chords

Events by table row (A0, both bands, 2 h). The three ARL beds share one geometry and one tree history: 43 052 kept, 182 births, 4 returning, 144 re-hosts, no root handover, tree-edge slip, merge or split. The golden sim (GPS and Galileo): 83 170 kept, 350 births, 4 returning, 242 re-hosts, 2 tree-edge slips. On r2 and r3c the legacy rule steps 128 of the 144 uncertified re-hosts by a fraction and 16 by an unknown step; the re-host targets step by integers from -232 to +195 against the incumbent frame.

Would-be certifications against truth (`ird_frame_census.py cert`, GPS, the previous epoch's chord table):

| bed | determined chord integers | wrong | re-hosts certified | certified re-hosts wrong |
| --- | --- | --- | --- | --- |
| mic (reading) | 109 404 (L1C 48 144, L2W 61 260 of 93 178 rows) | 54 675 | 63 of 144 | 34 of 63 |
| r2 | 7 282 (3 641 rows, 3.9 %) | 6 965 | 0 of 144 | |
| r3c | 7 614 (3 807 rows, 4.1 %) | 7 400 | 0 of 144 | |
| gs (reading) | 16 792 (8 396 of 93 553 rows) | 16 163 | 16 of 242 (12 GPS) | 12 of 12 GPS |
| r2, scale 3 | 1 516 (758 rows) | 1 286 | 0 | |
| r3c, scale 3 | 1 662 (831 rows) | 1 526 | 0 | |

The chord floats agree with the producer's double differences through the integer targets (11 384 to 11 840 four-cycle chords matched per bed, largest gap 8e-5 cycle), so the certifications fail on the floats, not on the frame algebra. Settled chords (present 60 min or more) sit p50 0.86 and 0.82 cycle (L1C, L2W) from their frame integers on the microscope, 26.7 and 27.1 on r2 and 22.9 and 23.9 on r3c. Under eq34 the producer's cycle basis keeps its full dimension (rows plus fallbacks equal E - V + C in every build), and its lattice ledger on r3c holds 3 577 wrong integers and adds 96 wrong rows (R1: 0 and 0).

### 3.5 Decisions D0, D-S and D-N

**D0** (bars on r3c and the golden sim: DCB sd at most 0.17 m, three times the golden's; W5 no higher than A0's; float consumer within +10 % of A0's in hours 1 and 2):

| switch | DCB sd, r3c / golden (h1, h2) | W5 | consumer against A0, r3c / golden (h1, h2) | verdict |
| --- | --- | --- | --- | --- |
| A0f, floor off | 0.68, 3.09 / 0.59, 4.24 m | 0 | -33 %, -36 % / +30 %, +17 % | FAIL (DCB; golden consumer) |
| A0b, birth only | 6.74, 8.27 / 5.37, 8.40 m | 0 | -11 %, -2 % / +52 %, +54 % | FAIL (DCB; golden consumer) |

The floor stays (A0 is the D0 winner) and the GF-datum defect is recorded: under the chain the receivers' geometry-free code bias spreads 7 to 10 m by hour 2, with the floor on or off (the golden: 0.05 m). It blocks the ionosphere follow-up (card IRD section 10).

**D-S** (A0s against A0): the unit test the rule names, `test_eq34_seed_round`, does not exist (`docs/SPEC_eq34_seed_round.md` records it as owed): FAIL. Seed target steps after birth: 0 on every bed (204 to 370 targets seed-rounded): pass. Chord census: the carried wide lane jumps above 0.5 cycle on 19 to 22 % of arc-epochs (A0: 4.9 to 5.4 %) and the receiver DCB sd reaches 15 to 29 m: worse, FAIL. Hour-1 float p50 0.107, 0.083, 0.280 and 0.106 m on the microscope, r2, r3c and the golden sim (A0: 0.149, 0.224, 0.417, 0.071), +49 % on the golden sim: FAIL. Beyond the clauses: hour 2 reaches 1.155, 0.987 and 0.974 m on r2, r3c and the golden sim, the microscope withholds 123 of 240 epochs on sigma0, the RPB NEES reaches 299 to 623 (0.015 on the microscope) and r2's pin audit is SHORT. Seed-round is not adopted.

**D-N:** A0's RPB NEES on the microscope, r2, r3c and the golden sim is 1.35, 2.31, 2.58 and 1.11: above 2 on r2 and r3c, so IRD-5b is owed before the D1 verdict.

### 3.6 The bars frozen by the calibration rule

The floor F is read on the D0 winner (A0) over pairs whose tree paths saw no event for 30 min (`ird_frame_census.py floor`, the pre-hold wire; slip, handover, split and merge rows end a run). W-micro is read over the microscope's settled hour (01:30:30 to 02:30:30), the others over the 2 h run; n is at least 17 818 in every cell.

| bar | B | F | frozen bar (1.25 F) |
| --- | --- | --- | --- |
| W-micro E1 p50 / p99 (mic) | 0.02 / 0.10 | 0.2445 / 0.4952 | 0.306 / 0.619 |
| W-micro E2 p50 / p99 | 0.02 / 0.10 | 0.2592 / 0.4949 | 0.324 / 0.619 |
| W-micro WL p50 / p99 | 0.02 / 0.10 | 0.2561 / 0.4957 | 0.320 / 0.620 |
| W-WL p50, r2 / r3c / gs | 0.08 | 0.2431 / 0.2464 / 0.2468 | 0.304 / 0.308 / 0.309 |
| W-WL, 90 % within 0.20 (as p90) | 0.20 | 0.4450 / 0.4486 / 0.4512 | 0.556 / 0.561 / 0.564 |
| W-NL p50 | 0.10 | 0.2564 / 0.2461 / 0.2509 | 0.321 / 0.308 / 0.314 |
| W-NL, 85 % within 0.25 (as p85) | 0.25 | 0.4291 / 0.4242 / 0.4254 | 0.536 / 0.530 / 0.532 |

Every B is MISSED (F > 0.8 B) and every bar is frozen at 1.25 F, as the rule says. The floors are the uniform null: the event-free wire of the as-built chain carries no integer information, and the frozen bars lie above what a uniform wire reads, so they would pass noise. They are recorded as the rule fixes them and bind only once the stop is closed; revisiting them is the author's ruling, since the rule says never revised.

### 3.7 The fixtures

The fixtures are built from A0's chain trace on rung 3b (`_rap_ird2_a0_r3b`), where the L1C and L2W trees agree in all 240 epochs, r1 is S13000SIM throughout and no tree event falls in epochs 150 to 165. The slips sit at epoch 158 (TOW 265 740; t5 at 175, TOW 266 250) on arcs that are tree edges in the slipped bands from 10 min before to 20 min after, none at r1 except tr's and none on G01 (the DD trace's reference). `scripts/ird_fixture_gate.py verify` checks each fixture tree against rung 3b and `gate` checks each A0 arm.

| fixture | the change | the edge at k - 1 and k | below the child | at the event epoch | verdict |
| --- | --- | --- | --- | --- | --- |
| t1 | S10000SIM G09 L1C +1 | G09 under S10000SIM, depth 3 | 3 satellites, 3 stations | `slip_deseat` in each band, row TreeEdgeSlip | PASS |
| t2 | S03000SIM G25 L1C +4, L2W +5 (MW-blind) | G25 under S03000SIM, depth 3 | 3, 11 | `slip_deseat`; no `T0_mw_detect` row | PASS |
| t3 | S23000SIM G02 L2W +1 with LLI | G02 under S23000SIM, depth 3 | 2, 6 | `slip_deseat`, L2W with the LLI cause bit | PASS |
| t4 | S45000SIM G32 L1C +2, L2W +2 | S45000SIM under G32 (a child of r1), depth 2 | 3, 7 | `slip_deseat` | PASS |
| t5 | t2's shape and arc at settled TOW 266 250 | as t2 | 3, 11 | `slip_deseat` | PASS |
| tr | S13000SIM G03 L1C +1, r1's own edge | G03 under r1, depth 1 | 5, 15 | `slip_deseat` | PASS |
| rg | r1 blanked 10 min (epochs 152 to 171) | the root, 8 child satellites at 151 | | 4 `root_handover` rows (to S30000SIM), 36 re-hosts by handover | PASS |
| rv6 | S45000SIM blanked 6 min (152 to 163) | under G32, depth 2, children G15 and G24 | | 12 carry re-hosts; the return at 164 is a TreeEdgeSlip, not Returning | PASS |
| rv16 | the same, 16 min (152 to 183) | as rv6 | | 12 carry re-hosts; the return at 184 writes 2 `returning` rows (gap 990 s) | PASS |

Products equal A0 r3b's up to each event epoch and differ from it on. Each slip fixture differs from rung 3b only in the slipped station's phase fields from the slip epoch on (t3's LLI digit at 158 only), its `truth_slips.csv` and the scenario summary; the gap beds blank exactly 200, 92 and 237 GPS records. On these fixtures Returning is reached by rv16 only and RootHandover by rg only; r1 comes back from its gap as a leaf at depth 4, not as the root.

### 3.8 Attribution of the stop

The frozen rule makes the stop binding: nothing from IRD-3 on is built until its cause is closed. The census attributes it as follows.

1. **The frame is integer.** 0 non-integer targets, pin identity in every epoch, pin audit COMPLETE, and the chord floats equal the producer's double differences through the integer targets (section 3.4).
2. **The double differences are wrong, and no datum choice moves a double difference.** The producer's DD floats against truth over the run: A0 on the microscope p50 0.190 cycle with 6 056 wrong fixes, where R1 on the same bed reads 0.008 with 0 wrong; r2 5.61, r3c 5.56 and the golden sim 3.27 cycles, where R1 reads 0.27, 0.28 and 0.26 with 0 wrong on every bed.
3. **They drift; the events do not step them.** On the microscope (`ird_dd_drift.py drift`; wrong is \|e\| > 0.25 cycle) the wrong share by 10 min bin is, for R1, 0.39, 0.01 and then 0 from 20 min; for A0 0.60, 0.30 and 0.20 at 20 to 30 min, then 0.29, 0.39, 0.39, 0.39, 0.44, 0.51, 0.55, 0.60 and 0.62; A0f takes the same course (0.37, 0.05, 0.07, 0.30, then up to 0.63). Of A0's 157 good-to-wrong transitions, 117 coincide with no event, 24 with a RAIM T4 prior drop, 11 with an arc birth and 5 with a re-host. At the 48 chain-event epochs the DD steps of keys whose arcs had no event of their own are no larger than at quiet epochs (L1C p50 0.0062 and p99 0.22 cycle, against 0.0057 and 0.30; `ird_dd_drift.py steps`), but A0's quiet steps are three times R1's (p50 0.0018, p99 0.13).
4. **The QC reacts to the drift.** On the microscope A0 raises 554 RAIM T4 attributions with 1 108 carried priors dropped (R1: none), 1 531 block-Baarda outliers (R1: 267) and 2 526 prior-residual events (R1: 1 741).
5. **Neither event-handling knob is the cause.** With `phase_datum_eq34_carry_shift` off the chain takes the legacy re-reference (196 re-references, 2 800 carries erased, none S-transformed) and the DDs go 84 % wrong (\|e\| p50 1.76 cycle), with 33 epochs withheld and RPB NEES 35.6. With `carry_rebase_complete` off the microscope run is byte-identical to A0 (`products.txt` and every carry).
6. **The RPB floor is not the cause on the microscope:** A0f drifts as A0 does (wrong 0.37 of all rows against 0.43; \|e\| p50 0.151 against 0.190).

Reading: the static chords are off the integers because the float solution under bundle A0 drifts away from integer double differences after its first half hour, on the microscope as on every other bed. The chain's frame and its event steps are not the cause. The candidates are the bundle's remaining differences from R1: the prioritized and held machinery off (`phase_datum_prioritized`, `phase_datum_held_basis`, `phase_datum_held_mark_cold`, `phase_datum_held_cold_release`, `phase_datum_handover_continuous`, `phase_anchor_carry_reref`, `phase_datum_gate_drop_erase`, `phase_datum_min_stations` 1) and the chain's hard pins, re-applied every epoch on a moving tree, with the RPB time link. The cause is closed by a one-change bisection on the microscope graded with `ird_dd_drift.py drift`, with R1, whose double differences are exact from 20 min, as the control.

### 3.9 Declared adaptations

1. **`network.phase_datum_eq34_rpb_prior_birth_only`** (card IRD section 7: "only if D0 needs it") is built here, default off, and KEEP_OFF in the ledger after its D0 result. The floor row is skipped while the bias is time-linked (a carry inside the leveling horizon with q > 0). Off, its exe is identical to the IRD-1 exe on A0 r3c (`_rap_ird2_a0x_r3c`: `products.txt` and every trace; `network_datum_graph`, `network_service_health` and `network_run_summary` after their wall-clock columns).
2. **Sigma scale 3** is a constant of the observatory (`kChainCertSigmaScale`, 2.0 to 3.0): card IRD section 7's `_sigma_scale` rider belongs to IRD-4's `rehost_cert` knob, which is not built. The rebuilt exe's A0 arms on r2 and r3c are byte-identical to the IRD-1 exe's in `products.txt`, the carries, the receiver states and the chain edges; only the chord table's integers differ.
3. **Hours.** The hours 1 and 2 of D0 and D-S are the first and second hours of each 2 h arm (the golden sim from 00:30:15). The DCB spread is `ird_chord_census.py`'s receiver DCB sd at the end of each hour.
4. **Floors.** W-micro's floor is read over the microscope's settled hour, the bar's own window. The share clauses are calibrated in their quantile form (p90, p85), the grader's printed extension, since the rule is stated for magnitudes.
5. **Fixture generation** (by an agent, with the box at full load): each slip fixture is rung 3b with the slipped station regenerated by `gnss_simulator synth-network` on a 3-station roster. Regenerating all 61 stations of `arl_r3b_2h.json` with the same simulator reproduces `runs/simarl_r3b` (854 of 854 files byte-identical). The simulator copy has its baked project root patched to `C:/GnssProcessor` (both sha256 values in each fixture's `GENERATED.txt`). The three gap fixtures are RINEX strips of rung 3b (beds of `ird_make_arms.py` that pass a windowed strip to `rapid_arm.py make --rinex`), not `_2h` and `_producer` files; their arms set `processing.max_duration_s` and `network.max_duration_s` to 7200 instead of `--hours 2`, which counts from 00:00 and would cut the rewritten files at epoch 186.
6. **Attribution arms and `scripts/ird_dd_drift.py`** are outside section 6 and change nothing in it.

### 3.10 Standing

IRD-2 RAN on branch `claude/distracted-germain-d93ac2` (not merged), and the card STOPS by its static-algebra stop rule (section 3.8): nothing from IRD-3 on is built until the cause is closed. Owed, in order: the cause, by a one-change bisection of bundle A0 on the microscope against R1, graded on the double differences; then, before the card resumes, the certification defect (wrong at sigma scale 3 on r2 and r3c, owed before IRD-4), IRD-5b (decision D-N, before D1) and the GF-datum defect (decision D0), which blocks the ionosphere follow-up. Also fixed by the rules: IRD-5 is not built and certification is pair-only, seed-round is not adopted, and the W bars are frozen at 1.25 F, above the null.

## 4. 2026-10-31: the bisection of the stop

### 4.1 What ran

One-change arms of bundle A0 on the microscope, 2 h each, exe `gnss_processor_ird1f.exe` (sha256[:16] `1cc358ebd6bd3a98`, the exe of section 3's microscope arms), under `D:/GnssProcessorSupport/runs_nom/`, made by `scripts/ird_make_arms.py` on the pinned base `a02ccfb`. Bundle A0 differs from R1 by the chain (`phase_datum_eq34`, `phase_datum_eq34_carry_shift`, `carry_rebase_complete`) and by the held machinery off (seven booleans and `phase_datum_min_stations` 1); its float re-pin keys are the loader defaults. Graders: `ird_dd_drift.py drift` and its new `reffree` mode (commit `1397a8e`: each epoch's DD errors less an additive fit a(sta_r) + b(sat_j) per band, which removes the reference arc's error, since that error enters every key), the chain-edge trace and the per-epoch traces.

| arm | run | the change |
| --- | --- | --- |
| A0hb | `_rap_ird2_bs_a0hb_mic` | A0 with the held machinery as the golden arms it, `phase_datum_min_stations` kept at 1 |
| R1C | `_rap_ird2_bs_r1c_mic` | R1 plus the chain (A0 less every held knob) |
| A0m2 | `_rap_ird2_bs_a0m2_mic` | A0, `phase_datum_min_stations` 2 |
| A0q0 | `_rap_ird2_bs_a0q0_mic` | A0, `phase_datum_eq34_rpb_q_m2s` 0 (no receiver phase-bias time link) |
| A0ma | `_rap_ird2_bs_a0ma_mic` | A0, `eq34_anchor_min_arc_ep` 10 (INC-1: a new tree edge grows on an arc at least 10 epochs old where one exists) |
| A0lg | `_rap_ird2_bs_a0lg_mic` | A0, `phase_datum_eq34_longevity_election` |
| A0xo | `_rap_ird2_bs_a0xo_mic` | A0, `phase_datum_eq34_extend_only` |
| A0tl | `_rap_ird2_bs_a0tl_mic` | A0, `phase_datum_time_linked` (INC-2, without INC-1) |

Section 3's A0f (RPB floor off) and `_rap_ird2_bxns_mic` (carry shift off) are graded with them.

### 4.2 Bundle A0 is R1 plus the chain

A0hb, R1C and A0m2 are byte-identical to A0 in `products.txt`, `network_producer_ar.csv`, `network_amb_state.csv` and `network_rx_states.csv`. The chain-edge traces of A0hb and R1C differ from A0's in `life_stamp` only (784 rows: the flag-epoch stamp the held basis writes). Under the chain the loader disables the continuous handover and the carry re-reference, the gate-drop erase rides the re-reference, and the held basis acts in the prioritized election, which the chain election replaces; on this bed no column has fewer than two trackers, so the coverage gate excludes nothing. On the microscope A0 is therefore R1 plus the chain, and the cause lies in the chain. A0xo is byte-identical to A0 too (no tree edge outlives a row gap on this bed).

### 4.3 The chain's own switches

Double differences against truth by `ird_dd_drift.py reffree` (the residual after the epoch's additive fit; bin 1 is epochs 31 to 50, before the first satellite return, and the last bin epochs 231 to 240), with the chain-edge census of the run (GPS, both bands): re-host rows onto a first-sighting arc by an unknown step (`rehost_cold`), and kept edges re-pinned by a nonzero step.

| arm | cold re-hosts | kept re-pins | residual p50 / share > 0.25, bin 1 | the same, last bin | raw wrong, last bin |
| --- | --- | --- | --- | --- | --- |
| R1 | | | 0.021 / 0.01 | 0.001 / 0.00 | 0.00 |
| A0 | 34 | 87 | 0.065 / 0.18 | 0.243 / 0.49 | 0.62 |
| A0f (section 3) | 34 | 87 | 0.030 / 0.02 | 0.257 / 0.51 | 0.63 |
| A0q0 | 34 | 87 | 0.051 / 0.14 | 0.295 / 0.53 | 0.68 |
| A0lg | 24 | 103 | 0.071 / 0.22 | 0.062 / 0.21 | 0.26 |
| A0ma | 54 | 285 | 0.452 / 0.62 | 0.156 / 0.41 | 0.50 |
| A0tl | 62 | 793 | 1.497 / 0.77 | 2.019 / 0.78 | 0.88 |
| carry shift off (section 3) | 42 | 84 | 0.121 / 0.32 | 2.780 / 0.79 (its last bin, epochs 211 to 230) | 0.94 |

The time link is not the cause: A0q0 drifts as A0 does. The longevity election, with fewer cold re-hosts, ends the run with a quarter of A0's residual; INC-1 and INC-2, with more cold re-hosts and re-pins, are far worse in the first hour. The counts do not order the arms exactly (A0ma ends below A0).

### 4.4 The record

- **Before the first satellite return the chain tracks R1.** With the floor off (A0f), at TOW 262 890 (epoch 63) the residual reads 0.0225 cycle with 0.9 % above 0.25 (R1: 0.0200 and 0.3 %), after 44 re-host rows (16 onto first-sighting arcs, all in the first 10 min, while the carries were young) and 12 kept re-pins. The chords, every chord row with no event filter, converge with the double differences: p50 \|frac\| 0.173 and 0.137 cycle (L1C, L2W) in epochs 20 to 30, 0.064 and 0.049 in 40 to 50, and 0.052 and 0.040 in 55 to 63.
- **The first satellite return breaks it.** G20 returns at TOW 262 920 (epoch 64) after a 720 s gap at every station. The chain re-attaches it under a new host (S06000SIM before the gap, S04000SIM after) as a Returning row at the first-sighting target 0. That epoch, the other satellites' clocks step 2.7 cm p50 (R1: 0.11 cm), the receiver code biases 1.0 cm p50 and 8.7 cm at most (0.09 cm the epoch before; R1: 0.25 cm), 512 carried arc priors of other satellites misfit across the network (up to 249 sigma, by the same metres on L1C and L2W) and RAIM refloats 8 arcs. On the same data R1 writes 16 prior-residual rows and no RAIM row. The residual's share above 0.25 goes from 0.009 to 0.065 (R1: 0.000) and grows from there, with steps at the next cold re-hosts (epochs 70 and 181), to 0.51 at the end. The arcs re-hosted cold after convergence sit at epochs 70, 88, 106, 139, 181, 187 and 210.
- **With the floor on, A0 converges more slowly before any of this.** At epoch 63 A0's residual reads 0.0456 with 9.4 % above 0.25; in bin 1, 0.065 against A0f's 0.030 and R1's 0.021. The RPB floor that D0 kept slows the convergence; from the first return A0 degrades as A0f does.
- **A cold re-host and a flipped kept edge (A0, S45000SIM, epochs 88 and 89).** At epoch 88 the tree re-hosts S45000SIM from under G24 to under G32 through a first-sighting arc by an unknown step (`rehost_cold`, known 0). The chain re-realizes G13 and G24, and S45000SIM's carried chords G05, G15, G18 and G23 stay in the old frame (card IRD C8). The G05 carry moves 1.17 m in one epoch against a 5.4 mm fold sigma, and RAIM drops the four arcs over epochs 88 to 90. At epoch 89 the kept edge S45000SIM-G24, flipped by the re-host, is re-pinned from 110 cycles to 0 (pinmove -20.93 m, card IRD C7). In the last 30 min this receiver's DDs sit 9 to 12 cycles off (median).

### 4.5 Reading

1. **The stop has an event cause.** The chain's frame is integer (section 3.2); with the floor off, its double differences track R1's until the first satellite return and break there, and its chords converge with them. The stop fired on damage from the chain's event handling (the satellite return, a cold re-host after convergence, a flipped kept edge: card IRD C7 and C8), not on its static algebra.
2. **An event's damage is network-wide.** It reaches every station's carried chords through the shared satellite clocks and biases, so the stop rule's 30 min window on a chord's own tree path does not isolate the static algebra on the as-built chain. Whether event-free chords reach 0.02 is not measured: the first return comes 32 min into the run, where the rule's 30 min window has almost no room.
3. **The RPB floor slows the convergence** (A0 against A0f). D0 kept it on its consumer and DCB bars; its cost to the chords is recorded here.
4. **Not the cause:** the held machinery and the coverage gate (inert under the chain on this bed), extend-only (inert), the time link, the carry shift (off is worse) and the complete rebase (inert, section 3.8).
5. **Correction of section 3.8.** Item 3 ("they drift; the events do not step them") and the reading's "the chain's frame and its event steps are not the cause" are superseded: the transition census read each cause at the transition epoch on the key's own arcs, while the damage starts at an event and reaches other keys over later epochs. Items 1, 2, 4, 5 and 6 stand.
6. **Open:** the path from the chain's re-admission of a satellite to the network-wide clock step is not traced to a line of code. R1's held-basis election re-admits the same satellite without it.

### 4.6 Standing

The cause of the stop is identified: the as-built event handling that IRD-3 onward is to replace, not the static algebra. The frozen rule says nothing from IRD-3 on is built until the cause is closed; whether an identified event cause closes a static-algebra stop is the author's ruling. Owed with that ruling: the satellite-return path (a test of card IRD C7 for a satellite vertex), and from section 3.10 the certification defect, IRD-5b (decision D-N) and the GF-datum defect (decision D0).
