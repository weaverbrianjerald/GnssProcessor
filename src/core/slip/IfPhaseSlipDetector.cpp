/*!
 * \file IfPhaseSlipDetector.cpp
 * \brief Between-satellite, between-epoch IF phase slip detector.
 *
 * Organization:
 *  - Construction / tracing
 *  - Signal helpers
 *  - Statistics (median, MAD outlier check)
 *  - Observation grouping / reference selection
 *  - Double-difference processing
 *  - Top-level detect()
 *  - External reset
 */

#include "slip/IfPhaseSlipDetector.hpp"
#include "common/Constants.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>

namespace GnssProcessor {

    // =================================================================
    // Construction / tracing
    // =================================================================

    IfPhaseSlipDetector::IfPhaseSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("if_slip_detector")->info(
            "IfPhaseSlipDetector initialized: ref_elev_thresh={:.0f} deg, outlier_sigma={:.1f}, pos_sigma_gate={:.2f} m",
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

    void IfPhaseSlipDetector::writeTraceRecord(const SatData& sd,
        const std::string& sys_str,
        double grad, double delta_grad,
        const std::string& ref_sat, bool ref_changed,
        double lambda_if, bool is_slip,
        const std::string& slip_reason,
        double position_sigma_3d, bool pos_gated) const {

        auto logger = log_manager_.getLogger("if_slip_detector");

        if (is_slip) {
            logger->info("[W{} {:.3f}] {} {} elev={:.1f} grad={:.6f} delta_grad={:.6f} "
                "ref={} lambda_if={:.6f} slip_reason={} pos_sigma={:.3f}{}",
                current_epoch_.week, current_epoch_.tow,
                sd.satellite, sys_str, sd.elevation,
                grad, delta_grad, ref_sat, lambda_if, slip_reason,
                position_sigma_3d, pos_gated ? " [pos_gated]" : "");
        } else {
            logger->debug("[W{} {:.3f}] {} {} elev={:.1f} grad={:.6f} delta_grad={:.6f} "
                "ref={}{} lambda_if={:.6f} reason={} pos_sigma={:.3f}{}",
                current_epoch_.week, current_epoch_.tow,
                sd.satellite, sys_str, sd.elevation,
                grad, delta_grad, ref_sat,
                ref_changed ? " [changed]" : "", lambda_if, slip_reason,
                position_sigma_3d, pos_gated ? " [pos_gated]" : "");
        }

        if (!trace_file_.is_open()) return;

        trace_file_ << std::fixed << std::setprecision(3)
            << current_epoch_.week << "," << current_epoch_.tow << ","
            << sd.satellite << "," << sys_str << ","
            << std::setprecision(2) << sd.elevation << ","
            << sd.phase_if_key << ","
            << std::setprecision(6) << sd.phi_if_omc << ","
            << grad << "," << delta_grad << ","
            << ref_sat << "," << (ref_changed ? 1 : 0) << ","
            << lambda_if << "," << (is_slip ? 1 : 0) << "," << slip_reason << ","
            << std::setprecision(3) << position_sigma_3d << "," << (pos_gated ? 1 : 0) << "\n";
    }

    // =================================================================
    // Signal helpers
    // =================================================================

    bool IfPhaseSlipDetector::findPhaseIfLabel(const PppObservation& obs, std::string& phase_if_key) const {
        phase_if_key.clear();
        for (const auto& [k, v] : obs.observed)
            if (k.find('_') != std::string::npos && !k.empty() && k[0] == 'L') {
                phase_if_key = k;
                return true;
            }
        return false;
    }

    bool IfPhaseSlipDetector::splitIfLabel(const std::string& label,
        std::string& sig1, std::string& sig2) const {
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

        const double denom = f1 * f1 - f2 * f2;
        if (std::abs(denom) < 1e-12) return 0.0;

        const double c1 = (f1 * f1) / denom;
        const double c2 = (f2 * f2) / denom;
        const double lambda_if = std::abs(c1 * Constants::SPEED_OF_LIGHT / f1
                                        - c2 * Constants::SPEED_OF_LIGHT / f2);
        return (std::isfinite(lambda_if) && lambda_if > 0.0) ? lambda_if : 0.0;
    }

    // =================================================================
    // Statistics (median, MAD outlier check)
    // =================================================================

    double IfPhaseSlipDetector::computeMedian(std::vector<double>& values) {
        if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        return (n % 2 == 0) ? 0.5 * (values[n / 2 - 1] + values[n / 2])
                            : values[n / 2];
    }

    std::map<std::string, double> IfPhaseSlipDetector::checkOutliers(
        const std::map<std::string, double>& values,
        const std::string& ref_satellite,
        double lambda_if) const {

        std::map<std::string, double> result = values;

        for (size_t pass = 0; pass < kMaxOutlierPasses; ++pass) {
            // Collect non-ref, finite values
            std::vector<double> non_ref_vals;
            for (const auto& [sat, v] : result)
                if (sat != ref_satellite && std::isfinite(v))
                    non_ref_vals.push_back(v);
            if (non_ref_vals.empty()) break;

            // MAD-based adaptive threshold
            const double median_val = computeMedian(non_ref_vals);
            std::vector<double> abs_devs(non_ref_vals.size());
            for (size_t i = 0; i < non_ref_vals.size(); ++i)
                abs_devs[i] = std::abs(non_ref_vals[i] - median_val);
            const double mad = computeMedian(abs_devs);
            const double thres_std = kOutlierSigmaMultiplier * 1.4826 * mad;

            // Max absolute value across all entries
            double max_abs_val = 0.0;
            for (const auto& [sat, v] : result)
                if (std::isfinite(v))
                    max_abs_val = std::max(max_abs_val, std::abs(v));

            if (thres_std <= lambda_if / 4.0 && max_abs_val <= 0.5 * lambda_if)
                break;

            // Remove worst outlier exceeding 0.5 * lambda_if
            double worst_val = 0.0;
            std::string worst_sat;
            for (const auto& [sat, v] : result) {
                if (sat == ref_satellite || !std::isfinite(v)) continue;
                if ((std::abs(v) > thres_std || std::abs(v) > lambda_if) &&
                    std::abs(v) > worst_val) {
                    worst_val = std::abs(v);
                    worst_sat = sat;
                }
            }

            if (worst_val > 0.5 * lambda_if && !worst_sat.empty())
                result[worst_sat] = std::numeric_limits<double>::quiet_NaN();
            else
                break;
        }

        return result;
    }

    // =================================================================
    // Observation grouping / reference selection
    // =================================================================

    std::map<std::string, std::vector<IfPhaseSlipDetector::SatData>>
    IfPhaseSlipDetector::groupByConstellation(const std::vector<PppObservation>& observations) const {

        std::map<std::string, std::vector<SatData>> result;

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;

            std::string phase_if_key;
            if (findPhaseIfLabel(obs, phase_if_key)) {
                // --- Combined (PPP) path: IF key already exists ---
                std::string sig1, sig2;
                if (!splitIfLabel(phase_if_key, sig1, sig2)) continue;

                auto obs_it = obs.observed.find(phase_if_key);
                auto mod_it = obs.modeled.find(phase_if_key);
                if (obs_it == obs.observed.end() || mod_it == obs.modeled.end()) continue;

                const double phi_if_omc = obs_it->second - mod_it->second;
                if (!std::isfinite(phi_if_omc)) continue;

                const double lambda_if = computeIfWavelengthMeters(obs, sig1, sig2);
                if (lambda_if <= 0.0) continue;

                result[PppUtils::getSystemString(obs.system)].push_back(
                    { obs.satellite, phase_if_key, sig1, sig2,
                      phi_if_omc, obs.elevation, lambda_if, obs.system, obs.los });
            } else {
                // --- Uncombined (PPP-RTK) path: synthesize IF OMC ---
                // Find two phase signals on different frequency bands.
                std::vector<std::tuple<std::string, double, double>> phase_signals; // sig, observed, modeled
                for (const auto& [key, val] : obs.observed) {
                    if (key.empty() || key[0] != 'L') continue;
                    if (key.find('_') != std::string::npos) continue; // skip IF combinations
                    auto mod_it = obs.modeled.find(key);
                    if (mod_it == obs.modeled.end()) continue;
                    phase_signals.emplace_back(key, val, mod_it->second);
                }
                if (phase_signals.size() < 2) continue;

                // Use first two phase signals to form IF combination.
                const auto& [sig1, obs1, mod1] = phase_signals[0];
                const auto& [sig2, obs2, mod2] = phase_signals[1];

                const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
                const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);
                if (!std::isfinite(f1) || !std::isfinite(f2) || f1 <= 0.0 || f2 <= 0.0) continue;

                const double f1sq = f1 * f1;
                const double f2sq = f2 * f2;
                const double denom = f1sq - f2sq;
                if (std::abs(denom) < 1e-12) continue;

                const double c1 = f1sq / denom;
                const double c2 = -f2sq / denom;

                const double omc1 = obs1 - mod1;
                const double omc2 = obs2 - mod2;
                const double phi_if_omc = c1 * omc1 + c2 * omc2;
                if (!std::isfinite(phi_if_omc)) continue;

                const double lambda_if = computeIfWavelengthMeters(obs, sig1, sig2);
                if (lambda_if <= 0.0) continue;

                const std::string synth_key = sig1 + "_" + sig2;

                result[PppUtils::getSystemString(obs.system)].push_back(
                    { obs.satellite, synth_key, sig1, sig2,
                      phi_if_omc, obs.elevation, lambda_if, obs.system, obs.los });
            }
        }

        return result;
    }

    bool IfPhaseSlipDetector::selectReference(const std::vector<SatData>& sat_vec,
        ConstellationState& cs) const {

        // Check if current reference is still present and above threshold
        bool ref_valid = false;
        for (const auto& sd : sat_vec)
            if (sd.satellite == cs.ref_satellite && sd.elevation >= kRefElevThreshDeg) {
                ref_valid = true;
                cs.ref_elevation = sd.elevation;
                break;
            }

        if (ref_valid) return false;

        // Select highest-elevation satellite
        const std::string old_ref = cs.ref_satellite;
        cs.ref_satellite.clear();
        cs.ref_elevation = -1.0;
        for (const auto& sd : sat_vec)
            if (sd.elevation > cs.ref_elevation) {
                cs.ref_elevation = sd.elevation;
                cs.ref_satellite = sd.satellite;
            }

        return !old_ref.empty() && cs.ref_satellite != old_ref;
    }

    // =================================================================
    // Double-difference processing
    // =================================================================

    std::map<std::string, double> IfPhaseSlipDetector::processDoubleDifferences(
        const std::vector<SatData>& sat_vec,
        const std::string& sys_str,
        ConstellationState& cs,
        const std::map<std::string, double>& grad_phi_if,
        double lambda_if,
        bool ref_changed,
        double position_sigma_3d,
        const Eigen::Vector3d& delta_pos,
        std::vector<std::string>& reset_keys) {

        auto logger = log_manager_.getLogger("if_slip_detector");
        const bool pos_gated = (position_sigma_3d > kPositionSigmaThresholdM);

        // Build sat lookup
        std::map<std::string, const SatData*> sat_map;
        for (const auto& sd : sat_vec) sat_map[sd.satellite] = &sd;

        // Find reference SatData
        const SatData* ref_data = sat_map.count(cs.ref_satellite) ? sat_map.at(cs.ref_satellite) : nullptr;
        if (!ref_data) return {};

        // --- Handle reference change: apply median offset to prev gradients ---
        if (ref_changed && !cs.prev_grad.empty()) {
            std::vector<double> offsets;
            for (const auto& [sat, g] : grad_phi_if) {
                auto prev_it = cs.prev_grad.find(sat);
                if (prev_it != cs.prev_grad.end())
                    offsets.push_back(g - prev_it->second);
            }
            if (!offsets.empty()) {
                const double offset = computeMedian(offsets);
                for (auto& [sat, prev_g] : cs.prev_grad) prev_g += offset;
                logger->debug("{}: Reference changed to {} (elev {:.1f}), applied median offset {:.6f} m",
                    sys_str, cs.ref_satellite, cs.ref_elevation, offset);
            }
        }

        // --- Compute between-epoch double differences ---
        std::map<std::string, double> delta_grad;
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
        for (const auto& [sat, dd] : delta_grad)
            if (!std::isfinite(dd)) { all_finite = false; break; }

        // --- Geometry correction: remove (e_sat - e_ref) · ?pos ---
        // The between-satellite single-difference does NOT perfectly cancel
        // geometry.  The epoch-to-epoch position change produces a residual
        // of (e_sat - e_ref) · ?pos in each double difference.  Correct it
        // using the current-epoch LOS vectors and the position change.
        if (delta_pos.norm() > 1e-6) {
            const Eigen::Vector3d& ref_los = ref_data->los;
            for (auto& [sat, dd] : delta_grad) {
                if (!std::isfinite(dd)) continue;
                auto sit = sat_map.find(sat);
                if (sit == sat_map.end()) continue;
                const double geom_correction = (sit->second->los - ref_los).dot(delta_pos);
                dd -= geom_correction;
            }
        }

        if (pos_gated) {
            // Return geometry-corrected DDs for tracing/caching but suppress slip flagging
            return delta_grad;
        }

        // --- Reference satellite slip check (runs regardless of all_finite) ---
        // Collect finite non-reference double differences. If the median is
        // large, the reference satellite slipped  not the non-ref satellites.
        // This must run before individual outlier detection so that the
        // common-mode reference slip is removed first.
        {
            std::vector<double> non_ref_dds;
            for (const auto& [sat, dd] : delta_grad)
                if (sat != cs.ref_satellite && std::isfinite(dd))
                    non_ref_dds.push_back(dd);

            if (non_ref_dds.size() >= 2) {
                const double median_dd = computeMedian(non_ref_dds);

                if (std::abs(median_dd) > 0.5 * lambda_if) {
                    const std::string ref_amb_key = cs.ref_satellite + "_" + ref_data->phase_if_key;
                    reset_keys.push_back(ref_amb_key);

                    logger->warn("{}: Reference satellite {} IF phase slip: "
                        "median(delta_grad)={:.4f} m ({:.1f} IF cyc) > 0.5*lambda_IF={:.4f} m "
                        "(n_finite={}); reset {}",
                        sys_str, cs.ref_satellite, median_dd,
                        median_dd / lambda_if, 0.5 * lambda_if,
                        non_ref_dds.size(), ref_amb_key);

                    writeTraceRecord(*ref_data, sys_str, 0.0, median_dd,
                        cs.ref_satellite, false, lambda_if, true, "ref_slip",
                        position_sigma_3d, false);

                    // Correct prev gradients by the median offset and recompute
                    // double diffs so that subsequent outlier detection sees only
                    // the per-satellite residual after removing the reference slip.
                    for (auto& [sat, prev_g] : cs.prev_grad)
                        if (sat != cs.ref_satellite) prev_g += median_dd;

                    delta_grad.clear();
                    all_finite = true;
                    for (const auto& [sat, g] : grad_phi_if) {
                        auto prev_it = cs.prev_grad.find(sat);
                        if (prev_it != cs.prev_grad.end()) {
                            delta_grad[sat] = g - prev_it->second;
                        } else {
                            delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();
                            all_finite = false;
                        }
                    }
                }
            }
        }

        // --- Non-reference outlier detection ---
        if (all_finite && delta_grad.size() >= 2) {
            // Full MAD-based adaptive outlier check
            auto cleaned = checkOutliers(delta_grad, cs.ref_satellite, lambda_if);
            for (const auto& [sat, dd] : cleaned) {
                if (sat == cs.ref_satellite) continue;
                if (!std::isfinite(dd) && std::isfinite(delta_grad[sat])) {
                    auto sit = sat_map.find(sat);
                    if (sit == sat_map.end()) continue;
                    const std::string amb_key = sat + "_" + sit->second->phase_if_key;
                    reset_keys.push_back(amb_key);

                    logger->warn("{}: IF phase slip for {}: delta_grad={:.4f} m, lambda_IF={:.4f} m; reset {}",
                        sys_str, sat, delta_grad[sat], lambda_if, amb_key);

                    writeTraceRecord(*sit->second, sys_str,
                        grad_phi_if.at(sat), delta_grad.at(sat),
                        cs.ref_satellite, ref_changed, lambda_if,
                        true, "non_ref_outlier", position_sigma_3d, false);
                }
            }
        } else {
            // Incomplete data: apply fixed threshold on available diffs
            for (const auto& [sat, dd] : delta_grad) {
                if (!std::isfinite(dd) || std::abs(dd) <= lambda_if) continue;
                auto sit = sat_map.find(sat);
                if (sit == sat_map.end()) continue;
                const std::string amb_key = sat + "_" + sit->second->phase_if_key;
                reset_keys.push_back(amb_key);

                logger->warn("{}: IF phase slip (fixed threshold) for {}: |delta_grad|={:.4f} m > lambda_IF={:.4f} m; reset {}",
                    sys_str, sat, std::abs(dd), lambda_if, amb_key);

                writeTraceRecord(*sit->second, sys_str,
                    grad_phi_if.count(sat) ? grad_phi_if.at(sat)
                        : std::numeric_limits<double>::quiet_NaN(),
                    dd, cs.ref_satellite, ref_changed, lambda_if,
                    true, "fixed_threshold", position_sigma_3d, false);
            }
        }

        return delta_grad;
    }

    // =================================================================
    // Top-level detect()
    // =================================================================

    std::vector<std::string> IfPhaseSlipDetector::detect(
        const std::vector<PppObservation>& observations,
        double position_sigma_3d,
        const Eigen::Vector3d& receiver_position) {

        std::vector<std::string> reset_keys;
        auto logger = log_manager_.getLogger("if_slip_detector");
        last_if_jumps_.clear();

        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        const bool pos_gated = (position_sigma_3d > kPositionSigmaThresholdM);
        if (pos_gated)
            logger->debug("IF slip detection gated: position_sigma_3d={:.3f} m > threshold={:.2f} m",
                position_sigma_3d, kPositionSigmaThresholdM);

        auto by_constellation = groupByConstellation(observations);

        for (auto& [sys_str, sat_vec] : by_constellation) {
            if (sat_vec.size() < 2) continue;

            auto& cs = constellation_state_[sys_str];

            // Compute epoch-to-epoch position change for geometry correction.
            Eigen::Vector3d delta_pos = Eigen::Vector3d::Zero();
            if (cs.has_prev_position && receiver_position.norm() > 1e-6) {
                delta_pos = receiver_position - cs.prev_position;
            }

            // Median IF wavelength
            std::vector<double> lambdas;
            lambdas.reserve(sat_vec.size());
            for (const auto& sd : sat_vec) lambdas.push_back(sd.lambda_if);
            const double lambda_if = computeMedian(lambdas);

            // Reference selection
            const bool ref_changed = selectReference(sat_vec, cs);

            // Find reference data
            const SatData* ref_data = nullptr;
            for (const auto& sd : sat_vec)
                if (sd.satellite == cs.ref_satellite) { ref_data = &sd; break; }
            if (!ref_data) continue;

            // Between-satellite single differencing
            std::map<std::string, double> grad_phi_if;
            for (const auto& sd : sat_vec)
                grad_phi_if[sd.satellite] = sd.phi_if_omc - ref_data->phi_if_omc;

            // Double-difference processing (if previous epoch exists).
            // Returns geometry-corrected, reference-slip-corrected delta_grads.
            std::map<std::string, double> delta_grad;
            if (!cs.prev_grad.empty())
                delta_grad = processDoubleDifferences(sat_vec, sys_str, cs, grad_phi_if,
                    lambda_if, ref_changed, position_sigma_3d, delta_pos, reset_keys);

            // For satellites without a previous epoch, fill NaN
            for (const auto& [sat, g] : grad_phi_if)
                if (delta_grad.find(sat) == delta_grad.end())
                    delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();

            // Build elevation lookup for caching
            std::map<std::string, double> sat_elev;
            for (const auto& sd : sat_vec)
                sat_elev[sd.satellite] = sd.elevation;

            // Write trace records for non-slip satellites
            if (trace_file_.is_open()) {
                for (const auto& sd : sat_vec) {
                    bool was_reset = false;
                    for (const auto& key : reset_keys)
                        if (key.find(sd.satellite) == 0) { was_reset = true; break; }
                    if (was_reset) continue;

                    writeTraceRecord(sd, sys_str,
                        grad_phi_if.count(sd.satellite) ? grad_phi_if.at(sd.satellite)
                            : std::numeric_limits<double>::quiet_NaN(),
                        delta_grad.count(sd.satellite) ? delta_grad.at(sd.satellite)
                            : std::numeric_limits<double>::quiet_NaN(),
                        cs.ref_satellite, ref_changed, lambda_if,
                        false, pos_gated ? "pos_gated" : "ok",
                        position_sigma_3d, pos_gated);
                }
            }

            // Cache IF jump values for external query (joint MW+IF repair)
            for (const auto& [sat, dd] : delta_grad) {
                IfJumpInfo info;
                info.delta_grad_m = dd;
                info.lambda_if_m = lambda_if;
                info.elevation_deg = sat_elev.count(sat) ? sat_elev.at(sat) : 0.0;
                info.valid = std::isfinite(dd);
                last_if_jumps_[sat] = info;
            }

            // Store gradients for next epoch (exclude reset satellites)
            cs.prev_grad.clear();
            cs.prev_los.clear();
            for (const auto& [sat, g] : grad_phi_if) {
                bool was_reset = false;
                for (const auto& key : reset_keys)
                    if (key.find(sat + "_") == 0) { was_reset = true; break; }
                if (!was_reset) {
                    cs.prev_grad[sat] = g;
                    auto sit_it = std::find_if(sat_vec.begin(), sat_vec.end(),
                        [&](const SatData& sd) { return sd.satellite == sat; });
                    if (sit_it != sat_vec.end())
                        cs.prev_los[sat] = sit_it->los;
                }
            }

            // Store receiver position for geometry correction on next epoch.
            if (receiver_position.norm() > 1e-6) {
                cs.prev_position = receiver_position;
                cs.has_prev_position = true;
            }
        }

        return reset_keys;
    }

    // =================================================================
    // External reset
    // =================================================================

    void IfPhaseSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("if_slip_detector");
        for (auto& [sys, cs] : constellation_state_) {
            auto it = cs.prev_grad.find(satellite);
            if (it != cs.prev_grad.end()) {
                logger->debug("Cleared IF phase history for {} in constellation {}", satellite, sys);
                cs.prev_grad.erase(it);
                cs.prev_los.erase(satellite);
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
        last_if_jumps_.clear();
        log_manager_.getLogger("if_slip_detector")->debug("All IF phase slip state cleared");
    }

    IfPhaseSlipDetector::IfJumpInfo IfPhaseSlipDetector::getLastIfJump(
        const std::string& satellite) const {
        auto it = last_if_jumps_.find(satellite);
        if (it != last_if_jumps_.end())
            return it->second;
        return {};
    }

} // namespace GnssProcessor
