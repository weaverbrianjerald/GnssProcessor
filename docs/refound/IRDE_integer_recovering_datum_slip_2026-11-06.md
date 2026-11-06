# IRDE: the integer-recovering datum, continued (IRD-4's slip row)

**STATUS**

- **id:** IRDE
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRDD's continuation (that file nears the 60 KB bar), from IRD-4's `slip` row on; card IRD keeps the binding parts and cards IRDB, IRDC and IRDD the earlier results. The `slip` row LANDED default off (entry `slip` of `network.phase_datum_eq34_event_rows`). An arc in the epoch's free-epoch latch leaves the tree at the slip epoch, on both bands of a pair family. The old child re-attaches through a new edge, which the rehost row releases under the hold; a slipped arc no other route can replace stays and the slip row releases it. As built the slipped edge stays pinned: on ird_t1 its target falls to 0 (a known step of +57 and -91 cycles on the subtree), and on every fixture the cycle stays in the arc's phase row (on the microscope twins a block-Baarda outlier with row t up to 697 for the rest of the run, 0 with the row). Off identity passes on six pairs, and the row is inert in every product and trace on three beds with no tree-edge slip. The class's as-built steps fall to 0, both edges carry identities, and the float consumer and W5 pass (hour 2 on ird_t1 0.117 m against 0.122, on ird_tr 0.118 against 0.121; 240 of 240 epochs emitted). I-b FAILED: undeclared integer steps at the slip epoch 508 to 224 on ird_t1 and 213 to 146 on ird_tr, 331 and 6 to 0 on the microscope twins, 324 and 0 to 0 with the floor off; the fractional bar is missed on every graded bed. On ird_t1 the integer steps left are band noise and a clock-anchor decoupling that the slip-free control has too. On ird_tr three undeclared satellites also step, non-dispersively, through receivers whose wet delay this posture holds metres off (S45000SIM at 8.4 m, truth 0.13 m). With the floor off, ird_tr passes I-b outright (wide lane max 0.026 cycle, 0 integer steps) and ird_t1 reads 0 integer steps against its slip-free control. The release costs what the rehost row's costs: more double differences above 0.25 cycle for about 20 min (floor-off 0.112 to 0.187 at the slip epoch on the larger moved side). The consumer clause is a weak test here: the phase-bias hold keeps the emitted bias of a moved key unchanged, so on the microscope beds the consumer differs from the parent by 1e-4 m.
- **source:** [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 4 (item 4), section 5 (the `TreeEdgeSlip` row), section 6 (the fixtures) and section 11 (IRD-4's order and gate); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 7.3 (the hold on certified re-hosts); [IRDD](IRDD_integer_recovering_datum_rows_2026-11-03.md) sections 3 to 5 (the earlier rows, whose gate form this card follows); [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md) section 3.7 (the fixtures' tree edges)
- **knobs / posture:** the `slip` entry of `network.phase_datum_eq34_event_rows` (default empty; ledger row UNVALIDATED, its text updated); arms A1krhRs, A0fkrhRs and A0fkrRs of `scripts/ird_make_arms.py`; beds `ird_t1m` and `ird_trm` (rung 1c with zero-ionosphere rows and the scripted slip of t1 and tr; configs `config/sim/ird/ird_{t1m,trm}_2h.json`). No golden change
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird4slip.exe` (sha256[:16] `e105882041a409da`: `104b103` plus the row); arms `_rap_ird4s_*`, and the parent on the root row's exe `_rap_ird4s0_a1krhR_{t1,tr}`; fixtures `simird_{t1m,trm}` (each with a `GENERATED.txt`; working files in `_ird2_gen/mini_{t1m,trm}*`). All under `D:/GnssProcessorSupport/runs_nom/`
- **gates (frozen at pre-registration):** card IRD section 11 (the row's gate on ird_t1 and ird_tr, cumulative on A1); the microscope twins and the floor-off arms are readings

**Outcome.** Question: does IRD-4's `slip` row pass its gate on ird_t1 and ird_tr? It LANDED default off: a slipped tree edge leaves the tree at the slip epoch, the class's as-built steps fall to 0, both edges carry identities and the consumer holds its bound. I-b FAILED: undeclared integer steps 508 to 224 and 213 to 146, 0 on the microscope twins, and the fractional bar is missed; with the floor off ird_tr passes. The consumer clause is weak here (the hold). Standing: `merge_split` is next in table order, and no bed reaches that class yet.

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
