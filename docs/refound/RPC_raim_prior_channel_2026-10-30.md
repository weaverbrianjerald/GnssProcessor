# RPC: the post-fit RAIM's prior channel (carried-prior rows tested against their own column)

**STATUS**

- **id:** RPC
- **topic:** raim-qc
- **status:** open
- **summary:** Pre-registered 2026-10-30, gates frozen in section 6 before any run of a changed executable. The first defect card RPA named (section 4, "What this points to"): the post-fit RAIM pools admit whitened carried-prior rows as phase data, so a large prior residual is spread over every ambiguity column its row clears and dilutes the observation pool, while card RPA's cure (observation rows only) removed the only brake on a cold-start G07 clock runaway at BSHM00ISR. The new key `network.raim_prior_channel` (default off) keeps the observation pools to observation rows and tests every carried-prior row on its own channel, in the cell of its own column, with the T4 isolated-arc test; a flag answers the carry it belongs to.
- **source:** card [RPA](RPA_raim_pool_data_rows_2026-10-08.md) sections 4 and 5 (the defect and the event); card [ICF](ICF_ignition_code_fault_2026-10-09.md) sections 1, 6 and 7 (the conduit, and the cold-start bound now armed in both goldens); card [NOM](NOM_network_object_model_2026-10-08.md) section 16 (11 of 12 refloats through prior rows); card [MGT](MGT_maturity_gate_telemetry_2026-10-30.md) (the T4 block and the maturity gate's telemetry); `src/core/network/qc/Raim.cpp`, `src/core/network/qc/RaimWithinEpoch.cpp`, `src/core/network/carry/Coupling.cpp`
- **knobs / posture:** new `network.raim_prior_channel` (default off; not fingerprinted, a QC key like the other `raim_*` keys); read, not changed: `network.raim_pool_data_rows_only` (KEEP_OFF), `network.gross_los_cold_bound_m` (12 m in both goldens, set to 0 in the three event arms only). Base: `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json` of main `5127429`.
- **dates in the text:** 2026-10-30
- **hashes / run dirs:** base exe `gnss_processor_rpc0.exe` (the Release build of main `5127429`), changed exe `gnss_processor_rpc1.exe` (the Release build of this branch), both under `D:/GnssProcessorSupport/gates/exe/x64-Release/`; run root `D:/GnssProcessorSupport/runs_nom`, arms `_rap_rpc_*` (section 5)
- **gates (frozen at pre-registration):** section 6

**Outcome.** Owed at close.

## 1. The question

The post-fit RAIM pass (`networkPostFitRaim`) and the within-epoch tier (`weAssess`) form one pool per carried
ambiguity column of a station: every row whose coefficient on the column exceeds 0.5 and whose kind the declared rule
admits (`raimPoolAdmitsKind`). On the float vehicle the station block also holds the whitened carried-prior rows of the
local carry block (`block_ambiguity_carry`, `block_rx_cb_carry` on both goldens), and one such row enters the pool of
every column it clears. On card RPA's 2 h field arm the prior rows were about 90 % of the post-fit pool's samples
(1 653 864 ambiguity-block and 295 004 receiver code-bias block visits against 203 446 observation rows) and decided 11
of 12 isolated-arc (T4) refloats (card NOM section 16). Card RPA's knob admitted observation rows only and failed its
gate G3: at BSHM00ISR, in the cold start, the prior rows' refloats were the only check of a G07 clock runaway driven by a
faulty L1 receiver channel (-11.67 m without them, -2.98 m with them; card ICF section 6 traced the conduit to the line
of sight's carried ambiguity, which makes the faulty line of sight redundant).

Card RPA's section 4 names the first defect: the prior rows should be tested, but on their own channel and against their
own column (the row's `cpl_row_local_cols`), so that a large prior residual triggers a response to the carry it belongs
to, without being spread over every column it touches and without diluting the observation pool. This card builds that
channel, default off, and grades it.

Since card RPA, card ICF's cold-start bound (`network.gross_los_cold_bound_m`, 12 m) has been armed in both goldens
(ledger RECOMMEND_ON): on the current golden the gross line-of-sight screen declares BSHM00ISR G07 at 259410 and holds the
clock within 0.03 m, so the prior rows are no longer the only brake there. The brake that card RPA's G3 asked for is
therefore graded on the posture card RPA measured it on: the golden with the cold-start bound set to 0 (section 5, the
three event arms).

## 2. What a whitened block row says about its column

The carried local block of a station is folded as rows of `L^-1 [I | M | y]`, where `Sigma = L L^T` is the joint prior
covariance of the carried local columns in push order (ambiguities first, then the receiver code biases on the goldens)
and `M` the regression on the shared border (`CouplingRowBuilder::buildLocalBlock`). Row `k` is lower-triangular: it has
coefficients on block columns `0..k` and its diagonal on column `k`, its primary column, which `cpl_row_local_cols`
records and whose identity its tag carries. Its residual is the standardized innovation of column `k` given columns
`0..k-1` (the sequential, Cholesky-order decomposition of the prior residual vector).

Two consequences fix the design:

- **Row `k` belongs to column `k` in the Cholesky order, and only there.** Admitting it to the pool of every column it
  clears counts one prior residual once per column, at full weight. Testing it once, in the cell of column `k`, is the
  own-column rule of card RPA section 4.
- **A fault in column `j` reaches row `j` and every later row.** With the prior residual `e = delta * u_j`, the whitened
  residual is `delta * L^-1 u_j`, non-zero in rows `j..n-1` in proportion to `(L^-1)_kj`. For two columns with prior
  correlation `rho` the later row carries `|rho| / sqrt(1 - rho^2)` times the own row's share, more than the own row when
  `|rho| > 0.707`. The channel's cells are per line of sight (the largest own-column statistic over the line of sight's
  signals), so a spread between the bands of one satellite (strongly correlated through its slant ionosphere) stays in
  the right cell; a spread across satellites, or from an ambiguity into the receiver code-bias rows pushed last, is the
  risk read by reading R-b (section 4). The order-free alternative is the column's w-test, `T_j = (a_j^T eps) / ||a_j||`
  with `a_j` column `j` of the prior rows' `A_r`, which localizes a single-column fault to its column; it is not built
  here (card RPA section 4 specifies the row's own column), and section 4 states when it would be the next step.

## 3. What is built

| piece | where | what it does |
| --- | --- | --- |
| the key | `network.raim_prior_channel` (`GnssConfig`, `GnssConfigLoadNetwork.cpp`, `NetworkKnobs::raim_prior_channel`) | default off; requires `network.raim` and `network.raim_attribution` (the loader disables it with a warning otherwise); an arming line; emitted in the run manifest only when armed |
| the observation pools | both pool walks (`Raim.cpp`, `RaimWithinEpoch.cpp`) | with the key on they admit the observation phase rows only, each to the pool of its own ambiguity column: card RPA's rule (`raimPoolAdmitsRow`), so sigma0, the T3 and T4 cells and the within-epoch tier read observation residuals only. The key sets the resolved `NetworkKnobs::raim_pool_data_rows_only`; the config key `network.raim_pool_data_rows_only` is unchanged |
| the prior channel's rows | `raimPriorChannelCollect` (`network/qc/RaimPriorChannel`) | every row of the station's coupling region (rows `cpl_row_start` to `cpl_row_start + cpl_row_local_cols.size()`, kinds `coupling_scalar` and the six `block_*_carry`), its statistic `|v| sqrt(w)` (divided by `sqrt(r_i)` under `network.raim_redundancy_normalize`, the observation pool's convention; a row at the redundancy floor is counted and left out), its own column `cpl_row_local_cols[k]`, and its subject |
| the subject of a row | `raimPriorSubjectOf` | a line-of-sight carry (an ambiguity, a slant ionosphere or a phase colour: the satellite of the row's tag), a receiver code bias (the code-bias state key of the own column, from the station's `pl_cb` list), or an unwired station carry (the wet delay, the receiver clock, or a coupling row with no satellite that is not a code bias) |
| the prior table and its test | `raimPriorChannelSelect` | one cell per (station, subject): the largest statistic of the subject's own-column rows; a median polish of the table, a robust scale of its own (1.4826 times the median of the epoch's prior statistics, floored at `network.raim_s0_floor`), and the T4 isolated-arc test (`raimSelectIsolatedArcs`): an interaction above `network.raim_k_sigma` times that scale, the observation channel's wholesale satellites excluded, the maturity gate's verdict applied, no persistence gate |
| the responses | `networkPostFitRaim` | a line-of-sight flag takes the T4 arc response (`refloat_sat_at`: the bounded drop of the arc's carried ambiguities on the float vehicle, the slant-ionosphere and phase-colour erase, the pending-reseed mark and the held-basis and suspect stamps), once per arc and not again for an arc the observation channel refloats; a receiver code-bias flag erases that carried code bias (the reset the harvest's step test takes); an unwired flag is counted and written, and changes nothing |
| events | `network_events.csv`, tier `T4_prior` | `prior_arc` (refloat), `prior_rxcb` (reset), `prior_unwired` (none) and `prior_held` (hold, the maturity gate), with the statistic, the bar and the row's kind, own column and the coefficient check below in the detail; the `T4_attrib` rows keep their meaning |
| the attribution trace | `network_raim_attrib.csv` | `PCELL` rows beside the `ARC` rows (the prior cells above prominence 4, at most 20 per epoch, with `acted`) |
| the coefficient check | each prior row | the row's coefficient on its own column against its largest coefficient on another carried column of the station; a row whose largest coefficient is on another column is counted (the size of the Cholesky-order spread of section 2) and named in a flag's event |
| run-end | `networkRaimPriorChannelRunEnd`, a section of the RAIM report | passes, rows by kind, rows at the redundancy floor, cells, the scale, flags by subject, responses, arcs both channels flagged, held flags, unwired flags (warned), the largest statistic with its identity, the coefficient check; ARMED BUT NEVER RAN, NO PRIOR ROWS (the key acts on nothing) and NO FLAG are said in words |

Off, every walk, rule, counter, trace and log line is today's; nothing prints. The ambiguity drops the channel takes are
counted in the existing `amb_raim_drop` response row (RAIM's attribution) and in `network_amb_state.csv` under the cause
`drop:t4_prior`.

**Declared limitations** (triaged per standing invariant 3):

- No persistence gate in the channel (`network.raim_arc_persist_epochs` above 1 applies to the observation channel only;
  the loader warns). Field-inert: both goldens run K = 1.
- The within-epoch tier keeps the observation pool only and has no prior channel. Field-inert on this evidence: card
  RPA's 2 h field arm counted 0 within-epoch triggers with and without its knob; counted again here.
- The receiver code-bias reset is not a row of the parameter-response table (`network/assembly/Responses.hpp`): a new
  site would add an entry to the `param_responses` record of every run's manifest, so the key-off identity would not
  hold. The channel's tally and its event rows carry the count and the identities; the table row is a migration owed
  when the key is armed. Field-inert while the key is off.
- The wet delay and the receiver clock in the block have no channel response (neither is armed in either golden;
  counted as unwired and warned at run end and at load).
- A station with no checkable ambiguity column is skipped by both channels, as today.

## 4. Readings (pre-registered)

- **R-a (the channel keeps the brake).** At BSHM00ISR the own row of the G07 line of sight's carried ambiguity flags,
  its refloat stops the runaway as the spread refloats did, and elsewhere the spread refloats give way to fewer responses
  each attributed to one carry. Prediction: G3 passes; the users move within the bars of G4 and G5.
- **R-b (the Cholesky order misplaces the fault).** A carry's fault surfaces mostly in later rows (other satellites, or
  the receiver code-bias rows pushed last), so the channel answers the wrong carry: at BSHM00ISR the G07 cell does not
  flag in time, or the receiver code bias is reset instead, and the runaway is not braked. Prediction: G3 fails with a
  `prior_rxcb` reset or a flag on another satellite at BSHM00ISR, and the coefficient check counts many rows dominated by
  another column. The remedy is then the column's w-test of section 2, a card of its own.

## 5. Arms

One base exe (the Release build of main `5127429`) and one changed exe (the Release build of this branch). Every arm is
a golden copy made with `scripts/rapid_arm.py make` under `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`; every sim
arm sets `network.start_offset_hours=0.504167`. At most two arms run at once (card NOM5's gate agents share the box);
no wall-clock number is taken.

| tag | exe | base | change | length |
| --- | --- | --- | --- | --- |
| `rpc_b_f2`, `rpc_b_s2` | base | field, sim golden | none | 2 h |
| `rpc_off_f2`, `rpc_off_s2` | changed | field, sim golden | none (the key off) | 2 h |
| `rpc_on_f2`, `rpc_on_s2` | changed | field, sim golden | `network.raim_prior_channel=true` | 2 h |
| `rpc_rpa_f2` | changed | field golden | `network.raim_pool_data_rows_only=true` (card RPA's knob: the pool restriction without the channel) | 2 h |
| `rpc_c0off_f2` | changed | field golden | `network.gross_los_cold_bound_m=0` (card RPA's posture: the prior rows in the pool) | 2 h |
| `rpc_c0rpa_f2` | changed | field golden | the cold bound 0 and `network.raim_pool_data_rows_only=true` (no brake) | 2 h |
| `rpc_c0on_f2` | changed | field golden | the cold bound 0 and `network.raim_prior_channel=true` | 2 h |
| `rpc_off_f6`, `rpc_on_f6` | changed | field golden | none; the key | 6 h |
| `rpc_off_s6`, `rpc_on_s6` | changed | sim golden | none; the key | 6 h |

The three event arms change two keys against the golden on purpose: the cold bound is set to 0 to restore the posture
on which card RPA measured the event, and each pair among them differs by one key. Census: `scripts/clock_level/gcensus.py
make rpcc_off <rpc_off_f6> <rpc_off_f6> <exe>` and `make rpcc_on <rpc_off_f6> <rpc_on_f6> <exe>` (the consumer knobs are
the control's for both wires; the key is a producer key), graded with `gcensus.py grade rpcc_off rpcc_on`. The 6 h arms
run once G1 and G2 pass, whatever G3 reads.

## 6. Gates (frozen 2026-10-30, before any run of the changed executable)

| gate | arms | measure | bar |
| --- | --- | --- | --- |
| G0 unit | Debug `gnss_tests` | the new suite `RaimPriorChannel` and the full suite | the new cases pass; the full suite green apart from fixture-gated skips |
| G1 key off | `rpc_b_*2` against `rpc_off_*2` | `python scripts/nom_td_gate.py <A> <B> --whole` on the field and the sim pair | IDENTICAL on both (exit 0); a reported difference fails the gate |
| G2 mechanism (invariant 4) | `rpc_on_f2` | the run-end section and the events | the channel ran on every post-fit pass, tested `block_ambiguity_carry` and `block_rx_cb_carry` rows, and the observation pools refused every prior row; each `T4_prior` response row matches its counter (arcs refloated, code biases reset); 0 unwired flags (neither golden folds a wet delay or a receiver clock in the block) |
| G3 the brake (BSHM00ISR) | `rpc_c0off_f2`, `rpc_c0rpa_f2`, `rpc_c0on_f2` | G07's emitted clock record (`C,G07` of `products.txt`), `E` = the largest absolute change from its 259380 value over TOW 259410 to 259650 | (a) the event reproduces: `E(c0rpa)` at least 5 m, else G3 reads NOT APPLICABLE with the numbers and the brake is unproven; (b) `E(c0on)` at most `E(c0off)` + 1.0 m and at most half of `E(c0rpa)`; (c) at least one `T4_prior` `prior_arc` refloat of BSHM00ISR G07 between 259410 and 259650 in `rpc_c0on_f2` |
| G4 field, 6 h | `rpc_on_f6` against `rpc_off_f6` | WTZZ00DEU 3D p50 by hour (`logs/closed_loop_trace.csv`, hour 1 from the first row, `scripts/clock_level/p50.py`); the 16-site census (`gcensus.py`, each site's mean hourly p50 over hours 2 to 6, paired); the clock wire (`scripts/ftc_step_attrib.py --r3 --systems G,E`, `e_clk` de-commoned p50 over hours 2 to 6, all and tracked); epochs emitted and withheld | WTZZ00DEU mean change over hours 2 to 6 at most +1.0 mm and no hour of 2 to 6 worse by more than 10 mm; census paired median change at most +1.0 mm and no site worse by more than 10 mm; clock wire not worse by more than 10 % (card RPA's clause); as many epochs emitted as the control and none withheld that the control emits |
| G5 sim, 6 h | `rpc_on_s6` against `rpc_off_s6` | SZZ000SIM 3D p50 by hour from the run start (00:30:15); withholds | mean change over hours 2 to 6 at most +1.0 mm, no hour worse by more than 10 mm, no added withhold; a byte-identical sim arm records G5 as not applicable |

Reported, no bar:

- **D1** refloat counts by tier on every arm: `T4_attrib` arcs, `T4_prior` arcs, receiver code-bias resets, unwired
  flags, `T3_raim`, the within-epoch triggers, and the run-end ambiguity response rows (`amb_raim_drop`,
  `amb_drop_latch_slip`, `amb_gross_erase`), by half hour where the events allow.
- **D2** what the channel adds over the pool restriction alone: `rpc_on_f2` against `rpc_rpa_f2`.
- **D3** the BSHM00ISR read on `rpc_c0on_f2` (which rows flag between 259410 and 259650, their subjects, their coefficient
  check) and on the golden arms (`rpc_off_f2`, `rpc_on_f2`) where the cold bound acts first; and card RPA's other ready
  event, KIT300UZB TOW 263670 to 263730.
- **D4** hour 1 of every consumer, and the coefficient check's run total on the field arms (reading R-b).

**Verdict.** PASS: G0 to G5 hold (G3 and G5 where they apply); the key stays UNVALIDATED in the ledger until the author
decides. FAIL: G1, G2, G3, G4 or G5 misses; the failed clause is reported and the ledger verdict proposed is KEEP_OFF.
Arming is the author's decision, through a ledger row and the golden generator.

<!-- Results are appended below this line under dated headings. -->

## 7. 2026-10-30: results

### 7.1 Executables and runs

| role | executable | sha256[:16] | source |
| --- | --- | --- | --- |
| base | `gnss_processor_rpc0.exe` | `95319872ea22e966` | Release build of main `5127429` |
| changed | `gnss_processor_rpc1.exe` | `266ec04b510b6f01` | Release build of `b3c6b1f` (this branch) |

Every arm of section 5 exits 0 and emits all its epochs with none withheld (2 h field: 240 of 242 synced; 2 h sim:
241). The box was shared with other sessions' arms throughout; no wall-clock figure is quoted. Every number below was
read from the run directories (`products.txt`, `logs/network_events.csv`, `logs/network_raim_attrib.csv`,
`logs/closed_loop_trace.csv`, `stdout.txt`).

### 7.2 Gates G0 to G3

| gate | result | verdict |
| --- | --- | --- |
| G0 unit | Debug `gnss_tests`: 3 665 tests in 498 suites, 3 632 passed, 33 skipped, none failed; every skip a fixture absent from this worktree (SIF 25, GPT3 5, BRDC, IONEX and SP3 one each). The new suite `RaimPriorChannel` (13 cases) passes | PASS |
| G1 key off | `nom_td_gate.py --whole`, base against changed exe: field 2 h IDENTICAL (`products.txt` in full, every CSV, the 12 manifest records, the whole `stdout.txt` and main log, `rt_engine.log`, the exit code); sim 2 h IDENTICAL | PASS |
| G2 mechanism | `rpc_on_f2`: the channel ran on 234 passes, the 234 post-fit walks; 13 513 station-epochs with carried-prior rows; rows tested 379 082 (`block_ambiguity_carry` 342 739, `block_rx_cb_carry` 36 343), none at the redundancy floor. Both pools admitted `data_phase` only (345 032 visits) and refused `block_ambiguity_carry` 4 276 000 and `block_rx_cb_carry` 635 330 visits, with 0 own-column disagreements. Flags 29: 15 line-of-sight refloats (15 `prior_arc refloat` rows, 30 ambiguities reached), 12 on arcs the observation channel refloated in the same pass (12 `obs_refloat` rows), 2 code-bias resets (2 `prior_rxcb reset` rows); 0 held, 0 unwired | PASS |
| G3 the brake | G07's emitted clock, `E` from its 259380 value (+0.535 m) over 259410 to 259650: `rpc_c0rpa_f2` 11.886 m (+0.120, -1.208, -2.792, -4.488, -6.225, -7.940, -9.660, -11.351 m at 259620): the event reproduces; `rpc_c0off_f2` 0.441 m; `rpc_c0on_f2` 0.440 m, at most 1.441 m and at most 5.943 m; `T4_prior prior_arc` refloats of BSHM00ISR G07 at 259410, 259470, 259530, 259590, 259620 and 259770 (flag statistics 32.2, 58.2, 62.6, 74.0, 6.8, 19.2) | PASS |

The 0.44 m of every arm, the golden's included (0.429 m), is the 259410 epoch's own move (+0.535 to +0.120 m), which
precedes every response (card ICF section 7 measured 0.444 m).

### 7.3 Readings at 2 h (D1 to D4)

**D1, refloats by tier** (2 h field; by half hour in brackets):

| arm | `T4_attrib` arcs | `T4_prior` refloats | on arcs `T4_attrib` took | code-bias resets | `amb_raim_drop` | `amb_drop_latch_slip` | `amb_gross_erase` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `rpc_off_f2` (golden) | 30 (12 14 4) | | | | 60 | 196 | 8 |
| `rpc_rpa_f2` (pools only) | 18 (0 11 7) | | | | 36 | 200 | 8 |
| `rpc_on_f2` (the key) | 18 (0 11 7) | 15 (7 6 2) | 12 (0 10 2) | 2 | 66 | 200 | 8 |
| `rpc_c0off_f2` | 40 (22 14 4) | | | | 77 | 196 | 0 |
| `rpc_c0rpa_f2` | 18 (0 11 7) | | | | 36 | 200 | 0 |
| `rpc_c0on_f2` | 18 (0 11 7) | 24 (16 6 2) | 12 (0 10 2) | 5 | 79 | 200 | 0 |

No arm has a `T3_raim` or a within-epoch (`T2_within_epoch`) row. On the sim (`rpc_off_s2`, `rpc_on_s2`) no arm has a T4
row of either channel (`amb_raim_drop` 0, `amb_drop_latch_slip` 2 in both); the key-on sim arm tested 413 642
carried-prior rows (largest statistic 2.17) and flagged none.

**D2, what the channel adds to the pool restriction.** `rpc_rpa_f2` and `rpc_on_f2` take the same 18 observation-channel
refloats. The channel adds 15 refloats and 2 code-bias resets: BSHM00ISR G07, G09, G14 and its GPS code bias at 259410;
MAC100AUS G31 and its Galileo code bias at 259800; LCK400IND G04 at 259830 (the own row at 9.45 with no other carried
column on it, the refloat card NOM section 16 found driven by a prior row: it is that carry's own); MAJU00MHL G08 at
260040; YKRO00CIV E10 at 260490 and G05 at 262380; HKSL00HKG E13, E19 and E26 between 262590 and 263040, beside the
observation channel's refloats of the station's persistent E04 misfit.

**D3, the two events.**

| arm | BSHM00ISR, 259410 to 259650 |
| --- | --- |
| golden (`rpc_off_f2`) | the cold bound declares G07 at 259410 (24.8 m against 12 m) and refloats it at 259470, 259530 and 259590; T4 refloats E04, E14, E19, G05 and G07 at 259410, five cells with one interaction (10.7533): one prior row in five pools |
| `rpc_c0off_f2` (prior rows in the pools) | 15 T4 refloats: E04, E14, E19, G05, G07 at 259410; E04, G05, G07 at 259470; E04, E19, G05, G07 at 259530; E04, G05, G07 at 259590 |
| `rpc_c0rpa_f2` (observation rows only) | none; G07's clock runs away to -11.35 m |
| `rpc_c0on_f2` (the channel) | G07's own row at 259410, 259470, 259530, 259590, 259620 and 259770; G09 at 259410 and 259470; G14 at 259410 to 259590; the GPS code bias at 259410 to 259590 |

At 259410 the channel's cells at BSHM00ISR read G07 32.21, G14 21.44, G09 14.84 and the GPS code bias 14.08 (flagged),
the Galileo code bias -6.13 and E19 -6.13 (not): the cells the spread rule could not separate. KIT300UZB, 263670 to
263730: the golden refloats G21 at 263700 only; with observation rows in the pools (`rpc_rpa_f2`, `rpc_on_f2`) the
observation channel refloats E21, E26, G02, G08, G19 and G21 at 263670, and the channel flags G21's own carry row
(statistic 183.6) on the arc already refloated.

**D4.** Held-out consumer at 2 h (WTZZ00DEU 3D p50, mm): golden 58.80 12.30, the key 58.80 11.95, pools only 58.70
11.90; event posture: `c0off` 51.05 12.10, `c0on` 50.35 11.95, `c0rpa` 53.60 14.75 (the runaway costs +2.65 mm in hour
2). Sim (SZZ000SIM from 00:30:15): 99.45 32.60 in both arms, `products.txt` and the closed-loop trace byte-identical;
only `sigma0_global` and `prior_tension_ratio` of the epoch status move (the constraint-aware sigma0's data side no
longer counts the prior rows). Coefficient check: 60.3 % of the tested rows on the field and 60.2 % on the sim have their
largest coefficient on another carried column than their own. The check counts a row's other band of the same line of
sight (the two ambiguities of one satellite are strongly correlated through its slant ionosphere), which stays in the
row's own cell; at BSHM00ISR the flagged G07 row's own coefficient (2.778) and its largest other (2.780) are equal to the
third digit. Reading R-b is not supported on this event: the fault's own carry flags first and the brake holds.
