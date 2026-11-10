# CRS: why the event-harness leg consumer resumes a metre from convergence

**STATUS**

- **id:** CRS
- **topic:** harness
- **status:** closed
- **summary:** MEASURED and ATTRIBUTED 2026-10-26 (the caveat of card SCK section 9: in every null leg of card SMX the held-out consumer SZZ000SIM is 0.74 to 1.16 m off, against 1 to 7 cm in its seed over the same epochs). The leg generator stages the consumer correctly: the leg's consumer snapshot is the seed's converged consumer byte for byte (53 states, full covariance, position sigma 1.7 cm), and it is restored. Two defects meet at the first leg epoch. (1) Producer, every posture: the emit-side phase-bias hold is not in the producer snapshot (its rows are `Persist::ColdStart`), so the resumed producer restarts every held key at generation 1 on the current estimate. Every phase bias on the wire re-levels by up to 20.3 m (`g`), 21.0 m (`c`) and 12.8 m (`n`), while clocks and code biases stay within 2 to 47 mm of the seed's wire. The only declaration is the resume-seam mark (every clock, code-bias and phase-bias counter +1), which the consumer ignores on a held key by design, and the generation stays 1, so no key is re-floated by the hold reaction. The consumer's post-fit check re-floats the worst satellites; the 6 to 14 ambiguities it keeps absorb steps of 0.1 to 2.4 m, and they carry the metre. (2) Consumer, GPS + Galileo only: the consumer snapshot does not carry the initializer's latches, so the restored consumer re-seeds its Galileo ISB (10.76 to 0.40 m) and both receiver DCBs (-1.37 to 6.28 m, 1.05 to 7.19 m) from one epoch of code under its restored tight covariance. Four consumer-only replays per posture separate the two: continuous consumer on the seed's wire, the seed to 4 decimals; restored consumer on the seed's wire, cm on `c` and `n`, 14 m on `g`; continuous consumer on the leg's wire, about 100 m on `g` and `c`, a 33 m transient on `n`; restored consumer on the leg's wire, the leg to 4 decimals. Fixing the producer alone would move the `g` legs from 1.1 m toward 14 m; both fixes are needed. Ruled out: the generator's staging, the WLS clock re-seed at the restore (outputs identical to 4 decimals with it suppressed), the accumulated wind-up (identical). Nothing built or armed; decisions for the author in section 8.
- **source:** card [SCK](SCK_sat_clock_step_2026-10-25.md) section 9 (the caveat) and section 10 item 4; card [SMX](../../refound/SMX_scenario_matrix_2026-10-24.md), whose consumer channels read this consumer; the brief of the session that ran card SCK: diagnose why the leg consumer resumes far from convergence and say what a fix would need, measurement and attribution only. Background: card [PBH](PBH_phase_bias_hold_2026-09-19.md) section 6.6 (the hold is not snapshotted; "conservative, ungraded").
- **knobs / posture:** no knob built, armed or changed; no golden, ledger or code edited. Postures as card SMX: `g` (the sim golden, GPS + Galileo, `emit_pb_hold_phase_clock`, wire `pbhold=2`), `c` (GPS only, the regime of card SCL2B, `emit_pb_hold_smoothed`, `pbhold=3`), `n` (`c` plus the broadcast a priori clock). The replay arms of section 7 are each null leg's own `leg.json` with the replay keys of `scripts/splice_wire.py` set; diagnostic arms add only logging levels, and two add `processing.data_gap_tolerance_s` 45 (section 6.4).
- **dates in the text:** 2026-10-26
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/worktrees/smx/build/x64-Release/gnss_processor_smx1.exe` (sha256[:16] `cac2bbdea1d05965`; card SMX's gate G0 shows it hash-equal to the seeds' `gnss_processor_smx_main.exe` with no injection); seeds `evseed_smx_{g,c,n}` and null legs `evleg_smx_smx1_null60_{g,c,n}` (snapshot `ep0485`, TOW 275580, 61 leg epochs TOW 275610 to 277410) under `D:/GnssProcessorSupport/runs_nom`; this card's consumer-only replays `cres_<posture>_*` there (section 9), spliced wires `cres_<posture>_splice_products.txt`; traces read: `products.txt`, `stdout.txt`, `logs/network_events.csv`, `logs/closed_loop_trace.csv`, `logs/closed_loop_sat_trace.csv`, `logs/kalman_filter.log`, `kalman_debug.log`, `logs/measurement_model.log`, `logs/ppp_model.log`, `logs/state_initializer.log`, the consumer snapshots
- **gates:** none (an attribution card).

**Outcome.** Question: why is the held-out consumer of every event-harness leg 0.7 to 1.2 m from convergence when the seed it resumes from is at 1 to 7 cm? Decisive result: the snapshot is the converged consumer and is restored, but the resumed producer re-levels every held phase bias (up to 21 m) under a declaration held keys ignore, and on the GPS + Galileo golden the restored consumer also re-seeds its ISB and receiver DCBs. Replays isolate each: the seam alone breaks a consumer that never restarted (about 100 m on `g` and `c`), the re-seed alone costs 14 m on `g`, and together they give the legs' 0.7 to 1.2 m. Verdict: two defects, producer and consumer, both owed. Standing: section 8; nothing armed.

## 1. The question

Card SCK section 9 found the leg consumer of every SMX null leg 1.07 to 1.14 m off on `g`, 0.74 to 1.16 m on `c` and 0.89 to 1.00 m on `n`, against 0.7 to 3.5, 1.1 to 6.7 and 0.9 to 6.2 cm in the seeds over the same epochs, with the jump present at the first leg epoch (`g`: 1.106 m at TOW 275610, the seed 0.012 m). At that epoch the consumer warm-restores, reports a code-bias and clock "datum re-reference" on every satellite and re-seeds its receiver clock, and the producer's resume seam marks 63 of 64 GPS and 49 of 50 Galileo phase-bias keys. Three readings were open: the resume is applied and then undone by the producer's re-realization; the snapshot is not the converged state; or the leg generator stages the wrong file.

## 2. Method

- **Wire against wire.** The seed and the leg are runs of the same lineage on exes card SMX shows hash-equal, so the seed's `products.txt` is the wire the leg should have emitted. Both are compared record by record at the leg's epochs (clock `C`, code bias `B`, phase bias `P`, the `gen=` token, the `E` record's datum flag).
- **Consumer against consumer.** The per-satellite consumer trace (`closed_loop_sat_trace.csv`: applied corrections, discontinuity counters and reactions, innovations, ambiguity and its variance) of the leg is compared with the seed's at the same epoch.
- **Separation by consumer-only replay.** The replay driver (`network.closed_loop.replay_products_path`) runs the consumer alone on a recorded wire with the leg's own consumer configuration. Per posture, four arms cross the consumer's start (continuous from the seed's start, or restored from the leg's consumer snapshot) with the wire (the seed's own, or the seed's spliced to the leg's at TOW 275610 by `scripts/splice_wire.py`). A restored arm starts at TOW 275610 (start offset 4.0375 h from the wire's first epoch, half an epoch backed off) with the leg's restore keys (`restore_scope` full, seam refloat off).
- **Inside the first update.** Diagnostic copies of the replay arms raise the `kalman_filter` logger to debug and trace (the trace writes `kalman_debug.log`: predicted state and covariance, innovations, H, R), `measurement_model` to trace (the per-row decomposition) and `state_initializer` to info.
- **One edit to an input, declared.** The consumer snapshot's fingerprint includes the route flag of the in-process ionosphere supply, which a file replay cannot receive (the driver warns it is inert). A replay therefore refuses the leg's snapshot on fingerprint (live `30d7d945a4f6f13f` on `g`, `4c0d48bf82719ce5` on `c` and `n`). The disposable snapshot copy of each restored arm carries the replay's fingerprint in its `M` line; nothing else in it is edited. The edit is safe on these postures because the supply carries nothing here: the continuous replay reproduces the seed's in-process consumer, and the restored replay on the leg's wire reproduces the leg's in-process consumer, both to 4 decimals on every posture (section 7).

## 3. The leg's consumer snapshot is the converged consumer

- `seed_consumer.txt` of the `g` null leg is byte-identical to the seed's `state_snapshot.txt.consumer.ep0485`, written at TOW 275580, the network snapshot's TOW. It holds 53 states (6 core, 30 ambiguities, 15 slant ionosphere, the GPS and Galileo receiver DCBs), the full lower-triangular covariance (X variance 2.8e-4 m², sigma 1.7 cm), 140 counter baselines (`D`), 56 phase and 56 code A5 flags (`F`, `C`).
- Its position is the seed consumer's epoch-485 solution to the millimetre ((5378997.682, -1593331.704, 3025603.098); 3D error 0.0217 m).
- The leg restores it in full at the first leg epoch (`WARM RESUME applied - 53 states restored`, age 30 s, scope full, seam refloat off).
- At the first update the restored predicted state equals the continuous consumer's to 1e-6 in every element (section 6.2), so the state that arrives is the converged one. The generator's staging (`scripts/event_leg.py` of branch `claude/sweet-poincare-454cec`, `_leg_common`) is not the defect.

## 4. The producer's resume seam re-levels every held phase bias

**The wire.** Leg minus seed at TOW 275610, the leg's first emitted epoch:

| posture | clock, max abs diff | code bias | phase bias, max abs diff | `E` record | resume-seam mark |
|---|---|---|---|---|---|
| `g` | 2 mm (GPS, 32 sats), 3 mm (Galileo, 25) | 0.0000 m | GPS L1C 17.14, L2W 20.33; Galileo L1C 7.27, L5Q 18.46 m | `datum_changed` 1, `EG` | G 63/64, E 49/50 keys, max 20.325 m |
| `c` | 35 mm (32) | 0.0000 m | L1C 17.41, L2W 21.04 m | 1, `G` | 62/64, max 21.042 m |
| `n` | 47 mm (31) | 0.0000 m | L1C 7.63, L2W 12.80 m | 1, `G` | 62/62, max 12.789 m |

The phase-bias differences are levels, not transients: on `g` the L2W maximum is 20.325 m at the first leg epoch and 20.324 m four epochs later, and every family's median and spread are unchanged over those epochs. They are not integer cycles (G12 L1C -0.501 m, -2.63 cycles) and often have the ionospheric ratio between the two signals of a satellite (G12 -0.501 and -0.838 m, ratio 1.67; E02 +0.452 and +0.808 m, ratio 1.79).

**The generations.** On the seed's wire at TOW 275580, 104 of 114 held keys are in generation 1 and 10 in generation 2 on `g` (58 + 6 of 64 on `c`, 56 + 6 of 62 on `n`). On the leg's wire every key is in generation 1, and the producer logs 114 `PB_HOLD gen_start ... first gen=1` events at TOW 275610.

**The mechanism.** Every row of the hold's state is declared `Persist::ColdStart` in `src/core/network/state/Entities.cpp`: `pb_hold` and `smh_clone` per satellite signal (lines 655 to 657), `pb_hold_frz_M` per constellation (line 228), `pb_hold_prev_clock_m` and `pb_hold_frz_prev_clk` per satellite (lines 501 to 504). A warm resume therefore starts the hold empty, and `networkPbHold` treats each key as a first sighting (`src/core/network/emit/PbHold.cpp` line 183): generation 1, held value equal to the current estimate. The seed's held values were frozen at the start of each key's generation, up to four hours earlier, and moved since only by the hold's transfer, so the re-level is the whole accumulated difference between the estimate and the held value. The restored producer's clocks and code biases, which are snapshotted, are continuous to 2 to 47 mm. The leg generator arms `network.resume_pb_seam_mark`, which compares the first emit with the restored emitter baselines, finds more than 10 % of a constellation's keys beyond the threshold, and declares the constellation's datum changed: every clock, code-bias and phase-bias counter of that constellation steps by 1.

## 5. Why the consumer does not answer, and what carries the metre

At TOW 275610 the leg consumer (`closed_loop_sat_trace.csv` against the seed's):

- **Every counter +1 on every key** (clock, code bias and phase bias: 30 keys on `g`, 16 on `c` and `n`). The clock and code-bias counters drive `reinflate_isb` and `reinflate_disc` (the logged "datum re-reference"), although those values did not move.
- **No phase reaction anywhere** (`ev_pb` none on all keys). Every key is held, so `reactToPbHold_` (`src/core/realtime/RealTimeEngineSsrBreaks.cpp` line 662) claims it: the counter change is consumed and ignored by design ("the hold exists so that the consumer does not react to the producer's discontinuity marks"), and a re-float needs a change of generation. The restored consumer's hold ledger is empty (it is not in the consumer snapshot), so all 30 keys are first sightings; its run-end line reads `first_sight 30 / held 1688 / refloats 0; disc-counter changes IGNORED 46`. A continuous consumer's ledger would see generation 1 to 1 on 22 of its 30 keys and 2 to 1 on the other 8 (section 7).
- **The post-fit check re-floats the worst satellites** (ambiguity variance 1e6 after the update): 16 of 30 keys on `g` (8 satellites; their phase-bias steps 0.35 to 20.32 m), 10 of 16 on `c` (0.20 to 21.04 m), 2 of 16 on `n` (7.63 to 8.47 m).
- **The kept ambiguities take the rest.** Kept keys and their steps: 14 on `g` (0.25 to 1.98 m), 6 on `c` (0.29 to 0.92 m), 14 on `n` (0.11 to 2.41 m). For each kept key the leg's phase innovation equals the seed's minus that key's phase-bias step plus one term common to the constellation (the clock re-seed of section 6.4 and the ISB), to 0.6 mm (`g` GPS), 1.4 mm (`g` Galileo), 9.6 mm (`c`) and 29 mm (`n`). The kept ambiguities are the restored, tight values fitted to the seed's held phase biases; each takes a per-key step that is not common to a constellation, so it cannot be absorbed by the clock and reaches the position: 1.106 m on `g` at the first epoch, 0.744 m on `c`, 0.934 m on `n`, held for the whole leg (`g` 1.068 to 1.141 m) by ambiguities that stay tight.

The metre is therefore carried by the kept ambiguities and the position, and its source is the producer's re-level of held phase biases that the consumer has no declared reason to re-float.

## 6. The consumer's own resume on the GPS + Galileo golden

### 6.1 The initializer re-seeds restored states

The consumer snapshot (format v1, `include/realtime/RtStateSnapshot.hpp`) carries `M`, `G`, `K`, `X`, `P`, `D`, `F` and `C` records. It does not carry the initializer's latches. `MeasurementModel::computeMeasurementUpdate` runs the first-epoch seeds of the inter-system bias and of the receiver code biases while `isIsbInitialized()` and `isCodeBiasInitialized()` are false (`src/core/measurement/MeasurementModel.cpp` lines 195 and 219), and on a restored consumer they are. At the first post-restore epoch (arm `cres_g_A2s_seedwire_warm_seedlog`):

```
Initialized Galileo ISB: 0.399 m (1.330e-09 s) from 7 code obs (GPS ref: 8 obs)
Initialized code bias GPS_DCB: 6.275 m from 8 satellites
Initialized code bias Galileo_DCB: 7.189 m from 7 satellites
```

against restored values 10.758, -1.373 and 1.052 m. The seeds overwrite the values and keep the restored covariance, which is tight. On `c` the GPS DCB is re-seeded the same way (6.255 m against a restored -1.376 m).

### 6.2 What the first update sees

The continuous replay and the restored replay on the seed's own wire, first pass of the update at TOW 275610 (`kalman_debug.log` of arms `cres_g_A0m_*` and `cres_g_A2m_*`):

- predicted state identical (no element differs by more than 1e-6); H and R identical; covariance identical except the 15 ionosphere variances (restored 0.122 to 0.136 m², continuous 0.125 to 0.496 m²: the ionosphere process noise reads a per-satellite elevation memory that is empty after a restore and falls back to zenith scaling, `src/core/filter/StateManagerProcessNoise.cpp` line 229);
- innovations: GPS phase rows identical; GPS code rows +11.822 m (C1C) and +19.470 m (C2W) on every satellite, minus the receiver DCB coefficients (-1.5457, -2.5457) times the re-seed's DCB step of +7.648 m (-1.373 to 6.275); Galileo phase rows +10.359 m on every satellite, minus the ISB step of -10.359 m (10.758 to 0.399); Galileo code rows +18.096 and +24.234 m, the ISB term plus 1.2606 and 2.2606 times the Galileo DCB step of +6.137 m (1.052 to 7.189).

The row decomposition confirms it: the receiver code-bias term of G10 C1C is +2.122 m in the continuous consumer and -9.700 m in the restored one.

### 6.3 Consequence

On the seed's unbroken wire (arm `cres_g_A2_seedwire_warm`), the restored consumer's first update moves the state by 19.47 m (continuous: 9.51 m, the receiver clock's step), with 51 of 60 rows beyond the 6-sigma post-fit threshold (largest 599.6, G12 L1C; continuous: none, largest 2.40). The check re-floats 10 of 15 satellites and then excludes code rows, the position is not updated at the first epoch (0.022 m), and from the second epoch the consumer sits 13.25 to 14.28 m off for the rest of the leg.

On `c` and `n` the same arm tracks the seed (section 7), although the GPS DCB is re-seeded there too: a DCB change reaches the code rows only, and is absorbed. On `g` the additional change is the Galileo ISB, which moves every Galileo phase row by 10.36 m against tight restored ambiguities. The attribution of the `g` failure to the ISB rather than the Galileo DCB is by this contrast between postures; no arm separates the two.

In the leg this defect is masked. The seam mark's clock and code-bias counter steps make the consumer re-inflate its ISB and receiver code-bias variances, which frees exactly the re-seeded states; the leg reads 1.1 m rather than 14 m for that reason.

### 6.4 Ruled out

- **The clock re-seed.** The restore leaves the epoch-cadence window empty, so the first post-restore spacing (30 s) is read as a gap against a 1 s tolerance (`src/core/realtime/RealTimeEngineAdmit.cpp` line 194), and `seedClockFromWls` overwrites the restored clock with a code-only WLS value (`Seeded clock: wls=34914.0 kf=34875.0 d=39.0`; the truth is about 34884.5). With `processing.data_gap_tolerance_s` 45 (arms `cres_g_A2g_*` and `cres_g_A3g_*`) the re-seed does not fire and every consumer output is identical to 4 decimals: the clock is re-estimated freely at each update, and the re-seed is a log line, not a cause.
- **The accumulated wind-up.** It is not in the consumer snapshot, but at TOW 275610 the continuous and restored consumers apply the same angle to every one of the 15 satellites (arms `cres_g_A0t_*`, `cres_g_A2t_*`; every angle within (-π, π] on this static receiver).
- **The staging** (section 3).

## 7. Separation: four replays per posture

Consumer 3D error in metres: at TOW 275610 (the first leg epoch), and the median and maximum over the 61 leg epochs.

| posture | A0: continuous consumer, seed's wire | A2: restored consumer, seed's wire | A1: continuous consumer, leg's wire | A3: restored consumer, leg's wire |
|---|---|---|---|---|
| `g` | 0.012; 0.018, 0.035 | 0.022 (no update); 13.96, 14.28 | 0.022 (no update); 99.8, 103.1 | 1.106; 1.106, 1.141 |
| `c` | 0.023; 0.035, 0.067 | 0.022; 0.035, 0.066 | 0.030 (no update); 93.9, 102.4 | 0.744; 1.076, 1.160 |
| `n` | 0.029; 0.031, 0.062 | 0.028; 0.031, 0.062 | 32.59; 0.41, 32.59 | 0.934; 0.948, 0.996 |

- A0 equals the seed's own consumer and A3 equals the leg's own consumer, to 4 decimals at every epoch on every posture: the replay is an exact stand-in for both in-process consumers.
- A2 against A0: the consumer's resume, alone, costs nothing on `c` and `n` (`c`: 0.0223 against 0.0232 m at the first epoch, median 0.0352 against 0.0354 m) and 14 m on `g` (section 6).
- A1 against A0: the producer's seam, alone, breaks a consumer that never restarted. On `g` its first update needs 112.9 m with 50 of 60 rows beyond the threshold (largest 1102); the filter rejects the update for two epochs (position sigma 54.8, 77.5, 94.9 m) and then accepts a solution about 100 m off. This consumer did re-float the 8 keys it tracked in generation 2 (G12, G25, E02, E25; `HOLD generation 2 -> 1`); its 22 keys in generation 1 re-levelled without an edge. On `n` the error peaks at 32.6 m and returns to 0.39 m by the end of the leg.
- A3, the leg, is the two defects together. The seam mark that is part of the producer's defect rescues the consumer from its own defect on `g` (section 6.3), and the post-fit check's choice of which satellites to keep sets the 0.74 to 1.16 m.

Card PBH section 6.6 expected a warm resume to be conservative because "a consumer holding generation 2 then sees a change and re-floats". Graded here: at the seam 104 of 114 keys on `g` (58 of 64 on `c`, 56 of 62 on `n`) were in generation 1 and re-levelled with no edge, and the one consumer that re-floated its generation-2 keys (A1) still lost about 100 m to the rest.

## 8. What a fix would need (decisions for the author)

Nothing is built or armed. Each item is the author's decision.

1. **Producer, needed on every posture: carry the hold across a warm resume.** Record and restore the hold's rows (`pb_hold` and the smoothed hold's `smh_clone` per satellite signal, `pb_hold_frz_M` per constellation, `pb_hold_prev_clock_m` and `pb_hold_frz_prev_clk` per satellite), so the first leg emit continues the seed's held values and generations as the restored clocks and code biases already do. The resume-seam mark should then find no keys to mark (inferred from its test, which compares the first emit with the restored baselines; not run). The alternative, declaring the restart by starting every restarted generation above the restored one, turns each producer restart into a re-float of every held key at every consumer, and a restored consumer would see it only with item 3.
2. **Consumer, needed on the GPS + Galileo golden as soon as item 1 lands: a restore must not re-run the first-epoch seeds of the states it restored.** The initializer's latches belong in the consumer snapshot, or a restore that brings an ISB or a receiver code-bias state sets them. Item 1 alone would remove the seam mark that masks this defect today and move every `g` leg from the 1.1 m of A3 toward the 14 m of A2.
3. **Consumer, latent: the hold ledger in the consumer snapshot.** A restored consumer reads every held key's generation at its first epoch as a first sighting, so a generation edge across the outage (the alternative in item 1, or any real re-level during it) is missed. Smaller items of the same snapshot: the ionosphere elevation memory (zenith scaling of the first predicted ionosphere variances) and the epoch-cadence window (the false gap of section 6.4).
4. **Harness: card SMX version 2 waits on items 1 and 2.** The leg generator sets `realtime.state_snapshot_seam_refloat` false, which the consumer's own log line calls "only sound against a producer proven emission-continuous across the outage"; item 1 makes that condition true. Until then every consumer number of SMX version 1 and of card SCK is read on a consumer that absorbed 0.1 to 2.4 m phase-bias steps into 6 to 14 kept ambiguities. Event-minus-null differences remain differences, but of that consumer.

## 9. Run record

All runs on the exe above, Release, at most three at a time, under `D:/GnssProcessorSupport/runs_nom`. Each is a consumer-only replay (no producer solve), 7 to 27 s.

- Per posture `<p>` in `g`, `c`, `n`: `cres_<p>_A0_seedwire_cont`, `cres_<p>_A1_splice_cont`, `cres_<p>_A2_seedwire_warm`, `cres_<p>_A3_splice_warm`; wire `cres_<p>_splice_products.txt` (485 seed epochs then 61 leg epochs). The first `cres_<p>_A2` run of each posture was refused on fingerprint and cold-started; its outputs were overwritten by the rerun with the fingerprint edit of section 2.
- Diagnostics on `g`: `cres_g_A2g_seedwire_warm_gap45`, `cres_g_A3g_splice_warm_gap45` (gap tolerance 45 s); `cres_g_A0t_seedwire_cont_trace`, `cres_g_A2t_seedwire_warm_trace` (wind-up angles, post-fit debug); `cres_g_A0k_seedwire_cont_kf`, `cres_g_A1k_splice_cont_kf` (post-fit debug); `cres_g_A0m_seedwire_cont_kfmat`, `cres_g_A2m_seedwire_warm_kfmat` (`kalman_debug.log`, 114 MB and 12 MB); `cres_g_A0d_seedwire_cont_decomp`, `cres_g_A2d_seedwire_warm_decomp` (row decomposition); `cres_g_A2s_seedwire_warm_seedlog` and, on `c`, `cres_c_A2s_seedwire_warm_seedlog` (initializer log).

## 10. 2026-11-06: items 1 and 2 of section 8 built (card CRS2)

Card [CRS2](CRS2_resume_hold_and_latches_2026-11-06.md) built item 1 (the hold's rows in the producer snapshot, `network.state_snapshot_hold_capture` / `network.resume_hold_restore`) and item 2 (the initializer's latches in the consumer snapshot, `realtime.state_snapshot_latch_capture` / `realtime.state_snapshot_latch_restore`), all default off and armed by the event harness. On the re-written seeds the null leg's consumer reads 0.012, 0.020 and 0.072 m at the first leg epoch on `g`, `c` and `n` (this card: 1.106, 0.744, 0.934 m) and tracks the seed's own consumer within 1 mm, 7 mm and 21 mm (median) over the leg; the wire is continuous to 3 mm on `g` and to centimetres on the smoothed-hold postures (the clone re-born, card CRS2 section 5.2). The prediction of section 8 item 2 held once the resume seam mark was read after the hold (card CRS2 section 5.3): the producer fix alone leaves the `g` consumer at 13.96 m median, arm A2's number. Item 3 (the consumer's hold ledger, the ionosphere elevation memory, the cadence window) remains.
