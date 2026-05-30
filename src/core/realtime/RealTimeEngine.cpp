/**
 * @file RealTimeEngine.cpp
 * @brief Real-time GNSS PPP processing engine implementation.
 *
 * Mirrors the per-epoch logic of Application (post-processing) but designed
 * for epoch-at-a-time streaming operation rather than batch iteration.
 */

#include "realtime/RealTimeEngine.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "math/Coordinates.hpp"
#include "common/Constants.hpp"
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

    // ProductParser (loads static products: ATX, BIA, broadcast nav).
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

    // Load products.
    product_parser_->loadProducts();

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
    {
        Eigen::VectorXd sv = state_manager_->getStateVector();
        if (sv.size() >= static_cast<Eigen::Index>(state_manager_->getPositionIndex() + 3)) {
            sv.segment<3>(state_manager_->getPositionIndex()) = initial_position_;
            state_manager_->setStateVector(sv);
        }
    }

    // Processing components.
    ppp_model_       = std::make_unique<PppModel>(config_, *product_parser_, log_manager_, *rinex_parser_, *state_manager_);
    meas_model_      = std::make_unique<MeasurementModel>(*state_manager_, *ppp_model_, config_, log_manager_);
    kalman_filter_   = std::make_unique<KalmanFilter>(*state_manager_, config_, log_manager_, *ppp_model_);
    wls_estimator_   = std::make_unique<WlsEstimator>(config_, *state_manager_, log_manager_);
    mw_slip_detector_  = std::make_unique<MwSlipDetector>(config_, log_manager_);

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

    ppp_model_->initializeState(ppp_state_);
    state_manager_->syncPppStateToStateVector(ppp_state_);

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    std::filesystem::path log_dir{"logs"};
    const auto& logging_cfg = config_.getLoggingConfig();
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir, is_ppp_rtk, logging_cfg);

    if (logging_cfg.enable_mw_slip_trace) {
        mw_slip_detector_->enableTracing(log_dir);
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

    // Push decoded SSR records into the shared SsrCorrectionStore.  The
    // orbit / clock query paths in Geometry and GnssCorrections consult
    // the store transparently — no further wiring at the per-epoch level.
    auto& ssr_store = SsrCorrectionStore::getInstance();
    SsrHeader hdr{};   // Header re-emit isn't available per-record yet; pass empty.
    for (const auto& r : parser.getSsrOrbits())         ssr_store.pushOrbit(r, hdr);
    for (const auto& r : parser.getSsrClocks())         ssr_store.pushClock(r, hdr);
    for (const auto& r : parser.getSsrCombined())       ssr_store.pushCombined(r, hdr);
    for (const auto& r : parser.getSsrCodeBiases())     ssr_store.pushCodeBias(r, hdr);
    for (const auto& r : parser.getSsrPhaseBiases())    ssr_store.pushPhaseBias(r, hdr);
    for (const auto& r : parser.getSsrUras())           ssr_store.pushUra(r, hdr);
    for (const auto& r : parser.getSsrHighRateClocks()) ssr_store.pushHighRateClock(r, hdr);

    const std::size_t n_ssr =
        parser.getSsrOrbits().size()      + parser.getSsrClocks().size() +
        parser.getSsrCombined().size()    + parser.getSsrCodeBiases().size() +
        parser.getSsrPhaseBiases().size() + parser.getSsrUras().size() +
        parser.getSsrHighRateClocks().size();
    if (n_ssr > 0) {
        log_manager_.getLogger("rt_engine")->trace(
            "RTCM {}: pushed {} SSR records to store ({} sats cached)",
            parser.lastMessageType(), n_ssr, ssr_store.size());
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

void RealTimeEngine::feedObservations(const std::vector<RinexObservation>& observations) {
    processEpoch(observations);
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
            if (first_epoch_) first_epoch_ = false;
        }
    }

    // --- KF predict ---
    kalman_filter_->predict(dt);
    state_manager_->updatePppStateFromVector(
        state_manager_->getStateVector(), ppp_state_, *state_manager_);

    // --- Cycle slip detection (MW, pre-estimation) ---
    {
        const auto slip_results = mw_slip_detector_->detectLargeMwResets(modeled_obs);
        const double reset_std = config_.getAmbiguityStd();
        for (const auto& sr : slip_results) {
            state_manager_->resetAmbiguity(sr.amb_key, reset_std);
        }
    }

    // --- KF measurement update ---
    EpochMeasurement meas_update = meas_model_->computeMeasurementUpdate(
        modeled_obs, state_manager_->getStateVector());
    if (meas_update.innovations.size() > 0 && meas_model_->validateMeasurementUpdate(meas_update)) {
        kalman_filter_->update(meas_update);
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
            meas_update, /*have_applied=*/true,
            *state_manager_, /*slipped_sats=*/{}, reset_sats,
            config_.getElevationMaskDeg());
        trace_logger_->logDops(
            gps_time, modeled_obs, meas_update, /*have_applied=*/true,
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
