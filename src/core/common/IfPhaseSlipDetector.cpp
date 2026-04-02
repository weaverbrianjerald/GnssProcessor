/**
 * @file IfPhaseSlipDetector.cpp
 * @brief Implementation of the between-satellite, between-epoch IF phase slip detector.
 *
 * Ported from stochastic_model_phase_v03.m / compute_if_phase_diff().
 *
 * Algorithm summary (per constellation, per epoch):
 *   1. For each satellite with an IF phase observable (e.g. "L1C_L2W"),
 *      compute the IF phase OMC:  phi_IF = observed_IF - modeled_IF.
 *      The model subtraction removes the large satellite-specific geometric
 *      range (~20,000 km, changing by ~10 km/30s) and satellite clock, which
 *      do NOT cancel in between-satellite differencing.  After subtraction
 *      the OMC contains receiver clock, ZWD*mf_w, ISB, and float ambiguity
 *      in addition to noise.  These DO cancel in the double difference:
 *        - rx_clk, ISB, phase_clk are common-mode and cancel in
 *          between-satellite single differencing (step 3).
 *        - ZWD*mf_w and ambiguity are nearly constant between consecutive
 *          epochs and cancel in between-epoch double differencing (step 4).
 *      This keeps the detector independent of Kalman filter state.
 *   2. Select a high-elevation reference satellite. If the reference changes,
 *      apply a median offset correction to the stored previous-epoch gradients
 *      so the between-epoch differences remain continuous.
 *   3. Between-satellite single difference (gradient):
 *          grad_phi_IF(sat) = phi_IF(sat) - phi_IF(ref)
 *   4. Between-epoch double difference:
 *          delta_grad(sat) = grad_phi_IF(sat, t) - grad_phi_IF(sat, t-1)
 *   5. If median(delta_grad of non-ref sats) > 0.5 * lambda_IF:
 *      -> reference satellite likely slipped. Flag ref, correct prev, recompute.
 *   6. Apply iterative MAD-based outlier detection on delta_grad for non-ref sats.
 *   7. Any satellite flagged as outlier -> schedule ambiguity reset.
 *   8. Store grad_phi_IF(t) for next epoch.
 */

#include "IfPhaseSlipDetector.hpp"
#include "Constants.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>

namespace GnssProcessor {

    IfPhaseSlipDetector::IfPhaseSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        auto logger = log_manager_.getLogger("if_slip_detector");
        logger->info("IfPhaseSlipDetector initialized: ref_elev_thresh={:.0f} deg, outlier_sigma={:.1f}, pos_sigma_gate={:.2f} m",
            kRefElevThreshDeg, kOutlierSigmaMultiplier, kPositionSigmaThresholdM);
    }

    bool IfPhaseSlipDetector::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("if_slip_detector");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "if_slip_trace.csv";
        trace_file_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!trace_file_.is_open()) {
            logger->warn("Could not open IF slip trace file '{}'", trace_path.string());
            return false;
        }

        trace_file_ << "week,tow_s,satellite,system,elevation_deg,"
                    << "phase_if_key,phi_if_omc_m,"
                    << "grad_phi_if_m,delta_grad_m,"
                    << "ref_satellite,ref_changed,"
                    << "lambda_if_m,is_slip,slip_reason,"
                    << "pos_sigma_3d_m,pos_gated\n";

        logger->info("IF slip tracing enabled: {}", trace_path.string());
        return true;
    }

    bool IfPhaseSlipDetector::findPhaseIfLabel(const PppObservation& obs, std::string& phase_if_key) const {
        phase_if_key.clear();
        for (const auto& [k, v] : obs.observed) {
            if (k.find('_') != std::string::npos && !k.empty() && k[0] == 'L') {
                phase_if_key = k;
                return true;
            }
        }
        return false;
    }

    bool IfPhaseSlipDetector::splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const {
        sig1.clear();
        sig2.clear();
        const size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= label.size()) return false;
        sig1 = label.substr(0, pos);
        sig2 = label.substr(pos + 1);
        return (!sig1.empty() && !sig2.empty());
    }

    double IfPhaseSlipDetector::computeIfWavelengthMeters(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2) const {

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);

        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) return 0.0;

        const double f1_sq = f1 * f1;
        const double f2_sq = f2 * f2;
        const double denom = f1_sq - f2_sq;
        if (std::abs(denom) < 1e-12) return 0.0;

        // IF combination coefficients
        const double c1 = f1_sq / denom;
        const double c2 = f2_sq / denom;   // positive

        // Per-signal wavelengths
        const double lambda1 = Constants::SPEED_OF_LIGHT / f1;
        const double lambda2 = Constants::SPEED_OF_LIGHT / f2;

        // Effective IF wavelength: c1*lambda1 - c2*lambda2
        // (both c1 and c2 are positive; result is the wavelength of 1 IF cycle)
        const double lambda_if = std::abs(c1 * lambda1 - c2 * lambda2);
        return (std::isfinite(lambda_if) && lambda_if > 0.0) ? lambda_if : 0.0;
    }

    std::map<std::string, double> IfPhaseSlipDetector::checkOutliers(
        const std::map<std::string, double>& values,
        const std::string& ref_satellite,
        double lambda_if) const {

        // Copy input so we can set outliers to NaN
        std::map<std::string, double> result = values;

        for (size_t pass = 0; pass < kMaxOutlierPasses; ++pass) {
            // Collect non-ref, finite values
            std::vector<double> non_ref_vals;
            non_ref_vals.reserve(result.size());
            for (const auto& [sat, v] : result) {
                if (sat != ref_satellite && std::isfinite(v)) {
                    non_ref_vals.push_back(v);
                }
            }
            if (non_ref_vals.empty()) break;

            // MAD computation
            std::sort(non_ref_vals.begin(), non_ref_vals.end());
            const size_t n = non_ref_vals.size();
            const double median_val = (n % 2 == 0)
                ? 0.5 * (non_ref_vals[n / 2 - 1] + non_ref_vals[n / 2])
                : non_ref_vals[n / 2];

            std::vector<double> abs_devs(n);
            for (size_t i = 0; i < n; ++i) {
                abs_devs[i] = std::abs(non_ref_vals[i] - median_val);
            }
            std::sort(abs_devs.begin(), abs_devs.end());
            const double mad = (n % 2 == 0)
                ? 0.5 * (abs_devs[n / 2 - 1] + abs_devs[n / 2])
                : abs_devs[n / 2];

            const double thres_std = kOutlierSigmaMultiplier * 1.4826 * mad;

            // Find max absolute value across all (including ref's value of 0)
            double max_abs_val = 0.0;
            for (const auto& [sat, v] : result) {
                if (std::isfinite(v)) {
                    max_abs_val = std::max(max_abs_val, std::abs(v));
                }
            }

            // Only check if noise level is high or there's a large value
            if (thres_std <= lambda_if / 4.0 && max_abs_val <= 0.5 * lambda_if) {
                break;  // noise is small and no large values -> done
            }

            // Find and remove worst outlier if it exceeds 0.5 * lambda_if
            double worst_val = 0.0;
            std::string worst_sat;
            for (const auto& [sat, v] : result) {
                if (sat == ref_satellite) continue;
                if (!std::isfinite(v)) continue;
                const bool over_adaptive = std::abs(v) > thres_std;
                const bool over_fixed = std::abs(v) > lambda_if;
                if ((over_adaptive || over_fixed) && std::abs(v) > worst_val) {
                    worst_val = std::abs(v);
                    worst_sat = sat;
                }
            }

            if (worst_val > 0.5 * lambda_if && !worst_sat.empty()) {
                result[worst_sat] = std::numeric_limits<double>::quiet_NaN();
            } else {
                break;  // no more outliers above threshold
            }
        }

        return result;
    }

    void IfPhaseSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("if_slip_detector");
        for (auto& [sys, cs] : constellation_state_) {
            auto it = cs.prev_grad.find(satellite);
            if (it != cs.prev_grad.end()) {
                logger->debug("Cleared IF phase history for {} in constellation {}", satellite, sys);
                cs.prev_grad.erase(it);
            }
            if (cs.ref_satellite == satellite) {
                logger->debug("Reference satellite {} for {} was reset, will reselect next epoch", satellite, sys);
                cs.ref_satellite.clear();
                cs.ref_elevation = 0.0;
            }
        }
    }

    void IfPhaseSlipDetector::resetAll() {
        constellation_state_.clear();
        log_manager_.getLogger("if_slip_detector")->debug("All IF phase slip state cleared");
    }

    std::vector<std::string> IfPhaseSlipDetector::detect(const std::vector<PppObservation>& observations,
        double position_sigma_3d) {

        std::vector<std::string> reset_keys;
        auto logger = log_manager_.getLogger("if_slip_detector");

        // Capture epoch time from first valid observation
        for (const auto& obs : observations) {
            if (!obs.satellite.empty()) {
                current_epoch_ = obs.time;
                break;
            }
        }

        // Position-uncertainty gating: when the 3D position sigma exceeds the
        // threshold, the differential geometric error (e_sat - e_ref) · ?pos
        // can be many times the IF wavelength.  We still compute and store
        // gradients so that the between-epoch continuity is maintained, but we
        // suppress all outlier detection to avoid false slip flags.
        const bool pos_gated = (position_sigma_3d > kPositionSigmaThresholdM);
        if (pos_gated) {
            logger->debug("IF slip detection gated: position_sigma_3d={:.3f} m > threshold={:.2f} m",
                position_sigma_3d, kPositionSigmaThresholdM);
        }

        // Group observations by constellation
        struct SatData {
            std::string satellite;
            std::string phase_if_key;
            std::string sig1, sig2;
            double phi_if_omc;      // observed - modeled for IF phase
            double elevation;
            double lambda_if;
            System system;
        };

        std::map<std::string, std::vector<SatData>> by_constellation;

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;

            std::string phase_if_key;
            if (!findPhaseIfLabel(obs, phase_if_key)) continue;

            std::string sig1, sig2;
            if (!splitIfLabel(phase_if_key, sig1, sig2)) continue;

            // IF phase OMC = observed - modeled.
            // The model subtraction is essential: it removes the satellite-specific
            // geometric range (~20,000 km, changing ~10 km/30s) and satellite clock
            // which do NOT cancel in between-satellite differencing.
            // After subtraction the OMC contains rx_clk + ZWD*mf_w + ISB +
            // phase_clk + ambiguity + noise, all of which DO cancel:
            //   - Between-satellite diff cancels rx_clk, ISB, phase_clk (common-mode).
            //   - Between-epoch diff cancels ZWD*mf_w (slowly varying) and ambiguity (constant per arc).
            auto obs_it = obs.observed.find(phase_if_key);
            auto mod_it = obs.modeled.find(phase_if_key);
            if (obs_it == obs.observed.end() || mod_it == obs.modeled.end()) continue;

            const double phi_if_omc = obs_it->second - mod_it->second;
            if (!std::isfinite(phi_if_omc)) continue;

            const double lambda_if = computeIfWavelengthMeters(obs, sig1, sig2);
            if (lambda_if <= 0.0) continue;

            const std::string sys_str = PppUtils::getSystemString(obs.system);

            SatData sd;
            sd.satellite = obs.satellite;
            sd.phase_if_key = phase_if_key;
            sd.sig1 = sig1;
            sd.sig2 = sig2;
            sd.phi_if_omc = phi_if_omc;
            sd.elevation = obs.elevation;
            sd.lambda_if = lambda_if;
            sd.system = obs.system;

            by_constellation[sys_str].push_back(std::move(sd));
        }

        // Process each constellation independently
        for (auto& [sys_str, sat_vec] : by_constellation) {
            if (sat_vec.size() < 2) continue;  // need at least 2 satellites

            auto& cs = constellation_state_[sys_str];

            // Compute median IF wavelength for this constellation
            std::vector<double> if_wavelengths;
            if_wavelengths.reserve(sat_vec.size());
            for (const auto& sd : sat_vec) {
                if_wavelengths.push_back(sd.lambda_if);
            }
            std::sort(if_wavelengths.begin(), if_wavelengths.end());
            const size_t nw = if_wavelengths.size();
            const double lambda_if_median = (nw % 2 == 0)
                ? 0.5 * (if_wavelengths[nw / 2 - 1] + if_wavelengths[nw / 2])
                : if_wavelengths[nw / 2];

            // Build set of current satellite IDs
            std::map<std::string, const SatData*> sat_map;
            for (const auto& sd : sat_vec) {
                sat_map[sd.satellite] = &sd;
            }

            // --- Reference satellite selection ---
            bool ref_changed = false;
            const bool ref_present = !cs.ref_satellite.empty() &&
                sat_map.count(cs.ref_satellite) > 0;
            const bool ref_below_threshold = ref_present &&
                sat_map.at(cs.ref_satellite)->elevation < kRefElevThreshDeg;

            if (!ref_present || ref_below_threshold) {
                // Select highest-elevation satellite as new reference
                std::string best_sat;
                double best_elev = -1.0;
                for (const auto& sd : sat_vec) {
                    if (sd.elevation > best_elev) {
                        best_elev = sd.elevation;
                        best_sat = sd.satellite;
                    }
                }

                if (!cs.ref_satellite.empty() && best_sat != cs.ref_satellite) {
                    ref_changed = true;
                }
                cs.ref_satellite = best_sat;
                cs.ref_elevation = best_elev;
            } else {
                cs.ref_elevation = sat_map.at(cs.ref_satellite)->elevation;
            }

            // Find reference in sat_vec
            const SatData* ref_data = nullptr;
            for (const auto& sd : sat_vec) {
                if (sd.satellite == cs.ref_satellite) {
                    ref_data = &sd;
                    break;
                }
            }
            if (!ref_data) continue;

            // --- Between-satellite single differencing ---
            // grad_phi_IF(sat) = phi_IF_OMC(sat) - phi_IF_OMC(ref)
            // Since the model removed the satellite-specific geometric range and
            // satellite clock, the OMC is ~O(m) (dominated by rx_clk + ambiguity).
            // The between-satellite diff cancels rx_clk, ISB, phase_clk, leaving
            // only differential ZWD*mf_w + ambiguity difference + noise (~cm).
            std::map<std::string, double> grad_phi_if;
            for (const auto& sd : sat_vec) {
                grad_phi_if[sd.satellite] = sd.phi_if_omc - ref_data->phi_if_omc;
            }

            // --- Between-epoch double differencing ---
            std::map<std::string, double> delta_grad;
            bool have_prev = !cs.prev_grad.empty();

            if (have_prev && !pos_gated) {
                // Handle reference satellite change: apply median offset
                if (ref_changed) {
                    // Compute temporary double differences with new ref
                    std::map<std::string, double> temp_delta;
                    for (const auto& [sat, g] : grad_phi_if) {
                        auto prev_it = cs.prev_grad.find(sat);
                        if (prev_it != cs.prev_grad.end()) {
                            temp_delta[sat] = g - prev_it->second;
                        }
                    }

                    // Compute median offset from all satellites that have both epochs
                    std::vector<double> offset_vals;
                    for (const auto& [sat, dd] : temp_delta) {
                        if (std::isfinite(dd)) {
                            offset_vals.push_back(dd);
                        }
                    }
                    if (!offset_vals.empty()) {
                        std::sort(offset_vals.begin(), offset_vals.end());
                        const size_t nm = offset_vals.size();
                        const double offset_median = (nm % 2 == 0)
                            ? 0.5 * (offset_vals[nm / 2 - 1] + offset_vals[nm / 2])
                            : offset_vals[nm / 2];

                        // Correct the previous gradients
                        for (auto& [sat, prev_g] : cs.prev_grad) {
                            prev_g += offset_median;
                        }

                        logger->debug("{}: Reference changed to {} (elev {:.1f}), applied median offset {:.6f} m",
                            sys_str, cs.ref_satellite, cs.ref_elevation, offset_median);
                    }
                }

                // Compute double differences
                bool all_finite = true;
                for (const auto& [sat, g] : grad_phi_if) {
                    auto prev_it = cs.prev_grad.find(sat);
                    if (prev_it != cs.prev_grad.end()) {
                        delta_grad[sat] = g - prev_it->second;
                    } else {
                        delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();
                        all_finite = false;
                    }
                }

                // Check if all double differences are finite
                for (const auto& [sat, dd] : delta_grad) {
                    if (!std::isfinite(dd)) {
                        all_finite = false;
                        break;
                    }
                }

                if (all_finite && delta_grad.size() >= 2) {
                    // --- Reference satellite slip check ---
                    std::vector<double> non_ref_dds;
                    for (const auto& [sat, dd] : delta_grad) {
                        if (sat != cs.ref_satellite && std::isfinite(dd)) {
                            non_ref_dds.push_back(dd);
                        }
                    }

                    if (!non_ref_dds.empty()) {
                        std::sort(non_ref_dds.begin(), non_ref_dds.end());
                        const size_t nm = non_ref_dds.size();
                        const double median_dd = (nm % 2 == 0)
                            ? 0.5 * (non_ref_dds[nm / 2 - 1] + non_ref_dds[nm / 2])
                            : non_ref_dds[nm / 2];

                        if (std::abs(median_dd) > 0.5 * lambda_if_median) {
                            const std::string ref_amb_key = cs.ref_satellite + "_" + ref_data->phase_if_key;
                            reset_keys.push_back(ref_amb_key);

                            logger->warn("{}: Reference satellite {} IF phase slip detected: "
                                "median(delta_grad)={:.4f} m > 0.5*lambda_IF={:.4f} m; reset {}",
                                sys_str, cs.ref_satellite, median_dd,
                                0.5 * lambda_if_median, ref_amb_key);

                            if (trace_file_.is_open()) {
                                trace_file_ << std::fixed << std::setprecision(3)
                                    << current_epoch_.week << "," << current_epoch_.tow << ","
                                    << cs.ref_satellite << "," << sys_str << ","
                                    << std::setprecision(2) << cs.ref_elevation << ","
                                    << ref_data->phase_if_key << ","
                                    << std::setprecision(6) << ref_data->phi_if_omc << ","
                                    << 0.0 << "," << median_dd << ","
                                    << cs.ref_satellite << "," << 0 << ","
                            << lambda_if_median << "," << 1 << ",ref_slip,"
                                    << std::setprecision(3) << position_sigma_3d << "," << 0 << "\n";
                            }

                            // Correct prev gradients for non-ref sats
                            for (auto& [sat, prev_g] : cs.prev_grad) {
                                if (sat != cs.ref_satellite) {
                                    prev_g += median_dd;
                                }
                            }

                            // Recompute double differences after correction
                            delta_grad.clear();
                            for (const auto& [sat, g] : grad_phi_if) {
                                auto prev_it = cs.prev_grad.find(sat);
                                if (prev_it != cs.prev_grad.end()) {
                                    delta_grad[sat] = g - prev_it->second;
                                } else {
                                    delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();
                                }
                            }
                        }

                        // --- Non-reference satellite outlier check ---
                        auto cleaned = checkOutliers(delta_grad, cs.ref_satellite, lambda_if_median);

                        for (const auto& [sat, dd] : cleaned) {
                            if (sat == cs.ref_satellite) continue;
                            if (!std::isfinite(dd) && std::isfinite(delta_grad[sat])) {
                                auto sat_it = sat_map.find(sat);
                                if (sat_it != sat_map.end()) {
                                    const std::string amb_key = sat + "_" + sat_it->second->phase_if_key;
                                    reset_keys.push_back(amb_key);

                                    logger->warn("{}: IF phase slip detected for {}: "
                                        "delta_grad={:.4f} m, lambda_IF={:.4f} m; reset {}",
                                        sys_str, sat, delta_grad[sat], lambda_if_median, amb_key);

                                    if (trace_file_.is_open()) {
                                        trace_file_ << std::fixed << std::setprecision(3)
                                            << current_epoch_.week << "," << current_epoch_.tow << ","
                                            << sat << "," << sys_str << ","
                                            << std::setprecision(2) << sat_it->second->elevation << ","
                                            << sat_it->second->phase_if_key << ","
                                            << std::setprecision(6) << sat_it->second->phi_if_omc << ","
                                            << grad_phi_if.at(sat) << "," << delta_grad.at(sat) << ","
                                            << cs.ref_satellite << "," << (ref_changed ? 1 : 0) << ","
                                            << lambda_if_median << "," << 1 << ",non_ref_outlier,"
                                            << std::setprecision(3) << position_sigma_3d << "," << 0 << "\n";
                                    }
                                }
                            }
                        }
                    }
                } else {
                    // Not all finite -> apply fixed threshold on whatever we have
                    for (const auto& [sat, dd] : delta_grad) {
                        if (std::isfinite(dd) && std::abs(dd) > lambda_if_median) {
                            auto sat_it = sat_map.find(sat);
                            if (sat_it != sat_map.end()) {
                                const std::string amb_key = sat + "_" + sat_it->second->phase_if_key;
                                reset_keys.push_back(amb_key);

                                logger->warn("{}: IF phase slip (fixed threshold) for {}: "
                                    "|delta_grad|={:.4f} m > lambda_IF={:.4f} m; reset {}",
                                    sys_str, sat, std::abs(dd), lambda_if_median, amb_key);
                            }
                        }
                    }
                }
            } else if (have_prev && pos_gated) {
                // Position is too uncertain for reliable detection.
                // Still compute double differences for tracing but do NOT flag slips.
                for (const auto& [sat, g] : grad_phi_if) {
                    auto prev_it = cs.prev_grad.find(sat);
                    if (prev_it != cs.prev_grad.end()) {
                        delta_grad[sat] = g - prev_it->second;
                    } else {
                        delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();
                    }
                }
            }

            // --- Write trace records for all satellites ---
            if (trace_file_.is_open()) {
                for (const auto& sd : sat_vec) {
                    bool was_reset = false;
                    for (const auto& key : reset_keys) {
                        if (key.find(sd.satellite) == 0) { was_reset = true; break; }
                    }
                    if (was_reset) continue;

                    const double g = grad_phi_if.count(sd.satellite) ? grad_phi_if.at(sd.satellite)
                        : std::numeric_limits<double>::quiet_NaN();
                    const double dd = delta_grad.count(sd.satellite) ? delta_grad.at(sd.satellite)
                        : std::numeric_limits<double>::quiet_NaN();

                    trace_file_ << std::fixed << std::setprecision(3)
                        << current_epoch_.week << "," << current_epoch_.tow << ","
                        << sd.satellite << "," << sys_str << ","
                        << std::setprecision(2) << sd.elevation << ","
                        << sd.phase_if_key << ","
                        << std::setprecision(6) << sd.phi_if_omc << ","
                        << g << "," << dd << ","
                        << cs.ref_satellite << "," << (ref_changed ? 1 : 0) << ","
                        << lambda_if_median << "," << 0
                        << (pos_gated ? ",pos_gated," : ",ok,")
                        << std::setprecision(3) << position_sigma_3d << "," << (pos_gated ? 1 : 0) << "\n";
                }
            }

            // --- Store current gradients for next epoch ---
            cs.prev_grad.clear();
            for (const auto& [sat, g] : grad_phi_if) {
                bool was_reset = false;
                for (const auto& key : reset_keys) {
                    if (key.find(sat + "_") == 0) { was_reset = true; break; }
                }
                if (!was_reset) {
                    cs.prev_grad[sat] = g;
                }
            }
        }

        return reset_keys;
    }

} // namespace GnssProcessor
