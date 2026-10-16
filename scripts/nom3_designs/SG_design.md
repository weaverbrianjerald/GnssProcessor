# NOM3 step SG: the buildable design (the solve as a linear orchestrator)

Read-only design for card NOM3 step SG (`docs/refound/NOM3_network_giants_2026-10-09.md`, section 3 "SG,
the solve", findings 2 and 3 of section 2, the frozen gates of section 5, the order of section 6, and
the EP1 pre-registration at the end of the card). No repository file was changed, nothing was built
or run. Precedent method: card NOM2 sections 6 to 8 (step TD: groups first, a generated
character-exact cut, a relocation verifier, a declared table).

Revision of 2026-10-14: the review's ten defects were each checked against the code and all ten are
confirmed; section 9 lists each with its evidence and the place it is fixed.

## 0. Bases, notation, and the design in one page

**Three trees.** Every claim below is made against one of them.

| name | what it is | how it was obtained |
| --- | --- | --- |
| `main` | main `31c393f` (EP0, WCF, HV0 landed) | `git show 31c393f:<path>` |
| `post` | main plus the REMOVE-key deletion as it stands on its track branches | `scratchpad/nextsteps/sg/post.cpp` and `post.hpp`: `main`'s `NetworkSolveGlue.{cpp,hpp}` with the track tips merged by `git merge-file` in the order I, K, R1, R2 (`rmk/L` touches neither file; every track has merge base `31c393f`; tips I `6923a8e`, K `63ef17d`, R1 `45e93a7`, R2 `7c462c7`). R1 and R2 merge cleanly. **I and K conflict at the epilogue**: I deletes the SH-field gate (`SG.cpp:2742-2845`) while K rewrites `SG.cpp:2846` ("18" to "17" crossing locals) and deletes 2852 (`emit_rate_warm`); applying the two diffs in sequence fails in both orders. `post.cpp` is the merge with that one conflict resolved to I's deletion plus K's two edits (`sg2/post_resolved.cpp`, built from the `merge-file` output, is byte-equal to `sg/post.cpp`) |
| `B_SG` | the base SG is cut from: the deletion merged to main, then EP1a-1, EP1b and EP1c | does not exist yet; described from `post` and the EP1 specification (`scratchpad/ep1design/EP1_spec.md` sections 2.4, 3.4, Appendix A row 52, and the EP1c paragraph of section 7) |

Citations: `SG.cpp:N` is `src/core/filter/NetworkSolveGlue.cpp` line N at `main`; `post:N` is line N of
the reconstruction; `SG.hpp:N` is the header at `main`. Other files are cited at `main` unless a
branch is named. Measurement scripts (read-only): in `scratchpad/nextsteps/sg/`, `measure_sg.py`
(sections by anchor text, counts), `writes_sg.py` (run-state writes per section), `pieces.py`,
`branch_reach_sg.py` (branch markers in existing arm outputs), `sbasis_members.py`; in
`scratchpad/nextsteps/sg2/`, `check_sg1.py` (the revised cut of section 3.2: coverage of every code
line, a brace scan per new function over its pieces in order, and a name check that every solve-scope
name a body reads is bindable in its new function), `linemap.py` (post to main line map),
`jumps.py` (jump statements whose target lies outside a range). Their outputs are beside them or
quoted where used. `check_sg1.py` on `post.cpp` (`sg2/check_sg1_out.txt`): 0 uncovered and 0 doubly
covered code lines; all 24 bodies brace-balanced over their pieces; every solve-scope name bound in 23
of 24 bodies, and in the T2 stage every name except the two that declared classes rewrite
(`buildCarryDdPriors`, EP1's direct call; `result`, RESULT_FIELD).

**The design in one page.**

1. `networkSolveGlue` keeps EP1's signature after EP1c, `(NetworkRun& run, const NetworkEpoch& ep,
   EpochWorkspace& ws, std::vector<int>& est_index_of, EpochDdBuild& dd_build)`, and its body becomes
   a linear list of 14 stage calls, four exits through one table and an epilogue (about 75 lines with
   the frozen comments). Each stage is a function in EP1's seam form, `R networkX(NetworkRun& run,
   const NetworkEpoch& ep[, kept...][, outputs...])`, opening with a generated preamble that binds
   the names its body reads; the body below is the frozen text.
2. The stages hand each other four typed values held by the orchestrator: `SolveDatum` (the plan, the
   pivot-handover verdict), the solution `x_s`, `WithinEpochOutcome` (T2's result, including the
   sigma0 pools the ceiling reads) and `Sigma0CeilingDecision` (the carve-out); plus EP1's
   `NetworkPostFitHandoff`, seeded here. The epilogue moves them into the unchanged
   `NetworkSolveGlueResult`, whose post-solve readers EP1 bound field by field.
3. Inside the solve, every object the solve writes (`ws`, `est_index_of`, `dd_build`) reaches a stage
   only as a parameter, never through its `ep` member: one access path per object (the card WCF
   lesson), checked by S11.
4. The five exits are one declared table, `kSolveSkips` (disposition, status columns, trace row, H
   line, tail), and one function, `networkSolveSkip`, called from the orchestrator. Three rows write no
   H line and no trace row (finding 3): kept, and listed for the author.
5. New homes: `filter/NetworkCodeBiasDatum.cpp`, `filter/NetworkClockDatum.cpp`,
   `filter/NetworkRankGate.cpp` (with `networkRankGateRunEnd`), `filter/NetworkRaimWithinEpoch.cpp`
   (T2, the sigma0 ceiling, `networkWithinEpochRaimRunEnd`), and, a deviation, `filter/
   NetworkSbasisCompletion.cpp` (with `networkSbasisCompletionRunEnd`). Small stages join existing
   files beside their concept (`NetworkPrefitCensus`, `NetworkBiasAdmission`, `NetworkClockAnchor`,
   `NetworkDatumGraph`); the fold-order stages and the primary solve stay in `NetworkSolveGlue.cpp`.
6. Three sub-steps, each identical output: SG1 the stage cut with the exit table; SG2 T2's internals
   (its lambdas become functions, the culprit choice a pure function with unit tests); SG3 the RHS
   trip's explicit form retired and the printed code-site citations moved (a declared text move).
7. Tooling reuses EP1's reading layer, binding model and control harness (branch `nom/ep1-tools`,
   tip `f800032`); a new generator (`scripts/nom_sg_cut.py`) and verifier (`scripts/nom_sg_verify.py`)
   add the stage table with explicit block forms, the crossing-local realization table and the
   exit-table equivalence check.

---

## 1. The measured state

### 1.1 The function at `main`, after the deletion, and after EP1

| quantity | `main` (`31c393f`) | `post` (after the deletion) | after EP1 (`B_SG`, derived) | source |
| --- | --- | --- | --- | --- |
| span | `SG.cpp:100-2866`, 2 767 lines | `post:93-2612`, 2 520 lines (247 fewer) | about 2 520 lines | `measure_out.txt` |
| code lines (not blank, not comment) | 1 691 | 1 514 (177 fewer) | about 1 514 | same |
| parameters | 25 (`SG.hpp:120-145`) | 24 (`buildEnsembleGauges` gone, K `dcbc6cf`) | 5 after EP1c: `run, ep, ws, est_index_of, dd_build` (EP1b leaves 6 with `funnel`; EP1c makes the DD build's funnel const and removes the solve's `funnel` output) | EP1 spec 3.4, section 7 |
| alias preamble | 85 declarations (`SG.cpp:126-204`, `262-270`; the inventory's 83 plus EP0's two knob lines `SG.cpp:129-130`), 16 unused | 77, 15 unused | EP1 inserts 21 generated binding lines above it (Appendix A row 52 minus the outputs and the callbacks) | `measure_out.txt` |
| unused aliases | `emitter, consumer, q_clock, prev_carry, this_epoch_anchor, prev_clk, clock_osc, relevel_iode, eph_cutover_disc_pending, local_carry, zwd_carry, clock_sum_to_zero, n_perconst_diffuse_published, phase_datum_ensemble, phase_datum_eq34, t` | the same minus `phase_datum_ensemble` | unchanged | same |
| exits (`result.action = EpochAction::SkipEpoch`) | 5: `SG.cpp:762, 1327, 1461, 2506, 2736` | 5: `post:677, 1240, 1369, 2357, 2587` | 5 | same |
| banners saying "four" exits | `SG.cpp:6`, `SG.hpp:13` | unchanged | unchanged | finding 3 |
| log call sites / event call sites / catch clauses | 23 / 21 (plus the lambda's own forwarding call) / 15 | 19 / 19 / 13 | unchanged | same |
| lambdas | 9: `emitEpochStatus` 217, `emitEvent` 255, `freqOf` 307, `handover_affects` 794 (dead), `push` 1021, `ci_classify` 1605, `assess` 1662, `pool_s0` 1783, `resolve_dropping` 1819 | the same 9 (`post:201, 239, 289, 709, 934, 1513, 1570, 1691, 1727`) | unchanged | same |
| `ctx.counters` fields named / `ctx.carry` / `ctx.knobs` | 76 / 54 / 45 | 73 / 43 / 40 | unchanged | same |
| maximum brace depth | 9 (`SG.cpp:441`) | 9 (`post:379`) | 9 | same |
| body at column 0 | `SG.cpp:275-2740` | `post:259-2591` | unchanged | read |

The 20 catch blocks of the inventory counted comment mentions; the clauses are 15 at `main`.

### 1.2 Section by section, tagged

Tags: **kept** (survives the deletion and EP1 unchanged), **rmk** (the deletion removes it: track
commit and the deletion plan's commit number `Cn`, `scratchpad/rmk/PLAN.md` section 2), **EP1** (EP1
rewrites it). Ranges are `main` / `post`.

| # | `main` | `post` | what | tag | stage after SG |
| --- | --- | --- | --- | --- | --- |
| S00 | 60-98 | 59-91 | `networkClockDatumDeclaration` (also read by the handover preview, `NetworkHandoverStransform.cpp:11, 310`) | kept, except the ensemble alias 64 and branch 71-75 (the `else if` at 79 becomes `if`): **rmk** K `dcbc6cf` (C11) | moved whole to `NetworkClockDatum.cpp` |
| S01 | 100-125 | 93-117 | signature | `buildEnsembleGauges` 124: **rmk** K `dcbc6cf`; the rest **EP1** (5 parameters after EP1c) | the orchestrator's signature (EP1's, unchanged) |
| S02 | 126-204 | 118-188 | alias preamble | kept, except: `held_clock_ref` 145, `this_epoch_gf_pin_real` 148, `family_census` 149, `basis_relatch_disc_pending` 155, `clock_signal_basis_hold` 181 (**rmk** R1 `d5d2c7a`, C20); `carry_cpl_joint` 167 (R1 `74b038e`, C23); `emit_clock_rate_on` 188 (K `49fbd1c`, C9); `phase_datum_ensemble` 201 (K `dcbc6cf`, C11) | dissolved: each stage's preamble repeats the lines its body reads (the TD1b pattern); the 15 unused lines go |
| S03 | 205-216 | 189-200 | census-fed H sigma0, before any H write | kept | `networkCensusSigma0` (`NetworkPrefitCensus.cpp`) |
| S04 | 217-261 | 201-245 | `emitEpochStatus` and `emitEvent` lambdas | kept | `emitEpochStatus`'s body becomes `networkSolveEpochStatusRow` (called only by `networkSolveSkip`); `emitEvent`'s text is repeated in the preambles of the four stages that call it |
| S05 | 262-274 | 246-258 | `cap_*` aliases, `NetworkSolveGlueResult result;` | kept | aliases into the preambles that read them; `result` declared at the epilogue |
| S06 | 275-304 | 259-286 | unconditional per-epoch clears of the datum scratch | kept, except 297-298 (GF and family-GF pin clears; R1 `d5d2c7a`) and comment edits 277-280 (R1 `d5d2c7a`, `45e93a7`) | `networkCodeBiasDatum`, before its guard |
| S07a | 305-315 | 287-297 | W6 guard (opened at `post:287`, closed at `post:634`), `freqOf` | kept | `networkCodeBiasDatum` |
| S07b | 316-358 | none | family-GF census, `gf_family_constellations` | **rmk** R1 `d5d2c7a` (C20) | none |
| S07c | 360-399 | 298-337 | ISC bridging roster | kept | `networkIscBridgedRoster` (helper, `NetworkCodeBiasDatum.cpp`) |
| S07d | 400-448 | 338-386 | extra-band roster | kept (comment 410-411 edited, R1) | `networkExtraBandRoster` (helper) |
| S07e | 449-502 | 387-434 | `buildCodeBiasDatumRows` call, carry hand-offs, roster pin | kept, except 9 arguments (`clock_signal_basis_hold`, GF datum, `sat_if_level_datum`, `gf_family_constellations`, `held_clock_ref`, the two GF pin sets, `basis_relatch_disc_pending`, `emitEvent`) at 458-477 (R1 `d5d2c7a`) | `networkCodeBiasDatum` |
| S07f | 503-609 | 435-539 | fold (block `if (cbd.H.rows() > 0) {` opened at `post:435`, closed at 539), telemetry, ISC verdicts, replay capture | kept, except `n_if_level_rows/sats` 536-537 (R1 `d5d2c7a`) | fold and capture in `networkCodeBiasDatum`; telemetry and verdicts (`post:440-532`) to `networkCodeBiasDatumRecord` (helper) |
| S07g | 610-637 | 540-567 | card GFC first-sighting family prior fold and capture | kept | `networkCodeBiasDatum` |
| S07h | 638-669 | 568-599 | card KP prior fold and capture | kept | `networkFoldCodeBiasKaulaPrior` (`NetworkBiasAdmission.cpp`, beside `networkBiasAdmissionRunEnd`) |
| S07i | 670-719 | 600-634 | W6 first-N log (600-633), the guard's closing brace (634) | kept, except the GF and family-GF clauses 684-698 and the format's `{}{}{}` (R1 `d5d2c7a`) | the log to `networkCodeBiasDatumFirstNLog` (helper); the brace closes the guard in `networkCodeBiasDatum` |
| S08 | 721-765 | 636-680 | `NetworkSbasis sb;`, the declaration, HLT ineligible set, `buildSharedDatum`; **exit DatumFailed** | kept | `networkClockDatum` (`NetworkClockDatum.cpp`) |
| S09 | 766-779 | 681-694 | clock constraint rows fold | kept | `networkClockDatum` |
| S10 | 780-798 | 695-713 | pivot-handover verdict; `handover_affects` (dead, 789-797 / 704-712) | kept | `networkClockDatum`; the dead lambda dropped |
| S11 | 799-820 | 714-733 | SH-VTEC and pin-bridge priors, `last_datum_changed` | phase-hold prior 803 and comment 805-806: **rmk** K `dcbc6cf` (C11) | `networkPreGatePriors` (`NetworkSolveGlue.cpp`) |
| S11c | 821-846 | 734-759 | comments: the drop-and-heal pointer, the rank-gate tolerance rationale | kept | the file banner of `NetworkRankGate.cpp` |
| S12 | 847-886 | 760-799 | estimability spectrum dump (budgeted) | kept | `networkEstimabilityDump` (`NetworkRankGate.cpp`) |
| S13 | 887-926 | 800-839 | `bool gate_ok = true;` and the DG6 comment; KP K2 margin one-shot | kept | `gate_ok` into `networkRankGate`; the margin to `networkKaulaMargin` (`NetworkBiasAdmission.cpp`) |
| S14 | 927-1123 | 840-1036 | S-basis completion with the DG spectral hand-off | kept | `networkSbasisCompletion` (`NetworkSbasisCompletion.cpp`), helpers `networkSbcContinuityTargets` (`post:854-893`) and `networkDatumGraphSpectralHandoff` (`post:906-946`, to `NetworkDatumGraph.cpp`) |
| S15 | 1124-1253 | 1037-1166 | rank gate, heal, re-gate, withhold accounting | kept | `networkRankGate` (`NetworkRankGate.cpp`) |
| S16 | 1254-1292 | 1167-1205 | envelope, `networkDatumInfoRatio`, DG fallback | kept | `networkRankGate` |
| S17 | 1293-1330 | 1206-1243 | **exit GateWithheld**: trace row flag 0, status row, H line | kept | `kSolveSkips` row and `networkSolveSkip` |
| S18 | 1331-1340 | 1244-1250 | `clk_model_warm`; dead aliases `clk_pred`, `clk_prior_info` | `emit_rate_warm` 1335-1337: **rmk** K `49fbd1c` (C9) | `networkClockModelWarm` (`NetworkSolveGlue.cpp`); the dead aliases dropped |
| S19 | 1341-1367 | 1251-1277 | eph-cutover relevel, handover S-transform, D2 prior, clock-oscillator prior (order load-bearing) | kept | `networkPostGatePriors` (`NetworkSolveGlue.cpp`) |
| S20 | 1368-1420 | 1278-1330 | cold a-priori clock anchor | kept | `networkFoldColdClockAnchor` (`NetworkClockAnchor.cpp`), called last in `networkPostGatePriors` |
| S21c | 1421-1434 | 1331-1344 | comment: why the pivot baseline advances only at emit | kept | the orchestrator, before the solve |
| S21 | 1435-1464 | 1345-1372 | `est.solve`, card-1d checksum; **exit SolveFailed** | comment 1442-1444: **rmk** R2 `c4c4021` (C25) | `networkPrimarySolve` (`NetworkSolveGlue.cpp`); its `Eigen::VectorXd x_s;` line to the orchestrator |
| S22 | 1465-1482 | 1373-1390 | hand-off seeds: `clock_refloat_sats`, `n_arc_refloat_epoch`, `retro_flag_sats`, `retro_feed` | kept (the epilogue's three hand-off lines are **EP1** FIELD_MOVE) | the orchestrator's `NetworkPostFitHandoff handoff;`; `retro_feed` becomes `NetworkKnobs::retro_feed` |
| S23 | 1483-1497 | 1391-1405 | Q3 guarded temporal clock anchor | kept | `networkClockAnchorGuardEpoch` (`NetworkClockAnchor.cpp`) |
| S24 | 1498-1548 | 1406-1456 | T2 per-epoch state, era liveness, per-epoch resets | kept | `networkWithinEpochAdapt` (`NetworkRaimWithinEpoch.cpp`); the crossing locals realized in `WithinEpochOutcome` |
| S25a | 1549-1621 | 1457-1529 | T2 entry, budget escalation, WE-CI column mask, `ci_classify` | kept | `networkWithinEpochAdapt` |
| S25b | 1622-1644 | 1530-1552 | `orig_added`, the culprit sets as references into `ws.we_culprits` (card WCF) | kept | `networkWithinEpochAdapt` |
| S25c | 1645-1794 | 1553-1702 | `assess` | kept | `networkWithinEpochAdapt` (SG1); `weAssess` (SG2) |
| S25d | 1795-2146 | 1703-1997 | `resolve_dropping`, the replay | `replay_row_tags` 1824-1827, the joint branch 1889-1891, 1900-1902, 1917-1958, the closure arguments 2105-2106, 2121-2122: **rmk** R1 `74b038e` (C23); `buildEnsembleGauges()` 2127-2131: **rmk** K `dcbc6cf` (C11); `buildCarryDdPriors();` 2141 / `post:1992`: **EP1** CALLBACK_DIRECT, becomes `networkBuildCarryDdPriors(run, ep, dd_build);` (EP1c's form; `..., dd_build, funnel);` between EP1b and EP1c) | `networkWithinEpochAdapt` (SG1); `weReplay`, `weReaddStation`, `weRefoldShared` (SG2) |
| S25e | 2147-2174 | 1998-2025 | first assess, primary clocks, T2 flag rate, WE-CI trips | kept | `networkWithinEpochAdapt` |
| S25f | 2175-2378 | 2026-2229 | the adaptation loop with its undo | kept | `networkWithinEpochAdapt` (SG1); `weChooseCulprit`, `weUndo` (SG2) |
| S25g | 2379-2480 | 2230-2331 | settle: PRM count, clock meter, counters, log, events, couple and retro feed, `result.we_landed`, `sigma0_adapt` | kept | `networkWithinEpochAdapt` (SG1); `weSettle` (SG2) |
| S26 | 2481-2486 | 2332-2337 | `ws.we_culprits.clear()` | kept | tail of `networkWithinEpochAdapt` |
| S27 | 2487-2509 | 2338-2360 | **exit WeRestoreFailed**: error line, status row | kept | the error line stays in `networkWithinEpochAdapt`; the tail is the `kSolveSkips` row |
| S28 | 2510-2520 | 2361-2371 | reduce-once invariant check | kept | tail of `networkWithinEpochAdapt` |
| S29 | 2521-2597 | 2372-2448 | A4 per-constellation carve-out decision | kept | `networkSigma0Ceiling` (`NetworkRaimWithinEpoch.cpp`) |
| S30 | 2598-2605 | 2449-2456 | the recorder's "sigma0" edge | kept | `networkSigma0Ceiling` |
| S31 | 2606-2652 | 2457-2503 | withhold-era bookkeeping, both edges | kept | `networkSigma0Ceiling` |
| S32 | 2653-2738 | 2504-2589 | **exit Sigma0Withheld**: warning, event, counters, trace flag 1, status row with the T2 meters, H line, RHS trip, free erase | kept | warning, event and `++n_sigma0_withheld` stay in `networkSigma0Ceiling`; the tail is the `kSolveSkips` row; trip and erase are `networkSigma0WithholdTail` |
| S32b | 2739-2742 | 2590-2592 | PRM: the landed replay's withheld priors on a kept epoch | kept | tail of `networkSigma0Ceiling` |
| S33 | 2743-2845 | none | SH-field health gate verdict | **rmk** I `620c2b6` (C14) | none |
| S34 | 2846-2866 | 2593-2612 | epilogue (18 fields at `main`; 17 after K `49fbd1c` removes `emit_rate_warm` and rewrites the comment at 2846, the line the I/K merge conflicts on) | three lines **EP1** FIELD_MOVE (`result.handoff.*`) | the orchestrator's epilogue |

The deletion also removes, outside the function: the include `SphericalHarmonicIonosphere.hpp`
(`SG.cpp:16`, I `620c2b6`) and, in the header, `emit_rate_warm` (`SG.hpp:74`) and the
`buildEnsembleGauges` parameter and banner text (`SG.hpp:26-28, 115, 144`; K). The R1 WIP commit
`45e93a7` and the I WIP commit `6923a8e` are ungated; in this file the R1 WIP edits are comment-only
(two hunks, `post:260-263` and `post:349-352`).

### 1.3 Writes to run state, by section (`post`, from `writes_sg.py` and reading)

| section | `ctx.carry` | `ctx.counters` | other |
| --- | --- | --- | --- |
| S03 | `ep_h_sigma0_census`, `h_sig0_census_nonzero` | | |
| S06 | `this_epoch_if_pin_sel`, `_if_pin_real`, `_if_pin_holds`, `_if_pin_changes`, `_cb_family_rows/births/birth_skips`, `_cb_ifpin`, `_cb_reseed`, `_cb_reseed_channels`, `_extra_band_held` (clears) | | |
| S07e | the six `this_epoch_*` hand-offs at `post:420-427`, and the builder's carry maps (`code_bias_prev_`, `code_bias_ifpin_pair_`, `code_bias_sat_seen_`, `code_bias_datum_resumed_`) | | |
| S07f | `this_epoch_extra_band_held`, `this_epoch_isc_obs`, `this_epoch_isc_pinned` | 21 telemetry fields (`n_w6_iono_none_*`, `n_sat_gf_*`, `n_isc_*`, `n_extra_band_*`, `n_s2z_reseed_*`, `sat_gf_by_const`), `prm_resp` | est rows; `ws.cap_H/z/row_kinds/constraint_weight/has_constraint` |
| S07g | | | est rows; `ws.cap_birth_*` |
| S07h | | `n_kp_epochs`, `n_kp_cols` | est prior; `ws.cap_priors/cap_prior_kinds` |
| S08 | | `hlt_unhealthy_registered`, `hlt_pivot_skips`, `hlt_pivot_fallbacks`; exit: `n_skipped`, `prev_week`, `prev_tow` | |
| S09 | | | est rows |
| S11 | `last_datum_changed` | | est priors; `ws.cap_priors` |
| S12 | | `spectrum_diag_budget` | traces |
| S13 | | `kp_margin_logged` | log |
| S14 | `dg_spec`, `sbc_active_sats` | `n_sbc_*` (6), `sbc_max_dirs` | est rows; `ws.cap_sbc_H/z`; events |
| S15-S16 | `dg_spec` | `n_gate_evaluated`, `n_gate_withheld` (+ `_over`, `_under`), `n_heal_attempt_*`, `n_healed_*`, `nullspace_obs_min/max` | est heal priors; `ws.cap_priors`; events; traces |
| S17 | | `n_skipped`, `prev_week`, `prev_tow` | trace row, status row, H line or `setup.pending_h_pre_open` |
| S19-S20 | through the seams: carry re-bases, `clock_osc` etc. | | est priors; `ws.cap_*` |
| S21 | | `n_cpl_solve_dump`; exit: `n_skipped`, `prev_*` | `x_s` |
| S23 | through `networkClockAnchorGuard` | | `x_s`, `ws.cap_*` |
| S24 | `ep_sigma0_first`, `ep_we_refloat_sats`, `ep_we_clk_move_max`, `ep_we_clk_move_sat` (resets) | | |
| S25c | | `raim_pool_within` | |
| S25d | | `n_pl_blk_refloat_rewhiten/_fail`, `we_culprit_withhold.resolves` | est rebuilt; `est_index_of`; `dd_build` through the DD rebuild |
| S25e-g | `ep_sigma0_first`, `ep_we_*` | `we_ci_*` (6), `we_resolve_failed` (+ `_tow`, `_at`), `we_undo_sta_dropped`, `we_restore_failed`, `we_refloat_sat_total`, `we_refloat_arc_total`, `we_exclude_sta_total`, `we_clk_move_max_*` (3) | `ws.we_culprits`; `x_s`; the hand-off sets; events |
| S27 | | `n_skipped`, `prev_*` | status row |
| S31 | `wh_era_len`, `wh_last_loop_ep` | `n_wh_*` (7) | events |
| S32 | `local_carry` (erase) | `n_skipped`, `n_sigma0_withheld`, `n_wh_free_erases`, `prm_resp`, `prev_*` | trace row, status row, H line, RHS dump |
| S32b | | `prm_resp` | |

### 1.4 How the function-scope locals cross sections (`post`)

| local | declared | read (sections) | written after declaration | realization after SG |
| --- | --- | --- | --- | --- |
| `sb` | 637 | S08, S14, S15 | no (const methods only) | one `NetworkSbasis sb;` per stage that calls it (FRESH_STATELESS, section 3.3) |
| `plan` | 642 | S09, S12, S14 (incl. the DG hand-off 929), S15-S17, S19, S21, S23, S25 (assess, replay), S32 (trace row, recorder dump 2563), S34 | S08 only | `SolveDatum::plan` |
| `changed_consts` | 701 | S19, S23, S34 | S10 only | `SolveDatum::changed_consts` |
| `datum_changed` | 702 | S11, S14, S17, S19, S21, S23, S27, S32, S34 | no (const) | `SolveDatum::datum_changed` |
| `gate_ok` | 800 | S15, S17 (the exit's test) | S15 | a local of `networkRankGate`, mapped by its generated tail to the returned `EpochAction` |
| `gate_observed`, `gate_first_observed`, `gate_ev_max` | 1037-1039 | S15-S16 only | S15 | locals of `networkRankGate` (they cross no stage) |
| `clk_model_warm` | 1247 | S19, S34 | no | the orchestrator's `const bool clk_model_warm` |
| `x_s` | 1345 | S23, S25, S34 | S21, S23, S25 | the orchestrator's `Eigen::VectorXd x_s` (the frozen line, moved) |
| `clock_refloat_sats` | 1380 | S23 (the guard, 1403) | S25g (2309) | `NetworkPostFitHandoff::clock_refloat_sats` (EP1's type) |
| `retro_flag_sats` | 1388 | none in the solve | S25g (2314) | `NetworkPostFitHandoff::retro_flag_sats` |
| `n_arc_refloat_epoch` | 1381 | none in the solve | none (the epilogue moves it, 2602; the post-fit tiers write it through the hand-off) | `NetworkPostFitHandoff::n_arc_refloat_epoch` |
| `retro_feed` | 1389 | S25g (2313), S34 | no | `NetworkKnobs::retro_feed` |
| `n_adapt`, `sigma0_adapt`, `prm_we_free_landed`, `wh_era_live`, `wh_escalated`, `we_sigma0_pool_c`, `we_t2_opp`, `we_t2_flag` | 1415-1448 | S25, S27, S29-S32b, S34 | S25 | `WithinEpochOutcome` members |
| `we_adapt_attempted`, `we_restore_failed`, `we_restore_what` | 1416, 1422-1423 | S25, S27, S28 | S25 | stay locals of `networkWithinEpochAdapt` (every use is inside it) |
| `result.we_landed` | (field) | S34 | S25g `post:2328` | `WithinEpochOutcome::landed` |
| `perconst_active`, `_publish`, `_dec_withhold`, `perconst_stats` | 2414-2417 | S31, S32, S34 | S29 | `Sigma0CeilingDecision` members |
| `a4_over_ceiling` | 2455 | S31, S32 | no | stays a local of `networkSigma0Ceiling` |

Realized locals: 18 (3 datum, 3 hand-off, 8 T2, 4 ceiling), plus `result.we_landed` (RESULT_FIELD),
the `x_s` declaration moved to the orchestrator, and `retro_feed` to a knob.

### 1.5 Findings re-examined on the post-deletion code

**Finding 1 (WCF).** Fixed on main (`ee18652`..`e63b498`): the culprit sets are references into
`ws.we_culprits` (`SG.cpp:1640-1643`), the replay's writer-end counter sits before the rebuild
(`SG.cpp:2139-2140`). SG must keep both the reference binding and the counter (negative controls C1
and C15).

**Finding 2 (the replay's fold order), narrowed by the deletion.** The primary system is folded in this
order: the stations and their carry priors (PASS B), the DD priors (the driver's `SB_DDBUILD`,
`ApplicationNetwork.cpp:2451-2468`), then in the solve the W6 rows (`post:439`), the GFC birth rows
(`post:560`), the KP prior (`post:581`), the clock rows (`post:691`), the SH and pin-bridge priors
(`post:714, 722`), the SBC rows (`post:986`), the heal priors (`post:1082`, gate-failure path), the D2
carry or clone fold (`post:1271`), the clock-oscillator prior (`post:1274`), the cold anchor
(`post:1324`). The replay folds: the stations (`post:1735-1919`), W6 (`post:1927-1930`), birth
(`1935`), clock rows (`1941-1945`), SBC (`1949-1953`), every captured prior in one call (`1960-1966`),
the dense D2 or clones (`1975-1979`), the shared history (`1982`), the DD priors last (`1990-1992`).
Under `ctx.knobs.carry_gauge_free` the replay folds every captured prior gauge-free (`post:1962-1963`)
while the primary folds only the D2 group gauge-free (`NetworkBorderPrior.cpp` at `rmk/R1`: 528, 677,
740) and every other captured prior plainly: the KP prior (`post:581`), the SH and pin-bridge priors
(`NetworkBorderPrior.cpp` at `rmk/R1`: 160, 239), the heal anchors, the oscillator prior (`rmk/R1`:
1039) and the cold anchor (`post:1324`).

What the deletion changes: R1 `74b038e` (C23) removes `network.shared_carry_gauge_free` and the closure
flag. The predicate survives through the Q3 path only: `k.carry_gauge_free = k.cta_on &&
k.cta_gauge_free` (`NetworkKnobs.cpp:293` at `rmk/R1`, from `310-312` at main), that is
`network.clock_anchor_gauge_free` with the Q3 anchor on. Both goldens set it false
(`golden_field_refound.json:326`, `golden_sim_refound.json:325`). So the information asymmetry is
reachable only on that arming; the order difference (DD priors last, KP after the clock and SBC rows)
remains on every replay and moves only last bits. SG names the order in the replay's Doxygen and
moves no fold; the fold tape that would equalize it is an output-moving card of its own (decision 2).

**Finding 3 (five exits, three silent).** Measured at `post`: DatumFailed (`post:671-679`) and
SolveFailed (`post:1361-1371`) write a status row only; WeRestoreFailed (`post:2346-2359`) writes a
status row with the T2 sigma0 columns but not the T2 meters that Sigma0Withheld carries
(`post:2515-2529`); none of the three writes a trace row or an H line. GateWithheld (`post:1206-1242`)
and Sigma0Withheld write all three. SG declares this in the table and changes nothing (decision 1).

**New, minor (1).** The header says the value fields of a SkipEpoch result are default
(`SG.hpp:64-66`); `result.we_landed` is written before exits 4 and 5 (`SG.cpp:2477`). No reader is
affected (the driver returns before reading it, `ApplicationNetwork.cpp:2477, 2536`). SG's
`networkSolveSkip` returns a fresh result, which makes the statement true.

**New, minor (2).** The KP margin's comment (`post:807-809`) says the margin is logged "on the same
reduced system the gate is about to judge (lambda_max here equals the gate's)". That holds only on an
epoch where SBC folds nothing (its rows are added at `post:986`, after the margin at 815) and no
clock-clone view is formed (the gate then judges the clone-marginalized view, `post:1046-1049`). The
margin is a one-shot on the first KP epoch, so the difference is a log line, not output. SG moves the
comment verbatim; decision 13 offers a comment-only correction in SG3.

### 1.6 Branch reach on the existing arms

`branch_reach_sg.py` over the 37 arm directories `runs/_rap_g2_ep0_*` (15), `runs/_rap_td1b_new_*`
(18) and `runs/_rap_wcfp_*` (4), `reach_out.txt`:

- none reaches any of the five exits, the heal, SBC engagement or refusal, the A4 carve-out, the
  withhold era, a re-solve failure or an undo; no main log on C: (1 412 run directories) contains the
  sigma0 withhold line;
- on the field golden the T2 sigma0 sits at its floor: `runs/_rap_g2_ep0_f2/logs/
  network_epoch_status.csv` has 240 epochs, 234 assessed, every one with `sigma0` = `sigma0_first` =
  1.0 (`network.raim_sigma0_floor` default 1.0, `GnssConfigLoadNetwork.cpp:7367-7368`), 6 warm-up
  epochs at 0.0. The 1.0 is the floor, not the empty-pool value (`post:1676` sets 1.0 unfloored when
  the pool is empty): the within-epoch pool admitted 345 032 data-phase rows over the 234 walks
  (`stdout.txt:2492`). The loader disables any ceiling at or below 1.0 (`GnssConfigLoadNetwork.cpp:
  7505-7512`, and again after the service bundle at 12612-12620), so on the golden no ceiling lies
  between 1.0 and the settled sigma0: the A4 arms of section 5 are built on that measurement;
- T2 adapts on `joint` only (3 adaptations, 3 isolated-arc refloats, 83 clock-move events);
- the KP K2 margin line prints once on the 10 arms built on the retired golden (`cil`, `wcff`,
  `fcs2`, `shd`, `trc` and the four `wcfp` probes of G2, `cil` of TD1b); the W6 first-N line on every
  arm with W6 (not `glo`, not `zero`); the card-1d checksum on most.

`joint`'s posture arms `network.carry_coupling_joint_whiten` (`runs/_rap_n13s_joint/rapid.json`),
which R1 deletes; after the deletion the key is ignored (the loader has no unknown-key check, PLAN
section 0) and the arm becomes the scalar-coupling replay with the same injected slip. Its reach must
be re-measured on the post-deletion reference. The card RPD arms show that forcing the trigger
reaches T2 with arc and station culprits on the field golden (`docs/refound/
RPD_replay_station_prior_2026-10-01.md` section 3.2: trigger 0.5, `raim_k_sigma` 3.0 gave 30
re-solves, 18 arcs, 12 stations; 1.5 gave 234, 160, 74), and that the re-solve failure, the undo and
the restore-failure exit were reached only with an uncommitted injector (section 7.4).

---

## 2. The new types and functions

All stage functions take EP1's seam form: `(NetworkRun& run, const NetworkEpoch& ep, kept...,
outputs...)`, the run always non-const (EP1 deviation 1; `NetworkRun.hpp`: "Passed as NetworkRun&,
never const"). Helpers that a unit test should call take plain values. Every declaration below
carries full Doxygen in its header (`@brief`, one `@param` per parameter saying read or written,
`@return`); the intent of each is given here in one or two lines.

New functions: 26 (14 stages, 8 helpers, and 4 for the exits: `networkSolveSkip`,
`networkSigma0WithholdTail`, and the file-local `networkSolveEpochStatusRow` and
`networkWithheldHeartbeat`), plus the two `SolveSkipFacts::of` factories; the orchestrator is
rewritten; `networkClockDatumDeclaration` and three run-end reports move whole. SG2 adds 8 more
(section 3.4).

### 2.1 `include/filter/NetworkSolveGlue.hpp`: the stage table, the typed values, the exits

The banner is the header a newcomer reads first. It replaces the INC-9b text (`SG.hpp:2-35`).

```cpp
/**
 * @file NetworkSolveGlue.hpp
 * @brief The network producer's solve for one epoch: the datum rows, the null-space gate, the
 *        estimator solve, the within-epoch RAIM tier and the sigma0 ceiling, as a fixed list of
 *        stages (card NOM3 step SG).
 *
 * networkSolveGlue calls the stages below in this order and hands each the values the earlier
 * ones produced.  The order is load-bearing: the estimator accumulates the folds in this order
 * (its increments are not bit-symmetric, card KP), and every later stage reads what an earlier
 * one wrote.  Four stages can end the epoch, five ways; each way is a row of kSolveSkips.
 *
 *  #   stage                         home                              hands on              exit
 *  1   networkCensusSigma0           filter/NetworkPrefitCensus        ctx.carry H sigma0
 *  2   networkCodeBiasDatum          filter/NetworkCodeBiasDatum       est rows, ws capture
 *  3   networkClockDatum             filter/NetworkClockDatum          SolveDatum            DatumFailed
 *  4   networkPreGatePriors          filter/NetworkSolveGlue           est priors, ws
 *  5   networkEstimabilityDump       filter/NetworkRankGate            traces
 *  6   networkKaulaMargin            filter/NetworkBiasAdmission       log, once
 *  7   networkSbasisCompletion       filter/NetworkSbasisCompletion    est rows, ws
 *  8   networkRankGate               filter/NetworkRankGate            est heal priors, ws   GateWithheld
 *  9   networkClockModelWarm         filter/NetworkSolveGlue           clk_model_warm
 * 10   networkPostGatePriors         filter/NetworkSolveGlue           est priors, ws
 * 11   networkPrimarySolve           filter/NetworkSolveGlue           x_s                   SolveFailed
 * 12   networkClockAnchorGuardEpoch  filter/NetworkClockAnchor         x_s, ws
 * 13   networkWithinEpochAdapt       filter/NetworkRaimWithinEpoch     WithinEpochOutcome    WeRestoreFailed
 * 14   networkSigma0Ceiling          filter/NetworkRaimWithinEpoch     Sigma0CeilingDecision Sigma0Withheld
 *
 * Order constraints a new stage must respect:
 *  - stage 1 precedes every H line of the epoch (the gate and sigma0 exits read its value);
 *  - stage 2 clears this epoch's datum scratch even when the W6 rows are not built, and its
 *    first-N log reads the estimability budget before stage 5 spends it;
 *  - everything the gate must see is folded before stage 8: the W6 rows, the GFC birth rows and
 *    the KP prior (stage 2), the clock rows (stage 3), the SH and pin-bridge priors (stage 4),
 *    the SBC rows (stage 7); the gate, the dump and the heal read one tolerance (DG2);
 *  - stage 6 takes lambda_max of the reduced system after the KP prior and the clock rows and
 *    before the SBC rows; it equals the gate's only on an epoch where SBC folds nothing and no
 *    clock-clone view is formed (the gate then judges the clone-marginalized view);
 *  - after the gate, in this order: the ephemeris re-level, the handover S-transform, the D2
 *    carry prior, the clock-oscillator prior, the cold clock anchor (stage 10);
 *  - stage 12 runs after the solve and before T2, so a prior fault is refloated before T2
 *    attributes it to data;
 *  - stage 14's recorder edge precedes the epoch's SB_FRAMEDIAG consumer; its withhold tail
 *    dumps the recorder, then erases the freed carries;
 *  - the carve-out is decided in stage 14 and applied in NetworkSolPrep.
 * A new pre-gate or post-gate fold goes into networkPreGatePriors / networkPostGatePriors at
 * its place in that order; a new T2 response class goes to filter/NetworkRaimWithinEpoch; a new
 * way to end the epoch is a SolveSkip row.  No stage has its own section timer: the whole solve
 * is SB_SOLVEGLUE.
 *
 * One access path per object: the solve writes ws, est_index_of and dd_build, and inside the
 * solve a stage reaches them only through those parameters, never through ep.ws,
 * ep.assembly.est_index_of or ep.dd_build.  The one place the two paths meet is EP1's DD build:
 * the within-epoch replay calls networkBuildCarryDdPriors(run, ep, dd_build), which reads the
 * culprit sets as ep.ws.we_culprits (and the slip funnel as ep.funnel) while stage 13 adapts
 * the sets through its ws parameter.  They are one object because the spine passes ep.ws and
 * every stage receives the solve's own ws by reference (card WCF; never a copy).
 */
```

The typed values (plain aggregates; each member's comment names its writer):

```cpp
/// The clock datum of this epoch (stage 3): the S-basis plan and whether the clock pivot moved.
struct SolveDatum {
    NetworkSbasis::Plan plan;            ///< Pins or constraint rows, the declared dimension, the pivots (networkClockDatum).
    bool                datum_changed{false};   ///< A clock-pivot handover was signalled this epoch.
    std::set<char>      changed_consts;  ///< The constellations whose pivot changed (the scoped response).
};

/// What the within-epoch tier T2 hands the ceiling and the result (stage 13).
struct WithinEpochOutcome {
    int    n_adapt{0};            ///< Re-solves that landed.
    double sigma0_adapt{0.0};     ///< The final within-epoch sigma0; 0 when the tier did not assess.
    long long prm_free_landed{0}; ///< The landed replay's withheld carried priors (card PRM), counted on a kept epoch.
    bool   era_live{false};       ///< The previous loop epoch was an A4-withheld decision (the era continues).
    bool   escalated{false};      ///< The withhold-era budget escalation was in effect.
    std::map<char, std::vector<double>> sigma0_pool_c;   ///< The last assessment's rows per constellation (the carve-out; permuted by stage 14).
    bool   t2_opp{false};         ///< The B8/T2 flag-rate opportunity, committed at the emit site.
    bool   t2_flag{false};        ///< The first-assess sigma0 exceeded the trigger.
    WithinEpochCulprits landed;   ///< The landed replay's culprits for the fixed-cycle ledger (card FCF).
};

/// The sigma0 ceiling's per-constellation decision (stage 14), applied by NetworkSolPrep.
struct Sigma0CeilingDecision {
    bool active{false};                                     ///< A publishable subset exists.
    std::set<char> publish;                                 ///< Constellations kept in the emission.
    std::set<char> dec_withhold;                            ///< Constellations the decision withheld.
    std::map<char, NetworkEstimator::PerConstSigma0> stats; ///< sigma0_c and rows, for the log and events.
};
```

The gate's counts (`gate_observed`, `gate_first_observed`, `gate_ev_max`) are read only inside
`networkRankGate` (1.4), so they stay its locals and no verdict type is needed; the stage returns
`EpochAction`.

The exits:

```cpp
/// The five ways the solve ends an epoch early (card NOM3 step SG).  Enumerators in the order
/// the stages can reach them; kSolveSkips has one row per enumerator, in this order.
enum class SolveSkip : std::uint8_t { DatumFailed, GateWithheld, SolveFailed, WeRestoreFailed, Sigma0Withheld };

/// The values an exit's row and tail read, from the stages that ran before it.  Plain values and
/// non-owning pointers to the orchestrator's own objects; empty (all null or zero) at DatumFailed.
struct SolveSkipFacts {
    const NetworkSbasis::Plan* plan{nullptr};       ///< Stage 3's plan: the trace row's declared dimension, the sigma0 tail's recorder dump.
    bool   datum_changed{false};                    ///< The status row's datum_changed column.
    double sigma0_adapt{0.0};                       ///< The T2 sigma0 (status row; the sigma0 H line without the census feed); 0.0 before T2.
    int    n_adapt{0};                              ///< Re-solves landed (status row).
    const std::vector<int>* est_index_of{nullptr};  ///< The solve's station-slot map (the sigma0 tail's recorder dump).
    /// The facts after stage 3 (GateWithheld, SolveFailed).
    static SolveSkipFacts of(const SolveDatum& d);
    /// The facts after stage 13 (WeRestoreFailed, Sigma0Withheld).
    static SolveSkipFacts of(const SolveDatum& d, const WithinEpochOutcome& we,
                             const std::vector<int>& est_index_of);
};

/// One exit, declared: what networkSolveSkip writes for it.  Every exit counts n_skipped, writes
/// one network_epoch_status row with `disposition` and advances prev_week / prev_tow last.
struct SolveSkipRow {
    SolveSkip   skip;
    const char* disposition;      ///< The status row's disposition (the frozen literal).
    const char* site;             ///< The stage that detects it (documentation).
    bool        col_datum_changed;///< Status row: datum_changed.
    bool        col_t2_sigma0;    ///< Status row: sigma0 = sigma0_adapt, n_adapt, sigma0_first (when assessed).
    bool        col_t2_meters;    ///< Status row: n_we_refloat_sats, we_clk_move_max_m / _sat (when assessed).
    int         trace_flag;       ///< network trace row with this withheld flag; -1 = no row.
    bool        h_line;           ///< Product heartbeat (H with no E line), or the pre-open buffer.
    NetworkProducerStatus::State h_state;   ///< Its state token when h_line.
    void (*tail)(NetworkRun&, const NetworkEpoch&, const SolveSkipFacts&);  ///< Run after the H line, before the time stamp; nullptr = none.
};
extern const std::array<SolveSkipRow, 5> kSolveSkips;   // defined in NetworkSolveGlue.cpp

/// Ends the epoch through @p skip's row: n_skipped, the trace row, the status row, the H line,
/// the tail, then prev_week / prev_tow.  @return a default result with action SkipEpoch.
NetworkSolveGlueResult networkSolveSkip(NetworkRun& run, const NetworkEpoch& ep, SolveSkip skip,
                                        const SolveSkipFacts& f);
```

The table, as frozen at `post` (each cell is read from the frozen exit code):

| row | disposition | site | datum_changed | T2 sigma0 cols | T2 meters | trace flag | H line | tail | frozen code |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| DatumFailed | `datum_failed` | `networkClockDatum` | no | no | no | none | none | none | `post:673-678` |
| GateWithheld | `gate_withheld` | `networkRankGate` | yes | no | no | 0 | `WithheldGate`, sigma0 = census when `h_sigma0_census`, else 0 | none | `post:1207-1241` |
| SolveFailed | `solve_failed` | `networkPrimarySolve` | yes | no | no | none | none | none | `post:1364-1370` |
| WeRestoreFailed | `we_restore_failed` | `networkWithinEpochAdapt` | yes | yes | no | none | none | none | `post:2350-2358` |
| Sigma0Withheld | `sigma0_withheld` | `networkSigma0Ceiling` | yes | yes | yes | 1 | `WithheldSigma0`, sigma0 = census when `h_sigma0_census`, else `sigma0_adapt` | `networkSigma0WithholdTail` (RHS dump, free erase) | `post:2510, 2512-2588` |

The trace row's declared dimension is `f.plan->declared_dimension` (the frozen `plan.declared_dimension`
at `post:1210, 2514`). The GateWithheld H sigma0 is `census ? census : f.sigma0_adapt` with
`f.sigma0_adapt` = 0.0 there (the gate precedes T2), which equals the frozen default
`NetworkProducerStatus::sigma0{0.0}` (`SsrCorrectionStore.hpp:54`) left unassigned at `post:1228`.

The orchestrator (`src/core/filter/NetworkSolveGlue.cpp`), written against `B_SG` (after EP1c):

```cpp
NetworkSolveGlueResult networkSolveGlue(NetworkRun& run, const NetworkEpoch& ep,
                                        EpochWorkspace& ws, std::vector<int>& est_index_of,
                                        EpochDdBuild& dd_build) {
    networkCensusSigma0(run, ep, est_index_of);
    networkCodeBiasDatum(run, ep, ws);
    SolveDatum datum;
    if (networkClockDatum(run, ep, datum) == EpochAction::SkipEpoch)
        return networkSolveSkip(run, ep, SolveSkip::DatumFailed, SolveSkipFacts{});
    networkPreGatePriors(run, ep, datum, ws);
    networkEstimabilityDump(run, ep, datum);
    networkKaulaMargin(run);
    networkSbasisCompletion(run, ep, datum, ws);
    if (networkRankGate(run, ep, datum, ws) == EpochAction::SkipEpoch)
        return networkSolveSkip(run, ep, SolveSkip::GateWithheld, SolveSkipFacts::of(datum));
    const bool clk_model_warm = networkClockModelWarm(run);
    networkPostGatePriors(run, ep, datum, clk_model_warm, ws);
    // [the frozen comment post:1331-1343: why the pivot baseline advances only at the emit site]
    Eigen::VectorXd x_s;   // the frozen declaration (post:1345), moved
    if (networkPrimarySolve(run, ep, datum, x_s) == EpochAction::SkipEpoch)
        return networkSolveSkip(run, ep, SolveSkip::SolveFailed, SolveSkipFacts::of(datum));
    // [the frozen comments post:1373-1379, 1382-1387: what the hand-off sets mean]
    NetworkPostFitHandoff handoff;
    networkClockAnchorGuardEpoch(run, ep, datum, handoff, x_s, ws);
    WithinEpochOutcome we;
    if (networkWithinEpochAdapt(run, ep, datum, x_s, ws, est_index_of, dd_build, handoff, we)
            == EpochAction::SkipEpoch)
        return networkSolveSkip(run, ep, SolveSkip::WeRestoreFailed,
                                SolveSkipFacts::of(datum, we, est_index_of));
    Sigma0CeilingDecision ceiling;
    if (networkSigma0Ceiling(run, ep, we, ceiling) == EpochAction::SkipEpoch)
        return networkSolveSkip(run, ep, SolveSkip::Sigma0Withheld,
                                SolveSkipFacts::of(datum, we, est_index_of));
    // The hand-off to the epoch (the frozen epilogue post:2593-2611, its names realized).
    NetworkSolveGlueResult result;
    result.x_s = std::move(x_s);
    result.plan = std::move(datum.plan);
    result.changed_consts = std::move(datum.changed_consts);
    result.datum_changed = datum.datum_changed;
    result.clk_model_warm = clk_model_warm;
    result.handoff = std::move(handoff);
    result.retro_feed = run.ctx.knobs.retro_feed;
    result.n_adapt = we.n_adapt;
    result.sigma0_adapt = we.sigma0_adapt;
    result.perconst_active = ceiling.active;
    result.perconst_publish = std::move(ceiling.publish);
    result.perconst_dec_withhold = std::move(ceiling.dec_withhold);
    result.perconst_stats = std::move(ceiling.stats);
    result.we_t2_opp = we.t2_opp;
    result.we_t2_flag = we.t2_flag;
    result.we_landed = std::move(we.landed);
    return result;
}
```

### 2.2 The stages and helpers, with their homes

| function | signature (EP1 form; outputs last) | home | intent (Doxygen @brief) |
| --- | --- | --- | --- |
| `networkCensusSigma0` | `void (NetworkRun&, const NetworkEpoch&, const std::vector<int>& est_index_of)` | `NetworkPrefitCensus.{hpp,cpp}` | The census-fed H sigma0 of this epoch, computed once before any H line (measure-only; 0.0 when off or empty). `est_index_of` is the solve's own map, passed (one access path). |
| `networkCodeBiasDatum` | `void (NetworkRun&, const NetworkEpoch&, EpochWorkspace& ws)` | `NetworkCodeBiasDatum.{hpp,cpp}` (new) | The W6 satellite code-bias datum: clears the epoch's datum scratch, builds and folds the rows, records their telemetry, folds the GFC birth and KP priors, captures all of it for the replay. |
| `networkIscBridgedRoster` | `std::set<std::string> (const std::vector<NetStation>& stations, const std::vector<bool>& present, const GnssConfig& config_)` | same | The (satellite, signal) inter-signal corrections some present station's data determine (RF-0f); empty unless `network.sat_isc_state`. Unit-testable. |
| `networkExtraBandRoster` | same parameters | same | The (satellite, extra signal, pin pair) code-bias columns the data determine (B5D); empty unless `network.sat_gf_extra_band_free`. Unit-testable. |
| `networkCodeBiasDatumRecord` | `void (NetworkRun&, const NetworkEpoch&, const NetworkSbasis::CodeBiasDatumRows& cbd, const std::set<std::string>& isc_bridged)` | same | The on-path telemetry of the folded rows and the emit site's per-key ISC verdicts and held extra-band keys (RF-0b, RF-0c, RF-0f, B5D). Its body reads `this_epoch_if_pin_real` and `registered_code_keys`, bound by its preamble. |
| `networkCodeBiasDatumFirstNLog` | `void (NetworkRun&, const NetworkEpoch&, const NetworkSbasis::CodeBiasDatumRows& cbd)` | same | The first-N W6 summary line, gated on the estimability budget before the dump spends it. |
| `networkFoldCodeBiasKaulaPrior` | `void (NetworkRun&, const NetworkEpoch&, EpochWorkspace& ws)` | `NetworkBiasAdmission.{hpp,cpp}` | The card KP physical-bound prior on the admitted OSB columns, folded and captured. |
| `networkKaulaMargin` | `void (NetworkRun&)` | same | The one-shot K2 margin of the KP prior against the rank-gate cutoff, after the clock rows and before the SBC rows. |
| `networkClockDatumDeclaration` | unchanged (`SG.hpp:109-111`) | `NetworkClockDatum.{hpp,cpp}` (new) | Moved whole. |
| `networkClockDatum` | `EpochAction (NetworkRun&, const NetworkEpoch&, SolveDatum& datum)` | same | Elects the clock datum (HLT-aware, sticky), folds the constraint-realized clock rows, decides the pivot-handover verdict; SkipEpoch when the election throws. |
| `networkPreGatePriors` | `void (NetworkRun&, const NetworkEpoch&, const SolveDatum&, EpochWorkspace& ws)` | `NetworkSolveGlue.cpp` | The soft priors the gate must see (SH-VTEC, open pin bridges) and the datum verdict the next election reads. |
| `networkEstimabilityDump` | `void (NetworkRun&, const NetworkEpoch&, const SolveDatum&)` | `NetworkRankGate.{hpp,cpp}` (new) | The budgeted spectrum of the system the gate is about to judge (DG5, RF-NS). |
| `networkSbasisCompletion` | `void (NetworkRun&, const NetworkEpoch&, const SolveDatum&, EpochWorkspace& ws)` | `NetworkSbasisCompletion.{hpp,cpp}` (new) | S-basis completion: constrains each separated extra near-null direction at its carried level, events engage, release, skip and refusal (invariant 7). |
| `networkSbcContinuityTargets` | `Eigen::VectorXd (const NetworkRunContext& ctx, const NetworkEstimator& est, bool datum_changed)` | same | The carried shared state mapped into this epoch's columns (zero on a datum re-reference). Its preamble binds `auto& code_bias_prev_ = ctx.carry.code_bias_prev_;` (the frozen alias, const through the const context; every estimator method it calls is const, `NetworkEstimator.hpp:164-218`). Unit-testable. |
| `networkDatumGraphSpectralHandoff` | `void (NetworkRunContext& ctx, const NetworkSbasis::GaugeCompletion& sbc, const NetworkSbasis::Plan& plan)` | `NetworkDatumGraph.{hpp,cpp}` | The census's spectral half with direction identities, taken before completion (`dgs.declared = plan.declared_dimension`, `post:929`). |
| `networkRankGate` | `EpochAction (NetworkRun&, const NetworkEpoch&, const SolveDatum&, EpochWorkspace& ws)` | `NetworkRankGate.{hpp,cpp}` | The null-space rank gate with the prioritized drop-and-heal and one re-gate; the withhold accounting, the envelope, the datum-direction ratio, the DG fallback. SkipEpoch when the gate withholds. |
| `networkClockModelWarm` | `bool (NetworkRun&)` | `NetworkSolveGlue.cpp` | The two-state clock model is past its warm-up (its predictions fold and its update runs). |
| `networkPostGatePriors` | `void (NetworkRun&, const NetworkEpoch&, const SolveDatum&, bool clk_model_warm, EpochWorkspace& ws)` | `NetworkSolveGlue.cpp` | The folds after the gate, in their load-bearing order: ephemeris re-level, handover S-transform, D2 carry, clock oscillator, cold clock anchor. |
| `networkFoldColdClockAnchor` | `void (NetworkRun&, const NetworkEpoch&, EpochWorkspace& ws)` | `NetworkClockAnchor.{hpp,cpp}` | The cold-start a-priori satellite-clock Tikhonov anchor, when its sigma binds and the datum is not broadcast-anchored. |
| `networkPrimarySolve` | `EpochAction (NetworkRun&, const NetworkEpoch&, const SolveDatum&, Eigen::VectorXd& x_s)` | `NetworkSolveGlue.cpp` | The shared solve and the card-1d checksum; SkipEpoch when the solve throws. |
| `networkClockAnchorGuardEpoch` | `void (NetworkRun&, const NetworkEpoch&, const SolveDatum&, const NetworkPostFitHandoff&, Eigen::VectorXd& x_s, EpochWorkspace& ws)` | `NetworkClockAnchor.{hpp,cpp}` | The Q3 guard between the solve and T2 (one call to `networkClockAnchorGuard`, whose explicit form stays for `tests/test_clock_anchor.cpp`; it takes the refloat set const, `NetworkClockAnchor.hpp:347`). |
| `networkWithinEpochAdapt` | `EpochAction (NetworkRun&, const NetworkEpoch&, const SolveDatum&, Eigen::VectorXd& x_s, EpochWorkspace& ws, std::vector<int>& est_index_of, EpochDdBuild& dd_build, NetworkPostFitHandoff& handoff, WithinEpochOutcome& we)` | `NetworkRaimWithinEpoch.{hpp,cpp}` (new) | T2: assess, drop the dominant culprit, replay and re-solve, undo a failed re-solve, settle; then the reduce-once check. SkipEpoch when an undo cannot complete. |
| `networkSigma0Ceiling` | `EpochAction (NetworkRun&, const NetworkEpoch&, WithinEpochOutcome& we, Sigma0CeilingDecision& ceiling)` | same | The A4 ceiling: the per-constellation carve-out, the recorder edge, both withhold-era edges, the withhold decision; on a kept epoch the PRM response. (It reads no datum value once the exit row holds the trace and status writes.) |
| `networkSigma0WithholdTail` | `void (NetworkRun&, const NetworkEpoch&, const SolveSkipFacts& f)` | same | The sigma0 exit's tail: the recorder dump of the withheld epoch, then the free erase (G5-safe order). Its preamble binds `const NetworkSbasis::Plan& plan = *f.plan;` and `const std::vector<int>& est_index_of = *f.est_index_of;`. |
| `networkSolveEpochStatusRow` | `void (NetworkRun&, const NetworkEpoch&, const char* disposition, const EpochStatusRow& r)` | `NetworkSolveGlue.cpp` (file-local) | The former `emitEpochStatus` lambda: stamps the admission and DD-fold columns, writes the row. |
| `networkWithheldHeartbeat` | `void (NetworkRun&, const NetworkEpoch&, NetworkProducerStatus::State, double sigma0)` | same (file-local) | The H line of a withheld epoch (or its pre-open buffer); the two frozen copies made one. |

Run-end reports moved beside their per-epoch code, bodies byte-identical: `networkRankGateRunEnd`
(`NetworkEstimator.cpp:1768-1852`, declared `NetworkEstimator.hpp:1724`) to `NetworkRankGate.cpp`;
`networkWithinEpochRaimRunEnd` (`NetworkRaim.cpp:1449-1505`, `NetworkRaim.hpp:313`) to
`NetworkRaimWithinEpoch.cpp`; `networkSbasisCompletionRunEnd` (`NetworkSbasis.cpp:2536-2569`,
`NetworkSbasis.hpp:1872`) to `NetworkSbasisCompletion.cpp`. None is edited by the deletion's tracks
(same length on every `rmk/*` tip). Three rows of `kNetworkRunEndReports` change their home string
(`NetworkRunEnd.cpp:111, 113, 117` at main and at `rmk/R1`; the tracks edit other rows of the table)
and the table's includes follow; the home column is not printed (`NetworkTeardown.cpp:44`) and
`NetworkRunEndTable` checks only that it is a `.cpp` path (`test_network_run_end.cpp:55-60`).
`tests/test_network_run_end.cpp` includes `filter/NetworkRaim.hpp` (line 30) only for
`networkWithinEpochRaimRunEnd` (its WCF placement test, line 95; no other `NetworkRaim.hpp` name
occurs in the file), so that include becomes `filter/NetworkRaimWithinEpoch.hpp`. The historical TD
tooling still names the old homes: `scripts/nom_td_cut.py` rows 463-480 (`filter/NetworkEstimator`,
`filter/NetworkSbasis`, `filter/NetworkRaim`) and `scripts/nom_td_manifest.json` 232, 265, 327; both
are recorded under decision 7.

New knob: `NetworkKnobs::retro_feed` = `k.ura_retro || k.ura_refloat_inflate > 1.0` in
`NetworkKnobs::snapshot` beside `ura_retro` and `ura_refloat_inflate` (`NetworkKnobs.cpp:196, 198`),
Doxygen "the retro-bump / URA-inflate feed is armed (B1, P0-2)", a case in `NetworkKnobsEpochDriver`.

### 2.3 Sizes after SG1, on the `post` basis

| file | lines today | after SG1 (approximate) |
| --- | --- | --- |
| `NetworkSolveGlue.cpp` | 2 868 at `main`, 2 614 at `post` | about 450: banner and includes 70, orchestrator 75, pre-gate 30, clock-model warmth 15, post-gate 45, primary solve 45, the table, `networkSolveSkip`, its two writers and the facts factories 150 |
| `NetworkCodeBiasDatum.cpp` (new) | | about 400 (`post:259-634` minus KP's 32 lines, plus preambles) |
| `NetworkClockDatum.cpp` (new) | | about 140 |
| `NetworkRankGate.cpp` (new) | | about 380 (comments 25, dump 40, gate 175, the moved report 85, preambles) |
| `NetworkSbasisCompletion.cpp` (new) | | about 260 (196, the moved report 34, preambles) |
| `NetworkRaimWithinEpoch.cpp` (new) | | about 1 300 (T2 965, ceiling 220, tail 30, the moved report 57, preambles) |
| `NetworkBiasAdmission.cpp` | 180 | about 260 |
| `NetworkClockAnchor.cpp` | 1 087 | about 1 170 |
| `NetworkPrefitCensus.cpp` | 841 | about 860 |
| `NetworkDatumGraph.cpp` | 694 | about 725 |
| `NetworkSbasis.cpp` | 3 031 at `main`, 2 660 at `rmk/R1` | about 2 630 |
| `NetworkEstimator.cpp` | 1 911 at `main`, 1 812 at `rmk/R1` | about 1 720 |
| `NetworkRaim.cpp` | 1 665 at `main`, 1 528 at `rmk/R2` | about 1 330 (after all tracks and the report move) |

No new file exceeds 2 400 lines; `NetworkSbasis.cpp` already does and SG adds nothing to it.

---

## 3. The cut

### 3.1 How the generator reads the table

Like `scripts/nom_td_cut.py` (one canonical table, frozen pieces, a manifest the verifier pairs
with) and like EP1's rows (no line number in a row; anchors resolved by `--survey` at whatever base
it is given, a missing anchor refuses the generation). Each stage function is: generated Doxygen,
signature, generated preamble, frozen pieces in order with the generated statements between them
(EXTRACT calls), generated tail (`return EpochAction::Proceed;`, or the typed return), all inside a
marked region (`// ---- Solve stages (card NOM3 step SG; generated by scripts/nom_sg_cut.py) ----` ...
`// ---- End of the solve stages ----`) at the end of the home `.cpp`. Comments travel with their code
and are never reflowed.

**A piece** is (first-line anchor, last-line anchor, form, indent delta). Its lines are whole lines
of the frozen source. The form states how the piece sits in the block structure, because a stage
whose pieces are separated by extracted blocks keeps a block open across them (the W6 guard at
`post:287`, opened by R04 and closed by R11c; the `if (cbd.H.rows() > 0) {` block at `post:435`,
opened by R07 and closed inside R09):

| form | meaning | brace scan of the piece alone |
| --- | --- | --- |
| `whole` | balanced | net 0, never below 0 |
| `open` | ends inside k blocks it opened | net +k, never below 0 |
| `close` | starts inside k blocks an earlier piece of the same function opened | net -k |
| `mid` | both | as scanned; the function's running depth stays at or above 0 |

The function-level rule: over its pieces in order, the running brace depth starts at 0, never goes
below 0 and ends at 0, and every generated statement between two pieces sits at the depth the
earlier piece left (its indentation is that depth's). A piece that holds only a closing brace is
anchored by `close_of(<anchor of the opening line>)`, resolved by brace matching in the frozen
source, so the closer cannot attach to the wrong block. `check_sg1.py` applies this rule to the table
of 3.2 on `post` (all 24 bodies pass) and the verifier applies it at `B_SG` (S1b).

The body of the solve sits at column 0 (`post:259-2591`), so pieces from it gain 4 columns (the new
function's body is one block deep); pieces already inside a block keep their depth relative to the
new body (an extracted block at the frozen depth 1 becomes a helper body at depth 1: delta 0; one at
depth 2 loses 4 columns).

**The preamble.** One line per name the body reads from the solve's scope, in the order the solve
declared them, dependency-closed (a line that reads `nc` brings `config_`). The kinds, a closed list:

1. EP1's binding lines of the solve at `B_SG`, verbatim (`NetworkRunContext& ctx = run.ctx;`, `int
   n_clock = ep.columns.n_clock;`, ...; 21 lines, Appendix A row 52 without the outputs and the
   callbacks);
2. the solve's own alias lines (`SG.cpp:126-204, 262-270`), verbatim; a helper whose parameters are
   plain values may carry an alias line whose right side reads only its parameters (the continuity
   targets' `auto& code_bias_prev_ = ctx.carry.code_bias_prev_;` through its `ctx` parameter);
3. realization lines for the crossing locals of section 1.4: `const T& x = v.m;` for a reader,
   `T& x = v.m;   // written` for a writer, where `v` is the typed value parameter; the sigma0
   tail's two lines dereference `SolveSkipFacts` pointers (`const NetworkSbasis::Plan& plan =
   *f.plan;`);
4. the knob line `const bool retro_feed = ctx.knobs.retro_feed;` (KNOB);
5. the frozen `emitEvent` lambda, verbatim (LAMBDA_TEXT), in the four stages that call it;
6. (SG2 only) the one-line forwarding lambda of a T2 callable (CALLABLE).

The solve's outputs (`ws`, `est_index_of`, `dd_build`) and the orchestrator's values (`x_s`,
`clk_model_warm`, `cbd`, `isc_bridged`, ...) arrive as parameters that keep their frozen names, so
they need no line. No preamble line binds `ep.ws`, `ep.assembly.est_index_of` or `ep.dd_build` (S11).

### 3.2 SG1: the table

Ranges are `post` (and `main`), inclusive. Forms as in 3.1. "Out" lists the output parameters the row
declares. Every code line of `post:93-2612` lies in exactly one row (`check_sg1.py`).

| row | pieces `post` (`main`), form | new function, file | indent | out | declared edits |
| --- | --- | --- | --- | --- | --- |
| R00 | 59-91 (60-98), whole | moved whole, `NetworkClockDatum.cpp`; declaration from `SG.hpp:95-111` to `NetworkClockDatum.hpp` | 0 | | HOME_MOVE; `NetworkHandoverStransform.cpp:11` includes `NetworkClockDatum.hpp` |
| R01 | 189-200 (205-216), whole: from `// The census-fed H σ̂₀` through the closing brace of `if (ctx.knobs.h_sigma0_census) {` | `networkCensusSigma0`, `NetworkPrefitCensus.cpp` | 0 | | kept parameter `est_index_of` |
| R02 | 202-237 (218-253), whole: the body of the lambda at 201 (217) | `networkSolveEpochStatusRow`, `NetworkSolveGlue.cpp` | -4 | | LAMBDA_TO_FN (parameters `disposition`, `r` keep their names) |
| R03 | 239-245 (255-261) | repeated in the preambles of the four stages that call it: R19 (SBC), R20 (gate), R30 (T2), R31 (ceiling) | 0 | | LAMBDA_TEXT |
| R04 | 259-297 (275-315; `main` 316-358 is S07b, deleted by R1), open (+1: the W6 guard `post:287`) | `networkCodeBiasDatum` (head: the clears, the guard, `freqOf`) | +4 | ws | |
| R05 | 298-336 (360-398), whole | `networkIscBridgedRoster` | 0 | | EXTRACT: in place, at the guard's depth, `std::set<std::string> isc_bridged = networkIscBridgedRoster(stations, present, config_);`; helper tail `return isc_bridged;` |
| R06 | 338-385 (400-447), whole | `networkExtraBandRoster` | 0 | | EXTRACT, likewise (`extra_band_determined`) |
| R07 | 387-439 (449-507), mid (+1: `if (cbd.H.rows() > 0) {` `post:435`) | `networkCodeBiasDatum` (rows, hand-offs, roster pin, fold) | +4 | ws | |
| R08 | 440-532 (508-602), whole | `networkCodeBiasDatumRecord` | -4 | | EXTRACT: in place, inside the R07 block, `networkCodeBiasDatumRecord(run, ep, cbd, isc_bridged);` |
| R09 | 533-567 (603-637), mid (-1: `post:539` closes the R07 block) | `networkCodeBiasDatum` (capture, GFC birth fold) | +4 | ws | |
| R10 | 568-599 (638-669), whole | `networkFoldCodeBiasKaulaPrior`, `NetworkBiasAdmission.cpp` | 0 | ws | EXTRACT: in place, inside the guard, `networkFoldCodeBiasKaulaPrior(run, ep, ws);` |
| R11 | 600-633 (670-718), whole | `networkCodeBiasDatumFirstNLog` | 0 | | EXTRACT: in place, inside the guard, `networkCodeBiasDatumFirstNLog(run, ep, cbd);` |
| R11c | 634 (719), close (-1), anchor `close_of(R04's guard line)` | `networkCodeBiasDatum` (the guard's closing brace) | +4 | | |
| R12 | 636-672 (721-757), open (+1: the `catch` at `post:671`); 679-703 (764-788), close | `networkClockDatum`, `NetworkClockDatum.cpp` | +4 | datum | REALIZE `plan`, `changed_consts` (T-B3), `datum_changed` (T-B7); EXIT_RETURN: 673-678 (758-763) become `return EpochAction::SkipEpoch;` (the warning 672 stays); FRESH_STATELESS `sb` |
| R13 | 704-712 (789-797) | none | | | DROP (dead: no reader, `measure_sg.py`) |
| R14 | 714-732 (799-819), whole | `networkPreGatePriors`, `NetworkSolveGlue.cpp` | +4 | ws | |
| R15 | 734-758 (821-845) | the file banner of `NetworkRankGate.cpp` | +0 | | COMMENT_MOVE |
| R16 | 760-798 (847-885), whole | `networkEstimabilityDump`, `NetworkRankGate.cpp` | +4 | | |
| R17 | 800-806 (887-893), whole | `networkRankGate` (first lines) | +4 | | DECL_MOVE past R18 (no read between): `gate_ok` stays a local of the gate |
| R18 | 807-838 (894-925), whole | `networkKaulaMargin`, `NetworkBiasAdmission.cpp` | +4 | | |
| R19 | 840-853 (927-940), open (+1: `if (ctx.knobs.sbasis_complete_on) {` `post:853`); 894-905 (981-992), mid; 947-1035 (1034-1122), close | `networkSbasisCompletion`, `NetworkSbasisCompletion.cpp` | +4 | ws | FRESH_STATELESS `sb`; inner EXTRACTs R19a, R19b |
| R19a | 854-893 (941-980), whole | `networkSbcContinuityTargets` | 0 | | EXTRACT: in place `Eigen::VectorXd sbc_x_prev = networkSbcContinuityTargets(ctx, est, datum_changed);` (the frozen declarator at 860 with its initializer replaced); helper tail `return sbc_x_prev;` |
| R19b | 906-946 (993-1033), whole | `networkDatumGraphSpectralHandoff`, `NetworkDatumGraph.cpp` | 0 | | EXTRACT: in place `networkDatumGraphSpectralHandoff(ctx, sbc, plan);` |
| R20 | 1037-1205 (1124-1292), whole | `networkRankGate` (gate, heal, envelope, info ratio, DG fallback) | +4 | ws | FRESH_STATELESS `sb`; generated tail `return gate_ok ? EpochAction::Proceed : EpochAction::SkipEpoch;` (EXIT_TEST) |
| R21 | 1206-1242 (1293-1329) | none: the GateWithheld row | | | EXIT_TABLE |
| R22 | 1244-1247 (1331-1334), whole | `networkClockModelWarm` | +4 | | RESULT_RETURN (`const bool clk_model_warm = E;` becomes `return E;`) |
| R23 | 1248-1249 (1338-1339) | none | | | DROP (dead aliases, no reader) |
| R24 | 1251-1276 (1341-1366), whole | `networkPostGatePriors` | +4 | ws | then the call `networkFoldColdClockAnchor(run, ep, ws);` (EXTRACT of R25) |
| R25 | 1278-1329 (1368-1419), whole | `networkFoldColdClockAnchor`, `NetworkClockAnchor.cpp` | +4 | ws | EXTRACT |
| R26 | 1331-1343 (1421-1433) | the orchestrator, above the solve call | +4 | | COMMENT_MOVE |
| R27 | 1345 (1435): the orchestrator; 1346-1363 (1436-1455), open (+1: the `catch` at 1361); 1371 (1463), close | `networkPrimarySolve` | +4 | x_s | DECL_MOVE of `Eigen::VectorXd x_s;` to the orchestrator; EXIT_RETURN: 1364-1370 (1456-1462) become `return EpochAction::SkipEpoch;` (the warning 1362-1363 stays) |
| R28 | 1373-1389 (1465-1481) | the orchestrator: comments above `NetworkPostFitHandoff handoff;` | +4 | | REALIZE the three hand-off locals (T-B3, T-B5: the member defaults equal the frozen initializers); KNOB `retro_feed` |
| R29 | 1391-1404 (1483-1496), whole | `networkClockAnchorGuardEpoch`, `NetworkClockAnchor.cpp` | +4 | x_s, ws | |
| R30 | 1406-2349 (1498-2498), open (+1: `if (we_restore_failed) {` `post:2346`); 2359-2370 (2508-2519), close | `networkWithinEpochAdapt`, `NetworkRaimWithinEpoch.cpp` | +4 | x_s, ws, est_index_of, dd_build, handoff, we | REALIZE the eight `WithinEpochOutcome` locals (T-B5, T-B7 for `wh_era_live`, T-B3 for `we_sigma0_pool_c`); RESULT_FIELD `result.we_landed` (`post:2328`); EXIT_RETURN: 2350-2358 (2499-2507) become `return EpochAction::SkipEpoch;` (the error line 2347-2349 stays); the replay's direct call (EP1, after EP1c `networkBuildCarryDdPriors(run, ep, dd_build);`) unchanged |
| R31 | 2372-2509 (2521-2658), open (+1: `if (a4_over_ceiling && !perconst_active) {` `post:2504`); 2511 (2660), mid; 2589-2591 (2738-2740), close | `networkSigma0Ceiling`, `NetworkRaimWithinEpoch.cpp` | +4 | we, ceiling | REALIZE the four `Sigma0CeilingDecision` locals (T-B3, T-B5); EXIT_RETURN before 2589: `return EpochAction::SkipEpoch;`; EXIT_TABLE for 2510 (2659), 2512-2555 (2661-2704), 2586-2588 (2735-2737) with COUNTER_ORDER (`++n_sigma0_withheld` 2511 stays at the site, before the skip's `++n_skipped`); the row's tail 2556-2585 (2705-2734) is `networkSigma0WithholdTail` |
| R32 | 2593-2611 (2846-2864; `main` 2846 is the "18 crossing locals" comment K rewrites) | the orchestrator's epilogue | 0 | | RESULT_FIELD (names realized through the typed values; the three EP1 `result.handoff.*` lines become `result.handoff = std::move(handoff);`, MERGE) |
| R33 | the solve's preamble at `B_SG` (EP1's 21 lines, `SG.cpp:126-204, 262-272`) | | the stage preambles | | PREAMBLE; the 15 unused aliases and `NetworkSolveGlueResult result;` (256) appear in no preamble |

Moves outside the function, in the same commit: the three run-end reports (HOME_MOVE, 2.2), the
three `kNetworkRunEndReports` home strings and the table's includes, the include of
`tests/test_network_run_end.cpp` (2.2), `CMakeLists.txt` (five sources, one test), the knob, and the
header banners (`SG.hpp:2-35`, `SG.cpp:1-10`, the INC-9b text, which still says "four sites").

### 3.3 Declared substitutions (a closed list; anything else fails)

| class | accepted change | proof obligation | entries |
| --- | --- | --- | --- |
| PREAMBLE | a stage or helper opens with bindings | each line is of one of the six kinds of 3.1, verbatim where the kind says so; the set is dependency-closed; every name the body reads that the solve declared at function scope is bound or is a parameter; no line binds `ep.ws`, `ep.assembly.est_index_of`, `ep.dd_build` | every stage and helper |
| REALIZE | a crossing local's declaration becomes its typed value member (EP1's T-B3 default-constructed, T-B5 scalar with equal default, T-B7 initialized store at the frozen position) | the member's type text equals the local's; its default equals the frozen initializer (T-B5) or the store is the frozen initializer at the frozen position (T-B7); no stage reads it before that position; a declaration with two declarators (`bool we_t2_opp = false, we_t2_flag = false;`) realizes both | 18 locals (1.4: 3 datum, 3 hand-off, 8 T2, 4 ceiling) |
| EXTRACT | an inner block becomes a helper's body and one statement takes its place | body verbatim; the helper's parameters bind the names the block read (same storage, declarator kind kept or `const` added) and its preamble follows PREAMBLE rooted at its parameters; the block's outputs are the helper's return (`return x;` of the block's one declared result, whose frozen declarator is kept at the call site with the call as initializer) or its non-const parameters; the range holds no `break`, `continue` or `return` whose target lies outside it (`jumps.py`; such a statement stays at the call site after the call) | R05, R06, R08, R10, R11, R19a, R19b, R25 |
| EXIT_RETURN | `result.action = EpochAction::SkipEpoch; return result;` and the bookkeeping before it become `return EpochAction::SkipEpoch;` | the removed statements equal the row of `kSolveSkips` for that exit (EXIT_TABLE) | R12, R27, R30, R31 |
| EXIT_TEST | the frozen exit test `if (!gate_ok) {` becomes the gate's generated tail `return gate_ok ? EpochAction::Proceed : EpochAction::SkipEpoch;` and the orchestrator's test | nothing between the frozen tail of R20 and the test reads or writes anything the orchestrator's next statements depend on (R21 is the whole range between) | R20 |
| EXIT_TABLE | the five frozen tails are `kSolveSkips` and `networkSolveSkip` | per exit, the ordered effect list (counter increments, trace stream template, status-row field assignments, H fields, tail calls, the `prev_*` advance, return) equals the frozen tail's, modulo COUNTER_ORDER and the GateWithheld H sigma0 VALUE_EQUAL; the sigma0 tail's body is the frozen `post:2556-2585` | R21, R31 and the EXIT_RETURN sites |
| COUNTER_ORDER | `++n_sigma0_withheld` now precedes `++n_skipped` | no statement between the two old positions and the new ones reads either counter (lexer scan of `networkSolveSkip`'s path) | 1 |
| VALUE_EQUAL | `hst.sigma0 = census ? census : 0.0` for GateWithheld equals the frozen unassigned default 0.0 | `NetworkProducerStatus::sigma0{0.0}` (`SsrCorrectionStore.hpp:54`); `SolveSkipFacts::of(datum)` leaves `sigma0_adapt` at 0.0 | 1 |
| RESULT_FIELD | a write or read of `result.X` names its realization (`we.landed`, `datum.plan`, ...) | the realization table; the epilogue's moves happen after every stage, as the frozen epilogue did | R30, R32 |
| MERGE | three member moves into `result.handoff` become one struct move | `NetworkPostFitHandoff` has exactly those three members (EP1 spec 2.4) | R32 |
| RESULT_RETURN | `const T x = E;` as a stage's only output becomes `return E;` | the range holds no other statement that reads `x` | R22 |
| KNOB | `const bool retro_feed = ura_retro || ura_refloat_inflate > 1.0;` (`post:1389`) becomes `NetworkKnobs::retro_feed` | the knob's initializer is the frozen text with each alias replaced by the knob field it aliased (`SG.cpp:168-169`); readers bind `const bool retro_feed = ctx.knobs.retro_feed;` | 1 |
| FRESH_STATELESS | `NetworkSbasis sb;` is declared in each of the three stages that call it | `NetworkSbasis` has no non-static data member (`sbasis_members.py` over `NetworkSbasis.hpp:242-1770`) and `buildSharedDatum`, `completeSbasisDimension`, `assertNullSpaceRank` are `const` (`NetworkSbasis.hpp:1512-1517, 1718-1722, 1766-1770`); the solve calls no other method on it (`post:666, 902, 1050, 1098`) | 3 |
| LAMBDA_TEXT | the frozen `emitEvent` lambda repeated in a stage preamble | text identical to `post:239-245` | 4 |
| LAMBDA_TO_FN | `emitEpochStatus`'s body is `networkSolveEpochStatusRow`'s | body verbatim; the lambda's captures bind through the preamble | 1 |
| DROP | dead code removed | no reader in the frozen function (`handover_affects`, `clk_pred`, `clk_prior_info`, the 15 aliases) | R13, R23, R33 |
| DECL_MOVE | a declaration moves to where it is first needed | no statement between reads it | R17 (`gate_ok` past the KP margin), R27 (`x_s` to the orchestrator) |
| COMMENT_MOVE | comment-only lines move to a banner | no code line in the range | R15, R26, R28 |
| HOME_MOVE | a whole function moves to another file | byte-identical body; one definition and one declaration; the run-end table row's home string names the new file | R00 and the three reports |
| REBIND (SG2) | the locals a block declared and later statements read become `const` bindings to the members of the pure function's returned value | one binding per name read after the block, with the frozen type (or `const` reference to it); no later statement writes the name (the `const` proves it at compile time); names not read after the block are not bound | T4 |
| CATCH_BODY (SG2) | a catch clause's body becomes a helper's body | EXTRACT's obligations; the `catch (...)` header, the call and any jump statement that leaves the clause (`break;` out of the adaptation loop) stay at the call site; the caught exception is a `const std::exception&` parameter with the handler's name | T5 |

### 3.4 SG2: T2's internals (re-surveyed on SG1's tree before it starts)

The three lambdas of R30 become free functions in `NetworkRaimWithinEpoch.cpp`; at each old
definition a one-line forwarding lambda keeps every call site character-exact (EP1's CALLABLE class),
and an extracted helper whose body calls one of them binds it by the same forwarding line in its
own preamble (kind 6). The captured locals the functions share become one plain struct:

```cpp
/// The within-epoch tier's working state for one epoch (card NOM3 step SG2): the locals the
/// frozen T2 block shared between its assessment, its replay and its adaptation loop.
struct WeEpochState {
    bool   we_ci{false};                         ///< network.raim_we_code_iono
    std::vector<char> ci_cb_col;                 ///< WE-CI: shared columns that are satellite code biases
    std::vector<char> orig_added;                ///< stations the primary solve admitted
    bool   data_rows_only{false};                ///< card RPA admission rule
    double s0_code_we{0.0}, s0_iono_we{0.0};     ///< the last assessment's WE-CI pool sigma0
    std::map<std::string, double> ci_sat_code_max;
    std::map<int, double>         ci_sta_max;
    std::set<std::string>         dw_code_sats;  ///< chosen satellite code-row de-weights
    std::string    replay_fail_at;               ///< the station being re-added when a replay threw
    int            replay_fail_idx{-1};
    std::set<int>  undo_dropped;                 ///< stations an undo could not re-add
    long long      we_free_replay{0};            ///< carried arcs whose prior the current replay withholds
};

/// The culprit the adaptation loop drops next (card NOM3 step SG2): its granularity and identity.
struct WeChoice {
    enum class Kind { None, Arc, Sat, Sta };
    Kind        kind{Kind::None};
    int         arc_sta{-1};       ///< Arc culprit: the station.
    std::string arc_sat;           ///< Arc culprit: the satellite.
    std::string sat;               ///< Satellite culprit.
    int         sta{-1};           ///< Station culprit.
};
```

| row | frozen range `post` (`main`) | new function | declared edits |
| --- | --- | --- | --- |
| T1 | 1513-1528 (1605-1620) | `weClassifyCiRow(const std::vector<char>& ci_cb_col, const StationAssembly&, Eigen::Index row, bool& sh_row, bool& code_row, std::string& row_sat)` | CALLABLE: forwarding lambda at `post:1513` |
| T2 | 1570-1701 (1662-1793) | `weAssess(NetworkRun&, const NetworkEpoch&, const SolveDatum&, const std::vector<int>& est_index_of, WeEpochState& st, WithinEpochOutcome& we, const Eigen::VectorXd& xs, double& sigma0_out, sat_eff&, sta_eff&, arc_eff&)` | CALLABLE; the five captured outputs bind to `st` and `we` (the inventory's risk 1); maps by reference, no copy (risk 8) |
| T3 | 1727-1996 (1819-2145) | `weReplay(...) -> Eigen::VectorXd` with inner EXTRACTs `weReaddStation` (`post:1741-1918`, `main` 1835-2060: the per-station body, with the loop index `i` a parameter; the skip at 1736-1740 and its `continue` stay in `weReplay`) and `weRefoldShared` (`post:1920-1982`, `main` 2062-2126) | CALLABLE; Doxygen names the fold order of finding 2; the direct `networkBuildCarryDdPriors(run, ep, dd_build)` call and the WCF counter before it stay in `weReplay` |
| T4 | 2043-2096 (2192-2245) | `weChooseCulprit(const arc_eff&, const sat_eff&, const sta_eff&, const WithinEpochCulprits&, double sigma0, bool surgical, double sta_bias, double raim_k, int raim_min_sta) -> WeChoice` (pure) | EXTRACT; TYPE_MOVE `enum class Kind {...} kind = Kind::None;` (`post:2085`) to `WeChoice::Kind` (the line becomes `Kind kind = Kind::None;`, with `using Kind = WeChoice::Kind;` in the function and in the T2 stage's preamble); its preamble binds the culprit sets `const std::set<std::string>& drop_sats = culprits.sats;` etc. (the frozen declarators of `post:1549-1551` with `const` added); generated tail `return WeChoice{kind, best_arc_sta, best_arc_sat, best_sat, best_sta};`; REBIND at the call site of the five names read after the block (`post:2098-2145` reads `kind`, `best_arc_sta`, `best_arc_sat`, `best_sat`, `best_sta`; `best_arc_eff`, `best_sat_eff`, `best_sta_eff`, `best_score` are not read after 2096): `const WeChoice choice = weChooseCulprit(...); const Kind kind = choice.kind; const int best_arc_sta = choice.arc_sta; const std::string& best_arc_sat = choice.arc_sat; const std::string& best_sat = choice.sat; const int best_sta = choice.sta;`; first unit tests (surgical-first tie-break, consensus escalation) |
| T5 | 2151-2223 (2300-2372): the catch body without its last statement | `weUndo(NetworkRun&, const NetworkEpoch&, const SolveDatum&, WeEpochState& st, WithinEpochOutcome& we, Eigen::VectorXd& x_s, const std::exception& ex, int iter, the seven landed values of `post:2036-2042`, double sigma0, double trig, std::string& adapt_detail, ...)` (the generator derives the full list from the binding check) | CATCH_BODY: the call site reads `catch (const std::exception& ex) {` (2150), `weUndo(...);`, `break;` (2224, the jump out of the adaptation loop `post:2027`), `}` (2225); `resolve_dropping` and `emitEvent` bound in its preamble (kinds 6 and 5) |
| T6 | 2230-2330 (2379-2479) | `weSettle(...)` (PRM count, clock meter, counters, log, events, couple, retro feed, publish) | EXTRACT (`jumps.py`: no jump leaves the range; the `return` at 2281 belongs to an immediately-invoked lambda inside it) |

`kMaxAdapt` and `kWeCiCodeWeightScale` hoist to file-scope `constexpr` with the same names (their two
roles, the adaptation budget and the undo's station-drop budget, are named apart only with an author
decision, 8.9). The replay's install predicate (`post:1892-1893`, lacking PASS B's oracle-pin
disjunct `R.opin_rows > 0`, `ApplicationNetworkAssembly.cpp:5971` at `main`) is left as is: unifying
it is value-equal only under `carry_warm` and belongs with the decoupling fix (decision 8.10).

### 3.5 SG3: the RHS trip's form and the printed citations

- `networkRhsAttribTrip`'s explicit form (`NetworkRhsAttrib.hpp:807-815`) becomes
  `networkRhsAttribTripAt(NetworkRun&, const NetworkEpoch&, const NetworkEstimator::Datum& plan_datum,
  const std::vector<int>& est_index_of, const Eigen::VectorXd* x_s)` with a generated preamble and an
  unchanged body; `est_index_of` stays a parameter so that inside the solve it is reached through the
  solve's own map (S11). EP1's epoch form (the observer the driver and EP3's table call) forwards
  `ep.solve.plan.shared_datum, ep.assembly.est_index_of, &ep.solve.x_s` (the frozen driver call passes
  `&x_s`, `ApplicationNetwork.cpp:2798-2799`); the sigma0 tail calls `networkRhsAttribTripAt(run, ep,
  plan.shared_datum, est_index_of, nullptr)` with the tail's two realized names (the frozen call passes
  no `x_s`, `post:2562-2563`). This retires the explicit form EP1 kept "until step SG rewrites the
  solve's exit" (EP1 spec 3.6). `ApplicationNetworkEpoch.cpp` is not touched.
- Declared output text move, three strings that name a file the code left: the contributions table's
  builder column for `cold_clock_anchor`, "networkSolveGlue, a-priori clock anchor
  (NetworkSolveGlue.cpp)" (`NetworkContributions.cpp:307`) to "networkFoldColdClockAnchor
  (NetworkClockAnchor.cpp)"; the responses table's site column for `amb_t2_replay_free` and
  `amb_withhold_erase` (`NetworkResponses.cpp:54, 72`) to "NetworkRaimWithinEpoch.cpp ...". The
  strings "folded by networkSolveGlue" (`NetworkContributions.cpp:83, 237, 244, 248`) stay true and
  are not changed. HV0's precedent (`827fba6`): the gate reverses the declared substitution and must
  then read IDENTICAL.
- With decision 13, the KP margin comment (`post:807-809`) is corrected to say what 1.5 measures
  (comment only; listed as a declared hand edit for S13).
- CLAUDE.md's routing tree ("New per-epoch estimation/QC step" bullet), its seam contract and the
  network tour gain the stage table's rules; the card gets SG's result section; `docs/INDEX.md`.

---

## 4. Tooling and the verifier checks (gate G-R)

### 4.1 Files

| file | role | reused from |
| --- | --- | --- |
| `scripts/nom_sg/stages.py` | the canonical table of 3.2 and 3.4 (rows by anchor, piece forms, outputs, kept parameters, typed values) | the row style of `scripts/nom_ep1/model.py` (`Seam`) and `nom_td_cut.py` (`R(...)` with pieces) |
| `scripts/nom_sg/declared.py` | the closed list of 3.3, each entry with its reason and a locator that finds its frozen object at any base | the structure of `scripts/nom_ep1/declared.py` |
| `scripts/nom_sg/realize.py` | the crossing-local realization table of 1.4 (local, typed value, member, transform) | EP1's T-B3, T-B5, T-B7 definitions |
| `scripts/nom_sg_cut.py` | the generator: `--survey`, `--stage sg1`, `--stage sg2`, `--stage sg3`, `--check` | `scripts/nom_ep1/textops.py` (edits, Doxygen blocks, write_target, line ends), `gen_seams.py` (binding-line and Doxygen emitters: `decl_doc`, `fwd_lines`), `nom_td_cut.py` (marked regions, the manifest) |
| `scripts/nom_sg_verify.py` | the verifier: S1 to S14 and the controls | below |
| `scripts/nom_epoch_lint.py` | one new check, L-SG | EP1's lint |

All reused names exist at `nom/ep1-tools` tip `f800032` (the EP1a-0 review round; `nom_ep1_verify.py`
is 4 824 lines there) and at main for the TD and RT scripts (checked by name).
`scripts/nom_cpp.py` supplies the reading layer (lexer, `statements`, `parse_declaration`,
`split_params` with the W/R/V/F parameter kinds, `FileIndex` with data members, `Scopes`,
`calls_in`, `Resolver`, `GitTree`). From `scripts/nom_ep1_verify.py` the verifier imports the binding
canon (`Canon`, `frozen_canon`, `normalize`, `apply_declarator`), the stray-edit scan of V1e, the
event-list builder of V6 (calls, log calls with level and format literal, brackets, returns),
`include_graph` and `reach` (V9), `_preflight` (V13), `find_defs`, `struct_members`, `doc_before`,
`params_doc`, and the control harness (`Control`, `run_controls`). From `scripts/nom_td_verify.py`:
the frozen-line coverage accounting (`expected_lines`, `compare`) and `events()`; from
`scripts/rt_seam_verify.py`: `strip_comments`. The frozen source is read from git at `--base`.
`scratchpad/nextsteps/sg2/check_sg1.py` is the prototype of S1, S1b and the name half of S3.

### 4.2 Checks (exit 0 only when all pass; comments and blank lines ignored)

- **S1 Coverage.** Every frozen code line of `networkSolveGlue` and `networkClockDatumDeclaration`
  at `B_SG` is carried exactly once by one new body (at its row's indent delta) or is the old side of
  one declared entry.
- **S1b Block forms.** For each new function, the running brace depth over its pieces in order
  starts at 0, never goes below 0 and ends at 0; each piece's scanned net and minimum match its
  declared form (3.1); a `close_of` piece's brace matches, in the frozen source, the opener its
  anchor names; every generated statement between two pieces sits at the depth the earlier piece
  left.
- **S2 Bodies.** Each new function's body below its preamble equals its rows' pieces code line for
  code line; the only other lines are the declared ones (calls in place of EXTRACTs, EXIT_RETURNs,
  EXIT_TEST, the generated tail).
- **S3 Bindings.** For every stage and helper, every free name its body reads resolves, in the new
  source, to the storage object it denoted in the frozen solve: through the preamble, the parameter,
  the orchestrator's argument, to either a solve parameter (identity; through EP1's spine binding to
  the epoch or the run) or a typed value member that realizes a frozen local (1.4), or, in the sigma0
  tail, the object a `SolveSkipFacts` pointer holds (the address of the orchestrator's `datum.plan`
  and of its `est_index_of` parameter). A reference stays a reference; a copy only where the frozen
  name was a copy; `const` may be added. This is the check card WCF's defect fails.
- **S4 Realization.** Each realized member: type text equals the local's; default equals the frozen
  initializer (T-B5) or the frozen initializer is stored at the frozen position (T-B7); its first
  read in stage order follows its first write.
- **S5 Order.** The ordered event list of the orchestrator, with every stage and helper call
  expanded in place, equals the frozen solve's: every call by callee (every `est.add*`, `est.solve`,
  `est.beginEpoch`, `est.contribution*` included, so the fold order is proved), every log call with
  level and format literal, every `emitEvent` with its tier and event literals, every `prmRespond`,
  every trace, status or H write, every return. Exit paths are compared through S6.
- **S6 Exits.** For each `kSolveSkips` row, the effect list `networkSolveSkip` produces (rendered
  from the row: the counter, the trace stream template with the flag folded, the status-row field
  assignments, the H fields and the sigma0 expression, the tail, the `prev_*` advance) equals the
  frozen tail's (3.2, R21 and the four EXIT_RETURN sites), modulo COUNTER_ORDER and VALUE_EQUAL;
  the row's disposition literal is the frozen literal; the enum order equals the row order.
- **S7 Homes.** The three run-end reports and `networkClockDatumDeclaration` are byte-identical at
  their new homes, defined and declared once; the run-end table changed only in three home strings
  and its includes; `tests/test_network_run_end.cpp` changed only in its include; the historical TD
  tooling records the new homes (decision 8.7).
- **S8 Documentation.** Each new declaration's Doxygen has `@brief`, one `@param` per parameter
  saying read or written, `@return`; each stage's Doxygen names the typed values it reads and writes.
- **S9 Names.** EP1's V9 (no new overload visible to a frozen call) and V13 (no free `run`, `ep` in a
  moved body other than the parameters; the fallback names where a body declares them) on every new
  function.
- **S10 The replay chain.** The spine passes `ep.ws`, `ep.assembly.est_index_of`, `ep.dd_build` to
  the solve (EP1's V3a, after EP1c); the orchestrator passes its own parameters to
  `networkWithinEpochAdapt` unchanged; inside it the replay (SG1: the lambda's by-reference capture;
  SG2: `weReplay`'s parameters) calls `networkBuildCarryDdPriors(run, ep, dd_build)` with that same
  `dd_build`; the DD build reads the funnel as `ep.funnel` (const after EP1c) and the solve neither
  receives nor writes it; and `ws.we_culprits` written by the T2 sets is the object
  `ep.ws.we_culprits` the DD build reads. The WCF writer-end counter sits immediately before the call.
- **S11 Availability and one access path.** No stage or helper binds `ep.solve`, `ep.ar_fixes`,
  `ep.fixed_cycles`, `ep.prep` or `ep.emitted` (not written until the solve returns; EP1's V10
  machinery), and none binds `ep.ws`, `ep.assembly.est_index_of` or `ep.dd_build` (the solve writes
  them; inside it they are reached only through its parameters).
- **S12 Stateless.** `NetworkSbasis` has no non-static data member and the three methods are `const`
  (FileIndex), the obligation of FRESH_STATELESS.
- **S13 Stray edits.** Every hunk of `git diff <B_SG>` lies in a generated region, a new file, an
  include or forward declaration, or a declared hand edit (EP1's V1e).
- **S14 The knob.** `NetworkKnobs::retro_feed`'s initializer equals the frozen initializer with the
  alias-to-field substitution; the knob is not in the snapshot fingerprint's field list (no
  fingerprint move).

**L-SG (permanent, `nom_epoch_lint.py`).** The orchestrator's stage calls, in order, equal the
banner table of `NetworkSolveGlue.hpp`; each exit the orchestrator takes names a `SolveSkip`
enumerator; every enumerator has one `kSolveSkips` row; no stage in the table binds `ep.ws`,
`ep.assembly.est_index_of` or `ep.dd_build`. Run with the seam contract's lint.

**Unit tests (permanent).** `tests/test_network_solve_skip.cpp` (new, suite `NetworkSolveSkipTable`):
one row per enumerator in order; the disposition literals; GateWithheld trace flag 0 and
`WithheldGate`; Sigma0Withheld flag 1, `WithheldSigma0` and the tail; DatumFailed, SolveFailed and
WeRestoreFailed with no H line and no trace row (pins finding 3 until the author decides). A
`retro_feed` case in `NetworkKnobsEpochDriver` (`tests/test_network_knobs_epoch_driver.cpp`). SG2:
`WeChooseCulprit` cases. The roster helpers' and the continuity targets' tests need stations with
state managers and an estimator; they are written with EP2's `tests/support/NetworkTestRun.hpp`
fixture (EP1 spec 3.6), or in SG1 if the fixture exists by then.

### 4.3 Negative controls (pre-registered; each must fail its named check first)

| id | mutation | must fail |
| --- | --- | --- |
| C1 | `networkWithinEpochAdapt` takes `EpochWorkspace ws` by value | S3, S10 (card WCF's class) |
| C2 | the replay's DD rebuild passes a local `EpochDdBuild` | S10 |
| C3 | `networkPreGatePriors` called after `networkSbasisCompletion` | S5 |
| C4 | `networkFoldCodeBiasKaulaPrior` called after the clock rows | S5 (fold order) |
| C5 | a stage binds `ep.solve.plan` instead of `datum.plan` | S11 |
| C6 | GateWithheld's trace flag set to 1 | S6 |
| C7 | the sigma0 tail erases before the recorder dump | S6 |
| C8 | `datum_changed` bound to a stage-local copy written after the verdict | S3 or S4 |
| C9 | a data member added to `NetworkSbasis` | S12 |
| C10 | a one-token edit inside a moved body | S2 |
| C11 | GateWithheld's H sigma0 reads `sigma0_adapt` without the 0.0 default | S6 |
| C12 | the reduce-once check moved before the restore exit | S5 |
| C13 | `we_sigma0_pool_c` bound by value in `networkSigma0Ceiling` | S3 (declarator) |
| C14 | WeRestoreFailed's row given the T2 meter columns | S6 |
| C15 | the WCF `resolves` counter line dropped from the replay | S2, S10 |
| C16 | `networkCensusSigma0` called after `networkRankGate` | S5 |
| C17 | `networkCensusSigma0` binds `const std::vector<int>& est_index_of = ep.assembly.est_index_of;` instead of taking the parameter | S11 |
| C18 | row R11c (the guard's closing brace) dropped from the table | S1, S1b |
| C19 | `SolveSkipFacts::of` stores the address of a copy of the plan | S3 |
| C20 | R04 declared `whole` instead of `open` | S1b |

Positive control: the verifier with the frozen tree as candidate reports every check n/a except S13,
which passes; the generator on `B_SG` twice writes nothing the second time (`--check`).

---

## 5. Gate arms (G-EP: `scripts/nom_td_gate.py --whole`)

Same-build Release, renamed executables (`gnss_processor_sgref.exe` built at `B_SG`,
`gnss_processor_sg1.exe` etc.), both sides under `RAPID_RUNS_ROOT` on D:. Each new arm is first run on
the reference alone and counts as coverage only when its evidence column is non-zero (the inventory's
rule). Arm directories `runs/_rap_sg_{ref,sg1,sg2,sg3}_<arm>`. 32 arms in 31 rows (`snapw` and
`snapr` share one).

**The A4 arms rest on two measured facts** (1.6): the loader disables a ceiling at or below 1.0
(`GnssConfigLoadNetwork.cpp:7505-7512`, 12612-12620), and on the field golden the T2 sigma0 is its
floor 1.0 on every assessed epoch (234 of 234, `runs/_rap_g2_ep0_f2`). A ceiling "above 1.0 and below
the settled sigma0" therefore does not exist on the golden, so `A4`, `A4h` and `era` raise the floor
instead (`network.raim_sigma0_floor`, no upper bound in the loader, 7367-7368): with the floor at 1.6
and the ceiling at 1.5, every assessed epoch whose pool is non-empty (all 234 on `f2`) has sigma0 at
least 1.6 and is withheld, with no calibration; the floor stays below the T2 trigger (3.0), so T2
assesses without adapting. With the ceiling now armed and above 1.0, the era knobs
survive the interlocks at `GnssConfigLoadNetwork.cpp:851-861` (and the erase's `relevel_free_epoch`
interlock at 863-867, satisfied by the golden, 236). `A4c` needs a sigma0 that varies across the
ceiling by constellation, so it lowers the floor and tightens the phase weight, and is calibrated.

| arm | posture (from `rapid_arm.py`, golden-based unless named) | reaches in the solve | evidence (required before the arm counts) |
| --- | --- | --- | --- |
| `f2` | field golden 2 h | every stage on its normal path: W6 rows on the `none` posture (`golden_field_refound.json:928`) with the GFC birth prior (951), pin-mode election with the sticky pivot (329) and the scoped handover (347), the estimability dump (270), SBC evaluated (341), the gate, the D2 dense fold (305), the Q3 guard (317), T2 assessed (227), the A4 ceiling at 6.0 (228), the card-1d checksum | W6 first-N 6 lines, checksum 20 lines |
| `s2` | sim golden 2 h from 00:30:15 | the same on the sim | |
| `zero` | `f2`, start offset 30 h | the solve is never called (exit 1) | exit code 1 |
| `snapw`, `snapr` | as G1/G2 | the solve after a snapshot write and a warm resume | snapshot bytes |
| `k3` | `f2` 1 h, `raim_sigma0_trigger` 0.5, `raim_k_sigma` 3.0 (RPD's `frc`) | T2 on every assessed epoch: assess, arc and station culprits, the replay's scalar coupling refloat and carry-prior subset, the shared re-fold incl. the dense capture, the undo snapshot, settle, PRM T2 response | T2 run-end: re-solves, arcs, stations > 0 |
| `k15` | as `k3`, `raim_k_sigma` 1.5 | more culprits; the consensus escalation if `raim_min_sta` stations clear | satellite culprits > 0, else covered by `cul` |
| `cul` | EP1's: `joint` with injected `sat_clock_step` and `rx_clock_step` | satellite and station culprits | per-kind counts |
| `joint` | NOM2's smoke (`runs/_rap_n13s_joint`): trigger 0.9, level vehicle, injected 5-cycle slip (its joint-whiten key is ignored after R1) | T2 with isolated-arc culprits | re-measured counts |
| `weci` | `k3` + `raim_we_code_iono` true, `raim_we_code_trigger` 1.0, `raim_we_iono_trigger` 1.0 | the WE-CI column mask, pools, trips, station exclusion and code de-weight, the replay's code-row de-weight | `we_ci_sta_excl`, `we_ci_sat_dw` > 0 |
| `gfr` | NOM2's `gf` smoke (`runs/_rap_n13x_gf`: level vehicle, coupling off, `clock_anchor_gauge_free` true) + `k3` knobs | finding 2's branch: the replay's gauge-free fold of every captured prior and of the dense capture | re-solves > 0 |
| `plain` | NOM2's `replay` smoke (`runs/_rap_n13s_replay`: coupling off) + `k3` | the replay's plain re-add and the diagonal carry-prior subset | re-solves > 0 |
| `shxr` | NOM2's `shx` smoke (shared history, clock clones) + `k3` | the clone and shared-history replay folds | re-solves > 0 on clone epochs |
| `f3r` | `k3` + `phase_leveling_local_block` true (the key survives the deletion; only its `_young_mode` and `_min_age_ep` companions are in `remove_rows.json`) | the local-block re-whiten in the replay | `n_pl_blk_refloat_rewhiten` + `_fail` > 0 |
| `ciw` | `wcff` + `raim_we_code_iono` true and low code/iono triggers | the only route to a T2 replay on `carry_invariant` (the phase pool is empty there, card WCF's correction): the replay's direct DD rebuild with a non-empty carry | re-solves > 0 on that arm |
| `wcff` | as G1/G2 (retired golden: `carry_invariant` 136, eq34 135, SH 131, KP 802, SBC 820) | the eq34 declaration branch, the SH prior, the KP fold and margin, the carry-invariant DD build | KP margin line |
| `A4` | `f2` + `raim_sigma0_floor` 1.6 + `raim_withhold_sigma0` 1.5 | the sigma0 exit on every assessed epoch, trace flag 1, status row with the T2 meters, H line with the census sigma0 (`h_sigma0_prefit_census` 313), the recorder dump (`rhs_attrib_on_trip` 269) | `n_sigma0_withheld` > 0; `logs/rhs_attrib/` compared |
| `A4h` | `A4` + `h_sigma0_prefit_census` false | the H line's `sigma0_adapt` branch (1.6 on every withheld epoch) | `n_sigma0_withheld` > 0; H lines |
| `era` | `A4` + `raim_withhold_adapt_escalate` 6 (above the stock budget 5, as the loader requires, 7523-7531) + `raim_withhold_free_erase` true | the era's enter edge, the escalated budget on every later era epoch (exhausted: the floor keeps sigma0 above the ceiling), the tail's free erase | `n_sigma0_withheld` > 0, `n_wh_era_entries` > 0, `n_wh_escalated_epochs` > 0, `n_wh_free_erases` > 0. The era's exit edges are not reached on this posture (no assessed epoch clears a floored sigma0); they count on `A4c` |
| `A4c` | `f2` + `raim_sigma0_floor` 0.01 + the phase weight tightened (`filter.measurement_noise.phase_std_m`, 0.002 m in the golden, line 184) until the smoke's settled sigma0 lies between about 1.2 and 2.5 + `per_const_withhold` true, `per_const_withhold_min_rows` 5 + `raim_withhold_adapt_escalate` 6, `raim_withhold_free_erase` true; `raim_withhold_sigma0` (above 1.0) placed between the GPS and Galileo sigma0_c read on a 0.25 h smoke of the reference at a trial ceiling of 1.01 (the RTA-10 line prints sigma0_c per constellation) | the carve-out decision and its line, withheld and kept epochs alternating, the era's exit edges (`perconst_published`, `cleared`) | carve-out lines > 0; `n_wh_era_exits` > 0 for the exit edges. If the smoke shows no constellation separation, the carve-out is named unreached and keeps its unit tests (`tests/test_network_estimator.cpp`, `decidePerConstWithhold`) and S1 to S6 |
| `gate` | EP1's: `f2` + `rank_gate_tol` 1e-3, then 1e-2 | the gate exit, DG6 over and under, the heal and re-gate, the SBC refusal (cap) | `n_gate_withheld` > 0, `phase_heal` events |
| `sbc` | `f2` + `rank_gate_tol` 1e-6 (SBC counts extras at the same tolerance, `post:902-905`) | SBC engage, release, the capture; with `k3` knobs also its replay re-fold | `n_sbc_epochs` > 0 |
| `kp` | `f2` + `cb_kaula_prior` true | the KP fold and capture, the margin | margin line, `n_kp_epochs` > 0 |
| `isc` | `f2` + `sat_isc_state` true | the ISC roster and verdicts | `n_isc_observed` + `n_isc_pinned` > 0 |
| `xband` | NOM2's `tri` (three bands) + `sat_gf_extra_band_free` true | the extra-band roster and the held keys | `n_extra_band_*` > 0 |
| `hlt` | `f2` + `sat_health` "pivot" | the HLT ineligible set in the election (card GAD keeps E14 and E18, flagged unhealthy) | `hlt_unhealthy_registered` > 0 |
| `s2z` | `f2` + `clock_datum_sum_to_zero` true | the sum-to-zero declaration and clock rows (and their replay re-fold with `k3` knobs) | `clock_constraint_rows` > 0 |
| `brdc` | a broadcast-anchored posture (`clock_datum_broadcast_anchored` true on the broadcast a priori) | the broadcast-anchored declaration and rows; the cold anchor's skip | rows > 0 |
| `anchor` | `f2` + `apriori_clock_anchor_sigma_m` 0.05 | the cold anchor fold and capture | `cold_clock_anchor` contributions > 0 |
| `hand3` | EP1's `hand` (`clock_monitor_couple_raim`, `ura_retro_bump`) with the `k15` knobs | T2's couple and retro-feed inserts into the hand-off (`post:2308-2314`), read by the stash | refloat counts with a satellite culprit |
| `legacy` | EP1's: `phase_datum_prioritized` false with a clock-datum station outage | T2's legacy-pivot skip in the assessment, the undo's legacy-pivot rule | |
| `census` | EP1's (every measure-only instrument; with `trace_estimability_budget` raised) | the dump on more epochs, the DG hand-off | |

**What no arm reaches, named.** The DatumFailed exit (`buildSharedDatum` throws only on an invalid
broadcast anchor sigma, which the loader refuses: EP1 spec 6), the SolveFailed exit (`est.solve`
throws only on a logic error), a replay re-solve failure, the undo's station drop and the
WeRestoreFailed exit (RPD section 7.4: reached only with an uncommitted injector), the reduce-once
error line (`post:2366-2370`, a defect check), and the withhold era's exit edges and the carve-out
unless `A4c` shows its counts. Each has static coverage by S1 to S6 and the table test; decision 8.4
offers a paired injector build. Branches an arm above fails to show are added to this list at the gate
with their evidence. NOM3 section 5's week-rollover arm (EP1's `roll`) is not needed for SG: the
solve reads the week-aware `dt` from `ep.clock` and only writes `prev_week` / `prev_tow` at its exits;
finding 4's TOW-only step is in the driver's DD-carry predict (`ApplicationNetwork.cpp:2466-2467`),
outside the solve. EP1's `fc`, `raw` and `cil` lose their postures with the deletion (FC-1, the lane
carry), and none of them reached a solve branch the arms above do not.

**Cost.** An idle-machine ABBA pair on both 2 h goldens per sub-step, within 1 %. Expected nil: 14
calls per epoch across translation units, no new allocation (the typed values are the frozen
locals), moves where the frozen code moved.

---

## 6. Work split and order

**Order.** The REMOVE-key deletion lands on main (tracks I, K, L, R1, R2 with their WIP review
rounds finished, then C30 and C31). EP1 lands on it (EP1a-1, EP1b with W2 owning
`NetworkSolveGlue.*`, EP1c). Then SG1, SG2, SG3 in sequence. SG cannot start its code before EP1 has
merged: EP1 rewrites the solve's signature, its preamble, the replay call and the epilogue, and SG's
frozen base is EP1's output. SG0, the tooling (scripts only), can start now on top of
`nom/ep1-tools` (tip `f800032`), surveying a scratch merge of the five track tips with the I/K
epilogue conflict resolved as in section 0.

**File ownership while SG is open.**

| step | owns | shared, sequenced |
| --- | --- | --- |
| SG1 | `NetworkSolveGlue.{hpp,cpp}`; new `NetworkCodeBiasDatum`, `NetworkClockDatum`, `NetworkRankGate`, `NetworkSbasisCompletion`, `NetworkRaimWithinEpoch` (`.hpp` and `.cpp`); `NetworkPrefitCensus.*`, `NetworkBiasAdmission.*`, `NetworkClockAnchor.*`, `NetworkDatumGraph.*`; the report moves out of `NetworkEstimator.*`, `NetworkRaim.*`, `NetworkSbasis.*`; `NetworkRunEnd.cpp` (three strings, includes); `NetworkHandoverStransform.cpp` (one include); `tests/test_network_solve_skip.cpp`; `tests/test_network_run_end.cpp` (one include); `scripts/nom_td_manifest.json` and `scripts/nom_td_cut.py` (the three homes, decision 7) | `CMakeLists.txt` (one line per file, with PB and HV); `NetworkKnobs.cpp` and `NetworkRunContext.hpp` (the knob: two lines; HV1 edits the same header, rebase by regeneration); `tests/test_network_knobs_epoch_driver.cpp` |
| SG2 | `NetworkRaimWithinEpoch.*`; `tests/test_network_we_choice.cpp` (new) | none |
| SG3 | `NetworkRhsAttrib.*`, `NetworkContributions.cpp`, `NetworkResponses.cpp`, the KP margin comment (decision 13), CLAUDE.md, the card | EP3's observer table names `networkRhsAttribTrip(run, ep)`, which keeps that form |

**Relative to the other steps.**
- **PB1** (`ApplicationNetworkAssembly.cpp`, PASS B's parameter groups and cut) is disjoint from SG and
  runs in parallel. PB's proposed shared install predicate (`stationPriorInstalls`, used by PASS B
  and the replay) lands after SG2, in the replay's new home (decision 8.10).
- **HV1** (the harvest) is disjoint except `NetworkRunContext.hpp`; SG's knob lands first or HV1
  rebases its generated edits.
- **EP2 to EP4** own `ApplicationNetworkEpoch.cpp` and setup; SG touches neither (the solve's spine line
  and its signature stay EP1's; the trip's observer form is kept).
- **TD2** (one run-end report per subsystem) must treat the within-epoch tier as its own subsystem
  with its report in `NetworkRaimWithinEpoch.cpp`, or merge into it; it should not pull the report back
  into `NetworkRaim.cpp`.
- **N3b** (the W6 rosters onto `SignalRoster`) waits for SG1, whose two roster helpers are the
  natural seam.
- One worker per sub-step suffices (one function in, 26 new functions out; EP1's four-worker split
  answered 62 seams in many files). Review on two lenses as EP0 was (fidelity and binding).

---

## 7. Deviations from NOM3 section 3's SG text, each with its reason

1. **Parameters.** The card says 29 to about 8; EP1 already takes the solve to 5 (after EP1c), so SG
   keeps EP1's signature. Stage functions take 1 to 9 parameters; the largest is T2's, whose outputs
   are what its replay rewrites.
2. **What is gone.** The SH-field health gate verdict (the inventory's stage 18) is deleted (I
   `620c2b6`), so no stage hosts it; the ensemble-gauge rebuild in the replay is deleted (K
   `dcbc6cf`), so EP1's replay has one direct builder call, not two; the family-GF census, the basis
   hold's arguments and the joint re-whiten branch are deleted (R1). SG is sized on what remains.
3. **SBC's home** is a new `NetworkSbasisCompletion.cpp` with its run-end report moved there, not
   `NetworkSbasis.cpp`, which is 2 660 lines after R1, above the 2 400-line bar of step TD.
4. **The estimability dump** goes to `NetworkRankGate.cpp`, not `NetworkFrameDiag.cpp`: it describes
   the system the gate judges at the gate's tolerance (DG2), and its rationale comment is the gate's.
5. **The fold-order stages** (`networkPreGatePriors`, `networkPostGatePriors`) and the primary solve stay
   in `NetworkSolveGlue.cpp`: the order of the folds is the solve's concern, and the folds themselves
   stay in their seams (`NetworkBorderPrior.cpp`, `NetworkRelevel.cpp`, `NetworkHandoverStransform.cpp`).
6. **"T2 with its own counters"** is read as its own file, report and Doxygen; the `we_*` counters stay
   in `NetworkServiceCounters` (a sub-struct move is HV1's kind of field move, decision 8.8).
7. **The typed values do not replace `NetworkSolveGlueResult`'s fields.** EP1 binds the post-solve
   seams to those fields (27 call sites read `ep.solve.*` in its Appendix A at `a453448`, fewer after
   the deletion); the orchestrator's epilogue moves the stage values into them (decision 8.3).
8. **`retro_feed` becomes a knob** (EP0's rule for configuration-derived values); not in the card.
9. **The Q3 guard** is called through a one-call stage (`networkClockAnchorGuardEpoch`), because its
   explicit form has a test caller (`tests/test_clock_anchor.cpp`).
10. **Three sub-steps** instead of one: SG1 (cut and table), SG2 (T2's internals), SG3 (the trip's form
    and the printed text). Each has one kind of proof; SG3 is the only one with a declared output move.
11. **The exits' bookkeeping runs from the orchestrator**, the detecting stage keeping its own log line
    and event; the sigma0 exit's recorder dump and free erase are the row's tail.
12. **No gate verdict type.** The gate's counts never leave `networkRankGate` (1.4), so the stage
    returns `EpochAction` like the other exit stages, and only the frozen `gate_ok` decides it.

---

## 8. Decisions for the author (defaults in parentheses, used by the tooling's dry run)

1. **Finding 3, the three silent exits.** DatumFailed, SolveFailed and WeRestoreFailed write no H line
   and no trace row, and WeRestoreFailed lacks the T2 meters Sigma0Withheld carries. Keep (SG is
   identical output and pins the table in a test), or give them an H line (a new state token, or
   `WithheldGate`) and the trace row in a card of their own with a declared output move. (Keep in SG;
   a follow-up card if wanted.)
2. **Finding 2, the fold tape.** Make the replay re-apply every shared fold in the primary's order and
   form (removes the gauge-free asymmetry on the Q3 gauge-free arming and the last-bit order
   difference on every replay). It moves output on adapted epochs. (Not in SG; its own card after SG2,
   whose `weRefoldShared` is the place.)
3. **Typed values in the result.** Keep `NetworkSolveGlueResult` flat with the epilogue, or nest it
   (`result.datum`, `result.we`, `result.ceiling`) and regenerate EP1's bindings at every post-solve
   call site that reads `ep.solve.*` (27 at `a453448`). (Flat.)
4. **Unreachable branches.** Name DatumFailed, SolveFailed, the re-solve failure, the undo and
   WeRestoreFailed unreached with static coverage, or build card RPD's environment-gated injector into
   both the reference and the candidate (an uncommitted, identical patch on both sides) to reach them
   on a gate arm. (Static coverage; the paired injector if its patch is recoverable.)
5. **SBC's home.** New `NetworkSbasisCompletion.cpp`, or `NetworkSbasis.cpp`. (New file.)
6. **SG1 with or without the exit table.** One step (the table in SG1), or an intermediate commit that
   keeps the exits verbatim inside their stages and adds the table after (one generator flag).
   (One step.)
7. **The historical TD tooling.** Record the three moved reports' new homes in
   `scripts/nom_td_manifest.json` (232, 265, 327) and in `scripts/nom_td_cut.py`'s rows (463-480)
   beside the deletion's C30 entries, or retire `nom_td_cut.py` / `nom_td_verify.py` at their last
   passing commit (PLAN Q9). (Record both.)
8. **T2's counters.** Leave the `we_*` fields in `NetworkServiceCounters`, or group them as a
   `WithinEpochCounters` sub-struct (touches the run-end report and every writer). (Leave; HV1 or a
   later field-move step.)
9. **`kMaxAdapt`'s two roles.** Hoist as one constant (identical), or name the adaptation budget and the
   undo's station-drop budget apart (the same value; text only). (Hoist; rename in SG2 if wanted.)
10. **The replay's install predicate.** Unify with PASS B's (`ApplicationNetworkAssembly.cpp:5971`)
    with the T2-from-`carry_warm` decoupling fix, as PB proposes, or before it (value-equal today
    because T2 runs only under `carry_warm`, `post:1457`). (With the decoupling fix.)
11. **The printed citations.** Move the three strings that name `NetworkSolveGlue.cpp` (SG3, a declared
    text move), or leave them stale. (Move.)
12. **The RHS trip's explicit form.** Retire it in SG3 with the `...At` form, or keep EP1's adapter.
    (Retire.)
13. **The KP margin comment** (`post:807-809`, 1.5). Correct it in SG3 to say the margin is taken
    before the SBC rows and equals the gate's system only without SBC rows and without a clone view
    (comment only, no output), or keep it verbatim. (Correct.)

The card's section 5 gates apply as frozen: G-EP whole run on the arms above, G-R as section 4, the
suite, the cost pair. This design's counts are re-measured by `nom_sg_cut.py --survey` on `B_SG` before
the SG1 code commit, as EP1's were.

---

## 9. The review of 2026-10-14: each defect checked and disposed

All ten were confirmed against the code; none was rejected.

| # | defect | check | fixed in |
| --- | --- | --- | --- |
| 1 | The cut split two statements: R01 lacked `post:200` (the census `if`'s closing brace) and no row carried `post:634` (the W6 guard's closing brace); first/last-statement anchors could not express pieces that leave a block open | `post:196` opens at col 4 and closes at 200; `post:287` opens the guard at col 0, `post:634` closes it; `post:435` opens the `cbd` block that R07 leaves open and R09 closes at 539. Brace scan of the old rows: R01 +1, R04+R07+R09 +1 | 3.1 (piece forms, `close_of`, the function-level rule, S1b), 3.2 (R01 to 200, R11c added, every split row's form stated), controls C18, C20; `check_sg1.py`: all 24 bodies balanced, no uncovered line |
| 2 | Verbatim bodies read names their signatures could not bind: `plan` in the sigma0 tail (`post:2563`) and the DG hand-off (`post:929`), `est_index_of` in the census (`post:198`) and the tail | read at `post`; `est_index_of` is a solve output, not one of EP1's 21 binding lines | `networkDatumGraphSpectralHandoff` takes `const NetworkSbasis::Plan& plan`; `SolveSkipFacts` holds `plan` and `est_index_of` pointers bound by the tail's preamble; the census takes `est_index_of` as a kept parameter; the one-access-path rule (S11, control C17); a name check over every body (`check_sg1.py`) |
| 3 | `A4` (0.99) is disabled by the loader, so `A4`, `A4h`, `era` never reach the exit | `GnssConfigLoadNetwork.cpp:7505-7512` and 12612-12620 at main; the era interlocks 851-861. Further measured: on `f2` the T2 sigma0 is 1.0 on 234 of 234 assessed epochs, so a ceiling in (1.0, settled sigma0) does not exist on the golden | section 5: `A4`, `A4h`, `era` raise the floor to 1.6 with the ceiling at 1.5; `A4c` lowers the floor, tightens the phase weight and is calibrated; every arm's required counts stated, including `n_wh_escalated_epochs` and `n_wh_free_erases` for `era` |
| 4 | SG2's T5 split the try/catch and held the `break` out of the adaptation loop; T4's nine locals had no rebinding rule | `post:2150` opens the catch, 2225 closes it, 2224 is `break;` for the loop at 2027; T4's names read after it are `kind`, `best_arc_sta`, `best_arc_sat`, `best_sat`, `best_sta` | T5 is `post:2151-2223` (CATCH_BODY: header, call, `break;` and brace at the call site, `ex` a parameter); T4 returns `WeChoice` and the call site REBINDs the five names; EXTRACT now refuses a range with an outward jump (`jumps.py`) |
| 5 | `B_SG` includes EP1c, which removes `funnel`, but the signatures, banner, S10 and R30 kept it | EP1 spec section 7: EP1c makes the DD build's funnel const and the solve loses its `funnel` output | every signature, the orchestrator, the banner, S10, R30 and the 1.2 S25d row written against the post-EP1c form |
| 6 | Two helpers took `const NetworkRun&` | EP1 deviation 1 and `NetworkRun.hpp` ("never const"); the first-N log's `NetworkEstimator& est = run.engines.est;` needs a non-const run | `networkClockModelWarm` and `networkCodeBiasDatumFirstNLog` take `NetworkRun&` |
| 7 | Moving `networkWithinEpochRaimRunEnd` breaks `tests/test_network_run_end.cpp`; `nom_td_cut.py` still names the old homes | the test includes `filter/NetworkRaim.hpp` (30) only for that report (95); `nom_td_cut.py` 463-480 | 2.2, 3.2, S7, the SG1 ownership row, decision 7 |
| 8 | Counts did not reproduce | the section 5 table has 31 rows and 32 arms; 26 new functions (14 + 8 + 4); `n_arc_refloat_epoch` occurs only at `post:1381` and 2602; the tooling tip is `f800032` | 2 (function count), 5 (arm count), 1.4 (the hand-off rows split), 0 and 6 (tip) |
| 9 | "each applied cleanly" was false: I and K conflict in the epilogue | `git merge-file` of K onto main+I reports one conflict at the epilogue; `post.cpp` equals the hand resolution (`sg2/post_resolved.cpp`) | section 0, 1.2 S34, section 6, and the risks |
| 10 | The banner said stage 6 measures the gate's system and omitted the GFC birth rows | SBC rows fold at `post:986`, after the margin at 815; the gate may judge the clone view (`post:1046-1049`); the birth rows fold at `post:560` | the banner's order constraints; finding "new, minor (2)" in 1.5 and decision 13 for the frozen comment that makes the same claim |
