/**
 * @file MwSlipDetectorStats.cpp
 * @brief The MW detector's statistics: spreads, the local ring, the cross-satellite
 *        pool, the adaptive threshold and the in-house bias.
 *
 * The per-arc MAD spreads (TD1, TD2, the local ring), the cross-satellite noise pool
 * and its three breadth tiers, the elevation weight, the adaptive threshold, and the
 * in-house phase-bias estimate (used only with
 * processing.inhouse_bias_estimation).
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class);
 * card UOM step U5m-c moved these bodies here unchanged (scripts/uom_u5m_verify.py
 * proves the move against the single source file of f823a36).
 */

#include "slip/MwSlipDetector.hpp"
#include "math/RobustStats.hpp"
#include "common/Constants.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // Zenith-referenced elevation weight for MW noise: w(el) = 1/sin(el).
    // MW deviation noise is code-multipath-dominated and inflates toward
    // the horizon; w normalizes a per-arc deviation to its zenith
    // equivalent on the way into the cross-sat pool (push dev/w) and
    // re-expands the pooled zenith σ to a per-arc floor on the way out
    // (σ·w).  Clamped at 10° (matching the elevation mask), so a
    // near-horizon arc can neither inflate the factor (w ≤ 1/sin10° ≈ 5.76)
    // nor reach the sin→0 singularity.  This is a geometric *shape* on top of
    // the empirical cross-sat MAD scale, not an a-priori stochastic σ: the
    // magnitude stays data-driven, so it cannot saturate the gate.
    double MwSlipDetector::mwElevationWeight(double elev_deg) {
        constexpr double kMinElevDeg = 10.0;
        const double el = (std::isfinite(elev_deg) && elev_deg > kMinElevDeg)
                              ? elev_deg : kMinElevDeg;
        return 1.0 / std::sin(el * Constants::PI / 180.0);
    }

    // =================================================================
    // Statistics / threshold
    // =================================================================

    double MwSlipDetector::computeMadSigma(const MwArcStats& stats) {
        if (stats.diff_window.size() < kMinEpochsForAdaptive)
            return std::numeric_limits<double>::quiet_NaN();
        return RobustStats::madSigma(stats.diff_window);
    }

    double MwSlipDetector::computeTd2MadSigma(const MwArcStats& stats) {
        if (stats.td2_window.size() < kMinEpochsForAdaptive)
            return std::numeric_limits<double>::quiet_NaN();
        return RobustStats::madSigma(stats.td2_window);
    }

    double MwSlipDetector::activeStatScale() const {
        if (detect_reference_ != MwDetectReference::TimeDifference)
            return 1.0;
        return (td_order_ == MwTdOrder::Second)
                   ? 2.449489742783178   // √6: var(TD2) = 6·var(level)
                   : 1.4142135623730951; // √2: var(TD1) = 2·var(level)
    }

    double MwSlipDetector::computeLocalReference(const MwArcStats& stats,
                                                 double* out_sigma_scaled) {
        // √2 rescale: the legacy spread statistic is the MAD of first
        // time-differences, which for white noise estimates √2·σ_MW (the
        // difference of two independent samples), while this window's MAD
        // estimates σ_MW directly.  Scaling keeps the configured
        // mw_sigma_multiplier on its historical TD1-equivalent scale so the
        // two reference modes gate comparably at the same multiplier.
        constexpr double kTd1EquivScale = 1.4142135623730951;  // √2

        if (out_sigma_scaled)
            *out_sigma_scaled = std::numeric_limits<double>::quiet_NaN();
        if (stats.mw_window_size == 0)
            return std::numeric_limits<double>::quiet_NaN();

        // Median/MAD are order-independent, so the valid ring prefix
        // [0, mw_window_size) can be summarized directly, without unwrapping.
        const double* first = stats.mw_window.data();
        const double* last  = first + stats.mw_window_size;
        const double  med   = RobustStats::median(first, last);

        // σ matures at the same floor as the TD1 MAD (kMinEpochsForAdaptive):
        // a MAD from a handful of lucky-quiet samples collapses toward zero
        // and would retire the cross-sat warmup floor early in exactly the
        // post-gap / cold-start regime the breadth cascade was built for.
        if (out_sigma_scaled && stats.mw_window_size >= kMinEpochsForAdaptive) {
            const double mad_sigma = RobustStats::madSigma(first, last);
            if (std::isfinite(mad_sigma))
                *out_sigma_scaled = kTd1EquivScale * mad_sigma;
        }
        return med;
    }

    void MwSlipDetector::pushLocalWindow(MwArcStats& stats, double mw_m) const {
        stats.mw_window[stats.mw_window_head] = mw_m;
        stats.mw_window_head = (stats.mw_window_head + 1) % local_window_len_;
        if (stats.mw_window_size < local_window_len_)
            ++stats.mw_window_size;
    }

    void MwSlipDetector::shiftLocalWindow(MwArcStats& stats, double delta_m) {
        for (size_t i = 0; i < stats.mw_window_size; ++i)
            stats.mw_window[i] += delta_m;
    }

    double MwSlipDetector::computeMadOfDevs(std::vector<double> devs) {
        if (devs.size() < kMinCrossSatSamples)
            return std::numeric_limits<double>::quiet_NaN();
        return RobustStats::madSigma(devs);
    }

    std::string MwSlipDetector::makeGroupKey(const std::string& stats_key) {
        // Reduce "<3-char-sat>_<sig1>_<sig2>" to a cross-sat pooling key of
        // "<constellation>_<band1>_<band2>", where bandN is the 2-char
        // frequency prefix of sigN ("L1", "L2", "L5", …) with the tracking
        // attribute dropped:
        //   "G01_L1L_L2L" → "G_L1_L2"
        //   "G01_L1W_L2W" → "G_L1_L2"  (same group)
        //
        // Pooling by frequency pair rather than full RINEX signal pair joins
        // L1L/L2L, L1W/L2W, L1L/L2W, L1W/L2L … into one statistic.  The MW
        // noise scale is set by the frequency pair (shared widelane wavelength,
        // similar code noise), not the attribute, so this multiplies the
        // sample count for sparse attribute combinations.  For example, when
        // only a few GPS sats track L1L/L2L, a per-attribute group never reaches
        // kMinCrossSatSamples and collapses to the kMinSigmaMeters floor (a
        // too-tight threshold that resets every epoch).  Distinct frequency
        // pairs (L1/L2 vs L1/L5) stay separate; only the attribute is
        // collapsed, never the band.
        if (stats_key.empty()) return {};
        const auto us1 = stats_key.find('_');
        if (us1 == std::string::npos || us1 == 0) return {};
        const std::string rest = stats_key.substr(us1 + 1);  // "<sig1>_<sig2>"
        const auto us2 = rest.find('_');
        if (us2 == std::string::npos) return {};
        const std::string sig1 = rest.substr(0, us2);
        const std::string sig2 = rest.substr(us2 + 1);
        if (sig1.size() < 2 || sig2.size() < 2) return {};
        return std::string(1, stats_key.front()) + "_"
             + sig1.substr(0, 2) + "_" + sig2.substr(0, 2);
    }

    void MwSlipDetector::finalizeCrossSatSigmas() {
        // Aggregate the epoch's per-band-pair deviation pool into three tiers
        // of increasing breadth, so a sparse band-pair group (e.g. only 2–3
        // sats track L1L/L2L, or the very first epochs before any arc has a
        // poolable deviation) can borrow a noise estimate from a broader but
        // still-comparable population instead of collapsing to the
        // kMinSigmaMeters floor.  The pooled quantity is each arc's
        // zenith-normalized active-statistic deviation (from the arc's own
        // reference, or a time-difference in the TimeDifference modes): MW noise
        // with the per-arc bias removed.  MW noise in metres is code-dominated,
        // hence broadly comparable across band-pairs and (clean) constellations.
        //   Tier 1: per (constellation × band-pair)  [cross_sat_sigma_m_]
        //   Tier 2: per constellation, all band-pairs [cross_sat_sigma_cons_]
        //   Tier 3: global, all constellations except GLONASS [..._global_]
        // GLONASS is excluded from the global tier only: its FDMA inter-
        // channel noise is the least comparable, so it must not contaminate
        // the estimate the clean constellations fall back on (a GLONASS arc
        // therefore drops from its own constellation tier straight to the
        // gross-slip gate).  Each tier updates only when it clears
        // kMinCrossSatSamples; otherwise the last known value persists.
        std::map<std::string, std::vector<double>> cons_devs;  // "G" → devs
        std::vector<double> global_devs;                       // non-GLONASS

        // EWMA blend onto the previous tier value; the first finite MAD
        // bootstraps the average (NaN prev → take raw).  Smooths out the
        // per-epoch MAD jitter of a thin cross-section while still tracking
        // the slowly-varying noise envelope.
        const double alpha = cross_sat_ewma_alpha_;
        const auto blend = [alpha](double prev, double raw) {
            return std::isfinite(prev) ? alpha * raw + (1.0 - alpha) * prev : raw;
        };

        for (auto& [group_key, devs] : cross_sat_devs_) {
            if (group_key.empty() || devs.empty()) continue;
            const char cons_c = group_key.front();
            // Aggregate (copies) into the broader tiers before the per-group
            // MAD move-consumes the vector.
            auto& cd = cons_devs[std::string(1, cons_c)];
            cd.insert(cd.end(), devs.begin(), devs.end());
            if (cons_c != 'R')
                global_devs.insert(global_devs.end(), devs.begin(), devs.end());

            if (devs.size() < kMinCrossSatSamples) continue;
            const double sigma_m = computeMadOfDevs(std::move(devs));
            if (std::isfinite(sigma_m) && sigma_m > 0.0) {
                auto it = cross_sat_sigma_m_.find(group_key);
                const double prev = (it != cross_sat_sigma_m_.end())
                                        ? it->second
                                        : std::numeric_limits<double>::quiet_NaN();
                cross_sat_sigma_m_[group_key] = blend(prev, sigma_m);
            }
        }

        for (auto& [cons, cd] : cons_devs) {
            if (cd.size() < kMinCrossSatSamples) continue;
            const double sigma_m = computeMadOfDevs(std::move(cd));
            if (std::isfinite(sigma_m) && sigma_m > 0.0) {
                auto it = cross_sat_sigma_cons_.find(cons);
                const double prev = (it != cross_sat_sigma_cons_.end())
                                        ? it->second
                                        : std::numeric_limits<double>::quiet_NaN();
                cross_sat_sigma_cons_[cons] = blend(prev, sigma_m);
            }
        }

        if (global_devs.size() >= kMinCrossSatSamples) {
            const double sigma_m = computeMadOfDevs(std::move(global_devs));
            if (std::isfinite(sigma_m) && sigma_m > 0.0)
                cross_sat_sigma_global_ = blend(cross_sat_sigma_global_, sigma_m);
        }

        cross_sat_devs_.clear();
    }

    double MwSlipDetector::crossSatSigmaFloor(const std::string& stats_key,
                                               double elev_deg) const {
        // Cross-satellite σ resolved by a breadth cascade so a sparse band-pair
        // group borrows from a broader-but-comparable population rather than
        // collapsing to kMinSigmaMeters:
        //   band-pair → constellation (all pairs) → global (non-GLONASS).
        // Stored values are EWMA-smoothed and zenith-referenced (the pool
        // normalizes each deviation by 1/sin(el)); re-expand to this arc's
        // elevation on use.  All tiers are empirical MADs (no stochastic-model
        // σ), pooled at coarser granularity when the fine group is thin.
        if (stats_key.empty()) return std::numeric_limits<double>::quiet_NaN();
        const char cons_c = stats_key.front();
        double sigma_xsat = std::numeric_limits<double>::quiet_NaN();

        const std::string group_key = makeGroupKey(stats_key);
        if (!group_key.empty()) {                                 // Tier 1
            auto it = cross_sat_sigma_m_.find(group_key);
            if (it != cross_sat_sigma_m_.end() && std::isfinite(it->second))
                sigma_xsat = it->second;
        }
        if (!std::isfinite(sigma_xsat)) {                         // Tier 2
            auto it = cross_sat_sigma_cons_.find(std::string(1, cons_c));
            if (it != cross_sat_sigma_cons_.end() && std::isfinite(it->second))
                sigma_xsat = it->second;
        }
        if (!std::isfinite(sigma_xsat) && cons_c != 'R'           // Tier 3 (non-GLONASS)
            && std::isfinite(cross_sat_sigma_global_)) {
            sigma_xsat = cross_sat_sigma_global_;
        }
        if (!std::isfinite(sigma_xsat))
            return std::numeric_limits<double>::quiet_NaN();
        return sigma_xsat * mwElevationWeight(elev_deg);
    }

    double MwSlipDetector::computeAdaptiveThreshold(const MwArcStats& stats,
                                                     const std::string& stats_key,
                                                     double elev_deg,
                                                     double mode_sigma_m) const {
        // Fully data-driven gate: no stochastic-model-derived σ source is
        // consulted at any stage (a predicted σ_MW saturated the gate at the
        // cap, making it constant rather than statistical).  The gate has two
        // regimes with a continuous warmup→per-arc handover:
        //
        //   Mature arc: the caller passes the mature spread of the active
        //   detection statistic (Hatch dev → TD1 MAD; local dev → √2-scaled
        //   ring MAD; TD1 dev → TD1 MAD; TD2 dev → TD2 MAD; see the header
        //   table) and it governs the gate.  Outside HatchMean mode, deviation
        //   and spread are the same quantity by construction; gating one
        //   statistic with another's spread (trend-lagged level dev against
        //   trend-collapsed TD1 MAD) is the false-slip mechanism that the
        //   legacy HatchMean pairing keeps.  The cross-sat ("all satellites
        //   together") σ is not applied as a floor here: it is recomputed each
        //   epoch from a thin cross-section, and a single noisy population
        //   value must not override a converged per-arc estimate (applying it
        //   made threshold_cycles jitter while mw_sigma_cycles stayed smooth).
        //   Only the absolute kMinSigmaMeters floor remains.
        if (std::isfinite(mode_sigma_m)) {
            return sigma_multiplier_ * std::max(mode_sigma_m, kMinSigmaMeters);
        }

        //   Warmup arc: no trustworthy per-arc MAD yet.  Floor the gate on the
        //   (EWMA-smoothed) cross-sat σ for this group, plus a per-arc spread
        //   term once samples exist.  The multiplier matches the mature branch
        //   so the gate is continuous across the handover at
        //   kMinEpochsForAdaptive.  The per-arc spread term is mode-specific:
        //
        //   - Level modes (HatchMean / LocalMedian): the Welford σ_emp over
        //     raw MW levels, statistic-consistent with a level deviation,
        //     and the legacy behavior (bit-identical).
        //
        //   - TimeDifference modes: the young TD MAD, i.e. the RobustStats MAD
        //     over the partial diff_window (TD1) / td2_window (TD2), even below
        //     kMinEpochsForAdaptive.  σ_emp is not used here: it is a level
        //     variance, and on a trending arc it measures trend + noise (a ramp
        //     of span S contributes ≈ S/√12), widening the warmup gate by whole
        //     cycles while the TD deviation it gates stays at noise scale (a
        //     fresh arc was observed blind to ≤2-cycle slips for ~10 epochs).
        //     The young MAD is trend-immune like the mature branch, and it
        //     cannot collapse the gate in this position: a young MAD from
        //     lucky-quiet samples near zero is a hazard only when it governs
        //     alone, and as one term inside max(kMinSigma, cross-sat floor,
        //     young MAD) a collapse leaves the other floors in charge (it can
        //     lift, never undercut).  It also makes the warmup→mature handover
        //     continuous in the statistic itself (young MAD at 14 samples →
        //     trusted MAD at 15).
        const double xsat_floor   = crossSatSigmaFloor(stats_key, elev_deg);
        const bool   have_cross_sat = std::isfinite(xsat_floor);

        double sigma_floor = kMinSigmaMeters;
        if (have_cross_sat)
            sigma_floor = std::max(sigma_floor, xsat_floor);
        if (detect_reference_ == MwDetectReference::TimeDifference) {
            // Key the window on the epoch's active statistic, not the
            // configured order: under MwTdOrder::Second the gate falls back
            // to TD1 whenever prev_diff is NaN (fresh-arc second sample,
            // post-outlier excision), and on a sub-mature locked arc the
            // post-excision epoch reaches this branch with a non-empty
            // td2_window.  Flooring a TD1 (√2-scale) deviation with a young
            // TD2 (√6-scale) MAD would run that one epoch ~√3 loose.
            // Matching the window keeps the per-arc term scale-consistent
            // with the deviation it gates in every fallback case.  (A fresh
            // arc's fallback epoch has both windows effectively empty, so the
            // cross-sat floor governs; a 1-sample window gives MAD = 0, so
            // likewise no lift.)
            const bool td2_active = td_order_ == MwTdOrder::Second
                                    && std::isfinite(stats.prev_diff);
            const auto& win = td2_active ? stats.td2_window
                                         : stats.diff_window;
            if (!win.empty()) {
                const double young_mad = RobustStats::madSigma(win);
                if (std::isfinite(young_mad))
                    sigma_floor = std::max(sigma_floor, young_mad);
            }
        } else if (stats.count >= 2 && stats.M2 > 0.0) {
            const double sigma_emp_m =
                std::sqrt(stats.M2 / static_cast<double>(stats.count - 1));
            sigma_floor = std::max(sigma_floor, sigma_emp_m);
        }
        double thresh = sigma_multiplier_ * sigma_floor;

        // True cold start: no cross-sat σ at any cascade tier (the first epoch
        // or two before any arc has a poolable deviation, or a pathologically
        // sparse constellation).  With no trustworthy noise estimate, floor the
        // gate at a gross-slip threshold of min(λ_WL, 1 m), applied directly and
        // not multiplied by sigma_multiplier_: with zero noise information only
        // a jump of ~1 widelane cycle or more is flagged.  σ_emp can lift the
        // gate but never lower it.  The floor drops out once any tier populates.
        if (!have_cross_sat) {
            const double lam = stats.lambda_wl;
            const double gate = (std::isfinite(lam) && lam > 0.0)
                                    ? std::min(lam, 1.0) : 1.0;
            thresh = std::max(thresh, gate);
        }
        return thresh;
    }

    // =================================================================
    // In-house phase bias estimation
    // =================================================================

    void MwSlipDetector::applyEstimatedBias(double& mw_m, bool& bia_corrected,
                                             const std::string& stats_key) const {
        if (bia_corrected) return;
        auto it = sat_pair_bias_.find(stats_key);
        if (it == sat_pair_bias_.end() || it->second.commit_count == 0) return;
        mw_m -= it->second.bias_m;
        bia_corrected = true;
    }

    bool MwSlipDetector::tryCommitOrUpdateBias(MwArcStats& stats,
                                                const std::string& stats_key,
                                                double lambda_wl,
                                                double epoch_s) {
        if (!(lambda_wl > 0.0)) return false;
        if (stats.count < kBiasCommitMinCount) return false;
        if (stats.M2 <= 0.0) return false;  // need accumulated variance to assess σ_mean

        const double var_per_sample = stats.M2 / static_cast<double>(stats.count - 1);
        const double sigma_mean_m   = std::sqrt(var_per_sample / static_cast<double>(stats.count));
        const double sigma_mean_cyc = sigma_mean_m / lambda_wl;
        if (sigma_mean_cyc > kBiasCommitMaxSigmaCyc) return false;

        const double mean_cyc = stats.mean / lambda_wl;
        const double rounded  = std::round(mean_cyc);
        const double frac     = mean_cyc - rounded;
        if (std::abs(frac) > kBiasCommitMaxFrac) return false;

        // Observation: the residual that BIA would have removed.  Sign
        // convention is "subtract from raw mw_m to get integer-near".
        const double bias_obs_m = frac * lambda_wl;
        const double bias_obs_var = sigma_mean_m * sigma_mean_m;

        auto logger = log_manager_.getLogger("mw_slip_detector");
        auto it = sat_pair_bias_.find(stats_key);
        const bool first_commit = (it == sat_pair_bias_.end()
                                    || it->second.commit_count == 0);

        if (first_commit) {
            EstimatedBias b;
            b.bias_m              = bias_obs_m;
            b.bias_var_m2         = bias_obs_var;
            b.commit_count        = 1;
            b.last_update_epoch_s = epoch_s;
            sat_pair_bias_[stats_key] = b;
            logger->info("In-house bias commit (first) for {}: "
                         "bias={:.4f} m ({:.3f} cyc), σ={:.4f} m, "
                         "rounded N_WL={}, count={}",
                         stats_key, bias_obs_m, frac, sigma_mean_m,
                         static_cast<int>(rounded), stats.count);
        } else {
            // Cross-arc consistency check + EWMA update.
            const double diff_m  = bias_obs_m - it->second.bias_m;
            const double comb_var = bias_obs_var + it->second.bias_var_m2;
            const double k_sigma  = std::sqrt(std::max(comb_var, 1e-12));
            if (std::abs(diff_m) > 5.0 * k_sigma) {
                logger->warn("In-house bias inconsistency for {}: "
                             "obs={:.4f} m vs stored={:.4f} m "
                             "(Δ={:.4f} m, 5σ={:.4f} m); refusing update — "
                             "treat current arc as fresh bootstrap.",
                             stats_key, bias_obs_m, it->second.bias_m,
                             diff_m, 5.0 * k_sigma);
                return false;
            }
            // EWMA on bias mean; variance shrinks slowly toward observation variance.
            it->second.bias_m =
                (1.0 - kBiasEwmaAlpha) * it->second.bias_m
                + kBiasEwmaAlpha * bias_obs_m;
            it->second.bias_var_m2 =
                (1.0 - kBiasEwmaAlpha) * it->second.bias_var_m2
                + kBiasEwmaAlpha * bias_obs_var;
            ++it->second.commit_count;
            it->second.last_update_epoch_s = epoch_s;
            logger->info("In-house bias update for {}: "
                         "obs={:.4f} m, blended={:.4f} m ({:.3f} cyc), "
                         "n_commits={}, count={}",
                         stats_key, bias_obs_m, it->second.bias_m,
                         it->second.bias_m / lambda_wl,
                         it->second.commit_count, stats.count);
        }

        // Re-anchor the running arc so subsequent epochs treat the mean as
        // integer-near and the lock criterion sees a fully-corrected arc,
        // as the BIA-corrected path produces without extra work.  The mean
        // is not pinned to rounded_wl × λ: the bias subtraction already
        // centers it near integer (commit gating requires
        // |frac| < kBiasCommitMaxFrac = 0.20 and σ_mean < 0.05 cyc, so the
        // residual after subtraction is well under the drift demotion
        // threshold), and keeping it live lets Welford continue tracking
        // the arc, so the drift-demotion safety net that protects
        // BIA-corrected locks applies here too.
        const auto& stored = sat_pair_bias_.at(stats_key);
        stats.mean         -= stored.bias_m;
        stats.mean_lag     -= stored.bias_m;   // lagged snapshot rides the same frame (MwArcStats doc)
        stats.prev_mw      -= stored.bias_m;
        // The local raw-MW ring holds pre-commit (uncorrected) samples;
        // re-anchor it with the mean, or the commit epoch's local deviation
        // jumps by the full bias (decimeters), the same false-fire that the
        // in-place mw_m adjustment in processMwSample prevents for the
        // Hatch deviation.
        shiftLocalWindow(stats, -stored.bias_m);
        stats.bia_corrected = true;
        stats.rounded_wl    = static_cast<int>(rounded);
        stats.wl_locked     = true;
        return true;
    }

} // namespace GnssProcessor
