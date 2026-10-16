# NOM3 step PB1: the station assembly as stages and a declared step table (buildable design)

Read-only design for card NOM3 step PB (`docs/refound/NOM3_network_giants_2026-10-09.md`, section 3 "PB", gates of section 5, order of section 6). No repository file was changed, nothing was built or run. The method is card NOM2 step TD (sections 6 to 8): parameter groups first, then a generated character-exact cut walked by a declared table, verified by a relocation verifier and gated on identical output.

**Bases and measurements.**
- Main `31c393f` ("main" below). Every "main N" line is `src/core/app/ApplicationNetworkAssembly.cpp` at that commit unless another file is named.
- The REMOVE-key deletion is not on main. Its five track branches (`rmk/I`, `rmk/K`, `rmk/L`, `rmk/R1`, `rmk/R2`) all branch from `31c393f` after the shared commit `a621b16` (C1). Their versions of the assembly file were merged three-way onto main in the scratchpad (`nextsteps/pb1/asm_merged.cpp`; one conflict, two adjacent fold blocks each deleted by a different track, resolved by deleting both). "Merge N" lines refer to that file. Tracks I and R1 end in commits marked "WIP (ungated)" (`6923a8e`, `45e93a7`), so every count is re-measured on the post-deletion main before the build.
- Every deleted or edited main line of the function is attributed to its commit by `nextsteps/pb1/attrib.py` (per-commit `git diff -U0` with a line map; output `attrib_asm.json`). Sections, sizes and alias counts: `sections_pb1.py`, `aliases_pb1.py` in the same folder.
- EP1 is specified (`ep1design/EP1_spec.md`, pre-registered in card NOM3) and its tooling is on branch `nom/ep1-tools` (`768aee3` plus uncommitted edits, worktree `wt_ep1t`). PB1's frozen base `<B>` is the main that carries the deletion and EP1. No row below carries a line number for the generator: rows name anchors, and the survey resolves them at `<B>` (EP1's rule).

**Revision of 2026-10-14 (review round).** Eight defects were checked against the code and all eight confirmed; none was rejected. (1) Arms `ionob`, `zwdb` and `rxclk` sat on the float vehicle, where the loader forces `network.phase_leveling_local_block` off (`GnssConfigLoadNetwork.cpp` 12868-12878) before the ZWD, ionosphere and receiver-clock in-block interlocks test that raw knob (13580-13586, 13670-13677, 13805-13814; the same order on `rmk/I` and `rmk/R1`), so steps c2 and c3 and the ZWD block column were unreachable on them; `zseam` added the fingerprinted `network.carry_zwd` (`NetworkSnapshotFingerprint.cpp` 266) and so cold-started; `l5b` could not reach the diagonal receiver-IFB counters, which `if (rxcb_blk_on) continue;` (merge 4679) skips on every golden-based arm. The arms are rebased (a level-vehicle base `lvb`, `zseam` with the unfingerprinted seam knob only, a new `l5bd`) and section 5 reports the reach that remains, including what no arm reaches. (2) The thread pair is no longer an identity gate over the whole run: it declares its masks, pins Eigen to one thread in both runs' epoch loops, and names the remaining Eigen difference out of scope. (3) `networkAssemblyProduce` takes `(run, ep, gim, assembly)`, binds `se`, the clock and PASS A in its preamble, and is a row of the epoch contract test. (4) The pending coupling-row order is stated as the code builds it. (5) PB-V5 follows the CALLABLE helpers, with a checked route premise for the local block's fallback, so sections 1.6 and 2.5 agree. (6) The const-through-pointer gap is listed in full and PB-V7c refuses any non-const call through a pointer member of the view. (7) The arms that lose part of their posture are corrected (`gf` loses nothing; `shd`, `trc`, `wcff` lose the SH health gate, the EWL and UWL lanes and `carry_invariant_honesty`). (8) `NetworkCoupling.cpp` is sized on the post-deletion tree (904 lines on `rmk/R1`). Found while revising, by the same check: the `gim` arm named `network.w6_iono_constraint` `gim`, a value the loader does not accept (it takes `auto` or `none`, `GnssConfigLoadNetwork.cpp` 3837-3848); the arm now uses card NOM N2a's smoke posture (`auto`, `sat_gf_datum` `none`, the ionosphere weighting on). Section 1.7 gains findings 6 and 7 (what the float vehicle refuses, and that no posture builds all six block-column families).

---

## 0. The design in one page

1. **After EP1, `networkPassB_` already has its parameter groups.** EP1 converts its 23 parameters (22 after the deletion) to `networkPassB_(NetworkRun&, const NetworkEpoch&, EpochPassA& pass_a, EpochAssembly& assembly)` with a generated binding preamble (EP1 spec, Appendix A row 47; fallback names `nrun` / `nep` because the body declares `ep`, main 3574). What PB1 removes is the second preamble below it: 104 alias declarations at main, 81 after the deletion.
2. **PB1a cuts the serial stages.** Seven EP1-form seams, each opening with exactly the frozen alias lines it reads: the SH-deviation aging, the coupling reinit history, the tally, the structural fuse, the consume, the in-solve track and the eq34 audit (672 post-deletion lines, six files, two new). The five knob-only block predicates and `relevel_free_on` become `NetworkKnobs` fields (EP0's method), which also gives the harvest's duplicate derivations one owner.
3. **PB1b cuts the produce loop into two declared tables.** `kStationBuildSteps` (12 top-level steps, each one contribution family, walked in order for one station inside the existing OpenMP loop) and `kStationCarrySteps` (11 steps of the carried-prior section, walked by the `carried_priors` step over one `StationCarryFrame`). 23 steps in 16 family files (10 new), the new ones named once for both PB1b and HV1, so each family file later also holds its harvest half.
4. **The parallel contract is a type.** A top-level step is `StationStep (*)(StationBuild&)`: the station's `NetStation&` and `StationAssembly&` writable, everything else (run context, configuration, estimator, setup, epoch, other stations) through const references. A carry step is `void (*)(StationCarryFrame&)`. The compiler rejects a shared write through any by-value member. Const does not pass through a pointer member: 16 `std::unique_ptr` members of the view (seven per `NetStation`, six in `NetworkSetupResult`, three in `NetworkRunContext`) hand out writable objects. The verifier closes that gap: PB-V7c refuses every write and every non-const call reached through a pointer member, except through the station's own `st`.
5. **Bodies stay character-exact.** Each new function opens with generated bindings (root bindings plus verbatim copies of the frozen alias lines it reads); the body below is the frozen text, shifted. Every edit beyond that is one of 14 declared substitution classes (section 3.3).
6. **The orders that fix the bytes are the table orders.** Row order (data, pins, GLONASS anchors, zero-ionosphere rows, ionosphere ties, SD-EWL, coupling region, LGM rows), diagonal-prior order (ambiguity, oracle pin, ZWD, receiver code bias) and block-column order (ambiguity, ZWD, ionosphere, receiver clock, receiver code bias, phase colour) follow from the table order. The pending coupling-row order follows from it plus one frozen order inside `local_block`: ambiguity rows on the inline route, or, when the block fails, the phase-colour fallback and then the ambiguity fallback; then the wet delay. The verifier proves the table order equal to the frozen piece order and a unit test pins all four.
7. **Gates.** G-PB-R (relocation; reuses `nom_cpp.py`, `nom_td_verify.py` and `nom_ep1_verify.py` machinery, adds 12 checks and 14 negative controls), G-EP whole run on the post-deletion arms plus 15 purpose-built arms (three on a level-vehicle base, since the float vehicle refuses three in-block carries), a thread-count comparison with declared masks on the reference build, an idle ABBA cost pair within 1 %.

---

## 1. The measured state

### 1.1 The function, at main and after the deletion

| quantity | main `31c393f` | after the deletion (merge) | source |
| --- | --- | --- | --- |
| extent | 3038-6119, 3 082 lines (1 797 code) | merge 2849-5440, 2 592 lines (1 531 code) | `sections_pb1.py` |
| parameters | 23 (3038-3055) | 22 (`iono_deweight` gone, `ebf0f9e`) | merge 2849-2865 |
| alias preamble | 3056-3266: 104 declarations (61 knob copies, 16 `ctx.carry`, 8 `ctx.counters`, 5 setup, 3 parameter-registry q, 3 config, 1 trace, 7 derived) | merge 2866-3035: 81 (44 knob, 13 carry, 7 counters, 5 setup, 3 q, 3 config, 1 trace, 5 derived) | `aliases_pb1.py` |
| dead aliases | 5: `pl_on` 3078, `eq34_rpb_prior_sigma` 3237, `phase_hold_sigma` 3240, `phase_hold_on` 3241, `phase_hold_var` 3242 | 2: `pl_on` merge 2888, `eq34_rpb_prior_sigma` merge 3011 | same |
| produce loop | 3489-5536, 2 048 lines; carry section 3997-5450, 1 454 | merge 3198-4885, 1 688; carry section merge 3634-4799, 1 166 | |
| maximum brace depth | 16 (main 4798) | 16 (merge 4318) | |
| exits | one `SkipEpoch` (6107-6116), else `Proceed` (6118); three legacy-pivot aborts set `epoch_ok = false` (5874, 5887, 5927) | same three (merge 5196, 5209, 5248) | |
| station-loop exits in the produce | `continue` on `!present[i]` (3491), empty innovations (3544), H/state mismatch (3545-3549) | merge 3200, 3207, 3211 | every other `continue` binds an inner loop (depth scan) |

The inventory figures (2 997 lines, 27 parameters, 100 aliases) are at `6e39dc4`; since then EP0 replaced four parameters by knobs (main 3061-3066), card L5B added the receiver-IFB re-coupling count (3568-3589) and card ARL the test-only zero-ionosphere rows (3919-3956).

### 1.2 Section map (each region tagged)

"Kept" means the region survives the deletion unchanged in substance; "EP1" means EP1 changes its bindings only (the generated preamble), never its text.

| id | main lines (lines / code) | what | deletion | merge lines | PB1 destination |
| --- | --- | --- | --- | --- | --- |
| S00 | 3038-3055 (18) | signature | `iono_deweight` (`ebf0f9e`, C15) | 2849-2865 | EP1: `(NetworkRun&, const NetworkEpoch&, EpochPassA&, EpochAssembly&)` |
| S01 | 3056-3267 (212 / 112) | alias preamble, `emitEpochStatus` forwarder (3264-3266) | 55 lines: young inflate `4fec9c0` 19, coupling options `74b038e` 9, iono carry `633de7a` 8, LV tools `1c2b093` 6, R1 review `45e93a7` 5, F1 `18ffc0d` 4, ensemble `dcbc6cf` 4 | 2866-3036 | distributed to the functions that read each line (PB1a, PB1b) |
| S02 | 3268-3283 (16 / 2) | banner, D0 timer start | kept | 3037-3052 | stays in `networkPassB_` |
| S03 | 3296-3327 (32 / 20) | slant-iono carry slip edges | deleted whole (`633de7a`, C18) | none | |
| S04 | 3284-3295 comment + 3328-3430 (115 / 71) | SH-deviation carry: slip erase or D5 re-tap, GM aging, staleness, de-mean | adaptive-iono boost 3347-3361 deleted, 3394-3407 edited (`9218340`, C16) | 3053-3139 (87 / 52) | PB1a `networkShDevCarryAge` |
| S05 | 3431-3488 (58 / 20) | coupling reinit history (cards 1d, 1g) | kept | 3140-3197 | PB1a `networkCouplingReinitHistory` |
| S06 | 3489-3495 (7) | `#pragma omp parallel for`, present test, slot, try, station | kept | 3198-3204 | PB1b `networkAssemblyProduce` |
| P01 | 3496-3541 (46) | LV-1 seed frame | deleted whole (`1c2b093`, C19), with `networkShSlantAtLos_` (1231-1264) | none | |
| P02 | 3542-3567 (26 / 14) | measurement update, the two exits, the split (Data* kinds), parameter inventory record | kept | 3205-3230 | `data_rows` |
| P02b | 3568-3589 (22 / 13) | card L5B receiver-IFB re-coupling count (`st.l5b`) | kept | 3231-3252 | `rx_ifb_recouple` |
| P03 | 3590-3632 (43 / 11) | congruence row geometry (`R.row_pos_enu`) | kept | 3253-3295 | `congruence_geometry` |
| P04 | 3633-3651 (19) | ensemble datum columns | deleted whole (`dcbc6cf`, C11) | none | |
| P05 | 3652-3857 (206 / 141) | phase-datum pins: AmbiguityPin, Eq34RpbPin, Eq34RpbRankFloor, Eq34RpbTimeLink | kept | 3296-3501 | `phase_datum_pins` |
| P06 | 3858-3918 (61 / 37) | GLONASS anchors: GloIfbAnchor, GloIonoAnchor | kept | 3502-3562 | `glonass_anchors` |
| P06b | 3919-3956 (38 / 32) | zero-ionosphere rows: SimZeroIono | kept | 3563-3600 | `sim_zero_iono` |
| P07 | 3957-3982 (26 / 12) | ShIonoTie or GimIono | deweight-gate arguments 3969-3977 (`ebf0f9e`) | 3601-3619 | `iono_constraint` |
| P08 | 3983-3996 (14 / 7) | SdEwl | kept (the SD-EWL keys are not among the 144) | 3620-3633 | `sd_ewl` |
| P09 | 3997-4016 (20 / 3) | `legacy_local_inject` | `|| iono_on` (4016, `633de7a`) | 3634-3653 | `carried_priors` frame (PURE_DECL_MOVE) |
| P10 | 4017-4049 (33 / 17) | RAIM checkable-column census (`R.qc_amb`) | kept | 3654-3686 | `raim_columns` |
| P11 | 4050-4106 (57 / 19) | frame gate, `x_lin`, `inj`, `inj_prior`, `CouplingRowBuilder cb` | `iono_on` disjunct 4062, census sets 4070-4077 (`633de7a`); builder initializer 4086 (`74b038e`) | 3687-3735 | `carried_priors` frame |
| P12 | 4107-4210 (104 / 45) | `do_amb_carry`, three lambdas, `PlPend`, elevations | F1 text and route 4121-4144 (`18ffc0d`); `PlPend::var_add` 4195-4201 (`4fec9c0`) | 3736-3813 | `ambiguity` (lambdas to functions, `PlPend` to a type, `do_amb_carry` to the frame) |
| P13 | 4211-4375 (165 / 90) | ambiguity carry walk (`R.pl_amb`; PriorAmbiguityCarry, CouplingScalar, or deferred) | iono census inserts (`633de7a`), inflate 4333-4372 (`4fec9c0`), F1 deferral 4355-4360 (`18ffc0d`) | 3814-3947 | `ambiguity` |
| P14 | 4376-4436 (61 / 33) | I-in-block columns (`R.pl_iono`) | comment (`18ffc0d`) | 3948-4006 | `slant_iono_block` |
| P15 | 4437-4553 (117 / 62) | RX-CLK-in-block columns, drift-predicted copies | comment, `var_add` initializer | 4007-4120 | `rx_clock_block` |
| P16 | 4554-4618 (65 / 39) | RXCB-in-block columns | `var_add` initializer 4609 | 4121-4185 | `rx_cb_block` |
| P17 | 4619-4661 (43 / 34) | LGM phase-colour block columns, GM-predicted copies | kept | 4186-4228 | `phase_colour_block` |
| P18 | 4662-4999 (338 / 199) | F3 local block (Block* kinds), ZWD push, degrade edges, fallbacks | F3 age gate and young legs, F1-scaled fallback: 104 lines (`18ffc0d` 99, `4fec9c0` 5) | 4229-4471 (243 / 139) | `local_block` |
| P19 | 5000-5056 (57 / 34) | oracle pin: PriorAmbiguityOraclePin | kept | 4472-4528 | `oracle_pin` |
| P20 | 5057-5131 (75 / 47) | ZWD B9: PriorZwdCarry or CouplingScalar | kept | 4529-4603 | `zwd` |
| P21 | 5132-5196 (65) | slant-iono diagonal carry: PriorIonoCarry | deleted whole (`633de7a`) | none | |
| P22 | 5197-5368 (172 / 80) | rx-CB walk: PriorRxCbDatum, PriorRxCbDatumPrePd, PriorRxCbCarry | LV-2 and LV-4: 5284-5336 edited, 5357-5366 deleted (`1c2b093`) | 4604-4717 (114 / 58) | `rx_cb` |
| P23 | 5369-5374 (6) | coupling region append | comment (`74b038e`) | 4718-4723 | `coupling_region` |
| P24 | 5375-5450 (76 / 55) | sort, coalesce and copy out, twice | comment | 4724-4799 | `prior_finalize` |
| P25 | 5451-5504 (54 / 45) | LGM rows: PhaseColourBirth, PhaseColourBackstop | kept | 4800-4853 | `phase_colour_rows` |
| P26 | 5505-5536 (32 / 23) | Z-row capture; `R.ok`; catch | kept | 4854-4885 | `zwd_capture`; `R.ok` and the catch stay in the produce |
| S07 | 5537-5727 (191 / 128) | serial tally (43 fold lines and 10 `prmRespond` calls after the deletion), F3 fallback warning, coupling fold, CTU add, SD-EWL trace drain | iono, LOS-scale and young folds (27 lines: `633de7a`, `18ffc0d`, `4fec9c0`) | 4886-5049 (164 / 108) | PB1a `networkAssemblyTally` |
| S08 | 5728-5750 (23 / 14) | structural fuse | kept | 5050-5072 | PB1a `networkPrefitFuseEpoch` |
| S09-S11 | 5751-5992 (242) | PD-drop naming lambda, consume, PD roll-up | `n_cpl_joint_fallback` count 5897 (`74b038e`) | 5073-5313 (241) | PB1a `networkAssemblyConsume` |
| S12 | 5993-6040 (48 / 36) | in-solve count, re-admission events | kept | 5314-5361 | PB1a `networkSolveStationsTrack` |
| S13 | 6041-6091 (51 / 38) | eq34 chain-pin audit, seed-round targets | kept | 5362-5412 | PB1a `networkEq34PinAudit` |
| S14-S16 | 6092-6119 (28) | D0 close, contributions drain, skip tail | kept | 5413-5440 | stay in `networkPassB_` |

### 1.3 What the deletion removes from PASS B, by commit

544 main lines of the function are deleted or edited (54 replacement lines), net 3 082 to 2 592.

| commit | track, plan | removes | main lines |
| --- | --- | --- | --- |
| `18ffc0d` | R1, C22 (F1, F3 age gate) | alias `pl_los_scale` (3082); F1 route and text (4121-4144, 4355-4360); the F3 age gate and young legs (4685-4741, 4941-4967); the F1-scaled fallback (4970-4992); the LOS-scale and young folds (5601-5608, 5641) | 154 |
| `633de7a` | I, C18 (diagonal slant-iono carry) | `iono_*` aliases (3191-3195, 3248); S03; the `iono_on` disjuncts (4016, 4062); the census sets (4070-4077, 4233, 4261, 4324); P21; the fold (5595-5600) | 125 |
| `1c2b093` | I, C19 (LV-1, LV-2, LV-5, LV-4, S2H) | LV aliases (3201-3202, 3209-3212); P01 and `networkShSlantAtLos_`; the de-mean and step gate of P22 | 112 |
| `4fec9c0` | R1, C21 (young-arc inflate) | `yng`, `yng_on`, `yng_dt_ep` (3157-3175); `PlPend::var_add`; the inflate in P13; seven `var_add` initializers; the young fold (5679-5689) | 73 |
| `9218340` | I, C16 (adaptive iono q) | the boost in S04 | 29 |
| `dcbc6cf` | K, C11 (ensemble datum) | `phase_datum_ensemble`, `phase_hold_*` aliases; P04 | 23 |
| `74b038e` | R1, C23 (coupling options) | `carry_cpl_joint`, `carry_cpl_shmarg` and the `!carry_cpl_joint` term of `pl_blk_on` (3109-3116, 3144); `n_cpl_joint_fallback` (3255, 5897); the builder initializer (4086) | 13 |
| `ebf0f9e` | I, C15 (deweight gate) | parameter `iono_deweight` (3043); the SH tie's gate arguments (3969-3977) | 10 |
| `45e93a7` | R1 review round (WIP) | preamble comments (3084, 3087, 3145-3147) | 5 |

Not in PASS B: the lane layer (tracks L) lived in the DD build and the install; PASS B never built a lane row. The SD-EWL rows (`network.sd_ewl.*`) are not among the 144 keys (`rmk/remove_rows.json`) and stay; track L touches only four lines of their builder (`337e69e` 1, `8440ca7` 3, in `networkAppendSdEwlRows_` 543-1125). The SH health gate (C14) and the PB emit filter (C12) do not reach this function.

### 1.4 What EP1 changes

EP1 (spec row 47) converts the parameters to bindings: `se`, `ctx`, `setup`, `traces`, `est`, the two funnel sets, `carry_warm`, `pl_now_s`, `modeled` and `present` (written, `pass_a`), the columns' `shared_phase_keys`, `registered_clock_sats`, `registered_code_keys`, `n_clock`, `n_pb`, `n_present`, and `asm_res`, `est_index_of`, `total_pins`, `sec_asm_ms_epoch` (written, `assembly`), `sec_asm_wall_ms` (`run.timing.asm_wall_ms`). The driver sizes `ep.assembly.asm_res` and `est_index_of` before the call (spec 4.2). The definition uses `nrun` / `nep` because the L5B block declares `const long long ep` (main 3574). EP1c tightens `modeled` to const if it compiles (PASS B only reads it). EP1 changes no text below its preamble.

### 1.5 Writes to run state

Write scan over the merge (assignments, increments, mutating calls on `ctx.*`, plus the aliases):

| stage | writes |
| --- | --- |
| S04 | `ctx.carry.sh_dev_carry` (erase, aging), `shdev_slip_resets_total`, `shdev_d5_keeps_total`, the three de-mean totals (merge 3065-3139) |
| S05 | `ctx.carry.tu_reinit_hist` through its alias (merge 3140-3197) |
| produce | nothing in `ctx`, `est`, `traces` or `setup`; per station only `asm_res[i]` and `stations[i]` (its state manager through the measurement pre-pass, `st.l5b`, `st.sd_ewl_ctr`) |
| S07 | 39 named `ctx.counters` fields plus the coupling-term aliases, `prm_resp` (10 sites), `ctx.param_inventory` (drain), `ctx.carry.oracle_pin` totals, `traces.sd_ewl_trace` |
| S08 | `R.w` of each station (through `networkPrefitFuseStation`), `ctx.counters.pc` |
| S09-S11 | the estimator's normal equations, `est_index_of`, `total_pins`, `present[i]`, `pd_drop_*`, `pd_drop_pivot_abort`, `mgnss.nongps_only_pd_drops`, TOPOLOGY events |
| S12 | `sta_solve_track`, `sta_readmit_*`, `sta_in_solve_*` (through `cnt`), TOPOLOGY events |
| S13 | `eq34_pin_audit` (through `pa`), `ctx.carry.eq34_prev_target`, `seed_round_*` |
| S14-S16 | `sec_asm_ms_epoch`, `timing.asm_wall_ms`, `ctx.contributions`, `n_skipped`, the trace row, the epoch-status row, `prev_week` / `prev_tow` |

### 1.6 Station contribution kinds

`ContributionKind` has 60 kinds at main (57 at card NOM N2a; `include/filter/NetworkContributionKind.hpp` 42-113) and 53 after the deletion (`PriorIonoCarry` by `633de7a`; `PhaseHold`, `EnsemblePhaseGauge` by `dcbc6cf`; `CouplingJoint` by `74b038e`; `CbGfPin`, `CbFamilyGf`, `CbIfLevel` by `d5d2c7a`). The station kinds go from 31 to 29. Each is tagged where it is created (a `RowTag` of kind, satellite and signal, aligned with the rows; a diagonal prior carries its tag in `carry_tags` / `prior_tags`):

| kinds | created at (main) | PB1b step |
| --- | --- | --- |
| DataCode, DataCodeCompanion, DataPhase, DataPhaseSecond | the split: `tagDataRows` (308), 491, 529 | `data_rows` |
| AmbiguityPin, Eq34RpbPin, Eq34RpbRankFloor, Eq34RpbTimeLink | 3702, 3705, 3739, 3798, 3821 | `phase_datum_pins` |
| GloIfbAnchor, GloIonoAnchor | 3889, 3897 | `glonass_anchors` |
| SimZeroIono | 3937 | `sim_zero_iono` |
| ShIonoTie, GimIono | the SH builder (1479), the GIM builder (1203) | `iono_constraint` |
| SdEwl | the SD-EWL builder (1054) | `sd_ewl` |
| PriorAmbiguityCarry | `pl_emit_row`'s diagonal route (4177, `carry_cpl_cond` off) | `ambiguity` (inline route only) |
| CouplingScalar (pending row) | `pl_emit_row`'s coupling route (`buildCplRow`, `NetworkCoupling.cpp` 278); the phase-colour fallback's `buildCplRow` | `ambiguity` (inline route, `carry_cpl_cond` on), `local_block` (the two fallbacks after a failed block; always the coupling route, because the fallback walks `pl_pend`, which only the `pl_blk_on` branch fills, and `pl_blk_on` implies `carry_cpl_cond`) |
| Block{Ambiguity,Zwd,Iono,RxClock,RxCb,PhaseColour}Carry | `buildLocalBlock` (`NetworkCoupling.cpp` 166-171) | `local_block` (columns from `ambiguity`, `slant_iono_block`, `rx_clock_block`, `rx_cb_block`, `phase_colour_block`) |
| CouplingScalar, Block* rows appended | `appendCouplingRows` (`NetworkCoupling.cpp` 811-812) | `coupling_region` |
| PriorAmbiguityOraclePin | 5052 | `oracle_pin` |
| PriorZwdCarry, CouplingScalar | 5100, 5089 | `zwd` |
| PriorRxCbDatum, PriorRxCbDatumPrePd, PriorRxCbCarry | 5265, 5262, 5354 | `rx_cb` |
| PhaseColourBirth, PhaseColourBackstop | 5484, 5481 | `phase_colour_rows` |

### 1.7 Found while mapping

1. **The block predicates have two derivations.** PASS B derives `pl_blk_on = pl_local_block && carry_cpl_cond && !carry_cpl_joint` (main 3114) and the four in-block predicates from it (3148, 3156, 3186, 3208); the harvest derives the same four without the `!carry_cpl_joint` term (`src/core/app/NetworkLocalCarryHarvest.cpp` 203, 208, 213, 223). They agree today only because the loader disables the in-block knobs under joint whitening and refuses the receiver code-bias one (`src/core/config/GnssConfigLoadNetwork.cpp` 13587-13589, 13678-13679, 13814-13816, 13067); after `74b038e` they agree textually. PB1a gives them one owner (section 2.3).
2. **`R.pl_zwd` / `R.pl_zwd_present` have three writers** (the block's ZWD push 4788-4789, B9 5073-5074, the capture 5523-5525), with identical values by construction. Named for PB2.
3. **Const is shallow through a pointer member.** Through a const reference, a `std::unique_ptr` member still hands out a writable object, so the const view leaves 16 of them open: every station's `sm`, `model`, `mm`, `mw_slip`, `iono_var`, `turbo_edit`, `if_slip` (`include/filter/NetworkRunTypes.hpp` 55, 56, 57, 70, 79, 141, 147; reached through `setup.stations[j]`), the setup's `sync`, `brdc_apriori`, `sh_model`, `sh_model2`, `product_writer`, `consumer` (`include/app/NetworkSetupResult.hpp` 69, 83, 112, 116, 138, 157), and the run context's `ctx.carry.slip_oracle`, `oracle_pin`, `rhs_prev_frame` (`include/app/NetworkRunContext.hpp` 1916, 1926, 3110). The view types hold no raw pointer member and no `std::function`; the one `std::shared_ptr` reached (`LocalCarry::blk`, `NetworkRunTypes.hpp` 621) points to const. A non-const call such as `setup.consumer->x()` or `ctx.carry.slip_oracle->x()` would compile inside a step; the verifier refuses it (PB-V7c). On the merge every call the produce makes through a pointer member is const or goes through the station's own `st` (the GIM's `isLoaded` and `slantIonoDelay`, the SH model's `basis`, `numCoeffs`, `coeffs`; `st.sm` getters and `st.mm->computeMeasurementUpdate`), and the oracle-pin table is bound as a pointer to const (merge 2912-2914), so the check passes on the frozen text.
6. **On the float vehicle three in-block carries cannot be armed.** The loader forces the raw `network.phase_leveling_local_block` off under `ambiguity_vehicle` `float_kf` (`GnssConfigLoadNetwork.cpp` 12868-12878), and the later interlocks of `carry_zwd_in_block`, `carry_iono_in_block` and `carry_rx_clock_in_block` test that raw knob and disable themselves (13580-13586, 13670-13677, 13805-13814; the same order on `rmk/I`, the forcing at 12415 before the three interlocks at 12875-13110, and on `rmk/R1`, 12331 before 13006-13191). The receiver code-bias interlock accepts the vehicle (13074) and card LGM requires it (13098-13110). `NetworkKnobs.cpp` 130, 160, 165 snapshot the disabled values. So on both goldens steps `slant_iono_block` and `rx_clock_block` and the ZWD column of `local_block` are unreachable; they run only on the level vehicle with the F3 knob. Whether the three interlocks should accept the vehicle, as the receiver code-bias one does, is a behaviour question for the author, outside PB1 (identical output); it is named for the card.
7. **No posture builds all six block-column families.** Phase colour requires the float vehicle (finding 6), and the ZWD, ionosphere and receiver-clock columns require the level vehicle with F3. The six-family column order is therefore proved by the verifier (PB-V4) and the unit test; the runs exercise two subsets (`lgm`: ambiguity, receiver code bias, phase colour; `ionob`, `zwdb`, `rxclk`: ambiguity, one of the three, receiver code bias).
4. **Printed text names the moved code.** About 31 string-literal lines at main name `ApplicationNetworkAssembly.cpp` or a moved helper as a code site: the contributions builder column (`NetworkContributions.cpp` 68-77, 149, 155, 159) and the parameter-responses "where" column (`NetworkResponses.cpp` 48-126, ten lines after C18) print on every run; `NetworkParamRegistry.cpp` 323 prints in the parameters table; the rest only on postures (`GnssConfigLoadNetwork.cpp` 1773, 5896, 12846, 14043; `NetworkMdb.cpp` 438; `NetworkSignalIntegrity.cpp` 546; `NetworkGrossLos.cpp` 1304). Literals that say only "PASS B" stay true.
5. **The parallel contract holds** on the merge: the produce range writes no run state (section 1.5); the carry maps are read only through const accessors (`find`, `count`, `lower_bound`), and `operator[]` would not compile on the const view.

### 1.8 Duplicated idioms (PB2's list, re-measured after the deletion)

| idiom | sites (merge) |
| --- | --- |
| post-update copies of the station state vector | six: 3311 (`piv_x`), 3570 (`zi_xlin`), 3670 (`x_lin_qc`), 3700 (`x_lin`), 4810 (`x_lin_pc`), 4867 (`x_lin_z`); the seventh (3206) is the pre-update argument |
| epoch spacing with the plausibility test | two: 3974, 4057 (the third, `yng_dt_ep`, deleted by `4fec9c0`) |
| sort and coalesce | twice in P24 (4724-4799) |
| grow the block by n rows | nine: 3484, 3548, 3586, 4839; the SD-EWL (987, rollback 1036), GIM (1176) and SH (1339) builders; `NetworkCoupling.cpp` 797, 853 (main) |
| L⁻¹ whitening | three: 1030, 1194, 1380 |
| the gauge-pin predicate | the member (merge 92-98), the byte copy in `NetworkDdFrac.cpp` 59-66, inline tests (`NetworkEmitStore.cpp` 191, `NetworkOraclePin.cpp` 419, 514) |
| the consume's install gate | copied in the replay, `NetworkSolveGlue.cpp` 2034, without the oracle-pin disjunct (documented 2021-2033) |
| vestigial records | `pl_iono_blk_cols`, `pl_rxclk_blk_cols`, `pl_rxcb_blk_cols` (`NetworkRunTypes.hpp` 1511, 1458, 1472); readers only `tests/test_carry_iono_in_block.cpp` 312 and `tests/test_carry_rx_clock_in_block.cpp` 337 |
| the skip tail | S16 and the column registration's skip (main 3024-3032) |
| the tally's hand folds | 43 fold lines and 10 response calls in S07 |

---

## 2. The new types and functions

### 2.1 PB1a: the serial stages (EP1 seam form)

Each is `R networkX(NetworkRun& run, const NetworkEpoch& ep[, kept][, outputs])`, opening with a generated preamble: the root bindings EP1's generator emits (frozen declarators, `// written` on writes), then verbatim copies of the frozen alias lines its body reads. Reads of the assembly a stage does not write go through `ep.assembly` (the same object PASS B's `assembly` output writes; within PASS B the stages run in the order below, so each reads only what an earlier stage wrote).

```cpp
// include/filter/NetworkShDevVtec.hpp
/**
 * @brief Ages the SH-deviation carry once per epoch, before PASS B's parallel produce
 *        reads it (T5v2; Iono-2 v3 de-mean; v3k erase-or-re-tap split).
 *
 * Every (station, satellite) in this epoch's slip funnel is erased (a raw-confirmed slip)
 * or re-tapped to steady state (a D5-only entry under network.sh_dev_carry_d5_keep); every
 * entry is then GM-aged to this epoch and dropped past 5 tau; the de-mean removes the
 * information-weighted common mode.  No-op unless NetworkKnobs::sh_dev_on.
 * @param run The run: writes ctx.carry.sh_dev_carry and its shdev_* totals.
 * @param ep  The epoch, read: se (the time), funnel (carry_slipped_ambs, carry_slipped_raw).
 */
void networkShDevCarryAge(NetworkRun& run, const NetworkEpoch& ep);

// include/filter/NetworkCoupling.hpp
/**
 * @brief Records this epoch's reinit-history entries the coupling rows marginalize on
 *        (cards 1d and 1g): the inflate-q stash flags, the clock refloats, and under 1g the
 *        anchor handovers, eq34 re-references and datum-slip re-realizations.
 * @param run The run: writes ctx.carry.tu_reinit_hist.
 * @param ep  The epoch, read: clock.pl_now_s.
 */
void networkCouplingReinitHistory(NetworkRun& run, const NetworkEpoch& ep);

// include/filter/NetworkStationBuild.hpp (new in PB1a; PB1b adds the tables)
/**
 * @brief Folds the per-station tallies of the produce into the run counters, serially and in
 *        station order (the parameter inventory drain, the response table, the oracle pin,
 *        the inject-side stale skips, the F3 block telemetry and its non-SPD warning, the
 *        LGM edges, the coupling-term census with the resume seam, card CTU, the SD-EWL
 *        trace).  Step PB2 splits it by family.
 * @param run The run: writes ctx.counters, ctx.param_inventory, the oracle-pin totals,
 *            traces.sd_ewl_trace.
 * @param ep  The epoch, read: se, pass_a (present), columns (the registered keys), assembly.
 */
void networkAssemblyTally(NetworkRun& run, const NetworkEpoch& ep);

// include/filter/NetworkPrefitCensus.hpp
/**
 * @brief The structural fuse over every assembled station (SPEC_prefit_census_observability
 *        section 4): the last writer of each data row's weight.  No-op unless
 *        network.prefit_fuse_sigma > 0.
 * @param assembly [in/out] the station blocks; writes R.w.
 */
void networkPrefitFuseEpoch(NetworkRun& run, const NetworkEpoch& ep, EpochAssembly& assembly);

// include/filter/NetworkSolveStations.hpp (new; beside app/NetworkSolveStationsRunEnd.cpp)
/**
 * @brief Folds every assembled station into the estimator in station order: the empty,
 *        mismatch and failed dispositions, addStation with the pre-PD prior, the carried
 *        diagonal prior, the PD-rejection naming (card RF-PD) and its per-epoch roll-up.
 * @param pass_a   [in/out] present: a dropped station is cleared.
 * @param assembly [in/out] est_index_of, total_pins.
 * @return false when the legacy pivot's block was refused (the epoch is skipped).
 */
bool networkAssemblyConsume(NetworkRun& run, const NetworkEpoch& ep,
                            EpochPassA& pass_a, EpochAssembly& assembly);
/**
 * @brief Counts the stations that entered the solve and marks every return (invariant 7's
 *        re-coupling edge, card RF-PD-fix).  No-op on an aborted epoch.
 * @param epoch_ok The consume's verdict (kept: an orchestrator value, not an epoch member).
 */
void networkSolveStationsTrack(NetworkRun& run, const NetworkEpoch& ep, bool epoch_ok);

// include/filter/NetworkChainDatum.hpp
/**
 * @brief The eq34 chain-pin audit: declared, folded, landed and short pins per group, and
 *        the seed-round targets of accepted stations for the next election.  No-op unless
 *        NetworkKnobs::phase_datum_eq34 and the epoch was not aborted.
 */
void networkEq34PinAudit(NetworkRun& run, const NetworkEpoch& ep, bool epoch_ok);
```

### 2.2 `networkPassB_` after PB1a and after PB1b

After PB1a the produce loop is still inline (with the alias lines it reads); after PB1b `networkPassB_` is the orchestrator below (about 60 lines). It stays an `Application` member because it is the one place that reads `gim_` (decision 4). With the L5B block gone from its body, its definition drops the fallback names (V13, preamble only).

```cpp
EpochAction Application::networkPassB_(NetworkRun& run, const NetworkEpoch& ep,
                                       EpochPassA& pass_a, EpochAssembly& assembly) {
    // ---- generated bindings: se, ctx, traces, est, n_clock, n_pb, n_present, total_pins
    //      (written), sec_asm_ms_epoch (written), sec_asm_wall_ms, asm_res, est_index_of;
    //      the frozen alias lines sec_timers_on, n_skipped, prev_tow, prev_week, trace and the
    //      emitEpochStatus forwarder ----
    // --- PASS B: per-station assembly, then serial scatter ---   [frozen banner, S02]
    std::chrono::steady_clock::time_point sec_asm_t0;
    if (sec_timers_on) sec_asm_t0 = std::chrono::steady_clock::now();
    networkShDevCarryAge(run, ep);                       // S04
    networkCouplingReinitHistory(run, ep);               // S05
    networkAssemblyProduce(run, ep, gim_, assembly);     // S06, P02-P26 (PB1b)
    networkAssemblyTally(run, ep);                       // S07
    networkPrefitFuseEpoch(run, ep, assembly);           // S08
    const bool epoch_ok = networkAssemblyConsume(run, ep, pass_a, assembly);   // S09-S11
    networkSolveStationsTrack(run, ep, epoch_ok);        // S12
    networkEq34PinAudit(run, ep, epoch_ok);              // S13
    /* S14 D0 close, S15 contributions drain, S16 skip tail: frozen text */
    return EpochAction::Proceed;
}
```

### 2.3 PB1a: the knob predicates

Six `NetworkKnobs` fields computed in `NetworkKnobs::snapshot` with the frozen expressions (post-deletion), each with Doxygen naming its readers: `pl_blk_on` (`pl_local_block && carry_cpl_cond`), `zwd_blk_on`, `iono_blk_on`, `rxclk_blk_on`, `rxcb_blk_on` (each `<knob> && pl_blk_on`, `rxcb_blk_on` also `&& rx_cb_on`), `relevel_free_on` (`config.networkCycleSlipRelevelFreeEpoch()`). PASS B's alias lines bind the same names (`const bool pl_blk_on = ctx.knobs.pl_blk_on;`), so no body text changes (EP0 precedent). The harvest's four derivations (finding 1) switch to the knobs in the same commit (decision 6). Test: suite `NetworkKnobsStationAssembly` in a new file, each knob against its frozen expression on postures that set it both ways and on both goldens (the `NetworkKnobsEpochDriver` pattern).

### 2.4 PB1b: the types (`include/filter/NetworkStationBuild.hpp`)

```cpp
/**
 * @file NetworkStationBuild.hpp
 * @brief One station's block in PASS B, as two declared tables of steps (card NOM3 step PB1b).
 *
 * PASS B builds every present station's block in parallel (networkAssemblyProduce: one OpenMP
 * iteration per station, schedule(dynamic)) and folds the blocks into the estimator serially,
 * in station order (networkAssemblyConsume).  The parallel half is kStationBuildSteps: one
 * row per family of the station's contributions, walked in table order for one station.  The
 * carried-prior family is itself a table, kStationCarrySteps, walked over one StationCarryFrame.
 *
 * The parallel contract, as a type.  A step receives its station writable (NetStation: the
 * measurement pre-pass seeds new states; StationAssembly: the station's own slot) and
 * everything else through const references: the run context (knobs, carry maps, counters),
 * the configuration, the estimator's column layout, the setup, the epoch, every other station.
 * A write to shared state does not compile, except through a pointer member, where const
 * stops: each station's sm, model, mm, mw_slip, iono_var, turbo_edit and if_slip (reached
 * through setup.stations), the setup's sync, brdc_apriori, sh_model, sh_model2,
 * product_writer and consumer, and ctx.carry.slip_oracle, oracle_pin and rhs_prev_frame.
 * scripts/nom_pb1_verify.py check PB-V7c refuses every write and every non-const call
 * reached through one of them, except through this station's own st.
 *
 * The orders a step may not change, because the normal equations sum in them:
 *   - rows: data, phase-datum pins, GLONASS anchors, zero-ionosphere rows, ionosphere ties,
 *     SD-EWL, the coupling region, LGM rows (the table's first-appearance order);
 *   - diagonal priors (StationCarryFrame::inj): ambiguity, oracle pin, wet delay, receiver
 *     code bias, sorted by column with std::sort (not stable), so duplicates coalesce in
 *     insertion order;
 *   - local-block columns: ambiguity, wet delay, ionosphere, receiver clock, receiver code
 *     bias, phase colour (declared as spans in NetworkContributions.cpp kBlk*);
 *   - pending coupling rows (CouplingRowBuilder::crows): the ambiguity rows on the inline
 *     route (no block); or, when the station's block fails to factor, the phase-colour
 *     fallback rows and then the ambiguity fallback rows (both built by local_block, in that
 *     order); then the wet-delay row.  Phase colour and the block exist only under
 *     pl_blk_on, which defers every ambiguity, so the two cases never mix.
 * tests/test_network_station_build.cpp pins all four against the tables.
 *
 * Adding a station contribution: a new step (or an existing family's step) in the family's
 * own file, one row here with the kinds it creates, its kinds' ContributionSpec builder naming
 * the step, and the order constraints above checked by the test.
 */

/// What a top-level step tells the walk.
enum class StationStep : std::uint8_t {
    Next,  ///< Go on to the next step.
    Done,  ///< The station has no usable block this epoch (no innovations, or an H/state
           ///< mismatch); its slot says which, and the consume drops it.
};

/// One station's assembly in PASS B: the station (writable) and a const view of the run.
/// Built by networkAssemblyProduce for one OpenMP iteration and passed to every step.
struct StationBuild {
    // ---- the const view: the run, the epoch, the other stations ----
    const NetworkRunContext&                    ctx;     ///< Knobs, carry maps (find only), counters (read).
    const GnssConfig&                           config;  ///< Bound as config_ in the steps.
    const NetworkEstimator&                     est;     ///< The column layout (frozen after registration).
    const NetworkSetupResult&                   setup;   ///< Stations (the gauge-pin test reads this station only), the SH models.
    const SyncedEpoch&                          se;      ///< The epoch's observations and time.
    const EpochClock&                           clock;   ///< dt, pl_now_s, carry_warm.
    const EpochPassA&                           pass_a;  ///< modeled, present, shared_phase_keys.
    const std::unique_ptr<GlobalIonosphereMap>& gim;     ///< Application::gim_ for the GIM rows (null unless loaded); the GIM step passes it as the builder's gim_.
    // ---- this station, writable ----
    const int        i;   ///< Roster index (the frozen loop variable's type: the carry keys are tuple<int, ...>).
    NetStation&      st;  ///< Its state manager and measurement model.
    StationAssembly& R;   ///< Its slot, asm_res[i].
    // ---- what one step hands a later one ----
    EpochMeasurement em;  ///< The measurement update (data_rows writes, congruence_geometry reads).
};

using StationBuildFn = StationStep (*)(StationBuild& build);
using ContributionKindSet = std::uint64_t;   // in NetworkContributionKind.hpp, with kindBit(k); static_assert(kContributionKinds <= 64)

/// One row of the station build: one family of a station's contributions.
struct StationBuildStep {
    const char*         name;    ///< Short name ("phase_datum_pins").
    const char*         home;    ///< The family's file (its inject here; its harvest half, HV1).
    ContributionKindSet kinds;   ///< The row and prior kinds this step can create; 0 for a record-only step.
    StationBuildFn      build;   ///< Gates itself at its first line.
};
inline constexpr std::size_t kStationBuildStepCount = 12;
extern const std::array<StationBuildStep, kStationBuildStepCount> kStationBuildSteps;

/// An ambiguity column waiting for the station's local block (or its per-column fallback):
/// written by the ambiguity step, read by the local-block step (was a local struct, main 4190).
struct PlPend {
    Eigen::Index col; int amb_idx; const LocalCarry* lc;
    double var_eff;   ///< var + q·gap, before the tail.
    double gap;       ///< Clamped.
    std::string sat, signal;
};

/// The carried-prior section of one station (cards Increment 1, B9, CTU, F3, LGM, RXCB-DATUM,
/// rx-CB carry): the values its steps hand each other.  Built by networkStationCarriedPriors on
/// its own stack when the station carries anything this epoch; never copied.
struct StationCarryFrame {
    StationBuild&            build;                ///< The station and the const view.
    const bool               legacy_local_inject;  ///< The leveling carry injects here (frozen P09).
    const bool               do_amb_carry;         ///< The ambiguity carry injects (frozen P12; off under carry_invariant).
    const Eigen::VectorXd    x_lin;                ///< The station's linearization point (one copy).
    CouplingRowBuilder       cb;                   ///< The coupling rows and the local block (card 1, F3).
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>> inj;        ///< Post-PD diagonal priors.
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>> inj_prior;  ///< Pre-PD datum rows (RF-PD-fix).
    std::vector<PlPend>        pl_pend;            ///< Ambiguity columns for the block.
    std::vector<LocalBlockCol> iono_bc, rxclk_bc, rxcb_bc, pc_bc;  ///< The other families' block columns.
    std::vector<LocalCarry>    rxclk_pred, pc_pred;  ///< The predicted copies those columns point at; each step reserves
                                                     ///< before its first push (frozen text), and no later step pushes.
    std::vector<char>          rxcb_ifb;           ///< Which rxcb_bc columns are receiver IFBs (card L5B).
};
using StationCarryFn = void (*)(StationCarryFrame& carry);
struct StationCarryStep { const char* name; const char* home; ContributionKindSet kinds; StationCarryFn build; };
inline constexpr std::size_t kStationCarryStepCount = 11;
extern const std::array<StationCarryStep, kStationCarryStepCount> kStationCarrySteps;

/**
 * @brief Builds one station's block: walks kStationBuildSteps in order.
 * @return false when a step ended the build (the slot names the cause).
 */
bool networkStationBuild(StationBuild& build);

/**
 * @brief PASS B's parallel produce: one station's block per OpenMP iteration, into its own
 *        slot.  The frozen loop, its present test and its catch; the body is the walk.
 *        An epoch writer in EP1's form (run, ep, kept, outputs), a row of
 *        tests/test_network_epoch_contract.cpp.
 * @param gim      Kept: Application::gim_ (the GIM rows), the one input NetworkRun does not
 *                 hold (decision 4).
 * @param assembly [in/out] asm_res: each iteration writes its own station's slot.
 */
void networkAssemblyProduce(NetworkRun& run, const NetworkEpoch& ep,
                            const std::unique_ptr<GlobalIonosphereMap>& gim,
                            EpochAssembly& assembly);
```

The produce after PB1b. The generated preamble binds every name the body uses (EP1's rule: below the bindings the body never spells `ep.`); the frozen lines are the loop, the present test, the slot, the try and the catch; the new lines are the construction and the walk:

```cpp
void networkAssemblyProduce(NetworkRun& run, const NetworkEpoch& ep,
                            const std::unique_ptr<GlobalIonosphereMap>& gim,
                            EpochAssembly& assembly) {
    // ---- generated bindings (card NOM3 step PB1b) ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    NetworkSetupResult& setup = run.setup;
    NetworkEstimator& est = run.engines.est;
    const SyncedEpoch& se = ep.se;
    const EpochClock& epoch_clock = ep.clock;            // new root name (V13 pre-flight: free in the frozen text)
    const EpochPassA& pass_a = ep.pass_a;                // new root name (same)
    const std::vector<bool>& present = pass_a.present;
    std::vector<StationAssembly>& asm_res = assembly.asm_res;   // written
    auto& stations = setup.stations;                     // frozen alias line
    const int n_sta_total = setup.n_sta_total;           // frozen alias line
    const bool parallel_on = setup.parallel_on;          // frozen alias line
    const int n_threads = setup.n_threads;               // frozen alias line
    // ---- end of bindings ----
    #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
    for (int i = 0; i < n_sta_total; ++i) {
        if (!present[i]) continue;
        StationAssembly& R = asm_res[i];
        try {
            NetStation& st = stations[i];
            StationBuild build{ctx, config_, est, setup, se, epoch_clock, pass_a, gim, i, st, R};
            if (!networkStationBuild(build)) continue;   // empty innovations or an H/state mismatch
            R.ok = true;
        } catch (const std::exception& ex) {
            R.threw = true; R.err = ex.what();
        } catch (...) {
            R.threw = true; R.err = "unknown exception";
        }
    }
}
```

`pass_a` and `present` are read through `ep` (const): the produce writes neither, and within PASS B no stage before it writes them, so the const read sees the object PASS B's `pass_a` output names (the driver passes `ep.pass_a`). The binding declarators follow the frozen ones (EP1 V3a: `present` frozen as the writable PASS B parameter, bound here as a const reference, which CONST_VIEW allows).

`carried_priors` builds the frame from the frozen statements (P09, P11, the P12 predicate), then walks the carry table:

```cpp
StationStep networkStationCarriedPriors(StationBuild& build) {
    /* generated bindings */
    const bool legacy_local_inject =
        carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on
                       || zwd_seam_live);                                    // frozen P09
    if (!(legacy_local_inject || rx_cb_on || rx_cb_datum || opin_on))
        return StationStep::Next;                                           // frozen P11 gate
    StationCarryFrame carry{build, legacy_local_inject,
        /*do_amb_carry=*/(phase_datum_prioritized || !st.is_pivot) && !carry_invariant,  // frozen P12
        /*x_lin=*/st.sm->getStateVector(),                                              // frozen P11
        /*cb=*/CouplingRowBuilder{est, shc, q_clock, q_cb, q_pb, carry_cpl_tu,
                                  ctx.knobs.carry_cpl_gauge, ctx.knobs.carry_cpl_reinit,
                                  tu_reinit_hist, R}};                                  // frozen P11
    CouplingRowBuilder& cb = carry.cb;
    /* frozen: the readmit window and card CTU's epoch inputs on cb */
    for (const StationCarryStep& s : kStationCarrySteps) s.build(carry);
    return StationStep::Next;
}
```

The three P12 lambdas become functions in `filter/NetworkLevelingCarry.cpp` (bodies verbatim under their bindings): `double networkAmbCarryFinishVar(const StationCarryFrame&, double var_eff, const std::string& sat)`, `void networkAmbStateNote(StationCarryFrame&, const std::string& sat, const std::string& signal, double var_eff, AmbFoldRoute route)`, `void networkAmbCarryEmitRow(StationCarryFrame&, Eigen::Index col, int amb_idx, const LocalCarry& lc, double var_eff, double gap_clamped, const std::string& sat = {}, const std::string& signal = {})`. The two steps that call them (`ambiguity`, `local_block`) bind the frozen names to forwarding lambdas with the frozen parameter lists, default arguments included (EP1's CALLABLE precedent), so the call sites are unchanged; PB2 replaces them by direct calls as an explained move.

The four row builders called by the steps become free functions in their family files, with the `Application` members they read as leading parameters under the same names (bodies unchanged): `networkSplitStation(const GnssConfig& config_, ...)`, `networkAppendSdEwlRows(const GnssConfig& config_, ...)`, `networkAppendShIonoRows(const GnssConfig& config_, ...)`, `networkAppendGimIonoRows(const GnssConfig& config_, const std::unique_ptr<GlobalIonosphereMap>& gim_, ...)`, and the gauge-pin predicate `networkIsGaugePinned(...)` (it reads no member). Code-only scan of the merge: `config_` 4, 1, 1, 1 uses and `gim_` 3 (GIM), no other member and no `this`.

### 2.5 The two tables

`kStationBuildSteps` (walked in this order):

| # | name | function | home | kinds | records |
| --- | --- | --- | --- | --- | --- |
| 1 | `data_rows` | `networkStationDataRows` | `filter/NetworkStationSplit` (new; the split moves here) | DataCode, DataCodeCompanion, DataPhase, DataPhaseSecond | `h2local`, `n_data_rows`, `param_inv`; Done on empty innovations or H/state mismatch |
| 2 | `rx_ifb_recouple` | `networkRxIfbRecoupleEdge` | `filter/NetworkRxCodeBiasCarry` (new) | none | `st.l5b.recouple`, `last_est_epoch` |
| 3 | `congruence_geometry` | `networkCongruenceRowGeometry` | `filter/NetworkCongruence` | none | `row_pos_enu` |
| 4 | `phase_datum_pins` | `networkPhaseDatumPinRows` | `filter/NetworkPhaseDatum` | AmbiguityPin, Eq34RpbPin, Eq34RpbRankFloor, Eq34RpbTimeLink | `np`, `pin_audit`, `pin_seed_targets`, `pl_rpb`, `prm_rpb_gap_skip` |
| 5 | `glonass_anchors` | `networkGlonassAnchorRows` | `filter/NetworkStationAnchors` (new) | GloIfbAnchor, GloIonoAnchor | |
| 6 | `sim_zero_iono` | `networkSimZeroIonoRows` | `filter/NetworkStationAnchors` | SimZeroIono | |
| 7 | `iono_constraint` | `networkIonoConstraintRows` | `filter/NetworkIonoConstraintRows` (new; the SH and GIM builders move here) | ShIonoTie, GimIono | `gim_rows` |
| 8 | `sd_ewl` | `networkSdEwlStationRows` | `filter/NetworkSdEwlAr` (the builder moves here) | SdEwl | `sd_ewl_trace`, `sd_ewl_rows`; `st.sd_ewl_ctr` |
| 9 | `raim_columns` | `networkRaimColumnCensus` | `filter/NetworkRaim` | none | `qc_amb` |
| 10 | `carried_priors` | `networkStationCarriedPriors` | `filter/NetworkStationCarry` (new) | the union of the carry table's | the frame |
| 11 | `phase_colour_rows` | `networkPhaseColourRows` | `filter/NetworkPhaseColourCarry` (new) | PhaseColourBirth, PhaseColourBackstop | `pl_pc`, `pc_birth_rows`, `pc_backstop_rows` |
| 12 | `zwd_capture` | `networkZwdRowCapture` | `filter/NetworkZwdCarry` (new) | none | `pl_zwd`, `pl_zwd_present` |

`kStationCarrySteps` (walked by `carried_priors`):

| # | name | function | home | guard (frozen enclosing conditions) | kinds | records |
| --- | --- | --- | --- | --- | --- | --- |
| c1 | `ambiguity` | `networkAmbiguityCarryColumns` | `filter/NetworkLevelingCarry` (new) | `legacy_local_inject`, then the frozen `if (do_amb_carry)` | PriorAmbiguityCarry, CouplingScalar | `pl_amb`, `relevel_free_skips`, `inject_stale_skips`, `prm_amb_*`, `amb_state_fold`; `pl_pend` |
| c2 | `slant_iono_block` | `networkSlantIonoBlockColumns` | `filter/NetworkSlantIonoCarry` (new) | `legacy_local_inject && do_amb_carry` | (BlockIonoCarry, through c6) | `pl_iono`, `pl_blk_iono_gap_drop`, `prm_iono_gap_skip`; `iono_bc` |
| c3 | `rx_clock_block` | `networkRxClockBlockColumns` | `filter/NetworkRxClockCarry` (new) | same | (BlockRxClockCarry) | `pl_rxclk`, gap counters; `rxclk_bc`, `rxclk_pred` |
| c4 | `rx_cb_block` | `networkRxCbBlockColumns` | `filter/NetworkRxCodeBiasCarry` | same | (BlockRxCbCarry) | `pl_rxcb_restored_skip`, `prm_rxcb_gap_skip`; `rxcb_bc`, `rxcb_ifb` |
| c5 | `phase_colour_block` | `networkPhaseColourBlockColumns` | `filter/NetworkPhaseColourCarry` | same | (BlockPhaseColourCarry) | `pc_planned`; `pc_bc`, `pc_pred` |
| c6 | `local_block` | `networkStationLocalBlock` | `filter/NetworkCoupling` (the builder's owner) | same, then its own frozen `if` | Block{Ambiguity,Zwd,Iono,RxClock,RxCb,PhaseColour}Carry, CouplingScalar (the two fallbacks; route `pl_emit_row: coupling`, a checked premise) | `pl_blk_*`, `pl_zwd*`, `pl_cb_injected`, `pc_folded`, `pc_*_folds`, the vestigial `pl_*_blk_cols`; `st.l5b.blk_folds` |
| c7 | `oracle_pin` | `networkOraclePinPriors` | `filter/NetworkOraclePin` | `legacy_local_inject && opin_on` | PriorAmbiguityOraclePin | `opin_*` |
| c8 | `zwd` | `networkZwdCarryPrior` | `filter/NetworkZwdCarry` | `legacy_local_inject && (zwd_on || zwd_seam_live)` | PriorZwdCarry, CouplingScalar | `pl_zwd*`, `pl_blk_zwd_fallback_diag`, `prm_zwd_gap_skip` |
| c9 | `rx_cb` | `networkRxCbPriors` | `filter/NetworkRxCodeBiasCarry` | `rx_cb_on || rx_cb_datum` | PriorRxCbDatum, PriorRxCbDatumPrePd, PriorRxCbCarry | `pl_cb`, `pl_cb_*`, `prm_rxcb_*`; `st.l5b.diag_*` |
| c10 | `coupling_region` | `networkCouplingRegionRows` | `filter/NetworkCoupling` | `carry_cpl_cond` | (appends CouplingScalar and Block* rows) | `cpl_row_start`, the coupling census |
| c11 | `prior_finalize` | `networkStationPriorFinalize` | `filter/NetworkStationCarry` | none (the frozen bare block, unwrapped) | none | `carry_*`, `prior_*`, `carry_merged`, `prior_merged` |

The 23 steps cover the 29 station kinds; CouplingScalar is declared by four steps (built by c1, c6, c8; appended by c10), which the kind sets say.

**Kinds through a CALLABLE helper, and the one route premise.** The ambiguity carry's row builder `networkAmbCarryEmitRow` (the frozen `pl_emit_row`, merge 3779-3799) has two routes: `carry_cpl_cond` on builds a coupling row (`buildCplRow`, CouplingScalar), off emplaces a diagonal prior (PriorAmbiguityCarry, the only kind token in the helper, merge 3786-3787). A step that calls it creates the kinds of both routes unless a declared premise excludes one. `ambiguity` calls it on the inline route under either value of `carry_cpl_cond`, so its set holds both kinds. `local_block` calls it only in the fallback over `pl_pend` (merge 4461-4468), and `pl_pend` is filled only inside `ambiguity`'s `if (pl_blk_on)` branch (merge 3933-3942), with `pl_blk_on = pl_local_block && carry_cpl_cond` (K1); so its call always takes the coupling route and its set holds CouplingScalar, not PriorAmbiguityCarry. The premise is a row of `scripts/nom_pb1/declared.py` (`routes`: `local_block`, `pl_emit_row`, `coupling`) and PB-V5 checks both of its legs; a run cannot show it, since no arm can make a premise false.

Family file sizes after PB1b (post-deletion lines moved, before bindings): `NetworkStationSplit` 426 + 26, `NetworkSdEwlAr` 671 + 583 + 14, `NetworkIonoConstraintRows` 167 + 66 + 19, `NetworkStationAnchors` 99, `NetworkLevelingCarry` 212, `NetworkCoupling` 904 + 57 + 242 + 6 = about 1 209 (904 on `rmk/R1`, the only track that changes it: `74b038e` removes about 210 lines of the joint whitening, SH marginalize and gauge-free closure from main's 1 114; then the reinit history of PB1a, the local block and the coupling region), `NetworkRxCodeBiasCarry` 22 + 65 + 114, the others under 200. No home exceeds 2 400 lines (step TD's bar). `ApplicationNetworkAssembly.cpp` goes from about 5 440 lines after the deletion to about 1 600 (PASS A, the prefilter, the column registration, the PASS B orchestrator). Every size is re-measured at `<B>` by the survey.

### 2.6 What the compiler checks, and what it cannot

- A step writes only `build.st`, `build.R`, `build.em`, its frame and its locals: every other member is a const reference, so an assignment, `operator[]` on a carry map or a non-const call through them does not compile.
- `static_assert`s in `tests/test_network_station_build.cpp`: the const-view members are const references (`std::is_const_v<std::remove_reference_t<decltype(...)>>`), the two function-pointer types are exact, the table sizes equal their counts.
- The order cases in the same file (suite `NetworkStationBuildOrder`): the table orders behind rows, diagonal priors and block columns are asserted against the declared lists of 2.4; the pending coupling-row order is asserted on built rows, in two cases. Inline route: a frame with `carry_cpl_cond` on and the block off, one carried ambiguity and a carried ZWD, walked through `ambiguity` and `zwd`; `cb.crows` tags read (ambiguity, wet delay). Failed block: a frame under `pl_blk_on` with one pending ambiguity, one phase-colour column and a carried ZWD, whose harvested cross covariance makes the block non-SPD, walked through `local_block` and `zwd`; the tags read (phase colour, ambiguity, wet delay).
- Not checked by the compiler (finding 3): writes and non-const calls through the 16 `std::unique_ptr` members reached from the view (seven per station in `setup.stations[j]`, six in `setup`, three in `ctx.carry`). PB-V7c refuses them by a scan of every step body and every moved helper body; the station's own `st` is the one root through which a step may write (its measurement pre-pass seeds `st.sm`).
- The frame's vectors whose elements other vectors point into (`rxclk_pred`, `pc_pred`) keep the frozen reserve-before-push rule; PB-V8 checks that only the step that reserves them pushes to them.

---

## 3. The cut

### 3.1 Row form (for `scripts/nom_pb1_cut.py`, in the style of `nom_td_cut.py`)

```python
S(func, name, home, pieces, kinds=(), table="top" | "carry" | "stage" | "helper",
  guard="auto" | "verbatim" | ("split", "<enclosing conjunction>"),
  exits=(), frame=(), callables=(), member_to_free=(), result=(), header=None,
  doc="<what the function builds, which records it writes>")
```

`pieces` are anchors (a frozen statement's first code text or banner comment) that the survey resolves to line ranges at `<B>` and writes into `scripts/nom_pb1_manifest.json`; the generator refuses an anchor that resolves twice or not at all. Lines below are main (merge) for orientation only.

### 3.2 The rows

**PB1a (stage table).**

| row | function | home | frozen pieces, main (merge) | transforms |
| --- | --- | --- | --- | --- |
| A1 | `networkShDevCarryAge` | `filter/NetworkShDevVtec` | the aging banner 3284-3295 and `if (sh_dev_on) {` 3328-3429 (3053-3139) | GUARD, ALIAS_SPLIT |
| A2 | `networkCouplingReinitHistory` | `filter/NetworkCoupling` | the card 1d/1g banner and two `if`s 3431-3487 (3140-3196) | ALIAS_SPLIT |
| A3 | `networkAssemblyTally` | `filter/NetworkStationBuild` (new) | 5538-5726 (4887-5048) | ALIAS_SPLIT |
| A4 | `networkPrefitFuseEpoch` | `filter/NetworkPrefitCensus` | 5728-5749 (5050-5071) | ALIAS_SPLIT |
| A5 | `networkAssemblyConsume` | `filter/NetworkSolveStations` (new) | 5751-5991 (5073-5312) | ALIAS_SPLIT, RETURN (`return epoch_ok;` appended) |
| A6 | `networkSolveStationsTrack` | `filter/NetworkSolveStations` | 5993-6039 (5314-5360) | GUARD (`if (epoch_ok)`), KEPT (`epoch_ok`) |
| A7 | `networkEq34PinAudit` | `filter/NetworkChainDatum` | 6041-6090 (5362-5411) | GUARD (`phase_datum_eq34 && epoch_ok`), KEPT |
| A8 | `networkPassB_` | `app/ApplicationNetworkAssembly.cpp` | S02, the inline produce, S14-S16 | STAGE_CALL (seven calls at the frozen positions), ALIAS_SPLIT (its preamble keeps only the lines it and the inline produce read) |
| K1 | six `NetworkKnobs` fields | `app/NetworkKnobs.cpp`, `app/NetworkRunContext.hpp` | the derived alias lines (main 3098, 3114, 3148, 3156, 3186, 3208) and the harvest's four (`NetworkLocalCarryHarvest.cpp` 203-223) | KNOB_MOVE |

**PB1b (step tables, helpers, the produce).**

| row | function | table | frozen pieces, main (merge) | transforms |
| --- | --- | --- | --- | --- |
| B0 | `networkAssemblyProduce` | stage, epoch writer `(run, ep, gim, assembly)` | S06 3489-3495, `R.ok` and the catch 5530-5536 (3198-3204, 4879-4885) | WALK, ROOT (`se`, `epoch_clock`, `pass_a`, `present`, `asm_res`), ALIAS_SPLIT (`stations`, `n_sta_total`, `parallel_on`, `n_threads`) |
| B1 | `networkStationBuild` | | new (the walk) | generated |
| T1 | `networkStationDataRows` | top | 3542-3567 (3205-3230) | EXIT ×2, RESULT (`em`), MEMBER_TO_FREE (split) |
| T2 | `networkRxIfbRecoupleEdge` | top | 3568-3589 (3231-3252) | GUARD |
| T3 | `networkCongruenceRowGeometry` | top | 3590-3632 (3253-3295) | GUARD, RESULT read (`const EpochMeasurement& em = build.em;`) |
| T4 | `networkPhaseDatumPinRows` | top | 3652-3857 (3296-3501) | verbatim (a declaration then an `if`) |
| T5 | `networkGlonassAnchorRows` | top | 3858-3918 (3502-3562) | GUARD |
| T6 | `networkSimZeroIonoRows` | top | 3919-3956 (3563-3600) | GUARD |
| T7 | `networkIonoConstraintRows` | top | 3957-3982 (3601-3619) | MEMBER_TO_FREE (SH, GIM) |
| T8 | `networkSdEwlStationRows` | top | 3983-3996 (3620-3633) | GUARD, MEMBER_TO_FREE |
| T9 | `networkRaimColumnCensus` | top | 4017-4049 (3654-3686) | GUARD, MEMBER_TO_FREE (gauge pin) |
| T10 | `networkStationCarriedPriors` | top | 3997-4016 and 4050-4106, the P12 predicate 4119-4120, the frame's close 5450 (3634-3653, 3687-3735, 3748-3749, 4799) | PURE_DECL_MOVE ×2, GUARD, FRAME |
| T11 | `networkPhaseColourRows` | top | 5451-5504 (4800-4853) | GUARD |
| T12 | `networkZwdRowCapture` | top | 5505-5529 (4854-4878) | GUARD |
| C1 | `networkAmbiguityCarryColumns` | carry | 4107-4210 less the lambdas, `PlPend` and the predicate; 4211-4375 (3736-3947) | GUARD, SPLIT_GUARD (its `if (do_amb_carry)` closes after P13), FRAME, CALLABLE, TYPE_MOVE |
| C1a-c | the three ambiguity-carry functions | helper | the lambdas 4153-4157, 4163-4168, 4169-4189 (3763-3799) | CALLABLE |
| C2 | `networkSlantIonoBlockColumns` | carry | 4376-4436 (3948-4006) | SPLIT_GUARD, FRAME (`iono_bc`) |
| C3 | `networkRxClockBlockColumns` | carry | 4437-4553 (4007-4120) | SPLIT_GUARD, FRAME (`rxclk_bc`, `rxclk_pred`) |
| C4 | `networkRxCbBlockColumns` | carry | 4554-4618 (4121-4185) | SPLIT_GUARD, FRAME (`rxcb_bc`, `rxcb_ifb`) |
| C5 | `networkPhaseColourBlockColumns` | carry | 4619-4661 (4186-4228) | SPLIT_GUARD, FRAME (`pc_bc`, `pc_pred`) |
| C6 | `networkStationLocalBlock` | carry | 4662-4998 (4229-4470) | SPLIT_GUARD, GUARD, CALLABLE |
| C7 | `networkOraclePinPriors` | carry | 5000-5056 (4472-4528) | SPLIT_GUARD, GUARD |
| C8 | `networkZwdCarryPrior` | carry | 5057-5130 (4529-4602) | SPLIT_GUARD, GUARD |
| C9 | `networkRxCbPriors` | carry | 5197-5368 (4604-4717) | GUARD |
| C10 | `networkCouplingRegionRows` | carry | 5369-5374 (4718-4723) | GUARD |
| C11 | `networkStationPriorFinalize` | carry | 5375-5449 (4724-4798) | UNWRAP |
| H1-H5 | `networkIsGaugePinned`, `networkSplitStation`, `networkAppendSdEwlRows`, `networkAppendShIonoRows`, `networkAppendGimIonoRows` | helper | main 96-102, 111-536, 543-1125, 1284-1482, 1142-1207 | MEMBER_TO_FREE (declarations leave `Application.hpp`) |
| D1 | the declared output moves | | section 3.4 | OUTPUT_MOVE |

The generator writes each function in a marked region at the end of its home `.cpp` (`// ---- PASS B station steps (card NOM3 step PB1b; generated by scripts/nom_pb1_cut.py) ----` and its end marker), its declaration with Doxygen in the home header, the two tables in `src/core/filter/NetworkStationBuild.cpp`, and the `CMakeLists.txt` entries. Doxygen for each step is generated from the row (`doc`, kinds, records, guard) plus the frozen banner comment of its first piece. Re-running it on its own output changes nothing; an edit inside a marked region belongs in the rows or the verifier fails.

### 3.3 Declared substitutions (a closed list)

| class | accepted edit | proof obligation (checked) |
| --- | --- | --- |
| ALIAS_SPLIT | a frozen alias line copied verbatim into a function that reads it; dropped where unread (the 2 dead aliases are dropped everywhere) | the line's text equals a frozen preamble line; its initializer's roots resolve to the frozen objects; no statement of PASS B before the function's call writes a value alias's source (S04 and S05 write only `sh_dev_carry` and `tu_reinit_hist`, bound by reference) |
| ROOT | a root binding (`ctx`, `config_`, `setup`, `est`, `se`, `traces`, `log`, `st`, `R`, `i`, `h2local`, frame members; in the produce also `epoch_clock` and `pass_a`, new names that PB-V12 proves free in the frozen text) at the top of a function | resolves (through `StationBuild` / `StationCarryFrame` aggregate initialization, then EP1's realization) to the frozen root; a step's root may add `const` (CONST_VIEW), never remove it |
| KNOB_MOVE | a derived alias reads its `NetworkKnobs` field | the snapshot expression equals the frozen expression (text and the unit test) |
| GUARD / UNWRAP | `if (C) { body }` becomes `if (!C) return ...;` and the body one level out; a bare `{ }` loses its braces | `nom_td_cut.single_shape` shape; `negate` text |
| SPLIT_GUARD | a frozen braced block cut across several steps; each piece guards on the conjunction of its frozen enclosing conditions, or keeps the frozen `if` with an inserted closing brace | the conjunction equals the path in the frozen nesting tree; its operands are const frame members, knobs or `st.is_pivot`, unwritten by any step |
| PURE_DECL_MOVE | `legacy_local_inject` (from before P10) and `do_amb_carry` (from P12) computed at the frame's construction | the initializer has no call; no statement between the two positions writes an operand |
| FRAME | a frozen local read by a later step is a frame member; its frozen declaration becomes a reference to the member at the same position (EP1 T-B3); `x_lin` and `cb` are the frame's initializers with their frozen initializer text | one frozen local per member, type text equal, initializer equal; the `cb` initializer's names resolve to the same objects at both positions and none is written between them |
| RESULT | `EpochMeasurement em = <call>;` becomes `build.em = <call>;` plus `EpochMeasurement& em = build.em;` (EP1 T-B7) | call text equal; G-EP covers move assignment against direct initialization |
| EXIT | the two station-loop `continue;` of `data_rows` become `return StationStep::Done;` | each is a loop-level `continue` in the frozen loop (depth scan); no other loop-level `continue` exists in a step |
| CALLABLE | a frozen lambda's body is a function; the step binds the frozen name to a forwarding lambda with the frozen parameter list | bodies equal; parameter lists and defaults equal |
| TYPE_MOVE | the local `struct PlPend` is a namespace-scope type in `NetworkStationBuild.hpp`; the deleted `var_add` member is not reintroduced | member list equal |
| MEMBER_TO_FREE | an `Application` helper is a free function with leading `config_` (and `gim_`) parameters; a call site gains those arguments and drops the trailing `_` | bodies equal; call sites equal modulo the name and the leading arguments |
| STAGE_CALL / WALK / RETURN | the calls the orchestrator, the produce and the consume gain (`return epoch_ok;`, the walk, the `StationBuild` construction) | listed lines only |
| OUTPUT_MOVE | a printed string literal rewritten to name the new code site (3.4) | the literal is in the list; nothing else in any string literal of `src/` changed (literal-diff scan) |

Not substitutions, and therefore failures: a change of declarator kind other than CONST_VIEW, a change of root, a guard that is not the frozen conjunction, a reordered step, an unlisted body edit.

### 3.4 Declared output moves

- PB1a: none expected (no printed literal names the serial stages' file; the survey confirms and lists any it finds).
- PB1b: the contributions table's builder column (`NetworkContributions.cpp` 68-77, 149, 155, 159: printed at the first emitted epoch and at run end in `main_log`, and in the manifest's contributions record), regenerated from the step table so each station kind names the step and file that create it (decision 3); the parameter-responses "where" column (`NetworkResponses.cpp` 48, 50, 52, 56, 89, 105, 107, 114, 121, 126, printed on every run); the parameter-registry source (`NetworkParamRegistry.cpp` 323); and the posture-only literals of finding 4. G-EP reverses these substitutions before it compares (step HV0's precedent, `827fba6`).

---

## 4. Verifier checks (gate G-PB-R)

**Reused, unchanged.** From `scripts/nom_cpp.py` (branch `nom/ep1-tools`): `lex`, `Tokens`, `statements`, `parse_declaration`, `split_params`, `FileIndex`, `Scopes`, `calls_in`, `Resolver`, `GitTree`. From `scripts/nom_td_verify.py`: the frozen-range carriage with shift, guard and unwrap (`expected_lines`, `compare`), every frozen code line carried once, the ordered run-end events (`events`), the table-order check (`table_rows`). From `scripts/nom_ep1_verify.py`: `Substitutions` and the declared-class registry, `Frozen` / `Candidate`, the binding derivation and its normalization (check V3a, frozen and new source resolved separately), the struct scanners (`struct_members`, `class_parts`), the ordered event list (V6), the include graph and new-overload check (V9), the name pre-flight (`_preflight`, V13), the Doxygen readers (`doc_before`, `params_doc`), and the negative-control harness (`Control`, `run_controls`). From `nom_ep1/`: `gen_seams` (EP1-form signatures, binding preambles with `// written`, generated Doxygen) and `textops` (non-overlapping edits, generated regions). From `nom_td_cut.py`: the row form, `single_shape`, `negate`. From `scripts/nom_td_gate.py --whole`: gate G-EP.

**New files.** `scripts/nom_pb1/rows.py` (the canonical rows of 3.2, no lines), `scripts/nom_pb1/declared.py` (3.3 and 3.4, the route premises of 2.5 and the thread comparison's masks of section 5, each entry with its reason and a locator), `scripts/nom_pb1_cut.py` (`--survey`, `--stage pb1a|pb1b`, `--check`), `scripts/nom_pb1_verify.py`, `tests/test_network_station_build.cpp`, the knob test of 2.3. New in the reading layer: aggregate-initialization binding (the k-th initializer of `StationBuild{...}` / `StationCarryFrame{...}` binds the k-th member) and the frozen nesting tree of a function body (the path of enclosing conditions to each statement).

**Checks** (exit 0 only when all pass; comments and blank lines ignored):

- **PB-V1 coverage.** Every frozen code line of `networkPassB_` and of the five helpers is carried exactly once, or is the old side of a declared substitution.
- **PB-V2 bodies.** Each new function's body below its bindings equals its frozen pieces, shifted, modulo the declared classes; the bindings region holds declarations only (and the forwarding lambdas).
- **PB-V3 bindings.** Every binding line is an ALIAS_SPLIT copy, a ROOT, a KNOB_MOVE or a FRAME reference; each resolves to its frozen root; declarators keep their kind (CONST_VIEW excepted).
- **PB-V4 tables.** `kStationBuildSteps` and `kStationCarrySteps` equal the canonical rows (name, home, kinds, function) in the order of their frozen pieces; each function is declared once in its home header and defined once in its home source.
- **PB-V5 kinds.** Every kind a step body creates is in its row's set; the union over both tables equals the 29 station kinds of `contributionSpecs()`. "Creates" is followed transitively: a `ContributionKind::X` token in the body; a builder that tags X (the split, `buildCplRow`, `buildLocalBlock`, `appendCouplingRows`, the SH, GIM and SD-EWL builders); and every CALLABLE helper the body calls through its forwarding lambda (`networkAmbCarryEmitRow`, `networkAmbStateNote`, `networkAmbCarryFinishVar`), whose own tokens and builder calls count for the caller, split by the helper's frozen route conditions. A route is excluded for one caller only by a declared premise, checked on both legs: for `local_block`'s fallback, (i) every write to `pl_pend` (`push_back`, `emplace_back`, assignment) in any step lies under the frozen `if (pl_blk_on)` of `ambiguity`, and (ii) K1's snapshot expression for `pl_blk_on` has `carry_cpl_cond` as a conjunct, which makes the helper's `if (carry_cpl_cond)` true at that call. So `ambiguity` must declare PriorAmbiguityCarry and CouplingScalar, and `local_block` CouplingScalar (and not PriorAmbiguityCarry); sections 1.6 and 2.5 are generated from the same rows.
- **PB-V6 guards.** Each step's guard equals the conjunction of its pieces' frozen enclosing conditions; no step writes a guard operand.
- **PB-V7 parallel contract.** (a) Step types exactly `StationStep(StationBuild&)` and `void(StationCarryFrame&)`. (b) The view members of `StationBuild` are const references; the writable members are exactly `st`, `R`, `em`. (c) Scan of every step body, every CALLABLE helper body and every moved builder body. (c1) No assignment, increment or mutating container call (`insert`, `erase`, `emplace*`, `push_back`, `clear`, `resize`, `swap`, `operator[]` on a map or set) whose root is outside `{st, R, build.em, the frame, the function's locals}`. (c2) Pointer members: the scanner lists every `std::unique_ptr`, `std::shared_ptr` (to non-const) and raw pointer-to-non-const member of `NetworkRunContext` (recursively through its value members), `NetworkSetupResult` and `NetStation` from their headers (`struct_members`; 16 today, finding 3), so a pointer member added later is covered without editing the check. For every access path that crosses one of them (`->`, `*`, `.get()`) and is not rooted at the step's own `st`: no write through it, no non-const reference or pointer bound to its pointee, and every member function called through it resolves (`class_parts` on the pointee's class) to an overload set whose members are all `const`-qualified; a binding that takes the pointee into a pointer or reference to const (the oracle-pin table, merge 2912-2914) is allowed. (c3) `stations[` and `setup.stations[` are indexed only by `i`, and only for reads (the gauge-pin helper's `stations[i].is_pivot`, `pinned_amb_targets`). (d) PASS B has exactly one `#pragma omp parallel for`, in `networkAssemblyProduce`, with the frozen clause text; no OpenMP pragma in a step.
- **PB-V8 frame.** Each frame member realizes one frozen local at its frozen declaration point; `rxclk_pred` and `pc_pred` are pushed only by the step that reserves them, after the reserve.
- **PB-V9 callables and helpers.** The three lambda bodies equal their functions'; each forwarding lambda's parameter list (defaults included) equals the frozen lambda's; each moved helper's body equals its frozen body; each call site differs only by the name and the leading arguments.
- **PB-V10 output moves.** Every changed string literal in `src/` is in the declared list, with the declared new text.
- **PB-V11 events.** The ordered list of log calls (logger, level, format literal), event writes and trace-stream writes of the frozen function equals that of the new code composed in call order (stages in orchestrator order, steps in table order, the carry table inside `carried_priors`).
- **PB-V12 names.** EP1's V13 pre-flight on the new functions (`run`, `ep`, `build`, `carry` are not free identifiers in any frozen body: a scan of the merge finds `b` in four lambda parameters of P24 and `f` in the consume, so neither name is used; the L5B body's local `ep` does not collide, since its step takes `build`).

**Negative controls** (each a mutation of the candidate tree that must fail its named check before a pass is believed):

| id | mutation | expected failure |
| --- | --- | --- |
| P1 | `oracle_pin` and `zwd` swapped in the carry table (diagonal-prior order) | PB-V4, and the order test |
| P2 | `rx_cb` guarded by `legacy_local_inject` | PB-V6 |
| P3 | a step writes `stations[j].sm->...` for `j != i` | PB-V7c |
| P4 | `rxclk_pred` declared as a local of `rx_clock_block` (the pointers dangle) | PB-V8 |
| P5 | `StationBuild::i` declared `std::size_t` (the tuple key changes type; it still compiles through the converting constructor) | PB-V3 |
| P6 | `SimZeroIono` created in `glonass_anchors` without its row declaring it | PB-V5 |
| P7 | the `data_rows` exits rewritten to `return StationStep::Next;` | PB-V2 (EXIT) |
| P8 | the `pl_emit_row` forwarder without its default arguments | PB-V9 (and the compiler) |
| P9 | a `zwd` binding `zwd_q_eff = ctx.knobs.zwd_q` | PB-V3 |
| P10 | a builder string edited outside the declared list | PB-V10 |
| P11 | the F3 non-SPD warning moved from the tally into `local_block` (a log line inside the parallel loop) | PB-V11 |
| P12 | `legacy_local_inject` moved past a statement that writes `st.is_pivot` | PB-V6 (PURE_DECL_MOVE) |
| P13 | a `pl_pend.push_back` added to `ambiguity` after its `if (pl_blk_on)` branch closes (the route premise's first leg) | PB-V5 |
| P14 | a step calls a non-const member through a pointer member of the view (`setup.consumer->` or `ctx.carry.slip_oracle->` with a non-const method; it compiles) | PB-V7c (c2) |

A positive control runs too: the verifier on the PB1a tree alone passes with only the PB1a classes.

**What G-PB-R cannot prove, and who covers it.** Move assignment of `em` against direct initialization, code generation after the produce leaves its translation unit (inlining, `/fp:precise`), thread scheduling: G-EP on every arm (`f2` parallel, `ser` serial), the reference thread comparison of section 5 and the cost pair. Reachability: section 5.

---

## 5. Gate arms (G-EP: `scripts/nom_td_gate.py --whole`, same-build Release, renamed executables)

Reference: the build of `<B>` (main with the deletion and EP1); candidates: PB1a, then PB1b. Every arm is rebuilt from the post-deletion goldens (GPS + Galileo since card GAD), so the deleted keys drop from each arm's file. The arms' settings are taken from `runs/_rap_td1b_ref_*/arm.json` and `runs/_rap_g2_ref2_*/arm.json`, diffed against their `f2` and against the deletion's key list (`rmk/remove_rows.json`; only keys set to a non-default value count as lost posture).

- **Retired by the deletion:** `cil` (the lane carry), `fcs`, `fcs2` (FC-1).
- **Lose part of their posture, keep the rest:** `diagpri` loses `network.carry_iono` (`633de7a`, C18) and keeps the level vehicle with diagonal carries; `joint` loses `network.carry_coupling_joint_whiten` (`74b038e`, C23) and keeps the level vehicle, the coupling rows and the T2 replay; `shd`, `trc` and `wcff` each lose `network.sh_health_gate` (`620c2b6`, C14), the EWL and UWL lane layers (`network.mw_wl.ewl_enable`, `uwl_enable`, `uwl_cold_fix`, `bridge_sample_debias`; track L, `8440ca7` and `337e69e`) and `network.carry_invariant_honesty` (`84d0e2c`, C-K), with their PASS B reach unchanged (the SH ties, S04, the `carry_invariant` path; none of the lost keys reaches PASS B, section 1.3).
- **`gf` loses nothing:** it sets `network.clock_anchor_gauge_free` and never set the deleted `shared_carry_gauge_free`.

**Two bases, because no single posture reaches every family** (findings 6 and 7). The goldens use the float vehicle, which supplies the station block structurally but forces the raw F3 knob off, so the ZWD, ionosphere and receiver-clock in-block carries disable themselves on any golden-based arm. Those three arms start from a level-vehicle base, **`lvb`**: the field golden with `network.ambiguity_vehicle` `level`, `network.phase_datum_held_basis` false (it requires the float vehicle, `GnssConfigLoadNetwork.cpp` 12928-12937) and `network.phase_leveling_local_block` true, keeping the golden's `carry_conditional_coupling` true (the way `joint` and `replay` leave the golden for the level vehicle, here with F3 on). On `lvb`, `pl_blk_on` comes from the F3 knob (`NetworkKnobs.cpp` 51), the golden's `carry_rx_code_bias_in_block` stays valid (13074 accepts the F3 knob) and `carry_coupling_exact_update` stays valid (its prerequisites, 5342-5356, do not involve the vehicle). `lvb` is a base, not an arm; card LGM's phase colour requires the float vehicle, so it stays on the golden (`lgm`).

"Evidence" is what the arm's own output must show for its reach to count, usually the step's kind count in the contributions table; a reach whose evidence is zero is reported as unreached, not as covered.

| arm | posture | PASS B reach | evidence |
| --- | --- | --- | --- |
| `f2` | field golden, 2 h | `data_rows` (DataCode, DataCodeCompanion via the second code channel, DataPhase), `congruence_geometry` (`congruence_screen`), prioritized pins, `raim_columns` (continuous handover), the frame, `ambiguity` deferred to the block (float vehicle: `pl_local_block` from the vehicle, `NetworkKnobs.cpp` 49-51), `rx_cb_block`, `local_block` (BlockAmbiguityCarry, BlockRxCbCarry), `rx_cb` (PriorRxCbDatumPrePd: `station_prior_before_pd`), `coupling_region`, `prior_finalize`, S05 (time update and reinit sources), tally, consume, S12 (`trace_topology`) | kind counts > 0 |
| `s2` | sim golden, 2 h from 00:30:15 | the same on the sim bed | same |
| `gal`, `tri` | GPS + Galileo; three-band | multi-constellation companions and the band-5 code rows; the non-GPS-only PD drop count | counts |
| `sp2` | second phase channel | DataPhaseSecond | count |
| `glo` | GLONASS | `glonass_anchors` | GloIfbAnchor, GloIonoAnchor |
| `sdewl` | SD-EWL, triple frequency | `sd_ewl`, the trace drain | SdEwl count |
| `lgm` | phase-colour state (float vehicle, required) | `phase_colour_block`, BlockPhaseColourCarry, `phase_colour_rows`, the LGM tally | counts |
| `bridge` | eq34 with the pin bridge | eq34 pins, Eq34RpbPin, Eq34RpbRankFloor, S13 | the audit line |
| `legacy` (EP1) | legacy datum | legacy pins over `shared_phase_keys`, `!st.is_pivot` in the gates, the predicate's pivot branch | AmbiguityPin at the pivot |
| `diagpri` | level vehicle, coupling off, diagonal carries (ZWD; the ionosphere half deleted) | `ambiguity` inline diagonal (PriorAmbiguityCarry), `zwd` diagonal (PriorZwdCarry), `rx_cb` diagonal carry (PriorRxCbCarry) | counts |
| `replay`, `joint` | level vehicle with an injected slip | free-epoch and pending-reseed skips; `joint`: CouplingScalar through `pl_emit_row` without a block | PRM response counts |
| `gf` | clock-anchor gauge-free (level vehicle, coupling off) | the diagonal routes on another posture | counts |
| `bcl`, `shx` | clock clones, shared history | `cb.cxu` from the clone copy, `cb.shx` | clone run-end lines |
| `shd` | SH with the deviation carry (de-mean, D5 keep), `carry_invariant` | `iono_constraint` (ShIonoTie), S04 erase, re-tap, de-mean | SH tie count, `shdev_*` totals |
| `wcff` | `carry_invariant`, T2 forced | `do_amb_carry` false, `raim_columns` through `raim_ci_on` | the RAIM pool admissions |
| `snapw`, `snapr` | snapshot write, warm resume | `zwd_capture`; the resume coupling census in the tally | snapshot bytes; resume lines |
| `vce` | VCE family `pl` | the α² factor in `networkAmbCarryFinishVar` | the VCE line |
| `zero`, `cold`, `pdx`, `dec`, `trc`, `A4`, `gate`, `fcf`, `hand`, `cul`, `census`, `roll` | EP1's driver and solve arms | PASS B on the normal path (`zero` never reaches it) | as EP1 |
| **`l5b`** (new) | `tri` with `network.estimate_rx_extra_band_code_delay` (effective only with a third band, `GnssConfigLoadNetwork.cpp` 14119) | `rx_ifb_recouple` (`st.l5b.recouple` needs an IFB column to return after a gap), the IFB block folds in `local_block` (`st.l5b.blk_folds`, merge 4423: the golden's `carry_rx_code_bias_in_block` puts the receiver code biases in the block) | the L5B run-end line: `blk_folds` > 0; `recouple` reported as reached only if > 0 |
| **`l5bd`** (new) | `l5b` with `network.carry_rx_code_bias_in_block` false | the diagonal rx-CB walk past `if (rxcb_blk_on) continue;` (merge 4679), with the IFB counts `st.l5b.diag_folds` (4715) and, only on an innovation withhold, `diag_innov_skips` (4699) | the L5B run-end line: `diag_folds` > 0 |
| **`szi`** (new) | sim golden with `network.sim_zero_iono_sigma_m` | `sim_zero_iono` | SimZeroIono count |
| **`gim`** (new) | the GIM rows: the golden with `estimation.ionosphere_weighting.enable` true, `network.sat_gf_datum` `none` and `network.w6_iono_constraint` `auto` (the loader accepts only `auto` and `none`, 3837-3848; `auto` selects the external GIM when the SH field is off; card NOM N2a's smoke `runs/_rap_n13r_gim` logged "GIM loaded for the satellite code-bias ionosphere constraint") | GimIono | count, `gim_rows` outcomes |
| **`ionob`** (new) | `lvb` with `network.carry_iono_in_block` | `slant_iono_block`, BlockIonoCarry, the degrade edge (`pl_blk_iono_gap_drop`); `ambiguity` deferred under the F3 knob | BlockIonoCarry count; the loader's "slant-iono carry IN BLOCK" line, not a DISABLING warning |
| **`zwdb`** (new) | `lvb` with `network.carry_zwd` and `network.carry_zwd_in_block` | the ZWD push in `local_block`, BlockZwdCarry, the B9 skip (`!R.pl_zwd_in_block`, merge 4553) | BlockZwdCarry count; "ZWD carry IN BLOCK" line; `pl_blk_zwd_fallback_diag` reported, reached only on a failed block |
| **`rxclk`** (new) | `lvb` with `network.carry_rx_clock_in_block` and `network.carry_rx_clock_isb` (GPS + Galileo, so ISB columns exist) | `rx_clock_block` with drift-predicted copies and ISBs, BlockRxClockCarry | BlockRxClockCarry count; no DISABLING warning |
| **`zwdc`** (new) | field golden with `network.carry_zwd` | `zwd` through `buildCplRow` (CouplingScalar, `carry_cpl_cond` on) | ZWD run-end line |
| **`zseam`** (new) | `snapr` with `network.zwd_seam_reseed` only (not fingerprinted, `NetworkSnapshotFingerprint.cpp` 660; `carry_zwd` is, 266, and would refuse the resume, `NetworkWarmResume.cpp` 186-200); it resumes the snapshot its own build's `snapw` wrote, whose Z rows the unconditional capture recorded | the window opened at the restore (`zwd_seam_ep_left`, `NetworkWarmResume.cpp` 344; default 480 epochs, open for the whole 900 s run), the `zwd` step's guard through `zwd_seam_live` with `zwd_on` false, `zwd_q_eff`'s seam branch, the coupling row through `buildCplRow` | "ZWD seam re-seed: ARMED at the resume seam" with a non-zero Z-row count; the window's run-end counters |
| **`dup`** (new) | sim golden, `diagpri`'s carry settings (level vehicle, coupling off, `carry_zwd`, `carry_rx_code_bias_in_block` false), `network.station_prior_before_pd` false, an `oracle_pin` inject event | `oracle_pin`; the coalesce in `prior_finalize` (RXCB-DATUM with the rx-CB carry on one column, a pin on a carried ambiguity) | `carry_merged` non-empty (contributions merged count) |
| **`eq34x`** (new) | `bridge` with `network.phase_datum_eq34_seed_round`, `network.eq34_fresh_pin_sigma_scale` > 1, `network.eq34_fresh_pin_ramp_ep` > 0, `network.phase_datum_eq34_rpb_q_m2s` > 0 | the seed-round targets, the fresh-pin weight, Eq34RpbTimeLink | counts |
| **`fuse`** (new) | field golden with `network.prefit_fuse_sigma` 6 | S08 | fuse epochs |
| **`rmarg`** (new) | field golden with `network.readmit_coupling_marg_ep` 2 | `cb.readmit_marg` | diverted count |
| **`wsx`** (new) | field golden with `network.carry_coupling_exact_cross` | `cb.cxu_cross` | CTU counts |
| **`ser`** (new) | field golden with `network.parallel_stations` false and `processing.eigen_threads` 1 | the serial produce (the `if(parallel_on)` clause false) | G-EP IDENTICAL, reference against candidate, like every arm |

**The thread comparison (a report on the reference, not a gate on PB1).** `f2` against `ser` compares two configurations, so the whole-run G-EP cannot hold between them whatever PB1 does: the run manifest records `parallel_stations`, `n_threads` and the configuration hash (`NetworkRunTraces.cpp` 1723, 1735, 1741), `network_run_summary.csv` writes `n_threads` and the parallel flag (2668), and the logs print the thread posture. Eigen is pinned to one thread only inside the epoch loop and only when `parallel_on` (`ApplicationNetwork.cpp` 934-935, restored at 2931), so `ser` also sets `processing.eigen_threads` 1, which makes every dense operation inside the loop single-threaded in both runs. What the comparison covers and masks:
- compared: `products.txt` in full and every CSV (G-EP part 1, `--no-arm-check`), and the manifest record by record (part 2);
- masked (declared in `scripts/nom_pb1/declared.py`, each with its locator): the manifest's `parallel_stations` and `n_threads` (on top of G-EP's own `config_hash_fnv1a64` mask); the run summary's `n_threads` and parallel columns; the logs (parts 3 and 4) are not compared, since they print the thread posture and the Eigen thread count;
- out of scope, named: Eigen's thread count outside the loop (setup before the first epoch, the run-end reports after it: 20 threads in `f2`, 1 in `ser`). A difference whose first differing record precedes the first epoch or follows the last is attributed to it and reported, not counted against the walk.

It is run once, on the reference build, before PB1b: if it is IDENTICAL under these masks, the parallel produce is thread-count independent at `<B>`, which is what the contract type of 2.4 promises. PB1 then needs no separate candidate comparison: G-EP on `f2` and on `ser` (reference against candidate) carries the property over by transitivity. If the reference comparison already differs, that is a finding about `<B>` for the card, and decision 9 decides whether PB1b waits for it.

**Named unreached** (static G-R coverage only, unless an arm shows a count):
- the F3 non-SPD fallback and its warning (`n_amb_block_fallback`), and with it the pending order "phase-colour fallback, then ambiguity fallback" (proved by PB-V4 and the unit test) and `pl_blk_zwd_fallback_diag` (unless `zwdb` shows one);
- the six-family block-column order in one block (finding 7; `lgm` and `ionob` / `zwdb` / `rxclk` exercise two subsets);
- the frame predicate's `zwd_seam_live` disjunct as the deciding term: on the prioritized goldens `phase_datum_prioritized` already makes `legacy_local_inject` true, so the disjunct decides only at a legacy-datum pivot station with `carry_zwd` off; `zseam` reaches the `zwd` step's guard through it, not the frame predicate;
- the consume's three legacy-pivot aborts and so the skip tail S16 (`est.numShared() == 0` cannot newly occur inside PASS B, since the column registration already skips at 0);
- the `data_rows` exits (empty innovations, H/state mismatch) and a throwing step (`R.threw`);
- the oracle pin's non-finite branches;
- the diagonal rx-CB innovation withhold (`pl_cb_innov_skips`, and its IFB count `st.l5b.diag_innov_skips`), unless `cul`, `l5bd` or an injected receiver code-bias step shows one;
- `st.l5b.recouple`, unless `l5b` shows one;
- the PD-rejection naming (S09), unless an arm shows `pd_drop_total` > 0.

Each is listed in the card's result with its static coverage.

**Suite and cost.** The unit suite passes (Debug), with the new `NetworkStationBuildTable`, `NetworkStationBuildOrder` and `NetworkKnobsStationAssembly` suites. Cost: an idle-machine ABBA pair on both 2 h goldens per sub-step, within 1 % (NOM3 section 5). Expected change nil: 23 indirect calls per station-epoch against the measurement update and the block build.

---

## 6. Work split and order

**Order.** The REMOVE-key deletion lands (all five tracks), then EP1 (EP1a-0 tooling, EP1a-1, EP1b with worker W1 converting `networkPassB_`, EP1c), then PB1a, then PB1b. PB1a and PB1b are gated alone (PB1a's G-EP needs no output move; PB1b's declares the printed site citations). PB2 follows PB1b; PB3 (the neighbours) follows PB2.

**PB1a files** (one worker): `src/core/app/ApplicationNetworkAssembly.cpp`; `include/filter/NetworkShDevVtec.hpp`, `src/core/filter/NetworkShDevVtec.cpp`; `include/filter/NetworkCoupling.hpp`, `src/core/filter/NetworkCoupling.cpp`; `include/filter/NetworkStationBuild.hpp`, `src/core/filter/NetworkStationBuild.cpp` (new); `include/filter/NetworkPrefitCensus.hpp`, `src/core/filter/NetworkPrefitCensus.cpp`; `include/filter/NetworkSolveStations.hpp`, `src/core/filter/NetworkSolveStations.cpp` (new); `include/filter/NetworkChainDatum.hpp`, `src/core/filter/NetworkChainDatum.cpp`; `src/core/app/NetworkKnobs.cpp`, `include/app/NetworkRunContext.hpp` (the knob block); `src/core/app/NetworkLocalCarryHarvest.cpp` (four lines, decision 6); `CMakeLists.txt`; the knob test; EP1's `tests/test_network_epoch_contract.cpp` (rows for the seven new epoch-form seams).

**PB1b files** (one generator run, one worker, two review lenses as EP1's: binding and fidelity): the ten new family file pairs of 2.5; `NetworkCongruence`, `NetworkPhaseDatum`, `NetworkSdEwlAr`, `NetworkRaim`, `NetworkOraclePin`, `NetworkCoupling`, `NetworkStationBuild`; `NetworkContributions.cpp`, `NetworkResponses.cpp`, `NetworkParamRegistry.cpp` and the posture-only literal files (output moves); `ApplicationNetworkAssembly.cpp`; `include/app/Application.hpp` (five helper declarations out); `NetworkContributionKind.hpp` (the kind set); `CMakeLists.txt`; `tests/test_network_station_build.cpp`; EP1's `tests/test_network_epoch_contract.cpp` (one row: `networkAssemblyProduce` is a new epoch writer, `void(NetworkRun&, const NetworkEpoch&, const std::unique_ptr<GlobalIonosphereMap>&, EpochAssembly&)`, output `assembly`, kept `gim`; EP1's rule "a new epoch writer is a row" of that test).

**Beside the other steps.**
- **HV1** creates six of the same family files (`NetworkLevelingCarry`, `NetworkPhaseColourCarry`, `NetworkSlantIonoCarry`, `NetworkRxClockCarry`, `NetworkRxCodeBiasCarry`, `NetworkZwdCarry`; its inventory's names, adopted here). Default: PB1b lands first and creates them with an inject section; HV1 adds the harvest section in its own generated region. Alternative: one skeleton commit fixing the names, then both in parallel (decision 5).
- **SG** owns `NetworkSolveGlue.cpp` and creates `NetworkRaimWithinEpoch.cpp`; PB1b adds one function to `NetworkRaim.cpp` and its header: a textual merge only, taken in sequence.
- **EP2** moves the slip-funnel type (`filter/NetworkSlipFunnel`) that S04 reads through `ep.funnel`: an include changes, no PB text.
- **N3b** reads signal-choice sites inside the split, which PB1b moves to `NetworkStationSplit.cpp`: N3b after PB1b, or rebased by regeneration.
- **TD2** moves run-end reports by subsystem; PB1 moves none.
- Concurrency: from PB1a to PB1b about 35 files are owned; any commit to an owned file in the window is absorbed by regeneration from the new `<B>`, never a hand merge.

**Docs.** The card NOM3 result section (verifier summary, arm table, named unreached, the reference thread comparison, and finding 6 as an open behaviour question: the float vehicle refuses the ZWD, ionosphere and receiver-clock in-block carries); CLAUDE.md's network routing tree gains the rule "a new station row or prior family goes in its family file and is a row of `kStationBuildSteps` or `kStationCarrySteps`", and the line counts.

---

## 7. Deviations from NOM3 section 3, each with its reason

1. **PB1a is the serial-stage cut, not the parameter groups.** EP1 already gives `networkPassB_` its groups (spec row 47), and an alias preamble can leave a function without body edits only by being distributed to the functions that read it; PB1a distributes it to the serial stages and PB1b to the steps.
2. **Two tables (12 + 11 = 23 steps) rather than one flat table.** The carried-prior section shares a frame (`x_lin`, the two prior vectors, the coupling builder, the block columns and their predicted copies) under three nested gates; a flat table would put the frame in every step's scratch and re-test the frame gate in each.
3. **The step functions do not take `(NetworkRun&, const NetworkEpoch&)`.** A writable run inside the parallel loop is the defect the contract type exists to rule out (EP1 deviation 1: const does not pass through the run's references); the steps take the const view.
4. **The four row builders become free functions in PB1b**, not PB3: the steps live in family files and cannot call private `Application` members. They move whole; their internal decomposition stays PB3's.
5. **The tally stays one function** (`networkAssemblyTally`) until PB2 splits it with the `StationAssembly` tally sub-struct: a per-family split reorders the folds (integer sums into distinct counters, output-identical, but a reorder the verifier would have to prove commutative).
6. **The phase-datum pins stay one step** (the inventory proposed two): AmbiguityPin and the eq34 RPB rows share the pin vectors and one append.
7. **The local block lives in `NetworkCoupling.cpp`**, the owner of `buildLocalBlock` and `CouplingRowBuilder`, rather than a new file.
8. **The family file names follow the harvest inventory** (`NetworkPhaseColourCarry`, `NetworkSlantIonoCarry`), so the second decomposition does not rename the first (PB inventory risk 9).
9. **Families after the deletion.** No ensemble columns, LV-1 frame, diagonal slant-iono carry, F1 scale or young-arc inflate; the SD-EWL rows stay (they are not among the 144 keys).
10. **Printed site citations are rewritten** (declared output moves), which section 3 did not mention.

---

## 8. Decisions for the author (defaults in parentheses)

1. Two tables (default) or one flat table of 23 steps.
2. The printed site citations: rewritten as declared output moves (default, step HV0's precedent) or left naming the old file.
3. `ContributionSpec::builder` for the station kinds generated from the step table (default: one source for the "contributions as armed" builder column) or kept as hand text.
4. `networkPassB_` stays an `Application` member to pass `gim_` (default), or the GIM joins `NetworkRun` (an EP1 type change) and `networkPassB_` becomes free.
5. PB1b creates the shared family files and HV1 adds the harvest halves (default), or a skeleton commit first so the two run in parallel.
6. PB1a also switches the harvest's four block predicates to the knobs (default yes, one owner), or HV1 does it.
7. The consume's verdict as a kept parameter of the two census stages (default) or an `EpochAssembly` member.
8. The tally split by family in PB2 (default) or in PB1b.
9. The thread comparison (`f2` against `ser`, masks declared, Eigen pinned to one thread in the loop of both): run once on the reference build as a precondition report, with PB1 covered by G-EP on both arms and transitivity (default); or also on the candidate. If the reference comparison differs, PB1b waits for that finding to be explained (default) or proceeds with it named.
10. The unreached branches (section 5) with static coverage only (default), or a test-only fault knob, a new configuration key outside the identical-output scope.
11. The forwarding lambdas of the three ambiguity-carry helpers kept in PB1b and replaced by direct calls in PB2 as an explained move (default yes).

---

## Appendix: measurement files (scratchpad `nextsteps/pb1/`)

`asm_main.cpp` (main's file), `asm_I.cpp` ... `asm_R2.cpp` (each track's), `asm_merged.cpp` (the three-way merge), `attrib.py` and `attrib_asm.json` (per-commit attribution of every deleted or edited main line), `sections_pb1.py` (section map, main and merge, line and code counts), `aliases_pb1.py` (alias census and dead aliases). For the 2026-10-14 revision, `nextsteps/pb1fix/armdiff.py` (each gate arm's `arm.json` flattened and diffed against its `f2`) and `armlost.py` (the deleted keys each arm sets to a non-default value beyond its `f2`, against `rmk/remove_rows.json`: `cil`, `fcs`, `fcs2` retired; `diagpri`, `joint`, `shd`, `trc`, `wcff` partial; every other arm none). Every count in this document comes from these files or from the repository at `31c393f`, `rmk/*` and `nom/ep1-tools`; none was obtained by building or running.
