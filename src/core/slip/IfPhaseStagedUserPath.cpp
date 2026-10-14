/*!
 * \file IfPhaseStagedUserPath.cpp
 * \brief STAGED: user path, no config reaches it.  The branches of
 *        IfPhaseSlipDetector written for the user and kinematic paths, which
 *        no shipped configuration reaches.
 *
 * The detector is built only by network setup (ApplicationNetworkSetup.cpp,
 * `network.cycle_slip.if_phase_td`); the user toggle has no instantiation
 * path (the header's note).  On the network path:
 *  - flagging is always deferred to the network pool (setDeferFlagging(true)
 *    at construction), so the per-station outlier flagging
 *    (flagNonReferenceOutliers, checkOutliers) never runs;
 *  - the caller passes position_sigma_3d = 0 and never arms the
 *    model-nuisance gate (setNuisanceGate is called by unit tests only), so
 *    the epoch gate (gateEpoch) evaluates every epoch and is always open;
 *  - the caller passes the pinned station position, so the epoch-to-epoch
 *    position change is 0 and the geometry correction
 *    (applyGeometryCorrection) never changes a double difference;
 *  - the observations are uncombined, so no observation carries an IF label
 *    and groupCombinedIfRow is never called.
 * The code is kept as it was (the unit tests exercise it) and gathered here so
 * that a reader of the network path can skip it.  The gate's early return in
 * processDoubleDifferences stays at its call site.
 */

#include "slip/IfPhaseSlipDetector.hpp"
#include "slip/IfPhaseChainWork.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

    // =================================================================
    // STAGED: user path, no config reaches it
    // =================================================================

    bool IfPhaseSlipDetector::gateEpoch(double position_sigma_3d, double model_nuisance_m) {
        auto logger = log_manager_.getLogger("if_slip_detector");

        // The model-nuisance gate (§6.14.6 item 3) is the counterpart of the
        // position gate for the term that survives on the network path.  Both set
        // the same `pos_gated` flag because the response is the same: keep
        // differencing and keep the gradient chain current, but do not flag on a
        // statistic whose null hypothesis does not hold this epoch.
        const bool nuisance_gated =
            (nuisance_max_m_ > 0.0) && (model_nuisance_m > nuisance_max_m_);
        const bool pos_gated = (position_sigma_3d > kPositionSigmaThresholdM) || nuisance_gated;
        epoch_gated_ = pos_gated;   // the single definition processDoubleDifferences reads
        if (pos_gated)
            logger->debug("IF slip detection gated: position_sigma_3d={:.3f} m (thr {:.2f}), "
                "model_nuisance={:.4f} m (thr {:.4f}, 0 = off)",
                position_sigma_3d, kPositionSigmaThresholdM,
                model_nuisance_m, nuisance_max_m_);
        return pos_gated;
    }

    void IfPhaseSlipDetector::groupCombinedIfRow(const PppObservation& obs,
        const std::string& phase_if_key,
        std::map<std::string, std::vector<SatData>>& result) const {
        // --- Combined (PPP) path: IF key already exists ---
        std::string sig1, sig2;
        if (!splitIfLabel(phase_if_key, sig1, sig2)) return;

        auto obs_it = obs.observed.find(phase_if_key);
        auto mod_it = obs.modeled.find(phase_if_key);
        if (obs_it == obs.observed.end() || mod_it == obs.modeled.end()) return;

        const double phi_if_omc = obs_it->second - mod_it->second;
        if (!std::isfinite(phi_if_omc)) return;

        const double lambda_if = computeIfWavelengthMeters(obs, sig1, sig2);
        if (lambda_if <= 0.0) return;

        result[PppUtils::getSystemString(obs.system)].push_back(
            { obs.satellite, phase_if_key, sig1, sig2,
              phi_if_omc, obs.elevation, lambda_if, obs.system, obs.los });
    }

    void IfPhaseSlipDetector::applyGeometryCorrection(ChainWork& w) {
        const auto& delta_pos = w.chain.delta_pos;
        const auto& ref_data = w.ref_data;
        const auto& sat_map = w.sat_map;
        auto& delta_grad = w.result.delta_grad;

        // --- Geometry correction: remove (e_sat - e_ref) · Δpos ---
        // The between-satellite single difference does not fully cancel
        // geometry.  The epoch-to-epoch position change leaves a residual of
        // (e_sat - e_ref) · Δpos in each double difference; it is removed here
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
    }

    void IfPhaseSlipDetector::flagNonReferenceOutliers(ChainWork& w) const {
        const auto& logger = w.logger;
        const auto& sys_str = w.chain.sys_str;
        const auto& grad_phi_if = w.chain.grad_phi_if;
        const auto& lambda_if = w.chain.lambda_if;
        const auto& position_sigma_3d = w.chain.position_sigma_3d;
        const auto& cs = w.cs;
        const auto& sat_map = w.sat_map;
        auto& delta_grad = w.result.delta_grad;
        auto& reset_keys = w.result.reset_keys;
        const auto& all_finite = w.all_finite;

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

                    logSlipRecord(*sit->second, sys_str,
                        grad_phi_if.at(sat), delta_grad.at(sat),
                        cs.ref_satellite, lambda_if,
                        "non_ref_outlier", position_sigma_3d);
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

                logSlipRecord(*sit->second, sys_str,
                    grad_phi_if.count(sat) ? grad_phi_if.at(sat)
                        : std::numeric_limits<double>::quiet_NaN(),
                    dd, cs.ref_satellite, lambda_if,
                    "fixed_threshold", position_sigma_3d);
            }
        }
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

} // namespace GnssProcessor
