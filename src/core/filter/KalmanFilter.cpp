/*!
 * \file KalmanFilter.cpp
 * \brief Kalman filter implementation for PPP/PPP-RTK state estimation.
 *
 * Prediction and measurement update, decomposed into focused helpers:
 * seedCovarianceDiagonals() (initial P seeding), handlePhaseOutlier()
 * (ambiguity reset), handleCodeOutlier() (measurement exclusion), and
 * resetFilterCovariance() (full reset when outliers accumulate).
 */

#include "filter/KalmanFilter.hpp"
#include "filter/BaardaReliability.hpp"   // δ₀ / MDB, shared with the network path
#include "logging/GatedSink.hpp"
#include "logging/KalmanFilterLogger.hpp"
#include "measurement/MeasurementModel.hpp"
#include "slip/CycleSlipManager.hpp"
#include "model/PppModel.hpp"
#include "math/TimeUtils.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <chrono>
#include <stdexcept>
#include <limits>
#include "common/Constants.hpp"

namespace GnssProcessor {

    // ===================================================================
    // File-scope helpers
    // ===================================================================

    namespace {

    // Carrier wavelength for a (satellite, band prefix) pair.  Pivot-slip
    // detection and per-row phase outlier handling use it for the λ-fraction
    // floor below which a residual cannot be an integer cycle slip.
    //
    // @param sat       Satellite ID (e.g. "G11"); constellation from char 0.
    // @param band_str  Band prefix (e.g. "L1"); only the frequency digit is used.
    // @return          Wavelength in meters, or 0.0 if the pair is unknown.
    inline double bandWavelengthForSat(const std::string& sat,
                                        const std::string& band_str) {
        if (sat.empty() || band_str.size() < 2) return 0.0;
        const char prefix = sat[0];
        const char d      = band_str[1];
        double freq = 0.0;
        switch (prefix) {
        case 'G': case 'J': case 'S':  // GPS / QZSS / SBAS share band-digit mapping
            if      (d == '1') freq = Constants::GPS_L1_FREQ;
            else if (d == '2') freq = Constants::GPS_L2_FREQ;
            else if (d == '5') freq = Constants::GPS_L5_FREQ;
            break;
        case 'E':  // Galileo
            if      (d == '1') freq = Constants::GAL_E1_FREQ;
            else if (d == '5') freq = Constants::GAL_E5A_FREQ;
            else if (d == '7') freq = Constants::GAL_E5B_FREQ;
            else if (d == '6') freq = Constants::GAL_E6_FREQ;
            else if (d == '8') freq = Constants::GAL_E5_FREQ;
            break;
        case 'C':  // BeiDou
            if      (d == '1') freq = Constants::BDS_B1C_FREQ;
            else if (d == '2') freq = Constants::BDS_B1_FREQ;   // B1I (legacy band-2 RINEX code)
            else if (d == '5') freq = Constants::BDS_B2A_FREQ;
            else if (d == '7') freq = Constants::BDS_B2_FREQ;   // B2b
            else if (d == '6') freq = Constants::BDS_B3_FREQ;
            else if (d == '8') freq = Constants::BDS_B2AB_FREQ;
            break;
        case 'R':  // GLONASS: the FDMA center frequency suffices for the λ/4 floor
            if      (d == '1') freq = Constants::GLONASS_G1_BASE_FREQ;
            else if (d == '2') freq = Constants::GLONASS_G2_BASE_FREQ;
            else if (d == '3') freq = Constants::GLONASS_G3_BASE_FREQ;
            else if (d == '4') freq = Constants::GLONASS_G1A_FREQ;
            else if (d == '6') freq = Constants::GLONASS_G2A_FREQ;
            break;
        default: break;
        }
        if (!(freq > 0.0)) return 0.0;
        return Constants::SPEED_OF_LIGHT / freq;
    }

    // Sparse-H helpers.
    //
    // H is stored dense (~m × n) but each row holds only ~5 non-zeros out of
    // ~350 columns (geometry + ambiguity + maybe tropo / iono), so dense
    // `H·P`, `HP·Hᵀ` and `K·H` waste O(n²·m) ops on zeros.  These build a
    // per-row (column, value) list once per epoch and walk it instead,
    // producing the same dense result the dense matmul would.
    //
    // The 1e-12 threshold separates real geometry partials from the FP noise
    // of zero-initialized blocks.

    constexpr double kHSparseThreshold = 1e-12;

    struct SparseHRow {
        std::vector<int>    cols;
        std::vector<double> vals;
    };

    /// Build per-row (col, val) lists of all non-zeros in H.
    std::vector<SparseHRow> buildSparseH(const Eigen::MatrixXd& H) {
        std::vector<SparseHRow> out(static_cast<size_t>(H.rows()));
        for (Eigen::Index i = 0; i < H.rows(); ++i) {
            auto& r = out[static_cast<size_t>(i)];
            r.cols.reserve(8);
            r.vals.reserve(8);
            for (Eigen::Index j = 0; j < H.cols(); ++j) {
                const double v = H(i, j);
                if (std::abs(v) > kHSparseThreshold) {
                    r.cols.push_back(static_cast<int>(j));
                    r.vals.push_back(v);
                }
            }
        }
        return out;
    }

    /// HP = H · P  (m×n).  Row-by-row sparse accumulation.
    Eigen::MatrixXd sparseHP(const std::vector<SparseHRow>& Hs,
                              const Eigen::MatrixXd& P) {
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        const Eigen::Index n = P.cols();
        Eigen::MatrixXd out = Eigen::MatrixXd::Zero(m, n);
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto& r = Hs[static_cast<size_t>(i)];
            const size_t nnz = r.cols.size();
            for (size_t k = 0; k < nnz; ++k) {
                out.row(i).noalias() += r.vals[k] * P.row(r.cols[k]);
            }
        }
        return out;
    }

    /// S_HHt = HP · Hᵀ  (m×m, symmetric).  Column-walk per pair.
    Eigen::MatrixXd sparseHPHt(const Eigen::MatrixXd& HP,
                                const std::vector<SparseHRow>& Hs) {
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        Eigen::MatrixXd out(m, m);
        for (Eigen::Index i = 0; i < m; ++i) {
            for (Eigen::Index j = i; j < m; ++j) {
                const auto& rj = Hs[static_cast<size_t>(j)];
                double s = 0.0;
                const size_t nnz = rj.cols.size();
                for (size_t k = 0; k < nnz; ++k) {
                    s += HP(i, rj.cols[k]) * rj.vals[k];
                }
                out(i, j) = s;
                if (i != j) out(j, i) = s;
            }
        }
        return out;
    }

    /// K·H (n×n).  K is n×m, H is m×n.  For each non-zero in row i of H,
    /// add val * K.col(i) into the corresponding column of the output.
    Eigen::MatrixXd sparseKH(const Eigen::MatrixXd& K,
                              const std::vector<SparseHRow>& Hs,
                              Eigen::Index n) {
        Eigen::MatrixXd out = Eigen::MatrixXd::Zero(n, n);
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto& r = Hs[static_cast<size_t>(i)];
            const size_t nnz = r.cols.size();
            for (size_t k = 0; k < nnz; ++k) {
                out.col(r.cols[k]).noalias() += r.vals[k] * K.col(i);
            }
        }
        return out;
    }

    /// H·v  (m,).  Sparse row-wise dot.
    Eigen::VectorXd sparseHv(const std::vector<SparseHRow>& Hs,
                              const Eigen::VectorXd& v) {
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        Eigen::VectorXd out(m);
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto& r = Hs[static_cast<size_t>(i)];
            double s = 0.0;
            const size_t nnz = r.cols.size();
            for (size_t k = 0; k < nnz; ++k) {
                s += r.vals[k] * v(r.cols[k]);
            }
            out(i) = s;
        }
        return out;
    }

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
        const double amb_std   = initial_std.ambiguity >= 0           ? initial_std.ambiguity           : 1e4;

        const size_t pos_idx   = state_manager_.getPositionIndex();

        if (isUnsetVariance(P(pos_idx, pos_idx)))
            P.block(pos_idx, pos_idx, 3, 3) = Eigen::Matrix3d::Identity() * pos_std * pos_std;

        // Baseline mode: the core layout has no clock, tropo, phase-clock,
        // gradient, ISB or IMU states, so only the ambiguity diagonals follow.
        if (config_.isBaselineMode()) {
            const size_t num_amb = state_manager_.getAmbiguities().size();
            if (num_amb > 0) {
                P.bottomRightCorner(num_amb, num_amb).diagonal().setConstant(amb_std * amb_std);
            }
            return;
        }

        const double clock_std = initial_std.clock >= 0               ? initial_std.clock               : 1e6;
        const double tropo_std = initial_std.troposphere >= 0         ? initial_std.troposphere         : 0.5;
        const double isb_std   = initial_std.system_time_offset >= 0  ? initial_std.system_time_offset  : 100.0;

        const size_t tropo_idx = state_manager_.getTropoIndex();

        if (state_manager_.hasClock()) {
            const size_t clock_idx = state_manager_.getClockIndex();
            if (isUnsetVariance(P(clock_idx, clock_idx)))
                P(clock_idx, clock_idx) = clock_std * clock_std;
        }
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

        if (state_manager_.hasImuBiases()) {
            const auto& ic = config_.getImuConfig();
            const size_t att_idx = state_manager_.getAttitudeIndex();
            const size_t ab_idx  = state_manager_.getAccelBiasIndex();
            const size_t gb_idx  = state_manager_.getGyroBiasIndex();
            const double att_std = ic.initial_attitude_std;
            const double ab_std  = ic.initial_accel_bias_std;
            const double gb_std  = ic.initial_gyro_bias_std;
            for (size_t k = 0; k < 3; ++k) {
                if (att_idx + k < state_size && isUnsetVariance(P(att_idx + k, att_idx + k)))
                    P(att_idx + k, att_idx + k) = att_std * att_std;
                if (ab_idx + k < state_size && isUnsetVariance(P(ab_idx + k, ab_idx + k)))
                    P(ab_idx + k, ab_idx + k) = ab_std * ab_std;
                if (gb_idx + k < state_size && isUnsetVariance(P(gb_idx + k, gb_idx + k)))
                    P(gb_idx + k, gb_idx + k) = gb_std * gb_std;
            }
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
        ppp_model_(ppp_model), measurement_model_(state_manager, ppp_model, config, log_manager),
        residual_history_(
            // Translate GnssConfig::ResidualHistoryConfig → tracker's
            // own Config struct.  Keeping the tracker independent of
            // GnssConfig avoids a header-dependency cycle and lets the
            // tracker be reused outside this codebase if useful.
            ResidualHistoryTracker::Config{
                config.getResidualHistoryConfig().enabled,
                config.getResidualHistoryConfig().alpha,
                config.getResidualHistoryConfig().warmup_n,
                config.getResidualHistoryConfig().z_threshold,
                config.getResidualHistoryConfig().fraction_threshold
            },
            log_manager) {

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

        // Wrap the kalman_filter sinks in a GatedSink so beginEpoch/endEpoch can
        // route intermediate-pass output away from disk without touching call
        // sites.  Null when the flag is off, making those hooks free no-ops.
        if (config_.getLoggingConfig().kalman_filter_concise_log) {
            gate_ = log_manager_.installGatedSink("kalman_filter");
            if (gate_) {
                log_manager_.getLogger("kalman_filter")->info(
                    "Concise log mode enabled: only first + final pass per epoch will be retained");
            }
        }

        const size_t state_size = state_manager_.getStateSize();
        // Minimum core size.  Baseline mode carries position (3) and DD
        // ambiguities only (no clock, tropo, velocity, IMU or ISB states), so its
        // minimum is 3.  BSSD drops the receiver clock (it cancels in SD), taking
        // the base from 5 (pos+clk+tropo) to 4 (pos+tropo), with no per-system
        // ISB states.
        const size_t min_core = config.isBaselineMode()
            ? 3  // position only
            : [&] {
                size_t b = config.enableBetweenSatelliteSd() ? 4 : 5;
                if (config.estimateTropoGradients()) b += 2;
                if (config.estimateVelocity())       b += 3;
                if (config.estimatePhaseClock())     b += 1;
                if (config.enableImu())              b += 9;
                return b;
              }();
        if (state_size < min_core) {
            log_manager_.getLogger("kalman_filter")->error(
                "Invalid state size from StateManager: {}, minimum {} required for mode {}",
                state_size, min_core, modeLabel(config));
            throw std::invalid_argument("Invalid state size");
        }

        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        seedCovarianceDiagonals(P, state_size);

        const size_t num_amb = state_manager_.getAmbiguities().size();
        const size_t core_size = state_size - num_amb
            - state_manager_.getIonosphereStates().size()
            - state_manager_.getIonosphereRateStates().size();
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
        if (dt <= 0.0) {
            // dt <= 0 means identity transition and zero process noise, so this is
            // a no-op.  Returning early avoids getTransitionMatrix /
            // getProcessNoiseMatrix, which hard-throw on dt <= 0, keeping the API
            // robust to duplicate epochs (e.g. a stream repeating a TOW).  Upstream
            // already skips those; this is the backstop.
            log_manager_.getLogger("kalman_filter")->debug(
                "predict: non-positive time step dt={:.6f} — skipping propagation (no-op)", dt);
            return;
        }
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

        // Under BSSD the phase-clock state (when estimated) is unobserved: its H
        // column is zero and its process noise is suppressed.  Zero its
        // cross-covariances so stale correlations do not leak into the SD
        // solution; the diagonal is kept.  The receiver clock and ISB states are
        // not allocated under BSSD, so those freeze calls are guarded no-ops.
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

            // Freeze BSSD pivot iono states (their column is zeroed in the
            // S-transform).  The constant-velocity companion rate is frozen too:
            // its F-coupling and Q are already suppressed and PivotManager holds
            // it at (0,0), so zeroing cross-covariances completes the isolation.
            for (const auto& sat : state_manager_.getFrozenPivotSatellites()) {
                const int iono_idx = state_manager_.getIonosphereIndex(sat);
                if (iono_idx >= 0)
                    freeze_state(static_cast<size_t>(iono_idx));
                if (config_.enableIonoRate()) {
                    const int rate_idx = state_manager_.getIonosphereRateIndex(sat);
                    if (rate_idx >= 0)
                        freeze_state(static_cast<size_t>(rate_idx));
                }
            }
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
    // IMU-aided prediction
    // ===================================================================

    void KalmanFilter::predictImu(const GnssTime& t_start, const GnssTime& t_end,
                                   const ImuIntegrator& imu) {
        auto klogger = log_manager_.getLogger("kalman_filter");

        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();
        const size_t state_size = state_manager_.getStateSize();

        // --- Extract current IMU-related states ---
        const Eigen::Vector3d pos = x.segment<3>(state_manager_.getPositionIndex());
        Eigen::Vector3d vel = Eigen::Vector3d::Zero();
        if (state_manager_.hasVelocity())
            vel = x.segment<3>(state_manager_.getVelocityIndex());

        Eigen::Vector3d accel_bias = Eigen::Vector3d::Zero();
        Eigen::Vector3d gyro_bias  = Eigen::Vector3d::Zero();
        if (state_manager_.hasImuBiases()) {
            accel_bias = x.segment<3>(state_manager_.getAccelBiasIndex());
            gyro_bias  = x.segment<3>(state_manager_.getGyroBiasIndex());
        }

        // --- Run strapdown mechanization ---
        const ImuPrediction pred = imu.integrate(t_start, t_end, pos, vel, C_b_e_,
                                                  accel_bias, gyro_bias);
        if (!pred.valid) {
            klogger->warn("IMU integration returned no valid samples, falling back to standard predict");
            const double dt = TimeUtils::continuousGpsSeconds(t_end)
                            - TimeUtils::continuousGpsSeconds(t_start);
            predict(std::max(dt, 0.01));
            return;
        }

        // --- Apply mechanized state to the state vector ---
        x.segment<3>(state_manager_.getPositionIndex()) = pred.position;
        if (state_manager_.hasVelocity())
            x.segment<3>(state_manager_.getVelocityIndex()) = pred.velocity;
        C_b_e_ = pred.attitude;

        // Attitude error state is reset to zero after each mechanization
        // (the attitude correction is directly applied to C_b_e_).
        if (state_manager_.hasImuBiases()) {
            x.segment<3>(state_manager_.getAttitudeIndex()).setZero();
        }

        // --- Build the full-state transition and process noise ---
        // The IMU supplies a 15x15 Phi/Q over the INS error states
        // [dp(3), dv(3), dpsi(3), dba(3), dbg(3)], mapped here to full-state
        // indices.  Non-INS states (clock, tropo, ambiguities, ISB) keep the
        // standard random-walk noise from predict().

        Eigen::MatrixXd F_full = Eigen::MatrixXd::Identity(state_size, state_size);
        Eigen::MatrixXd Q_full = state_manager_.getProcessNoiseMatrix(pred.dt_total);

        // Map INS error-state indices to full state indices.
        // INS order: pos(0-2), vel(3-5), att(6-8), ba(9-11), bg(12-14)
        std::vector<size_t> ins_map(15, std::numeric_limits<size_t>::max());
        ins_map[0] = state_manager_.getPositionIndex();
        ins_map[1] = state_manager_.getPositionIndex() + 1;
        ins_map[2] = state_manager_.getPositionIndex() + 2;
        if (state_manager_.hasVelocity()) {
            ins_map[3] = state_manager_.getVelocityIndex();
            ins_map[4] = state_manager_.getVelocityIndex() + 1;
            ins_map[5] = state_manager_.getVelocityIndex() + 2;
        }
        if (state_manager_.hasImuBiases()) {
            ins_map[6]  = state_manager_.getAttitudeIndex();
            ins_map[7]  = state_manager_.getAttitudeIndex() + 1;
            ins_map[8]  = state_manager_.getAttitudeIndex() + 2;
            ins_map[9]  = state_manager_.getAccelBiasIndex();
            ins_map[10] = state_manager_.getAccelBiasIndex() + 1;
            ins_map[11] = state_manager_.getAccelBiasIndex() + 2;
            ins_map[12] = state_manager_.getGyroBiasIndex();
            ins_map[13] = state_manager_.getGyroBiasIndex() + 1;
            ins_map[14] = state_manager_.getGyroBiasIndex() + 2;
        }

        // Inject the INS Phi and Q into the full matrices.
        for (size_t i = 0; i < 15; ++i) {
            if (ins_map[i] >= state_size) continue;
            for (size_t j = 0; j < 15; ++j) {
                if (ins_map[j] >= state_size) continue;
                F_full(ins_map[i], ins_map[j]) = pred.Phi(i, j);
                Q_full(ins_map[i], ins_map[j]) = pred.Q_ins(i, j);
            }
        }

        // --- Covariance propagation ---
        P = F_full * P * F_full.transpose() + Q_full;
        P = 0.5 * (P + P.transpose());

        // --- Store ---
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);

        klogger->info("IMU prediction: {} samples over {:.3f}s, pos_delta={:.3f}m",
            pred.num_samples, pred.dt_total, (pred.position - pos).norm());

        if (logger_) logger_->logStateVector(x, "IMU Predicted State Vector");
        if (logger_) logger_->logCovarianceMatrix(P, "IMU Predicted Covariance Matrix");
    }

    // ===================================================================
    // Aiding-source-aided prediction (composable generalization of predictImu)
    // ===================================================================

    void KalmanFilter::spliceAndPropagate(Eigen::VectorXd& x, Eigen::MatrixXd& P,
                                          Eigen::MatrixXd& Q_full, std::size_t state_size,
                                          const std::vector<PredictContribution>& contributions) {
        // Identity-seeded transition, as in predictImu (F_full = I): the means of
        // states no contribution owns (non-INS states) are not propagated.
        Eigen::MatrixXd F_full = Eigen::MatrixXd::Identity(state_size, state_size);

        for (const auto& c : contributions) {
            if (!c.valid) continue;

            // Nominal write-backs (pos/vel/att-zero for the IMU), out-of-range
            // / sentinel indices skipped exactly as predictImu skips them.
            for (size_t k = 0; k < c.nominal_indices.size(); ++k) {
                const size_t idx = c.nominal_indices[k];
                if (idx < state_size)
                    x(static_cast<Eigen::Index>(idx)) =
                        c.nominal_values(static_cast<Eigen::Index>(k));
            }

            // Write the contribution's Phi/Q over the full-state indices it
            // owns.  This is assignment, not accumulation (as in predictImu), so
            // the baseline process noise on owned rows is not counted twice.
            const size_t n = c.indices.size();
            for (size_t i = 0; i < n; ++i) {
                if (c.indices[i] >= state_size) continue;
                const auto ri = static_cast<Eigen::Index>(c.indices[i]);
                for (size_t j = 0; j < n; ++j) {
                    if (c.indices[j] >= state_size) continue;
                    const auto cj = static_cast<Eigen::Index>(c.indices[j]);
                    F_full(ri, cj) = c.Phi(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j));
                    Q_full(ri, cj) = c.Q(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j));
                }
            }
        }

        P = F_full * P * F_full.transpose() + Q_full;
        P = 0.5 * (P + P.transpose());
    }

    void KalmanFilter::predictAided(double dt,
                                    const std::vector<PredictContribution>& contributions) {
        // Empty / all-invalid => byte-identical GNSS-only predict.  Delegating
        // (rather than reimplementing) inherits x=F*x, the BSSD freeze-state
        // surgery, and the dt<=0 guard for free.
        double baseline_dt = 0.0;
        bool any_valid = false;
        for (const auto& c : contributions) {
            if (c.valid) { any_valid = true; baseline_dt = std::max(baseline_dt, c.dt_total); }
        }
        if (!any_valid) { predict(dt); return; }

        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();
        const size_t state_size = state_manager_.getStateSize();

        // Baseline Q over the contributions' integrated interval (the largest
        // valid dt_total), as in predictImu, rather than the GNSS dt: the
        // non-owned rows keep their configured random-walk process noise, scaled
        // by the interval the INS integrated.
        Eigen::MatrixXd Q_full = state_manager_.getProcessNoiseMatrix(baseline_dt);
        spliceAndPropagate(x, P, Q_full, state_size, contributions);

        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);

        if (logger_) logger_->logStateVector(x, "Aided Predicted State Vector");
        if (logger_) logger_->logCovarianceMatrix(P, "Aided Predicted Covariance Matrix");
    }

    // ===================================================================
    // Measurement update
    // ===================================================================

    KalmanFilter::UpdateResult KalmanFilter::update(EpochMeasurement& measurement) {
        auto klogger = log_manager_.getLogger("kalman_filter");
        // epoch_tag feeds the side-channel KalmanFilterLogger (a separate file,
        // not managed by spdlog), so it stays formatted.  epoch_prefix is empty:
        // the spdlog %E field already carries the epoch, and repeating it in the
        // message body adds clutter.
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix;

        last_reset_satellites_.clear();
        last_reset_signals_.clear();
        was_complete_reset_ = false;

        // Concise-log gating: pass 1 emits live; passes ≥2 append to the gate
        // buffer without clearing, so every pass's outlier lines accumulate and
        // endEpoch replays them in order.  beginEpoch clears between epochs.
        ++pass_index_;
        if (gate_) {
            if (pass_index_ == 1) {
                gate_->setPolicy(GatedSink::Policy::Passthrough);
            } else {
                gate_->setPolicy(GatedSink::Policy::Buffer);
            }
        }

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

        // Local copies of the pre-update state.  Post-update results go into
        // x_new / P_new, so the copies can be handed to the outlier validator
        // without a separate backup (a ~1 MB matrix copy at n = 350).  The
        // validator edits them in place on the outlier path (see below).
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();

        // --- Persistent-bias R-inflation ---------------------------------
        // When a phase row's EWMA residual mean has warmed up past
        // persistent_bias_min_cycles · λ, add mean² to its R diagonal.  The bias
        // is not a cycle slip but an unmodeled channel effect (multipath, PCV
        // miscal, inter-channel delay), so the row is downweighted in proportion
        // to its drift rather than dropped, and recovers as the EWMA decays.
        const double persist_floor_cyc = config_.getPersistentBiasMinCycles();
        if (persist_floor_cyc > 0.0 && residual_history_.isEnabled()) {
            const Eigen::Index n_rows = measurement.innovations.size();
            size_t inflated_count = 0;
            for (Eigen::Index i = 0; i < n_rows; ++i) {
                const auto& sat_id = measurement.satellite_ids[static_cast<size_t>(i)];
                const auto& sig    = measurement.signal_types[static_cast<size_t>(i)];
                if (sig.empty() || sig[0] != 'L') continue;
                // Iono-free combos don't have a single-λ interpretation;
                // skip them (consistent with the per-row outlier λ-floor).
                if (sig.find('_') != std::string::npos) continue;
                if (sig.size() < 2) continue;

                const auto [prim, _ref] = splitBssdSatId(sat_id);
                const auto stats_opt = residual_history_.getStats(prim, sig);
                if (!stats_opt.has_value()) continue;
                const auto& st = stats_opt.value();
                // Require a warmed-up EWMA history, by the tracker's own rule
                // (n ≥ estimation.residual_history.warmup_n).
                if (!st.warmed_up) continue;

                const std::string band_str = sig.substr(0, 2);
                const double lambda = bandWavelengthForSat(prim, band_str);
                if (!(lambda > 0.0)) continue;

                const double floor_m = persist_floor_cyc * lambda;
                if (std::abs(st.mean) <= floor_m) continue;

                const double extra_var = st.mean * st.mean;
                R_(i, i) += extra_var;
                ++inflated_count;
                klogger->info(
                    "{}Persistent-bias R-inflation on {} {}: EWMA |mean|={:.4f} m "
                    "> floor={:.4f} m (λ={:.4f} m, n={}); R diagonal +={:.4e} m²",
                    epoch_prefix, prim, sig, std::abs(st.mean),
                    floor_m, lambda, st.n, extra_var);
            }
            if (inflated_count > 0) {
                klogger->debug("{}Persistent-bias R-inflation applied to {} phase rows",
                    epoch_prefix, inflated_count);
            }
        }

        // --- Apply pending Huber R-inflations from previous pass -----------
        // The Huber post-fit dispatch queues R targets for sub-cycle phase rows
        // whose |sres_post| exceeds huber_k.  Applying them at the start of each
        // rebuild pass means the next update sees the downweighted R.  Cleared at
        // the epoch boundary by beginEpoch.
        if (!pending_r_inflations_.empty()) {
            const Eigen::Index n_rows = measurement.innovations.size();
            size_t applied = 0;
            for (Eigen::Index i = 0; i < n_rows; ++i) {
                const auto& sat_id = measurement.satellite_ids[static_cast<size_t>(i)];
                const auto& sig    = measurement.signal_types[static_cast<size_t>(i)];
                if (sig.empty()) continue;
                const auto [prim, _ref3] = splitBssdSatId(sat_id);
                const std::string key = prim + "|" + sig;
                auto it = pending_r_inflations_.find(key);
                if (it == pending_r_inflations_.end()) continue;
                if (it->second > R_(i, i)) {
                    R_(i, i) = it->second;
                    ++applied;
                }
            }
            if (applied > 0) {
                klogger->debug("{}Applied {} pending Huber R-inflations to this pass",
                    epoch_prefix, applied);
            }
        }

        if (logger_) logger_->logVector(measurement.innovations, "Innovations Vector", epoch_tag);
        if (logger_) logger_->logMatrix(measurement.H, "Measurement Matrix H", epoch_tag);
        if (logger_) logger_->logMatrix(R_, "Measurement Noise Matrix R", epoch_tag);

        // --- Per-phase timers --------------------------------------------
        // Per-call values go to the kalman_filter log at debug level; the
        // cumulative summary comes from logKalmanFilterTimings() at end of run.
        // steady_clock::now() costs a few ns, negligible against the matrix products.
        using clk = std::chrono::steady_clock;
        const auto t0 = clk::now();

        // --- Build per-row sparse view of H once per call ----------------
        // H has ~5 non-zeros / row out of ~n columns (geometry, amb, optional
        // tropo / iono).  The four products below switch from dense O(n²·m)
        // / O(n·m²) to O(nnz_per_row · n · m) / O(nnz_per_row · m²).
        const std::vector<SparseHRow> Hs = buildSparseH(measurement.H);

        // --- Innovation covariance & Kalman gain ---
        const Eigen::MatrixXd HP = sparseHP(Hs, P);          // m×n
        Eigen::MatrixXd S        = sparseHPHt(HP, Hs);       // m×m
        S += R_;                                              // diagonal-add
        const auto t_after_S = clk::now();

        // The condition number feeds a trace-level log line only; the JacobiSVD
        // (O(m³) with a heavy constant) is skipped unless trace is enabled.
        if (klogger->should_log(spdlog::level::trace)) {
            Eigen::JacobiSVD<Eigen::MatrixXd> svd(S);
            const double cond = svd.singularValues()(0)
                / svd.singularValues()(svd.singularValues().size() - 1);
            klogger->trace("{}S matrix condition number: {:.3e}", epoch_prefix, cond);
        }

        // Build K via a single solve against H·P instead of materializing
        // S⁻¹ explicitly.  K = P·Hᵀ·S⁻¹ = (S⁻¹·H·P)ᵀ since S is symmetric.
        // Saves an O(m³) solve-with-identity and an O(n·m²) extra matmul.
        Eigen::MatrixXd K;
        Eigen::LDLT<Eigen::MatrixXd> ldlt(S);
        if (ldlt.info() == Eigen::Success) {
            K = ldlt.solve(HP).transpose();
        } else {
            klogger->warn("{}LDLT failed for S matrix, falling back to pseudo-inverse",
                epoch_prefix);
            const Eigen::MatrixXd S_pinv =
                Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>(S).pseudoInverse();
            K = HP.transpose() * S_pinv;
        }
        if (logger_) logger_->logKalmanGain(K, "Kalman Gain Matrix K", epoch_tag);
        const auto t_after_K = clk::now();

        // --- State correction (into a new vector; the pre-update x stays intact) ---
        const Eigen::VectorXd dx = K * measurement.innovations;
        const Eigen::VectorXd x_new = x + dx;
        klogger->debug("{}Kalman update dx.norm={:.3e}", epoch_prefix, dx.norm());

        // --- Joseph covariance update (into a new matrix) ---
        // Build (I - K·H) via sparse-K·H plus an in-place diagonal +1.
        // Saves the n²·m K·H matmul (most of K·H lands on zero columns
        // of H anyway).
        Eigen::MatrixXd I_minus_KH = -sparseKH(K, Hs, static_cast<Eigen::Index>(state_size));
        I_minus_KH.diagonal().array() += 1.0;
        Eigen::MatrixXd P_new = I_minus_KH * P * I_minus_KH.transpose()
                              + K * R_ * K.transpose();
        P_new = 0.5 * (P_new + P_new.transpose());
        const auto t_after_P = clk::now();

        // --- Post-fit residuals ---
        measurement.residuals = measurement.innovations - sparseHv(Hs, dx);

        // Sr = R - H·P_post·Hᵀ.  Only the diagonal is consumed (outlier handler,
        // residuals trace), and per-row dot products get it in O(nnz·m) instead
        // of the O(n²·m + n·m²) full build.  Off-diagonals stay zero.
        const Eigen::MatrixXd HP_new = sparseHP(Hs, P_new);  // m×n
        Eigen::VectorXd sr_diag(measurement.H.rows());
        for (Eigen::Index i = 0; i < measurement.H.rows(); ++i) {
            const auto& r = Hs[static_cast<size_t>(i)];
            double hphi = 0.0;
            for (size_t k = 0; k < r.cols.size(); ++k) {
                hphi += r.vals[k] * HP_new(i, r.cols[k]);
            }
            sr_diag(i) = R_(i, i) - hphi;
        }

        // Post-fit residual covariance: diagonal Sr by default.  When the
        // off-diagonal-R kernel is on and a dense R was supplied (BSSD /
        // baseline-DD, where R carries SD/DD cross-covariance), build the full
        // Sr = R − H·P⁺·Hᵀ so the redundancy and w-test QC honour correlated
        // observations.  Its diagonal equals sr_diag, so the outlier path is
        // unaffected either way.
        const bool dense_r_supplied =
            (measurement.R_full.rows() == measurement.innovations.size() &&
             measurement.R_full.cols() == measurement.innovations.size());
        Eigen::MatrixXd Sr_full;   // empty unless the off-diagonal kernel is active
        if (config_.enableFullRKernel() && dense_r_supplied) {
            Sr_full = R_ - sparseHPHt(HP_new, Hs);          // m×m
            Sr_full = 0.5 * (Sr_full + Sr_full.transpose());
            measurement.residuals_covariance = Sr_full;
        } else {
            measurement.residuals_covariance = sr_diag.asDiagonal();
        }
        const auto t_after_Sr = clk::now();

        // Phase totals in microseconds (wall clock).  Static accumulators
        // survive across calls; logKalmanFilterTimings() prints the running
        // totals on demand (called from Application end-of-run).
        const auto us = [](clk::time_point a, clk::time_point b) -> long long {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };
        const long long us_S  = us(t0, t_after_S);
        const long long us_K  = us(t_after_S, t_after_K);
        const long long us_P  = us(t_after_K, t_after_P);
        const long long us_Sr = us(t_after_P, t_after_Sr);
        ++timing_n_calls_;
        timing_us_S_  += us_S;
        timing_us_K_  += us_K;
        timing_us_P_  += us_P;
        timing_us_Sr_ += us_Sr;
        klogger->debug(
            "{}KF timing (us): S={} K={} P={} Sr={} | n_state={} m={} ",
            epoch_prefix, us_S, us_K, us_P, us_Sr,
            state_size, measurement.H.rows());

        // Standardize post-fit residuals against Sr = R − H·P_post·Hᵀ, floored
        // at the row's own noise variance R(i,i).
        //
        // Sr, not the innovation covariance S = H·P·Hᵀ + R, is the covariance of
        // v = z − H·x_post; S would over-scale v and suppress detection.  Where
        // one state absorbs a row's innovation almost completely (a DD phase
        // ambiguity with a large initial variance gives K_amb ≈ 1,
        // P_post_amb ≈ 0), Sr = R·S⁻¹·R falls far below R, and the floor keeps
        // the statistic in measurement-σ units.  Because H·P_post·Hᵀ is positive
        // semidefinite, Sr(i,i) ≤ R(i,i) on every row, so the floor binds
        // whenever R(i,i) > 0 and the denominator is √R(i,i).  A 1-cycle L1 slip
        // (0.19 m on a 2 mm observation) is about 95σ and trips detection.
        Eigen::VectorXd sres(measurement.residuals.size());
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double sii = (i < sr_diag.size()) ? sr_diag(i) : 0.0;
            // The floor uses the working R(i,i) (the inflated value), not the
            // formal measurement.R_v.  With the formal value a Huber or EWMA
            // R-inflation would never register: the standardized residual would
            // stay pinned to the formal noise, |sres| could not shrink on the
            // rebuild, and the inflation loop would diverge.  The 1e-12 term
            // covers the case where both Sr and R are unset.
            const double r_floor = (i < R_.rows() && R_(i, i) > 0.0) ? R_(i, i) : 0.0;
            const double denom = std::sqrt(std::max({ sii, r_floor, 1e-12 }));
            const double val = measurement.residuals(i);
            sres(i) = (std::isfinite(val) && std::isfinite(denom) && denom > 0.0) ? (val / denom) : 0.0;
        }
        measurement.residuals_standardized = sres;

        // --- Statistical QC layer: a-posteriori variance factor
        //     σ̂₀² = Σ vᵢ²/Rᵢᵢ / dof, plus Baarda data-snooping (redundancy rᵢ,
        //     w-test wᵢ, MDBᵢ).  Pure diagnostics stashed on `measurement`;
        //     never mutates state or covariance, and skipped entirely when both
        //     toggles are off.  A full Sr, if built, is passed through so the
        //     redundancy and w-test honour correlated R.
        if (config_.enableVarianceFactor() || config_.enableDataSnooping()) {
            computeResidualQc_(measurement, R_, sr_diag,
                               Sr_full.size() != 0 ? &Sr_full : nullptr);
        }

        // --- Outlier handling ---
        // Runs for both PPP and baseline.  x and P are the pre-update values.
        // validatePostFitResiduals edits them in place when it handles an
        // outlier (ambiguity resets, a full covariance reset); the branch below
        // writes those edits back.
        auto [is_outlier, is_reset] = validatePostFitResiduals(
            measurement,
            x,
            P);
        was_complete_reset_ = is_reset;

        if (is_outlier) {
            // The outlier handlers reset the affected ambiguity in the local x
            // and P.  Without this writeback those edits are lost on return, the
            // slipped ambiguity keeps its wrong value, and the same residual
            // reappears on every rebuild pass and every epoch.  Costs an
            // n²-element copy, only on the (rare) outlier path.
            state_manager_.setStateVector(x);
            state_manager_.setCovariance(P);

            klogger->debug("{}{}. Measurement rebuild required.",
                epoch_prefix,
                is_reset ? "Complete filter reset requested by residual check"
                         : "Outlier handled (ambiguity reset/R change)");

            if (logger_) logger_->logStateVector(x, "State After Outlier Handling", epoch_tag);
            if (logger_) logger_->logCovarianceMatrix(P, "Covariance After Outlier Handling", epoch_tag);

            return UpdateResult::NeedsMeasurementRebuild;
        }

        // --- Commit ---
        // Stash the prior (x, P) this update read as the IEKF anchor for
        // Application::runGeometryRelinearization.  Being the prior the converged
        // update ran on, it already matches the post-update state dimension.
        // Gated to avoid the ~1 MB P copy when relinearization is off.
        if (config_.relinearizeGeometry() && !config_.tightlyCoupledImu()) {
            last_update_prior_x_ = x;
            last_update_prior_P_ = P;
        }
        state_manager_.setStateVector(x_new);
        state_manager_.setCovariance(P_new);
        state_manager_.setStateEpochTime(measurement.time);

        // --- Apply pending stuck-code-bias resets -------------------------
        // Queued by validatePostFitResiduals's clean-commit branch and applied
        // right after commit so the next epoch starts re-seeded.  Reads via
        // state_manager_.getStateVector() rather than x_new because
        // resetSatBiasState mutates state_vector_ in place, and later resets in
        // this batch need the updated view.
        if (!pending_sat_bias_resets_.empty()) {
            const double sb_std   = config_.getInitialStd().sat_bias;
            const double cold_var = sb_std * sb_std;
            for (const auto& [key, delta_value] : pending_sat_bias_resets_) {
                const int idx = state_manager_.getSatBiasIndex(key);
                if (idx < 0) continue;  // pruned between detect and apply
                const double cur = state_manager_.getStateVector()(idx);
                state_manager_.resetSatBiasState(key, cur + delta_value, cold_var);
                // Clear the row's EWMA + streak history; the new
                // bias state's converged value is unrelated to the
                // pre-reset history.
                const auto bar = key.find('|');
                if (bar != std::string::npos) {
                    residual_history_.resetSatSignal(
                        key.substr(0, bar), key.substr(bar + 1));
                }
            }
            pending_sat_bias_resets_.clear();
        }

        // --- IMU attitude error feedback ---
        // After the GNSS update, the attitude error state dpsi has been
        // corrected.  Apply it to the rotation matrix and reset dpsi to zero.
        if (state_manager_.hasImuBiases()) {
            const size_t att_idx = state_manager_.getAttitudeIndex();
            const Eigen::Vector3d dpsi = x_new.segment<3>(att_idx);
            if (dpsi.norm() > 1e-15) {
                // Small-angle rotation correction: C_corrected = (I + [dpsi]x) * C
                Eigen::Matrix3d dR;
                dR <<       1.0, -dpsi(2),  dpsi(1),
                       dpsi(2),       1.0, -dpsi(0),
                      -dpsi(1),  dpsi(0),       1.0;
                C_b_e_ = dR * C_b_e_;
                // Re-orthonormalize.
                Eigen::JacobiSVD<Eigen::Matrix3d> svd(C_b_e_, Eigen::ComputeFullU | Eigen::ComputeFullV);
                C_b_e_ = svd.matrixU() * svd.matrixV().transpose();
                if (C_b_e_.determinant() < 0.0) C_b_e_.col(2) *= -1.0;

                // Reset the attitude error state to zero.
                Eigen::VectorXd x_reset = state_manager_.getStateVector();
                x_reset.segment<3>(att_idx).setZero();
                state_manager_.setStateVector(x_reset);
            }
        }

        if (logger_) logger_->logStateVector(x_new, "Updated State Vector", epoch_tag);
        if (logger_) logger_->logCovarianceMatrix(P_new, "Updated Covariance Matrix", epoch_tag);

        return UpdateResult::Applied;
    }

    // ===================================================================
    // Iterated-EKF (Gauss–Newton) update from an explicit prior
    // ===================================================================

    KalmanFilter::UpdateResult KalmanFilter::updateFromPrior(
        EpochMeasurement& measurement,
        const Eigen::VectorXd& x_prior,
        const Eigen::MatrixXd& P_prior) {
        auto klogger = log_manager_.getLogger("kalman_filter");

        // --- Input validation ---
        const Eigen::Index m = measurement.innovations.size();
        if (m == 0 || measurement.H.rows() != m || measurement.R_v.size() != m) {
            klogger->warn("IEKF relinearization: invalid measurement (innovations {}, H {}x{}, R_v {})",
                m, measurement.H.rows(), measurement.H.cols(), measurement.R_v.size());
            return UpdateResult::Skipped;
        }
        const Eigen::Index n = static_cast<Eigen::Index>(state_manager_.getStateSize());
        if (measurement.H.cols() != n || x_prior.size() != n ||
            P_prior.rows() != n || P_prior.cols() != n) {
            klogger->error("IEKF relinearization: dimension mismatch "
                "(H {}x{}, x_prior {}, P_prior {}x{}, state {})",
                measurement.H.rows(), measurement.H.cols(), x_prior.size(),
                P_prior.rows(), P_prior.cols(), n);
            return UpdateResult::Skipped;
        }

        // --- Build R (full if provided, else diagonal from R_v) ---
        Eigen::MatrixXd R;
        if (measurement.R_full.rows() == m && measurement.R_full.cols() == m) {
            R = measurement.R_full;
        } else {
            R = measurement.R_v.asDiagonal();
        }

        // Linearization point xᵢ: the committed state `measurement` was
        // linearized at.  First IEKF pass, that is the posterior update()
        // converged on; later passes, the previous iterate.
        const Eigen::VectorXd x_lin = state_manager_.getStateVector();

        // Gauss-Newton (IEKF) innovation against the fixed prior x⁻:
        //   ν = (z − h(xᵢ)) − H·(x⁻ − xᵢ) = innovations + H·(xᵢ − x⁻).
        // The offset term makes the step independent of the linearization point
        // in the linear limit, so josephPseudoUpdate(x⁻, P⁻, …, ν) gives the MAP
        // update with K and (I−KH)P⁻ both formed from the prior, and measurement
        // information is not counted twice across passes.
        const Eigen::VectorXd nu =
            measurement.innovations + measurement.H * (x_lin - x_prior);

        Eigen::VectorXd x_new;
        Eigen::MatrixXd P_new;
        josephPseudoUpdate(x_prior, P_prior, measurement.H, nu, R, x_new, P_new);

        if (!x_new.allFinite() || !P_new.allFinite()) {
            klogger->warn("IEKF relinearization: non-finite result; skipping commit "
                "(StateManager state left unchanged)");
            return UpdateResult::Skipped;
        }

        // --- Post-fit residuals about x_new (for the residual trace) ---
        //   r = (z − h(xᵢ)) − H·(x_new − xᵢ).
        measurement.residuals =
            measurement.innovations - measurement.H * (x_new - x_lin);

        // Standardized residuals with Sr = R − H·P_new·Hᵀ, the denominator
        // floored at R(i,i) of the R this update used, the same convention as
        // update(), so the residual trace is directly comparable across the two
        // paths.  This path applies no persistent-bias or Huber R-inflation.
        const Eigen::MatrixXd HP_new = measurement.H * P_new;   // m×n
        Eigen::VectorXd sr_diag(m);
        Eigen::VectorXd sres(m);
        for (Eigen::Index i = 0; i < m; ++i) {
            const double hph = HP_new.row(i).dot(measurement.H.row(i));
            const double sii = R(i, i) - hph;
            sr_diag(i) = sii;
            const double r_floor = (R(i, i) > 0.0) ? R(i, i) : 0.0;
            const double denom = std::sqrt(std::max({ sii, r_floor, 1e-12 }));
            const double val = measurement.residuals(i);
            sres(i) = (std::isfinite(val) && denom > 0.0) ? (val / denom) : 0.0;
        }
        measurement.residuals_standardized = sres;

        // Full Sr = R − H·P⁺·Hᵀ when the off-diagonal kernel is on and a dense R
        // was supplied (as in update(); the diagonal is unchanged), then the
        // same statistical QC as the primary path, so the residual trace
        // carries vf / redundancy / w-test on relinearized epochs too
        // (items #13A and #9).
        const bool dense_r_supplied =
            (measurement.R_full.rows() == m && measurement.R_full.cols() == m);
        Eigen::MatrixXd Sr_full;
        if (config_.enableFullRKernel() && dense_r_supplied) {
            Sr_full = R - HP_new * measurement.H.transpose();   // m×m
            Sr_full = 0.5 * (Sr_full + Sr_full.transpose());
            measurement.residuals_covariance = Sr_full;
        } else {
            measurement.residuals_covariance = sr_diag.asDiagonal();
        }
        if (config_.enableVarianceFactor() || config_.enableDataSnooping()) {
            computeResidualQc_(measurement, R, sr_diag,
                               Sr_full.size() != 0 ? &Sr_full : nullptr);
        }

        // --- Commit ---
        state_manager_.setStateVector(x_new);
        state_manager_.setCovariance(P_new);
        state_manager_.setStateEpochTime(measurement.time);

        klogger->debug("IEKF relinearization: dx.norm={:.3e}, step|Δpos|={:.4f} m",
            (x_new - x_prior).norm(), (x_new.head<3>() - x_lin.head<3>()).norm());
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

        // --- Baseline DD path ---
        // DD ambiguities use a different state-key format from the PPP /
        // BSSD path ("DD|cons|sat_i|rov_code|pivot" vs "sat_sig"), and only
        // one DD ambiguity exists per row (the pivot is not a state).  Hand
        // off to the baseline-aware variant before any PPP-format key
        // construction runs.
        if (config_.isBaselineMode()) {
            handleBaselinePhaseOutlier(measurement, outlier_sat, outlier_sig,
                                       primary_sat, ref_sat, x, P, epoch_prefix);
            return;
        }

        const bool bssd_active = config_.enableBetweenSatelliteSd();

        // In BSSD mode, identify the pivot satellite for this constellation.
        // Primary source: the pivot manager's map.  Fallback: the ref_sat
        // extracted from the SD pair name (BssdTransform always forms pairs
        // as "non_pivot-pivot", so ref_sat is the pivot when present).
        const auto& bssd_pivots = measurement_model_.getBssdPivotSatellites();
        const std::string primary_cons = PppUtils::getSystemString(primary_sat);
        auto piv_it = bssd_pivots.find(primary_cons);
        const std::string pivot_sat = (bssd_active && piv_it != bssd_pivots.end())
                                    ? piv_it->second
                                    : (bssd_active && !ref_sat.empty())
                                        ? ref_sat : std::string{};

        // Compute the pivot's absolute seed once (used by all non-pivot resets).
        // The pivot may use a different tracking attribute on the same band
        // (e.g. L1W vs L1C), so match by frequency band (first two chars).
        double pivot_abs_seed = 0.0;
        std::string pivot_band_sig;  // actual signal type the pivot uses on this band
        if (bssd_active && !pivot_sat.empty()) {
            const std::string outlier_band = (outlier_sig.size() >= 2)
                                           ? outlier_sig.substr(0, 2) : outlier_sig;
            for (const auto& obs : measurement.observations) {
                if (obs.satellite != pivot_sat) continue;
                // Try exact match first, then band match.
                if (obs.observed.count(outlier_sig)) {
                    pivot_abs_seed = state_manager_.computeAmbiguitySeed(obs, outlier_sig, x);
                    pivot_band_sig = outlier_sig;
                } else {
                    for (const auto& [sig, val] : obs.observed) {
                        if (sig.size() >= 2 && sig.substr(0, 2) == outlier_band) {
                            pivot_abs_seed = state_manager_.computeAmbiguitySeed(obs, sig, x);
                            pivot_band_sig = sig;
                            break;
                        }
                    }
                }
                break;
            }
        }

        // Helper: reset a single ambiguity and track the satellite
        auto resetOneAmbiguity = [&](const std::string& sat, const std::string& label) {
            const std::string amb_key = sat + "_" + outlier_sig;
            const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);

            klogger->debug("{}Phase outlier action ({}): ambiguity reset for key={}, amb_idx={}",
                epoch_prefix, label, amb_key, amb_idx);

            if (amb_idx < 0) {
                klogger->warn("{}Phase outlier but ambiguity key not found: {}", epoch_prefix, amb_key);
                return;
            }

            // The pivot's SD ambiguity is 0 by definition, so it stays frozen.
            // Inflating its variance would trigger the BSSD disturbance
            // detector and, from there, an unnecessary pivot handover.
            if (bssd_active && sat == pivot_sat) {
                klogger->debug("{}Phase outlier on BSSD pivot {} {}: keeping frozen at 0",
                    epoch_prefix, sat, outlier_sig);
                last_reset_satellites_.push_back(sat);
                last_reset_signals_.emplace_back(sat, outlier_sig);
                return;
            }
            // Safety: if BSSD is active but pivot_sat is unexpectedly empty,
            // log a diagnostic so the issue is visible.
            if (bssd_active && pivot_sat.empty()) {
                klogger->warn("{}Phase outlier on {} {}: BSSD active but pivot lookup "
                              "returned empty (cons={})",
                              epoch_prefix, sat, outlier_sig, primary_cons);
            }

            const double prev_amb = x(amb_idx);
            double seed_value = 0.0;

            // Compute absolute seed from current state.
            double abs_seed = 0.0;
            for (const auto& obs : measurement.observations) {
                if (obs.satellite != sat) continue;
                abs_seed = state_manager_.computeAmbiguitySeed(obs, outlier_sig, x);
                break;
            }
            if (bssd_active && !pivot_sat.empty()) {
                // Convert to SD seed: N_sd = N_abs(i) - N_abs(pivot)
                seed_value = abs_seed - pivot_abs_seed;
            } else {
                seed_value = abs_seed;
            }

            x(amb_idx) = seed_value;
            P.row(amb_idx).setZero();
            P.col(amb_idx).setZero();
            P(amb_idx, amb_idx) = amb_std * amb_std;
            state_manager_.noteAmbiguityDiscontinuity(amb_key);

            klogger->debug("{}Reinitialized ambiguity for {} {} (idx {}): prev={:.6f} m -> seed={:.3f} m, std={:.3f} m{}",
                epoch_prefix, sat, outlier_sig, amb_idx, prev_amb, seed_value, amb_std,
                bssd_active ? " (SD)" : "");

            last_reset_satellites_.push_back(sat);
            last_reset_signals_.emplace_back(sat, outlier_sig);
        };

        resetOneAmbiguity(primary_sat, "primary");

        // Under BSSD the ref_sat is always the pivot (pairs are formed
        // "non_pivot-pivot"), and the pivot's SD ambiguity is 0 by definition:
        // resetting it would invalidate every SD row that references it and
        // force an unneeded handover.  The ref_sat is therefore reset only
        // outside BSSD (e.g. combined-mode IF pairs with a non-pivot reference).
        // The pair format guarantees the second element is the pivot even if
        // its lookup was empty.
        if (!ref_sat.empty() && !bssd_active) {
            resetOneAmbiguity(ref_sat, "BSSD-ref");
        } else if (!ref_sat.empty()) {
            klogger->debug("{}Phase outlier on SD pair {}: skipping ref_sat {} reset (BSSD pivot)",
                epoch_prefix, outlier_sat, ref_sat);
        }
    }

    // -------------------------------------------------------------------
    // Baseline (DD) phase outlier handler.  Design notes are in the header.
    // -------------------------------------------------------------------
    void KalmanFilter::handleBaselinePhaseOutlier(const EpochMeasurement& measurement,
        const std::string& outlier_sat, const std::string& outlier_sig,
        const std::string& primary_sat, const std::string& ref_sat,
        Eigen::VectorXd& x, Eigen::MatrixXd& P,
        const std::string& epoch_prefix) {

        auto klogger = log_manager_.getLogger("kalman_filter");
        const double amb_std = config_.getAmbiguityStd();

        if (primary_sat.empty() || ref_sat.empty()) {
            klogger->warn("{}Baseline phase outlier: malformed pair '{}' "
                          "(expected 'sat_i-pivot' from MeasurementModel)",
                          epoch_prefix, outlier_sat);
            return;
        }

        // Recover the rover RINEX code: aliased rows have "rov/ref" in
        // signal_types, e.g. "L2W/L2C".  The DD ambiguity key uses the
        // rover side only (see MeasurementModel::computeMeasurementUpdate).
        std::string rov_code = outlier_sig;
        const auto slash = rov_code.find('/');
        if (slash != std::string::npos) {
            rov_code = rov_code.substr(0, slash);
        }

        // Constellation prefix matches MeasurementModel's ddConstellationOf.
        // Inlined here to avoid a cross-module dependency for one switch.
        auto consPrefix = [](char sys) -> std::string {
            switch (sys) {
            case 'G': return "GPS";
            case 'E': return "Galileo";
            case 'R': return "GLONASS";
            case 'C': return "BeiDou";
            case 'J': return "QZSS";
            case 'S': return "SBAS";
            case 'I': return "IRNSS";
            default:  return std::string(1, sys);
            }
        };
        if (primary_sat.empty()) return;
        const std::string cons = consPrefix(primary_sat[0]);
        const std::string amb_key =
            "DD|" + cons + "|" + primary_sat + "|" + rov_code + "|" + ref_sat;

        const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
        if (amb_idx < 0) {
            klogger->warn("{}Baseline phase outlier: DD ambiguity not found for key '{}'",
                          epoch_prefix, amb_key);
            return;
        }

        // Recover the pre-update DD residual.  MeasurementModel sets
        // innov = innov_pre − x(amb_idx) on phase rows, so re-seeding at
        // x(amb_idx) + innov makes innov_pre the new float and the next epoch
        // starts with a small innovation whatever the slip magnitude.
        Eigen::Index row_idx = -1;
        const Eigen::Index nrows = measurement.innovations.size();
        for (Eigen::Index i = 0; i < nrows; ++i) {
            if (i >= static_cast<Eigen::Index>(measurement.satellite_ids.size())) break;
            if (i >= static_cast<Eigen::Index>(measurement.signal_types.size())) break;
            if (measurement.satellite_ids[static_cast<size_t>(i)] == outlier_sat
             && measurement.signal_types[static_cast<size_t>(i)] == outlier_sig) {
                row_idx = i;
                break;
            }
        }

        const double prev_amb = x(amb_idx);
        double seed_value = prev_amb;
        if (row_idx >= 0) {
            seed_value = prev_amb + measurement.innovations(row_idx);
        } else {
            klogger->warn("{}Baseline phase outlier: row not found for sat='{}' sig='{}', "
                          "re-seed degraded to current state value",
                          epoch_prefix, outlier_sat, outlier_sig);
        }

        x(amb_idx) = seed_value;
        P.row(amb_idx).setZero();
        P.col(amb_idx).setZero();
        P(amb_idx, amb_idx) = amb_std * amb_std;
        state_manager_.noteAmbiguityDiscontinuity(amb_key);

        klogger->debug("{}Baseline phase outlier: re-seeded {} (idx {}): "
                       "prev={:.4f} m -> seed={:.4f} m, std={:.3f} m",
                       epoch_prefix, amb_key, amb_idx, prev_amb, seed_value, amb_std);

        last_reset_satellites_.push_back(primary_sat);
        last_reset_signals_.emplace_back(primary_sat, outlier_sig);
    }

    void KalmanFilter::handleCodeOutlier(EpochMeasurement& measurement,
        const std::string& outlier_sat, const std::string& outlier_sig,
        size_t max_idx, const std::string& epoch_prefix) {

        auto klogger = log_manager_.getLogger("kalman_filter");
        const auto [primary_sat, ref_sat] = splitBssdSatId(outlier_sat);

        const std::string excl_key = primary_sat + "_" + outlier_sig;
        measurement.excluded_measurements.insert(excl_key);

        // Bump the consecutive-rejection streak (cleared by any epoch that keeps
        // the row).  At estimation.stuck_code_bias.min_streak the clean-commit
        // path resets the sat-bias state, breaking the loop where an
        // over-converged bias keeps rejecting its own row.
        residual_history_.recordRejection(primary_sat, outlier_sig);

        klogger->debug("{}Code outlier action: excluding measurement {} {} (key={})",
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

        const double pos_var   = initial_std.position >= 0 ? initial_std.position * initial_std.position : 10000.0;
        P(0, 0) = pos_var;
        P(1, 1) = pos_var;
        P(2, 2) = pos_var;

        // Baseline mode: no clock, tropo, gradient, ISB, iono or IMU states to
        // reset, so the ambiguity diagonals are reset here and the function returns.
        if (config_.isBaselineMode()) {
            for (const auto& [key, idx] : state_manager_.getAmbiguities()) {
                if (idx < static_cast<size_t>(P.rows())) {
                    P(idx, idx) = amb_std * amb_std;
                    state_manager_.noteAmbiguityDiscontinuity(key);
                }
            }
            klogger->warn("{}Complete reset applied (baseline): state_size={}, ambiguity_std={:.3f} m",
                epoch_prefix, static_cast<size_t>(P.rows()), amb_std);
            return;
        }

        const double clk_var   = initial_std.clock >= 0    ? initial_std.clock * initial_std.clock       : 90000.0;
        const double tropo_var = initial_std.troposphere >= 0 ? initial_std.troposphere * initial_std.troposphere : 0.25;
        if (state_manager_.hasClock()) {
            const size_t clk_idx = state_manager_.getClockIndex();
            P(clk_idx, clk_idx) = clk_var;
        }
        const size_t tropo_idx = state_manager_.getTropoIndex();
        P(tropo_idx, tropo_idx) = tropo_var;

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
            // Constant-velocity model: the companion rate rows were just zeroed
            // by P.setZero(); restore their cold-start variance so they are not
            // mistaken for frozen (0-diagonal) pivots.
            if (config_.enableIonoRate()) {
                const double ir_var = initial_std.ionosphere_rate * initial_std.ionosphere_rate;
                for (const auto& [sat, idx] : state_manager_.getIonosphereRateStates()) {
                    if (idx < static_cast<size_t>(P.rows()))
                        P(idx, idx) = ir_var;
                }
            }
        }

        // Ambiguities: reset all to the initial variance, except that BSSD pivot
        // ambiguities and pivot ionosphere stay frozen (value 0, variance 0).
        // Otherwise the disturbance detector would fire on every epoch after a
        // complete reset, because every satellite would appear disturbed.
        const auto& bssd_pivots = measurement_model_.getBssdPivotSatellites();
        const bool bssd_active = config_.enableBetweenSatelliteSd();

        for (const auto& [key, idx] : state_manager_.getAmbiguities()) {
            if (idx >= static_cast<size_t>(P.rows())) continue;

            // Check if this ambiguity belongs to a BSSD pivot satellite.
            if (bssd_active) {
                const auto us = key.find('_');
                if (us != std::string::npos) {
                    const std::string sat = key.substr(0, us);
                    const std::string cons = PppUtils::getSystemString(sat);
                    auto piv_it = bssd_pivots.find(cons);
                    if (piv_it != bssd_pivots.end() && piv_it->second == sat) {
                        P(idx, idx) = 0.0;  // keep frozen
                        continue;
                    }
                }
            }
            P(idx, idx) = amb_std * amb_std;
            state_manager_.noteAmbiguityDiscontinuity(key);
        }

        // Freeze BSSD pivot ionosphere states after reset.
        if (bssd_active && config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
            for (const auto& [sat, idx] : state_manager_.getIonosphereStates()) {
                if (idx >= static_cast<size_t>(P.rows())) continue;
                const std::string cons = PppUtils::getSystemString(sat);
                auto piv_it = bssd_pivots.find(cons);
                if (piv_it != bssd_pivots.end() && piv_it->second == sat)
                    P(idx, idx) = 0.0;  // keep frozen
            }
            // Freeze the companion pivot iono-rate states (constant-velocity model).
            if (config_.enableIonoRate()) {
                for (const auto& [sat, idx] : state_manager_.getIonosphereRateStates()) {
                    if (idx >= static_cast<size_t>(P.rows())) continue;
                    const std::string cons = PppUtils::getSystemString(sat);
                    auto piv_it = bssd_pivots.find(cons);
                    if (piv_it != bssd_pivots.end() && piv_it->second == sat)
                        P(idx, idx) = 0.0;  // keep frozen
                }
            }
        }

        // IMU bias states
        if (state_manager_.hasImuBiases()) {
            const auto& ic = config_.getImuConfig();
            const double att_var = ic.initial_attitude_std * ic.initial_attitude_std;
            const double ab_var  = ic.initial_accel_bias_std * ic.initial_accel_bias_std;
            const double gb_var  = ic.initial_gyro_bias_std * ic.initial_gyro_bias_std;
            const size_t att_idx = state_manager_.getAttitudeIndex();
            const size_t ab_idx  = state_manager_.getAccelBiasIndex();
            const size_t gb_idx  = state_manager_.getGyroBiasIndex();
            for (size_t k = 0; k < 3; ++k) {
                if (att_idx + k < static_cast<size_t>(P.rows())) P(att_idx + k, att_idx + k) = att_var;
                if (ab_idx + k < static_cast<size_t>(P.rows())) P(ab_idx + k, ab_idx + k) = ab_var;
                if (gb_idx + k < static_cast<size_t>(P.rows())) P(gb_idx + k, gb_idx + k) = gb_var;
            }
        }

        klogger->warn("{}Complete reset applied: state_size={}, ambiguity_std={:.3f} m",
            epoch_prefix, static_cast<size_t>(P.rows()), amb_std);
    }

    // ===================================================================
    // Statistical QC: a-posteriori variance factor + Baarda data-snooping
    // ===================================================================

    void KalmanFilter::computeResidualQc_(
        EpochMeasurement& meas, const Eigen::MatrixXd& R,
        const Eigen::VectorXd& sr_diag, const Eigen::MatrixXd* Sr_full) const {

        const Eigen::Index m = meas.residuals.size();
        if (m == 0) {
            meas.variance_factor = 1.0;
            meas.variance_dof    = 0.0;
            meas.residual_redundancy.resize(0);
            meas.residual_wtest.resize(0);
            meas.residual_mdb.resize(0);
            return;
        }

        // Baarda non-centrality δ₀ at the conventional 80% test power
        // (β=0.20 → Φ⁻¹(0.80)=0.8416); δ₀≈4.13 for the default k=3.29.  σ₀²=1
        // since R is in absolute units, so MDBᵢ = σᵢ·δ₀/√rᵢ.
        //
        // δ₀, the r-floor (1e-6) and the r→0 ⇒ ∞ sentinel come from
        // filter/BaardaReliability.hpp, so the network path's MDB is
        // commensurable with this one by construction rather than by a second
        // copy of the formula.
        const double delta_0 = baardaDelta0(config_.dataSnoopingCritical());
        const ResidualQcStats q =
            residualQcKernel(meas.residuals, R, sr_diag, Sr_full, delta_0);

        meas.residual_redundancy = q.redundancy;
        meas.residual_wtest      = q.wtest;
        meas.residual_mdb        = q.mdb;
        meas.variance_dof        = q.dof;
        meas.variance_factor     =
            (q.dof > 1e-9 && std::isfinite(q.wssr)) ? (q.wssr / q.dof) : 1.0;
    }

    KalmanFilter::ResidualQcStats KalmanFilter::residualQcKernel(
        const Eigen::VectorXd& v, const Eigen::MatrixXd& R,
        const Eigen::VectorXd& sr_diag, const Eigen::MatrixXd* Sr_full,
        double delta_0) {

        const Eigen::Index m = v.size();
        constexpr double kSrFloor  = 1e-12;   // guard /√Sr

        Eigen::VectorXd redundancy(m);
        Eigen::VectorXd wtest(m);
        Eigen::VectorXd mdb(m);
        double wssr = 0.0;   // Σ vᵀR⁻¹v  (weighted post-fit residual SSR)
        double dof  = 0.0;   // Σ rᵢ      (effective redundancy / degrees of freedom)

        bool use_full = (Sr_full != nullptr &&
                         Sr_full->rows() == m && Sr_full->cols() == m &&
                         R.rows() == m && R.cols() == m);

        // P = R⁻¹, formed only for the correlation-aware branch.  An R that is not
        // positive definite has no weight matrix, so that branch falls back to the
        // diagonal one rather than testing against an invented P.
        Eigen::MatrixXd Rinv;
        if (use_full) {
            const Eigen::LDLT<Eigen::MatrixXd> R_ldlt(R);
            use_full = (R_ldlt.info() == Eigen::Success) && R_ldlt.isPositive();
            if (use_full) {
                Rinv = R_ldlt.solve(Eigen::MatrixXd::Identity(m, m));
                use_full = Rinv.allFinite();
            }
        }

        if (use_full) {
            // Correlation-aware path (off-diagonal R kernel, item #13A).  With the
            // weight matrix P = R⁻¹ and the residual cofactor Q_v = Sr:
            //   redundancy matrix  Q_v·P = Sr·R⁻¹   → rᵢ = (Sr R⁻¹)ᵢᵢ, dof = tr
            //   w-test (Baarda)    wᵢ = (P v)ᵢ / √(P Q_v P)ᵢᵢ
            //   MDB                MDBᵢ = δ₀ / √(P Q_v P)ᵢᵢ
            //   wssr               = vᵀ P v
            // In a Kalman update P·v = S⁻¹·z and P·Q_v·P = S⁻¹, so wᵢ is the
            // innovation w-test (S⁻¹z)ᵢ/√(S⁻¹)ᵢᵢ.  Until 2026-10-07 this branch used
            // Sr⁻¹ in place of P: that statistic is N(0,1) under H₀ but tests the
            // direction S·R⁻¹·eᵢ rather than observation i, so it lost power against
            // a blunder in row i and disagreed with the diagonal branch even for a
            // diagonal R (Snow, Adjustment Computations, ch. 9; LASER 5-97..5-100).
            const Eigen::MatrixXd Red = (*Sr_full) * Rinv;
            const Eigen::MatrixXd PQP = Rinv * (*Sr_full) * Rinv;
            const Eigen::VectorXd Pv  = Rinv * v;
            wssr = v.dot(Pv);
            for (Eigen::Index i = 0; i < m; ++i) {
                double r_i = Red(i, i);
                r_i = std::isfinite(r_i) ? std::min(std::max(r_i, 0.0), 1.0) : 0.0;
                redundancy(i) = r_i;
                dof += r_i;
                // wᵢ and MDBᵢ are written through the row's own σ so their guards
                // match the diagonal branch: uᵢ = Rᵢᵢ·(P v)ᵢ (m) and
                // sᵢ = Rᵢᵢ²·(P Q_v P)ᵢᵢ (m²) are exactly vᵢ and Sr_ii when R is
                // diagonal, and r̃ᵢ = Rᵢᵢ·(P Q_v P)ᵢᵢ is then rᵢ.
                const double Rii = R(i, i);
                const double pqp = PQP(i, i);
                const double u_i = Rii * Pv(i);
                const double s_i = Rii * Rii * pqp;
                wtest(i) = (Rii > 0.0 && std::isfinite(u_i) && std::isfinite(s_i))
                    ? u_i / std::sqrt(std::max(s_i, kSrFloor)) : 0.0;
                const double sigma_i = (Rii > 0.0) ? std::sqrt(Rii) : 0.0;
                const double r_mdb   = (Rii > 0.0 && std::isfinite(pqp)) ? Rii * pqp : 0.0;
                mdb(i) = baardaMinimalDetectableBias(sigma_i, r_mdb, delta_0);
            }
        } else {
            // Diagonal (uncorrelated) path, the common PPP case.
            for (Eigen::Index i = 0; i < m; ++i) {
                const double Rii  = (i < R.rows() && R(i, i) > 0.0) ? R(i, i) : 0.0;
                const double srii = (i < sr_diag.size()) ? sr_diag(i) : 0.0;
                const double v_i  = v(i);
                double r_i = (Rii > 0.0) ? srii / Rii : 0.0;
                r_i = std::isfinite(r_i) ? std::min(std::max(r_i, 0.0), 1.0) : 0.0;
                redundancy(i) = r_i;
                dof += r_i;
                if (Rii > 0.0 && std::isfinite(v_i)) wssr += (v_i * v_i) / Rii;
                // Baarda w-test: the studentized residual v/√Sr, not floored at R.
                const double srii_pos = std::max(srii, kSrFloor);
                wtest(i) = std::isfinite(v_i) ? v_i / std::sqrt(srii_pos) : 0.0;
                const double sigma_i = (Rii > 0.0) ? std::sqrt(Rii) : 0.0;
                mdb(i) = baardaMinimalDetectableBias(sigma_i, r_i, delta_0);
            }
        }

        ResidualQcStats out;
        out.redundancy = std::move(redundancy);
        out.wtest      = std::move(wtest);
        out.mdb        = std::move(mdb);
        out.wssr       = wssr;
        out.dof        = dof;
        return out;
    }

    // ===================================================================
    // Post-fit residual validation
    // ===================================================================

    std::pair<bool, bool> KalmanFilter::validatePostFitResiduals(
        EpochMeasurement& measurement, Eigen::VectorXd& x, Eigen::MatrixXd& P) {

        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix;  // see KalmanFilter::update for rationale

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

        // Pass-invariant denominator for the complete-reset fraction below: the
        // rows this epoch started with, i.e. the current usable rows plus every
        // row that earlier passes excluded.  At this point excluded_measurements
        // holds exactly the prior exclusions (this pass's dispatch has not run
        // yet), so the sum is the original count before any peeling.
        //
        // With the shrinking per-pass num_obs as the denominator, the reset
        // would trip after excluding only about p/(1+p) of the original set
        // (33 % at p = 0.5, not the configured 50 %), aborting a converging peel
        // and re-inflating the filter every epoch while a code outlier persists.
        const size_t total_obs_count =
            num_obs + measurement.excluded_measurements.size();

        const size_t meta_n = std::min({ num_obs, measurement.satellite_ids.size(), measurement.signal_types.size() });

        // --- Per-measurement diagnostic log ---
        // Helper: emit the per-residual table at a given log level.
        auto logResidualTable = [&](spdlog::level::level_enum lvl) {
            for (size_t i = 0; i < meta_n; ++i) {
                const auto ei = static_cast<Eigen::Index>(i);
                const double var_i = (i < static_cast<size_t>(vres.rows()) && i < static_cast<size_t>(vres.cols()))
                    ? vres(ei, ei) : std::numeric_limits<double>::quiet_NaN();
                const double sigma_i = std::sqrt(std::max(var_i, 0.0));
                const double s_i = sres(ei);
                const double s_abs = std::abs(s_i);
                klogger->log(lvl,
                    "{}  res[{:02d}] {} {}: v={:+.4f} m, sigma={:.4f} m, s={:+.3f}, |s|={:.3f}, outlier={}",
                    epoch_prefix, static_cast<int>(i),
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    res(ei), sigma_i, s_i, s_abs,
                    (std::isfinite(s_abs) && s_abs > threshold) ? "YES" : "no");
            }
        };

        klogger->debug("{}Post-fit residual check: n_obs={}, threshold={:.3f}, bad_count={}, bad_percent={:.3f}",
            epoch_prefix, num_obs, threshold, measurement.bad_residual_count, bad_meas_percent);

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

        // Per-row residual table.  Output gets strictly more concise as the
        // threshold rises:
        //   trace: full table every epoch
        //   debug: table only on outlier events
        //   info : none (the Residual summary line covers it)
        //   warn : none (pivot warnings and outlier-flagged warns only)
        if (klogger->should_log(spdlog::level::trace)) {
            logResidualTable(spdlog::level::trace);
        } else if (max_sres > threshold && klogger->should_log(spdlog::level::debug)) {
            logResidualTable(spdlog::level::debug);
        }

        // INFO when this pass commits cleanly (max_sres ≤ threshold), giving one
        // summary line per converged epoch; DEBUG when outliers will be handled,
        // since mid-rebuild summaries are diagnostic.  A pivot-slip escalation
        // carries its own warn-level message, so info here would be redundant.
        const auto residual_summary_level =
            (max_sres > threshold) ? spdlog::level::debug : spdlog::level::info;

        // RMS of the standardized residuals over finite entries, a one-number
        // check of the filter against its own covariance.  Because sres is
        // floored at R, a consistent filter gives RMS² near the mean
        // redundancy (below 1); values well above 1 mean the filter is
        // over-confident (R or P too tight), values far below it mean it is
        // over-conservative.
        double sum_sq = 0.0;
        std::size_t n_valid_sres = 0;
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double s = sres(i);
            if (std::isfinite(s)) {
                sum_sq += s * s;
                ++n_valid_sres;
            }
        }
        const double rms_sres = (n_valid_sres > 0)
            ? std::sqrt(sum_sq / static_cast<double>(n_valid_sres)) : 0.0;

        // Position σ from the predicted-state covariance diagonal.
        // ECEF position lives at state indices 0..2 by the
        // StateManager schema (see include/filter/StateKeySchema.hpp).
        // Reported in metres so users can eyeball convergence at a
        // glance from a single info line.
        const double sx = (P.rows() >= 3) ? std::sqrt(std::max(P(0, 0), 0.0)) : 0.0;
        const double sy = (P.rows() >= 3) ? std::sqrt(std::max(P(1, 1), 0.0)) : 0.0;
        const double sz = (P.rows() >= 3) ? std::sqrt(std::max(P(2, 2), 0.0)) : 0.0;

        klogger->log(residual_summary_level,
            "{}Residual summary: max|sres|={:.3f} at idx={}, RMS|sres|={:.3f}, "
            "over_threshold={}/{}, σ_pos=({:.3f}, {:.3f}, {:.3f}) m",
            epoch_prefix, max_sres, max_idx, rms_sres, num_over, num_obs, sx, sy, sz);

        // --- Statistical QC reporting (item #9) ---------------------------
        // Diagnostics only; the propagated state and covariance are untouched.
        // The variance factor rescales a reporting copy of σ_pos
        // (σ_pos → formal·√σ̂₀²) and never feeds back into P.
        if (config_.enableVarianceFactor()) {
            const double vf = measurement.variance_factor;
            const double sf = (std::isfinite(vf) && vf > 0.0) ? std::sqrt(vf) : 1.0;
            klogger->log(residual_summary_level,
                "{}Variance factor: sigma0^2={:.3f}, dof={:.2f}, "
                "sigma_pos(scaled)=({:.3f}, {:.3f}, {:.3f}) m",
                epoch_prefix, vf, measurement.variance_dof,
                sx * sf, sy * sf, sz * sf);
        }
        if (config_.enableDataSnooping() &&
            measurement.residual_wtest.size() == sres.size() &&
            measurement.residual_redundancy.size() == sres.size()) {
            constexpr double kUncheckableRedundancy = 0.1;
            const double w_crit = config_.dataSnoopingCritical();
            size_t n_uncheckable = 0, n_flagged = 0;
            double max_w = 0.0;
            size_t max_w_idx = 0;
            for (Eigen::Index i = 0; i < measurement.residual_wtest.size(); ++i) {
                if (measurement.residual_redundancy(i) < kUncheckableRedundancy) {
                    ++n_uncheckable;
                    continue;
                }
                const double w_i = std::abs(measurement.residual_wtest(i));
                if (!std::isfinite(w_i)) continue;
                if (w_i > max_w) { max_w = w_i; max_w_idx = static_cast<size_t>(i); }
                if (w_i > w_crit) ++n_flagged;
            }
            const std::string w_sat = (max_w_idx < measurement.satellite_ids.size())
                ? measurement.satellite_ids[max_w_idx] : std::string("?");
            const std::string w_sig = (max_w_idx < measurement.signal_types.size())
                ? measurement.signal_types[max_w_idx] : std::string("?");
            klogger->log(residual_summary_level,
                "{}Data-snooping: max|w|={:.3f} ({} {}) vs w_crit={:.2f}, "
                "flagged={}, uncheckable={}/{}",
                epoch_prefix, max_w, w_sat, w_sig, w_crit,
                n_flagged, n_uncheckable, num_obs);
        }

        // --- Pivot slip detector (MAD-based, percentage-thresholded) ---
        //
        // Runs before the per-row dispatch.  A BSSD pivot with a phase glitch
        // (sub-cycle drift, multipath ringing, partial reacquisition) offsets
        // every SD residual that uses it by the same sign and magnitude on the
        // same band.  The per-row |sres| > threshold check would read those as N
        // independent outliers and peel them one by one, resetting each
        // non-pivot ambiguity and clearing the MW arc histories of satellites
        // that did not slip.
        //
        // Rows are grouped by (pivot, signal) using the full RINEX-3 code, so
        // "L1W" and "L1C" are separate groups.  That matches the state
        // granularity of uncombined PPP-RTK and preserves sensitivity to a
        // disturbance confined to one tracking attribute, which band-level
        // grouping would dilute.  Each group is tested for the coherent-offset
        // signature:
        //   1. ≥ MIN_GROUP_SIZE rows (statistical power).
        //   2. ≥ ABOVE_FRACTION_MIN of |sres| above the per-row threshold; a
        //      fraction rather than a count, so the trigger scales with visibility.
        //   3. |median(residual)| ≥ MEDIAN_FLOOR_M, so numerical noise cannot fire it.
        //   4. MAD/|median| ≤ MAD_RATIO_MAX.  This is the robustness step: the
        //      median ignores one or two satellites that also slipped, and the
        //      MAD measures spread without being inflated by them.  Tight means
        //      a coherent pivot offset; loose means independent outliers.
        //
        // On detection the detector requests a forced pivot handover for the
        // constellation, records the satellites the handover must not elect,
        // and excludes all of the group's rows from the rebuild.  The per-row
        // dispatch is skipped for this pass, since the pivot slip explains
        // every outlier.
        {
            constexpr size_t MIN_GROUP_SIZE      = 4;
            constexpr size_t MIN_POOL_SIZE       = 3;  // cross-band detector
            constexpr size_t MIN_DISTINCT_BANDS  = 2;  // cross-band detector
            constexpr double ABOVE_FRACTION_MIN  = 0.5;
            constexpr double MAD_RATIO_MAX       = 0.4;
            constexpr double MEDIAN_FLOOR_M      = 0.005;

            // Robust band (k·MAD, floored) that classifies which non-pivot SD
            // rows in a firing group disagree with the coherent median.  Those
            // satellites are co-disturbed or slipping independently, so they
            // become replacement-pivot exclusions and the handover lands on a
            // consensus satellite.  k = 3 sits just inside the observed
            // co-offset separation (a second offset satellite lands about
            // 3.2·MAD off the median); the floor guards MAD → 0.  The band is
            // permissive on purpose: PivotManager's retired-pivot backstop
            // guarantees termination regardless.
            constexpr double HANDOVER_EXCLUDE_MAD_K   = 3.0;
            constexpr double HANDOVER_EXCLUDE_FLOOR_M = 0.02;

            // Wavelength-fraction floor for the median.  A real cycle slip
            // is an integer number of carrier wavelengths; below ~λ/4 the
            // residual cannot be a slip, only sub-cycle bias (multipath,
            // PCV miscal, kinematic dynamics).  Config-tunable per the
            // GnssConfig::getPivotSlipMinCycles() docstring; 0 disables.
            // Helper is shared with the per-row phase outlier handler;
            // see @c bandWavelengthForSat in the file-scope anonymous
            // namespace.
            const double cyc_floor_fraction = config_.getPivotSlipMinCycles();

            struct Group {
                std::vector<size_t> indices;
                std::vector<double> residuals;
                size_t above = 0;
            };
            // Per-signal groups keyed by (pivot, full RINEX-3 signal), so "L1W"
            // and "L1C" stay separate.
            std::map<std::pair<std::string, std::string>, Group> groups;
            // Cross-band per-pivot pools, used as a fallback when individual
            // signals carry too few non-pivot rows to clear MIN_GROUP_SIZE
            // but the multi-band picture still shows a coherent slip pattern.
            // @c pivot_pool_bands tracks distinct bands (the frequency axis),
            // not signals, because the cross-band detector's evidence depends
            // on independent frequency channels; two attributes on the same
            // carrier (L1W + L1C) are not independent.
            std::map<std::string, Group> pivot_pools;
            std::map<std::string, std::set<std::string>> pivot_pool_bands;

            // Exclude SD rows whose ATX correction is frequency-fallback-
            // substituted on either sat or pivot.  The substituted PCV adds a
            // small per-sat bias that, pooled across non-pivot rows in one band,
            // mimics a pivot slip (large median, tight MAD).  Because that bias
            // is structural rather than transient, without this gate "all"-mode
            // runs covering bands like GPS L5 flip-flop forever: every new pivot
            // reads as slipped.
            auto row_atx_substituted = [&](const std::string& sat,
                                            const std::string& pivot_sat,
                                            const std::string& signal) -> bool {
                if (sat.empty() || pivot_sat.empty() || signal.empty()) return false;
                const System sys =
                    sat[0] == 'G' ? System::GPS :
                    sat[0] == 'E' ? System::Galileo :
                    sat[0] == 'R' ? System::GLONASS :
                    sat[0] == 'C' ? System::BeiDou :
                    sat[0] == 'J' ? System::QZSS :
                    sat[0] == 'S' ? System::SBAS : System::Unknown;
                if (sys == System::Unknown) return false;
                if (ppp_model_.wasAtxSubstituted(sat, signal, sys)) return true;
                if (ppp_model_.wasAtxSubstituted(pivot_sat, signal, sys)) return true;
                return false;
            };

            for (size_t i = 0; i < meta_n; ++i) {
                const auto& sat_id = measurement.satellite_ids[i];
                const auto& sig    = measurement.signal_types[i];
                if (sig.size() < 2 || sig[0] != 'L') continue;
                const auto [prim, pivot] = splitBssdSatId(sat_id);
                if (pivot.empty() || prim == pivot) continue;  // not a non-pivot SD row
                if (row_atx_substituted(prim, pivot, sig)) continue;  // ATX-substituted: bias mimics slip
                const std::string band = sig.substr(0, 2);     // "L1", "L2", "L5": the cross-band gate axis only
                const double r  = res(static_cast<Eigen::Index>(i));
                const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                const bool   ab = std::isfinite(sa) && sa > threshold;

                // Per-signal grouping (full RINEX code) for the per-band
                // detector below, matching the per-signal state granularity
                // of uncombined PPP-RTK.
                auto& g = groups[{ pivot, sig }];
                g.indices.push_back(i);
                g.residuals.push_back(r);
                if (ab) ++g.above;

                auto& pp = pivot_pools[pivot];
                pp.indices.push_back(i);
                pp.residuals.push_back(r);
                if (ab) ++pp.above;
                pivot_pool_bands[pivot].insert(band);  // band, not signal: the independence axis
            }

            // Helper: median (modifies its argument).  Even-count median
            // averages the two middle values for stability.
            auto medianInPlace = [](std::vector<double>& v) -> double {
                const size_t n = v.size();
                std::nth_element(v.begin(), v.begin() + n / 2, v.end());
                if (n % 2 != 0) return v[n / 2];
                const double upper = v[n / 2];
                std::nth_element(v.begin(), v.begin() + n / 2 - 1, v.end());
                return 0.5 * (v[n / 2 - 1] + upper);
            };

            // Record the satellites a forced handover must not elect as the
            // new pivot for @p cons: the flagged @p pivot_sat itself, plus any
            // non-pivot row whose SD residual sits outside the coherent band
            // around @p med.  Such rows are co-disturbed or slipping
            // independently; electing one would re-anchor the SD frame on
            // another offset satellite and cause a further handover.  Keyed
            // per constellation; drained by takeRequestedPivotHandoverExcludeSats.
            auto recordHandoverExcludes = [&](const Group& grp, double med,
                                              double mad,
                                              const std::string& pivot_sat,
                                              const std::string& cons) {
                auto& ex = pivot_handover_exclude_sats_[cons];
                ex.insert(pivot_sat);
                const double band = std::max(HANDOVER_EXCLUDE_MAD_K * mad,
                                             HANDOVER_EXCLUDE_FLOOR_M);
                for (size_t k = 0; k < grp.indices.size(); ++k) {
                    if (std::abs(grp.residuals[k] - med) <= band) continue;
                    const auto [prim, _piv] =
                        splitBssdSatId(measurement.satellite_ids[grp.indices[k]]);
                    if (!prim.empty()) ex.insert(prim);
                }
            };

            // Record every non-pivot member of a firing group for post-handover
            // ionosphere / sat-bias re-inflation (code-detector path only; see
            // takeRequestedPivotHandoverIonoReinflateSats).  recordHandoverExcludes
            // keeps only the pivot and the few deviators outside the coherent
            // band; this set also holds the coherent majority inside the band,
            // which are the satellites that absorbed the pivot's code bias.
            // Re-inflating them lets the post-handover frame re-estimate the
            // states that the lossless re-reference cannot correct.
            auto recordIonoReinflate = [&](const Group& grp,
                                           const std::string& cons) {
                auto& reinf = pivot_handover_reinflate_sats_[cons];
                for (size_t k = 0; k < grp.indices.size(); ++k) {
                    const auto [prim, _piv] =
                        splitBssdSatId(measurement.satellite_ids[grp.indices[k]]);
                    if (!prim.empty()) reinf.insert(prim);
                }
            };

            bool any_pivot_slip = false;
            for (auto& [key, g] : groups) {
                const size_t n = g.indices.size();
                if (n < MIN_GROUP_SIZE) continue;
                const double frac = static_cast<double>(g.above) / static_cast<double>(n);
                if (frac < ABOVE_FRACTION_MIN) continue;

                std::vector<double> r_copy = g.residuals;
                const double med = medianInPlace(r_copy);
                if (std::abs(med) < MEDIAN_FLOOR_M) continue;

                const std::string pivot_sat = key.first;

                // Compute the MAD around the median first, so the cycle-floor
                // decision below can use cluster tightness as evidence.  A
                // tight cluster shows that the residuals are not independent
                // noise: multipath or atmosphere at sub-cycle scale gives
                // MAD/|med| well above the cutoff, while a coherent pivot
                // disturbance typically gives MAD/|med| ≤ 0.1.
                std::vector<double> abs_dev;
                abs_dev.reserve(g.residuals.size());
                for (double v : g.residuals) abs_dev.push_back(std::abs(v - med));
                const double mad = medianInPlace(abs_dev);
                const double mad_ratio = mad / std::abs(med);
                if (mad_ratio > MAD_RATIO_MAX) continue;

                // The λ-fraction floor rejects sub-cycle medians as "bias, not
                // slip".  This detector requests a handover, not a per-row
                // reset, and re-referencing is the right response to any
                // coherent pivot disturbance: wind-up error, a half-cycle
                // ambiguity, pivot-only multipath and a hardware bias change all
                // show sub-cycle coherent SD residuals.  The floor is therefore
                // bypassed at or below TIGHT_CLUSTER_MAD_RATIO, where a common
                // pivot disturbance is the only plausible explanation across
                // several rows (the handover then fires even on a 1 cm median).
                // Looser clusters keep the floor as a hedge against
                // single-frequency atmospheric events.
                constexpr double TIGHT_CLUSTER_MAD_RATIO = 0.2;
                if (cyc_floor_fraction > 0.0 && mad_ratio > TIGHT_CLUSTER_MAD_RATIO) {
                    // key.second is the full signal (e.g. "L1W"); the
                    // wavelength is per-band, so strip the attribute.
                    const std::string band_for_lambda = key.second.substr(0, 2);
                    const double lambda = bandWavelengthForSat(pivot_sat, band_for_lambda);
                    if (lambda > 0.0) {
                        const double abs_floor = cyc_floor_fraction * lambda;
                        if (std::abs(med) < abs_floor) {
                            klogger->debug(
                                "{}Pivot-slip candidate suppressed by λ/{:.2f} floor "
                                "on {} {} signal: |med|={:.4f} m < floor={:.4f} m "
                                "(λ={:.4f} m, MAD/|med|={:.2f} > tight-cluster "
                                "threshold {:.2f})",
                                epoch_prefix, 1.0 / cyc_floor_fraction,
                                pivot_sat, key.second, std::abs(med), abs_floor,
                                lambda, mad_ratio, TIGHT_CLUSTER_MAD_RATIO);
                            continue;
                        }
                    }
                }

                // Identify the constellation from the pivot's PRN prefix.  All
                // non-pivot rows in this group share the constellation by
                // construction (BSSD groups SD pairs within a constellation).
                const std::string cons      = PppUtils::getSystemString(pivot_sat);

                // Temporal corroboration via the residual-history tracker.
                //
                // The spatial criteria above also match genuine atmospheric
                // events (storm fronts, scintillation) that shift many
                // satellites at once.  The tracker adds a second axis, whether
                // each row is anomalous against its own history: among rows
                // with at least warmup_n samples, it counts the fraction with
                // |z| > z_threshold.  When that fraction reaches the tracker's
                // fraction threshold the log reports HIGH confidence; spatial
                // evidence alone reports MEDIUM.
                //
                // The handover fires either way: the temporal signal is
                // diagnostic only.  Promoting it to a gate is a one-line change
                // here once the false-positive rate is known.
                std::string confidence_tag = "MEDIUM (spatial-only)";
                size_t temporal_total = 0, temporal_outliers = 0;
                if (residual_history_.isEnabled()) {
                    for (size_t i = 0; i < g.indices.size(); ++i) {
                        const size_t row_idx = g.indices[i];
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig    = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        const double r = g.residuals[i];
                        const auto ev = residual_history_.evaluate(prim, sig, r);
                        if (ev.warmed_up) {
                            ++temporal_total;
                            if (ev.is_outlier) ++temporal_outliers;
                        }
                    }
                    if (temporal_total > 0) {
                        const double temporal_frac =
                            static_cast<double>(temporal_outliers) / static_cast<double>(temporal_total);
                        if (temporal_frac >= residual_history_.fractionThreshold()) {
                            confidence_tag = std::string("HIGH (temporal agrees: ")
                                + std::to_string(temporal_outliers) + "/"
                                + std::to_string(temporal_total) + " rows over z-threshold)";
                        } else {
                            confidence_tag = std::string("MEDIUM (temporal disagrees: ")
                                + std::to_string(temporal_outliers) + "/"
                                + std::to_string(temporal_total) + " rows over z-threshold, "
                                + "below fraction threshold)";
                        }
                    } else {
                        confidence_tag = "MEDIUM (temporal: no warmed-up rows yet)";
                    }
                }

                klogger->warn(
                    "{}PIVOT SLIP [{}] on {} {} {} signal: {}/{} SD residuals coherent "
                    "(median={:+.4f} m, MAD={:.4f} m, MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                    "Requesting forced pivot handover.",
                    epoch_prefix, confidence_tag, cons, pivot_sat, key.second /* signal */,
                    g.above, n, med, mad, mad_ratio, frac);

                // Request a pivot handover for this constellation. Application
                // forwards this to MeasurementModel::requestPivotHandover after
                // KF returns NeedsMeasurementRebuild; the next pass's
                // computeMeasurementUpdate triggers PivotManager's handover
                // path, which losslessly re-references all SD ambiguities,
                // ionosphere states, and satellite biases to a new pivot.
                // Resetting every non-pivot SD ambiguity in place would be
                // wrong: the non-pivots are stable, and only the pivot's
                // observation drifted.
                requested_pivot_handovers_.insert(cons);
                recordHandoverExcludes(g, med, mad, pivot_sat, cons);

                // Exclude every row in the affected group from this rebuild
                // pass so the per-row outlier handler doesn't pile on with
                // false-positive resets.  After the handover (next pass),
                // these rows will be re-formed against the new pivot and
                // residuals against clean satellites will be near zero.
                for (size_t row_idx : g.indices) {
                    const auto& sat_id = measurement.satellite_ids[row_idx];
                    const auto& sig_t  = measurement.signal_types[row_idx];
                    const auto [prim, _piv] = splitBssdSatId(sat_id);
                    measurement.excluded_measurements.insert(prim + "_" + sig_t);
                }
                any_pivot_slip = true;
            }

            // Cross-band pivot-slip detector.
            //
            // The per-band pass needs ≥ MIN_GROUP_SIZE rows on one signal.
            // Sparse per-band coverage (mixed BeiDou-2/3 with C2I/C7I against
            // C1X/C5X, new-block satellites with partial signal sets) can leave
            // every group short of that while the pooled picture is
            // unambiguous.  Rows are therefore pooled across bands per pivot,
            // with MIN_POOL_SIZE = 3 and at least MIN_DISTINCT_BANDS distinct
            // bands.  The multi-band requirement makes the pooled evidence
            // stronger than a within-band group: coherent same-sign residuals
            // across independent frequency channels are hard for multipath or
            // atmospherics to mimic.
            for (auto& [pivot_sat, pp] : pivot_pools) {
                const std::string cons = PppUtils::getSystemString(pivot_sat);
                // Skip if the per-signal detector already requested a
                // handover for this constellation: its row set is a
                // subset of this pool's rows, and a second warning would
                // report the same physical event again.
                if (requested_pivot_handovers_.count(cons)) continue;

                const size_t n = pp.indices.size();
                if (n < MIN_POOL_SIZE) continue;

                const auto& bands_set = pivot_pool_bands[pivot_sat];
                if (bands_set.size() < MIN_DISTINCT_BANDS) continue;

                const double frac = static_cast<double>(pp.above) / static_cast<double>(n);
                if (frac < ABOVE_FRACTION_MIN) continue;

                std::vector<double> r_copy = pp.residuals;
                const double med = medianInPlace(r_copy);
                if (std::abs(med) < MEDIAN_FLOOR_M) continue;

                std::vector<double> abs_dev;
                abs_dev.reserve(pp.residuals.size());
                for (double v : pp.residuals) abs_dev.push_back(std::abs(v - med));
                const double mad = medianInPlace(abs_dev);
                const double mad_ratio = mad / std::abs(med);
                if (mad_ratio > MAD_RATIO_MAX) continue;

                // Build a comma-joined band list for the log line.
                std::string bands_str;
                for (const auto& b : bands_set) {
                    if (!bands_str.empty()) bands_str += ",";
                    bands_str += b;
                }

                klogger->warn(
                    "{}PIVOT SLIP [cross-band] on {} {} ({} bands [{}]): {}/{} "
                    "SD residuals coherent across bands (median={:+.4f} m, "
                    "MAD={:.4f} m, MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                    "Requesting forced pivot handover.",
                    epoch_prefix, cons, pivot_sat, bands_set.size(), bands_str,
                    pp.above, n, med, mad, mad_ratio, frac);

                requested_pivot_handovers_.insert(cons);
                recordHandoverExcludes(pp, med, mad, pivot_sat, cons);

                for (size_t row_idx : pp.indices) {
                    const auto& sat_id = measurement.satellite_ids[row_idx];
                    const auto& sig_t  = measurement.signal_types[row_idx];
                    const auto [prim, _piv] = splitBssdSatId(sat_id);
                    measurement.excluded_measurements.insert(prim + "_" + sig_t);
                }
                any_pivot_slip = true;
            }

            // Code-side pivot disturbance detector (per-band + cross-band).
            //
            // The phase detectors above, gated on sig[0]=='C' instead of 'L'.  A
            // pivot-side code bias (DCB drift, band/attribute ATX miscalibration,
            // SV hardware glitch) puts the same offset on every SD code row that
            // uses that pivot.  Without a coherence test the per-row dispatch
            // peels the rows one at a time, the uncombined iono state absorbs the
            // bias through repeated updates, and bad_residual_count climbs until
            // the rebuild budget runs out.  The signature is outliers on every SD
            // pair against one pivot for one code signal, growing each epoch.
            //
            // Differences from the phase detector:
            //   - MEDIAN_FLOOR_M_CODE = 0.30 m, against 5 mm for phase.  Routine
            //     code multipath reaches 0.1-0.2 m without any pivot problem;
            //     0.30 m is about 0.6× a typical code σ, above multipath and
            //     below a real disturbance.
            //   - No wavelength floor (cycles do not apply to code).
            //   - No temporal corroboration: the EWMA tracker is tuned for phase
            //     variance, and code would need its own tuning.
            //   - No non-pivot multi-band variant.  Without the integer-cycle
            //     check, coherent code outliers on a non-pivot are bad
            //     measurements that the per-row dispatch already handles.
            //
            // The action matches the phase detectors (bulk-exclude and request
            // a forced handover) because the bias persists: firing across
            // consecutive epochs, it contaminates the pivot's iono state, and
            // re-referencing is the only structural fix.
            //
            // Reuses Group, medianInPlace, row_atx_substituted, any_pivot_slip
            // and requested_pivot_handovers_ from the enclosing block.
            {
                constexpr size_t MIN_GROUP_CODE        = 4;
                constexpr size_t MIN_POOL_CODE         = 3;
                constexpr size_t MIN_DISTINCT_BANDS_C  = 2;
                constexpr double ABOVE_FRACTION_MIN_C  = 0.5;
                constexpr double MAD_RATIO_MAX_C       = 0.4;
                constexpr double MEDIAN_FLOOR_M_CODE   = 0.30;  // m

                std::map<std::pair<std::string, std::string>, Group> code_groups;
                std::map<std::string, Group> code_pivot_pools;
                std::map<std::string, std::set<std::string>> code_pivot_pool_bands;

                for (size_t i = 0; i < meta_n; ++i) {
                    const auto& sat_id = measurement.satellite_ids[i];
                    const auto& sig    = measurement.signal_types[i];
                    if (sig.size() < 2 || sig[0] != 'C') continue;
                    // Skip combined / iono-free pseudo-signals: their
                    // residual isn't a clean per-signal bias and the
                    // coherence test would mix scales.
                    if (sig.find('_') != std::string::npos) continue;
                    const auto [prim, pivot] = splitBssdSatId(sat_id);
                    if (pivot.empty() || prim == pivot) continue;  // not a non-pivot SD row
                    if (row_atx_substituted(prim, pivot, sig)) continue;  // ATX-substituted: bias mimics disturbance
                    const std::string band = sig.substr(0, 2);  // "C5", "C7", "C1", ...: the cross-band gate axis only
                    const double r  = res(static_cast<Eigen::Index>(i));
                    const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                    const bool   ab = std::isfinite(sa) && sa > threshold;

                    // Per-signal grouping (full RINEX code, e.g. "C1W"
                    // vs "C1C") matches the per-signal state granularity of
                    // uncombined PPP-RTK: satellite biases and ATX corrections
                    // are keyed per signal, so an attribute-specific pivot
                    // disturbance affects only the matching states and is
                    // detected at that resolution.
                    auto& g = code_groups[{ pivot, sig }];
                    g.indices.push_back(i);
                    g.residuals.push_back(r);
                    if (ab) ++g.above;

                    auto& pp = code_pivot_pools[pivot];
                    pp.indices.push_back(i);
                    pp.residuals.push_back(r);
                    if (ab) ++pp.above;
                    code_pivot_pool_bands[pivot].insert(band);  // band, not signal: the independence axis
                }

                // --- Per-band code pivot-disturbance detector ---
                // Runs regardless of whether a phase detector already
                // requested a handover for this constellation: the row
                // exclusion still matters because phase exclusion didn't
                // touch the code rows.  The set insert into
                // @c requested_pivot_handovers_ is idempotent.
                for (auto& [key, g] : code_groups) {
                    const size_t n = g.indices.size();
                    if (n < MIN_GROUP_CODE) continue;
                    const double frac = static_cast<double>(g.above) / static_cast<double>(n);
                    if (frac < ABOVE_FRACTION_MIN_C) continue;

                    std::vector<double> r_copy = g.residuals;
                    const double med = medianInPlace(r_copy);
                    if (std::abs(med) < MEDIAN_FLOOR_M_CODE) continue;

                    std::vector<double> abs_dev;
                    abs_dev.reserve(g.residuals.size());
                    for (double v : g.residuals) abs_dev.push_back(std::abs(v - med));
                    const double mad = medianInPlace(abs_dev);
                    const double mad_ratio = mad / std::abs(med);
                    if (mad_ratio > MAD_RATIO_MAX_C) continue;

                    const std::string& pivot_sat = key.first;
                    const std::string  cons      = PppUtils::getSystemString(pivot_sat);

                    klogger->warn(
                        "{}PIVOT CODE DISTURBANCE on {} {} {} signal: {}/{} SD code "
                        "residuals coherent (median={:+.4f} m, MAD={:.4f} m, "
                        "MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                        "Requesting forced pivot handover.",
                        epoch_prefix, cons, pivot_sat, key.second /* signal */,
                        g.above, n, med, mad, mad_ratio, frac);

                    requested_pivot_handovers_.insert(cons);
                    recordHandoverExcludes(g, med, mad, pivot_sat, cons);
                    recordIonoReinflate(g, cons);
                    for (size_t row_idx : g.indices) {
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig_t  = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        measurement.excluded_measurements.insert(prim + "_" + sig_t);
                    }
                    any_pivot_slip = true;
                }

                // --- Cross-band code pivot-disturbance detector ---
                // Skip when a handover is already queued for the
                // constellation: the cross-band pool's row set is a
                // superset of any per-band group already excluded,
                // and the cross-band log line would otherwise duplicate
                // the per-band warning for the same physical event.
                for (auto& [pivot_sat, pp] : code_pivot_pools) {
                    const std::string cons = PppUtils::getSystemString(pivot_sat);
                    if (requested_pivot_handovers_.count(cons)) continue;

                    const size_t n = pp.indices.size();
                    if (n < MIN_POOL_CODE) continue;

                    const auto& bands_set = code_pivot_pool_bands[pivot_sat];
                    if (bands_set.size() < MIN_DISTINCT_BANDS_C) continue;

                    const double frac = static_cast<double>(pp.above) / static_cast<double>(n);
                    if (frac < ABOVE_FRACTION_MIN_C) continue;

                    std::vector<double> r_copy = pp.residuals;
                    const double med = medianInPlace(r_copy);
                    if (std::abs(med) < MEDIAN_FLOOR_M_CODE) continue;

                    std::vector<double> abs_dev;
                    abs_dev.reserve(pp.residuals.size());
                    for (double v : pp.residuals) abs_dev.push_back(std::abs(v - med));
                    const double mad = medianInPlace(abs_dev);
                    const double mad_ratio = mad / std::abs(med);
                    if (mad_ratio > MAD_RATIO_MAX_C) continue;

                    std::string bands_str;
                    for (const auto& b : bands_set) {
                        if (!bands_str.empty()) bands_str += ",";
                        bands_str += b;
                    }

                    klogger->warn(
                        "{}PIVOT CODE DISTURBANCE [cross-band] on {} {} ({} bands [{}]): "
                        "{}/{} SD code residuals coherent (median={:+.4f} m, MAD={:.4f} m, "
                        "MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                        "Requesting forced pivot handover.",
                        epoch_prefix, cons, pivot_sat, bands_set.size(), bands_str,
                        pp.above, n, med, mad, mad_ratio, frac);

                    requested_pivot_handovers_.insert(cons);
                    recordHandoverExcludes(pp, med, mad, pivot_sat, cons);
                    recordIonoReinflate(pp, cons);
                    for (size_t row_idx : pp.indices) {
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig_t  = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        measurement.excluded_measurements.insert(prim + "_" + sig_t);
                    }
                    any_pivot_slip = true;
                }
            }

            // Non-pivot multi-band cycle-slip detector.
            //
            // The cross-band pivot detector with its coherence axes rotated 90°:
            // one non-pivot satellite slipping on several bands at once gives
            // one satellite × many bands of same-sign anomalous residuals
            // against a clean pivot.  A real slip jumps the ambiguity by N
            // cycles on every band, so the SD residual is N·λ_band:
            // band-dependent in metres, band-independent in cycles.  That
            // integer-cycle consistency is the strongest discriminator from
            // atmospheric and multipath events, which scale with 1/f² in metres
            // (iono) or stay roughly constant in metres (tropo, multipath).
            //
            // Valuable as a GLONASS backstop, where the FDMA inter-channel WL
            // bias inflates the MW detector's MAD floor and costs sensitivity.
            //
            // On detection the satellite's ambiguities are marked for re-seed
            // and its rows are excluded from this pass.  No pivot handover is
            // requested, since the pivot is unaffected.
            {
                constexpr size_t MIN_NONPIVOT_BANDS = 2;
                constexpr double MIN_RESIDUAL_M     = 0.05;  // ~quarter L1 cycle
                constexpr double INT_CYCLE_TOL      = 0.30;
                constexpr int    MAX_SLIP_CYCLES    = 50;    // sanity cap

                struct NpvEntry {
                    std::vector<size_t> indices;
                    std::vector<double> residuals;
                    std::set<std::string> bands;
                    size_t above = 0;
                    int    sign = 0;     // 0=mixed/empty, ±1 = consistent
                };
                std::map<std::string, NpvEntry> nonpivot_pools;

                for (size_t i = 0; i < meta_n; ++i) {
                    const auto& sat_id = measurement.satellite_ids[i];
                    const auto& sig    = measurement.signal_types[i];
                    if (sig.size() < 2 || sig[0] != 'L') continue;
                    const auto [prim, pivot] = splitBssdSatId(sat_id);
                    if (pivot.empty() || prim == pivot) continue;

                    const double r  = res(static_cast<Eigen::Index>(i));
                    const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                    const bool   ab = std::isfinite(sa) && sa > threshold;

                    auto& e = nonpivot_pools[prim];
                    e.indices.push_back(i);
                    e.residuals.push_back(r);
                    e.bands.insert(sig.substr(0, 2));
                    if (ab) {
                        ++e.above;
                        // Sign tracking only over anomalous rows: noise-driven
                        // sign flips on quiet rows shouldn't disqualify the sat.
                        const int rs = (r > 0.0) ? +1 : (r < 0.0 ? -1 : 0);
                        if (e.sign == 0 && rs != 0)        e.sign = rs;
                        else if (rs != 0 && rs != e.sign)  e.sign = 0;  // mixed
                    }
                }

                // Local PppUtils for wavelength lookup.  Default GLONASS slot
                // table is used (accurate to <0.5 MHz, fine for the 0.3-cycle
                // tolerance in the integer check below).
                PppUtils ppp_utils_local(log_manager_);

                auto satToSystem = [](const std::string& sat) -> System {
                    if (sat.empty()) return System::Unknown;
                    switch (sat[0]) {
                    case 'G': return System::GPS;
                    case 'E': return System::Galileo;
                    case 'R': return System::GLONASS;
                    case 'C': return System::BeiDou;
                    case 'J': return System::QZSS;
                    case 'S': return System::SBAS;
                    default:  return System::Unknown;
                    }
                };

                const double amb_init_std = config_.getInitialStd().ambiguity;

                for (auto& [sat, e] : nonpivot_pools) {
                    if (e.bands.size() < MIN_NONPIVOT_BANDS) continue;
                    if (e.above       < MIN_NONPIVOT_BANDS) continue;
                    if (e.sign == 0) continue;  // mixed signs → not a slip

                    std::vector<double> abs_residuals;
                    abs_residuals.reserve(e.residuals.size());
                    for (double r : e.residuals) abs_residuals.push_back(std::abs(r));
                    const double med_abs = medianInPlace(abs_residuals);
                    if (med_abs < MIN_RESIDUAL_M) continue;

                    // Integer-cycle consistency: each row's |residual| / λ_band
                    // must round to the same non-zero integer with tolerance
                    // INT_CYCLE_TOL.  This is what distinguishes a real cycle
                    // slip from atmospheric / multipath events.
                    const System sys = satToSystem(sat);
                    int   common_cycles    = 0;
                    bool  first            = true;
                    bool  integer_consistent = true;
                    for (size_t k = 0; k < e.indices.size(); ++k) {
                        const auto& sig_t = measurement.signal_types[e.indices[k]];
                        const double lam = ppp_utils_local.getWavelength(sys, sig_t, sat);
                        if (!(std::isfinite(lam) && lam > 0.0)) {
                            integer_consistent = false; break;
                        }
                        const double cyc = std::abs(e.residuals[k]) / lam;
                        const int    ric = static_cast<int>(std::lround(cyc));
                        if (ric == 0 || ric > MAX_SLIP_CYCLES
                         || std::abs(cyc - static_cast<double>(ric)) > INT_CYCLE_TOL) {
                            integer_consistent = false; break;
                        }
                        if (first) { common_cycles = ric; first = false; }
                        else if (ric != common_cycles) {
                            integer_consistent = false; break;
                        }
                    }
                    if (!integer_consistent) continue;

                    std::string bands_str;
                    for (const auto& b : e.bands) {
                        if (!bands_str.empty()) bands_str += ",";
                        bands_str += b;
                    }
                    const std::string cons = PppUtils::getSystemString(sat);

                    klogger->warn(
                        "{}NON-PIVOT SLIP on {} {} ({} bands [{}], sign={:+d}, "
                        "N={} cycles, |median res|={:.4f} m, {}/{} rows above threshold). "
                        "Resetting ambiguities for this satellite.",
                        epoch_prefix, cons, sat, e.bands.size(), bands_str,
                        e.sign, common_cycles, med_abs, e.above, e.indices.size());

                    // Single-sat reset: mark each ambiguity for re-seed and
                    // wipe its row/col cross-covariance with diagonal inflated
                    // to the initial-ambiguity variance.  ensureRequiredStates
                    // on the next rebuild pass will compute a fresh SD seed
                    // against the (unchanged) pivot.
                    for (size_t row_idx : e.indices) {
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig_t  = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        const std::string amb_key = prim + "_" + sig_t;
                        measurement.excluded_measurements.insert(amb_key);

                        const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                        if (amb_idx >= 0 && amb_idx < static_cast<int>(P.rows())) {
                            const auto ai = static_cast<Eigen::Index>(amb_idx);
                            P.row(ai).setZero();
                            P.col(ai).setZero();
                            P(ai, ai) = amb_init_std * amb_init_std;
                        }
                        state_manager_.markAmbiguityPendingReseed(amb_key);
                    }
                    // Record at satellite granularity only.  Every phase
                    // ambiguity of the satellite was reset, which is the
                    // satellite-wide signature reinflateResetSatStates keys on,
                    // and it makes the callers' arc-clearing loops drop the whole
                    // MW / TurboEdit history, all of which a multi-band
                    // common-integer slip invalidates.  Without this entry the
                    // reset is invisible to arc clearing, re-inflation and
                    // smoother segmentation.
                    last_reset_satellites_.push_back(sat);
                    any_pivot_slip = true;
                }
            }

            if (any_pivot_slip) {
                ++measurement.bad_residual_count;
                klogger->debug("{}Pivot slip handled. Measurement rebuild required.", epoch_prefix);
                klogger->debug("{}Post-fit residual check result: outlier_found={}, complete_reset={}",
                    epoch_prefix, "true", "false");
                return { /*outlier_found=*/true, /*complete_reset=*/false };
            }
        }

        // --- Huber R-inflation pass (post-fit) ----------------------------
        // For sub-cycle phase rows whose post-fit |sres| exceeds the Huber
        // threshold, queue a target R(i,i) so the next rebuild pass downweights
        // the row until |sres| lands at that threshold, and set
        // outlier_found=true to trigger that pass.  Rows with |res| ≥ cyc_floor·λ
        // are cycle-slip territory and go to the reset dispatch instead.
        {
            const double huber_k     = config_.getPhaseHuberSresThreshold();
            const double cyc_floor   = config_.getPivotSlipMinCycles();
            const bool   use_innov   = config_.enableHuberInnovation();
            const bool   huber_on    = config_.enableHuberRInflation();
            // Rebuild-loop exit tolerance.  The post-fit IRLS fixed point
            // is |sres| = k exactly, so a bare "|sres| <= k" test would
            // chase an asymptote; the gate below requires a pass's
            // relative R step to clear this tolerance before re-queuing.
            // The innovation scheme lands on k in one shot, so there the
            // gate only blocks a no-op floating-point re-trigger.
            constexpr double kHuberRConvergenceTol = 0.05;
            bool huber_triggered     = false;
            if (huber_on && huber_k > 0.0) {
                for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(num_obs); ++i) {
                    const auto& sig = measurement.signal_types[static_cast<size_t>(i)];
                    if (sig.empty() || sig[0] != 'L') continue;
                    if (sig.find('_') != std::string::npos) continue;  // iono-free combos
                    if (sig.size() < 2) continue;

                    const auto& sat_id = measurement.satellite_ids[static_cast<size_t>(i)];
                    const auto [prim, _hub_piv] = splitBssdSatId(sat_id);
                    const std::string band_str = sig.substr(0, 2);
                    const double lambda = bandWavelengthForSat(prim, band_str);
                    if (!(lambda > 0.0)) continue;

                    // Two R-inflation schemes, selected by
                    // estimation.enable_huber_innovation:
                    //
                    //  - innovation one-shot: |s| = |v|/sqrt(HPHᵀ + R) is monotone
                    //    in R, so the R landing |s| = k is closed-form,
                    //    R_target = v²/k² − HPHᵀ, with HPHᵀ = (H·P⁻·Hᵀ)(i,i) the
                    //    R-independent prior variance in that row.  One pass.
                    //
                    //  - post-fit IRLS: |sres| = |res|/sqrt(R), inflated by
                    //    (|sres|/k)² per pass.  The post-fit residual drifts up as
                    //    the row loses weight, so this converges to k only
                    //    asymptotically and can overshoot first.
                    double abs_res  = 0.0;
                    double abs_sres = 0.0;
                    double r_target = 0.0;
                    if (use_innov) {
                        const double v = measurement.innovations(i);
                        abs_res = std::abs(v);
                        if (cyc_floor > 0.0 && abs_res >= cyc_floor * lambda) continue;
                        const double hph =
                            (measurement.H.row(i) * P).dot(measurement.H.row(i));
                        abs_sres = abs_res / std::sqrt(hph + R_(i, i));
                        if (abs_sres <= huber_k) continue;
                        r_target = (v * v) / (huber_k * huber_k) - hph;
                    } else {
                        abs_res = std::abs(res(i));
                        if (cyc_floor > 0.0 && abs_res >= cyc_floor * lambda) continue;
                        abs_sres = std::abs(sres(i));
                        if (abs_sres <= huber_k) continue;
                        const double factor = (abs_sres / huber_k) * (abs_sres / huber_k);
                        r_target = R_(i, i) * factor;
                    }

                    const std::string key = prim + "|" + sig;
                    auto it = pending_r_inflations_.find(key);
                    const double r_existing = (it != pending_r_inflations_.end())
                                            ? it->second : R_(i, i);
                    if (r_target > r_existing * (1.0 + kHuberRConvergenceTol)) {
                        pending_r_inflations_[key] = r_target;
                        huber_triggered = true;
                        if (use_innov) {
                            klogger->info(
                                "{}Huber R-inflation (innovation one-shot) for {} {}: "
                                "|v|/sqrt(S)={:.2f} > k={:.1f}, |innov|={:.4f} m "
                                "(sub-cycle, λ={:.4f} m); R(i,i): {:.3e} → {:.3e} m²",
                                epoch_prefix, prim, sig, abs_sres, huber_k,
                                abs_res, lambda, R_(i, i), r_target);
                        } else {
                            klogger->info(
                                "{}Huber R-inflation queued for {} {}: |sres|={:.2f} > k={:.1f}, "
                                "|res|={:.4f} m (sub-cycle, λ={:.4f} m); R(i,i): {:.3e} → {:.3e} m²",
                                epoch_prefix, prim, sig, abs_sres, huber_k,
                                abs_res, lambda, R_(i, i), r_target);
                        }
                    }
                }
            }

            if (huber_triggered) {
                klogger->debug(
                    "{}Huber R-inflation pass requesting measurement rebuild "
                    "({} rows queued)",
                    epoch_prefix, pending_r_inflations_.size());
                klogger->debug("{}Post-fit residual check result: outlier_found={}, complete_reset={}",
                    epoch_prefix, "true", "false");
                return { /*outlier_found=*/true, /*complete_reset=*/false };
            }
        }

        // --- Baarda data-snooping gate override ---------------------------
        // By default the gate fires on the fixed σ threshold (gate_* ≡ max_sres /
        // max_idx / threshold), so the dispatch below is byte-identical with
        // data-snooping off.
        //
        // When enabled, it fires on |wᵢ| > dataSnoopingCritical() over checkable
        // rows (redundancy ≥ kUncheckableRedundancy) and picks the worst such
        // row.  Low-redundancy rows are uncheckable: a blunder there is absorbed
        // into the estimate, so resetting its ambiguity would discard good
        // information.  The MW tiebreaker still works in the σ domain; when the
        // w-test catches something the σ cut missed, the tiebreaker collects no
        // candidates and the w-selected row is handled directly.
        double gate_stat = max_sres;
        size_t gate_idx  = max_idx;
        double gate_crit = threshold;
        if (config_.enableDataSnooping() &&
            measurement.residual_wtest.size() == sres.size() &&
            measurement.residual_redundancy.size() == sres.size()) {
            constexpr double kUncheckableRedundancy = 0.1;
            double max_w = -1.0;
            size_t max_w_idx = max_idx;
            for (Eigen::Index i = 0; i < measurement.residual_wtest.size(); ++i) {
                if (measurement.residual_redundancy(i) < kUncheckableRedundancy) continue;
                const double w_i = std::abs(measurement.residual_wtest(i));
                if (std::isfinite(w_i) && w_i > max_w) {
                    max_w = w_i;
                    max_w_idx = static_cast<size_t>(i);
                }
            }
            if (max_w >= 0.0) {
                gate_stat = max_w;
                gate_idx  = max_w_idx;
                gate_crit = config_.dataSnoopingCritical();
            }
        }

        // --- Dispatch outlier handling ---
        bool is_outlier_found = false;
        bool is_complete_reset = false;

        if (gate_stat > gate_crit) {

            // (The per-row residual table for outlier diagnosis is
            // logged by the level-dependent block above: at debug
            // level it is emitted when max_sres > threshold, at
            // info and warn it is not.  Setting the kalman_filter
            // level to debug shows the per-row context of an
            // outlier event.)

            // --- MW tiebreaker for competing phase outliers ---
            // With several satellites in one constellation at near-equal |sres|
            // the raw maximum is an unreliable pick, so MW deviation scores
            // choose the one most likely to have slipped.
            //
            // chosen_idx starts at gate_idx (≡ max_idx unless data-snooping
            // reselected via the w-test).  The tiebreaker refines only among rows
            // the σ cut also flags, so a w-test-only hit keeps gate_idx.
            size_t chosen_idx = gate_idx;
            if (!measurement.signal_types[max_idx].empty()
                && measurement.signal_types[max_idx][0] == 'L'
                && cycle_slip_manager_) {

                // Ratio threshold: a competitor is "close" if its |sres|
                // is within 5% of the maximum.
                constexpr double kRatioThresh = 0.95;

                const auto [max_prim, max_ref] = splitBssdSatId(
                    measurement.satellite_ids[max_idx]);
                const std::string max_cons = PppUtils::getSystemString(max_prim);

                // Collect all phase-outlier indices for the same constellation
                // whose |sres| is close to the maximum.
                struct Candidate {
                    size_t idx;
                    std::string sat;   // primary satellite
                    double sres_abs;
                    double mw_score;
                };
                std::vector<Candidate> candidates;

                for (size_t i = 0; i < meta_n; ++i) {
                    const auto& sig = measurement.signal_types[i];
                    if (sig.empty() || sig[0] != 'L') continue;
                    const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                    if (!std::isfinite(sa) || sa <= threshold) continue;
                    if (sa < max_sres * kRatioThresh) continue;

                    const auto [prim, ref] = splitBssdSatId(
                        measurement.satellite_ids[i]);
                    if (PppUtils::getSystemString(prim) != max_cons) continue;

                    // Deduplicate by satellite: one entry per satellite, the
                    // first qualifying row in row order.
                    bool found = false;
                    for (auto& c : candidates) {
                        if (c.sat == prim) { found = true; break; }
                    }
                    if (!found) {
                        candidates.push_back({i, prim, sa, 0.0});
                    }
                }

                if (candidates.size() > 1) {
                    // Query MW scores (skip if the MW detector is disabled).
                    if (const auto* mw = cycle_slip_manager_->mwDetector()) {
                        for (auto& c : candidates)
                            c.mw_score = mw->getWorstMwScore(c.sat);
                    }

                    // Pick the candidate with the highest MW score.
                    auto best = std::max_element(candidates.begin(),
                        candidates.end(), [](const Candidate& a, const Candidate& b) {
                            return a.mw_score < b.mw_score;
                        });

                    if (best != candidates.end() && best->mw_score > 0.0
                        && best->idx != max_idx) {
                        klogger->debug(
                            "{}MW tiebreaker: {} candidates within {:.0f}% of max "
                            "|sres|={:.3f}. Choosing {} (MW score={:.2f}) over {} "
                            "(MW score={:.2f})",
                            epoch_prefix, candidates.size(),
                            (1.0 - kRatioThresh) * 100.0, max_sres,
                            best->sat, best->mw_score,
                            candidates[0].sat == best->sat
                                ? candidates[1].sat : candidates[0].sat,
                            candidates[0].sat == best->sat
                                ? candidates[1].mw_score : candidates[0].mw_score);
                        chosen_idx = best->idx;
                    }
                }
            }

            const std::string outlier_sat = measurement.satellite_ids[chosen_idx];
            const std::string outlier_sig = measurement.signal_types[chosen_idx];
            // Capture residual values by value before dispatch: handleCodeOutlier
            // shifts measurement.residuals* to drop the offending row, after which
            // sres/res(chosen_idx) refer to the row that sat at chosen_idx+1.
            const double outlier_sres     = sres(static_cast<Eigen::Index>(chosen_idx));
            const double outlier_sres_abs = std::abs(outlier_sres);
            const double outlier_res      = res(static_cast<Eigen::Index>(chosen_idx));
            const double sigma_max = std::sqrt(std::max(
                vres(static_cast<Eigen::Index>(chosen_idx),
                     static_cast<Eigen::Index>(chosen_idx)), 0.0));

            klogger->warn(
                "{}Max standardized residual for {} {}: |s|={:.3f} (s={:+.3f}, residual {:.3f} m, sigma {:.3f} m, idx {})",
                epoch_prefix, outlier_sat, outlier_sig,
                outlier_sres_abs, outlier_sres, outlier_res, sigma_max, chosen_idx);

            if (!outlier_sig.empty() && outlier_sig[0] == 'L') {
                // λ-fraction floor on the raw phase residual (with sub-cycle slip
                // suppression enabled).  A real slip moves the observation by an
                // integer number of wavelengths, so anything below
                // cyc_floor_fraction × λ cannot be one, and an ambiguity reset
                // (variance inflation, loss of converged precision) is the wrong
                // response.  The row is still excluded from this rebuild: its
                // sub-cycle bias may be real (multipath, PCV miscalibration,
                // kinematic dynamics) and would pull position and clock if absorbed.
                //
                // Not applied to iono-free combinations ("L1C_L2W"-style): the
                // combined residual is not a clean multiple of any single λ, so
                // those rows still go through the reset.
                const double cyc_floor_fraction = config_.getPivotSlipMinCycles();
                const bool   subcycle_on        = config_.enableSubcycleSlipSuppression();
                const bool is_iono_free = (outlier_sig.find('_') != std::string::npos);
                bool suppressed_by_floor = false;
                if (subcycle_on && cyc_floor_fraction > 0.0 && !is_iono_free && outlier_sig.size() >= 2) {
                    const auto [prim_for_lambda, _] = splitBssdSatId(outlier_sat);
                    const std::string band_str = outlier_sig.substr(0, 2);
                    const double lambda = bandWavelengthForSat(prim_for_lambda, band_str);
                    if (lambda > 0.0
                        && std::abs(outlier_res) < cyc_floor_fraction * lambda) {
                        klogger->info(
                            "{}Phase outlier suppressed by λ/{:.2f} floor on {} {}: "
                            "|res|={:.4f} m < floor={:.4f} m (λ={:.4f} m); "
                            "excluding row, ambiguity state preserved",
                            epoch_prefix, 1.0 / cyc_floor_fraction,
                            outlier_sat, outlier_sig,
                            std::abs(outlier_res),
                            cyc_floor_fraction * lambda, lambda);
                        suppressed_by_floor = true;
                    }
                }

                const auto [prim, ref] = splitBssdSatId(outlier_sat);
                if (!suppressed_by_floor) {
                    handlePhaseOutlier(measurement, outlier_sat, outlier_sig, x, P, epoch_prefix);
                }
                measurement.excluded_measurements.insert(prim + "_" + outlier_sig);

                // Check for correlated phase outliers on other frequencies
                // for the same satellite.  When a cycle slip affects an SD
                // pair, all phase signals for that satellite typically show
                // outliers with nearly equal |sres| and opposite signs.
                // Resetting only one frequency leaves the others wrong and
                // costs extra rebuild passes.
                //
                // The λ-floor applies here too: sub-cycle correlated
                // residuals are bias, not slip, on every band.
                for (size_t i = 0; i < meta_n; ++i) {
                    if (i == chosen_idx) continue;
                    const auto& co_sig = measurement.signal_types[i];
                    if (co_sig.empty() || co_sig[0] != 'L') continue;
                    if (measurement.satellite_ids[i] != outlier_sat) continue;
                    const double co_abs = std::abs(sres(static_cast<Eigen::Index>(i)));
                    if (!std::isfinite(co_abs) || co_abs <= threshold) continue;

                    const double co_res = res(static_cast<Eigen::Index>(i));
                    bool co_suppressed = false;
                    if (subcycle_on && cyc_floor_fraction > 0.0
                        && co_sig.find('_') == std::string::npos
                        && co_sig.size() >= 2) {
                        const std::string co_band = co_sig.substr(0, 2);
                        const double co_lambda = bandWavelengthForSat(prim, co_band);
                        if (co_lambda > 0.0
                            && std::abs(co_res) < cyc_floor_fraction * co_lambda) {
                            klogger->info(
                                "{}Correlated phase outlier suppressed by λ/{:.2f} floor "
                                "on {} {}: |res|={:.4f} m < floor={:.4f} m",
                                epoch_prefix, 1.0 / cyc_floor_fraction,
                                outlier_sat, co_sig, std::abs(co_res),
                                cyc_floor_fraction * co_lambda);
                            co_suppressed = true;
                        }
                    }

                    klogger->debug(
                        "{}Correlated phase outlier on same satellite: {} {} "
                        "|s|={:.3f}, {} alongside {} {}",
                        epoch_prefix, outlier_sat, co_sig, co_abs,
                        co_suppressed ? "excluding (floor)" : "resetting",
                        outlier_sat, outlier_sig);

                    if (!co_suppressed) {
                        handlePhaseOutlier(measurement, outlier_sat, co_sig, x, P, epoch_prefix);
                    }
                    measurement.excluded_measurements.insert(prim + "_" + co_sig);
                }
            } else {
                handleCodeOutlier(measurement, outlier_sat, outlier_sig, chosen_idx, epoch_prefix);
            }

            is_outlier_found = true;
            measurement.bad_residual_count++;

            klogger->warn("{}Outlier flagged: idx={}, sat={}, sig={}, max_sres={:.3f} > threshold={:.3f}; bad_residual_count={}",
                epoch_prefix, chosen_idx, outlier_sat, outlier_sig,
                outlier_sres_abs,
                threshold, measurement.bad_residual_count);
        } else {
            klogger->debug("{}No outliers: max_sres={:.3f} <= threshold={:.3f}",
                epoch_prefix, max_sres, threshold);
        }

        // --- Complete reset if too many outliers accumulated ---
        // The denominator is the original measurement count before peeling (see
        // total_obs_count above), so the backstop fires at the configured
        // bad-measurement fraction rather than aborting the peel early.
        if (measurement.bad_residual_count > static_cast<size_t>(bad_meas_percent * total_obs_count)) {
            klogger->warn("{}Resetting filter: too many outliers ({}/{}), limit={:.3f}*n={}",
                epoch_prefix, measurement.bad_residual_count, total_obs_count,
                bad_meas_percent, static_cast<size_t>(bad_meas_percent * total_obs_count));

            resetFilterCovariance(P, epoch_prefix);

            is_outlier_found = true;
            is_complete_reset = true;
            measurement.bad_residual_count = 0;
        }

        klogger->debug("{}Post-fit residual check result: outlier_found={}, complete_reset={}",
            epoch_prefix,
            is_outlier_found ? "true" : "false",
            is_complete_reset ? "true" : "false");

        // Feed accepted residuals into the residual-history tracker, on
        // the clean-commit path only (no outlier detected, no complete
        // reset).  Excluded measurements are skipped because their
        // residuals were not applied to the state, so the EWMA
        // statistics describe only the rows that updated the state.
        if (residual_history_.isEnabled() && !is_outlier_found && !is_complete_reset) {
            for (size_t i = 0; i < meta_n; ++i) {
                const auto& sat_id = measurement.satellite_ids[i];
                const auto& sig    = measurement.signal_types[i];
                if (sat_id.empty() || sig.empty()) continue;
                const auto [prim, _piv] = splitBssdSatId(sat_id);
                const std::string excl_key = prim + "_" + sig;
                if (measurement.excluded_measurements.count(excl_key)) continue;
                const double r = res(static_cast<Eigen::Index>(i));
                if (!std::isfinite(r)) continue;
                residual_history_.record(prim, sig, r);
            }

            // --- Stuck-code-bias scan ----------------------------------
            // Queue resets (applied after the commit) for sat-bias states that
            // are either outlier-rejected for too many consecutive epochs (the
            // closed loop in which an over-converged bias keeps tripping the
            // gate on its own row) or whose post-fit EWMA mean has drifted past
            // the bias floor (the slow-drift case, in which the row survives but
            // the converged bias has moved away from the true value).
            //
            // BSSD pivots are skipped: PivotManager freezes their sat-bias at
            // (value 0, variance 0), and resetting it would break the SD frame.
            // A genuine pivot bias shows up as a coherent shift across the group
            // and belongs to the spatial pivot detectors instead.
            const auto& stuck_cfg = config_.getStuckCodeBiasConfig();
            if (stuck_cfg.enabled && config_.estimateSatBiases()) {
                const auto& pivots = state_manager_.getFrozenPivotSatellites();
                for (const auto& [key, _idx] : state_manager_.getSatBiasStates()) {
                    const auto bar = key.find('|');
                    if (bar == std::string::npos) continue;
                    const std::string sat = key.substr(0, bar);
                    const std::string sig = key.substr(bar + 1);

                    if (pivots.count(sat)) continue;  // frozen pivot

                    const auto ev = residual_history_.evaluateStuckBias(
                        sat, sig,
                        stuck_cfg.bias_floor_m,
                        stuck_cfg.min_streak);
                    if (!ev.stuck) continue;

                    // Queue (key, EWMA mean), applied post-commit in update().
                    // The EWMA mean is the sign-consistent residual offset, a much
                    // better re-seed than inflating variance and waiting for
                    // innovations to pull the bias back (many epochs with a
                    // converged P).  On the streak-only trigger (warmed_up false)
                    // the mean is 0 and only variance inflates, so the filter
                    // absorbs the next surviving innovation freely.
                    pending_sat_bias_resets_[key] = ev.warmed_up ? ev.ewma_mean : 0.0;

                    klogger->warn(
                        "{}Stuck code-bias detected on {} {}: "
                        "streak={}, EWMA mean={:+.4f} m (std={:.4f}, warmed_up={}); "
                        "queued reset (value+={:+.4f} m, variance -> cold start)",
                        epoch_prefix, sat, sig,
                        ev.streak, ev.ewma_mean, ev.ewma_std,
                        ev.warmed_up ? "yes" : "no",
                        ev.warmed_up ? ev.ewma_mean : 0.0);
                }
            }

            // Emit the per-epoch residual-history digest at info
            // level alongside the per-pass "Residual summary" line
            // above: how many rows are tracked and warmed up, and the
            // median and largest σ across rows, without per-row trace
            // logs.
            const auto rh_summary = residual_history_.summarize();
            if (rh_summary.entry_count > 0) {
                klogger->info(
                    "{}ResidualHistory: tracking {} (sat,sig) entries "
                    "({} warmed up); σ across rows: median={:.4f} m, max={:.4f} m",
                    epoch_prefix, rh_summary.entry_count, rh_summary.warmed_up,
                    rh_summary.median_std, rh_summary.max_std);
            }
        }

        return { is_outlier_found, is_complete_reset };
    }

    std::set<std::string> KalmanFilter::takeRequestedPivotHandovers() {
        std::set<std::string> out;
        out.swap(requested_pivot_handovers_);
        return out;
    }

    std::map<std::string, std::set<std::string>>
    KalmanFilter::takeRequestedPivotHandoverExcludeSats() {
        std::map<std::string, std::set<std::string>> out;
        out.swap(pivot_handover_exclude_sats_);
        return out;
    }

    std::map<std::string, std::set<std::string>>
    KalmanFilter::takeRequestedPivotHandoverIonoReinflateSats() {
        std::map<std::string, std::set<std::string>> out;
        out.swap(pivot_handover_reinflate_sats_);
        return out;
    }

    void KalmanFilter::beginEpoch() {
        // Reset the per-epoch pass counter and put the gate in passthrough
        // so pass 1 emits live. No-op when concise mode is disabled
        // (gate_ stays null and pass_index_ is unused outside update()).
        pass_index_ = 0;
        if (gate_) {
            gate_->setPolicy(GatedSink::Policy::Passthrough);
            gate_->clearBuffer();
        }

        // Clear pending Huber R-inflations: each epoch makes a fresh
        // decision from its own post-fit residuals; long-term persistent
        // biases are covered by the separate EWMA-based inflation.
        pending_r_inflations_.clear();

        // Defensive clear of stuck-code-bias resets.  In normal flow
        // the queue is drained inside update() the same epoch it was
        // populated, but a non-converging outlier loop or an early
        // return could leave entries behind; clearing here makes
        // each epoch start from a known-empty state.
        pending_sat_bias_resets_.clear();
    }

    void KalmanFilter::endEpoch() {
        // The buffer at this point holds the cumulative output of every
        // post-pass-1 update(): each pass's "Residual summary" line plus,
        // where applicable, that pass's outlier-handling block (Max sres,
        // Phase outlier action, Reinitialized ambiguity, Outlier flagged,
        // etc.).  Replaying it shows the intermediate outlier detail and
        // the final pass's residual check, which reports no outliers once
        // the rebuilds have converged.
        // Restore passthrough so any post-loop logging from the caller
        // (position summary, KF timing) emits live as usual.
        if (gate_) {
            gate_->replayBuffer();
            gate_->clearBuffer();
            gate_->setPolicy(GatedSink::Policy::Passthrough);
        }
    }

    void KalmanFilter::logKalmanFilterTimings() {
        // Route the end-of-run summary to main_log so it lands next to the
        // "Processing completed in N ms" line, where it is easy to find when
        // comparing timings between runs.  The per-epoch debug line in
        // update() goes to kalman_filter.log for detailed inspection.
        auto logger = log_manager_.getLogger("main_log");
        if (timing_n_calls_ == 0) {
            logger->info("KF timing summary: no update() calls recorded.");
            return;
        }
        const double n = static_cast<double>(timing_n_calls_);
        const double S_us  = static_cast<double>(timing_us_S_)  / n;
        const double K_us  = static_cast<double>(timing_us_K_)  / n;
        const double P_us  = static_cast<double>(timing_us_P_)  / n;
        const double Sr_us = static_cast<double>(timing_us_Sr_) / n;
        const double total_us = S_us + K_us + P_us + Sr_us;
        logger->info(
            "KF timing summary over {} update() calls (mean us / call):\n"
            "  S form  (H*P + S=HP*Ht+R) : {:8.1f}\n"
            "  K solve (LDLT + solve)    : {:8.1f}\n"
            "  P Joseph (I-KH P (I-KH)') : {:8.1f}\n"
            "  Sr diag (R - H*P_post*Ht) : {:8.1f}\n"
            "  ---------------------------------\n"
            "  total per call            : {:8.1f}\n"
            "  cumulative wall (s)       : {:8.3f}",
            timing_n_calls_, S_us, K_us, P_us, Sr_us, total_us,
            (total_us * timing_n_calls_) / 1.0e6);
        timing_us_S_  = 0;
        timing_us_K_  = 0;
        timing_us_P_  = 0;
        timing_us_Sr_ = 0;
        timing_n_calls_ = 0;
    }

    // ===================================================================
    // Pseudo-observation update
    // ===================================================================

    void KalmanFilter::josephPseudoUpdate(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                          const Eigen::MatrixXd& H, const Eigen::VectorXd& innovation,
                                          const Eigen::MatrixXd& R,
                                          Eigen::VectorXd& x_out, Eigen::MatrixXd& P_out) {
        const Eigen::Index m = H.rows();
        const Eigen::Index n = P.rows();
        const Eigen::MatrixXd PHt = P * H.transpose();
        const Eigen::MatrixXd S   = H * PHt + R;
        const Eigen::MatrixXd K   = PHt * S.ldlt().solve(Eigen::MatrixXd::Identity(m, m));
        const Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(n, n) - K * H;

        const Eigen::VectorXd x_new = x + K * innovation;
        Eigen::MatrixXd P_new = IKH * P * IKH.transpose() + K * R * K.transpose();
        P_new = (0.5 * (P_new + P_new.transpose())).eval();

        x_out = x_new;
        P_out = P_new;
    }

    void KalmanFilter::applyPseudoObservation(const Eigen::MatrixXd& H,
                                              const Eigen::VectorXd& innovation,
                                              const Eigen::MatrixXd& R) {
        Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();
        josephPseudoUpdate(x, P, H, innovation, R, x, P);
        state_manager_.setStateVector(x);
        state_manager_.setCovariance(P);
    }

} // namespace GnssProcessor