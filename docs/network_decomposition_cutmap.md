# ApplicationNetwork.cpp — symbol-anchored cut map (INC 0)

*Generated at INC 0 by the anatomy survey against the current working tree (`src/core/app/ApplicationNetwork.cpp`, 7,329 lines). Companion to [network_decomposition_plan.md](network_decomposition_plan.md).*

**Line numbers are ADVISORY** — they drift as edits land. The authoritative reference for every cut boundary is the **start-anchor** (a verbatim unique substring). Re-grep anchors at the start of each increment; a boundary that no longer resolves to a unique symbol is a plan defect to re-derive, not a judgment call.

Total units mapped: **288**, across 15 destinations. `cap*` = touches the RAIM replay channel (`cap_H`/`cap_z`/`cap_priors`/`cap_dense_keys`/`cap_dense_cov`).

## Destination summary (by increment)

| Increment | Destination | Units | Target file |
|---|---|---:|---|
| INC 1 | Config | 0 | GnssConfigLoadNetwork.cpp |
| INC 2 | RunState | 17 | include/filter/NetworkRunState.hpp |
| INC 2 | RunTypes | 9 | include/filter/NetworkRunTypes.hpp |
| INC 2 | RobustStats | 8 | include/filter/NetworkRobustStats.hpp |
| INC 2/6 | SlipQc | 12 | include/slip/NetworkSlipQc.{hpp,cpp} |
| INC 3/4 | RunTraces | 49 | include/logging/NetworkRunTraces.{hpp,cpp} |
| INC 4 | RunContext | 36 | include/app/NetworkRunContext.hpp + src NetworkRunContext.cpp |
| INC 5 | Setup | 15 | src/core/app/ApplicationNetworkSetup.cpp |
| INC 7 | Assembly | 19 | src/core/app/ApplicationNetworkAssembly.cpp |
| INC 7/8 | PhaseDatum | 15 | include/filter/NetworkPhaseDatum.{hpp,cpp} |
| INC 8 | Estimator | 5 | NetworkEstimator ADDITIONS |
| INC 8 | Sbasis | 6 | NetworkSbasis ADDITIONS |
| INC 9 | Raim | 26 | include/filter/NetworkRaim.{hpp,cpp} |
| INC 10 | ClockQc | 19 | include/filter/NetworkClockQc.{hpp,cpp} |
| INC 11 | Emit | 34 | src/core/app/ApplicationNetworkEmit.cpp |
| INC 12 | Orchestrator | 18 | ApplicationNetwork.cpp (SHRUNK, orchestrator only) |

## RunState — INC 2  →  `include/filter/NetworkRunState.hpp`  (17 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 166–176 | struct | `CarryPrior` |  | `/// One shared parameter carried across epochs (D2): its value and pos` | D2 cross-epoch shared-parameter carry: SharedParamKind kind, sat/signal, VtecSh (n,m,trig), value, var, inflate_q flag (clock-monitor soft-large-q rei |
| 182–190 | free-function | `sharedCarryKey` |  | `static std::string sharedCarryKey(SharedParamKind kind` | static free helper: builds the identity key (sat\|CLK / sat\|CB\|signal / sat\|PB\|signal) mirroring NetworkEstimator's internal key so SharedCarryCov |
| 204–208 | struct | `SharedCarryCov` | ✓ | `struct SharedCarryCov {` | C1 (network.shared_carry_full_cov) dense posterior covariance side-car over D2-carried Clock/PhaseBias/CodeBias entries: identity-key→row map + dense  |
| 215–219 | struct | `LocalCarry` |  | `/// One carried-forward ambiguity (phase-leveling, Increment 1): the f` | Phase-leveling Increment-1 per-ambiguity carry: value_abs (absolute float ambiguity, m), posterior var, last_seen_s (continuous-GPS for gap inflation  |
| 225–231 | struct | `PlAmb` |  | `/// One present, carry-eligible (non-pivot) ambiguity this epoch, reco` | PASS-B present-ambiguity record for the post-solve harvest/stash: A_r col, sat, signal, x_lin (this-epoch linearization point), amb_key (clears one-sh |
| 235–238 | struct | `PlZwd` |  | `/// One present station ZWD column this epoch (B9): its A_r column and` | B9 per-station ZWD present-record (A_r col + x_lin linearization point) mirroring PlAmb, for the post-solve ZWD carry harvest/stash. No cap*. |
| 299–307 | struct | `ClockOsc` |  | `struct ClockOsc {` | Two-state (offset x0, drift x1) Zucca–Tavella oscillator clock carry state: covariance p00/p01/p11, t_last (P0-3 true per-sat elapsed-time base — 0=ne |
| 310–310 | struct | `ClockOscPred` |  | `struct ClockOscPred { double x0, x1, p00, p01, p11; };` | One-step predicted ClockOsc mean+cov (Φ·x, Φ·P·Φᵀ+Q). Plain POD, no member initializers (uninitialized until clockOscPredict fills it) — a hazard if e |
| 1073–1082 | phase-block | `closedloop_metrics_state` |  | `// Closed-loop metrics + trace.` | Declares closed-loop accumulator state: Eigen::Vector3d cl_truth + Coordinates::Llh cl_truth_llh (ecefToLlh guarded on closed_loop_on), counters cl_ep |
| 1084–1087 | phase-block | `process_noise_carry_constants` |  | `// D2 process noise (variance growth per second) for the carry-forward` | const q_clock (sat_clock), q_pb (sat_phase_bias), q_cb (sat_code_bias W6) from config_.getProcessNoise(). The per-second variance-growth constants for |
| 1485–1489 | banner-section | `local_carry (phase-leveling Increment-1)` |  | `Phase-leveling per-station ambiguity carry (Increment 1)` | THE Increment-1 comment. std::map<std::tuple<int,std::string,std::string>,LocalCarry> local_carry; keyed (station idx, sat, signal); ORDERED for byte- |
| 1490–1492 | banner-section | `zwd_carry (B9)` |  | `Per-station ZWD carry (B9): persistent across` | std::map<int,LocalCarry> zwd_carry keyed by station index; inert unless carry_zwd. PlZwd/LocalCarry carry struct. |
| 2508–2515 | phase-block | `phase-leveling carry timing (pl_now_s / carr` |  | `// Phase-leveling carry timing (Increment 1): continuous-GPS seconds` | Increment-1 carry warmup gate. pl_now_s = TimeUtils::continuousGpsSeconds(se.time); carry_warm = pl_on && n_emitted>=pl_warmup. n_emitted counts EMITT |
| 3549–3660 | phase-block | `phase_leveling_carry_build` |  | `// --- Phase-leveling carry: record present ambiguities + build the` | Increment-1 diagonal carry: per non-anchored present ambiguity records PlAmb + (if warm carry, not stale, not pending-reseed, finite) builds Λ=1/var_e |
| 3649–3650 | lambda | `inj_sort_cmp` |  | `std::sort(inj.begin(), inj.end(),` | Ascending-column comparator on inj tuples → deterministic rank-1 update sequence for the carry inject. Captureless. Inside OMP body. |
| 5932–6004 | phase-block | `phase-leveling carry harvest (Increment 1, a` |  | `// --- Phase-leveling carry: harvest posterior + stash (Increment 1)` | if(carry_warm): per present non-pivot(or former pivot under phase_datum_prioritized) station harvest R.pl_amb via reconstructStation + conditionalLoca |
| 6517–6527 | phase-block | `ClockOsc seed branch (first warm / vanish-re` |  | `if (pit == clk_pred.end() \|\| !osc.init) {` | Seeds ClockOsc from solved z: x0=z,x1=0,p00=1,p01=0,p11=1e-6,t_last=pl_now_s,init=true; no fault test until it has a prediction. ClockOsc predict-upda |

## RunTypes — INC 2  →  `include/filter/NetworkRunTypes.hpp`  (9 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 117–161 | struct | `NetStation` |  | `/// Per-station runtime bundle: an independent single-receiver StateMa` | Per-station runtime bundle: name/ecef/is_pivot, unique_ptr StateManager/PppModel/MeasurementModel, PppState, MW slip (mw_slip/turbo_edit), IF-phase sl |
| 245–263 | struct | `StationAssembly` |  | `/// PASS B produce-phase result for one station: the assembled local J` | PASS-B per-station produce-phase result written by exactly one thread (indexed slot — determinism): ok/empty_innov/state_mismatch/threw flags, A_s/A_r |
| 1420–1432 | struct | `EpochStatusRow` |  | `struct EpochStatusRow {` | Local struct with std::optional<double/int> fields (sigma0,n_adapt,n_present,n_clock_sats,clock_median_m,clock_std_m,worst_dev_m,n_refloat_sats,n_retr |
| 1433–1434 | helper | `es_sigma0_trigger / es_withhold_ceiling` |  | `const double es_sigma0_trigger   = config_.networkRaimSigma0Trigger();` | Two const doubles (config echoes) written into every EpochStatusRow emission; RaimSigma0Trigger + RaimWithholdSigma0. |
| 5835–5835 | struct | `AmbAbs (DD-frac local struct)` |  | `struct AmbAbs { double value_m; };` | Local POD holding absolute float ambiguity (m) for DD-frac pool. Trivial one-field struct scoped to the trace block. |
| 6037–6037 | struct | `AmbStat (RAIM Pass-1 per-sat stat)` |  | `struct AmbStat { std::string sat; int sta; double r_max; };` | Local POD: per (station,sat) largest measurement-standardized post-fit residual for RAIM consensus. amb_stats vector. |
| 6051–6051 | struct | `Cell (RAIM attribution input cell)` |  | `struct Cell { int sta; std::string sat, sig; double r; };` | Local POD: per (station,sat,signal) carried-ambiguity residual, feeds median-polish attribution. Built when need_attrib (attrib_on\|\|attrib_act\|\|tr |
| 6121–6121 | struct | `RaimDetail (Pass-2 trace detail)` |  | `struct RaimDetail { std::string sat; int sta; double s_raw, s_norm; bo` | Local POD: per (station,sat) raw/normalized residual + spiked flag; built only when raim_trace open. |
| 6121–6121 | struct | `RaimDetail` |  | `struct RaimDetail { std::string sat; int sta; double s_raw, s_norm; bo` | Local POD struct declared inside the per-epoch loop body for RAIM tracing rows (sat, sta, s_raw, s_norm, spiked). Single-line definition. Consumed by  |

## RobustStats — INC 2  →  `include/filter/NetworkRobustStats.hpp`  (8 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 281–285 | free-function | `medianCopy` |  | `inline double medianCopy(std::vector<double> v) {` | inline: upper-middle median of a BY-VALUE copy (nth_element on v.size()/2). The upper-middle (not averaged) convention is load-bearing for byte-identi |
| 351–364 | free-function | `robustProminence` |  | `std::map<KeyT, double> robustProminence(const std::map<KeyT, double>& ` | TEMPLATE (template<class KeyT>) — must live in a HEADER, not a .cpp, when decomposed, or instantiations break. Robust z-score prominence z=(effect−med |
| 368–373 | struct | `MedianPolish` |  | `/// Result of a Tukey 2-way median polish: value ≈ overall + row_eff[r` | Result of the Tukey 2-way median polish: overall, row_eff (int station→effect), col_eff (string sat→effect), interaction vector (per-cell leftover, pa |
| 381–428 | free-function | `medianPolish2Way` |  | `inline MedianPolish medianPolish2Way(` | inline: Tukey median polish over sparse (int row, string col, double val) cells, iters=5 default. Byte-identity hazards: (a) fixed iteration count and |
| 394–399 | lambda | `med_idx (lambda in medianPolish2Way)` |  | `auto med_idx = [&](const std::vector<std::size_t>& idx) {` | Local lambda inside medianPolish2Way. [&] captures the enclosing R vector by reference and reads it; no writes, no other captures used. Moves with its |
| 5240–5243 | lambda | `medianOf` |  | `auto medianOf = [](std::vector<double>& v) {` | Local median helper nested inside the RT0-5 runaway block. Empty capture clause — pure function of its argument. Used for per-const dev/rate medians.  |
| 5828–5834 | lambda | `sysOfChar` |  | `auto sysOfChar = [](char c) -> System {` | Capture clause: [] (none). Pure char→System switch (G/E/C/J/S/default Unknown). No captures, no capstar. Local to DD-frac trace but a reusable free-he |
| 7194–7194 | lambda | `gaussAlpha` |  | `auto gaussAlpha = [](double k) { return std::erfc(k / std::sqrt(2.0));` | Stateless [] no-capture pure helper: two-sided Gaussian tail P(\|Z\|>k)=erfc(k/sqrt2). Called by the `emit` lambda. Prompt lists gaussAlpha explicitly |

## SlipQc — INC 2/6  →  `include/slip/NetworkSlipQc.{hpp,cpp}`  (12 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 146–149 | struct | `NetStation::IfPairScale (nested)` |  | `struct IfPairScale {` | Nested per-signal-pair EWMA scale (ewma_scale + clean-sample count) for IF-phase slip thresholding. Logically SlipQc, but physically nested inside Net |
| 273–278 | free-function | `ifElevMap` |  | `inline double ifElevMap(double el_deg) {` | inline: IF-phase slip elevation mapping 1/sin(el), clamped at 5° (kMinElDeg), floor 1e-3 → returns 1.0. Normalizes DD phase noise to zenith-equivalent |
| 434–444 | free-function | `parseIfPairResetKey` |  | `inline bool parseIfPairResetKey(const std::string& key, std::string& s` | inline: parses an IfPhaseSlipDetector reset key 'SAT_SIG1_SIG2' (e.g. G01_L1C_L2W) into sat/sig1/sig2 by two underscore finds; false on malformed (mis |
| 2672–2725 | phase-block | `phase-leveling T0 drain (integer REPAIR/LEAV` |  | `// Phase-leveling T0 (serial, station-ordered → deterministic), applie` | T0 serial drain. Reads t0_frac_max/t0_dn_max from config_ (2695-2696). For each present station's sta_repairs[i]: looks up local_carry[(i,sat,signal)] |
| 2740–2796 | phase-block | `MW-detect refloat drain` |  | `if (config_.networkCycleSlipMwDetect()) {` | Guarded by networkCycleSlipMwDetect(). Serial station-ordered. (a) consumes sta_mw_refloat[i] confident WL integer slips: refloat_signal on both sigs, |
| 2741–2755 | lambda | `refloat_signal` |  | `auto refloat_signal = [&](int i, const std::string& sat,` | MW-detect refloat helper. Guard: if !carry_warm return false (cold = diagnostic only). Iterates sm.getAmbiguities(), parses key via StateKeys::parseAm |
| 2831–2836 | phase-block | `IF-phase TD driver config hoist` |  | `if (config_.networkCycleSlipIfPhaseTd()) {` | Opens IF-phase TD block (networkCycleSlipIfPhaseTd). Reads k_mad, floor_frac, min_pool, ewma_alpha, shrink_kappa from config_ (networkCycleSlipIfPhase |
| 2840–2850 | lambda | `refloat_pair` |  | `auto refloat_pair = [&](int i, const std::string& sat,` | IF-TD refloat helper. Guard: if !carry_warm return (cold = diagnostic). Iterates sm.getAmbiguities(), parseAmbiguityKey, matches sat and (sig1\|\|sig2 |
| 2852–2866 | phase-block | `IF-TD by_pair pooling` |  | `// Pool this epoch's ΔΔφ_IF by signal pair across present non-pivot st` | Builds std::map<string,vector<PairSample>> by_pair from stations[i].if_slip->getLastIfJumps() over present non-pivot stations. Filters !valid/non-fini |
| 2855–2855 | struct | `PairSample struct` |  | `struct PairSample { int sta; std::string sat, sig1, sig2;` | Local POD: {int sta; string sat,sig1,sig2; double dd,el,lambda}. Pooling record for IF-TD by-signal-pair. Function-local struct — hoisting to RunTypes |
| 2868–2956 | phase-block | `IF-TD per-pair detection loop` |  | `int n_if_arcs = 0, n_sparse = 0, n_warm_sp = 0;` | Core IF-TD driver. Per pair: (1) common-mode center med = median(raw dd) via nth_element; (2) pooled robust scale s_pool=1.4826*median(\|z\|), z=(dd-m |
| 2958–2983 | phase-block | `IF-TD refsat common-mode drain` |  | `// Reference-satellite common-mode slips (per station; pool can't see ` | Folds detector's own refsat common-mode reset keys from sta_if_resets[i]: parseIfPairResetKey(rkey,sat,sig1,sig2) [free helper defined out of range],  |

## RunTraces — INC 3/4  →  `include/logging/NetworkRunTraces.{hpp,cpp}`  (49 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 808–835 | phase-block | `station_roster_viz_csv` |  | `// --- Station roster (viz) ------------------------------------------` | Guarded on nc.trace_stations. Writes network_stations.csv (station,role,ecef xyz) via std::ofstream roster; lists held-out consumer as role 'held_out' |
| 1089–1120 | phase-block | `trace_stream_declarations_block` |  | `// --- Trace ---------------------------------------------------------` | Banner section 'Trace'. Opens the block of std::ofstream + path declarations for every network CSV. Within 448-1120: std::ofstream trace + trace_path  |
| 1120–1128 | banner-section | `network_if_slip / raim / raim_attrib trace s` |  | `pre-detector's per-(station,sat,pair) ΔΔφ_IF and full threshold` | std::ofstream + const path decls for network_if_slip.csv, network_raim.csv, network_raim_attrib.csv. Banner comment leads (continues from before 1120) |
| 1129–1133 | banner-section | `clk_mon_trace stream decl` |  | `Per-satellite clock-monitor diagnostics (network.clock_monitor` | clk_mon_trace ofstream + clk_mon_trace_path for network_clock_monitor.csv. Gated later by trace_slip_events && (clockMonitor\|\|clockModel). |
| 1134–1139 | banner-section | `anchor_info_trace stream decl (B7)` |  | `B7: per-satellite a-priori-clock-anchor information fraction` | anchor_info_trace + path network_anchor_info.csv; diagnostic (trace_anchor_info). |
| 1140–1146 | banner-section | `dd_frac_trace stream decl + dd_ppp_utils (B5` |  | `B5: cross-station between-satellite DD float ambiguities` | dd_frac_trace + path network_dd_fractional.csv, plus std::unique_ptr<PppUtils> dd_ppp_utils (wavelength provider, built only when trace on). |
| 1147–1154 | banner-section | `slip_raw_trace stream decl + slip_raw_ppp_ut` |  | `RAW cycle-slip observables per (station,sat) every epoch` | slip_raw_trace + path network_slip_raw.csv + std::unique_ptr<PppUtils> slip_raw_ppp_utils. Independent of any detector. |
| 1155–1159 | banner-section | `epoch_status_trace stream decl (L1)` |  | `L1: per-epoch disposition + σ̂0 + clock-stats trace` | epoch_status_trace + path network_epoch_status.csv (trace_epoch_status). |
| 1160–1176 | banner-section | `health_trace stream decl + kHealthHeader (RT` |  | `RTB-6: operator health heartbeat (network.health_interval_s > 0)` | health_trace ofstream + path network_service_health.csv + static constexpr const char* kHealthHeader string literal (week,tow,n_emitted,...). Feeds Fl |
| 1177–1181 | banner-section | `events_trace stream decl (L2)` |  | `L2: structured per-event QC log (network.trace_events)` | events_trace + path network_events.csv (trace_events). |
| 1182–1185 | banner-section | `zwd_trace stream decl (L7)` |  | `L7: per-station ZWD trace (network.trace_zwd)` | zwd_trace + path network_zwd.csv (trace_zwd). |
| 1186–1189 | banner-section | `station_res_trace stream decl (L13)` |  | `L13: per-station residual field (network.trace_station_residuals)` | station_res_trace + path network_station_residuals.csv (trace_station_residuals). Heavy opt-in. |
| 1190–1415 | phase-block | `trace-open block (create_directories + heade` |  | `std::error_code ec;` | Braced scope: create_directories(log_dir_), then conditional open()+header-write for trace, soln, iono, vtec_sh, cl (closed-loop), if_slip/raim/raim_a |
| 1569–1709 | phase-block | `run-manifest writer (L15)` |  | `L15: run-provenance manifest (network.write_run_manifest)` | Guarded by nc.write_run_manifest. Contains jesc lambda; FNV-1a-64 cfg_hash over config_path_ bytes (ifstream binary read loop, seed 146959810393466560 |
| 1576–1585 | lambda | `jesc (manifest JSON-escape lambda)` |  | `auto jesc = [](const std::string& s) {` | Inside `if (nc.write_run_manifest)` block. Escapes backslash/quote/\n/\r for hand-emitted JSONL. Captures: [] (empty — no capture); pure by-value para |
| 1743–1755 | struct | `FlagRate struct + B8 flag-rate counters` |  | `--- B8 empirical-alpha flag-rate counters (network.trace_flag_rates)` | const bool trace_flag_rates=nc.trace_flag_rates; struct FlagRate{long opp=0,flag=0;}; then 7 instances fr_if_slip(T1),fr_we_trigger(T2),fr_raim_cell(T |
| 2100–2105 | phase-block | `t_loop_begin / n_loop_epochs [TIMING]` |  | `// [TIMING] Wall-clock of just the per-epoch loop (excludes ~startup: ` | Timing instrumentation decls: steady_clock::now() loop-begin anchor + n_loop_epochs counter for the per-epoch scaling metric; counts only epochs past  |
| 2377–2401 | phase-block | `P1 CSV rotation block` |  | `// P1: size-based rotation of the unbounded service CSVs` | P1 size-based rotation (network.trace_rotate_mb>0): local anonymous struct {ofstream* os; path* path; const char* header;} rotating[] over {trace/trac |
| 2403–2455 | phase-block | `RTB-6 health heartbeat write` |  | `// RTB-6: operator health heartbeat every network.health_interval_s of` | RTB-6 health CSV heartbeat: guarded on health_trace.is_open(); every nc.health_interval_s of content time writes a row with week/tow, n_emitted/n_skip |
| 2458–2486 | lambda | `emitEpochStatus` |  | `// L1: write one network_epoch_status.csv row for this epoch (guarded;` | L1 epoch-status writer lambda; guarded on epoch_status_trace.is_open() (no-op OFF). By-ref [&]; params (const char* disposition, const EpochStatusRow& |
| 2458–2486 | lambda | `emitEpochStatus` |  | `// L1: write one network_epoch_status.csv row for this epoch` | L1 status-row writer. Guarded no-op when epoch_status_trace not open. Reads EpochStatusRow optionals (sigma0,n_adapt,n_present,n_clock_sats,clock_medi |
| 2488–2503 | lambda | `emitEvent` |  | `// L2: write one network_events.csv row for a discrete QC action (guar` | L2 events writer lambda; guarded on events_trace.is_open() (no-op OFF). By-ref [&]; params (tier,event,sat,station,optional statistic,optional thresho |
| 2489–2503 | lambda | `emitEvent` |  | `// L2: write one network_events.csv row for a discrete QC action` | L2 event-row writer. Guarded no-op when events_trace not open. Sanitizes detail commas->semicolons for single CSV field. Takes no GpsTime — uses captu |
| 2987–3047 | phase-block | `RAW cycle-slip observables trace` |  | `// RAW cycle-slip observables (network.trace_slip_raw), computed strai` | network.trace_slip_raw diagnostic writer. Guarded by slip_raw_trace.is_open() && slip_raw_ppp_utils. Independent of armed detectors; written BEFORE ob |
| 3007–3007 | lambda | `raw-slip band-sort comparator` |  | `[](const std::string& a, const std::string& b) { return a[1] < b[1]; }` | Inline std::sort comparator ordering phase signals by band char (a[1]). Trivial captureless lambda inside the raw-slip trace block. Moves with RunTrac |
| 5624–5633 | phase-block | `emit counters advance (n_emitted/first_emit/` |  | `if (n_emitted == 0) first_emit_content_s = pl_now_s;` | first_emit_content_s anchor (RTB-7c), ++n_emitted, last_emit_content_s (RTB-6). R12: commits B8/T2 within-epoch-trigger flag-rate fr_we_trigger.opp/fl |
| 5679–5695 | phase-block | `per-satellite product trace (soln CSV)` |  | `// Per-satellite product trace (after emit, so discontinuity counters` | if(soln): per sat CLK/PB/CB rows with emitter.clockDiscontinuityCounter / discontinuityCounter / codeBiasDiscontinuityCounter (A2: 0 when off). Read-o |
| 5697–5733 | phase-block | `network-estimated slant-iono viz trace` |  | `// --- Network-estimated slant ionosphere (viz trace)` | if(iono_trace.is_open()): per station est.reconstructStation(est_index_of[i], x_s); build elaz from modeled[i]; per iono state via asm_res[i].h2local  |
| 5735–5767 | phase-block | `per-station ZWD trace (L7)` |  | `// --- Per-station ZWD trace (L7)` | if(zwd_trace.is_open()): locate ZWD col via getTropoIndex/h2local (NOT R.pl_zwd_present); est.reconstructStation + est.conditionalLocalVarianceDiag({z |
| 5769–5815 | phase-block | `a-priori-clock-anchor information-fraction d` |  | `// --- A-priori-clock-anchor information-fraction diagnostic (B7)` | if(anchor_info_trace.is_open()): anchor_info=1/σ²; per registered_clock_sats read est.reducedSharedNormals() diag total_info, clk_prior_info carry_inf |
| 5817–5913 | phase-block | `cross-station DD fractional-residual trace (` |  | `// --- Cross-station DD fractional-residual (B5)` | if(dd_frac_trace.is_open()): builds pool[(const,signal)][sat][sta]=abs float amb via reconstructStation; skips pivot(legacy)/gauge-pinned(isGaugePinne |
| 5915–5930 | phase-block | `co-estimated SH-VTEC coefficient trace` |  | `// --- Co-estimated SH-VTEC coefficient trace` | if(vtec_sh_trace.is_open() && sh_model): per sh_model->coeffs() est.vtecCoeffColumn(n,m,trig); val=x_s(col); σ from est.sharedCovariance() diag; emit  |
| 6155–6170 | phase-block | `RAIM consensus context trace` |  | `// Trace the consensus context: every station of a refloated sat` | if(raim_trace.is_open()): per raim_details cell where fset.count(sat)\|\|spiked, emit s_raw/s_norm/σ̂0/raim_k/spiked/sat_flag_stations/raim_min_sta/in |
| 6159–6170 | phase-block | `raim_trace consensus-context writer` |  | `// Trace the consensus context: every station of a refloated sat` | When raim_trace.is_open(): iterate raim_details, emit a row per (station of a refloated sat OR spiked near-miss). Reads raim_details, fset, sat_flag_s |
| 6274–6316 | phase-block | `attribution trace emission (SAT/STA/SIG/ARC)` |  | `if (attrib_on) {` | Guarded by attrib_on. Builds sat_n/sig_n/sta_n counts, defines emit lambda, emits SAT/STA/SIG rows above kAttribReportProm, sorts interaction by \|val |
| 6278–6286 | lambda | `attribution trace emit lambda` |  | `auto emit = [&](const char* dim, const std::string& entity, double eff` | Capture [&]. Reads by-ref: raim_attrib_trace (writes to stream), se.time.week/tow, mp.overall, sigma0. Params dim/entity/effect/prom/n/acted. Writes C |
| 6278–6286 | lambda | `emit (attribution trace lambda)` |  | `auto emit = [&](const char* dim, const std::string& entity, double eff` | Capture [&] (by-ref, full closure). Reads by-ref: raim_attrib_trace (writes to stream), se.time, mp.overall, sigma0. Params: dim, entity, effect, prom |
| 6300–6302 | lambda | `interaction sort comparator` |  | `std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b` | Inline std::sort comparator, capture [&]. Reads mp.interaction (by-ref) to order arc indices by descending \|interaction\|. Params a,b (size_t indices |
| 6847–6861 | phase-block | `clock-spread trace row (network trace)` |  | `// --- Trace ---------------------------------------------------------` | Computes sumsq/maxabs/rms over registered_clock_sats x_s(clockColumn); if trace open writes se.time, n_present, n_clock, n_pb, plan.declared_dimension |
| 6847–6861 | phase-block | `emitted-epoch clock-rms trace row` |  | `// --- Trace ---------------------------------------------------------` | Computes sumsq/maxabs/rms over registered_clock_sats x_s(clockColumn); writes CSV row to `trace` stream (week,tow,n_present,n_clock,n_pb,dim,rms,maxab |
| 6862–6896 | phase-block | `L1 emitted-epoch disposition status row` |  | `// L1: the emitted-epoch disposition row — the median+std+worst-sat cl` | Guarded epoch_status_trace.is_open(). Builds EpochStatusRow es (sigma0_adapt, n_adapt, n_present, n_clock, n_refloat=clock_refloat_sats.size, n_retro= |
| 6862–6896 | phase-block | `L1 EpochStatusRow emit (emitted)` |  | `// L1: the emitted-epoch disposition row — the median+std+worst-sat cl` | Builds EpochStatusRow es (sigma0_adapt,n_adapt,n_present,n_clock,refloat/retro counts,datum_changed) + median/std/worst-sat over registered_clock_sats |
| 7010–7019 | phase-block | `epoch-loop timing measurement` |  | `double loop_ms = 0.0;   // L3/L15: hoisted out of the timing block` | loop_ms hoisted (read later by run-summary CSV / manifest). Computes wall from t_loop_begin, log->info timing line (ms/epoch, stations, PARALLEL/SERIA |
| 7021–7033 | phase-block | `trace-stream teardown (close all)` |  | `if (trace) trace.close();` | Closes every open trace/log stream: trace, soln, iono_trace, vtec_sh_trace, cl_trace, if_slip_trace, raim_trace, raim_attrib_trace, epoch_status_trace |
| 7035–7056 | phase-block | `producer-complete summary log` |  | `log->info("Network producer complete: {} epochs emitted, {} skipped "` | End-of-run log->info/warn block: emitted/skipped/gate/sigma0 counts, RT0-5 clock-runaway warn, prioritized-datum heal info (n_healed_epochs/cols), anc |
| 7058–7073 | phase-block | `L15 end-of-run manifest record` |  | `// L15: append the end-of-run manifest record (JSONL` | Gated on nc.write_run_manifest. Reopen-append JSONL 'complete' record to manifest_path with n_synced/emitted/skipped/gate/sigma0/perconst/loop_ms. Man |
| 7189–7249 | banner-section | `trace_flag_rates B8 summary block` |  | `// --- B8: empirical false-alarm-rate summary per QC tier` | Gated on trace_flag_rates. Contains nested lambdas gaussAlpha + emit; opens network_flag_rates.csv; emits T1..T6 FlagRate rows (fr_if_slip/fr_we_trigg |
| 7204–7220 | lambda | `emit (flag-rate row)` |  | `auto emit = [&](const char* tier, const char* label, const FlagRate& f` | Captures [&] by-ref; reads log (log->info), fr_csv (ofstream), gaussAlpha (the sibling lambda). Computes rate=flag/opp; logs two format variants (has_ |
| 7232–7248 | phase-block | `network_run_summary.csv writer` |  | `// L3: run-summary CSV (same toggle).  ms_per_epoch uses n_loop_epochs` | Nested in the trace_flag_rates block. Writes network_run_summary.csv header + one row (synced/emitted/skipped/gate/sigma0/loop_ms/ms_per_epoch/station |

## RunContext — INC 4  →  `include/app/NetworkRunContext.hpp + src NetworkRunContext.cpp`  (36 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 837–873 | phase-block | `parallelism_thread_planning` |  | `// === Per-station compute parallelism ===` | Banner section. Computes int hw_max_threads (omp_get_max_threads under _OPENMP), n_sta_total, const bool parallel_on (nc.parallel_stations && n_sta_to |
| 875–889 | phase-block | `logger_prewarm` |  | `// Prewarm every logger the per-station model path can touch.` | Thread-safety hazard mitigation: when parallel_on, single-threaded getLogger() for every logger name the per-station tree touches (ppp_model, geometry |
| 1436–1440 | banner-section | `Epoch-loop persistent carry decls: prev_carr` |  | `=== Epoch loop ===` | Banner '=== Epoch loop ==='. std::vector<CarryPrior> prev_carry (D2 stash), SharedCarryCov prev_carry_cov (C1 dense cov side-car — touches C1 dense ca |
| 1441–1445 | banner-section | `last_pivot (A1)` |  | `A1: persistent last-EVER pivot per constellation` | std::map<std::string,std::string> last_pivot; MERGE-updated, read only when pivot_vanish_return_signal on. Datum-change detection state. |
| 1505–1515 | banner-section | `warmup ramp state: ura_ewma_updates / ramp_l` |  | `RTB-7(b/c): startup quality-ramp state` | size_t ura_ewma_updates, bool ramp_lifted, double first_emit_content_s=-1, double grade_metric_s=-1. Warmup URA floor lift + cold-start-grade; restore |
| 1516–1523 | phase-block | `phase-leveling knob consts: pl_on/pl_q_amb/p` |  | `const bool   pl_on         = config_.networkPhaseLeveling();` | Const config-knob hoist block (Knobs): pl_on,pl_q_amb,pl_warmup,pl_max_gap_s,pl_var_floor,pb_var_floor(B4 D2 PB floor),cb_var_floor(B4 D2 CB floor),fu |
| 1524–1529 | phase-block | `ZWD/URA knob consts: zwd_on/zwd_q/ura_on/ura` |  | `const bool   zwd_on        = config_.networkCarryZwd();` | Const knob hoist: zwd_on,zwd_q (B9 ZWD carry Q), ura_on,ura_alpha,ura_retro,ura_refloat_inflate (B1). |
| 1530–1534 | phase-block | `RAIM knob consts: raim_on/raim_k/raim_min_st` |  | `const bool   raim_on       = config_.networkRaim();` | Const knob hoist for RAIM tier: raim_on,raim_k,raim_min_sta,raim_withhold(A4 σ̂0 ceiling),raim_redun(B2 Baarda r_i normalize). Consumed by Raim destin |
| 1535–1540 | phase-block | `clock-monitor knob consts: clk_mon_on/abs/ju` |  | `const bool   clk_mon_on    = config_.networkClockMonitor();` | clk_mon_on,clk_mon_abs,clk_mon_jump,clk_mon_couple (on&&coupleRaim),clk_mon_reinit_sig,clk_mon_reinit_var (=sig*sig computed-initializer — byte-identi |
| 1541–1555 | phase-block | `datum/handover/RT knob consts: pivot_vr_sign` |  | `const bool   pivot_vr_signal = config_.networkPivotVanishReturnSignal(` | Const knob hoist: pivot_vr_signal(A1),emit_clock_disc(A2),sticky_pivot(RTA-1),clock_sum_to_zero(anchor-free gauge),scoped_handover(RTA-2),emit_gen_tim |
| 1556–1567 | phase-block | `clock-model knob consts: clk_model_on/q1/q2/` |  | `Two-state (offset+drift) oscillator clock model (network.clock_model):` | clk_model_on,clk_model_q1,clk_model_q2,clk_model_k,clk_model_r (=MeasStd*MeasStd computed-initializer B3 σ_floor², byte-identity product-order), clk_m |
| 1710–1719 | banner-section | `epoch-loop scalar state: prev_tow/prev_week/` |  | `double prev_tow = -1.0;` | prev_tow,prev_week,iod; generation_id (RTA-3 minted at first emit, uint32_t, 0=unset); last_contract_emit_s (RTA-5 S-contract writer, -1 until open).  |
| 1720–1741 | banner-section | `tally counters + health-record state` |  | `size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0, n_runaway_ep` | ServiceCounters: n_emitted/n_skipped/n_gate_withheld/n_runaway_epochs; RTB-6 health state last_health_write_s/last_emit_content_s/last_n_present/last_ |
| 2134–2145 | phase-block | `snap_path / live_fingerprint decls` |  | `// === RTB-7(a): state snapshot + warm resume ========================` | RTB-7(a) banner section. Declares snap_path (resolved rel→log_dir_), last_snapshot_write_s, live_fingerprint. Entirely outside product-file replay pat |
| 2153–2153 | lambda | `fingerprint add helper (fp += ...)` |  | `auto add = [&fp](const std::string& v) { fp += v; fp += '\x1f'; };` | Config-fingerprint accumulator lambda captured [&fp] BY-REF on local std::string fp: appends v + 0x1f unit-separator. Used across lines 2154-2183 to h |
| 2186–2341 | phase-block | `warm-resume restore block` |  | `if (!snap_path.empty() && std::filesystem::exists(snap_path)) {` | RTB-7 warm-resume: reads NetworkStateSnapshot via NetworkStateSnapshotIO::read; computes first_live_s from synced (start_offset fast-forward) + horizo |
| 3382–3402 | phase-block | `begin_epoch_and_cap_decls` | ✓ | `est.beginEpoch();` | est.beginEpoch(); then DECLARES the within-epoch-adaptation capture vars: cap_H, cap_z, cap_constraint_weight, cap_has_constraint, cap_priors, cap_den |
| 3957–3975 | lambda | `datum_changed / pivotHandoverSignalled + han` |  | `// Pivot handover: the previous epoch's shared estimate` | Computes datum_changed via pivotHandoverSignalled(plan.clock_pivot, prev_pivot, last_pivot, pivot_vr_signal, changed_consts). handover_affects lambda  |
| 4373–4421 | phase-block | `D2 carry-forward prior — diagonal branch` | ✓ | `// --- D2 carry-forward prior (diagonal, inflated over the gap)` | Banner 4373-4384 (post-gate/pre-solve placement, RTA-2 scoped skip). Guard: if((!datum_changed\|\|scoped_handover)&&!prev_carry.empty()&&!fullcov_on). |
| 4422–4528 | phase-block | `D2 carry-forward prior — C1 full-covariance ` | ✓ | `} else if ((!datum_changed \|\| scoped_handover) && !prev_carry.empty(` | C1 dense info-form carry Λ=(P_prev+Q)^-1 via est.addSharedPriorDense. Same entry selection/skip/var_eff surgery as diagonal branch (VtecSh skip, hando |
| 4500–4528 | phase-block | `C1_dense_carry_fold_finalize` |  | `for (std::size_t a = 0; a < kd; ++a)` | Tail of the C1 dense D2 covariance-fold try/catch (network.shared_carry_full_cov). On success stashes cap_dense_keys=dkeys, cap_dense_cov=cov_eff for  |
| 5152–5175 | phase-block | `emit_stamp_generation_id` |  | `// RTA-3: stamp the per-run solution-generation id, minted once from t` | RTA-3 solution-generation id mint. Gated `if (emit_gen_id)`: if generation_id==0 mint = week*604800 + floor(tow), clamp to 1 in week-0 corner, log->in |
| 5499–5508 | phase-block | `handover baseline advance (prev_pivot/last_p` |  | `// The handover-detection baseline advances ONLY here` | prev_pivot = plan.clock_pivot; merge-update last_pivot (never erase). Carry-lifecycle baseline that pairs with the emitted bump; a withheld epoch leav |
| 6749–6755 | phase-block | `D2 stash setup (prev_carry clear + pinned_co` |  | `// --- D2 stash for the next epoch -----------------------------------` | Clears prev_carry + prev_carry_cov (C1), declares fc_cols, grabs cov=est.sharedCovariance(), builds pinned_cols from plan.shared_datum columns. Reads  |
| 6756–6775 | lambda | `stash (D2 next-epoch carry lambda)` |  | `auto stash = [&](SharedParamKind kind, const std::string& sat,` | Capture [&]. Reads by-ref: pinned_cols (skip datum-pinned col -> return), cov (var=cov(col,col)), x_s (cp.value=x_s(col)), fullcov_on, and calls share |
| 6756–6775 | lambda | `stash` |  | `auto stash = [&](SharedParamKind kind, const std::string& sat,` | Captures [&] by-ref. Reads: pinned_cols, cov (est.sharedCovariance()), x_s, fullcov_on, and MUTATES prev_carry (push_back CarryPrior), prev_carry_cov. |
| 6776–6807 | phase-block | `clock/phase/code stash invocation` |  | `// A satellite flagged by the clock monitor (or, opt-in, the within-ep` | full_refloat = !(clk_mon_reinit_sig>0) (hard skip-stash vs inflate-q soft). If !clk_model_on: stash Clock for each registered_clock_sat (skip if flagg |
| 6785–6793 | phase-block | `clock-scalar-carry stash loop` |  | `// The satellite clock is stashed to prev_carry ONLY in scalar-carry m` | Guarded by !clk_model_on. full_refloat = !(clk_mon_reinit_sig>0). Iterates registered_clock_sats, calls stash(Clock,...) with flagged=inflate. clock_r |
| 6794–6801 | phase-block | `phase-bias stash loop` |  | `for (const auto& pk : shared_phase_keys) {` | Parses each shared_phase_key via StateKeys::parseSatPhaseBiasKey, stash(PhaseBias,sat,signal,phaseBiasColumn,flagged). Refloat gate on clock_refloat_s |
| 6802–6807 | phase-block | `code-bias (W6 OSB) stash loop` |  | `for (const auto& kv : registered_code_keys) {  // W6 satellite code bi` | stash(CodeBias,kv.first,kv.second,codeBiasColumn,flagged) over registered_code_keys. W6 satellite OSB carry. |
| 6808–6828 | phase-block | `C1 dense covariance block stash` |  | `// C1: stash the dense covariance block over the clock/phase/code entr` | Guarded fullcov_on && !fc_cols.empty(). Resizes prev_carry_cov.P (kfc x kfc), fills from cov(fc_cols[a],fc_cols[b]) O(k^2). allFinite guard -> warn +  |
| 6808–6828 | phase-block | `C1 dense shared-covariance stash block` |  | `// C1: stash the dense covariance block over the clock/phase/code entr` | fullcov_on gated. Builds prev_carry_cov.P (kfc x kfc) from cov over fc_cols; non-finite -> clear() with C1 warn. Dense full-cov shared carry (C1 featu |
| 6829–6845 | phase-block | `SH-VTEC coefficient stash` |  | `if (config_.estimateNetworkIonoSh() && sh_model) {   // SH-VTEC coeffi` | Guarded estimateNetworkIonoSh() && sh_model. For each sh_model->coeffs(): col=vtecCoeffColumn(n,m,trig), skip pinned_cols, skip non-finite val/var, pu |
| 6829–6845 | phase-block | `SH-VTEC coefficient carry stash` |  | `if (config_.estimateNetworkIonoSh() && sh_model) {   // SH-VTEC coeffi` | Iterates sh_model->coeffs(), vtecCoeffColumn(n,m,trig); skips pinned_cols; per-entry finite guard on val/var; pushes CarryPrior{kind=VtecSh}. This is  |
| 6912–6912 | phase-block | `prev_week/prev_tow advance` |  | `prev_week = se.time.week; prev_tow = se.time.tow;` | Advances cross-epoch time base (prev_week/prev_tow). Carry-lifecycle scalar, EMITTED-only. |
| 6914–7005 | phase-block | `RTB-7(a) periodic atomic state snapshot` |  | `// --- RTB-7(a): periodic atomic state snapshot ----------------------` | Gated on !snap_path.empty() + state_snapshot_interval_s. Builds NetworkStateSnapshot: counters, iod, prev_week/tow, URA/ramp EWMA, shared_carry from p |

## Setup — INC 5  →  `src/core/app/ApplicationNetworkSetup.cpp`  (15 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 63–111 | banner-section | `#include block + Eigen thread-pin + OpenMP g` |  | `#include "app/Application.hpp"` | Translation-unit includes (products, filter/NetworkEstimator+Sbasis+PivotHandover, slip detectors, SH-iono, RealTimeEngine, Eigen/Core for setNbThread |
| 458–472 | phase-block | `phase0_validate_inputs` |  | `// --- Phase 0: validate inputs (explicit RINEX + fixed coordinates)` | Banner-comment section 'Phase 0'. Loops nc.stations; hard-errors (return 1) any station with empty rinex_path (V1 requires explicit per-station paths; |
| 489–508 | phase-block | `phase2_roster_build` |  | `// --- Phase 2: multi-station RINEX ingestion + epoch alignment` | Builds BaselineConfig roster: clock_datum_station becomes roster.reference (pivot_found), all others push into roster.rovers. Hard-errors if datum sta |
| 510–530 | phase-block | `phase2_closedloop_roster_add` |  | `// Closed-loop integer-recovery harness: a HELD-OUT consumer station` | Binds const ClosedLoopConfig& cl = nc.closed_loop and const bool closed_loop_on. Also derives const bool brdc_rts from config_.networkAprioriSource()= |
| 532–558 | phase-block | `phase2_sync_construct_and_clip` |  | `MultiRinexSync sync(config_, roster, log_manager_, *product_parser_);` | Constructs MultiRinexSync sync (the ingestion engine, referenced later via sync.parserOf in buildStation). Computes parse-window clip: span_s = start_ |
| 560–580 | phase-block | `phase2_sync_load_synchronize` |  | `if (!sync.load()) {` | sync.load() (non-fatal warn — failed stations excluded), sync.synchronize() (hard-error return 1), binds const auto& synced = sync.getSyncedEpochs() ( |
| 582–590 | phase-block | `phase3_products_cache` |  | `// --- Phase 3: products + cache for geometry` | product_parser_->loadProducts(); binds ProductCache::getInstance() singleton reference product_cache; loadProducts hard-errors return 1. Logs orbit/cl |
| 591–650 | phase-block | `phase3_brdc_rts_nav_load` |  | `if (brdc_rts) {` | Card 5 RTF-1 broadcast a-priori branch. Reads config_.getNavigationPath() (hard-error if empty). Parses via RinexNavParser::parse into std::vector<Bro |
| 651–656 | phase-block | `phase3_sp3_orbit_guard` |  | `} else if (product_cache.getOrbitCount() == 0) {` | sp3 (default) mode: hard-error return 1 when no orbit data — the precise cache IS the a-priori. Byte-identical legacy path. |
| 658–703 | phase-block | `phase3b_gim_or_sh_iono` |  | `// --- Phase 3b: GIM for satellite code-bias estimation (W6)` | Constructs the iono constraint for satellite code-bias observability. Binds std::unique_ptr<SphericalHarmonicIonosphere> sh_model and const GlobalVtec |
| 710–790 | lambda | `buildStation` |  | `auto buildStation = [&](const StationConfig& cfg, bool is_pivot) -> bo` | THE ~80-line station constructor lambda. Captures: [&] by-ref (full capture). Reads/uses: sync (sync.parserOf), log, config_ (many getters: networkPha |
| 791–806 | phase-block | `buildStation_invocation_roster_loop` |  | `if (!buildStation(roster.reference, /*is_pivot=*/true)) {` | Drives buildStation: reference as pivot (hard-error return 1 on failure — datum is mandatory), then loops roster.rovers skipping the held-out closed-l |
| 1015–1071 | phase-block | `closedloop_consumer_setup` |  | `// --- Closed-loop consumer (held-out station) -----------------------` | Builds the held-out consumer. GnssConfig consumer_config = config_.deriveClosedLoopConsumerConfig(); std::unique_ptr<RealTimeEngine> consumer. When cl |
| 2089–2098 | omp-region | `eigen_threads_saved / setNbThreads(1)` |  | `// Pin Eigen to a single thread for the duration of the (parallelized)` | OpenMP/Eigen threading control: saves Eigen::nbThreads() and, when parallel_on, pins Eigen::setNbThreads(1) so Eigen dense kernels don't oversubscribe |
| 2107–2132 | phase-block | `est (NetworkEstimator hoist) + reduce_thread` |  | `// The joint estimator is HOISTED out of the epoch loop and reset()/re` | Hoists NetworkEstimator est out of the epoch loop (reset/reuse to avoid ~146GB heap churn at 305 stations). Sets reduce_threads = (parallel_on && nc.p |

## Assembly — INC 7  →  `src/core/app/ApplicationNetworkAssembly.cpp`  (19 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 1780–1900 | lambda | `splitStation` |  | `Column-split helper: split a station's H into the shared border A_s` | THE start-of-splitStation. Captures [&] (by-ref): reads config_ (estimateNetworkSatCodeBias), StateKeys/StateManager via st.sm. Params (NetStation& st |
| 1780–1900 | lambda | `splitStation` |  | `auto splitStation = [&](NetStation& st, const NetworkEstimator& est,` | By-ref [&] capture; READS config_ (estimateNetworkSatCodeBias), StateKeys statics; per-station splits em.H into shared border A_s and local block A_r  |
| 1902–1973 | lambda | `appendGimIonoRows` |  | `// GIM ionosphere pseudo-observation (W6 satellite code-bias prerequis` | By-ref [&]; READS gim_ (member ptr), config_.getIonosphereWeightingConfig(); appends GIM slant-iono pseudo-obs rows into station OWN block only (A_r l |
| 1975–2087 | lambda | `appendShIonoRows` |  | `// Co-estimated SH-VTEC ionosphere pseudo-observation (the design's hi` | By-ref [&]; the SH-VTEC hinge: READS sh_model (ptr), shc (SH config struct: earth_radius_km/layer_height_km/sun_fixed/representation_sigma_tecu), conf |
| 2517–2528 | phase-block | `PASS A setup + legacy-pivot i_lo` |  | `// --- PASS A: per-station geometry + shared-state layout ------------` | PASS A start: declares modeled (vector<vector<PppObservation>> per station) + present (vector<bool>). Banner explains legacy single-pivot models pivot |
| 2517–2519 | phase-block | `PASS A allocation (modeled/present vectors)` |  | `// --- PASS A: per-station geometry + shared-state layout` | Allocates modeled (vector<vector<PppObservation>> per station) and present (vector<bool>). These are EpochWorkspace members (RunTypes) but the populat |
| 2521–2551 | skip-site | `legacy pivot-first datum block` |  | `// Legacy single-pivot datum: model the pivot FIRST so its usable phas` | i_lo = phase_datum_prioritized?0:1 computed at 2528. Legacy path (!prioritized): models stations.front() pivot first via computeModels; TWO skip/withh |
| 2562–2588 | phase-block | `PASS A tail scope + private per-station buff` |  | `// PASS A tail (stations[1..]) — PARALLEL over stations when enabled.` | Opens brace scope. Declares per-station private slot buffers for the OMP loop: a_ok (vector<char>), a_err (vector<string>), sta_repairs (vector<vector |
| 2589–2659 | omp-region | `PASS A tail #pragma omp parallel for (FIRST ` |  | `#pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(p` | FIRST omp parallel-for. schedule(dynamic) num_threads(n_threads) if(parallel_on). Loop i=i_lo..n_sta_total. Per-i: computeModels into modeled[i] (dist |
| 2661–2670 | phase-block | `PASS A serial drain (present[] commit)` |  | `// Serial drain: commit present[] + log diagnostics in station order.` | Folds a_ok[i]->present[i] serially in station order (present is packed-bit vector<bool>, cannot be written concurrently). Logs a_err[i] warnings. i_lo |
| 3049–3098 | phase-block | `cross-station observability pre-filter` |  | `// --- Cross-station observability pre-filter` | kMinStationsPerSat=2 constexpr (3062). Counts stations observing each sat via sat_station_count over getSatClockStates + satelliteFromSatClockKey. dro |
| 3082–3082 | lambda | `obs-erase predicate lambda` |  | `[&](const PppObservation& o) { return dropped_sats.count(o.satellite) ` | remove_if predicate for erasing dropped satellites' observations from modeled[i]. Reads dropped_sats set. Moves with the Assembly observability prune. |
| 3263–3279 | phase-block | `shared-column registration (est.reset + regi` |  | `// --- Register the shared column layout (reset the hoisted estimator)` | est.reset() on hoisted estimator. Registers clock columns: per present station getSatClockStates -> satelliteFromSatClockKey -> est.registerClock, ded |
| 3403–3412 | phase-block | `asm_res_decl` |  | `// --- PASS B: per-station assembly, then SERIAL scatter -------------` | PASS B banner + std::vector<StationAssembly> asm_res(stations.size()) pre-sized slots for the parallel produce/serial consume split. StationAssembly i |
| 3414–3667 | omp-region | `passB_parallel_produce_omp` |  | `#pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(p` | THE SECOND #pragma omp parallel for (~3414). Per-station PARALLEL produce: computeMeasurementUpdate → splitStation into R.A_s/A_r/w/y/h2local; anchor  |
| 3485–3535 | phase-block | `glonass_local_anchor_rows` |  | `// GLONASS local-nuisance anchors (code-only network scope).` | Appends LOOSE zero-mean prior rows (weight 1.0) per GLONASS per-slot IFB column (isGlonassPerSlotIfbKey) and per GLONASS per-sat iono column to regula |
| 3537–3547 | phase-block | `iono_row_dispatch` |  | `// Ionosphere constraint (W6): the between-satellite structure that` | Dispatches appendShIonoRows (estimateNetworkIonoSh) else appendGimIonoRows (estimateNetworkSatCodeBias) into station's own block + shared SH border. T |
| 3668–3754 | phase-block | `passB_serial_consume_scatter` |  | `// Serial consume: scatter each station's block into the shared arrowh` | SERIAL scatter in deterministic station order. Disposition policy: empty_innov→present=false; state_mismatch/threw/addStation-reject → legacy pivot si |
| 3977–4055 | phase-block | `SH-VTEC prior fold (Kaula + temporal RW, pre` | ✓ | `// --- SH-VTEC prior (Kaula + temporal), folded BEFORE the gate` | Gated on estimateNetworkIonoSh()&&sh_model&&numShared>0. Builds sh_carry map from prev_carry (skipped on datum_changed — RTA-2 SH stays GLOBAL, no sco |

## PhaseDatum — INC 7/8  →  `include/filter/NetworkPhaseDatum.{hpp,cpp}`  (15 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 1446–1458 | banner-section | `anchor_incumbent / this_epoch_anchor / emitt` |  | `Prioritized phase-bias anchor (network.phase_datum_prioritized)` | Three std::map<std::string,std::string> keyed by sat_signal: anchor_incumbent (sticky), this_epoch_anchor (elected this epoch), emitted_anchor (last e |
| 1760–1774 | lambda | `isGaugePinned` |  | `Single source of truth for "is (sat,signal) at station i a gauge-pinne` | const bool phase_datum_prioritized decl at 1769 (config_.networkPhaseDatumPrioritized()). Lambda captures [&] (by-ref): reads phase_datum_prioritized  |
| 2553–2560 | phase-block | `shared_phase_keys seed` |  | `// Legacy: the pivot's allocated phase-bias keys ARE the shared set.` | Declares std::set<std::string> shared_phase_keys. Legacy: seeds from stations.front().sm->getSatPhaseBiasStates(). Prioritized: left empty (rebuilt po |
| 3099–3112 | phase-block | `legacy shared_phase_keys rebuild` |  | `// The final shared phase-bias set + which station anchors (gauge-pins` | Legacy (!phase_datum_prioritized): clear shared_phase_keys, refill from stations.front().sm->getSatPhaseBiasStates() (post-prune pivot pin set). Byte- |
| 3113–3261 | phase-block | `prioritized anchor-election block` |  | `const double pref_el = config_.networkPhaseDatumPrefElevDeg();` | Prioritized datum election (else branch). Reads pref_el, sticky, pdp_min_sta from config_ (networkPhaseDatumPrefElevDeg/Sticky/MinStations). (a) per-s |
| 3154–3157 | lambda | `cov (coverage lookup)` |  | `auto cov = [&](char c, size_t i) -> int {` | Nested-map lookup: returns cover[c][i] coverage count or 0. Used by candidate stable_sort comparator (3220). PhaseDatum election helper. |
| 3154–3157 | lambda | `cov` |  | `auto cov = [&](char c, size_t i) -> int {` | Coverage lookup helper for anchor ranking. Header says read past 3820 to close units, but this lambda BEGINS at 3154 (just before the 3160 window) and |
| 3160–3261 | phase-block | `phase_datum_prioritized_election_tail` |  | `for (const auto& [amb, pb] : usable_keys[i]) {` | Prioritized phase-datum: builds cand_set/union_keys, applies coverage-threshold gate (pdp_min_sta) pruning SatPhaseBiasStates via pruneSatPhaseBiasSta |
| 3166–3214 | phase-block | `pdp coverage-threshold gating` |  | `// Coverage-threshold gating of the anchored union` | network.phase_datum_min_stations gate (pdp_min_sta>1; 1=off byte-identical). Counts n_trk per "sat_signal" over usable_keys; erases union_keys entries |
| 3219–3223 | lambda | `candidate ranking stable_sort comparator` |  | `std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {` | Ranks candidate receivers per constellation: cov(c,a)>cov(c,b) descending, tie-break lexical stations[a].name<stations[b].name (determinism). Nested i |
| 3219–3223 | lambda | `gate_stable_sort_cmp` |  | `std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {` | Candidate ordering comparator: DESC coverage via cov(c,a) then lexical stations[a].name. Nested inside the election tail; determinism-critical. |
| 3227–3260 | phase-block | `anchor election loop` |  | `// (c) Elect one anchor per (sat,signal): keep the STICKY incumbent wh` | Clears all st.pinned_amb_keys, this_epoch_anchor, shared_phase_keys. Per union_keys entry: sticky incumbent kept if present+still tracks (anchor_incum |
| 3450–3457 | lambda | `foldAmb` |  | `auto foldAmb = [&](const std::string& amb_key) {` | Inside OMP region. Anchor ambiguity pin fold: for a pinned amb_key, look up amb_idx + h2local column, push pin column and innovation = -piv_x(amb_idx) |
| 4062–4129 | lambda | `healNearNullPhaseBias lambda (spectrum-based` | ✓ | `auto healNearNullPhaseBias = [&]() -> int {` | Full eigenVECTOR decomposition of reduced shared normals (SelfAdjointEigenSolver, ComputeEigenvectors) — O(n_shared^3), invoked ONLY from gate-failure |
| 5509–5538 | phase-block | `prioritized phase-anchor emit-baseline advan` |  | `// Prioritized phase-bias anchor: record per-(sat,signal) anchor` | if(phase_datum_prioritized): PHASE_ANCHOR anchor_handover emitEvent per (amb→sta) where emitted_anchor changed; advances emitted_anchor[amb]. Reads th |

## Estimator — INC 8  →  `NetworkEstimator ADDITIONS`  (5 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 3263–3336 | phase-block | `register_shared_columns` |  | `// --- Register the shared column layout (reset the hoisted estimator)` | est.reset() then registerClock/registerPhaseBias/registerCodeBias/registerVtecCoeff. Builds registered_clock_sats, shared_phase_keys→PB cols, register |
| 3345–3370 | phase-block | `col_desc_estimability` |  | `// Column -> human descriptor map (W6 estimability diagnostic): lets t` | Builds col_desc string vector (CLK\|/PB\|/CB\|/SH\| labels) for the one-shot estimability spectrum dump. Guarded by !spectrum_diag_done && nc.trace_es |
| 4057–4060 | helper | `kNullSpaceTol constant` |  | `// Relative eigenvalue tolerance for the null-space rank gate` | constexpr double kNullSpaceTol = 1e-9. Hoisted so healNearNullPhaseBias and the gate share the SAME cutoff. Load-bearing constant for both the drop-an |
| 4131–4143 | banner-section | `null-space gate rationale banner` |  | `// Null-space rank gate.  The reduced shared normals are deliberately` | Pure comment block: derivation of the 1e-9 relative tolerance (7-decade gap between per-constellation clock gauges ~1e-10 σmax and weakest real direct |
| 4145–4212 | phase-block | `one-shot estimability diagnostic (spectrum t` |  | `// ---- One-shot estimability diagnostic (default-OFF)` | Gated on !spectrum_diag_done (sets spectrum_diag_done=true unconditionally — the latch that co-fires the W6-cb one-shot log above) then nc.trace_estim |

## Sbasis — INC 8  →  `NetworkSbasis ADDITIONS`  (6 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 3767–3905 | phase-block | `code_bias_datum_gauge_rows` | ✓ | `// --- Satellite code-bias datum constraints (W6, full multi-band) ---` | buildCodeBiasDatumRows: the W6 gauge rows. Builds (2) per-(constellation,signal) sum-to-zero rows (st0_row, NetworkSbasis::constellationOf) + (1) per- |
| 3811–3818 | lambda | `freqOf` |  | `auto freqOf = [&](const std::string& sat, const std::string& sig) -> d` | Carrier frequency lookup from any present station's measurement model, for the IF-pin weights. Inside the code-bias datum block. |
| 3819–3821 | lambda | `bandDigit` |  | `auto bandDigit = [](const std::string& sig) -> char {` | Captureless: returns RINEX band digit sig[1] ('C1W'->'1') for the band-sort. Inside code-bias datum block; used by the IF-pin sort comparator. |
| 3823–3905 | banner-section | `buildCodeBiasDatumRows (W6 gauge rows: IF pi` | ✓ | `std::map<std::string, std::vector<std::string>> sat_signals;` | W6 code-bias gauge-row construction folded via est.addSharedConstraintRows(H,z,kCodeBiasGaugeWeight). Builds per-satellite IF-pin rows (a1+a2=1, clock |
| 3854–3857 | lambda | `bandDigit_sort_cmp` |  | `std::sort(sigs.begin(), sigs.end(),` | Sorts a satellite's code signals by RINEX band digit so the first two distinct bands are the primary dual-frequency pair for the IF pin. Closes past 3 |
| 3947–3955 | phase-block | `clock sum-to-zero constraint fold (plan.cloc` |  | `// Sum-to-zero clock datum: fold the per-constellation` | Folds plan.clock_constraint_H/z/weight via est.addSharedConstraintRows BEFORE the null-space gate. No-op (0 rows) in PIN mode → byte-identical. Note:  |

## Raim — INC 9  →  `include/filter/NetworkRaim.{hpp,cpp}`  (26 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 4672–4703 | phase-block | `within_epoch_adaptation_decls` |  | `// --- Within-epoch adaptation: drop the dominant culprit(s) and re-so` | Within-epoch adaptation banner + locals: n_adapt, we_adapt_attempted, sigma0_adapt (A4 final σ̂0, 0=did-not-run), we_sigma0_pool_c map<char,vector<dou |
| 4722–4789 | lambda | `assess` |  | `auto assess = [&](const Eigen::VectorXd& xs, double& sigma0_out,` | σ̂0 + median-polish attribution over carried-ambiguity PHASE rows (R.pl_amb). Skips non-added/absent and (unless phase_datum_prioritized) pivot statio |
| 4802–4872 | lambda | `resolve_dropping` |  | `auto resolve_dropping = [&]() -> Eigen::VectorXd {` | CRITICAL capstar replay. Replays the epoch over est.beginEpoch() refloating culprit ambiguities. Rebuilds est_index_of. For each station computes refl |
| 4874–5003 | phase-block | `within_epoch_adapt_loop` |  | `double sigma0 = 1.0;` | Within-epoch adaptation driver loop (closes the `if (within_epoch_armed && carry_warm)` block). First assess(x_s,...) → sigma0_first; trace_flag_rates |
| 5016–5090 | phase-block | `perconst_sigma0_withhold_decision` |  | `// --- Within-epoch escape-hatch: withhold rather than emit a still-co` | Long banner (P0-4 gate-on-σ̂0-alone; RTA-10 per-const carve-out math + APPROXIMATION disclosure) then decision code. Decls: perconst_active, perconst_ |
| 6006–6432 | phase-block | `post-fit network RAIM block (Increment 3, ti` |  | `// --- Post-fit network RAIM (Increment 3, tier T2)` | Top-level if(raim_on && carry_warm) enclosing Pass1 residual build, σ̂0, Pass2 consensus, clock/retro feed, raim_trace, prominence attribution + attri |
| 6053–6098 | phase-block | `RAIM Pass 1: per-station post-fit residual b` |  | `for (size_t i = 0; i < stations.size(); ++i) {` | Per non-pivot station (phase_datum_prioritized keeps former pivot): reconstructStation, resid = R.y - R.A_s*x_s - R.A_r*x_r; optional est.rowRedundanc |
| 6100–6117 | phase-block | `RAIM σ̂0 robust variance factor + URA EWMA f` |  | `// Robust a-posteriori variance factor for this epoch, network-wide.` | sigma0 = max(1, 1.4826·median(r_pool)) via nth_element; if(ura_on) fold into ura_sigma0_ewma (ura_alpha EWMA) and ++ura_ewma_updates (B1 / RTB-7b). si |
| 6100–6117 | phase-block | `sigma0 robust variance factor` |  | `// Robust a-posteriori variance factor for this epoch, network-wide.` | Computes network-wide robust sigma0 = max(1, 1.4826*median(r_pool)) via nth_element; floored at 1. Then B1 URA fold: ura_sigma0_ewma EWMA update + ++u |
| 6119–6144 | phase-block | `RAIM Pass 2: normalize + cross-station conse` |  | `// Pass 2: normalize by σ̂0 and take the cross-station consensus` | Per amb_stat s_norm=r_max/σ̂0, spiked=s_norm>raim_k; B8/T3 fr_raim_cell; L13 station_res_trace per cell; ++sat_flag_stations; flagged = sats with ≥rai |
| 6119–6144 | phase-block | `Pass-2 normalize + consensus spike tally` |  | `// Pass 2: normalize by σ̂0 and take the cross-station consensus per s` | Builds sat_flag_stations (sat->#spiked stations), raim_details (only when raim_trace open), flagged vector + fset set (sats with >= raim_min_sta spike |
| 6145–6153 | phase-block | `RAIM consensus clock/retro feed` |  | `// A consensus-flagged satellite's slip leaked into its SHARED clock` | if(clk_mon_couple) fset→clock_refloat_sats; P0-2 if(retro_feed) fset→retro_flag_sats. Consensus→clock/retro attribution feed. |
| 6146–6153 | phase-block | `consensus clock/retro feed` |  | `// A consensus-flagged satellite's slip leaked into its SHARED clock` | For each sat in fset: if clk_mon_couple -> clock_refloat_sats.insert; if retro_feed -> retro_flag_sats.insert. Reads fset, clk_mon_couple, retro_feed; |
| 6172–6370 | phase-block | `prominence attribution + attribution-driven ` |  | `// --- Prominence attribution + optional attribution-driven refloat` | if(need_attrib && !cells.empty()): build (sta×sat) cell_max table, medianPolish2Way(tbl); B8/T4 fr_attrib on col_eff; robustProminence on col_eff/row_ |
| 6172–6370 | phase-block | `prominence attribution block` |  | `// --- Prominence attribution + optional attribution-driven refloat` | Large banner+block: median-polish 2-way attribution (bad sat=column / bad station=row / isolated arc=interaction / bad signal=marginal). Gated need_at |
| 6183–6183 | helper | `kAttribReportProm` |  | `constexpr double kAttribReportProm = 4.0;` | Constexpr robust-z threshold above which an attribution entity is logged. Local constant. |
| 6183–6183 | helper | `kAttribReportProm` |  | `constexpr double kAttribReportProm = 4.0;  // robust-z above which an ` | constexpr local threshold (robust-z 4.0) gating which attribution entities get logged. Used by the emit lambda callers at 6288/6292/6296/6306. Move al |
| 6300–6302 | lambda | `attribution ARC-sort comparator lambda` |  | `std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b` | Capture [&]. Reads by-ref mp.interaction. Sort comparator by \|interaction\| desc for arc logging order. No capstar. |
| 6318–6369 | phase-block | `attribution-driven refloat execute + log + T` |  | `// Execute the attribution-driven refloat (surgical: whole sat / whole` | Guarded attrib_act && (bad_sats\|\|bad_stations\|\|bad_arcs nonempty). Loops bad_sats over present stations (pivot guard via phase_datum_prioritized \ |
| 6321–6369 | phase-block | `attribution refloat execution + T4 events` |  | `if (attrib_act && (!bad_sats.empty() \|\| !bad_stations.empty()` | Applies refloat_sat_at across present && est_index_of>=0 && (prioritized\|\|!is_pivot) for bad_sats; bad_arcs directly; builds detail string; log->inf |
| 6322–6331 | lambda | `refloat_sat_at lambda` |  | `auto refloat_sat_at = [&](int i, const std::string& sat) -> int {` | Capture [&]. Reads/writes by-ref: asm_res[i].pl_amb (read), local_carry (erase), stations[i].sm (markAmbiguityPendingReseed). Params i, sat. Returns c |
| 6322–6331 | lambda | `refloat_sat_at (attribution)` |  | `auto refloat_sat_at = [&](int i, const std::string& sat) -> int {` | Capture [&]. Reads by-ref: asm_res (asm_res[i].pl_amb). Mutates by-ref: local_carry (erase tuple(i,sat,signal)), stations[i].sm->markAmbiguityPendingR |
| 6372–6385 | phase-block | `'so bad' within-epoch adaptation trigger (de` |  | `// --- "So bad" within-epoch adaptation trigger (detection + log)` | if(config_.networkRaimWithinEpoch() && sigma0>networkRaimSigma0Trigger()): log->warn + T2_within_epoch emitted_contaminated emitEvent. Full culprit-dr |
| 6372–6385 | phase-block | `within-epoch adaptation trigger (detect+log)` |  | `// --- "So bad" within-epoch adaptation trigger (detection + log) ---` | Guarded networkRaimWithinEpoch() && sigma0 > networkRaimSigma0Trigger(). Currently DETECT+ARM only (culprit-drop re-solve is staged/not here): warns ' |
| 6387–6431 | phase-block | `per-satellite consensus refloat (non-attribu` |  | `// Per-satellite consensus refloat — only when attribution is NOT` | if(!attrib_act && !flagged.empty()): per non-pivot station, for pl_amb with fset.count(sat) erase local_carry + markAmbiguityPendingReseed (outright d |
| 6387–6431 | phase-block | `per-satellite consensus refloat` |  | `// Per-satellite consensus refloat — only when attribution is NOT driv` | Guarded !attrib_act && !flagged.empty(). For each station (skip est_index_of<0 or pivot unless phase_datum_prioritized), for each pl_amb whose sat in  |

## ClockQc — INC 10  →  `include/filter/NetworkClockQc.{hpp,cpp}`  (19 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 313–324 | free-function | `clockOscPredict` |  | `inline ClockOscPred clockOscPredict(const ClockOsc& s, double dt, doub` | inline: Card-6 oscillator prediction. Forms Q(dt) blocks q11=q1·dt+q2·dt³/3, q12=q2·dt²/2, q22=q2·dt and propagates Φ=[[1,dt],[0,1]]. Byte-identity ha |
| 332–344 | free-function | `clockOscUpdate` |  | `[[maybe_unused]] inline double clockOscUpdate(ClockOsc& s, const Clock` | inline, [[maybe_unused]]: scalar KF measurement update of ClockOsc, H=[1 0], returns innovation variance S. NOTE per the doc comment (line 330-331) th |
| 1459–1462 | banner-section | `prev_clk (clock_monitor)` |  | `Per-satellite clock sanity monitor (network.clock_monitor): the previo` | std::map<std::string,std::pair<double,double>> prev_clk (sat -> {Δδtˢ,seen_s}); adjacent-epoch jump baseline; erased on refloat. |
| 1463–1465 | banner-section | `clock_osc (clock_model)` |  | `Two-state oscillator clock state per satellite (network.clock_model)` | std::map<std::string,ClockOsc> clock_osc; per-sat carried [offset,drift]+2x2 cov+t_last. ClockOsc struct itself lives in RunState. Inert unless clk_mo |
| 1466–1476 | banner-section | `relevel_iode (Card 6 brdc_rts)` |  | `Card 6 (RTF-2, network.apriori_source = brdc_rts): the broadcast-ephem` | std::map<std::string,int> relevel_iode; IODE the carried clock is referenced-to; advanced at eph-cutover RE-LEVEL before solve (mutates carried state  |
| 1477–1483 | banner-section | `eph_cutover_disc_pending (Card 6)` |  | `Card 6: sats whose eph rolled but whose PRIOR version was already evic` | std::set<std::string> eph_cutover_disc_pending; forced per-sat discontinuity queue for incomputable Δ_eph; defensive belt (unreachable in steady state |
| 2343–2349 | phase-block | `clk_anchor_sigma` |  | `// Card 5 (RTF-1 / plan D6): resolve the a-priori satellite-clock anch` | Card-5 loop-invariant: resolves clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM() once (policy in GnssConfig: explicit>0 wins; brdc_ |
| 4283–4293 | phase-block | `clk_model_warm + clk_pred / clk_prior_info d` |  | `// Two-state clock model warmup gate (mirrors carry_warm)` | clk_model_warm = clk_model_on && (n_emitted>=pl_warmup). Declares std::map<std::string,ClockOscPred> clk_pred (per-epoch two-state predictions stashed |
| 4295–4371 | phase-block | `Card 6 ephemeris-cutover re-level (brdc_rts)` |  | `// --- Card 6 (RTF-2 / plan D3): ephemeris-cutover re-level` | Long banner (4295-4326) on ordering rationale (re-base carried clock by -Δ_eph BEFORE prior injection/solve; placed after gate continue for relevel_io |
| 4530–4537 | banner-section | `two-state oscillator clock carry-forward pri` |  | `// --- Two-state oscillator clock carry-forward prior (network.clock_m` | UNIT SPANS PAST 4500/4537 — only the banner header is in range (4530-4537+). Documents the clock_osc predict-fold (predict [offset,drift], fold offset |
| 4530–4578 | phase-block | `clock_osc_carryforward_prior` |  | `// --- Two-state oscillator clock carry-forward prior (network.clock_m` | Two-state oscillator clock carry-forward prior (network.clock_model). Gated clk_model_warm && (!datum_changed \|\| scoped_handover). Iterates register |
| 4580–4623 | phase-block | `apriori_satclock_anchor_tikhonov` |  | `// --- A-priori satellite-clock anchor (cold-start conditioning) -----` | A-priori satellite-clock Tikhonov anchor (cold-start conditioning). Unnamed scope block { }. Gated clk_sigma finite && >0 && <100m (binding-std gate;  |
| 4655–4670 | phase-block | `clock_refloat_sats_decl` |  | `// Satellites whose carried SHARED state (clock + coupled phase/code b` | Declarations: std::set<std::string> clock_refloat_sats (carry-refloat set, empty unless network.clock_monitor), std::set<std::string> retro_flag_sats  |
| 5176–5203 | phase-block | `emit_stamp_clock_c1_drift` |  | `// B3b: publish the two-state clock-model drift as C1 (consumer extrap` | B3b: publish two-state clock-model drift as C1. Gated `if (clk_model_on)`: per sol.sat looks up clock_osc[sat] (skip !init), computes sat_dt=pl_now_s  |
| 5479–5498 | epoch-skip-site | `Card-6 eph-cutover forced disc present/pendi` |  | `// Card 6: land the forced per-sat discontinuities for eph cutovers` | eph_cutover_disc_pending present/still_pending split (mirrors retro split); emitter.markSatDiscontinuity(mark); RELEVEL eph_cutover_disc_landed emitEv |
| 6434–6592 | phase-block | `two-state oscillator clock update + NIS faul` |  | `// --- Two-state oscillator clock: measurement update + normalized-inn` | if(clk_model_warm): datum-change osc wipe (global clock_osc.clear vs RTA-2 scoped datumChangeAffectsKey erase, T5 emitEvent); build z_of/innov_of/S_of |
| 6434–6592 | phase-block | `two-state oscillator clock update + NIS faul` |  | `// --- Two-state oscillator clock: measurement update + normalized-inn` | Big banner block gated clk_model_warm (network.clock_model). Handles datum-change oscillator wipe (global clock_osc.clear vs RTA-2 scoped erase via da |
| 6445–6474 | phase-block | `clock-model datum-change osc wipe (global vs` |  | `if (datum_changed && !scoped_handover) {` | Global branch: clock_osc.clear() + T5 datum_osc_wipe emitEvent. Else RTA-2 scoped: erase only datumChangeAffectsKey(changed_consts,scoped=true) osc en |
| 6594–6712 | phase-block | `per-satellite carried-clock sanity monitor` |  | `// --- Per-satellite carried-clock sanity monitor (network.clock_monit` | Banner block gated clk_mon_on && !clk_model_on && n_emitted>=pl_warmup. Reads clk_of via x_s(clockColumn), per-constellation median (clk_by_const/med_ |

## Emit — INC 11  →  `src/core/app/ApplicationNetworkEmit.cpp`  (34 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 891–902 | phase-block | `owned_product_sink_emitter` |  | `// --- Owned product sink (closed-loop isolation) --------------------` | Constructs SsrCorrectionStore network_store (the OWNED sink, never the singleton — closed-loop isolation) and NetworkProductEmitter emitter with (kDef |
| 903–1013 | phase-block | `product_writer_contract_setup` |  | `// --- Optional on-disk product serialization ------------------------` | Optional NetworkProductWriter setup. Binds std::unique_ptr<NetworkProductWriter> product_writer, std::filesystem::path prod_path, SbasisContract pendi |
| 1493–1495 | banner-section | `URA carried state: ura_sigma0_ewma / ura_ref` |  | `B1 URA quality channel carried state (inert unless network.emit_ura)` | double ura_sigma0_ewma=1.0 (network σ̂0 EWMA scaling emitted URA), std::map<std::string,int> ura_refloat_horizon (sat->post-refloat inflation epochs). |
| 1496–1504 | banner-section | `ura_retro_sats / retro_active (B1 retro-bump` |  | `B1 retro-bump (P0-2/P1-3): per-SATELLITE, tier-independent, edge-trigg` | std::set<std::string> ura_retro_sats (pending per-sat disc at next emit) + retro_active (already-bumped, rising-edge once). URA datum-bump state. |
| 3372–3380 | skip-site | `skip_no_shared` |  | `if (est.numShared() == 0) {` | Epoch skip when no shared params: ++n_skipped, writes trace CSV row (all-zero counts), emits EpochStatusRow('sync_skip'), advances prev_week/prev_tow, |
| 3756–3765 | skip-site | `skip_epoch_not_ok` |  | `if (!epoch_ok \|\| est.numShared() == 0) {` | Second epoch skip: pivot loss / no shared. ++n_skipped, trace CSV row (with n_present/n_clock/n_pb/total_pins), EpochStatusRow('sync_skip'), advance p |
| 3907–3945 | phase-block | `decl / SbasisDeclaration setup + buildShared` |  | `// --- Datum + null-space gate + solve` | Constructs NetworkSbasis sb + SbasisDeclaration decl (reference_receiver_id, emit_code_bias, isb_datum SumToZeroPerConstellation when clock_sum_to_zer |
| 4214–4281 | skip-site | `null-space rank gate + drop-and-heal re-gate` |  | `bool gate_ok = true;` | Core observability gate: sb.assertNullSpaceRank(reducedSharedNormals, plan.declared_dimension, kNullSpaceTol) in try. On failure, calls healNearNullPh |
| 4625–4639 | banner-section | `handover_bump_deferral_comment` |  | `// A clock-pivot handover re-references the whole shared solution; the` | Pure comment banner: documents WHY markDatumChange + prev_pivot/last_pivot baseline advance are DEFERRED to the emit site (skipped-epoch discontinuity |
| 4641–4653 | phase-block | `primary_shared_solve` |  | `Eigen::VectorXd x_s;` | Primary shared solve x_s = est.solve(plan.shared_datum) with try/catch. On solve failure: log->warn, ++n_skipped, EpochStatusRow es + emitEpochStatus( |
| 5005–5014 | phase-block | `reduce_once_invariant_check` |  | `// Reduce-once invariant (design §3.5): the carried prior is injected` | Design-invariant guard: if (pl_on && !we_adapt_attempted && est.reduceRecomputeCount() != 1) log->error about doubled serial Schur path. Reads pl_on,  |
| 5091–5122 | skip-site | `whole_epoch_sigma0_withhold` |  | `if (raim_withhold > 0.0 && sigma0_adapt > raim_withhold && !perconst_a` | EPOCH WITHHOLD/SKIP SITE. Gated raim_withhold>0 && sigma0_adapt>raim_withhold && !perconst_active. log->warn + emitEvent('SIGMA0','withhold','skip_epo |
| 5124–5125 | phase-block | `emit_toSolutionEpoch` |  | `// --- Emit ----------------------------------------------------------` | Emit-section opener: NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, se.time, iod++). Post-increment of iod. This is the head of the emit block; t |
| 5126–5146 | phase-block | `emit_stamp_broadcast_eph_version` |  | `// Card 5 (RTF-2): stamp the broadcast-ephemeris VERSION the producer'` | Card 5 (RTF-2) brdc_rts clock_iode stamp. Gated `if (brdc_rts)`: LiveEphemerisStore::getInstance(), per sol.sat (skip !has_clock/empty/'R' GLONASS) se |
| 5147–5151 | phase-block | `emit_stamp_generation_time` |  | `// RT0-4: stamp the wall-clock TIME-OF-GENERATION (distinct from the c` | RT0-4: if (emit_gen_time) sol.generation_time = TimeUtils::nowGpsTime(). Non-deterministic wall clock, gated ⇒ OFF stays {0,0}, no G record, byte-iden |
| 5204–5207 | phase-block | `epoch_withheld_consts_decl` |  | `// RTA-10: constellations withheld from THIS epoch's emission — filled` | Decl: std::set<char> epoch_withheld_consts — filled by RT0-5 quarantine and/or per-const σ̂0 carve-out; feeds H heartbeat status (ok vs partial) + wit |
| 5208–5277 | phase-block | `emit_clock_runaway_quarantine` |  | `// RT0-5: per-constellation clock-runaway quarantine.  Group this epoc` | RT0-5 per-constellation clock-runaway quarantine (banner incl. GLOBAL-median inversion HAZARD). Gated `if (clk_runaway_on && !sol.sats.empty())`: gath |
| 5278–5343 | phase-block | `emit_perconst_sigma0_prune` |  | `// RTA-10 (network.per_const_withhold): apply the per-constellation σ̂` | RTA-10 apply per-const σ̂0 carve-out at emit (mirrors RT0-5 swap-prune). Gated `if (perconst_active)`: prunes sol.sats whose const not in perconst_pub |
| 5291–5343 | phase-block | `perconst_active prune apply (RTA-10 carve-ou` |  | `if (perconst_active) {` | Per-const σ̂0 carve-out prune of sol.sats (swap-prune identical to RT0-5); populates epoch_withheld_consts, n_perconst_partial_epochs; diffuse-publish |
| 5305–5322 | phase-block | `nested prune-warn lambda arg (fmt const/sit ` |  | `for (char cst : pruned) {` | Per-pruned-constellation warn + emitEvent RTA-10_perconst constellation_withhold; reads perconst_stats sigma0_c/rows, se.time.tow, raim_withhold, perc |
| 5344–5388 | phase-block | `emit_ura_pass_open` |  | `// B1: per-sat URA = EWMA(σ̂0) × formal clock σ (shared-covariance dia` | BOUNDARY UNIT — opens the B1 per-sat URA pass and extends PAST 5300/5348 (only its head read here to close the range). Gated `if (ura_on)`: nested RTB |
| 5344–5405 | phase-block | `URA per-sat pass (B1 EWMA×formal-σ + RTB-7 r` |  | `// B1: per-sat URA = EWMA(σ̂0) × formal clock σ` | if(ura_on): RTB-7(b) ramp-lift gate calls networkWarmupRampLifted, sets ramp_lifted/grade_metric_s; then per-sat URA = s0·√var over est.sharedCovarian |
| 5406–5448 | phase-block | `datum bump + PIVOT event serialization` |  | `// Datum bump + serialization, applied HERE (after a successful` | if(datum_changed): emitter.markDatumChange(changed_consts) vs global markDatumChange(); sets global_bump/serialized_consts. PIVOT event loop derives a |
| 5449–5478 | epoch-skip-site | `B1 retro-bump present/pending split (retro_e` |  | `std::set<std::string> retro_emit_sats;   // B1 retro-bump` | RT0-5 seam: splits ura_retro_sats into retro_emit_sats (present in post-quarantine sol.sats) vs retro_pending (absent → wait next emit); emitter.markS |
| 5539–5558 | phase-block | `B6 deferred contract writer open at first em` |  | `// B6 (P1-1): open the deferred contract writer at the FIRST emit` | if(contract_pending): stamps realized plan.clock_pivot into pending_contract.decl.clock_pivot_sat, sets generation_id, constructs NetworkProductWriter |
| 5559–5582 | phase-block | `emitToStore + RTA-5 periodic S-contract re-b` |  | `emitter.emitToStore(sol, network_store);` | emitter.emitToStore(sol,network_store); then RTA-5 re-broadcast: if contract_rebroadcast_s>0 && due\|\|datum_changed refresh realized pivots from last |
| 5583–5623 | phase-block | `product_writer epoch-status heartbeat + epoc` |  | `if (product_writer) {` | RTA-10 NetworkProducerStatus heartbeat (state Ok/Partial from epoch_withheld_consts, sigma0_adapt, n_sats_published, withheld list) writeEpochStatus;  |
| 5634–5677 | phase-block | `closed-loop consume (held-out station vs fre` |  | `// --- Closed-loop consume: held-out station vs the fresh network` | if(closed_loop_on && consumer): consumer->processEpoch on cl.station rows; computes pos/ENU/err3d, AR stats (lastArRan/lastArResult), first-fix tow/we |
| 6714–6747 | phase-block | `retro-feed flag accounting (URA horizon + ed` |  | `// B1: after all post-emit QC tiers, record the flagged sats for the N` | Guarded retro_feed. Union of clock_refloat_sats + retro_flag_sats -> flagged_now. Sets ura_refloat_horizon[sat]=pl_warmup (if ura_refloat_inflate>1 && |
| 6898–6910 | phase-block | `periodic emitted-epoch info log` |  | `if (n_emitted == 1 \|\| n_emitted % 60 == 0) {` | log->info summary line every 60 emitted epochs (stations/clocks/biases/datum dim/rms) + SH mean-VTEC a00 line. Emit-side operator logging tied to prod |
| 7075–7101 | phase-block | `B6 zero-epoch contract-writer skip check` |  | `// --- B6 (P1-1): serialized-contract round-trip self-check` | contract_pending && !prod_path.empty(): warns whether a stale PREVIOUS product file remains vs writer-never-opened; L14 writes network_contract_check. |
| 7102–7187 | phase-block | `B6 contract round-trip self-check` |  | `if (nc.serialize_sbasis_contract && product_writer && !prod_path.empty` | product_writer.reset() flush+close; NetworkProductReader::read back; RTA-5 per-receipt checkContract over contract_refresh records (s_receipts/fails/m |
| 7251–7252 | phase-block | `shared-store accumulated info log` |  | `log->info("Network: shared satellite product accumulated in an owned "` | Single log->info noting SsrCorrectionStore closed-loop sink + trace path. Emit/closed-loop sink annotation. |
| 7254–7317 | banner-section | `closed-loop integer-recovery summary` |  | `// --- Closed-loop integer-recovery summary` | Gated on closed_loop_on && consumer. Computes fix_rate, mean_fixed, converged-window median/max over cl_recent_err3d (sort), final_err = consumer->get |

## Orchestrator — INC 12  →  `ApplicationNetwork.cpp (SHRUNK, orchestrator only)`  (18 units)

| Lines (adv.) | Kind | Symbol | cap* | Start-anchor | Notes |
|---|---|---|:--:|---|---|
| 1–61 | banner-section | `file-doc-banner (runNetwork architecture)` |  | `/**  * @file ApplicationNetwork.cpp` | Doxygen file banner describing the S-system PPP-RTK network producer, PASS A/B/SOLVE/D2 pipeline, and V1 scope. Pure comment — must travel with runNet |
| 113–115 | banner-section | `namespace GnssProcessor { namespace { (anon-` |  | `namespace GnssProcessor {` | Opens the file-local anonymous namespace holding every free helper/struct below. On decomposition these types move to shared headers (RunState/RunType |
| 446–446 | banner-section | `anon-namespace close } // namespace` |  | `} // namespace` | Closes the file-local anonymous namespace. On decomposition, everything above migrates to shared headers under real (non-anonymous) linkage; ensure no |
| 448–470 | free-function | `Application::runNetwork() signature + banner` |  | `int Application::runNetwork() {` | Member-function entry point (only the head is in-range). Opens with: log = log_manager_.getLogger('main_log'); nc = config_.getNetworkConfig(); produc |
| 448–448 | free-function | `Application::runNetwork` |  | `int Application::runNetwork() {` | Method definition opener for the entire runNetwork producer; body spans far past 1120. All Setup phases 0-4, trace decls, and the epoch loop live insi |
| 449–456 | phase-block | `runNetwork_prologue_logger_banner` |  | `auto log = log_manager_.getLogger("main_log");` | Fetches main_log, binds const nc = config_.getNetworkConfig() (the reference used everywhere below), and logs the producer banner with station count + |
| 474–487 | skip-site | `phase1_resolveData` |  | `// --- Phase 1: resolve products + broadcast nav` | resolveData() STAYS in the orchestrator per task directive — do NOT hoist. Non-fatal warn on failure. Also lazily constructs product_parser_ (member u |
| 1484–1484 | helper | `start_anchor_s` |  | `double start_anchor_s = -1.0;` | first-epoch time anchor for network.start_offset_hours fast-forward. Drives epoch skip site. |
| 1756–1758 | helper | `max_dur_anchor_s / max_dur_anchored / spectr` |  | `double max_dur_anchor_s = 0.0;` | double max_dur_anchor_s, bool max_dur_anchored (max-duration fast-forward stop anchors → skip site); bool spectrum_diag_done (one-shot estimability sp |
| 2351–2351 | phase-block | `for (const auto& se : synced) — epoch loop h` |  | `for (const auto& se : synced) {` | The per-epoch loop head; se is the current synced SyncedEpoch. Loop body sequences all phases and stays in runNetwork (Orchestrator). All subsequent p |
| 2352–2362 | skip-site | `start_offset fast-forward skip` |  | `// Optional testing fast-forward (network.start_offset_hours): skip ep` | Epoch SKIP site (network.start_offset_hours): continues (skips) epochs until elapsed data span from start_anchor_s reaches start_offset_s, then cold-s |
| 2364–2375 | skip-site | `max-duration cutoff` |  | `// Optional max-duration cutoff (processing.max_duration_s): anchor on` | Max-duration cutoff (processing.max_duration_s): anchors max_dur_anchor_s on first processed epoch; BREAKs the loop once elapsed data span reaches get |
| 2505–2515 | phase-block | `dt / carry_warm epoch timing` |  | `const double dt = (prev_tow < 0.0) ? 0.0` | Per-epoch inter-epoch dt = week/tow delta vs prev_week/prev_tow (0 on first). pl_now_s = continuousGpsSeconds(se.time). carry_warm = pl_on && (n_emitt |
| 2505–2506 | phase-block | `dt (inter-epoch gap)` |  | `const double dt = (prev_tow < 0.0) ? 0.0` | Scalar: continuous-GPS seconds since previous epoch from prev_week/prev_tow and se.time. Week-rollover aware (604800). Feeds TurboEdit resolve() at 26 |
| 7006–7006 | skip-site | `emit-loop closing brace` |  | `}` | Closes the per-epoch emit loop body (matching the loop over synced epochs). Skipped epochs `continue` above and never reach the snapshot/carry code —  |
| 7008–7008 | phase-block | `Eigen thread restore` |  | `if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);` | Restores Eigen's own thread count after the parallel loop. OpenMP/Eigen threading teardown — byte-identity note: Eigen auto thread binding. |
| 7318–7325 | phase-block | `zero-epoch error return` |  | `if (n_emitted == 0) {` | If no epochs emitted, log->error with common-cause diagnostics and return 1. runNetwork failure exit. Orchestration return path. |
| 7326–7329 | phase-block | `runNetwork success return + closing braces` |  | `return 0;` | return 0 (success), closing brace of runNetwork at 7327, then `} // namespace GnssProcessor` at 7329 (EOF). Function + namespace close. |

---

## INC 1 — config-loader cut  →  `src/core/config/GnssConfigLoadNetwork.cpp`

*Boundaries in `src/core/config/GnssConfigLoad.cpp` (advisory lines; anchors authoritative).*

| Lines (adv.) | Symbol | Start-anchor |
|---|---|---|
| 2762–2799 | `parseCoordEpochYear (anon-namespace helper — move the ENTIRE `namespace { ... } // namespace` wrapper, 2762-2799; it contains ONLY this one function)` | `namespace {          /**          * @brief RTA-12: parse a coord` |
| 2804–4406 | `GnssConfig::loadNetworkSection_` | `bool GnssConfig::loadNetworkSection_(const nlohmann::json& json,` |
| 4408–4480 | `GnssConfig::applyNetworkConsumerCouplings` | `void GnssConfig::applyNetworkConsumerCouplings(GnssConfig& c, bo` |
| 4482–4502 | `GnssConfig::deriveClosedLoopConsumerConfig` | `GnssConfig GnssConfig::deriveClosedLoopConsumerConfig() const {` |
| 4504–4545 | `GnssConfig::applyNetworkConsumerProfile` | `void GnssConfig::applyNetworkConsumerProfile(LogManager& log_man` |

**Cross-section forcing writes that MOVE with the network section:**

- estimate_sat_clocks_ = true; (line 3137) — force ON, V1 producer
- estimate_sat_phase_biases_ = true; (line 3138) — force ON, V1 producer
- estimate_sat_biases_ (line 3139 default=false; re-enabled =true at line 3169 under W6 estimate_network_sat_code_bias_) — one estimation-section field, two write sites
- promote_sat_code_bias_ (line 3140 default=false; re-enabled =true at line 3170 under W6) — one estimation-section field, two write sites
- estimate_ifb_ = true; (line 3171 under W6 sat-code-bias, AND line 3335 under GLONASS admission) — one estimation-section field, two write sites in the network section
- global_vtec_sh_.enabled = true; (line 3165, under estimate_network_iono_sh_)
- iono_weighting_.enabled = true; (line 3178, under W6 sat-code-bias)
- model_corrections_.enable_code_phase_bias = false; (line 3183, under W6 — force external .BIA OFF so the network OSB is the sole code-bias source)

**Stays behind (must NOT move) / hazards:**

- The estimation-section raw-JSON peek at network.estimate_glonass at lines 999-1000: `const bool glonass_network = jval<bool>(json, "network.estimate_glonass", "estimate_glonass", false);` inside loadEstimationSection_ (the enable_glonass_ifb_/estimate_ifb_ warning-suppression guard, ~lines 991-1005). This READS a network.* key but lives in the estimation section and STAYS. It must keep working after the split — it deliberately peeks at the network JSON to suppress a misleading warning because loadNetworkSection_ (which force-enables estimate_ifb_ + enable_glonass_ifb_ under network.estimate_glonass) dispatches AFTER the estimation section. Do NOT move this; and preserve its comment referencing loadNetworkSection_.
- estimate_phase_clock_ = false; (line 3186, under W6) — an ADDITIONAL cross-section forcing write beyond the canonical eight (a 9th coupling); it mutates the estimation-owned estimate_phase_clock_ and MUST move with the network section too. Flagged separately so the '8' count is not read as exhaustive — the true set of estimation/model fields force-written by loadNetworkSection_ is: estimate_sat_clocks_, estimate_sat_phase_biases_, estimate_sat_biases_, promote_sat_code_bias_, estimate_ifb_, global_vtec_sh_.enabled, iono_weighting_.enabled, model_corrections_.enable_code_phase_bias, PLUS estimate_phase_clock_ (9) and enable_glonass_ifb_ (10, line 3336).
- enable_glonass_ifb_ = true; (line 3336, under GLONASS admission) — a 10th cross-section forcing write (estimation-owned glonass_ifb flag), also moves with the network section.
- CLOSING-LOG SLICE HAZARD in loadNetworkSection_: the terminating brace is at line 4406 and `return true;` at 4405, but immediately before them (lines 4385-4404) sit THREE multi-line log->info summary blocks — 'Network mode: {} stations…' (4385-4391), the per-station-parallelism ENABLED block (4392-4399), and the else SERIALLY block (4400-4404). A naive end-line (e.g. cutting at the first-seen `return true;`/bare `}` or an off-by-a-few line count) will slice through the middle of one of these log->info calls (e.g. mid-block at 4391/4392 or truncate the else at 4404). The whole 2804-4406 span, log blocks included, must move as a unit.
- Note: enable_glonass_network_ (line 3301), estimate_network_sat_code_bias_ (3152/3164), estimate_network_iono_sh_ (3161), network_service_mode_, network_apriori_source_, network_apriori_clock_anchor_sigma_m_ are network-OWNED mode flags parsed via jval INSIDE the section — they move WITH loadNetworkSection_ and are NOT cross-section writes (their home is the network section). Listed here only to disambiguate them from the estimation-owned forcing writes above.

---

## Cross-epoch state inventory — `NetworkCarryState` (three-place rule)

*Every mutable that lives OUTSIDE the per-epoch loop. `[SNAP]` = participates in the warm-resume fingerprint+restore+snapshot-write triple (all three edits live in `NetworkRunContext.cpp`). Re-derived post-C1: `SharedCarryCov`/`cap_dense_*` are the dense-carry additions.*

| Snap | Field | Type | Note |
|:--:|---|---|---|
| ✓ | `prev_carry` | `std::vector<CarryPrior>` | L1437. D2 shared-carry stash (Clock/PhaseBias/CodeBias values+var+inflate_q flag), identity-keyed by (kind,sat,signal).  |
|  | `prev_carry_cov` | `SharedCarryCov {std::map<std::string,std` | L1438. C1 dense (P_prev+Q)^-1 side-car of prev_carry over Clock/PhaseBias/CodeBias entries; identity-keyed row map via s |
|  | `cap_dense_keys / cap_dense_cov` | `std::vector<NetworkEstimator::SharedPrio` | L3400-3401. NOT cross-epoch — re-declared INSIDE the epoch loop each iteration as per-epoch scratch (captured at dense f |
|  | `fc_cols` | `std::vector<Eigen::Index>` | L6752. NOT cross-epoch — local to the stash block; shared-border column index per dense carry row, used only to copy cov |
| ✓ | `local_carry` | `std::map<std::tuple<int,std::string,std:` | L1489. Phase-leveling per-station ambiguity carry (value_abs,var,last_seen_s), keyed (station idx, sat, signal). ORDERED |
| ✓ | `zwd_carry` | `std::map<int, LocalCarry>` | L1492. B9 per-station ZWD carry keyed by station index (one ZWD per station). Inert unless zwd_on. Snapshot: snap.zwd_ca |
| ✓ | `clock_osc` | `std::map<std::string, ClockOsc>` | L1465. Two-state oscillator clock [offset x0, drift x1] + 2x2 cov (p00/p01/p11) + per-sat t_last + init flag. REPLACES s |
| ✓ | `prev_clk` | `std::map<std::string, std::pair<double,d` | L1462. Clock-monitor adjacent-epoch jump baseline: sat -> {Δδtˢ(m), seen_s}. Erased on refloat so post-reinit clock re-b |
| ✓ | `prev_pivot` | `std::map<std::string, std::string>` | L1439. constellation -> pivot sat, advanced at emit; drives internal handover detection. Snapshot: snap.prev_pivot. |
| ✓ | `last_pivot` | `std::map<std::string, std::string>` | L1445. A1 persistent last-EVER pivot per constellation; MERGE-updated (never wholesale-overwritten), NEVER erased. Read  |
|  | `anchor_incumbent / this_epoch_anchor / emitted_anchor` | `std::map<std::string,std::string> x3` | L1456-1458. Prioritized phase-bias anchor incumbency, keyed 'sat_signal'. anchor_incumbent = sticky incumbent station na |
| ✓ | `c1_zoh_active` | `std::set<std::string>` | L1440. L2/T5 sats currently emitting c1=0 (ZOH), edge-trigger for the R6 event. Snapshot: snap.c1_zoh_active. |
| ✓ | `relevel_iode` | `std::map<std::string,int>` | L1476. Card 6 brdc_rts: broadcast IODE each sat's carried clock state is referenced to. Advanced at eph-cutover RE-LEVEL |
| ✓ | `eph_cutover_disc_pending` | `std::set<std::string>` | L1483. Card 6: sats whose eph rolled but prior version evicted (Δ_eph incomputable) -> forced disc queued to next emit.  |
| ✓ | `ura_sigma0_ewma` | `double (=1.0)` | L1494. B1 network-wide σ̂0 EWMA scaling emitted URA (one-epoch lag). Inert unless ura_on. Snapshot: snap.ura_sigma0_ewma |
| ✓ | `ura_refloat_horizon` | `std::map<std::string,int>` | L1495. sat -> epochs remaining of post-refloat URA inflation; aged down each epoch (L6743). Snapshot: snap.ura_refloat_h |
| ✓ | `ura_retro_sats / retro_active` | `std::set<std::string> x2` | L1503-1504. B1 retro-bump: ura_retro_sats = sats flagged LAST epoch, applied as per-sat disc at NEXT actual emit (surviv |
| ✓ | `ura_ewma_updates` | `size_t (=0)` | L1512. RTB-7 σ̂0 EWMA update count (post-fit RAIM tier); half of the warmup ramp-lift rule. Snapshot: snap.ura_ewma_upda |
| ✓ | `ramp_lifted` | `bool (=false)` | L1513. RTB-7 warmup URA floor lifted (product-grade reached). Warm resume restores as NOT-degraded (grade 0). Snapshot:  |
| ✓ | `first_emit_content_s` | `double (=-1.0)` | L1514. RTB-7 content time of FIRST emit (grade anchor). Snapshot: snap.first_emit_content_s (grade metric preserved acro |
|  | `grade_metric_s` | `double (=-1.0)` | L1515. RTB-7(c) cold-start-to-product-grade seconds; -1 before lift, 0 = warm resume. Derived (recomputed at resume from |
|  | `start_anchor_s` | `double (=-1.0)` | L1484. First-epoch time anchor for start_offset_hours fast-forward. Testing knob, set once at first synced epoch (L2359) |
| ✓ | `prev_tow / prev_week` | `double(-1.0) / int(-1)` | L1710-1711. Previous processed epoch GPS time; drives D2 q·dt gap inflation across epochs. Snapshot: snap.prev_tow, snap |
| ✓ | `iod` | `int (=0)` | L1712. Issue-of-data counter. Snapshot: snap.iod. |
| ✓ | `generation_id` | `uint32_t (=0)` | L1716. RTA-3 per-run solution-generation id, minted at FIRST emit from epoch GPS time; 0=unset. Warm resume PRESERVES th |
|  | `last_contract_emit_s` | `double (=-1.0)` | L1719. RTA-5 product-time of last written S-contract line; -1 until writer opens; drives contract re-broadcast (L5574).  |
| ✓ | `last_emit_content_s` | `double (=-1.0)` | L1723. RTB-6 content time of last EMITTED epoch (health last_emit_age). Snapshot: snap.last_emit_content_s. |
| ✓ | `last_health_write_s` | `double (=-1.0)` | L1722. Content time of last health heartbeat row. Snapshot: snap.last_health_write_s. |
|  | `last_snapshot_write_s` | `double (=-1.0)` | L2144. Product time of last snapshot write; gates snapshot cadence (L6922). Declared with the RTB-7 block (after the mai |
|  | `last_n_present / last_n_clock_sats` | `int(0)/int(0)` | L1724-1725. Most-recent processed-epoch station-present + registered-clock-sat counts for the health heartbeat (set L334 |
|  | `health_win_ms_anchor / health_win_epochs_anchor` | `double(0.0)/size_t(0)` | L1730-1731. P4(b) per-window ms/epoch anchors (wall ms + loop-epoch count as of the previous health row); reset each hea |
|  | `n_loop_epochs` | `size_t (=0)` | L2105. Count of epochs that entered processing (past fast-forward/max-duration); the scaling/timing denominator paired w |
|  | `max_dur_anchor_s / max_dur_anchored` | `double(0.0)/bool(false)` | L1756-1757. processing.max_duration_s cutoff anchor+latch, set once at first processed epoch (L2368). Loop-control latch |
|  | `spectrum_diag_done` | `bool (=false)` | L1758. One-shot estimability-spectrum dump latch (set L4165, gated on nc.trace_estimability). Diagnostic latch, not snap |
|  | `contract_pending / pending_contract` | `bool(false) / SbasisContract` | L917-918. Deferred v2 S-basis contract writer one-shot: pending_contract holds startup-known fields, realized per-const  |

**`NetworkServiceCounters` (per-run, non-carry):**

- n_emitted (size_t, L1720) — snapshot: snap.n_emitted
- n_skipped (size_t, L1720) — snapshot: snap.n_skipped
- n_gate_withheld (size_t, L1720) — snapshot: snap.n_gate_withheld
- n_runaway_epochs (size_t, L1720) — snapshot: snap.n_runaway_epochs
- n_sigma0_withheld (size_t, L1732) — snapshot: snap.n_sigma0_withheld (subset of n_skipped)
- n_perconst_partial_epochs (size_t, L1740) — snapshot: snap.n_perconst_partial
- n_perconst_diffuse_published (size_t, L1741) — snapshot: snap.n_perconst_diffuse
- n_healed_epochs / n_healed_cols (size_t, L1736) — prioritized-datum drop-and-heal observability; NOT snapshotted
- n_gated_epochs / n_gated_key_epochs (size_t, L1739) — phase_datum_min_stations coverage-gate tallies; NOT snapshotted
- FlagRate fr_if_slip / fr_we_trigger / fr_raim_cell / fr_raim_sat / fr_attrib / fr_clk_model / fr_clk_mon (struct FlagRate{long opp,flag} x7, L1748-1755) — B8 empirical-alpha flag-rate counters, gated on trace_flag_rates, summarized at end of run, NEVER snapshotted
- HEALTH-WINDOW ANCHORS: last_health_write_s / last_emit_content_s (snapshotted); health_win_ms_anchor / health_win_epochs_anchor / last_n_present / last_n_clock_sats (NOT snapshotted) — see carry_fields
- TIMERS: t_loop_begin (const steady_clock, L2104) + n_loop_epochs (loop-scoped, not snapshotted)

**`NetworkKnobs` note:** NetworkKnobs read-order concern: the ~60 `const bool/double ... = config_.networkXxx()` cache reads span L1516-1567 (pl_on..start_offset_s) PLUS a second scattered band at L1085-1087 (q_clock/q_pb/q_cb from getProcessNoise) and L1433-1434 (es_sigma0_trigger/withhold). The three-place-rule refactor must snapshot ALL of these into a single NetworkKnobs struct read ONCE — the concern is that several knobs already have interdependencies computed at read time that would break if reordered/re-read lazily: clk_mon_couple = clk_mon_on && networkClockMonitorCoupleRaim() (L1538, depends on an earlier-read cache, not the raw getter); clk_mon_reinit_var = clk_mon_reinit_sig^2 (L1540); clk_model_r = networkClockModelMeasStdM()^2 (L1564); clk_model_max_gap DEFAULTS to pl_max_gap_s when unset (L1566 comment) so pl_max_gap_s must be resolved first; the snapshot config-fingerprint (L2152-2184) reads MANY of these cached locals (not the getters) to build the warm-resume gate, so any knob added to NetworkKnobs that is datum-relevant MUST also enter that fingerprint or a resume across its change silently folds carry in the wrong frame. Read-order is currently correct only because these are sequential const initializers; moving them into a struct is safe but the derived members (clk_mon_couple, *_var, clk_model_r, clk_model_max_gap default) must stay computed AFTER their inputs.\n\nclk_anchor_sigma purity question: clk_anchor_sigma (L2349, const double = config_.resolveNetworkAprioriClockAnchorSigmaM()) is loop-INVARIANT and pure — resolved exactly once, AFTER the warm-resume block but BEFORE the epoch loop. It belongs in NetworkKnobs, NOT NetworkCarryState. Two call sites read it (fold L4606 clk_sigma, B7 diagnostic L5782 clk_sigma_a) and the comment at L2343-2348 explicitly states the resolution policy lives in GnssConfig as the single source of truth (explicit>0 wins; brdc_rts auto->3.0m; sp3 auto->initial_std.sat_clock). Purity is clean: it reads only config, no cross-epoch state, no per-epoch input. The only subtlety is that networkAprioriClockAnchorSigmaM() (the RAW config value, not the resolved one) is in the warm-resume fingerprint (L2183) while clk_anchor_sigma is the RESOLVED value — correct, because the resolution is deterministic from config so fingerprinting the raw input suffices."

**`NetStation` fields deliberately NOT snapshotted (exemption-commented):**

- NetStation.if_pair_scale (std::map<std::string,IfPairScale{double ewma_scale; long count}>, L146-150) — per-signal-pair EWMA of elevation-normalized robust IF-phase noise scale + clean-sample counter; persists across epochs on the NetStation, drives the pool->per-station shrinkage in the PASS-A IF-phase slip threshold. DELIBERATELY NOT snapshotted (per-station filter state cold-starts on warm resume, L6929-6935).
- NetStation.mw_slip (std::unique_ptr<MwSlipDetector>, L135) — per-station MW cycle-slip arc state (Welford/MAD arc statistics inside the detector); persistent, station-local, parallel-safe. NOT snapshotted (slip-detector history re-warms).
- NetStation.turbo_edit (std::unique_ptr<TurboEditResolver>, L136) — MW+GF integer-repair resolver arc state; NOT snapshotted.
- NetStation.if_slip (std::unique_ptr<IfPhaseSlipDetector>, L141) — IF-phase triple-difference between-sat/between-epoch OMC detector state; NOT snapshotted.
- NetStation.pinned_amb_keys (std::set<std::string>, L160) — prioritized-datum 'sat_signal' keys this station anchors THIS epoch; cleared+rebuilt every epoch by the serial anchor election, so not cross-epoch persistent (per-epoch scratch on a persistent object) and not snapshotted.
- NetStation.sm (StateManager) / .model (PppModel) / .mm (MeasurementModel) — per-station StateManager ambiguity/iono/receiver-bias marks + per-station covariance evolve across epochs but are DELIBERATELY NOT snapshotted (L6929-6935: 'per-STATION filter state is DELIBERATELY NOT snapshotted... cold-start and re-warm within a few epochs; post-fit RAIM absorbs the transient'). This is the RTB-7 recovery-budget trade — the design boundary between NetworkCarryState (shared, network-level, snapshotted) and per-station filter state (not snapshotted).
- NetStation.pstate (PppState, L128) — per-station position/clock/tropo scratch; recomputed each epoch, not cross-epoch carry.
- emitter (NetworkProductEmitter, L899) — owns its OWN cross-epoch stream state (disc-counter baselines, first-sighting sets, pending bumps, last-seen stamps) that IS snapshotted separately via emitter.restoreState/serialize into snap.emitter_state (L2229, L6997) — a SECOND state container alongside the driver carries, restored all-or-nothing BEFORE the driver layer.

---

## Output-artifact surface (byte-identity harness reference)

*Consumed by `scripts/compare_network_run.py`. The set is CONFIG-DEPENDENT — each golden records its own exact set; the harness fails on any missing/extra file.*

| Artifact | Kind | Trip condition |
|---|---|---|
| `<log_dir>/network_trace.csv` | csv-trace | ALWAYS-ON (unconditional; header written at startup, rows every processed epoch) |
| `<log_dir>/network_solution.csv` | csv-trace | ALWAYS-ON (unconditional). Rows only on EMITTED epochs (guarded by `if (soln)` a |
| `<log_dir>/network_stations.csv` | roster | network.trace_stations == true (nc.trace_stations). Static one-shot at startup;  |
| `<log_dir>/network_ionosphere.csv` | csv-trace | network.trace_ionosphere == true (nc.trace_ionosphere) |
| `<log_dir>/network_vtec_sh.csv` | csv-trace | config_.estimateNetworkIonoSh() && global_vtec_sh.trace (shc.trace) both true |
| `<log_dir>/network_if_slip.csv` | csv-trace | network.trace_slip_events == true (nc.trace_slip_events) |
| `<log_dir>/network_raim.csv` | csv-trace | network.trace_slip_events == true (nc.trace_slip_events) |
| `<log_dir>/network_raim_attrib.csv` | csv-trace | network.trace_slip_events == true (nc.trace_slip_events). Rows only when an enti |
| `<log_dir>/network_clock_monitor.csv` | csv-trace | nc.trace_slip_events && (config_.networkClockMonitor() \|\| config_.networkClock |
| `<log_dir>/network_anchor_info.csv` | csv-trace | network.trace_anchor_info == true (nc.trace_anchor_info) |
| `<log_dir>/network_dd_fractional.csv` | csv-trace | network.trace_dd_fractional == true (nc.trace_dd_fractional) |
| `<log_dir>/network_slip_raw.csv` | csv-trace | network.trace_slip_raw == true (nc.trace_slip_raw) |
| `<log_dir>/network_epoch_status.csv` | csv-trace | network.trace_epoch_status == true (nc.trace_epoch_status). One row per processe |
| `<log_dir>/network_service_health.csv` | health | network.health_interval_s > 0.0 (nc.health_interval_s). Appended every health_in |
| `<log_dir>/network_events.csv` | log | network.trace_events == true (nc.trace_events). Long-format, one row per discret |
| `<log_dir>/network_zwd.csv` | csv-trace | network.trace_zwd == true (nc.trace_zwd) |
| `<log_dir>/network_station_residuals.csv` | csv-trace | network.trace_station_residuals == true (nc.trace_station_residuals). Rows only  |
| `<log_dir>/closed_loop_trace.csv` | csv-trace | closed_loop_on && consumer (network.closed_loop enabled with a held-out station  |
| `<log_dir>/network_flag_rates.csv` | run-summary | network.trace_flag_rates == true (trace_flag_rates). Written once at end of run |
| `<log_dir>/network_run_summary.csv` | run-summary | network.trace_flag_rates == true (same toggle as flag_rates). Written once at en |
| `<log_dir>/network_contract_check.csv` | manifest | network.serialize_sbasis_contract == true (nc.serialize_sbasis_contract) AND (pr |
| `<log_dir>/network_run_manifest.json` | manifest | network.write_run_manifest == true (nc.write_run_manifest). JSONL: startup objec |
| `<network.product_output_path> (relative ` | product | network.product_output_path non-empty. Header S-line only when network.serialize |
| `<network.state_snapshot_path> (relative ` | snapshot | network.state_snapshot_path non-empty. Written on EMITTED epochs only, every net |
| `<log_dir>/<module>.log  (main_log.log, p` | log | ALWAYS-ON per logger touched (5MB x3 rotation). Not a network CSV but part of th |

**Wall-clock / build-stamp fields masked (field-level, frozen in the harness maskspec):**

- network_run_manifest.json startup record: written_utc (ApplicationNetwork.cpp:1611, strftime %Y-%m-%dT%H:%M:%SZ of std::time(nullptr))
- network_run_manifest.json startup record: build.compiled = __DATE__ " " __TIME__ (ApplicationNetwork.cpp:1614, compile-time build date/time)
- network_run_manifest.json complete record: loop_ms (ApplicationNetwork.cpp:7071, wall-clock steady_clock loop duration in ms)
- network_run_summary.csv: loop_ms column (ApplicationNetwork.cpp:7243, wall-clock steady_clock)
- network_run_summary.csv: ms_per_epoch column (ApplicationNetwork.cpp:7243 = loop_ms/n_loop_epochs, wall-clock derived)
- network_service_health.csv: ms_per_epoch_window column (ApplicationNetwork.cpp:2437, windowed wall-clock ms/epoch from steady_clock)
- network_service_health.csv: last_emit_age_s column (ApplicationNetwork.cpp:2438 emit_age; content-time derived but reflects run progress — determinist
- product file (product_output_path): G-line generation_time week,tow (NetworkProductIO.cpp:437-439; ApplicationNetwork.cpp:5151 sol.generation_time = T
- state snapshot file (state_snapshot_path): M-line wall_utc field (NetworkStateSnapshot.cpp:118-120; ApplicationNetwork.cpp:6939 snap.wall_utc = Networ
- every <module>.log spdlog line: leading [%E] Unix-epoch-seconds wall-clock timestamp (LogManager.cpp:363 set_pattern) — applies to main_log.log and al


---

## INC 0 audit findings (freeze-independent)

**OpenMP determinism (byte-identity precondition).** Both parallel regions
(`ApplicationNetwork.cpp:2589` PASS A tail, `:3414` PASS B produce) use
`schedule(dynamic)`, so run-to-run identity rests entirely on per-index-slot-write
discipline. Audit:

- **No** `#pragma omp critical` / `atomic` / `ordered` / `barrier` anywhere in the file.
- PASS B produce loop body (`for i`) writes only to `asm_res[i]` (the per-station
  `StationAssembly` slot); error paths set flags (`R.empty_innov` / `R.state_mismatch`
  / `R.err`) and `continue` — **no logging, no shared mutation inside the region**.
- All `log->warn(...)` sites in the PASS B span fire in the **serial consume** loop
  that drains `asm_res[i]` in station order (deterministic) — never in the parallel
  produce loop. Same for the single PASS A `log->warn` (geometry-threw error path).
- Consequence: `.log` line ordering is deterministic on clean epochs; even on
  QC-tripping epochs (station drops), only the diagnostic `.log` files could reorder —
  the harness excludes `.log` content from byte-compare by default for exactly this
  reason. The DATA artifacts (CSVs / manifest / snapshot) are written serially.

**Empirical determinism (verified).** `compare_network_run.py selfcheck` on the two
independent parallel 1h runs `runs/c1_fullcov/on_1h` vs `on_1h_v2` reports **CLEAN** —
every `network_*.csv`, the manifest counters, and all data artifacts are byte-identical
run-to-run under the frozen maskspec; the only run-to-run difference was one wall-clock
duration token in `run.log` (now masked). This is the strongest available evidence that
the parallel path is deterministic *before any code moves*.

**Floating-point mode.** No `/fp:fast` in `CMakeLists.txt` or `cmake/`; MSVC default
`/fp:precise` applies. (Confirm no target-level override before freezing.)

**Harness maskspec (frozen at INC 0).** Wall-clock / build-stamp fields masked
field-level: `network_run_manifest.json` `written_utc` + `build.compiled` + `loop_ms`;
`network_run_summary.csv` `loop_ms` + `ms_per_epoch`; `network_service_health.csv`
`ms_per_epoch_window` + `last_emit_age_s`; state-snapshot M-line `wall_utc`; product
G-line `generation_time` (only under `emit_generation_time`); `run.log` timestamp
prefix + decimal-seconds duration tokens; per-module `.log` `[%E]` epoch prefix. The
maskspec SHA is recorded in every captured golden manifest and is READ-ONLY once frozen.
