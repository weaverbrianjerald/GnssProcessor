# IRD: the integer-recovering datum on the eq34 chain

**STATUS**

- **id:** IRD
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-10-13 (step IRD-0, before any grading run): a frame-preserving event algebra for the eq34 de Jonge chain on the float vehicle, observatory first, proven end to end on the sim. The frame is the pin set {(arc, k_e)} plus the root receiver's phase-bias pin; a re-host keeps the frame only when its target is the new edge's integer in the incumbent frame, read at t-1. The card freezes the S-basis derivation, the event table, bundle A0, the reference arm R1, every bar, every decision rule and the bar-calibration rule (sections 3 to 11). IRD-0 (section 12): six graders landed, every selftest passes; the positive control (a truth wire from `simarl_r1c`) grades 0; the wire's E_b predicts the consumer's float (slopes 1.015 and 0.987). The IRD-0 negative control FAILED (nine hour-cells beyond its 0.02 cycle tolerance, a tolerance inside the null's own 95 % band) and the card stopped. The census reproduces the as-built chain at the null (chord WL 0.251 to 0.253, R 0.33, receiver DCB sd 9.5 to 13.0 m). By the author's ruling of 2026-10-13 the control was re-registered before any new grade (section 13): every channel above 0.15 cycle and within max(0.05, the null's own 99 % band) of the null, on 44 cells including the goldens re-based onto main `a02ccfb` (GPS + Galileo; Galileo unclaimed on the golden beds, excluded on the ARL beds). It PASSES in 44 of 44 cells (smallest p50 0.220, largest gap 0.029; section 13.6); the card continues with IRD-1.
- **source:** the author's directive of 2026-10-13 ("Proceed with the integer-recovering datum on the chain", implementation authorized); [FCFC](FCFC_fixed_cycle_ledger_2026-10-11.md) section 9 (the held producer solve does not reach the consumer; the integer-recovering datum is the lever); [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) sections 4, 5 and 13; [AGT](AGT_arc_graph_shared_tree_2026-10-10.md); [AGTB](AGTB_arc_graph_continued_2026-10-10.md) sections 3 to 5; [SBASIS-THEORY-E](../cards/network-vehicle/SBASIS-THEORY-E.md) E4 and E6; [CHAIN-SLANT-ODIJK](../cards/network-vehicle/CHAIN-SLANT-ODIJK.md) 14.2; [SPEC_eq34_seed_round](../SPEC_eq34_seed_round.md); [SPEC_eq34_reattach_hygiene](../SPEC_eq34_reattach_hygiene.md); [PRM3](PRM3_responses_2026-10-02.md); [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md) lines 619 to 623 and 1183; the design panel's critic findings C1 to C8 (section 2)
- **knobs / posture:** bundle A0 of existing keys (section 6); the new knobs of section 7, every one default off (none built yet); base goldens pinned by sha256 at main `3524acb` for IRD-0 (section 6), re-based once onto main `a02ccfb` at IRD-0r (section 13.4; ARL beds carry `processing.active_systems` `["GPS"]`)
- **dates in the text:** 2026-10-13
- **hashes / run dirs:** IRD-0 base `config/golden/golden_sim_refound.json` at `3524acb` sha256 `9080e7ecaf57200e8c18de1bd638d339ea1407931362e6e072282e240c69e57e`, `golden_field_refound.json` at `3524acb` sha256 `cb08c38210b634637bc1b0cd8c517224b36e5b896102f845e7534a81811481fc`; from IRD-0r the base at `a02ccfb`, sim sha256 `613f0016b1b21175d233312a735a4f40eed2f7bc7515a4ce613cd8f326a90eab`, field sha256 `a392a0bc9f1efb82fca8f1120bbfb483d61a623d053293e175e19a0fea0ede43`; IRD-0r exe `build/x64-Release/gnss_processor_ird0r.exe` (main `b08cb1f`), re-based golden arms `runs/_rap_ird0r_{gs,gf}`, grades in `runs/_ird0r_census/`; census arms `D:/GnssProcessorSupport/runs/_rap_prm_k3x2_{qs,qf,es,ef}` and `_rap_prm_q3ah6_{gs,gf}`, `runs/_rf0hv14_sim6`; positive-control template `runs/_rap_arl_r1c/products.txt` on fixture `runs/simarl_r1c`, truth wires `runs/_ird0_truthwire_r1c{,_gauge}/products.txt`; every IRD-0 grading output in `runs/_ird0_census/` (section 12)
- **gates (frozen at pre-registration):** sections 8 to 11 (the grader definitions of section 8 included)

**Outcome.** Question: can the eq34 chain on the float vehicle be made integer-recovering through events, with a held-out consumer fixing correct integers? IRD-0 built the graders (every selftest passes); the positive control grades a truth wire at 0 and the wire's E_b predicts the consumer's float (slopes 1.015, 0.987). The IRD-0 negative control FAILED: on the golden arms the chord wide lane and the 6 h wire's E_L1 read up to 0.029 cycle from the size-matched null against a 0.02 tolerance, and the card stopped (section 12). By the author's ruling the control was re-registered (section 13): far from integer (above 0.15 cycle) and at the null within max(0.05, the null's 99 % band), on 44 cells including the goldens re-based onto `a02ccfb`. It PASSES in all 44 (section 13.6). Standing: open; IRD-1 next, on the `a02ccfb` base.

## 1. The question

A held-out user's between-satellite single difference is integer if and only if every emitted bias difference D_sp is integer (`tests/test_network_estimator.cpp` 412 to 435). The golden fixes the clock and code S-basis (the ionosphere-free clock pin, the satellite geometry-free code bias at broadcast T_GD, the receiver code-bias datum). What remains is a phase family of dimension n_c + m_c per (system, band, component). The question: can the eq34 chain, which already runs on the float vehicle (card PRM3's arms, pin audit complete, 241 of 241 epochs emitted), be made integer-recovering through events, and does a held-out consumer then fix correct integers? The claim covers GPS L1C/L2W on the sim; Galileo (card GAD) and the field L1W group run the chain unclaimed, and the field is a census only. The uncombined model and the slant ionosphere states are untouched.

## 2. References and the critic's findings

The design was synthesized by a judged panel on 2026-10-13 (three designs, two judges, one completeness critic). The critic's contradictions, resolved against the code at `3524acb`:

- **C1.** eq34 has run on the float vehicle: `scripts/prm_ab.py` defines `EQ34` (line 95) and `eq34c` (adds `carry_rebase_complete`); every such arm ran with both emission holds on, so the datum's wire has never been graded.
- **C2.** A softened tree pin (INC-3) or an INC-2 bridge does not realize its target exactly on the float vehicle: the gauge is over-determined by the carried phase-bias priors and the RPB time link, so the data pull the pinned ambiguity off its integer.
- **C3.** The RPB rank floor (sigma 1 m, zero mean) does not bias the emitted P in steady state, but it becomes the datum of each receiver's geometry-free direction (RPB, slant ionosphere, receiver DCB); eq34 arm receiver DCB spread sd 9.37 m against 0.056 m on the golden (`network_rx_states.csv`, TOW 264660).
- **C4.** `truth_clock.csv` is the receiver clock; the satellite clock truth is 0 without `truth_satclock.csv`.
- **C5.** The "INERT under carry_invariant" loader warnings (`GnssConfigLoadNetwork.cpp` 11660 to 11672) print on the float vehicle, where `pin_round`, `carry_shift` and `rpb_q` are live.
- **C6.** Zero-target cold pins are metres on `simnet60_quiet_col_24h`, kilometres on the legacy sim draws and on field RINEX integers.
- **C7.** Kept edges are keyed on parent identity (`NetworkChainDatum.cpp` 640, 653) over an undirected incumbent adjacency, so a flipped kept arc takes pinmove = target minus a stale carry; a vertex absent at t-1 takes baseline 0 (690 to 691), so a returning receiver's RPB and chords shift by metres (inference; a test is owed).
- **C8.** A partial unknown step erases only the (sat, signal) of `eq34_reref` and the receiver's `rpb_carry`; its chord carries outside the moved subtree survive in the old frame.

## 3. The S-basis derivation

Per system c, signal sigma and connected component K of the bipartite graph G (receivers R_K, satellites S_K, tracked arcs E_K), in metres:

    phi_rs = rho_rs + dt_r - dt^s + delta_r - delta^s - mu_b I_rs + lambda N_rs + eps
    p_rs   = rho_rs + dt_r - dt^s + d_r - d^s + mu_b I_rs + e

With the code S-basis fixed by the golden, the phase family's unknowns are {delta_r}, {delta^s} and {N_rs}. Its null space has dimension n_c + m_c: delta_r -> delta_r + alpha_r, delta^s -> delta^s + beta_s, lambda N_rs -> lambda N_rs - alpha_r + beta_s. An S-basis is integer-recovering when the freedom it leaves maps every N to N plus an integer.

**The chain (de Jonge; CHAIN-SLANT-ODIJK 14.2).** Choose a spanning tree T_K (n_c + m_c - 1 edges), pin N_e = k_e (an integer) on every tree edge, and pin the root receiver's delta_r1 = c_1 (one real pin, the RPB). Then alpha_r1 = 0 and alpha_r = beta_s on every tree edge, so the family is full rank. The estimable quantities are:

    b~^s  = delta^s - c_1 + lambda z^s          z^s: an integer combination of (N_e - k_e) on the tree path r1 -> s
    d~_r  = delta_r - c_1 + lambda z_r           (the RPB state of receiver r != r1)
    N~_rs = N_rs + z_r - z^s = zeta_rs           (every chord: an integer)

A user's float single difference absorbs (N_u^s - N_u^p) plus D_sp = z^s - z^p: an integer, so the user's integers are recoverable (SBASIS-THEORY-E E4, E6). Without RPB states (the golden), the receivers' fractional phase offsets land in the chords and, through the anchors, in b~^s.

**The frame.** F = {(e, k_e) : e in T_K} plus (r1, c_1). Two frames give products that differ by Delta b^s = lambda Delta z^s + Delta c. Integer recovery survives an event if and only if every Delta z^s is an integer and Delta c is common to the component; a consumer holding integers survives it only if every nonzero Delta z^s is declared.

**Re-host.** Tree edge e leaves, chord e' joins. In the incumbent frame at t-1 the chord had the integer zeta_old(e'). A target k_e' = zeta_old(e') leaves the frame unchanged (no shift: the pin agrees with the carries up to epsilon, and the solve absorbs epsilon as information). A target zeta_old(e') + j steps the far side of the cut by the integer j. The as-built shift, pinmove = lambda k_e' - x^_e'(t-1) = -lambda epsilon (`NetworkChainDatum.cpp` 661, walked at 803 with variances untouched), moves the subtree's carries by the new edge's float error: a fraction across the cut.

**Cut release.** Every carried prior that relates the moved side M to the rest is erased (each carried arc with one endpoint on each side, at present and absent stations; M's phase-bias priors and RPB carries); the new pin keeps an integer target; the moved keys are declared. The real-valued RPBs absorb any fraction between the two sides, so the frame stays integer-recovering, with a declared but unknown integer offset.

**Root handover or split.** The new root's RPB is pinned at its carried value: Delta c = 0.

**Certification is joint across L1 and L2.** With free ionosphere the per-band chord float is 0.21 cycle on L1 (card ARL section 6), the wide lane 0.06 cycle and the ionosphere-free part 0.04 NL cycle. A pair family grows one forest per system with `band_mask` 0b11 (both bands close the same cycle); certification runs `ils::ilsFixDecorrelatedPartial` on the chord's 2x2 marginal with sigma scaled by 2 (card FCFB's measured optimism), cross-checked against card AGT's lattice (`latticeImplied`, `NetworkArcGraph.hpp` 707); a conflict counts as uncertified.

## 4. The as-built chain on the float vehicle

Five ways the as-built event handling turns estimates into frame steps:

1. A re-host takes pinmove = target - carry (`NetworkChainDatum.cpp` 661), the new edge's float error; the shift walk moves the subtree's carries and emitted biases by it.
2. A root handover is marked unknown (646 to 648) and erases the whole component.
3. Edges are keyed on parent identity (640, 653); a returning vertex takes baseline 0 (691).
4. Tree-edge slips are read from `carry_slipped_ambs` (143), written only under `carry_invariant`.
5. The 1 m zero-mean RPB floor outweighs the 10 m DCB prior and becomes each receiver's geometry-free datum (C3).

The design panel's review of the PRM arms reports the product at the uniform null (chord wide-lane |frac| p50 0.253; per-arc circular R 0.34 against 0.998 on the golden; 44 re-references, 470 erased arcs and 77 shift calls in 2 h on a slip-free bed; receiver DCB spread 9.37 m against 0.056 m). IRD-0 re-grades these numbers with frozen graders before any code is built.

## 5. The event table (`kChainEventSpec`, filter/NetworkChainFrame, built at IRD-1)

One row per event class: its pin target, the step it puts on the frame, what happens to the carries, and what the wire declares. Each row's integer rule is switched by its own entry of `network.phase_datum_eq34_event_rows`; unarmed, the row keeps its legacy rule.

| row | legacy rule (as built) | integer rule | step | carry | wire |
| --- | --- | --- | --- | --- | --- |
| Kept | keyed on parent identity; a flip takes target - stale carry | arc identity plus life stamp; target frozen; no prune fallback to 0 | Zero | Keep | None |
| Birth | 0, or lambda round(float) under seed-round | unchanged (seed-round by decision D-S) | New | Keep | NewKey |
| Returning | baseline 0: pinmove lambda round(carry), marked known | certified from its own t-1 carries; else cut release and treated as a birth | Zero or CutRelease | Keep or CutRelease | None or DeclareMoved |
| RehostCertified | pinmove = target - carry (a fraction) | zeta_old(e') from the t-1 chord table, jointly certified; no shift | Zero | Keep | None |
| RehostUncertified | as above | rounded integer target, cut release, declaration | CutRelease | CutRelease | DeclareMoved |
| RootHandover | unknown: the component's carries erased (D4) | the new r1's RPB pinned at its carried value | Zero | Keep | None (pdr= changes) |
| TreeEdgeSlip | read from `carry_slipped_ambs` (dead on float_kf) | read `relevel.free_ambs` with its cause bits, de-seat at the slip epoch, re-host from t-1 values | Zero or CutRelease | ReleaseArc | None or DeclareMoved |
| Merge | none | cut release of the absorbed side | CutRelease | CutRelease | ComponentChange |
| Split | none | the split-off root pinned at its carried RPB; the cmp change declared | Zero | Keep | ComponentChange |

Entries of `phase_datum_eq34_event_rows`: `kept`, `returning`, `rehost`, `root`, `slip`, `merge_split`.

## 6. Base, bundle A0, the reference arm and the arms

**Base.** Every arm is a flat copy of the golden at main `3524acb` (sha256 in the STATUS block), made by `scripts/ird_make_arms.py`, which writes the golden's revision and sha256 into every `rapid.json`. The working tree holds card GAD's golden edits (GPS plus Galileo, closed 2026-10-13, uncommitted at IRD-0). Rule: if GAD lands before IRD-2, the card re-bases once, before IRD-2's runs, and re-runs the references; it never re-bases mid-increment.

**Bundle A0** (existing keys; one datum swap, documented as a bundle on card EXL's precedent):

- chain on: `network.phase_datum_eq34` true, `network.phase_datum_eq34_carry_shift` true, `network.carry_rebase_complete` true;
- prioritized and held machinery off: `network.phase_datum_prioritized` false, `network.phase_datum_held_basis` false, `network.phase_datum_held_mark_cold` false, `network.phase_datum_held_cold_release` false, `network.phase_datum_handover_continuous` false, `network.phase_anchor_carry_reref` false, `network.phase_datum_gate_drop_erase` false, `network.phase_datum_min_stations` 1;
- float re-pin paths at their off values: `network.phase_datum_eq34_seed_round` false (decision D-S), `network.phase_datum_eq34_seed_round_rehost_frame` false (INC-2), `network.phase_datum_time_linked` false (bridge), `network.eq34_fresh_pin_ramp_ep` 0, `network.eq34_fresh_pin_sigma_scale` 1.0 (INC-3);
- the slip set of card ARL section 13 (the list `validateNetworkFixedCycles_` checks, `GnssConfigLoadNetwork.cpp` 1575 to 1590): `network.cycle_slip.gf_arc_jump` true with `gf_arc_jump_k` 6, `network.cycle_slip.relevel_free_epoch` true, `network.cycle_slip.if_phase_td` true, `network.cycle_slip.if_phase_td_reseed_on_flag` true, `network.ambiguity_resolution.suspect_epochs` 10;
- the search, log-only: `network.ambiguity_resolution.{enable, tree_basis, decorrelated_partial, block_search, cycle_basis, trace, trace_lattice}` true. `fixed_cycles.fold` stays off (it refuses eq34, `GnssConfigLoadNetwork.cpp` 1497); IRD reads the AGT lattice, not the fold.

**Reference arm R1:** the golden plus the slip set, the search and the IRD traces (section 7). It isolates the datum from the slip fixes.

**Microscope:** `config/sim/arl/arl_r1c` (receiver phase biases at every station) plus `network.sim_zero_iono_sigma_m` 0.001. It resolves a hundredth of a cycle and separates the static algebra from the events. Its zero-ionosphere rows remove the receiver geometry-free direction, so decision D0 is taken on r2, r3c and the golden bed.

**Decision D0's switch** uses existing knobs: `network.phase_datum_eq34_rpb_prior_sigma_m` (1.0, or 0 for floor off) and `network.phase_datum_eq34_rpb_q_m2s`.

**Arms.** IRD-2: R1; A0 = R1 + bundle A0; A0h = A0 with `emit_pb_hold` and `emit_pb_hold_phase_clock` off; A0f = A0 with `rpb_prior_sigma_m` 0; A0s = the D0 winner with seed-round; beds the microscope, r2, r3c and the golden sim graded from 00:30:15. IRD-3: A1 = A0 + D0 + D-S + family pair. IRD-4: the event rows armed cumulatively on A1 in table order, then `refuse_uncertified_first`, then the claim (arm C). IRD-5: A2 = C + `pair_lattice`. IRD-6: A2 (plus IRD-5b if built) on the ladder. IRD-8: held-out consumers dropped with `rapid_arm.py --drop-station` from both the IRD producer and a same-build golden reference. Every arm runs on one renamed Release exe; no field ON arm before IRD-6 has passed (invariant 1).

**Fixtures** (`config/sim/ird/`, built at IRD-2 from A0's chain trace on `arl_r3b`; each differs from r3b in one station's scripted slips or RINEX; each with a `_2h` and a `_producer` file): ird_t1 L1C +1 on a satellite-side tree edge whose subtree holds at least 3 satellites; ird_t2 (+4, +5), MW-blind; ird_t3 L2W +1 with LLI; ird_t4 (+2, +2) on an r1-adjacent edge; ird_t5 the t2 shape at settled TOW 266250; ird_tr a slip on r1's own edge; ird_rg r1's RINEX blanked for 10 min; ird_rv a non-root station blanked for 6 min and for 16 min.

## 7. New knobs and owners (built from IRD-1; every one default off or legacy)

**Measure-only (IRD-1).** `network.trace_chain_datum` writes `logs/network_chain_edges.csv` (`week,tow,system,family,band,comp,r1,edge_sta,edge_sat,child,parent,depth,target_m,target_cyc,prev_target_m,pinmove_m,known,legacy_cause,row,row_armed,certified,cert_source,flipped,returning,life_stamp`), `logs/network_chain_frame.csv` (the t-1 chord table: `week,tow,family,arc_sta,arc_sat,float_cyc_1,float_cyc_2,q11,q12,q22,int_1,int_2,source,conflict,lattice_int`) and CHAIN rows in `network_events.csv` for both edges of every event (`rehost`, `cut_release`, `root_handover`, `returning`, `slip_deseat`, `merge`, `split`, `declared_keys`; the `detail` column carries `sig=<signal>` on a key row). `network.trace_pb_prehold` writes `logs/network_pb_prehold.csv` immediately before `networkPbHold` (`week,tow,sat,signal,est_pb_m,est_pb_sigma_m,est_clk_m,phase_visible_m,emit_pb_m,gen,role,comp,r1,attach_sta,depth`, role one of tree, chord_only, root_adj, gated, bridged). `network.trace_rpb_states` writes `logs/network_rpb_states.csv` (`week,tow,station,system,signal,value_m,sigma_m,pinned,floor_w,timelink_var_m2,parent_sat,depth,comp,event,shift_m`). Run end: `networkChainFrameRunEnd`, one row of `kNetworkRunEndReports`, counting non-integer targets, soft pins and bridges on claimed keys, legacy D4 erases and fractional known shifts, events by row and step class, certified share by source, conflicts, wrong-certification candidates, cut-released arcs, PB priors and RPBs, root handovers, components per family, returning and flipped vertices, gated keys, declared keys and the RPB time-link NEES.

**Datum (IRD-3 to IRD-5).** `network.phase_datum_eq34_family` "signal" (default) or "pair"; `network.phase_datum_eq34_event_rows` (section 5); `network.phase_datum_eq34_rehost_cert` "pair" (default) or "pair_lattice", riders `_sigma_scale` 2.0 and `_determined_sigma_cyc` 0.05; `network.phase_datum_eq34_refuse_uncertified_first`; `network.phase_datum_eq34_integer_claim` [] (default) or ["GPS"], the one owner of the claim (requires eq34, float_kf, carry_shift, carry_rebase_complete, family pair, every event row, the ARL slip set through one shared helper, min_stations 1, trace_chain_datum; refuses prioritized, held_basis, handover_continuous, phase_anchor_carry_reref, gate_drop_erase, seed_round_rehost_frame, phase_datum_time_linked, a fresh-pin ramp or sigma scale, extend_only, longevity_election, slip_aware_election, ensemble and state_snapshot_path); `network.phase_datum_eq34_rpb_prior_birth_only` (only if D0 needs it); `network.phase_datum_eq34_rpb_in_block` (only if D-N fires).

**Emission (IRD-7).** `RealizedPhaseDatum {Prioritized, HeldBasis, Ensemble, Chain}` replaces the bool of `toSolutionEpoch`; `signal_integer` per key under the claim; `network.declare_integer_by_datum` (default off; changes golden P field 5, a separate ledger decision); `network.emit_wire_chain_tokens` (`cmp=`, `genc=`); H2 `network.emit_pb_hold_chain_generations` (riders `emit_pb_hold_settle_age_s` 1800, `emit_pb_hold_settle_period_s` 900); H1 the holds off; H3 `network.emit_rebase_chain` (IRD-7b, only if H1 and H2 both fail).

**Consumer (IRD-1, IRD-7).** `makeAmbiguityResolverConfig` (one shared builder; copies `enable_partial_ar` and `min_arc_info`, a defect fix logged at run start); `network.closed_loop.ar_observe`; `network.closed_loop.ar_search` "full" or "decorrelated_partial" (rider `ar_determined_sigma_cyc` 0.05); `network.closed_loop.trace_ar_fixes` (`logs/closed_loop_ar_fixes.csv`: `week,tow,system,signal,sat,ref_sat,cmp,float_cyc,cond_sigma_cyc,fixed_int,determined,kept,z_fixed,z_total,ratio,p_s,gen_sat,gen_ref`; `logs/closed_loop_ar_epoch.csv`: `week,tow,n_pairs,n_wl_det,n_band_det,fix_e,fix_n,fix_u,float_e,float_n,float_u`); `network.require_signal_integer_for_ar` extended to amb_datum 2 and equal `cmp=`; `network.closed_loop.pb_hold_gen_translate`.

**Owners.** `NetworkArcGraph` stays the only owner of trees, cycles and cut sides (`forestFundamentalCycle`, `cutSide`, `ArcRow::certified`, `ForestRules::refuse_uncertified_first`); `NetworkCarryDisposition` owns the cut release; `NetworkPbHold` owns generations; one run-end row owns the census; `NetworkParamRegistry`'s RPB row declares the RPB disposition.

## 8. Graders (IRD-0) and their definitions

Every grader has `--selftest` and prints n before any verdict.

- **Wire, dual band** (`scripts/simnet_wire_integer_consistency.py --bands L1C,L2W --xcorr --wl --nl --source prehold|products --split comp,depth,gen,role --events`). Per satellite s and band b, with dX = emitted - truth (truth: `truth_bias.csv` osb_m; satellite clock truth 0 without `truth_satclock.csv`): Wp_b = dC - dP_b, Wc_b = dC - dB_b, x = (Wc_1 - Wc_2)/(mu_1 - mu_2), E_b = (Wp_b + mu_b x)/lambda_b. Over all satellite pairs of one system per epoch: D_b = E_b^s - E_b^p, D_WL = D_1 - D_2 (the consumer's wide lane), g = D_1 + 3.529 (D_WL - round(D_WL)) (NL cycles), and the Melbourne-Wubbena wide lane as a check (equal to D_WL when the range-like term r = Wc_1 - mu_1 x is 0).
- **Size-matched null.** One uniform fraction per unit (a satellite and its hold generations for the wire; an arc for the chords), the same statistic over the same occupancy (pair-epoch or arc-epoch weights), 2000 draws; the null value is the mean of the draws, its band the 2.5 and 97.5 percentiles. "Within 0.02 cycle of the null" means |observed p50 - null value| <= 0.02.
- **Chords** (`scripts/ird_chord_census.py`). Every carried arc of `network_amb_state.csv` (erase rows excluded) that is not a pinned edge (`network_phase_anchor.csv`, and `network_chain_edges.csv` once it exists) at that epoch; per band value_abs/lambda and the wide lane value_L1/lambda_1 - value_L2/lambda_2 (L1C and L1W paired with L2W, pooled); |wrap| p50 against the null; per-arc circular R of the wide lane over 30 min windows; the per-epoch wide-lane jump (consecutive epochs of one arc, unwrapped) p50 and p99; the receiver DCB spread (`network_rx_states.csv` CB GPS_DCB, sd across stations at the last epoch of each hour); the run-end counters (re-references, erased carries, S-transformed carries, shift calls, RPB erases, pin audit).
- **Frame** (`scripts/simnet_chain_frame_grade.py`). At every chain event epoch (CHAIN rows), the step D(t) - D(t_prev) of every pair on the pre-hold P, per class; a pair is declared when either key is in that epoch's `declared_keys` rows; I-b's bars.
- **Truth wire** (`scripts/simnet_truth_wire.py`). A template `products.txt` rewritten with C = the satellite-clock truth (0), B and P at truth OSB, signal_integer 1, `cmp=1`, `gen=1`, amb_datum 2; optional per-(satellite, signal) integer gauge lambda k.
- **Consumer** (`scripts/consumer_ar_grade.py`). `fixes`: a fix is correct if and only if fixed_int = (N_c^s - N_c^p) + round(D_sp), with N_c the slip-accumulated truth plus the consumer arc's wind-up branch k = round(wu_sim(t0)) at the consumer's own arc start; pairs with |wrap(D_sp)| > 0.25 are ungradeable; the Wilson 95 % upper bound on the wrong rate. `float`: the consumer's unwrapped single-difference float error ((amb_s - amb_ref)/lambda - (N_c^s - N_c^ref)) regressed on the wire's E_b difference, per band, from 03:00:30 (the design's validation window) to the run's end.
- **Arms** (`scripts/ird_make_arms.py`): the bundle, the arm and bed tables, the sha pin, and the refusal of any knob the loader does not read.

## 9. Bars (frozen)

- **I-a.** Run-end census: 0 non-integer targets; 0 soft pins and 0 bridges on claimed keys; 0 legacy D4 erases and 0 fractional known shifts under the claim.
- **I-b.** At every chain event epoch on the sim, the truth SD-potential step of every undeclared pair has a wrapped size of at most 0.02 cycle (microscope, per band) or 0.05 cycle (wide lane, elsewhere). Every integer step is declared. Every event class reaches n >= 1 on its own fixture; an unexercised class reads as FAILED.
- **I-c.** 0 wrong certified re-hosts against truth.
- **W-micro.** r1c with zero-ionosphere rows, settled hour: per band and on the wide lane, p50 |wrap D| <= 0.02 cycle and p99 <= 0.10.
- **W-WL.** r2, r3c and the golden bed: p50 <= 0.08 cycle, with at least 90 % of pair-epochs within 0.20.
- **W-NL.** g = D1 + 3.529(D_WL - round(D_WL)): p50 <= 0.10 NL cycle, with at least 85 % within 0.25. Per-band values are reported without a bar.
- **Calibration rule.** At the close of IRD-2, each W bar B is compared with its event-free floor F, measured on the D0 winner over pairs whose tree paths saw no event for 30 min, with n >= 200 (otherwise B stands). If F <= 0.8B, B stands. Otherwise the bar becomes 1.25F and B is recorded as MISSED. The result is frozen before IRD-3 and never revised.
- **K-a.** 0 wrong among >= 1000 settled determined integers per held-out station on each 2 h bed. On the 6 h bed, the 95 % Wilson upper bound is below 3e-4.
- **K-b.** The WL is determined on >= 50 % and the band on >= 20 % of settled pairs (arc age >= 30 min), reaching >= 70 % of the truth-wire ceiling.
- **K-c.** The fixed (conditioned) 3D p50 is <= 0.8x the float in hours 1 and 2 of the 6 h bed; time to 3D < 0.10 m is >= 20 % earlier; a consumer started at hour 3 reaches a sustained 5 cm (10 consecutive epochs) >= 30 % sooner.
- **K-d.** The golden and A0 wires get 0 accepted fixes with the gate on.
- **K-e.** K-a holds at all four held-out stations; K-b and K-c at three or more.
- **P-a.** The held float consumer's 3D p50 is within max(+10 %, +5 mm) of the same-build golden in hours 2 to 6 and within +25 % in hour 1 (6 h, graded from 00:30:15).
- **P-b.** Sim census medians of the held-out stations are no worse by more than 10 % in any hour from hour 2.
- **P-c.** 720 of 720 epochs emitted; W5 withholds no more than the golden; postfit sigma0 within 10 %; ARL float DD p50 per band within +0.01 cycle of R1; 0 wrong producer fixes; receiver DCB sd <= 3x the golden.

## 10. Decisions and their rules (frozen)

- **IRD-2, hold is emission-only.** A0h's `network_amb_state.csv` and `network_rx_states.csv` are byte-identical to A0's. Otherwise A0h is not A0's wire, and D3 is re-planned.
- **IRD-2, static-algebra stop rule.** On the microscope, chords whose fundamental cycle saw no chain event for 30 min must read per-band p50 |frac| <= 0.02 cycle. If they do not, the static chain is not integer on the float vehicle: the card stops, attributes the cause from the census, and builds nothing from IRD-3 on until the cause is closed.
- **D0 (RPB floor).** The floor goes off if A0f, on r3c and the golden bed, shows a DCB sd <= 3x the golden's (0.17 m), W5 withholds no higher than A0's, and float-consumer p50 within +10 % of A0's in hours 1 and 2. Otherwise build birth_only and re-measure. Otherwise keep the floor and record the GF-datum defect, which blocks the ionosphere follow-up.
- **D-S (seed-round births).** Adopted if `test_eq34_seed_round` passes, 0 seed target steps occur after birth, the chord census is no worse, and hour-1 float p50 is within +10 %.
- **D-N.** An RPB NEES above 2 on the sim makes IRD-5b owed before the D1 verdict.
- **IRD-2, lattice under eq34.** Dimension = E - V + C in every sampled epoch and 0 wrong fixes on r3c. Otherwise IRD-5 is not built and certification is pair-only.
- **IRD-2, would-be certification on r2 and r3c.** 0 wrong against truth. Otherwise sigma_scale becomes 3 by this frozen rule, is re-measured, and is fixed before IRD-4.
- **D1 (IRD-6).** I-a, I-b, I-c and the frozen W bars hold on the 6 h golden bed and every 2 h bed; P-c holds. A missed bar is written as FAILED in the Outcome.
- **D3 (IRD-8, the wire).** If H2 passes the emitted W bars, K-a to K-c and P-a, H2 is the card's wire. If H2 fails and H1 passes, H1 is the wire, with its P-a numbers stated. If both pass, H2. If both fail the emitted W bars, IRD-7b (H3) is built and graded on the same gates.
- **D4 (IRD-8).** The consumer verdict, with every missed bar written as FAILED.
- Golden adoption of any wire change stays the author's ruling, through the ledger.

## 11. Increments and their gates (frozen)

**IRD-0** (this step): the card, the graders with selftests, the zero-build census. Gates: (1) every selftest passes; (2) **negative control**: on the golden arms the chord wide lane and the wire E_L1, E_L2 and WL each read within 0.02 cycle of the size-matched null, with n > 0 in every hour; otherwise the instruments are void and the card stops; (3) positive control: a truth wire synthesized from `runs/simarl_r1c` grades 0 on every pair and band; (4) E_b predicts the consumer's amb_m on `_rf0hv14_sim6` with a slope in [0.95, 1.05]; (5) the census reading of eq34 and eq34c is entered as a reading, not a verdict; (6) `docs_index_check.py` and `make_golden_config.py --verify` both pass. The golden arms are `_rap_prm_q3ah6_{gs,gf}` (first 2 h) and `runs/_rf0hv14_sim6` (graded from 00:30:15); the census arms `_rap_prm_k3x2_{qs,qf,es,ef}`. These field arms are the only field reading until IRD-9.

**IRD-1** (observatory): the seam, traces and run-end row of section 7 in observe mode (the election never reads them while no event row is armed); loader honesty (log text only); the consumer's shared builder, observe mode, decorrelated branch, conditioned position, fix traces and the extended gate. Gates: (1) all new knobs off, `nom_td_gate.py` IDENTICAL on both refound goldens at 2 h, on ARL r3c with the search (`network_producer_ar.csv` hash aa6779cc) and on the retired eq34 lineage `golden_field_gps_dual` at 2 h against `runs/_rap_agt_ref_gd2`; (2) traces on: `products.txt` and every pre-existing CSV identical, the new CSVs non-empty, declared pins = folded pins = pin audit every epoch; (3) consumer: observe mode bit-identical to ar_mode 0 (unit test and a 2 h golden arm); the decorrelated branch matches the producer's ils call on synthetic Q; truth-wire Q5 replays on r1c, r2 and r3c at SZZ000SIM give 0 wrong among >= 1000 settled determined integers (coverage and time to fix recorded as the ceiling); a replay of `_rf0hv14_sim6` gives 0 accepted fixes with the gate on (the wrong rate with the gate off recorded); (4) the full Debug suite passes; `rt_seam_verify.py` reports only the declared IRD hunks; `rt_seam_gate.ps1` passes with the knobs off.

**IRD-2** (the as-built chain measured, arms only): section 6's arms on the four sim beds; events by table row with the share and truth correctness of the would-be certifications; the chord census; the pre-hold wire grade and the emitted grade on A0h; DCB spread and W5; RPB NEES; lattice dimension and producer wrong fixes; the float consumer by hour; the fixtures. Gates: pin audit COMPLETE, 0 non-integer targets, every counter printed; the section 10 rules (hold emission-only, static-algebra stop, D0, D-S, D-N, lattice, would-be certification); the W bars frozen by the calibration rule; each fixture's event on its intended tree edge.

**IRD-3** (pair family): off identical (AGT-4 chain oracle, both goldens); on, identical L1 and L2 tree edge sets per pair family every epoch, pins per band = sum of (n_c + m_c - 1) + 1 per component, pin audit COMPLETE, mismatch 0; the oracle extension passes; A1 against its signal-family parent with every edge-set difference attributed to a band-only arc, float consumer by hour within max(+10 %, +5 mm) and P-c on r3c and the golden bed.

**IRD-4** (event rows, then the claim): per row, off byte-identical; on its own fixture (kept and rehost: r3c and the microscope; returning: ird_rv; root: ird_rg; slip: ird_t1 and ird_tr; merge_split: whatever the census reports), and only where the class reaches n >= 1, the class's legacy or fractional steps fall to 0, I-b holds for the class, both edges are counted with identities (invariant 7); no regression against the previous arm (float consumer by hour within max(+10 %, +5 mm), W5 no higher). Arm C: I-a, I-b, I-c; W-micro on r1c; W-WL and W-NL on the pre-hold P on r2, r3c and the golden bed (2 h) at the frozen bars; P-c. The loader refuses each forbidden combination, with a unit test per interlock.

**IRD-5** (lattice certification, frame-lattice coherence): the certified share of re-hosts does not fall against C; 0 wrong lattice-sourced targets; sim coherence mismatches equal the truth-graded wrong lattice rows (0 expected); ms per epoch at 60 and 300 stations within +10 % of C with the search on; arm C's gate still holds. **IRD-5b** (conditional on D-N): RPB NEES within [0.5, 2] on r3c and the golden bed; W-WL, W-NL and P-c no worse than A2; off byte-identical.

**IRD-6** (the ladder, D1): every tree-edge rung de-seats the slipped or vanished edge in the event epoch with a row naming its cause bit, 0 undeclared integer and 0 fractional steps (I-b), 0 wrong certified re-hosts, the pre-hold wire grade on undeclared pairs within noise of the r3b control, 0 wrong producer fixes; chord rungs (ARL s1 to s8, tw2, tw8) 0 wrong; the adversarial arm (ird_t2 without `gf_arc_jump`) reported with the duration and keys of the undeclared step; D1; invariant 1 met for tree edges, which unblocks IRD-9's field arms.

**IRD-7** (emission and declarations): off byte-identical (no tokens, golden P field 5 unchanged); the consumer's float position invariant to 1e-9 m across a settle regeneration including the phase-clock term; a Q5 replay reproduces the disc counters and generations byte for byte; on the EMITTED P, W-WL and W-NL hold on r2, r3c and the golden bed (2 h) (H2: post-settle generations; H1: all settled keys); I-b on the emitted wire through ird_t1, ird_tr, ird_rg and ird_rv with every step declared; signal_integer 0 on every gated, bridged and open-frame key.

**IRD-8** (the consumer proof, D3 and D4): K-a to K-e with N and the Wilson bound per station and bed; P-a, P-b, P-c against the same-build golden; the D3 and D4 rules of section 10. Beds in order r1c, r2, r3c, the IRD-6 rungs, the golden sim bed at 6 h; a late-start consumer at `start_offset_hours` 3.504167; negative controls the golden wire and A0's wire.

**IRD-9** (field census, 300 stations, deferral triage, the brief): field 2 h and 6 h complete with every epoch emitted, WTZZ00DEU p50 within +15 % of the same-build golden in hours 2 to 6, census medians no worse than +10 % with ARHT00ATA and KOKB00USA named, the chord WL gap to its null reported with n, coherence mismatches at most 1e-3 of lattice rows, no integer verdict claimed for the field; n300 with 0 undeclared steps and ms per epoch within +10 % of the golden n300; `make_golden_config.py --verify` and `docs_index_check.py` pass. Deferrals triaged with data (invariant 3): consumer write-back, the slant-ionosphere product, snapshot support, the Galileo and L1W claims, lattice re-host targets. No golden or ledger change without the author's ruling.

<!-- Results are appended below this line under dated headings. -->

## 12. 2026-10-13: IRD-0, the graders and the zero-build census (STOPPED: the negative control fails)

**What landed.** Sections 1 to 11 (written before the final graders ran); the CARDS.md entry; six graders, each with a selftest: the dual-band mode of `scripts/simnet_wire_integer_consistency.py` (the one owner of the wire algebra and the size-matched null, which the others import), `scripts/ird_chord_census.py`, `scripts/simnet_chain_frame_grade.py`, `scripts/simnet_truth_wire.py`, `scripts/consumer_ar_grade.py` and `scripts/ird_make_arms.py`. No C++ changed and no producer arm ran. Every grading output and the two diagnostic scripts below are kept in `runs/_ird0_census/`.

**Declared adaptations.**

1. The INDEX line: `docs/INDEX.md` states that a program card's index line under rule 3 is its entry in `refound/CARDS.md`; that entry is the line, and INDEX.md is not edited.
2. Grader development included an exploratory prototype read of the golden wire and chords (the same statistics) before the final graders ran. The definitions of section 8 and the bars of sections 9 to 11 are the design's and were not changed after it; the plain reading of "within 0.02 cycle of the null" (|p50 - null value| <= 0.02) is kept although it fires the stop rule. The null value is 0.25 for any choice of null unit, so the verdict does not depend on that choice; only the null's band does.
3. The wire grader's WL is the consumer's wide lane D_1 - D_2; the Melbourne-Wubbena wide lane is printed as a check. The two differ by the satellite differences of the range-like term r (the clock wire's own error; |r| p50 0.23 to 1.86 m on `_rf0hv14_sim6`), so MW is not the consumer's quantity on this wire.
4. `ird_make_arms.py` builds the ARL beds from the pinned golden with every station re-pointed at the fixture; `config/sim/arl/arl_*_producer.json` equal the golden at `3524acb` apart from those paths and the search knobs, which the arms set.
5. Truth wires are kept at `runs/_ird0_truthwire_r1c/products.txt` and `runs/_ird0_truthwire_r1c_gauge/products.txt` (template `runs/_rap_arl_r1c/products.txt`) for IRD-1's replays.

**Gate 1, selftests: PASS.** Wire dual-band 18 checks, chain frame 9, truth wire 6, consumer grade 6, chord census 9, arm maker 11; 0 failures (`runs/_ird0_census/selftests.txt`).

**Gate 2, negative control: FAILED; the stop rule fires.** |wrap| p50 by hour, gap to the size-matched null in brackets. Chord n 27 245 to 55 834 arc-epochs per hour (514 to 599 arcs); wire n 28 006 to 59 520 pair-epochs (32 or 33 satellites); n > 0 in every hour. Null values 0.2493 to 0.2504; null 95 % bands about [0.227, 0.273] (chords) and [0.221, 0.267] (wire).

| golden arm (hours from) | channel | h0 | h1 | h2 | h3 | h4 | h5 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `_rap_prm_q3ah6_gs` (00:30:15, 2 h) | chord WL | 0.265 (+0.015) | **0.272 (+0.022)** | | | | |
| | wire E_L1 / E_L2 / WL | 0.250 / 0.245 / 0.238 | 0.255 / 0.245 / 0.241 | | | | |
| `_rap_prm_q3ah6_gf` (00:00, 2 h) | chord WL | 0.249 (-0.001) | **0.272 (+0.022)** | | | | |
| `runs/_rf0hv14_sim6` (00:30:15) | chord WL | 0.240 (-0.010) | 0.243 (-0.007) | **0.230 (-0.020)** | **0.221 (-0.029)** | 0.233 (-0.017) | **0.228 (-0.022)** |
| | wire E_L1 | 0.231 (-0.020) | **0.224 (-0.026)** | **0.224 (-0.026)** | **0.223 (-0.027)** | **0.222 (-0.028)** | 0.233 (-0.017) |
| | wire E_L2 | 0.246 (-0.004) | 0.244 (-0.006) | 0.251 (+0.000) | 0.257 (+0.007) | 0.254 (+0.004) | 0.251 (+0.000) |
| | wire WL | 0.256 (+0.005) | 0.254 (+0.003) | 0.256 (+0.005) | 0.256 (+0.006) | 0.258 (+0.008) | 0.258 (+0.008) |

Nine cells breach 0.02 (bold; the `_rf0hv14_sim6` h2 chord cell by 0.0001). By the rule of section 11 the instruments are void and the card stops.

What the failure consists of (a reading; the rule stands as written):

- **The tolerance is inside the null's own band.** The size-matched nulls' 95 % bands extend about 0.022 either side of the null value for the chords and 0.028 below and 0.017 above it for the wire, so a structureless run breaches 0.02 in about 6 to 7 % of hour-cells per channel (2 000 null draws on `_rf0hv14_sim6` hour 3: chords 6.2 %, wire 6.6 %). Of the nine cells, five lie inside the null band, one at its edge (gf h1 0.2722 against 0.2722) and three outside it (gs h1 chord 0.2720 against 0.2702; `_rf0hv14_sim6` chord h3 0.2205 against 0.2272 and E_L1 h3 0.2227 against 0.2236).
- **The sim golden's chords are not independent fractions.** Per-station circular R of the chord wide lanes across a station's arcs at one epoch: p50 0.49 to 0.59 on the sim golden against 0.31 [0.26, 0.37] for independent fractions; 0.33 on the field golden and 0.38 on the eq34 sim arm (`diag_station_R.txt`). Each sim receiver's fraction is shared by its chords, so the independent units are stations rather than arcs and the pooled p50 wanders further than the arc-unit null allows.
- **The 6 h golden wire is one frozen realization.** The phase-bias hold starts 64 of 74 generations at the first epoch, so the four failing E_L1 hours of `_rf0hv14_sim6` are one draw of about 32 satellites, not four readings; the same posture on an older build (`_rap_prm_q3ah6_gs`) reads E_L1 within 0.005. Split by a shared current anchor, the different-anchor pairs alone read 0.224 to 0.234 (same-anchor pairs 0.18 to 0.25, `diag_anchor_split.txt`), so the deficit is not the current same-anchor pairs alone.
- **The design record held this value.** The critic called the golden wire "at the null (0.222 to 0.257)" and the panel's probe read E_L1 0.222 to 0.229; the frozen tolerance excludes 0.222.

**Gate 3, positive control: PASS.** The truth wire from `runs/simarl_r1c` grades E_L1, E_L2, WL and NL (and MW) at p50 0 and p99 0 in both hours (58 776 and 55 650 pair-epochs, 32 satellites); with an integer gauge (seed 13) p99 3e-9 cycle.

**Gate 4, E_b predicts the consumer: PASS.** `_rf0hv14_sim6`, consumer SZZ000SIM, 03:00:30 to the end: L1C slope 1.015 (r 0.998, n 2 254), L2W 0.987 (r 0.997, n 2 254); over the whole run from 00:30:15 1.009 and 1.006 (n 4 187). The residual (the free-ionosphere consumer's own per-band float error) has sd 2.2 to 3.0 cycles and |wrap| p50 0.23 to 0.27.

**Gate 5, census (a reading, not a verdict).** 2 h from 00:30:15 (sim) and 00:00 (field). Chord WL p50 by hour with the gap; per-arc circular R of the wide lane over 30 min (independent-uniform null 0.116); per-epoch wide-lane jump p99 (max); receiver DCB sd at the end of hours 0 and 1; run-end counters.

| arm | chord WL h0, h1 | R | jump p99 (max) | DCB sd (m) | re-references / erased / S-transformed / shift calls / RPB erased | pin audit; emitted |
| --- | --- | --- | --- | --- | --- | --- |
| eq34 sim `_rap_prm_k3x2_qs` | 0.251 (+0.001), 0.253 (+0.003) | 0.332 | 16.9 (295) | 9.45, 11.21 | 44 / 470 / 4 906 / 77 / 62 | COMPLETE 241 (L1C, L2W); 241 |
| eq34c sim `_rap_prm_k3x2_es` | `products.txt` and `network_amb_state.csv` byte-identical to eq34's | | | | | |
| eq34 field `_rap_prm_k3x2_qf` | 0.251 (+0.002), 0.248 (-0.002) | 0.237 | 68.9 (1 440) | 12.99, 12.75 | 75 / 674 / 4 809 / 124 / 101 | COMPLETE 240 (L1C, L1W, L2W); 240 |
| eq34c field `_rap_prm_k3x2_ef` | 0.251 (+0.002), 0.248 (-0.002) | 0.239 | 68.9 (1 440) | 12.99, 12.76 | the same | the same |
| golden sim `_rap_prm_q3ah6_gs` | 0.265 (+0.015), 0.272 (+0.022) | 0.960 | 0.27 (101) | 0.056, 0.042 | none (no chain) | 720 of 720 (6 h) |
| golden field `_rap_prm_q3ah6_gf` | 0.249 (-0.001), 0.272 (+0.022) | 0.674 | 5.9 (187) | 1.59, 1.46 | none | 720 of 720 (6 h) |
| golden sim 6 h `_rf0hv14_sim6` | 0.240 to 0.221 over 6 h | 0.992 | 0.155 (44) | 0.027 to 0.062 | none | 720 of 720 |

The eq34 sim wire (held): E_L1 0.249 and 0.251, E_L2 0.254 and 0.257, WL 0.253 and 0.257, NL 0.251 and 0.260, every gap within 0.01 of the null. Per-band chords on every arm read 0.243 to 0.260. The design panel's review numbers reproduce: chord WL 0.253, R 0.33 (0.34 quoted), jump p99 16.9 (17), 44 re-references, 470 erased, 77 shift calls, DCB sd 9.45 m at TOW 264600 (9.37 m quoted at 264660). These field arms are the only field reading until IRD-9.

**Gate 6:** `python scripts/docs_index_check.py` and `python scripts/make_golden_config.py --verify` pass.

**Base drift.** Card GAD landed on main during IRD-0 (`a02ccfb`, golden sim sha256 `613f0016...`, GPS plus Galileo). No IRD-0 reading used the golden as a base, so IRD-0 stays on the `3524acb` pin; by section 6's rule the card re-bases once, before IRD-2's runs, if it resumes (`ird_make_arms.py` `GOLDEN_REV` and both sha256 pins change together).

**Standing.** Stopped at IRD-0 by the negative-control rule as written. Nothing from IRD-1 on is built, and no producer arm was run. The graders, the arm maker and the truth wires stay as instruments. Resuming requires the author's ruling on the negative control; a re-registered control would be a new pre-registration written before any new run, not a re-wording of this one.

## 13. 2026-10-13: Re-registration of the negative control (the author's ruling, 2026-10-13)

Sections 13.1 to 13.5 were written before any arm of this step was made and before any new grade was computed. The rule of section 11 (IRD-0 gate 2) and its result in section 12 stay as written.

### 13.1 The ruling and what was already seen

The author's ruling of 2026-10-13 on the failed control: "re-register and resume".

Disclosure. The IRD-0 control failed (section 12): with the tolerance |p50 - null| <= 0.02 cycle, nine cells breached it: chord WL `_rap_prm_q3ah6_gs` h1 0.272 (+0.022), `_rap_prm_q3ah6_gf` h1 0.272 (+0.022), `_rf0hv14_sim6` h2 0.230 (-0.020), h3 0.221 (-0.029), h5 0.228 (-0.022); wire E_L1 `_rf0hv14_sim6` h1 to h4 0.224, 0.224, 0.223, 0.222 (-0.026 to -0.028). Section 12's table omits the Melbourne-Wubbena wide lane, which also breached on `_rf0hv14_sim6` h0 0.223 (-0.027) and h1 0.224 (-0.026) (`runs/_ird0_census/neg_wire_rf0hv14_sim6.txt`); the stop held under either reading of WL. Every value of the 34 IRD-0 cells (section 12's table) was seen before this rule was written. All 34 lie above 0.22 cycle and within 0.030 of their nulls, so the new rule passes them by inspection; the only cells it grades unseen are the 10 cells of the re-based golden arms (section 13.3).

### 13.2 The purpose of the control

The graders must read a non-integer product as far from integer, so that a later integer reading is meaningful. The golden's wire and chords carry no integer structure (no chain, no receiver phase-bias states), so the instrument must place them near the uniform null and well away from the integer bars of section 9 (W-WL p50 <= 0.08, W-NL p50 <= 0.10). A control whose tolerance lies inside the null's own sampling band (the IRD-0 rule: 0.02 against a 95 % band of about +/-0.022) tests the null's tail, not the instrument.

### 13.3 The new rule (frozen)

On every golden arm and hour-cell, every channel passes both parts:

1. **Far from integer:** p50 |wrap| > 0.15 cycle.
2. **At the null:** with g = p50 - null (the null value: the mean of the size-matched null's draws for that cell's n and occupancy), and q_0.5, q_99.5 the 0.5 and 99.5 percentiles of the same draws (the null's own two-sided 99 % band), -max(0.05, null - q_0.5) <= g <= max(0.05, q_99.5 - null).

with n > 0 in every cell. A breach of either part in any cell, or n = 0, stops the card again: status parked, nothing from IRD-1 on built. If every cell passes, the card continues with IRD-1.

Channels: the chord wide lane (GPS L1C, else L1W, with L2W, pooled; `scripts/ird_chord_census.py`) and the wire E_L1, E_L2 and WL (D_1 - D_2, the consumer's wide lane of section 8; `scripts/simnet_wire_integer_consistency.py --bands L1C,L2W --systems G`). The field arms carry the chord wide lane only (no truth). The Melbourne-Wubbena wide lane and the NL are reported without a bar. Both graders read GPS only; the null is 2000 draws with the seeds of IRD-0.

Cells (44):

| arm | window | channels | hours | cells |
| --- | --- | --- | --- | --- |
| `_rap_prm_q3ah6_gs` | 2 h from 00:30:15 | chord WL, E_L1, E_L2, WL | 2 | 8 |
| `_rap_prm_q3ah6_gf` (field) | 2 h from 00:00 | chord WL | 2 | 2 |
| `runs/_rf0hv14_sim6` | 00:30:15 to the end (h5 a half hour) | chord WL, E_L1, E_L2, WL | 6 | 24 |
| `_rap_ird0r_gs` (re-based sim golden) | 2 h from 00:30:15 | chord WL, E_L1, E_L2, WL | 2 | 8 |
| `_rap_ird0r_gf` (re-based field golden) | 2 h from 00:00 | chord WL | 2 | 2 |

The two re-based arms are the a02ccfb goldens unchanged (no knob under test), made with `scripts/ird_make_arms.py --arm G` and run on `build/x64-Release/gnss_processor_ird0r.exe` (main `b08cb1f`, which contains `a02ccfb`). A field arm of the unchanged golden is not an ON arm; invariant 1's refusal of field arms before IRD-6 stays for every other arm.

### 13.4 The re-base onto main a02ccfb

By section 6's rule the card re-bases once, before IRD-2's runs; it does so now, before IRD-1, and does not re-base again. `ird_make_arms.py` pins `GOLDEN_REV` `a02ccfb` with sim sha256 `613f0016b1b21175d233312a735a4f40eed2f7bc7515a4ce613cd8f326a90eab` and field sha256 `a392a0bc9f1efb82fca8f1120bbfb483d61a623d053293e175e19a0fea0ede43`, changed together.

What card GAD changed in both goldens (and nothing else): `processing.active_systems` `["GPS","Galileo"]`; `processing.signal_priorities.Galileo` `["C1C","C1X","L1C","L1X","C5Q","C5X","L5Q","L5X"]`; `estimation.clock_disc_isb_reinflate_std` 10.0; `model_corrections.enable_receiver_cross_system_atx_fallback` true; `network.code_datum_family_difference` true; `network.code_family_birth_prior_sigma_m` 0.10.

How the IRD arms treat Galileo:

- **The golden sim and field beds keep GPS + Galileo** (the golden posture). Galileo runs unclaimed (section 1 and the design's base-drift note): no integer claim, no bar, and the chain on the A0 arms grows Galileo forests that no grader reads.
- **The ARL beds (microscope, r1c, r2, r3b, r3c) exclude Galileo** by an explicit bed knob, `processing.active_systems=["GPS"]`, written into every arm's `rapid.json` `ird` block. The ARL fixtures carry Galileo E1/E5a, but card ARL's rungs, its slip-set validation and the r3c search reference of IRD-1 gate 1 (`network_producer_ar.csv` hash `aa6779cc`) were all measured GPS-only; the bed knob keeps those beds comparable to that record and keeps the microscope's reading free of a second constellation. With one system, card GAD's five `n_sys >= 2` keys are out of regime (ledger A16). Whether the r3c hash reproduces on the re-based GPS-only bed is read at IRD-1 and recorded; IRD-1's identity gates are same-build comparisons either way.
- **Every IRD grader reads GPS only** (the chord census filters GPS satellites; the wire, frame and consumer graders take `--systems G`). The claim stays GPS L1C/L2W on the sim.

### 13.5 Annotations to sections 3 and 12 (the IRD-0 review)

The body text above is not rewritten; these annotations correct or complete it.

1. **Section 12, gate 2, third bullet:** `_rap_prm_q3ah6_gs` is not an older build. It is the later run (products 2026-10-03 against 2026-10-01 for `_rf0hv14_sim6`) of the same posture, differing in one knob: `network.start_offset_hours` 0.504167 (first E record 00:31:00 against 00:00:30). Its holds froze 30 minutes later, outside the fixture's start artifact, while 64 of `_rf0hv14_sim6`'s 74 PB_HOLD generations start at TOW 259230 (00:00:30), inside it. The start, not the build, is the likelier reason the two frozen realizations differ.
2. **Section 12, gate 2 table:** the MW row of the `_rf0hv14_sim6` wire (h0 to h5): 0.223 (-0.027), 0.224 (-0.026), 0.244 (-0.007), 0.248 (-0.002), 0.253 (+0.003), 0.245 (-0.005); on `_rap_prm_q3ah6_gs` 0.235 (-0.016) and 0.238 (-0.012). Under the MW reading of WL two more cells breach (h0, h1); the stop held under either definition.
3. **Section 12, gate 5 (invariant 4):** `network.carry_rebase_complete` is byte-identical on the sim (`_rap_prm_k3x2_qs` against `_es`: `products.txt` and `logs/network_amb_state.csv` identical at 77 shift calls), although the run-end line of `_es` lists ten sites as moved. That line is a declaration of the sites, not a count of moved values. Open item: per-site moved-value counts are owed before A0 relies on the knob at IRD-2 (the IRD-1 run-end row counts them). On the field the two arms differ (`products.txt` line 29872).
4. **Section 3, sign:** with phi = ... + delta_r - delta^s + lambda N and the pin delta_r1 := c_1, the estimables are b~^s = delta^s - delta_r1 + c_1 + lambda z^s and d~_r = delta_r - delta_r1 + c_1 + lambda z_r. Section 3's "delta^s - c_1" and "delta_r - c_1" drop the term delta_r1. The frame formula (Delta b^s = lambda Delta z^s + Delta c) and the integer-recovery argument are unaffected, because delta_r1 - c_1 is common to the component.
5. **Section 12, the reproduction sentence:** the design panel's golden per-arc circular R of 0.998 does not reproduce: 0.960 measured on `_rap_prm_q3ah6_gs` (2 h) and 0.992 on `_rf0hv14_sim6`.
6. **`scripts/simnet_chain_frame_grade.py`** now fails a run that carries no CHAIN row unless `--allow-empty` is passed (an unarmed or inert `trace_chain_datum` would otherwise read as no failure, invariant 4), and scans every non-event epoch: an integer step there is an undeclared integer step and fails bar I-b; a fractional step above the bar there is counted and reported (the frozen fractional bar is scoped to event epochs). The declared-count against observed-count check and the adversarial arm's report at IRD-6 read this scan.
7. **The two diagnostics** of section 12 are promoted from `runs/_ird0_census/` to `scripts/ird0_station_R.py` and `scripts/ird0_anchor_split.py`. The dual-band wire grader now prints the truth-OSB cross-station disagreement count and refuses a fixture where it is nonzero, as `scripts/simnet_truth_wire.py` does.

### 13.6 Result: the re-registered control PASSES in 44 of 44 cells

Graded after sections 13.1 to 13.5 were written. Exe `gnss_processor_ird0r.exe` (Release at main `b08cb1f`). The arms `runs/_rap_ird0r_gs` (241 epochs, 267 s) and `runs/_rap_ird0r_gf` (240 epochs, 432 s) exited 0; `_rap_ird0r_gf/products.txt` is byte-identical to card GAD's 2 h identity arm `runs/_rap_galp3_id_f2`. The edited graders reproduce every IRD-0 value exactly (p50, null and band; the draws and seeds are unchanged). Truth OSB: 342 keys over 61 stations, 0 cross-station disagreements. Outputs: `runs/_ird0r_census/` (`ctl_wire_*`, `ctl_chord_*`, `selftests.txt`).

p50 |wrap| by hour, gap to the size-matched null in brackets:

| arm | channel | h0 | h1 | h2 | h3 | h4 | h5 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `_rap_prm_q3ah6_gs` | chord WL | 0.265 (+0.015) | 0.272 (+0.022) | | | | |
| | E_L1 / E_L2 / WL | 0.250 / 0.245 / 0.238 | 0.255 / 0.245 / 0.241 | | | | |
| `_rap_prm_q3ah6_gf` | chord WL | 0.249 (-0.001) | 0.272 (+0.022) | | | | |
| `_rf0hv14_sim6` | chord WL | 0.240 (-0.010) | 0.243 (-0.007) | 0.230 (-0.020) | 0.221 (-0.029) | 0.233 (-0.017) | 0.228 (-0.022) |
| | E_L1 | 0.231 (-0.020) | 0.224 (-0.026) | 0.224 (-0.026) | 0.223 (-0.027) | 0.222 (-0.028) | 0.233 (-0.017) |
| | E_L2 | 0.246 (-0.004) | 0.244 (-0.006) | 0.251 (+0.000) | 0.257 (+0.007) | 0.254 (+0.004) | 0.251 (+0.000) |
| | WL | 0.256 (+0.005) | 0.254 (+0.003) | 0.256 (+0.005) | 0.256 (+0.006) | 0.258 (+0.008) | 0.258 (+0.008) |
| `_rap_ird0r_gs` | chord WL | 0.265 (+0.015) | 0.266 (+0.016) | | | | |
| | E_L1 | 0.244 (-0.007) | 0.243 (-0.008) | | | | |
| | E_L2 | 0.244 (-0.006) | 0.244 (-0.006) | | | | |
| | WL | 0.250 (-0.000) | 0.249 (-0.001) | | | | |
| `_rap_ird0r_gf` | chord WL | 0.256 (+0.007) | 0.233 (-0.017) | | | | |

The `_rap_prm_q3ah6_gs` wire gaps are -0.001 / -0.005 / -0.012 (h0) and +0.005 / -0.005 / -0.010 (h1). n is 27 245 to 59 520 per cell. Smallest p50 0.220 and largest |gap| 0.029 (`_rf0hv14_sim6` chord WL h3). The null's 99 % half-bands are 0.027 to 0.048 below and 0.020 to 0.032 above the null, so the 0.05 floor set the tolerance in every cell. The 10 cells unseen before the rule read p50 0.233 to 0.266 with |gap| at most 0.017. Reported without a bar: MW and NL lie within 0.028 of their nulls in every cell (largest MW `_rf0hv14_sim6` h0, -0.027).

Readings on the re-based goldens (no bar): sim per-arc circular R 0.959, receiver DCB sd 0.073 and 0.054 m; field R 0.809, DCB sd 1.106 and 0.932 m, per-epoch wide-lane jump p99 1.70 cycle. These are IRD-2's same-base golden references.

**Verdict.** Every cell passes both parts, so the card continues with IRD-1 on the `a02ccfb` base. Gates of this step: grader selftests 72 checks, 0 failures; `docs_index_check.py` and `make_golden_config.py --verify` pass (77 armed in regime on both goldens). The card is near the 60 KB bar: results from IRD-1 on go to a continuation card.
