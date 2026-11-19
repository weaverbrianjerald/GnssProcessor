# UHT: a per-decision trace of the user engine's integer holds (2026-11-18)

**STATUS**

- **id:** UHT
- **topic:** consumer
- **status:** open
- **summary:** Design of a default-off, per-decision trace of the user engine's integer fixing (reference election, wide-lane decision, narrow-lane search, hold memory, release, correction state), meant to explain the three short wrong holds of NRF section 118.1 (UTQI E18-E06 DOY 146, QUIN G10-G02 DOY 146, ASCG G20-G23 DOY 147). After the check against the code, the three leads stand (the hold under mode 1 is the `hold_variance` write-back, the hold check is skipped silently in at least five cases, the runner-up of the integer search is computed and discarded), while the key-off identity, the column count and the reuse of the observe trace's column names need revision before a build. Nothing is built; the runs it needs later are listed.
- **source:** plan package 9 of the post-reset program (a design: a per-decision trace for user holds), from NRF section 118.1 (`NRF_network_ring_fixing_2026-11-14_part16.md`:124-142) and item (c) of section 123 (`NRF_network_ring_fixing_2026-11-14_part18.md`:34); commits read: 7fb0737 for network code and main 5ddc422 for user-engine code (the draft itself was read at 03dfcbc and cites 8ca1a20, whose AmbiguityResolver.cpp is byte-identical to main). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The user fixing behind the three wrong holds is the code of 8ca1a20 (on main as 54e4f12), absent from the chain/audit branch, which has no held-pair memory, no float wide lane and no release; a build of that branch would not even run the wide-lane-constrained search. For these users the hold is a Joseph write-back inside `resolve` at `hold_variance`; `holdAmbiguities` does nothing under mode 1. Today's records leave five groups of decision inputs unrecorded, and the hold check is skipped silently in at least five cases. The proposed trace adds one CSV, keyed `estimation.ambiguity_resolution.trace_decisions`. It can name a reference change, a declared correction step and an early float; a missed slip and a wrong network integer are reachable only by joins.

Read at commit 03dfcbc of branch chain/audit (worktree D:/GnssProcessorSupport/worktrees/chain). User-fixing code that 03dfcbc does not contain is cited at 8ca1a20 and marked "@8ca". No file was built or run, and no run folder was opened.

## 1. Which code made the decisions

- The users of section 118.1 were replays on engine rscb6 (part15.md:212). Their recipe sets `wl_from_float`, `wl_branch_check`, `hold_release_cyc 0.25`, `nl_sigma_floor_cyc 0.04`, `nl_iono_free_row`, `refloat_partner_signal`, `enable_partial_ar`, `hold_variance 1e-7`, `ratio_threshold 5` and `"mode": 1` (scripts/irdu_users.py:43-46).
- At 03dfcbc none of those resolver keys has a loader: they appear only in scripts and documents. `RealTimeEngine::resolveAmbiguities` calls `resolve` and then `holdAmbiguities`, and nothing else (src/core/realtime/RealTimeEngineEstimate.cpp:254-285). The release path, the partner refloat and the code-bias reset request exist only @8ca (same file, lines 286-304).
- Neither 8ca1a20 nor 54e4f12 is an ancestor of 03dfcbc (`git merge-base --is-ancestor`, first-stage read). AmbiguityResolver.cpp @8ca is reported identical to main 5ddc422, and the record states main reproduced rscb6's user replays. The rscb6 source commit itself is inferred from that reproduction, not from a build hash.
- [corrected 2026-11-19: see Corrections, item 1] Consequence: the trace is built on main, or on chain after chain takes main. A user run on a chain/audit build would take its wide lane from the MW detector (AmbiguityResolver.cpp:532) and would not reproduce the three runs (inferred).
- The users are between-satellite single-difference users (`consumer_undifferenced false`).

## 2. What is decided where today, and what goes unrecorded

Per-epoch order (RealTimeEngine.cpp@5ddc422:543-584): generation change; phase-bias discontinuity reaction; code-bias and clock re-inflation; measurement update; ZWD constraint and ionosphere prior; `resolveAmbiguities`; `writeEpochTraces`; the ambiguity level check; the per-satellite trace. `resolveAmbiguities` runs the measure-only observe search, then `resolve`, and on success installs `fixed_state` and `fixed_covariance` into the filter (RealTimeEngineEstimate.cpp:279-281).

### 2.1 The hold

- `holdAmbiguities` returns at once unless `ar_mode == 3` (AmbiguityResolver.cpp:2775; @8ca:3506, verified). It does nothing for these users.
- The hold is the write-back of `KalmanFilter::josephPseudoUpdate` with R = `hold_variance` = 1e-7 m2 (sigma about 0.3 mm): @8ca:2440-2442 for the full set, @8ca:2328-2331 for the partial set (verified).
- Every later epoch's float is the previous epoch's integer plus one update. The wide-lane decision and the release test therefore read a state already pulled to the held integers (inferred). The separate float user is the only independent check, which is how the grader found the three runs.

### 2.2 Reference election

- Decided in `buildDdPairs` (AmbiguityResolver.cpp:251-383; @8ca:357-483). Eligibility: a nonzero value (excludes an uninitialized seed), a nonzero variance (excludes the frozen pivot, :315), elevation at or above the AR mask (20 degrees, :321), not unresolvable (:326), inside `ar_active_systems` (:332-336).
- Each (constellation, signal) group takes its highest-elevation satellite with a strict `>`, every epoch, with no hysteresis (:350-371). The filter's own frame pivot is sticky (PivotManager.cpp:308-323) and is never an AR candidate.
- Recorded: only as `sat_ref` inside per-pair log lines. Frame-pivot handovers are logged (PivotManager.cpp:682, :799). No line reports a change of AR reference.
- Effect of a change: the held-pair key is `sat_i|sat_ref|sig_hi|sig_lo` (@8ca:513), and the wide-lane persistence runs are keyed the same way (@8ca:1531). Old entries stop matching. The memory cannot carry a hold over, but the state can: single differences pulled to the old integers form an integer-consistent double difference under the new reference, so the new search may accept at once (inferred).

### 2.3 Wide-lane decision

- Path: `resolveWlConstrained` @8ca:1864-1880, `floatWidelane_` (:1313-1332), `decideWlBranchOfPair_` (:1494-1579), `decideWlBranch` (:1399-1448).
- Inputs: w and sigma_w from four single-difference ambiguity states and their 4x4 covariance; r0 and r1, the N_1 fractions of the ionosphere-free row for candidates w0 and w1; sigma_n with the 0.04-cycle floor (:1521-1522); gap = |shift - round(shift)|.
- Rules: the code admits w0 when |d| + 3 sigma_w <= 0.40 (`wl_max_frac`); refused when 1 + |d| <= 3 sigma_w; the phase is decisive when |r| + 3 sigma_n <= gap/2. A code-admitted w0 is taken at once, with no persistence and no phase confirmation, unless a decisive phase contradicts it. A phase decision must persist 120 s (:1542-1549). A persisted contradiction requests a reset of the receiver-satellite code-bias states (:2002-2010).
- Recorded: `WLBRANCH` at info (:1567-1574) with tow, pair, signals, w, sigma_w, w0, w1, r0, r1, sigma_n, gap, code admit, candidate, phase, persist, decision and reason; `WLFLOAT` (:1873-1878); a run-end count (:1591-1597).
- Unrecorded: the four state values and their covariance; the code-bias and slant-ionosphere states behind w; whether the pair's state was written back by the previous epoch's fix; the loss of a persistence run on a reference change or a prune (:1581-1589, silent).

### 2.4 Narrow-lane decision

- @8ca:2024-2467. T maps double differences to the ionosphere-free row (:1973-1975); a pair amplified 20 or more times is dropped with a log line (:1955-1968). Q_reduced = T Qdd T' plus the shared-reference floor (:2083-2087). The search value is y + shift dd_wl (:2192-2197), searched by MLAMBDA (:2202).
- Gates in order: ratio >= 5 and <= `ratio_upper_bound` 1e4 (:2232-2240, again at :692); bootstrap success >= `min_success_rate` (loader default 0.999, GnssConfigLoad.cpp@5ddc422:2003-2004); per-pair |frac| <= `nl_max_per_pair_frac` (default 0.15, :2249-2265). On failure `ils::selectFixableSubset` drops the largest-variance entry, or the worst-fraction entry when the fraction gate fails (IlsCore.cpp@5ddc422:312-344).
- Recorded: the ratio line (:2217); the failing gate with P_s and worst pair (:2233-2263); the success summary (:2336-2338, :2448-2450); `QBCOND`; `ARCAGE` per candidate (:2100-2116); at debug, per pair "N_1_float N_1_fixed dd_wl DD1 DD2" (:2372-2377) and "[PAR] ... N_1_fixed" (:2322); `CLFRAC` and `CLCOV` per candidate, debug only (:596-640, single-difference fractions); one AR line per epoch in closed_loop_trace.csv (include/realtime/ConsumerTrace.hpp:47-50).
- Unrecorded: the runner-up integer vector; the narrow-lane fraction of every candidate (only the worst, only on rejection); N_1_float on the partial path (unconfirmed); the elimination sequence with the ratio and P_s of each tried subset (IlsCore has no logging); the off-diagonals of Q_reduced.
- The runner-up exists and is discarded: `ils::mlambda` searches two candidates (src/core/filter/IlsCore.cpp:234, `search(L, d, z_float, E, sq, 2)`) and back-transforms only `E.col(0)` (:249). The first-stage trace design's claim that MLAMBDA returns only the best vector is refuted.

### 2.5 Ends of a hold

- Held-pair test @8ca:1767-1838: `holdMisfit_` (:309-319) gives d_wl and d_if (narrow-lane cycles) against the remembered n1 and wl; a pair is released when either exceeds `hold_release_cyc` 0.25. `chooseReleaseSatellites_` (:321-334) blames the reference when it has two or more contradicted pairs forming a majority of its held pairs, otherwise sat_i. Released satellites sit out the search (:1857) and are refloated after the fix is installed (RealTimeEngineEstimate.cpp@8ca:287-296). Holds are remembered only on success with `hold_release_cyc > 0` (:511-514, verified).
- Partner refloat (`PARTNERREL`, :270-281). Arc restarts erase held pairs with no log line (:283-290, verified).
- [corrected 2026-11-19: see Corrections, item 2] Two unchecked paths, both silent: a group that `splitGroup` finds unusable is skipped before the lookup (:1773); a key miss after a reference change is a bare `continue` (:1779-1781). Both verified. Only the second was named by the first-stage reads.
- Recorded: `HOLDREL` only above threshold (:1792-1796, :1812-1815, :1829-1830), `PARTNERREL`, the "AR release" line. Unrecorded: the misfit below threshold, the skipped tests, the erasures.
- With R = 1e-7 m2 written back each epoch, one update rarely moves a held double difference 0.25 cycle, so a wrong hold most likely ends by an arc restart or a reset rather than by `HOLDREL` (inferred; each run's last epochs settle it).

### 2.6 Correction state

- `noteSsrDiscontinuity` resets one ambiguity per (sat|signal) (RealTimeEngineSsrBreaks.cpp:304-311); a freshness flip resets it (:323-357); a hold-generation step is handled by `reactToPbHold_` (:663-695).
- Recorded: reset log lines; in closed_loop_sat_trace.csv `pb_m`, `pb_disc`, `ev_pb` (which names `reset_hold_gen` and `reset_gen`, include/realtime/ConsumerSatTrace.hpp:48-51), `amb_m`, `amb_var`. That trace is written after AR, so `amb_m` is the held value. It carries no generation value, no correction age and no record identity.
- `signal_integer` is read only when `network.require_signal_integer_for_ar` is on (default off; GnssConfigLoadNetwork.cpp:8148-8151), through MeasurementModelRow.cpp:116-130 and `markAmbiguityUnresolvable` (MeasurementModelStates.cpp:775-786, cleared each epoch at :727, logged at trace level), and by the observe search (RealTimeEngineArObserve.cpp:122). `wide_lane_integer` has no user-side reader at either commit.
- Slip resets are unnamed: the five `resetAmbiguity` sites of RealTimeEngineSlipQc.cpp (:131, :151, :155, :185, :267, verified) call no `noteConsumerSatEvent_`.

### 2.7 Summary of unrecorded inputs

1. Reference: elevations, eligibility drops, a change event.
2. Wide lane: the four ambiguity values and covariance, the states behind w, a "state already held" bit.
3. Narrow lane: the runner-up vector, every candidate's fraction, the partial elimination path, the Q_reduced off-diagonals.
4. [corrected 2026-11-19: see Corrections, item 2] Release: the misfit below threshold, both unchecked paths, the silent erasures of holds and persistence runs.
5. Corrections: generation and record identity (`cmp`) per key, `signal_integer`, `wide_lane_integer`, the step in `pb_m`, and the cause of a slip reset.

## 3. The trace

### 3.1 Key and identity when off

- Key `estimation.ambiguity_resolution.trace_decisions` (bool, default false), a member of `AmbiguityResolverConfig`, loaded beside the other AR keys (GnssConfigLoad.cpp:1940-2010), with a ledger row (UNVALIDATED) in the same commit.
- [corrected 2026-11-19: see Corrections, item 4] Off: no row is built, no log line changes, and the one new piece of trace-only memory (last reference of each group, last `pb_m` per key) is updated only when armed. The cost is one bool test. The resolution's state, covariance, `fixed_dd_rows` and `held_pairs` are the same with the key on or off (unit case 9).

### 3.2 Files and owners

- `logs/closed_loop_ar_decisions.csv` from the replay driver (ApplicationConsumerReplay.cpp, beside the observe trace at :438-447) and the in-process path (network/diag/RunTraces, as RunTraces.hpp:531 does for the observe trace). `logs/ar_decisions.csv` from post-processing (PppEpochUpdate.cpp@8ca:623), with correction columns blank.
- Pattern of the observe trace: a header constant, a plain row struct and a pure formatter in a new include/filter/ArDecisionTrace.hpp; the resolver appends to `AmbiguityResolution::decisions` only when armed; the engine fills the correction columns after `resolve`; the driver owns the stream.
- One addition to the satellite trace: an `ev_amb` column naming the slip-reset cause at the five RealTimeEngineSlipQc.cpp sites. The satellite trace stays the per-satellite record; the decision trace joins it on (week, tow, sat, sig).

### 3.3 Row kinds

`epoch` (one per AR call), `pair` (one per satellite pair and band couple reaching the wide-lane stage), `hold` (memory events), `refloat` (one per refloated satellite).

### 3.4 Columns and their sources

[corrected 2026-11-19: see Corrections, item 3] Column names reuse the observe trace's where the quantity is the same (`ref_sat`, `cmp`, `float_cyc`, `cond_sigma_cyc`, `fixed_int`, `gen_sat`, `gen_ref`; include/realtime/ArObserve.hpp:330-332), so one grader reads both.

1. Identity: `week,tow,kind,outcome,system,sat,ref_sat,sig_hi,sig_lo,elev_sat,elev_ref`. From `buildDdPairs` (@8ca:357-488).
2. Reference: `ref_prev,ref_changed,ref_epochs,held_state`. The first three need a trace-only map from group to (reference, epochs), updated in `buildDdPairs` when armed. `held_state` is 1 when both keys of the pair were in the previous epoch's fixed rows (from the previous resolution's `fixed_dd_rows`). Proposed by the fixing-path read, omitted by the trace-design read; adopted on the skeptic's recommendation.
3. Wide lane: `wl_source` (mw, float, branch), `w_float,w_sigma,dd_wl,wl_reason,r0,r1,sigma_n,gap,persist_s,code_admits`. From `floatWidelane_` (@8ca:1313, called at :1866 and :1883), `WlBranchDecision` (AmbiguityResolver.hpp@8ca:512-518) and the `WLBRANCH` values (:1568-1574). MW source: `wl_int_i`, `wl_int_ref` (:1897-1905).
4. [corrected 2026-11-19: see Corrections, item 3] Narrow lane: `float_cyc` (`y_for_mlambda(k)`, :2197), `cond_sigma_cyc` (sqrt of Q_reduced(k,k) after the floor, :2083-2086), `fixed_int` (`afix_n1(k)`, :2202), `nl_frac` (their difference, :2249), `fixed_int_2nd` (the runner-up vector's entry). `fixed_int_2nd` needs an optional output of `ils::mlambda` back-transforming `E.col(1)` the same way as `E.col(0)` (IlsCore.cpp:249), one integer matrix-vector product. The first-stage `n1_alt` (the rounding neighbour) is dropped: the runner-up of an integer least-squares search is not the per-pair rounding neighbour.
5. Set values on `epoch` rows, new values only: `n_nl,s1,s2,max_pair_frac,worst_pair,partial,n_kept,n_differ_2nd` (sources :2216, :2242, :2248-2253, `pf.kept` at :2270; `n_differ_2nd` counts entries where the two best vectors differ). `ar_ran`, `ar_success`, `num_fixed`, `ratio`, `ar_gate` and `p_succ` stay in closed_loop_trace.csv and are not repeated.
6. Thresholds are not columns. `ratio_threshold`, `ratio_upper_bound`, `min_success_rate`, `nl_max_per_pair_frac`, `wl_max_frac`, `nl_sigma_floor_cyc`, `hold_release_cyc`, `hold_variance`, `elevation_mask_ar` are written once to the run manifest (the trace-design read had them on every `epoch` row; moved on the skeptic's objection).
7. [corrected 2026-11-19: see Corrections, item 3] Arc and slip state for each of the four keys (i_hi, i_lo, ref_hi, ref_lo): `gen`, `gen_stable` (epochs since the generation changed), `arc_epochs,arc_info,pending_reseed`, from `key_gen_`, `arcs_` and `isAmbiguityPendingReseed` (@8ca:211-257). Pair flags `partner_released`, `released_skip` (:1765, :1856). `gen_sat` and `gen_ref` carry the high band's values.
8. Hold memory: `held,held_n1,held_wl,held_since_tow,d_wl,d_if,hold_ok,held_other_ref`. Misfit from `holdMisfit_` (:309-319); `held_since_tow` is a new member of `HeldPairFix` (AmbiguityResolver.hpp@8ca:155); `held_other_ref` marks an entry for sat_i held against a different reference and therefore not checked.
9. Hold write-back: `hold_innov_m`, the innovation `v` passed to `josephPseudoUpdate` at @8ca:2440 (full) and :2328 (partial) for the pair's row, copied into the resolution there. The first-stage source (`holdAmbiguities`, @8ca:3541-3550) is refuted: that function returns at :3506 under mode 1 and the column would always be blank.
10. Corrections, for sat and ref on both signals, filled by the engine: `cmp` (from `wire.cmp`, RealTimeEngineArObserve.cpp:114), `pb_gen` (`hold_generation`, :113-118), `pb_disc` (`discontinuity_counter`, RealTimeEngineSsrDiag.cpp:273-277), `pb_int`, `pb_wli` (`signal_integer`, `wide_lane_integer` of `SsrPhaseBiasEntry`), `pb_step_m` (this epoch's `pb_m` minus the last; one trace-only value per key), `pb_ev` (this epoch's `noteConsumerSatEvent_` names for the key, RealTimeEngineSsrBreaks.cpp:308, :352, :688 and others). `pb_age_s` (now minus `getPhaseBiasTime`, RealTimeEngineSsrDiag.cpp:104) is one value per satellite, since the age is read per satellite, not per signal.
11. Outcome codes. `pair`: `gate_<n>` (numbers from `gate_reason`, include/filter/AmbiguityResolver.hpp:63-76), `released_skip`, `wl_<WlBranchReason>`, `wl_frac`, `wl_no_mw`, `immature` (@8ca:1916), `fixed_full`, `fixed_partial`, `dropped_partial`, `rej_ratio`, `rej_upper`, `rej_ps`, `rej_frac`, `mlambda_fail`. `hold`: `commit` (:513), `check_ok`, `contradicted`, `release_sat` (:1829), `forget_restart` (:284-289), `forget_release` (:1832-1836), `unchecked_refchange` (:1781), `unchecked_unusable` (:1773). `refloat`: `hold_release`, `partner` (RealTimeEngineEstimate.cpp@8ca:286-304).

### 3.5 Events at both edges

The trace treats each information valve of the fixing path as invariant 7 asks: the decoupling edge (a hold committed, a satellite released, a pair skipped or unchecked) and the re-coupling edge (the first search on a new reference, the first epoch after a refloat, the first hold after a reset) each have an outcome code with the identities of both satellites and both signals. The near-constant held integer of invariant 1 has its response visible: `contradicted`, `forget_restart` and the refloat rows are the wired responses, and `unchecked_*` rows are the epochs where no response could fire.

## 4. Cost

- [corrected 2026-11-19: see Corrections, item 5] About one `pair` row per pair reaching the wide-lane stage, roughly 10 to 25 over GPS and Galileo, plus one `epoch` row and occasional `hold` rows (inferred). Roughly 55 columns per `pair` row.
- Nearly all values are already computed. New work per epoch: one map lookup per group, 4 to 6 generation reads and 4 store reads per pair, one runner-up back-transform, and the formatting (about 25 `fmt::format` calls). Small against MLAMBDA and the update (inferred).
- [corrected 2026-11-19: see Corrections, item 5] Size: about 3 000 epochs and 15 to 20 MB per user-day at 30 s; 30 times that at 1 s (inferred, not measured). Off: one bool test.

## 5. Offline read of the three runs

Runs: UTQI00USA E18-E06, DOY 146, 33 epochs, one wide-lane cycle from the float user, present also on today's stream (34 epochs); QUIN G10-G02, DOY 146, 14 epochs, and ASCG G20-G23, DOY 147, 16 epochs, each one narrow-lane cycle away and absent on today's streams (part16.md:142). Each read needs one replay per user with the key on, on the rscb6-equivalent build (main), plus the float user's satellite trace.

For each run, in this order:

1. Reference. `ref_changed`, `unchecked_refchange` or `held_state` = 1 at a new reference within a few epochs before onset. A held state carried across a reference change with `nl_frac` near zero at the first new-reference epoch points to a pulled state accepted under the new key.
2. Correction step. A `pb_gen` or `pb_disc` step, a nonzero `pb_step_m` without a declared event, a `cmp` change (the relabel), `pb_int` 0, or a large `pb_age_s` on either satellite before onset. A step on the reference moves every pair of its group. A step that refloated one signal only shows as a `gen` step on one of the four keys.
3. Early float. `float_cyc` (QUIN, ASCG) or `w_float` (UTQI) near plus or minus 0.5 with small `cond_sigma_cyc` or `w_sigma`; low `arc_epochs` or `arc_info`; `n_differ_2nd` including the pair with `s2/s1` near the threshold; `wl_reason` naming the row of the decision table that admitted the integer (for UTQI, whether the code admitted w0 with no persistence, :1542-1549).
4. End. `contradicted` or `release_sat`, `forget_restart`, a set rejection with no release, or a reset (`gen` step or `ev_amb`) while the pair stayed `fixed_*`.
5. Float comparison. `hold_innov_m`, `d_wl` and `d_if` against the float user's double difference built from both runs' `amb_m`. `d_wl` below `hold_release_cyc` while the float user differs by one cycle means the check confirmed itself through the pulled state.

What the read can claim. A reference change, a declared correction step and an early float are read from the trace. A missed slip is not: by definition there is no reset, no `gen` step and no `ev_amb`, and under the 1e-7 m2 write-back the misfit may stay under 0.25 (inferred). It is inferred only through joins: the pre-fit `innov_phase_m` jump in the satellite trace and the detector statistics (the MW slip trace exists, RealTimeEngineInit.cpp:370, `enable_mw_slip_trace`). A wrong network integer is not separable from an early float inside the user trace either: `pb_int` is the network's claim, not its correctness. It needs `cmp` and `pb_gen` per key joined to the network's own integer record for that satellite and signal, and the float user.

UTQI's E18 and ARHT's E14 (the earlier Galileo wrong holds, part15.md:217) are the two Galileo satellites in eccentric orbits. Whether that matters is untested; `pb_ev` and `innov_phase_m` on E18 would show it.

## 6. Unit cases (no engine run)

1. Header and formatter have the same column count; one fully filled row has fixed text; a blank is distinct from zero for every optional column (as tests/test_consumer_sat_trace.cpp checks for its trace).
2. Each outcome code maps to its word; an unknown value maps to "unknown".
3. Reference memory: groups {G05 at 60 degrees, G10 at 40}, then {G05 at 30, G10 at 50}. The second epoch has `ref_prev` G05, `ref_changed` 1, `ref_epochs` 1.
4. `held_state`: a pair whose two keys were in the previous `fixed_dd_rows` gives 1 under a new reference.
5. A held pair under the old reference gives one `unchecked_refchange` row; an unusable group gives `unchecked_unusable`. Built on the `ArHoldReleaseTestAccess` seam (tests/test_ar_hold_release.cpp@8ca).
6. A generation step on one key gives `forget_restart` for every held pair of that satellite.
7. A misfit above `hold_release_cyc` gives `contradicted` plus `release_sat` for the satellite `chooseReleaseSatellites_` picks; a reference contradicted in two of three pairs gives the reference.
8. `fixed_int_2nd`: a two-ambiguity case with a known runner-up returns it; `n_differ_2nd` counts the differing entries. The best vector is unchanged by the new output.
9. `hold_innov_m` on the mode-1 write-back (full and partial paths) equals the `v` passed to `josephPseudoUpdate`.
10. [corrected 2026-11-19: see Corrections, item 4] Key off: `decisions` is empty and every other member of the resolution matches the key-on resolution (state, covariance, `fixed_dd_rows`, `held_pairs`).

## 7. Steps to build it later

1. Base: main, or chain after chain takes main (the user fixing code is not in 03dfcbc).
2. ArDecisionTrace.hpp (header, row, formatter, outcome words) and its unit cases 1-2.
3. Resolver: the key, the trace-only reference map, the rows at the cited sites, `held_since_tow`, the `hold_innov_m` copy at the write-back; cases 3-7, 9-10.
4. IlsCore: the optional runner-up output; case 8.
5. Engine: correction columns after `resolve`; `ev_amb` at the five slip-reset sites.
6. Drivers: the stream in the replay driver and RunTraces; thresholds into the run manifest; ledger row; Debug compile and `gnss_tests`.
7. Same-build byte identity, key off, on one user replay and one network arm (invariant 6).

Runs needed later (none requested now): one 10-epoch smoke with the key on; six Release user replays with the key on (fixing and float at UTQI00USA and QUIN on the DOY 146 streams, and at ASCG on DOY 147), with UTQI also on today's stream; one key-off replay for the identity check.

## Decisions for the author

1. Build base: main now, or wait for chain to take main.
2. Whether the runner-up output of `ils::mlambda` (a change to a shared search routine, default off) is acceptable.
3. Whether the satellite trace gains the `ev_amb` column (a change to an existing trace's header, which its grader reads).
4. [corrected 2026-11-19: see Corrections, item 2] Whether the two unchecked paths (:1773, :1781) are only traced, or later given a response (re-check a held state under the new reference); the second is a behaviour change and a separate step.
5. The scope of the read: the three runs only, or also ARHT00ATA E14 on the member records (469 and 223 pair-epochs, part15.md:217).

## Not checked

- The exact source commit of rscb6 (inferred equal to 8ca1a20's resolver from main's reproduction).
- Which executable runs the five-day users (rscb6, or a chain/audit build without the resolver keys).
- Whether the users' run configurations override `min_success_rate`, `nl_max_per_pair_frac` or `network.require_signal_integer_for_ar`; run folders were not opened. If `hold_release_cyc` were 0 there, the hold memory and its silent paths did not act.
- The grader `user_fix_vs_float_free.py` and its definition of a held pair-epoch (taken from part15.md:212 and part16.md:142, not from code).
- Whether the three runs began by a code-admitted wide lane or a phase decision, and how they ended (readable from existing logs' `WLBRANCH`, `HOLDREL` and reset lines).
- N_1_float on the partial path (`pf` contents not read).
- Whether the graphic and ionosphere-free slip detectors expose per-key test values on the user side.
- The cost and size figures (inferred, not measured).
- Line numbers @8ca other than those marked verified were taken from the first-stage reads.

## 2026-11-19: corrections from the check against the code

An independent agent checked 44 claims against the code: 35 confirmed (2 with moved lines), 5 wrong, 4 unconfirmed. Trees: "main" is C:/GnssProcessor at 5ddc422 (user-engine code); "7fb0737" is the frozen chain/audit checkout (network code, record). AmbiguityResolver.cpp is byte-identical at 8ca1a20 and 5ddc422, so the draft's "@8ca" lines in that file were checked on main. No build, run or run folder was used. Items 1 to 5 are the claims ruled wrong and carry markers in the body; items 6 to 12 are gaps and unconfirmed claims, without markers.

1. **A chain/audit build and the wide lane (section 1, bullet 4).** The document says a user run on a chain/audit build would take its wide lane from the MW detector (AmbiguityResolver.cpp:532). The code shows the replay sets `setArIgnoreMwWl(!wl_only)` (ApplicationConsumerReplay.cpp:370, 7fb0737); with an empty wide-lane map, `if (!wl_ambiguities.empty())` skips `resolveWlConstrained` (AmbiguityResolver.cpp ~531, 7fb0737) and the per-band search runs. Corrected reading: such a build would have its MW feed suppressed and run the per-band search without a wide-lane constraint. The conclusion (the trace is built on main) stands more firmly.
2. **Exactly two unchecked hold paths (Outcome, 2.5, 2.7 item 4, decision 4).** The document names two silent paths that skip the hold check. The code confirms both (AmbiguityResolver.cpp:1773-1774 and :1779-1781, main) but shows further silent cases: an out-of-range index skip (:1782-1784); a held pair absent from the groups (below the 20 degree mask, unresolvable, no observation or frequency: :419-445, :1714-1720); and every epoch that returns before `resolveWlConstrained` (min_epochs, sigma gate, no pairs: :550-664). The list of unchecked paths, the outcome codes and decision 4 cover all five cases. The sentence "Only the second was named by the first-stage reads" is process text and carries no finding.
3. **Reuse of the observe trace's column names (3.4 head, items 4 and 7).** The document says names are reused only where the quantity is the same. The code shows the observe trace's `gen_sat` and `gen_ref` are the phase-bias `hold_generation` (ArObserve.cpp:274-275, main), while the design uses them for `key_gen_`; the observe trace's `cond_sigma_cyc` is a conditional sigma (ArObserve.hpp:214, main), while the design uses the floored marginal sqrt(Q(k,k)). Either the design's columns are renamed or they carry the observe meanings.
4. **Key-off identity (3.1, unit case 10).** The document says that with the key off no output changes and `held_pairs` is equal on and off. The design contradicts itself: `held_since_tow` is a new `HeldPairFix` member (3.4 item 8), so setting it only when armed breaks case 10 and setting it always contradicts "updated only when armed"; the `ev_amb` column changes the satellite trace header with the key off; the thresholds written to the run manifest are not gated. Required: state whether `held_since_tow` is armed-only and reconcile case 10; with the key off no file is opened, no manifest line is written and the satellite trace header is unchanged, or `ev_amb` moves under the new key. The trace-only memory also includes the previous fixed pairs by key (for `held_state`).
5. **Roughly 55 columns per pair row (section 4).** The design's own list sums to about 94 (11+4+13+5+22+8+1+30), about 102 with the epoch columns. Corrected figure: about 95 columns; the size estimate roughly doubles.
6. **The users' recipe (section 1, bullet 1), unconfirmed.** scripts/irdu_users.py serves the simulated users (USERS list line 42, executable rscb3 line 27). The observed-data users of section 118.1 were run by scripts_s20/runs_nrf46.sh, which is in neither tree and was not read. Corrected reading: the recipe cited is that of the simulated users.
7. **`held_state` from the previous resolution's `fixed_dd_rows` (3.4 item 2), unconfirmed, a design gap.** `fixed_dd_rows` are state indices (AmbiguityResolver.hpp:114, main); the resolver keeps no previous resolution; the engine clears `last_ar_result_` at RealTimeEngineEstimate.cpp:253 (main); after a failed epoch the rows are empty while the state stays pulled. Required definition: both keys in the last accepted fix, by key, held in a trace-only map.
8. **Real-time seam rules (3.2), unconfirmed.** The engine seam file and the owner of the last-`pb_m` map are not named. The project rules require a private member seam (observability: RealTimeEngineSsrDiag.cpp, called from `resolveAmbiguities`) and an engine member, not `RtEpochContext`, for memory that crosses epochs.
9. **Per-epoch order (section 2), incomplete.** RealTimeEngine.cpp:537-584 (main) also has slip detection at :537, before the reactions, and the declared-level-step and ephemeris-jump reactions at :548-549, after the clock re-inflation.
10. **Slip-reset sites (2.6, 3.2).** The five sites are confirmed (RealTimeEngineSlipQc.cpp:131, 151, 155, 185, 267, main); a sixth discontinuity, `noteAmbiguityDiscontinuity` at :163, is not in the `ev_amb` plan.
11. **Runner-up on the partial path (3.4 item 4).** The partial path needs the runner-up from `selectFixableSubset` as well, not only from the full search.
12. **Count of replays (section 7), unconfirmed.** Three receivers by two variants is six replays; UTQI on today's stream adds one or two more.

### Line numbers that moved

- `network.require_signal_integer_for_ar` default: GnssConfigLoadNetwork.cpp:8148-8151 in the document, 8090-8092 on main.
- AR keys in the loader: GnssConfigLoad.cpp:1940-2010 in the document, 1983-2147 on main.
- RunTraces.hpp:531 on main, 529 at 7fb0737.
- The per-epoch order spans RealTimeEngine.cpp:537-584 on main (the document cites 543-584).
- Confirmed in addition (main): N_1_float is unrecorded on the partial path (the `[PAR]` line at AmbiguityResolver.cpp:2322 logs N_1_fixed only); `ils::mlambda` has a sole caller (AmbiguityResolver.cpp:81); the code-bias reset request is applied only with estimateRxSatCodeBias (RealTimeEngineEstimate.cpp:298); persistence runs are also erased on any non-decisive epoch (:1561).

### What stands

- Lead 1: `holdAmbiguities` is a no-op under mode 1; the hold is the `hold_variance` write-back computed in the resolver and installed by the engine (RealTimeEngineEstimate.cpp:280-281).
- Lead 2: the two cited unchecked paths are silent, but they are a subset of five; the trace and decision 4 cover all of them.
- Lead 3: the runner-up of the integer search is computed and discarded (IlsCore.cpp:234, :249).
- The build base is main: a chain/audit build would not even run the wide-lane-constrained search.
- Decision 1 (build base: main now, or after chain takes main) stands.
- Decision 2 (the optional runner-up output of `ils::mlambda`, default off) stands; the partial path needs the same output from `selectFixableSubset`.
- Decision 3 (the `ev_amb` column in the satellite trace) stands, with the note that the column changes output with the new key off unless it moves under that key.
- Decision 4 (trace only, or later a response for unchecked holds) stands with its scope widened to the five silent cases.
- Decision 5 (the three runs only, or also ARHT00ATA E14) stands; the run figures (33/34, 14, 16 epochs; ARHT 469 and 223 pair-epochs) are confirmed.

### Left unconfirmed

- The observed-data users' run configurations (`hold_release_cyc`, `amb_level_check`, `min_success_rate`).
- The grader user_fix_vs_float_free.py and its definition of a held pair-epoch.
- Cost figures other than the column count.
- The IlsCore search sort order beyond IlsCore.cpp:195-206.
- ConsumerSatTrace column positions beyond the header comment.
- Whether the ambiguity level refloat (RealTimeEngineEstimate.cpp:578-584) is another end of a hold for these users.
- The exact source commit of rscb6 (inferred from main's reproduction).
