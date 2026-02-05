/*!
 * \file KalmanFilter.cpp
 * \brief Kalman filter implementation for PPP/PPP-RTK state estimation.
 *
 * Implements prediction and measurement update logic used by the GNSS processor.
 * The Kalman filter interacts with a StateManager for state/covariance storage,
 * a GnssConfig for runtime parameters, and a PppModel/MeasurementModel for
 * measurement modelling. Detailed logging is performed via \c LogManager and
 * \c KalmanFilterLogger.
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

    /**
     * @brief Construct a KalmanFilter instance.
     *
     * Initializes local measurement helper and Kalman logger, validates the
     * state size obtained from \c StateManager against the processing mode,
     * and seeds the covariance diagonal entries for core states and ambiguities
     * using values from \c GnssConfig if the corresponding diagonal entries
     * are unset/invalid. Persists the initialized covariance back to the
     * \c StateManager and emits initial state/covariance logs.
     *
     * @param state_manager Reference to the StateManager that owns the filter state.
     * @param config Read-only reference to configuration parameters (GnssConfig).
     * @param log_manager Reference to LogManager for emitting diagnostic messages.
     * @param ppp_model Read-only reference to the PPP model used by the measurement model.
     *
     * @throws std::invalid_argument if the state size from \c state_manager is smaller
     *         than the minimum required for the configured processing mode.
     */
    KalmanFilter::KalmanFilter(StateManager& state_manager, const GnssConfig& config,
        LogManager& log_manager, const PppModel& ppp_model)
        : state_manager_(state_manager), config_(config), log_manager_(log_manager),
        ppp_model_(ppp_model), measurement_model_(state_manager, ppp_model, config, log_manager) {

        // Create kalman_debug.log only when runtime logging level is Trace.
        // Check the module logger "kalman_filter" (will be created by LogManager if necessary)
        try {
            auto kalman_logger = log_manager_.getLogger("kalman_filter");
            if (kalman_logger && kalman_logger->level() == spdlog::level::trace) {
                try {
                    logger_ = std::make_unique<KalmanFilterLogger>("kalman_debug.log");
                    kalman_logger->info("Kalman debug file enabled: kalman_debug.log");
                }
                catch (const std::exception& e) {
                    kalman_logger->warn("Failed to open kalman debug file: {}", e.what());
                    logger_.reset();
                }
            }
        }
        catch (...) {
            // If LogManager throws or getLogger fails, silently continue without file logger.
        }

        // Get total state size (core states + initial ambiguities)
        size_t core_size = state_manager_.getStateSize() - state_manager_.getAmbiguities().size();
        size_t state_size = state_manager_.getStateSize();
        size_t base_core_size = config.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
        if (state_size < base_core_size) {
            log_manager_.getLogger("kalman_filter")->error("Invalid state size from StateManager: {}, minimum {} required for mode {}",
                state_size, base_core_size,
                config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            throw std::invalid_argument("Invalid state size");
        }

        // Initialize state vector and matrices from StateManager (local temporaries only)
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        // Set initial variances for core states from config
        const auto& initial_std = config_.getInitialStd();
        double pos_std = initial_std.position > 0 ? initial_std.position : 1000.0;
        double clock_std = initial_std.clock > 0 ? initial_std.clock : 1e6;
        double tropo_std = initial_std.troposphere > 0 ? initial_std.troposphere : 0.5;
        double iono_std = initial_std.ionosphere > 0 ? initial_std.ionosphere : 5.0;
        double amb_std = initial_std.ambiguity > 0 ? initial_std.ambiguity : 1e4;
        double time_offset_std = initial_std.system_time_offset > 0 ? initial_std.system_time_offset : 100.0;

        size_t pos_idx = state_manager_.getPositionIndex();
        size_t clock_idx = state_manager_.getClockIndex();
        size_t tropo_idx = state_manager_.getTropoIndex();
        size_t iono_idx = state_manager_.getIonoIndex();

        auto is_unset = [](double v) {
            // treat zero or non-finite as unset
            return !(std::isfinite(v) && v > 0.0);
        };

        // Only override diagonal entries if they are unset/invalid (allow StateManager seeding to persist)
        if (is_unset(P(pos_idx, pos_idx)))
            P.block(pos_idx, pos_idx, 3, 3) = Eigen::Matrix3d::Identity() * pos_std * pos_std;
        if (is_unset(P(clock_idx, clock_idx)))
            P(clock_idx, clock_idx) = clock_std * clock_std;
        if (is_unset(P(tropo_idx, tropo_idx)))
            P(tropo_idx, tropo_idx) = tropo_std * tropo_std;
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK && iono_idx < state_size && is_unset(P(iono_idx, iono_idx)))
            P(iono_idx, iono_idx) = iono_std * iono_std;
        
        // Initialize inter-system time offset covariances if unset
        for (const auto& sys : state_manager_.getSystemsWithTimeOffsets()) {
            size_t idx = state_manager_.getSystemTimeOffsetIndex(sys);
            if (idx < state_size && is_unset(P(idx, idx))) {
                P(idx, idx) = time_offset_std * time_offset_std;
            }
        }

        // Set initial variances for ambiguities (if any)
        size_t num_ambiguities = state_manager_.getAmbiguities().size();
        if (num_ambiguities > 0) {
            P.bottomRightCorner(num_ambiguities, num_ambiguities).diagonal().setConstant(amb_std * amb_std);
            log_manager_.getLogger("kalman_filter")->info("Initialized {} ambiguity states with variance: {:.6f} meters^2",
                num_ambiguities, amb_std * amb_std);
        }

        log_manager_.getLogger("kalman_filter")->info("KalmanFilter initialized with state size: {} (core={}, time_offsets={}, ambiguities={}), mode: {}, P ({}x{}), x ({})",
            state_size, core_size, state_manager_.getNumSystemTimeOffsets(), num_ambiguities,
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            P.rows(), P.cols(), x.size());

        // Persist to StateManager
        state_manager_.setCovariance(P);

        // Log initial state/covariance
        if (logger_) logger_->logStateVector(x, "Initial State Vector");
        if (logger_) logger_->logCovarianceMatrix(P, "Initial Covariance Matrix");
    }

    /**
     * @brief Default destructor.
     *
     * Cleans up resources owned by the KalmanFilter object. Logging and other
     * RAII-managed resources are released automatically.
     */
    KalmanFilter::~KalmanFilter() = default;

    /**
     * @brief Predict the filter state forward by a time increment.
     *
     * This method:
     *  - Retrieves the current state vector and covariance from \c StateManager.
     *  - Obtains the process noise matrix Q and the state transition matrix F for the given time step.
     *  - Computes the predicted state (x <- F * x) and Joseph-form covariance update
     *    (P <- F * P * F^T + Q).
     *  - Persists the predicted state and covariance back into \c StateManager.
     *
     * The method logs matrix contents using \c KalmanFilterLogger for debugging.
     *
     * @param dt Time step in seconds to advance the filter.
     *
     * @throws std::runtime_error if the filter has not been properly initialized (empty state/covariance).
     * @throws std::invalid_argument if the Q or F matrices returned by \c StateManager
     *         do not match the expected state dimension.
     */
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
        log_manager_.getLogger("kalman_filter")->trace("Predicted state for dt={:.3f} s: x size={}, P size={}x{}",
            dt, x.size(), P.rows(), P.cols());

        // Store
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);

        // Log after prediction
        if (logger_) logger_->logStateVector(x, "Predicted State Vector");
        if (logger_) logger_->logCovarianceMatrix(P, "Predicted Covariance Matrix");
    }

    /**
     * @brief Apply a measurement update (Kalman gain) for an epoch of observations.
     *
     * The update performs the following steps:
     *  - Validates the provided measurement vectors/matrices.
     *  - Builds the measurement-noise matrix R from \c measurement.R_v.
     *  - Computes the innovation covariance S = H P H^T + R and inverts S
     *    (using pseudo-inverse if S is ill-conditioned).
     *  - Computes the Kalman gain K = P H^T S^{-1}.
     *  - Applies the state correction x <- x + K * innovations and updates the
     *    covariance using the Joseph form to improve numerical stability.
     *  - Computes post-fit residuals, residual covariance and standardized residuals.
     *  - Calls \c validatePostFitResiduals which may modify the previous-state
     *    copies (x_prev, P_prev) to implement ambiguity resets or full resets.
     *  - If an outlier action was taken, reverts the state/covariance to the
     *    saved pre-update copies and returns \c UpdateResult::NeedsMeasurementRebuild.
     *  - Otherwise commits the updated state/covariance and sets the state epoch time.
     *
     * The method logs matrices and diagnostic summaries using \c KalmanFilterLogger and \c LogManager.
     *
     * @param measurement EpochMeasurement describing innovations, H, R_v and metadata.
     *                    The function writes back computed residuals and covariances into this object.
     * @return UpdateResult indicating whether the update was applied, skipped, or requires measurement rebuild.
     *
     * @throws std::invalid_argument if measurement matrix dimensions mismatch with the state.
     */
    KalmanFilter::UpdateResult KalmanFilter::update(EpochMeasurement& measurement) {
        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";
        const std::string epoch_label = " @ " + epoch_tag;

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

        // Build diagonal R
        R_ = Eigen::MatrixXd::Zero(measurement.innovations.size(), measurement.innovations.size());
        for (Eigen::Index i = 0; i < measurement.innovations.size(); ++i) {
            R_(i, i) = measurement.R_v(i);
        }

        // Pull current state
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        if (logger_) logger_->logVector(measurement.innovations, "Innovations Vector", epoch_tag);
        if (logger_) logger_->logMatrix(measurement.H, "Measurement Matrix H", epoch_tag);
        if (logger_) logger_->logMatrix(R_, "Measurement Noise Matrix R", epoch_tag);

        // Kalman gain
        Eigen::MatrixXd Ht = measurement.H.transpose();
        Eigen::MatrixXd S = measurement.H * P * Ht + R_;
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(S);
        double cond = svd.singularValues()(0) / svd.singularValues()(svd.singularValues().size() - 1);
        klogger->trace("{}S matrix condition number: {:.3e}", epoch_prefix, cond);

        Eigen::MatrixXd S_inv;
        /*
        if (S.determinant() < 1e-6 || cond > 1e10) {
            klogger->warn("{}Ill-conditioned S matrix, using pseudo-inverse, determinant={:.3e}, condition={:.3e}",
                epoch_prefix, S.determinant(), cond);
            S_inv = Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>(S).pseudoInverse(); // assumes: nothing (works for singular)
        } else {
            S_inv = S.inverse();
        }
        */
        //S_inv = S.inverse(); // assumes: square + nonsingular
        S_inv = S.llt().solve(Eigen::MatrixXd::Identity(S.rows(), S.cols())); // assumes: symmetric + positive definite

        // TODO: evaluate S.ldlt().solve(...)

        // Kalman gain
        Eigen::MatrixXd K = P * Ht * S_inv;
        if (logger_) logger_->logKalmanGain(K, "Kalman Gain Matrix K", epoch_tag);

        // Save pre-update copy for outlier handling (and *allow* validatePostFitResiduals to modify these)
        Eigen::VectorXd x_prev = x;
        Eigen::MatrixXd P_prev = P;

        // Update
        Eigen::VectorXd dx = K * measurement.innovations;
        x = x + dx;

        klogger->debug("{}Kalman update dx.norm={:.3e}", epoch_prefix, dx.norm());

        // Joseph covariance update
        Eigen::MatrixXd I = Eigen::MatrixXd::Identity(state_size, state_size);
        Eigen::MatrixXd KH = K * measurement.H;
        Eigen::MatrixXd I_minus_KH = I - KH;
        P = I_minus_KH * P * I_minus_KH.transpose() + K * R_ * K.transpose();

        // Post-fit residuals: r = v - H*dx = (I - H K) v
        measurement.residuals = measurement.innovations - measurement.H * dx;

        // Post-fit residual covariance: Sr = (I - H K) * S * (I - H K)^T
        // Use measurement-space identity (m x m) because S is m x m.
        const Eigen::Index m = measurement.H.rows();
        Eigen::MatrixXd Im = Eigen::MatrixXd::Identity(m, m); // m x m
        Eigen::MatrixXd HK = measurement.H * K;               // m x m
        Eigen::MatrixXd W = Im - HK;                          // m x m
        Eigen::MatrixXd Sr = W * S * W.transpose();           // m x m
        measurement.residuals_covariance = Sr;

        // Standardized post-fit residuals (sigma units) for QC/outlier checks
        Eigen::VectorXd sres(measurement.residuals.size());
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double sii = (i < Sr.rows() && i < Sr.cols()) ? Sr(i, i) : std::numeric_limits<double>::quiet_NaN();
            const double denom = std::sqrt(std::max(sii, 1e-12));
            const double val = measurement.residuals(i);
            sres(i) = std::isfinite(val) && std::isfinite(denom) && denom > 0.0 ? (val / denom) : 0.0;
        }
        measurement.residuals_standardized = sres;

        // Outlier handling: may modify x_prev/P_prev (ambiguity reset/R inflation/full reset)
        bool is_residual_outlier = false;
        bool is_complete_reset = false;
        std::tie(is_residual_outlier, is_complete_reset) = validatePostFitResiduals(measurement, x_prev, P_prev);

        if (is_residual_outlier) {
            state_manager_.setStateVector(x_prev);
            state_manager_.setCovariance(P_prev);

            if (is_complete_reset) {
                klogger->info("{}Complete filter reset requested by residual check", epoch_prefix);
            } else {
                klogger->info("{}Outlier handled (ambiguity reset/R change). Measurement rebuild required.", epoch_prefix);
            }

            if (logger_) logger_->logStateVector(x_prev, "State After Outlier Handling", epoch_tag);
            if (logger_) logger_->logCovarianceMatrix(P_prev, "Covariance After Outlier Handling", epoch_tag);

			return UpdateResult::NeedsMeasurementRebuild; // cancel the update and reconstruct measurements for fresh innovations
        }

        // Accept update
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);
        state_manager_.setStateEpochTime(measurement.time);

        // Constraint: ZWD should not go negative (physically impossible)
        // If it does, it indicates the ZHD model is over-predicting.
        // We clamp to a small positive value and log a warning.
		// Not needed because it is easy to evaluate the ZWD state from log files.
		// This might also mask a deeper issue, so use with caution.
        /*
        const size_t tropo_idx = state_manager_.getTropoIndex();
        if (tropo_idx < static_cast<size_t>(x.size()) && x(tropo_idx) < 0.0) {
            klogger->warn(
                "{}ZWD went negative ({:.4f} m) - clamping to 0.01 m.",
                epoch_prefix, x(tropo_idx));
            x(tropo_idx) = 0.01;  // Small positive value
            state_manager_.setStateVector(x);
        }
        */
        

        // Log per-satellite residual summary for diagnostics
        {
            const size_t n_res = std::min({ static_cast<size_t>(measurement.residuals.size()), 
                                            measurement.satellite_ids.size(), 
                                            measurement.signal_types.size() });
            std::string res_summary;
            for (size_t i = 0; i < n_res; ++i) {
                double res_i = measurement.residuals(static_cast<Eigen::Index>(i));
                char buf[32];
                snprintf(buf, sizeof(buf), "%s:%+.2fm ", 
                         measurement.satellite_ids[i].c_str(), res_i);
                res_summary += buf;
            }
            klogger->debug("{}Post-fit residuals: {}", epoch_prefix, res_summary);
        }

        if (logger_) logger_->logStateVector(x, "Updated State Vector", epoch_tag);
        if (logger_) logger_->logCovarianceMatrix(P, "Updated Covariance Matrix", epoch_tag);

        return UpdateResult::Applied;
    }

    /**
     * @brief Validate post-fit residuals and apply corrective actions when necessary.
     *
     * This routine inspects standardized post-fit residuals stored in \c measurement
     * and decides whether an outlier action is required:
     *  - For a large phase residual (signal types starting with 'L') it attempts an ambiguity reset:
     *    the ambiguity state is zeroed and its covariance reinitialized in the provided \p P/x.
     *  - For a large code residual it inflates a persistent measurement variance scale using
     *    \c StateManager::inflateMeasurementVarianceScale.
     *  - If too many bad observations are accumulated (per configuration \c bad_meas_percent),
     *    a complete filter reset is performed by zeroing \p P and reinitializing the core-state diagonal.
     *
     * The method logs diagnostic messages and returns a pair indicating whether an outlier
     * was found and whether a complete reset was applied.
     *
     * IMPORTANT: The \p x and \p P parameters are pre-update copies of the state and covariance.
     * They may be modified by this method to reflect ambiguity resets or the complete-reset
     * covariance. The caller is expected to use these modified copies if an outlier action
     * is requested (the caller will revert the StateManager to these values).
     *
     * @param measurement EpochMeasurement containing residuals, residual covariance and metadata.
     *                    The method may update \c measurement.bad_residual_count.
     * @param x Pre-update state vector copy; may be modified to reflect ambiguity reset.
     * @param P Pre-update covariance matrix copy; may be modified to reflect ambiguity reset or full reset.
     * @return std::pair<bool,bool> where
     *         - first == true if any outlier action was taken,
     *         - second == true if a complete filter reset was applied.
     */
    std::pair<bool, bool> KalmanFilter::validatePostFitResiduals(EpochMeasurement& measurement, Eigen::VectorXd& x, Eigen::MatrixXd& P) {
        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        const double threshold = config_.getResidualThreshold();
        const double bad_meas_percent = config_.getBadMeasPercent();
        const double amb_std = config_.getAmbiguityStd();        // meters
        const double code_noise_amplify = config_.getCodeNoiseAmplify(); // m^2

        bool is_outlier_found = false;
        bool is_complete_reset = false;

        Eigen::VectorXd res = measurement.residuals;
        Eigen::MatrixXd vres = measurement.residuals_covariance;
        Eigen::VectorXd sres = measurement.residuals_standardized;

        const size_t num_obs = static_cast<size_t>(res.size());
        if (num_obs == 0) {
            klogger->warn("{}No observations for post-fit validation", epoch_prefix);
            return { false, false };
        }

        const size_t meta_n = std::min({ num_obs, measurement.satellite_ids.size(), measurement.signal_types.size() });

        klogger->debug(
            "{}Post-fit residual check: n_obs={}, threshold={:.3f}, bad_count={}, bad_percent={:.3f}",
            epoch_prefix, num_obs, threshold, measurement.bad_residual_count, bad_meas_percent);

        for (size_t i = 0; i < meta_n; ++i) {
            const double var_i = (i < static_cast<size_t>(vres.rows()) && i < static_cast<size_t>(vres.cols()))
                ? vres(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i))
                : std::numeric_limits<double>::quiet_NaN();
            const double sigma_i = std::sqrt(std::max(var_i, 0.0));
            const double s_i = sres(static_cast<Eigen::Index>(i));
            const double s_abs = std::abs(s_i);
            const bool is_out = std::isfinite(s_abs) && (s_abs > threshold);

            klogger->debug(
                "{}  res[{:02d}] {} {}: v={:+.4f} m, sigma={:.4f} m, s={:+.3f}, |s|={:.3f}, outlier={}",
                epoch_prefix,
                static_cast<int>(i),
                measurement.satellite_ids[i],
                measurement.signal_types[i],
                res(static_cast<Eigen::Index>(i)),
                sigma_i,
                s_i,
                s_abs,
                is_out ? "YES" : "no");
        }

        if (meta_n != num_obs) {
            klogger->warn(
                "{}Residual metadata size mismatch: res={}, sat_ids={}, sig_types={}. Logged {} entries.",
                epoch_prefix,
                num_obs, measurement.satellite_ids.size(), measurement.signal_types.size(), meta_n);
        }

        Eigen::VectorXd sres_abs = sres.cwiseAbs();
        size_t max_idx = 0;
        const double max_sres = sres_abs.maxCoeff(&max_idx);

        size_t num_phase = 0;
        for (size_t i = 0; i < num_obs; ++i) {
            if (!measurement.signal_types[i].empty() && measurement.signal_types[i][0] == 'L') ++num_phase;
        }

        size_t num_over = 0;
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double s_abs = std::abs(sres(i));
            if (std::isfinite(s_abs) && s_abs > threshold) ++num_over;
        }

        klogger->debug(
            "{}Residual summary: max|sres|={:.3f} at idx={}, over_threshold={}/{}, num_phase={}",
            epoch_prefix,
            max_sres, max_idx, num_over, num_obs, num_phase);

        if (max_sres > threshold) {
            const double sigma_max = std::sqrt(std::max(vres(static_cast<Eigen::Index>(max_idx), static_cast<Eigen::Index>(max_idx)), 0.0));

            klogger->warn(
                "{}Maximum standardized residual (magnitude) for {} {}: |s|={:.3f} (s={:+.3f}, residual {:.3f} m, sigma {:.3f} m, idx {})",
                epoch_prefix,
                measurement.satellite_ids[max_idx], measurement.signal_types[max_idx],
                max_sres, sres(static_cast<Eigen::Index>(max_idx)), res(static_cast<Eigen::Index>(max_idx)), sigma_max, max_idx);

            if (!measurement.signal_types[max_idx].empty() && measurement.signal_types[max_idx][0] == 'L') {
                const std::string amb_key = measurement.satellite_ids[max_idx] + "_" + measurement.signal_types[max_idx];
                const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);

                klogger->info(
                    "{}Phase outlier action: ambiguity reset requested for key={}, amb_idx={}", epoch_prefix, amb_key, amb_idx);

                if (amb_idx >= 0) {
                    const double prev_amb = x(amb_idx);
                    x(amb_idx) = 0.0;

                    P.row(amb_idx).setZero();
                    P.col(amb_idx).setZero();
                    P(amb_idx, amb_idx) = amb_std * amb_std;

                    klogger->info(
                        "{}Reinitialized ambiguity for {} {} (index {}): prev={:.6f} m -> 0.0 m, std={:.3f} m",
                        epoch_prefix,
                        measurement.satellite_ids[max_idx], measurement.signal_types[max_idx], amb_idx, prev_amb, amb_std);
                } else {
                    klogger->warn("{}Phase outlier but ambiguity key not found: {}", epoch_prefix, amb_key);
                }
            } else {
                const double base_var = measurement.R_v(static_cast<Eigen::Index>(max_idx));
                double factor = 1.0;

                if (std::isfinite(base_var) && base_var > 0.0) {
                    factor = code_noise_amplify / base_var;
                }
                if (!std::isfinite(factor) || factor < 1.0) factor = 1.0;

                state_manager_.inflateMeasurementVarianceScale(
                    measurement.satellite_ids[max_idx],
                    measurement.signal_types[max_idx],
                    factor);

                klogger->info(
                    "{}Code outlier action: inflated persistent variance scale for {} {}: base_var={:.3f} m^2, target_var={:.3f} m^2, factor={:.3f}",
                    epoch_prefix,
                    measurement.satellite_ids[max_idx],
                    measurement.signal_types[max_idx],
                    base_var,
                    code_noise_amplify,
                    factor);
            }

            is_outlier_found = true;
            measurement.bad_residual_count++;

            klogger->warn(
                "{}Outlier flagged: idx={}, sat={}, sig={}, max_sres={:.3f} > threshold={:.3f}; bad_residual_count={}",
                epoch_prefix,
                max_idx,
                measurement.satellite_ids[max_idx],
                measurement.signal_types[max_idx],
                max_sres,
                threshold,
                measurement.bad_residual_count);
        } else {
            klogger->debug(
                "{}No outliers: max_sres={:.3f} <= threshold={:.3f}", epoch_prefix, max_sres, threshold);
        }

        if (measurement.bad_residual_count > static_cast<size_t>(bad_meas_percent * num_obs)) {
            klogger->warn(
                "{}Resetting filter: too many outliers ({}/{}), limit={:.3f}*n={}",
                epoch_prefix,
                measurement.bad_residual_count,
                num_obs,
                bad_meas_percent,
                static_cast<size_t>(bad_meas_percent * num_obs));

            P.setZero();
            const size_t base_core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
            const size_t num_time_offsets = state_manager_.getNumSystemTimeOffsets();
            const size_t core_size = base_core_size + num_time_offsets;

            P.diagonal().head(base_core_size) << 10000.0, 10000.0, 10000.0,
                90000.0,
                0.25;

            if (base_core_size == 6) {
                P(5, 5) = 25.0;
            }
            
            // Reset time offset covariances
            for (const auto& sys : state_manager_.getSystemsWithTimeOffsets()) {
                size_t idx = state_manager_.getSystemTimeOffsetIndex(sys);
                if (idx < static_cast<size_t>(P.rows())) {
                    P(idx, idx) = 10000.0; // reset to large variance
                }
            }

            const size_t state_size = static_cast<size_t>(P.rows());
            if (state_size > core_size) {
                P.diagonal().tail(state_size - core_size).setConstant(amb_std * amb_std);
            }

            is_outlier_found = true;
            is_complete_reset = true;
            measurement.bad_residual_count = 0;

            klogger->warn(
                "{}Complete reset applied: P core_size={}, state_size={}, ambiguity_std={:.3f} m",
                epoch_prefix, core_size, state_size, amb_std);
        }

        klogger->debug(
            "{}Post-fit residual check result: outlier_found={}, complete_reset={}",
            epoch_prefix,
            is_outlier_found ? "true" : "false",
            is_complete_reset ? "true" : "false");

        return { is_outlier_found, is_complete_reset };
    }

} // namespace GnssProcessor