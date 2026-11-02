# IRDC: the integer-recovering datum, continued (the event sources)

**STATUS**

- **id:** IRDC
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRDB's continuation (that file reached the 60 KB bar), from the trace of the IRD-2 stop's event causes on; card IRD keeps the binding parts and card IRDB the results of IRD-1, IRD-2 and the bisection. The shock at the first satellite return under bundle A0 (IRDB section 4.4) is traced to its source (section 2): the chain loses a satellite's tree identity across a gap (extend-only can never keep a tree edge, because a pinned arc carries no leveling entry), reads the return as a first sighting with a known zero step, and so neither shifts nor erases the satellite's old-frame carries; the wind-up restart latches them free, and the float vehicle folds them at a birth variance the chain makes as tight as 7.8 mm, against a 2.1 m frame step. One knob confirms it: with `phase_leveling_max_gap_s` 600 the carries are pruned inside the 720 s gap, the shock is gone (612 to 6 leveling priors above 4 sigma, clock step 2.74 to 0.18 cm) and the end-of-run reference-free residual falls from 0.257 to 0.036 cycle; R1 does not move. The stop rule still fails (event-free chords 0.090 and 0.065 cycle). Every kept re-pin is a stale-carry re-pin (one horizon after its own re-host, or after a re-reference of its satellite), which corrects IRDB section 4.4's flipped edge. The remaining drift is traced too (section 3): card IRD C8 damage, where an unknown step re-references a subtree and its receivers' chords to outside satellites survive in the old frame, from two sources: the cold re-host at epoch 88 (about half the end-of-run share, through a creep across the receivers below G32) and a flipped kept edge at epoch 210, which the election keys on parent identity (card IRD C7) and, with no carry on a pinned arc, declares an unknown step although its arc and target are unchanged (the other half). Most post-convergence "cold re-hosts" of IRDB section 4.4 are such flips (18 of A0's 34 rows). No causal arm: the fixes are card IRD's `kept` and `rehost` event rows (IRD-4), held by the frozen rule. Every frame step that damages A0f's double differences on the microscope (the RPB floor off) is traced to its line, none in the static algebra. The author ruled on 2026-11-02 that these causes close the stop (section 4), so IRD-3 onward may be built in card IRD section 11's order.
- **source:** [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md) sections 4.4 to 4.6 (the bisection and its open item); [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 4 (the as-built defects C7 and C8) and sections 10 and 11 (the stop rule)
- **knobs / posture:** none added; the diagnostic arms set `network.phase_leveling_max_gap_s` 600 (the golden's 900) on IRDB's A0f (bundle A0, RPB floor off) and on R1; no golden change
- **dates in the text:** 2026-11-02
- **hashes / run dirs:** branch `claude/distracted-germain-d93ac2`, no code change; exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird1f.exe` (sha256[:16] `1cc358ebd6bd3a98`), base `a02ccfb`; arms `D:/GnssProcessorSupport/runs_nom/_rap_ird2_tr_{a0fh6,r1h6}_mic`, read against `_rap_ird2_{a0f,r1,a0}_mic`
- **gates (frozen at pre-registration):** card IRD section 10 (the static-algebra stop rule)

**Outcome.** Question: what turns the first satellite return into a network-wide shock under bundle A0 (IRDB section 4.6)? The chain reads the returning satellite as a first sighting with a known zero step, so its old-frame carries are neither shifted nor erased, and the float vehicle folds them at a birth variance the chain makes mm-level, against a 2.1 m frame step. Pruning those carries by one knob removes the shock and cuts the end-of-run residual seven-fold; R1 is unchanged. The stop rule still fails (0.090 cycle against 0.020). The rest of the drift (section 3) is card IRD C8 damage from the cold re-host at epoch 88 and a flipped kept edge at epoch 210, which the election reads as an unknown step (C7). Standing: every frame step that damages the double differences on the microscope (RPB floor off) is traced, none in the static algebra; the fixes are IRD-4's event rows. The author ruled that the traced causes close the stop (section 4); IRD-3 is next.

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

## 3. 2026-11-02: the remaining drift traced (the re-host class, card IRD C7 and C8)

### 3.1 The events

The 600 s arm of section 2.2 (`_rap_ird2_tr_a0fh6_mic`) is A0f with the return path cut. After convergence (epoch 40) its unknown frame steps are one genuine cold re-host (epoch 88) and nine flipped kept edges at seven epochs; A0f has the same events at the same epochs, less the epoch-88 flip of S45000SIM-G24. The observatory labels a flipped kept edge `rehost_cold` on a `Kept` row with `flipped` 1, so the post-convergence "cold re-hosts" of IRDB section 4.4 are flips except epoch 88's, and of the 34 cold re-host rows IRDB section 4.3 counts for A0 (both bands), 18 are flips.

### 3.2 The flip (card IRD C7)

When a vertex's parent edge dies, the forest re-attaches the detached fragment with one new edge and keeps the fragment's own edges (`ArcGraph.cpp:542`), so the path from the new edge to the fragment's old top reverses. The election keys an edge on its child's parent (`ChainDatum.cpp:701` for a satellite child, `:715` for a receiver child), so a reversed edge reads as a re-host. Its step is taken from the arc's chord carry where one exists (`:722`, card IRD C7 as written), but a pinned arc is never harvested, so a tree edge born as one, or pinned for longer than the horizon, has none and the step is declared unknown (`:740`). The reversed edge's whole subtree is then re-referenced (`:950`) and its receivers' RPB carries erased (`:958`), although the arc and its target are unchanged and the true step is the parent's known step, passed on exactly.

Epoch 210 of the 600 s arm: G32 loses its edge to the root S13000SIM. The fragment below it re-attaches through S42000SIM-G28, a known re-host by that chord's float error (0.055 m on L1C, -0.115 m on L2W: 0.29 and 0.47 cycles; `RehostUncertified`). The kept edge S42000SIM-G32 reverses, and G32's subtree of six satellites (G13, G15, G21, G22, G24, G32) and eleven receivers is re-referenced with 158 carries reborn; S33000SIM-G14 reverses at the same epoch behind G14's certified re-host. In that epoch the subtree's receiver phase biases move by -0.20 to +0.36 m on L1C, against a true step of a few centimetres.

### 3.3 The damage (card IRD C8)

The re-reference erases the subtree's (satellite, signal) keys at every station and its receivers' RPB carries (`ChainDatum.cpp:1047`); the subtree receivers' chords to satellites outside it survive in the old frame, off by the true step. Every post-convergence unknown step of the 600 s arm:

| epoch | unknown roots | subtree satellites / receivers | carries reborn | outside chords (L1C), n / above 4 sigma | true step, cycles L1C / L2W | DD share above 0.25, epoch t-1 to t+3 |
| --- | --- | --- | --- | --- | --- | --- |
| 70 | G25 (flip) | 5 / 8 | 130 | 40 / 1 | 0.08 / 0.06 | 0.004 to 0.006 |
| 88 | S45000SIM (cold re-host), G24 (flip) | 2 / 6 | 38 | 35 / 4 | unknown | 0.007 to 0.027 |
| 106 | S04000SIM (flip) | 2 / 1 | 66 | 5 / 0 | not traced | 0.047 to 0.050 |
| 139 | S12000SIM (flip) | 0 / 1 | 0 | 7 / 0 | not traced | 0.067 to 0.072 |
| 181 | G10, S52000SIM (flips) | 1 / 3 | 22 | 19 / 3 | 0.12 / 0.06 | 0.076 to 0.079 |
| 187 | G21 (flip) | 1 / 0 | 28 | 0 / 0 | 0.38 / 0.37 | 0.073 to 0.074 |
| 210 | G32, S33000SIM (flips) | 6 / 11 | 158 | 46 / 12 | 0.29 / 0.47 | 0.078 to 0.148 |

The true step is the RPB shift of the flipped edge's parent receiver (`network_rpb_states.csv`); "not traced" marks a satellite parent, whose step the traces do not record. The DD share moves only where both are present: outside chords and a sizable true step (epoch 210, and the cold re-host at 88). A mass rebirth alone does not move it (epoch 70: 130 carries reborn, step 0.08 cycles), nor does a large step with no receiver in the subtree (epoch 187). A0f, already off the integers after G20's return, shows the same events with larger steps: +0.065 at epoch 70 (0.17 / 0.14 cycles, 18 of 40 outside chords above 4 sigma) and +0.046 at epoch 181 (0.46 / 0.35 cycles).

### 3.4 The cold re-host at epoch 88 and the creep

S45000SIM re-hosts onto a first-sighting arc of G32, whose target 0 sits on a code-seeded float (card IRD C6), so its step is genuinely unknown. Its four outside chords (G05, G15, G18, G23) survive in the old frame: in the 600 s arm the G05 chord reads 0.41, 0.12, -0.05 and -2.25 m over epochs 87 to 90 (in A0 it moves 1.17 m in one epoch, IRDB section 4.4), and RAIM refloats G18 and G15. From epoch 100 every one of S45000SIM's DDs is wrong (12 of 12); over epochs 88 to 160 RAIM refloats 29 arcs, all at receivers below G32 (S48, S45, S53, S40, S35 and S56) and mostly on S45000SIM's outside satellites, and the wrong DDs spread across those receivers. The share climbs from 0.007 at epoch 87 to 0.083 at epoch 160, flat until the epoch-210 flip adds 0.070; the run ends at 0.147.

### 3.5 Reading

1. **The remaining drift is C8 damage from two sources:** the cold re-host at epoch 88 (about half the end-of-run share, through its creep) and the flipped kept edge at epoch 210 (the other half). The flips at epochs 70, 106, 139, 181 and 187 carry small true steps or no outside chords and move the share by 0.005 or less on the 600 s arm.
2. **Card IRD C7 is refined:** a flipped kept edge takes "target minus a stale carry" only while its arc keeps a chord carry from a recent re-host; past the horizon it takes an unknown step and re-references its subtree. Either way the flip itself carries no frame step.
3. **No causal arm.** No existing knob separates the flip or the outside chords. The fixes are card IRD's event rows `kept` (arc identity, target frozen) and `rehost` (a cut release for an uncertified re-host), built at IRD-4, which the frozen rule holds until the author's ruling; their fixture gates on the microscope are the planned causal test.
4. **With section 2, the microscope's event damage is attributed:** the satellite return, the epoch-88 cold re-host and the epoch-210 flip, each a frame step that meets carries left in the old frame.

### 3.6 Standing

No code changed. Every frame step that damages A0f's double differences on the microscope (bundle A0 with the RPB floor off) is traced to its line, and none is in the static algebra; the floor's slower convergence (IRDB section 4.4) is the one other cost recorded. The author's ruling on the stop is owed (IRDB section 4.6).

## 4. 2026-11-02: the author's ruling on the IRD-2 stop

### 4.1 The ruling

Asked after sections 2 and 3, "Do the traced event causes close the IRD-2 stop?", the author answered: "Yes, close it."

### 4.2 What it closes

- The stop that card IRD section 10's static-algebra rule put on the card at IRD-2 (IRDB section 3) is closed. Its cause is attributed to the as-built event handling (IRDB section 4, sections 2 and 3 here), not to the static algebra.
- The rule's measured verdict stands as recorded: microscope chords event-free for 30 min read 0.22 cycle on A0 and 0.090 with the return path cut, against 0.02. The bar is not re-registered.
- The rule's clause "builds nothing from IRD-3 on until the cause is closed" is met: IRD-3 onward may be built, in the order and with the gates of card IRD section 11.

### 4.3 What follows, from the frozen plan

- IRD-3 (the pair family) is the next increment of card IRD section 11.
- The traced causes fall to IRD-4's event rows (card IRD section 5): `returning` (section 2, the returning satellite's frame step), `kept` (section 3.2, arc identity with the target frozen) and `rehost` (sections 3.3 and 3.4, the cut release of an uncertified or cold re-host). Each row's fixture gate on the microscope is the causal test of its traced cause.
- Still owed from IRD-2: the certification defect (sigma scale 3 still wrong; by the frozen rule it is fixed before IRD-4), IRD-5b (decision D-N) and the GF-datum defect (decision D0).
