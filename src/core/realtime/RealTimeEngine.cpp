/**
 * @file RealTimeEngine.cpp
 * @brief Real-time GNSS PPP processing engine implementation.
 *
 * Mirrors the per-epoch logic of Application (post-processing) but designed
 * for epoch-at-a-time streaming operation rather than batch iteration.
 */

#include "RealTimeEngine.hpp"
#include "ProductCache.hpp"
#include "Coordinates.hpp"
#include "Constants.hpp"
#include <cmath>
#include <limits>
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
    // we construct it but parsing the observation file is optional — if one is
    // configured we parse it to seed the header, otherwise we set defaults.
    rinex_parser_ = std::make_unique<RinexParser>(config_, log_manager_, *product_parser_);

    std::filesystem::path obs_path = config_.getObservationPath();
    if (!obs_path.empty() && std::filesystem::exists(obs_path)) {
        if (!rinex_parser_->parse()) {
            log->warn("Failed to parse RINEX for header seeding — using defaults");
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
    if_slip_detector_  = std::make_unique<IfPhaseSlipDetector>(config_, log_manager_);

    ppp_model_->initializeState(ppp_state_);
    state_manager_->syncPppStateToStateVector(ppp_state_);

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    std::filesystem::path log_dir{"logs"};
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir, is_ppp_rtk);

    mw_slip_detector_->enableTracing(log_dir);
    if (config_.enableIfSlipDetector()) {
        if_slip_detector_->enableTracing(log_dir);
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
    RtcmParser parser("");
    if (!parser.parseMessage(frame)) return;

    // Convert RtcmObservations to RinexObservations and buffer them.
    for (const auto& rtcm_obs : parser.getObservations()) {
        RinexObservation obs;
        obs.satellite    = rtcm_obs.satellite;
        obs.system       = rtcm_obs.system;
        obs.time         = rtcm_obs.time;
        obs.measurements = rtcm_obs.measurements;
        obs.variances    = rtcm_obs.variances;

        // Detect epoch boundary.
        if (pending_obs_.empty()) {
            pending_epoch_time_ = obs.time;
        } else if (std::abs(obs.time.tow - pending_epoch_time_.tow) > 0.5) {
            // New epoch — process the buffered one.
            processEpoch(pending_obs_);
            pending_obs_.clear();
            pending_epoch_time_ = obs.time;
        }

        pending_obs_.push_back(std::move(obs));
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

    const std::string epoch_prefix =
        "[RT " + TimeUtils::formatGnssTime(current_time) + "] ";

    // --- Time step / gap detection ---
    double dt;
    bool gap_detected = false;
    if (first_epoch_) {
        dt = 1.0; // assume 1 s for first real-time epoch
        log->info("{}First epoch", epoch_prefix);
    } else {
        dt = current_time.tow - prev_tow_;
        if (dt < 0.0) dt += TimeUtils::SECONDS_IN_WEEK; // week rollover
        gap_detected = dt > 10.0; // >10 s gap
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
    if (!ppp_model_->computeModels(filtered_obs, ppp_state_, modeled_obs)) {
        log->warn("{}Failed to compute models", epoch_prefix);
        return false;
    }

    // --- WLS seeding ---
    {
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

    // --- Cycle slip detection ---
    {
        const auto mw_resets = mw_slip_detector_->detectLargeMwResets(modeled_obs);
        const double reset_std = config_.getAmbiguityStd();
        for (const auto& key : mw_resets) {
            state_manager_->resetAmbiguity(key, reset_std);
        }
    }
    if (config_.enableIfSlipDetector()) {
        const auto if_resets = if_slip_detector_->detect(modeled_obs, prev_pos_sigma_3d_);
        const double reset_std = config_.getAmbiguityStd();
        for (const auto& key : if_resets) {
            state_manager_->resetAmbiguity(key, reset_std);
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

    // --- Position sigma for next epoch's IF slip gating ---
    {
        const Eigen::MatrixXd P = state_manager_->getCovariance();
        size_t pi = state_manager_->getPositionIndex();
        if (P.rows() > static_cast<Eigen::Index>(pi + 2)) {
            double var3d = P(pi, pi) + P(pi + 1, pi + 1) + P(pi + 2, pi + 2);
            prev_pos_sigma_3d_ = std::sqrt(std::max(var3d, 0.0));
        }
    }

    ++epoch_count_;

    log->info("{}Epoch {}: pos=({:.3f},{:.3f},{:.3f}) clk={:.3f} zwd={:.4f} sats={}",
              epoch_prefix, epoch_count_,
              ppp_state_.position.x(), ppp_state_.position.y(), ppp_state_.position.z(),
              ppp_state_.clock_bias, ppp_state_.troposphere_zwd,
              modeled_obs.size());

    // --- Trace logging ---
    if (trace_logger_ && trace_logger_->isInitialized()) {
        const GnssTime utc = gpsToUtcTime(current_time);
        trace_logger_->logState(utc, *state_manager_);
        trace_logger_->logAmbiguities(utc, *state_manager_);
        trace_logger_->logIonosphere(utc, *state_manager_);
        trace_logger_->logResiduals(utc, meas_update);
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
