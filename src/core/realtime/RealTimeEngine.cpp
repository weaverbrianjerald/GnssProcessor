/**
 * @file RealTimeEngine.cpp
 * @brief Real-time GNSS PPP processing engine implementation.
 *
 * Mirrors the per-epoch logic of Application (post-processing) but designed
 * for epoch-at-a-time streaming operation rather than batch iteration.
 */

#include "realtime/RealTimeEngine.hpp"
#include "app/EpochUpdateSupport.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "sbf/SbfParser.hpp"
#include "math/Coordinates.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <fmt/format.h>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RealTimeEngine::RealTimeEngine(GnssConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager) {
}

// ---------------------------------------------------------------------------
// GPS -> UTC helper
// ---------------------------------------------------------------------------

GnssTime RealTimeEngine::gpsToUtcTime(const GnssTime& gps_time) {
    const double gps_secs = TimeUtils::continuousGpsSeconds(gps_time);
    const double utc_secs = gps_secs - TimeUtils::LEAP_SECONDS;
    int utc_week = static_cast<int>(std::floor(utc_secs / TimeUtils::SECONDS_IN_WEEK));
    double utc_tow = utc_secs - static_cast<double>(utc_week) * TimeUtils::SECONDS_IN_WEEK;
    if (utc_tow < 0.0) { utc_tow += TimeUtils::SECONDS_IN_WEEK; --utc_week; }
    return GnssTime{ utc_week, utc_tow };
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool RealTimeEngine::initialize() {
    auto log = log_manager_.getLogger("rt_engine");
    log->info("Initializing real-time engine");

    // ProductParser (loads static products: ATX, BIA, broadcast nav).  In a
    // live session orbits/clocks come from the broadcast ephemeris + IGS-SSR,
    // not precise SP3/CLK, so those are skipped below.
    product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);

    // RinexParser is still needed as a structural dependency of PppModel for
    // header metadata (antenna type, approximate position).  In real-time mode
    // we construct it but parsing the observation file is optional  if one is
    // configured we parse it to seed the header, otherwise we set defaults.
    rinex_parser_ = std::make_unique<RinexParser>(config_, log_manager_, *product_parser_);

    std::filesystem::path obs_path = config_.getObservationPath();
    if (!obs_path.empty() && std::filesystem::exists(obs_path)) {
        if (!rinex_parser_->parse()) {
            log->warn("Failed to parse RINEX for header seeding  using defaults");
        }
    }

    // Live SBF/RTCM streams carry no RINEX ANT # / TYPE record.  Seed the
    // receiver antenna type from config when the header is empty so the
    // receiver ATX entry can be matched (PCO/PCV + the require_atx_coverage
    // gate).  The RINEX header (post-processing) always wins — this only
    // fills a genuinely-empty field.  This is the single injection seam;
    // Application::run() (post-processing) never calls setReceiverAntennaType.
    if (rinex_parser_->getReceiverAntennaType().empty()) {
        const std::string& ov = config_.getModelCorrections().receiver_antenna_type_override;
        if (!ov.empty()) {
            rinex_parser_->setReceiverAntennaType(ov);
            log->info("Seeded receiver antenna type from config override: '{}'", ov);
        }
    }

    // Load products.  Skip precise SP3/CLK when a live broadcast+SSR source
    // supplants them (SSR corrections enabled and/or a live socket primary);
    // a real-time *file* replay with no corrections still loads them so a
    // historical run can use precise products.
    const bool skip_precise = config_.realtimeUsesLiveOrbitClockSource();
    product_parser_->loadProducts(!skip_precise);

    auto& cache = ProductCache::getInstance();
    if (!cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Failed to initialize product cache");
        return false;
    }
    log->info("Product cache: {} orbits, {} clocks",
              cache.getOrbitCount(), cache.getClockCount());

    // State manager.
    constexpr size_t core_state_size = 5; // Pos(3) + Clk(1) + Tropo(1)
    state_manager_ = std::make_unique<StateManager>(core_state_size, log_manager_, config_);

    // Seed initial position.
    initial_position_ = Eigen::Vector3d(0.0, 0.0, Constants::EARTH_RADIUS);
    auto approx_pos = rinex_parser_->getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { valid_pos = false; break; }
    }
    if (valid_pos && approx_pos != std::array<double, 3>{0.0, 0.0, 0.0}) {
        initial_position_ = Eigen::Vector3d(approx_pos[0], approx_pos[1], approx_pos[2]);
    }
    // NOTE: the seed is applied to ppp_state_ AFTER initializeState() below, not
    // here.  initializeState() + syncPppStateToStateVector() run later in this
    // function and would clobber any early state-vector write with ppp_state_'s
    // zero position (a live SBF/RTCM stream carries no RINEX APPROX POSITION).

    // Processing components.
    ppp_model_       = std::make_unique<PppModel>(config_, *product_parser_, log_manager_, *rinex_parser_, *state_manager_);
    meas_model_      = std::make_unique<MeasurementModel>(*state_manager_, *ppp_model_, config_, log_manager_);
    kalman_filter_   = std::make_unique<KalmanFilter>(*state_manager_, config_, log_manager_, *ppp_model_);
    wls_estimator_   = std::make_unique<WlsEstimator>(config_, *state_manager_, log_manager_);
    mw_slip_detector_  = std::make_unique<MwSlipDetector>(config_, log_manager_);

    // TurboEdit MW+GF joint slip repair (high-rate only).  The real-time path
    // streams at the receiver's native rate (typically 1 Hz), so the geometry-
    // free jump used to split a dual-frequency slip per signal is valid here.
    if (config_.getCycleSlipConfig().turbo_edit) {
        turbo_edit_resolver_ = std::make_unique<TurboEditResolver>(config_, log_manager_);
        log->info("Real-time TurboEdit MW+GF joint cycle-slip repair ENABLED.");
    }

    // Higher-order ionosphere is not yet wired into the real-time path:
    // the engine relies on RTCM SSR for ionospheric content, not on an
    // IONEX GIM, and an SSR-to-STEC adapter for HigherOrderIonosphere has
    // not been written.  Surface this clearly so a user who enabled
    // higher-order globally doesn't silently get the first-order-only
    // pipeline at runtime.
    if (config_.getModelCorrections().enable_higher_order_ionosphere) {
        log_manager_.getLogger("ionosphere")->warn(
            "Real-time mode: higher_order_ionosphere is enabled but unsupported "
            "in the streaming pipeline (no GIM source) — the correction will be "
            "skipped.  Disable the flag in real-time configs to avoid this warning.");
    }

    // Provide BIA satellite biases to the MW detector for WL integer rounding
    mw_slip_detector_->setCorrections(&ppp_model_->getCorrections());
    if (turbo_edit_resolver_)
        turbo_edit_resolver_->setCorrections(&ppp_model_->getCorrections());

    // Integer ambiguity resolution (MLAMBDA) — same configuration surface as
    // the post-processing path (Application::run).  The WL feed comes from the
    // MW slip detector; no CycleSlipManager is required in the realtime path.
    if (config_.getArMode() > 0) {
        AmbiguityResolverConfig ar_config;
        ar_config.ar_mode               = config_.getArMode();
        ar_config.ratio_threshold       = config_.getArRatioThreshold();
        ar_config.elevation_mask_ar     = config_.getArElevationMask();
        ar_config.hold_variance         = config_.getArHoldVariance();
        ar_config.min_epochs            = config_.getArMinEpochs();
        ar_config.max_position_sigma_m  = config_.getArMaxPositionSigma();
        ar_config.ratio_upper_bound     = config_.getArRatioUpperBound();
        ar_config.wl_only               = config_.getArWlOnly();
        ar_config.wl_only_pseudo_var    = config_.getArWlOnlyPseudoVar();
        ar_config.wl_max_frac           = config_.getArWlMaxFrac();
        ar_config.min_success_rate      = config_.getArMinSuccessRate();
        ar_config.nl_max_per_pair_frac  = config_.getArNlMaxPerPairFrac();
        ar_config.ar_active_systems     = config_.getArActiveSystems();
        ar_config.state_ambiguity_basis = config_.getStateAmbiguityBasis();
        ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
        log->info("Real-time AR enabled: mode={}, wl_only={}, ratio>={:.1f}, elev_mask={:.1f} deg",
                  ar_config.ar_mode, ar_config.wl_only ? "on" : "off",
                  ar_config.ratio_threshold, ar_config.elevation_mask_ar);
    }

    ppp_model_->initializeState(ppp_state_);
    // initializeState() resolves the a-priori from a configured reference
    // position or the RINEX APPROX POSITION; a live SBF/RTCM stream has neither,
    // so it leaves the position at zero.  Without a finite a-priori the first
    // computeModels() rejects every satellite on the rx_pos.norm() < 1e3 sanity
    // check, so WLS never runs and the filter can never bootstrap.  Fall back to
    // the coarse Earth-centred seed computed above; WLS refines it to metres on
    // the first epoch with enough satellites.
    if (ppp_state_.position.norm() < 1e3 || !ppp_state_.position.allFinite()) {
        ppp_state_.position = initial_position_;
        log->info("Seeded a-priori position from Earth-centred fallback "
                  "({:.1f}, {:.1f}, {:.1f}) m (no reference/RINEX position for the live stream)",
                  initial_position_.x(), initial_position_.y(), initial_position_.z());
    }
    state_manager_->syncPppStateToStateVector(ppp_state_);

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    std::filesystem::path log_dir{"logs"};
    const auto& logging_cfg = config_.getLoggingConfig();
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir, is_ppp_rtk, logging_cfg);

    if (logging_cfg.enable_mw_slip_trace) {
        mw_slip_detector_->enableTracing(log_dir);
    }
    if (logging_cfg.enable_turbo_edit_trace && turbo_edit_resolver_) {
        turbo_edit_resolver_->enableTracing(log_dir);
    }

    log->info("Real-time engine initialized (position seeded at ({:.1f}, {:.1f}, {:.1f}))",
              initial_position_.x(), initial_position_.y(), initial_position_.z());
    return true;
}

// ---------------------------------------------------------------------------
// RTCM frame ingestion
// ---------------------------------------------------------------------------

void RealTimeEngine::feedRtcmFrame(const std::vector<uint8_t>& frame) {
    // Decode the frame through the existing RtcmParser message API.
    // This is a temporary file-less parser invocation.
    RtcmParser parser;
    parser.parseFrame(frame);

    // Observations: buffer until a new epoch is detected, then submit the
    // previous epoch.  MSM4/MSM7 decode is a follow-up phase — until then
    // getObservations() is empty and this loop is a no-op.
    for (const auto& obs : parser.getObservations()) {
        if (pending_obs_.empty()) {
            pending_epoch_time_ = obs.time;
        } else if (std::abs(obs.time.tow - pending_epoch_time_.tow) > 0.5) {
            // New epoch  process the buffered one.
            processEpoch(pending_obs_);
            pending_obs_.clear();
            pending_epoch_time_ = obs.time;
        }
        pending_obs_.push_back(obs);
    }

    // Push decoded SSR records into the shared SsrCorrectionStore, carrying the
    // real message header (SSR epoch + IOD-SSR) so staleness / IOD diagnostics
    // reflect the provider solution, not wall-clock receipt.  The orbit / clock
    // query paths in Geometry and GnssCorrections consult the store
    // transparently — no further wiring at the per-epoch level.
    auto& ssr_store = SsrCorrectionStore::getInstance();
    const SsrHeader& hdr = parser.getLastSsrHeader();
    for (const auto& r : parser.getSsrOrbits())         ssr_store.pushOrbit(r, hdr);
    for (const auto& r : parser.getSsrClocks())         ssr_store.pushClock(r, hdr);
    for (const auto& r : parser.getSsrCombined())       ssr_store.pushCombined(r, hdr);
    for (const auto& r : parser.getSsrCodeBiases())     ssr_store.pushCodeBias(r, hdr);
    for (const auto& r : parser.getSsrPhaseBiases())    ssr_store.pushPhaseBias(r, hdr);
    for (const auto& r : parser.getSsrUras())           ssr_store.pushUra(r, hdr);
    for (const auto& r : parser.getSsrHighRateClocks()) ssr_store.pushHighRateClock(r, hdr);

    // Push decoded broadcast ephemeris into the live store — this is what lets
    // Geometry::getSatellitePV resolve satellite orbits with no SP3 file loaded.
    auto& eph_store = LiveEphemerisStore::getInstance();
    for (const auto& eph : parser.getBroadcastEphemeris()) eph_store.insert(eph);

    const std::size_t n_ssr =
        parser.getSsrOrbits().size()      + parser.getSsrClocks().size() +
        parser.getSsrCombined().size()    + parser.getSsrCodeBiases().size() +
        parser.getSsrPhaseBiases().size() + parser.getSsrUras().size() +
        parser.getSsrHighRateClocks().size();
    if (n_ssr > 0) {
        log_manager_.getLogger("rt_engine")->trace(
            "RTCM {}: pushed {} SSR records (epoch wk{} tow{:.0f}, IOD-SSR {}) "
            "to store ({} sats cached)",
            parser.lastMessageType(), n_ssr, hdr.epoch_time.week,
            hdr.epoch_time.tow, hdr.iod_ssr, ssr_store.size());
    }
    if (!parser.getBroadcastEphemeris().empty()) {
        log_manager_.getLogger("rt_engine")->trace(
            "RTCM {}: cached {} broadcast ephemeris ({} sats in live store)",
            parser.lastMessageType(), parser.getBroadcastEphemeris().size(),
            eph_store.size());
    }
    if (!parser.getStationCoords().empty()) {
        const auto& s = parser.getStationCoords().front();
        log_manager_.getLogger("rt_engine")->info(
            "RTCM {}: station {} ARP=({:.4f},{:.4f},{:.4f})",
            parser.lastMessageType(), s.reference_station_id,
            s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z());
    }
}

// ---------------------------------------------------------------------------
// Direct observation feed
// ---------------------------------------------------------------------------

void RealTimeEngine::feedSbfFrame(const std::vector<uint8_t>& block) {
    SbfParser parser;
    parser.parseFrame(block);

    // Seed the filter a-priori from the receiver's own PVTGeodetic solution
    // until the KF has bootstrapped.  A live SBF stream carries no RINEX
    // APPROX POSITION, so without this the a-priori is the coarse Earth-centred
    // fallback (geographic pole) from which almost every satellite sits below
    // the elevation mask and the filter can never converge.  A configured
    // reference_position always wins; once first_epoch_ clears, the KF owns the
    // position and we stop overwriting it.
    if (first_epoch_ && !config_.hasReferencePosition()) {
        if (const auto& pvt = parser.getReceiverPositionEcef()) {
            const size_t pidx = state_manager_->getPositionIndex();
            Eigen::VectorXd sv = state_manager_->getStateVector();
            if (sv.size() >= static_cast<Eigen::Index>(pidx + 3)) {
                sv.segment<3>(pidx) = *pvt;
                state_manager_->setStateVector(sv);
                ppp_state_.position = *pvt;
                if (!apriori_seeded_from_pvt_) {
                    log_manager_.getLogger("rt_engine")->info(
                        "Seeded a-priori from PVTGeodetic: ({:.3f}, {:.3f}, {:.3f}) m",
                        pvt->x(), pvt->y(), pvt->z());
                    apriori_seeded_from_pvt_ = true;
                }
            }
        }
    }

    // Broadcast ephemeris -> live store (same path RTCM nav uses).  These are
    // the receiver's DECODED nav blocks, emitted only on ephemeris change.
    auto& eph_store = LiveEphemerisStore::getInstance();
    for (const auto& eph : parser.getBroadcastEphemeris()) eph_store.insert(eph);
    if (!parser.getBroadcastEphemeris().empty()) {
        log_manager_.getLogger("rt_engine")->trace(
            "SBF block {}: cached {} broadcast ephemeris ({} sats in live store)",
            parser.lastBlockNumber(), parser.getBroadcastEphemeris().size(), eph_store.size());
    }

    // Raw navigation subframes -> assemble ephemeris -> live store.  Unlike the
    // on-change decoded-nav blocks above, the receiver streams these
    // continuously, so this is the primary ephemeris source for a live SBF run.
    for (const auto& m : parser.getRawNavMessages()) {
        if (auto eph = subframe_assembler_.consume(m)) {
            eph_store.insert(*eph);
            log_manager_.getLogger("rt_engine")->trace(
                "SBF raw-nav: assembled {} ephemeris (iode {}, {} sats in live store)",
                eph->satellite, eph->iode, eph_store.size());
        }
    }

    // A MeasEpoch carries a complete epoch of observations — process it
    // directly (no multi-message assembly as in the RTCM/MSM path).
    const auto& obs = parser.getObservations();
    if (!obs.empty()) {
        processEpoch(obs);
    }
}

void RealTimeEngine::feedObservations(const std::vector<RinexObservation>& observations) {
    processEpoch(observations);
}

void RealTimeEngine::flush() {
    if (!pending_obs_.empty()) {
        processEpoch(pending_obs_);
        pending_obs_.clear();
    }
}

// ---------------------------------------------------------------------------
// Process a single epoch
// ---------------------------------------------------------------------------

bool RealTimeEngine::processEpoch(const std::vector<RinexObservation>& epoch_obs) {
    auto log = log_manager_.getLogger("rt_engine");

    if (epoch_obs.empty()) return false;

    const GnssTime current_time = epoch_obs.front().time;
    state_manager_->setStateEpochTime(current_time);
    LogManager::setCurrentEpochTime(current_time);

    // Mode tag only — the spdlog %E field already carries the GPS time.
    // "[RT] " distinguishes realtime-engine output from post-processing
    // when both modes share a console.
    const std::string epoch_prefix = "[RT] ";

    // --- Time step / gap detection ---
    double dt;
    bool gap_detected = false;
    if (first_epoch_) {
        dt = 1.0; // assume 1 s for first real-time epoch
        log->info("{}First epoch", epoch_prefix);
    } else {
        dt = current_time.tow - prev_tow_;
        if (dt < 0.0) dt += TimeUtils::SECONDS_IN_WEEK; // week rollover
        const double gap_tol = config_.getDataGapTolerance() > 0.0
                             ? config_.getDataGapTolerance()
                             : 1.0;  // default: 1 s (typical RT rate)
        gap_detected = dt > gap_tol;
    }
    prev_tow_ = current_time.tow;
    prev_epoch_time_ = current_time;

    // --- Sync PPP state ---
    try {
        state_manager_->updatePppStateFromVector(
            state_manager_->getStateVector(), ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log->error("{}State sync failed: {}", epoch_prefix, e.what());
        return false;
    }

    // --- Compute modeled observations ---
    // Filter to active systems.
    std::vector<RinexObservation> filtered_obs;
    {
        const auto& active = config_.getActiveSystems();
        for (const auto& obs : epoch_obs) {
            std::string sys_str;
            switch (obs.system) {
            case System::GPS:     sys_str = "GPS";     break;
            case System::Galileo: sys_str = "Galileo"; break;
            case System::GLONASS: sys_str = "GLONASS"; break;
            case System::BeiDou:  sys_str = "BeiDou";  break;
            case System::QZSS:    sys_str = "QZSS";    break;
            case System::SBAS:    sys_str = "SBAS";    break;
            default: continue;
            }
            if (std::find(active.begin(), active.end(), sys_str) != active.end()) {
                filtered_obs.push_back(obs);
            }
        }
    }
    if (filtered_obs.empty()) {
        log->debug("{}No observations after system filtering", epoch_prefix);
        return false;
    }

    std::vector<PppObservation> modeled_obs;
    std::map<std::string, DropReason> drop_reasons;
    if (!ppp_model_->computeModels(filtered_obs, ppp_state_, modeled_obs, &drop_reasons)) {
        log->warn("{}Failed to compute models", epoch_prefix);
        return false;
    }

    // --- Corrections-ready startup gate (real-time + SSR only) ---
    // Defer ALL estimation until every modeled above-mask satellite is
    // broadcast+SSR (cm-grade clock, phase retained — NOT broadcast_only) and a
    // solvable floor of them is present, so the filter seeds code+phase together
    // on a clean state instead of cold-starting code-only on metre-grade
    // broadcast clocks (and baking a contaminated iono/bias/ambiguity partition
    // as each sat's SSR flips it cm-grade mid-arc).  No-op when disabled or in
    // post-processing (computeModels never yields broadcast_only with precise
    // CLK present).  Latches via estimation_started_ so it runs only at startup.
    if (config_.getCorrectionsReadyGate().enabled && !estimation_started_) {
        const auto& gate = config_.getCorrectionsReadyGate();

        // Per-constellation tally: cm-grade (broadcast+SSR, phase-retained) and
        // total above-mask counts per active constellation.
        std::map<std::string, int> cm_per_cons, total_per_cons;
        size_t n_cm = 0;
        std::vector<std::string> not_ready;
        for (const auto& o : modeled_obs) {
            const std::string cons = PppUtils::getSystemString(o.satellite);
            ++total_per_cons[cons];
            if (o.broadcast_only) { not_ready.push_back(o.satellite); }
            else { ++n_cm; ++cm_per_cons[cons]; }
        }

        // Per-constellation quorum: every constellation that can contribute SD
        // geometry (>=2 above-mask sats) must have at least min(per_system, its
        // above-mask count) cm-grade sats.  This guarantees MULTI-constellation
        // phase geometry at start — without it the gate latches on GPS-only
        // phase while Galileo/BeiDou are still broadcast-only, which drives the
        // ZWD<->height null-space runaway and flags all Galileo code.  per_system
        // <= 0 disables the per-system check (flat global floor only).
        bool per_system_ok = true;
        std::vector<std::string> cons_waiting;
        if (gate.min_satellites_per_system > 0) {
            for (const auto& [cons, total] : total_per_cons) {
                if (total < 2) continue;   // a lone sat contributes no SD row
                const int need = std::min(gate.min_satellites_per_system, total);
                const int have = cm_per_cons.count(cons) ? cm_per_cons[cons] : 0;
                if (have < need) {
                    per_system_ok = false;
                    cons_waiting.push_back(cons + " " + std::to_string(have) + "/" + std::to_string(need));
                }
            }
        }

        const bool ready = n_cm >= static_cast<size_t>(gate.min_satellites)
                        && per_system_ok
                        && (!gate.require_all_above_mask || not_ready.empty());

        if (++warmup_epochs_ == 1) warmup_start_time_ = current_time;
        bool timed_out = false;
        if (!ready && gate.timeout_s > 0.0) {
            const double elapsed = TimeUtils::continuousGpsSeconds(current_time)
                                 - TimeUtils::continuousGpsSeconds(warmup_start_time_);
            timed_out = elapsed >= gate.timeout_s;
        }

        std::string waiting;
        for (size_t i = 0; i < not_ready.size(); ++i) {
            if (i) waiting += ",";
            waiting += not_ready[i];
        }
        std::string cons_short;
        for (size_t i = 0; i < cons_waiting.size(); ++i) {
            if (i) cons_short += ", ";
            cons_short += cons_waiting[i];
        }

        if (!ready && !timed_out) {
            if (warmup_epochs_ == 1 || (warmup_epochs_ % 10) == 0) {
                log->info("{}Startup gate: deferring — {} cm-grade total (need >= {}), "
                          "per-constellation [{}], {} still broadcast-only [{}]",
                          epoch_prefix, n_cm, gate.min_satellites,
                          cons_short.empty() ? "ok" : cons_short,
                          not_ready.size(), waiting);
                // SSR-store census localises the problem: zero orbit/clock counts
                // for a constellation => decode/store gap; non-zero counts while it
                // stays broadcast-only => an IODE-match issue (see the per-sat
                // "SSR clock present ... but NOT applied" trace in GnssCorrections).
                log->info("{}  SSR store census: {}",
                          epoch_prefix, SsrCorrectionStore::getInstance().censusSummary());
            }
            return false;   // accumulate nav/SSR; no predict/update; first_epoch_ stays true
        }

        estimation_started_ = true;
        if (timed_out)
            log->warn("{}Startup gate: timeout ({:.0f}s) — {} sat(s) still broadcast-only; "
                      "starting in DEGRADED mode", epoch_prefix, gate.timeout_s, not_ready.size());
        else
            log->info("{}Startup gate satisfied: {} cm-grade sat(s) across {} constellation(s) — "
                      "starting estimation (code+phase together){}",
                      epoch_prefix, n_cm, cm_per_cons.size(),
                      not_ready.empty() ? "" : " (broadcast-only stragglers join as SSR arrives)");
    }

    // --- Drop broadcast-grade satellites (precise SSR PPP-RTK) ---
    // Once estimation runs, a broadcast_only sat carries metre-grade code (no
    // SSR clock) and no phase.  Feeding it pollutes the cm-grade solution and
    // (worse) lets it seat as a BSSD pivot, injecting a coherent offset into
    // every SD code row.  Drop such sats ENTIRELY here (not just their phase,
    // which computeModels already stripped) so they rejoin cleanly once their
    // SSR arrives; also covers a sat going broadcast_only mid-run (the latched
    // gate does not).  Gated so broadcast-only-float runs keep their code.
    if (config_.getCorrectionsReadyGate().drop_broadcast_grade) {
        size_t dropped = 0;
        for (auto it = modeled_obs.begin(); it != modeled_obs.end(); ) {
            if (it->broadcast_only) { it = modeled_obs.erase(it); ++dropped; }
            else ++it;
        }
        if (dropped > 0)
            log->debug("{}Dropped {} broadcast-grade sat(s) (no SSR clock) from the precise solution",
                       epoch_prefix, dropped);
    }

    // --- WLS seeding ---
    // Under BSSD the KF carries no receiver clock state to seed — skip WLS
    // entirely (its clock estimate is meaningless to the SD filter).
    if (state_manager_->hasClock()) {
        const auto wls_result = wls_estimator_->estimate(current_time, modeled_obs);
        if (wls_result.success() && wls_result.state.size() >= 4) {
            const double wls_clock = wls_result.state(3);
            const Eigen::VectorXd sv = state_manager_->getStateVector();
            const size_t clk_idx = state_manager_->getClockIndex();
            const double kf_clock = (sv.size() > static_cast<Eigen::Index>(clk_idx))
                                    ? sv(clk_idx) : 0.0;

            constexpr double kJumpThresh = 0.00005 * 299792458.0; // 0.5 ms
            const double delta = wls_clock - kf_clock;
            const bool should_seed = (std::abs(delta) >= kJumpThresh)
                                     || gap_detected || first_epoch_;
            if (should_seed && sv.size() > static_cast<Eigen::Index>(clk_idx)) {
                Eigen::VectorXd sv2 = sv;
                sv2(clk_idx) = wls_clock;
                state_manager_->setStateVector(sv2);
                ppp_state_.clock_bias = wls_clock;
                log->info("{}Seeded clock: wls={:.1f} kf={:.1f} d={:.1f}",
                          epoch_prefix, wls_clock, kf_clock, delta);
            }
        }
    }

    // --- BSSD code/phase re-gauge: remove the bulk receiver clock ---
    // Under BSSD the filter carries NO receiver-clock state (StateManager gates
    // it off; buildJacobianRow uses rx_clk=0).  Each per-satellite code-bias
    // state is seeded as (observed - predicted), so it absorbs the FULL
    // receiver clock at its first epoch — on this unsteered AsteRx-U that is up
    // to ~150 km (the "initial=-94715.207 m" sat-bias seed).  Subtract a single
    // per-epoch receiver-clock estimate, COMMON to every satellite, from every
    // code & phase observable BEFORE seeding / predict / SD.  Because the value
    // is identical across satellites it cancels EXACTLY in the between-satellite
    // single difference: the SD measurement, the Kalman update and the position
    // solution are provably unchanged — this is a gauge / conditioning change,
    // never a model change.  What it buys: the bias states seed at DCB scale
    // (~m), the undifferenced residuals no longer drift with the clock, and a
    // failed pivot re-reference is bounded by the clock-estimate error rather
    // than the full unsteered clock.
    //
    // The estimate is the MEDIAN over satellites of the undifferenced CODE
    // innovation (observed - modeled; modeled excludes rx_clk under BSSD).  The
    // median is dominated by the common receiver clock and is robust to the
    // per-satellite DCB / iono / multipath spread.  The standalone WLS/SPP
    // clock is deliberately NOT used on this path: WlsEstimator::estimate reads
    // state(getClockIndex()), and getClockIndex() is unallocated (SIZE_MAX)
    // under BSSD — which is exactly why the WLS-seeding block above is gated on
    // hasClock() and skipped here.  Only the KF `observed` map is touched;
    // raw_observed — consumed by the MW / slip detectors, whose combinations
    // already cancel the clock — is left untouched.  Real-time path only
    // (post-processing PPP-RTK runs Application::run, never this engine).
    if (config_.enableBetweenSatelliteSd() && !state_manager_->hasClock()) {
        std::vector<double> code_innov;
        code_innov.reserve(modeled_obs.size());
        for (const auto& o : modeled_obs) {
            for (const auto& [code, val] : o.observed) {
                if (code.empty() || code[0] != 'C') continue;      // code only
                const auto mit = o.modeled.find(code);
                if (mit == o.modeled.end()) continue;
                const double innov = val - mit->second;
                if (std::isfinite(innov)) code_innov.push_back(innov);
            }
        }
        if (!code_innov.empty()) {
            const size_t mid = code_innov.size() / 2;
            std::nth_element(code_innov.begin(), code_innov.begin() + mid, code_innov.end());
            const double common_clock_m = code_innov[mid];
            if (std::isfinite(common_clock_m)) {
                size_t n_reduced = 0;
                for (auto& o : modeled_obs) {
                    for (auto& kv : o.observed) {
                        const std::string& code = kv.first;
                        if (code.empty()) continue;
                        const char c0 = code[0];
                        if (c0 != 'C' && c0 != 'L') continue;      // code + phase
                        kv.second -= common_clock_m;               // common-mode: SD-invariant
                        ++n_reduced;
                    }
                }
                log->debug("{}BSSD re-gauge: removed common rx clock {:.3f} m "
                           "(median of {} code innovations) from {} observables",
                           epoch_prefix, common_clock_m, code_innov.size(), n_reduced);
            }
        }
    }

    // First real epoch processed — clear first_epoch_ UNCONDITIONALLY.  This was
    // previously inside the hasClock() WLS block above, but BSSD / PPP-RTK has no
    // receiver-clock state, so that block is skipped and the flag never cleared.
    // Leaving it set wedged the engine: dt stayed pinned at 1 s (gaps never
    // detected) and the PVTGeodetic a-priori was re-seeded every epoch, hard-
    // pinning the position so the filter could never converge.  Cold-start
    // epochs with no modelable observations return earlier, so reaching here
    // means this epoch produced a usable measurement set.
    first_epoch_ = false;

    // --- KF predict ---
    kalman_filter_->predict(dt);
    state_manager_->updatePppStateFromVector(
        state_manager_->getStateVector(), ppp_state_, *state_manager_);

    // --- Cycle slip detection / repair (MW, pre-estimation) ---
    {
        const auto slip_results = mw_slip_detector_->detectLargeMwResets(modeled_obs);
        const double reset_std = config_.getAmbiguityStd();

        // TurboEdit MW+GF joint repair (high-rate): pair the MW wide-lane
        // integer with the geometry-free phase jump to split a dual-frequency
        // slip into per-signal integer repairs (covariance preserved) and to
        // catch the MW dN1==dN2 blind spot.  Satellites it handles are skipped
        // by the default reset loop below; everything else falls back to a
        // plain ambiguity reset (the original real-time behavior).
        std::set<std::string> turbo_handled_sats;
        if (turbo_edit_resolver_) {
            const auto te = turbo_edit_resolver_->resolve(slip_results, modeled_obs, dt);

            auto isBssdPivot = [&](const std::string& sat) -> bool {
                if (!config_.enableBetweenSatelliteSd()) return false;
                const auto& pivots = meas_model_->getBssdPivotSatellites();
                auto it = pivots.find(PppUtils::getSystemString(sat));
                return it != pivots.end() && it->second == sat;
            };

            // Confident per-signal integer repairs (additive, covariance preserved).
            // The BSSD pivot's SD ambiguity is frozen at 0, so an integer shift is
            // meaningless there — reset instead and let the KF coherent-disturbance
            // detector / forwardKfPivotHandovers drive the re-referencing.
            for (const auto& r : te.repairs) {
                const std::string key = r.satellite + "_" + r.signal;
                const int idx = state_manager_->getAmbiguityIndex(key);
                if (idx < 0) continue;
                if (isBssdPivot(r.satellite)) {
                    state_manager_->resetAmbiguity(key, reset_std);
                    log->info("{}TurboEdit repair refused for {} {} (BSSD pivot); reset",
                              epoch_prefix, r.satellite, r.signal);
                } else if (!(r.lambda > 0.0)) {
                    state_manager_->resetAmbiguity(key, reset_std);
                    log->info("{}TurboEdit repair refused for {} {} (invalid wavelength); reset",
                              epoch_prefix, r.satellite, r.signal);
                } else {
                    Eigen::VectorXd x = state_manager_->getStateVector();
                    const double prev = x(idx);
                    x(idx) += static_cast<double>(r.dN) * r.lambda;
                    state_manager_->setStateVector(x);
                    log->info("{}TurboEdit REPAIR {} {}: dN={:+d} cyc, delta={:+.4f} m, "
                              "x: {:.4f} -> {:.4f} (covariance preserved)",
                              epoch_prefix, r.satellite, r.signal, r.dN,
                              static_cast<double>(r.dN) * r.lambda, prev, x(idx));
                }
                turbo_handled_sats.insert(r.satellite);
            }

            // Satellites the GF jump flagged but could not resolve (iono
            // disturbed / non-integer split): reset their phase ambiguities.
            for (const auto& sat : te.reset_sats) {
                const PppObservation* obs = nullptr;
                for (const auto& o : modeled_obs)
                    if (o.satellite == sat) { obs = &o; break; }
                if (!obs) continue;
                for (const auto& [sig, val] : obs->observed) {
                    (void)val;
                    if (sig.empty() || sig[0] != 'L') continue;
                    if (sig.find('_') != std::string::npos) continue;
                    const std::string key = sat + "_" + sig;
                    if (state_manager_->getAmbiguityIndex(key) < 0) continue;
                    state_manager_->resetAmbiguity(key, reset_std);
                }
                turbo_handled_sats.insert(sat);
            }
        }

        // Default reset path for everything TurboEdit did not handle.  LLI
        // resets (empty signal pair) always reset — a loss-of-lock is
        // authoritative; pair slips on a TurboEdit-handled satellite are
        // skipped to preserve its targeted per-signal repair.
        for (const auto& sr : slip_results) {
            const bool is_lli = sr.sig1.empty() || sr.sig2.empty();
            std::string sat = sr.satellite;
            if (sat.empty()) {
                const auto p = sr.amb_key.find('_');
                sat = (p != std::string::npos) ? sr.amb_key.substr(0, p) : sr.amb_key;
            }
            if (!is_lli && turbo_handled_sats.count(sat)) continue;
            state_manager_->resetAmbiguity(sr.amb_key, reset_std);
        }
    }

    // --- SSR phase-bias discontinuity -> ambiguity reset ---
    // An incremented IGS-SSR discontinuity counter means the satellite phase
    // bias for a signal jumped, breaking the integer property of that signal's
    // ambiguity; reset it before the KF update.  No-op when no SSR phase biases
    // are present (post-processing / corrections disabled).
    {
        auto& ssr = SsrCorrectionStore::getInstance();
        const double reset_std = config_.getAmbiguityStd();
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.modeled) {
                const std::string& code = kv.first;
                if (code.empty() || code[0] != 'L') continue;       // phase only
                if (code.find('_') != std::string::npos) continue;   // raw signal only
                if (auto pb = ssr.getPhaseBias(obs.satellite, code)) {
                    if (state_manager_->noteSsrDiscontinuity(
                            obs.satellite + "|" + code, pb->discontinuity_counter)) {
                        state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                        log->info("{}SSR phase-bias discontinuity: reset {} {}",
                                  epoch_prefix, obs.satellite, code);
                    }
                }
            }
        }
    }

    // --- KF measurement update (with outlier / pivot-handover rebuild loop) ---
    // The real-time path previously called update() once and discarded its
    // result, so a NeedsMeasurementRebuild return (post-fit outlier or a forced
    // pivot-handover request from the KF's coherent-disturbance detector) was
    // silently dropped: the measurement was never re-applied and the handover
    // was never forwarded to PivotManager, leaving the disturbed pivot in place
    // for every subsequent epoch.  This mirrors the core of
    // Application::runFilterUpdateLoop: rebuild on NeedsMeasurementRebuild,
    // forward each constellation's forced handover, carry excluded rows, and
    // peel one outlier per pass.  AR runs AFTER convergence on the committed
    // state (below).  Geometry relinearization is intentionally not replicated
    // here — it is a post-processing-only refinement.
    EpochMeasurement meas_update;
    bool update_applied = false;
    {
        // Pass budget + forced-pivot-handover forwarding are shared with the
        // post-processing driver via EpochUpdateSupport so the two paths can
        // never diverge on them again (the divergence that once left real-time
        // with no handover at all).
        const int max_passes =
            computeRebuildPassBudget(*state_manager_, config_, modeled_obs);

        std::set<std::string> carry_excluded;
        size_t carry_bad = 0;
        for (int pass = 0; pass < max_passes; ++pass) {
            meas_update = meas_model_->computeMeasurementUpdate(
                modeled_obs, state_manager_->getStateVector(), carry_excluded);
            if (meas_update.innovations.size() == 0
                || !meas_model_->validateMeasurementUpdate(meas_update))
                break;

            meas_update.bad_residual_count = carry_bad;
            const auto result = kalman_filter_->update(meas_update);
            if (result != KalmanFilter::UpdateResult::NeedsMeasurementRebuild) {
                update_applied = true;   // clean commit (or post-reset commit)
                break;
            }

            // Rebuild requested: carry the running tally/exclusions, then forward
            // any forced pivot handovers (re-references the SD frame and
            // re-inflates contaminated non-pivot states on the next pass).
            carry_bad = meas_update.bad_residual_count;
            carry_excluded = meas_update.excluded_measurements;
            forwardKfPivotHandovers(
                *kalman_filter_, *meas_model_, mw_slip_detector_.get(),
                log_manager_, epoch_prefix, carry_excluded);

            // Re-inflate iono / sat-bias variances for any sat whose phase
            // ambiguities the KF fully reset this pass, so the reseed doesn't
            // bake a stale partition.  Shared with the post-processing driver;
            // previously absent on the real-time path (the known asymmetry).
            // No-op on a complete reset / when slip_state_reinflation is off.
            reinflateResetSatStates(*kalman_filter_, *state_manager_,
                                    *meas_model_, config_);
        }
    }

    // --- Soft a-priori ZWD constraint ---
    // Anchors the BSSD [up, ZWD] near-null subspace each epoch so a common-mode
    // start error cannot run ZWD/height to ~1000 m.  One pseudo-obs row pulling
    // ZWD toward a climatological prior; applyPseudoObservation updates BOTH the
    // state and the covariance (shrinking the [up,ZWD] correlation), which is
    // what lets the height recover — a value-only clamp would not.  Reached only
    // after the startup gate, so it never fires during warm-up.
    {
        const auto& zc = config_.getZwdConstraint();
        if (zc.enabled && zc.sigma_m > 0.0) {
            const Eigen::VectorXd x = state_manager_->getStateVector();
            const size_t tropo_idx = state_manager_->getTropoIndex();
            if (tropo_idx < static_cast<size_t>(x.size())) {
                Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, x.size());
                H(0, static_cast<Eigen::Index>(tropo_idx)) = 1.0;
                Eigen::VectorXd z(1);
                z(0) = zc.apriori_m - x(static_cast<Eigen::Index>(tropo_idx));
                Eigen::MatrixXd R(1, 1);
                R(0, 0) = zc.sigma_m * zc.sigma_m;
                kalman_filter_->applyPseudoObservation(H, z, R);
            }
        }
    }

    // --- Integer ambiguity resolution (MLAMBDA) ---
    // Mirrors Application::run: only after a successful float update, feed the
    // MW-resolved WL integers + fractions to the resolver; on a confident fix,
    // apply the fixed state/covariance and fix-and-hold.
    if (ambiguity_resolver_ && update_applied) {
        const auto wl_ambs  = mw_slip_detector_->getResolvedWlAmbiguities();
        const auto wl_fracs = mw_slip_detector_->getResolvedWlFractions();
        auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs, wl_ambs, wl_fracs);
        if (ar_result.success) {
            state_manager_->setStateVector(ar_result.fixed_state);
            state_manager_->setCovariance(ar_result.fixed_covariance);
            ambiguity_resolver_->holdAmbiguities(*state_manager_, ar_result);
            log->info("{}AR: fixed {}/{} ambs, ratio={:.2f}",
                      epoch_prefix, ar_result.num_fixed, ar_result.num_candidates, ar_result.ratio);
        }
    }

    // --- Extract posterior state ---
    state_manager_->updatePppStateFromVector(
        state_manager_->getStateVector(), ppp_state_, *state_manager_);

    ++epoch_count_;

    log->info("{}Epoch {}: pos=({:.3f},{:.3f},{:.3f}) clk={:.3f} zwd={:.4f} sats={}",
              epoch_prefix, epoch_count_,
              ppp_state_.position.x(), ppp_state_.position.y(), ppp_state_.position.z(),
              ppp_state_.clock_bias, ppp_state_.troposphere_zwd,
              modeled_obs.size());

    // --- Trace logging ---
    if (trace_logger_ && trace_logger_->isInitialized()) {
        // Trace files carry pure GPS time so timestamps line up exactly
        // with RINEX epoch records.  See Application::writeEpochTraces for
        // the rationale (no leap-second conversion in either direction).
        const GnssTime& gps_time = current_time;
        trace_logger_->logState(gps_time, *state_manager_);
        trace_logger_->logAmbiguities(gps_time, *state_manager_);
        trace_logger_->logIonosphere(gps_time, *state_manager_);
        trace_logger_->logSatBiases(gps_time, *state_manager_);
        trace_logger_->logResiduals(gps_time, meas_update);

        // Satellite-status strip chart: real-time engine has no CSM-driven
        // MW slip set (slip detection lives in the post-processing
        // Application path), so the only slip signal here is the KF's
        // ambiguity-reset list from the latest update.  Raw epoch_obs is
        // forwarded so the logger can surface BelowMask rows for sats
        // dropped by ModelGeometry.  No-op for an unconfigured trace.
        const auto& reset_vec = kalman_filter_->getLastResetSatellites();
        const std::set<std::string> reset_sats(reset_vec.begin(), reset_vec.end());
        trace_logger_->logSatelliteStatus(
            gps_time, epoch_obs, modeled_obs, drop_reasons,
            meas_update, /*have_applied=*/update_applied,
            *state_manager_, /*slipped_sats=*/{}, reset_sats,
            config_.getElevationMaskDeg());
        trace_logger_->logDops(
            gps_time, modeled_obs, meas_update, /*have_applied=*/update_applied,
            *state_manager_);
    }

    return true;
}

// ---------------------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------------------

Eigen::Vector3d RealTimeEngine::getPosition() const {
    return ppp_state_.position;
}

double RealTimeEngine::getClockBias() const {
    return ppp_state_.clock_bias;
}

double RealTimeEngine::getTroposphereZwd() const {
    return ppp_state_.troposphere_zwd;
}

} // namespace GnssProcessor
