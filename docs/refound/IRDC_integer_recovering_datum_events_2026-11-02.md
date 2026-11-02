# IRDC: the integer-recovering datum, continued (the event sources)

**STATUS**

- **id:** IRDC
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRDB's continuation (that file reached the 60 KB bar), from the trace of the IRD-2 stop's event causes on; card IRD keeps the binding parts and card IRDB the results of IRD-1, IRD-2 and the bisection. The shock at the first satellite return under bundle A0 (IRDB section 4.4) is traced to its source (section 2): the chain loses a satellite's tree identity across a gap (extend-only can never keep a tree edge, because a pinned arc carries no leveling entry), reads the return as a first sighting with a known zero step, and so neither shifts nor erases the satellite's old-frame carries; the wind-up restart latches them free, and the float vehicle folds them at a birth variance the chain makes as tight as 7.8 mm, against a 2.1 m frame step. One knob confirms it: with `phase_leveling_max_gap_s` 600 the carries are pruned inside the 720 s gap, the shock is gone (612 to 6 leveling priors above 4 sigma, clock step 2.74 to 0.18 cm) and the end-of-run reference-free residual falls from 0.257 to 0.036 cycle; R1 does not move. The stop rule still fails (event-free chords 0.090 and 0.065 cycle). Every kept re-pin is a stale-carry re-pin (one horizon after its own re-host, or after a re-reference of its satellite), which corrects IRDB section 4.4's flipped edge. The re-host class (card IRD C8) remains; the author's ruling on the stop is owed.
- **source:** [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md) sections 4.4 to 4.6 (the bisection and its open item); [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 4 (the as-built defects C7 and C8) and sections 10 and 11 (the stop rule)
- **knobs / posture:** none added; the diagnostic arms set `network.phase_leveling_max_gap_s` 600 (the golden's 900) on IRDB's A0f (bundle A0, RPB floor off) and on R1; no golden change
- **dates in the text:** 2026-11-02
- **hashes / run dirs:** branch `claude/distracted-germain-d93ac2`, no code change; exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird1f.exe` (sha256[:16] `1cc358ebd6bd3a98`), base `a02ccfb`; arms `D:/GnssProcessorSupport/runs_nom/_rap_ird2_tr_{a0fh6,r1h6}_mic`, read against `_rap_ird2_{a0f,r1,a0}_mic`
- **gates (frozen at pre-registration):** card IRD section 10 (the static-algebra stop rule)

**Outcome.** Question: what turns the first satellite return into a network-wide shock under bundle A0 (IRDB section 4.6)? The chain reads the returning satellite as a first sighting with a known zero step, so its old-frame carries are neither shifted nor erased, and the float vehicle folds them at a birth variance the chain makes mm-level, against a 2.1 m frame step. Pruning those carries by one knob removes the shock and cuts the end-of-run residual seven-fold; R1 is unchanged. The stop rule still fails (0.090 cycle against 0.020). Standing: the return path is closed as a cause, the re-host class (card IRD C8) is open, and the author's ruling on the stop is owed.

## 1. What this card continues

Card IRDB records IRD-1, IRD-2 and the bisection of the IRD-2 stop; with this trace its file would pass the 60 KB bar, so the trace is recorded here. IRDB section 4.6 leaves the source of the satellite-return shock open, and section 2 below traces it. Card IRD keeps the derivation, the as-built defects (section 4), the event table, the arms, the graders, the bars and the frozen gates.

<!-- Results are appended below this line under dated headings. -->

## 2. 2026-11-02: the satellite-return shock traced

### 2.1 The path

A0f on the microscope (IRDB section 3, exe `ird1f`, base `a02ccfb`): G20 returns at TOW 262 920 (epoch 64) after 720 s at every station. The path, with the as-built lines:

1. **The chain loses the satellite's identity across the gap.** The incumbent tree (`anchor_incumbent`, `eq34_sat_root`, `eq34_prev_target`) is replaced at every election by the live tree (`ChainDatum.cpp:1195`). The extend-only merge would keep a tree edge while its own arc's leveling carry lives (`ChainDatum.cpp:1141`), but a pinned arc is never harvested: none of A0f's 180 tree edges at epoch 63 holds a carry. Extend-only therefore never keeps a tree edge, on any bed, which is why A0xo is byte-identical to A0 (IRDB section 4.2).
2. **The return is read as a first sighting with a known zero step.** The new host S04000SIM tracks G20 for the first time. With no incumbent the pin-to-round does not engage (line 620) and the target is the first-sighting 0 (line 618); the edge counts as unchanged (line 701), its pin move is 0 - 0 (line 751), and the shift walk passes the vertex as known with a zero step (lines 942 to 950). G20 enters neither `eq34_reref` nor the shift set.
3. **The realized phase bias steps.** G20's phase bias moves from -9.654 to -7.532 m on L1C and from -14.763 to -12.651 m on L2W (+2.121 and +2.112 m: 11.15 and 8.65 cycles).
4. **The old-frame carries fold at a birth variance the chain makes tight.** G20's leveling carries at 13 tracking stations are inside `phase_leveling_max_gap_s` (900 s) and still in the old frame. The wind-up restart latches them free (`cycle_slip.windup_restart_refloat`, 13 `wu_restart` rows), and under the float vehicle a freed arc is not skipped: it is dropped to its birth variance with its value kept (`LevelingCarry.cpp:295` and `:308`, `AmbFloat.cpp:57`; the comment there assumes "a prior at birth variance is dominated by the epoch's data"). The birth variance is the carry's first stash (epoch 7), which under the chain has sigma 7.8 mm at S17000SIM, 2.8 cm at S19000SIM and 7.8 to 45 cm elsewhere; under R1 the same arcs start at 14 to 57 cm.
5. **The tension.** The solve moves 12 of the 13 carries by -1.97 to -2.16 m, the frame step; S17000SIM's prior holds its carry at -1.19 m. The remaining 0.87 m reaches RAIM (S17000SIM/G20 at 153 sigma, S04000SIM's other arcs at 22 to 84 sigma), the 512 prior-residual rows and the clock step of IRDB section 4.4.
6. **R1 erases the same carries.** At the same epoch R1's held basis re-references G20 and erases its 15 carries (`erase:anchor_reref`, `PhaseAnchorEpoch.cpp:585`). The loader disables that erase under the chain because the chain "already owns the eq34_reref carry hygiene this knob mirrors" (`GnssConfigLoadNetwork.cpp:10246`); for a returning satellite vertex it does not.

G20's shared phase-bias prior is not involved: no epoch-64 fold carries one (62 pb rows for 31 satellites; G20's first folds at epoch 65).

### 2.2 The causal arm

One knob on A0f and on R1: `network.phase_leveling_max_gap_s` 600 (the golden's 900), exe `ird1f`, arms `_rap_ird2_tr_a0fh6_mic` and `_rap_ird2_tr_r1h6_mic`. G20's 15 carries are then gap-pruned inside the 720 s gap, and every G20 arc is born at the return.

| arm | pl priors > 4 sigma, epoch 64 | pb priors > 4 sigma | clock step p50 | RAIM refloats | residual p50 / share > 0.25, epoch 64 | the same, last bin (raw wrong) | stop rule p50, L1C / L2W |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A0f | 612 of 782 | 48 of 62 | 2.74 cm | 8 | 0.028 / 0.065 | 0.257 / 0.51 (0.63) | 0.210 / 0.215 |
| A0f, 600 s | 6 of 782 | 2 of 62 | 0.18 cm | 0 | 0.019 / 0.007 | 0.036 / 0.15 (0.22) | 0.090 / 0.065 |
| R1 | 94 of 900 | 0 of 62 | 0.12 cm | 0 | 0.015 / 0.000 | 0.001 / 0.00 (0.00) | |
| R1, 600 s | 94 of 900 | 0 of 62 | 0.12 cm | 0 | 0.015 / 0.000 | 0.001 / 0.00 (0.00) | |

The prior columns count the CLEAN rows of `network_prior_residual.csv`, the clock step is the other satellites' change from epoch 63, the residual is `ird_dd_drift.py reffree` (IRDB section 4.3) and the stop rule is IRDB section 3's (`ird_frame_census.py stop --strict-rows`, event-free chords). A0f and its 600 s arm read the same to four decimals at epochs 62 and 63; from epoch 64 the 600 s arm converges as R1 does (residual p50 0.019, 0.014, 0.012 over epochs 64 to 66; R1 0.015, 0.009, 0.007). The knob leaves R1 unchanged. The run's other return (G12, epoch 224) follows a 3 150 s gap, past both horizons.

### 2.3 The kept re-pin at the horizon

A re-hosted tree edge takes its target from its arc's chord carry (`lambda`·round(carry/`lambda`)). That carry is not refreshed while the arc is pinned, so it is pruned one horizon after the re-host, and in the next epoch the kept edge re-pins to the first-sighting 0 by its whole integer: S41000SIM-G02, re-hosted at epoch 24 at 81 cycles, re-pins by -15.41 m at epoch 44 with the 600 s horizon and at epoch 55 with 900 s. Every kept re-pin of A0, A0f and the 600 s arm (L1C) has one of two causes: one horizon after the arc's own re-host (39 of A0's and A0f's 42, 58 of the 600 s arm's 59), or the epoch after a re-reference of the edge's satellite (the rest); the knob moves the first kind by ten epochs. They are known steps that pass the shift walk, and the double differences do not register them (epochs 62 and 63 above, where the two arms' re-pins differ).

Correction of IRDB section 4.4: the S45000SIM-G24 re-pin at epoch 89 (110 cycles to 0) is not a flipped edge (`flipped` 0 in A0, A0f and the 600 s arm). Its target's chord carry, frozen since the edge's re-host at epoch 61, left with the epoch-88 re-reference of G13 and G24 in A0 and A0f, and with the horizon prune at epoch 80 in the 600 s arm, which re-pins at epoch 81. The `eq34_reref` erase writes no `erase:` row to `network_amb_state.csv` (`ChainDatum.cpp:1047`), so that departure leaves no trace row.

### 2.4 Reading

1. **The source term is found and its causality confirmed.** A returning satellite vertex whose identity the chain lost across the gap takes the first-sighting target with a known zero step (card IRD C7: "a returning vertex takes baseline 0"). Its old-frame carries survive, and the float vehicle's bounded drop folds them at a birth variance the chain makes mm-level. Pruning them removes the shock and cuts the end-of-run residual seven-fold.
2. **What remains on the 600 s arm** creeps from epoch 88 and steps at epochs 210 to 213, where a re-host re-references six satellites (G13, G15, G21, G22, G24 and G32: 12 `eq34_reref` keys, 158 carries reborn) and the block-Baarda test, the MDB census and the congruence test respond the next epoch: the card IRD C8 class, not traced here.
3. **The stop rule still fails with the return path cut** (event-free chords 0.090 and 0.065 cycle against 0.020). Closing this path alone does not pass the frozen rule on the as-built chain.
4. **The 600 s horizon is a diagnostic, not a candidate:** it also bounds the ZWD and receiver phase-bias carries, and R1 does not move with it.

### 2.5 Standing

No code changed. The satellite-return path is traced and closed as a cause; the ruling on the stop remains the author's (IRDB section 4.6), now with this path traced, the re-host class (C8) open and the untraced `eq34_reref` erase noted.
