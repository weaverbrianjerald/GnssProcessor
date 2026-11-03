# IRDD: the integer-recovering datum, continued (IRD-4's event rows)

**STATUS**

- **id:** IRDD
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRDC's continuation (that file reached the 60 KB bar), from the trace of the epoch-88 divergence on; card IRD keeps the binding parts, card IRDB the results of IRD-1 and IRD-2, card IRDC the event causes, IRD-3, the certification rulings and IRD-4's kept row. The epoch-88 divergence of IRDC section 8.4 is traced (section 2): it is the zero-target cold pin of card IRD C6, not the kept row's rule. At the only cold re-host after convergence (S45000SIM onto a first-sighting arc of G32), both arms take the same decisions, and the zero target meets the old frame that the subtree's surviving chords keep (C8). The conflict is the arc's whole old-frame float: -21.98 / +31.02 cycles as built and +42.02 / -86.98 with the row, which differ by exactly the integer the row's frozen targets left at S45000SIM (64 / -118); its fraction is 0.02 cycle. A scratch diagnostic that changes only this pin's target moves every reading with the integer, the consumer included; with the target rounded to the old-frame float, the event is free, the double differences are integer-clean from 70 min in both arms (reference-free share above 0.25 cycle 0.00) and the two arms agree to 0.006 cycle. The epoch-210 re-host of IRDC section 3 is this pin's aftermath too. On the A1 beds the row leaves the pin's integer unchanged, so the r3c consumer failure of IRDC section 8 is not this mechanism.
- **source:** [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) sections 3.4, 8.4 and 8.5 (the open item); [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 2 (C6, C8), section 5 (the `RehostUncertified` row) and section 11 (IRD-4's order and gates)
- **knobs / posture:** no producer code change; the arms are IRDC section 8.4's A0f and A0fk on the microscope with `network.phase_leveling_max_gap_s` 600; a scratch diagnostic, not committed, adds a whole number of cycles to the target of one cold re-host pin (section 2.3); no golden or ledger change
- **dates in the text:** 2026-11-03
- **hashes / run dirs:** branch `claude/distracted-germain-d93ac2` at `65ef3ca`; native arms `_rap_ird4b_{tr,trk}_mic` (exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird4k2.exe`, sha256[:16] `4266492fc70cedeb`); diagnostic exe `gnss_processor_ird4dx.exe` in the same folder (sha256[:16] `3de7d0ca2b383fdf`: `65ef3ca` plus the patch kept beside it as `gnss_processor_ird4dx_patch.py`), arms `_rap_ird4x_{tr,trk}_{swap,round}_mic`; all arms under `D:/GnssProcessorSupport/runs_nom/`, the readouts of section 2 in its `_ird4x_tools/`
- **gates (frozen at pre-registration):** card IRD section 11 (IRD-4's per-row gate, its order and the hold on certified re-hosts); section 2 is a trace with no gate of its own

**Outcome.** Question: why does the kept row's arm part from the as-built arm at the epoch-88 cold re-host on the floor-off microscope (IRDC section 8.4)? The cold arc is pinned at 0 while its subtree's outside chords keep the old frame, so the solve carries the arc's whole old-frame float. Its integer is a datum choice the data cannot observe, and the row's frozen targets changed it by 64 / -118 cycles. Swapping that integer swaps every reading; rounding it away makes the event free in both arms. Verdict: card IRD C6, not the row. Standing: IRD-4 continues with `returning`; `rehost` is the fix (section 2.6).

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
