/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK NETWORK producer).
 *
 * Extracted from Application.cpp to keep the per-mode processing pipelines in
 * dedicated translation units (mirrors ApplicationBaseline.cpp /
 * ApplicationRealtime.cpp).  The network producer is a joint multi-station
 * estimator: per-station local normals reduced onto a shared satellite-
 * parameter block via an exact Schur complement, estimating the estimable
 * satellite clock + satellite phase bias under an explicitly declared
 * minimum-constraint S-basis, emitted in IGS-SSR convention for the companion
 * PPP_RTK_User consumer.
 *
 * See docs/ssystem_conformance_2026-06-15.md (§7) and the approved design plan
 * for the full architecture and the staged build-out (W1..W8).
 *
 * ===========================================================================
 * W4-S2b — the live epoch driver
 * ===========================================================================
 *
 * Per solution epoch the driver runs the arrowhead estimator end to end:
 *
 *   PASS A (layout)  for each present station: PppModel::computeModels →
 *       MeasurementModel::ensureNetworkSharedStates pre-allocates the shared
 *       satellite states (per-sat clock + per-(sat,signal) phase bias) using the
 *       SAME usability gate the measurement build uses.  The DATUM (pivot)
 *       station is processed first so its usable phase-signal set restricts the
 *       phase-bias columns to ones backed by a pivot-receiver ambiguity pin.
 *       The estimator's shared column layout is then registered (clock for every
 *       satellite any station sees; phase bias for the pivot's signal set).
 *
 *   PASS B (assembly)  for each present station: MeasurementModel::compute-
 *       MeasurementUpdate produces an undifferenced H/innovations/R block (no
 *       receiver-side differencing — DifferencingMode::None).  Its columns are
 *       split: the satellite clock (−1) and phase-bias (+1) columns scatter into
 *       the shared border A_s; the 3 receiver-position columns are DROPPED
 *       (coordinates held fixed); the rest become the station's private nuisance
 *       block A_r.  The pivot station additionally folds tight-prior pins on its
 *       own ambiguities (the LOCAL half of the S-basis — the emitted phase bias
 *       then carries that pivot-receiver ambiguity lump, Odijk Eq. 4).
 *
 *   SOLVE  NetworkSbasis builds the SHARED minimum-constraint datum (one
 *       satellite-clock pin per constellation), the null-space rank gate
 *       verifies the reduced shared system carries exactly that deficiency, then
 *       NetworkEstimator::solve produces the shared estimate.  NetworkProduct-
 *       Emitter serializes it into an OWNED SsrCorrectionStore (closed-loop
 *       isolation — never the process-wide singleton the user side reads).
 *
 *   D2 (between-epoch)  the shared posterior (value, variance) is carried
 *       forward as a diagonal prior on the next epoch's shared border, inflated
 *       by the per-parameter process noise over the epoch gap.
 *
 * Scope (V1): estimable satellite CLOCK + PHASE BIAS, coordinates/orbits fixed,
 * GPS + Galileo, per-band uncombined, float (no network AR).  Satellite code
 * bias, atmosphere products, real-time branch, GLONASS FCN bias, and network QC
 * are later slices (W6/W7/Phase 2).  Each station's coordinates MUST be known
 * (held fixed) and its observation RINEX path MUST be explicit in the config —
 * the PathResolver network gate is deferred, so resolveData() here serves only
 * products + broadcast nav.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // Card 4 (M1) phase D: emit-layer frame re-base
#include "products/NetworkPbEmitFilter.hpp" // M5 prereq: PB emission-continuity filter
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // M4: combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // RTB-7: atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // Card 5 (RTF-1): broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // Card 5 (RTF-1): RINEX-nav → LiveEphemerisStore loader
#include "filter/NetworkEstimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // A1/A2: pivot-handover datum-change detector
#include "filter/NetworkCarryVce.hpp"        // M2: LS-VCE carry-row variance-component monitor
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // Card 4 (M1) C3b: DD cycle-basis invariant carry
#include "filter/NetworkMwWlLayer.hpp"             // M5 card WL-1: MW-seeded wide-lane pseudo-obs layer
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // INC-2: run-loop record types (NetStation / StationAssembly / carry PODs)
#include "filter/NetworkRobustStats.hpp"   // INC-2: median/MAD robust attribution helpers
#include "filter/NetworkPhaseDatum.hpp"    // INC-8b: census + ensemble datum + anchor election + drop-and-heal
#include "filter/NetworkChainDatum.hpp"    // INC-8b: Card-3 eq34 single-receiver-chain datum
#include "filter/NetworkRaim.hpp"        // INC-9: post-fit network RAIM
#include "filter/NetworkClockQc.hpp"    // INC-10: post-solve satellite-clock QC
#include "filter/NetworkSolPrep.hpp"    // INC-11pre: emit-side solution prep + disc signalling
#include "filter/NetworkEmitStore.hpp"    // INC-11: emit / store / product-write + closed-loop
#include "filter/NetworkIonoDiag.hpp"    // INC-11b
#include "filter/NetworkDdFrac.hpp"    // INC-11b
#include "filter/NetworkShDevVtec.hpp"    // INC-11b
#include "filter/NetworkShHealth.hpp"    // INC-11b
#include "filter/NetworkLeveling.hpp"    // INC-11b
#include "filter/NetworkStashStatus.hpp"    // INC-11b
#include "logging/CsvRotator.hpp"     // P1: size-based rotation of the unbounded service CSVs
#include "logging/NetworkRunTraces.hpp" // INC-3: trace-stream ownership + open logic + roster + FlagRate
#include "app/NetworkRunContext.hpp"    // INC-4: knobs + Tier-1 carry state + service counters (ctx)
#include "app/NetworkSetupResult.hpp"   // INC-5: setup-phase handoff (setup) + navAvailabilityGpsSeconds
#include "slip/MwLaneTiers.hpp"         // UWL-1 per-constellation lane-tier band table
#include "slip/MwSlipDetector.hpp"      // T0 phase-leveling slip repair (Increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // INC-2: slip-QC pure helpers (ifElevMap / parseIfPairResetKey)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // B5: signal wavelengths for the DD fractional-cycle diagnostic
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (pin Eigen to 1 thread inside our per-station OpenMP region — avoids nested oversubscription and keeps the result deterministic)
#include <fmt/format.h>

#ifdef _OPENMP
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota (netar ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {

int Application::runNetwork() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    log->info("PPP-RTK Network producer (V1): {} stations, clock datum station '{}'{}",
              nc.stations.size(), nc.clock_datum_station,
              nc.clock_datum_satellite.empty()
                  ? std::string{}
                  : (" + datum sat '" + nc.clock_datum_satellite + "'"));

    // --- Phase 0: input validation — relocated to ApplicationNetworkSetup.cpp
    // (decomposition INC-5); identical error text + exit status.
    if (const int rc = networkValidateInputs_(); rc != 0) return rc;

    // --- Phase 1: resolve products + broadcast nav --------------------------
    // Station RINEX is explicit, so resolveData() here only fills product /
    // navigation paths (orbits + clocks needed for per-station geometry).  A
    // failure is non-fatal: the product-cache load below surfaces a precise
    // "no orbits" diagnostic if products are genuinely missing.
    if (!resolveData()) {
        log->warn("Network: resolveData() failed; relying on explicit product / "
                  "navigation paths in the config (product-cache load will report "
                  "if orbits are missing).");
    }

    // --- Phases 2-4 + sinks + contract + consumer — relocated to
    // ApplicationNetworkSetup.cpp (decomposition INC-5).  `setup` sits at its
    // final address for the whole run: MultiRinexSync references setup.roster,
    // and the closed-loop engine holds &setup.network_store plus a GnssConfig&
    // to setup.consumer_config (see NetworkSetupResult.hpp).
    NetworkSetupResult setup;
    if (const int rc = networkSetup_(setup); rc != 0) return rc;
    // Alias block (INC-3/4/5 migration mechanic): the run loop reads the setup
    // outputs under their former local names; every use-site below is unchanged.
    MultiRinexSync& sync = *setup.sync;
    const auto& synced = sync.getSyncedEpochs();
    const ClosedLoopConfig& cl = nc.closed_loop;
    const bool closed_loop_on = setup.closed_loop_on;
    const bool brdc_rts = setup.brdc_rts;
    auto& brdc_nav_staged = setup.brdc_nav_staged;
    auto& brdc_nav_cursor = setup.brdc_nav_cursor;
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    const int seam_threads = setup.seam_threads;
    const bool carry_par = setup.carry_par;
    SsrCorrectionStore& network_store = setup.network_store;
    NetworkProductEmitter& emitter = *setup.emitter;
    auto& product_writer = setup.product_writer;
    auto& prod_path = setup.prod_path;
    auto& pending_contract = setup.pending_contract;
    auto& contract_pending = setup.contract_pending;
    auto& consumer = setup.consumer;

    // Closed-loop metrics + trace.
    const Eigen::Vector3d cl_truth(cl.station.position_ecef.data());
    const Coordinates::Llh cl_truth_llh =
        closed_loop_on ? Coordinates::ecefToLlh(cl_truth) : Coordinates::Llh{};
    std::size_t cl_epochs = 0, cl_ar_ran = 0, cl_ar_fixed = 0;
    long long   cl_sum_fixed = 0;
    double cl_first_fix_tow = -1.0; int cl_first_fix_week = -1;
    std::deque<double> cl_recent_err3d;   // last-N 3D errors → converged-segment RMS
    constexpr std::size_t kClRecentWindow = 30;

    // ------------------------------------------------------------------
    // Decomposition INC-4: the network run context.  ctx.knobs holds the const
    // config snapshots (NetworkKnobs::snapshot preserves the original getter
    // read order); ctx.carry the Tier-1 cross-epoch run state (the RTB-7
    // snapshot's superset); ctx.counters the service counters/anchors.  The
    // alias blocks that replaced the former declaration stretches keep every
    // downstream use-site textually unchanged (the INC-3 migration mechanic).
    NetworkRunContext ctx(config_, nc);

    // D2 process noise (variance growth per second) for the carry-forward prior.
    const double q_clock = ctx.knobs.q_clock;        // m²/s
    const double q_pb    = ctx.knobs.q_pb;   // m²/s
    const double q_cb    = ctx.knobs.q_cb;    // m²/s (W6 OSB carry-forward)

    // --- Trace ----------------------------------------------------------------
    // Stream + path ownership and the startup open logic live in
    // logging/NetworkRunTraces (decomposition INC-3); each stream's full
    // documentation — column semantics, arming toggle, P1 rotation policy — is
    // on the member in that header.  The references below keep every writer
    // use-site in this TU textually unchanged; the per-epoch writers themselves
    // still live here and move with the increments that introduce the state
    // they read (INC-4+).
    NetworkRunTraces traces(log_dir_);
    auto& cl_trace             = traces.cl_trace;
    auto& trace                = traces.trace;
    auto& soln                 = traces.soln;
    auto& iono_trace           = traces.iono_trace;
    auto& vtec_sh_trace        = traces.vtec_sh_trace;
    auto& vtec_sh_health_trace = traces.vtec_sh_health_trace;
    auto& if_slip_trace        = traces.if_slip_trace;
    auto& raim_trace           = traces.raim_trace;
    auto& raim_attrib_trace    = traces.raim_attrib_trace;
    auto& clk_mon_trace        = traces.clk_mon_trace;
    auto& anchor_info_trace    = traces.anchor_info_trace;
    auto& dd_frac_trace        = traces.dd_frac_trace;
    auto& slip_raw_trace       = traces.slip_raw_trace;
    auto& epoch_status_trace   = traces.epoch_status_trace;
    auto& health_trace         = traces.health_trace;
    auto& events_trace         = traces.events_trace;
    auto& zwd_trace            = traces.zwd_trace;
    auto& station_res_trace    = traces.station_res_trace;
    auto& carry_vce_trace      = traces.carry_vce_trace;
    // Paths referenced outside the open logic (P1 rotation + the exit summary).
    const auto& trace_path        = traces.trace_path;
    const auto& soln_path         = traces.soln_path;
    const auto& health_trace_path = traces.health_trace_path;
    std::unique_ptr<PppUtils> dd_ppp_utils;   // B5: wavelength provider (built only when the DD trace is on)
    // Producer-side network AR (network.ambiguity_resolution, Phase 2+): a
    // wavelength provider for the between-station DD meters→cycles conversion,
    // built only when armed (default OFF => the AR block is skipped entirely =>
    // byte-identical float product).
    const auto& netar = nc.ambiguity_resolution;
    std::unique_ptr<PppUtils> ar_ppp_utils;
    if (netar.enable) ar_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    // Slip-QC hardening channel (docs/SPEC_slip_qc_hardening.md): knobs +
    // per-station cross-epoch state + this epoch's raw evidence + run-end
    // counters, threaded by reference into the PASS-A slip seam and the
    // assembly pre-filter.  Sized once here so the per-station EWMAs survive
    // the whole run.
    SlipQcChannel slip_qc;
    {
        const auto& sq = nc.slip_qc;
        auto& sp = slip_qc.params;
        sp.lli_corroborate     = sq.lli_corroborate;
        sp.chatter_frac_hi     = sq.lli_chatter_frac_hi;
        sp.chatter_frac_lo     = sq.lli_chatter_frac_lo;
        sp.chatter_min_epochs  = sq.lli_min_epochs;
        sp.chatter_alpha       = sq.lli_ewma_alpha;
        sp.k_mw                = sq.lli_k_mw;
        sp.k_gf                = sq.lli_k_gf;
        sp.mw_floor_cyc        = sq.lli_mw_floor_cyc;
        sp.gf_floor_m          = sq.lli_gf_floor_m;
        sp.step_alpha          = sq.lli_step_alpha;
        sp.gap_instrument      = sq.gap_instrument;
        slip_qc.hist.resize(stations.size());
    }
    traces.openAll(config_, nc, shc, sh_model.get(), closed_loop_on && consumer, log);
    // B5/raw-slip wavelength providers — gated on the streams openAll just
    // opened (same conditions as the former inline opens); constructed HERE
    // because they need log_manager_, which the traces unit does not hold.
    // The raw-slip provider now serves THREE consumers (the trace plus Cards L
    // and G), so it is built whenever ANY of them asked — spec §1's
    // "compute once per epoch when any consumer needs it".  With none armed it
    // stays null and the whole raw-observable path is skipped.
    if (dd_frac_trace) dd_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    if (slip_qc.params.anyConsumer(static_cast<bool>(slip_raw_trace)))
        slip_qc.utils = std::make_unique<PppUtils>(log_manager_);



    // === Epoch loop =========================================================
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_clock_refloat_sats = ctx.carry.prev_clock_refloat_sats;
    auto& tu_reinit_hist = ctx.carry.tu_reinit_hist;
    auto& prev_carry_cov = ctx.carry.prev_carry_cov;
    auto& prev_pivot = ctx.carry.prev_pivot;
    auto& c1_zoh_active = ctx.carry.c1_zoh_active;
    auto& last_pivot = ctx.carry.last_pivot;
    auto& held_clock_ref = ctx.carry.held_clock_ref;
    auto& this_epoch_if_pin_sel = ctx.carry.this_epoch_if_pin_sel;
    auto& last_if_pin_sel = ctx.carry.last_if_pin_sel;
    auto& anchor_incumbent = ctx.carry.anchor_incumbent;
    auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
    auto& emitted_anchor = ctx.carry.emitted_anchor;
    auto& eq34_datum_rx = ctx.carry.eq34_datum_rx;
    auto& eq34_rx_parent = ctx.carry.eq34_rx_parent;
    auto& eq34_sat_root = ctx.carry.eq34_sat_root;
    auto& eq34_prev_el = ctx.carry.eq34_prev_el;
    auto& eq34_emitted_pdr = ctx.carry.eq34_emitted_pdr;
    auto& eq34_ppp_utils = ctx.carry.eq34_ppp_utils;
    auto& eq34_reref_total = ctx.carry.eq34_reref_total;
    auto& eq34_erase_total = ctx.carry.eq34_erase_total;
    auto& eq34_shift_total = ctx.carry.eq34_shift_total;
    auto& eq34_gap_repin_total = ctx.carry.eq34_gap_repin_total;
    auto& eq34_mismatch_epochs = ctx.carry.eq34_mismatch_epochs;
    auto& eq34_retained_edges = ctx.carry.eq34_retained_edges;
    auto& eq34_prev_target = ctx.carry.eq34_prev_target;
    auto& eq34_reref = ctx.carry.eq34_reref;
    auto& prev_clk = ctx.carry.prev_clk;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& clock_rate_est = ctx.carry.clock_rate_est;
    auto& relevel_iode = ctx.carry.relevel_iode;
    auto& eph_cutover_disc_pending = ctx.carry.eph_cutover_disc_pending;
    auto& basis_relatch_disc_pending = ctx.carry.basis_relatch_disc_pending;
    auto& code_bias_prev_ = ctx.carry.code_bias_prev_;
    auto& code_bias_ifpin_pair_ = ctx.carry.code_bias_ifpin_pair_;
    auto& this_epoch_cb_ifpin = ctx.carry.this_epoch_cb_ifpin;
    auto& this_epoch_cb_reseed = ctx.carry.this_epoch_cb_reseed;
    auto& code_bias_sat_seen_ = ctx.carry.code_bias_sat_seen_;
    auto& code_bias_datum_resumed_ = ctx.carry.code_bias_datum_resumed_;
    auto& cb_reseed_disc_pending = ctx.carry.cb_reseed_disc_pending;
    auto& start_anchor_s = ctx.carry.start_anchor_s;
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    auto& rpb_carry = ctx.carry.rpb_carry;
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& ura_refloat_horizon = ctx.carry.ura_refloat_horizon;
    auto& arc_spike_streak = ctx.carry.arc_spike_streak;
    auto& ura_retro_sats = ctx.carry.ura_retro_sats;
    auto& retro_active = ctx.carry.retro_active;
    auto& ura_ewma_updates = ctx.carry.ura_ewma_updates;
    auto& ramp_lifted = ctx.carry.ramp_lifted;
    auto& first_emit_content_s = ctx.carry.first_emit_content_s;
    auto& grade_metric_s = ctx.carry.grade_metric_s;
    const bool   pl_on         = ctx.knobs.pl_on;
    const double pl_q_amb      = ctx.knobs.pl_q_amb;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const double pl_max_gap_s  = ctx.knobs.pl_max_gap_s;
    const double pl_var_floor  = ctx.knobs.pl_var_floor;  // lock-in guard; 0 = off
    const bool   pl_warmup_on_conv  = ctx.knobs.pl_warmup_on_conv;  // T1d
    const bool   pl_refloat_on_conv = ctx.knobs.pl_refloat_on_conv; // T1d
    const double pl_conv_rms_thresh = ctx.knobs.pl_conv_rms_thresh;     // T1d
    const double pb_var_floor  = ctx.knobs.pb_var_floor;  // B4: D2 phase-bias carry floor; 0 = off
    const double cb_var_floor  = ctx.knobs.cb_var_floor;  // B4: D2 code-bias carry floor;  0 = off
    const bool   fullcov_on    = ctx.knobs.fullcov_on;       // C1: dense (P_prev+Q)⁻¹ shared carry
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // Card 1: conditional local carry (coupling rows) — requires fullcov_on (enforced at load)
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint; // Card 1b: JOINTLY whiten the coupling rows (departed cols → M_w) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_tu   = ctx.knobs.carry_cpl_tu;  // Card 1d: border time update of the coupling rows at injection (κ shrink + M²κq inflation) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_shmarg = ctx.knobs.carry_cpl_shmarg; // Card 1d-SH: fully marginalize VtecSh coupling terms (GM border ≠ RW κ model — severs the amb↔SH carry feedback) — requires carry_cpl_cond (enforced at load)
    const bool   zwd_on        = ctx.knobs.zwd_on;                 // B9: carry per-station ZWD across epochs
    const double zwd_q         = ctx.knobs.zwd_q;                // B9: ZWD carry random-walk process noise (m²/s)
    // M2 (LS-VCE carry-q): the carry-row variance-component MONITOR.  Accumulates
    // the post-fit carry innovations (carried − solved) into a residual-form
    // LS-VCE estimator and logs σ̂ (Floor m² + Rate m²/s) per family per
    // constellation against the hand floors.  NO feedback into the solve — the
    // whole path is gated on carry_vce, so OFF is byte-identical.  The estimator
    // is seeded with the PL floor/q as its weak a-priori (worth ~1 row; immaterial
    // vs the thousands of carry rows/epoch — pb/cb families share that weak PL-
    // scale prior, which only sets the regularisation and the no-data fallback).
    const bool   carry_vce     = ctx.knobs.carry_vce;
    const std::vector<std::string> vce_families =
        carry_vce ? NetworkCarryVce::parseComponents(config_.networkCarryVceComponents())
                  : std::vector<std::string>{};
    const bool   vce_wants_pb = std::find(vce_families.begin(), vce_families.end(),
                                          std::string("pb")) != vce_families.end();
    const bool   vce_wants_cb = std::find(vce_families.begin(), vce_families.end(),
                                          std::string("cb")) != vce_families.end();
    NetworkCarryVce carry_vce_est(vce_families,
        static_cast<double>(config_.networkCarryVceWindowEpochs()),
        /*split_by_const=*/true,
        /*seed_floor=*/pl_var_floor, /*seed_rate=*/pl_q_amb);
    int last_vce_hour = -1;   // content-hour bucket for the per-hour VCE log line
    double first_vce_s = -1.0;  // content time of the first monitored epoch (hour anchor)
    const bool   ura_on        = ctx.knobs.ura_on;                  // B1: emit per-sat URA quality channel
    const double ura_alpha     = ctx.knobs.ura_alpha;             // B1: σ̂0 EWMA smoothing
    const bool   ura_retro     = ctx.knobs.ura_retro;             // B1: retro-bump flagged SATELLITE(s) on next emit
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate; // B1: URA multiplier post-refloat
    const bool   raim_on       = ctx.knobs.raim_on;            // post-fit network RAIM (Increment 3, T2)
    const double raim_k        = ctx.knobs.raim_k;      // standardized post-fit residual flag threshold
    const double raim_s0_floor = ctx.knobs.raim_s0_floor; // floor on the robust a-posteriori σ̂0 (1.0 default; lower lets σ̂0 < 1)
    const int    raim_min_sta  = ctx.knobs.raim_min_sta; // cross-station consensus count
    const int    raim_arc_persist = ctx.knobs.raim_arc_persist; // consecutive-epoch persistence gate for isolated-arc refloats (1 = act immediately, byte-identical)
    const bool   raim_maturity_gate   = ctx.knobs.raim_maturity_gate;   // T2d: gate isolated-arc refloats on maturity
    const int    raim_maturity_epochs = ctx.knobs.raim_maturity_epochs;  // T2d: maturity horizon
    const double raim_withhold = ctx.knobs.raim_withhold; // A4: >0 withholds (not emits) an epoch whose post-adaptation σ̂0 exceeds this ceiling; 0 = off
    const bool   raim_redun    = ctx.knobs.raim_redun; // B2: Baarda per-row r_i normalization of the RAIM residual
    const bool   clk_mon_on    = ctx.knobs.clk_mon_on;       // per-sat carried-clock sanity monitor + refloat
    const double clk_mon_abs   = ctx.knobs.clk_mon_abs;  // |Δδtˢ − median| refloat bound (m)
    const double clk_mon_jump  = ctx.knobs.clk_mon_jump; // one-epoch clock jump refloat bound (m)
    const bool   clk_mon_couple = ctx.knobs.clk_mon_couple;  // also refloat RAIM-flagged sats' clocks (opt-in)
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;  // >0: inflate-q to this σ; ≤0: hard skip-stash refloat
    const double clk_mon_reinit_var = ctx.knobs.clk_mon_reinit_var;   // reinit variance folded next epoch for a flagged sat
    const bool   pivot_vr_signal = ctx.knobs.pivot_vr_signal;  // A1: also signal a vanish-return pivot change
    const bool   emit_clock_disc = ctx.knobs.emit_clock_disc;            // A2: clock disc channel + per-constellation bump scoping
    const bool   sticky_pivot    = ctx.knobs.sticky_pivot;              // RTA-1: keep the incumbent clock pivot while it stays registered
    const bool   clock_sum_to_zero = ctx.knobs.clock_sum_to_zero;    // anchor-free per-constellation clock gauge (Σδtˢ=0) instead of a pivot pin — no pivot/handover machinery
    const bool   clock_brdc_anchored = ctx.knobs.clock_brdc_anchored; // brdc_rts: per-sat broadcast anchors ARE the clock gauge (pre-gate) — no pivot machinery AND no sum-to-zero membership wander
    const bool   clock_signal_basis_hold = ctx.knobs.clock_signal_basis_hold; // W6: HOLD each sat's IF-pin reference pair fixed across epochs (time-consistent δtˢ↔dˢ split) instead of re-selecting from each epoch's coverage — inert without estimate_sat_code_bias
    const bool   cb_time_linked  = ctx.knobs.cb_time_linked;      // Card 2 (W6): impose the code-bias datum (IF pin + per-(const,signal) s2z) as a TIME datum — pin the CHANGE of the previous epoch's solved values, not the level to zero; mutually exclusive with clock_signal_basis_hold (load-validated)
    const bool   scoped_handover = ctx.knobs.scoped_handover;   // RTA-2: per-constellation scoping of the INTERNAL handover response
    const bool   pb_excl_unmatched = ctx.knobs.pb_excl_unmatched; // withhold no-ATX-match (uncalibrated receiver) stations from satellite phase-bias emission
    const bool   emit_gen_time   = ctx.knobs.emit_gen_time;       // RT0-4: stamp wall-clock time-of-generation per emitted epoch
    const bool   emit_gen_id     = ctx.knobs.emit_gen_id;         // RTA-3: stamp a per-run solution-generation id (first-emit epoch time)
    const bool   ser_disc_ctrs   = ctx.knobs.ser_disc_ctrs;    // RTA-4: serialize post-emit disc-counter VALUES on C/B/P records
    const double contract_rebroadcast_s = ctx.knobs.contract_rebroadcast_s; // RTA-5: S-contract re-broadcast period (product time, s; 0 = off)
    const bool   clk_runaway_on  = ctx.knobs.clk_runaway_on;        // RT0-5: per-constellation clock-runaway quarantine
    const double clk_runaway_mag = ctx.knobs.clk_runaway_mag;         // RT0-5: median |clock−global median| ceiling (m)
    const double clk_runaway_rate= ctx.knobs.clk_runaway_rate;      // RT0-5: median |drift| ceiling (m/s)
    const bool   perconst_on       = ctx.knobs.perconst_on;          // RTA-10: per-constellation carve-out of the A4 σ̂0 withhold
    const int    perconst_min_rows = ctx.knobs.perconst_min_rows;   // RTA-10: min σ̂0-pool rows for a constellation to be judgeable
    const bool   emit_epoch_status = ctx.knobs.emit_epoch_status;     // RTA-10: per-epoch H heartbeat in the product file (incl. withheld epochs)
    // Two-state (offset+drift) oscillator clock model (network.clock_model): when on, the
    // satellite clock is carried via the Allan-derived Q model in clock_osc (below) instead of
    // the scalar prev_carry random walk, and a satellite is flagged by the normalized-innovation
    // dynamics-fault test rather than the absolute/jump bounds.
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    const double clk_model_q1   = ctx.knobs.clk_model_q1;
    const double clk_model_q2   = ctx.knobs.clk_model_q2;
    const double clk_model_k    = ctx.knobs.clk_model_k;
    const double clk_model_r = ctx.knobs.clk_model_r;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;  // R11: clock-model carry reseed / drift-reset / C1-ZOH bound (defaults to pl_max_gap_s)
    // Emit C1 (clock rate) from the finite-differenced solved clock WITHOUT the two-state carry
    // (network.emit_clock_rate): decoupled rate estimator (clock_rate_est below) — no prior, no
    // refloat ⇒ byte-identical solution.  Inert when clk_model_on (which already supplies C1).
    const bool   emit_clock_rate_on = ctx.knobs.emit_clock_rate_on;
    const double start_offset_s = ctx.knobs.start_offset_s;  // testing fast-forward (0 = off)

    // L15: run-provenance manifest — startup record (body relocated to
    // NetworkRunTraces, decomposition INC-4; manifest_path is a traces member).
    traces.writeRunManifestStartup(config_, nc, ctx.knobs, config_path_,
                                   stations.size(), n_threads, log);
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& iod = ctx.counters.iod;
    auto& generation_id = ctx.counters.generation_id;
    auto& last_contract_emit_s = ctx.counters.last_contract_emit_s;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& n_gate_withheld = ctx.counters.n_gate_withheld;
    auto& n_runaway_epochs = ctx.counters.n_runaway_epochs;
    auto& last_health_write_s = ctx.counters.last_health_write_s;
    auto& last_emit_content_s = ctx.counters.last_emit_content_s;
    auto& last_n_present = ctx.counters.last_n_present;
    auto& last_n_clock_sats = ctx.counters.last_n_clock_sats;
    auto& health_win_ms_anchor = ctx.counters.health_win_ms_anchor;
    auto& health_win_epochs_anchor = ctx.counters.health_win_epochs_anchor;
    auto& n_sigma0_withheld = ctx.counters.n_sigma0_withheld;
    auto& n_healed_epochs = ctx.counters.n_healed_epochs;
    auto& n_healed_cols = ctx.counters.n_healed_cols;
    auto& n_cpl_joint_fallback = ctx.counters.n_cpl_joint_fallback;
    auto& n_cpl_stash = ctx.counters.n_cpl_stash;
    auto& n_cpl_stash_empty = ctx.counters.n_cpl_stash_empty;
    auto& n_cpl_harvest_fail = ctx.counters.n_cpl_harvest_fail;
    auto& n_cpl_terms_present = ctx.counters.n_cpl_terms_present;
    auto& n_cpl_terms_departed = ctx.counters.n_cpl_terms_departed;
    auto& n_cpl_terms_shrunk = ctx.counters.n_cpl_terms_shrunk;
    auto& n_cpl_row_dump = ctx.counters.n_cpl_row_dump;
    auto& n_cpl_solve_dump = ctx.counters.n_cpl_solve_dump;
    auto& n_gated_epochs = ctx.counters.n_gated_epochs;
    auto& n_gated_key_epochs = ctx.counters.n_gated_key_epochs;
    auto& n_perconst_partial_epochs = ctx.counters.n_perconst_partial_epochs;
    auto& n_perconst_diffuse_published = ctx.counters.n_perconst_diffuse_published;

    // --- B8 empirical-alpha flag-rate counters (network.trace_flag_rates) -----
    // Pure observation: {opportunities, flags} per QC tier, summarized at end of run
    // as realized rate vs the implied one-sided Gaussian α for the configured k.  Never
    // fed back into any decision; every increment is guarded so OFF is byte-identical.
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    auto& fr_if_slip = ctx.counters.fr_if_slip;
    auto& fr_we_trigger = ctx.counters.fr_we_trigger;
    auto& fr_raim_cell = ctx.counters.fr_raim_cell;
    auto& fr_raim_sat = ctx.counters.fr_raim_sat;
    auto& fr_attrib = ctx.counters.fr_attrib;
    auto& fr_clk_model = ctx.counters.fr_clk_model;
    auto& fr_clk_mon = ctx.counters.fr_clk_mon;
    auto& max_dur_anchor_s = ctx.counters.max_dur_anchor_s;
    auto& max_dur_anchored = ctx.counters.max_dur_anchored;
    auto& spectrum_diag_budget = ctx.counters.spectrum_diag_budget;

    // Single source of truth for "is (sat,signal) at station i a gauge-pinned
    // ambiguity this epoch?" — the predicate that replaces the scattered is_pivot
    // gauge tests.  Legacy (network.phase_datum_prioritized OFF): membership ==
    // station is THE pivot, which pins the whole shared_phase_keys set, so this
    // returns exactly is_pivot and every rewritten site is byte-identical.
    // Prioritized: membership == this "sat_signal" key is in station i's elected
    // pinned_amb_keys (the anchor for that gauge freedom, its pin folded into its
    // OWN local block).  Key form matches the pin fold (parts->satellite + "_" +
    // parts->signal) and the ambiguity-state key.
    // Fix A (network.phase_datum_ensemble): the anchor-free sum-to-zero phase-bias
    // datum REUSES all the prioritized SPOF-cure machinery (no pivot-first, every
    // station carries its arcs, per-(sat,signal) shared set) — it differs only in
    // the gauge REALIZATION (a kernel sum-to-zero fold instead of a per-(sat,signal)
    // anchor pin) — so drive the prioritized code paths under EITHER mode and
    // diverge only at the anchor election (skipped under ensemble) and the pin fold
    // (naturally inert: no station gets a pinned_amb_key under ensemble).
    const bool phase_datum_ensemble    = ctx.knobs.phase_datum_ensemble;
    // Card 3: the Eq-34 / de Jonge single-receiver-chain datum.  Like the ensemble
    // (Fix A), it drives ALL the prioritized SPOF-cure machinery (no pivot-first,
    // every station carries its arcs, union shared set) and diverges only at the
    // election (a spanning-tree chain election replacing the anchor election) and
    // the pin fold (per-tree-edge ambiguity pins with a target + a datum-receiver
    // RPB pin).  Mutually exclusive with the ensemble (enforced at config load).
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;
    // Card 4 (M1) C3b: the DD cycle-basis invariant carry gate (declared early so the
    // superseded eq34 hygiene knobs below can gate on it).  Requires phase_datum_eq34.
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    // Card 3: information weight (1/σ²) of the per-epoch zero-mean RPB soft prior
    // (the station-local floor for the near-null RPB split); 0 = disabled.
    const double eq34_rpb_prior_sigma = ctx.knobs.eq34_rpb_prior_sigma;
    const double eq34_rpb_prior_w = ctx.knobs.eq34_rpb_prior_w;
    // Card 3: q (m²/s) of the local RPB time-linking carry; 0 = v1 fresh re-estimation.
    // Card 4 supersedes the local δ_{r,σ} time-link (the DD carry is the phase time-link);
    // under carry_invariant the RPB re-estimates fresh (δ cancels in the gauge-free DD).
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    // Card 3 EXPERIMENTAL: S-transform carries on pin displacement (OFF = erase-based hygiene).
    // Card 4 supersedes the carry S-transform hygiene (the datum is re-realized freely).
    const bool eq34_shift_on = ctx.knobs.eq34_shift_on;
    // Card 3 (I4): grow NEW spanning-tree edges by arc longevity (OFF = legacy lexical growth).
    const bool eq34_longevity = ctx.knobs.eq34_longevity;
    // Card 3 (I2): retain sticky-tree identity across carry-alive row gaps (OFF = wholesale replace).
    const bool eq34_extend_only = ctx.knobs.eq34_extend_only;
    // Card 4 (M1) C3b: the DD cycle-basis invariant carry driver + its persistent state
    // (the carried z-store lives across epochs).  SUPERSEDES the S-dependent PhaseBias/
    // local/rpb carries + eq34 pin-round/shift hygiene for phase keys (bypassed below);
    // GLONASS (FDMA) excluded.  OFF = no z-store, all folds gated (byte-identical).
    NetworkInvariantCarryDriver::Config carry_cfg;
    carry_cfg.qz = config_.networkCarryInvariantQz();
    // Card 4 (M1) D5: the estimation-side fine-slip innovation gate (spec D5 Def-1) — the
    // Card-4-native replacement for the RAIM fine-slip layer the invariant bypass empties.
    // Requires carry_invariant (config-rejected otherwise); OFF = byte-identical.
    const bool carry_innov_gate = carry_invariant && config_.networkCarryInvariantInnovGate();
    carry_cfg.innov_gate = carry_innov_gate;
    carry_cfg.innov_k    = config_.networkCarryInvariantInnovK();
    // D5 v2: LS-VCE calibration of the gate threshold (σ̂₀²); requires innov_gate (config-rejected).
    carry_cfg.innov_calibrate        = carry_innov_gate && config_.networkCarryInvariantInnovCalibrate();
    carry_cfg.innov_calibrate_window = config_.networkCarryInvariantInnovCalibrateWindow();
    // M5 card WL-3 (docs/SPEC_lane_carry_card.md): the lane-coordinate carry.  The loader
    // enforces carry_invariant + mw_wl.ewl_enable.  lane_joint discriminates the LANE
    // shape (joint 2K pair blocks, lane prior/q, lane measurement rows, fold-on-change
    // fixing) from the §6.1 EQUIVALENCE shape (all lane features at their isotropic/off
    // sentinels ⇒ split per-band emission, information-identical to the per-band carry).
    const auto& lane_cfg = config_.getNetworkConfig().lane_carry;
    const bool lane_on = carry_invariant && lane_cfg.enable;
    const bool lane_joint = lane_on
        && (lane_cfg.q_wl_scale != 1.0 || lane_cfg.sigma_wl_fresh_cyc > 0.0
            || lane_cfg.lane_meas || lane_cfg.bridge_meas);
    if (lane_on) {
        carry_cfg.q_wl_scale         = lane_cfg.q_wl_scale;
        carry_cfg.sigma_wl_fresh_cyc = lane_cfg.sigma_wl_fresh_cyc;
        carry_cfg.lane_joint         = lane_joint;
        // LF-1 INC-1: the optimized fold shape.  Inert unless lane_joint (the only shape with
        // lane rows); the driver primitives + finalizeLaneBatch key off carry_cfg.fold_blocked.
        carry_cfg.fold_blocked       = lane_joint && lane_cfg.fold_blocked;
        // LF-1 INC-2: pinned-cycle increment skip (also inert unless lane_joint).
        carry_cfg.skip_pinned_increments = lane_joint && lane_cfg.skip_pinned_increments;
        carry_cfg.skip_pinned_within_n   = lane_cfg.skip_pinned_within_n;
    }
    NetworkInvariantCarryDriver carry_driver(carry_cfg);
    // M5 card WL-1: the MW-seeded wide-lane pseudo-observation layer (docs/SPEC_mw_wl_card.md).
    // Rides the carry driver's chord basis, so it requires carry_invariant (config-rejected
    // otherwise).  OFF (mw_wl.enable=false) ⇒ NetworkMwWlLayer::build returns no blocks and the
    // per-station MW detectors are not built for it, so the producer is byte-identical.
    const bool mw_wl_on = carry_invariant && config_.networkMwWlSeed();
    // M5 card WL-2 (docs/SPEC_ewl_wl_cascade_card.md §3.1): when the EWL lane is armed the
    // constellation's THIRD band (the EWL pair's uncarried leg — GPS L5, Galileo E5b) is
    // deliberately EXCLUDED from the invariant-carry driver so that lane acquires no unearned
    // stiff prior (the F6 wall) and its EWL rows win cleanly.  The band stays in the eq34
    // election/datum (shared_phase_keys/pins are built upstream, untouched).  OFF ⇒ the third
    // band is carried exactly as today (byte-identical).  v1 = GPS+Galileo (GLONASS never carried).
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    const bool ewl_carry_exclude = carry_invariant && nmw.ewl_enable;
    // M5 card UWL-1 (docs/SPEC_uwl_card.md §3.1): under the armed card the uncarried band
    // comes from the per-constellation lane-tier table (Galileo's top band is selectable:
    // '7' = E5b λ 9.768 m, '8' = E5 AltBOC λ 19.537 m).  uwl_enable=false keeps the legacy
    // digits verbatim — the §6.1(b) gate: arming a constellation without the card changes
    // NOTHING.  For GPS (and Galileo at the default '7') the table reproduces the legacy
    // digits, so armed GPS-only runs stay bit-identical too.
    const bool uwl_excl_on = ewl_carry_exclude && nmw.uwl_enable;
    const char uwl_gal_top = nmw.uwl_gal_top_band;
    const bool uwl_dual    = nmw.uwl_dual_top;   // increment 5: BOTH {b_top, b_top2} uncarried
    auto isUncarriedThirdBand = [uwl_excl_on, uwl_gal_top, uwl_dual](
                                    char cons, const std::string& signal) -> bool {
        const char band = signal.size() >= 2 ? signal[1] : '\0';   // "L5Q" → '5'
        if (uwl_excl_on) {
            const auto tier = mwLaneTierOf(cons, uwl_gal_top, false, uwl_dual);
            return tier && (band == tier->b_top
                            || (tier->b_top2 != '\0' && band == tier->b_top2));
        }
        switch (cons) {
            case 'G': return band == '5';   // GPS  EWL = (L2,L5) ⇒ L5 uncarried
            case 'E': return band == '7';   // Galileo EWL = (E5b,E5a) ⇒ E5b(7) uncarried
            default:  return false;         // BDS/QZSS deferred; GLONASS excluded from the carry anyway
        }
    };
    NetworkMwWlLayer::Config mw_wl_cfg;
    mw_wl_cfg.enable             = mw_wl_on;
    mw_wl_cfg.min_arc_epochs     = config_.getNetworkConfig().mw_wl.min_arc_epochs;
    mw_wl_cfg.sigma_floor_cyc    = config_.getNetworkConfig().mw_wl.sigma_floor_cyc;
    mw_wl_cfg.max_rows_per_epoch = config_.getNetworkConfig().mw_wl.max_rows_per_epoch;
    mw_wl_cfg.debug_r_scale      = config_.getNetworkConfig().mw_wl.debug_r_scale;   // F6 probe (diagnostic)
    NetworkMwWlLayer mw_wl_layer(mw_wl_cfg);
    if (mw_wl_on && mw_wl_cfg.debug_r_scale != 1.0)
        log->warn("Network (M5/WL-1): network.mw_wl.debug_r_scale = {} — DIAGNOSTIC probe, the WL "
                  "rows' Pz is scaled (not honest MW variance).  Do NOT grade this run.",
                  mw_wl_cfg.debug_r_scale);
    // M5 card WL-2 EWL lane (SPEC_ewl_wl_cascade_card.md §3.2): the SAME layer, run on the
    // (band2, band5) pair with FFR statistic-gated fixing.  The third band is uncarried
    // (excluded above) so these rows win with no F6 fight; a passed FFR folds an integer-hard
    // row.  Rides the primary-sig1 cycle basis exactly as WL-1.
    const bool ewl_on = carry_invariant && nmw.ewl_enable;
    // M5 card UWL-1: the per-constellation top tier.  uwl_on generalizes the EWL lane's
    // hardcoded GPS band digits to the lane-tier table (Galileo UWL (E5b|E5,E5a)); the
    // cold fix additionally admits n=1 UNLAGGED statistics to the DECISION on tiers with
    // λ ≥ uwl_cold_min_lambda_m (SPEC §3.3 — GPS's 5.861 m EWL deliberately excluded).
    const bool uwl_on      = ewl_on && nmw.uwl_enable;
    const bool uwl_cold_on = uwl_on && nmw.uwl_cold_fix;
    // 60-ep age-bias fix: the bridge accumulates PER-SAMPLE iono-de-biased GF, so
    // every read-time C_iono·Σc·ı̃ subtraction is DISABLED (the frames must never
    // mix — subtracting twice re-creates the bias with the opposite sign).
    const bool bridge_debias_on = ewl_on && nmw.bridge_sample_debias;
    // E6 mid-ladder bridge hop (census-motivated): chain the WL decision through the
    // Galileo (E6,E5a) λ 2.931 m rung — re-rounding at λ_MID strips the top hop's
    // λ-scale phase/iono amplification before it reaches the 0.751 m WL statistic.
    const bool uwl_mid_on  = uwl_on && nmw.uwl_mid_enable;
    // Increment 5: per-cycle preferred('8')/fallback('7') Galileo top rung + the
    // untaken-sibling closure statistic (2·N(8,5) = N(7,5)); hard gate default OFF
    // (measure the residual distribution first — SPEC reconciliation note 2).
    const bool uwl_dual_on = uwl_on && nmw.uwl_dual_top;
    const bool uwl_sib_gate_on = uwl_dual_on && nmw.uwl_sib_gate;
    // v2 census recovery: minority receiver-family primary pairs get their OWN lane
    // pair groups (admissible census-pair SET instead of the single majority pair).
    const bool uwl_family_on = uwl_on && lane_on && nmw.uwl_family_pairs;
    NetworkMwWlLayer::Config ewl_cfg;
    ewl_cfg.enable              = ewl_on;
    ewl_cfg.min_arc_epochs      = nmw.ewl_min_arc_epochs;
    ewl_cfg.sigma_floor_cyc     = nmw.sigma_floor_cyc;
    ewl_cfg.max_rows_per_epoch  = nmw.max_rows_per_epoch;
    ewl_cfg.ffr_fix             = true;
    ewl_cfg.fix_sigma_cyc       = nmw.ewl_fix_sigma_cyc;
    ewl_cfg.fix_frac_cyc        = nmw.ewl_fix_frac_cyc;
    ewl_cfg.fix_weight_sigma_cyc= nmw.fix_weight_sigma_cyc;
    ewl_cfg.max_abs_xlin_m      = nmw.lane_max_abs_xlin_m;
    // UWL-1: one statistic-layer instance per TABLED constellation when the card is armed
    // (per-cons telemetry — the F-A dump taught us pooled lines hide which lane is
    // storming); a single filterless instance otherwise (the legacy shape — for a
    // GPS-only run the 'G'-filtered instance is bit-identical to it, the filter never
    // fires).  An armed constellation that decides 0 cycles prints a loud 0-line
    // (invariant #4), never silence.
    struct EwlLane {
        char cons;                        ///< cons filter (0 = all — the legacy single lane).
        char top;                         ///< UWL-1: the instance's top-tier band digit ('\0' = legacy single lane).
        NetworkMwWlLayer layer;
        NetworkMwWlLayer::Counters run, epoch;
    };
    std::vector<EwlLane> ewl_lanes;
    if (ewl_on) {
        if (uwl_on) {
            for (const char cons : { 'G', 'E' }) {
                const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                               nmw.uwl_mid_enable, nmw.uwl_dual_top);
                if (!tier) continue;
                NetworkMwWlLayer::Config cfg = ewl_cfg;
                cfg.cons_filter = cons;
                // Increment 5: the PREFERRED rung's instance runs FIRST (its ewl_fixed
                // entries precede the fallback's, so the walk's dedupe keeps the wider-λ
                // decision); the fallback instance decides only what the preferred one
                // did not (a cycle with any preferred-band-less arc dies there as
                // skip_missing_band — the per-cycle "where tracked" resolution).
                ewl_lanes.push_back(EwlLane{ cons, tier->b_top, NetworkMwWlLayer(cfg), {}, {} });
                if (tier->b_top2 != '\0')
                    ewl_lanes.push_back(EwlLane{ cons, tier->b_top2,
                                                 NetworkMwWlLayer(cfg), {}, {} });
            }
        } else {
            ewl_lanes.push_back(EwlLane{ '\0', '\0', NetworkMwWlLayer(ewl_cfg), {}, {} });
        }
    }
    // M5 card WL-2 WL lane (SPEC §3.3/§3.4): the phase-bridge WL decision + the NL-preserving
    // inject.  The bridge path needs the EWL integers, so it runs only when BOTH masters are on;
    // wl_inject alone runs the MW(1,2) fallback path (§3.5, deferred to a later increment — v1
    // requires the bridge, else no WL decision is produced and the lane is inert but armed).
    // WL-3 supersedes the WL-2 hold/inject walk at runtime (loader-warned): under
    // lane_carry the decided integers become fold-on-change LANE fixes on the pair
    // blocks and the inject primitive survives only as the pair-block fallback.
    const bool wl_inject_on = ewl_on && nmw.wl_inject_enable && !lane_on;
    // WL-3 per-epoch lane measurement sources (§3.2); the FFR fix walk runs whenever
    // the joint lane shape is armed (fold-on-change is the §3.3 core, no separate knob).
    const bool lane_meas_on   = lane_on && lane_joint && lane_cfg.lane_meas;
    const bool lane_bridge_on = lane_on && lane_joint && lane_cfg.bridge_meas;
    const bool lane_fix_on    = lane_on && lane_joint;
    // LF-1 INC-4: fan the MW lane-increment folds across components (byte-identical to serial).
    const bool lane_parallel_components = lane_on && lane_joint && lane_cfg.parallel_components;
    // One decided WL inject: the two bands' carry groups + the shared chord ArcKey + the integer
    // target + the per-band linearization DD sums (F5 correction frame) + wavelengths.  Computed
    // in buildCarryDdPriors from DETECTOR statistics (§3.0.1 decision source), APPLIED post-solve
    // against the fresh posterior (so the preserved NL sum is this epoch's).  Refreshed each build
    // (the last before the accepted solve wins, mirroring ewl_epoch).
    struct WlInjectPlan {
        int    group_l1{-1}, group_l2{-1};
        ArcKey chord{0, 0};
        double w{0.0};                 ///< integer WL DD target ∇ΔN_WL (cyc).
        double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
        double lambda1{0.0}, lambda2{0.0};
        bool   pair{false};            ///< WL-3: group_l1 (== group_l2) is a PAIR group — read/edit via the pair accessors (the fallback path; ~0 in steady state).
        char   cons{'\0'};             ///< UWL-1: the chord's constellation (per-cons inject telemetry).
    };
    std::vector<WlInjectPlan> wl_inject_epoch;   // per-build; consumed post-installPosterior.
    // F-A attribution dump (network.mw_wl.d5_dump, log-only): adjacency tags for each D5
    // flag — the (station, sat) pairs an EWL-FIXED cycle touched this build, the chords of
    // hold-folded cycles this build, and the chords injected LAST epoch (an inject's
    // residual tension lands in the NEXT install; the injected chord itself is exempt, its
    // component siblings are not).  All rebuilt per accepted build; empty unless d5_dump.
    const bool mw_d5_dump = (ewl_on || wl_inject_on || lane_on) && nmw.d5_dump;
    // F-B lane trace (network.mw_wl.lane_trace, log-only): per epoch, the carried WL lane
    // value of EVERY primary cycle whose chord is carried in both bands — the constrained
    // (held/injected) and unconstrained populations in one run, so the injected-cycle circR
    // is finally gradable (scripts/grade_lane_trace.py).  Entries built per accepted build;
    // written post-install against the FRESH posterior.
    const bool mw_lane_trace = (wl_inject_on || lane_fix_on) && nmw.lane_trace;
    const bool mw_tagsets    = mw_d5_dump || mw_lane_trace;   // hold/inject tag sets feed both
    struct LaneTraceEntry {
        ArcKey chord{0, 0};
        int    g1{-1}, g2{-1};          ///< the two bands' carry group ids.
        double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
        double lam1{0.0}, lam2{0.0};
        std::string sat;                ///< chord satellite (chord.u = station index).
    };
    std::vector<LaneTraceEntry> lane_trace_epoch;   // per-build; consumed post-install.
    auto& lane_trace_header_written = ctx.carry.lane_trace_header_written;
    auto& d5_ewl_stasat = ctx.carry.d5_ewl_stasat;
    auto& d5_hold_chords = ctx.carry.d5_hold_chords;
    auto& d5_inject_chords_prev = ctx.carry.d5_inject_chords_prev;
    auto& d5_inject_chords_now = ctx.carry.d5_inject_chords_now;
    auto& d5_dump_header_written = ctx.carry.d5_dump_header_written;
    struct WlCounters {
        long long rows_seen{0};          ///< FFR-fixed EWL rows inspected by the bridge.
        long long cycles_decided{0};     ///< bridge cycles that passed the WL FFR gate.
        long long holds_folded{0};       ///< hold rows folded (carry already lane-consistent).
        long long injects{0};            ///< injects applied post-solve (carry moved to the integer).
        double    inject_abs_sum{0.0};   ///< Σ |w − carried lane| at inject (median-ish |Δ| via /injects).
        long long skip_bridge_missing{0};///< an arc lacked a mature GF_b bridge arc.
        long long skip_no_iono{0};       ///< §3.4 v2: an arc lacked a slant-iono estimate ⇒ can't de-bias.
        long long skip_bridge_wide{0};   ///< bridge σ_row (incl. iono variance) > bridge_max_sigma_cyc.
        long long skip_not_fixed{0};     ///< bridge FFR failed (σ or |frac|).
        long long skip_closure{0};       ///< MW(1,2) cross-check disagreed with the bridge integer.
        long long skip_quarantine{0};    ///< an arc's |x_lin| exceeded lane_max_abs_xlin_m.
        long long skip_basis{0};         ///< inject: the chord is not 1:1 across BOTH bands' carry groups.
        double    sum_bridge_R{0.0};     ///< Σ decided rows' bridge R (cyc²) for the mean-R report.
        // --- WL-3 lane-carry telemetry (SPEC_lane_carry_card.md §3.2/§3.3). ---
        long long lane_mw_rows{0};       ///< per-epoch MW(1,2) lane increments folded into pair blocks.
        long long lane_bridge_rows{0};   ///< per-epoch bridge lane increments folded (EWL-fixed cycles).
        long long lane_fix_folded{0};    ///< fold-on-change FFR fixes FOLDED (decision new/changed).
        long long lane_fix_unchanged{0}; ///< FFR decisions seen with an unchanged integer (no fold — the F-A once-only discipline).
        long long lane_skip_meas{0};     ///< lane-increment cycles skipped (immature/stale/mixed-frame/slipped arc/quarantine).
        long long lane_pinned_skips{0};  ///< LF-1 INC-2: MW/bridge increments skipped because the chord's lane is committed-pinned (laneIncrementSkippable).  0 ⇒ the lever is inert (invariant #4).
        // --- UWL-1 cold-fix telemetry (SPEC_uwl_card.md §3.3). ---
        long long cold_bridge_cycles{0}; ///< decided cycles whose bridge read was the UNLAGGED cold serve (post-slip early re-pin).
        // --- E6 mid-ladder hop telemetry (uwl_mid_enable). ---
        long long mid_decided{0};        ///< decided cycles whose WL statistic came through the two-hop mid chain.
        long long mid_fallback{0};       ///< cycles with a live mid chain whose mid FFR failed ⇒ fell back to the direct hop.
        long long mid_closure{0};        ///< mid integers blocked by the MW(6,5) closure ⇒ fell back to the direct hop.
        long long mid_disagree{0};       ///< two-hop and direct integers BOTH FFR-credible yet different ⇒ cycle skipped (wrong-integer backstop).
        // --- Increment 5: dual-top tier telemetry (uwl_dual_top). ---
        long long top2_decided{0};       ///< decided cycles that came through the FALLBACK rung ('7' — the preferred '8' could not decide them).
        long long dual_dedupe{0};        ///< fallback-rung entries skipped because the preferred rung already decided the (block,row) this build.
        long long sib_checked{0};        ///< decided cycles whose untaken-sibling MW statistic was measurable across every arc.
        long long sib_unavail{0};        ///< decided cycles whose sibling statistic was NOT measurable (an arc lacks the sibling band / MW arc).
        long long sib_int_off{0};        ///< checked cycles whose sibling residual rounds to a NONZERO integer (wrong-integer signal — or a convention offset; measure first).
        long long sib_blocked{0};        ///< decisions blocked by the armed sibling gate (uwl_sib_gate; the cycle falls to the fallback rung the same epoch).
        double    sib_abs_frac_sum{0.0}; ///< Σ |frac(sibling residual)| over checked cycles (mean = the convention-offset dial: ~0 clean, ~0.5 half-cycle convention).
    };
    WlCounters wl_run, wl_epoch;
    // UWL-1: per-constellation WL-walk tallies when the card is armed (pooled lines hide
    // which lane is storming).  wl_run stays the aggregate — the per-epoch cons tallies
    // fold into BOTH at the accepted-install accumulation.
    std::map<char, WlCounters> wl_cons_epoch, wl_cons_run;
    // Accepted-install accumulation helper (one place — the field list bit-rots in two).
    auto wlAccumulate = [](WlCounters& a, const WlCounters& b) {
        a.rows_seen           += b.rows_seen;
        a.cycles_decided      += b.cycles_decided;
        a.holds_folded        += b.holds_folded;
        a.injects             += b.injects;
        a.inject_abs_sum      += b.inject_abs_sum;
        a.skip_bridge_missing += b.skip_bridge_missing;
        a.skip_no_iono        += b.skip_no_iono;
        a.skip_bridge_wide    += b.skip_bridge_wide;
        a.skip_not_fixed      += b.skip_not_fixed;
        a.skip_closure        += b.skip_closure;
        a.skip_quarantine     += b.skip_quarantine;
        a.skip_basis          += b.skip_basis;
        a.sum_bridge_R        += b.sum_bridge_R;
        a.lane_mw_rows        += b.lane_mw_rows;
        a.lane_bridge_rows    += b.lane_bridge_rows;
        a.lane_fix_folded     += b.lane_fix_folded;
        a.lane_fix_unchanged  += b.lane_fix_unchanged;
        a.lane_skip_meas      += b.lane_skip_meas;
        a.lane_pinned_skips   += b.lane_pinned_skips;
        a.cold_bridge_cycles  += b.cold_bridge_cycles;
        a.mid_decided         += b.mid_decided;
        a.mid_fallback        += b.mid_fallback;
        a.mid_closure         += b.mid_closure;
        a.mid_disagree        += b.mid_disagree;
        a.top2_decided        += b.top2_decided;
        a.dual_dedupe         += b.dual_dedupe;
        a.sib_checked         += b.sib_checked;
        a.sib_unavail         += b.sib_unavail;
        a.sib_int_off         += b.sib_int_off;
        a.sib_blocked         += b.sib_blocked;
        a.sib_abs_frac_sum    += b.sib_abs_frac_sum;
    };
    auto& lane_pair_arcs = ctx.carry.lane_pair_arcs;
    auto& lane_resid_arcs = ctx.carry.lane_resid_arcs;
    auto& lane_pair_tree_mismatch = ctx.carry.lane_pair_tree_mismatch;
    // FDMA-correct per-signal wavelengths for the WL/EWL-row coefficients and z.  Built when
    // EITHER lane is armed (mirrors ar_ppp_utils).
    std::unique_ptr<PppUtils> mw_wl_ppp_utils;
    if (mw_wl_on || ewl_on || lane_on) mw_wl_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    NetworkMwWlLayer::Counters mw_wl_run;    // run-total eligibility/skip tallies (invariant #4)
    NetworkMwWlLayer::Counters mw_wl_epoch;  // per-build tallies (the last build before the accepted solve wins)
    // M5 card WL-1 F6 diagnostic: one-shot per-arc/per-row attribution dump of the FIRST
    // arming epoch's WL rows (docs/HANDOFF_mw_wl_card.md §6c).  Populated in
    // buildCarryDdPriors (build-time: z_MW/z_lin/z_corr + per-arc MW̄/x_lin, where the
    // resolver + revmap are in scope) and finalized post-solve (reconstructed_abs from
    // reconstructStation + the kernel's WL-block innovation).  Log-only; default OFF.
    const bool mw_wl_f6_dump = mw_wl_on && config_.getNetworkConfig().mw_wl.f6_dump;
    struct F6Arc {
        int         est_station{-1};
        int         col1{-1}, col2{-1};
        std::string sat, sig1, sig2;
        double      coeff{0.0};       // signed cycle coefficient c (±1 for a plain DD)
        double      mw_mean_m{0.0};   // ZD-consistent arc MW mean (m)
        double      mw_var_m2{0.0};
        long long   mw_count{0};
        bool        bia_corrected{false};
        double      x_lin1{0.0}, x_lin2{0.0};              // amb linearization values (m)
        double      lambda1{0.0}, lambda2{0.0}, lambda_wl{0.0};
    };
    struct F6Row {
        std::vector<F6Arc> arcs;
        double z_mw{0.0}, z_lin{0.0}, z_corr{0.0}, zhat_block{0.0};
    };
    struct F6Capture {
        bool                 pending{false};   // captured this epoch, awaiting the post-solve write
        bool                 written{false};   // one-shot latch (whole run)
        double               week{0.0}, tow{0.0};
        int                  loop_epoch{-1};
        std::size_t          wl_block_index{0};
        std::vector<F6Row>   rows;
    } mw_wl_f6;
    // Card 4 (M1) phase D: the emit-layer frame re-base (docs/HANDOFF_card4_phaseD.md).
    // Re-references the emitted per-(sat,σ) phase biases onto the previous EMISSION
    // frame so the freely-re-realized eq34 datum's per-epoch gauge churn does not
    // storm the consumer with spurious ambiguity resets; a genuine reference-path
    // slip's integer step is left intact so the emitter jump test still fires.
    // Default-ON when carry_invariant is armed (spec D7); the OFF path is
    // carry_invariant=false (byte-identical — the engine is never touched).
    const bool emit_rebase_on = carry_invariant && config_.networkCarryInvariantEmitRebase();
    NetworkEmitRebase emit_rebase_engine;
    // M5 prerequisite: PB emission-continuity filter — a slip-gated per-key scalar
    // KF on the RE-BASED emitted biases + a self-tuning k·σ̂_step jump gate that
    // clears the threshold-in-noise discontinuity floor.  Runs INSIDE the re-base
    // block (it filters the re-based emission frame), so it requires emit_rebase_on;
    // OFF (default) ⇒ filterEpoch is never called (byte-identical — the engine is
    // constructed like emit_rebase_engine but stays inert).
    const bool emit_pb_filter_on = emit_rebase_on && config_.networkEmitPbFilter();
    NetworkPbEmitFilter::Config pb_filter_cfg;
    pb_filter_cfg.q_m2_s         = config_.networkEmitPbFilterQ();
    pb_filter_cfg.jump_k         = config_.networkEmitPbFilterJumpK();
    pb_filter_cfg.meas_sigma_m   = config_.networkEmitPbFilterMeasSigmaM();
    pb_filter_cfg.max_gap_s      = config_.networkEmitPbFilterMaxGapS();
    pb_filter_cfg.window_epochs  = config_.networkEmitPbFilterWindowEpochs();
    pb_filter_cfg.gate_v2        = config_.networkEmitPbFilterGateV2();  // PB-gate-v2 merge point (leaf wiring)
    NetworkPbEmitFilter pb_filter_engine(pb_filter_cfg);
    const bool pb_filter_emit_sigma = config_.networkEmitPbFilterEmitSigma();
    auto& carry_sat_vertex = ctx.carry.carry_sat_vertex;
    auto& carry_group_id = ctx.carry.carry_group_id;
    auto& carry_glonass_warned = ctx.carry.carry_glonass_warned;
    auto& uwl_warned_cons = ctx.carry.uwl_warned_cons;
    auto& lane_census_logged = ctx.carry.lane_census_logged;
    auto& lane_admit_pairs = ctx.carry.lane_admit_pairs;
    auto& lane_family_logged = ctx.carry.lane_family_logged;
    auto& carry_prev_tow = ctx.carry.carry_prev_tow;
    auto& carry_epochs = ctx.carry.carry_epochs;
    auto& carry_born_total = ctx.carry.carry_born_total;
    auto& carry_marg_total = ctx.carry.carry_marg_total;
    auto& carry_merges_total = ctx.carry.carry_merges_total;
    auto& carry_splits_total = ctx.carry.carry_splits_total;
    auto& carry_swaps_total = ctx.carry.carry_swaps_total;
    auto& carry_zdim_last = ctx.carry.carry_zdim_last;
    auto& carry_slipkills_total = ctx.carry.carry_slipkills_total;
    auto& carry_dropped_total = ctx.carry.carry_dropped_total;
    auto& carry_innov_pending = ctx.carry.carry_innov_pending;
    auto& carry_innov_flags_total = ctx.carry.carry_innov_flags_total;
    auto& carry_innov_retaps_total = ctx.carry.carry_innov_retaps_total;
    auto& carry_innov_map_miss = ctx.carry.carry_innov_map_miss;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // Fix A seed-once + time-lock (network.phase_datum_hold_sigma_m).  The un-held
    // ensemble references the emitted bias to mean_r(ẑ_r) — a per-epoch-re-realized
    // float-ambiguity mean whose absolute LEVEL is code/iono-precision-limited — so
    // the product churns ~metre/epoch (a dynamic datum on a static product).  When
    // >0 the gauge is SEEDED once per (sat,signal) and thereafter the shared bias is
    // HELD by a stiff temporal prior of this sigma: the Σz=0 fold is skipped for a
    // (sat,signal) that already has a carried bias, and the D2 stash carries that
    // bias with a fixed (clean) hold-variance instead of dropping it.  0 = re-realize
    // every epoch (the churny baseline); inert unless phase_datum_ensemble.
    const double phase_hold_sigma = ctx.knobs.phase_hold_sigma;
    const bool   phase_hold_on    = ctx.knobs.phase_hold_on;
    const double phase_hold_var   = ctx.knobs.phase_hold_var;
    // Assembly helpers relocated to ApplicationNetworkAssembly.cpp (INC-7):
    // isGaugePinned / splitStation / appendGimIonoRows (+ appendShIonoRows
    // below) are now the Application member functions networkIsGaugePinned_ /
    // networkSplitStation_ / networkAppendGimIonoRows_ / networkAppendShIonoRows_
    // — former [&] captures are explicit parameters or member reads.
    auto& sh_dev_carry = ctx.carry.sh_dev_carry;
    const double sh_dev_tau = sh_model ? shc.rep_error_tau_s : 0.0;
    const bool   sh_dev_on  = sh_dev_tau > 0.0;

    // ---- Station-adaptive SH-VTEC-constraint deweight (iono deweight card) ----------
    // Per-station factor w_s ∈ (0,1] scaling the INFORMATION of this station's SH↔slant
    // iono pseudo-obs (σ² ×= 1/w_s ⇒ info ×= w_s).  w_s→0 decouples the station's per-LOS
    // iono from the global SH field in BOTH directions (the single joint pseudo-obs means
    // one w_s covers both) while the station still serves the shared
    // clocks/biases/datum/position.  All the gate math (v1 static list + v2 network-relative
    // IGG auto-trigger, EWMA + hysteresis + slew) lives in NetworkIonoDeweightGate; here we
    // just feed it per-station statistics in the serial post-solve harvest and READ
    // iono_deweight.weight() in the parallel assembly (const find, race-free — the
    // sh_dev_carry contract).  Empty static list + adaptive OFF ⇒ weight() is 1.0 for every
    // station ⇒ byte-identical to OFF (invariant #6).
    const IonoConstraintDeweightConfig& idw = config_.getIonoConstraintDeweightConfig();
    NetworkIonoDeweightGate iono_deweight(
        std::set<std::string>(idw.stations.begin(), idw.stations.end()), idw.factor,
        idw.adaptive, idw.window_epochs, idw.sigma_hi, idw.sigma_lo, idw.floor,
        idw.rate_limit, idw.min_epochs);
    const bool idw_on = iono_deweight.active();
    if (idw_on && !sh_model)
        log->warn("Network (iono deweight): configured but network.estimate_iono_sh is OFF — "
                  "there is no SH field to deweight; the setting is inert this run.");
    if (sh_model && iono_deweight.staticCount() > 0) {
        std::set<std::string> idw_present;
        for (const auto& st : stations) idw_present.insert(st.name);
        for (const auto& nm : iono_deweight.staticStations())
            if (!idw_present.count(nm))
                log->warn("Network (iono deweight): statically-listed station '{}' is not in this "
                          "network — ignored (check the name).", nm);
    }

    // appendShIonoRows relocated to ApplicationNetworkAssembly.cpp (INC-7):
    // Application::networkAppendShIonoRows_ — sh_model / iono_deweight / shc /
    // sh_dev state now passed explicitly at the PASS-B call site.

    // Pin Eigen to a single thread for the duration of the (parallelized) epoch
    // loop.  _OPENMP is defined project-wide so Eigen threads its own dense
    // kernels; nesting those inside our per-station OpenMP region would cause
    // thread oversubscription AND non-deterministic reduction order in the
    // per-station matmuls.  Single-threaded Eigen inside each station thread is
    // fully thread-safe (separate matrices) and deterministic; restored after the
    // loop.  Only touched when parallelism is active, so the serial path (and the
    // small-network case) is byte-identical to before.
    const int eigen_threads_saved = Eigen::nbThreads();
    if (parallel_on) Eigen::setNbThreads(1);

    // [TIMING] Wall-clock of just the per-epoch loop (excludes ~startup: product
    // cache load + per-station RINEX parse).  Isolates the per-epoch compute that
    // scales with station count — the scaling metric.  Counts only epochs that
    // actually entered processing (past the max-duration cutoff).
    const auto t_loop_begin = std::chrono::steady_clock::now();
    auto& n_loop_epochs = ctx.counters.n_loop_epochs;
    auto& decim_counter = ctx.counters.decim_counter;
    auto& sh_a00_was_in_band = ctx.carry.sh_a00_was_in_band;
    auto& sh_converged_latch = ctx.carry.sh_converged_latch;
    auto& sh_coeff_prev = ctx.carry.sh_coeff_prev;

    // The joint estimator is HOISTED out of the epoch loop and reset()/reused each
    // epoch.  Constructing a fresh NetworkEstimator per epoch reallocated one dense
    // Schur block PER STATION every epoch; the process heap retained that churn as
    // committed memory, saturating at ~0.5 GB/station (≈146 GB at 305 stations over
    // a 121-epoch run → exhausted RAM).  Reusing the estimator's station-block pool
    // (NetworkNormalEquations::reset) holds the footprint to one epoch's working set.
    NetworkEstimator est;
    // OPT-IN fan-out of the Schur reduction.  Kept SEPARATE from the build-phase
    // parallelism: the build's serial-ordered scatter is byte-identical to a serial
    // run, but the reduce fan-out reassociates the per-station summation, so it is
    // reproducible per thread count yet ULP-different from the serial reduce (the
    // network's ill-conditioning amplifies that to ~1e-5 in the products — physically
    // null, but it breaks serial≡parallel byte-identity).  Default OFF: the reduce
    // stays serial (byte-identical serial≡parallel) while the build still threads and
    // the deterministic serial-tail wins (eigensolver gate, no double-reduce, factor
    // reuse) still apply.  Enable network.parallel_reduce for max-speed runs.
    //
    // Guardrail (Fable lean review, Jul-14 eve): parallel_reduce OpenMP-fans reduce()'s
    // per-station Schur loop; parallel_carry OpenMP-fans the fold's per-station reduction
    // and est.solve()'s dense algebra (fold GEMMs + the K×K middle inverse) is itself
    // Eigen-threaded — so armed together they nest OpenMP × Eigen inside reduce() and
    // oversubscribe.  They are therefore MUTUALLY EXCLUSIVE, resolved at LOAD time
    // (loadNetworkSection_ forces parallel_carry OFF when both are set), so by here at most
    // one of nc.parallel_reduce / nc.parallel_carry is true.  The `!carry_par` below is thus
    // belt-and-suspenders: it can no longer be false while reduce is armed.
    const bool reduce_par   = parallel_on && nc.parallel_reduce && !carry_par;
    const int  reduce_threads = reduce_par ? n_threads : 1;
    est.setReduceThreads(reduce_threads);
    // Deterministic-parallel DD carry-fold fan-out (network.parallel_seam); 1 = serial.
    est.setSeamThreads(seam_threads);
    // network.parallel_carry: when armed, solve()'s foldReducedDdPriors runs SERIAL-over-
    // blocks with WITHIN-BLOCK per-station threading (Eigen stays PINNED, so the driver does
    // NOT un-pin it around solve()).  OFF ⇒ the fold keeps seam_threads (block-parallel
    // seam), byte-identical.
    est.setCarryParallelThreads(carry_par ? n_threads : 1);
    // Card DG2 (network.rank_gate_tol): resolve the null-space rank-gate tolerance
    // ONCE, here with the estimator's other knobs, so the gate call sites, the
    // estimability dump and the prioritized drop-and-heal all read the SAME value
    // off the estimator instead of each reaching for the compile constant.  The
    // default is NetworkEstimator::kNullSpaceTol, so an unset key sends the
    // identical double through the identical arithmetic (byte-identical).
    est.setRankGateTol(config_.networkRankGateTol());
    // network.parallel_carry: RAII un-pin of Eigen's internal threading around ONE
    // compute-bound-GEMM carry stage.  Used at exactly TWO sites — buildDdPriors (W·Pᶻ·Wᵀ)
    // and installPosterior (posterior-covariance read-back) — whose cost is big dense GEMMs
    // Eigen threads well.  The THIRD stage, the fold (foldReducedDdPriors), is deliberately
    // NOT wrapped: its cost is the per-station reduction (small inner-dim matmuls + a serial
    // LDLT), which Eigen can't thread — it self-fans with its OWN OpenMP and Eigen PINNED,
    // so wrapping solve() here would nest that OpenMP with Eigen's GEMM threads.  The epoch
    // loop pins Eigen to one thread (line ~3086, to avoid nesting inside the per-station
    // OpenMP assembly region); these two stages run OUTSIDE that region, so un-pinning fans
    // their GEMMs across cores.  Restores the pin on EVERY exit path (an early `continue`, a
    // withheld-epoch skip, or a thrown call) so the next epoch's assembly / the fold's own
    // OpenMP is never left with Eigen un-pinned.  Inert unless @c on — so an OFF (default)
    // run never touches Eigen's thread count here (byte-id).
    struct CarryEigenScope {
        bool active;
        int  saved;
        CarryEigenScope(bool on, int nthreads) : active(on), saved(Eigen::nbThreads()) {
            if (active) Eigen::setNbThreads(nthreads);
        }
        ~CarryEigenScope() { if (active) Eigen::setNbThreads(saved); }
        CarryEigenScope(const CarryEigenScope&) = delete;
        CarryEigenScope& operator=(const CarryEigenScope&) = delete;
    };
    // Pin-aware station PD guard (network.station_pd_pin_aware): median-|pivot|
    // reference scale for the local-block rank test — the prioritized datum's
    // in-block 1e10 anchor pins otherwise eject marginal stations every epoch.
    est.setStationPdMedianScale(config_.networkStationPdPinAware());
    if (reduce_par)
        log->info("Network: Schur reduce PARALLEL across {} threads "
                  "(serial≡parallel byte-identity intentionally relaxed to ~1e-5)",
                  n_threads);

    // === Tier D0: per-epoch section timers (network.trace_section_timers) =====
    // Arm the estimator's wall-clock accumulators around assembly / reduce / solve
    // / the shared-covariance full inverse / the per-station covariance consumers,
    // so a profiling run can see which term dominates the per-epoch cost (⇒ which
    // Tier-D scaling lever is worth pulling).  The estimator accumulates CONTINUOUSLY
    // across the run; the driver snapshots it per epoch for the per-epoch delta line
    // and reads the run total at exit.  OFF (default) ⇒ no clock reads, byte-identical.
    const bool sec_timers_on = nc.trace_section_timers;
    est.setPerfTiming(sec_timers_on);
    est.resetSectionTimers();
    // INC-C0: arm the carry driver's per-op timers on the SAME toggle — splits the
    // SB_DDBUILD / SB_INSTALL wall brackets into basis-projection vs posterior read-back
    // vs D5-gate (the dense-K² ops the perf-restructure card ranks).  Byte-identical off.
    carry_driver.setPerfTiming(sec_timers_on);
    carry_driver.resetPerfTimers();
    double sec_asm_wall_ms = 0.0;      // D0: run-total driver-side assembly wall (produce + serial scatter)
    std::size_t sec_profiled_epochs = 0;  // D0: #epochs that reached the post-solve per-epoch readout
    if (sec_timers_on)
        log->info("Network (D0): per-epoch section timers ARMED "
                  "(assembly / reduce / solve / cov-inverse / cov-consumers).");

    // === Tier D0b: sub-instrument the "other" bucket (network.trace_section_timers) ==
    // "other" = loop wall − assembly − reduce − solve − cov-inverse − cov-consumers was
    // measured at ~81 % (42.9 s/ep) of the epoch on the 300 s GPS-only tier and scales
    // with LOS count (docs/HANDOFF_perlos_parallel_sweeps.md §1) — yet none of it was
    // timed individually.  These per-block driver-side wall timers attribute it.  For each
    // block, net = wall − Δ(reduce + solve + cov-inverse + cov-consumers) incurred INSIDE
    // it, so the recon/solve time already counted in the five D0 sections is NOT double-
    // counted: net is the block's TRUE contribution to "other" (a block that reconstructs
    // N stations shows its driver glue as net, its O(nₛ²) recon as cov-consumers).  The
    // brackets are disjoint and tile the loop body between the assembly wall and the
    // emitted-epoch readout; the residual (other − Σ net) is logged as "unattributed".
    // Inert (no clock reads, no est.sectionTimers() calls) when the toggle is OFF — the
    // subBegin/subEnd lambdas early-return, so byte-identity with an untimed run holds.
    enum SubBlock {
        SB_SLIP = 0,     // block 1: PASS-A slip scans (MW refloat + IF-TD jumps), pre-assembly
        SB_PREASM,       //          pre-assembly measurement build (post-slip → assembly wall)
        SB_GAUGEBUILD,   //          buildEnsembleGauges (Σz=0 gauges) — OFF (ensemble) in the tier
        SB_DDBUILD,      //          buildCarryDdPriors: eq34 arc graph + carry_driver.buildDdPriors
                         //          (W·Pᶻ·Wᵀ dense DD-cov projection over K carried chords) [Card-4]
        SB_SOLVEGLUE,    //          pre-solve prep + clock predict + solve + within-epoch re-solve
                         //          (net removes the internal solve/reduce)
        SB_INSTALL,      //          carry_driver.installPosterior (DD posterior mean/cov readback) [Card-4]
        SB_NETAR,        //          producer-side network AR Phase-2 (log-only; OFF in the tier)
        SB_SOLPREP,      //          toSolutionEpoch + per-sat discontinuity signalling
        SB_EMITSTORE,    // block 7: emit re-base + PB-emit filter + store push + solution CSV
        SB_IONODIAG,     // block 4: iono / ZWD / anchor-info viz traces (per-station recon sweeps)
        SB_DDFRAC,       // block 6: cross-station DD fractional-residual (B5) trace
        SB_SHDEV_VTEC,   // block 4: T5 SH-deviation carry KF + co-estimated VTEC coeff trace
        SB_T1A,          // block 3: T1a SH-field health probe + fused H1 adaptive-deweight gate
        SB_LEVELING,     // block 5: phase-leveling carry harvest + D2/C1 stash + T1d SH-conv
        SB_RAIM,         // block 2: post-fit network RAIM pass-1/2 + median-polish attribution
        SB_CLOCKUPD,     // block 8: two-state clock update + fault QC + retro-flag feed
        SB_STASH_STATUS, // block 5/8: D2/C1/SH carry re-stash + VCE fold + epoch-status QC
        SB_COUNT
    };
    static constexpr const char* kSubNames[SB_COUNT] = {
        "slip-scans", "pre-asm", "gauge-build", "dd-build", "solve-glue", "install-post",
        "netar", "sol-prep", "emit-store", "iono-diag", "dd-frac", "shdev-vtec",
        "t1a-health", "leveling", "raim", "clock-upd", "stash-status"
    };
    struct SubBlockTimer {
        double      wall_ms{0.0};    // driver wall bracketing the block (run cumulative)
        double      timed_ms{0.0};   // Δ(reduce+solve+cov-inv+cov-consumers) inside it (subtract for net)
        double      covcon_ms{0.0};  // of which cov-consumers (reported alongside; the D1 lever)
        std::size_t covcalls{0};     // cov-consumer calls inside it
    };
    std::vector<SubBlockTimer> sec_sub(SB_COUNT);        // run totals
    std::vector<SubBlockTimer> sec_sub_ep0(SB_COUNT);    // per-epoch loop-top snapshot of sec_sub
    std::vector<std::chrono::steady_clock::time_point> sub_wall0(SB_COUNT);
    std::vector<NetworkNormalEquations::SectionTimers>  sub_tmr0(SB_COUNT);
    auto subBegin = [&](SubBlock b) {
        if (!sec_timers_on) return;
        sub_wall0[b] = std::chrono::steady_clock::now();
        sub_tmr0[b]  = est.sectionTimers();
    };
    auto subEnd = [&](SubBlock b) {
        if (!sec_timers_on) return;
        const auto& t = est.sectionTimers();
        sec_sub[b].wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sub_wall0[b]).count();
        sec_sub[b].timed_ms +=
            (t.reduce_ms        - sub_tmr0[b].reduce_ms)
          + (t.solve_ms         - sub_tmr0[b].solve_ms)
          + (t.cov_inverse_ms   - sub_tmr0[b].cov_inverse_ms)
          + (t.cov_consumers_ms - sub_tmr0[b].cov_consumers_ms);
        sec_sub[b].covcon_ms += t.cov_consumers_ms   - sub_tmr0[b].cov_consumers_ms;
        sec_sub[b].covcalls  += t.cov_consumer_calls - sub_tmr0[b].cov_consumer_calls;
    };

    // === RTB-7(a): state snapshot + warm resume ==============================
    // Entirely OUTSIDE the product-file replay path: the snapshot restores the
    // producer's in-memory cross-epoch state; NetworkProductIO replay still
    // reproduces every discontinuity counter with no snapshot involved.  Path
    // empty (default) ⇒ nothing below runs — byte-identical.
    auto& snap_path = ctx.carry.snap_path;
    if (!nc.state_snapshot_path.empty()) {
        snap_path = nc.state_snapshot_path;
        if (snap_path.is_relative()) snap_path = log_dir_ / snap_path;
    }
    auto& last_snapshot_write_s = ctx.carry.last_snapshot_write_s;
    auto& live_fingerprint = ctx.carry.live_fingerprint;
    networkSnapshotFingerprint(ctx, config_, stations);
    networkTryWarmResume(ctx, config_, sync, emitter, log);

    // Card 5 (RTF-1 / plan D6): resolve the a-priori satellite-clock anchor σ ONCE
    // (loop-invariant, config-only) so the anchor fold and its B7 information-fraction
    // diagnostic below share one value.  The resolution policy (explicit > 0 wins;
    // brdc_rts auto → 3.0 m; sp3 auto → initial_std.sat_clock, unchanged) lives in
    // GnssConfig so it is a single source of truth and unit-testable in isolation.
    // The < 100 m binding gate at the fold site is unchanged and stays in the driver.
    const double clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM();

    // CB-ADM: run the W6 code-bias admission census ONCE, pre-loop, so the
    // registration site can gate on a run-latched (sat,signal) verdict instead
    // of a per-epoch >= 2-station count (see filter/NetworkBiasAdmission.hpp).
    // Armed only when both the master flag and the W6 slice are on; otherwise
    // ctx.admission.active stays false and the legacy per-epoch path is used.
    //
    // The census window is [i0, i0+K) of the synced vector, where i0 is the
    // FIRST epoch the loop will process.  The loop start is a LAZY time-anchored
    // fast-forward (network.start_offset_hours, below), not a precomputed index —
    // so replicate that condition here against the first synced epoch's time to
    // find i0 (start_offset <= 0 => i0 = 0).  A warm-resume that starts a
    // different span reclassifies over that span (per-process, not carried in the
    // snapshot — a known, field-inert property for post-processing).
    if (config_.networkCbAdmission() && config_.estimateNetworkSatCodeBias()
        && !synced.empty()) {
        std::size_t i0 = 0;
        if (start_offset_s > 0.0) {
            const double anchor_s =
                TimeUtils::continuousGpsSeconds(synced.front().time);
            while (i0 < synced.size()
                   && TimeUtils::continuousGpsSeconds(synced[i0].time) - anchor_s
                          < start_offset_s)
                ++i0;
        }
        ctx.admission = NetworkBiasAdmission::runCensus(
            synced, i0, config_.networkCbAdmissionCensusEpochs(),
            config_.networkCbAdmissionMinStations());
        const auto& adm = ctx.admission;
        ctx.counters.n_cb_admitted =
            static_cast<std::size_t>(adm.n_admitted);
        ctx.counters.n_cb_quarantined_census =
            static_cast<std::size_t>(adm.n_quarantined_census);
        if (adm.active) {
            struct ConsTally { int seen = 0, admitted = 0, lowcount = 0, noifpin = 0; };
            std::map<char, ConsTally> per_cons;
            for (const auto& p : adm.census) {
                ConsTally& t = per_cons[p.satellite.empty() ? '?' : p.satellite[0]];
                ++t.seen;
                if (p.verdict == NetworkBiasAdmission::Verdict::Admitted)      ++t.admitted;
                else if (p.verdict == NetworkBiasAdmission::Verdict::LowCount) ++t.lowcount;
                else                                                           ++t.noifpin;
            }
            log->info("Network (CB-ADM): census over {} epochs (K_eff) x {} stations "
                      "[from i0={} of {} synced] — effective_min={}, sustained_floor={}; "
                      "{} (sat,signal) pairs seen, {} admitted, {} quarantined.",
                      adm.census_epochs_used, adm.census_stations, i0, synced.size(),
                      adm.effective_min, adm.sustained_floor,
                      static_cast<int>(adm.census.size()), adm.n_admitted,
                      adm.n_quarantined_census);
            for (const auto& [c, t] : per_cons)
                log->info("Network (CB-ADM):   [{}] {} pairs: {} admitted, {} low-count, "
                          "{} no-IF-pin.", c, t.seen, t.admitted, t.lowcount, t.noifpin);
            constexpr int kMaxRows = 40;
            int shown = 0;
            for (const auto& p : adm.census) {
                if (shown++ >= kMaxRows) {
                    log->info("Network (CB-ADM):   ... ({} more pairs omitted from the table)",
                              static_cast<int>(adm.census.size()) - kMaxRows);
                    break;
                }
                const char* verdict = "ADMIT";
                const char* reason  = "both tiers pass";
                if (p.verdict == NetworkBiasAdmission::Verdict::LowCount) {
                    verdict = "QUAR";
                    reason  = "support < effective_min (tier-1)";
                } else if (p.verdict == NetworkBiasAdmission::Verdict::NoIfPin) {
                    verdict = "QUAR";
                    reason  = "no IF-pinnable pair, < 2 distinct bands (tier-2)";
                }
                log->info("Network (CB-ADM):   {:<11} support={:<3} {:<5} ({})",
                          p.key, p.support, verdict, reason);
            }
        } else {
            log->warn("Network (CB-ADM): census produced NO classification (empty window "
                      "at i0={} of {} synced) — falling back to the legacy per-epoch "
                      ">= 2-station gate.", i0, synced.size());
        }
    }

    for (const auto& se : synced) {
        // Decomposition INC-4: per-epoch workspace (the RAIM-replay capture
        // channel + the clock-model prediction maps) — constructed FRESH every
        // iteration (fresh construction IS the per-epoch reset semantics); see
        // filter/NetworkRunTypes.hpp EpochWorkspace for the writer-site contract.
        EpochWorkspace ws;
        // Optional testing fast-forward (network.start_offset_hours): skip epochs until the
        // elapsed data span from the first synced epoch reaches the offset, then cold-start the
        // solve there.  Skipped epochs are not counted, modeled, solved, or stashed, so the
        // carry begins empty at the offset — reproduces an externally-driven late-hour event
        // (e.g. the ~4.3 h lock-in) after a fresh warm-up, not the exact accumulated history.
        if (start_offset_s > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (start_anchor_s < 0.0) start_anchor_s = now_s;
            if (now_s - start_anchor_s < start_offset_s) continue;
        }
        LogManager::setCurrentEpochTime(se.time);

        // Card 8 (replay parity): feed the staged broadcast nav records whose
        // availability (toc, GPS scale) this epoch has reached.  The store's
        // current/previous retention then evolves exactly as under live
        // arrival — IODE rolls happen at their true times (Card 6 re-levels
        // fire), and the fit-window guard sees fresh records instead of the
        // end-of-day pair a bulk load would leave.  The first processed epoch
        // catches up over any start_offset span in one sorted batch, which
        // ends in the identical store state as epoch-by-epoch feeding.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            while (brdc_nav_cursor < brdc_nav_staged.size()
                   && navAvailabilityGpsSeconds(brdc_nav_staged[brdc_nav_cursor]) <= now_s) {
                eph_store.insert(brdc_nav_staged[brdc_nav_cursor], /*from_igs=*/true);
                ++brdc_nav_cursor;
            }
        }

        // Optional max-duration cutoff (processing.max_duration_s): anchor on the
        // first synced epoch, stop once the elapsed data span reaches the limit.
        if (config_.getMaxDuration() > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (!max_dur_anchored) { max_dur_anchor_s = now_s; max_dur_anchored = true; }
            else if (now_s - max_dur_anchor_s >= config_.getMaxDuration()) {
                log->info("Network: reached max processing duration ({:.1f} s of data); stopping",
                          config_.getMaxDuration());
                break;
            }
        }
        // T2e: epoch decimation — process only every Nth synced epoch (N=1 ⇒ modulo 0 always
        // ⇒ never skips ⇒ byte-identical).  Placed AFTER the start-offset + max-duration guards
        // (so the physical-time window/break stay evaluated on every raw epoch) and BEFORE
        // ++n_loop_epochs (so decimated-out epochs are not counted — n_loop_epochs stays the
        // processed-epoch count).  The brdc_rts nav catch-up is idempotent across skips.
        {
            const size_t dn = static_cast<size_t>(std::max(1, nc.epoch_decimation_n));
            if ((decim_counter++ % dn) != 0) continue;
        }
        ++n_loop_epochs;  // [TIMING]

        // D0: per-epoch section-timer baseline.  Deltas of the estimator's (run-
        // cumulative) accumulators against this snapshot give THIS epoch's reduce /
        // solve / cov-inverse / cov-consumer cost; sec_epoch_t0 anchors the epoch
        // wall; sec_asm_ms_epoch is filled at the assembly wall below.  Inert (no
        // clock read) when the toggle is OFF.
        NetworkNormalEquations::SectionTimers sec_t0;
        std::chrono::steady_clock::time_point sec_epoch_t0;
        double sec_asm_ms_epoch = 0.0;
        if (sec_timers_on) {
            sec_t0 = est.sectionTimers();
            sec_epoch_t0 = std::chrono::steady_clock::now();
            sec_sub_ep0 = sec_sub;   // D0b: per-epoch deltas of the sub-block run totals
        }

        // P1 rotation of the unbounded service CSVs (body relocated to
        // NetworkRunTraces, INC-4; OFF => byte-identical no-op).
        traces.rotateServiceCsvs(nc, log);

        // RTB-6 health heartbeat (body relocated to NetworkRunTraces, INC-4;
        // window anchors + last_health_write_s live in ctx.counters).
        traces.writeHealthRow(se.time, nc, ctx.counters, t_loop_begin,
                              ura_on, raim_on, pl_on, ura_sigma0_ewma,
                              last_pivot, grade_metric_s);

        // L1/L2: the row-writer bodies live in NetworkRunTraces (INC-4); these
        // thin forwarders keep every downstream call site textually unchanged.
        auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
            traces.emitEpochStatus(se.time, disposition, r);
        };

        auto emitEvent = [&](const char* tier, const char* event,
                             const std::string& sat, const std::string& station,
                             std::optional<double> statistic, std::optional<double> threshold,
                             const char* action, const std::string& detail) {
            traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                             action, detail);
        };

        const double dt = (prev_tow < 0.0) ? 0.0
            : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));

        // Phase-leveling carry timing (Increment 1): continuous-GPS seconds of this
        // epoch (for the per-ambiguity inter-epoch gap) and whether the cold-start
        // warmup has elapsed.  Count EMITTED product epochs (not raw loop epochs) so
        // the carry waits for the clock/SH convergence horizon — locking an
        // unconverged ambiguity onto a contaminated cold-start value would propagate
        // it (design §3.7).  n_emitted here excludes the current epoch (not yet emitted).
        const double pl_now_s   = TimeUtils::continuousGpsSeconds(se.time);
        // T1d: when convergence-gated warmup is armed AND an SH field is active, warm the
        // carry only once the field has gone stationary (sh_converged_latch, set post-solve
        // below from the epoch-over-epoch coefficient RMS Δ + a₀₀ physical band) — otherwise
        // the fixed emitted-epoch count.  Falls back to the epoch count when there is no SH
        // field to converge.  OFF (default) ⇒ the exact epoch-count gate ⇒ byte-identical.
        const bool   carry_warm = pl_on && ((pl_warmup_on_conv && sh_model)
                                                ? sh_converged_latch
                                                : (static_cast<int>(n_emitted) >= pl_warmup));

        // Card 4 (M1) C3c: this epoch's slip event feed for the DD invariant carry —
        // (app station index, ambiguity key) pairs the PASS-A slip detectors flagged.
        // Populated at the DETECTION site (the slip/NetworkSlipQc.cpp drains)
        // rather than read from pending-reseed at fold time, because PASS-B
        // MeasurementModel::compute re-seeds and CLEARS the pending-reseed flag before
        // buildCarryDdPriors runs (the flag is a one-shot the assembly consumes).  Also
        // recorded REGARDLESS of carry_warm (the detectors run cold, and the DD carry is
        // live from epoch 1) so the carry is slip-protected through the whole warmup —
        // not only after the SH field's convergence gate releases.  carry_invariant-only.
        std::set<std::pair<int, std::string>> carry_slipped_ambs;
        // Card 4 (M1) D5: seed this epoch's slip set with the innovation gate's PREVIOUS-epoch
        // flags (a fine slip caught post-solve last epoch re-taps its arc fresh here, via the
        // same C3c membership-exclusion path the coarse detectors feed).  One-shot: consumed now.
        if (carry_innov_gate && !carry_innov_pending.empty()) {
            carry_slipped_ambs.insert(carry_innov_pending.begin(), carry_innov_pending.end());
            carry_innov_pending.clear();
        }

        // --- PASS A: per-station geometry + shared-state layout -------------
        std::vector<std::vector<PppObservation>> modeled(stations.size());
        std::vector<bool> present(stations.size(), false);
        // Legacy: the pivot's allocated phase-bias keys ARE the shared set.
        // Prioritized: seeded empty here; the anchor election after the cross-
        // station observability prune rebuilds it as the anchored UNION over
        // present stations (each key backed by exactly one elected pin).
        std::set<std::string> shared_phase_keys;
        // Pivot-first head + PASS-A parallel tail + slip scans relocated to
        // ApplicationNetworkAssembly.cpp (INC-7).  networkPassAHead_ models the
        // legacy pivot (skip bookkeeping verbatim inside; SkipEpoch replaces the
        // former `continue`s) and seeds the legacy shared set; the SB_SLIP wall
        // times the scan exactly as before.
        if (networkPassAHead_(se, ctx, setup, traces, modeled, present,
                              shared_phase_keys) == EpochAction::SkipEpoch)
            continue;

        subBegin(SB_SLIP);   // D0b block 1: PASS-A slip scans (pre-assembly)
        ++slip_qc.epoch_idx;   // monotonic PROCESSED-epoch index (Card G gap arithmetic)
        networkPassASlipScan_(se, ctx, setup, traces, dt, pl_now_s, carry_warm,
                              modeled, present, shared_phase_keys,
                              carry_slipped_ambs, slip_qc);
        subEnd(SB_SLIP);       // D0b: close block 1 (slip scans)
        subBegin(SB_PREASM);   // D0b: pre-assembly measurement build (post-slip → assembly wall)

        // RAW slip observables + cross-station observability pre-filter relocated
        // to ApplicationNetworkAssembly.cpp (INC-7): the writer-before-prune
        // ordering contract is structural inside networkAssemblyPrefilter_.
        int n_dropped_sats = 0;
        networkAssemblyPrefilter_(se, setup, traces, slip_qc,
                                  modeled, present, n_dropped_sats);
        // The final shared phase-bias set + which station anchors (gauge-pins) each
        // (sat,signal).  Two realizations of the SAME minimum constraint (exactly
        // one ambiguity pin per phase-bias freedom):
        //   Legacy (network.phase_datum_prioritized OFF): the single pivot's pruned
        //     phase-bias states ARE the shared set (each backed by a pivot-receiver
        //     pin).  Byte-identical.
        //   Prioritized: elect, per (sat,signal), the highest-coverage receiver that
        //     tracks it with a live ambiguity as its anchor — decoupled from the
        //     clock pivot, so a satellite low/unseen at the clock pivot keeps a clean
        //     gauge at another station instead of jumping.
        if (!phase_datum_prioritized) {
            shared_phase_keys.clear();
            for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
                shared_phase_keys.insert(kv.first);
        } else {
            const bool   sticky  = config_.networkPhaseDatumSticky();

            // This epoch's candidate census + the network.phase_datum_min_stations
            // coverage gate.  Relocated to filter/NetworkPhaseDatum at INC-8b:
            // built ONCE here and shared by whichever realization the dispatch
            // below picks.  The builder also PRUNES the phase-bias states of
            // gated keys, which all three elections depend on having already
            // happened, and it is the sole writer of the two run-end gate
            // counters (computing the census twice would double-count them).
            PhaseDatumCandidates cnd;
            networkBuildPhaseDatumCandidates(config_, stations, present, modeled,
                                             cnd, n_gated_epochs, n_gated_key_epochs);

            if (phase_datum_ensemble) {
                networkElectEnsembleDatum(stations, cnd, this_epoch_anchor,
                                          shared_phase_keys);
            } else if (phase_datum_eq34) {
                // Card 3 chain datum.  MUTATES the carries pre-solve and fills
                // ctx.carry.eq34_reref, so it must stay ahead of the D3c RPB
                // allocation below (it is the sole writer of shared_phase_keys on
                // this path, and D3c derives its need_rpb set from that) and hence
                // ahead of est.beginEpoch / PASS B.  Full ordering contract in
                // filter/NetworkChainDatum.hpp.
                networkElectChainDatum(ctx, config_, log_manager_, log, stations,
                                       present, cnd, shared_phase_keys);
            } else {
                networkElectPhaseAnchors(sticky, stations, present, cnd,
                                         anchor_incumbent, this_epoch_anchor,
                                         shared_phase_keys);
            }  // end anchor election (!ensemble && !eq34)
        }

        // Card 3 (eq34) RPB allocation (rule D3c): a present station gets the
        // per-(receiver,system,signal) split state δ_{r,σ} iff it carries an
        // UNGATED shared phase-bias column on σ (a phase-bias state whose key is in
        // shared_phase_keys) — i.e. iff it is a vertex of the D2 graph.  A station
        // whose σ-keys are all gated allocates no RPB and keeps legacy lumping.
        // Re-derived fresh each epoch (allocate what's needed, prune the rest); runs
        // BEFORE est.reset()/PASS B so buildJacobianRow's +1 RPB coefficient sees the
        // column and splitStation routes it into the local block A_r.  A spanning
        // tree touches every vertex, so every RPB-carrying station carries ≥1
        // tree-edge pin ⇒ its N_rr stays PD.  Inert (no allocation) when eq34 off.
        if (phase_datum_eq34) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> need_rpb;
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    if (!shared_phase_keys.count(kv.first)) continue;   // gated → legacy lump
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (!p || p->satellite.empty()) continue;
                    need_rpb.insert(StateKeys::makeReceiverPhaseBiasKey(
                        PppUtils::getSystemString(p->satellite), p->signal));
                }
                for (const auto& rk : need_rpb)
                    if (sm.getReceiverPhaseBiasIndex(rk) < 0)
                        sm.addReceiverPhaseBiasState(rk, 0.0);
                sm.pruneReceiverPhaseBiasStatesExcept(need_rpb);
            }
        }

        // Shared-column registration + counts + estimability descriptors relocated
        // to ApplicationNetworkAssembly.cpp (INC-7).  Outputs are fresh per epoch;
        // the empty-system skip keeps its exact bookkeeping inside (SkipEpoch
        // return), and the `continue` below bypasses subEnd(SB_PREASM) exactly as
        // the original site did.  est.beginEpoch() runs inside on Proceed.
        std::vector<std::string> registered_clock_sats;
        std::vector<std::pair<std::string, std::string>> registered_code_keys;
        std::vector<std::string> col_desc;
        int n_clock = 0, n_pb = 0, n_cb = 0, n_present = 0;
        if (networkAssemblyRegisterColumns_(se, ctx, setup, traces, est, present,
                                            shared_phase_keys, registered_clock_sats,
                                            registered_code_keys, col_desc,
                                            n_clock, n_pb, n_cb, n_present)
            == EpochAction::SkipEpoch)
            continue;

        // Capture the shared-border folds (code-bias gauge constraint + all carried /
        // anchor priors) applied to THIS epoch's arrowhead, so the within-epoch
        // adaptation (drop a culprit → re-solve) can replay the identical shared system
        // over a fresh beginEpoch().  addSharedPrior is additive, so the three prior
        // sets concatenate into one replayable vector.  Populated only when the
        // adaptation is armed; otherwise the appends are cheap no-ops.
        const bool within_epoch_armed = config_.networkRaimWithinEpoch();
        auto& cap_H = ws.cap_H;
        auto& cap_z = ws.cap_z;
        auto& cap_constraint_weight = ws.cap_constraint_weight;
        auto& cap_has_constraint = ws.cap_has_constraint;
        auto& cap_priors = ws.cap_priors;
        auto& cap_dense_keys = ws.cap_dense_keys;
        auto& cap_dense_cov = ws.cap_dense_cov;

        subEnd(SB_PREASM);   // D0b: close pre-assembly (the assembly wall below is timed separately)

        // --- PASS B relocated to ApplicationNetworkAssembly.cpp (INC-7):
        // networkPassB_ owns the D0 assembly wall (raw chrono pair — moves
        // whole, sinks by reference), the T5 sh-dev aging, the Card-1d reinit
        // history, the parallel produce loop (pragma verbatim, incl. the
        // coupling-injection cluster — INC-7b carves that later), and the
        // serial consume; the failed/empty-epoch skip keeps its exact
        // bookkeeping inside (SkipEpoch return).
        std::vector<StationAssembly> asm_res(stations.size());
        int  total_pins = 0;
        // est-station index per station (the order addStation was called) so the
        // post-solve back-substitution can recover each present station's local
        // slant-iono block; -1 for stations not added this epoch.
        std::vector<int> est_index_of(stations.size(), -1);
        if (networkPassB_(se, ctx, setup, traces, est, iono_deweight,
                          sh_dev_tau, sh_dev_on, carry_slipped_ambs,
                          carry_warm, pl_now_s,
                          modeled, present, shared_phase_keys,
                          registered_clock_sats, registered_code_keys,
                          n_clock, n_pb, n_present,
                          asm_res, est_index_of, total_pins,
                          sec_asm_ms_epoch, sec_asm_wall_ms) == EpochAction::SkipEpoch)
            continue;
        subBegin(SB_GAUGEBUILD); // D0b: buildEnsembleGauges + carry-driver lambda defs (pre-DD-build)

        // Fix A (network.phase_datum_ensemble): build the anchor-free sum-to-zero
        // phase-bias gauges from every tracking station's ambiguity column and hand
        // them to the estimator, which folds Σ_r zˢ_{r,σ}=0 through the kernel
        // (Schur-projected) at the gate/solve reduce.  One gauge per (sat,signal)
        // that has a registered shared phase-bias column (thin/gated keys have no
        // column → their ambiguity floats).  Re-invoked on the within-epoch RAIM
        // re-solve (station set / indices may change).  Empty / no-op when OFF.
        auto buildEnsembleGauges = [&]() {
            if (!phase_datum_ensemble) return;
            // The (sat,signal) that actually have a shared phase-bias column this
            // epoch (a gauge is needed only for those; thin/gated signals float).
            std::set<std::string> shared_ss;
            for (const auto& pk : shared_phase_keys) {
                const auto p = StateKeys::parseSatPhaseBiasKey(pk);
                if (p) shared_ss.insert(p->satellite + "\x1f" + p->signal);
            }
            // Seed-once + time-lock: a (sat,signal) that already carries a HELD bias
            // (from the previous epoch's D2 stash) is datumed by that stiff carried
            // prior — do NOT also fold Σz=0 for it (a hard w→∞ fold would override the
            // finite hold prior and re-realize the churny level).  Only fresh /
            // re-seeded (sat,signal) get a gauge this epoch.  Empty when hold is off.
            std::set<std::string> held;
            if (phase_hold_on)
                for (const CarryPrior& cp : prev_carry)
                    // var>0 mirrors the hold-prior injector's guard EXACTLY, so a
                    // (sat,signal) is fold-skipped iff it will actually get a hold
                    // prior — a var<=0 carry (e.g. a corrupt warm-resume snapshot) is
                    // NOT held here, so it re-seeds via the fold instead of ending up
                    // un-datumed.
                    if (cp.kind == SharedParamKind::PhaseBias && cp.var > 0.0)
                        held.insert(cp.sat + "\x1f" + cp.signal);
            std::map<std::string, NetworkNormalEquations::SumZeroGauge> by_key;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                for (const auto& [sat, sig, col] : asm_res[i].ens_amb_cols) {
                    const std::string ss = sat + "\x1f" + sig;
                    if (!shared_ss.count(ss)) continue;   // no shared phase bias → floats
                    if (held.count(ss)) continue;         // held by the carried prior → skip the fold
                    by_key[ss].terms.emplace_back(
                        static_cast<std::size_t>(est_index_of[i]), col);
                }
            }
            std::vector<NetworkNormalEquations::SumZeroGauge> gauges;
            gauges.reserve(by_key.size());
            for (auto& [k, gz] : by_key) {
                if (gz.terms.empty()) continue;
                // Record the shared phase-bias column this gauge fixes, so the
                // estimator can treat it as an EXACT datum column in the datum-frame
                // conditional covariance (the fold leaves it carrying an artificial
                // gauge-direction variance — see SumZeroGauge::shared_col).  k is
                // "sat\x1fsignal"; every key in by_key came from shared_ss, so it has
                // a registered phase-bias column.
                const auto us = k.find('\x1f');
                if (us != std::string::npos) {
                    const std::string sat = k.substr(0, us);
                    const std::string sig = k.substr(us + 1);
                    try { gz.shared_col = est.phaseBiasColumn(sat, sig); }
                    catch (...) { gz.shared_col = -1; }
                }
                gauges.push_back(std::move(gz));
            }
            est.setPhaseSumZeroGauges(std::move(gauges));
        };
        buildEnsembleGauges();

        // Card 4 (M1) C3c: within-epoch RAIM culprit set forwarded into the DD-carry
        // re-solve rebuild (spec §3 culprit-withhold).  The pointers alias the RAIM
        // block's drop sets while it runs, else null.  The PASS-A slip feed
        // (carry_slipped_ambs, above) is the primary source; both are carry_invariant-only
        // ⇒ OFF byte-identical (buildCarryDdPriors early-returns when the toggle is off).
        const std::set<std::string>*                carry_we_drop_sats = nullptr;
        const std::map<int, std::set<std::string>>* carry_we_drop_arcs = nullptr;

        // Card 4 (M1) D5: reverse map (group key → chord ArcKey → ambiguity state key) so a
        // post-solve innovation flag (group, chord) maps back to its (station, ambiguity) for
        // the next epoch's slip funnel.  Rebuilt with the blocks each buildCarryDdPriors call
        // (the last call before the solve is authoritative).  Empty unless carry_innov_gate.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key;
        // WL-3: the pair groups' BAND-2 ambiguity keys, parallel to carry_arc_amb_key
        // (band-1).  A D5 flag on a pair chord feeds BOTH bands into the slip funnel
        // (§3.4 union refloat).  Empty unless lane_carry + carry_innov_gate.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key2;

        // --- Card G (docs/SPEC_slip_qc_hardening.md §5): arc-resumption bridge
        //     classification.  INSTRUMENTATION ONLY — nothing below changes a
        //     decision; it answers G1(b): of the arcs that came back after a
        //     tracking break whose RAW level shifted, how many re-entered
        //     carrying state that bridged the break (the poison case) versus as
        //     a fresh float (benign)?
        //
        //     Placement is load-bearing: AFTER the slip drains (so
        //     carry_slipped_ambs — the only thing that forces a Card-4 chord to
        //     re-tap — is complete for this epoch) and BEFORE buildCarryDdPriors
        //     re-keys the carry (so carriedChordValue / carry_sat_vertex /
        //     carry_group_id still describe the state the arc is RETURNING TO).
        //     carry_group_id is read with find(), never operator[] — minting a
        //     group id here would corrupt the driver's stable keying.
        //
        //     The unit is the (station, satellite, signal) ARC, not the
        //     resumption: chords are per-arc, so a dual-band satellite returning
        //     once contributes two chords that can be bridged independently.
        if (nc.slip_qc.gap_instrument && !slip_qc.epoch.resumptions.empty()) {
            // Does this carry_group_id key's signal component cover `sig`?  Under
            // the WL-3 lane shape a paired arc lives in a group keyed
            // "<sigA>+<sigB>", not in its per-band group — a per-band-only lookup
            // would report every paired arc as un-bridged (a silent false NEGATIVE
            // on the exact question this card exists to answer).
            auto groupCoversSignal = [](const std::string& gsig, const std::string& sig) {
                if (gsig == sig) return true;
                std::size_t p = 0;
                while (p <= gsig.size()) {
                    const std::size_t q = gsig.find('+', p);
                    const std::size_t e = (q == std::string::npos) ? gsig.size() : q;
                    if (gsig.compare(p, e - p, sig) == 0) return true;
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
                return false;
            };
            for (const auto& g : slip_qc.epoch.resumptions) {
                const int i = g.station;
                if (i < 0 || i >= static_cast<int>(stations.size())) continue;
                const char cons = g.satellite.empty() ? '\0' : g.satellite.front();
                const auto svit = carry_sat_vertex.find(g.satellite);
                for (const std::string& sig : {g.sig1, g.sig2}) {
                    if (sig.empty()) continue;
                    const std::string amb_key = StateKeys::makeAmbiguityKey(g.satellite, sig);
                    const bool slip_marked = carry_slipped_ambs.count({i, amb_key}) != 0;
                    // (a) Legacy phase-leveling carry: a value inside the
                    //     max-gap window is exactly what the PASS-B injector
                    //     would fold across the break.
                    bool pl_bridged = false;
                    {
                        const auto cit = local_carry.find(std::make_tuple(i, g.satellite, sig));
                        if (cit != local_carry.end()) {
                            const double gap = pl_now_s - cit->second.last_seen_s;
                            pl_bridged = (gap >= 0.0 && gap <= pl_max_gap_s);
                        }
                    }
                    // (b) Card-4 invariant DD carry: does the driver still hold a
                    //     carried chord for this arc from BEFORE the break?
                    //     carry_sat_vertex ALREADY stores the kSatBase-offset
                    //     vertex id, so it is used verbatim — re-adding the base
                    //     would look up a chord that cannot exist and report
                    //     "never bridged" for everything.
                    bool dd_bridged = false;
                    if (carry_invariant && svit != carry_sat_vertex.end() && cons != '\0') {
                        for (const auto& [gkey, gid] : carry_group_id) {
                            if (gkey.first != cons || gid == 0) continue;
                            if (!groupCoversSignal(gkey.second, sig)) continue;
                            double zg = 0.0, vg = 0.0;
                            if (carry_driver.carriedChordValue(gid, ArcKey(i, svit->second),
                                                               zg, vg)) {
                                dd_bridged = true;
                                break;
                            }
                        }
                    }
                    const bool bridged = (pl_bridged || dd_bridged) && !slip_marked;
                    // H2: the arc ledger mirrors the RESUMPTION three-way split
                    // (shifted / measured-and-unshifted / unmeasurable) and every
                    // cell is counted, so it closes as bridged+fresh on each row
                    // instead of leaving a reader to infer the missing one.
                    // Unmeasurable is kept SEPARATE from clean deliberately: an
                    // arc that bridged a break whose level nobody could check is
                    // not benign, it is UNKNOWN, and G3's "shifted-and-bridged ≈ 0
                    // ⇒ defer" ruling is only as strong as that number is small.
                    if (g.shifted) {
                        if (bridged) ++slip_qc.counters.gap_shifted_bridged;
                        else         ++slip_qc.counters.gap_shifted_fresh;
                    } else if (g.measurable) {
                        if (bridged) ++slip_qc.counters.gap_clean_bridged;
                        else         ++slip_qc.counters.gap_clean_fresh;
                    } else {
                        if (bridged) ++slip_qc.counters.gap_unmeas_bridged;
                        else         ++slip_qc.counters.gap_unmeas_fresh;
                    }
                    // The per-arc event carries the SAME three-way verdict as the
                    // counters above — an unmeasurable resumption is tagged
                    // `unmeas_*`, never `clean_*`, and its ΔGF is written BLANK
                    // exactly as its ΔMW already was.  A fabricated "dGF=0.0000"
                    // on an arc with no pre-gap level reads as "the level did not
                    // move", which is the absence-of-evidence claim this whole
                    // instrument exists to avoid making.
                    emitEvent("T0_gap", "arc_resume", g.satellite, stations[i].name,
                              g.measurable ? std::optional<double>(g.d_mw) : std::nullopt,
                              std::nullopt,
                              g.shifted     ? (bridged ? "shifted_bridged" : "shifted_fresh")
                              : g.measurable ? (bridged ? "clean_bridged"   : "clean_fresh")
                                             : (bridged ? "unmeas_bridged"  : "unmeas_fresh"),
                              fmt::format("{};gap_ep={};dGF={};pl={};dd={};slip={}",
                                          sig, g.gap_epochs,
                                          g.measurable ? fmt::format("{:.4f}", g.d_gf)
                                                       : std::string{},
                                          pl_bridged ? 1 : 0, dd_bridged ? 1 : 0,
                                          slip_marked ? 1 : 0));
                }
            }
        }

        // Card 4 (M1) C3b: build this epoch's DD-prior blocks from the eq34 election
        // tree and fold the carried invariant DDs into the producer solve.  Each arc
        // (receiver, satellite) ambiguity on a (const,signal) is resolved to its
        // estimator (station, local ambiguity column); pinned_amb_targets marks the
        // spanning-tree edges (the gauge), the rest are chords (the carried DDs).
        // GLONASS (FDMA) excluded.  Vertex/group ids are stable across epochs (sat
        // label anchored; receiver = app index).  Re-invoked on the RAIM re-solve
        // (est.beginEpoch there clears the priors + rebuilds est_index_of), mirroring
        // buildEnsembleGauges.  No-op / OFF byte-identical unless carry_invariant.
        auto buildCarryDdPriors = [&]() {
            if (!carry_invariant) return;
            constexpr int kSatBase = 1000000;   // satellite vertices sit above every receiver index
            std::map<int, NetworkInvariantCarryDriver::GroupInput> groups;
            if (carry_innov_gate) { carry_arc_amb_key.clear(); carry_arc_amb_key2.clear(); }  // D5: rebuilt with this call's arcs

            // --- Shared WL-1/WL-2/WL-3 machinery (revmap / arc_signals / lin_of / iono_of /
            // primary / band_top_of / λ cache).  HOISTED above the group build because the WL-3
            // pair regroup routes each primary-band ambiguity by its (rx,sat) primary pair.
            // Byte-identical when every lane is off: nothing here is populated, and building
            // it earlier has no effect on the solve (pure reads of station state).
            const bool mw_mach_on = mw_wl_on || ewl_on || lane_on;
            auto sysFromSat = [](const std::string& sat) -> System {
                if (sat.empty()) return System::Unknown;
                switch (sat.front()) {
                    case 'G': return System::GPS;     case 'E': return System::Galileo;
                    case 'R': return System::GLONASS; case 'C': return System::BeiDou;
                    case 'J': return System::QZSS;    case 'S': return System::SBAS;
                    default:  return System::Unknown;
                }
            };
            // Reverse map (est_station,col)->arc identity and per-(rx,sat) ambiguity
            // signals over ALL resolvable ambiguities (both bands, incl. those NOT in
            // shared_phase_keys — the WL row needs the L2 column even when L2 is not an
            // eq34 arc).  GLONASS excluded (never enters the carry blocks anyway).
            struct Rev { int rx; std::string sat; std::string signal; };
            std::map<std::pair<int, int>, Rev> revmap;
            std::map<std::pair<int, std::string>, std::vector<std::pair<std::string, int>>> arc_signals;
            // (est_station, local col) → ambiguity LINEARIZATION value (m).  The kernel
            // solves corrections about x_lin (no KF write-back), so the WL layer must
            // re-express the absolute MW measurement in the correction frame (F5).
            std::map<std::pair<int, int>, double> lin_of;
            // WL-2 §3.4 v2: (rx app index, sat) → the estimated L1-referenced slant iono (m)
            // for the bridge de-bias.  The v1 Welford-only guard is falsified by a STABLE
            // differential iono (Fable): a constant iono bias has ~0 Welford variance yet
            // biases the rounded WL integer (near-integer bias ⇒ slips the FFR frac gate).
            // We subtract the network's own iono estimate; the residual DD-iono estimate
            // error surfaces in |frac| and is caught by the FFR gate + MW(1,2) closure.
            std::map<std::pair<int, std::string>, double> iono_of;
            // Estimator primary pair per (rx,sat): the two lowest DISTINCT bands (ties
            // lexicographic for determinism); v1 restricts WL rows to this pair (§11 Q3).
            struct PP { std::string sig1, sig2; int col1{ -1 }, col2{ -1 }; bool ok{ false }; };
            std::map<std::pair<int, std::string>, PP> primary;
            // WL-2 EWL: the band-5 (GPS L5 / Galileo E5a) signal + column per (rx,sat),
            // for the (band2, band5) extra-wide lane.  arc_signals is sorted by band digit
            // then name, so the first '5' is deterministic.
            std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top_of;
            // Increment 5 (uwl_dual_top): the FALLBACK top rung's signal + column per
            // (rx,sat) (Galileo '7' when the preferred b_top is '8').  Empty unless a
            // tier arms a fallback rung; the fallback lane instance's resolver and the
            // walk's fallback-tagged rows read THIS map instead of band_top_of.
            std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top2_of;
            // E6 mid hop (uwl_mid_enable): the mid-rung band signal per (rx,sat) — only
            // the SIGNAL is needed (the hops are phase-only detector reads; no estimator
            // column enters the chain).  Empty unless the tier arms a mid rung.
            std::map<std::pair<int, std::string>, std::string> band_mid_of;
            // WL-3: the constellation's ONE census-preferred primary pair — only arcs whose
            // primary pair matches it join the pair group; minority-channel arcs stay fully
            // per-band (as today), so they keep their own carried cycles instead of spawning
            // flickering micro pair-groups.  The third (EWL) band never becomes a pair band.
            std::map<char, std::pair<std::string, std::string>> lane_pref_pair;
            // Per-(sat,signal) wavelength cache (FDMA-correct via the satellite id).
            std::map<std::pair<std::string, std::string>, double> lam_cache;
            auto lam = [&](const std::string& sat, const std::string& sig) -> double {
                const auto k = std::make_pair(sat, sig);
                const auto it = lam_cache.find(k);
                if (it != lam_cache.end()) return it->second;
                const double v = mw_wl_ppp_utils->getWavelength(sysFromSat(sat), sig, sat);
                lam_cache.emplace(k, v);
                return v;
            };
            if (mw_mach_on) {
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (est_index_of[i] < 0) continue;
                    const int es = est_index_of[i];
                    const auto& h2l = asm_res[i].h2local;
                    const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                    for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(key);
                        if (!parts || parts->satellite.empty() || parts->signal.empty()) continue;
                        if (parts->satellite.front() == 'R') continue;   // GLONASS excluded
                        const auto hit = h2l.find(static_cast<int>(sidx));
                        if (hit == h2l.end() || hit->second < 0) continue;
                        const int col = static_cast<int>(hit->second);
                        revmap[{es, col}] = { static_cast<int>(i), parts->satellite, parts->signal };
                        arc_signals[{static_cast<int>(i), parts->satellite}]
                            .push_back({ parts->signal, col });
                        if (static_cast<Eigen::Index>(sidx) < x_lin.size())
                            lin_of[{es, col}] = x_lin(static_cast<Eigen::Index>(sidx));
                    }
                    if (wl_inject_on || lane_bridge_on) {
                        std::map<std::string, double> sat_iono;   // per-sample de-bias push
                        for (const auto& [sat, iidx] : stations[i].sm->getIonosphereStates()) {
                            const Eigen::Index ii = static_cast<Eigen::Index>(iidx);
                            if (ii >= 0 && ii < x_lin.size()) {
                                iono_of[{ static_cast<int>(i), sat }] = x_lin(ii);
                                if (bridge_debias_on) sat_iono[sat] = x_lin(ii);
                            }
                        }
                        // 60-ep age-bias fix: push this station's estimable slant-iono
                        // values for the detector's per-sample bridge de-bias.  Applies
                        // from the NEXT detector pass (one-epoch lag, F1-consistent);
                        // idempotent within an epoch (RAIM re-solve safe).
                        if (bridge_debias_on && stations[i].mw_slip)
                            stations[i].mw_slip->setBridgeIonoEstimates(std::move(sat_iono));
                    }
                }
                // WL-3: network-preferred channel per (constellation, band).  The
                // lexicographic tie-break fragments the primary pairs across stations
                // (L2L where tracked, L2W elsewhere), which splits the pair groups'
                // graphs and forfeits every cross-boundary cycle (~1/3 of the primary
                // cycle space at 50 stations, measured).  Under lane_carry the pair
                // prefers the constellation's MAJORITY channel per band (deterministic
                // census over this build's resolvable ambiguities; ties lexicographic),
                // falling back to the arc's own lowest channel where the majority one
                // is untracked.  WL-1/WL-2-only shapes keep the legacy tie-break.
                std::map<std::pair<char, char>, std::string> chan_pref;
                if (lane_on) {
                    std::map<std::pair<char, std::string>, long long> chan_count;
                    for (const auto& [rs, sigs] : arc_signals) {
                        const char cons = rs.second.empty() ? '?' : rs.second.front();
                        for (const auto& [sig, col] : sigs) ++chan_count[{cons, sig}];
                    }
                    for (const auto& [k, cnt] : chan_count) {
                        const char band = k.second.size() >= 2 ? k.second[1] : '\0';
                        if (band == '\0') continue;
                        std::string& best = chan_pref[{k.first, band}];
                        long long bc = -1;
                        if (!best.empty()) {
                            const auto bit = chan_count.find({k.first, best});
                            bc = (bit != chan_count.end()) ? bit->second : -1;
                        }
                        if (best.empty() || cnt > bc || (cnt == bc && k.second < best))
                            best = k.second;
                    }
                    // UWL-1 (measured falsification of the per-band census): channel
                    // choices are CORRELATED per receiver family — Galileo splits into
                    // (L1C,L5Q) and (L1X,L5X) families, and the per-band majorities
                    // composed the PHANTOM pair (L1C,L5X) that NO station tracks: zero
                    // pair arcs formed and the whole constellation silently stayed
                    // per-band (group census, 10-ep GPS+GAL smoke).  Under the armed
                    // card the census is therefore JOINT: count each (rx,sat)'s raw
                    // two-lowest-band channel pair and elect the majority pair per
                    // constellation (ties lexicographic — the §8.2 deterministic seam),
                    // then steer the per-band preferences to the elected pair so
                    // preferChan / the bridge keys align with it.  The minority FAMILY
                    // stays fully per-band (v1 accepts ~half the Galileo pair space;
                    // per-family pair groups are the v2 recovery — the group keying
                    // already supports them).  uwl_enable=false keeps the per-band
                    // census verbatim (the WL-3 validated shape, byte-identical).
                    if (uwl_on) {
                        std::map<std::pair<char, std::pair<std::string, std::string>>,
                                 long long> pair_count;
                        for (auto& [rs, sigs] : arc_signals) {
                            std::sort(sigs.begin(), sigs.end(),
                                      [](const std::pair<std::string, int>& a,
                                         const std::pair<std::string, int>& b) {
                                const char ba = a.first.size() >= 2 ? a.first[1] : '\0';
                                const char bb = b.first.size() >= 2 ? b.first[1] : '\0';
                                if (ba != bb) return ba < bb;
                                return a.first < b.first;
                            });
                            if (sigs.empty()) continue;
                            const char cons = rs.second.empty() ? '?' : rs.second.front();
                            // Two lowest CARRIED bands (the uncarried top band never
                            // pairs — mirror the routing's exclusion exactly).
                            std::string s1;
                            char b1 = '\0';
                            for (const auto& [sig, col] : sigs)
                                if (sig.size() >= 2 && !isUncarriedThirdBand(cons, sig)) {
                                    s1 = sig; b1 = sig[1]; break;
                                }
                            if (b1 == '\0') continue;
                            for (const auto& [sig, col] : sigs)
                                if (sig.size() >= 2 && sig[1] != b1
                                    && !isUncarriedThirdBand(cons, sig)) {
                                    ++pair_count[{cons, { s1, sig }}];
                                    break;
                                }
                        }
                        std::map<char, std::pair<std::string, std::string>> joint_best;
                        std::map<char, long long> joint_cnt;
                        for (const auto& [k, cnt] : pair_count) {
                            auto itb = joint_best.find(k.first);
                            if (itb == joint_best.end() || cnt > joint_cnt[k.first]
                                || (cnt == joint_cnt[k.first] && k.second < itb->second)) {
                                joint_best[k.first] = k.second;
                                joint_cnt[k.first]  = cnt;
                            }
                        }
                        for (const auto& [cons, pr] : joint_best) {
                            const char b1 = pr.first.size()  >= 2 ? pr.first[1]  : '\0';
                            const char b2 = pr.second.size() >= 2 ? pr.second[1] : '\0';
                            if (b1 == '\0' || b2 == '\0') continue;
                            chan_pref[{cons, b1}] = pr.first;
                            chan_pref[{cons, b2}] = pr.second;
                        }
                    }
                }
                for (auto& [rs, sigs] : arc_signals) {
                    std::sort(sigs.begin(), sigs.end(),
                              [](const std::pair<std::string, int>& a,
                                 const std::pair<std::string, int>& b) {
                        const char ba = a.first.size() >= 2 ? a.first[1] : '\0';
                        const char bb = b.first.size() >= 2 ? b.first[1] : '\0';
                        if (ba != bb) return ba < bb;
                        return a.first < b.first;
                    });
                    PP pp;
                    if (!sigs.empty()) { pp.sig1 = sigs.front().first; pp.col1 = sigs.front().second; }
                    const char b1 = pp.sig1.size() >= 2 ? pp.sig1[1] : '\0';
                    for (const auto& [sig, col] : sigs) {
                        if (sig.size() >= 2 && b1 != '\0' && sig[1] != b1) {
                            pp.sig2 = sig; pp.col2 = col; break;
                        }
                    }
                    if (lane_on && !sigs.empty()) {
                        const char cons = rs.second.empty() ? '?' : rs.second.front();
                        const char b2 = pp.sig2.size() >= 2 ? pp.sig2[1] : '\0';
                        auto preferChan = [&](char band, std::string& sig_io, int& col_io) {
                            const auto pit2 = chan_pref.find({cons, band});
                            if (pit2 == chan_pref.end() || pit2->second == sig_io) return;
                            for (const auto& [sig, col] : sigs)
                                if (sig == pit2->second) { sig_io = sig; col_io = col; return; }
                        };
                        if (b1 != '\0') preferChan(b1, pp.sig1, pp.col1);
                        if (b2 != '\0') preferChan(b2, pp.sig2, pp.col2);
                    }
                    pp.ok = (!pp.sig1.empty() && !pp.sig2.empty());
                    primary[rs] = pp;
                }
                // v2 (uwl_family_pairs): admit MINORITY family pairs into the run-latched
                // admissible set.  Counted over the STEERED primary pairs — the exact
                // quantity the routing below matches — NOT the raw two-lowest census
                // (measured defect, smoke E: L2L < L2W lexicographically, so every GPS
                // dual-tracker's RAW pair read (L1C,L2L) and inflated its count to 143
                // while routing saw 1 steered arc — a stranded 1-arc micro pair-group
                // that forfeits the arc's per-band cycles).  The majority pair routes
                // through lane_pref_pair exactly as v1 (its admission here is a harmless
                // duplicate); a minority pair is admitted the first epoch its steered
                // count reaches the floor, then stays latched (see the declaration).
                if (uwl_family_on) {
                    std::map<std::pair<char, std::pair<std::string, std::string>>,
                             long long> pp_count;
                    for (const auto& [rs, pp] : primary) {
                        if (!pp.ok) continue;
                        const char cons = rs.second.empty() ? '?' : rs.second.front();
                        ++pp_count[{ cons, { pp.sig1, pp.sig2 } }];
                    }
                    for (const auto& [k, cnt] : pp_count) {
                        if (cnt < nmw.uwl_family_min_arcs) continue;
                        if (lane_admit_pairs[k.first].insert(k.second).second
                            && lane_family_logged
                                   .insert({ k.first,
                                             k.second.first + "+" + k.second.second })
                                   .second)
                            log->info("Network (M5/UWL-1 family census): constellation '{}' "
                                      "admitted pair ({}, {}) — {} steered primary arc(s) "
                                      "(floor {}); latched for the run.",
                                      k.first, k.second.first, k.second.second, cnt,
                                      nmw.uwl_family_min_arcs);
                    }
                }
                if (lane_on) {
                    // The constellation's ONE preferred pair: the two lowest census bands,
                    // skipping the EWL third band (it must stay UNCARRIED — WL-2 §3.1).
                    std::map<char, std::vector<char>> cons_bands;
                    for (const auto& [k, sig] : chan_pref) {
                        const std::string probe = std::string("L") + k.second;   // "L<band>"
                        if (isUncarriedThirdBand(k.first, probe)) continue;
                        cons_bands[k.first].push_back(k.second);
                    }
                    for (auto& [cons, bands] : cons_bands) {
                        std::sort(bands.begin(), bands.end());
                        if (bands.size() < 2) continue;
                        lane_pref_pair[cons] = { chan_pref[{cons, bands[0]}],
                                                 chan_pref[{cons, bands[1]}] };
                        // Census visibility (once per run per cons): which pair the lane
                        // carries — fragmentation across channels is the WL-3 lesson and
                        // a Galileo pre-flight (SPEC_uwl_card.md §8.1); measure, don't assume.
                        if (lane_census_logged.insert(cons).second)
                            log->info("Network (M5/WL-3 census): constellation '{}' preferred "
                                      "primary pair = ({}, {}) over census bands [{}].",
                                      cons, lane_pref_pair[cons].first,
                                      lane_pref_pair[cons].second,
                                      std::string(bands.begin(), bands.end()));
                    }
                    // Align the detector-side bridge arcs with the census: without this,
                    // every (rx,sat) whose min-name channel differs from the census pair
                    // has NO bridge arc under the caller's key and the WL decision path
                    // starves (measured 9-of-29 decided cycles).  Applies from the next
                    // detector pass; the re-keyed arcs re-warm in ~2 epochs.
                    if (lane_bridge_on && !chan_pref.empty())
                        for (size_t i = 0; i < stations.size(); ++i)
                            if (stations[i].mw_slip)
                                stations[i].mw_slip->setBridgePreferredChannels(chan_pref);
                }
                for (auto& [rs, sigs] : arc_signals) {
                    // UWL-1 §3.1: the top-tier partner band digit.  Legacy (card OFF) =
                    // first '5' for EVERY constellation — which is what silently killed
                    // Galileo (its '5' is E5a, the primary sig2 itself ⇒ the degenerate
                    // (L5Q,L5Q) pair with no detector arc).  Armed = the lane-tier table
                    // (GPS '5' reproduces the legacy pick bit-exactly; Galileo '7'/'8').
                    char top_digit  = '5';
                    char mid_digit  = '\0';
                    char top2_digit = '\0';   // increment 5: the fallback rung's digit
                    const char cons = rs.second.empty() ? '?' : rs.second.front();
                    if (uwl_on) {
                        const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                                       nmw.uwl_mid_enable, nmw.uwl_dual_top);
                        if (!tier) {
                            if (ewl_on && cons != '?' && uwl_warned_cons.insert(cons).second)
                                log->warn("Network (M5/UWL-1): constellation '{}' has NO lane "
                                          "tier in the armed table — its cycles contribute no "
                                          "EWL/UWL statistic and no bridge (BDS-3 deferred on "
                                          "clock divergence; GLONASS FDMA-excluded).  Loud by "
                                          "design (SPEC §3.1).", cons);
                            continue;   // no top-band entry ⇒ counted skip_missing_band downstream
                        }
                        top_digit  = tier->b_top;
                        mid_digit  = tier->b_mid;
                        top2_digit = tier->b_top2;
                    } else if (ewl_on && cons != 'G' && cons != '?'
                               && uwl_warned_cons.insert(cons).second) {
                        log->warn("Network (M5/UWL-1): constellation '{}' enters the EWL lane "
                                  "under the LEGACY GPS band digits — its top-tier pair is "
                                  "degenerate and every cycle dies silently as skip_missing_mw "
                                  "(SPEC_uwl_card.md §1 fact 3).  Arm network.mw_wl.uwl_enable "
                                  "for the per-constellation lane-tier table.", cons);
                    }
                    for (const auto& [sig, col] : sigs)
                        if (sig.size() >= 2 && sig[1] == top_digit) { band_top_of[rs] = { sig, col }; break; }
                    if (top2_digit != '\0')
                        for (const auto& [sig, col] : sigs)
                            if (sig.size() >= 2 && sig[1] == top2_digit) { band_top2_of[rs] = { sig, col }; break; }
                    if (mid_digit != '\0')
                        for (const auto& [sig, col] : sigs)
                            if (sig.size() >= 2 && sig[1] == mid_digit) { band_mid_of[rs] = sig; break; }
                }
            }
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const auto& h2l = asm_res[i].h2local;
                for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts || parts->satellite.empty()) continue;
                    const char cons = parts->satellite.front();
                    if (cons == 'R') {   // GLONASS FDMA: no common-λ integer DD (spec OPEN-3)
                        if (!carry_glonass_warned) {
                            log->warn("Network[Card4]: GLONASS ambiguities are EXCLUDED from the "
                                      "invariant carry (FDMA — no common wavelength for an integer "
                                      "DD; IE-FDMA is a v2 item).  They keep their legacy treatment.");
                            carry_glonass_warned = true;
                        }
                        continue;
                    }
                    // The satellite phase-bias key is per-SATELLITE ("<sat>|<sig>|PB") — NOT
                    // the per-system receiver-bias key.  Only ambiguities backing an ungated
                    // shared phase bias (a vertex of the eq34 graph) become DD arcs.
                    // WL-2 §3.1: keep the constellation's EWL third band OUT of the carry
                    // (uncarried lane ⇒ no stiff prior ⇒ EWL rows win).  OFF ⇒ not taken.
                    if (ewl_carry_exclude && isUncarriedThirdBand(cons, parts->signal)) continue;
                    const std::string pbkey =
                        StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal);
                    if (!shared_phase_keys.count(pbkey)) continue;   // not an eq34 arc (gated/legacy lump)
                    const auto hit = h2l.find(static_cast<int>(sidx));
                    if (hit == h2l.end() || hit->second < 0) continue;   // unresolvable ⇒ chord refloats
                    // C3c slip feed: a PASS-A slip detection (MW wide-lane / LLI / IF-phase,
                    // recorded at the detector so the PASS-B pending-reseed clear can't hide
                    // it) or a within-epoch RAIM culprit (§3) invalidates this arc's carried
                    // DD — re-tap it fresh this epoch instead of folding a stiff pre-slip prior.
                    auto ambSlipped = [&](const std::string& amb_key) -> bool {
                        if (carry_slipped_ambs.count({static_cast<int>(i), amb_key})) return true;
                        if (carry_we_drop_sats
                            && carry_we_drop_sats->count(parts->satellite)) return true;
                        if (carry_we_drop_arcs) {
                            const auto dit = carry_we_drop_arcs->find(static_cast<int>(i));
                            if (dit != carry_we_drop_arcs->end()
                                && dit->second.count(parts->satellite)) return true;
                        }
                        return false;
                    };
                    int& sv = carry_sat_vertex[parts->satellite];
                    if (sv == 0) sv = kSatBase + static_cast<int>(carry_sat_vertex.size());

                    // --- WL-3 pair routing (SPEC_lane_carry_card.md §2): a primary-pair
                    // ambiguity whose partner band ALSO passes every per-band test (eq34-
                    // shared pbkey + resolvable column) joins the constellation's PAIR
                    // group (emitted once, from the band-1 iteration, with both columns);
                    // otherwise it falls through to the residual per-band group as today.
                    if (lane_on) {
                        const auto pit = primary.find({ static_cast<int>(i), parts->satellite });
                        const PP* pp = (pit != primary.end() && pit->second.ok)
                                           ? &pit->second : nullptr;
                        // Only the constellation's census-preferred pair joins the pair
                        // group — a minority-channel (rx,sat) keeps today's per-band carry
                        // (its cycles survive per band instead of spawning a flickering
                        // micro pair-group), and the EWL third band never pairs.
                        // v2 (uwl_family_pairs): additionally, a LATCHED minority-family
                        // pair routes into its OWN pair group (the group key is already
                        // the pair string, so families stay disjoint graphs).
                        const auto lpp = lane_pref_pair.find(cons);
                        bool pair_admissible =
                            pp && lpp != lane_pref_pair.end()
                            && pp->sig1 == lpp->second.first && pp->sig2 == lpp->second.second;
                        if (!pair_admissible && uwl_family_on && pp) {
                            const auto fit = lane_admit_pairs.find(cons);
                            pair_admissible = fit != lane_admit_pairs.end()
                                && fit->second.count({ pp->sig1, pp->sig2 }) != 0;
                        }
                        if (pair_admissible
                            && (parts->signal == pp->sig1 || parts->signal == pp->sig2)) {
                            const bool is1 = parts->signal == pp->sig1;
                            const std::string& other_sig = is1 ? pp->sig2 : pp->sig1;
                            const int other_col          = is1 ? pp->col2 : pp->col1;
                            const bool other_ok = other_col >= 0
                                && shared_phase_keys.count(
                                       StateKeys::makeSatPhaseBiasKey(parts->satellite, other_sig));
                            if (other_ok) {
                                if (!is1) continue;   // the pair arc was/will be emitted from band-1
                                int& pgk = carry_group_id[{cons, pp->sig1 + "+" + pp->sig2}];
                                if (pgk == 0) pgk = static_cast<int>(carry_group_id.size());
                                auto& G = groups[pgk];
                                G.key = pgk;
                                if (!G.pair) {
                                    G.pair    = true;
                                    G.lambda1 = lam(parts->satellite, pp->sig1);
                                    G.lambda2 = lam(parts->satellite, pp->sig2);
                                }
                                const std::string key2 =
                                    StateKeys::makeAmbiguityKey(parts->satellite, pp->sig2);
                                NetworkInvariantCarryDriver::ArcInput a;
                                a.rx_vertex   = static_cast<int>(i);
                                a.sat_vertex  = sv;
                                a.tree_edge   = stations[i].pinned_amb_targets.count(key) != 0;
                                a.est_station = est_index_of[i];
                                a.local_col   = static_cast<int>(hit->second);
                                a.local_col2  = pp->col2;
                                // Per-band slip states; the driver unions them under the LANE
                                // shape (§3.4 — either band refloats both) and keeps them
                                // per-band under the §6.1 equivalence shape (exactly the two
                                // per-band groups' behavior).
                                a.slipped     = ambSlipped(key);
                                a.slipped2    = ambSlipped(key2);
                                // Diagnostic: band-1's eq34 tree tag governs the pair basis;
                                // count disagreements with band-2's election (a mismatch means
                                // the two per-band trees differ on the shared graph, so the
                                // §6.1 equivalence leg is not exact for this component).
                                if (a.tree_edge
                                    != (stations[i].pinned_amb_targets.count(key2) != 0))
                                    ++lane_pair_tree_mismatch;
                                ++lane_pair_arcs;
                                G.arcs.push_back(a);
                                if (carry_innov_gate) {
                                    carry_arc_amb_key[pgk][ArcKey(static_cast<int>(i), sv)]  = key;
                                    carry_arc_amb_key2[pgk][ArcKey(static_cast<int>(i), sv)] = key2;
                                }
                                continue;
                            }
                            ++lane_resid_arcs;   // partner missing/unshared → residual per-band group
                        }
                    }

                    int& gk = carry_group_id[{cons, parts->signal}];
                    if (gk == 0) gk = static_cast<int>(carry_group_id.size());
                    NetworkInvariantCarryDriver::ArcInput a;
                    a.rx_vertex   = static_cast<int>(i);
                    a.sat_vertex  = sv;
                    a.tree_edge   = stations[i].pinned_amb_targets.count(key) != 0;
                    a.est_station = est_index_of[i];
                    a.local_col   = static_cast<int>(hit->second);
                    a.slipped     = ambSlipped(key);
                    groups[gk].key = gk;
                    groups[gk].arcs.push_back(a);
                    // D5: record chord arcs for the innovation-flag reverse lookup (tree edges
                    // never carry a DD, so they cannot be flagged — but record all; harmless).
                    if (carry_innov_gate)
                        carry_arc_amb_key[gk][ArcKey(static_cast<int>(i), sv)] = key;
                }
            }
            std::vector<NetworkInvariantCarryDriver::GroupInput> glist;
            glist.reserve(groups.size());
            for (auto& [k, g] : groups) glist.push_back(std::move(g));
            auto blocks = carry_driver.buildDdPriors(glist);
            // UWL-1 group census (once per run, log-only): which carry groups exist and
            // which are PAIR groups — the WL-3 fragmentation pre-flight (SPEC §8.1).
            if (uwl_on && lane_census_logged.insert('#').second)
                for (const auto& [gk, gid] : carry_group_id) {
                    std::size_t n_arcs = 0; bool is_pair = false;
                    for (const auto& gi : glist)
                        if (gi.key == gid) { n_arcs = gi.arcs.size(); is_pair = gi.pair; break; }
                    log->info("Network (M5/UWL-1 group census): group {} = cons '{}' '{}'"
                              "{} ({} arc(s) this epoch).",
                              gid, gk.first, gk.second, is_pair ? " [PAIR]" : "", n_arcs);
                }

            // --- M5 card WL-1: append the MW-seeded wide-lane pseudo-obs blocks. ---
            // Ride the carry's chord basis: each carry block row is a fundamental cycle on
            // ONE band's ambiguity columns; the WL layer pairs each cycle arc to its L2
            // ambiguity column and folds Σ c·MW̄/λ_WL as a rank-K constraint on the
            // cross-band columns.  Appended AFTER the carry blocks so their indices [0,Nc)
            // stay valid for installPosterior (which reads only its own Nc blocks).
            if (mw_mach_on) {
                // Both lanes read the PURE carry blocks (not each other's appended rows), so
                // collect their outputs here and append after both build.  The WL bridge lane
                // adds a third block set (integer-hard hold rows on the (1,2) columns).
                std::vector<NetworkNormalEquations::DdPriorBlock> wl_out, ewl_out, wl_hold_out;
                std::vector<NetworkMwWlLayer::FixedEwlRow> ewl_fixed;   // WL-2 §3.4 bridge input
                // Increment 5: per-entry top-rung tag, parallel to ewl_fixed ('\0' on the
                // legacy single lane; the lane's b_top / b_top2 digit under the UWL card).
                std::vector<char> ewl_fixed_top;
              if (mw_wl_on) {   // --- WL-1 float lane (primary L1/L2 pair) ---
                mw_wl_epoch = NetworkMwWlLayer::Counters{};   // per-build; the last build before the accepted solve wins
                auto resolver = [&](std::size_t est_station, Eigen::Index local_col,
                                    NetworkMwWlLayer::ArcData& out) -> bool {
                    const auto rit = revmap.find(
                        { static_cast<int>(est_station), static_cast<int>(local_col) });
                    if (rit == revmap.end()) return false;
                    const Rev& rv = rit->second;
                    const auto pit = primary.find({ rv.rx, rv.sat });
                    const PP* pp = (pit != primary.end()) ? &pit->second : nullptr;
                    if (pp && pp->ok && rv.signal == pp->sig1
                        && static_cast<int>(local_col) == pp->col1) {
                        out.is_primary_sig1 = true;
                        out.col1    = pp->col1;
                        out.col2    = pp->col2;
                        out.lambda1 = lam(rv.sat, pp->sig1);
                        out.lambda2 = lam(rv.sat, pp->sig2);
                        MwSlipDetector::MwArcZdMean zd;
                        if (stations[rv.rx].mw_slip)
                            zd = stations[rv.rx].mw_slip->arcZdMean(rv.sat, pp->sig1, pp->sig2);
                        out.have_mw       = zd.valid;
                        out.mw_zd_mean_m  = zd.mean_m;
                        out.mw_var_m2     = zd.var_of_mean_m2;
                        out.mw_count      = static_cast<long long>(zd.count);
                        out.lambda_wl     = zd.lambda_wl;
                        out.bia_corrected = zd.bia_corrected;   // frame flag (mixed-frame cycle guard)
                        {   // correction-frame re-expression inputs (F5)
                            const auto l1it = lin_of.find({ static_cast<int>(est_station),
                                                            static_cast<int>(pp->col1) });
                            const auto l2it = lin_of.find({ static_cast<int>(est_station),
                                                            static_cast<int>(pp->col2) });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) {
                                out.have_mw = false;   // no linearization value ⇒ cannot frame the row
                            } else {
                                out.x_lin1 = l1it->second;
                                out.x_lin2 = l2it->second;
                            }
                        }
                    } else {
                        // Not the primary L1 signal: a secondary/other-band carry group
                        // (→ skip_not_primary) or a single-band band-1 arc lacking L2
                        // (is_primary_sig1 true + col2=-1 → skip_missing_band).
                        out.is_primary_sig1 =
                            (!pp || !pp->ok) && (rv.signal.size() >= 2 && rv.signal[1] == '1');
                        out.col1    = local_col;
                        out.col2    = -1;
                        out.have_mw = false;
                    }
                    return true;
                };
                auto wl_blocks = mw_wl_layer.build(blocks, resolver, mw_wl_epoch);
                // --- F6 attribution capture: the FIRST arming epoch's WL rows (log-only). ---
                // Re-resolve each cycle arc (resolver + revmap in scope here) to record the
                // per-arc MW̄/x_lin and the row aggregates z_MW/z_lin/z_corr; the post-solve
                // site adds reconstructed_abs + the kernel innovation.  The last build before
                // the accepted solve wins (tow-matched at the write site).
                if (mw_wl_f6_dump && !mw_wl_f6.written && !wl_blocks.empty()) {
                    const std::size_t Nc = blocks.size();      // WL blocks append AFTER the carry blocks
                    const auto& wb = wl_blocks.front();        // first carry component's WL block
                    mw_wl_f6.rows.clear();
                    mw_wl_f6.week           = se.time.week;
                    mw_wl_f6.tow            = se.time.tow;
                    mw_wl_f6.loop_epoch     = static_cast<int>(n_loop_epochs);
                    mw_wl_f6.wl_block_index = Nc;              // first WL block's index in `blocks`
                    const std::size_t nrow = std::min<std::size_t>(wb.rows.size(), std::size_t{8});
                    for (std::size_t r = 0; r < nrow; ++r) {
                        const auto& terms = wb.rows[r];
                        F6Row fr;
                        fr.zhat_block = (static_cast<Eigen::Index>(r) < wb.zhat.size())
                                            ? wb.zhat(static_cast<Eigen::Index>(r)) : 0.0;
                        for (std::size_t t = 0; t + 1 < terms.size(); t += 2) {
                            const auto& tL1 = terms[t];       // rows are [L1_0,L2_0, L1_1,L2_1, ...]
                            NetworkMwWlLayer::ArcData ad;
                            if (!resolver(tL1.station, tL1.local_col, ad)) continue;
                            F6Arc a;
                            a.est_station   = static_cast<int>(tL1.station);
                            a.col1          = static_cast<int>(ad.col1);
                            a.col2          = static_cast<int>(ad.col2);
                            a.coeff         = (ad.lambda1 > 0.0) ? tL1.coeff * ad.lambda1 : tL1.coeff;
                            a.mw_mean_m     = ad.mw_zd_mean_m;
                            a.mw_var_m2     = ad.mw_var_m2;
                            a.mw_count      = ad.mw_count;
                            a.bia_corrected = ad.bia_corrected;
                            a.x_lin1        = ad.x_lin1;
                            a.x_lin2        = ad.x_lin2;
                            a.lambda1       = ad.lambda1;
                            a.lambda2       = ad.lambda2;
                            a.lambda_wl     = ad.lambda_wl;
                            const auto r1 = revmap.find({ a.est_station, a.col1 });
                            if (r1 != revmap.end()) { a.sat = r1->second.sat; a.sig1 = r1->second.signal; }
                            const auto r2 = revmap.find({ a.est_station, a.col2 });
                            if (r2 != revmap.end()) a.sig2 = r2->second.signal;
                            if (a.lambda_wl > 0.0) fr.z_mw += a.coeff * a.mw_mean_m / a.lambda_wl;
                            if (a.lambda1 > 0.0 && a.lambda2 > 0.0)
                                fr.z_lin += (a.coeff / a.lambda1) * a.x_lin1
                                          - (a.coeff / a.lambda2) * a.x_lin2;
                            fr.arcs.push_back(std::move(a));
                        }
                        fr.z_corr = fr.z_mw - fr.z_lin;
                        mw_wl_f6.rows.push_back(std::move(fr));
                    }
                    mw_wl_f6.pending = !mw_wl_f6.rows.empty();
                }
                wl_out = std::move(wl_blocks);
              }  // --- end WL-1 float lane ---

              if (ewl_on) {   // --- WL-2 EWL lane: (band2, band-top) pair, FFR statistic-gated fix ---
                for (auto& lane : ewl_lanes) lane.epoch = NetworkMwWlLayer::Counters{};
                // Same eligibility as WL-1 (a primary-sig1 cycle), but the row is the top-tier
                // DD on the SAME (rx,sat) arcs, pairing the estimator's primary sig2 (channel
                // discipline) with the constellation's top band: GPS EWL (L2,L5); Galileo UWL
                // (E5b|E5,E5a) under the armed card.  (hi,lo) is ordered by ACTUAL frequency —
                // GPS's shared band is the pair's HIGH side, Galileo's its LOW side — so the
                // MW statistic keys (sat, hi, lo) and λ_top stays positive (UWL-1 §2).  The
                // top band is uncarried (excluded above) so the rows win with no fight.
                // Increment 5: the lane instance under construction selects which top-rung
                // map its resolver reads — band_top_of for the preferred (and every
                // single-top) instance, band_top2_of for the fallback instance.  Set per
                // lane in the build loop below; single-top shapes never repoint it.
                const auto* ewl_top_map = &band_top_of;
                auto ewl_resolver = [&](std::size_t est_station, Eigen::Index local_col,
                                        NetworkMwWlLayer::ArcData& out) -> bool {
                    const auto rit = revmap.find(
                        { static_cast<int>(est_station), static_cast<int>(local_col) });
                    if (rit == revmap.end()) return false;
                    const Rev& rv = rit->second;
                    out.cons = rv.sat.empty() ? '?' : rv.sat.front();
                    const auto pit = primary.find({ rv.rx, rv.sat });
                    const PP* pp = (pit != primary.end()) ? &pit->second : nullptr;
                    if (pp && pp->ok && rv.signal == pp->sig1
                        && static_cast<int>(local_col) == pp->col1) {
                        out.is_primary_sig1 = true;
                        const auto b5 = ewl_top_map->find({ rv.rx, rv.sat });
                        if (b5 == ewl_top_map->end()) { out.col2 = -1; out.have_mw = false; return true; }
                        const std::string& sig_m = pp->sig2;           // shared band = primary sig2
                        const int          col_m = pp->col2;
                        const std::string& sig_t = b5->second.first;   // top-tier partner band
                        const int          col_t = b5->second.second;
                        const double lam_m = lam(rv.sat, sig_m);
                        const double lam_t = lam(rv.sat, sig_t);
                        // (hi, lo) by actual frequency (smaller λ = higher f).  GPS: L2 > L5
                        // reproduces the legacy assignment bit-exactly.
                        const bool m_hi = lam_m > 0.0 && lam_t > 0.0 && lam_m < lam_t;
                        const std::string& sig_hi = m_hi ? sig_m : sig_t;
                        const std::string& sig_lo = m_hi ? sig_t : sig_m;
                        const int col_hi = m_hi ? col_m : col_t;
                        const int col_lo = m_hi ? col_t : col_m;
                        out.col1    = col_hi;                          // high-frequency side
                        out.col2    = col_lo;                          // low-frequency side
                        out.lambda1 = m_hi ? lam_m : lam_t;
                        out.lambda2 = m_hi ? lam_t : lam_m;
                        MwSlipDetector::MwArcZdMean zd;
                        if (stations[rv.rx].mw_slip)                    // (higher, lower)-freq order
                            zd = stations[rv.rx].mw_slip->arcZdMean(rv.sat, sig_hi, sig_lo);
                        out.have_mw       = zd.valid;
                        out.mw_cold       = false;
                        out.mw_zd_mean_m  = zd.mean_m;
                        out.mw_var_m2     = zd.var_of_mean_m2;
                        out.mw_count      = static_cast<long long>(zd.count);
                        out.lambda_wl     = zd.lambda_wl;              // = λ_top = c/(f_hi − f_lo)
                        out.bia_corrected = zd.bia_corrected;
                        // UWL-1 §3.3 cold fix: when the LAGGED statistic is immature, serve the
                        // UNLAGGED n≥1 cold read — DECISION-ONLY, and only on tiers whose λ
                        // earns it (the floor excludes GPS's 5.861 m EWL).  At n=1 the Welford
                        // variance does not exist; substitute the a-priori per-sample σ.
                        if (uwl_cold_on && stations[rv.rx].mw_slip
                            && (!zd.valid
                                || zd.count < static_cast<std::size_t>(ewl_cfg.min_arc_epochs))) {
                            const auto zc = stations[rv.rx].mw_slip->arcZdMeanCold(
                                                rv.sat, sig_hi, sig_lo);
                            if (zc.valid && zc.lambda_wl >= nmw.uwl_cold_min_lambda_m) {
                                out.have_mw       = true;
                                out.mw_cold       = true;
                                out.mw_zd_mean_m  = zc.mean_m;
                                out.mw_var_m2     = std::isfinite(zc.var_of_mean_m2)
                                                        ? zc.var_of_mean_m2
                                                        : nmw.uwl_cold_sigma_m * nmw.uwl_cold_sigma_m;
                                out.mw_count      = static_cast<long long>(zc.count);
                                out.lambda_wl     = zc.lambda_wl;
                                out.bia_corrected = zc.bia_corrected;
                            }
                        }
                        const auto lmit = lin_of.find({ static_cast<int>(est_station), col_hi });
                        const auto ltit = lin_of.find({ static_cast<int>(est_station), col_lo });
                        if (lmit == lin_of.end() || ltit == lin_of.end()) out.have_mw = false;
                        else { out.x_lin1 = lmit->second; out.x_lin2 = ltit->second; }
                    } else {
                        out.is_primary_sig1 =
                            (!pp || !pp->ok) && (rv.signal.size() >= 2 && rv.signal[1] == '1');
                        out.col1    = local_col;
                        out.col2    = -1;
                        out.have_mw = false;
                    }
                    return true;
                };
                ewl_out.clear();
                ewl_fixed.clear();
                ewl_fixed_top.clear();
                for (auto& lane : ewl_lanes) {
                    // Increment 5: the fallback instance ('7' under a dual tier) resolves
                    // against band_top2_of; everything else keeps band_top_of.  The tag
                    // rides along per fixed row so the WL walk reads the SAME rung's
                    // bridge/cold arcs the decision came from.
                    const bool is_top2 = uwl_dual_on && lane.top != '\0'
                        && [&] {
                               const auto t = mwLaneTierOf(lane.cons, nmw.uwl_gal_top_band,
                                                           nmw.uwl_mid_enable, nmw.uwl_dual_top);
                               return t && t->b_top2 != '\0' && lane.top == t->b_top2;
                           }();
                    ewl_top_map = is_top2 ? &band_top2_of : &band_top_of;
                    std::vector<NetworkMwWlLayer::FixedEwlRow> lane_fixed;
                    auto lane_blocks = lane.layer.build(blocks, ewl_resolver, lane.epoch, &lane_fixed);
                    for (auto& b : lane_blocks) ewl_out.push_back(std::move(b));
                    ewl_fixed.insert(ewl_fixed.end(), lane_fixed.begin(), lane_fixed.end());
                    ewl_fixed_top.insert(ewl_fixed_top.end(), lane_fixed.size(), lane.top);
                }
                ewl_top_map = &band_top_of;   // leave the pointer in its legacy state
                if (mw_d5_dump) {   // F-A dump: the (station,sat) pairs EWL-FIXED cycles touch
                    d5_ewl_stasat.clear();
                    for (const auto& fx : ewl_fixed)
                        for (const auto& t : blocks[fx.block].rows[fx.row]) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit != revmap.end())
                                d5_ewl_stasat.insert({ rit->second.rx, rit->second.sat });
                        }
                }
              }  // --- end WL-2 EWL lane ---

              // Per-build reset (the last build before the accepted solve wins; RAIM re-solve safe).
              if (wl_inject_on || lane_on) {
                  wl_inject_epoch.clear();
                  wl_epoch = WlCounters{};
                  wl_cons_epoch.clear();   // UWL-1 per-cons walk tallies
              }
              if (mw_tagsets) d5_hold_chords.clear();

              if (lane_meas_on) {
                // --- WL-3 §3.2: per-epoch MW(1,2) lane increments on EVERY eligible pair
                // chord — the carried block becomes the code-coherence accumulator.  Each
                // arc contributes its detector-LAGGED raw sample exactly once (the Welford
                // mean stays decision-side); a cycle with an immature/stale/mixed-frame arc,
                // a this-epoch slip (its lagged sample is pre-slip), or a quarantined
                // linearization is skipped for the epoch.  Serial in (block, chord) order —
                // the §8.2 deterministic seam (runs before the bridge/fix walk below).
                constexpr double kLaneMwSigmaFloorCyc = 0.2;   // young-arc Welford collapse guard
                // LF-1 INC-4 (§3.5): each joint block's MW folds are independent (no shared Pᶻ)
                // and stay in-order within the block, so fanning OVER blocks is BYTE-IDENTICAL to
                // serial — per-block counter deltas sum order-independently, and Eigen is re-pinned
                // to 1 inside the region (the pcarry un-pin stays OUTSIDE — the dev300s nesting
                // trap).  The per-op driver timer is silenced for the parallel pass (it would race
                // a single double); the app-side row counts stay exact via the per-block deltas.
                const long long mw_nb = static_cast<long long>(carry_driver.numBlocks());
                std::vector<WlCounters> mw_pb(static_cast<std::size_t>(std::max<long long>(0, mw_nb)));
                const bool mw_par        = lane_parallel_components && n_threads > 1;
                const int  mw_eigen_save = Eigen::nbThreads();
                const bool mw_perf_save  = carry_driver.perfTiming();
                if (mw_par) { Eigen::setNbThreads(1); carry_driver.setPerfTiming(false); }
                #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(mw_par)
                for (long long bi = 0; bi < mw_nb; ++bi) {
                    const std::size_t b = static_cast<std::size_t>(bi);
                    if (carry_driver.blockPairBand(b) != 3) continue;
                    WlCounters& mwc = mw_pb[b];   // this block's counter delta (merged serially below)
                    const auto& chords_b = carry_driver.blockChordsForTest(b);
                    for (std::size_t k = 0; k < chords_b.size(); ++k) {
                        if (k >= blocks[b].rows.size()) break;
                        const auto& terms = blocks[b].rows[k];   // band-1 cycle terms
                        if (terms.empty()) continue;
                        // LF-1 INC-2: a committed-pinned lane (σ=0.01) does not move under an MW
                        // increment (σ≈0.06-0.2) — skip the O(K²) downdate.  No-op when OFF.
                        if (carry_driver.laneIncrementSkippable(b, k)) {
                            ++mwc.lane_pinned_skips; continue;
                        }
                        double z_mw_m = 0.0, R_m2 = 0.0, z_lin_wl = 0.0, lam_wl = 0.0;
                        double stamp = 0.0;
                        bool   have_stamp = false, ok = true;
                        int    frame = -1;   // uniform bia_corrected frame guard
                        for (const auto& t : terms) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit == revmap.end()) { ok = false; break; }
                            const Rev& rv = rit->second;
                            const auto pit = primary.find({ rv.rx, rv.sat });
                            if (pit == primary.end() || !pit->second.ok
                                || rv.signal != pit->second.sig1
                                || static_cast<int>(t.local_col) != pit->second.col1) {
                                ok = false; break;
                            }
                            const PP& pp = pit->second;
                            // A slip re-tapped this chord this epoch — the lagged (pre-slip)
                            // sample would poison the fresh lane; skip for one epoch.
                            bool slipped = carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig1) })
                                        || carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig2) });
                            if (!slipped && carry_we_drop_sats
                                && carry_we_drop_sats->count(rv.sat)) slipped = true;
                            if (!slipped && carry_we_drop_arcs) {
                                const auto dit = carry_we_drop_arcs->find(rv.rx);
                                if (dit != carry_we_drop_arcs->end()
                                    && dit->second.count(rv.sat)) slipped = true;
                            }
                            if (slipped) { ok = false; break; }
                            MwSlipDetector::MwArcZdLast zl;
                            if (stations[rv.rx].mw_slip)
                                zl = stations[rv.rx].mw_slip->arcZdLast(rv.sat, pp.sig1, pp.sig2);
                            if (!zl.valid || !(zl.lambda_wl > 0.0)) { ok = false; break; }
                            if (frame < 0) frame = zl.bia_corrected ? 1 : 0;
                            else if (frame != (zl.bia_corrected ? 1 : 0)) { ok = false; break; }
                            if (!have_stamp) { stamp = zl.epoch_s; have_stamp = true; }
                            else if (std::abs(zl.epoch_s - stamp) > 0.1) { ok = false; break; }
                            const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                            const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                            if (nmw.lane_max_abs_xlin_m > 0.0 &&
                                (std::abs(l1it->second) > nmw.lane_max_abs_xlin_m ||
                                 std::abs(l2it->second) > nmw.lane_max_abs_xlin_m)) { ok = false; break; }
                            const double la1 = lam(rv.sat, pp.sig1), la2 = lam(rv.sat, pp.sig2);
                            if (!(la1 > 0.0 && la2 > 0.0)) { ok = false; break; }
                            z_mw_m   += t.coeff * zl.value_m;
                            R_m2     += t.coeff * t.coeff * zl.var_m2;
                            z_lin_wl += (t.coeff / la1) * l1it->second
                                      - (t.coeff / la2) * l2it->second;
                            lam_wl = zl.lambda_wl;
                        }
                        if (!ok || !(lam_wl > 0.0)) { ++mwc.lane_skip_meas; continue; }
                        const double y = z_mw_m / lam_wl - z_lin_wl;
                        const double R = std::max(R_m2 / (lam_wl * lam_wl),
                                                  kLaneMwSigmaFloorCyc * kLaneMwSigmaFloorCyc);
                        if (carry_driver.applyLaneMeasurement(blocks, b, k, y, R))
                            ++mwc.lane_mw_rows;
                        else
                            ++mwc.lane_skip_meas;
                    }
                }
                if (mw_par) { Eigen::setNbThreads(mw_eigen_save); carry_driver.setPerfTiming(mw_perf_save); }
                for (const auto& d : mw_pb) {   // deterministic serial merge (order-independent sums)
                    wl_epoch.lane_pinned_skips += d.lane_pinned_skips;
                    wl_epoch.lane_skip_meas    += d.lane_skip_meas;
                    wl_epoch.lane_mw_rows      += d.lane_mw_rows;
                }
              }

              if ((wl_inject_on || lane_fix_on || lane_bridge_on) && !ewl_fixed.empty()) {
                // --- WL-2 WL lane: phase bridge → WL integer → hold row + inject plan (§3.3/§3.4) ---
                // For each FFR-fixed EWL cycle, bridge phase-only from the fixed N_EWL to a fast
                // WL integer w = [Σc·GF_b + λ_EWL·N_EWL]/λ_WL, cross-check the plain MW(1,2), and
                // either FOLD an integer-hard hold row (carry already lane-consistent ⇒ innovation
                // ~0, no fight) or STASH an inject (carry moves to w post-solve, NL preserved).
                // sat_vertex → satellite label (reverse of carry_sat_vertex) to map a chord's
                // ArcKey.v back to its satellite (for the primary-pair + group-id lookups).
                std::map<int, std::string> sat_of_vertex;
                for (const auto& [s, v] : carry_sat_vertex) sat_of_vertex[v] = s;
                // Group hold rows by their source carry block so shared (1,2) columns fold jointly.
                std::map<std::size_t, std::size_t> hold_block_of;   // carry block b → wl_hold_out index
                // Increment 5 (uwl_dual_top): a (block,row) decided by the PREFERRED rung
                // is consumed — the fallback rung's entry for the same row dedupes (the
                // preferred instance's entries precede it in ewl_fixed by construction).
                // A preferred entry that FAILS a gate does NOT consume, so the fallback
                // proceeds the same epoch (including a sibling-gate block — the demotion
                // is free).  The lane bridge increment is separately once-per-row: both
                // rungs' bridges measure the same lane and must not double-fold.
                std::set<std::pair<std::size_t, std::size_t>> walk_decided, lane_incr_done;

                for (std::size_t fxi = 0; fxi < ewl_fixed.size(); ++fxi) {
                    const auto& fx = ewl_fixed[fxi];
                    const char  fx_top = fxi < ewl_fixed_top.size() ? ewl_fixed_top[fxi] : '\0';
                    if (fx.block >= blocks.size() || fx.block >= carry_driver.numBlocks()) {
                        ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
                    }
                    const auto& chords_b = carry_driver.blockChordsForTest(fx.block);
                    if (fx.row >= chords_b.size() || fx.row >= blocks[fx.block].rows.size()) {
                        ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
                    }
                    const ArcKey chord = chords_b[fx.row];
                    // UWL-1: resolve the chord's constellation up front — the walk tallies
                    // per-cons under the armed card (wc == wl_epoch otherwise, bit-identical).
                    const auto svit0 = sat_of_vertex.find(chord.v);
                    const std::string sat_lbl =
                        svit0 != sat_of_vertex.end() ? svit0->second : std::string{};
                    const char ccons = sat_lbl.empty() ? '?' : sat_lbl.front();
                    WlCounters& wc = uwl_on ? wl_cons_epoch[ccons] : wl_epoch;
                    ++wc.rows_seen;
                    // Increment 5: resolve the chord constellation's tier once — which rung
                    // this entry came from, which top map its bridge reads, and whether an
                    // untaken sibling exists for the closure statistic.
                    const auto ctier = uwl_on
                        ? mwLaneTierOf(ccons, nmw.uwl_gal_top_band, nmw.uwl_mid_enable,
                                       nmw.uwl_dual_top)
                        : std::optional<MwLaneTier>{};
                    const bool fx_is_top2 = ctier && ctier->b_top2 != '\0'
                                            && fx_top == ctier->b_top2;
                    const bool dual_cons  = uwl_dual_on && ctier && ctier->b_top2 != '\0';
                    const auto& walk_top_map = fx_is_top2 ? band_top2_of : band_top_of;
                    const auto& sib_top_map  = fx_is_top2 ? band_top_of : band_top2_of;
                    if (dual_cons && walk_decided.count({ fx.block, fx.row })) {
                        ++wc.dual_dedupe;   // the preferred rung already decided this row
                        continue;
                    }
                    const auto&  terms = blocks[fx.block].rows[fx.row];   // the fundamental cycle
                    if (terms.empty()) continue;

                    // Walk the cycle arcs: accumulate the bridge DD, the MW(1,2) closure DD, and
                    // the per-band linearization DD (F5).  Bail on the first ineligible arc.
                    double sum_gf = 0.0, R_bridge = 0.0;          // Σc·GF_b (m), Σc²·var/λ_wl² (cyc²)
                    double z_mw12 = 0.0, R_mw12 = 0.0;            // plain MW(1,2) DD (cyc) + var
                    double z_lin1 = 0.0, z_lin2 = 0.0;            // Σc·x_lin per band (m)
                    double iono_dd = 0.0, iono_co = 0.0;         // §3.4 v2 de-bias: Σc·Î₁ (m), C_iono
                    double lam_wl = 0.0, lam_ewl = 0.0, lam1 = 0.0, lam2 = 0.0;
                    bool   ok = true, mw12_ok = true, quarantined = false, iono_ok = true;
                    // E6 mid hop (uwl_mid_enable): hop-hi (mid lane vs top lane) + hop-lo
                    // (WL lane vs mid lane) accumulators, plus the MW(6,5) closure for the
                    // intermediate integer.  mid_ok stays true only if EVERY arc serves both
                    // hop bridges mature (else the cycle falls back to the direct hop).
                    bool   mid_ok = uwl_mid_on;
                    double sum_gf_hi = 0.0, R_hi = 0.0;           // Σc·GF_hop-hi (m), Σc²·var/λ_mid² (cyc²)
                    double sum_gf_lo = 0.0, R_lo = 0.0;           // Σc·GF_hop-lo (m), Σc²·var/λ_wl² (cyc²)
                    double iono_co_hi = 0.0, iono_co_lo = 0.0, lam_mid = 0.0;
                    double z_mw65 = 0.0, R_mw65 = 0.0;            // MW(mid,b2) closure DD (mid cyc) + var
                    bool   mw65_ok = true;
                    // Increment 5 sibling closure: the untaken {7,8} rung's MW statistic as
                    // a DD over the same cycle.  Measured on every dual-tier decision; the
                    // hard gate is separate (uwl_sib_gate).  Any arc without the sibling
                    // band / a valid MW arc drops the check (counted sib_unavail).
                    bool   sib_ok = dual_cons;
                    double z_sib = 0.0, R_sib = 0.0, lam_sib = 0.0;
                    // WL-3 §3.2 bridge lane increment: the LAGGED RAW GF_b DD (once-only,
                    // phase-precision) accumulated alongside the Welford DECISION statistic.
                    double sum_gf_raw = 0.0, R_gf_raw = 0.0;      // Σc·GF_b_raw (m), Σc²·var/λ_wl² (cyc²)
                    double raw_stamp = 0.0;
                    bool   raw_ok = lane_bridge_on, raw_have_stamp = false;
                    bool   bridge_cold = false;   // UWL-1 §3.3: any arc served by the cold bridge
                    for (const auto& t : terms) {
                        const auto rit = revmap.find({ static_cast<int>(t.station),
                                                       static_cast<int>(t.local_col) });
                        if (rit == revmap.end()) { ok = false; break; }
                        const Rev& rv = rit->second;
                        const auto pit = primary.find({ rv.rx, rv.sat });
                        if (pit == primary.end() || !pit->second.ok) { ok = false; break; }
                        const PP& pp = pit->second;
                        const auto b5 = walk_top_map.find({ rv.rx, rv.sat });
                        if (b5 == walk_top_map.end()) { ok = false; break; }
                        auto brg = stations[rv.rx].mw_slip
                            ? stations[rv.rx].mw_slip->arcBridge(rv.sat, pp.sig1, pp.sig2, b5->second.first)
                            : MwSlipDetector::MwBridgeMean{};
                        const std::size_t brg_need =
                            static_cast<std::size_t>(std::max(2, ewl_cfg.min_arc_epochs));
                        if (uwl_cold_on && stations[rv.rx].mw_slip
                            && (!brg.valid || brg.count < brg_need)) {
                            // UWL-1 §3.3: post-slip the LAGGED bridge trails by one epoch; the
                            // UNLAGGED cold serve (n≥2) re-pins one epoch earlier.  DECISION-
                            // ONLY — the lane increment below stays on the lagged arcBridgeLast,
                            // and the λ floor keeps this a top-tier (Galileo) privilege.
                            const auto bc = stations[rv.rx].mw_slip->arcBridgeCold(
                                                rv.sat, pp.sig1, pp.sig2, b5->second.first);
                            if (bc.valid && bc.count >= brg_need
                                && bc.lambda_ewl >= nmw.uwl_cold_min_lambda_m) {
                                brg = bc;
                                bridge_cold = true;
                            }
                        }
                        if (!brg.valid || brg.count < brg_need) {
                            ok = false; break;
                        }
                        const double l1 = lam(rv.sat, pp.sig1), l2 = lam(rv.sat, pp.sig2);
                        if (!(l1 > 0.0 && l2 > 0.0 && brg.lambda_wl > 0.0 && brg.lambda_ewl > 0.0)) {
                            ok = false; break;
                        }
                        const double coeff = t.coeff;
                        sum_gf   += coeff * brg.gf_mean_m;
                        R_bridge += coeff * coeff * brg.var_of_mean_m2 / (brg.lambda_wl * brg.lambda_wl);
                        // E6 mid hop: read this arc's two hop bridges (lagged; the cold
                        // serve stays a direct-hop privilege) + the MW(mid,b2) closure.
                        // Any missing/immature piece drops the WHOLE cycle to the direct
                        // hop (λ_mid must be uniform across the cycle's arcs).
                        if (mid_ok) {
                            const auto b6 = band_mid_of.find({ rv.rx, rv.sat });
                            MwSlipDetector::MwBridgeMean bh, blo;
                            if (b6 != band_mid_of.end() && stations[rv.rx].mw_slip) {
                                bh  = stations[rv.rx].mw_slip->arcBridge(
                                          rv.sat, b6->second, pp.sig2, b5->second.first);
                                blo = stations[rv.rx].mw_slip->arcBridge(
                                          rv.sat, pp.sig1, pp.sig2, b6->second);
                            }
                            if (!bh.valid || bh.count < brg_need
                                || !blo.valid || blo.count < brg_need
                                || !(bh.lambda_wl > 0.0) || !(blo.lambda_ewl > 0.0)) {
                                mid_ok = false;
                            } else {
                                sum_gf_hi += coeff * bh.gf_mean_m;
                                R_hi      += coeff * coeff * bh.var_of_mean_m2
                                             / (bh.lambda_wl * bh.lambda_wl);
                                sum_gf_lo += coeff * blo.gf_mean_m;
                                R_lo      += coeff * coeff * blo.var_of_mean_m2
                                             / (blo.lambda_wl * blo.lambda_wl);
                                iono_co_hi = bh.iono_coeff;
                                iono_co_lo = blo.iono_coeff;
                                lam_mid    = bh.lambda_wl;   // == blo.lambda_ewl (CDMA-uniform)
                                if (mw65_ok) {   // MW(mid,b2) closure for the mid integer
                                    const auto mw65 = stations[rv.rx].mw_slip->arcZdMean(
                                                          rv.sat, b6->second, pp.sig2);
                                    if (mw65.valid && mw65.lambda_wl > 0.0) {
                                        z_mw65 += coeff * mw65.mean_m / mw65.lambda_wl;
                                        R_mw65 += coeff * coeff * mw65.var_of_mean_m2
                                                  / (mw65.lambda_wl * mw65.lambda_wl);
                                    } else {
                                        mw65_ok = false;
                                    }
                                }
                            }
                        }
                        // Increment 5: this arc's untaken-sibling MW read (7-fx → the '8'
                        // rung's (8,5) statistic, 8-fx → the finer (7,5) one).  Ordered
                        // (hi,lo) by actual frequency like every MW key.
                        if (sib_ok) {
                            const auto bs = sib_top_map.find({ rv.rx, rv.sat });
                            MwSlipDetector::MwArcZdMean mws;
                            if (bs != sib_top_map.end() && stations[rv.rx].mw_slip) {
                                const double lam_s = lam(rv.sat, bs->second.first);
                                const double lam_m = lam(rv.sat, pp.sig2);
                                const bool s_hi = lam_s > 0.0 && lam_m > 0.0 && lam_s < lam_m;
                                mws = stations[rv.rx].mw_slip->arcZdMean(
                                          rv.sat,
                                          s_hi ? bs->second.first : pp.sig2,
                                          s_hi ? pp.sig2 : bs->second.first);
                            }
                            if (mws.valid && mws.lambda_wl > 0.0) {
                                z_sib  += coeff * mws.mean_m / mws.lambda_wl;
                                R_sib  += coeff * coeff * mws.var_of_mean_m2
                                          / (mws.lambda_wl * mws.lambda_wl);
                                lam_sib = mws.lambda_wl;
                            } else {
                                sib_ok = false;
                            }
                        }
                        if (raw_ok) {   // WL-3: the raw lagged sample (skip on slip/stale/immature)
                            bool slipped = carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig1) })
                                        || carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig2) });
                            if (!slipped && carry_we_drop_sats
                                && carry_we_drop_sats->count(rv.sat)) slipped = true;
                            if (!slipped && carry_we_drop_arcs) {
                                const auto dit = carry_we_drop_arcs->find(rv.rx);
                                if (dit != carry_we_drop_arcs->end()
                                    && dit->second.count(rv.sat)) slipped = true;
                            }
                            MwSlipDetector::MwBridgeLast bl;
                            if (!slipped && stations[rv.rx].mw_slip)
                                bl = stations[rv.rx].mw_slip->arcBridgeLast(
                                         rv.sat, pp.sig1, pp.sig2, b5->second.first);
                            if (slipped || !bl.valid || !(bl.lambda_wl > 0.0)) {
                                raw_ok = false;
                            } else if (!raw_have_stamp) {
                                raw_stamp = bl.epoch_s; raw_have_stamp = true;
                            } else if (std::abs(bl.epoch_s - raw_stamp) > 0.1) {
                                raw_ok = false;   // mixed epochs — the DD is not any epoch's
                            }
                            if (raw_ok) {
                                sum_gf_raw += coeff * bl.gf_m;
                                R_gf_raw   += coeff * coeff * bl.var_m2
                                              / (bl.lambda_wl * bl.lambda_wl);
                            }
                        }
                        // §3.4 v2 iono de-bias: subtract the network's own slant-iono estimate DD.
                        // (We do NOT price the local MARGINAL iono variance into the σ-guard: it is
                        // dominated by the ABSOLUTE / common-mode iono uncertainty that CANCELS in a
                        // closed DD cycle, so Σc²·var grossly over-states the true DD-iono error and
                        // would gate the bridge past its ep~4-6 advantage.  The DD-iono ESTIMATE
                        // error — small, since common-mode cancels — surfaces directly in |frac| and
                        // is caught by the FFR gate; the iono-free MW(1,2) closure is the backstop.
                        // The exact joint-DD-iono covariance / iono-column fold is a v3 refinement.)
                        const auto iit = iono_of.find({ rv.rx, rv.sat });
                        if (iit == iono_of.end()) { iono_ok = false; break; }
                        iono_dd += coeff * iit->second;
                        const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                        const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                        if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                        z_lin1 += coeff * l1it->second;
                        z_lin2 += coeff * l2it->second;
                        if (nmw.lane_max_abs_xlin_m > 0.0 &&
                            (std::abs(l1it->second) > nmw.lane_max_abs_xlin_m ||
                             std::abs(l2it->second) > nmw.lane_max_abs_xlin_m)) { quarantined = true; break; }
                        // Plain MW(1,2) closure statistic (integrity cross-check, §3.4).
                        MwSlipDetector::MwArcZdMean mw = stations[rv.rx].mw_slip
                            ? stations[rv.rx].mw_slip->arcZdMean(rv.sat, pp.sig1, pp.sig2)
                            : MwSlipDetector::MwArcZdMean{};
                        if (mw.valid && mw.lambda_wl > 0.0) {
                            z_mw12 += coeff * mw.mean_m / mw.lambda_wl;
                            R_mw12 += coeff * coeff * mw.var_of_mean_m2 / (mw.lambda_wl * mw.lambda_wl);
                        } else {
                            mw12_ok = false;
                        }
                        lam_wl = brg.lambda_wl; lam_ewl = brg.lambda_ewl; lam1 = l1; lam2 = l2;
                        iono_co = brg.iono_coeff;
                    }
                    if (quarantined) { ++wc.skip_quarantine; continue; }
                    if (!ok || !(lam_wl > 0.0)) { ++wc.skip_bridge_missing; continue; }
                    if (!iono_ok)  { ++wc.skip_no_iono; continue; }  // no iono estimate ⇒ can't de-bias

                    // --- WL-3 §3.2: fold the per-epoch BRIDGE lane increment (raw lagged GF_b
                    // DD, iono-de-biased, honest per-sample R) — BEFORE the FFR decision gates,
                    // because the continuous lane tightening is wanted even on epochs where the
                    // integer decision is withheld.  Once-only: the raw sample enters here and
                    // nowhere else; the Welford mean below stays decision-side.
                    // Increment 5: both rungs' bridges measure the SAME lane — at most one
                    // bridge increment folds per (block,row) per build (a failed preferred
                    // attempt does not consume, so the fallback may still tighten).
                    if (lane_bridge_on
                        && !(dual_cons && lane_incr_done.count({ fx.block, fx.row }))
                        // LF-1 INC-2: a committed-pinned lane skips its bridge increment (no-op OFF).
                        && carry_driver.laneIncrementSkippable(fx.block, fx.row)) {
                        ++wc.lane_pinned_skips;
                    } else if (lane_bridge_on
                        && !(dual_cons && lane_incr_done.count({ fx.block, fx.row }))) {
                        constexpr double kLaneBridgeSigmaFloorCyc = 0.02;   // phase-precision floor
                        if (raw_ok) {
                            const double z_raw = (sum_gf_raw
                                                  - (bridge_debias_on ? 0.0 : iono_co * iono_dd)
                                                  + lam_ewl * static_cast<double>(fx.integer)) / lam_wl;
                            const double y_raw = z_raw - (z_lin1 / lam1 - z_lin2 / lam2);
                            const double R_raw = std::max(R_gf_raw,
                                kLaneBridgeSigmaFloorCyc * kLaneBridgeSigmaFloorCyc);
                            if (carry_driver.applyLaneMeasurement(blocks, fx.block, fx.row, y_raw, R_raw)) {
                                ++wc.lane_bridge_rows;
                                if (dual_cons) lane_incr_done.insert({ fx.block, fx.row });
                            } else {
                                ++wc.lane_skip_meas;
                            }
                        } else {
                            ++wc.lane_skip_meas;
                        }
                    }

                    // z_WL^bridge = [Σc·GF_b − C_iono·Σc·Î₁ + λ_EWL·N_EWL]/λ_WL (cyc); FFR on the
                    // iono-de-biased DETECTOR stat (decision source, never the posterior).  The
                    // de-bias removes the integer-valued differential-iono bias (the v1
                    // falsification); the residual DD-iono ESTIMATE error surfaces in |frac|.
                    const double sigma_direct = std::sqrt(std::max(R_bridge, 0.0));   // Welford phase noise
                    // Per-sample de-bias armed ⇒ sum_gf is already iono-free frame-by-
                    // frame; the read-time subtraction is DISABLED (never subtract twice).
                    const double z_wl_direct  = (sum_gf
                                                 - (bridge_debias_on ? 0.0 : iono_co * iono_dd)
                                                 + lam_ewl * static_cast<double>(fx.integer)) / lam_wl;

                    // --- E6 mid hop: chain top → mid → WL.  Hop-hi determines the mid-rung
                    // integer m = round([Σc·GF_hi − C_hi·Σc·Î₁ + λ_TOP·N_TOP]/λ_MID) under its
                    // own FFR gates + the MW(mid,b2) closure; hop-lo then re-expresses the WL
                    // statistic against the ROUNDED m — stripping the top hop's λ_TOP-scale
                    // phase/iono amplification (~3.3× smaller λ multiplier at λ_MID 2.931 m).
                    // Any failure falls back to the direct hop (counted, never silent).
                    double z_wl = z_wl_direct, sigma_bridge = sigma_direct;
                    bool   used_mid = false;
                    if (mid_ok && lam_mid > 0.0) {
                        const double sigma_hi = std::sqrt(std::max(R_hi, 0.0));
                        const double z_mid = (sum_gf_hi
                                              - (bridge_debias_on ? 0.0 : iono_co_hi * iono_dd)
                                              + lam_ewl * static_cast<double>(fx.integer)) / lam_mid;
                        const double m = std::round(z_mid);
                        bool mid_fix = sigma_hi < nmw.wl_fix_sigma_cyc
                                    && std::abs(z_mid - m) < nmw.wl_fix_frac_cyc;
                        if (mid_fix && mw65_ok) {
                            // The mid integer has its own free integrity check: the plain
                            // MW(mid,b2) DD must agree within its σ (invariant #1).
                            const double tol65 = 0.5 + 3.0 * std::sqrt(std::max(R_mw65, 0.0));
                            if (std::abs(z_mw65 - m) > tol65) { mid_fix = false; ++wc.mid_closure; }
                        } else if (!mid_fix) {
                            ++wc.mid_fallback;
                        }
                        if (mid_fix) {
                            z_wl = (sum_gf_lo
                                    - (bridge_debias_on ? 0.0 : iono_co_lo * iono_dd)
                                    + lam_mid * m) / lam_wl;
                            sigma_bridge = std::sqrt(std::max(R_lo, 0.0));
                            used_mid = true;
                        }
                    }

                    if (sigma_bridge > nmw.bridge_max_sigma_cyc) { ++wc.skip_bridge_wide; continue; }
                    const double w = std::round(z_wl);
                    if (!(sigma_bridge < nmw.wl_fix_sigma_cyc &&
                          std::abs(z_wl - w) < nmw.wl_fix_frac_cyc)) { ++wc.skip_not_fixed; continue; }
                    // Wrong-integer backstop: if the DIRECT hop is ALSO FFR-credible on its own
                    // and rounds to a different integer, decide neither (counted).
                    if (used_mid) {
                        const double wd = std::round(z_wl_direct);
                        if (sigma_direct < nmw.wl_fix_sigma_cyc
                            && std::abs(z_wl_direct - wd) < nmw.wl_fix_frac_cyc
                            && wd != w) { ++wc.mid_disagree; continue; }
                    }

                    // Closure: the plain MW(1,2) DD must agree with w within its own σ (free
                    // integrity check).  Blocked only when MW(1,2) is present AND clearly off.
                    // NOTE (UWL-1 §3.3): in a post-slip cold window the primary-pair MW arc has
                    // usually reset too (mw12_ok=false ⇒ not blocked) — a cold decision is
                    // frac-gated + D5-backstopped + re-decided idempotently as stats mature.
                    if (mw12_ok) {
                        const double tol = 0.5 + 3.0 * std::sqrt(std::max(R_mw12, 0.0));
                        if (std::abs(z_mw12 - w) > tol) { ++wc.skip_closure; continue; }
                    }

                    // --- Increment 5 sibling closure (§1b): the untaken rung's statistic
                    // must sit on N_top·λ_top/λ_sib.  8-fx → z(7,5) vs 2·N₈ (the FINER
                    // sibling: a ±1 error in N₈ moves the residual by 2 — strongly
                    // detectable); 7-fx → z(8,5) vs N₇/2 (an odd N₇ makes the target
                    // half-integer — the AltBOC-coherence parity, visible in |frac|).
                    // MEASURED on every dual decision; the hard gate blocks only a
                    // ≥1-sibling-cycle disagreement and DEMOTES the cycle to the fallback
                    // rung (the fallback entry follows in ewl_fixed and is not deduped,
                    // because a blocked decision never consumes the row).
                    if (dual_cons) {
                        if (sib_ok && lam_sib > 0.0 && lam_ewl > 0.0) {
                            ++wc.sib_checked;
                            const double expect =
                                static_cast<double>(fx.integer) * (lam_ewl / lam_sib);
                            const double r     = z_sib - expect;
                            const double rfrac = r - std::round(r);
                            wc.sib_abs_frac_sum += std::abs(rfrac);
                            if (std::llround(r) != 0) ++wc.sib_int_off;
                            if (uwl_sib_gate_on) {
                                const double tol_s =
                                    0.5 + 3.0 * std::sqrt(std::max(R_sib, 0.0));
                                if (std::abs(r) > tol_s) { ++wc.sib_blocked; continue; }
                            }
                        } else {
                            ++wc.sib_unavail;
                        }
                    }

                    ++wc.cycles_decided;
                    wc.sum_bridge_R += R_bridge;
                    if (bridge_cold) ++wc.cold_bridge_cycles;
                    if (used_mid)    ++wc.mid_decided;
                    if (fx_is_top2)  ++wc.top2_decided;
                    if (dual_cons)   walk_decided.insert({ fx.block, fx.row });

                    // Group ids for the chord's two carried bands (chord.v → sat → primary pair,
                    // resolved at the loop top).
                    if (sat_lbl.empty()) { ++wc.skip_basis; continue; }
                    const char cons = ccons;
                    const auto pcit = primary.find({ chord.u, sat_lbl });
                    if (pcit == primary.end() || !pcit->second.ok) { ++wc.skip_basis; continue; }

                    if (lane_fix_on) {
                        // --- WL-3 §3.3: the FFR fix on the PAIR block — an information-
                        // idempotent lane-coordinate SET applied every DECIDED epoch (recenter
                        // + recollapse; a one-shot fold-on-change measured out — the decided
                        // lane drifts back to MW quality under q + noisy increments), plus the
                        // pair inject-fallback plan (applied post-solve only if the carry still
                        // disagrees by > inject_disagree_min_cyc; ≈0 in steady state — the
                        // retirement telemetry proves it).
                        const auto pgit = carry_group_id.find(
                            { cons, pcit->second.sig1 + "+" + pcit->second.sig2 });
                        if (pgit == carry_group_id.end()) { ++wc.skip_basis; continue; }
                        const double z_lin_wl = z_lin1 / lam1 - z_lin2 / lam2;
                        const int rc = carry_driver.applyLaneFix(
                            blocks, fx.block, fx.row, static_cast<long long>(std::llround(w)),
                            w - z_lin_wl, nmw.fix_weight_sigma_cyc);
                        if (rc == 1) {
                            ++wc.lane_fix_folded;
                            if (mw_tagsets) d5_hold_chords.insert(chord);
                        } else if (rc == 0) {
                            ++wc.lane_fix_unchanged;
                        } else {
                            ++wc.skip_basis;   // not a pair chord (residual-group cycle)
                            continue;
                        }
                        WlInjectPlan pl;
                        pl.pair = true;
                        pl.group_l1 = pl.group_l2 = pgit->second;
                        pl.chord = chord;
                        pl.w = w;
                        pl.z_lin1 = z_lin1; pl.z_lin2 = z_lin2;
                        pl.lambda1 = lam1;  pl.lambda2 = lam2;
                        pl.cons = cons;
                        wl_inject_epoch.push_back(pl);
                        continue;
                    }

                    const auto g1it = carry_group_id.find({ cons, pcit->second.sig1 });
                    const auto g2it = carry_group_id.find({ cons, pcit->second.sig2 });
                    if (g1it == carry_group_id.end() || g2it == carry_group_id.end()) {
                        ++wc.skip_basis; continue;
                    }
                    const int group_l1 = g1it->second, group_l2 = g2it->second;

                    // Pre-solve carried lane value (for the hold-row gate): fold the integer-hard
                    // hold row ONLY when the carry already agrees (innovation ~0 ⇒ no F6 fight);
                    // otherwise skip it this epoch and let the post-solve inject move the value.
                    double z1p = 0.0, v1p = 0.0, z2p = 0.0, v2p = 0.0;
                    const bool have_l1 = carry_driver.carriedChordValue(group_l1, chord, z1p, v1p);
                    const bool have_l2 = carry_driver.carriedChordValue(group_l2, chord, z2p, v2p);
                    bool agree = false;
                    if (have_l1 && have_l2) {
                        const double A1 = (z1p + z_lin1) / lam1, A2 = (z2p + z_lin2) / lam2;
                        agree = std::abs(w - (A1 - A2)) <= nmw.inject_disagree_min_cyc;
                    }
                    if (agree) {
                        // Build/append the WL hold row (WL-cycle correction frame): +c/λ1 on col1,
                        // −c/λ2 on col2; zhat = w − Σ[(c/λ1)x_lin1 − (c/λ2)x_lin2]; Pz = fix_weight².
                        std::size_t ob;
                        auto hit = hold_block_of.find(fx.block);
                        if (hit == hold_block_of.end()) {
                            ob = wl_hold_out.size();
                            hold_block_of[fx.block] = ob;
                            wl_hold_out.emplace_back();
                        } else { ob = hit->second; }
                        std::vector<NetworkNormalEquations::DdPriorBlock::Term> hrow;
                        double z_lin_wl = 0.0;
                        hrow.reserve(terms.size() * 2);
                        bool hok = true;
                        for (const auto& t : terms) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit == revmap.end()) { hok = false; break; }
                            const auto pit = primary.find({ rit->second.rx, rit->second.sat });
                            if (pit == primary.end() || !pit->second.ok) { hok = false; break; }
                            const PP& pp = pit->second;
                            const double l1 = lam(rit->second.sat, pp.sig1);
                            const double l2 = lam(rit->second.sat, pp.sig2);
                            if (!(l1 > 0.0 && l2 > 0.0)) { hok = false; break; }
                            hrow.push_back({ t.station, pp.col1, +t.coeff / l1 });
                            hrow.push_back({ t.station, pp.col2, -t.coeff / l2 });
                            const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                            const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) { hok = false; break; }
                            z_lin_wl += (t.coeff / l1) * l1it->second - (t.coeff / l2) * l2it->second;
                        }
                        if (hok) {
                            auto& dst = wl_hold_out[ob];
                            dst.rows.push_back(std::move(hrow));
                            const Eigen::Index k = static_cast<Eigen::Index>(dst.rows.size());
                            dst.zhat.conservativeResize(k);
                            dst.zhat(k - 1) = w - z_lin_wl;
                            dst.Pz.conservativeResize(k, k);
                            dst.Pz.row(k - 1).setZero();
                            dst.Pz.col(k - 1).setZero();
                            dst.Pz(k - 1, k - 1) = nmw.fix_weight_sigma_cyc * nmw.fix_weight_sigma_cyc;
                            ++wc.holds_folded;
                            if (mw_tagsets) d5_hold_chords.insert(chord);
                        }
                    }
                    // Stash the inject plan (applied post-solve against the FRESH posterior).  The
                    // 1:1-basis requirement is re-checked there (both bands must carry the chord).
                    WlInjectPlan pl;
                    pl.group_l1 = group_l1; pl.group_l2 = group_l2; pl.chord = chord;
                    pl.w = w; pl.z_lin1 = z_lin1; pl.z_lin2 = z_lin2;
                    pl.lambda1 = lam1; pl.lambda2 = lam2;
                    pl.cons = cons;
                    wl_inject_epoch.push_back(pl);
                }
              }  // --- end WL-2 WL lane ---

              if (mw_lane_trace) {   // F-B: lane-trace entries for EVERY resolvable primary cycle
                lane_trace_epoch.clear();
                std::map<int, std::string> sat_of_v;
                for (const auto& [s, v] : carry_sat_vertex) sat_of_v[v] = s;
                for (std::size_t b = 0; b < blocks.size(); ++b) {   // pure carry blocks (pre-append)
                    const auto& chords_b = carry_driver.blockChordsForTest(b);
                    for (std::size_t r = 0; r < blocks[b].rows.size(); ++r) {
                        if (r >= chords_b.size()) break;
                        const auto& terms = blocks[b].rows[r];
                        if (terms.empty()) continue;
                        LaneTraceEntry e;
                        e.chord = chords_b[r];
                        bool ok = true;
                        for (const auto& t : terms) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit == revmap.end()) { ok = false; break; }
                            const auto pit = primary.find({ rit->second.rx, rit->second.sat });
                            if (pit == primary.end() || !pit->second.ok
                                || rit->second.signal != pit->second.sig1
                                || static_cast<int>(t.local_col) != pit->second.col1) { ok = false; break; }
                            const PP& pp = pit->second;
                            const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                            const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                            e.z_lin1 += t.coeff * l1it->second;
                            e.z_lin2 += t.coeff * l2it->second;
                            e.lam1 = lam(rit->second.sat, pp.sig1);
                            e.lam2 = lam(rit->second.sat, pp.sig2);
                        }
                        if (!ok || !(e.lam1 > 0.0 && e.lam2 > 0.0)) continue;
                        const auto svit = sat_of_v.find(e.chord.v);
                        if (svit == sat_of_v.end()) continue;
                        e.sat = svit->second;
                        const char cons = e.sat.empty() ? '?' : e.sat.front();
                        const auto pcit = primary.find({ e.chord.u, e.sat });
                        if (pcit == primary.end() || !pcit->second.ok) continue;
                        if (lane_on) {
                            // WL-3: both bands live in ONE pair group; the write site reads
                            // the joint block via carriedPairValue (g1 == g2 marks it).
                            const auto pgit = carry_group_id.find(
                                { cons, pcit->second.sig1 + "+" + pcit->second.sig2 });
                            if (pgit == carry_group_id.end()) continue;
                            e.g1 = e.g2 = pgit->second;
                        } else {
                            const auto g1it = carry_group_id.find({ cons, pcit->second.sig1 });
                            const auto g2it = carry_group_id.find({ cons, pcit->second.sig2 });
                            if (g1it == carry_group_id.end() || g2it == carry_group_id.end()) continue;
                            e.g1 = g1it->second; e.g2 = g2it->second;
                        }
                        lane_trace_epoch.push_back(std::move(e));
                    }
                }
              }
                for (auto& wb : wl_out)      blocks.push_back(std::move(wb));   // WL-1 first ([Nc, Nc+n_wl))
                if (nmw.ewl_fold_rows)                                          // F-A discriminant: false ⇒
                    for (auto& wb : ewl_out) blocks.push_back(std::move(wb));   // decide/expose but fold nothing
                for (auto& wb : wl_hold_out) blocks.push_back(std::move(wb));   // then WL hold rows
            }
            // LF-1 INC-1: under fold_blocked the lane primitives skipped their per-row
            // re-symmetrize; restore exact symmetry once per joint block now that this epoch's
            // MW / bridge / fix rows are all applied (no-op unless fold_blocked).  Runs on every
            // build incl. the RAIM re-solve — the blocks are rebuilt each pass.
            carry_driver.finalizeLaneBatch(blocks);
            est.setDdPriors(std::move(blocks));
        };
        subEnd(SB_GAUGEBUILD);   // D0b: close gauge build + carry-driver setup
        subBegin(SB_DDBUILD);    // D0b: carry_driver.beginEpoch + buildCarryDdPriors (W·Pᶻ·Wᵀ)
        if (carry_invariant) {
            // The z-store predict (q_z·Δt on the carried DD covariance) runs ONCE per
            // epoch; the build/fold may re-run on the RAIM re-solve.  D5 v2: the epoch time
            // (σ̂₀² reporting bucket) + clean flag (always clean here — under carry_invariant
            // the eq34 datum re-realizes freely, so there is no re-level storm to gate; the
            // robust geo-mean shrugs off the content-hr-4 event tail on its own).
            const double dt = std::isfinite(carry_prev_tow) ? (se.time.tow - carry_prev_tow) : 0.0;
            carry_driver.beginEpoch(dt > 0.0 ? dt : 0.0, se.time.tow, /*clean=*/true);
        }
        { CarryEigenScope es(carry_par, n_threads); buildCarryDdPriors(); }  // parallel_carry: thread W·Pᶻ·Wᵀ
        subEnd(SB_DDBUILD);      // D0b: close buildCarryDdPriors (the Card-4 DD-cov projection)
        subBegin(SB_SOLVEGLUE);  // D0b: pre-solve prep + clock predict + solve + within-epoch re-solve

        // W6 satellite code-bias datum rows relocated to
        // NetworkSbasis::buildCodeBiasDatumRows (INC-8) — full design rationale
        // (IF pin + per-(constellation,signal) sum-to-zero, Card-2 time-linking,
        // basis hold/latch/re-latch) in the method Doxygen.  The fold + the
        // RAIM-replay cap_* capture and the first-N summary stay HERE (writer-site
        // contract, NetworkRunTypes.hpp).
        // Reset the hold-OFF IF-pin selection scratch for THIS epoch (mirrors the
        // per-epoch this_epoch_anchor.clear()).  Unconditional — done even when the
        // code-bias block below is skipped (no registered keys) so a stale prior-epoch
        // selection can never reach the emit-site comparison.  A no-op on the empty
        // map in the default (emit_clock_disc OFF) config.
        this_epoch_if_pin_sel.clear();
        this_epoch_cb_ifpin.clear();   // Card 2: this epoch's IF-pin pairs (persisted at harvest)
        this_epoch_cb_reseed.clear();  // Card 2: this epoch's re-seeds (merged into the pending disc set at the emit site — withheld epochs never reach it)
        if (config_.estimateNetworkSatCodeBias() && !registered_code_keys.empty()) {
            // Carrier frequency of a (sat,signal) from any present station's model.
            auto freqOf = [&](const std::string& sat, const std::string& sig) -> double {
                for (size_t i = 0; i < stations.size(); ++i)
                    if (present[i]) {
                        const double f = stations[i].mm->carrierFrequency(sig, sat);
                        if (std::isfinite(f) && f > 0.0) return f;
                    }
                return 0.0;
            };

            const auto cbd = NetworkSbasis::buildCodeBiasDatumRows(
                est, registered_code_keys, freqOf,
                cb_time_linked, clock_signal_basis_hold, emit_clock_disc,
                code_bias_prev_, code_bias_ifpin_pair_,
                code_bias_sat_seen_, code_bias_datum_resumed_,
                held_clock_ref, this_epoch_if_pin_sel,
                this_epoch_cb_ifpin, this_epoch_cb_reseed,
                basis_relatch_disc_pending, emitEvent);
            if (cbd.H.rows() > 0) {
                est.addSharedConstraintRows(cbd.H, cbd.z, NetworkSbasis::kCodeBiasGaugeWeight);
                if (within_epoch_armed) {
                    cap_H = cbd.H; cap_z = cbd.z;
                    cap_constraint_weight = NetworkSbasis::kCodeBiasGaugeWeight;
                    cap_has_constraint = true;
                }
            }
            // --- Card KP: loose physical-bound prior on the ADMITTED OSB columns ---
            // Folded AFTER the tight gauge rows and BEFORE the rank gate, through
            // the DIAGONAL prior channel rather than the gauge stack, so
            // plan.declared_dimension is untouched by construction (K3).  Targets
            // the CODE-dominated residual direction the phase drop-and-heal cannot
            // reach: the heal anchors PB columns only, so a near-null direction
            // with zero phase weight is structurally beyond it.  OFF ⇒ empty list
            // ⇒ addSharedPrior returns immediately (byte-identical).
            if (config_.networkCbKaulaPrior()) {
                const auto kp = NetworkSbasis::buildCodeBiasKaulaPriors(
                    registered_code_keys, config_.networkCbKaulaSigmaM());
                if (!kp.empty()) {
                    est.addSharedPrior(kp);
                    // The RAIM within-epoch replay re-solves from a fresh
                    // beginEpoch(); a prior missing there would silently give the
                    // replayed solve a DIFFERENT datum from the first one.
                    if (within_epoch_armed)
                        cap_priors.insert(cap_priors.end(), kp.begin(), kp.end());
                    ++ctx.counters.n_kp_epochs;
                    ctx.counters.n_kp_cols += kp.size();
                    // The K2 anti-fragility margin is measured AFTER the gate (see
                    // the one-shot block there), not here: it needs lambda_max of
                    // the SAME reduced system the gate judges, and calling
                    // reducedSharedNormals() at this point would force the Schur
                    // reduction BEFORE the clock datum rows are folded.  That is
                    // survivable — addSharedInfo carries a shared-border delta
                    // through a cached reduction in place — but the clock rows'
                    // w·HᵀH increment is not bit-symmetric, so reducing early would
                    // make the first epoch's last bits depend on whether KP is armed.
                }
            }
            if (spectrum_diag_budget > 0)   // first-N (spectrum block decrements below)
                log->warn("Network[W6-cb]: nshared={}, n_cb={}, folded {} code-bias gauge "
                          "rows (IF pin for {} sats + sum-to-zero for {} (const,signal) levels)",
                          est.numShared(), n_cb, cbd.H.rows(), cbd.n_if_pin_sats,
                          cbd.n_s2z_channels);
        }

        // --- Datum + null-space gate + solve --------------------------------
        NetworkSbasis sb;
        SbasisDeclaration decl;
        decl.reference_receiver_id = nc.clock_datum_station;
        decl.emit_code_bias = config_.estimateNetworkSatCodeBias();  // W6: self-describe the OSB datum
        // Fix A: self-describe the anchor-free ensemble phase-bias frame so a
        // consumer knows the emitted bias references the receiver ensemble (Σ_r z=0),
        // not a single pivot receiver's ambiguity.
        if (phase_datum_ensemble)
            decl.amb_datum = SbasisDeclaration::AmbDatum::SumToZeroPerReceiver;
        // Card 3: self-describe the Eq-34 single-receiver-chain phase-bias frame so a
        // consumer knows the emitted between-satellite reference is integer-consistent
        // (each chain hop an integer) — wire value 2 rides the existing v2 S line.
        else if (phase_datum_eq34)
            decl.amb_datum = SbasisDeclaration::AmbDatum::SingleReceiverChain;
        if (clock_brdc_anchored) {
            // Broadcast-anchored clock gauge: buildSharedDatum branches on this to
            // emit one per-satellite anchor row (Δδtˢ→0 = its own broadcast
            // a-priori) at 1/σ² — the anchors ARE the datum (no pivot, empty datum,
            // declared dimension 0), and unlike sum-to-zero the frame does not
            // redistribute when membership changes.
            decl.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
            decl.broadcast_anchor_sigma_m = clk_anchor_sigma;  // D6-resolved (brdc_rts auto = 3.0 m; load-validated binding)
        } else if (clock_sum_to_zero) {
            // Anchor-free clock gauge: buildSharedDatum branches on this to emit
            // per-constellation sum-to-zero constraint rows (no pivot, empty datum).
            decl.isb_datum = SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
        } else if (!nc.clock_datum_satellite.empty()) {
            decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
                = nc.clock_datum_satellite;
        }

        NetworkSbasis::Plan plan;
        try {
            // RTA-1: in sticky mode the incumbent baseline is last_pivot, NOT
            // prev_pivot — prev_pivot is wholesale-replaced at each emit, so a
            // constellation absent from the last emitted epoch drops out of it,
            // and its return would spuriously re-select (and, with the
            // vanish-return arm off, SILENTLY re-reference the datum).
            // last_pivot is merge-updated at the same emit site, so per
            // constellation it holds the pivot of the last emitted product that
            // CONTAINED it — exactly the datum a consumer of that constellation
            // last adopted.  Both maps advance only at the emit site (P0-1), so
            // stickiness tracks the datum of the last EMITTED product: a
            // handover epoch that is subsequently withheld leaves the incumbent
            // unchanged, and if the incumbent returns before any new-datum epoch
            // emits, the datum simply never moves.
            plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                                       sticky_pivot ? &last_pivot : nullptr);
        } catch (const std::exception& ex) {
            log->warn("Network: buildSharedDatum failed ({}); skipping epoch", ex.what());
            ++n_skipped;
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              emitEpochStatus("datum_failed", es); }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Constraint-realized clock datums: fold the gauge rows onto the shared
        // border BEFORE the null-space gate — sum-to-zero mode carries one
        // per-constellation Σδtˢ=0 row, broadcast-anchored mode one per-satellite
        // Δδtˢ→0 anchor row at 1/σ² (the anchors ARE the datum there) — so plan
        // carries constraint rows instead of hard pins and declared_dimension is 0.
        // Folded exactly like the code-bias gauge (additive information; the
        // within-epoch re-solve re-applies them from plan, which is in scope
        // there).  Empty (0 rows) in PIN mode → this is a no-op, byte-identical.
        if (plan.clock_constraint_H.rows() > 0)
            est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                        plan.clock_constraint_weight);

        // Pivot handover: the previous epoch's shared estimate lives in the OLD
        // datum, so a changed clock pivot invalidates the D2 carry-forward (and
        // forces a discontinuity bump downstream).
        // A1/A2: detect a clock-pivot datum re-reference and record WHICH
        // constellations changed (for the A2 per-constellation emit-bump scoping).
        // Arm (b) (vanish-return) is gated on network.pivot_vanish_return_signal;
        // when off this reproduces the legacy adjacent-epoch-only detection.
        std::set<char> changed_consts;
        const bool datum_changed = pivotHandoverSignalled(
            plan.clock_pivot, prev_pivot, last_pivot, pivot_vr_signal, changed_consts);
        // RTA-2: does THIS key's carried state belong to a retired datum?  With
        // network.scoped_handover_response off this is exactly datum_changed (the
        // legacy global response, byte-identical); when on, only keys of the
        // constellation(s) whose pivot changed — the SAME changed_consts set the
        // A2 emit bump consumes (single source of truth) — are invalidated.
        const auto handover_affects = [&](const std::string& sat) {
            return datum_changed
                && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
        };

        networkInjectShVtecPrior(ctx, ws, config_, est, sh_model.get(),
                                 sh_model2.get(), datum_changed,
                                 dt, within_epoch_armed);

        networkInjectPhaseHoldPrior(ctx, ws, est, within_epoch_armed);

        // Prioritized phase-bias datum drop-and-heal relocated to
        // filter/NetworkPhaseDatum (INC-8) — the seam's seed; the anchor election
        // + incumbent carry join it at INC-8b.  Full rationale + the ★ PERF note
        // (full eigenvector decomposition — gate-failure path ONLY, never per
        // epoch) in the function Doxygen.

        // Null-space rank gate.  The reduced shared normals are deliberately
        // ill-conditioned: the phase rows pin the satellite-clock / phase-bias
        // SUM to ~σ_phase precision (weight ~1e5), while CODE alone separates the
        // two (the clock-vs-bias split), so that split is orders of magnitude
        // weaker — yet still firmly non-zero.  Empirically the spectrum shows a
        // clean ~7-decade gap between the true gauge freedoms (the per-
        // constellation clock datum, σ ~ 1e-10 of σ_max) and the weakest REAL
        // direction (σ ~ 1e-2).  The default 1e-6 relative tolerance sits above
        // the weak-but-real directions and miscounts them as deficiencies; a
        // 1e-9 relative tolerance lands squarely in that GPS+Galileo-era gap,
        // counting only the genuine gauges while still catching a truly unpinned
        // datum.
        //
        // Card DG (2026-07-22): the gap MOVES with the constellation mix, so the
        // tolerance is a per-config CALIBRATION parameter (network.rank_gate_tol),
        // resolved once into est.setRankGateTol() and read here — the same value
        // the estimability dump and the drop-and-heal read (DG2: no consumer keeps
        // a private tolerance, or the dump would describe a decision the gate did
        // not make and the heal would soft-anchor weak-but-REAL directions).  It
        // defaults to NetworkEstimator::kNullSpaceTol, so an unset key is
        // byte-identical.  Adding BeiDou is the second instance of the same
        // miscount 1e-6 → 1e-9 already fixed once: the BDS weak-but-real family
        // layer sits ≈1.7e-10 of λmax, ~3 decades deeper relatively, with a
        // six-decade measured gap down to the declared gauges.

        // ---- One-shot estimability diagnostic (default-OFF) ---------------
        // Spectrum dump relocated to NetworkEstimator::dumpEstimabilitySpectrum
        // (INC-8) — the derivation and the eigen-vs-SVD cost rationale live in
        // the method Doxygen.  The one-shot LATCH is always taken so the
        // co-located W6-cb one-shot log above still fires exactly once.
        // DG5: the returned struct is the SAME numbers the WARN lines print,
        // mirrored into network_estimability.csv so the storm analyzer reads a
        // CSV instead of re-parsing log text.  The log text itself is unchanged.
        if (spectrum_diag_budget > 0) {
            --spectrum_diag_budget;
            if (nc.trace_estimability) {
                const auto dump = est.dumpEstimabilitySpectrum(
                    se.time.tow, plan.declared_dimension, col_desc, log);
                if (dump.valid) {
                    traces.writeEstimabilityEpochRow(se.time, dump.tow, dump.n_shared,
                                                     dump.declared_dim, dump.ev_max,
                                                     dump.cutoff, dump.n_below);
                    for (const auto& d : dump.dirs)
                        traces.writeEstimabilityDirRow(se.time, d.ev, d.top);
                }
            }
        }

        bool gate_ok = true;
        // DG6: the observed near-null count, captured on BOTH the passing and the
        // throwing path (the exception text cannot be parsed back into a number).
        // "Under-specified" (observed > declared — a missing datum constraint) and
        // "over-specified" (observed < declared — a tolerance below THIS config's
        // gauge depth, i.e. a mis-calibration withholding a healthy config) demand
        // opposite fixes, so they are counted separately.
        // Card KP — the K2 anti-fragility margin, logged ONCE, on the SAME reduced
        // system the gate is about to judge (lambda_max here == the gate's, and
        // the reduction now includes the clock datum rows).  The CB-ADM card
        // rejected a loose prior whose information sat AT the rank cutoff, so this
        // one must sit >= 2 decades ABOVE it; a sub-2-decade margin WARNS.
        if (config_.networkCbKaulaPrior() && !ctx.counters.kp_margin_logged
            && ctx.counters.n_kp_cols > 0) {
            ctx.counters.kp_margin_logged = true;
            const Eigen::MatrixXd& Nss_kp = est.reducedSharedNormals();
            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es_kp(Nss_kp, Eigen::EigenvaluesOnly);
            if (es_kp.info() == Eigen::Success && es_kp.eigenvalues().size() > 0) {
                const double lmax = es_kp.eigenvalues()(es_kp.eigenvalues().size() - 1);
                const double cutoff = est.rankGateTol() * std::max(lmax, 1.0);
                const double info = 1.0 / (config_.networkCbKaulaSigmaM()
                                           * config_.networkCbKaulaSigmaM());
                const double decades = (cutoff > 0.0 && info > 0.0)
                    ? std::log10(info / cutoff) : 0.0;
                if (decades >= 2.0)
                    log->info("Network (Card KP): prior info {:.3e} vs rank-gate cutoff {:.3e} "
                              "— margin {:.1f} decades ABOVE the cutoff (K2 requires >= 2.0).  "
                              "sigma {:g} m on the admitted OSB columns.",
                              info, cutoff, decades, config_.networkCbKaulaSigmaM());
                else
                    log->warn("Network (Card KP): prior info {:.3e} sits only {:.1f} decades "
                              "above the rank-gate cutoff {:.3e} — BELOW the K2 minimum of 2.0.  "
                              "This is the exact configuration the CB-ADM card rejected (prior "
                              "information AT the cutoff either trips the gate or survives by "
                              "coincidence).  TIGHTEN network.cb_kaula_sigma_m or re-place "
                              "network.rank_gate_tol from this config's estimability dump before "
                              "trusting the result.", info, decades, cutoff);
            }
        }

        int gate_observed       = -1;   // the LAST evaluation this epoch (post-heal if a re-gate ran)
        int gate_first_observed = -1;   // this epoch's FIRST evaluation — the DG6 min/max sample
        ++ctx.counters.n_gate_evaluated;
        try {
            sb.assertNullSpaceRank(est.reducedSharedNormals(), plan.declared_dimension,
                                   est.rankGateTol(), &gate_observed);
            gate_first_observed = gate_observed;
        } catch (const std::exception& ex) {
            // assertNullSpaceRank writes observed_out BEFORE the equality test, so
            // the count that caused this throw is already in gate_observed; latch it
            // now, before a possible re-gate overwrites it.
            gate_first_observed = gate_observed;
            // The reduced shared system carries MORE gauge freedoms than the
            // declared datum can pin — typically a satellite that is momentarily
            // unobservable (seen by a single station at marginal geometry), or —
            // under the prioritized datum — a phase-bias column whose anchor was
            // dropped / whose separation is weak.  For the prioritized case, try to
            // soft-anchor exactly those near-null phase-bias columns and RE-GATE
            // before withholding (the eigenVECTOR heal runs ONLY here, not every
            // epoch).  If the datum is still deficient after healing (or on the
            // legacy path), the product would be a meaningless least-norm split, so
            // WITHHOLD (the plan's "gate emission until convergence" safeguard).
            bool recovered = false;
            // PA2: per-healed-column attribution for the trace_events rows below.
            // Declared OUTSIDE the inner try so the rows are still emitted when the
            // RE-GATE (not the heal) is what threw — a heal that fired and did not
            // rescue the epoch is exactly the case worth seeing.
            std::vector<PhaseHealEvent> heal_events;
            int healed_n = 0;   // >0 only once the heal RETURNED (its priors are folded)
            try {
                const int healed = healNearNullPhaseBias(
                    est, shared_phase_keys, phase_datum_prioritized,
                    within_epoch_armed, cap_priors,    // 0 unless prioritized + near-null PB
                    &heal_events);
                healed_n = healed;
                // PA2: count the ATTEMPT before the re-gate can throw — the
                // success-only pair below cannot distinguish "the heal never ran"
                // from "the heal ran every epoch and never rescued the gate".
                if (healed > 0) {
                    ++ctx.counters.n_heal_attempt_epochs;
                    ctx.counters.n_heal_attempt_cols += static_cast<size_t>(healed);
                }
                if (healed > 0) {
                    sb.assertNullSpaceRank(est.reducedSharedNormals(),
                                           plan.declared_dimension,
                                           est.rankGateTol(), &gate_observed);
                    recovered = true;
                    ++n_healed_epochs;
                    n_healed_cols += static_cast<size_t>(healed);
                    log->debug("Network: TOW {:.1f} soft-anchored {} near-null phase-bias "
                               "column(s) (dropped anchor / weak separation) — gate recovered.",
                               se.time.tow, healed);
                }
            } catch (const std::exception& ex2) {
                // The heal (eigensolve / addSharedPrior) or the re-gate reported a
                // deficiency it could not resolve — withhold rather than crash.
                log->warn("Network: TOW {:.1f} drop-and-heal failed ({}); withholding.",
                          se.time.tow, ex2.what());
            }
            // PA2: one row per soft-anchored column (station-less by nature — the
            // heal acts on a SHARED column).  statistic = the eigenvalue of the
            // near-null direction the column spans most heavily, threshold = that
            // |component|; action records whether the heal actually rescued the
            // epoch, so an attributable-but-futile heal is distinguishable from a
            // working one.  Gated on healed_n > 0 so a heal that THREW mid-fold
            // cannot emit rows claiming columns were anchored when no prior was
            // installed — the re-gate throw (healed_n > 0, recovered false) is the
            // case worth seeing, and it is preserved.  No-op unless
            // network.trace_events.
            if (healed_n > 0) {
                for (const auto& h : heal_events)
                    emitEvent("GATE", "phase_heal", h.sat, "", h.ev, h.span,
                              recovered ? "soft_anchor" : "soft_anchor_insufficient",
                              h.signal);
            }
            if (!recovered) {
                gate_ok = false;
                ++n_gate_withheld;
                // DG6: split the withhold by DIRECTION.  observed > declared is a
                // missing datum constraint (the BeiDou storm); observed < declared
                // means the tolerance sits BELOW this config's own gauge depth, so
                // the gauges stopped being counted — a mis-calibration withholding a
                // HEALTHY config, which no amount of datum work will fix.  gate_observed
                // is whatever the LAST gate evaluation of this epoch measured.
                if (gate_observed >= 0 && gate_observed < plan.declared_dimension)
                    ++ctx.counters.n_gate_withheld_over;
                else
                    ++ctx.counters.n_gate_withheld_under;
                log->debug("Network: observability gate at TOW {:.1f} withheld product ({})",
                           se.time.tow, ex.what());
                emitEvent("GATE", "gate_withheld", "", "", std::nullopt, std::nullopt,
                          "skip_epoch", ex.what());
            }
        }
        // DG6: run-wide observed-near-null envelope, ONE sample per epoch (the
        // FIRST gate evaluation — a post-heal re-gate answers a different
        // question and is already counted by n_healed_epochs).  This is the
        // evidence DG3's placement rule is audited against: a run whose observed
        // count never leaves {declared} needs no calibration at all.
        if (gate_first_observed >= 0) {
            auto& lo = ctx.counters.nullspace_obs_min;
            auto& hi = ctx.counters.nullspace_obs_max;
            if (lo < 0 || gate_first_observed < lo) lo = gate_first_observed;
            if (hi < 0 || gate_first_observed > hi) hi = gate_first_observed;
        }
        if (!gate_ok) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << plan.declared_dimension << ",0,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("gate_withheld", es); }
            // RTA-10: product-level heartbeat for a WITHHELD epoch — an H line with NO
            // E line, so a live tail can tell "gate withheld" from "producer died".
            // Only once the deferred product writer has opened (the first emit): earlier
            // withheld epochs precede the product file's existence by construction.
            // σ̂0 is unavailable pre-solve (0 per the H grammar); withheld="*" = whole epoch.
            if (emit_epoch_status && product_writer) {
                NetworkProducerStatus hst;
                hst.time.week = se.time.week;
                hst.time.tow  = se.time.tow;
                hst.state     = NetworkProducerStatus::State::WithheldGate;
                hst.withheld  = "*";
                product_writer->writeEpochStatus(hst);
                if (nc.product_flush_per_epoch) product_writer->flush();
            }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Two-state clock model warmup gate (mirrors carry_warm): seed/predict only after the
        // cold-start convergence horizon.  Its per-epoch predictions (for the post-solve update
        // + normalized-innovation test) are stashed here.
        const bool clk_model_warm = clk_model_on && (static_cast<int>(n_emitted) >= pl_warmup);
        // network.emit_clock_rate: same warmup gate as the clock model — skip the cold-start
        // epochs where the floated clock would finite-difference into a huge spurious C1 rate.
        const bool emit_rate_warm = emit_clock_rate_on && (static_cast<int>(n_emitted) >= pl_warmup);
        auto& clk_pred = ws.clk_pred;
        auto& clk_prior_info = ws.clk_prior_info;

        // --- Card 6 (RTF-2 / plan D3): ephemeris-cutover re-level ------------------------------
        // The producer's estimable per-sat clock is Δδtˢ = (true sat clock) − (broadcast
        // a-priori clock of the version this epoch's geometry uses).  When that broadcast
        // version rolls (old IODE → new IODE) the a-priori jumps by
        // Δ_eph(t) = c·(brdc_new(t) − brdc_old(t)); the TRUE clock is continuous, so the
        // estimable Δδtˢ must jump by −Δ_eph to keep representing the same satellite.  Left
        // alone, the carried clock prior — the TIGHT two-state clock_osc prediction, or the
        // D2 q_clock carry — is a full Δ_eph out of frame and FIGHTS the new-frame
        // measurements: a slow transient that also trips the two-state NIS / clock-monitor
        // jump test as a spurious per-sat fault.  So we deterministically re-base the CARRIED
        // clock state by −Δ_eph, and the solve runs entirely in the new frame — emitted C0 is
        // continuous-in-reconstruction (the consumer forms brdc(new_iode)+C0, Card 7) with NO
        // discontinuity bump.
        //
        // ORDERING (deliberate deviation from the plan's "after the solve, before the C1
        // pass"): the re-base MUST precede the prior injection (below) and the solve.  The
        // plan's post-solve placement anchored to where Card 5 STAMPS clock_iode (after
        // toSolutionEpoch); but by then x_s was already solved against the OLD-frame prior, so
        // a post-hoc x_s shift is exact ONLY when the prior dominates (tight) and OVER-corrects
        // a data-dominated (loose) solve.  Shifting the PRIOR is unconditionally exact for any
        // prior weight AND — being a deterministic function of the broadcast ephemeris alone —
        // is independent of the solve, so it applies identically on every disposition (emit /
        // σ̂0-withhold / solve-fail).  The IODE this epoch uses is read straight from the
        // shared LiveEphemerisStore (Card 5's own equivalence note: toe granularity ≫
        // intra-epoch light-travel, so the emit-site query lands on the version the emission
        // path used), so no dependence on the post-solve stamp remains.
        //
        // Placed after the observability-gate `continue`: a gate-withheld epoch leaves the
        // carried clock state untouched (its post-emit update never runs), so deferring the
        // re-level to the next non-gate-withheld epoch keeps relevel_iode and the carried
        // state in lock-step.  On the σ̂0/solve-fail paths the block DID run, the shift landed,
        // and the (skipped) post-emit updates leave it in place — no double-shift either way.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            for (const std::string& sat : registered_clock_sats) {
                if (sat.empty() || sat[0] == 'R') continue;          // GLONASS out of scope (plan D8)
                const auto now = eph_store.computeSatelliteClock(sat, se.time);
                if (!now) continue;                                  // no eph held → nothing to track
                const int new_iode = now->iode;
                auto bit = relevel_iode.find(sat);
                if (bit == relevel_iode.end()) {                     // first sighting: seed, no shift
                    relevel_iode[sat] = new_iode;
                    continue;
                }
                if (bit->second == new_iode) continue;               // no roll — the common case
                const int old_iode = bit->second;
                const auto shift = eph_store.ephCutoverShiftMetres(sat, se.time, old_iode);
                relevel_iode[sat] = new_iode;                        // baseline advances regardless
                if (shift) {
                    // Re-base the carried clock state by −Δ_eph.  Offset / level term ONLY —
                    // the drift (c1) and the 2×2 covariance are frame-invariant and untouched.
                    const double d = *shift;
                    if (auto oit = clock_osc.find(sat); oit != clock_osc.end() && oit->second.init)
                        oit->second.x0 -= d;                          // two-state offset (clock_model)
                    for (CarryPrior& cp : prev_carry)                 // D2 scalar clock carry (model off)
                        if (cp.kind == SharedParamKind::Clock && cp.sat == sat) cp.value -= d;
                    if (auto jit = prev_clk.find(sat); jit != prev_clk.end())
                        jit->second.first -= d;                       // clock-monitor jump baseline
                    // Card 1: the coupling stash references this clock via ŝ_prev terms —
                    // re-level them into the new frame too, else every coupling row's RHS
                    // is off by M·Δ_eph after a cutover (the row's functional u = r + Mᵀs
                    // is frame-consistent only if ALL carried references move together).
                    if (carry_cpl_cond) {
                        for (auto& [lk, lcv] : local_carry)
                            for (CoupledShared& cs2 : lcv.coupling)
                                if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                                    cs2.s_prev -= d;
                        for (auto& [zk, zcv] : zwd_carry)
                            for (CoupledShared& cs2 : zcv.coupling)
                                if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                                    cs2.s_prev -= d;
                    }
                    log->info("Network (Card 6): {} eph cutover IODE {}->{} — re-leveled carried "
                              "clock by {:.4f} m (−Δ_eph); no discontinuity bump.",
                              sat, old_iode, new_iode, -d);
                    emitEvent("RELEVEL", "eph_cutover_relevel", sat, "", -d, std::nullopt,
                              "relevel", fmt::format("iode={}->{}", old_iode, new_iode));
                } else {
                    // Prior version no longer retained → Δ_eph incomputable → the clock stepped
                    // by an unknown amount.  Force a per-sat discontinuity (consumer resets this
                    // sat's network-corrected ambiguity across the step); it lands at the sat's
                    // next actual emit (below).  Effectively unreachable with per-epoch checking.
                    eph_cutover_disc_pending.insert(sat);
                    log->warn("Network (Card 6): {} eph cutover IODE {}->{} but the prior version "
                              "is no longer held — forcing a per-sat discontinuity (Δ_eph "
                              "incomputable).", sat, old_iode, new_iode);
                    emitEvent("RELEVEL", "eph_cutover_disc", sat, "", std::nullopt, std::nullopt,
                              "disc", fmt::format("iode={}->{}", old_iode, new_iode));
                }
            }
        }

        networkInjectD2CarryPrior(ctx, ws, traces, se.time, est, datum_changed,
                                  changed_consts, dt, within_epoch_armed, log);

        networkInjectClockOscPrior(ctx, ws, traces, se.time, est, registered_clock_sats,
                                   clk_model_warm, datum_changed, changed_consts,
                                   pl_now_s, dt, within_epoch_armed);

        // --- A-priori satellite-clock anchor (cold-start conditioning) ------
        // The shared clock column is Δδtˢ relative to the a-priori PRECISE product
        // clock, so it is genuinely small (sub-metre for precise post-processing).
        // With no absolute anchor the per-epoch joint solve floats it by hundreds
        // of metres at cold start to absorb the clock↔iono↔bias confusion (no
        // carry-forward exists yet at epoch 1, and epoch 2+ then anchors to that
        // floated value).  A Tikhonov prior pulling each Δδtˢ toward 0 with
        // σ = initial_std.sat_clock (the documented "Δδtˢ vs a-priori product clock"
        // quality: sub-m precise, looser for RT broadcast) regularizes ONLY the
        // ill-conditioned cold-start direction: the per-constellation clock DATUM
        // stays fixed by the explicit Datum (a large-weight pin that dominates this
        // soft prior), so the few-metre datum offset the data determines survives.
        // Applied POST-gate (like the carry-forward) so the null-space rank gate
        // still sees the clock datum as a gauge, and it combines in information space
        // with the carry-forward (anchor-toward-0 + RW-toward-previous).  Gated to a
        // BINDING std (< 100 m): the legacy loose default (1000 m) leaves the anchor
        // off, so every existing network config is byte-identical.
        //
        // SCOPE (measured, 305-sta GPS+Gal, 2026-06-30; see [[project_global_sh_iono]]):
        // σ=0.05 m collapses cold-start clock-Δ rms 56→2.9 m for ~6 epochs, then the
        // W6 clock↔bias↔ambiguity systematic (data wants ~80 m, info ≫ the anchor)
        // reasserts.  This improves cold-start CLOCK-PRODUCT quality only — the
        // co-estimated SH-VTEC field divergence is a SEPARATE, DECOUPLED cold-start
        // estimability defect (its large-scale level is unobservable under the
        // common-mode-free iono R); pinning the clock does NOT move the field.
        {
            const double clk_sigma = clk_anchor_sigma;  // m (D6-resolved; sp3 auto == initial_std.sat_clock)
            // Broadcast-anchored datum: the SAME per-satellite anchors were already
            // folded PRE-gate as the gauge realization (buildSharedDatum constraint
            // rows) — folding them again here would double the anchor information
            // (effective σ/√2).  Skip; every other mode keeps the post-gate
            // regularizer exactly as before.
            if (!clock_brdc_anchored
                && std::isfinite(clk_sigma) && clk_sigma > 0.0 && clk_sigma < 100.0) {
                const double clk_info = 1.0 / (clk_sigma * clk_sigma);
                std::vector<NetworkEstimator::SharedPrior> clk_anchor;
                clk_anchor.reserve(registered_clock_sats.size());
                for (const std::string& sat : registered_clock_sats) {
                    NetworkEstimator::SharedPrior p;
                    p.kind  = SharedParamKind::Clock;
                    p.sat   = sat;
                    p.value = 0.0;          // anchor Δδtˢ toward the a-priori clock
                    p.info  = clk_info;
                    clk_anchor.push_back(p);
                }
                est.addSharedPrior(clk_anchor);
                if (within_epoch_armed)
                    cap_priors.insert(cap_priors.end(), clk_anchor.begin(), clk_anchor.end());
            }
        }

        // A clock-pivot handover re-references the whole shared solution; the
        // forced bump steps BOTH phase and code discontinuity counters next
        // emit (the code per-(const,signal) sum-to-zero and IF-pin re-references
        // are detected by the emitter from the emitted epoch itself — see
        // NetworkProductEmitter; only this clock-pivot signal is driver-known).
        // Both the emitter datum bump (markDatumChange) AND the prev_pivot /
        // last_pivot baseline advance are DEFERRED to the emit site: the maps
        // must describe the datum of the last EMITTED product (the consumer's
        // view).  Advancing them here — before the solve — would make a handover
        // epoch that is subsequently skipped (solve failure, σ̂0 withhold)
        // compare equal next epoch: the discontinuity signal would be lost
        // forever AND the old-datum prev_carry/clock_osc would fold as priors
        // into the new-datum solve.  With the baseline frozen across skipped
        // epochs, every skip path re-detects the handover until one epoch
        // actually emits in the new datum.

        Eigen::VectorXd x_s;
        try {
            // parallel_carry: the WHOLE est.solve() is Eigen-threaded work — foldReducedDdPriors
            // fans its per-station reduction across threads itself (Eigen PINNED), and its dense
            // GEMMs + the K×K middle inverse run under Eigen's own threading.  So, unlike the two
            // isolated build/install GEMM stages, solve() is NOT wrapped in a CarryEigenScope: an
            // un-pin here would nest the fold's OpenMP with Eigen's GEMM threads.  It is also why
            // parallel_carry cannot coexist with parallel_reduce (which would OpenMP-fan this same
            // Eigen-threaded solve across stations) — the loader keeps them mutually exclusive.
            x_s = est.solve(plan.shared_datum);
            // Card 1d debug: full-precision solve checksum for A/B-arm differencing
            // (print-precision-proof).  One-shot budget; log-only.
            if (carry_cpl_cond && n_cpl_solve_dump > 0) {
                --n_cpl_solve_dump;
                log->info("Network (Card 1d dbg): solve checksum TOW {:.1f}: sum={:.17e} "
                          "norm={:.17e} n={}", se.time.tow, x_s.sum(), x_s.norm(), x_s.size());
            }
        } catch (const std::exception& ex) {
            log->warn("Network: solve failed at TOW {:.1f} ({}); skipping epoch",
                      se.time.tow, ex.what());
            ++n_skipped;
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("solve_failed", es); }   // x_s empty — no clock stats
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Satellites whose carried SHARED state (clock + coupled phase/code bias) is to be
        // REFLOATED at the end-of-epoch stash: the per-satellite clock monitor (below, the
        // detector for a lone lock-in the σ̂0 RAIM misses) plus — only when
        // clock_monitor_couple_raim is opted in — every satellite the within-epoch / post-fit
        // RAIM already flags (so a slip that leaked into the shared clock reinitializes the
        // clock too, not just the ambiguity).  Empty unless network.clock_monitor, so the stash
        // stays byte-identical when off.
        std::set<std::string> clock_refloat_sats;
        int n_arc_refloat_epoch = 0;   // T2d: isolated-arc RAIM refloats executed this epoch (heartbeat metric)
        // B1 retro-bump / URA-inflate feed (P0-2): the union of satellites ANY QC tier
        // flags this epoch, independent of clock_monitor_couple_raim.  clock_refloat_sats
        // only captures the RAIM tiers under the couple opt-in (it drives the CARRY
        // refloat, a numeric action), so a separate accumulator is needed so the
        // retro-bump reaches every tier without forcing a carry refloat.  Populated only
        // when a URA feature that consumes it is on; inert (byte-identical) otherwise.
        std::set<std::string> retro_flag_sats;
        const bool retro_feed = ura_retro || ura_refloat_inflate > 1.0;

        // --- Within-epoch adaptation: drop the dominant culprit(s) and re-solve --------
        // When σ̂0 (the robust a-posteriori variance factor) shows the joint solve is
        // contaminated, peel the single most-prominent culprit — a whole SATELLITE (its
        // rows de-weighted network-wide) or a whole STATION (excluded) — re-solve THIS epoch
        // over a fresh beginEpoch() replay of the captured shared system, and repeat until
        // σ̂0 falls below the trigger or a cap.  Only the (rare, already-contaminated) trigger
        // epochs pay the re-solve; try/catch reverts to the last good solve on any failure.
        // Uses the same robust median-polish attribution as the RAIM refloat.
        int  n_adapt = 0;
        bool we_adapt_attempted = false;   // a within-epoch re-solve was launched (est layout was rebuilt)
        double sigma0_adapt = 0.0;         // A4: final within-epoch σ̂0 (0 = adaptation did not run this epoch)
        // RTA-10 (network.per_const_withhold): per-CONSTELLATION partition of the LAST
        // assess()'s standardized-residual pool — the very rows whose median is the global
        // σ̂0 the A4 withhold compares, keyed by the ambiguity's satellite constellation
        // (pa.sat[0]).  Cleared and refilled on every assess() so, like sigma0_adapt, it
        // describes the FINAL within-epoch solve.  Populated only when the carve-out is
        // armed (empty otherwise — no cost, no behavior change on the default path).
        std::map<char, std::vector<double>> we_sigma0_pool_c;
        // R12: the B8/T2 flag-rate (first-solve σ̂0 > trigger) is DECIDED here but COMMITTED at
        // the emit site, so a subsequently-withheld epoch is excluded from T2's denominator —
        // matching T3–T6, which only accrue on emitted epochs.  Without this, T2 counted withheld
        // (contaminated) epochs while the post-fit tiers did not, giving incomparable alphas.
        bool we_t2_opp = false, we_t2_flag = false;
        if (within_epoch_armed && carry_warm) {
            const double trig      = config_.networkRaimSigma0Trigger();
            const bool   surgical  = config_.networkRaimSurgicalStation();
            const double sta_bias  = surgical ? config_.networkRaimStationDropBias() : 1.0;
            constexpr int    kMaxAdapt   = 5;

            std::vector<char> orig_added(stations.size(), 0);
            for (size_t i = 0; i < stations.size(); ++i)
                orig_added[i] = (est_index_of[i] >= 0) ? 1 : 0;

            // Culprit sets.  A bad AMBIGUITY (cycle slip — the dominant contamination the
            // carried-phase RAIM sees) needs REINITIALIZATION, not measurement de-weighting: the
            // re-solve REFLOATS the culprit's ambiguities (withholds their stale carry prior) so
            // they re-estimate from full-weight data and absorb the slip locally.  A whole bad
            // STATION (a data-quality problem, not a slip) is EXCLUDED — the de-weight/exclude
            // tool, which is the right one for a bad measurement.
            std::set<std::string> drop_sats;                       // refloat that sat's ambiguities network-wide
            std::set<int>         drop_stations;                   // excluded entirely (bad-data case)
            std::map<int, std::set<std::string>> drop_arcs;        // (station → sats) refloat at that station ONLY
            // C3c §3: the DD-carry re-solve (buildCarryDdPriors, via resolve_dropping)
            // treats a mid-epoch RAIM culprit as slipped, so its stale carry is not
            // re-folded (refold from unchanged predicted state, H13 + culprit-withhold).
            // Aliases live only while this block runs; nulled before the sets leave scope.
            carry_we_drop_sats = &drop_sats;
            carry_we_drop_arcs = &drop_arcs;

            // σ̂0 + median-polish effects of the CURRENT shared solution.  The (station × sat)
            // residual table (carried-ambiguity PHASE rows only) decomposes additively into a
            // per-satellite column effect, a per-station row effect, and an isolated-arc
            // interaction.  Computed over EVERY present ambiguity — a refloated ambiguity fits
            // its own data (its residual collapses after the re-solve), so σ̂0 stays an HONEST
            // measure of the REMAINING contamination without special-casing; the culprit is kept
            // out of the next pick by the candidate-set exclusion, not by hiding it here.
            auto assess = [&](const Eigen::VectorXd& xs, double& sigma0_out,
                              std::map<std::string, double>& sat_eff,
                              std::map<int, double>& sta_eff,
                              std::map<std::pair<int, std::string>, double>& arc_eff) {
                std::vector<double> r_pool;
                if (perconst_on) we_sigma0_pool_c.clear();   // RTA-10: re-partition per assess
                std::map<std::pair<int, std::string>, double> cell_max;
                for (size_t i = 0; i < stations.size(); ++i) {
                    // Prioritized: keep the former pivot in the σ̂0 pool — it now
                    // carries fault-testable non-anchored arcs; its anchored (pinned)
                    // keys are already absent from R.pl_amb (per-(sat,signal) partition).
                    if (!orig_added[i] || est_index_of[i] < 0
                        || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
                    const StationAssembly& R = asm_res[i];
                    if (R.pl_amb.empty() || R.A_s.cols() != xs.size()) continue;
                    Eigen::VectorXd x_r;
                    try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), xs); }
                    catch (...) { continue; }
                    if (x_r.size() != R.A_r.cols()) continue;
                    const Eigen::VectorXd resid = R.y - R.A_s * xs - R.A_r * x_r;
                    Eigen::VectorXd row_r;   // B2: per-row Baarda redundancy (empty when off)
                    if (raim_redun) {
                        try {
                            row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                                      R.A_s, R.A_r, R.w, plan.shared_datum);
                        } catch (...) { row_r.resize(0); }
                    }
                    for (const auto& pa : R.pl_amb) {
                        if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
                        double r_max = 0.0;
                        for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                            if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;
                            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                            double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                            if (raim_redun && row < row_r.size()) {   // B2: standardize v/(σ·√r_i)
                                constexpr double kRedFloor = 1e-6;
                                const double ri = row_r(row);
                                if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                                r /= std::sqrt(ri);
                            }
                            r_pool.push_back(r);
                            // RTA-10: the SAME standardized row (weighting and B2 √r_i
                            // normalization included; redundancy-floored rows excluded by
                            // the continue above), partitioned by constellation.
                            if (perconst_on && !pa.sat.empty())
                                we_sigma0_pool_c[pa.sat[0]].push_back(r);
                            if (r > r_max) r_max = r;
                        }
                        const auto key = std::make_pair(static_cast<int>(i), pa.sat);
                        auto it = cell_max.find(key);
                        if (it == cell_max.end() || r_max > it->second) cell_max[key] = r_max;
                    }
                }
                sigma0_out = 1.0;
                if (!r_pool.empty()) {
                    std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
                    sigma0_out = std::max(raim_s0_floor, 1.4826 * r_pool[r_pool.size() / 2]);
                }
                std::vector<std::tuple<int, std::string, double>> tbl;
                tbl.reserve(cell_max.size());
                for (const auto& [k, v] : cell_max) tbl.emplace_back(k.first, k.second, v);
                const MedianPolish mp = medianPolish2Way(tbl);
                sat_eff = mp.col_eff;
                sta_eff = mp.row_eff;
                arc_eff.clear();
                for (std::size_t k = 0; k < tbl.size(); ++k)
                    arc_eff[std::make_pair(std::get<0>(tbl[k]), std::get<1>(tbl[k]))] = mp.interaction[k];
            };

            // Replay the epoch over a fresh arrowhead (beginEpoch reopens the frozen layout;
            // only reset() would clear it), REFLOATING the culprit ambiguities.  Rebuilds
            // est_index_of so reconstructStation / harvest stay valid on the final solve.  The
            // measurements stay at FULL weight — de-weighting is the tool for a bad measurement,
            // whereas a slipped ambiguity needs a new VALUE.  A satellite culprit refloats its
            // ambiguities at every station; an arc culprit at one station only; a whole-station
            // culprit is excluded (its data, not a single ambiguity, is bad).  A refloated
            // ambiguity keeps its full-weight data (so its local block stays positive-definite —
            // no drop-induced singularity) but WITHHOLDS its carry prior, so the re-solve lets it
            // find its new value and absorb the slip locally; the harvest then re-stashes that
            // corrected value (self-contained reinit — no reliance on the post-fit refloat).
            auto resolve_dropping = [&]() -> Eigen::VectorXd {
                est.beginEpoch();
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (!orig_added[i] || drop_stations.count(static_cast<int>(i))) {
                        est_index_of[i] = -1;
                        continue;
                    }
                    const StationAssembly& R = asm_res[i];
                    // A_r columns of THIS station's ambiguities to refloat (withhold their prior).
                    // A SATELLITE culprit refloats its ambiguities at every station tracking it: a
                    // satellite-level contamination pulls the shared clock at ALL its stations, and
                    // its arcs are contaminated to varying degrees — a strict per-arc gate on the
                    // (inflated) σ̂0 would miss the moderately-slipped ones and leave a worse
                    // mixed state, so the whole satellite is reinitialized consistently.  An ARC
                    // culprit refloats at ONE station only (surgical, isolated slip).
                    std::set<Eigen::Index> refloat_cols;
                    const auto dai = drop_arcs.find(static_cast<int>(i));
                    for (const auto& pa : R.pl_amb)
                        if (drop_sats.count(pa.sat) ||
                            (dai != drop_arcs.end() && dai->second.count(pa.sat)))
                            refloat_cols.insert(pa.col);
                    // Card 1: a refloated ambiguity's carry is a coupling ROW inside A_s/A_r
                    // (not a carry_cols diagonal prior), so withholding it means ZEROING that
                    // row before re-adding the block — the coupling-row analogue of the
                    // diagonal-path prior withhold below.  The refloated ambiguity keeps its
                    // full-weight DATA rows, so its local block stays PD.  A copy so the
                    // primary R (and any iterated replay) is untouched.
                    if (carry_cpl_cond && carry_cpl_joint && !R.cpl_raw.empty()
                        && !refloat_cols.empty() && R.cpl_row_start >= 0) {
                        // Card 1b: the joint whitening MIXES a station's coupling rows, so a
                        // refloat cannot zero individual output rows — re-whiten the SURVIVING
                        // rows (dropping the refloated locals) and re-append them after the
                        // unchanged data rows.  A re-whiten failure drops all coupling rows for
                        // the epoch (safe degrade: the refloated arcs re-float from data anyway).
                        Eigen::MatrixXd Hr, Hs;
                        Eigen::VectorXd z;
                        const bool ok = whitenCouplingRows(R.cpl_raw, R.cpl_white_pw, refloat_cols,
                                                           R.A_r.cols(), R.A_s.cols(), Hr, Hs, z);
                        const Eigen::Index nd = R.cpl_row_start;             // data + SH rows (unchanged)
                        const Eigen::Index nk = ok ? Hr.rows() : 0;          // surviving coupling rows
                        Eigen::MatrixXd As(nd + nk, R.A_s.cols()), Ar(nd + nk, R.A_r.cols());
                        Eigen::VectorXd rw(nd + nk), ry(nd + nk);
                        As.topRows(nd) = R.A_s.topRows(nd);
                        Ar.topRows(nd) = R.A_r.topRows(nd);
                        rw.head(nd)    = R.w.head(nd);
                        ry.head(nd)    = R.y.head(nd);
                        if (nk > 0) {
                            As.bottomRows(nk) = Hs;
                            Ar.bottomRows(nk) = Hr;
                            rw.tail(nk).setOnes();
                            ry.tail(nk) = z;
                        }
                        est_index_of[i] = static_cast<int>(est.addStation(As, Ar, rw, ry));
                    } else if (carry_cpl_cond && !refloat_cols.empty() && R.cpl_row_start >= 0) {
                        Eigen::MatrixXd As = R.A_s, Ar = R.A_r;
                        Eigen::VectorXd rw = R.w, ry = R.y;
                        for (std::size_t rr = 0; rr < R.cpl_row_local_cols.size(); ++rr)
                            if (refloat_cols.count(R.cpl_row_local_cols[rr])) {
                                const Eigen::Index row =
                                    R.cpl_row_start + static_cast<Eigen::Index>(rr);
                                As.row(row).setZero(); Ar.row(row).setZero();
                                rw(row) = 0.0; ry(row) = 0.0;
                            }
                        est_index_of[i] = static_cast<int>(est.addStation(As, Ar, rw, ry));
                    } else {
                        est_index_of[i] = static_cast<int>(est.addStation(R.A_s, R.A_r, R.w, R.y));
                    }
                    // B9: re-inject the pivot's ZWD carry too (its R.carry_cols is empty
                    // unless network.carry_zwd), so the within-epoch re-solve matches the
                    // primary solve.  refloat_cols only ever names ambiguity columns, so a
                    // pivot's lone ZWD column is never in it (empty → the fast path).
                    if (carry_warm && !R.carry_cols.empty()) {
                        if (refloat_cols.empty()) {
                            est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                                R.carry_cols, R.carry_dNrr, R.carry_dur);
                        } else {
                            // Inject the carry prior for every carried ambiguity EXCEPT the
                            // refloated ones (those are left free → re-estimated from data).
                            std::vector<Eigen::Index> cc;
                            Eigen::VectorXd cdn(R.carry_cols.size()), cdu(R.carry_cols.size());
                            cc.reserve(R.carry_cols.size());
                            Eigen::Index m = 0;
                            for (std::size_t k = 0; k < R.carry_cols.size(); ++k)
                                if (!refloat_cols.count(R.carry_cols[k])) {
                                    cc.push_back(R.carry_cols[k]);
                                    cdn(m) = R.carry_dNrr(static_cast<Eigen::Index>(k));
                                    cdu(m) = R.carry_dur(static_cast<Eigen::Index>(k));
                                    ++m;
                                }
                            if (!cc.empty()) {
                                cdn.conservativeResize(m);
                                cdu.conservativeResize(m);
                                est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                                    cc, cdn, cdu);
                            }
                        }
                    }
                }
                if (cap_has_constraint) est.addSharedConstraintRows(cap_H, cap_z, cap_constraint_weight);
                // Re-apply the sum-to-zero clock gauge on the fresh arrowhead (plan is
                // in scope; its rows are layout-frozen for the epoch so they match the
                // primary solve).  No-op / byte-identical in PIN mode (0 rows).
                if (plan.clock_constraint_H.rows() > 0)
                    est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                                plan.clock_constraint_weight);
                if (!cap_priors.empty()) est.addSharedPrior(cap_priors);
                // C1: replay the dense D2 fold on the fresh arrowhead.  The layout is
                // frozen for the epoch, so the key→column mapping and the LDLT are
                // bit-reproducible — the re-solve sees the identical shared system.
                // Non-empty only when the primary dense fold succeeded (its fallback
                // routed through cap_priors above), so this cannot throw where the
                // primary did not.
                if (!cap_dense_keys.empty())
                    est.addSharedPriorDense(cap_dense_keys, cap_dense_cov);
                // Fix A: rebuild the ensemble phase-bias gauges on the fresh arrowhead
                // (beginEpoch reopened the layout but est_index_of was rebuilt above and
                // refloated stations may have dropped, so the stored gauges' station
                // indices are stale).  No-op when OFF.
                buildEnsembleGauges();
                // Card 4 (M1) C3b: rebuild the DD-prior blocks on the fresh arrowhead
                // (beginEpoch cleared them + est_index_of was rebuilt above).  The
                // predict is NOT re-run — only the fold is rebuilt.  No-op when OFF.
                buildCarryDdPriors();
                return est.solve(plan.shared_datum);
            };

            double sigma0 = 1.0;
            std::map<std::string, double> sat_eff;
            std::map<int, double> sta_eff;
            std::map<std::pair<int, std::string>, double> arc_eff;
            assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
            const double sigma0_first = sigma0;
            if (trace_flag_rates) { we_t2_opp = true; we_t2_flag = (sigma0_first > trig); }  // B8/T2 (R12: committed at emit, below)
            std::string adapt_detail;
            for (int iter = 0; iter < kMaxAdapt && sigma0 > trig; ++iter) {
                // Best NEW culprit per granularity (all gated on effect/σ̂0 > raim_k).  An
                // isolated ARC is the finest, least-destructive peel; a whole STATION is the
                // coarsest (it discards good arcs too) and is de-rated by sta_bias so it is
                // only chosen when it dominates the best satellite/arc by that margin.
                // Preference on equal score is ARC > SAT > STA (surgical-first tie-break).
                std::string best_arc_sat; int best_arc_sta = -1; double best_arc_eff = 0.0;
                if (surgical)
                    for (const auto& [key, eff] : arc_eff) {
                        const int s = key.first; const std::string& sat = key.second;
                        if (drop_stations.count(s) || drop_sats.count(sat)) continue;
                        const auto it = drop_arcs.find(s);
                        if (it != drop_arcs.end() && it->second.count(sat)) continue;
                        if (eff > best_arc_eff) { best_arc_eff = eff; best_arc_sta = s; best_arc_sat = sat; }
                    }
                std::string best_sat; double best_sat_eff = 0.0;
                for (const auto& [sat, eff] : sat_eff)
                    if (!drop_sats.count(sat) && eff > best_sat_eff) { best_sat_eff = eff; best_sat = sat; }
                int best_sta = -1; double best_sta_eff = 0.0;
                for (const auto& [sta, eff] : sta_eff)
                    if (!drop_stations.count(sta) && eff > best_sta_eff) { best_sta_eff = eff; best_sta = sta; }

                // Consensus escalation: a satellite with ≥ raim_min_sta arcs currently over the
                // gate is a NETWORK-WIDE problem (its shared clock/phase-bias is corrupted), not a
                // stack of isolated arcs — reinitialize the whole satellite in ONE iteration (one
                // re-solve) rather than peeling arc by arc.  When the best arc belongs to such a
                // satellite, redirect to the satellite branch.  Reuses the RAIM cross-station
                // consensus count (§ post-fit RAIM).
                if (surgical && best_arc_sta >= 0 && !drop_sats.count(best_arc_sat)) {
                    int n_bad = 0;
                    for (const auto& [key, eff] : arc_eff) {
                        if (key.second != best_arc_sat || drop_stations.count(key.first)) continue;
                        const auto it = drop_arcs.find(key.first);
                        if (it != drop_arcs.end() && it->second.count(key.second)) continue;
                        if (eff / sigma0 > raim_k) ++n_bad;
                    }
                    if (n_bad >= raim_min_sta) {
                        if (best_arc_eff > best_sat_eff) { best_sat = best_arc_sat; best_sat_eff = best_arc_eff; }
                        best_arc_sta = -1;   // drop the whole satellite instead of this one arc
                    }
                }

                enum class Kind { None, Arc, Sat, Sta } kind = Kind::None;
                double best_score = -1.0;   // strict '>' below → earlier (more surgical) wins ties
                if (best_arc_sta >= 0 && best_arc_eff / sigma0 > raim_k && best_arc_eff > best_score) {
                    best_score = best_arc_eff; kind = Kind::Arc;
                }
                if (!best_sat.empty() && best_sat_eff / sigma0 > raim_k && best_sat_eff > best_score) {
                    best_score = best_sat_eff; kind = Kind::Sat;
                }
                if (best_sta >= 0 && best_sta_eff / sigma0 > raim_k &&
                    (best_sta_eff / sta_bias) > best_score) {
                    best_score = best_sta_eff / sta_bias; kind = Kind::Sta;
                }
                if (kind == Kind::None) break;  // nothing bad enough left to drop

                if (kind == Kind::Arc) {
                    drop_arcs[best_arc_sta].insert(best_arc_sat);
                    adapt_detail += fmt::format("{}arc:{}/{}", adapt_detail.empty() ? "" : ",",
                                                stations[best_arc_sta].name, best_arc_sat);
                } else if (kind == Kind::Sat) {
                    drop_sats.insert(best_sat);
                    adapt_detail += fmt::format("{}sat:{}", adapt_detail.empty() ? "" : ",", best_sat);
                } else {
                    drop_stations.insert(best_sta);
                    adapt_detail += fmt::format("{}sta:{}", adapt_detail.empty() ? "" : ",",
                                                stations[best_sta].name);
                }

                Eigen::VectorXd x_try;
                we_adapt_attempted = true;
                try { x_try = resolve_dropping(); }
                catch (const std::exception& ex) {
                    // Should be unreachable now (the per-station full-weight retry uses the
                    // known-PD original assembly).  Last resort: restore the clean no-drop solve
                    // so the emit / harvest run on a consistent estimator rather than a
                    // half-rebuilt one (the old "keep prior solve" left est mid-rebuild).
                    log->warn("Network[RAIM within-epoch]: re-solve failed ({}); restoring clean solve.",
                              ex.what());
                    drop_sats.clear(); drop_stations.clear(); drop_arcs.clear();
                    try { x_s = resolve_dropping(); } catch (...) {}
                    break;
                }
                x_s = x_try;
                ++n_adapt;
                assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
            }
            if (n_adapt > 0) {
                log->info("Network[RAIM within-epoch]: TOW {:.1f} re-conditioned in {} re-solve(s) — "
                          "refloated {} sat / {} arc, {} sta excluded [{}]; σ̂0 {:.2f} → {:.2f} (trigger {:.2f}).",
                          se.time.tow, n_adapt, drop_sats.size(),
                          [&] { size_t n = 0; for (const auto& kv : drop_arcs) n += kv.second.size(); return n; }(),
                          drop_stations.size(), adapt_detail, sigma0_first, sigma0, trig);
                // T2: one event per settled culprit (whole-sat drop, per-(sta,sat) arc drop,
                // excluded station).  statistic=final σ̂0, threshold=trigger, detail=first->final.
                if (events_trace.is_open()) {
                    const std::string span = fmt::format("{:.2f}->{:.2f}", sigma0_first, sigma0);
                    for (const auto& s : drop_sats)
                        emitEvent("T2_within_epoch", "we_refloat_sat", s, "", sigma0, trig,
                                  "refloat", span);
                    for (const auto& kv : drop_arcs)
                        for (const auto& s : kv.second)
                            emitEvent("T2_within_epoch", "we_refloat_arc", s,
                                      stations[kv.first].name, sigma0, trig, "refloat", span);
                    for (int sta : drop_stations)
                        emitEvent("T2_within_epoch", "we_exclude_station", "",
                                  stations[sta].name, sigma0, trig, "exclude", span);
                }
            }

            // A within-epoch satellite refloat reinitialized that sat's AMBIGUITY at every
            // station; its coupled shared clock is just as suspect, so refloat the carried
            // clock too (the stash below skips it).  Opt-in (clock_monitor_couple_raim).
            if (clk_mon_couple)
                for (const auto& s : drop_sats) clock_refloat_sats.insert(s);
            // P0-2: a within-epoch whole-satellite drop contaminated that sat's emitted
            // correction — feed the retro-bump regardless of the (numeric) couple opt-in.
            if (retro_feed)
                for (const auto& s : drop_sats) retro_flag_sats.insert(s);

            sigma0_adapt = sigma0;   // A4: publish the final within-epoch σ̂0 for the withhold gate
        }
        // C3c §3: drop_sats/drop_arcs leave scope with the RAIM block — retire the aliases
        // so the next epoch's primary buildCarryDdPriors never dereferences a dangling set.
        carry_we_drop_sats = nullptr;
        carry_we_drop_arcs = nullptr;

        // Reduce-once invariant (design §3.5): the carried prior is injected
        // pre-gate, so the single gate reduce() must be the ONLY per-station Schur
        // recompute this epoch — UNLESS the within-epoch adaptation deliberately re-solved
        // (each drop→re-solve is another reduce()).  A second recompute otherwise means a
        // stray post-gate local-block change silently doubled the serial path.
        if (pl_on && !we_adapt_attempted && est.reduceRecomputeCount() != 1) {
            log->error("Network[phase-leveling]: reduce() recomputed {} times this epoch "
                       "(expected exactly 1) — a stray post-gate local-block change doubled "
                       "the serial Schur path.", est.reduceRecomputeCount());
        }

        // --- Within-epoch escape-hatch: withhold rather than emit a still-contaminated epoch
        // The capped adaptation (drop culprit → re-solve, kMaxAdapt passes) may not fully
        // isolate the fault; if σ̂0 STILL exceeds the withhold ceiling the shared solution
        // is too corrupted to publish.  Reuse the observability-gate disposition (no emit,
        // no carry stash, epoch skipped — the contaminated solve never enters the store or
        // the carry) instead of emitting it with only a log warning.  OFF (raim_withhold ≤ 0)
        // is byte-identical: the epoch emits exactly as today.
        //
        // P0-4: gate on σ̂0 ALONE, NOT on we_adapt_attempted.  A DIFFUSE contamination
        // that leaves NO single dominant culprit (kind == Kind::None) never launches a
        // re-solve, so we_adapt_attempted stays false — yet σ̂0 is high, and that is
        // exactly the case the withhold ceiling should catch.  sigma0_adapt is set to the
        // assessed σ̂0 whenever the within-epoch RAIM block runs (whether or not a culprit
        // was dropped) and stays 0 otherwise, so a clean/OFF epoch (sigma0_adapt == 0)
        // still can never trip it.
        // RTA-10 (network.per_const_withhold): per-CONSTELLATION carve-out of the σ̂0
        // withhold.  When the ceiling trips, judge each constellation by its OWN σ̂0_c —
        //
        //   σ̂0_c = max(1, 1.4826 · median{ r_i : i ∈ constellation c's rows })
        //
        // over EXACTLY the rows the global σ̂0 pools (carried-ambiguity PHASE rows of
        // every present non-pivot station, r_i = |v_i|·√w_i, divided by √(redundancy r̄_i)
        // under B2 with its uncheckable-row exclusion), partitioned by the ambiguity's
        // satellite constellation.  Same statistic, same ceiling, restricted support.
        //
        // APPROXIMATION (stated, not hidden): the residuals are evaluated at the JOINT
        // solution — a contamination in constellation A perturbs shared per-station
        // nuisances (receiver clock, ZWD, iono) that leak into B's residuals, and each
        // row's B2 redundancy is its share of the JOINT system's redundancy, not of a
        // per-constellation solve.  σ̂0_c therefore under-separates cross-constellation
        // coupling; the "every constellation individually clean while the global σ̂0
        // tripped" outcome below is the honest edge of that approximation and is
        // published-with-warning + counted (n_perconst_diffuse_published), not silently
        // absorbed.  A constellation also needs >= per_const_withhold_min_rows rows to
        // be JUDGEABLE — fewer cannot certify cleanliness under a contaminated epoch.
        //
        // The RANK/OBSERVABILITY gate above keeps whole-epoch semantics untouched: a
        // rank deficiency means the declared datum itself is invalid — the product
        // would be a meaningless least-norm split, so no "healthy subset" exists to
        // carve out.  This carve-out applies ONLY to the σ̂0 (contamination) withhold.
        bool           perconst_active = false;   // a publishable subset exists this epoch
        std::set<char> perconst_publish;          // constellations to keep in the emission
        std::set<char> perconst_dec_withhold;     // constellations the decision withheld
        std::map<char, NetworkEstimator::PerConstSigma0> perconst_stats;  // σ̂0_c + rows (events/log)
        if (perconst_on && raim_withhold > 0.0 && sigma0_adapt > raim_withhold) {
            for (auto& [cst, pool] : we_sigma0_pool_c) {
                NetworkEstimator::PerConstSigma0 st;
                st.rows = static_cast<int>(pool.size());
                if (!pool.empty()) {   // same median × 1.4826, floor 1 as the global σ̂0
                    std::nth_element(pool.begin(), pool.begin() + pool.size() / 2, pool.end());
                    st.sigma0_c = std::max(raim_s0_floor, 1.4826 * pool[pool.size() / 2]);
                }
                perconst_stats[cst] = st;
            }
            const NetworkEstimator::PerConstWithholdDecision dec =
                NetworkEstimator::decidePerConstWithhold(perconst_stats, raim_withhold,
                                                         perconst_min_rows);
            if (!dec.publish.empty()) {
                perconst_active       = true;
                perconst_publish      = dec.publish;
                perconst_dec_withhold = dec.withhold;
                std::string detail;
                for (const auto& [cst, st] : perconst_stats)
                    detail += fmt::format("{}{}:{:.2f}/{}{}", detail.empty() ? "" : " ", cst,
                                          st.sigma0_c, st.rows,
                                          dec.publish.count(cst) ? "" : "(withheld)");
                log->info("Network (RTA-10): TOW {:.1f} global σ̂0={:.2f} > ceiling {:.2f} — "
                          "per-constellation carve-out publishes {} of {} judged "
                          "constellation(s) [σ̂0_c/rows: {}].",
                          se.time.tow, sigma0_adapt, raim_withhold, dec.publish.size(),
                          perconst_stats.size(), detail);
            }
            // dec.publish empty ⇒ NO constellation is individually publishable — fall
            // through to the whole-epoch withhold below, exactly as without the toggle.
        }
        if (raim_withhold > 0.0 && sigma0_adapt > raim_withhold && !perconst_active) {
            log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} > withhold ceiling {:.2f} "
                      "after {} re-solve(s) — WITHHOLDING epoch (too contaminated to publish).",
                      se.time.tow, sigma0_adapt, raim_withhold, n_adapt);
            emitEvent("SIGMA0", "withhold", "", "", sigma0_adapt, raim_withhold,
                      "skip_epoch", fmt::format("{} re-solve(s)", n_adapt));
            ++n_skipped;
            ++n_sigma0_withheld;   // R8: dedicated count of σ̂0-ceiling withholds (reported in the summary)
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << plan.declared_dimension << ",1,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.sigma0 = sigma0_adapt; es.n_adapt = n_adapt;
              es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("sigma0_withheld", es); }
            // RTA-10: product-level heartbeat for the WITHHELD epoch (H line, no E line)
            // — a live tail can tell "σ̂0 withheld" from "producer died".  Only once the
            // deferred product writer has opened (the first emit); earlier withheld
            // epochs precede the product file itself.  withheld="*" = whole epoch.
            if (emit_epoch_status && product_writer) {
                NetworkProducerStatus hst;
                hst.time.week = se.time.week;
                hst.time.tow  = se.time.tow;
                hst.state     = NetworkProducerStatus::State::WithheldSigma0;
                hst.sigma0    = sigma0_adapt;
                hst.withheld  = "*";
                product_writer->writeEpochStatus(hst);
                if (nc.product_flush_per_epoch) product_writer->flush();
            }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        subEnd(SB_SOLVEGLUE);   // D0b: close solve glue (net removed the internal solve/reduce)
        subBegin(SB_INSTALL);   // D0b: carry_driver.installPosterior (DD posterior mean/cov readback)
        // Card 4 (M1) C3b: install the DD posterior (mean + covariance) back into the
        // carry from the FINAL accepted x_s + datum — the invariant DDs the next epoch
        // projects forward.  POST-ACCEPTANCE (after the observability + σ̂0 withhold
        // gates' skip-paths above), so a withheld/contaminated epoch never poisons the
        // carry; and OUTSIDE the within-epoch RAIM block, so it runs every accepted
        // epoch (not only RAIM-armed ones).  A failure resets the carry (safe cold
        // start).  No-op when OFF.
        if (carry_invariant) {
            // parallel_carry: thread installPosterior's posterior-cov GEMMs.  Run it
            // SERIAL-over-blocks (n_threads=1, its byte-identical-to-serial path) so Eigen's
            // threading does not nest with the seam's per-block OpenMP.
            try { CarryEigenScope es(carry_par, n_threads);
                  carry_driver.installPosterior(est, x_s, plan.shared_datum,
                                                carry_par ? 1 : seam_threads); }
            catch (const std::exception& ex) {
                log->warn("Network[Card4]: DD-carry install failed at TOW {:.1f} ({}); "
                          "carry reset (refloats next epoch).", se.time.tow, ex.what());
                carry_driver.reset();
            }
            carry_prev_tow = se.time.tow;
            // M5 card WL-1: fold the accepted build's WL eligibility tallies into the run
            // totals (this install runs only for an accepted epoch; the last buildCarryDdPriors
            // before the accepted solve set mw_wl_epoch).  Invariant #4: counters or it didn't happen.
            if (mw_wl_on) {
                mw_wl_run.cycles_seen       += mw_wl_epoch.cycles_seen;
                mw_wl_run.rows_built        += mw_wl_epoch.rows_built;
                mw_wl_run.rows_capped       += mw_wl_epoch.rows_capped;
                mw_wl_run.skip_not_primary  += mw_wl_epoch.skip_not_primary;
                mw_wl_run.skip_missing_band += mw_wl_epoch.skip_missing_band;
                mw_wl_run.skip_missing_mw   += mw_wl_epoch.skip_missing_mw;
                mw_wl_run.skip_young_arc    += mw_wl_epoch.skip_young_arc;
                mw_wl_run.skip_bad_lambda   += mw_wl_epoch.skip_bad_lambda;
                mw_wl_run.skip_mixed_frame  += mw_wl_epoch.skip_mixed_frame;
                mw_wl_run.sum_R             += mw_wl_epoch.sum_R;

                // --- F6 one-shot attribution write (log-only; docs/HANDOFF_mw_wl_card.md §6c). ---
                // The accepted epoch's x_s is final here; add reconstructed_abs (the estimator's
                // current WL DD over the same cycle) + the kernel WL-block innovation, then dump.
                if (mw_wl_f6_dump && !mw_wl_f6.written && mw_wl_f6.pending
                    && mw_wl_f6.tow == se.time.tow) {
                    std::map<int, Eigen::VectorXd> xr_of;   // est_station -> local corrections
                    for (const auto& fr : mw_wl_f6.rows)
                        for (const auto& a : fr.arcs)
                            if (a.est_station >= 0 && !xr_of.count(a.est_station)) {
                                try { xr_of[a.est_station] = est.reconstructStation(
                                          static_cast<std::size_t>(a.est_station), x_s); }
                                catch (...) { xr_of[a.est_station] = Eigen::VectorXd(); }
                            }
                    Eigen::VectorXd v_wl;                   // kernel innovation v = z_free − Gᵀx_s − ẑ
                    {
                        Eigen::MatrixXd cov_v_wl;           // (unused; v is cov-free)
                        try {
                            const Eigen::MatrixXd cov0 =
                                Eigen::MatrixXd::Zero(x_s.size(), x_s.size());
                            est.ddPriorInnovation(mw_wl_f6.wl_block_index, x_s, cov0, v_wl, cov_v_wl);
                        } catch (...) { v_wl.resize(0); }
                    }
                    std::error_code ec_f6;
                    std::filesystem::create_directories(log_dir_, ec_f6);
                    std::ofstream f6(log_dir_ / "network_mw_wl_f6.csv");
                    if (f6) {
                        f6 << "# M5 WL-1 F6 attribution dump — first arming epoch (week "
                           << mw_wl_f6.week << ", tow " << fmt::format("{:.1f}", mw_wl_f6.tow)
                           << ", loop_epoch " << mw_wl_f6.loop_epoch << ", wl_block_index "
                           << mw_wl_f6.wl_block_index << ")\n";
                        f6 << "row,n_arcs,z_mw_cyc,z_mw_frac,z_lin_cyc,z_corr_cyc,zhat_block_cyc,"
                              "reconstructed_abs_cyc,recon_minus_zmw_cyc,recon_abs_frac,innovation_v_cyc\n";
                        for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                            const auto& fr = mw_wl_f6.rows[r];
                            double recon = 0.0; bool recon_ok = true;
                            for (const auto& a : fr.arcs) {
                                const auto it = xr_of.find(a.est_station);
                                double xr1 = 0.0, xr2 = 0.0;
                                if (it != xr_of.end()
                                    && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                                    && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                                    xr1 = it->second(a.col1); xr2 = it->second(a.col2);
                                } else { recon_ok = false; }
                                if (a.lambda1 > 0.0 && a.lambda2 > 0.0)
                                    recon += (a.coeff / a.lambda1) * (a.x_lin1 + xr1)
                                           - (a.coeff / a.lambda2) * (a.x_lin2 + xr2);
                            }
                            const double zmw_frac = fr.z_mw - std::round(fr.z_mw);
                            const double vrow = (static_cast<Eigen::Index>(r) < v_wl.size())
                                                    ? v_wl(static_cast<Eigen::Index>(r))
                                                    : std::numeric_limits<double>::quiet_NaN();
                            f6 << r << ',' << fr.arcs.size() << ','
                               << fmt::format("{:.4f}", fr.z_mw) << ','
                               << fmt::format("{:.4f}", zmw_frac) << ','
                               << fmt::format("{:.4f}", fr.z_lin) << ','
                               << fmt::format("{:.4f}", fr.z_corr) << ','
                               << fmt::format("{:.4f}", fr.zhat_block) << ','
                               << (recon_ok ? fmt::format("{:.4f}", recon) : std::string("nan")) << ','
                               << (recon_ok ? fmt::format("{:.4f}", recon - fr.z_mw) : std::string("nan")) << ','
                               << (recon_ok ? fmt::format("{:.4f}", recon - std::round(recon)) : std::string("nan")) << ','
                               << fmt::format("{:.4f}", vrow) << '\n';
                        }
                        f6 << "row,arc,est_station,sat,sig1,sig2,coeff,mw_mean_m,mw_arc_cyc,mw_count,"
                              "mw_sigma_m,bia_corrected,col1,col2,lambda1,lambda2,lambda_wl,"
                              "x_lin1_m,x_lin2_m,xr1_m,xr2_m,n1_abs_cyc,n2_abs_cyc,wl_arc_kf_cyc\n";
                        for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                            const auto& fr = mw_wl_f6.rows[r];
                            for (std::size_t k = 0; k < fr.arcs.size(); ++k) {
                                const auto& a = fr.arcs[k];
                                const auto it = xr_of.find(a.est_station);
                                double xr1 = 0.0, xr2 = 0.0; bool have_xr = false;
                                if (it != xr_of.end()
                                    && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                                    && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                                    xr1 = it->second(a.col1); xr2 = it->second(a.col2); have_xr = true;
                                }
                                const double n1abs = (a.lambda1 > 0.0) ? (a.x_lin1 + xr1) / a.lambda1 : 0.0;
                                const double n2abs = (a.lambda2 > 0.0) ? (a.x_lin2 + xr2) / a.lambda2 : 0.0;
                                const double mw_arc_cyc = (a.lambda_wl > 0.0) ? a.mw_mean_m / a.lambda_wl : 0.0;
                                f6 << r << ',' << k << ',' << a.est_station << ','
                                   << a.sat << ',' << a.sig1 << ',' << a.sig2 << ','
                                   << fmt::format("{:.1f}", a.coeff) << ','
                                   << fmt::format("{:.4f}", a.mw_mean_m) << ','
                                   << fmt::format("{:.4f}", mw_arc_cyc) << ','
                                   << a.mw_count << ','
                                   << fmt::format("{:.4f}", std::sqrt(std::max(a.mw_var_m2, 0.0))) << ','
                                   << (a.bia_corrected ? 1 : 0) << ','
                                   << a.col1 << ',' << a.col2 << ','
                                   << fmt::format("{:.6f}", a.lambda1) << ','
                                   << fmt::format("{:.6f}", a.lambda2) << ','
                                   << fmt::format("{:.6f}", a.lambda_wl) << ','
                                   << fmt::format("{:.4f}", a.x_lin1) << ','
                                   << fmt::format("{:.4f}", a.x_lin2) << ','
                                   << (have_xr ? fmt::format("{:.4f}", xr1) : std::string("nan")) << ','
                                   << (have_xr ? fmt::format("{:.4f}", xr2) : std::string("nan")) << ','
                                   << fmt::format("{:.4f}", n1abs) << ','
                                   << fmt::format("{:.4f}", n2abs) << ','
                                   << fmt::format("{:.4f}", n1abs - n2abs) << '\n';
                            }
                        }
                        mw_wl_f6.written = true;
                        mw_wl_f6.pending = false;
                        log->info("Network (M5/WL-1 F6): wrote {} — first arming epoch tow {:.1f}, "
                                  "{} row(s); attribute z_mw_frac (MW integer-ness) + "
                                  "recon_minus_zmw (state-frame gap).",
                                  (log_dir_ / "network_mw_wl_f6.csv").string(), mw_wl_f6.tow,
                                  mw_wl_f6.rows.size());
                    }
                }
            }
            // M5 card WL-2 EWL lane: same accepted-install accumulation (invariant #4),
            // per lane instance (UWL-1: one per tabled constellation when armed).
            if (ewl_on) {
                for (auto& lane : ewl_lanes) {
                    lane.run.cycles_seen       += lane.epoch.cycles_seen;
                    lane.run.rows_built        += lane.epoch.rows_built;
                    lane.run.rows_fixed        += lane.epoch.rows_fixed;
                    lane.run.rows_fixed_cold   += lane.epoch.rows_fixed_cold;
                    lane.run.rows_cold_unfixed += lane.epoch.rows_cold_unfixed;
                    lane.run.rows_capped       += lane.epoch.rows_capped;
                    lane.run.skip_not_primary  += lane.epoch.skip_not_primary;
                    lane.run.skip_missing_band += lane.epoch.skip_missing_band;
                    lane.run.skip_missing_mw   += lane.epoch.skip_missing_mw;
                    lane.run.skip_young_arc    += lane.epoch.skip_young_arc;
                    lane.run.skip_bad_lambda   += lane.epoch.skip_bad_lambda;
                    lane.run.skip_mixed_frame  += lane.epoch.skip_mixed_frame;
                    lane.run.skip_quarantine   += lane.epoch.skip_quarantine;
                    lane.run.sum_R             += lane.epoch.sum_R;
                }
            }
            // M5 card WL-2 WL lane (§3.3): apply the stashed inject plan against the FRESH
            // posterior (this epoch's NL), THEN fold the run counters.  The plan was decided
            // from DETECTOR statistics (§3.0.1) in the accepted build; here we read the carried
            // lane value + NL sum and, only if the carry still disagrees with the WL integer by
            // more than inject_disagree_min_cyc (self-limiting), set the lane difference to the
            // integer while preserving the phase-determined sum.  injectCarriedChord marks each
            // moved chord D5-exempt for the NEXT install (the §3.3 storm guard) internally.
            if (wl_inject_on || lane_fix_on) {
                for (const auto& pl : wl_inject_epoch) {
                    double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
                    if (pl.pair
                            ? !carry_driver.carriedPairValue(pl.group_l1, pl.chord, z1, v1, z2, v2)
                            : (!carry_driver.carriedChordValue(pl.group_l1, pl.chord, z1, v1) ||
                               !carry_driver.carriedChordValue(pl.group_l2, pl.chord, z2, v2))) {
                        ++wl_run.skip_basis;   // not 1:1 across BOTH bands' carry bases — skip (§3.3)
                        if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis;
                        continue;
                    }
                    const double A1 = (z1 + pl.z_lin1) / pl.lambda1;   // absolute band DDs (cyc)
                    const double A2 = (z2 + pl.z_lin2) / pl.lambda2;
                    const double lane = A1 - A2, s = A1 + A2;          // WL DD + preserved NL sum
                    if (std::abs(pl.w - lane) <= nmw.inject_disagree_min_cyc)
                        continue;   // already lane-consistent (hold/fix row / prior inject) ⇒ no fight
                    const double A1p = 0.5 * (pl.w + s), A2p = 0.5 * (s - pl.w);
                    const double sg1 = nmw.wl_fix_sigma_cyc * pl.lambda1;
                    const double sg2 = nmw.wl_fix_sigma_cyc * pl.lambda2;
                    bool moved = false;
                    if (pl.pair) {
                        // WL-3 fallback: one joint edit, both bands, one D5 exemption (§3.3
                        // "should fire ~never in steady state — telemetry proves it").
                        moved = carry_driver.injectCarriedChordPair(
                            pl.group_l1, pl.chord,
                            A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1,
                            A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
                    } else {
                        const bool i1 = carry_driver.injectCarriedChord(
                            pl.group_l1, pl.chord, A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1);
                        const bool i2 = carry_driver.injectCarriedChord(
                            pl.group_l2, pl.chord, A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
                        moved = i1 && i2;
                    }
                    if (moved) { ++wl_run.injects; wl_run.inject_abs_sum += std::abs(pl.w - lane);
                                 if (uwl_on) {
                                     auto& wcr = wl_cons_run[pl.cons ? pl.cons : '?'];
                                     ++wcr.injects; wcr.inject_abs_sum += std::abs(pl.w - lane);
                                 }
                                 if (mw_tagsets) d5_inject_chords_now.insert(pl.chord); }
                    else       { ++wl_run.skip_basis;
                                 if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis; }
                }
                // Accepted-install accumulation: the pooled epoch tallies (also the whole walk
                // when the UWL card is off) plus, under UWL-1, the per-cons walk tallies —
                // folded into BOTH the aggregate and the per-cons run totals.
                wlAccumulate(wl_run, wl_epoch);
                for (const auto& [wcons, we] : wl_cons_epoch) {
                    wlAccumulate(wl_run, we);
                    wlAccumulate(wl_cons_run[wcons], we);
                }

                // F-B lane trace: write every entry's POST-install carried lane + tags.
                if (mw_lane_trace && !lane_trace_epoch.empty()) {
                    std::map<ArcKey, double> w_of;   // decided cycles' integer targets
                    for (const auto& pl : wl_inject_epoch) w_of[pl.chord] = pl.w;
                    std::error_code ec_lt;
                    std::filesystem::create_directories(log_dir_, ec_lt);
                    std::ofstream ltf(log_dir_ / "network_wl_lane_trace.csv", std::ios::app);
                    if (ltf) {
                        if (!lane_trace_header_written && ltf.tellp() == std::streampos(0))
                            ltf << "week,tow,chord_u,chord_v,station,sat,lane_cyc,frac_cyc,"
                                   "w,decided,held,injected\n";
                        lane_trace_header_written = true;
                        for (const auto& e : lane_trace_epoch) {
                            double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
                            if (lane_on
                                    ? !carry_driver.carriedPairValue(e.g1, e.chord, z1, v1, z2, v2)
                                    : (!carry_driver.carriedChordValue(e.g1, e.chord, z1, v1) ||
                                       !carry_driver.carriedChordValue(e.g2, e.chord, z2, v2)))
                                continue;   // not 1:1 carried in both bands this epoch
                            const double lane = (z1 + e.z_lin1) / e.lam1
                                              - (z2 + e.z_lin2) / e.lam2;
                            const double frac = lane - std::round(lane);
                            const auto wit = w_of.find(e.chord);
                            const std::string sta_name =
                                (e.chord.u >= 0 && e.chord.u < static_cast<int>(stations.size()))
                                    ? stations[static_cast<std::size_t>(e.chord.u)].name
                                    : std::string{};
                            ltf << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                                << ',' << e.chord.u << ',' << e.chord.v << ',' << sta_name
                                << ',' << e.sat << ',' << fmt::format("{:.4f}", lane) << ','
                                << fmt::format("{:.4f}", frac) << ',';
                            if (wit != w_of.end()) ltf << fmt::format("{:.0f}", wit->second);
                            ltf << ',' << (wit != w_of.end() ? 1 : 0) << ','
                                << (d5_hold_chords.count(e.chord) ? 1 : 0) << ','
                                << (d5_inject_chords_now.count(e.chord) ? 1 : 0) << '\n';
                        }
                    }
                }
            }
            const auto& cc = carry_driver.counters();
            carry_epochs       = cc.epochs;
            carry_born_total   = cc.chords_born;
            carry_marg_total   = cc.chords_marginalized;
            carry_merges_total = cc.merges;
            carry_splits_total = cc.splits;
            carry_swaps_total  = cc.tree_swaps;
            carry_zdim_last    = cc.z_dim;
            carry_slipkills_total = cc.slip_kills;
            carry_dropped_total   = cc.chords_dropped;
            carry_innov_flags_total  = cc.innov_flags;
            carry_innov_retaps_total = cc.innov_retaps;
            // D5: fold the post-solve innovation flags into the NEXT epoch's slip funnel.
            // Each (group, chord) maps back to its (station, ambiguity) via the reverse map
            // built with this epoch's blocks; next epoch's buildCarryDdPriors excludes it from
            // the A_old membership so the affected cycle re-taps fresh (the C3c path).
            if (carry_innov_gate) {
                for (const auto& [gk, chord] : carry_driver.lastInnovFlags()) {
                    const auto git = carry_arc_amb_key.find(gk);
                    const auto ait = (git != carry_arc_amb_key.end())
                                         ? git->second.find(chord) : std::map<ArcKey, std::string>::const_iterator{};
                    if (git == carry_arc_amb_key.end() || ait == git->second.end()) {
                        // A flagged chord that the reverse map (built with THIS epoch's blocks)
                        // cannot resolve should never happen — both come from the same
                        // buildCarryDdPriors.  Warn once so a wiring regression is not silent;
                        // the flag is dropped (safe: q_z + next-epoch detection are the backstop).
                        if (++carry_innov_map_miss == 1)
                            log->warn("Network[Card4 D5]: an innovation flag (group {}, arc {}→{}) did "
                                      "not resolve to an ambiguity in the reverse map at TOW {:.1f} — "
                                      "the next-epoch re-tap is skipped for it (reverse-map/block "
                                      "mismatch; investigate if this recurs).",
                                      gk, chord.u, chord.v, se.time.tow);
                        continue;
                    }
                    // The insert RESULT is the de-duplication signal the attribution
                    // needs: carry_innov_pending was cleared at this epoch's head
                    // (the C3c drain), so `.second` is true exactly once per
                    // (station, ambiguity) per epoch.  Under the WL-3 joint pair
                    // block the gate flags per band ROW, so the same (group, chord)
                    // arrives twice and only the first insert is a real re-tap.
                    const bool first_this_epoch =
                        carry_innov_pending.insert({chord.u, ait->second}).second;
                    // Card G(c): make the D5 re-tap counter ATTRIBUTABLE.  The audit's
                    // 1 560 re-taps were un-decomposable — a raw-step coincidence test
                    // at the same (station, satellite, epoch) separates "the gate caught
                    // a real observable event" from "the gate is firing on model
                    // tension".  Instrumentation only; the flag is unchanged either way.
                    // Both units are counted: FLAG ROWS (continuity with the 2026-07-21
                    // headline) and distinct funnel ARCS (what actually re-taps).
                    if (nc.slip_qc.gap_instrument) {
                        ++slip_qc.counters.d5_retaps;
                        if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs;
                        if (const auto pk = StateKeys::parseAmbiguityKey(ait->second))
                            if (slip_qc.epoch.stepped.count({chord.u, pk->satellite})) {
                                ++slip_qc.counters.d5_retaps_stepped;
                                if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs_stepped;
                            }
                    }
                    // WL-3 §3.4: a pair chord's flag refloats BOTH bands next epoch (they
                    // share the graph edge) — feed the band-2 ambiguity into the funnel too.
                    if (lane_on) {
                        const auto g2it = carry_arc_amb_key2.find(gk);
                        if (g2it != carry_arc_amb_key2.end()) {
                            const auto a2it = g2it->second.find(chord);
                            if (a2it != g2it->second.end())
                                carry_innov_pending.insert({chord.u, a2it->second});
                        }
                    }
                }
            }
            // F-A attribution dump (log-only, network.mw_wl.d5_dump): one CSV row per D5
            // flag with adjacency tags.  Uses the SAME reverse map as the funnel above.
            //
            // Card D5-A/D2 (docs/SPEC_slip_qc_followups.md §1): the trailing
            // `raw_stepped` column is the CHEAP EXACT-EPOCH raw-step tag — did the
            // receiver's own MW/GF level step at this (station, satellite) on this
            // epoch?  It is deliberately TRI-STATE, not 0/1:
            //   1     — this (station, sat) is in slip_qc.epoch.stepped (Card G's
            //           empirical per-station gate fired);
            //   0     — a step COULD be formed (adjacent-epoch dual-band predecessor)
            //           and it was flat;
            //   blank — no evidence at all (raw observables not computed this run, arc
            //           start / gap / single-band, or the flag's ambiguity key did not
            //           resolve).  Writing 0 there would be a false "no raw step"
            //           claim on exactly the population D5-A exists to decompose;
            //           absence of evidence is not evidence of absence.
            // The ±1-epoch offline join in scripts/grade_slip_agreement.py --d5 stays
            // the source of truth; this column is the zero-cost exact-epoch tag that
            // makes network_d5_flags.csv self-contained.  Log-only either way: the
            // solve never reads it.
            if (mw_d5_dump && carry_innov_gate) {
                const auto& flags  = carry_driver.lastInnovFlags();
                const auto& sigmas = carry_driver.lastInnovFlagSigmas();
                if (!flags.empty()) {
                    std::error_code ec_d5;
                    std::filesystem::create_directories(log_dir_, ec_d5);
                    std::ofstream d5f(log_dir_ / "network_d5_flags.csv", std::ios::app);
                    if (d5f) {
                        if (!d5_dump_header_written && d5f.tellp() == std::streampos(0))
                            d5f << "week,tow,group,chord_u,chord_v,station,amb_key,std_innov,"
                                   "ewl_stasat,hold_chord,injected_prev,raw_stepped\n";
                        d5_dump_header_written = true;
                        for (std::size_t fi = 0; fi < flags.size(); ++fi) {
                            const auto& [gk, chord] = flags[fi];
                            std::string amb, sat;
                            const auto git = carry_arc_amb_key.find(gk);
                            if (git != carry_arc_amb_key.end()) {
                                const auto ait = git->second.find(chord);
                                if (ait != git->second.end()) amb = ait->second;
                            }
                            if (!amb.empty())
                                if (const auto parts = StateKeys::parseAmbiguityKey(amb))
                                    sat = parts->satellite;
                            const std::string sta_name =
                                (chord.u >= 0 && chord.u < static_cast<int>(stations.size()))
                                    ? stations[static_cast<std::size_t>(chord.u)].name
                                    : std::string{};
                            // D2 tri-state raw-step tag (see the banner above).  Reads
                            // the SAME per-epoch evidence the run-end d5_retaps_stepped
                            // counter uses, so the CSV and the counter cannot disagree.
                            std::string raw_stepped;
                            if (slip_qc.epoch.computed && !sat.empty() && chord.u >= 0
                                && static_cast<std::size_t>(chord.u)
                                       < slip_qc.epoch.verdict.size()) {
                                if (slip_qc.epoch.stepped.count({ chord.u, sat })) {
                                    raw_stepped = "1";
                                } else {
                                    const auto& vmap =
                                        slip_qc.epoch.verdict[static_cast<std::size_t>(chord.u)];
                                    const auto vit = vmap.find(sat);
                                    if (vit != vmap.end() && vit->second.measured)
                                        raw_stepped = "0";
                                }
                            }
                            d5f << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                                << ',' << gk << ',' << chord.u << ',' << chord.v << ','
                                << sta_name << ',' << amb << ','
                                << fmt::format("{:.2f}",
                                       fi < sigmas.size() ? sigmas[fi] : 0.0) << ','
                                << (d5_ewl_stasat.count({ chord.u, sat }) ? 1 : 0) << ','
                                << (d5_hold_chords.count(chord) ? 1 : 0) << ','
                                << (d5_inject_chords_prev.count(chord) ? 1 : 0) << ','
                                << raw_stepped << '\n';
                        }
                    }
                }
            }
            if (mw_tagsets) {   // rotate: this epoch's applied injects tag NEXT install's flags
                d5_inject_chords_prev.swap(d5_inject_chords_now);
                d5_inject_chords_now.clear();
            }
        }
        subEnd(SB_INSTALL);     // D0b: close installPosterior (the Card-4 posterior readback)
        subBegin(SB_NETAR);     // D0b: producer-side network AR Phase-2 (log-only; OFF in tier)

        // --- Producer-side network AR (Phase 2: DD fixability attempt, LOG ONLY) ---
        // The between-station double-difference ambiguities are datum-INVARIANT
        // estimable quantities, so this fix attempt is independent of the phase-bias
        // datum in use.  Phase 2 is READ-ONLY (no re-pin / re-solve yet — that is
        // Phase 3): it reconstructs the per-station float ambiguities, forms an
        // independent DD basis (star topology per (const,signal)), builds the exact
        // DD covariance from est.jointConditionalCovariance, converts to cycles, and
        // runs ils::ilsFix to log how many integers the network COULD fix.  Gated on
        // network.ambiguity_resolution.enable + min_epochs; byte-identical when off.
        if (netar.enable && n_emitted >= static_cast<size_t>(std::max(0, netar.min_epochs))
            && x_s.size() > 0 && ar_ppp_utils) {
            struct ArRec { double value_m; std::size_t est_id; Eigen::Index col; };
            // (const,signal) -> sat -> station index -> absolute float ambiguity + its
            // (est id, A_r column) for the covariance request.  GPS+Galileo only.
            std::map<std::pair<std::string, std::string>,
                     std::map<std::string, std::map<std::size_t, ArRec>>> ar_pool;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                Eigen::VectorXd x_r;
                try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s); }
                catch (...) { continue; }
                const auto& h2l = asm_res[i].h2local;
                const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    const std::string cst = NetworkSbasis::constellationOf(parts->satellite);
                    if (cst != "G" && cst != "E") continue;   // GPS+Gal integer-capable; GLONASS FDMA stays float
                    if (stations[i].sm->isAmbiguityUnresolvable(key)) continue;
                    const int amb_idx = static_cast<int>(sidx);
                    if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                    const auto hit = h2l.find(amb_idx);
                    if (hit == h2l.end() || hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                    const double value_m = x_lin(amb_idx) + x_r(hit->second);
                    if (!std::isfinite(value_m)) continue;
                    ar_pool[{cst, parts->signal}][parts->satellite][i] =
                        ArRec{value_m, static_cast<std::size_t>(est_index_of[i]),
                              static_cast<Eigen::Index>(hit->second)};
                }
            }
            // Independent DD basis: per (const,signal), a reference sat (tracked at the
            // most stations) and a reference station (tracking ref-sat + the most sats);
            // one DD per (other station r, other sat j) that both r and ref-station track.
            std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
            std::map<std::pair<std::size_t, Eigen::Index>, int> req_idx;
            auto reqOf = [&](const ArRec& r) -> int {
                const std::pair<std::size_t, Eigen::Index> k{r.est_id, r.col};
                auto it = req_idx.find(k);
                if (it != req_idx.end()) return it->second;
                const int idx = static_cast<int>(reqs.size());
                reqs.push_back(k); req_idx[k] = idx; return idx;
            };
            struct DDrow { int i_rj, i_rp, i_fj, i_fp; double dd_m, lambda; };
            std::vector<DDrow> dds;
            int n_const_signal = 0;
            for (auto& [cs, satmap] : ar_pool) {
                std::string ref_sat; std::size_t best = 0;
                for (auto& [sat, sm] : satmap)
                    if (sm.size() > best) { best = sm.size(); ref_sat = sat; }
                if (ref_sat.empty() || satmap[ref_sat].size() < 2) continue;
                std::size_t ref_i = 0, ref_cnt = 0; bool have_ref = false;
                for (auto& [sta_i, rec] : satmap[ref_sat]) {
                    (void)rec;
                    std::size_t cnt = 0;
                    for (auto& [sat, sm] : satmap) if (sm.count(sta_i)) ++cnt;
                    if (!have_ref || cnt > ref_cnt) { ref_cnt = cnt; ref_i = sta_i; have_ref = true; }
                }
                if (!have_ref) continue;
                const System sys = (cs.first == "G") ? System::GPS : System::Galileo;
                const double lambda = ar_ppp_utils->getWavelength(sys, cs.second, ref_sat);
                if (!(std::isfinite(lambda) && lambda > 0.0)) continue;
                const ArRec& fp = satmap[ref_sat].at(ref_i);   // ref station, ref sat
                ++n_const_signal;
                for (auto& [sat, sm] : satmap) {
                    if (sat == ref_sat) continue;
                    const auto fj_it = sm.find(ref_i);          // ref station, sat j
                    if (fj_it == sm.end()) continue;
                    for (const auto& [sta_i, rj] : sm) {
                        if (sta_i == ref_i) continue;
                        const auto rp_it = satmap[ref_sat].find(sta_i);   // station r, ref sat
                        if (rp_it == satmap[ref_sat].end()) continue;
                        const double dd_m = (rj.value_m - rp_it->second.value_m)
                                          - (fj_it->second.value_m - fp.value_m);
                        dds.push_back(DDrow{ reqOf(rj), reqOf(rp_it->second),
                                             reqOf(fj_it->second), reqOf(fp), dd_m, lambda });
                    }
                }
            }
            if (!dds.empty() && reqs.size() >= 2) {
                Eigen::MatrixXd cov_undiff;
                try { cov_undiff = est.jointConditionalCovariance(reqs, plan.shared_datum); }
                catch (...) { cov_undiff = Eigen::MatrixXd(); }
                if (cov_undiff.rows() == static_cast<Eigen::Index>(reqs.size())
                    && cov_undiff.rows() > 0) {
                    const int nd = static_cast<int>(dds.size());
                    Eigen::MatrixXd D = Eigen::MatrixXd::Zero(nd, static_cast<int>(reqs.size()));
                    Eigen::VectorXd y_cyc(nd);
                    std::vector<double> lam(nd);
                    for (int a = 0; a < nd; ++a) {
                        const DDrow& d = dds[a];
                        D(a, d.i_rj) += 1.0; D(a, d.i_rp) -= 1.0;
                        D(a, d.i_fj) -= 1.0; D(a, d.i_fp) += 1.0;
                        y_cyc(a) = d.dd_m / d.lambda;
                        lam[a] = d.lambda;
                    }
                    Eigen::MatrixXd cov_dd_m = D * cov_undiff * D.transpose();
                    Eigen::MatrixXd Q_cyc(nd, nd);
                    for (int a = 0; a < nd; ++a)
                        for (int b = 0; b < nd; ++b)
                            Q_cyc(a, b) = cov_dd_m(a, b) / (lam[a] * lam[b]);
                    Q_cyc = 0.5 * (Q_cyc + Q_cyc.transpose());
                    // --- ILS dimension cap (network.ambiguity_resolution.ils_max_dim) ---
                    // MLAMBDA's search does not return on the uncapped full-network basis
                    // (~2500 DDs at 305 stations, Jul-15).  When armed (> 0), search only
                    // the ils_max_dim lowest-formal-σ DDs — the standard partial-AR
                    // subset (ascending Q_aa) — and leave the rest float.  The summary
                    // statistics (med σ / med frac / near-int) stay FULL-basis so the
                    // convergence gauge is unaffected by the cap.
                    Eigen::VectorXd y_fix = y_cyc;
                    Eigen::MatrixXd Q_fix = Q_cyc;
                    int nd_fix = nd;
                    if (netar.ils_max_dim > 0 && nd > netar.ils_max_dim) {
                        nd_fix = netar.ils_max_dim;
                        std::vector<int> order(static_cast<std::size_t>(nd));
                        std::iota(order.begin(), order.end(), 0);
                        std::sort(order.begin(), order.end(), [&](int a, int b) {
                            return Q_cyc(a, a) < Q_cyc(b, b);
                        });
                        y_fix.resize(nd_fix);
                        Q_fix.resize(nd_fix, nd_fix);
                        for (int a = 0; a < nd_fix; ++a) {
                            y_fix(a) = y_cyc(order[static_cast<std::size_t>(a)]);
                            for (int b = 0; b < nd_fix; ++b)
                                Q_fix(a, b) = Q_cyc(order[static_cast<std::size_t>(a)],
                                                    order[static_cast<std::size_t>(b)]);
                        }
                    }
                    ils::IlsGateConfig gates;
                    gates.min_success_rate     = netar.min_success_rate;
                    gates.ratio_threshold      = netar.ratio_threshold;
                    gates.ratio_upper_bound    = netar.ratio_upper_bound;
                    gates.nl_max_per_pair_frac = netar.nl_max_per_pair_frac;
                    gates.enable_partial_ar    = netar.enable_partial_ar;
                    ils::IlsResult fix;
                    try { fix = ils::ilsFix(y_fix, Q_fix, gates); }
                    catch (...) { fix = ils::IlsResult(); }
                    // Median DD formal σ (cycles) — the convergence gauge: as the
                    // network float tightens toward cm this drops and the DDs become
                    // fixable.  Also the worst float-fraction (evidence the products
                    // are integer-consistent even before they are confidently fixable).
                    std::vector<double> dsig; dsig.reserve(nd);
                    std::vector<double> dfrac; dfrac.reserve(nd);
                    int n_near = 0;   // DDs whose float is within 0.15 cyc of an integer
                    for (int a = 0; a < nd; ++a) {
                        if (Q_cyc(a, a) > 0.0) dsig.push_back(std::sqrt(Q_cyc(a, a)));
                        const double f = std::abs(y_cyc(a) - std::round(y_cyc(a)));
                        dfrac.push_back(f);
                        if (f <= 0.15) ++n_near;
                    }
                    auto med = [](std::vector<double>& v) {
                        if (v.empty()) return 0.0;
                        std::sort(v.begin(), v.end());
                        return v[v.size() / 2];
                    };
                    const double med_sig  = med(dsig);
                    const double med_frac = med(dfrac);
                    log->info("Network AR (Phase 2, log-only): TOW {:.1f} — {} DD / {} undiff-amb / "
                              "{} (const,signal); fixed {}{} of {} searched{} (ratio {:.2f}, "
                              "p_succ {:.4f}, med_DD_sigma {:.4f} cyc, med_DD_frac {:.3f}, "
                              "near_int {}/{})",
                              se.time.tow, nd, static_cast<int>(reqs.size()), n_const_signal,
                              fix.ok ? static_cast<int>(fix.kept.size()) : 0,
                              fix.partial ? " partial" : "", nd_fix,
                              nd_fix < nd ? " (ILS-capped)" : "", fix.ratio, fix.p_succ,
                              med_sig, med_frac, n_near, nd);
                }
            }
        }

        // --- One-shot full-arrowhead NEQ export (network.neq_export_epoch) -----------
        // Post-solve READ-ONLY dump of the assembled normal equations for offline
        // spectral / weak-direction attribution: border N_ss (raw + reduced) + RHS,
        // every station's N_sr / N_rr (rebuilt from its LDLᵀ) / u_r as row-major
        // little-endian double .bin, plus shared-column and per-station local-column
        // labels.  Solution is untouched; fires on exactly one epoch per run.
        if (nc.neq_export_epoch > 0
            && n_emitted + 1 == static_cast<size_t>(nc.neq_export_epoch)
            && x_s.size() > 0) {
            const std::filesystem::path dir = log_dir_ / "neq_export";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            auto appendBin = [](std::ofstream& f, const Eigen::MatrixXd& M) {
                // Explicit row-major copy so the offline reader needs no
                // storage-order convention (Eigen's default is column-major).
                const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic,
                                    Eigen::RowMajor> R = M;
                f.write(reinterpret_cast<const char*>(R.data()),
                        static_cast<std::streamsize>(sizeof(double) * R.size()));
            };
            auto sanitize = [](std::string s) {
                for (char& ch : s) if (ch == ',') ch = ';';
                return s;
            };
            const auto& kernel = est.normals();
            {
                std::ofstream f(dir / "Nss_raw.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.sharedNormals());
            }
            {
                std::ofstream f(dir / "us_raw.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.sharedRhs());
            }
            {
                std::ofstream f(dir / "Nss_red.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.reducedSharedNormals());
            }
            {
                std::ofstream f(dir / "us_red.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.reducedSharedRhs());
            }
            {
                std::ofstream f(dir / "shared_labels.csv", std::ios::trunc);
                f << "col,label\n";
                for (Eigen::Index c = 0; c < est.numShared(); ++c)
                    f << c << ',' << sanitize(est.sharedColumnLabel(c)) << '\n';
            }
            std::ofstream sidx(dir / "stations_index.csv", std::ios::trunc);
            sidx << "est_slot,name,n_local\n";
            std::ofstream slab(dir / "stations_labels.csv", std::ios::trunc);
            slab << "est_slot,local_col,label\n";
            std::ofstream nsr(dir / "stations_Nsr.bin", std::ios::binary | std::ios::trunc);
            std::ofstream nrr(dir / "stations_Nrr.bin", std::ios::binary | std::ios::trunc);
            std::ofstream urf(dir / "stations_ur.bin", std::ios::binary | std::ios::trunc);
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const auto r = static_cast<std::size_t>(est_index_of[i]);
                const Eigen::Index nl = kernel.numLocal(r);
                sidx << r << ',' << stations[i].name << ',' << nl << '\n';
                appendBin(nsr, kernel.stationCrossBlock(r));
                appendBin(nrr, kernel.stationLocalNormals(r));
                appendBin(urf, kernel.stationLocalRhs(r));
                // Local col → StateManager label via the assembly's h2local inverse;
                // columns with no SM mapping (none expected) stay "?".
                std::vector<std::string> names(static_cast<std::size_t>(nl), "?");
                const auto sm_labels = stations[i].sm->getStateLabels();
                for (const auto& kv : asm_res[i].h2local) {
                    const auto lc = static_cast<Eigen::Index>(kv.second);
                    if (lc >= 0 && lc < nl
                        && static_cast<std::size_t>(kv.first) < sm_labels.size())
                        names[static_cast<std::size_t>(lc)] =
                            sm_labels[static_cast<std::size_t>(kv.first)];
                }
                for (Eigen::Index c = 0; c < nl; ++c)
                    slab << r << ',' << c << ','
                         << sanitize(names[static_cast<std::size_t>(c)]) << '\n';
            }
            {
                std::ofstream meta(dir / "meta.csv", std::ios::trunc);
                meta << "week,tow,epoch,n_shared,n_stations\n"
                     << se.time.week << ',' << se.time.tow << ','
                     << nc.neq_export_epoch << ',' << est.numShared() << ','
                     << kernel.numStations() << '\n';
            }
            log->info("Network: NEQ export written to {} (epoch {}, n_shared {}, "
                      "{} station blocks)",
                      dir.string(), nc.neq_export_epoch, est.numShared(),
                      kernel.numStations());
        }

        subEnd(SB_NETAR);      // D0b: close producer-AR / pre-emit tail
        subBegin(SB_SOLPREP);  // D0b: toSolutionEpoch + per-sat discontinuity signalling
        NetworkSolPrepResult sp_ = networkSolPrep(ctx, config_, log, traces, setup, se, est, x_s,
                                                  plan, pl_now_s, datum_changed, changed_consts,
                                                  sigma0_adapt, perconst_active, perconst_publish,
                                                  perconst_dec_withhold, perconst_stats);
        auto& sol = sp_.sol;
        auto& epoch_withheld_consts = sp_.epoch_withheld_consts;
        auto& global_bump = sp_.global_bump;
        auto& serialized_consts = sp_.serialized_consts;
        auto& retro_emit_sats = sp_.retro_emit_sats;
        subEnd(SB_SOLPREP);     // D0b: close sol-prep + discontinuity signalling
        subBegin(SB_EMITSTORE); // D0b block 7: re-base + PB-emit filter + store push + solution CSV
        networkEmitStore(ctx, config_, log, traces, setup, se, est, x_s, pl_now_s,
                         datum_changed, sigma0_adapt, sol, epoch_withheld_consts, global_bump,
                         serialized_consts, retro_emit_sats, emit_rebase_on, emit_pb_filter_on,
                         pb_filter_emit_sigma, emit_rebase_engine, pb_filter_engine, present,
                         est_index_of, modeled, shared_phase_keys, carry_slipped_ambs,
                         we_t2_opp, we_t2_flag, cl_epochs, cl_ar_ran, cl_ar_fixed, cl_sum_fixed,
                         cl_first_fix_tow, cl_first_fix_week, cl_recent_err3d);
        subEnd(SB_EMITSTORE);   // D0b: close block 7 (emit/store, order-sensitive serial)
        subBegin(SB_IONODIAG);  // D0b block 4a: iono / ZWD / anchor-info viz traces
        networkIonoDiag(ctx, config_, log, traces, setup, ws, se, est, x_s, plan, asm_res,
                        est_index_of, modeled, registered_clock_sats, clk_anchor_sigma);
        subEnd(SB_IONODIAG);   // D0b: close block 4a (iono/ZWD/anchor traces)
        subBegin(SB_DDFRAC);   // D0b block 6: cross-station DD fractional-residual (B5) trace
        networkDdFrac(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                      est_index_of, dd_ppp_utils);
        subEnd(SB_DDFRAC);       // D0b: close block 6 (dd-frac B5)
        subBegin(SB_SHDEV_VTEC); // D0b block 4b: T5 SH-deviation carry KF + VTEC coeff trace
        networkShDevVtec(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                         est_index_of, modeled, iono_deweight, sh_dev_on);
        subEnd(SB_SHDEV_VTEC);  // D0b: close block 4b (T5 sh-dev + VTEC trace)
        subBegin(SB_T1A);       // D0b block 3: T1a SH health probe + fused adaptive-deweight gate
        networkShHealth(ctx, config_, log, traces, setup, se, est, x_s, asm_res, est_index_of,
                        modeled, iono_deweight, carry_vce_est, datum_changed, pl_now_s,
                        first_vce_s, last_vce_hour);
        subEnd(SB_T1A);        // D0b: close block 3 (T1a health + deweight)
        subBegin(SB_LEVELING); // D0b block 5: phase-leveling carry harvest + T1d SH-convergence
        networkLeveling(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                        est_index_of, carry_vce_est, carry_warm, pl_now_s,
                        registered_clock_sats, registered_code_keys, shared_phase_keys);
        subEnd(SB_LEVELING);   // D0b: close block 5 (leveling + T1d SH-convergence)
        subBegin(SB_RAIM);     // D0b block 2: post-fit network RAIM + median-polish attribution
        if (raim_on && carry_warm) {
            networkPostFitRaim(ctx, config_, log, traces, se,
                               stations, asm_res, est_index_of, present, x_s, est,
                               plan.shared_datum, clock_refloat_sats, retro_flag_sats,
                               retro_feed, n_arc_refloat_epoch);
        }
        subEnd(SB_RAIM);       // D0b: close block 2 (RAIM)
        subBegin(SB_CLOCKUPD); // D0b block 8: two-state clock update + fault QC + retro-flag feed
        networkClockQc(ctx, config_, log, traces, ws, se, est, x_s,
                       registered_clock_sats, datum_changed, changed_consts,
                       clk_model_warm, emit_rate_warm, pl_now_s, dt,
                       clock_refloat_sats, retro_flag_sats, retro_feed);
        subEnd(SB_CLOCKUPD);       // D0b: close block 8 (clock update + retro feed)
        subBegin(SB_STASH_STATUS); // D0b: D2/C1/SH carry re-stash + VCE fold + epoch-status QC
        networkStashStatus(ctx, config_, log, traces, setup, se, est, x_s, plan, carry_vce_est,
                           clock_refloat_sats, n_arc_refloat_epoch, registered_clock_sats,
                           registered_code_keys, retro_emit_sats, shared_phase_keys,
                           sigma0_adapt, total_pins, vce_wants_cb, vce_wants_pb, dt,
                           datum_changed, changed_consts, n_clock, n_present, n_pb, n_cb,
                           n_dropped_sats, n_adapt);
        subEnd(SB_STASH_STATUS);   // D0b: close the stash + epoch-status tail (before the readout)

        // D0: per-epoch section-timer readout (emitted-epoch path).  Deltas of the
        // estimator's run-cumulative accumulators against the loop-top snapshot give
        // THIS epoch's kernel cost.  The driver assembly wall (sec_asm_ms_epoch) covers
        // the primary produce+scatter section; reduce/solve/cov-inverse/cov-consumers
        // happen after the wall, so `other` (QC decision logic + emit + I/O + any
        // within-epoch RAIM re-solve) = total − assembly − reduce − solve − cov-inverse
        // − cov-consumers.  d_estadd is the kernel est.addStation delta — a CROSS-CUTTING
        // figure (it also captures re-solve adds that fall AFTER the wall, so it is NOT a
        // strict sub-part of the assembly wall) reported separately, not nested under it.
        if (sec_timers_on) {
            const auto& stmr = est.sectionTimers();
            const double d_total  = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sec_epoch_t0).count();
            const double d_reduce = stmr.reduce_ms        - sec_t0.reduce_ms;
            const double d_solve  = stmr.solve_ms         - sec_t0.solve_ms;
            const double d_covinv = stmr.cov_inverse_ms   - sec_t0.cov_inverse_ms;
            const double d_covcon = stmr.cov_consumers_ms - sec_t0.cov_consumers_ms;
            const double d_estadd = stmr.add_station_ms   - sec_t0.add_station_ms;
            const std::size_t d_covcalls =
                stmr.cov_consumer_calls - sec_t0.cov_consumer_calls;
            const double d_other = d_total - sec_asm_ms_epoch - d_reduce - d_solve
                                   - d_covinv - d_covcon;
            ++sec_profiled_epochs;
            log->info("Network (D0) TOW {:.1f}: total {:.1f} ms | assembly-wall {:.1f} "
                      "| reduce {:.1f} | solve {:.1f} | cov-inv {:.1f} | cov-consumers {:.1f} "
                      "({} calls) | other {:.1f} | est.addStation {:.1f} (cross-cutting) | "
                      "{} shared cols, {} stations",
                      se.time.tow, d_total, sec_asm_ms_epoch, d_reduce, d_solve,
                      d_covinv, d_covcon, d_covcalls, d_other, d_estadd,
                      static_cast<long long>(est.numShared()), n_present);

            // D0b: per-epoch sub-block attribution of `other`.  net = wall − Δ(timed
            // sections) inside each block; Σ net should approach d_other, and the residual
            // is logged as "unattributed" (loop regions this instrument does not bracket).
            std::string d0b;
            double d0b_net_sum = 0.0;
            for (int b = 0; b < SB_COUNT; ++b) {
                const double net = (sec_sub[b].wall_ms  - sec_sub_ep0[b].wall_ms)
                                 - (sec_sub[b].timed_ms - sec_sub_ep0[b].timed_ms);
                d0b_net_sum += net;
                d0b += fmt::format("{}={:.1f} ", kSubNames[b], net);
            }
            log->info("Network (D0b) TOW {:.1f}: other-net ms [ {}] Σnet {:.1f} | other {:.1f} "
                      "| unattributed {:.1f}",
                      se.time.tow, d0b, d0b_net_sum, d_other, d_other - d0b_net_sum);
        }

        // RTB-7(a) periodic atomic snapshot (INC-4: body in NetworkRunContext.cpp;
        // stays AFTER the prev_week/prev_tow advance — snap captures THIS epoch).
        networkWriteStateSnapshot(ctx, config_, se.time, pl_now_s, emitter, log);
    }

    if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);  // restore Eigen's own threading

    double loop_ms = 0.0;   // L3/L15: hoisted out of the timing block so the run-summary CSV / manifest can read it
    {  // [TIMING] per-epoch scaling measurement (loop wall only; startup excluded)
        loop_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_loop_begin).count();
        log->info("Network[timing]: epoch-loop wall {:.1f} ms over {} epochs = {:.2f} ms/epoch "
                  "| {} stations | {} | threads {}",
                  loop_ms, n_loop_epochs,
                  n_loop_epochs ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0,
                  stations.size(), parallel_on ? "PARALLEL" : "SERIAL", n_threads);
    }

    // === Tier D0: section-timer RUN TOTALS ===================================
    // The whole-run breakdown that decides which Tier-D lever matters.  The kernel
    // accumulators are run-cumulative (assembly / reduce / solve / cov-inverse /
    // cov-consumers); `other` = the epoch-loop wall minus those + the driver
    // assembly wall (QC decision logic, emit, I/O, tracers).  Only the ratios are
    // load-bearing — absolute ms depend on the machine and the parallel settings.
    //
    // On the card's "QC tiers" section: the RAIM / attribution / clock-monitor tiers
    // are not a wall-separable phase — they are interleaved with and dominated by
    // covariance-consumer calls (rowRedundancy / reconstructStation), which ARE timed
    // and attributed to cov-consumers above; their residual decision logic (median
    // polish, standardization, thresholding) is cheap and falls into `other`.  So the
    // D1-relevant QC cost is already isolated in cov-consumers — bracketing the
    // scattered QC blocks would only double-count those covariance calls.
    if (sec_timers_on) {
        const auto& stmr = est.sectionTimers();
        const double other_ms = loop_ms - sec_asm_wall_ms - stmr.reduce_ms - stmr.solve_ms
                                - stmr.cov_inverse_ms - stmr.cov_consumers_ms;
        auto pct = [&](double ms) {
            return loop_ms > 0.0 ? 100.0 * ms / loop_ms : 0.0;
        };
        log->info("Network (D0) section-timer RUN TOTALS over {} profiled / {} loop epochs "
                  "(epoch-loop wall {:.0f} ms):", sec_profiled_epochs, n_loop_epochs, loop_ms);
        log->info("Network (D0)   assembly-wall {:>9.0f} ms ({:4.1f}%)  [primary produce+scatter]",
                  sec_asm_wall_ms, pct(sec_asm_wall_ms));
        log->info("Network (D0)   reduce        {:>9.0f} ms ({:4.1f}%)  [{} recomputes]",
                  stmr.reduce_ms, pct(stmr.reduce_ms), stmr.reduce_recomputes);
        log->info("Network (D0)   solve         {:>9.0f} ms ({:4.1f}%)  [{} solves]",
                  stmr.solve_ms, pct(stmr.solve_ms), stmr.solve_calls);
        log->info("Network (D0)   cov-inverse   {:>9.0f} ms ({:4.1f}%)  [D1 selected-inverse target]",
                  stmr.cov_inverse_ms, pct(stmr.cov_inverse_ms));
        log->info("Network (D0)   cov-consumers {:>9.0f} ms ({:4.1f}%)  [{} calls; D1 per-station target]",
                  stmr.cov_consumers_ms, pct(stmr.cov_consumers_ms), stmr.cov_consumer_calls);
        log->info("Network (D0)   other         {:>9.0f} ms ({:4.1f}%)  [QC / emit / I/O / tracers]",
                  other_ms, pct(other_ms));
        // Cross-cutting: kernel est.addStation (normal-forming + N_rr factorization).  Mostly
        // inside the assembly wall, but also captures within-epoch RAIM re-solve adds that fall
        // after the wall (in `other`), so it is NOT a strict sub-part of the assembly-wall bucket.
        log->info("Network (D0)   est.addStation {:>8.0f} ms ({:4.1f}%)  [{} adds; cross-cutting — "
                  "overlaps assembly-wall + any re-solve in other]",
                  stmr.add_station_ms, pct(stmr.add_station_ms), stmr.add_station_calls);
        // P0b: foldReducedDdPriors internal breakdown (serial-fold path only, i.e. under
        // parallel_carry) — which sub-part of the 49%-of-epoch DD fold dominates.
        if (stmr.fold_loop_ms + stmr.fold_minv_ms + stmr.fold_gmg_ms > 0.0)
            log->info("Network (D0)   foldDdPriors: per-station-loop {:>8.0f} ms ({:4.1f}%) | "
                      "M-inverse {:>8.0f} ms ({:4.1f}%) | GMGᵀ {:>7.0f} ms ({:4.1f}%)  [serial-fold path]",
                      stmr.fold_loop_ms, pct(stmr.fold_loop_ms),
                      stmr.fold_minv_ms, pct(stmr.fold_minv_ms),
                      stmr.fold_gmg_ms, pct(stmr.fold_gmg_ms));

        // === INC-C0: the invariant-carry driver's per-op split + census =========
        // Splits the SB_DDBUILD / SB_INSTALL wall brackets (which lump the whole driver)
        // into the primitive ops the perf-restructure card ranks — the dense-K² change-of-
        // basis projection (buildDdPriors) and posterior read-back (installPosterior) that
        // INC-C2's factored (A + U Σ_ss Uᵀ) store removes.  The census reconciles the
        // "K≈7834 never marginalized" finding (§0): whether marginalization has nothing to
        // kill (giant live components, case (a)) or the kill path is silent (case (b)).
        if (carry_invariant) {
            const auto& dp = carry_driver.perfTimers();
            log->info("Network (D0/INC-C0) carry-driver per-op over {} build / {} serial-install "
                      "epoch(s): basis-project {:>8.0f} ms ({:4.1f}%) | post-mean {:>7.0f} ms "
                      "({:4.1f}%) | post-cov {:>8.0f} ms ({:4.1f}%) | D5-gate {:>6.0f} ms ({:4.1f}%) "
                      "| time-update {:>5.0f} ms  [basis-project + post-cov = the dense-K² ops INC-C2 factors]",
                      dp.build_epochs, dp.install_epochs,
                      dp.basis_project_ms, pct(dp.basis_project_ms),
                      dp.post_mean_ms, pct(dp.post_mean_ms),
                      dp.post_cov_ms, pct(dp.post_cov_ms),
                      dp.d5_gate_ms, pct(dp.d5_gate_ms), dp.time_update_ms);
            // LF-1 INC-0: the WL-3 lane-fold split — the ~10³ per-epoch rank-1/rank-2 covariance
            // touches the perf card exists to shrink, which SB_DDBUILD lumps into basis-project.
            log->info("Network (D0/LF-1) lane-fold per-op: applyLaneMeasurement {:>8.0f} ms ({:4.1f}%) "
                      "over {} row(s) | applyLaneFix {:>8.0f} ms ({:4.1f}%) over {} row(s)  "
                      "[measurement = irreducible O(K²)/row dense downdate; fix = the ~65%-decided "
                      "SET INC-1 makes O(K)].",
                      dp.lane_meas_ms, pct(dp.lane_meas_ms), dp.lane_meas_calls,
                      dp.lane_fix_ms, pct(dp.lane_fix_ms), dp.lane_fix_calls);
            const auto& cc = carry_driver.counters();
            const long long marg_total = cc.chords_marginalized + cc.chords_marg_perchord;
            const long long chords_folded = cc.chords_carried + cc.chords_born;
            log->info("Network (D0/INC-C0) carry census: max K_c {} (single largest component) | "
                      "peak z-dim {} | marginalizations {} ({} per-chord + {} whole-block) | "
                      "births/tap-offs {} | slip refloats {}.",
                      cc.max_component_kc, cc.max_z_dim, marg_total,
                      cc.chords_marg_perchord, cc.chords_marginalized,
                      cc.chords_born, cc.slip_kills);
            // LF-1 INC-0: emitted-component fold-dim distribution (SPEC §7.4 — INC-4's Amdahl
            // ceiling ≈ heaviest component's work / total.  A single tall high bucket ⇒ one
            // giant G component dominates; a spread ⇒ the component-parallel fold balances).
            log->info("Network (D0/LF-1) K_c histogram (emitted components by fold-dim, log2 "
                      "buckets 0|1|2|4|8|16|32|64|128|256+): {} {} {} {} {} {} {} {} {} {}",
                      cc.comp_kc_hist[0], cc.comp_kc_hist[1], cc.comp_kc_hist[2], cc.comp_kc_hist[3],
                      cc.comp_kc_hist[4], cc.comp_kc_hist[5], cc.comp_kc_hist[6], cc.comp_kc_hist[7],
                      cc.comp_kc_hist[8], cc.comp_kc_hist[9]);
            // INC-C0 D-c viability: short fundamental cycles ⇒ sparse DD information matrix
            // J = Pᶻ⁻¹ ⇒ the exact info-form carry (SPEC_carry_factored_pz_C2 D-c) is bounded;
            // long cycles ⇒ dense J ⇒ D-c gives no bound (choose D-b/component-cap instead).
            log->info("Network (D0/INC-C0) D-c viability: max fundamental-cycle length {} | "
                      "mean {:.1f} over {} folded chord(s) — {} (short ⇒ sparse J ⇒ D-c bounded; "
                      "long ⇒ dense J ⇒ dense per-block cost is intrinsic).",
                      cc.max_cycle_len,
                      chords_folded ? static_cast<double>(cc.sum_cycle_len)
                                      / static_cast<double>(chords_folded) : 0.0,
                      chords_folded,
                      cc.max_cycle_len <= 8 ? "SPARSE-J likely" : "DENSE-J risk");
            // INC-C1 growth bound (also a 12 h robustness assert): a large, still-growing carry
            // with ZERO marginalization over a long run means dead chords are being retained —
            // the §0 case (b) kill-path bug.  A large per-chord count instead means case (a).
            if (marg_total == 0 && cc.max_z_dim > 0 && cc.epochs > 100)
                log->warn("Network (INC-C1 growth bound): {} carried DD(s) at peak with ZERO "
                          "marginalization(s) over {} driven epoch(s) — dead chords are NOT being "
                          "killed (INC-C0 §0 case (b)); K grows unbounded.  Audit the kill path "
                          "(NetworkInvariantCarryDriver::buildDdPriors projection-W column drop).",
                          cc.max_z_dim, cc.epochs);
        }

        // === Tier D0b: `other` sub-block RUN TOTALS, ranked =================
        // net = wall − Δ(reduce+solve+cov-inverse+cov-consumers) inside each block, so the
        // recon/solve already in the five D0 sections above is not double-counted.  Σ net vs
        // the D0 `other` figure closes the accounting; "unattributed" = loop regions no
        // bracket covers (pre-slip pivot pass, the trace/readout tail, small comment gaps).
        // This is the ranked table the perlos card acts on (HANDOFF_perlos_parallel_sweeps
        // §3 gate P0): P1/P2 target the measured top blocks, not §2's guesses.
        std::vector<int> d0b_order(SB_COUNT);
        for (int b = 0; b < SB_COUNT; ++b) d0b_order[b] = b;
        std::sort(d0b_order.begin(), d0b_order.end(), [&](int a, int c) {
            return (sec_sub[a].wall_ms - sec_sub[a].timed_ms)
                 > (sec_sub[c].wall_ms - sec_sub[c].timed_ms);
        });
        double d0b_sum_net = 0.0;
        for (const SubBlockTimer& s : sec_sub) d0b_sum_net += s.wall_ms - s.timed_ms;
        log->info("Network (D0b) `other` sub-block RUN TOTALS over {} profiled epoch(s) "
                  "(ranked by net; net excludes recon/solve already in the D0 sections):",
                  sec_profiled_epochs);
        for (int b : d0b_order) {
            const SubBlockTimer& s = sec_sub[b];
            const double net = s.wall_ms - s.timed_ms;
            log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  | wall {:>9.0f} ms "
                      "| cov-consumers {:>8.0f} ms ({} calls)",
                      kSubNames[b], net, pct(net), s.wall_ms, s.covcon_ms, s.covcalls);
        }
        log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  [Σ over blocks]",
                  "TOTAL", d0b_sum_net, pct(d0b_sum_net));
        log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  [D0 `other` − Σ; loop "
                  "regions no bracket covers]", "unattributed",
                  other_ms - d0b_sum_net, pct(other_ms - d0b_sum_net));
    }

    // Card DG5/DG6: the run-wide rank-gate telemetry rides INSIDE the one artifact
    // a trace-armed run is allowed to add (network_estimability.csv), not as a new
    // run-end log line — the estim golden arm byte-gates the log TEXT, and an
    // unarmed control run must stay text-identical while still yielding the
    // arm-vs-arm numbers the storm analyzer compares.  No-op with the trace off.
    traces.writeEstimabilityRunRow(ctx.counters, est.rankGateTol());
    traces.closeAll();   // INC-4: the exact former 14-stream close list

    log->info("Network producer complete: {} epochs emitted, {} skipped "
              "({} of those withheld by the observability gate, {} by the A4 σ̂0 ceiling) — "
              "of {} synced.",
              n_emitted, n_skipped, n_gate_withheld, n_sigma0_withheld, synced.size());
    // Card DG6 — rank-gate calibration telemetry.  Gated on the tolerance being
    // ARMED (non-default) so a default run's log text is byte-identical, per DG5's
    // "with the trace on, the new FILE is the only artifact delta".  The same
    // numbers reach every trace-armed run through network_estimability.csv's RUN
    // record, so the control arm is not left dark.  Standing invariant #4: an
    // armed calibration whose counters never move is a MISWIRE, not a quiet run.
    if (config_.networkRankGateTol() != NetworkEstimator::kNullSpaceTol)
        log->info("Network (Card DG): rank gate evaluated on {} epoch(s) at tol={:g} — {} "
                  "emitted, {} withheld UNDER-specified (observed > declared: a missing "
                  "datum constraint), {} withheld OVER-specified (observed < declared: the "
                  "tolerance sits below THIS config's gauge depth, so the declared gauges "
                  "stopped being counted — RE-CALIBRATE, do not chase the datum).  Observed "
                  "near-null count over the run: min {}, max {} (declared dim is the "
                  "target).  [Placement rule: cutoff >= 2 decades above the gauge depth and "
                  ">= 2 decades below the weakest-real layer, read off this config's own "
                  "network_estimability.csv.]",
                  ctx.counters.n_gate_evaluated, config_.networkRankGateTol(),
                  n_emitted, ctx.counters.n_gate_withheld_under,
                  ctx.counters.n_gate_withheld_over,
                  ctx.counters.nullspace_obs_min, ctx.counters.nullspace_obs_max);
    if (config_.networkCbKaulaPrior())
        log->info("Network (Card KP): admitted-OSB physical-bound prior folded on {} epoch(s), "
                  "{} column-epoch(s) total at sigma {:g} m.  [Invariant #4: an ARMED run "
                  "reporting 0 epochs never reached a solve with the prior — check that "
                  "network.estimate_sat_code_bias is on and that code columns registered.]",
                  ctx.counters.n_kp_epochs, ctx.counters.n_kp_cols,
                  config_.networkCbKaulaSigmaM());
    if (config_.networkCbAdmission())
        log->info("Network (CB-ADM): {} code pairs admitted (shared columns), {} "
                  "quarantined at census, {} late-quarantined (first seen post-census) "
                  "— census {} (active={}).  [Invariant #4: a BDS-armed ON run with 0 "
                  "quarantined-at-census is a MISWIRE.]  Card H2 orphan window: {} "
                  "(key, epoch) pair(s) where an ADMITTED code key was per-epoch-withheld "
                  "(< 2 present stations) while its same-signal PHASE column stayed shared "
                  "— the deliberate cost of keeping the phase co-gate at CENSUS "
                  "granularity; expected 0 at 305-station density, and only actionable if "
                  "it is nonzero AND coincident with gate withholds.",
                  ctx.counters.n_cb_admitted, ctx.counters.n_cb_quarantined_census,
                  ctx.counters.n_cb_late_quarantined,
                  ctx.admission.active ? "ran" : "did-not-run", ctx.admission.active,
                  ctx.counters.n_cbadm_phase_orphan_key_epochs);
    // --- Slip-QC hardening run-end telemetry (docs/SPEC_slip_qc_hardening.md) ---
    // Standing invariant #4: a card whose counters are all zero on an armed run
    // is a MISWIRE, not a quiet run.  Card E's line is unconditional under
    // mw_detect because reporting only refloats is the L3 defect it fixes.
    if (config_.networkCycleSlipMwDetect()) {
        const auto& qcc = slip_qc.counters;
        log->info("Network (slip-QC card E / MW-detect): {} wide-lane slip detection(s) "
                  "({} refloated) + {} LLI detection(s) ({} refloated) over the run.  "
                  "Detections are counted whether the carry was warm or COLD — a cold run "
                  "still feeds the Card-4 C3c funnel, and reporting only refloats is what "
                  "made that wiring look dead.  [MW ↔ IF agreement is now readable from "
                  "network_events.csv alone: scripts/grade_slip_agreement.py]",
                  qcc.mw_detections, qcc.mw_refloats,
                  qcc.lli_detections, qcc.lli_refloats);
    }
    if (nc.slip_qc.lli_corroborate) {
        const auto& qcc = slip_qc.counters;
        log->info("Network (slip-QC card L / LLI corroboration): {} LLI assertion(s) seen — "
                  "{} corroborated by an MW step, {} by a GF step, {} with no measurable step, "
                  "{} asserted OFF the primary dual-frequency pair (MW/GF structurally cannot "
                  "witness those — widen the primitive if this is large); {} SUPPRESSED as "
                  "uncorroborated at a non-credible station.  Station-epoch credibility: "
                  "{} credible / {} corroboration-required.  No station, satellite or "
                  "observation was excluded from estimation — only the SLIP ACTION was "
                  "qualified.", qcc.lli_asserted, qcc.lli_corr_mw, qcc.lli_corr_gf,
                  qcc.lli_unmeasurable, qcc.lli_offpair, qcc.lli_suppressed,
                  qcc.lli_credible_epochs, qcc.lli_demoted_epochs);
        // M4: the per-run top-offenders line — the audit's STK2/ISHI/MCIL/CCJ2 list
        // must be reproducible from the log alone, without the CSV traces.
        std::vector<std::pair<long long, std::size_t>> offenders;
        for (std::size_t si = 0; si < slip_qc.hist.size(); ++si)
            if (slip_qc.hist[si].lli_suppressed > 0)
                offenders.emplace_back(slip_qc.hist[si].lli_suppressed, si);
        std::sort(offenders.begin(), offenders.end(),
                  [&](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return stations[a.second].name < stations[b.second].name;
                  });
        if (offenders.empty()) {
            log->info("Network (slip-QC card L): no station's LLI was demoted this run "
                      "(every assertion was corroborated or came from a credible station).");
        } else {
            std::string top;
            for (std::size_t k = 0; k < offenders.size() && k < 8; ++k) {
                const auto& h = slip_qc.hist[offenders[k].second];
                top += fmt::format("{}{} {}/{} suppressed (LLI-frac EWMA {:.2f}, {} demotion(s), "
                                   "{} promotion(s), now {})",
                                   top.empty() ? "" : "; ",
                                   stations[offenders[k].second].name,
                                   h.lli_suppressed, h.lli_asserted, h.lli_frac_ewma,
                                   h.lli_demotions, h.lli_promotions,
                                   h.lli_credible ? "credible" : "corroboration-required");
            }
            log->info("Network (slip-QC card L) TOP LLI OFFENDERS ({} station(s) demoted): {}",
                      offenders.size(), top);
        }
    }
    if (nc.slip_qc.gap_instrument) {
        const auto& qcc = slip_qc.counters;
        // H2 (spec-followups N3): the two counting levels are LABELLED at every
        // number.  RESUMPTIONS are per (station, satellite) re-entry; ARCS are per
        // (station, satellite, signal) — a dual-band satellite returning once is
        // 1 resumption and 2 independently bridgeable arcs, which is why the arc
        // totals exceed the resumption totals.  Both arc ledgers are closed
        // (bridged + fresh) so no reader has to infer a missing cell.
        log->info("Network (slip-QC card G / gap instrumentation): {} RESUMPTION(s) "
                  "[unit: (station,satellite) re-entry after ≥1 missed epoch] = {} SHIFTED "
                  "(raw MW/GF level moved across the break) + {} measured-and-unshifted + {} "
                  "UNMEASURABLE (no pre-gap dual-band level to compare).  Bridge "
                  "classification [unit: ARCS = (station,satellite,signal); a dual-band "
                  "resumption contributes 2]: SHIFTED → {} arc(s) re-entered with BRIDGED "
                  "carry state (THE POISON CASE) + {} fresh = {}; unshifted → {} bridged (the "
                  "intended behaviour) + {} fresh = {}; UNMEASURABLE → {} bridged (state rode "
                  "a break nobody could check — unknown, not clean) + {} fresh = {}.  D5 "
                  "attribution: {} FLAG ROW(s) of which {} coincident with a raw step, = {} "
                  "distinct funnel arc(s) re-tapped ({} raw-step-coincident) — the row count "
                  "double-counts a pair chord flagged on BOTH bands (WL-3 joint block), the "
                  "arc count is what actually re-taps; both use the exact-epoch evidence the "
                  "network_d5_flags.csv raw_stepped column reports.  [G3 decision "
                  "rule: shifted-AND-bridged ≈ 0 on a warm graded run ⇒ the mechanism is "
                  "DEFERRED WITH DATA — record this number, and read it together with the "
                  "unmeasurable-bridged count; nonzero ⇒ spec the follow-up card that marks "
                  "such arcs slipped at re-entry.]",
                  qcc.gap_resumptions, qcc.gap_shifted,
                  qcc.gap_resumptions - qcc.gap_shifted - qcc.gap_unmeasurable,
                  qcc.gap_unmeasurable,
                  qcc.gap_shifted_bridged, qcc.gap_shifted_fresh,
                  qcc.gap_shifted_bridged + qcc.gap_shifted_fresh,
                  qcc.gap_clean_bridged, qcc.gap_clean_fresh,
                  qcc.gap_clean_bridged + qcc.gap_clean_fresh,
                  qcc.gap_unmeas_bridged, qcc.gap_unmeas_fresh,
                  qcc.gap_unmeas_bridged + qcc.gap_unmeas_fresh,
                  qcc.d5_retaps, qcc.d5_retaps_stepped,
                  qcc.d5_retap_ambs, qcc.d5_retap_ambs_stepped);
    }
    if (nc.slip_qc.ref_gate) {
        IfPhaseSlipDetector::RefGateCounters rg;
        for (const auto& st : stations) {
            if (!st.if_slip) continue;
            const auto& c = st.if_slip->refGateCounters();
            rg.legacy_candidates      += c.legacy_candidates;
            rg.fires                  += c.fires;
            rg.suppressed_scale       += c.suppressed_scale;
            rg.suppressed_thin        += c.suppressed_thin;
            rg.suppressed_persistence += c.suppressed_persistence;
            rg.fires_station_event    += c.fires_station_event;
            rg.reselects              += c.reselects;
        }
        log->info("Network (slip-QC card R / IF-TD reference gate): {} legacy candidate(s) "
                  "(|median| > {:.2f}·λ_IF with n≥2) → {} fired ({} of them as an incoherent "
                  "multi-satellite STATION EVENT — reference arc reset, median NOT rebased).  "
                  "Suppressed: {} by the empirical scale / coherence test, {} thin-family "
                  "deferral(s) (< {} DDs), {} by the consecutive-fire cap; {} forced reference "
                  "re-selection(s).  [candidates − fires is exactly what the hardening removed.]",
                  rg.legacy_candidates, nc.slip_qc.ref_floor_frac, rg.fires,
                  rg.fires_station_event, rg.suppressed_scale, rg.suppressed_thin,
                  nc.slip_qc.ref_min_n, rg.suppressed_persistence, rg.reselects);
    }
    if (config_.networkPhaseDatumEq34())
        log->info("Network (Card 3/eq34): {} (sat,σ) re-reference(s), {} local carry "
                  "value(s) erased, {} carry value(s) S-transformed (v2-shift), "
                  "{} gap re-pin(s) (I2 extend-only: arcs resumed to their pre-gap "
                  "integer), {} pin/gauge-mismatch epoch(s) over the run.",
                  eq34_reref_total, eq34_erase_total, eq34_shift_total,
                  eq34_gap_repin_total, eq34_mismatch_epochs);
    if (carry_invariant)
        log->info("Network (Card 4/M1 invariant carry): {} epoch(s) driven, {} carried DD(s) at "
                  "run end; {} chord birth(s), {} marginalization(s), {} slip refloat(s), "
                  "{} unresolvable-cycle drop(s), {} tree swap(s) (basis re-expression), "
                  "{} component merge(s), {} split(s) over the run "
                  "(S-system-invariant DDs; q_z={} m²/s).",
                  carry_epochs, carry_zdim_last, carry_born_total, carry_marg_total,
                  carry_slipkills_total, carry_dropped_total,
                  carry_swaps_total, carry_merges_total, carry_splits_total,
                  config_.networkCarryInvariantQz());
    if (sh_dev_on)
        log->info("Network (Iono-2/T5v2 per-LOS deviation carry): {} warm harvest(s), "
                  "{} cold seed(s), {} slip reset(s) (C3c funnel), {} no-information "
                  "harvest skip(s), {} deviation(s) carried at run end (τ={} s, "
                  "σ_ss={} TECU vertical, exact posterior harvest).",
                  ctx.carry.shdev_warm_total, ctx.carry.shdev_cold_total,
                  ctx.carry.shdev_slip_resets_total, ctx.carry.shdev_nodata_total,
                  sh_dev_carry.size(), shc.rep_error_tau_s,
                  shc.representation_sigma_tecu);
    if (mw_wl_on) {
        const double mean_R = mw_wl_run.rows_built > 0
            ? mw_wl_run.sum_R / static_cast<double>(mw_wl_run.rows_built) : 0.0;
        log->info("Network (M5/WL-1 MW-seeded wide-lane): {} WL row(s) folded over {} primary "
                  "cycle(s) inspected (mean R = {:.4f} cyc², i.e. σ ≈ {:.3f} cyc); {} capped by "
                  "max_rows_per_epoch.  Skips — {} non-primary group, {} missing L2 band, {} no "
                  "MW arc (channel/pivot), {} young arc (< {} ep), {} bad λ, {} mixed bias "
                  "frame.  Cross-check the KF-WL circR vs scripts/mw_census.py (Phase-A success "
                  "= circR climbing toward ~0.6 as arcs mature; SPEC §7.5).",
                  mw_wl_run.rows_built, mw_wl_run.cycles_seen, mean_R, std::sqrt(mean_R),
                  mw_wl_run.rows_capped, mw_wl_run.skip_not_primary, mw_wl_run.skip_missing_band,
                  mw_wl_run.skip_missing_mw, mw_wl_run.skip_young_arc,
                  config_.getNetworkConfig().mw_wl.min_arc_epochs, mw_wl_run.skip_bad_lambda,
                  mw_wl_run.skip_mixed_frame);
    }
    if (ewl_on) {
        // UWL-1: one line per lane instance — per-constellation when the card is armed
        // (a pooled line hides which lane is storming; an armed constellation that
        // decided 0 cycles is a LOUD 0-line, invariant #4), the legacy pooled line
        // otherwise (lane.cons == 0).
        for (const auto& lane : ewl_lanes) {
            const auto& er = lane.run;
            const double mean_R = er.rows_built > 0
                ? er.sum_R / static_cast<double>(er.rows_built) : 0.0;
            const char* label = lane.cons == '\0' ? "all-cons"
                              : lane.cons == 'G'  ? "GPS EWL (L2,L5) λ 5.861 m"
                              : lane.cons == 'E'
                                  ? (lane.top == '8'
                                         ? (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5,E5a) λ 19.537 m [preferred]"
                                                : "Galileo UWL (E5,E5a) λ 19.537 m")
                                         : (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5b,E5a) λ 9.768 m [fallback]"
                                                : "Galileo UWL (E5b,E5a) λ 9.768 m"))
                                  : "?";
            log->info("Network (M5/WL-2 {} lane): {} row(s) decided ({} INTEGER-FIXED via FFR, {} "
                      "cold-fixed n<2, {} cold-unfixed; rows {} into the solve) over "
                      "{} primary cycle(s) inspected (mean R = {:.4f} cyc²); {} capped.  Skips — {} "
                      "non-primary, {} missing band, {} no MW arc, {} young arc (< {} ep), {} bad "
                      "λ, {} mixed frame, {} x_lin quarantine.  Uncarried top band; the integers feed "
                      "the WL bridge from the STATISTIC; success = fixes by ep ~{}.",
                      label, er.rows_built + er.rows_fixed_cold, er.rows_fixed,
                      er.rows_fixed_cold, er.rows_cold_unfixed,
                      config_.getNetworkConfig().mw_wl.ewl_fold_rows ? "FOLDED" : "NOT folded",
                      er.cycles_seen, mean_R,
                      er.rows_capped, er.skip_not_primary, er.skip_missing_band,
                      er.skip_missing_mw, er.skip_young_arc,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs, er.skip_bad_lambda,
                      er.skip_mixed_frame, er.skip_quarantine,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs + 1);
        }
    }
    if (wl_inject_on) {
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-2 WL lane, phase bridge + inject): {} EWL-fixed row(s) bridged → "
                  "{} WL cycle(s) decided (mean bridge R = {:.4f} cyc², σ ≈ {:.3f} cyc, iono-de-biased); "
                  "{} hold row(s) folded, {} inject(s) (mean |Δlane| = {:.2f} cyc).  Skips — {} no/young "
                  "bridge arc, {} no iono estimate, {} bridge σ>{} guard, {} FFR-unfixed, {} MW(1,2) "
                  "closure, {} x_lin quarantine, {} not-1:1 basis.  Bridge = phase-only, DD-iono removed "
                  "via the network estimate; residual estimate-error gated by FFR |frac| + MW(1,2) "
                  "closure (§3.4 v2); inject moves the carried lane to the integer, NL preserved (§3.3).",
                  wl_run.rows_seen, wl_run.cycles_decided, mean_R, std::sqrt(mean_R),
                  wl_run.holds_folded, wl_run.injects, med_dl,
                  wl_run.skip_bridge_missing, wl_run.skip_no_iono, wl_run.skip_bridge_wide,
                  config_.getNetworkConfig().mw_wl.bridge_max_sigma_cyc,
                  wl_run.skip_not_fixed, wl_run.skip_closure, wl_run.skip_quarantine,
                  wl_run.skip_basis);
    }
    if (uwl_on && (wl_inject_on || lane_on)) {
        // UWL-1: the per-constellation WL-walk breakdown (never pooled-only — SPEC §2).
        for (const auto& [wcons, wr] : wl_cons_run) {
            const double mean_R = wr.cycles_decided > 0
                ? wr.sum_bridge_R / static_cast<double>(wr.cycles_decided) : 0.0;
            const double med_dl = wr.injects > 0
                ? wr.inject_abs_sum / static_cast<double>(wr.injects) : 0.0;
            log->info("Network (M5/UWL-1 '{}' WL walk): {} row(s) bridged → {} decided (mean "
                      "bridge R = {:.4f} cyc², {} via COLD bridge, {} via E6 MID hop), {} hold(s), "
                      "{} lane fix(es) moved + {} re-pinned, {} inject(s) (mean |Δlane| = {:.2f} "
                      "cyc).  Skips — {} no/young bridge, {} no-iono, {} bridge-wide, {} "
                      "FFR-unfixed, {} closure, {} quarantine, {} basis; mid path — {} FFR "
                      "fallback, {} MW(6,5) closure, {} direct-disagree (skipped).",
                      wcons, wr.rows_seen, wr.cycles_decided, mean_R, wr.cold_bridge_cycles,
                      wr.mid_decided,
                      wr.holds_folded, wr.lane_fix_folded, wr.lane_fix_unchanged,
                      wr.injects, med_dl,
                      wr.skip_bridge_missing, wr.skip_no_iono, wr.skip_bridge_wide,
                      wr.skip_not_fixed, wr.skip_closure, wr.skip_quarantine, wr.skip_basis,
                      wr.mid_fallback, wr.mid_closure, wr.mid_disagree);
            // Increment 5 (uwl_dual_top): the dual-rung + sibling-closure breakdown.
            // mean|frac| ≈ 0 ⇒ the 2·N(8,5)=N(7,5) identity holds on this data;
            // ≈ 0.5 ⇒ a receiver half-cycle convention offsets it (learn a constant
            // BEFORE arming uwl_sib_gate — SPEC reconciliation note 2).
            if (nmw.uwl_dual_top
                && (wr.top2_decided || wr.dual_dedupe || wr.sib_checked
                    || wr.sib_unavail || wr.sib_blocked))
                log->info("Network (M5/UWL-1 '{}' dual-top): {} of {} decided via the "
                          "FALLBACK (E5b,E5a) rung, {} fallback row(s) deduped (preferred "
                          "already decided).  Sibling closure: {} checked (mean |frac| = "
                          "{:.3f}), {} off-integer, {} unmeasurable, {} BLOCKED (gate {}).",
                          wcons, wr.top2_decided, wr.cycles_decided, wr.dual_dedupe,
                          wr.sib_checked,
                          wr.sib_checked > 0
                              ? wr.sib_abs_frac_sum / static_cast<double>(wr.sib_checked)
                              : 0.0,
                          wr.sib_int_off, wr.sib_unavail, wr.sib_blocked,
                          nmw.uwl_sib_gate ? "ON" : "off/measure-only");
        }
    }
    if (lane_on) {
        // WL-3 run-end line (invariant #4: counters or it didn't happen).  In the LANE
        // shape the interesting numbers are the continuous increments + fold-on-change
        // ratio + the inject-RETIREMENT count (≈0 expected — the WL-2 fallback path).
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-3 lane-coordinate carry): {} pair component(s) reconciled "
                  "({} pair arc(s), {} residual primary-band arc(s), {} tree-tag mismatch(es), "
                  "{} spanning-forest completion(s)); "
                  "lane increments folded — {} MW(1,2) row(s), {} bridge row(s), {} skipped "
                  "(immature/stale/mixed/slip/quarantine), {} pinned-skipped (LF-1 INC-2).  "
                  "FFR lane fixing: {} decided "
                  "cycle(s) (mean bridge R = {:.4f} cyc²) → {} integer(s) MOVED/new, {} "
                  "re-pinned (idempotent lane SET — information-once-only).  Inject FALLBACK: "
                  "{} fired (mean |Δlane| = {:.2f} cyc; ≈0 expected — continuous tightening "
                  "supersedes the WL-2 injects), {} not-1:1 basis skips.",
                  carry_driver.counters().pair_components,
                  lane_pair_arcs, lane_resid_arcs, lane_pair_tree_mismatch,
                  carry_driver.counters().pair_tree_promotions,
                  wl_run.lane_mw_rows, wl_run.lane_bridge_rows, wl_run.lane_skip_meas,
                  wl_run.lane_pinned_skips,
                  wl_run.cycles_decided, mean_R,
                  wl_run.lane_fix_folded, wl_run.lane_fix_unchanged,
                  wl_run.injects, med_dl, wl_run.skip_basis);
    }
    if (carry_innov_gate)
        log->info("Network (Card 4/M1 D5 fine-slip innovation gate): {} carried-DD innovation "
                  "flag(s), {} fresh re-tap(s) over the run (k={}; the estimation-side reset for "
                  "the fine slips the coarse MW/LLI/IF-TD layer is relaxed against — each flagged "
                  "chord re-taps data-only + marks its arc slipped for the next epoch).",
                  carry_innov_flags_total, carry_innov_retaps_total,
                  config_.networkCarryInvariantInnovK());
    if (carry_innov_gate && config_.networkCarryInvariantInnovCalibrate())
        log->info("Network (Card 4/M1 D5 v2 LS-VCE calibration): final empirical inflation σ̂₀ = "
                  "{:.2f}× (the gate threshold is k·σ̂₀ = {:.1f} formal-σ; σ̂₀>1 quantifies how "
                  "optimistic the formal Cov(v) is — the M2 monitor's finding, now closing the loop "
                  "on the gate itself).", carry_driver.innovInflationSigma0(),
                  config_.networkCarryInvariantInnovK() * carry_driver.innovInflationSigma0());
    if (idw_on) {
        std::string idw_list;
        for (const auto& nm : iono_deweight.everDeweighted()) {
            if (!idw_list.empty()) idw_list += ' ';
            idw_list += nm;
        }
        if (idw.adaptive)
            log->info("Network (iono deweight): {} station(s) ever deweighted (peak {} "
                      "simultaneous), {} adaptive engage / {} release event(s) over the run "
                      "(static w_s={:.3f}; adaptive gate {:.2f}σ↑ / {:.2f}σ↓ → floor {:.3f}; "
                      "network-relative robust z-score, chronic >3σ background removed by the "
                      "median-centering).{}{}",
                      iono_deweight.everDeweighted().size(), iono_deweight.peakDeweighted(),
                      iono_deweight.triggerEvents(), iono_deweight.releaseEvents(),
                      iono_deweight.staticWeight(), idw.sigma_hi, idw.sigma_lo, idw.floor,
                      idw_list.empty() ? "" : "  Stations: ", idw_list);
        else
            log->info("Network (iono deweight v1): {} station(s) statically deweighted at "
                      "w_s={:.3f} for the whole run — the SH-iono coupling was relaxed while "
                      "they kept serving the shared clocks/biases/datum/position.{}{}",
                      iono_deweight.staticCount(), iono_deweight.staticWeight(),
                      idw_list.empty() ? "" : "  Stations: ", idw_list);
    }
    if (carry_innov_gate && carry_innov_map_miss > 0)
        log->warn("Network (Card 4/M1 D5): {} innovation flag(s) over the run did not resolve in the "
                  "flag→ambiguity reverse map (next-epoch re-tap skipped for them) — a reverse-map/"
                  "block wiring inconsistency; investigate.", carry_innov_map_miss);
    if (emit_rebase_on) {
        // Card 4 (M1) phase D: the emit-layer re-base telemetry (spec §3) — the
        // integer cycles + common-fractional shift removed onto the previous emission
        // frame (an S-selection on OUTPUTS ONLY; user DDs are invariant to it).  Feeds
        // the consumer A/B: high integer/frac churn with a silent disc counter is the
        // datum-freedom the carry buys; a surviving disc step is a genuine event.
        const auto& rb = emit_rebase_engine.counters();
        log->info("Network (Card 4/M1 phase D emit re-base): {} emit epoch(s), {} component "
                  "re-base(s), {} integer cycle(s) removed across {} key(s), {} reference-path "
                  "slip step(s) preserved; mean |common-fractional δ| = {:.4f} m over the run "
                  "(OUTPUTS-only S-selection; DDs invariant).",
                  rb.epochs, rb.components, rb.integer_cycles, rb.keys_rebased,
                  rb.slips_preserved,
                  rb.frac_shift_n ? rb.frac_shift_abs_sum / static_cast<double>(rb.frac_shift_n)
                                  : 0.0);
    }
    if (emit_pb_filter_on) {
        // PB emission-continuity filter telemetry (M5 prerequisite).  A high
        // smoothed:reset ratio with a small mean posterior σ is the win — the
        // per-epoch noise absorbed into the filter, so the emitter jump test fires
        // (via the reset raw-value steps) only at genuine events.  ref-path + gap
        // resets are the structural (never-laundered) events; innovation resets are
        // the calibrated k·σ̂_step genuine steps.  ON-path counters (inv #4).
        const auto& pf = pb_filter_engine.counters();
        const long long resets = pf.resets_refpath + pf.resets_innovation + pf.resets_gap;
        log->info("Network (M5 prereq / PB emission-continuity filter): {} emit epoch(s), "
                  "{} smoothed / {} reset ({} ref-path slip, {} innovation, {} gap) key-epoch(s), "
                  "{} first-sighting init(s); mean posterior σ = {:.4f} m (self-tuning k·σ̂_step "
                  "gate, k={}; OUTPUTS-only — the solve, user DDs and dd-frac grading are "
                  "byte-unchanged; the emitter jump test stays the sole disc authority).",
                  pf.epochs, pf.smoothed, resets, pf.resets_refpath, pf.resets_innovation,
                  pf.resets_gap, pf.inits,
                  pf.smoothed ? pf.post_sigma_sum / static_cast<double>(pf.smoothed) : 0.0,
                  config_.networkEmitPbFilterJumpK());
        if (config_.networkEmitPbFilterGateV2()) {   // PB-gate-v2 merge point (run-end telemetry)
            std::string sig;
            for (const auto& kv : pb_filter_engine.stepSigmas())
                sig += fmt::format("{}{}={:.4f}", sig.empty() ? "" : " ",
                                   kv.first.empty() ? "pool" : kv.first, kv.second);
            log->info("Network (PB filter GATE v2): {} row(s) fed to σ̂_step (censoring-free: "
                      "smoothed + innovation-gated), converged σ̂_step [m] {}; model-scaled gate "
                      "k·√(Ppred+R), k={:.1f} — rate-transferable (one k for 30 s & 300 s).",
                      pf.fed_rows, sig.empty() ? std::string("(none)") : sig,
                      config_.networkEmitPbFilterJumpK());
        }
    }
    if (clk_runaway_on && n_runaway_epochs > 0)
        log->warn("Network (RT0-5): {} epoch(s) dropped at least one constellation for clock "
                  "runaway (magnitude ceiling {:.0f} m / rate ceiling {:.2f} m/s).",
                  n_runaway_epochs, clk_runaway_mag, clk_runaway_rate);
    if (n_healed_epochs > 0)
        log->info("Network: prioritized-datum drop-and-heal recovered the gate on {} epoch(s) "
                  "({} near-null phase-bias column(s) soft-anchored).",
                  n_healed_epochs, n_healed_cols);
    // Card PA2 — the heal's ATTEMPTS, which the success-only line above hides.
    // Gated on attempts so a run that never reaches the gate-failure path (every
    // healthy config, incl. the whole golden matrix) prints nothing new.
    if (ctx.counters.n_heal_attempt_epochs > 0) {
        // The insufficiency caveat is the whole reason the ATTEMPT counters exist,
        // but it is only TRUE when the heal fired more than it rescued.  Printing
        // it when attempts == recoveries (a fully successful heal — the PA4 emit
        // case) would read as a warning on a clean run.
        const bool insufficient = ctx.counters.n_heal_attempt_epochs > n_healed_epochs;
        log->info("Network (Card PA): drop-and-heal FIRED on {} epoch(s), soft-anchoring {} "
                  "near-null phase-bias column(s); {} epoch(s) were RECOVERED by it ({} "
                  "column(s)).{}  Per-column attribution (sat/signal/eigenvalue/span) is in "
                  "network_events.csv under tier GATE, event phase_heal, when "
                  "network.trace_events is armed.",
                  ctx.counters.n_heal_attempt_epochs, ctx.counters.n_heal_attempt_cols,
                  n_healed_epochs, n_healed_cols,
                  insufficient
                      ? "  Attempts > recoveries means the heal is reaching the right columns "
                        "but cannot resolve the residual alone on some epochs — that is PA5's "
                        "insufficiency criterion, not a dead heal."
                      : "  Every fired heal recovered the gate.");
    }
    if (carry_cpl_joint && n_cpl_joint_fallback > 0)
        log->warn("Network (Card 1b): {} station-epoch(s) had a non-SPD joint coupling-row "
                  "covariance after the ridge retry and fell back to the Card-1 per-row scalar "
                  "marginalization for that station (the between-row DD correlation is not "
                  "recovered on those). Investigate if the count is a large fraction of "
                  "station-epochs.", n_cpl_joint_fallback);
    if (carry_cpl_cond)
        log->info("Network (Card 1/1d): coupling-carry volume — stashes: {} with a regression, "
                  "{} EMPTY-regression (diagonal degrade), {} harvest failures; injected terms: "
                  "{} present, {} departed, {} kappa-shrunk (time-update {}).",
                  n_cpl_stash, n_cpl_stash_empty, n_cpl_harvest_fail,
                  n_cpl_terms_present, n_cpl_terms_departed, n_cpl_terms_shrunk,
                  carry_cpl_tu ? "ON" : "OFF");
    if (carry_vce) {
        // M2 (LS-VCE): run-end honesty summary — the trailing-window measured
        // carry-variance FLOOR σ̂ vs the hand floor, per (family, constellation).
        // ratio > 1 ⇒ the hand floor is optimistic (real carry disagreement
        // exceeds it); ratio < 1 ⇒ the hand floor is conservative.  The per-hour
        // series is in network_carry_vce.csv.
        log->info("Network (M2/LS-VCE): carry-variance monitor summary "
                  "(components='{}', window={} ep) — ROBUST (geometric-mean, tail-safe) "
                  "carry-innovation variance vs the hand floor:",
                  config_.networkCarryVceComponents(), config_.networkCarryVceWindowEpochs());
        for (const auto& fc : carry_vce_est.keys()) {
            const auto e = carry_vce_est.estimate(fc.first, fc.second);
            const double cfg_floor = (fc.first == "pb") ? pb_var_floor
                                   : (fc.first == "cb") ? cb_var_floor
                                                        : pl_var_floor;
            const double rrat = (cfg_floor > 0.0) ? e.robust_floor / cfg_floor : 0.0;
            const double mrat = (cfg_floor > 0.0)
                ? e.sigma[NetworkCarryVce::kFloor] / cfg_floor : 0.0;
            log->info("  [{}|{}] robust σ̂ = {:.3e} m² (hand {:.3e}, ratio {:.1f}); "
                      "mean σ̂ = {:.3e} m² (ratio {:.0f}, event-incl.); {} rows.",
                      fc.first, fc.second.empty() ? std::string("*") : fc.second,
                      e.robust_floor, cfg_floor, rrat,
                      e.sigma[NetworkCarryVce::kFloor], mrat, e.n_rows);
        }
    }
    if (n_gated_epochs > 0)
        log->info("Network: anchored-union coverage gate (< {} trackers) excluded thin shared "
                  "phase-bias column(s) on {} epoch(s) ({} column-epoch(s) total; their phase "
                  "rows served the satellite clock through the local blocks).",
                  config_.networkPhaseDatumMinStations(), n_gated_epochs, n_gated_key_epochs);
    if (perconst_on && (n_perconst_partial_epochs > 0 || n_perconst_diffuse_published > 0))
        log->info("Network (RTA-10): σ̂0 carve-out — {} epoch(s) published PARTIALLY (>=1 "
                  "constellation pruned), {} DIFFUSE epoch(s) published in full (every judged "
                  "constellation individually clean while the global σ̂0 tripped).",
                  n_perconst_partial_epochs, n_perconst_diffuse_published);

    // L15 end-of-run manifest record (body relocated to NetworkRunTraces, INC-4).
    traces.writeRunManifestComplete(nc, ctx.counters, synced.size(), loop_ms);

    // --- B6 (P1-1): serialized-contract round-trip self-check --------------------
    // Close the writer (flush the header), read it back, and cross-check the parsed
    // S-basis contract against what we serialized.  This is a LIVE checkContract call
    // site (previously dead code) that guards the write→read→consume replay path a
    // real product consumer would take: a header serialization/parse regression fails
    // here loudly instead of silently mis-declaring the datum to a downstream user.
    if (contract_pending && !prod_path.empty()) {
        // Zero epochs emitted: the deferred writer never opened, and the round-trip
        // self-check below is skipped.  The stale previous file was removed at
        // staging — unless that removal failed (locked/read-only), in which case a
        // PREVIOUS run's products still sit at the path: check, don't assert.
        if (std::filesystem::exists(prod_path))
            log->warn("Network[B6]: no epochs were emitted AND a stale product file from a "
                      "PREVIOUS run remains at '{}' (removal failed at startup) — do NOT "
                      "consume it; it is not this run's output.", prod_path.string());
        else
            log->warn("Network[B6]: no epochs were emitted — the deferred contract writer never "
                      "opened; no product file exists at '{}' (round-trip self-check skipped).",
                      prod_path.string());
        // L14: record the distinguishable "writer never opened" outcome so an absent
        // contract-check CSV unambiguously means "the check never ran".
        if (nc.serialize_sbasis_contract) {
            std::ofstream cc(log_dir_ / "network_contract_check.csv");
            if (cc) cc << "check,status,detail\n"
                          "overall,FAIL,writer never opened (zero epochs emitted)\n";
        }
    }
    if (nc.serialize_sbasis_contract && product_writer && !prod_path.empty()) {
        product_writer.reset();   // flush + close so the reader sees the complete header
        SbasisContract parsed;
        bool           rok = false;
        const std::vector<NetworkProductRecord> check_recs =
            NetworkProductReader::read(prod_path.string(), &parsed, &rok);
        // RTA-5: with re-broadcast armed, run checkContract on EVERY S receipt (each
        // record's contract_refresh), not once — the per-receipt behaviour a late-
        // joining consumer relies on.  The per-receipt expectation drops the pivot
        // map (pivots legitimately evolve across re-broadcasts; the FINAL pivot map
        // is checked separately below against the last-written contract) but keeps
        // every run-constant convention field AND the run's single generation id.
        // Gated on the toggle so an OFF run's check output is byte-identical.
        std::size_t s_receipts = 0, s_receipt_fails = 0;
        std::vector<std::string> s_receipt_mism;
        if (contract_rebroadcast_s > 0.0) {
            SbasisContract receipt_expect = pending_contract;
            receipt_expect.decl.clock_pivot_sat.clear();
            for (std::size_t ri = 0; ri < check_recs.size(); ++ri) {
                if (!check_recs[ri].contract_refresh.present) continue;
                ++s_receipts;
                std::vector<std::string> rm;
                if (!NetworkProductReader::checkContract(check_recs[ri].contract_refresh,
                                                         receipt_expect, &rm)) {
                    ++s_receipt_fails;
                    for (const auto& m : rm)
                        s_receipt_mism.push_back(fmt::format("receipt {} (epoch {}): {}",
                                                             s_receipts, ri, m));
                }
            }
            if (s_receipt_fails > 0) {
                log->warn("Network[RTA-5]: {} of {} re-broadcast S receipt(s) FAILED the "
                          "per-receipt contract check:", s_receipt_fails, s_receipts);
                for (const auto& m : s_receipt_mism) log->warn("    - {}", m);
            } else {
                log->info("Network[RTA-5]: {} S-contract receipt(s) parsed positionally; every "
                          "one passed the per-receipt contract check.", s_receipts);
            }
        }
        std::vector<std::string> mism;
        if (!parsed.present) {
            log->warn("Network[B6]: the serialized S-basis contract did not parse back from '{}' — "
                      "header round-trip FAILED.", prod_path.string());
        } else if (!NetworkProductReader::checkContract(parsed, pending_contract, &mism)) {
            log->warn("Network[B6]: serialized S-basis contract does not round-trip cleanly "
                      "({} mismatch(es)):", mism.size());
            for (const auto& m : mism) log->warn("    - {}", m);
        } else if (parsed.decl.clock_pivot_sat != pending_contract.decl.clock_pivot_sat) {
            log->warn("Network[B6]: the realized clock-pivot map did not round-trip through the header.");
        } else {
            log->info("Network[B6]: S-basis contract round-trips cleanly "
                      "(write → read → checkContract), {} realized pivot(s) serialized.",
                      parsed.decl.clock_pivot_sat.size());
        }
        // L14: machine-readable check result.  The pivot-map round-trip is checked SEPARATELY
        // (checkContract only compares the expected-side pivots, so a realized-pivot mismatch is
        // NOT in `mism`) and must fail 'overall' too.  Written on PASS as well — an ABSENT file
        // means the check never ran (crash / zero epochs), distinct from a recorded FAIL.
        const bool pivot_ok = parsed.present
            && (parsed.decl.clock_pivot_sat == pending_contract.decl.clock_pivot_sat);
        const bool overall = rok && parsed.present && mism.empty() && pivot_ok
            && (contract_rebroadcast_s <= 0.0 || s_receipt_fails == 0);
        const std::filesystem::path cc_path = log_dir_ / "network_contract_check.csv";
        std::ofstream cc(cc_path);
        if (cc) {
            cc << "check,status,detail\n";
            cc << "file_readable," << (rok ? "PASS" : "FAIL") << ",\n";
            cc << "contract_present," << (parsed.present ? "PASS" : "FAIL") << ",\n";
            for (const auto& m : mism)
                cc << "field_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            // RTA-5 (toggle-gated so an OFF run's CSV is byte-identical): per-receipt
            // contract-check summary across every positional S line in the file.
            if (contract_rebroadcast_s > 0.0) {
                cc << "s_receipts," << (s_receipt_fails == 0 ? "PASS" : "FAIL") << ','
                   << s_receipts << " receipt(s); " << s_receipt_fails << " failed\n";
                for (const auto& m : s_receipt_mism)
                    cc << "s_receipt_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            }
            cc << "pivot_map_roundtrip," << (pivot_ok ? "PASS" : "FAIL") << ",\n";
            cc << "overall," << (overall ? "PASS" : "FAIL") << ','
               << parsed.decl.clock_pivot_sat.size() << " realized pivot(s) serialized\n";
            cc.close();
        } else {
            log->warn("Network[L14]: failed to open contract-check CSV {}", cc_path.string());
        }
    }

    // B8 flag-rate empirical-alpha summary + L3 run-summary CSV (bodies relocated
    // to NetworkRunTraces, INC-4; ONE trace_flag_rates gate covers both).
    traces.writeFlagRateAndRunSummary(config_, ctx.knobs, ctx.counters, synced.size(),
                                      stations.size(), n_threads, parallel_on, loop_ms, log);

    log->info("Network: shared satellite product accumulated in an owned "
              "SsrCorrectionStore (closed-loop sink). Trace: {}", trace_path.string());

    // --- Closed-loop integer-recovery summary -------------------------------
    if (closed_loop_on && consumer) {
        const double fix_rate = cl_ar_ran ? (100.0 * static_cast<double>(cl_ar_fixed)
                                             / static_cast<double>(cl_ar_ran)) : 0.0;
        const double mean_fixed = cl_ar_fixed ? (static_cast<double>(cl_sum_fixed)
                                                 / static_cast<double>(cl_ar_fixed)) : 0.0;
        // Converged-window robust stats: median (typical accuracy, spike-immune)
        // + max (surfaces wrong-fix excursions the RMS would smear).
        double conv_med = 0.0, conv_max = 0.0;
        if (!cl_recent_err3d.empty()) {
            std::vector<double> w(cl_recent_err3d.begin(), cl_recent_err3d.end());
            std::sort(w.begin(), w.end());
            conv_med = w[w.size() / 2];
            conv_max = w.back();
        }
        const Eigen::Vector3d final_err = consumer->getPosition() - cl_truth;
        const Eigen::Vector3d final_enu = Coordinates::ecefToEnu(final_err, cl_truth_llh);

        const char* product_label = !cl.consume_network_products
            ? "BASELINE (precise products only — network store NOT consumed)"
            : (cl.consume_network_clock ? "FULL (network clock + phase bias)"
                                        : "BIAS-ONLY (precise clock + network phase bias)");
        const std::string mode_label =
            std::string(cl.consumer_undifferenced ? "UNDIFFERENCED" : "BSSD")
            + " user / " + product_label;

        // Honest, fix-rate-led verdict.  WL-only AR does not fix the narrow lane,
        // so the post-fix position stays at the WL-float level (~m) even on a
        // healthy product — the WL fix RATE, not the position, is the recovery
        // metric.  Flag a marginal result and point at the A/B baseline knob so a
        // reader never reads a degraded run as a clean pass.
        std::string verdict;
        if (cl_ar_fixed == 0)
            verdict = "NO integers recovered — inspect closed_loop_trace.csv / SSR coverage";
        else if (fix_rate >= 50.0)
            verdict = fmt::format("RECOVERED INTEGERS — {:.0f}% WL fix rate from our own "
                                  "network products", fix_rate);
        else
            verdict = fmt::format("PARTIAL — {:.0f}% WL fix rate (marginal); compare with the "
                                  "precise-products baseline (network.closed_loop."
                                  "consume_network_products=false)", fix_rate);

        log->info("==== Closed-loop integer-recovery harness — held-out '{}' ====",
                  cl.station.name);
        log->info("  mode                      : {}", mode_label);
        log->info("  consumer epochs processed : {}", cl_epochs);
        log->info("  AR invoked / accepted     : {} / {}  ({:.1f}% fix rate)",
                  cl_ar_ran, cl_ar_fixed, fix_rate);
        log->info("  mean WL integers per fix  : {:.1f}", mean_fixed);
        if (cl_first_fix_tow >= 0.0)
            log->info("  first integer fix at      : week {} tow {:.1f}",
                      cl_first_fix_week, cl_first_fix_tow);
        else
            log->info("  first integer fix         : NONE — no epoch recovered integers");
        log->info("  position error (final)    : {:.3f} m 3D  (E {:.3f}, N {:.3f}, U {:.3f})",
                  final_err.norm(), final_enu.x(), final_enu.y(), final_enu.z());
        log->info("  position (last {} ep)      : median {:.3f} m, max {:.3f} m 3D "
                  "(max >> median ⇒ wrong-fix excursions)",
                  cl_recent_err3d.size(), conv_med, conv_max);
        log->info("  VERDICT                   : {}", verdict);
        log->info("  closed-loop trace         : {}",
                  (log_dir_ / "closed_loop_trace.csv").string());
        log->info("==============================================================");
    }
    if (n_emitted == 0) {
        log->error("Network: produced ZERO solution epochs. Common causes: stations "
                   "do not overlap in time; the local nuisance block is rank-"
                   "deficient (disable receiver code-bias / phase-clock estimation "
                   "for the network config); or the shared datum is under-specified — "
                   "a near-null W6 code-bias direction withholds every epoch when sparse "
                   "(often BeiDou) satellites register OSB columns with < 2 IF-pinnable "
                   "signals.  Arm the run-latched admission census (network.cb_admission; "
                   "tune network.cb_admission_min_stations / _census_epochs) so those "
                   "pairs stay station-local sponges, and confirm >= 2 overlapping stations.");
        return 1;
    }
    return 0;
}

} // namespace GnssProcessor
