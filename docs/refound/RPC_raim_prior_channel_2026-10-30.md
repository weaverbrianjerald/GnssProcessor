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
