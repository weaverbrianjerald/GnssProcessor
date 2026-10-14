/**
 * @file MwSlipDetectorQuery.cpp
 * @brief The MW detector's read-only queries.
 *
 * The locked wide-lane integers with their fractions and standard errors (read by
 * AmbiguityResolver through CycleSlipManager), the mature arc keys, the across-gap
 * settle evidence (TurboEdit's WL-settle deferral) and the worst per-satellite
 * deviation score (the KalmanFilter's outlier tiebreak).
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class);
 * card UOM step U5m-c moved these bodies here unchanged (scripts/uom_u5m_verify.py
 * proves the move against the single source file of f823a36).
 */

#include "slip/MwSlipDetector.hpp"
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // =================================================================
    // Per-satellite MW quality query
    // =================================================================

    double MwSlipDetector::getWorstMwScore(const std::string& satellite) const {
        const std::string prefix = satellite + "_";
        double worst = 0.0;
        for (const auto& [key, stats] : mw_stats_) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            if (stats.count < kMinEpochsForAdaptive) continue;
            const double sigma = stats.last_sigma;
            if (!std::isfinite(sigma) || sigma <= 0.0) continue;
            const double score = std::abs(stats.last_dev) / sigma;
            if (score > worst) worst = score;
        }
        return worst;
    }

    // =================================================================
    // Resolved WL accessor
    // =================================================================

    std::map<std::string, int> MwSlipDetector::getResolvedWlAmbiguities() const {
        std::map<std::string, int> resolved;

        // Only fully-locked arcs are exposed.  The detector's k_σ-bounded
        // statistical lock criterion (with kWlLockMinN floor on sample
        // count) gates entry; the drift-demotion check unlocks any arc
        // whose live mean migrates away from the rounded integer, so any
        // arc still flagged @c wl_locked here has cumulative-mean
        // evidence within @c kWlLockDriftMaxCyc of its integer.
        // Consumers can apply the returned integer without re-validating;
        // @c getResolvedWlFractions reports the live |drift| + kσ_mean
        // for any consumer that wants to rank or weight by lock
        // confidence.
        for (const auto& [key, stats] : mw_stats_) {
            if (!stats.wl_locked) continue;
            if (!stats.bia_corrected) continue;
            if (!(stats.lambda_wl > 0.0)) continue;
            // Mid-confirmation (TD modes): the TD gate fired last epoch and
            // the step-vs-outlier decision lands with the next sample, so the
            // locked integer is suspect for this epoch; withhold it from AR
            // rather than expose a possibly stale value.
            if (stats.td_confirm_pending) continue;
            resolved[key] = stats.rounded_wl;
        }

        // BSSD MW pivots: SD vs self is identically 0 cycles by
        // construction.  Even though pivot arcs have
        // @c bia_corrected=false (receiver bias is not removed from the
        // undifferenced MW), their value in the SD frame is
        // unambiguously 0, so AR can use them as the reference end of
        // a DD.  Without this entry, AR fails whenever its pivot
        // coincides with the MW pivot for that (cons, signal-pair),
        // the common case since both pickers use highest elevation.
        for (const auto& [group_key, pivot_sat] : sd_pivot_sat_) {
            const auto first_us = group_key.find('_');
            if (first_us == std::string::npos) continue;
            const std::string sig_pair = group_key.substr(first_us + 1);
            const std::string pivot_stats_key = pivot_sat + "_" + sig_pair;
            resolved.try_emplace(pivot_stats_key, 0);
        }

        return resolved;
    }

    std::map<std::string, double> MwSlipDetector::getResolvedWlFractions() const {
        std::map<std::string, double> fractions;
        // 3σ confidence bound; matches the lock criterion's k_σ.
        constexpr double kFracKSigma = 3.0;

        // Mirrors getResolvedWlAmbiguities: only fully-locked arcs are
        // reported.  Since the lock does not pin the mean to
        // rounded_wl × λ, the running fractional residual
        // |mean/λ − rounded_wl| is informative: it tracks the live
        // drift between cumulative evidence and the locked
        // integer.  The reported value is the worst-case-frac
        // expression that the lock criterion itself uses:
        //
        //     |drift| + kσ · σ_mean / λ_WL
        //
        // Consumers (e.g. AmbiguityResolverConfig::wl_max_frac) can
        // apply a single confidence gate that captures both observed
        // drift and statistical uncertainty.  An arc whose drift has
        // grown beyond kWlLockDriftMaxCyc would have already been
        // demoted by processMwSample, so values reported here are
        // bounded by kWlLockDriftMaxCyc + kσ · σ_mean.
        for (const auto& [key, stats] : mw_stats_) {
            if (!stats.wl_locked) continue;
            if (!stats.bia_corrected) continue;
            if (!(stats.lambda_wl > 0.0)) continue;
            if (stats.count < 2) continue;
            // Mirrors getResolvedWlAmbiguities: integer withheld while a
            // TD step/outlier confirmation is pending.
            if (stats.td_confirm_pending) continue;

            // σ_mean from the (now-live) Welford accumulator, in cycles.
            double sigma_mean_cyc = 0.0;
            if (stats.M2 > 0.0) {
                const double var_per_sample =
                    stats.M2 / static_cast<double>(stats.count - 1);
                const double sigma_mean_m =
                    std::sqrt(var_per_sample / static_cast<double>(stats.count));
                sigma_mean_cyc = sigma_mean_m / stats.lambda_wl;
            }

            const double drift_cyc =
                stats.mean / stats.lambda_wl - stats.rounded_wl;
            fractions[key] = std::abs(drift_cyc) + kFracKSigma * sigma_mean_cyc;
        }

        // Pivots: 0 cycles, no uncertainty in SD-vs-self.
        for (const auto& [group_key, pivot_sat] : sd_pivot_sat_) {
            const auto first_us = group_key.find('_');
            if (first_us == std::string::npos) continue;
            const std::string sig_pair = group_key.substr(first_us + 1);
            const std::string pivot_stats_key = pivot_sat + "_" + sig_pair;
            fractions.try_emplace(pivot_stats_key, 0.0);
        }

        return fractions;
    }

    std::map<std::string, MwSlipDetector::WlLockQuality>
    MwSlipDetector::getResolvedWlQuality() const {
        std::map<std::string, WlLockQuality> quality;
        // Population and key space identical to getResolvedWlAmbiguities /
        // getResolvedWlFractions: an arc whose integer is withheld must not
        // report a σ either, or a consumer could weight a lock it cannot use.
        for (const auto& [key, stats] : mw_stats_) {
            if (!stats.wl_locked) continue;
            if (!stats.bia_corrected) continue;
            if (!(stats.lambda_wl > 0.0)) continue;
            if (stats.count < 2) continue;
            if (stats.td_confirm_pending) continue;

            // σ of the mean, in cycles: the same expression
            // getResolvedWlFractions forms before folding it into the
            // worst-case-frac scalar.  count ≥ 2 is guaranteed above, so the
            // (count − 1) divisor is safe.
            double sigma_mean_cyc = 0.0;
            if (stats.M2 > 0.0) {
                const double var_per_sample =
                    stats.M2 / static_cast<double>(stats.count - 1);
                const double sigma_mean_m =
                    std::sqrt(var_per_sample / static_cast<double>(stats.count));
                sigma_mean_cyc = sigma_mean_m / stats.lambda_wl;
            }
            quality[key] = WlLockQuality{ sigma_mean_cyc, stats.count, /*exact=*/false };
        }

        // BSSD MW pivots: SD vs self is identically 0 cycles with no
        // uncertainty, so success is 1 regardless of maturity.  try_emplace, so
        // a pivot that also has a real locked arc above keeps its measured
        // quality (as the fractions accessor treats the same keys).
        for (const auto& [group_key, pivot_sat] : sd_pivot_sat_) {
            const auto first_us = group_key.find('_');
            if (first_us == std::string::npos) continue;
            const std::string sig_pair = group_key.substr(first_us + 1);
            const std::string pivot_stats_key = pivot_sat + "_" + sig_pair;
            quality.try_emplace(pivot_stats_key,
                                WlLockQuality{ 0.0, 0, /*exact=*/true });
        }
        return quality;
    }

    // =================================================================
    // Active arc keys accessor
    // =================================================================

    std::vector<std::string> MwSlipDetector::getActiveArcKeys() const {
        std::vector<std::string> keys;
        keys.reserve(mw_stats_.size());
        for (const auto& [key, stats] : mw_stats_) {
            // Only return arcs with enough history to have detected a slip
            // if one occurred.  Immature arcs defer slip detection (the MAD
            // is not yet valid), so treating them as "clean" would create
            // false dN_i - dN_j = 0 constraints that conflict with real slips
            // detected on mature arcs for the same satellite.
            if (stats.diff_window.size() >= kMinEpochsForAdaptive)
                keys.push_back(key);
        }
        return keys;
    }

    std::map<std::string, MwSlipDetector::AcrossGapWlEvidence>
    MwSlipDetector::getAcrossGapWlEvidence(double epoch_now_s) const {
        std::map<std::string, AcrossGapWlEvidence> out;
        for (const auto& [key, dl] : demoted_levels_) {
            if (epoch_now_s > 0.0 && dl.epoch_s > 0.0 &&
                epoch_now_s - dl.epoch_s > kDemotedLevelMaxAgeS)
                continue;  // aged out (purged lazily on the next detect pass)
            const auto it = mw_stats_.find(key);
            if (it == mw_stats_.end()) continue;          // no post-restart arc yet
            const auto& arc = it->second;
            if (arc.count < 2 || dl.lambda_wl <= 0.0) continue;
            // Frame comparability: the stash and the live arc must be in the
            // same bias frame (a bia_corrected flip moves the level by the
            // full WL bias, decimeters).
            if (arc.bia_corrected != dl.bia_corrected) continue;

            AcrossGapWlEvidence ev;
            ev.float_cycles = (arc.mean - dl.pre_level_m) / dl.lambda_wl;
            ev.post_count   = arc.count;
            const double var_per_sample =
                arc.M2 / static_cast<double>(arc.count - 1);
            ev.sigma_mean_cycles =
                std::sqrt(std::max(0.0, var_per_sample /
                                            static_cast<double>(arc.count)))
                / dl.lambda_wl;
            // Empirical settle criterion, identical in form to the WL lock
            // gate: the estimate is trusted once the worst-case (3σ-bounded)
            // fractional residual sits inside the rounding basin.
            const double frac =
                std::abs(ev.float_cycles - std::round(ev.float_cycles));
            ev.stable = arc.count >= kWlSettleMinEpochs &&
                        std::isfinite(ev.sigma_mean_cycles) &&
                        (frac + 3.0 * ev.sigma_mean_cycles) < 0.5;
            out[key] = ev;
        }
        return out;
    }

} // namespace GnssProcessor
