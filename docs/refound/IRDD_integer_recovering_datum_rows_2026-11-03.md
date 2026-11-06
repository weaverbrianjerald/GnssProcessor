# IRDD: the integer-recovering datum, continued (IRD-4's event rows)

**STATUS**

- **id:** IRDD
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRDC's continuation (that file reached the 60 KB bar), from the trace of the epoch-88 divergence on; card IRD keeps the binding parts, card IRDB the results of IRD-1 and IRD-2, card IRDC the event causes, IRD-3, the certification rulings and IRD-4's kept row. The epoch-88 divergence of IRDC section 8.4 is traced (section 2): it is the zero-target cold pin of card IRD C6, not the kept row's rule. At the only cold re-host after convergence (S45000SIM onto a first-sighting arc of G32), both arms take the same decisions, and the zero target meets the old frame that the subtree's surviving chords keep (C8). The conflict is the arc's whole old-frame float: -21.98 / +31.02 cycles as built and +42.02 / -86.98 with the row, which differ by exactly the integer the row's frozen targets left at S45000SIM (64 / -118); its fraction is 0.02 cycle. A scratch diagnostic that changes only this pin's target moves every reading with the integer, the consumer included; with the target rounded to the old-frame float, the event is free, the double differences are integer-clean from 70 min in both arms (reference-free share above 0.25 cycle 0.00) and the two arms agree to 0.006 cycle. The epoch-210 re-host of IRDC section 3 is this pin's aftermath too. On the A1 beds the row leaves the pin's integer unchanged, so the r3c consumer failure of IRDC section 8 is not this mechanism. IRD-4's `returning` row (section 3) LANDED default off: every returning edge on ird_rv16, the microscope and r3c is released with its cut and its declared keys identified, and its legacy known zero step is gone. At the golden's 900 s horizon the floor-off microscope's wire is byte-identical to the 600 s horizon arm's (the return shock of IRDC section 2 removed), and the search's wrong would-be fixes halve on both microscope beds. I-b FAILED for the class on every bed (the background drift; on the microscope 0 undeclared integer steps against 154 and 72), and the consumer holds its bound on every bed. The `rehost` row (section 4) LANDED default off, its certified branch held, so every re-host takes the cut release: 144 re-hosts released on each ARL bed (128 known fractional steps to 0), off byte-identical in every output. It removes the producer's event damage: on the floor-off microscope no double-difference creep follows epochs 88 and 210 (last bins 0.008 to 0.015 cycle against 0.080 to 0.117), and the search's wrong would-be fixes fall from 3 282 to 20. The float consumer FAILED in hour 2 on every bed (about twice the previous arm, +21 to +69 mm), as did I-b; the releases erase 2 944 converged carries in 2 h. The author chose to continue in table order with the hold as built (2026-11-05). The `root` row (section 5) LANDED default off: a new root's receiver phase bias is pinned at its carried value, so the 14 satellites on its side keep their frame and carries; the 44 unknown steps of the class go to 0 and its undeclared integer steps fall from 235 to 262 to 0, off identical in every product and trace, the float consumer within its bound on every bed. With no release at the same epoch the double differences do not move at the handover (0.052 cycle before, 0.045 at it, against 0.451 as built). I-b FAILED on the fractional bar on every bed, by 2 to 11 of 91 pairs, at the size the same pairs reach with no event.
- **source:** [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) sections 3.4, 8.4 and 8.5 (the open item); [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 2 (C6, C8), section 5 (the `RehostUncertified` row) and section 11 (IRD-4's order and gates)
- **knobs / posture:** section 2: no producer code change; the arms are IRDC section 8.4's A0f and A0fk on the microscope with `network.phase_leveling_max_gap_s` 600; a scratch diagnostic, not committed, adds a whole number of cycles to the target of one cold re-host pin (section 2.3). Section 3: the `returning` entry of `network.phase_datum_eq34_event_rows` (default empty; ledger row UNVALIDATED, its text updated); arms A1kr and A0fkr of `scripts/ird_make_arms.py`. Section 4: the `rehost` entry of the same knob (its certified branch held; `network.phase_datum_eq34_rehost_cert` not built); arms A1krh and A0fkrh. Section 5: the `root` entry of the same knob; arms A1krhR, A0fkrhR and A0fkrR and the bed `ird_rg_mic`. No golden change
- **dates in the text:** 2026-11-03, 2026-11-05
- **hashes / run dirs:** section 2: branch `claude/distracted-germain-d93ac2` at `65ef3ca`; native arms `_rap_ird4b_{tr,trk}_mic` (exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird4k2.exe`, sha256[:16] `4266492fc70cedeb`); diagnostic exe `gnss_processor_ird4dx.exe` in the same folder (sha256[:16] `3de7d0ca2b383fdf`: `65ef3ca` plus the patch kept beside it as `gnss_processor_ird4dx_patch.py`), arms `_rap_ird4x_{tr,trk}_{swap,round}_mic`; the readouts of section 2 in `_ird4x_tools/`. Section 3: exe `gnss_processor_ird4r.exe` in the same folder (sha256[:16] `43e36cf987770c3d`: `0c15644` plus the row), arms `_rap_ird4r_*`. Section 4: exe `gnss_processor_ird4h.exe` in the same folder (sha256[:16] `69821d23f490be77`: `8c68cc1` plus the row), arms `_rap_ird4h_*`. Section 5: exe `gnss_processor_ird4root.exe` in the same folder (sha256[:16] `71d2d73ad65398fe`: `2f18696` plus the row), arms `_rap_ird4root_*`. All arms under `D:/GnssProcessorSupport/runs_nom/`
- **gates (frozen at pre-registration):** card IRD section 11 (IRD-4's per-row gate, its order and the hold on certified re-hosts); section 2 is a trace with no gate of its own; sections 3, 4 and 5 grade the `returning`, `rehost` and `root` rows' gates

**Outcome.** Questions: why does the kept row's arm part from the as-built arm at the epoch-88 cold re-host (IRDC section 8.4), and do the `returning`, `rehost` and `root` rows pass their gates? The epoch-88 cause is card IRD C6, the zero-target cold pin, not the row (section 2). The `returning` row LANDED default off: it removes the return shock, giving the 600 s arm's wire byte for byte; I-b FAILED (section 3). The `rehost` row LANDED default off with its certified branch held: it removes the C6 and C8 double-difference damage, but the float consumer FAILED in hour 2 on every bed, about twice the previous arm, as did I-b (section 4). The `root` row LANDED default off: it removes the handover's frame step and its undeclared integer steps, the consumer holds its bound, and I-b FAILED on the fractional bar at the size the same pairs reach with no event (section 5). Standing: the author chose to continue in table order with the hold as built; `slip` is next.

## 1. What this card continues

Card IRDC reached the 60 KB bar at its section 8 (IRD-4's kept row). Its section 8.5 left one item open: on the floor-off microscope the kept row's arm gains on the consumer and loses on the producer's double differences from the cold re-host at epoch 88, an event the row leaves to the as-built rule. This card holds IRD-4 from that trace on. Card IRD section 11 keeps the binding plan: the rows armed in table order (`kept`, `returning`, `rehost`, `root`, `slip`, `merge_split`), each row's gate, and the hold on certified re-hosts (IRDC section 7.3).

## 2. 2026-11-03: the epoch-88 divergence traced (the zero-target cold pin, card IRD C6)

### 2.1 The event

The two arms of IRDC section 8.4, A0f and A0fk (A0f plus the kept row) on the microscope with `phase_leveling_max_gap_s` 600 (`_rap_ird4b_{tr,trk}_mic`), take the same decisions at epoch 88 (TOW 263640). It is the only cold re-host after convergence in either arm, and in the four A1 arms of IRDC section 8.3.

- S45000SIM re-hosts onto a first-sighting arc of G32: `rehost_cold` on a `RehostUncertified` row, target 0, step unknown.
- Its edge to G24 reverses: G24 is re-attached from S29000SIM below S45000SIM. The as-built rule reads the reversed edge as an unknown step; the kept row holds it at its target (111 / -113 cycles), but G24's step is unknown in both arms, because its new parent's is.
- G24 and G13 are re-referenced (four `eq34_reref` keys), so their leveling carries are erased at every station. The six receivers below the new edge (S45000SIM, and S35000SIM, S40000SIM, S48000SIM, S53000SIM and S56000SIM below G24) lose their RPB carries (sigma 0.0096 m before, 0.11 to 0.13 m after). Their chords to satellites outside the subtree survive in the old frame (card IRD C8).

### 2.2 The conflict

The zero target replaces the cold arc's float in the old frame, which the surviving chords keep implying. In a chain frame every arc's float is a_rs = N_rs + o_r - o_s (o: a vertex's frame offset), so for any satellite s tracked by R = S45000SIM and by a station Q that also tracks K = G32,

    Z = N_RK + (a_Rs - N_Rs) - (a_Qs - N_Qs) + (a_QK - N_QK),

with the floats of epoch 87 (chords from `network_chain_frame.csv`, tree edges at their targets in `network_chain_edges.csv`) and the fixture's truth integers. Each arm's 21 (Q, s) estimates agree within 0.09 cycle (`_ird4x_tools/cold_conflict.py`).

| arm | Z, L1C / L2W (cycles) | Z, L1C / L2W (m) |
| --- | --- | --- |
| A0f | -21.98 / +31.02 | -4.18 / +7.58 |
| A0fk | +42.02 / -86.98 | +8.00 / -21.24 |

The two differ by exactly 64 / -118 cycles, S45000SIM's integer frame offset between the arms: its RPB differs by -12.179 / +28.817 m, the frozen target of its edge to G24 (111 / -113), which the as-built rule re-pins to 0 at epoch 81, less G24's own 47 / 5 cycles (IRDC section 8.4). G32's frame is the same in both arms (its chords at 13 stations agree within 0.003 cycle, one within 0.08). The fraction is 0.020 / 0.016 cycle in both.

The solve leaves most of the conflict on the pinned arc's own phase rows: post-fit -5.07 / +6.31 m in A0f and +11.17 / -17.12 m in A0fk. The rest goes to the subtree's other phase rows (the largest 0.56 against 2.45 m) and its priors: 152 against 452 prior residuals above 4 sigma at the epoch, and 0 against 3 RAIM refloats.

### 2.3 The causal arms

A scratch diagnostic, not committed (`65ef3ca` plus 32 lines in `ChainDatum.cpp`), adds a whole number of cycles to the target of one cold re-host pin. The pin is chosen by station and satellite through the environment variable `GNSS_DIAG_COLD_PIN`; unset or unmatched, it changes nothing. In every arm the diagnostic fired once per band at epoch 88. Each arm's trace rows before epoch 88 are byte-identical to its native arm's (40 trace files, about 615 000 rows), apart from two wall-clock columns and the run-end loop time.

- **Swap:** each arm takes the other's conflict (A0f target -64 / +118 cycles, A0fk +64 / -118).
- **Round:** each arm takes target round(Z), which leaves the 0.020 / 0.016 cycle fraction (A0f -22 / +31, A0fk +42 / -87). Z is read from truth, so these arms bound what a rounded target can do; they are not a producer rule.

| arm | kept row | conflict, cycles L1C / L2W | pinned arc's post-fit residual at epoch 88, m | prior residuals above 4 sigma at epoch 88 | RAIM T4 drops, epochs 88 to 160 / run | consumer 3D p50, hour 2, m | producer DD \|e\| p50, cycles | reference-free p50 / share above 0.25: run; last 10 min | stop rule, event-free chords L1C / L2W |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A0f | off | -21.98 / +31.02 | -5.07 / +6.31 | 152 | 58 / 230 | 0.064 | 0.040 | 0.022 / 0.076; 0.036 / 0.15 | 0.090 / 0.065 |
| A0fk swapped | on | -21.98 / +31.02 | -4.90 / +6.48 | 182 | 52 / 128 | 0.064 | 0.036 | 0.020 / 0.069; 0.030 / 0.12 | 0.098 / 0.065 |
| A0fk | on | +42.02 / -86.98 | +11.17 / -17.12 | 452 | 182 / 708 | 0.022 | 0.071 | 0.037 / 0.156; 0.120 / 0.36 | 0.196 / 0.188 |
| A0f swapped | off | +42.02 / -86.98 | +11.82 / -16.48 | 373 | 156 / 848 | 0.017 | 0.077 | 0.041 / 0.168; 0.128 / 0.36 | 0.191 / 0.174 |
| A0f rounded | off | +0.020 / +0.016 | +0.028 / +0.021 | 38 | 0 / 0 | 0.043 | 0.014 | 0.0085 / 0.023; 0.004 / 0.00 | 0.026 / 0.020 |
| A0fk rounded | on | +0.020 / +0.016 | +0.032 / +0.025 | 48 | 0 / 0 | 0.043 | 0.012 | 0.0074 / 0.022; 0.003 / 0.00 | 0.034 / 0.026 |

Hour 1 of the consumer reads 0.125 m in every arm. The fractional agreement of each pair, over the chord floats both arms carry (|frac(a - b)| p50 / p90, cycles; 0.003 / 0.062 at epoch 87 in every pair):

| pair | epoch 88 | epoch 120 | epoch 160 | epoch 240 |
| --- | --- | --- | --- | --- |
| A0fk against A0f (conflicts 64 / -118 apart) | 0.026 / 0.091 | 0.153 / 0.417 | 0.235 / 0.445 | 0.254 / 0.454 |
| A0fk swapped against A0f (equal conflicts) | 0.003 / 0.064 | 0.013 / 0.126 | 0.018 / 0.190 | 0.057 / 0.347 |
| A0f swapped against A0fk (equal conflicts) | 0.007 / 0.064 | 0.021 / 0.304 | 0.039 / 0.308 | 0.138 / 0.430 |
| A0fk rounded against A0f rounded | 0.003 / 0.062 | 0.004 / 0.069 | 0.003 / 0.061 | 0.006 / 0.025 |

With equal conflicts the arms agree at the event and part slowly afterwards, through the refloats the large conflict triggers; with no conflict they stay together to the run end. At epoch 160 the subtree receivers' RPBs agree in fraction within 0.006 cycle in the rounded pair, against up to 0.46 cycle in the swapped pairs.

### 2.4 The epoch-210 re-host

IRDC section 3.5 gave the other half of the end-of-run share to the flipped kept edge at epoch 210, whose re-host through S42000SIM-G28 takes that chord's float error as its step. The step is the epoch-88 aftermath: 0.288 / -0.472 cycle in A0f (`RehostUncertified`) against -0.015 / -0.013 cycle in A0f rounded, where the same re-host is certified. The rounded arm takes the same decisions at epoch 210 (the same reattachments, six satellites re-referenced), and its share above 0.25 stays at 0.00.

### 2.5 Reading

1. **The divergence is the zero-target cold pin's integer (card IRD C6), not the kept row's rule.** The two arms decide epoch 88 alike. Their conflicts differ by exactly the integer the row's frozen targets left at S45000SIM, and the swap carries every reading with that integer, the consumer included. With the integer gone the arms agree to 0.006 cycle at the run end. IRDC section 8.4's floor-off reading (consumer gain, producer loss) and its section 8.5 item 3 are this integer. With equal conflicts the row reads 0.064 against 0.064 m on the consumer and 0.036 against 0.040 cycle on the producer's double differences, with 128 RAIM T4 drops against 230.
2. **The as-built cold re-host is not gauge-invariant.** It pins the new arc at 0 while the subtree's outside chords keep the old frame (C8), so the solve carries the arc's whole old-frame float: 4 to 21 m here, of which 0.02 cycle is the fraction. Its damage scales with an integer that any change to the frame's integer gauge moves.
3. **With the integer removed, the event is free.** The pinned arc's residual is 3 cm, RAIM refloats nothing in the whole run, and the double differences are integer-clean from 70 min: reference-free share above 0.25 cycle 0.00, residual p50 0.004 cycle, DD \|e\| p50 0.012 to 0.014 against 0.040 as built. The epoch-210 re-host then steps by 0.015 cycle and is certified (section 2.4). On this 600 s arm, which cuts the satellite-return path (IRDC section 2), every post-convergence frame step that damaged the double differences in IRDC section 3 traces to this one pin. The stop rule reads 0.026 / 0.020 cycle as built (bar 0.020; A0f 0.090 / 0.065).
4. **The consumer's hour 2 follows the conflict without ordering it.** It reads 0.064 m with the 4 to 8 m conflict, 0.017 to 0.022 m with the 8 to 21 m conflict and 0.043 m with none. On this bed the double differences grade the event, and the consumer by hour does not.
5. **On the A1 beds the row leaves this integer unchanged.** At the same pin, Z is -52.3 / -179.3 cycles on r3c (medians, with the estimates spread by about 12 cycles under the RPB floor) and +41.20 / -87.87 on the microscope, alike with and without the row: under the golden's 900 s horizon the as-built target of S45000SIM-G24 stays at its epoch-61 re-host value through epoch 87 (its chord carry would be pruned at epoch 91). Section 8's r3c consumer failure is therefore not this mechanism. Both A1 beds carry a conflict of 8 to 44 m at this pin, in both arms.

### 2.6 Standing

No producer code changed. The epoch-88 divergence is traced to card IRD C6: the zero-target cold pin's integer, which the kept row's frozen targets moved by 64 / -118 cycles on the floor-off bed. The row is cleared of it, and IRDC section 8.4's floor-off reading is this integer's. The kept row's gate verdicts (IRDC section 8.3) are not re-graded here: I-b and the r3c consumer stay FAILED.

The fix is the `rehost` row's uncertified branch (card IRD section 5: a rounded integer target, a cut release, a declaration). On this bed its oracle bound is a free event. Without truth the row must round the new arc's float in the old frame. One candidate is the station's post-update float, read where the fold-deferred seed-round target is read (`PhaseDatum.cpp:1524`); whether it lands on round(Z) on the free-ionosphere beds is untested and belongs to that row's gate. IRD-4 continues in card IRD section 11's order with the `returning` row.

## 3. 2026-11-03: the `returning` row built and gated

### 3.1 The rule as built

The second row in table order (card IRD section 5): legacy "baseline 0: pinmove lambda round(carry), marked known"; integer rule "certified from its own previous carries; else cut release and treated as a birth". The certified branch is held (IRDC section 7.2: no certification is consumed before it grades 0 wrong), so the row as built is the cut release for every returning edge. It is the entry `returning` of `network.phase_datum_eq34_event_rows`; without it every output is unchanged.

- **Selection.** A tree edge is released when the kept row did not hold it, its child vertex was absent from the last election's tree on the band (a satellite: no incumbent anchor; a receiver: no incumbent parent and no root of the last election), the child was a vertex of an earlier election's tree on the band, and its arc is not in the epoch's free-epoch latch (the slip row's case, counted as left to it). The memory of earlier trees is a new flag per band, `eq34_tree_seen`, on the satellite-signal and the station-signal entities (`Persist::ColdStart`, written only with the row armed): after a warm resume the first election's returns are births.
- **The pin.** The released edge takes the first-sighting target (0 on these arms, which run with the seed round off; the seed round's target when armed) in place of a target read from the arc's old-frame carry, with an unknown step.
- **The cut release.** The shift walk treats the edge's subtree, the moved side, as unknown: its satellites are re-referenced (their leveling carries erased at every station, their phase-bias priors marginalized) and its receivers' RPB carries dropped. Then every leveling carry of a receiver of the moved side on the band is erased, to satellites present or absent, so no chord to a satellite outside the cut survives in the old frame.
- **Identities (invariant 7).** Per released edge, a `leave` row (the vertex and the parent it had when last seen) and a `join` row (the new edge) under the event `returning`; per release, a `cut_release` row (signal, component, root, leveling carries erased, RPB carries dropped, satellite phase-bias priors, receivers and satellites of the moved side); per satellite key of the moved side, a `declared_keys` row (`cause=returning`), the rows the frame grader reads as declarations. The election's run-end eq34 block and the chain-frame block count each of them, and the as-built step each release replaced.
- **One fix at review, before the arms.** For the next election's kept-row comparison the observatory remembers a released edge's realized target, not its as-built draft: the release erases the old-frame carry the draft read, and remembering the draft would report the release itself as an as-built step one election later.
- **Tests.** 14 new unit tests and 4 updated, among them a random-lineage oracle (24 lineages, 960 elections, 3 949 releases) against an independent statement of the rule; the latch and former-root exclusions are checked by mutation. The full Debug suite: 3 768 run, 3 735 passed, 33 skipped (fixtures absent), 0 failed. `nom_epoch_lint.py`, `nom_n4_verify.py --whole`, `counters_split_check.py`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader` and `ird_make_arms.py --selftest` pass.

### 3.2 The arms

One exe, `gnss_processor_ird4r.exe` (sha256[:16] `43e36cf987770c3d`: `0c15644` plus the row), 2 h arms under `D:/GnssProcessorSupport/runs_nom/`:

- **Off identity:** A1 on r3c, the golden on the sim bed and A1k on r3c (`_rap_ird4r_{off_a1_r3c,off_gs,a1k_r3c}`) against the kept row's arms of the same names (`_rap_ird4b_*`, exe `gnss_processor_ird4k2.exe`).
- **The row:** A1kr (A1k plus `returning`) against A1k on ird_rv16, the microscope and r3c (`_rap_ird4r_{a1k,a1kr}_{rv16,mic,r3c}`); and the causal pair, A0fkr against A0fk on the floor-off microscope at the golden's 900 s horizon (`_rap_ird4r_{a0fk,a0fkr}_mic9`). Each armed config differs from its parent's in the event-row list alone.

The class on these beds: on every ARL bed G20 returns at TOW 262920 (under S04000SIM) with old-frame carries at 15 stations per band, and G12 at 267720 (under S06000SIM) with none left. ird_rv16 adds S45000SIM's return at 266520 after its 990 s gap, also with none left (the 900 s horizon pruned them). ird_rv6's return is a slip-row event (IRDB section 3.7), so ird_rv16 is the row's named fixture.

### 3.3 The gate

Off identity passes on all three pairs: the products and every trace CSV are identical, and both logs differ only in the entity inventory (114 declared fields against 112, and the two `eq34_tree_seen` rows, at 0 rows), by a sequence diff under the masks of `scripts/nom_td_gate.py`.

| clause | ird_rv16 | microscope (A1) | floor-off microscope | r3c |
| --- | --- | --- | --- | --- |
| the class reaches n >= 1 | 6 edges at 3 events | 4 at 2 | 4 at 2 | 4 at 2 |
| legacy or fractional steps fall to 0 | pass: 6 known zero steps to 0; all 6 under the rule, step unknown and declared | pass: 4 to 0 | pass: 4 to 0 | pass: 4 to 0 |
| I-b for the class (previous arm in brackets) | FAILED: E1 p50 0.064 cycle (non-event 0.085), 283 undeclared integer steps (0.059, 215) | FAILED: E1 p50 0.0081 (non-event 0.0094), E1 max 0.35 (180 pairs above 0.02), 0 undeclared integer steps (0.057, 154) | FAILED: E1 p50 0.0065 (non-event 0.0073), E1 max 0.14 (164 pairs above 0.02), 0 undeclared integer steps (0.050, 72) | FAILED: E1 p50 0.176 (non-event 0.128), 349 undeclared integer steps (0.159, 272) |
| both edges with identities | pass: 6 leave and 6 join rows, 6 `cut_release`, 4 `declared_keys` | pass: 4 and 4, 4, 4 | pass: 4 and 4, 4, 4 | pass: 4 and 4, 4, 4 |
| float consumer by hour, m (previous; bound max(+10 %, +5 mm)) | pass: 0.417, 0.048 (0.417, 0.049) | pass: 0.149, 0.021 (0.149, 0.026) | pass: 0.125, 0.022 (0.132, 0.048) | pass: 0.417, 0.050 (0.417, 0.052) |
| W5 no higher | pass: 0 against 0, 240 of 240 epochs emitted | pass | pass | pass |

On every bed the cut erased 30 leveling carries, all at G20's return (15 per band). The moved sides of G12 and S45000SIM held none, and no moved side held an RPB carry or a satellite phase-bias prior in the border stash (a satellite absent at the previous epoch has none there). The legacy re-references rise from 16 to 20 on every pair: the 4 declared keys. The kept row's hold rows are unchanged on ird_rv16 and r3c (96 and 90); on the microscope there is one more (90 against 89), at 264240 on G16 at S47000SIM, where the frozen target differs by one cycle between the arms after the release.

I-b is graded as for the kept row (IRDC section 8.5 item 5). On the floor beds (ird_rv16 and r3c, which keep the RPB floor) the class reads at the background drift in both arms. On the microscope the class's own step is at the non-event median and no integer step is undeclared; the bars fail on the background (26 417 non-event pairs above 0.02 cycle on E1 on the A1 microscope arm, 20 178 on the floor-off arm). The G20 pairs are not graded at the return epoch, where they have no previous value; the declarations are therefore exercised by the identities, not yet by I-b.

### 3.4 The causal reading: the return shock removed

IRDC section 2 traced the shock at the first satellite return: the chain read G20's return as a first sighting with a known zero step and the float vehicle folded its old-frame carries; a 600 s horizon, which prunes them, removed it. At the golden's 900 s horizon the row removes it (reference-free double differences, `scripts/ird_dd_drift.py reffree`; residual \|e\| p50 / share above 0.25 cycle):

| arm | 262890 (before) | 262920 (the return) | 263100 | run |
| --- | --- | --- | --- | --- |
| A0fk | 0.023 / 0.009 | 0.028 / 0.065 | 0.053 / 0.131 | 0.089 / 0.288 |
| A0fkr | 0.023 / 0.009 | 0.019 / 0.007 | 0.016 / 0.004 | 0.038 / 0.155 |
| A1k, microscope | 0.046 / 0.094 | 0.059 / 0.143 | 0.062 / 0.189 | 0.112 / 0.328 |
| A1kr, microscope | 0.046 / 0.094 | 0.044 / 0.090 | 0.038 / 0.090 | 0.061 / 0.215 |

- **The same wire as the 600 s horizon.** A0fkr's products, consumer trace and double-difference trace are byte-identical to those of the kept row's 600 s arm `_rap_ird4b_trk_mic` (sha256[:16] `3b86ea116ca73de7`, `ffeb67b777cd7223`, `128177cefa35b662`); their carry-state and search traces differ, because the release erases at the return what the horizon pruned during the gap. On this bed the cut release does to the wire exactly what the shorter horizon did, at the golden's horizon.
- **The search.** The measure-only search's would-be fixes, graded against truth (none is applied): wrong 6 182 to 3 282 on the floor-off bed and 7 208 to 3 973 on the A1 microscope; right 115 205 to 143 133 and 110 702 to 133 693. The largest wrong ones left are S45000SIM-G32 chords from 263640, 85 to 87 cycles off: the epoch-88 cold pin.
- **What remains.** On the floor-off bed the share above 0.25 cycle stays at 0.01 through the 10 min bin of the return and rises from the next, which holds the epoch-88 cold re-host (0.08 there, 0.36 in the last bin against 0.53). The row leaves that pin's conflict unchanged (section 2.2's readout at TOW 263610: +42.02 / -86.98 cycles against +42.01 / -86.99 on the floor-off bed, +41.22 / -87.86 against +41.20 / -87.87 on the A1 microscope).
- **The consumer.** Hour 2 0.048 to 0.022 m on the floor-off bed, the 600 s arm's value, and hour 1 0.132 to 0.125 m; 0.026 to 0.021 m on the A1 microscope (A1: 0.024 m).
- **The free beds.** On ird_rv16 and r3c the reference-free residual p50 is 2.4 and 2.9 cycles in both arms (raw wrong share 0.93 to 0.97): there this reading does not grade integers, and it does not move.

The static-algebra stop rule (card IRD section 10; ruled closed, IRDC section 4) reads FAIL on every arm, event-free chords p50 0.21 / 0.21 cycle on the A1 microscope (A1k 0.21 / 0.22) and 0.20 / 0.19 on the floor-off bed (A0fk 0.21 / 0.22).

### 3.5 Reading

1. **The row is the cut release the card specified, and it acts where the class lives.** Every returning edge on the four beds is released, with both edges, the cut and the declared keys identified, and its legacy known zero step is gone.
2. **It removes the return shock of IRDC section 2 at the golden's horizon.** On the floor-off bed its wire equals the 600 s arm's byte for byte, the double differences take no step at the return, and the search's wrong would-be fixes halve on both microscope beds.
3. **I-b FAILED for the class on every bed**, as for every class before any row: on the microscope the class's own step is at the non-event median with no undeclared integer step, and the bars fail on the background drift; on the floor beds the class reads at the background.
4. **No regression against A1k.** The consumer holds the bound on every bed and gains on the microscope; W5 is 0. Against A1, the kept row's r3c failure stays (hour 2 0.050 m against A1's 0.042 and the bound 0.047): its mechanism is the re-host estimate a reversed subtree inherits (IRDC section 8.5), the `rehost` row's.
5. **What remains on the microscope starts at the epoch-88 cold pin** (card IRD C6, section 2), whose conflict the row leaves unchanged.

### 3.6 Standing

The `returning` row LANDED default off; the ledger row of `network.phase_datum_eq34_event_rows` stays UNVALIDATED with its text updated, and the certified branch stays held. Of its gate: off identity (with the two inventory rows declared), the class's legacy steps, both edges' identities and the float consumer pass, and W5 is 0; I-b FAILED on every bed. Next in table order: `rehost`, whose uncertified branch carries the C6 fix (section 2.6) and whose certified branch stays held.

## 4. 2026-11-03: the `rehost` row built and gated

### 4.1 The rule as built

The third row in table order is the entry `rehost`, which names two rows of card IRD section 5: RehostCertified ("the chord's integer from the t-1 chord table, jointly certified; no shift") and RehostUncertified ("rounded integer target, cut release, declaration"). Under the author's hold (card IRDC section 7.3) every re-host takes the uncertified rule and the election reads no certification; the chord table and its would-be certification are still written and graded. Without the entry every output is unchanged.

- **Selection.** A tree edge is released when no earlier row decided it (the kept row did not hold it, the returning row did not release it), its child had a parent at the last election (a satellite: an incumbent anchor; a receiver: an incumbent receiver-side parent on the band), its arc was no tree edge then in either direction, and the arc is not in the epoch's free-epoch latch (the slip row's case, counted as left to it). These are the observatory's re-host class. The three reads of the last election's tree are one helper, shared by the kept row, this row and the trace.
- **The pin.** The draft's target stays: it is already an integer, lambda times the rounded carry of the new host on a carried re-host, and 0 (or the seed round's target) on a cold one. The step is unknown.
- **The cut release.** The returning row's (section 3.1): the moved side's satellites re-referenced, its receivers' RPB carries dropped and their leveling carries erased. Each release carries a typed tag of the row that released it, and the counts of what it took go to that row's counters.
- **The observatory.** RehostUncertified is built. RehostCertified is held: the event table gains a `held` flag, an entry is taken when every row naming it is built or held and one is built, and an armed held row's edges take the entry's built row's rule (the run-end table says so). The `rehost` leave and join rows of a released edge append the as-built tokens; `declared_keys` rows carry `cause=rehost`. `network.phase_datum_eq34_rehost_cert` (card IRD section 7) is not built: its held value is `held`, and the knob is built with the certified branch's release, with the values `held` (default), `pair` and `pair_lattice`.
- **One fix at review, before the arms.** With both releasing rows armed, the returning row's run-end line counted the rehost releases' `cut_release` and `declared_keys` rows; each line now counts its own.
- **Tests.** 7 new oracle cases (among them a random-lineage oracle of 8 lineages that restates the rule and checks it against the observatory's class), 5 observer and run-end cases, a loader case and 3 updated tests. The full Debug suite: 3 781 run, 3 748 passed, 33 skipped (fixtures absent), 0 failed, before the review fix; the chain-frame suite 27 of 27 after it. `nom_epoch_lint.py`, `nom_n4_verify.py --whole`, `counters_split_check.py`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader`, `simnet_chain_frame_grade.py --selftest` and `ird_make_arms.py --selftest` pass.

### 4.2 The arms

One exe, `gnss_processor_ird4h.exe` (sha256[:16] `69821d23f490be77`: `8c68cc1` plus the row), 2 h arms under `D:/GnssProcessorSupport/runs_nom/`:

- **Off identity:** A1 on r3c, the golden on the sim bed, and A1kr on r3c, the microscope and ird_rv16 (`_rap_ird4h_{off_a1_r3c,off_gs,a1kr_r3c,a1kr_mic,a1kr_rv16}`) against the returning row's arms of the same names (`_rap_ird4r_*`, exe `gnss_processor_ird4r.exe`).
- **The row:** A1krh (A1kr plus `rehost`) against A1kr on r3c and the microscope (the row's fixture beds) and on ird_rv16; and A0fkrh against A0fkr on the floor-off microscope at the 900 s horizon (`_rap_ird4h_{a1kr,a1krh}_{r3c,mic,rv16}`, `_rap_ird4h_{a0fkr,a0fkrh}_mic9`). Each armed config differs from its parent's in the event-row list alone.

The class on these beds: 144 re-host edges at 47 epochs on every ARL bed (152 at 48 on ird_rv16), 128 onto a carried arc and 16 cold. The previous chord table would certify 88 of them on the A1 microscope and 66 on the floor-off bed, none on r3c and ird_rv16.

### 4.3 The gate

Off identity passes on all five pairs, byte-identical in the products, every trace CSV, `stdout.txt`, the main log and the consumer's log (no entity field is added).

| clause | microscope (A1) | floor-off microscope | r3c | ird_rv16 |
| --- | --- | --- | --- | --- |
| the class reaches n >= 1 | 144 edges at 47 epochs | 144 at 47 | 144 at 47 | 152 at 48 |
| legacy or fractional steps fall to 0 | pass: 128 known fractional steps to 0; all 144 under the rule, step unknown and declared | pass: 128 to 0 | pass: 128 to 0 | pass: 136 to 0 |
| I-b for the class (previous arm in brackets) | FAILED: E1 p50 0.0118 cycle (non-event 0.0128), 2 048 undeclared integer steps (0.0108, 3 021) | FAILED: E1 p50 0.0056 (non-event 0.0062), 1 609 (0.0075, 2 389) | FAILED: E1 p50 0.168 (non-event 0.151), 16 633 (0.145, 15 107) | not graded |
| both edges with identities | pass: 144 leave and 144 join rows, 144 `cut_release`, 130 `declared_keys` | pass: the same | pass: the same | pass: 152 and 152, 152, 134 |
| float consumer by hour, m (previous; bound max(+10 %, +5 mm)) | FAILED in hour 2: 0.144, 0.043 (0.149, 0.021) | FAILED in hour 2: 0.124, 0.045 (0.125, 0.022) | FAILED in hour 2: 0.179, 0.111 (0.417, 0.050) | FAILED in hour 2: 0.186, 0.117 (0.417, 0.048) |
| W5 no higher | pass: 0 against 0, 240 of 240 epochs emitted | pass | pass | pass |

What the releases took over the 2 h, on every ARL bed: 2 944 leveling carries, 204 RPB carries and 130 satellite phase-bias priors, with 130 keys declared (ird_rv16: 3 064, 214, 134 and 134). The largest is the epoch-210 re-host of S42000SIM onto G28 (6 satellites, 11 receivers, 127 carries per band); the epoch-88 cold re-host of S45000SIM onto G32 releases exactly the moved side of section 2.1 (2 satellites, 6 receivers, 57 carries per band). At the re-host epochs the 1 908 declared pairs carry 1 082 integer steps (the moved keys). The kept row's hold rows rise from 90 to 128 on the microscope: inside a released side the kept edges hold their frozen targets while the as-built draft reads the refloated carries, a counterfactual reading with no step on the frame.

### 4.4 The producer and the consumer part ways

On the producer's integers the row removes the C6 and C8 damage of section 2 and card IRDC section 3:

| reading | microscope (A1): previous; row | floor-off microscope: previous; row |
| --- | --- | --- |
| reference-free double differences, run: residual p50 / share above 0.25 cycle | 0.061 / 0.215; 0.044 / 0.174 | 0.038 / 0.155; 0.018 / 0.084 |
| the same, last four 10 min bins, residual p50 | 0.076 to 0.117; 0.030 to 0.056 | 0.080 to 0.117; 0.008 to 0.015 |
| the search's would-be fixes (measure-only, against truth): wrong, right | 3 973, 133 693; 723, 165 195 | 3 282, 143 133; 20, 177 857 |
| stop rule, event-free chords p50 L1C / L2W, cycles (bar 0.020) | 0.206 / 0.205; 0.072 / 0.045 | 0.196 / 0.188; 0.021 / 0.017 |
| would-be certification against truth: wrong of gradeable | 11 821 of 52 052; 5 680 of 55 664 | not read; 429 of 52 262 (61 arcs) |

- No creep follows epochs 88 and 210. A release epoch shows a one-epoch spike (epoch 210: share 0.12, back to 0.03 four epochs later on the floor-off bed). The early bins lose while the network converges (floor-off bins 2 and 3: share 0.10 and 0.15 against 0.01).
- On the floor-off bed the search's partner fixes are 140 786 kept with 0 wrong, and the stop rule's L2W reading sits within the bar (L1C 0.001 cycle over). The stop rule stays ruled closed (card IRDC section 4); the reading is recorded, not graded.
- The free beds' double differences stay far from integers in both arms (reference-free p50 2.4 to 2.9 cycles).

On the consumer the row loses where the network has converged:

- **No added resets.** The consumer's datum discontinuities fall (clock 95 to 11 and code bias 256 to 88 on the A1 microscope; 733 to 155 and 1 504 to 368 on r3c), and its phase-bias hold generations are equal (4 in each arm).
- **Slower convergence.** 3D p50 per 10 min bin, bins 7 to 11: 0.038, 0.023, 0.018, 0.016, 0.019 m against 0.065, 0.048, 0.036, 0.031, 0.029 on the A1 microscope; 0.030, 0.016, 0.010, 0.021, 0.031 against 0.064, 0.051, 0.038, 0.030, 0.026 on the floor-off bed (whose last bin gains).
- **The free beds' ignition gains.** On r3c and ird_rv16 the previous arm's excursion in bins 2 to 4 (0.80, 0.71 and 0.34 m on r3c), the estimated step a reversed subtree inherits (IRDC section 8.5), falls to 0.25, 0.19 and 0.15 m; the bins after it lose (about 0.05 against 0.10 to 0.12 m).

### 4.5 Reading

1. **The row is the card's uncertified rule under the hold, and it acts on every re-host:** 144 released on each ARL bed, none with a fractional step, with both edges, the cut and the declared keys identified.
2. **It removes the producer's event damage.** No double-difference creep follows the epoch-88 cold re-host or the epoch-210 re-host; on the floor-off bed the double differences stay integer-clean to the run end, the search's would-be fixes are wrong 20 times against 3 282, and the chord floats reach the stop bar. It also removes the early excursion on the free beds, the mechanism of the kept row's r3c failure.
3. **It FAILS the float consumer in hour 2 on every bed** (+21 to +69 mm, about twice the previous arm) **and I-b on every bed.** About every 2.5 min a re-host erases its moved side's converged carries (2 944 in 2 h), and the estimates of the moved satellites start over from the new pins. The legacy rule kept that information through its fractional S-transformation. The consumer reads no declaration before IRD-7 and sees no added resets, so what it most likely loses is the precision of the moved satellites' products while they re-converge; that inference is not traced.
4. **The card's remedy is the certified branch:** a certified re-host keeps its carries with no shift. The chord table would certify 46 to 61 % of the re-hosts on the microscope beds, but those certifications are not 0 wrong (429 of 52 262 chord-epochs on the floor-off bed with the row), so the hold stands.

### 4.6 Standing

The `rehost` row LANDED default off; the ledger row of `network.phase_datum_eq34_event_rows` stays UNVALIDATED with its text updated. Of its gate: off identity (byte-identical in every output), the class's legacy steps, both edges' identities and W5 pass; the float consumer FAILED in hour 2 on every bed, and I-b FAILED on every bed. The author's decision is owed: IRD-4 continues in table order (`root`, then `slip` and `merge_split`) with this row as built, or the hold or the uncertified rule is revisited first.

## 5. 2026-11-05: the `root` row built and gated

The author's answer to the question section 4.6 left ("Continue IRD-4 with the root row, or revisit the hold first?") is "Yes, root next": IRD-4 continues in table order, with the hold and the uncertified rule as built.

### 5.1 The rule as built

The fourth row in table order is the entry `root`, card IRD section 5's RootHandover ("the new root's receiver phase bias pinned at its carried value": step Zero, carries Keep, no wire action). Without the entry every output is unchanged.

- **What the as-built rule does.** The root's pin is a constant the S-basis leaves free, and as built it is 0. A new root's real bias therefore becomes a step of the whole component that the election cannot compute, so every satellite that remembers the old root is marked unknown and re-referenced.
- **Which handovers.** A component of a band is a handover when its datum receiver was no root of the last election and one of its satellites remembers another root. The row pins it when the new root had a parent edge in the last tree on the band (that satellite names the old root), the component keeps the old root (the piece holding the old root's station, else the largest piece by receivers plus satellites, as the observatory reads a split), and the new root has a receiver phase-bias carry inside the horizon. Every other handover runs the as-built rule, counted by its cause.
- **The pin.** The new root's bias folds at the carried value in place of 0, at the same weight. A pinned bias is not refreshed by the carry, so the value is kept while the receiver stays a root (`StationSignalState::eq34_root_rpb_target`, replaced wholesale at each election, ColdStart); a root pinned at 0 stays at 0.
- **The edges.** A satellite edge whose satellite remembers exactly that old root is not marked unknown on that account, and the kept row's exclusion for it falls. A satellite that remembers any other root (a merge) stays unknown as built. An edge that moves still takes its own row: a fragment that re-attaches through a new edge is released by the rehost row.
- **Snapshot.** The loader drops the entry, with a warning, when `network.state_snapshot_path` is set: the target memory is not serialized, and a resumed leg would pin a new root at 0 under carries kept in the old frame.
- **The observatory.** RootHandover is built. The armed `root_handover` leave and join rows append `outcome=` and, when pinned, `rpb_target_m` and `rpb_sigma_m`. The election's run-end line counts the handovers, those pinned, the satellite edges kept and those left as built by cause (a run with no handover reads as no event, one with handovers and none pinned as inert), and the frame census has its own line.
- **One fix at review, before the arms.** The kept-edge count was taken at the draft, so it included released edges and edges below a release. It is now taken in the frame walk, for an edge whose frame step stays known.
- **Tests.** 9 new oracle cases (among them a random-lineage oracle over 320 epochs that restates the rule and reaches 280 handovers: 66 pinned, 69 no vertex, 38 not the keeper, 107 no carry), 5 observer and run-end cases, a loader case and 3 updated tests. No test reaches the fold; the arms read it (the bias trace, section 5.3). The full Debug suite on the final sources: 3 796 run, 3 763 passed, 33 skipped (fixtures absent), 0 failed. `nom_epoch_lint.py`, `nom_n4_verify.py --whole`, `counters_split_check.py`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader`, `simnet_chain_frame_grade.py --selftest` and `ird_make_arms.py --selftest` pass.

### 5.2 The arms

One exe, `gnss_processor_ird4root.exe` (sha256[:16] `71d2d73ad65398fe`: `2f18696` plus the row), 2 h arms under `D:/GnssProcessorSupport/runs_nom/`:

- **Off identity:** A1 on r3c, the golden on the sim bed, and A1krh on r3c and the microscope (`_rap_ird4root_{off_a1_r3c,off_gs,a1krh_r3c,a1krh_mic}`) against the rehost row's arms of the same names (`_rap_ird4h_*`, exe `gnss_processor_ird4h.exe`).
- **Inert:** A1krhR (A1krh plus `root`) on the microscope, where no root hands over, against A1krh (`_rap_ird4root_a1krhR_mic`).
- **The row:** A1krhR against A1krh on ird_rg (the row's fixture; r3b with r1 blanked 10 min) and on a new bed, `ird_rg_mic` (the microscope with ird_rg's one RINEX op), and A0fkrhR against A0fkrh on `ird_rg_mic` (`_rap_ird4root_{a1krh,a1krhR}_rg`, `_rap_ird4root_{a1krh,a1krhR,a0fkrh,a0fkrhR}_rgm`).
- **The row alone, a reading and not a gate arm:** A0fkrR against A0fkr on `ird_rg_mic`, with no rehost row (`_rap_ird4root_{a0fkr,a0fkrR}_rgm`).

Each armed config differs from its parent's in the event-row list alone. The class on these beds: one handover per band at TOW 265560, from the blanked S13000SIM to S30000SIM, with 22 root-handover edges per band (9 reversed), 57 kept edges and 10 re-hosts. The 10 re-hosts are the fragments that hung below the old root; they carry 33 receivers and 17 satellites per band, more than half the network, and 14 satellites stay on the pinned side.

### 5.3 The gate

Off identity passes on all four pairs: the products, every trace CSV, the run manifest and the consumer's log are byte-identical, and `stdout.txt` and the main log differ only by the new entity field's inventory row and the declared-field count (115 against 114). The row armed where no root hands over is inert on the microscope: the products and every trace are byte-identical, and the logs differ only in the text that names the row.

| clause | ird_rg (A1krhR) | microscope gap (A1krhR) | floor-off microscope gap (A0fkrhR) |
| --- | --- | --- | --- |
| the class reaches n >= 1 | 2 handovers, 44 edges, 1 epoch | the same | the same |
| legacy or fractional steps fall to 0 | pass: 44 unknown steps to 0, all 44 known with a zero step | pass: the same | pass: the same |
| I-b for the class at the handover epoch, 91 undeclared pairs (previous arm in brackets) | FAILED: WL max 0.099 cycle, 11 pairs over the 0.05 bar; 0 undeclared integer steps (WL 0.500, 82 pairs; 262) | FAILED: E1 max 0.023 cycle, 4 pairs over the 0.02 bar, E2 0.018, WL 0.005; 0 undeclared integer steps (E1 0.483, 89 pairs; E2 0.495, 89; WL 0.489, 82; 238) | FAILED: E1 max 0.021 cycle, 2 pairs over the bar, E2 0.016, WL 0.005; 0 undeclared integer steps (E1 0.498, 88; E2 0.493, 89; WL 0.477, 76; 235) |
| both edges with identities | pass: 2 leave and 2 join rows naming both roots, `outcome=pinned`, the target and its sigma; run end 2 pinned, 28 satellite edges kept, 0 left as built | pass: the same | pass: the same |
| float consumer by hour, m (previous; bound max(+10 %, +5 mm)) | pass: 0.186, 0.115 (0.186, 0.129) | pass: 0.144, 0.041 (0.144, 0.043) | pass: 0.124, 0.045 (0.124, 0.045) |
| W5 no higher | pass: 0 against 0, 240 of 240 epochs emitted | pass | pass |

- **The fold, read on the bias trace.** S30000SIM's bias holds its last solved value from the handover on, in every armed arm: 0.166 / 0.691 m (L1C / L2W) on ird_rg, -2.709 / -9.882 on the microscope gap, -10.197 / -17.380 on the floor-off gap, with carried sigmas of 0.010 to 0.019 m. In each previous arm it falls to 0 at that epoch.
- **What the row keeps.** The legacy re-references over the run fall from 206 to 178: the 14 satellites of the pinned side on two bands. At the handover epoch on the microscope gap, 142 of the 760 arcs of the carry trace stay carried, against 0 in the previous arm. The rehost row's release counts 660 carries against 438: the 222 carries between the moved receivers and the pinned side now fall to the release, where the as-built re-reference took them under its own count. The 66 RPB carries, 34 phase-bias priors and 34 declared keys are equal.

The I-b miss is at the size the same pairs reach with no event. The 91 pairs of the 14 kept satellites, per epoch (p50 of |wrap| / max, cycles; integer steps):

| TOW | event at the epoch | E1, microscope gap, row | E1, microscope gap, previous | E1, floor-off gap, row | E1, floor-off gap, previous | WL, ird_rg, row | WL, ird_rg, previous |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 265380 | none | 0.004 / 0.023; 0 | the same | 0.002 / 0.018; 0 | the same | 0.017 / 0.073; 0 | the same |
| 265530 | none | 0.004 / 0.014; 0 | the same | 0.005 / 0.019; 0 | the same | 0.023 / 0.085; 0 | the same |
| 265560 | the handover | 0.006 / 0.023; 0 | 0.285 / 0.483; 83 | 0.007 / 0.021; 0 | 0.232 / 0.498; 83 | 0.021 / 0.099; 0 | 0.288 / 0.500; 82 |
| 265590 | re-hosts | 0.005 / 0.020; 0 | 0.266 / 0.492; 58 | 0.005 / 0.023; 0 | 0.242 / 0.497; 51 | 0.012 / 0.037; 0 | 0.250 / 0.496; 69 |
| 265620 | re-hosts | 0.017 / 0.047; 0 | 0.252 / 0.497; 63 | 0.015 / 0.043; 0 | 0.265 / 0.494; 71 | 0.015 / 0.050; 0 | 0.174 / 0.491; 20 |

Over the 10 min before the handover, the 14 epochs with no event read these pairs' per-epoch maximum at p50 0.025 cycle, largest 0.093, with 9 of the 14 over the bar on the microscope gap; p50 0.022, largest 0.084, with 7 over the bar on the floor-off gap; and p50 0.082, largest 0.136, with 13 over the bar on ird_rg's wide lane. The handover epoch reads at those medians. The bar is not relaxed by this reading; it attributes the miss.

### 5.4 The causal reading: the handover's step removed

| reading (previous; row) | ird_rg (A1) | microscope gap (A1) | floor-off microscope gap | the row alone, floor-off gap |
| --- | --- | --- | --- | --- |
| reference-free double differences, residual p50: the epoch before the handover | 2.10 | 0.055 | 0.016 | 0.052 |
| the same at the handover epoch | 10.69; 5.32 | 0.919; 0.223 | 0.396; 0.097 | 0.451; 0.045 |
| the handover's 10 min bin, p50 / share above 0.25 cycle | 6.10 / 0.83; 2.40 / 0.80 | 0.345 / 0.56; 0.114 / 0.34 | 0.168 / 0.42; 0.050 / 0.23 | 0.144 / 0.39; 0.047 / 0.15 |
| the next four bins, p50 | 4.47, 3.18, 2.45, 2.03; 1.78, 1.31, 1.30, 1.08 | 0.164, 0.075, 0.086, 0.062; 0.074, 0.043, 0.047, 0.036 | 0.056, 0.025, 0.024, 0.020; 0.034, 0.020, 0.023, 0.019 | 0.039, 0.020, 0.015, 0.013; 0.068, 0.103, 0.138, 0.142 |
| the run, p50 / share above 0.25 cycle | 3.14 / 0.82; 2.29 / 0.80 | 0.065 / 0.24; 0.051 / 0.18 | 0.030 / 0.12; 0.026 / 0.11 | 0.025 / 0.10; 0.038 / 0.16 |
| the search's would-be fixes (measure-only, against truth): wrong, right | 2 124, 716; 1 708, 2 242 | 1 166, 157 412; 175, 170 271 | 56, 175 435; 20, 177 649 | 913, 166 388; 3 362, 142 313 |

- **The handover itself is free.** With no release at the same epoch (the row alone) the double differences read 0.052 cycle before the handover, 0.045 at it and 0.048 after it, against 0.451 as built. What remains at the handover in the cumulative arms (0.016 to 0.097 on the floor-off gap) comes with the 10 re-hosts of the same epoch.
- **The row needs the rehost row.** Bins 4 to 6 of A0fkr and the row alone already creep (0.015, 0.023, 0.040; A0fkrh reads 0.023, 0.016, 0.011), the re-host damage of section 4. As built the handover's erase clears that creep and the last bin reads 0.013; with the row alone nothing clears it, the last bin reads 0.142, the wrong would-be fixes rise from 913 to 3 362, and the consumer's hour 2 reads 0.0299 m against 0.0248 (its parent's bound 0.0298). This arm is a reading of the row in isolation and is not the row's gate: table order grades the row on A1krh.
- **The consumer.** 3D p50 per 10 min bin from the handover on: ird_rg 0.118, 0.107, 0.122, 0.050, 0.021 m against 0.127, 0.131, 0.156, 0.078, 0.030; microscope gap 0.051, 0.038, 0.028, 0.025, 0.025 against 0.052, 0.040, 0.031, 0.028, 0.028; floor-off gap within 1 mm in every bin (the last bin of each is partial). The rehost row's cost of section 4 stands on this bed too: the floor-off gap's hour 2 reads 0.025 m on A0fkr, 0.045 on A0fkrh and 0.045 with the row.
- **The stop rule's reading does not move** (event-free chords p50 L1C / L2W 0.0243 / 0.0187 cycle against 0.0243 / 0.0186 on the floor-off gap). Recorded, not graded; the stop stays ruled closed.

### 5.5 Reading

1. **The row does what card IRD section 5 states: Delta c = 0 at a root handover.** The new root's bias holds its carried value, the satellites on its side keep their frame and carries, and the undeclared integer steps at the handover fall from 235 to 262 to 0.
2. **Without a release at the same epoch the handover takes nothing from the double differences.**
3. **A handover's remaining cost is its fragments'.** The old root's departure leaves 10 fragments per band to re-attach through new edges in one epoch, and the uncertified rule releases each: more than half the network here. That is the rehost row's mechanism and cost (section 4), and this row neither adds to it nor removes it. The certified branch stays held.
4. **I-b FAILED on the fractional bar, by 2 to 11 of 91 pairs, at the size the same pairs reach with no event.** The integer clause of the bar holds for the class on every bed. The non-event epochs of these arms fail the same fractional bar widely, which the grader reports and does not grade.

### 5.6 Standing

The `root` row LANDED default off; the ledger row of `network.phase_datum_eq34_event_rows` stays UNVALIDATED with its text updated. Of its gate: off identity (every product and trace byte-identical, one new entity row in the logs), the class's legacy steps, both edges' identities, the float consumer and W5 pass; I-b FAILED on the fractional bar on every bed. One fixture handover is graded: a repeated handover, a pinned target kept over a long run and the three as-built causes are covered by the oracle only, and the field bed is not run. Next in table order: `slip` (ird_t1 and ird_tr), then `merge_split`.
