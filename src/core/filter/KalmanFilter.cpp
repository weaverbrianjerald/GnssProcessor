/*!
 * \file KalmanFilter.cpp
 * \brief The KalmanFilter driver: construction, predict, the measurement update, the
 *        IEKF step, the pseudo-observation update and the per-epoch hooks.
 *
 * The class's member functions live in five files by concern (card UOM step U2b):
 *   KalmanFilter.cpp           this driver: KalmanFilter(), predict, predictAided, update,
 *                              updateFromPrior, applyPseudoObservation, the take* drains,
 *                              beginEpoch, endEpoch, logKalmanFilterTimings, logQcCounters;
 *   KalmanKernels.cpp          the algebra: the sparse-H products, spliceAndPropagate,
 *                              josephPseudoUpdate;
 *   KalmanResidualQc.cpp       the Baarda kernel and the statistical QC fill;
 *   KalmanPostFitQc.cpp        validatePostFitResiduals, the post-fit outlier screen;
 *   KalmanOutlierResponse.cpp  the responses to an outlier (ambiguity reset, row
 *                              exclusion, complete reset) and the initial covariance seed.
 */

#include "filter/KalmanFilter.hpp"
#include "filter/KalmanKernels.hpp"       // sparse-H products
#include "filter/KalmanRowIdentity.hpp"   // splitBssdSatId, bandWavelengthForSat
#include "logging/GatedSink.hpp"
#include "logging/KalmanFilterLogger.hpp"
#include "math/TimeUtils.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace GnssProcessor {

    using kalman::SparseHRow;
    using kalman::buildSparseH;
    using kalman::sparseHP;
    using kalman::sparseHPHt;
    using kalman::sparseKH;
    using kalman::sparseHv;
    using kalman::splitBssdSatId;
    using kalman::bandWavelengthForSat;

    namespace {

    const char* modeLabel(const GnssConfig& cfg) {
        return cfg.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK";
    }

    } // anonymous namespace

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
    // Aiding-source-aided prediction (the strapdown INS predict, composable)
    // ===================================================================

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
        // valid dt_total) rather than the GNSS dt: the
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

        // Phase totals in microseconds (wall clock).  Member accumulators
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
                ++qc_counters_.stuck_bias_resets;
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
    // Rebuild requests, the per-epoch hooks and the run-end timing summary
    // ===================================================================

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
            logQcCounters();
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
        logQcCounters();
    }

    void KalmanFilter::logQcCounters() {
        // Run-end telemetry (standing invariant 4: a run-end counter is the on-path
        // evidence that a QC branch acted), printed beside the timing summary and reset
        // with it.
        log_manager_.getLogger("main_log")->info("{}", formatKalmanQcCounters(qc_counters_));
        qc_counters_ = KalmanQcCounters{};
    }

    // ===================================================================
    // Pseudo-observation update
    // ===================================================================

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
