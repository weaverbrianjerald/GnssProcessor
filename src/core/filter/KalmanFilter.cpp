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

#include "filter/KalmanFilter.hpp"
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

    // -------------------------------------------------------------------
    // Carrier wavelength lookup
    // -------------------------------------------------------------------
    //
    // Maps a (pivot/owning satellite, band prefix) pair to its carrier
    // wavelength.  Used by both pivot-slip detection and per-row phase
    // outlier handling to enforce a λ-fraction floor below which a
    // residual cannot represent an integer cycle slip.
    //
    // @param sat       Satellite identifier (e.g. "G11", "E33").  Constellation
    //                  is derived from the first character.
    // @param band_str  Two-character band prefix (e.g. "L1", "L2", "L5").
    //                  Only the second char (frequency digit) is used.
    // @return          Wavelength in meters, or 0.0 when the pair is unknown.
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
            else if (d == '2') freq = Constants::BDS_B1_FREQ;   // B1I — legacy band-2 RINEX code
            else if (d == '5') freq = Constants::BDS_B2A_FREQ;
            else if (d == '7') freq = Constants::BDS_B2_FREQ;   // B2b
            else if (d == '6') freq = Constants::BDS_B3_FREQ;
            else if (d == '8') freq = Constants::BDS_B2AB_FREQ;
            break;
        case 'R':  // GLONASS — FDMA center frequency is sufficient for λ/4 floor
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

    // -------------------------------------------------------------------
    // Sparse-H helpers
    // -------------------------------------------------------------------
    //
    // The measurement design matrix H is dense in storage (~m × n) but the
    // *content* is extremely sparse: each row has only ~5 non-zeros out of
    // ~350 columns (geometry block + ambiguity column + maybe tropo / iono).
    // Treating it as dense in `H·P`, `HP·Hᵀ`, and `K·H` wastes O(n²·m) ops
    // on zero entries.
    //
    // The helpers below build a per-row list of (column, value) pairs once
    // per epoch and then walk that list for each H-touching product.  The
    // result of each helper is the same dense matrix the dense matmul
    // would produce — the rest of the filter doesn't change shape.
    //
    // Threshold 1e-12 separates "real geometry partial" from FP noise
    // emitted by zero-initialized blocks.

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

        // Baseline mode: no clock/tropo/phase-clock/gradient/ISB/IMU states
        // in the core layout — skip seeding them, jump straight to ambiguities.
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

        // Concise-log gate: when enabled, install a GatedSink wrapper around
        // the kalman_filter logger's sinks so beginEpoch/endEpoch can route
        // intermediate-pass output away from disk without changing call sites.
        // The gate stays null when the flag is off — beginEpoch/endEpoch
        // become no-ops with no per-call cost.
        if (config_.getLoggingConfig().kalman_filter_concise_log) {
            gate_ = log_manager_.installGatedSink("kalman_filter");
            if (gate_) {
                log_manager_.getLogger("kalman_filter")->info(
                    "Concise log mode enabled: only first + final pass per epoch will be retained");
            }
        }

        const size_t state_size = state_manager_.getStateSize();
        // Baseline mode: state vector is position (3) + DD ambiguities only.
        // No clock / tropo / velocity / IMU / ISB states — skip the PPP
        // minimum-size check and the associated base_core_size computation.
        // BSSD mode: receiver clock is omitted (cancels in SD), so the base
        // drops from 5 (pos+clk+tropo) to 4 (pos+tropo) and the per-system
        // ISB states aren't allocated either.
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
            // A non-positive time step has no propagation: the transition is
            // identity and the process-noise increment is zero, so predict() is a
            // no-op.  Returning here — rather than calling getProcessNoiseMatrix /
            // getTransitionMatrix, which hard-throw on dt<=0 — keeps the public
            // KF API robust to duplicate / non-advancing epochs (e.g. a real-time
            // stream delivering the same TOW twice).  The engine already skips
            // such epochs upstream; this is the belt-and-suspenders backstop.
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

            // Freeze BSSD pivot ionosphere states (iono column zeroed in S-transform).
            // Under the constant-velocity model the companion rate is frozen too:
            // its F-coupling and Q are already suppressed for pivots, and
            // PivotManager holds it at (0,0); freezing its cross-covariances here
            // keeps stale correlations from leaking into the SD solution.
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
        // The IMU provides a 15x15 Phi and Q for the INS error states:
        //   [dp(3), dv(3), dpsi(3), dba(3), dbg(3)]
        // We map these to the full state vector indices.
        //
        // For non-INS states (clock, tropo, ambiguities, ISB, etc.) we use
        // the standard random-walk process noise (same as predict()).

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
        // Identity-seeded transition: non-owned (non-INS) state means are NOT
        // propagated, matching predictImu's F_full = I seed.
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

            // Inject the contribution's Phi/Q over the full-state indices it
            // owns — ASSIGNMENT, not accumulation (the same reason predictImu
            // uses = : avoid double-counting the baseline process noise on owned
            // rows).
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

        // Baseline Q over the contribution's integrated interval (dt_total) — as
        // predictImu does (NOT the GNSS dt): the non-owned rows keep their config
        // random-walk process noise scaled by the same interval the INS used.
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
        // epoch_tag is still consumed by the side-channel KalmanFilterLogger
        // (separate file, not spdlog-managed) so it stays formatted.
        // epoch_prefix is now empty: the spdlog %E field carries the same
        // info, and duplicating it in the message body looks cluttered.
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix;

        last_reset_satellites_.clear();
        last_reset_signals_.clear();
        was_complete_reset_ = false;

        // Concise-log gating: pass 1 emits live; passes ≥2 buffer into the
        // gate WITHOUT clearing — every intermediate pass's outlier-handling
        // lines and residual summary accumulate into one epoch-wide buffer.
        // endEpoch replays the whole buffer in order, so the user sees:
        //   pass 1 live   ▸   pass 2 outliers   ▸   pass 3 outliers   ▸
        //   …            ▸   final pass residual summary (no outliers)
        // beginEpoch clears the buffer between epochs, so accumulation
        // never bleeds across epoch boundaries.
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

        // Read once and treat as immutable pre-update values; the post-update
        // results are computed into x_new / P_new so we can hand pre-update
        // copies to the outlier validator without paying for an explicit
        // backup (~1 MB matrix copy at n=350).
        const Eigen::VectorXd x = state_manager_.getStateVector();
        const Eigen::MatrixXd P = state_manager_.getCovariance();

        // --- Persistent-bias R-inflation ---------------------------------
        // For phase rows whose (sat, signal) residual history shows a
        // warmed-up EWMA mean larger than persistent_bias_min_cycles · λ,
        // add @c mean² to the diagonal R entry.  This downweights the
        // row in proportion to how badly its historical mean has drifted
        // from zero, without resetting the ambiguity (the bias isn't a
        // cycle slip; the channel is just persistently biased by some
        // unmodeled effect: multipath, PCV miscal, inter-channel
        // hardware delay).  R-inflation lets the row keep contributing
        // *less* rather than dropping it entirely — automatic recovery
        // as the EWMA mean decays.
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
                // Require the row's EWMA history to be warmed up (the
                // tracker's own warmup threshold is internal; mirror its
                // public Config defaults here as a sane floor).
                if (st.n < 20) continue;

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
        // The Huber post-fit dispatch (see checkPostFitResiduals) detects
        // sub-cycle phase rows whose |sres_post| exceeds @c huber_k and
        // queues per-row R targets in @c pending_r_inflations_.  Applied
        // here at the start of every rebuild pass so the *next* update
        // runs against a downweighted R for those rows.  Cleared at
        // epoch boundary via @c beginEpoch / clearPendingRInflations.
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
        // Accumulators are static so they survive across update() calls;
        // they're dumped to the kalman_filter log at debug level on each
        // call (per-call values) and a cumulative summary is available via
        // logKalmanFilterTimings() at end of run.  std::chrono::steady_clock
        // is a few nanoseconds per now() — negligible vs the matmuls below.
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

        // Condition-number diagnostic is only consumed by a TRACE log line;
        // skip the JacobiSVD (O(m³) with a heavy constant) when not enabled.
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

        // --- State correction (into a new vector — pre-update x stays intact) ---
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

        // Sr = R - H·P_post·Hᵀ.  Only the diagonal is consumed (by the
        // outlier handler and the residuals trace); building the full m×m
        // matrix costs O(n²·m + n·m²), but per-row dot products give the
        // diagonals in O(nnz·m) total.  Off-diagonals stay zero — the
        // outlier-handler shrink loop is happy with that.
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

        // Post-fit residual covariance.  Default: the diagonal Sr only (the
        // outlier handler and residual trace consume Sr(i,i) alone).  Item
        // #13A: when the off-diagonal-R kernel is enabled AND a dense R was
        // supplied (BSSD / baseline-DD epochs, where R carries SD/DD cross-
        // covariance), form the full Sr = R − H·P⁺·Hᵀ so the redundancy and
        // w-test QC below honour correlated observations.  The diagonal of
        // the full Sr is identical to sr_diag, so the existing outlier path
        // sees the same per-row variances either way — only the off-diagonals
        // (previously dropped) are added, behind the flag.
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

        // Standardize post-fit residuals using the post-fit residual
        // covariance Sr(i,i) = (R - H*P_post*H')(i,i).
        //
        //  - Sr is the theoretically correct covariance for the post-fit
        //    residual vector (v = z - H*x_post).  Using the innovation
        //    covariance S = H*P*H' + R would over-scale the denominator
        //    and suppress outlier detection.
        //  - When Sr(i,i) is near-zero the measurement is almost perfectly
        //    explained; we clamp to a small positive value to avoid division
        //    by zero.
        // Standardize residuals using Sr = R - H·P_post·Hᵀ as the diagonal
        // denominator, clamped to the measurement's OWN noise variance R(i,i).
        //
        // Why the R floor: when one state in the filter absorbs a row's
        // innovation almost perfectly (e.g., a DD phase ambiguity with
        // huge initial variance → K_amb ≈ 1, P_post_amb ≈ 0), the formal
        // post-fit residual covariance collapses to Sr = R·S⁻¹·R which is
        // numerically much smaller than R itself.  The previous 1e-12
        // floor was unphysical — it let near-zero residuals register as
        // dozens of sigmas and caused DD phase updates to be rejected
        // every epoch.
        //
        // Clamping to R(i,i) keeps the well-posed PPP case unchanged
        // (Sr > R there) and restores physical meaning to the
        // standardized residual in absorption-heavy cases: "how many
        // measurement-sigma away from zero is this residual".  A real
        // cycle slip (e.g. 1 L1 cycle = 0.19 m on a 2 mm phase obs) is
        // still ~95σ and trips detection as usual.
        Eigen::VectorXd sres(measurement.residuals.size());
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double sii = (i < sr_diag.size()) ? sr_diag(i) : 0.0;
            // Floor the denominator at the *working* R(i,i) — the inflated
            // value, not the formal measurement.R_v.  Without this a Huber or
            // EWMA R-inflation never registers: the standardized residual
            // stays pinned to the formal noise, so |sres| can't shrink on the
            // rebuild and the inflation loop diverges.  The 1e-12 term guards
            // the pathological case where both Sr and R are unset.
            const double r_floor = (i < R_.rows() && R_(i, i) > 0.0) ? R_(i, i) : 0.0;
            const double denom = std::sqrt(std::max({ sii, r_floor, 1e-12 }));
            const double val = measurement.residuals(i);
            sres(i) = (std::isfinite(val) && std::isfinite(denom) && denom > 0.0) ? (val / denom) : 0.0;
        }
        measurement.residuals_standardized = sres;

        // --- Statistical QC layer (item #9): a-posteriori variance factor
        //     (σ̂₀² = Σ vᵢ²/Rᵢᵢ / dof) + Baarda data-snooping (per-row
        //     redundancy rᵢ, w-test wᵢ, MDBᵢ).  Pure diagnostics: the QC
        //     values are stashed on @c measurement (logged + optionally used
        //     by the data-snooping outlier gate) and never mutate the
        //     propagated state/covariance.  Skipped entirely when both
        //     toggles are off, so the hot path is unchanged.  When the
        //     off-diagonal kernel built a full Sr it is passed through so the
        //     redundancy/w-test honour correlated R.
        if (config_.enableVarianceFactor() || config_.enableDataSnooping()) {
            computeResidualQc_(measurement, R_, sr_diag,
                               Sr_full.size() != 0 ? &Sr_full : nullptr);
        }

        // --- Outlier handling ---
        // Runs for both PPP and baseline.  The Sr-clamped-to-R standardization
        // above keeps this check numerically well-posed even in DD phase
        // absorption scenarios, while preserving cycle-slip detection
        // (a 1-cycle slip is still ~95σ on a 2 mm phase obs).
        // x and P here are the pre-update values; validate doesn't mutate
        // them (the `&` parameters are legacy, not actually written to).
        auto [is_outlier, is_reset] = validatePostFitResiduals(
            measurement,
            const_cast<Eigen::VectorXd&>(x),
            const_cast<Eigen::MatrixXd&>(P));
        was_complete_reset_ = is_reset;

        if (is_outlier) {
            // Writeback: handlePhaseOutlier (and friends) modified x and P
            // in place to reset the affected ambiguity to a fresh seed
            // value with inflated covariance.  Without this writeback those
            // changes are local to this stack frame and silently discarded
            // when we return — the slipped ambiguity then stays at its
            // pre-update (corrupt) value and the same slip residual
            // reappears on every subsequent rebuild pass and every
            // subsequent epoch.  Costs an extra n²-element copy on the
            // outlier path, which fires far less often than the success
            // path; the persistence-vs-perf trade-off is the right one.
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
        // Stash the prior (x, P) this update read so
        // Application::runGeometryRelinearization can anchor its IEKF passes on
        // it.  This is the correct anchor: it is the very prior the converged
        // measurement update operated on, so it matches the post-update state
        // dimension (this epoch's ambiguity augmentation and any outlier resets
        // already applied).  Gated to the relinearization path to avoid the
        // ~1 MB P copy when the feature is off or under tightly-coupled IMU.
        if (config_.relinearizeGeometry() && !config_.tightlyCoupledImu()) {
            last_update_prior_x_ = x;
            last_update_prior_P_ = P;
        }
        state_manager_.setStateVector(x_new);
        state_manager_.setCovariance(P_new);
        state_manager_.setStateEpochTime(measurement.time);

        // --- Apply pending stuck-code-bias resets -------------------------
        // Queued by the stuck-code-bias scan inside
        // validatePostFitResiduals (clean-commit branch above).  Applied
        // here, immediately after commit, so the next epoch starts with
        // the re-seeded bias.  Reading the just-committed state via
        // state_manager_.getStateVector() rather than x_new because
        // resetSatBiasState mutates state_vector_ in place and we want a
        // consistent view for any subsequent resets in this batch.
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
                // Small-angle rotation correction: C_corrected = (I - [dpsi]x) * C
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


        // Per-satellite residual summary
        if (is_outlier) {
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

        // Linearization point xᵢ: the current committed state, where
        // `measurement` was linearized (its innovations are z − h(xᵢ) and
        // H = ∂h/∂x|xᵢ).  For the first IEKF pass this is the posterior the
        // primary update() converged on; for later passes it is the previous
        // iterate.
        const Eigen::VectorXd x_lin = state_manager_.getStateVector();

        // Gauss–Newton (IEKF) innovation against the FIXED prior x⁻:
        //   ν = (z − h(xᵢ)) − H·(x⁻ − xᵢ) = innovations + H·(xᵢ − x⁻).
        // The offset term makes the step independent of the linearization
        // point in the linear limit, so josephPseudoUpdate(x⁻, P⁻, …, ν)
        // yields the proper MAP update x⁺ = x⁻ + K·ν with K and (I−KH)P⁻ both
        // formed from the prior — no double-counting of measurement
        // information across passes.
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

        // Standardized residuals with Sr = R − H·P_new·Hᵀ, denominator floored
        // at the working R(i,i) — identical convention to update() so the
        // residual trace is directly comparable across the two paths.
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

        // Item #13A: full Sr = R − H·P⁺·Hᵀ when the off-diagonal kernel is on
        // and a dense R was supplied (mirrors update(); same value on the
        // diagonal).  Then the same statistical QC (item #9) the primary
        // path produces, so the residual trace carries vf / redundancy /
        // w-test on relinearized epochs too.
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
        // as "non_pivot-pivot", so ref_sat IS the pivot when present).
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

            // The pivot's SD ambiguity is always 0 by definition.  Do not
            // inflate its variance — that would trigger the BSSD disturbance
            // detector and cascade into an unnecessary pivot handover.
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

            klogger->debug("{}Reinitialized ambiguity for {} {} (idx {}): prev={:.6f} m -> seed={:.3f} m, std={:.3f} m{}",
                epoch_prefix, sat, outlier_sig, amb_idx, prev_amb, seed_value, amb_std,
                bssd_active ? " (SD)" : "");

            last_reset_satellites_.push_back(sat);
            last_reset_signals_.emplace_back(sat, outlier_sig);
        };

        resetOneAmbiguity(primary_sat, "primary");

        // In BSSD mode the ref_sat is always the pivot (the SD pair is
        // formed as "non_pivot-pivot" in BssdTransform).  The pivot's SD
        // ambiguity is 0 by definition — resetting it would corrupt every
        // other SD measurement that references it and trigger an
        // unnecessary pivot handover.  Only the primary (non-pivot)
        // satellite needs its ambiguity reset.
        //
        // In non-BSSD mode (or when the SD identifier contains a non-pivot
        // reference, e.g. combined-mode IF pairs), reset the ref_sat.
        // Guard: when bssd_active with a ref_sat, the ref_sat is the pivot
        // even if the pivot lookup was empty (the SD pair format guarantees
        // the second element is the pivot).
        if (!ref_sat.empty() && !bssd_active) {
            resetOneAmbiguity(ref_sat, "BSSD-ref");
        } else if (!ref_sat.empty()) {
            klogger->debug("{}Phase outlier on SD pair {}: skipping ref_sat {} reset (BSSD pivot)",
                epoch_prefix, outlier_sat, ref_sat);
        }
    }

    // -------------------------------------------------------------------
    // Baseline (DD) phase outlier handler — see header for design notes.
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
        // rover side only — see MeasurementModel::computeMeasurementUpdate.
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

        // Find the matching row to recover the pre-update DD residual.
        // MeasurementModel sets innov = innov_pre - x(amb_idx) for phase
        // rows, so re-seeding at x(amb_idx) + innov restores innov_pre as
        // the new ambiguity float.  Subsequent epochs will then start with
        // a small innovation regardless of slip magnitude.
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

        // Bump the per-row consecutive-rejection streak.  Cleared on
        // any future epoch where the row is kept (see
        // ResidualHistoryTracker::record).  Drives the stuck-bias
        // detector in the clean-commit path below: when the streak
        // reaches @c estimation.stuck_code_bias.min_streak the
        // corresponding sat-bias state is reset to break the loop
        // where an over-converged bias keeps rejecting its own row.
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

        // Baseline mode: no clock/tropo/gradient/ISB/iono/IMU states to
        // reset; go straight to the ambiguity block below.
        if (config_.isBaselineMode()) {
            for (const auto& [key, idx] : state_manager_.getAmbiguities()) {
                if (idx < static_cast<size_t>(P.rows())) {
                    P(idx, idx) = amb_std * amb_std;
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

        // Ambiguities — reset all to initial variance, but keep BSSD pivot
        // ambiguities and ionosphere frozen (value=0, variance=0).  Without
        // this the disturbance detector would fire on every epoch after a
        // complete reset because all satellites would appear disturbed.
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

        // Baarda non-centrality δ₀ for the configured critical value at the
        // conventional 80% test power (β=0.20 → Φ⁻¹(0.80)=0.8416).  Used only
        // to scale the minimal detectable bias; δ₀≈4.13 for the default
        // k=3.29.  The a-priori variance of unit weight σ₀² is 1 (R is in
        // absolute units), so MDBᵢ = σᵢ·δ₀/√rᵢ.
        const double delta_0 = config_.dataSnoopingCritical() + 0.8416;
        constexpr double kRedFloor = 1e-6;    // guard /√r
        constexpr double kSrFloor  = 1e-12;   // guard /√Sr
        const double kInf = std::numeric_limits<double>::infinity();

        Eigen::VectorXd redundancy(m);
        Eigen::VectorXd wtest(m);
        Eigen::VectorXd mdb(m);
        double wssr = 0.0;   // Σ vᵀR⁻¹v  (weighted post-fit residual SSR)
        double dof  = 0.0;   // Σ rᵢ      (effective redundancy / degrees of freedom)

        const bool use_full = (Sr_full != nullptr &&
                               Sr_full->rows() == m && Sr_full->cols() == m &&
                               R.rows() == m && R.cols() == m);

        if (use_full) {
            // Correlation-aware path (off-diagonal R kernel, item #13A):
            //   redundancy matrix  Red = Sr · R⁻¹   → rᵢ = Red(i,i), dof = tr(Red)
            //   w-test (Teunissen) wᵢ = (Sr⁻¹ v)ᵢ / √(Sr⁻¹)ᵢᵢ
            //   wssr              = vᵀ R⁻¹ v
            const Eigen::MatrixXd Rinv =
                R.ldlt().solve(Eigen::MatrixXd::Identity(m, m));
            const Eigen::MatrixXd Red   = (*Sr_full) * Rinv;
            const Eigen::MatrixXd Srinv =
                Sr_full->ldlt().solve(Eigen::MatrixXd::Identity(m, m));
            const Eigen::VectorXd Srinv_v = Srinv * meas.residuals;
            wssr = meas.residuals.dot(Rinv * meas.residuals);
            for (Eigen::Index i = 0; i < m; ++i) {
                double r_i = Red(i, i);
                r_i = std::isfinite(r_i) ? std::min(std::max(r_i, 0.0), 1.0) : 0.0;
                redundancy(i) = r_i;
                dof += r_i;
                const double srinv_ii = Srinv(i, i);
                wtest(i) = (std::isfinite(srinv_ii) && srinv_ii > kSrFloor &&
                            std::isfinite(Srinv_v(i)))
                    ? Srinv_v(i) / std::sqrt(srinv_ii) : 0.0;
                const double sigma_i = (R(i, i) > 0.0) ? std::sqrt(R(i, i)) : 0.0;
                mdb(i) = (r_i > kRedFloor) ? sigma_i * delta_0 / std::sqrt(r_i) : kInf;
            }
        } else {
            // Diagonal (uncorrelated) path — the common PPP case.
            for (Eigen::Index i = 0; i < m; ++i) {
                const double Rii  = (i < R.rows() && R(i, i) > 0.0) ? R(i, i) : 0.0;
                const double srii = (i < sr_diag.size()) ? sr_diag(i) : 0.0;
                const double v_i  = meas.residuals(i);
                double r_i = (Rii > 0.0) ? srii / Rii : 0.0;
                r_i = std::isfinite(r_i) ? std::min(std::max(r_i, 0.0), 1.0) : 0.0;
                redundancy(i) = r_i;
                dof += r_i;
                if (Rii > 0.0 && std::isfinite(v_i)) wssr += (v_i * v_i) / Rii;
                // True studentized residual (NOT floored at R) — the Baarda w-test.
                const double srii_pos = std::max(srii, kSrFloor);
                wtest(i) = std::isfinite(v_i) ? v_i / std::sqrt(srii_pos) : 0.0;
                const double sigma_i = (Rii > 0.0) ? std::sqrt(Rii) : 0.0;
                mdb(i) = (r_i > kRedFloor) ? sigma_i * delta_0 / std::sqrt(r_i) : kInf;
            }
        }

        meas.residual_redundancy = redundancy;
        meas.residual_wtest      = wtest;
        meas.residual_mdb        = mdb;
        meas.variance_dof        = dof;
        meas.variance_factor     =
            (dof > 1e-9 && std::isfinite(wssr)) ? (wssr / dof) : 1.0;
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

        // Stable denominator for the complete-reset fraction further below: the
        // number of measurements this epoch STARTED with, reconstructed as the
        // current usable rows plus everything earlier rebuild passes already
        // excluded.  @c excluded_measurements is seeded from the carried set by
        // computeMeasurementUpdate, so at this point — before this pass's own
        // dispatch adds to it — it holds exactly the prior exclusions; the sum
        // is therefore the original pre-peel count, invariant across passes.
        //
        // Using the per-pass (shrinking) @c num_obs as the denominator made the
        // reset trip after excluding only ~p/(1+p) of the original set (e.g.
        // ~37% at p=0.5, not the configured 50%), aborting an otherwise-
        // converging one-at-a-time peel and re-inflating the filter every epoch
        // — the persistent-code-outlier signature.  The original count keeps the
        // backstop firing only at the genuinely-configured bad fraction.
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

        // Per-row residual table — emitted only at TRACE (every
        // epoch) or at DEBUG when an outlier was detected.  The
        // log gets STRICTLY MORE concise as the threshold ascends:
        //   - trace: full per-row table every epoch
        //   - debug: per-row table only on outlier events
        //   - info  : no per-row data (Residual summary line covers it)
        //   - warn  : no per-row data (only pivot warnings + outlier-flagged warns)
        //
        // The previous warn-level fallback that re-emitted the table
        // when an outlier fired and debug was off has been removed —
        // it produced bulk per-row output at the warn threshold,
        // contradicting the "more concise as level rises" intent.
        if (klogger->should_log(spdlog::level::trace)) {
            logResidualTable(spdlog::level::trace);
        } else if (max_sres > threshold && klogger->should_log(spdlog::level::debug)) {
            logResidualTable(spdlog::level::debug);
        }

        // Severity-stratified by iteration outcome:
        //   - INFO when this pass will commit cleanly (max_sres ≤ threshold) —
        //     gives users one summary line per converged epoch by default.
        //   - DEBUG when outliers will be handled this pass; per-iteration
        //     summaries during rebuild loops are diagnostic, not summary.
        //
        // The pivot-slip detector below can still escalate this pass to a
        // forced rebuild, but its own warn-level message carries the
        // visible-by-default signal in that case — repeating the residual
        // summary at info would be redundant.
        const auto residual_summary_level =
            (max_sres > threshold) ? spdlog::level::debug : spdlog::level::info;

        // Compute RMS of standardized residuals over finite entries.
        // Useful as a "is the filter overall happy" scalar — RMS|sres|
        // near 1.0 means residuals are consistent with the modelled
        // covariance; values >> 1 mean the filter is over-confident
        // (R or P too tight); values << 1 mean over-conservative.
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
        // Diagnostics only — the propagated state/covariance are untouched.
        // The variance factor rescales a REPORTING copy of σ_pos
        // (σ_pos → formal·√σ̂₀²); it never feeds back into P.
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
        // Runs BEFORE the per-row outlier dispatch.  When a BSSD pivot
        // satellite has a phase tracking glitch (sub-cycle drift, brief
        // multipath ringing, partial reacquisition), every SD residual
        // that uses that pivot picks up the same offset — same sign,
        // similar magnitude, on the same frequency band.  The per-row
        // |sres|>threshold check sees N independent outliers and would
        // peel them off one by one (each non-pivot ambiguity reset
        // incorrectly), wasting N rebuild passes and corrupting per-sat
        // MW arc histories on innocent satellites.
        //
        // The detector groups phase rows by (pivot, signal) — full
        // RINEX-3 code, e.g. "L1W" vs "L1C" are separate groups — to
        // match the per-signal state granularity in PPP-RTK uncombined,
        // where ambiguity, sat-bias, and ATX corrections are all keyed
        // per-signal.  A disturbance isolated to one tracking attribute
        // (single-channel hardware glitch, attribute-specific ATX miscal)
        // would be diluted at the band level if both attributes are
        // tracked; per-signal keying preserves sensitivity.  The
        // cross-band detector below still pools across signals per
        // pivot and gates on distinct-band count for the independence
        // argument.  Each group is tested for the coherent-offset
        // signature using robust statistics:
        //   1. Group must have at least MIN_GROUP_SIZE rows (small groups
        //      have too little statistical power for the test).
        //   2. At least ABOVE_FRACTION_MIN of the group's |sres| must be
        //      above the per-row outlier threshold (vs. a fixed count, so
        //      the trigger scales with sat visibility).
        //   3. |median(residual)| must exceed MEDIAN_FLOOR_M to avoid
        //      firing on numerical noise.
        //   4. MAD / |median| must be below MAD_RATIO_MAX — this is the
        //      key robustness step: the median is unaffected by one or
        //      two satellites that ALSO genuinely slipped at the same
        //      epoch, and MAD measures spread without being inflated by
        //      those outliers.  Tight MAD relative to median ⇒ coherent
        //      pivot offset; loose MAD ⇒ independent outliers.
        //
        // On detection: each non-pivot SD ambiguity in the group is reset
        // via handlePhaseOutlier (BSSD non-pivot path), all rows are
        // added to excluded_measurements for the rebuild, and the
        // per-row dispatch below is skipped for this pass — the pivot
        // slip explains every outlier in the group, so peeling them off
        // individually is wasted work.
        {
            constexpr size_t MIN_GROUP_SIZE      = 4;
            constexpr size_t MIN_POOL_SIZE       = 3;  // cross-band detector
            constexpr size_t MIN_DISTINCT_BANDS  = 2;  // cross-band detector
            constexpr double ABOVE_FRACTION_MIN  = 0.5;
            constexpr double MAD_RATIO_MAX       = 0.4;
            constexpr double MEDIAN_FLOOR_M      = 0.005;

            // Robust band (k·MAD, floored) used to classify which non-pivot
            // SD rows in a firing group DISAGREE with the coherent median.
            // Those deviating sats are co-disturbed or independently slipping
            // and are recorded as replacement-pivot exclusions so the forced
            // handover lands on a consensus satellite (see recordHandoverExcludes).
            // k=3 sits just inside the observed co-offset separation (a second
            // offset sat lands ~3.2·MAD off the median); the absolute floor
            // guards the degenerate MAD→0 case. The PivotManager retired-pivot
            // backstop guarantees termination even if a borderline outlier is
            // missed here, so the band is deliberately permissive.
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
            // Per-signal groups, keyed by (pivot, signal) — full RINEX-3
            // code, so "L1W" and "L1C" form separate groups (matches the
            // per-signal state granularity in PPP-RTK uncombined).
            std::map<std::pair<std::string, std::string>, Group> groups;
            // Cross-band per-pivot pools, used as a fallback when individual
            // signals carry too few non-pivot rows to clear MIN_GROUP_SIZE
            // but the multi-band picture still shows a coherent slip pattern.
            // @c pivot_pool_bands tracks distinct *bands* (frequency axis)
            // not signals, because the cross-band detector's evidence value
            // depends on independent frequency channels — two attributes on
            // the same carrier (L1W + L1C) are not independent.
            std::map<std::string, Group> pivot_pools;
            std::map<std::string, std::set<std::string>> pivot_pool_bands;

            // Gate: exclude SD rows whose (sat, signal) has its ATX
            // correction frequency-fallback-substituted on either sat or
            // pivot.  The substituted PCV introduces a small per-sat
            // bias that, when summed across multiple non-pivot rows in
            // the same band, mimics a coherent pivot-slip signature
            // (large non-zero median, tight MAD-around-median).  Without
            // this gate, "all"-mode runs that include substituted-ATX
            // bands like GPS L5 produce a perpetual pivot-handover
            // flip-flop: every new pivot reads as slipped because the
            // bias is structural, not transient.
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
                const std::string band = sig.substr(0, 2);     // "L1", "L2", "L5" — cross-band gate axis only
                const double r  = res(static_cast<Eigen::Index>(i));
                const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                const bool   ab = std::isfinite(sa) && sa > threshold;

                // Per-signal grouping (full RINEX code) for the per-band
                // detector below — matches per-signal state granularity in
                // PPP-RTK uncombined.
                auto& g = groups[{ pivot, sig }];
                g.indices.push_back(i);
                g.residuals.push_back(r);
                if (ab) ++g.above;

                auto& pp = pivot_pools[pivot];
                pp.indices.push_back(i);
                pp.residuals.push_back(r);
                if (ab) ++pp.above;
                pivot_pool_bands[pivot].insert(band);  // band, not signal — independence axis
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

            // Record the satellites a forced handover must NOT elect as the
            // new pivot for @p cons: the flagged @p pivot_sat itself, plus any
            // non-pivot row whose SD residual sits outside the coherent band
            // around @p med.  Such rows are co-disturbed or independently
            // slipping — electing one just re-anchors the SD frame on another
            // offset sat, the handover flip-flop this guards against.  Keyed
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

            // Record EVERY non-pivot member of a firing group for post-handover
            // ionosphere / sat-bias re-inflation (the code-detector path only —
            // see takeRequestedPivotHandoverIonoReinflateSats).  This is the
            // COMPLEMENT of recordHandoverExcludes: the excludes are the pivot
            // plus the few deviators that fall OUTSIDE the coherent band, whereas
            // the satellites that actually absorbed the pivot's code bias are the
            // coherent majority INSIDE the band.  Re-inflating the contaminated
            // majority (not the deviators) is what lets the post-handover clean
            // frame re-estimate the states the lossless re-reference can't fix.
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

                // Compute MAD around the median *first* so the cycle-floor
                // decision below can use cluster tightness as evidence.  A
                // tight cluster is itself proof that the residuals are not
                // independent noise — multipath/atmosphere at sub-cycle
                // scale shows MAD/|med| well above the cutoff, while a
                // coherent pivot disturbance produces MAD/|med| <= 0.1
                // typically.
                std::vector<double> abs_dev;
                abs_dev.reserve(g.residuals.size());
                for (double v : g.residuals) abs_dev.push_back(std::abs(v - med));
                const double mad = medianInPlace(abs_dev);
                const double mad_ratio = mad / std::abs(med);
                if (mad_ratio > MAD_RATIO_MAX) continue;

                // Wavelength-fraction floor — a real *cycle* slip is ≥ 1 λ,
                // and the floor was originally added to reject sub-cycle
                // medians as "bias, not slip."  But the pivot-slip detector
                // fires a *handover*, not a per-row reset, and a handover
                // is the right response to any coherent pivot disturbance
                // regardless of whether the disturbance is an integer
                // cycle: phase wind-up error, half-cycle ambiguity, brief
                // pivot-only multipath, hardware bias change — all show
                // sub-cycle coherent SD residuals and all benefit from
                // re-referencing to a clean pivot.  The cycle floor is
                // therefore *bypassed* when the cluster is tighter than
                // TIGHT_CLUSTER_MAD_RATIO: at that tightness the only
                // plausible explanation across multiple non-pivot SD rows
                // is a common pivot disturbance, so we hand over even on
                // a 1 cm median.  Looser-but-still-passing-MAD clusters
                // (mad_ratio in (TIGHT, MAX]) keep the original floor as
                // a hedge against single-frequency atmospheric events
                // that happen to land below the loose-cluster bound.
                constexpr double TIGHT_CLUSTER_MAD_RATIO = 0.2;
                if (cyc_floor_fraction > 0.0 && mad_ratio > TIGHT_CLUSTER_MAD_RATIO) {
                    // key.second is now the full signal (e.g. "L1W"); the
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

                // -----------------------------------------------------------
                // Temporal corroboration via the residual-history tracker.
                //
                // The spatial criteria above (fraction-above-threshold +
                // tight MAD around a non-zero median) catch the geometric
                // signature of a pivot slip.  But spatial coherence alone
                // also matches genuine atmospheric events (storm fronts,
                // scintillation) that shift many sats simultaneously.
                //
                // The tracker adds the "is each row in this group also
                // anomalous against its OWN history?" axis.  When the
                // tracker is enabled and at least @c warmup_n samples
                // have accumulated for the group's rows, we count what
                // fraction of them have |z| > z_threshold against their
                // EWMA mean and stdev.  Spatial AND temporal agreement
                // logs as HIGH confidence; spatial alone logs as MEDIUM.
                // The handover request fires either way — the temporal
                // signal currently augments diagnostics rather than
                // gating the action.  Promoting it to a gate is a
                // one-line change here once you've gathered confidence
                // in the false-positive rate.
                // -----------------------------------------------------------
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
                // Resetting every non-pivot SD ambiguity in place would have
                // been wrong: the non-pivots are stable; only the pivot's
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

            // -----------------------------------------------------------
            // Cross-band pivot-slip detector
            //
            // The per-band pass requires ≥ MIN_GROUP_SIZE non-pivot SD rows
            // *within a single band*.  When phase coverage is sparse on any
            // one band — common for mixed BeiDou-2/BeiDou-3 (some sats on
            // C2I/C7I, others on C1X/C5X) or new-block satellites that have
            // partial signal sets — a real pivot disturbance can leave each
            // per-band group below 4 rows even when the *pooled* picture
            // is unambiguous.  Pool across bands per pivot and apply the
            // same coherence criteria with MIN_POOL_SIZE = 3 and an
            // explicit ≥ MIN_DISTINCT_BANDS requirement.  The multi-band
            // requirement is what makes this evidence *stronger* than
            // within-band: a coherent same-sign residual pattern across
            // independent frequency channels is hard to mimic with
            // multipath or atmospheric events.
            // -----------------------------------------------------------
            for (auto& [pivot_sat, pp] : pivot_pools) {
                const std::string cons = PppUtils::getSystemString(pivot_sat);
                // Skip if the per-signal detector already requested a
                // handover for this constellation — its row set is a
                // subset of this pool's rows and a duplicate warning
                // would mis-describe the same physical event.
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

            // -----------------------------------------------------------
            // Code-side pivot disturbance detector (per-band + cross-band)
            //
            // Mirror of the phase per-signal and cross-band detectors
            // above, gated on @c sig[0]=='C' instead of 'L'.  When the
            // pivot satellite has a code-side bias (DCB drift, ATX
            // miscal on a specific band/attribute, hardware glitch on
            // the pivot SV), every SD code row that uses the pivot
            // inherits the same bias — same sign, similar magnitude,
            // grouped per full RINEX signal (e.g. "C1W" and "C1C" form
            // separate per-signal groups, matching per-signal state
            // granularity in PPP-RTK uncombined).  The cross-band
            // detector still pools across signals per pivot and gates
            // on distinct *bands* (frequency axis) for the independence
            // argument.
            // Without a coherence test the per-row dispatch peels rows
            // off one-by-one across N rebuild passes, the iono state in
            // uncombined mode absorbs the bias through repeated updates,
            // and bad_residual_count climbs until the rebuild budget is
            // exhausted (typical user-observed signature: 6-9σ outliers
            // on every SD pair against a single pivot for a specific
            // code signal, growing in magnitude across epochs).
            //
            // Structural differences from the phase detector:
            //   - Higher median floor (MEDIAN_FLOOR_M_CODE = 0.30 m).
            //     Phase uses 5 mm because sub-cycle is meaningful; code
            //     noise floor sits well above that — routine multipath
            //     hits 0.1–0.2 m without indicating any pivot problem.
            //     0.30 m is ~0.6× a typical code σ for a well-modeled
            //     station, cleanly above multipath and below any real
            //     pivot disturbance signature.
            //   - No wavelength floor (cycles don't apply to code).
            //   - No temporal residual-history corroboration (the EWMA
            //     tracker is tuned for phase variance; code stats need
            //     separate tuning — can add later).
            //   - No non-pivot multi-band variant.  Without the
            //     integer-cycle consistency check, code-side coherent
            //     outliers on a non-pivot are just bad measurements;
            //     the per-row dispatch already handles them correctly
            //     and the only saving would be fewer rebuild passes.
            //
            // Same downstream action as phase: bulk-exclude affected
            // rows and request a forced pivot handover.  Justified by
            // the persistence signature — a code bias that fires for
            // multiple consecutive epochs (e.g. 6σ → 9σ across two
            // epochs) corrupts the pivot's iono state through repeated
            // measurement updates in uncombined mode, and re-referencing
            // to a clean pivot is the only structural fix.
            //
            // Reuses @c Group, @c medianInPlace, @c row_atx_substituted,
            // @c any_pivot_slip, and @c requested_pivot_handovers_ from
            // the enclosing pivot-slip block.
            // -----------------------------------------------------------
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
                    const std::string band = sig.substr(0, 2);  // "C5", "C7", "C1", ... — cross-band gate axis only
                    const double r  = res(static_cast<Eigen::Index>(i));
                    const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                    const bool   ab = std::isfinite(sa) && sa > threshold;

                    // Per-signal grouping (full RINEX code, e.g. "C1W"
                    // vs "C1C") matches per-signal state granularity in
                    // PPP-RTK uncombined — sat-biases and ATX corrections
                    // are keyed per-signal, so an attribute-specific
                    // pivot disturbance corrupts only the matching
                    // states and should be detected at that resolution.
                    auto& g = code_groups[{ pivot, sig }];
                    g.indices.push_back(i);
                    g.residuals.push_back(r);
                    if (ab) ++g.above;

                    auto& pp = code_pivot_pools[pivot];
                    pp.indices.push_back(i);
                    pp.residuals.push_back(r);
                    if (ab) ++pp.above;
                    code_pivot_pool_bands[pivot].insert(band);  // band, not signal — independence axis
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
                // constellation — the cross-band pool's row set is a
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

            // -----------------------------------------------------------
            // Non-pivot multi-band cycle-slip detector
            //
            // Mirror of the cross-band pivot detector, with the axes of
            // coherence rotated 90°: a single non-pivot satellite whose
            // phase observations slipped on multiple bands simultaneously
            // shows up as one satellite × many bands of same-sign anomalous
            // residuals against an otherwise-clean pivot.  A real cycle
            // slip jumps the integer ambiguity by N cycles on every band
            // the satellite tracks; the SD residual is N·λ_band, which is
            // band-dependent in metres but band-independent in cycles.
            // The integer-cycle consistency check is the strongest
            // discriminator from atmospheric / multipath events (which
            // scale with f² for iono and with ~constant in metres for
            // tropo/multipath, neither matching N·λ_band per-band).
            //
            // Especially valuable as a backstop for GLONASS, where the
            // MW pre-filter detector has reduced sensitivity due to the
            // FDMA receiver inter-channel WL bias inflating the MAD floor.
            //
            // On detection: ambiguities for the affected satellite are
            // marked pending re-seed and its rows excluded from the
            // current rebuild pass.  No constellation-wide pivot
            // handover — the pivot is unaffected.
            // -----------------------------------------------------------
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
                    // Record the reset at SATELLITE granularity only (no
                    // per-signal entries): every phase ambiguity of the sat
                    // was reset, which is exactly the sat-wide signature
                    // reinflateResetSatStates keys on for the iono/sat-bias
                    // partition guard, and the per-sat fallback in the
                    // callers' arc-clearing loops then drops the whole MW /
                    // TurboEdit arc history for the sat (a multi-band
                    // common-integer slip invalidates all of it).  Without
                    // this push the reset was invisible to arc clearing,
                    // reinflation, and smoother segmentation alike.
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
        // Detect sub-cycle phase rows whose post-fit |sres| exceeds the
        // Huber threshold but stays below the outlier threshold (or even
        // above it, as long as the residual is sub-cycle).  For each such
        // row, queue a target R(i,i) into @c pending_r_inflations_ so the
        // next rebuild pass downweights the row enough that |sres| lands
        // at the Huber threshold.  Trigger a rebuild via outlier_found=true
        // so the existing rebuild loop reruns the update with the inflated
        // R.  Rows where |res| ≥ cyc_floor·λ are skipped — those are
        // cycle-slip territory and the dispatch below handles them with
        // reset.
        {
            const double huber_k     = config_.getPhaseHuberSresThreshold();
            const double cyc_floor   = config_.getPivotSlipMinCycles();
            const bool   use_innov   = config_.enableHuberInnovation();
            const bool   huber_on    = config_.enableHuberRInflation();
            // Rebuild-loop exit tolerance.  The post-fit IRLS fixed point
            // is |sres| = k exactly, so a bare "|sres| <= k" test would
            // chase an asymptote; the gate below requires a pass's
            // relative R step to clear this before re-queuing.  The
            // innovation scheme lands on k in one shot — the gate then
            // only blocks its no-op floating-point re-trigger.
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

                    // Two R-inflation schemes for a flagged sub-cycle row,
                    // selected by estimation.enable_huber_innovation:
                    //
                    //  - innovation one-shot: standardize the pre-fit
                    //    innovation v by |s| = |v|/sqrt(HPHᵀ + R), monotone
                    //    in R — so the R that lands |s| = k is closed-form,
                    //    R_target = v²/k² - HPHᵀ, where HPHᵀ = (H·P⁻·Hᵀ)(i,i)
                    //    is the R-independent prior variance projected into
                    //    the row.  Converges in one rebuild pass.
                    //
                    //  - post-fit IRLS: standardize the post-fit residual,
                    //    |sres| = |res|/sqrt(R), and inflate by (|sres|/k)²
                    //    per pass.  The post-fit residual drifts up as the
                    //    row loses weight, so |sres| converges to k only
                    //    asymptotically (and can overshoot first).
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

        // --- Baarda data-snooping gate override (item #9) -----------------
        // Default: the outlier gate fires on the fixed σ threshold
        // (gate_stat/gate_idx/gate_crit ≡ max_sres/max_idx/threshold), so the
        // dispatch below is byte-identical when data-snooping is off.
        //
        // When data-snooping is enabled, the gate instead fires on the Baarda
        // w-test |wᵢ| > dataSnoopingCritical() over CHECKABLE rows (redundancy
        // ≥ floor), and selects the worst checkable |w| row to handle.
        // Low-redundancy rows are skipped as uncheckable: their residual is
        // unreliable (a blunder there is absorbed into the estimate, so
        // resetting the corresponding ambiguity would discard good
        // information).  The downstream MW tiebreaker still operates in the
        // σ-domain; if the w-test caught a blunder the σ cut missed, the
        // tiebreaker finds no σ-candidates and the w-selected row is handled
        // directly.
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

            // (The per-row residual table dump for outlier
            // diagnosis is now handled by the ascending-conciseness
            // block higher up: at debug level the table emits when
            // max_sres > threshold; at info/warn it doesn't, by
            // design.  Users wanting the per-row context for an
            // outlier event drop their kalman_filter level to
            // debug.)

            // --- MW tiebreaker for competing phase outliers ---
            // When multiple satellites in the same constellation have phase
            // outliers with nearly equal |sres|, the raw maximum pick is
            // unreliable.  Use MW deviation scores to identify the satellite
            // most likely to have actually slipped.
            //
            // chosen_idx is seeded from gate_idx (≡ max_idx unless
            // data-snooping reselected the row via the w-test).  The σ-domain
            // tiebreaker below only refines among rows the σ cut also flags;
            // when data-snooping fired on a row the σ cut missed, no
            // σ-candidate is collected and the w-selected gate_idx is kept.
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

                    // Deduplicate by satellite (one entry per sat, keep the
                    // worst |sres| for that satellite).
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
            // sres/res(chosen_idx) alias the row that was previously at chosen_idx+1.
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
                // Wavelength-fraction floor on the *raw* phase residual:
                // a real cycle slip moves the observation by an integer
                // number of wavelengths, so anything below
                // cyc_floor_fraction × λ cannot be a slip and the
                // ambiguity reset (variance inflation, loss of converged
                // precision) is the wrong response.  Skip the reset
                // but still exclude the row from this rebuild — its
                // sub-cycle bias may be real (multipath, PCV miscal,
                // kinematic dynamics) and would pull position/clock
                // if absorbed.  The KF state stays intact; next epoch
                // brings a fresh measurement.
                //
                // Skipped for iono-free combos ("L1C_L2W"-style) since
                // the combined residual isn't a clean multiple of any
                // single λ — those still go through reset.
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
                // Resetting only one frequency leaves the others corrupted,
                // causing unnecessary extra rebuild passes.
                //
                // The λ-floor applies here too — sub-cycle correlated
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
        // Denominator is the ORIGINAL pre-peel measurement count (see
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

        // Feed accepted residuals into the residual-history tracker.
        // Only fires on the "clean commit" path: no outliers detected
        // and no complete reset.  Excluded measurements are skipped
        // because their residuals weren't actually applied to the
        // state.  This keeps the EWMA statistics representative of
        // the rows the filter ACTUALLY used to update the state.
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
            // Detect (sat, signal) sat-bias states that have either
            // been outlier-rejected for too many consecutive epochs
            // (the closed-loop case where the bias state has over-
            // converged around the wrong value and its own row keeps
            // tripping the outlier gate) or whose post-fit EWMA mean
            // has drifted past the bias floor (the slow-drift case
            // where the row survives the gate but its converged bias
            // no longer matches the truth).  Queue resets for
            // application after commit so the next epoch starts with
            // a re-seeded bias state.
            //
            // Skips BSSD pivot satellites: their sat-bias is frozen
            // at (value=0, variance=0) by PivotManager and resetting
            // it would break the BSSD frame.  Any genuine bias
            // mismatch on a pivot manifests as a coherent shift on
            // every other sat in the group and is handled by the
            // existing spatial pivot-slip detector — a different
            // failure mode.
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

                    // Queue (key, EWMA mean) — applied post-commit in
                    // update().  EWMA mean is the sign-consistent
                    // residual offset and gives a much better
                    // re-seed than just inflating variance and
                    // letting the next innovation pull the bias
                    // (which can take many epochs with a converged
                    // P).  For the streak-only trigger
                    // (warmed_up == false) the mean is 0 and only
                    // the variance is inflated — the filter will
                    // then absorb the next surviving innovation
                    // freely.
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
            // above.  Provides at-a-glance "is the tracker warmed
            // up, what's the typical scale, anything wild" without
            // requiring users to scrape per-row trace logs.
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
        // return could leave entries behind — clearing here makes
        // each epoch start from a known-empty state.
        pending_sat_bias_resets_.clear();
    }

    void KalmanFilter::endEpoch() {
        // The buffer at this point holds the cumulative output of every
        // post-pass-1 update(): each pass's "Residual summary" line plus,
        // where applicable, that pass's outlier-handling block (Max sres,
        // Phase outlier action, Reinitialized ambiguity, Outlier flagged,
        // etc.).  Replaying it gives the user the intermediate-outlier
        // detail AND the final pass's residual check showing zero outliers
        // after rebuilds converged.
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
        // "Processing completed in N ms" line — easier to find when
        // comparing run-to-run timings.  The per-epoch debug line in
        // update() still goes to kalman_filter.log for detailed inspection.
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