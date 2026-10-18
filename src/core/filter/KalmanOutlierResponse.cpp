/**
 * @file KalmanOutlierResponse.cpp
 * @brief The responses to a post-fit outlier and the initial covariance seed.
 *
 *   handlePhaseOutlier          reset the ambiguity behind a phase outlier (BSSD-aware
 *                               seed; hands baseline rows to the DD variant);
 *   handleBaselinePhaseOutlier  the baseline (DD) ambiguity re-seed;
 *   handleCodeOutlier           exclude a code row from the measurement set;
 *   resetFilterCovariance       the complete reset: zero P, re-seed the diagonals;
 *   seedCovarianceDiagonals     the constructor's initial P for unset core states.
 *
 * Called from KalmanPostFitQc.cpp (the dispatch and the backstop) and from the
 * constructor.
 */

#include "filter/KalmanFilter.hpp"
#include "filter/KalmanRowIdentity.hpp"   // splitBssdSatId
#include "common/PppUtils.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <cmath>
#include <string>

namespace GnssProcessor {

    using kalman::splitBssdSatId;

    namespace {

    /// Predicate: value is non-finite or negative → needs initialization.
    /// Zero is a valid user-specified variance (fixed state).
    bool isUnsetVariance(double v) {
        return !std::isfinite(v) || v < 0.0;
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
                    pivot_abs_seed = measurement_model_.ambiguitySeed(obs, outlier_sig, x);
                    pivot_band_sig = outlier_sig;
                } else {
                    for (const auto& [sig, val] : obs.observed) {
                        if (sig.size() >= 2 && sig.substr(0, 2) == outlier_band) {
                            pivot_abs_seed = measurement_model_.ambiguitySeed(obs, sig, x);
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
                abs_seed = measurement_model_.ambiguitySeed(obs, outlier_sig, x);
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

} // namespace GnssProcessor
