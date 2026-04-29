/**
 * @file GfSlipDetector.cpp
 * @brief Geometry-free (GF) cycle slip detector using second-order time-difference.
 *
 * Organization:
 *  - Construction / tracing
 *  - Signal helpers
 *  - GF computation
 *  - Statistics / threshold / IVI
 *  - Core detection logic  (two-pass: collect TD2 ? assess IVI ? flag slips)
 *  - External reset
 */

#include "slip/GfSlipDetector.hpp"
#include "math/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>

namespace GnssProcessor {

    // =================================================================
    // Construction
    // =================================================================

    GfSlipDetector::GfSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("gf_slip_detector")->info(
            "GfSlipDetector initialized (adaptive MAD-based, k={:.1f}, floor={:.3f} m, "
            "min_epochs={}, cooldown={}, IVI_thresh={:.3f} m)",
            kMadSigmaMultiplier, kMinThresholdM, kMinEpochsForAdaptive,
            kCooldownEpochs, kIviThreshold);
    }

    // =================================================================
    // Tracing
    // =================================================================

    bool GfSlipDetector::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("gf_slip_detector");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "gf_slip_trace.csv";
        trace_file_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!trace_file_.is_open()) {
            logger->warn("Could not open GF trace file '{}'", trace_path.string());
            return false;
        }

        trace_file_ << "week,tow_s,satellite,system,elevation_deg,"
                    << "sig1,sig2,gf_m,td2_gf_m,"
                    << "mad_sigma_m,threshold_m,arc_count,"
                    << "ivi_m,iono_stable,"
                    << "is_slip,suppressed\n";

        logger->info("GF tracing enabled: {}", trace_path.string());
        return true;
    }

    // =================================================================
    // Signal helpers
    // =================================================================

    std::vector<GfSlipDetector::DualFreqPair>
    GfSlipDetector::collectDualFreqPairs(const PppObservation& obs) const {
        // Group individual phase signals by RINEX band number.
        std::map<char, std::string> phase_by_band;

        for (const auto& [key, val] : obs.raw_observed) {
            if (key.size() < 2) continue;
            if (key[0] != 'L') continue;
            // Skip combined (IF) labels containing '_'
            if (key.find('_') != std::string::npos) continue;
            const char band = key[1];
            if (band < '1' || band > '9') continue;
            if (phase_by_band.find(band) == phase_by_band.end())
                phase_by_band[band] = key;
        }

        std::vector<char> band_ids;
        for (const auto& [b, _] : phase_by_band)
            band_ids.push_back(b);

        std::vector<DualFreqPair> pairs;
        for (size_t i = 0; i < band_ids.size(); ++i)
            for (size_t j = i + 1; j < band_ids.size(); ++j)
                pairs.push_back({phase_by_band[band_ids[i]],
                                 phase_by_band[band_ids[j]]});
        return pairs;
    }

    // =================================================================
    // GF computation
    // =================================================================

    double GfSlipDetector::computeGfMeters(const PppObservation& obs,
                                            const std::string& sig1,
                                            const std::string& sig2) const {
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        auto it1 = obs.raw_observed.find(sig1);
        auto it2 = obs.raw_observed.find(sig2);
        if (it1 == obs.raw_observed.end() || it2 == obs.raw_observed.end())
            return NaN;

        const double L1_cyc = it1->second;
        const double L2_cyc = it2->second;
        if (!std::isfinite(L1_cyc) || !std::isfinite(L2_cyc))
            return NaN;

        // Convert cycles to meters
        const double lam1 = ppp_utils_.getWavelength(obs.system, sig1, obs.satellite);
        const double lam2 = ppp_utils_.getWavelength(obs.system, sig2, obs.satellite);
        if (lam1 <= 0.0 || lam2 <= 0.0)
            return NaN;

        const double L1_m = L1_cyc * lam1;
        const double L2_m = L2_cyc * lam2;

        // GF = L1 - L2 (contains ionosphere + ambiguity)
        const double gf = L1_m - L2_m;
        return std::isfinite(gf) ? gf : NaN;
    }

    // =================================================================
    // Statistics / threshold / IVI
    // =================================================================

    double GfSlipDetector::computeMadSigma(const std::deque<double>& window) {
        if (window.size() < kMinEpochsForAdaptive)
            return std::numeric_limits<double>::quiet_NaN();

        std::vector<double> sorted(window.begin(), window.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        const double median = (n % 2 == 0)
            ? 0.5 * (sorted[n / 2 - 1] + sorted[n / 2])
            : sorted[n / 2];

        std::vector<double> abs_devs(n);
        for (size_t i = 0; i < n; ++i)
            abs_devs[i] = std::abs(sorted[i] - median);
        std::sort(abs_devs.begin(), abs_devs.end());
        const double mad = (n % 2 == 0)
            ? 0.5 * (abs_devs[n / 2 - 1] + abs_devs[n / 2])
            : abs_devs[n / 2];

        return mad * 1.4826; // Gaussian-equivalent sigma
    }

    double GfSlipDetector::computeAdaptiveThreshold(const GfArcStats& stats) const {
        const double sigma = computeMadSigma(stats.diff_window);
        if (!std::isfinite(sigma))
            return std::numeric_limits<double>::quiet_NaN();

        return std::max(kMadSigmaMultiplier * sigma, kMinThresholdM);
    }

    double GfSlipDetector::computeIvi(const std::vector<double>& constellation_td2) {
        if (constellation_td2.size() < 3)
            return 0.0;

        // MAD of absolute TD2 values across satellites
        std::vector<double> abs_vals;
        abs_vals.reserve(constellation_td2.size());
        for (const double v : constellation_td2)
            abs_vals.push_back(std::abs(v));

        std::sort(abs_vals.begin(), abs_vals.end());
        const size_t n = abs_vals.size();
        const double median = (n % 2 == 0)
            ? 0.5 * (abs_vals[n / 2 - 1] + abs_vals[n / 2])
            : abs_vals[n / 2];

        std::vector<double> devs(n);
        for (size_t i = 0; i < n; ++i)
            devs[i] = std::abs(abs_vals[i] - median);
        std::sort(devs.begin(), devs.end());
        const double mad = (n % 2 == 0)
            ? 0.5 * (devs[n / 2 - 1] + devs[n / 2])
            : devs[n / 2];

        // IVI = median + MAD*1.4826 (robust measure of typical level + spread)
        return median + mad * 1.4826;
    }

    // =================================================================
    // Core detection logic  (two-pass architecture)
    // =================================================================

    std::vector<GfSlipResult> GfSlipDetector::detect(
        const std::vector<PppObservation>& observations) {

        auto logger = log_manager_.getLogger("gf_slip_detector");

        // Capture epoch time from first valid observation
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        // ------------------------------------------------------------------
        // Pass 1: Compute TD2 values for all arcs; collect per-constellation
        //         TD2 values for IVI computation.
        // ------------------------------------------------------------------
        struct ArcEpochData {
            const PppObservation* obs;
            std::string stats_key;
            std::string sig1, sig2;
            double gf;
            double td2_gf;          // NaN if not yet available
            double thresh;           // NaN if not yet computable
            double sigma;            // NaN if not yet computable
            bool   exceeds_thresh;   // threshold test (before suppression)
        };
        std::vector<ArcEpochData> arc_data;

        // constellation char -> list of TD2 values this epoch
        std::map<char, std::vector<double>> constellation_td2;

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg())
                continue;

            const auto pairs = collectDualFreqPairs(obs);
            const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

            for (const auto& pair : pairs) {
                const double gf = computeGfMeters(obs, pair.phase1, pair.phase2);
                if (!std::isfinite(gf)) continue;

                const std::string stats_key =
                    obs.satellite + "_" + pair.phase1 + "_" + pair.phase2;
                auto& stats = gf_stats_[stats_key];

                // Data gap detection
                if (stats.count > 0 && stats.last_epoch_s > 0.0) {
                    const double gap = epoch_s - stats.last_epoch_s;
                    if (gap > kMaxGapSeconds) {
                        logger->debug("Data gap {:.1f}s > {:.0f}s for {}: resetting GF arc",
                                      gap, kMaxGapSeconds, stats_key);
                        stats = GfArcStats{};
                    }
                }

                constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

                // Epoch 0 or 1: not enough history for TD2
                if (stats.count < 2) {
                    if (stats.count == 0) {
                        stats.prev_gf      = gf;
                        stats.last_epoch_s = epoch_s;
                        stats.count        = 1;
                        stats.diff_window.clear();
                    } else {
                        // count == 1
                        stats.prev_prev_gf = stats.prev_gf;
                        stats.prev_gf      = gf;
                        stats.last_epoch_s = epoch_s;
                        stats.count        = 2;
                    }

                    // Trace the initialization epoch
                    if (trace_file_.is_open()) {
                        trace_file_ << std::fixed << std::setprecision(3)
                                    << current_epoch_.week << "," << current_epoch_.tow << ","
                                    << obs.satellite << ","
                                    << PppUtils::getSystemString(obs.system) << ","
                                    << std::setprecision(2) << obs.elevation << ","
                                    << pair.phase1 << "," << pair.phase2 << ","
                                    << std::setprecision(6) << gf << ","
                                    << NaN << ","
                                    << NaN << "," << NaN << ","
                                    << stats.count << ","
                                    << NaN << "," << 1 << ","
                                    << 0 << "," << 0 << "\n";
                    }
                    continue;
                }

                // Epoch 2+: compute second-order time difference
                const double td2_gf = gf - 2.0 * stats.prev_gf + stats.prev_prev_gf;
                const double thresh = computeAdaptiveThreshold(stats);
                const double sigma  = computeMadSigma(stats.diff_window);
                const bool exceeds = std::isfinite(thresh) && std::abs(td2_gf) > thresh;

                arc_data.push_back({&obs, stats_key, pair.phase1, pair.phase2,
                                    gf, td2_gf, thresh, sigma, exceeds});

                // Collect for IVI
                const char cons = obs.satellite[0];
                constellation_td2[cons].push_back(td2_gf);
            }
        }

        // ------------------------------------------------------------------
        // Compute per-constellation IVI and global iono stability flag
        // ------------------------------------------------------------------
        std::map<char, double> ivi_by_cons;
        double max_ivi = 0.0;
        for (const auto& [cons, td2_vec] : constellation_td2) {
            const double ivi = computeIvi(td2_vec);
            ivi_by_cons[cons] = ivi;
            if (ivi > max_ivi) max_ivi = ivi;
        }
        iono_variability_global_ = max_ivi;
        iono_stable_global_ = (max_ivi < kIviThreshold);

        if (!iono_stable_global_) {
            logger->debug("Ionosphere active: IVI={:.4f} m > threshold {:.4f} m  "
                          "GF slip detections will be suppressed this epoch",
                          max_ivi, kIviThreshold);
        }

        // ------------------------------------------------------------------
        // Pass 2: Flag slips with suppression guards
        // ------------------------------------------------------------------
        std::vector<GfSlipResult> results;

        for (const auto& ad : arc_data) {
            auto& stats = gf_stats_[ad.stats_key];
            const char cons = ad.obs->satellite[0];
            const double ivi = ivi_by_cons.count(cons) ? ivi_by_cons.at(cons) : 0.0;
            const bool iono_stable = (ivi < kIviThreshold);

            // Determine suppression reason (if any)
            enum class Suppression { None, Cooldown, IonoActive, Oscillation };
            Suppression suppression = Suppression::None;

            if (ad.exceeds_thresh) {
                // Check cooldown
                if (stats.cooldown_remaining > 0) {
                    suppression = Suppression::Cooldown;
                }
                // Check ionospheric activity
                else if (!iono_stable) {
                    suppression = Suppression::IonoActive;
                }
                // Check oscillation guard: if last slip was opposite sign
                // and similar magnitude, this is likely an echo
                else if (stats.had_slip && std::abs(stats.last_slip_td2) > 0.0) {
                    const double ratio = ad.td2_gf / stats.last_slip_td2;
                    if (ratio < -(1.0 - kOscillationTolerance) &&
                        ratio > -(1.0 + kOscillationTolerance)) {
                        suppression = Suppression::Oscillation;
                    }
                }
            }

            const bool is_slip = ad.exceeds_thresh && (suppression == Suppression::None);
            const bool is_suppressed = ad.exceeds_thresh && (suppression != Suppression::None);

            // Trace
            if (trace_file_.is_open()) {
                trace_file_ << std::fixed << std::setprecision(3)
                            << current_epoch_.week << "," << current_epoch_.tow << ","
                            << ad.obs->satellite << ","
                            << PppUtils::getSystemString(ad.obs->system) << ","
                            << std::setprecision(2) << ad.obs->elevation << ","
                            << ad.sig1 << "," << ad.sig2 << ","
                            << std::setprecision(6) << ad.gf << ","
                            << ad.td2_gf << ","
                            << (std::isfinite(ad.sigma) ? ad.sigma : -1.0) << ","
                            << (std::isfinite(ad.thresh) ? ad.thresh : -1.0) << ","
                            << stats.count << ","
                            << std::setprecision(4) << ivi << ","
                            << (iono_stable ? 1 : 0) << ","
                            << (is_slip ? 1 : 0) << ","
                            << (is_suppressed ? 1 : 0) << "\n";
            }

            if (is_slip) {
                logger->warn(
                    "GF slip detected for {} ({}): |TD2_GF|={:.4f} m > thresh={:.4f} m, "
                    "sigma={:.4f} m, arc_n={}, IVI={:.4f} m",
                    ad.obs->satellite, PppUtils::getSystemString(ad.obs->system),
                    std::abs(ad.td2_gf), ad.thresh,
                    std::isfinite(ad.sigma) ? ad.sigma : -1.0, stats.count, ivi);

                results.push_back({ad.obs->satellite, "",
                                   ad.obs->satellite + "_" + ad.sig1, iono_stable});
                results.push_back({ad.obs->satellite, "",
                                   ad.obs->satellite + "_" + ad.sig2, iono_stable});

                // Record slip signature for oscillation guard on next arc
                stats.last_slip_td2 = ad.td2_gf;
                stats.had_slip = true;

                // Reset the arc with cooldown
                stats.prev_gf      = 0.0;
                stats.prev_prev_gf = 0.0;
                stats.last_epoch_s = 0.0;
                stats.count        = 0;
                stats.diff_window.clear();
                stats.cooldown_remaining = kCooldownEpochs;
                // Note: had_slip and last_slip_td2 are intentionally preserved
                continue;
            }

            if (is_suppressed) {
                const char* reason =
                    (suppression == Suppression::Cooldown)    ? "cooldown" :
                    (suppression == Suppression::IonoActive)  ? "iono_active" :
                    (suppression == Suppression::Oscillation) ? "oscillation" : "unknown";
                logger->debug(
                    "GF detection suppressed for {} ({}): |TD2|={:.4f} m > thresh={:.4f} m, "
                    "reason={}, IVI={:.4f} m",
                    ad.obs->satellite, PppUtils::getSystemString(ad.obs->system),
                    std::abs(ad.td2_gf), ad.thresh, reason, ivi);
            }

            // Update running statistics (even for suppressed exceedances:
            // do NOT add outlier TD2 values to the window to avoid threshold inflation)
            if (!ad.exceeds_thresh) {
                stats.diff_window.push_back(ad.td2_gf);
                if (stats.diff_window.size() > kGfWindowSize)
                    stats.diff_window.pop_front();
            }
            stats.count++;
            stats.prev_prev_gf = stats.prev_gf;
            stats.prev_gf      = ad.gf;
            stats.last_epoch_s = TimeUtils::continuousGpsSeconds(ad.obs->time);
            if (stats.cooldown_remaining > 0)
                --stats.cooldown_remaining;
            // Clear oscillation guard after a clean epoch
            if (!ad.exceeds_thresh) {
                stats.had_slip = false;
                stats.last_slip_td2 = 0.0;
            }
        }

        return results;
    }

    // =================================================================
    // External reset
    // =================================================================

    void GfSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("gf_slip_detector");

        const std::string prefix = satellite + "_";
        for (auto it = gf_stats_.begin(); it != gf_stats_.end(); )
            if (it->first.rfind(prefix, 0) == 0) {
                logger->debug("Cleared GF arc for {} (external reset, arc_n={})",
                    it->first, it->second.count);
                it = gf_stats_.erase(it);
            } else {
                ++it;
            }
    }

} // namespace GnssProcessor
