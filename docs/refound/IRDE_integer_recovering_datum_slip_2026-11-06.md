# IRDE: the integer-recovering datum, continued (IRD-4's slip and merge and split rows; the floor's wet-delay mechanism)

**STATUS**

- **id:** IRDE
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRDD's continuation (that file nears the 60 KB bar), from IRD-4's `slip` row on; card IRD keeps the binding parts and cards IRDB, IRDC and IRDD the earlier results. The `slip` row LANDED default off (entry `slip` of `network.phase_datum_eq34_event_rows`). An arc in the epoch's free-epoch latch leaves the tree at the slip epoch, on both bands of a pair family. The old child re-attaches through a new edge, which the rehost row releases under the hold; a slipped arc no other route can replace stays and the slip row releases it. As built the slipped edge stays pinned: on ird_t1 its target falls to 0 (a known step of +57 and -91 cycles on the subtree), and on every fixture the cycle stays in the arc's phase row (on the microscope twins a block-Baarda outlier with row t up to 697 for the rest of the run, 0 with the row). Off identity passes on six pairs, and the row is inert in every product and trace on three beds with no tree-edge slip. The class's as-built steps fall to 0, both edges carry identities, and the float consumer and W5 pass (hour 2 on ird_t1 0.117 m against 0.122, on ird_tr 0.118 against 0.121; 240 of 240 epochs emitted). I-b FAILED: undeclared integer steps at the slip epoch 508 to 224 on ird_t1 and 213 to 146 on ird_tr, 331 and 6 to 0 on the microscope twins, 324 and 0 to 0 with the floor off; the fractional bar is missed on every graded bed. On ird_t1 the integer steps left are band noise and a clock-anchor decoupling that the slip-free control has too. On ird_tr three undeclared satellites also step, non-dispersively, through receivers whose wet delay this posture holds metres off (S45000SIM at 8.4 m, truth 0.13 m). With the floor off, ird_tr passes I-b outright (wide lane max 0.026 cycle, 0 integer steps) and ird_t1 reads 0 integer steps against its slip-free control. The release costs what the rehost row's costs: more double differences above 0.25 cycle for about 20 min (floor-off 0.112 to 0.187 at the slip epoch on the larger moved side). The consumer clause is a weak test here: the phase-bias hold keeps the emitted bias of a moved key unchanged, so on the microscope beds the consumer differs from the parent by 1e-4 m. The `merge_split` row LANDED default off (section 8), on a cut fixture built for it: a split piece's new root is pinned at its carried bias and its edges held, so the frame does not step (0 integer pairs against a control on `ird_ms`, 304 cycles to 24.8 on `ird_msr`), and a merge is a declared cut release of the side the votes absorb. Off identity passes on seven pairs, two of which reach the class; the row is inert on five beds; identities, the consumer and W5 pass. I-b FAILED on the fractional bar on every graded bed and on integer steps on `ird_ms` (47, per-band, 0 against the control) and `ird_msr` (733, and 297 at the merge). Section 9 answers a question relayed at the author's word: the receiver floor causes A1's wet-delay state (first-epoch shift rms 3.59, 0.73, 0.09 m at sigma 1, 3, 10 against 3.52, 0.62, 0.06 predicted from a floor-off arm with nothing fitted), the DCB spread is largely its geometry-free half (slope -1, correlation -0.99 to -1.00 at sigma 10 and 100), the undeclared satellite steps are wet-delay differences (correlation 0.946), and `ird_tr` passes I-b at sigma 3 m, 10 m or off. Neither golden reaches it; an archived field arm with the chain on reads up to 29 m. D0 and D-N are reported to the author and nothing is changed.
- **source:** [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 4 (item 4), section 5 (the `TreeEdgeSlip`, `Merge` and `Split` rows), section 6 (the fixtures) and section 11 (IRD-4's order and gate); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 7.3 (the hold on certified re-hosts); [IRDD](IRDD_integer_recovering_datum_rows_2026-11-03.md) sections 3 to 5 (the earlier rows, whose gate form this card follows); [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md) section 3.7 (the fixtures' tree edges)
- **knobs / posture:** the `slip` entry of `network.phase_datum_eq34_event_rows` (default empty; ledger row UNVALIDATED, its text updated); arms A1krhRs, A0fkrhRs and A0fkrRs of `scripts/ird_make_arms.py`; beds `ird_t1m` and `ird_trm` (rung 1c with zero-ionosphere rows and the scripted slip of t1 and tr; configs `config/sim/ird/ird_{t1m,trm}_2h.json`). The `merge_split` entry, arms A1krhRsm, A0fkrhRsm and A0fkrRsm, beds `ird_ms`, `ird_msr`, `ird_msx`, their controls and microscope twins (`scripts/ird_make_arms.py`, `scripts/ird_fixture_gate.py`). Section 9's arms set one existing knob each. No golden change
- **dates in the text:** 2026-11-06, 2026-11-07
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird4slip.exe` (sha256[:16] `e105882041a409da`: `104b103` plus the row); arms `_rap_ird4s_*`, and the parent on the root row's exe `_rap_ird4s0_a1krhR_{t1,tr}`; fixtures `simird_{t1m,trm}` (each with a `GENERATED.txt`; working files in `_ird2_gen/mini_{t1m,trm}*`). Section 8: exe `gnss_processor_ird4ms.exe` (sha256[:16] `4547066185234026`: `288a80d`), arms `_rap_ird4ms_*`, and the slip row's exe on the new beds `_rap_ird4m0_*`. Section 9: the same exe, arms `_rap_ird4z_*`. All under `D:/GnssProcessorSupport/runs_nom/`
- **gates (frozen at pre-registration):** card IRD section 11 (the row's gate on ird_t1 and ird_tr, cumulative on A1); the microscope twins and the floor-off arms are readings; for `merge_split`, the fixture built here, cumulative on A1. Section 9's predictions were written before its arms ran; it changes no bar

**Outcome.** Question: do IRD-4's `slip` and `merge_split` rows pass their gates, and why does A1 hold receivers' wet delay metres off? Both rows LANDED default off: the classes' as-built steps fall to 0, both edges carry identities and the consumer holds its bound (a weak clause here). I-b FAILED on A1: undeclared integer steps 508 to 224 and 213 to 146 at the slip, 433 to 47 and 1 360 to 733 at the split, and the fractional bar is missed. The cause is the receiver floor (section 9): a zero-mean prior on a bias holding integer path sums, which moves T by metres, as predicted with nothing fitted, and the DCB one for one. Standing: every event row built; `ird_tr` passes I-b at sigma 3 m; D0 and D-N are the author's ([section 9.8](#98-what-bears-on-the-ruling-for-the-author)).

## 1. Where this card starts

Card IRDD reached the `root` row (its section 5) close to the 60 KB bar. The author answered "Yes, slip next": IRD-4 continues in table order, with the hold on certified re-hosts (IRDC section 7.3) and the uncertified rule as built. Card IRD section 11 keeps the binding plan and the gate: per row, off byte-identical; on its own fixture (slip: ird_t1 and ird_tr), and only where the class reaches n >= 1, the class's legacy or fractional steps fall to 0, I-b holds for the class, both edges are counted with identities; no regression against the previous arm (float consumer by hour within max(+10 %, +5 mm), W5 no higher).

## 2. The rule as built

The fifth row in table order is the entry `slip`, card IRD section 5's TreeEdgeSlip ("read `relevel.free_ambs` with its cause bits, de-seat at the slip epoch, re-host from t-1 values"; step Zero or CutRelease, carry ReleaseArc, wire None or DeclareMoved). Without the entry every output is unchanged.

- **What the as-built rule does.** The election reads slips from `carry_slipped_ambs`, which is empty on the float vehicle, so a slipped tree edge keeps its seat. The kept row leaves a latched arc alone, so the draft runs: a pinned arc has no carry to round, its target falls to 0, and the step is a known target difference. The cycle itself stays in the arc's phase row (section 5.1).
- **Which arcs.** Those of `RelevelEpochState::free_ambs`, whatever latched them: the MW, LLI and IF-TD drains, the injected-free tag, the wind-up restart, the gap re-entry response and the GF jump. The cause bits are read and reported. The latch is complete at the election (PASS A runs first, and RAIM never writes it).
- **The de-seat.** `chainArcSlipped` also reads the latch under the row. An incumbent edge on a latched arc loses its seed and a new edge on one is refused in the forest's first pass: the path of the slip-aware election, reached with the latch in place of the empty funnel. In a pair family this holds when either band's arc is latched, because the bands share one tree.
- **The re-host.** The old child's fragment re-attaches through a new edge, at whichever of its vertices the walk reaches first, and the path to the old child reverses inside it. The new edge is that edge's own row's. Under the hold the rehost row takes it (the rounded t-1 carry of the new host, an unknown step, the cut release, a declaration); with the rehost row off it runs the as-built rule. Certified re-hosts stay held, so the table's Zero branch is not built here either.
- **An arc that stays.** When no other route reaches the child, the second pass admits the latched arc and the row releases it (`ChainReleaseRow::Slip`), on the band whose arc is latched. The other band's pin keeps the frame, and the kept row can hold it. The target is the first-sighting one for a returning child, whose carry is in an old frame; the arc's previous target for an arc that was a tree edge at the last election, so the moved side steps by the slip and no more; else the draft. A child never seen in a tree is a birth and keeps the as-built rule.
- **The carry.** ReleaseArc is met by what is already there: the cut release erases the slipped arc's carry with its moved side, and with no release the latch's bounded drop stands.
- **The observatory.** TreeEdgeSlip is built. Each de-seated edge writes a `slip_deseat` leave row on the arc (cause bits, whether this band's arc is latched, old child and parent, previous target, new parent, the row that released it) and a join row on the edge of that release, on the child's new parent edge with no release, or on the arc's own names when the child has no parent now. An edge the row released writes a leave and a join row on the arc with the as-built facts it replaced. The election's run-end lines count the edges de-seated, those moved for the other band's latch, those released, the births, what the releases took and the cause bits; a run with no slipped tree edge reads as no event. The frame census has its own line. The row keeps no memory of its own beyond the earlier-tree memory it shares with the returning row, so a snapshot does not drop it.
- **Two fixes before the final arms.** A join row could name a blank satellite or station when the old child had no parent (found at review). The cause tally counted de-seated edges only; it now counts released edges too.
- **Tests.** 14 new oracle cases, among them a random-lineage oracle over 320 epochs that restates the rule apart from the production code (18 207 tree edges; 918 edges de-seated, 125 for the other band's latch, 777 in a released moved side, 141 with no release; 312 latched edges released, 73 at the first-sighting target, 131 at the previous target, 108 at the draft; 57 births), 7 observer and run-end cases, a loader case and 2 updated tests. The full Debug suite on the final sources: 3 818 run, 3 785 passed, 33 skipped (fixtures absent), 0 failed. `nom_epoch_lint.py`, `nom_n4_verify.py --whole`, `counters_split_check.py`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader`, `simnet_chain_frame_grade.py --selftest` and `ird_make_arms.py --selftest` pass.

## 3. The arms

One exe, `gnss_processor_ird4slip.exe` (sha256[:16] `e105882041a409da`: `104b103` plus the row), 2 h arms under `D:/GnssProcessorSupport/runs_nom/`:

- **Off identity:** A1 on r3c, the golden on the sim bed and A1krhR on the microscope, ird_rg, ird_t1 and ird_tr (`_rap_ird4s_{off_a1_r3c,off_gs,a1krhR_mic,a1krhR_rg,a1krhR_t1,a1krhR_tr}`) against the same arms on the root row's exe (`_rap_ird4root_*`, `_rap_ird4s0_*`).
- **Inert:** A1krhRs against A1krhR on the microscope, r3c and r3b, where no tree edge slips.
- **The gate:** A1krhRs against A1krhR on ird_t1 and ird_tr.
- **Readings:** the same pair on two new beds, `ird_t1m` and `ird_trm` (the microscope with the same scripted slip on the same arc: rung 1c's tree holds the same edges at the slip epoch, and `ird_fixture_gate.py verify` passes each against rung 1c); A0fkrhRs against A0fkrhR on those beds and on ird_t1 and ird_tr; A0fkrRs against A0fkrR on `ird_t1m` (no rehost row); A1krhRs against A1krhR on ird_t2 to ird_t5 and ird_rv6.
- **Slip-free controls:** A1krhR on r3b and the microscope, A0fkrhR on r3b and the microscope, each the bed of a fixture with no slip.

Each armed config differs from its parent's in the event-row list alone. The class: one slip at TOW 265740, latched on both bands with the IF-TD and GF jump bits (132). On ird_t1 G09 leaves S10000SIM and re-attaches under S02000SIM; the moved side is 4 satellites and 3 receivers per band. On ird_tr G03 leaves the root's own edge and its fragment re-attaches at S02000SIM under G01, with G03 reversed below it; the moved side is 6 satellites and 15 receivers per band.

## 4. The gate

Off identity passes on all six pairs: products, every trace CSV, the run manifest and the whole logs are identical. The row armed where no tree edge slips is inert on the microscope, r3c and r3b: the 42 products and trace files and the run manifest are byte-identical, and the logs differ only in the text that names the row (11 lines).

| clause (previous arm in brackets) | ird_t1 | ird_tr | microscope t1m | microscope trm | floor-off t1m | floor-off trm |
| --- | --- | --- | --- | --- | --- | --- |
| the class reaches n >= 1 | 1 epoch, 2 edges | 1, 2 | 1, 2 | 1, 2 | 1, 2 | 1, 2 |
| as-built steps on the slipped arc | pass: 0 edges (2, +57 and -91 cycles) | pass: 0 edges (2, known zero) | pass: 0 (2, +83 and -66) | pass: 0 (2, known zero) | pass: 0 (2, +83 and -66) | pass: 0 (2, known zero) |
| I-b: undeclared integer steps | FAILED: 224 (508) | FAILED: 146 (213) | pass: 0 (331) | pass: 0 (6) | pass: 0 (324) | pass: 0 (0) |
| I-b: fractional bar | FAILED: WL 0.497, 146 pairs (0.497, 171) | FAILED: WL 0.385, 86 (0.496, 169) | FAILED: E1 0.405, 179 (0.475, 228) | FAILED: E1 0.266, 96 (0.479, 195) | FAILED: E1 0.038, 53 (0.066, 153) | FAILED: E1 0.037, 47 (0.058, 130) |
| both edges with identities | pass: 2 leave and 2 join rows, then 2 cut_release and 8 declared_keys rows | pass: the same, 12 keys | pass | pass | pass | pass |
| float consumer by hour, m | pass: 0.186, 0.117 (0.186, 0.122) | pass: 0.186, 0.118 (0.186, 0.121) | pass: 0.144, 0.043 (the same) | pass: the same | pass: 0.124, 0.045 (the same) | pass: the same |
| W5 no higher | pass: 0, 240 of 240 emitted | pass | pass | pass | pass | pass |

The bars are the frozen ones (0.05 wide lane on the free bed, 0.02 per band on the microscope). The undeclared pairs number 351 on the t1 beds and 300 on the tr beds, against 465 in the previous arm, which declares nothing. On every arm the pin audit is complete and the pin identity holds in 240 of 240 epochs.

The same undeclared pairs at the slip epoch against those pairs at the 14 no-event epochs of the prior 10 min, per epoch (largest wrapped step: p50 of the quiet epochs, largest; the slip epoch, row, previous):

| bed, channel | quiet p50 | quiet largest | row | previous |
| --- | --- | --- | --- | --- |
| ird_t1, WL | 0.225 | 0.500 | 0.497 | 0.497 |
| ird_tr, WL | 0.149 | 0.202 | 0.385 | 0.450 |
| microscope t1m, E1 | 0.380 | 0.500 | 0.405 | 0.475 |
| microscope trm, E1 | 0.281 | 0.295 | 0.266 | 0.262 |
| floor-off t1m, E1 | 0.039 | 0.372 | 0.038 | 0.056 |
| floor-off trm, E1 | 0.024 | 0.059 | 0.037 | 0.040 |

**The integer steps left on the free bed.** Rung 3b with no slip and no row steps at this epoch too: `CLK_ANCHOR cta_decouple` on G15 and G24, with G15 at +6.23 / +4.84 cycles (wide lane +1.39). Those are the 26 wide-lane integer steps on ird_t1, in both arms. The per-band counts are noise on this bed: the same pairs on ird_t1 read E1 98, 92, 56, 51 and E2 100, 75, 57, 47 at the slip epoch and the next three. Against the slip-free control, the pairs of ird_t1 read at the slip epoch E1 p50 0.007, max 0.595, 1 pair above 0.5 cycle; wide lane max 0.070, 0 pairs. ird_tr does not clear that way: against the control G24, G13 and G15 step by -9.41 / -7.26, -6.56 / -5.06 and -5.86 / -4.59 cycles, 68 wide-lane pairs (section 5.3). Its raw wide-lane integer steps are G24's and G13's with the row (47) and G15's in the previous arm (24).

**The readings on ird_t2 to ird_t5 and ird_rv6** (undeclared integer steps, row (previous); hour 2 in m, row (previous)): t2 184 (215), 0.116 (0.122); t3 183 (484), 0.121 (0.122); t4 31 (215), 0.122 (0.122); t5 111 (139), 0.121 (0.121); rv6 35 (17), 0.117 (0.118). On t3 the L2W slip with LLI latches both bands (L2W with the LLI bit too). rv6 is the row's own release: S45000SIM returns through latched arcs (wind-up and gap bits), takes the first-sighting target on the arc to G10 and moves alone, with 3 carries and 1 receiver phase bias per band and no key to declare; its extra integer steps are band noise, wide lane 0 in both arms.

## 5. The causal readings

### 5.1 Where the slip goes

As built the cycle does not enter the frame. On the floor-off microscope G03 against the undeclared satellites, with the slip-free control taken out, reads E1 -0.011 cycle at the slip epoch and -0.100 at 268140. It stays in the pinned arc's phase row, which the block-Baarda census reports at each of its four epochs to the end of the run:

| arc, bed | previous arm, t_B at 265830, 266430, 267030 (row t at the first) | row |
| --- | --- | --- |
| S10000SIM-G09, microscope t1m | 61.3, 50.1, 40.1 (244) | none |
| S13000SIM-G03, microscope trm | 73.3, 49.8, 38.3 (289) | none |
| S10000SIM-G09, floor-off t1m | 140.3, 125.3, 101.1 (560) | 1 row, t_B 4.4 at 267030 |
| S13000SIM-G03, floor-off trm | 175.5, 133.3, 105.9 (697) | none |

On the free bed the census names neither arc in either arm. With the row the arc is free and G03 is declared moved (E1 -0.245 cycle at the slip epoch, +0.030 at 266340, +0.035 at 268140).

The undeclared integer steps of the previous arm on the t1 beds are not the slip but the draft: the target falls to 0 under the arc, and G09 with its subtree steps +57 / -91 cycles (ird_t1), +83 / -66 (microscope) as a known shift. On the tr beds the target is 0 already and nothing steps, which is why the floor-off trm reads 0 in both arms.

### 5.2 What the row does to the undeclared pairs

The undeclared pairs from the epoch before the slip, against the slip-free control on the same arm's base (E1, p50 / max, cycles, integer pairs):

| bed | row, slip epoch | row, +2 min | previous, slip epoch |
| --- | --- | --- | --- |
| floor-off t1m | 0.002 / 0.066 | 0.003 / 0.164 | 0.002 / 0.015 |
| floor-off trm | 0.003 / 0.016 | 0.006 / 0.025 | 0.001 / 0.004 |
| floor-off ird_t1 | 0.002 / 0.144 | 0.007 / 0.481 | 0.003 / 0.015 |
| floor-off ird_tr | 0.003 / 0.018 | 0.012 / 0.065 | 0.006 / 0.022 |
| microscope trm | 0.002 / 0.010 | 0.004 / 0.019 | 0.001 / 0.004 |
| microscope t1m | 0.004 / 0.246 | 0.008 / 2.80, 95 pairs | 0.002 / 0.015 |
| ird_t1 | 0.007 / 0.595, 1 pair | 0.069 / 3.40, 75 pairs | 0.005 / 0.116 |
| ird_tr | 0.044 / 9.42, 69 pairs | 0.250 / 7.62, 86 pairs | 0.008 / 0.091 |

With the floor off the frame outside the moved side holds, and I-b at the slip epoch reads: ird_tr PASS (wide lane max 0.026, 0 integer steps; previous 0.213 on 86 pairs, 39 steps), ird_t1 wide lane max 0.213 on 74 pairs, the size those pairs reach in the previous arm (0.214 on 74), with 56 undeclared integer steps (364), all per-band, and 0 against the control.

### 5.3 The floor-on posture

The large entries of the table are not a frame step of the row's. They are non-dispersive (E1 / E2 about 1.3, the wavelength ratio) and they follow receivers whose wet delay A1 holds far from truth, which the floor-off arms do not show. S45000SIM reads 8.11 m at the epoch before the slip (truth 0.13 m; -0.11 m with the floor off). G24 and G15 hang below it, and G13 below S35000SIM (-1.39 m). On ird_tr S45000SIM reads 9.48 m at the slip epoch against 8.37 m in the control, as the release erases its arcs to the moved satellites. On microscope t1m four undeclared satellites (G21, G02, G30, G14, the first two below S23000SIM) drift to -7.8 and -5.1 cycles after 20 min: S23000SIM's wet delay stands at -0.99 m (truth 0) and recovers to -0.15 m in the control by 266940, -0.74 m with the row, whose release resets the recovery. No row addresses that state. It goes with the receiver floor that card IRD's decision D0 keeps (S23000SIM reads -0.03 m at that epoch with the floor off), and it is not traced further here.

### 5.4 What the release costs

| reading (previous; row) | ird_t1 | ird_tr | microscope t1m | microscope trm | floor-off t1m | floor-off trm |
| --- | --- | --- | --- | --- | --- | --- |
| reference-free double differences, share above 0.25 cycle, slip epoch | 0.798; 0.805 | 0.801; 0.822 | 0.178; 0.203 | 0.178; 0.225 | 0.110; 0.130 | 0.112; 0.187 |
| the same at +1 min | 0.791; 0.787 | 0.787; 0.813 | 0.173; 0.182 | 0.174; 0.202 | 0.094; 0.116 | 0.097; 0.137 |
| the run, p50 / share | 2.44 / 0.805; 2.53 / 0.809 | 2.46 / 0.805; 2.62 / 0.810 | 0.044 / 0.174; 0.046 / 0.177 | 0.046 / 0.176; 0.047 / 0.184 | 0.018 / 0.084; 0.019 / 0.085 | 0.019 / 0.085; 0.021 / 0.098 |
| the search's would-be fixes (measure-only, against truth): wrong, right | 2 786, 2 014; 2 637, 1 923 | 2 645, 2 023; 2 664, 1 782 | 723, 165 153; 650, 167 476 | 879, 164 799; 1 030, 164 059 | 30, 178 283; 20, 178 180 | 165, 178 442; 48, 177 879 |
| carries erased by the release, both bands | 144 | 300 | 144 | 300 | 144 | 300 |

The 10 min bins on the floor-off beds return to the previous arm's level two to three bins after the slip (trm share above 0.25 cycle by bin from the slip: 0.12, 0.09, 0.06, 0.16 against 0.07, 0.04, 0.04, 0.15). The stop rule's reading does not move (event-free chords L1C p50 0.0212 against 0.0235 on floor-off t1m, 0.0217 against 0.0225 on trm). Recorded, not graded.

### 5.5 The row alone, and the consumer

- **The row alone (A0fkrRs against A0fkrR on `ird_t1m`, a reading and not a gate arm).** The de-seat stands and the new edge runs the as-built rule, a known fractional shift: undeclared integer steps 324 to 8, and the undeclared wide lane reads max 0.455 on 205 pairs (0.334 on 113), because nothing is declared. Hour 2 reads 0.0220 m (0.0219). The row needs the rehost row for its declaration, and carries that row's cost: the same bed's hour 2 reads 0.045 m with it.
- **The consumer clause is weak here.** The phase-bias hold keeps the emitted bias of a moved key: On floor-off t1m G09's pre-hold bias goes from 5.918 m to 5.930 m with the row and to -9.869 m in the previous arm, and its emitted bias stays between 4.83 and 4.84 m in both, in generation 1. So the consumer differs from the previous arm by a median of at most 1e-4 m on the microscope and floor-off arms, 7.6e-3 m on ird_t1 and 2.5e-3 m on ird_tr. Its by-hour bound passes, and that says little about the frame. The declarations reach the wire at IRD-7.

## 6. Reading

1. **The row does what card IRD section 5 states, with the hold in place.** The slipped edge leaves the tree at the slip epoch, with a row naming its cause bits; its fragment re-attaches at once; the slip's own misfit leaves the phase row; and the as-built draft's whole-cycle step is gone.
2. **The integer clause holds wherever the bed resolves it.** It reads 0 on the microscope twins, and 0 against the control on floor-off ird_t1 and ird_tr. It FAILED on ird_t1 and ird_tr under A1, the row's named arm, for causes this row does not own: band noise, a clock-anchor event the control has, and on ird_tr the wet-delay state that goes with the floor (section 5.3).
3. **The fractional bar is missed on every graded bed.** The slip epoch reads at or below the quiet median on two beds (microscope trm, floor-off t1m), inside the quiet range on three (ird_t1, microscope t1m, floor-off trm) and above it on ird_tr, whose excess is section 5.3's. The bar is not relaxed by this reading.
4. **The cost is the rehost row's.** Under the hold a slip on an edge with a large subtree releases all of it (15 of 60 receivers and 6 of 31 satellites on ird_tr), for a slip the as-built arm leaves in one phase row. The certified branch stays held.

**Not covered.** One slip per fixture, always latched at its own epoch; a slip the detectors flag late, or never, is not exercised (the adversarial arm belongs to IRD-6). A receiver-wide slip and a datum receiver whose arcs all slip are covered by the unit cases only: the second case moves the whole component but the root, which a root handover could avoid and this row does not attempt. The second pass can admit more latched arcs than the minimum when a clean fragment is reachable only through latched arcs, each released. The field bed is not run, and no golden changes.

## 7. Standing

The `slip` row LANDED default off; the ledger row of `network.phase_datum_eq34_event_rows` stays UNVALIDATED with its text updated. Of its gate: off identity, the class's legacy steps, both edges' identities, the float consumer and W5 pass; I-b FAILED on ird_t1 and ird_tr (integer steps and the fractional bar), with the attribution above. Next in table order: `merge_split`, whose fixture is "whatever the census reports": the 141 arms of IRD-2 to IRD-4 that carry the census line all report 0 merges and 0 splits, so that class has no bed yet. IRD-5b (D-N) and the GF-datum defect (D0) stay owed.

## 8. 2026-11-06: IRD-4's `merge_split` row

The author answered "Yes, merge_split next". This is the last row of card IRD section 5's table. The gate is the one quoted in section 1, with "whatever the census reports" as the fixture. The census reports 0 merges and 0 splits on the 141 arms of IRD-2 to IRD-4, so the fixture is built here. Section 9 answers a question relayed at the author's word.

### 8.1 The fixture

An abrupt cut fragments the old tree, so most of its event is rehost releases. A pure cut gives one split and one merge on one arc.

- **`ird_ms`, `ird_msr` (the pure cut, rung 3b).** From epoch 130 every arc across a bipartition of receivers and satellites is blanked except one bridge, so the tree re-hosts once and holds the small side below that arc. The bridge alone is blanked in epochs 158 to 177: a split at TOW 265740 and a merge at TOW 266340. The other arcs return at epoch 190 (TOW 266700). `ird_ms` cuts S41000SIM, S43000SIM, S53000SIM, S56000SIM, S59000SIM and G15, G17, G22, G24, G30 through S51000SIM-G15, so the small piece loses the old root S13000SIM. `ird_msr` cuts the side that holds the root (S02000SIM, S05000SIM, S10000SIM, S13000SIM, S18000SIM, S23000SIM, S31000SIM and G01, G03, G04, G16, G26, through S34000SIM-G01), so the large piece loses it.
- **`ird_msx` (the abrupt cut).** Everything crossing is blanked at once at the split: 16 rehost releases per run at that epoch beside the one split piece.
- **Controls and twins.** `ird_ms_ctl` and `ird_msr_ctl` keep the bridge. `ird_ms_mic`, `ird_msr_mic`, `ird_ms_ctl_mic` and `ird_msr_ctl_mic` are the same on rung 1c with zero-ionosphere rows.
- **Checks.** `ird_fixture_gate.py verify` reads 5 944 records blanked and 130 214 kept on `ird_ms`, 4 928 and 131 230 on `ird_msr`, 0 problems. `gate` passes on all four beds on the previous arm: the bridge is the one tree edge across the cut at epoch 157, there are two components in epochs 158 to 177 and one at 178, on both bands, and products equal the control's before epoch 158. The components at the split are 81 and 10 vertices below S13000SIM and S43000SIM (`ird_ms`), 79 and 12 below S25000SIM and S13000SIM (`ird_msr`).

### 8.2 The rule as built

Card IRD section 5's rows: Merge "cut release of the absorbed side", step and carry CutRelease, wire ComponentChange; Split "the split-off root pinned at its carried RPB; the cmp change declared", step Zero, carry Keep, wire ComponentChange. Without the entry every product and trace is unchanged.

- **Split.** A piece that does not keep the old root takes the root row's pin: its new root's receiver phase bias is pinned at its carried value on each band, satellites remembering the old root are kept in that frame and the kept row holds the edges. So the frame does not step, nothing is erased and nothing is declared. A piece whose new root has no carried bias inside the horizon stays as built. The loader drops `merge_split` without `root`, since the pin is that row's; the rule also holds when a snapshot drops `root`.
- **Merge.** A tree edge whose child's tree at the last election differs from the tree its parent's side carries is released (`ChainReleaseRow::Merge`) before the other rows, whichever row its arc would fall under, with the target the as-built rule drafted, an unknown step, a cut release of the absorbed side and a declaration. A vertex in no old tree inherits the tree above it. An edge with both ends in one old tree that the new root does not continue is `absorbed`: the kept row may hold it although its satellite remembers another root, and the release moves it with the side. The side with more satellites keeps its root, by the election's own votes.
- **The memory.** `eq34_tree_root`, one declared field on the station table and one on the satellite-signal table, ColdStart: the root of each vertex's tree at the last election, replaced wholesale each election and written only with the row armed.
- **The observatory.** Merge and Split are built rows of the chain frame. A released merge edge writes a `leave` row (the absorbed old root in the station column) and a `join` row with `old_root=`, `into_root=`, `child=`, `parent=` and the as-built facts it replaced, then the release's `cut_release` and `declared_keys` rows. Split rows carry `outcome=` and, with the row armed, `rpb_target_m` and `rpb_sigma_m`. A run with no such event reads as no event at run end.
- **The component change on the wire.** The `cmp=` token belongs to IRD-7 (`network.emit_wire_chain_tokens`), so at this step a component change is declared in the CHAIN rows only.
- **The extent of the release.** As built the cut release erases the moved satellites' arcs at every station, inside the moved side too, which is wider than card IRD section 3's "one endpoint on each side". Keeping the inside arcs needs the coupling terms handled and is not tried. It explains the declared transient at the merge (section 8.5).
- **A fix before the final arms.** The first draft took a crossing test ahead of the kept and rehost classes, which moved output on a merge bed with the entry absent (found at review); only `merge_released` now goes first. With the row off a merge edge whose child had a parent reads as a re-host, and the absorbed side's kept edges as a root handover.
- **Tests.** 9 new oracle cases, among them a random-lineage oracle over 320 epochs that restates the rule apart from the production code (14 678 tree edges; 48 edges joining two old trees released, 32 edges inside an absorbed side, 27 held at their targets, 1 372 vertices in no old tree inheriting the one above, 47 carries erased, 46 keys declared), the root row's oracle extended to split pieces (7 pinned, 14 with no carry), an absent-entry oracle (120 epochs with several components, 9 split pieces as built), 6 observer and run-end cases and a loader case. The full Debug suite on the final sources: 3 832 run, 3 799 passed, 33 skipped (fixtures absent), 0 failed. The epoch and entity lints, the counters check, both ledger verifies and the graders' selftests pass.

### 8.3 The arms

One exe, `gnss_processor_ird4ms.exe` (sha256[:16] `4547066185234026`: `288a80d`), 2 h arms under `D:/GnssProcessorSupport/runs_nom/`, `_rap_ird4ms_*`:

- **Off identity:** A1 on r3c, the golden on the sim bed and A1krhRs on the microscope, ird_t1 and ird_tr against the same arms on the slip row's exe (`_rap_ird4s_*`), and A1krhRs on `ird_ms` and `ird_msr` against the same (`_rap_ird4m0_*`), so two of the seven pairs do reach a merge and a split.
- **Inert:** A1krhRsm against A1krhRs on the microscope, r3c, r3b, `ird_ms_ctl` and `ird_msr_ctl`, which have no component event.
- **The gate:** A1krhRsm against A1krhRs on `ird_ms`, `ird_msr` and `ird_msx`.
- **Readings:** the same pair on the microscope twins; A0fkrhRsm against A0fkrhRs on `ird_ms`, `ird_msr` and their twins (the floor off); A0fkrRsm against A0fkrRs on `ird_ms_mic` (no rehost row).

### 8.4 The gate

Off identity passes on all seven pairs: products, every trace CSV and the run manifest are identical. The logs differ in 4 lines, the entity inventory's declared count (115 to 117) and the two new fields' rows at 0. The row armed with no component event is inert on all five beds: products, traces and manifest identical, and the logs differ in 16 lines that name the row or give the memory's rows.

| clause (previous arm in brackets) | `ird_ms` | `ird_msr` | `ird_msx` | microscope ms | microscope msr |
| --- | --- | --- | --- | --- | --- |
| the class reaches n >= 1 | 1 split, 1 merge | 1, 1 | 1, 1 | 1, 1 | 1, 1 |
| split: as-built unknown steps | pass: 0 (10 edges) | pass: 0 (52) | pass: 0 (4) | pass: 0 (10) | pass: 0 (52) |
| I-b, split: undeclared integer steps | FAILED: 47 (433) | FAILED: 733 (1 360) | pass: 0 (72) | pass: 0 (417) | FAILED: 64 (1 347) |
| I-b, split: fractional bar | FAILED: WL 0.174, 126 pairs (0.497, 144) | FAILED: WL 0.485, 261 (0.497, 423) | FAILED: WL 0.102, 24 (0.271, 47) | FAILED: E1 0.439, 202 (0.499, 210) | FAILED: E1 0.500, 403 (0.499, 436) |
| I-b, merge: undeclared integer steps (the class is not named in the previous arm) | pass: 0 | FAILED: 297 | pass: 0 | pass: 0 | FAILED: 4 |
| I-b, merge: fractional bar | FAILED: WL 0.074, 23 | FAILED: WL 0.411, 213 | FAILED: WL 0.097, 41 | FAILED: E1 0.226, 97 | FAILED: E1 0.495, 245 |
| both edges with identities | pass: at the split 2 leave and 2 join rows naming roots, outcome and pin; at the merge 2 leave, 2 join, 2 cut_release and 10 declared_keys rows | pass, 10 keys | pass, 12 keys | pass | pass |
| float consumer by hour, m | pass: 0.186, 0.156 (0.186, 0.156) | pass: 0.186, 0.105 (0.186, 0.109) | pass: 0.186, 0.111 (0.186, 0.122) | pass: 0.144, 0.045 (the same) | pass: 0.144, 0.040 (the same) |
| W5 no higher | pass: 0, 240 of 240 emitted | pass | pass | pass | pass |

The bars are the frozen ones (0.05 wide lane on the free bed, 0.02 per band on the microscope). On every arm the pin identity holds in 240 of 240 epochs. On every bed the row pins 2 split pieces (one per band) and releases 2 merge edges (one per band), as-built unknown on both. The releases take: `ird_ms` 24 carries, 10 receiver phase biases, 10 satellite phase-bias priors, 10 keys; `ird_msr` 44, 12, 10, 10; `ird_msx` 76, 18, 12, 12. The kept row holds 18 edges inside the absorbed side on `ird_ms`.

The floor-off arms, and the row with no rehost row (readings):

| clause (previous arm in brackets) | floor-off ms | floor-off msr | floor-off microscope ms | floor-off microscope msr | no rehost row, microscope ms |
| --- | --- | --- | --- | --- | --- |
| I-b, split | FAILED: WL 0.189, 129; 33 integer (0.489, 150; 438) | **PASS** (0.499, 406; 1 354) | FAILED: E1 0.053, 130; 0 integer (0.493, 142; 417) | FAILED: E1 0.120, 261; 0 integer (0.499, 436; 1 337) | **PASS** (0.467, 113; 403) |
| I-b, merge | FAILED: WL 0.080, 26; 0 integer | FAILED: WL 0.485, 202; 131 integer | FAILED: E1 0.134, 49; 0 integer | FAILED: E1 0.367, 231; 0 integer | FAILED: E1 0.025, 5; 0 integer |
| float consumer by hour, m | 0.277, 0.031 (the same) | 0.277, 0.031 (the same) | 0.124, 0.045 (the same) | 0.124, 0.045 (0.124, 0.044) | 0.125, 0.018 (the same) |

The undeclared pairs at the event epoch against the same pairs at the no-event epochs of the prior 10 min, on the row's arm (largest wrapped step per epoch):

| bed, channel | split: quiet p50, largest; event | merge: quiet p50, largest; event |
| --- | --- | --- |
| `ird_ms`, WL | 0.235, 0.450; 0.174 | 0.106, 0.243; 0.074 |
| `ird_msr`, WL | 0.433, 0.500; 0.485 | 0.362, 0.499; 0.411 |
| `ird_msx`, WL | 0.187, 0.469; 0.102 | 0.107, 0.251; 0.097 |
| microscope ms, E1 | 0.456, 0.499; 0.439 | 0.326, 0.476; 0.226 |
| microscope msr, E1 | 0.499, 0.500; 0.500 | 0.496, 0.500; 0.495 |
| floor-off ms, WL | 0.279, 0.430; 0.189 | 0.096, 0.296; 0.080 |
| floor-off msr, WL | 0.113, 0.318; 0.048 | 0.072, 0.499; 0.485 |
| floor-off microscope ms, E1 | 0.303, 0.489; 0.053 | 0.042, 0.241; 0.134 |
| floor-off microscope msr, E1 | 0.218, 0.406; 0.120 | 0.086, 0.462; 0.367 |
| no rehost row, microscope ms, E1 | 0.020, 0.058; 0.013 | 0.013, 0.046; 0.025 |

### 8.5 The causal readings

**The split against the control** (one-epoch step of the undeclared pairs, arm less control, E1 in cycles, max and integer pairs):

| bed | side | row | previous |
| --- | --- | --- | --- |
| `ird_ms` | inside the small piece | 0.019, 0 | 103.5, 10 |
| | outside | 0.019, 0 | 0.027, 0 |
| | across | 0.019, 0 | 66.7, 130 |
| `ird_msr` | inside | 0.017, 0 | 0.176, 0 |
| | outside | 24.8, 180 | 304.0, 325 |
| | across | 24.4, 40 | 195.2, 130 |
| floor-off microscope ms | inside | 0.004, 0 | 73.5, 9 |
| | outside | 0.0003, 0 | 0.005, 0 |
| | across | 0.010, 0 | 55.7, 130 |
| floor-off microscope msr | inside | 0.0001, 0 | 0.0001, 0 |
| | outside | 0.008, 0 | 255.4, 318 |
| | across | 0.006, 0 | 221.6, 130 |

So the split no longer steps: 0 integer pairs against the control wherever the floor is off and on A1's `ird_ms`. `ird_ms`'s 47 raw integer steps are per-band noise on this bed (E1 14, E2 33, wide lane 0), and 0 against the control. On A1's `ird_msr` the row takes the large piece from 304 cycles to a p50 of 3.4 and a max of 24.8. The new root S25000SIM is pinned (L1C -7.860 m, sigma 0.016 m; L2W +7.808 m), so what remains is not its frame: against the control the epoch's wet delay steps by up to 2.6 m at receivers that A1 holds metres off truth (S45000SIM reads +20.5 m at this epoch, S43000SIM -6.5 m), and G18, G15, G32, G13, G27, G31, G24 and G10 step 1.1 to 4.7 m non-dispersively with them, while the receiver phase biases step by at most 0.04 m. Section 9 explains that state; with the floor off the same pairs read 0.117 cycle at most on `ird_msr`, with 0 integer pairs, and I-b passes.

**The merge against the control.** Outside the moved side the row adds nothing: `ird_ms` 0.021 cycle at most, 0 integer pairs (0.027, 0 in the previous arm), and floor-off microscope ms 0.0018 (0.0027). The moved side is declared, and the row shrinks its transient: E1 p50 across the cut 3.0 cycles on `ird_ms` (previous 35.4), 0.18 on floor-off microscope ms (previous 8.8), and inside the moved side 3.2 (7.5) and 0.95 (1.3). That transient is non-dispersive (E1 / E2 about 1.28). Inside the moved side on floor-off microscope ms it reads 0.06 cycle p50 after 60 s, 0.42 after 5 min with the cut arcs still blanked, 0.04 after they return and 0.003 at run end. It is the cost of the wide release of section 8.2.

On the floor-off `ird_msr` beds the outside pairs read up to 2.27 cycles against the control at the merge (0.95 on the microscope), as the previous arm does (2.26, 0.83), so this is not the row; it is recorded and not traced. Over more than one epoch those pairs also carry the declared re-hosts of TOW 266160, 266190 and 266280.

**What the row costs and buys** (previous arm in brackets):

| reading | `ird_ms` | `ird_msr` | microscope ms | floor-off microscope ms | floor-off microscope msr |
| --- | --- | --- | --- | --- | --- |
| reference-free double differences, share above 0.25 cycle, split epoch | 0.735 (0.727) | 0.812 (0.831) | 0.172 (0.174) | 0.094 (0.104) | 0.126 (0.549) |
| the same at +30 s | 0.722 (0.735) | 0.801 (0.827) | 0.167 (0.165) | 0.089 (0.091) | 0.128 (0.502) |
| the same, merge epoch | 0.721 (0.721) | 0.800 (0.847) | 0.120 (0.121) | 0.015 (0.015) | 0.516 (0.533) |
| the run, p50 / share | 2.06 / 0.787 (2.06 / 0.787) | 2.65 / 0.805 (3.81 / 0.820) | 0.042 / 0.182 (0.042 / 0.182) | 0.020 / 0.094 (0.020 / 0.095) | 0.038 / 0.148 (0.041 / 0.167) |
| the search's would-be fixes (measure-only): wrong, right | 2 303, 2 997 (2 307, 2 965) | 1 386, 1 166 (1 428, 566) | 718, 156 574 (899, 156 385) | 150, 166 334 (150, 166 266) | 106, 163 573 (82, 161 832) |

The stop rule does not move: event-free chords L1C p50 0.0192 against 0.0190 (PASS on both) on floor-off microscope ms and 0.0242 against 0.0235 (FAIL on both) on floor-off microscope msr. The consumer clause is weak here, as in section 5.5: the phase-bias hold keeps the emitted bias of a moved key until IRD-7.

### 8.6 Reading

1. **The row does what card IRD section 5 states.** A split no longer steps the frame: the piece's root is pinned at its carried bias and its edges are held. A merge is a declared cut release of the side the votes absorb, its identities counted.
2. **The integer clause holds wherever the bed resolves it, and the split's whole-cycle steps are gone.** Against the control the split reads 0 integer pairs on A1 `ird_ms` and on every floor-off arm. It FAILED on `ird_msr` under A1 and its microscope twin, and at the merge on the same two beds, for the wet-delay state of section 9 (0 integer steps on the floor-off microscope arms).
3. **The fractional bar is missed on every graded bed.** On A1's beds the event epochs read at or below the quiet median on seven of the ten cells and inside the quiet range on the other three, though those quiet epochs themselves wander by up to half a cycle (section 9). With the floor off and no rehost row the split passes and the merge reads E1 0.025 on 5 pairs, inside a quiet range of 0.046. The bar is not relaxed by this reading.
4. **`ird_msr` is the row's hard case.** A split that takes the old root away from the large piece is the event that stepped 304 cycles, and the row holds it. At the merge the side with more satellites keeps its root, so the small side, which holds the old root, is the one absorbed; S25000SIM is root from then on.

**Not covered.** One split and one merge per fixture, on a cut built for the purpose; the 141 earlier arms show no natural event of this class. The piece with no carried bias inside the horizon is covered by the unit cases only (the fixtures pin every piece). A merge of more than two trees at one epoch and a split into more than two pieces are in no arm. The field bed is not run, and no golden changes.

### 8.7 Standing

The `merge_split` row LANDED default off; the ledger row of `network.phase_datum_eq34_event_rows` stays UNVALIDATED with its text updated. Of its gate: off identity, the split's as-built steps, both edges' identities, the float consumer and W5 pass; I-b FAILED on the fractional bar on every graded bed and on integer steps on `ird_ms` (per-band, 0 against the control) and `ird_msr`. All six rows of card IRD section 5's table are now built, with certified re-hosts held. IRD-5b (D-N) and the GF-datum defect (D0) stay owed, and section 9 bears on both.

## 9. 2026-11-07: why A1 holds receivers' wet delay metres off

A question relayed at the author's word after section 5.3: why does the `ird_tr` posture hold receivers' wet delay metres off, does the phase-bias floor cause it, is that a defect in the sim posture, the floor or the float solve, can the golden or the field bed reach it, and what does it imply for reading I-b under A1, for IRD-5b (D-N) and for the GF-datum defect (D0)? The 2026-11-02 ruling and every bar stand: nothing here reopens or changes them, no code changes, and what bears on the ruling is reported in section 9.8 for the author. The arms are one knob each on A1krhRs on `ird_tr` unless named, on the exe of section 8, `_rap_ird4z_*`.

### 9.1 The answer

1. **The floor causes it.** The receiver phase-bias floor is a zero-mean prior on a bias that the chain's frame makes tens of metres. Its ionosphere-free part is paid for by the wet delay, and the phase carries then hold the result for tens of minutes under a formal sigma of 2 cm. Its size is predicted from a floor-off arm alone with nothing fitted: at sigma 1 from arms already run, at 3 m and 10 m before those arms ran.
2. **The defect is in the floor as it sits on the chain.** The float solve does what its rows say, and nothing in the sim posture is specific to it.
3. **Neither golden reaches it,** because the floor's weight needs `phase_datum_eq34`, off in both. **The field does with the chain on:** an archived arm reads wet delay up to +29.3 m at a formal 2 cm. No field arm has the floor off, so the floor is not proved the cause there.
4. **Under A1, I-b reads this state.** With the floor at 3 m, at 10 m or off, `ird_tr` passes I-b at the slip epoch. The undeclared satellite steps are the wet-delay differences of the receivers above them.
5. **The GF-datum defect is the same prior's other half:** the receivers' DCB moves one for one against the bias's geometry-free part. **D-N's trigger** does not fire with the rows armed, but that NEES cannot see the wet-delay state.

### 9.2 The mechanism

- **The frame.** Tree edges pinned at zero targets at birth put integer path sums into the biases. With the floor off the receivers' ionosphere-free bias has a median |value| of 20.0 m at the first epoch (sd 28 m, 39 m by hour 2). Its two bands behave as independent path sums: corr(GF, IF) 0.973, slope 0.463, where two independent bands of equal variance give 0.971 and 0.461.
- **The floor.** `network.phase_datum_eq34_rpb_prior_sigma_m`, 1 m per band, folded on every non-root receiver's bias each epoch. Its weight needs the chain (`src/core/network/driver/Knobs.cpp:343`).
- **The ionosphere-free part moves the wet delay.** With the slant ionosphere free, the two per-band priors act on the ionosphere-free bias as a prior of weight 0.1127 / sigma^2 (2.98 m at sigma 1). At a birth the arcs are free and T is known through code alone (median formal 0.97 m). Code holds dt + mbar T and the tree edges are pinned, so a shift dT_r moves the biases below r by (m^s - mbar) dT_r. T is the cheapest free variable that relieves the prior. Minimizing the sum of dT^2 / s_T^2 and W (delta0 + d delta)^2 gives the shift (`floor_predict.py`).
- **The geometry-free part moves the DCB.** On a tree edge code and phase fix only the difference of the receiver's geometry-free phase bias and its DCB (the slant ionosphere is free and the ambiguities are pinned). With the floor off the DCB stays near 0 and the difference sits in the bias. The floor forces the bias to the min-norm split of its ionosphere-free value (corr(GF, IF) 1.000, slope 0.462 against 0.461), and the rest goes to the DCB.
- **A release repeats it.** A release leaves T to code again (formal 1.3 m) with the floor acting. S45000SIM at the re-host of TOW 262830 goes from -3.61 m to +11.33 m in two epochs, and with the floor off from +0.03 m to +1.60 m. As its arcs regain precision the floor-off error is +0.10 m at TOW 263430 and the floor-on error +3.32 m. A cold re-host at TOW 263640 starts again, and by TOW 264630 it reads +8.15 m at a formal 0.02 m (floor off -0.30 m).

### 9.3 The tests

Predictions were written before the arms ran. The first-epoch shift of T against sigma 0 was computed from a floor-off arm with nothing fitted: from A0fkrhRs on a slip-free bed before the run, and recomputed from sigma 0 after it, with the same numbers.

| arm | first-epoch shift, rms, max, m (predicted) | error from 15 min, p50, p90, max, m | per-epoch step p95, p99, m | DCB sd, h1, h2, m | I-b at the slip epoch: WL max, pairs; integer | consumer h1, h2, m |
| --- | --- | --- | --- | --- | --- | --- |
| sigma 1 (A1krhRs) | 3.59, 19.76 (3.52, 17.07) | 0.371, 2.02, 12.5 | 0.373, 1.85 | 9.12, 9.49 | FAILED: 0.385, 86; 146 | 0.186, 0.118 |
| sigma 3 | 0.73, 4.74 (0.62, 3.62) | 0.038, 0.182, 3.30 | 0.166, 1.18 | 3.30, 3.32 | PASS: 0.026; 0 | 0.222, 0.030 |
| sigma 10 | 0.09, 0.51 (0.06, 0.38) | 0.025, 0.156, 3.76 | 0.175, 1.22 | 2.93, 2.93 | PASS: 0.026; 0 | 0.269, 0.033 |
| sigma 100 | 0.003, 0.016 | 0.025, 0.166, 4.21 | 0.180, 1.26 | 0.75, 0.76 | PASS | 0.277, 0.031 |
| sigma 0 | 0 | 0.024, 0.166, 4.22 | 0.180, 1.26 | 0.67, 0.70 | PASS: 0.026; 0 | 0.277, 0.031 |
| birth only | 3.59, 19.76 | 0.185, 1.14, 8.74 | 0.286, 1.59 | 6.58, 6.43 | FAILED: 0.071, 14; 90 | 0.131, 0.036 |
| seed round | 0.15, 0.44 | 0.217, 7.14, 421 | 1.48, 5.99 | 1.72, 4.28 | FAILED: 0.446, 118; 164 | 0.278, 0.637 |
| `carry_zwd` | 3.59, 19.76 | 0.674, 2.88, 8.29 | 0.107, 0.954 | 8.16, 7.94 | FAILED: 0.124, 84; 29 | 0.187, 0.032 |
| code std 0.6 m | 6.77, 26.11 (8.79, 42.73) | 0.425, 3.37, 23.5 | 0.724, 2.88 | 26.0, 26.8 | FAILED: 0.238, 151; 103 | 0.361, 0.032 |
| code std 0.15 m | 1.49, 9.42 (1.24, 6.91) | 0.184, 0.789, 4.83 | 0.214, 1.30 | 4.81, 4.79 | PASS: 0.025; 0 | 0.154, 0.056 |

The code-std rows are read against sigma 0 at the default code weight, with s_T scaled in the prediction, and those two predictions were made after the arms. All 240 epochs are emitted on every arm.

| prediction | verdict |
| --- | --- |
| P1, the floor sets the first-epoch shift; falsified above 1.2 m rms at sigma 3 or a correlation below 0.7 | PASS at sigma 1 and 3 (correlation 0.875 and 0.735, permutation null p95 0.569 and 0.664). At sigma 10 the size holds (0.09 against 0.06 m) and the correlation misses, 0.646 against a null p95 of 0.697: the test has no power at that size. The microscope twin, a separate check: rms 0.44 against 0.41 m, correlation 0.950, null p95 0.323 |
| P2, the floor sets the whole run; sigma 10 near sigma 0, max below 2 m after hour 1 | The trend holds at every survey epoch within 1 mm. The max clause FAILED: sigma 10 reads 3.76 m at TOW 267570 on S14000SIM, where sigma 0 reads 4.22 m, a release leaving T to code (after hour 1, p99 0.778 against 0.794 m; sigma 1, 6.05 m) |
| P3, birth only keeps the first-epoch shift and later errors | PASS: identical first epoch; after hour 1 p50 0.139, max 6.35 m |
| P4, seed round has no tension at the first epoch | PASS: 0.15 m rms with the floor on. From hour 1 T reaches 421 m and the RPB NEES 76.9, as decision D-S recorded (IRDB section 3.5); not traced here |
| P5, a carried wet delay keeps the first epoch and shrinks the jumps | PASS: identical first epoch, step p95 0.373 to 0.107. The error then stops decaying (p50 0.63 to 0.67 m to the end of the run) |
| P6, the family is not the cause | PASS: the T trace of the single-tree family with the floor on is identical to A1krhRs's, and A1 with the floor off to A0fkrhRs's (the pair family reproduces A0 where both bands track, IRDC section 5) |
| P7, only code removes the error: the fitted decay rate scales with I_code | FAILED as stated: g0 0.0175 per epoch moves to 0.0148 (code std 0.6 m) and 0.0296 (0.15 m), a factor under 2. The reasoning was wrong: the arcs' carried variances come from code too, so the gain I_code / (I_code + I_arc) barely moves (0.041, 0.049, 0.055) |
| P8, I-b passes at the slip epoch with the floor off and at 10 m | PASS, and at 3 m |

One knob excludes another remover: `filter.initial_std.troposphere` 0.5 to 5 m leaves the products and every network trace byte-identical (only the consumer's traces move), so T has no prior in the producer.

### 9.4 What holds the error, and what stays unexplained

- **Established.** After a birth the error sits under a stiff solve: S45000SIM stands at +8.15 m with a formal 0.02 m. Code asks for the whole correction: on that receiver the slope of its ionosphere-free code residuals on the wet mapping is -1.08 times the error (correlation -0.894) and its phase rows' slope is near 0. From TOW 264000 on, 22.1 % of code rows stand above 3 sigma and 7.6 % above 5 (floor off 0.3 % and 0).
- **Falsified: frozen arcs.** A model in which each arc keeps the share it baked (`lockin.py`, geometry alone) tracks S45000SIM for about half an hour (+8.03 m measured, +8.71 predicted, 25 min on) and then diverges (+3.92, +8.92 at 53 min). Over 50 min the arcs' ionosphere-free values move rms 4.54 m (floor off 0.44), and after satellite and station effects 0.81 m (floor off 0.069), at a formal 0.19 m. G18 at S45000SIM moves -11.7 m.
- **Falsified: each arc pulled by its own code row.** Arc steps against the arc's own code residual correlate at -0.06 (S45000SIM) and 0.06 (six receivers; null p95 0.018), at 0.03 to 0.05 of the predicted Kalman gain.
- **Fitted, not proved: a collective mode.** The error decays at g0 0.0175 per epoch, a time constant of 57 epochs (29 min), over the quiet epochs of 55 receivers. The step correlates at 0.37 with the prediction -g e, with g = I_code / (I_code + I_arc) (null p95 0.035), but at 0.15 of its size, since I_arc does not separate the pinned edges. By elimination the remover is code: T has no prior and the phase rows do not know T. No arm isolates that (P7).
- **The floor's per-epoch fold works against the decay.** Birth only decays faster (g0 0.0270) and halves the error after hour 1 (p50 0.139 against 0.279 m).
- **The consumer follows the motion, not the level.** `carry_zwd` keeps the error (p50 0.67 m) yet reads 0.032 m in hour 2, and birth only 0.036 m, against 0.118 m at sigma 1 and 0.031 m with the floor off.
- **Not traced.** Why a cold re-host grows the error again under a falling formal sigma (section 9.2's TOW 263640); and why the code slope stands at 2.51 m (receiver median) when the error's p50 is 0.28 m (correlation -0.23 over all receivers and epochs), which is what satellites carrying other receivers' errors would give (section 9.5), not checked further.

### 9.5 The satellites, and I-b under A1

Each tree edge (r, s) is a pinned phase row, so against a control the satellite's ionosphere-free visible phase moves by m^s_r dT_r + X_r, one unknown per receiver. Walking the tree, the parent satellite gives X_r and every child is predicted with no fitted number (`sat_vs_t.py`). `ird_tr` against its slip-free control at the slip epoch: measured rms 0.85 m, residual 0.35 m, correlation 0.946, slope 1.40; with no wet-delay term the residual is 0.95 m. 2 min later: 0.62, 0.30, 0.878, 1.29, and 0.57 with no term. So section 5.3's undeclared satellites are the wet-delay differences of the receivers above them, and section 8.5's `ird_msr` steps carry the same signature.

**Reading.** Under A1 a row's I-b is dominated by this state and does not grade the row. The floor-off arms and the arms below grade the row. At floor sigma 3 m on the row fixtures (previous reading at sigma 1 in brackets): `ird_msr`'s split passes (0.485, 261 pairs; 733 integer steps), `ird_t1`'s slip reads WL 0.190 on 74 pairs with 38 per-band integer steps (0.497, 146; 224), `ird_ms`'s split WL 0.164 on 122 with 24 (0.174, 126; 47), and the merges WL 0.076 on 24 with 0 (0.074, 23; 0) and WL 0.427 on 221 with 95 (0.411, 213; 297), the same sizes as the floor-off arms of section 8.4. The bars are not relaxed by this reading; A1 is the arm the gate names.

### 9.6 Reach: the goldens and the field

- **The goldens.** Both have `phase_datum_eq34` false, so the floor's weight is 0. The sim golden's wet delay reads p50 0.005 m, max 0.021 m at the last survey epoch. The field golden after hour 1 ranges from -0.111 to +0.447 m at a formal 5 mm, with no truth and no metre-level state.
- **A truth-free reading.** The receiver median of |code slope| after hour 1 (`code_slope_census.py`): A1krhRs 2.51 m (p90 4.04, 56 of 60 receivers above 0.5 m); floor off 0.145 (0.406, 2); sigma 3 0.179 (0.421, 4); sim golden 0.122 (0.320, 0); field golden 0.137 (0.351, 3 of 58). It separates the postures and reads the field golden with the sim golden. It is not a per-receiver error estimate (section 9.4).
- **The field reaches it with the chain on.** Archived arm `_rap_prm_nom14_qf` (2026-10-09: the field golden plus `phase_datum_eq34` and `_carry_shift`, the floor at its default, 2 h) reads after hour 1: p1 -5.24, p99 25.06 m; GOPE00CZE +29.29, CHUR00CAN +26.85, GOP600CZE and JPLM00USA +11.8 m at a formal 18 to 41 mm; 28 of 58 receivers beyond 1 m; code slope 1.49 m (p90 7.95, 51 of 58 above 0.5 m). The retired posture (`_cm_a0`, carry-invariant vehicle) reads -47 to +44 m. No field arm has the chain on and the floor off, and card IRD section 6 allows no new one before IRD-6 has passed, so the floor is not proved the cause on the field. A one-knob pair would settle it and is the author's to allow.
- **Blind instruments on this bed.** The block-Baarda census reports no block in either arm, because every block is uncheckable on the free bed. RAIM's T4 attribution does react: 64 drop rows with the floor on (26 on S16000SIM, 20 on S23000SIM, 14 on S45000SIM), 0 with it off or at 3 m and 10 m. The RPB time-link NEES reads 0.768 against 0.549.

### 9.7 D0 and D-N

**D0.** The DCB shift against the floor-off arm follows the shift of the geometry-free bias with slope -1 and no fit (correlation, slope at the first epoch and at 2 h): sigma 100 -1.000, -0.99 and -0.962, -0.84; sigma 10 -0.999, -0.99 and -0.988, -0.96; sigma 3 -0.956, -1.08 and -0.976, -0.94; sigma 1 -0.967, -1.73 and -0.885, -1.18, where T's own moves enter. This test was formed after the first table and before the correlation was computed. So the DCB spread that decision D0 records is largely the floor's own, not an independent defect. Card IRDB section 3.5 read "with the floor on or off" before the rows: floor off 0.68, 3.09 m on r3c. With the rows armed the floor-off spread does not grow.

D0's beds with every event row armed (arms A1krhRsm, floor knob only; D0's bars: DCB sd at most 0.17 m, W5 no higher, consumer within +10 % of the floor-on arm in hours 1 and 2):

| bed | floor | consumer h1, h2, m | DCB sd h1, h2, m | W5 | RPB NEES | wet-delay error after hour 1: p50, p99, max, m |
| --- | --- | --- | --- | --- | --- | --- |
| r3c | 1 m | 0.179, 0.111 | 9.15, 9.49 | 0 | 0.746 | 0.290, 5.56, 15.1 |
| r3c | 3 m | 0.222 (+24 %), 0.030 (-73 %) | 3.30, 3.32 | 0 | 0.569 | 0.028, 0.617, 3.13 |
| r3c | off | 0.277 (+55 %), 0.031 (-72 %) | 0.67, 0.70 | 0 | 0.556 | 0.022, 0.733, 4.21 |
| golden sim | 1 m | 0.089, 0.035 | 7.03, 6.94 | 0 | 0.650 | 0.088, 0.874, 1.80 |
| golden sim | 3 m | 0.092 (+3 %), 0.033 (-6 %) | 3.35, 3.38 | 0 | 0.603 | 0.012, 0.087, 0.15 |
| golden sim | off | 0.093 (+4 %), 0.033 (-6 %) | 0.40, 0.40 | 0 | 0.590 | 0.009, 0.056, 0.09 |
| golden sim | no chain | 0.099, 0.033 | 0.07, 0.05 | 0 | n/a | 0.006, 0.028, 0.06 |

No switch passes D0's bars as they stand: the DCB sd stays above 0.17 m on both beds at every setting (off 0.70 and 0.40 m, against 0.05 m with no chain), and on r3c hour 1 stands +24 % and +55 %. The readings have moved, though. Before the rows the floor-off DCB reached 3.09 and 4.24 m by hour 2, and it now holds 0.70 and 0.40 m. On r3c the floor-on consumer in hour 2 is 3.6 times the floor-off one, where on the golden sim the two match. These are readings: D0 is not re-decided here.

**D-N.** Card IRD section 10: "An RPB NEES above 2 on the sim makes IRD-5b owed before the D1 verdict". A0, with no rows, read 1.35, 2.31, 2.58, 1.11 on the microscope, r2, r3c and the golden sim. A1 with every row reads 0.086, 0.766, 0.746 and 0.650 on the same beds. The trigger does not fire on the arm going forward. It is also blind to this state: the bias is held stiffly, so metres of wet-delay error read 0.768. IRD-5b's gate (NEES within [0.5, 2] on r3c and the golden bed) reads inside it; the microscope's 0.086 is below the range and outside the gate's beds.

### 9.8 What bears on the ruling, for the author

The 2026-11-02 ruling closed the certification rule as a float defect from events and the RPB floor, and IRDC section 7.3 holds the certified branch on A1 pending D0 ("the floor biases the floats from the first epoch on every bed"). This section supplies the mechanism of that bias; the ruling's text stands. Three things are the author's to weigh, and none is acted on here:

1. D0's verdict "the floor stays" was measured before the event rows, and they change its readings (section 9.7).
2. D0 offered the floor on, off and birth only. Sigma was not among the switches. On these beds 3 m gives the floor-off reading for the wet delay and I-b, with the DCB spread between the two (3.3 m) and on r3c an hour 1 consumer between the two (0.222 m, against 0.179 at 1 m and 0.277 off). The hour 1 consumer on r3c and its siblings is what the floor buys; on the golden sim all three settings match.
3. The prior's form is open: a zero-mean prior on a bias that holds integer path sums is the inconsistency, whatever its size. Candidates are not built or graded here.

**Not covered.** One fixture for the sweep (`ird_tr`), 2 h; the D0 beds at sigma 0 and 3 only; no new field arm. The decay of section 9.4 is a fit; the first-epoch test is the falsifiable one.
