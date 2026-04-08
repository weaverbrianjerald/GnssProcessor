/*!
 * \file KalmanFilter.cpp
 * \brief Kalman filter implementation for PPP/PPP-RTK state estimation.
 *
 * Implements prediction and measurement update logic used by the GNSS processor.
 * The implementation is decomposed into focused helpers:
 *  - seedCovarianceDiagonals()  — initial P seeding (constructor helper).
 *  - handlePhaseOutlier()       — ambiguity reset for phase residual outliers.
 *  - handleCodeOutlier()        — measurement exclusion for code residual outliers.
 *  - resetFilterCovariance()    — complete filter reset when too many outliers accumulate.
 */

#include "KalmanFilter.hpp"
#include "KalmanFilterLogger.hpp"
#include "MeasurementModel.hpp"
#include "PppModel.hpp"
#include "TimeUtils.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <stdexcept>
#include <limits>
#include "Constants.hpp"

namespace GnssProcessor {

    // ===================================================================
    // File-scope helpers
    // ===================================================================

    namespace {

    const char* modeLabel(const GnssConfig& cfg) {
        return cfg.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK";
    }

    /// Predicate: value is non-finite or negative → needs initialization.
    /// Zero is a valid user-specified variance (fixed state).
    bool isUnsetVariance(double v) {
        return !std::isfinite(v) || v < 0.0;
    }

    /// Split a possibly-BSSD satellite ID ("G01-G05") into primary and reference.
    std::pair<std::string, std::string> splitBssdSatId(const std::string& sat_id) {
        const size_t dash = sat_id.find('-');
        if (dash != std::string::npos && dash > 0 && dash + 1 < sat_id.size())
            return { sat_id.substr(0, dash), sat_id.substr(dash + 1) };
        return { sat_id, "" };
    }

    } // anonymous namespace

    // ===================================================================
    // Construction helpers
    // ===================================================================

    void KalmanFilter::seedCovarianceDiagonals(Eigen::MatrixXd& P, size_t state_size) const {
        const auto& initial_std = config_.getInitialStd();
        const double pos_std   = initial_std.position >= 0            ? initial_std.position            : 1000.0;
        const double clock_std = initial_std.clock >= 0               ? initial_std.clock               : 1e6;
        const double tropo_std = initial_std.troposphere >= 0         ? initial_std.troposphere         : 0.5;
        const double amb_std   = initial_std.ambiguity >= 0           ? initial_std.ambiguity           : 1e4;
        const double isb_std   = initial_std.system_time_offset >= 0  ? initial_std.system_time_offset  : 100.0;

        const size_t pos_idx   = state_manager_.getPositionIndex();
        const size_t clock_idx = state_manager_.getClockIndex();
        const size_t tropo_idx = state_manager_.getTropoIndex();

        if (isUnsetVariance(P(pos_idx, pos_idx)))
            P.block(pos_idx, pos_idx, 3, 3) = Eigen::Matrix3d::Identity() * pos_std * pos_std;
        if (isUnsetVariance(P(clock_idx, clock_idx)))
            P(clock_idx, clock_idx) = clock_std * clock_std;
        if (isUnsetVariance(P(tropo_idx, tropo_idx)))
            P(tropo_idx, tropo_idx) = tropo_std * tropo_std;

        if (config_.estimatePhaseClock()) {
            const double pc_std = initial_std.phase_clock >= 0 ? initial_std.phase_clock : 1e3;
            const size_t pc_idx = state_manager_.getPhaseClockIndex();
            if (pc_idx < state_size && isUnsetVariance(P(pc_idx, pc_idx)))
                P(pc_idx, pc_idx) = pc_std * pc_std;
        }

        if (config_.estimateTropoGradients()) {
            const double grad_std = initial_std.tropo_gradient >= 0 ? initial_std.tropo_gradient : 0.001;
            const size_t gn_idx = state_manager_.getTropoGradientNorthIndex();
            const size_t ge_idx = state_manager_.getTropoGradientEastIndex();
            if (gn_idx < state_size && isUnsetVariance(P(gn_idx, gn_idx)))
                P(gn_idx, gn_idx) = grad_std * grad_std;
            if (ge_idx < state_size && isUnsetVariance(P(ge_idx, ge_idx)))
                P(ge_idx, ge_idx) = grad_std * grad_std;
        }

        for (const auto& sys : state_manager_.getSystemsWithTimeOffsets()) {
            const size_t idx = state_manager_.getSystemTimeOffsetIndex(sys);
            if (idx < state_size && isUnsetVariance(P(idx, idx)))
                P(idx, idx) = isb_std * isb_std;
        }

        const size_t num_amb = state_manager_.getAmbiguities().size();
        if (num_amb > 0) {
            P.bottomRightCorner(num_amb, num_amb).diagonal().setConstant(amb_std * amb_std);
            log_manager_.getLogger("kalman_filter")->info(
                "Initialized {} ambiguity states with variance: {:.6f} m^2", num_amb, amb_std * amb_std);
        }
    }

    // ===================================================================
    // Constructor / Destructor
    // ===================================================================

    KalmanFilter::KalmanFilter(StateManager& state_manager, const GnssConfig& config,
        LogManager& log_manager, const PppModel& ppp_model)
        : state_manager_(state_manager), config_(config), log_manager_(log_manager),
        ppp_model_(ppp_model), measurement_model_(state_manager, ppp_model, config, log_manager) {

        // Optional file logger (trace mode only)
        try {
            auto kalman_logger = log_manager_.getLogger("kalman_filter");
            if (kalman_logger && kalman_logger->level() == spdlog::level::trace) {
                try {
                    logger_ = std::make_unique<KalmanFilterLogger>("kalman_debug.log");
                    kalman_logger->info("Kalman debug file enabled: kalman_debug.log");
                } catch (const std::exception& e) {
                    kalman_logger->warn("Failed to open kalman debug file: {}", e.what());
                    logger_.reset();
                }
            }
        } catch (...) {
            // Silently continue without file logger.
        }

        const size_t state_size = state_manager_.getStateSize();
        size_t base_core_size = 5;
        if (config.estimateTropoGradients()) base_core_size += 2;
        if (state_size < base_core_size) {
            log_manager_.getLogger("kalman_filter")->error(
                "Invalid state size from StateManager: {}, minimum {} required for mode {}",
                state_size, base_core_size, modeLabel(config));
            throw std::invalid_argument("Invalid state size");
        }

        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        seedCovarianceDiagonals(P, state_size);

        const size_t num_amb = state_manager_.getAmbiguities().size();
        const size_t core_size = state_size - num_amb - state_manager_.getIonosphereStates().size();
        log_manager_.getLogger("kalman_filter")->info(
            "KalmanFilter initialized with state size: {} (core={}, time_offsets={}, ambiguities={}), mode: {}, P ({}x{}), x ({})",
            state_size, core_size, state_manager_.getNumSystemTimeOffsets(), num_amb,
            modeLabel(config_), P.rows(), P.cols(), x.size());

        state_manager_.setCovariance(P);

        if (logger_) logger_->logStateVector(x, "Initial State Vector");
        if (logger_) logger_->logCovarianceMatrix(P, "Initial Covariance Matrix");
    }

    KalmanFilter::~KalmanFilter() = default;

    // ===================================================================
    // Prediction
    // ===================================================================

    void KalmanFilter::predict(double dt) {
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();
        if (x.size() == 0 || P.rows() == 0 || P.cols() == 0) {
            log_manager_.getLogger("kalman_filter")->error("Filter not initialized");
            throw std::runtime_error("Filter not initialized");
        }

        // Get total state size (core states + ambiguities)
        size_t state_size = state_manager_.getStateSize();

        // Get process noise matrix
        Eigen::MatrixXd Q = state_manager_.getProcessNoiseMatrix(dt);
        if (Q.rows() != static_cast<long>(state_size) || Q.cols() != static_cast<long>(state_size)) {
            log_manager_.getLogger("kalman_filter")->error("Process noise Q dimensions incompatible: Q ({}x{}), expected ({}x{})",
                Q.rows(), Q.cols(), state_size, state_size);
            throw std::invalid_argument("Q dimensions mismatch");
        }

        // Get transition matrix
        Eigen::MatrixXd F = state_manager_.getTransitionMatrix(dt);
        if (F.rows() != static_cast<long>(state_size) || F.cols() != static_cast<long>(state_size)) {
            log_manager_.getLogger("kalman_filter")->error("Transition matrix F dimensions incompatible: F ({}x{}), expected ({}x{})",
                F.rows(), F.cols(), state_size, state_size);
            throw std::invalid_argument("F dimensions mismatch");
        }

        // Log before prediction
        if (logger_) logger_->logMatrix(F, "Transition Matrix F");
        if (logger_) logger_->logMatrix(Q, "Process Noise Matrix Q");

        // Predict state
		// TODO: simple dynamics, eliminate F. Just add process noise to P for random walk.
        x = F * x;
        P = F * P * F.transpose() + Q;

        // When BSSD is active, the clock, phase-clock and ISB states are
        // not observed (H columns are zero).  Zero their cross-covariances
        // so stale correlations don't leak into the SD solution.  The
        // diagonal (variance) is left as-is — it is already frozen because
        // the process noise for these states is suppressed.
        if (config_.enableBetweenSatelliteSd()) {
            const auto freeze_state = [&](size_t idx) {
                if (idx < static_cast<size_t>(P.rows())) {
                    const auto i = static_cast<Eigen::Index>(idx);
                    const double var = P(i, i);
                    P.row(i).setZero();
                    P.col(i).setZero();
                    P(i, i) = var;
                }
            };
            freeze_state(state_manager_.getClockIndex());
            if (state_manager_.hasPhaseClock())
                freeze_state(state_manager_.getPhaseClockIndex());
            for (const auto& sys : state_manager_.getSystemsWithTimeOffsets())
                freeze_state(state_manager_.getSystemTimeOffsetIndex(sys));
        }

        log_manager_.getLogger("kalman_filter")->trace("Predicted state for dt={:.3f} s: x size={}, P size={}x{}",
            dt, x.size(), P.rows(), P.cols());

        // Store
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);

        // Log after prediction
        if (logger_) logger_->logStateVector(x, "Predicted State Vector");
        if (logger_) logger_->logCovarianceMatrix(P, "Predicted Covariance Matrix");
    }

    // ===================================================================
    // Measurement update
    // ===================================================================

    KalmanFilter::UpdateResult KalmanFilter::update(EpochMeasurement& measurement) {
        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        last_reset_satellites_.clear();
        was_complete_reset_ = false;

        // --- Input validation ---
        if (measurement.innovations.size() == 0 || measurement.H.rows() == 0 || measurement.R_v.size() == 0) {
            klogger->warn("{}Invalid measurement update: innovations ({}), H ({}x{}), R_v ({})",
                epoch_prefix,
                measurement.innovations.size(), measurement.H.rows(), measurement.H.cols(), measurement.R_v.size());
            return UpdateResult::Skipped;
        }

        const size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(measurement.H.cols()) != state_size) {
            klogger->error("{}Measurement matrix H columns ({}) do not match state size ({})",
                epoch_prefix, measurement.H.cols(), state_size);
            throw std::invalid_argument("H dimensions mismatch");
        }

        // --- Build R ---
        if (measurement.R_full.rows() == measurement.innovations.size() &&
            measurement.R_full.cols() == measurement.innovations.size()) {
            R_ = measurement.R_full;
        } else {
            R_ = measurement.R_v.asDiagonal();
        }

        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        if (logger_) logger_->logVector(measurement.innovations, "Innovations Vector", epoch_tag);
        if (logger_) logger_->logMatrix(measurement.H, "Measurement Matrix H", epoch_tag);
        if (logger_) logger_->logMatrix(R_, "Measurement Noise Matrix R", epoch_tag);

        // --- Innovation covariance & Kalman gain ---
        const Eigen::MatrixXd Ht = measurement.H.transpose();
        const Eigen::MatrixXd S = measurement.H * P * Ht + R_;

        Eigen::JacobiSVD<Eigen::MatrixXd> svd(S);
        const double cond = svd.singularValues()(0)
            / svd.singularValues()(svd.singularValues().size() - 1);
        klogger->trace("{}S matrix condition number: {:.3e}", epoch_prefix, cond);

        Eigen::MatrixXd S_inv;
        Eigen::LDLT<Eigen::MatrixXd> ldlt(S);
        if (ldlt.info() == Eigen::Success) {
            S_inv = ldlt.solve(Eigen::MatrixXd::Identity(S.rows(), S.cols()));
        } else {
            klogger->warn("{}LDLT failed for S matrix (condition={:.3e}), falling back to pseudo-inverse",
                epoch_prefix, cond);
            S_inv = Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>(S).pseudoInverse();
        }

        const Eigen::MatrixXd K = P * Ht * S_inv;
        if (logger_) logger_->logKalmanGain(K, "Kalman Gain Matrix K", epoch_tag);

        // --- Save pre-update copy ---
        Eigen::VectorXd x_prev = x;
        Eigen::MatrixXd P_prev = P;

        // --- State correction ---
        const Eigen::VectorXd dx = K * measurement.innovations;
        x = x + dx;
        klogger->debug("{}Kalman update dx.norm={:.3e}", epoch_prefix, dx.norm());

        // --- Joseph covariance update ---
        const Eigen::MatrixXd I_minus_KH = Eigen::MatrixXd::Identity(state_size, state_size)
            - K * measurement.H;
        P = I_minus_KH * P * I_minus_KH.transpose() + K * R_ * K.transpose();
        P = 0.5 * (P + P.transpose());

        // --- Post-fit residuals ---
        measurement.residuals = measurement.innovations - measurement.H * dx;
        const Eigen::MatrixXd Sr = R_ - measurement.H * P * Ht;
        measurement.residuals_covariance = Sr;

        Eigen::VectorXd sres(measurement.residuals.size());
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double sii = (i < Sr.rows() && i < Sr.cols()) ? Sr(i, i) : std::numeric_limits<double>::quiet_NaN();
            const double denom = std::sqrt(std::max(sii, 1e-12));
            const double val = measurement.residuals(i);
            sres(i) = (std::isfinite(val) && std::isfinite(denom) && denom > 0.0) ? (val / denom) : 0.0;
        }
        measurement.residuals_standardized = sres;

        // --- Outlier handling ---
        auto [is_outlier, is_reset] = validatePostFitResiduals(measurement, x_prev, P_prev);
        was_complete_reset_ = is_reset;

        if (is_outlier) {
            state_manager_.setStateVector(x_prev);
            state_manager_.setCovariance(P_prev);

            klogger->info("{}{}. Measurement rebuild required.",
                epoch_prefix,
                is_reset ? "Complete filter reset requested by residual check"
                         : "Outlier handled (ambiguity reset/R change)");

            if (logger_) logger_->logStateVector(x_prev, "State After Outlier Handling", epoch_tag);
            if (logger_) logger_->logCovarianceMatrix(P_prev, "Covariance After Outlier Handling", epoch_tag);

            return UpdateResult::NeedsMeasurementRebuild;
        }

        // --- Commit ---
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);
        state_manager_.setStateEpochTime(measurement.time);

        // Per-satellite residual summary
        {
            const size_t n_res = std::min({ static_cast<size_t>(measurement.residuals.size()),
                                            measurement.satellite_ids.size(),
                                            measurement.signal_types.size() });
            std::string res_summary;
            for (size_t i = 0; i < n_res; ++i) {
                const double r = measurement.residuals(static_cast<Eigen::Index>(i));
                res_summary += measurement.satellite_ids[i];
                res_summary += ':';
                // Use spdlog-compatible formatting style via a small buffer
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%+.2fm ", r);
                res_summary += buf;
            }
            klogger->debug("{}Post-fit residuals: {}", epoch_prefix, res_summary);
        }

        if (logger_) logger_->logStateVector(x, "Updated State Vector", epoch_tag);
        if (logger_) logger_->logCovarianceMatrix(P, "Updated Covariance Matrix", epoch_tag);

        return UpdateResult::Applied;
    }

    // ===================================================================
    // Outlier handling helpers
    // ===================================================================

    void KalmanFilter::handlePhaseOutlier(const EpochMeasurement& measurement,
        const std::string& outlier_sat, const std::string& outlier_sig,
        Eigen::VectorXd& x, Eigen::MatrixXd& P,
        const std::string& epoch_prefix) {

        auto klogger = log_manager_.getLogger("kalman_filter");
        const double amb_std = config_.getAmbiguityStd();
        const auto [primary_sat, ref_sat] = splitBssdSatId(outlier_sat);

        // Helper: reset a single ambiguity and track the satellite
        auto resetOneAmbiguity = [&](const std::string& sat, const std::string& label) {
            const std::string amb_key = sat + "_" + outlier_sig;
            const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);

            klogger->info("{}Phase outlier action ({}): ambiguity reset for key={}, amb_idx={}",
                epoch_prefix, label, amb_key, amb_idx);

            if (amb_idx < 0) {
                klogger->warn("{}Phase outlier but ambiguity key not found: {}", epoch_prefix, amb_key);
                return;
            }

            const double prev_amb = x(amb_idx);
            double seed_value = 0.0;
            for (const auto& obs : measurement.observations) {
                if (obs.satellite != sat) continue;
                seed_value = state_manager_.computeAmbiguitySeed(obs, outlier_sig, x);
                break;
            }

            x(amb_idx) = seed_value;
            P.row(amb_idx).setZero();
            P.col(amb_idx).setZero();
            P(amb_idx, amb_idx) = amb_std * amb_std;

            klogger->info("{}Reinitialized ambiguity for {} {} (idx {}): prev={:.6f} m -> seed={:.3f} m, std={:.3f} m",
                epoch_prefix, sat, outlier_sig, amb_idx, prev_amb, seed_value, amb_std);

            last_reset_satellites_.push_back(sat);
        };

        resetOneAmbiguity(primary_sat, "primary");

        // BSSD: also reset the reference satellite's ambiguity
        if (!ref_sat.empty())
            resetOneAmbiguity(ref_sat, "BSSD-ref");
    }

    void KalmanFilter::handleCodeOutlier(EpochMeasurement& measurement,
        const std::string& outlier_sat, const std::string& outlier_sig,
        size_t max_idx, const std::string& epoch_prefix) {

        auto klogger = log_manager_.getLogger("kalman_filter");
        const auto [primary_sat, ref_sat] = splitBssdSatId(outlier_sat);

        const std::string excl_key = primary_sat + "_" + outlier_sig;
        measurement.excluded_measurements.insert(excl_key);

        klogger->info("{}Code outlier action: excluding measurement {} {} (key={})",
            epoch_prefix, outlier_sat, outlier_sig, excl_key);

        // Remove the offending row from all measurement vectors/matrices
        const auto idx = static_cast<Eigen::Index>(max_idx);
        const Eigen::Index n = measurement.innovations.size();

        if (n > 1) {
            const Eigen::Index remaining = n - idx - 1;
            if (remaining > 0) {
                measurement.innovations.segment(idx, remaining) = measurement.innovations.segment(idx + 1, remaining);
                measurement.R_v.segment(idx, remaining) = measurement.R_v.segment(idx + 1, remaining);
                measurement.H.middleRows(idx, remaining) = measurement.H.middleRows(idx + 1, remaining);
                measurement.residuals.segment(idx, remaining) = measurement.residuals.segment(idx + 1, remaining);
                measurement.residuals_standardized.segment(idx, remaining) = measurement.residuals_standardized.segment(idx + 1, remaining);
                for (Eigen::Index r = idx; r < n - 1; ++r)
                    for (Eigen::Index c = idx; c < n - 1; ++c)
                        measurement.residuals_covariance(r, c) = measurement.residuals_covariance(r + 1, c + 1);
            }
            measurement.innovations.conservativeResize(n - 1);
            measurement.R_v.conservativeResize(n - 1);
            measurement.H.conservativeResize(n - 1, measurement.H.cols());
            measurement.residuals.conservativeResize(n - 1);
            measurement.residuals_standardized.conservativeResize(n - 1);
            measurement.residuals_covariance.conservativeResize(n - 1, n - 1);
        } else {
            measurement.innovations.resize(0);
            measurement.R_v.resize(0);
            measurement.H.resize(0, measurement.H.cols());
            measurement.residuals.resize(0);
            measurement.residuals_standardized.resize(0);
            measurement.residuals_covariance.resize(0, 0);
        }
        measurement.satellite_ids.erase(measurement.satellite_ids.begin() + static_cast<ptrdiff_t>(max_idx));
        measurement.signal_types.erase(measurement.signal_types.begin() + static_cast<ptrdiff_t>(max_idx));
    }

    void KalmanFilter::resetFilterCovariance(Eigen::MatrixXd& P, const std::string& epoch_prefix) {
        auto klogger = log_manager_.getLogger("kalman_filter");
        const auto& initial_std = config_.getInitialStd();
        const double amb_std = config_.getAmbiguityStd();

        P.setZero();

        // Core states: position (3), clock (1), tropo ZWD (1)
        const double pos_var   = initial_std.position >= 0 ? initial_std.position * initial_std.position : 10000.0;
        const double clk_var   = initial_std.clock >= 0    ? initial_std.clock * initial_std.clock       : 90000.0;
        const double tropo_var = initial_std.troposphere >= 0 ? initial_std.troposphere * initial_std.troposphere : 0.25;
        P(0, 0) = pos_var;
        P(1, 1) = pos_var;
        P(2, 2) = pos_var;
        P(3, 3) = clk_var;
        P(4, 4) = tropo_var;

        // Tropospheric gradients
        if (config_.estimateTropoGradients()) {
            const double grad_var = initial_std.tropo_gradient >= 0
                ? initial_std.tropo_gradient * initial_std.tropo_gradient : 1e-6;
            P(state_manager_.getTropoGradientNorthIndex(), state_manager_.getTropoGradientNorthIndex()) = grad_var;
            P(state_manager_.getTropoGradientEastIndex(), state_manager_.getTropoGradientEastIndex()) = grad_var;
        }

        // Inter-system time offsets
        const double isb_var = initial_std.system_time_offset >= 0
            ? initial_std.system_time_offset * initial_std.system_time_offset : 10000.0;
        for (const auto& sys : state_manager_.getSystemsWithTimeOffsets()) {
            const size_t idx = state_manager_.getSystemTimeOffsetIndex(sys);
            if (idx < static_cast<size_t>(P.rows()))
                P(idx, idx) = isb_var;
        }

        // Per-satellite ionosphere states (PPP-RTK)
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            const double iono_var = initial_std.ionosphere * initial_std.ionosphere;
            for (const auto& [sat, idx] : state_manager_.getIonosphereStates()) {
                if (idx < static_cast<size_t>(P.rows()))
                    P(idx, idx) = iono_var;
            }
        }

        // Ambiguities
        for (const auto& [key, idx] : state_manager_.getAmbiguities()) {
            if (idx < static_cast<size_t>(P.rows()))
                P(idx, idx) = amb_std * amb_std;
        }

        klogger->warn("{}Complete reset applied: state_size={}, ambiguity_std={:.3f} m",
            epoch_prefix, static_cast<size_t>(P.rows()), amb_std);
    }

    // ===================================================================
    // Post-fit residual validation
    // ===================================================================

    std::pair<bool, bool> KalmanFilter::validatePostFitResiduals(
        EpochMeasurement& measurement, Eigen::VectorXd& x, Eigen::MatrixXd& P) {

        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        const double threshold = config_.getResidualThreshold();
        const double bad_meas_percent = config_.getBadMeasPercent();

        const Eigen::VectorXd& res  = measurement.residuals;
        const Eigen::MatrixXd& vres = measurement.residuals_covariance;
        const Eigen::VectorXd& sres = measurement.residuals_standardized;

        const size_t num_obs = static_cast<size_t>(res.size());
        if (num_obs == 0) {
            klogger->warn("{}No observations for post-fit validation", epoch_prefix);
            return { false, false };
        }

        const size_t meta_n = std::min({ num_obs, measurement.satellite_ids.size(), measurement.signal_types.size() });

        // --- Per-measurement diagnostic log ---
        klogger->debug("{}Post-fit residual check: n_obs={}, threshold={:.3f}, bad_count={}, bad_percent={:.3f}",
            epoch_prefix, num_obs, threshold, measurement.bad_residual_count, bad_meas_percent);

        for (size_t i = 0; i < meta_n; ++i) {
            const auto ei = static_cast<Eigen::Index>(i);
            const double var_i = (i < static_cast<size_t>(vres.rows()) && i < static_cast<size_t>(vres.cols()))
                ? vres(ei, ei) : std::numeric_limits<double>::quiet_NaN();
            const double sigma_i = std::sqrt(std::max(var_i, 0.0));
            const double s_i = sres(ei);
            const double s_abs = std::abs(s_i);
            klogger->debug(
                "{}  res[{:02d}] {} {}: v={:+.4f} m, sigma={:.4f} m, s={:+.3f}, |s|={:.3f}, outlier={}",
                epoch_prefix, static_cast<int>(i),
                measurement.satellite_ids[i], measurement.signal_types[i],
                res(ei), sigma_i, s_i, s_abs,
                (std::isfinite(s_abs) && s_abs > threshold) ? "YES" : "no");
        }

        if (meta_n != num_obs) {
            klogger->warn("{}Residual metadata size mismatch: res={}, sat_ids={}, sig_types={}. Logged {} entries.",
                epoch_prefix, num_obs, measurement.satellite_ids.size(), measurement.signal_types.size(), meta_n);
        }

        // --- Find worst residual ---
        const Eigen::VectorXd sres_abs = sres.cwiseAbs();
        size_t max_idx = 0;
        const double max_sres = sres_abs.maxCoeff(&max_idx);

        size_t num_over = 0;
        for (Eigen::Index i = 0; i < sres.size(); ++i)
            if (std::isfinite(std::abs(sres(i))) && std::abs(sres(i)) > threshold)
                ++num_over;

        klogger->debug("{}Residual summary: max|sres|={:.3f} at idx={}, over_threshold={}/{}",
            epoch_prefix, max_sres, max_idx, num_over, num_obs);

        // --- Dispatch outlier handling ---
        bool is_outlier_found = false;
        bool is_complete_reset = false;

        if (max_sres > threshold) {
            const std::string outlier_sat = measurement.satellite_ids[max_idx];
            const std::string outlier_sig = measurement.signal_types[max_idx];
            const double sigma_max = std::sqrt(std::max(
                vres(static_cast<Eigen::Index>(max_idx), static_cast<Eigen::Index>(max_idx)), 0.0));

            klogger->warn(
                "{}Max standardized residual for {} {}: |s|={:.3f} (s={:+.3f}, residual {:.3f} m, sigma {:.3f} m, idx {})",
                epoch_prefix, outlier_sat, outlier_sig,
                max_sres, sres(static_cast<Eigen::Index>(max_idx)),
                res(static_cast<Eigen::Index>(max_idx)), sigma_max, max_idx);

            if (!outlier_sig.empty() && outlier_sig[0] == 'L') {
                handlePhaseOutlier(measurement, outlier_sat, outlier_sig, x, P, epoch_prefix);
            } else {
                handleCodeOutlier(measurement, outlier_sat, outlier_sig, max_idx, epoch_prefix);
            }

            is_outlier_found = true;
            measurement.bad_residual_count++;

            klogger->warn("{}Outlier flagged: idx={}, sat={}, sig={}, max_sres={:.3f} > threshold={:.3f}; bad_residual_count={}",
                epoch_prefix, max_idx, outlier_sat, outlier_sig,
                max_sres, threshold, measurement.bad_residual_count);
        } else {
            klogger->debug("{}No outliers: max_sres={:.3f} <= threshold={:.3f}",
                epoch_prefix, max_sres, threshold);
        }

        // --- Complete reset if too many outliers accumulated ---
        if (measurement.bad_residual_count > static_cast<size_t>(bad_meas_percent * num_obs)) {
            klogger->warn("{}Resetting filter: too many outliers ({}/{}), limit={:.3f}*n={}",
                epoch_prefix, measurement.bad_residual_count, num_obs,
                bad_meas_percent, static_cast<size_t>(bad_meas_percent * num_obs));

            resetFilterCovariance(P, epoch_prefix);

            is_outlier_found = true;
            is_complete_reset = true;
            measurement.bad_residual_count = 0;
        }

        klogger->debug("{}Post-fit residual check result: outlier_found={}, complete_reset={}",
            epoch_prefix,
            is_outlier_found ? "true" : "false",
            is_complete_reset ? "true" : "false");

        return { is_outlier_found, is_complete_reset };
    }

} // namespace GnssProcessor