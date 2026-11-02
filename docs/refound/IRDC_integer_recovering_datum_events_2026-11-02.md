# IRDC: the integer-recovering datum, continued (the event sources, IRD-3)

**STATUS**

- **id:** IRDC
- **topic:** network-vehicle
- **status:** open
- **summary:** Card IRDB's continuation (that file reached the 60 KB bar), from the trace of the IRD-2 stop's event causes on; card IRD keeps the binding parts and card IRDB the results of IRD-1, IRD-2 and the bisection. The shock at the first satellite return under bundle A0 (IRDB section 4.4) is traced to its source (section 2): the chain loses a satellite's tree identity across a gap (extend-only can never keep a tree edge, because a pinned arc carries no leveling entry), reads the return as a first sighting with a known zero step, and so neither shifts nor erases the satellite's old-frame carries; the wind-up restart latches them free, and the float vehicle folds them at a birth variance the chain makes as tight as 7.8 mm, against a 2.1 m frame step. One knob confirms it: with `phase_leveling_max_gap_s` 600 the carries are pruned inside the 720 s gap, the shock is gone (612 to 6 leveling priors above 4 sigma, clock step 2.74 to 0.18 cm) and the end-of-run reference-free residual falls from 0.257 to 0.036 cycle; R1 does not move. The stop rule still fails (event-free chords 0.090 and 0.065 cycle). Every kept re-pin is a stale-carry re-pin (one horizon after its own re-host, or after a re-reference of its satellite), which corrects IRDB section 4.4's flipped edge. The remaining drift is traced too (section 3): card IRD C8 damage, where an unknown step re-references a subtree and its receivers' chords to outside satellites survive in the old frame, from two sources: the cold re-host at epoch 88 (about half the end-of-run share, through a creep across the receivers below G32) and a flipped kept edge at epoch 210, which the election keys on parent identity (card IRD C7) and, with no carry on a pinned arc, declares an unknown step although its arc and target are unchanged (the other half). Most post-convergence "cold re-hosts" of IRDB section 4.4 are such flips (18 of A0's 34 rows). No causal arm: the fixes are card IRD's `kept` and `rehost` event rows (IRD-4), held by the frozen rule. Every frame step that damages A0f's double differences on the microscope (the RPB floor off) is traced to its line, none in the static algebra. The author ruled on 2026-11-02 that these causes close the stop (section 4), so IRD-3 onward may be built in card IRD section 11's order. IRD-3 (section 5): the pair family `network.phase_datum_eq34_family` landed default off. Off, the field and sim goldens and A0 on r3c are identical in every trace; on, it pins one tree on both bands at full rank with complete audits and, where both bands carry every arc (r3c and the golden sim), reproduces A0 byte for byte. P-c FAILED on four clauses, as A0 fails them. A one-band fixture (`ird_l2v16`) runs the band extension on data: the pair family keeps one tree where the signal family's trees split to the run end.
- **source:** [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md) sections 4.4 to 4.6 (the bisection and its open item); [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 4 (the as-built defects C7 and C8) and sections 10 and 11 (the stop rule)
- **knobs / posture:** `network.phase_datum_eq34_family` (IRD-3, section 5; default "signal", ledger UNVALIDATED); the diagnostic arms of sections 2 and 3 set `network.phase_leveling_max_gap_s` 600 (the golden's 900) on IRDB's A0f (bundle A0, RPB floor off) and on R1; no golden change
- **dates in the text:** 2026-11-02
- **hashes / run dirs:** branch `claude/distracted-germain-d93ac2`; sections 2 to 4: no code change, exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird1f.exe` (sha256[:16] `1cc358ebd6bd3a98`), base `a02ccfb`; arms `D:/GnssProcessorSupport/runs_nom/_rap_ird2_tr_{a0fh6,r1h6}_mic`, read against `_rap_ird2_{a0f,r1,a0}_mic`; section 5: commit `62fc34f`, exe `gnss_processor_ird3a.exe` (sha256[:16] `263539cae90262ec`), reference exe `gnss_processor_ird3ref.exe` (`5e3487bd2bdc32fd`), arms `_rap_ird3_*` under the same root
- **gates (frozen at pre-registration):** card IRD section 10 (the static-algebra stop rule); card IRD section 11 (IRD-3)

**Outcome.** Question: what turns the first satellite return into a network-wide shock under bundle A0 (IRDB section 4.6), and does the pair family hold IRD-3's gate? The chain reads the returning satellite as a first sighting with a known zero step, and the float vehicle folds its old-frame carries at a mm-level birth variance against a 2.1 m frame step; pruning them removes the shock (section 2). The rest of the drift is C8 damage from a cold re-host and a flipped kept edge (C7, section 3). The author ruled the stop closed (section 4). IRD-3 (section 5) passes every structural clause and reproduces A0 byte for byte where both bands carry every arc; P-c FAILED, as A0 fails it. Standing: the certification fix, then IRD-4's event rows.

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

## 5. 2026-11-02: IRD-3, the pair family

### 5.1 What was built

Commit `62fc34f` on branch `claude/distracted-germain-d93ac2` (not merged), default off: `network.phase_datum_eq34_family` "signal" (default) or "pair", ledger UNVALIDATED.

- **The family.** With "pair", a constellation that tracks exactly two phase signals of different bands in an epoch forms one pair family. Any other constellation (one signal, three, or two of one band) is elected per signal that epoch (`eq34_pair_fallbacks`).
- **The forest.** The pair family's components are those of the arcs both bands carry (`band_mask` 0b11, card IRD section 3). Each component elects one datum receiver: the configured receiver, else the root votes of every (satellite, band), else the remembered datum receiver of the first band that has one when it names a station of the component, else the coverage. One forest is grown per component, and its tree edges and its datum receiver's phase bias are pinned on both bands. Targets, carries, the shift walk, the hygiene and the observatory run band by band over the shared forest, in the order the signal family records them.
- **The band extension.** A vertex that only one band tracks (no arc of it is carried by both bands) is attached on that band afterwards, by extension edges grown from the pair trees' vertices, under the tree its band's graph reaches it from (`eq34_pair_ext_edges`). A component of one band's graph with no pair-tree vertex is elected on that band alone (`eq34_pair_band_only_comps`).
- **Code.** `networkElectChainDatum` (about 1 100 lines) is now a sequence of named steps over one `ChainElection`. The run-end eq34 block prints the four pair counters when the knob is on, and `network_chain_edges.csv` writes the family in its family column. The loader warns and reads "signal" for an unknown value, warns that "pair" is inert without the chain, and falls back to "signal" under `phase_datum_time_linked` (the pin bridge roots satellites band by band).
- **Tests.** The frozen AGT-4 oracle (`ChainElectionOnTheModuleEqualsTheFrozenElection`) passes unchanged at its earlier n (1 920 epochs, 95 693 pins). Three oracle cases are new: the pair family equals the signal family when both bands carry every arc (640 epochs, 29 788 pins); one tree at full rank with band-only arcs, satellites, stations and islands (960 epochs, 43 622 pins in 3 423 trees, 5 492 extension edges, 480 band-only components); and the family formation rule. Four loader cases are new. Full Debug suite: 3 730 run, 3 697 passed, 33 skipped (fixture-gated), 0 failed.
- **Exe.** `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird3a.exe`, sha256[:16] `263539cae90262ec`, for every arm of this section.

### 5.2 Declared adaptations

1. **Off identity on the chain path.** The goldens do not run the chain, so their arms test only the code around it; A0 on r3c is added as the chain path's off-identity arm. The references ran on the exe of the head before the change (`gnss_processor_ird3ref.exe`, the `ird2s3` build, sha256[:16] `5e3487bd2bdc32fd`): `_rap_ird3_ref_{gf,gs,a0_r3c}`, whose products equal those of IRD-1's and IRD-2's runs of the same configurations.
2. **The band extension** is not in card IRD, which names the both-band forest only. A band component that joins several pair trees through band-only vertices keeps one receiver phase-bias pin per pair tree on that band (the count still equals the gauge dimension), and the arcs joining them on that band carry the trees' receiver phase-bias difference. They are band-only arcs, which the pair's joint certification never covers.
3. **The datum receiver.** The datum receiver of the first band that has one decides that tier, and the coverage decides when it names no station of the component. The family's remembered datum receiver comes from its pair components only.
4. **Interlock.** The pair family and the pin bridge exclude each other in the loader and in the knob snapshot. The knob is not fingerprinted, as the other election knobs are not.
5. **2 h arms.** P-c's "720 of 720 epochs" reads as every epoch of the 2 h arm, and its postfit sigma0 as the p50 of `sigma0_pf` in `network_epoch_status.csv` against the golden's on the same bed. The golden on r3c is arm G on that bed (`_rap_ird3_g_r3c`; products identical to `_rap_ird1g_r3c`). R1 is IRDB section 3's (`_rap_ird2_r1_{r3c,gs}`, exe `ird1f`), a path the change does not reach.
6. **Graders.** `simnet_producer_ar_grade.py` (DD \|e\| p50 per band, kept and wrong fixes) and `ird_chord_census.py` (receiver DCB sd) reproduce IRDB section 3's A0 r3c readings on the reference run (\|e\| p50 5.561; 5 056 kept, 3 695 wrong; DCB sd 9.454 and 10.135 m). The structure gates read `network_chain_edges.csv` per (epoch, system, band): the two bands' edge sets and datum receivers, and the forest identity E + T = V (tree edges plus one receiver phase-bias pin per tree equal the vertices, the sum of n_c + m_c). The off-identity and A1-against-A0 comparisons are `nom_td_gate.py --whole --mask-time` (every CSV, the products, the manifest, the whole logs and the exit code).
7. **Readings beyond the gate** (section 5.4): A0 and A1 on IRD-2's slip fixtures t1 and t3, and on a new one-band fixture `ird_l2v16` (a bed of `scripts/ird_make_arms.py`: rung 3b with only the L2 of rv16's station blanked, over rv16's window).

### 5.3 The gates (card IRD section 11, IRD-3)

Arms, 2 h, under `D:/GnssProcessorSupport/runs_nom/`: knob off `_rap_ird3_off_{gf,gs,a0_r3c}` against the references; A1 `_rap_ird3_a1_{r3c,gs}` against A0 `_rap_ird3_off_a0_r3c` and `_rap_ird3_a0_gs`; the goldens on the two beds `_rap_ird3_g_r3c` and `_rap_ird3_off_gs`.

| gate | result |
| --- | --- |
| off identical: the AGT-4 chain oracle | pass: unchanged at its earlier n; against a verbatim copy of the old election (a temporary harness, not committed), every output and the whole observatory record identical over 1 600 epochs |
| off identical: field golden, 2 h | pass: identical in every CSV, the products, the manifest, the whole logs and the exit code |
| off identical: sim golden, 2 h | pass: identical in the same parts |
| off identical: A0 on r3c (the chain path), 2 h | pass: identical in the same parts |
| on: identical L1 and L2 tree edge sets per pair family every epoch | pass: r3c GPS 240 of 240 epochs; the golden bed GPS and Galileo 241 of 241 each; no edge and no datum receiver differs |
| on: pins per band = sum of (n_c + m_c - 1) + 1 per component; pin audit COMPLETE; mismatch 0 | pass: E + T = V in 480 of 480 (r3c) and 964 of 964 (golden bed) groups; every group's audit COMPLETE (2 on r3c, 4 on the golden bed); 0 pin/gauge-mismatch epochs |
| the oracle extension | pass: the three new cases (section 5.1) |
| A1 against A0: every edge-set difference attributed to a band-only arc | pass, with nothing to attribute: 0 differing edges in 480 and 964 groups; neither bed has a band-only vertex (0 extension edges, 0 band-only components, 0 fallbacks) |
| A1 against A0: float consumer by hour within max(+10 %, +5 mm) | pass: identical, 0.416 and 0.042 m on r3c, 0.071 and 0.029 m on the golden bed. A1's products are byte-identical to A0's on both beds; the whole runs differ only in the family column of the chain-edge trace and the two pair-family log lines |
| P-c on r3c and the golden bed | FAILED on four of six clauses, as A0 fails them (below) |

P-c on A1 (A0 reads the same):

| clause | r3c | golden bed | verdict |
| --- | --- | --- | --- |
| every epoch emitted | 240 of 240 | 241 of 241 | pass |
| W5 withholds no more than the golden | 0 (golden 0) | 0 (golden 0) | pass |
| postfit sigma0 within 10 % of the golden | 1.24 against 0.38 | 1.13 against 0.41 | FAILED |
| DD \|e\| p50 per band within +0.01 cycle of R1 | L1C 5.51 (R1 0.25), L2W 5.63 (0.31) | GPS L1C 3.64 (0.26), L2W 3.82 (0.32); Galileo L1C 2.70 (0.21), L5Q 2.81 (0.27) | FAILED |
| 0 wrong producer fixes | 3 695 of 5 056 kept | 12 552 of 17 258 kept | FAILED |
| receiver DCB sd at most 3x the golden, end of hours 1 and 2 | 9.45, 10.14 m (golden 0.024, 0.014) | GPS 7.11, 9.75 m (0.073, 0.054); Galileo 6.61, 6.44 m (0.055, 0.068) | FAILED |

### 5.4 Readings beyond the gate

**Slip fixtures t1 and t3** (a one-band slip on a tree edge, L1C and L2W): A1's products are byte-identical to A0's, the two band trees are identical every epoch and no extension edge is grown. Bundle A0 arms no slip-aware election, so a one-band slip moves no tree edge; the pair family's de-seed on either band acts only under that election. A0 on this exe reproduces IRD-2's A0 runs of both fixtures (products identical).

**The one-band fixture `ird_l2v16`.** S45000SIM, an interior vertex with two child satellites, loses L2W and C2W from epoch 152 to 183 (TOW 265 560 to 266 490); every other observation is rung 3b's.

| reading | A0 (signal family) | A1 (pair family) |
| --- | --- | --- |
| extension edges; band-only components | (knob off) | 32; 0 (the station hangs on L1C under G15 at every window epoch) |
| epochs whose two band trees differ; edges | 89 (to the run end); 878 | 32; 32 (the extension edges) |
| forest identity; pin audits; mismatch epochs | 480 of 480; COMPLETE; 0 | 480 of 480; COMPLETE; 0 |
| consumer 3D p50, hours 1 and 2 | 0.416, 0.046 m | 0.416, 0.046 m |
| DD \|e\| p50 against truth, L1C and L2W | 4.49, 4.59 | 4.45, 4.59 |
| fixes kept; wrong | 5 734; 4 012 | 5 850; 4 176 |
| reference-free residual p50, over the run and at its end | 2.54, 1.45 | 2.49, 1.36 |

At the loss, A0 keeps the station and its two children on L1C and re-hosts the children on L2W only, so its two band trees split and stay split to the run end. A1 re-hosts the children on both bands, to the hosts A0 takes on L2W (G15 under S43000SIM, G24 under S40000SIM), and keeps one tree. Its extension edge reverses the incumbent edge (G15 was the station's child), so the observatory labels the first window epoch's edge a cold re-host on a Kept row (card IRD C7). At the return the station's L2W arc re-enters as a Returning row read as a first sighting, in both arms. A1 differs from A0 from the loss on (146 (epoch, band) groups, none before it). IRD-2's full-gap arm over the same window (`_rap_ird2_a0_rv16`, exe `ird1f`) reads 2.47 and 1.34 cycles reference-free: A1 tracks the full gap, since its pair forest loses the station as a gap does.

### 5.5 Reading

On both gate beds every arc is carried on both bands at every epoch, and the signal family's two band trees already coincide (A0: no differing edge or datum receiver on the microscope, r3c and the golden bed). The pair family therefore elects the same forest, and A1 is A0 byte for byte in its products. The gate's structure clauses hold, and its A1-against-A0 clauses hold with nothing to attribute. P-c fails as A0 fails it: the double differences, the wrong fixes, the postfit sigma0 and the receiver DCB spread are the event damage of sections 2 and 3 and the GF-datum defect (IRDB section 3.5), which the family does not touch.

The one-band fixture runs the band extension on data. The structure holds through the loss and the return, and the pair family keeps one tree where the signal family's trees, once split by a one-band event, stay split to the run end: the case that joint certification across the two bands needs (card IRD section 3). Its cost is a re-host of the station's children on the band that kept tracking. On this bed every arm sits at bundle A0's event-damage floor (1.3 to 4 cycles reference-free), so the bed does not resolve that cost; the microscope with the floor off is the instrument (sections 2 and 3).

### 5.6 Standing

IRD-3 LANDED, default off (ledger UNVALIDATED), on branch `claude/distracted-germain-d93ac2` (not merged). Every structural and A1-against-A0 clause of its gate passes; P-c FAILED on four clauses, unchanged from A0. Card IRD's frozen rules name no stop at IRD-3. Next in card IRD section 11: the certification defect (sigma scale 3 still wrong; the frozen rule fixes it before IRD-4), then IRD-4's event rows armed on A1, whose `kept` row also meets the extension's reversed edge (C7).
