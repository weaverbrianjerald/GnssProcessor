/*!
 * \file MwSlipDetector.cpp
 * \brief Adaptive MW wide-lane slip detector.
 *
 * Organization:
 *  - Construction
 *  - Tracing (CSV output)
 *  - Signal / frequency helpers
 *  - MW computation
 *  - Statistics / threshold
 *  - Core detection logic
 *  - External reset
 */

#include "MwSlipDetector.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>

namespace GnssProcessor {

    // =================================================================
    // Construction
    // =================================================================

    MwSlipDetector::MwSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("mw_slip_detector")->info(
            "MwSlipDetector initialized (adaptive MAD-based, k={:.1f}, min_epochs={})",
            kMadSigmaMultiplier, kMinEpochsForAdaptive);
    }

    // =================================================================
    // Tracing (CSV output)
    // =================================================================

    bool MwSlipDetector::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("mw_slip_detector");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "mwwl_trace.csv";
        mwwl_trace_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!mwwl_trace_.is_open()) {
            logger->warn("Could not open MWWL trace file '{}'", trace_path.string());
            return false;
        }

        mwwl_trace_ << "week,tow_s,satellite,system,elevation_deg,"
                    << "sig1,sig2,mw_m,mw_cycles,"
                    << "mw_mean_m,mw_mean_cycles,"
                    << "dev_from_mean_m,dev_from_mean_cycles,"
                    << "mw_sigma_m,mw_sigma_cycles,"
                    << "arc_count,"
                    << "lambda_wl_m,threshold_m,threshold_cycles,"
                    << "is_slip,is_lli\n";

        logger->info("MWWL tracing enabled: {}", trace_path.string());
        return true;
    }

    void MwSlipDetector::writeTraceRecord(const PppObservation& obs,
                                           const std::string& sig1,
                                           const std::string& sig2,
                                           double mw_m,
                                           double mw_mean,
                                           double dev_from_mean,
                                           double mw_sigma,
                                           size_t arc_count,
                                           double lambda_wl,
                                           double thresh_m,
                                           bool is_slip,
                                           bool is_lli) const {
        if (!mwwl_trace_.is_open()) return;

        const auto to_cyc = [&](double val_m) -> double {
            return (lambda_wl > 0.0 && std::isfinite(val_m)) ? val_m / lambda_wl
                                                             : std::numeric_limits<double>::quiet_NaN();
        };

        const double elev_deg = std::isfinite(obs.elevation) ? obs.elevation
                                                             : std::numeric_limits<double>::quiet_NaN();

        mwwl_trace_ << std::fixed << std::setprecision(3)
                    << current_epoch_.week << "," << current_epoch_.tow << ","
                    << obs.satellite << ","
                    << PppUtils::getSystemString(obs.system) << ","
                    << std::setprecision(2) << elev_deg << ","
                    << sig1 << "," << sig2 << ","
                    << std::setprecision(6) << mw_m << "," << to_cyc(mw_m) << ",";

        auto csv_pair = [&](double val_m) {
            if (std::isfinite(val_m))
                mwwl_trace_ << val_m << "," << to_cyc(val_m) << ",";
            else
                mwwl_trace_ << ",,";
        };
        csv_pair(mw_mean);
        csv_pair(dev_from_mean);
        csv_pair(mw_sigma);

        mwwl_trace_ << arc_count << ","
                    << lambda_wl << "," << thresh_m << "," << to_cyc(thresh_m) << ","
                    << (is_slip ? 1 : 0) << "," << (is_lli ? 1 : 0) << "\n";
    }

    // =================================================================
    // Signal / frequency helpers
    // =================================================================

    bool MwSlipDetector::findIonoFreeLabels(const PppObservation& obs,
        std::string& phase_if_key, std::string& code_if_key) const {
        phase_if_key.clear();
        code_if_key.clear();

        for (const auto& [k, v] : obs.observed) {
            if (k.find('_') == std::string::npos || k.empty()) continue;
            if (k[0] == 'L' && phase_if_key.empty()) phase_if_key = k;
            if (k[0] == 'C' && code_if_key.empty()) code_if_key = k;
            if (!phase_if_key.empty() && !code_if_key.empty()) return true;
        }
        return (!phase_if_key.empty() && !code_if_key.empty());
    }

    bool MwSlipDetector::splitIfLabel(const std::string& label,
        std::string& sig1, std::string& sig2) const {
        sig1.clear();
        sig2.clear();
        const size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= label.size()) return false;
        sig1 = label.substr(0, pos);
        sig2 = label.substr(pos + 1);
        return (!sig1.empty() && !sig2.empty());
    }

    double MwSlipDetector::computeWideLaneWavelengthMeters(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2) const {

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);

        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) {
            log_manager_.getLogger("mw_slip_detector")->debug(
                "Invalid frequencies for {} {}: sig1={} f1={:.3f} MHz, sig2={} f2={:.3f} MHz",
                obs.satellite, PppUtils::getSystemString(obs.system),
                sig1, f1 / 1e6, sig2, f2 / 1e6);
            return 0.0;
        }

        const double df = std::abs(f1 - f2);
        if (!(std::isfinite(df) && df > 0.0)) return 0.0;

        const double lambda_wl = Constants::SPEED_OF_LIGHT / df;
        return (std::isfinite(lambda_wl) && lambda_wl > 0.0) ? lambda_wl : 0.0;
    }

    bool MwSlipDetector::hasAnyPhaseLockLoss(const PppObservation& obs) const {
        for (const auto& [type, lli] : obs.lli_flags)
            if (!type.empty() && type[0] == 'L' && lli != 0) return true;
        return false;
    }

    std::vector<MwSlipDetector::UncombinedPair>
    MwSlipDetector::collectUncombinedPairs(const PppObservation& obs) const {
        struct BandSignals { std::string code; std::string phase; };
        std::map<char, BandSignals> bands;

        for (const auto& [key, val] : obs.observed) {
            if (key.size() < 2) continue;
            const char prefix = key[0];
            if (prefix != 'C' && prefix != 'L') continue;
            if (key.find('_') != std::string::npos) continue;
            const char band = key[1];
            if (band < '1' || band > '9') continue;

            auto& bs = bands[band];
            if (prefix == 'C' && bs.code.empty())  bs.code  = key;
            if (prefix == 'L' && bs.phase.empty()) bs.phase = key;
        }

        std::vector<char> band_ids;
        for (const auto& [b, bs] : bands)
            if (!bs.code.empty() && !bs.phase.empty())
                band_ids.push_back(b);

        std::vector<UncombinedPair> pairs;
        for (size_t i = 0; i < band_ids.size(); ++i)
            for (size_t j = i + 1; j < band_ids.size(); ++j) {
                const auto& b1 = bands[band_ids[i]];
                const auto& b2 = bands[band_ids[j]];
                pairs.push_back({b1.phase, b2.phase, b1.code, b2.code});
            }
        return pairs;
    }

    // =================================================================
    // MW computation
    // =================================================================

    double MwSlipDetector::computeMwMeters(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2,
        const std::string& code1, const std::string& code2) const {

        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        if (sig1.empty() || sig2.empty() || sig1[0] != 'L' || sig2[0] != 'L') return NaN;
        if (code1.empty() || code2.empty() || code1[0] != 'C' || code2[0] != 'C') return NaN;

        auto itL1 = obs.raw_observed.find(sig1);
        auto itL2 = obs.raw_observed.find(sig2);
        auto itP1 = obs.raw_observed.find(code1);
        auto itP2 = obs.raw_observed.find(code2);

        if (itL1 == obs.raw_observed.end() || itL2 == obs.raw_observed.end() ||
            itP1 == obs.raw_observed.end() || itP2 == obs.raw_observed.end())
            return NaN;

        const double L1_cyc = itL1->second;
        const double L2_cyc = itL2->second;
        const double P1_m   = itP1->second;
        const double P2_m   = itP2->second;

        if (!std::isfinite(L1_cyc) || !std::isfinite(L2_cyc) ||
            !std::isfinite(P1_m) || !std::isfinite(P2_m))
            return NaN;

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);
        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) return NaN;

        const double f_diff = f1 - f2;
        const double f_sum  = f1 + f2;
        if (std::abs(f_diff) < 1.0 || std::abs(f_sum) < 1.0) return NaN;

        const double c = Constants::SPEED_OF_LIGHT;
        const double L1_m = L1_cyc * c / f1;
        const double L2_m = L2_cyc * c / f2;

        const double wl_phase_m = (f1 * L1_m - f2 * L2_m) / f_diff;
        const double nl_code_m  = (f1 * P1_m + f2 * P2_m) / f_sum;
        const double mw_m = wl_phase_m - nl_code_m;

        return std::isfinite(mw_m) ? mw_m : NaN;
    }

    // =================================================================
    // Statistics / threshold
    // =================================================================

    double MwSlipDetector::computeMadSigma(const MwArcStats& stats) {
        if (stats.diff_window.size() < kMinEpochsForAdaptive)
            return std::numeric_limits<double>::quiet_NaN();

        std::vector<double> sorted(stats.diff_window.begin(), stats.diff_window.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        const double median = (n % 2 == 0) ? 0.5 * (sorted[n / 2 - 1] + sorted[n / 2])
                                           : sorted[n / 2];

        std::vector<double> abs_devs(n);
        for (size_t i = 0; i < n; ++i)
            abs_devs[i] = std::abs(sorted[i] - median);
        std::sort(abs_devs.begin(), abs_devs.end());
        const double mad = (n % 2 == 0) ? 0.5 * (abs_devs[n / 2 - 1] + abs_devs[n / 2])
                                        : abs_devs[n / 2];

        return mad * 1.4826;
    }

    double MwSlipDetector::computeAdaptiveThreshold(const MwArcStats& stats, double lambda_wl) const {
        const double sigma = computeMadSigma(stats);
        if (!std::isfinite(sigma))
            return std::numeric_limits<double>::quiet_NaN();

        const double min_thresh = kMinThresholdCycles * lambda_wl;
        return std::max(kMadSigmaMultiplier * sigma, min_thresh);
    }

    bool MwSlipDetector::processMwSample(const PppObservation& obs,
        const std::string& stats_key,
        double mw_m, double lambda_wl,
        const std::string& sig1, const std::string& sig2) {

        auto& stats = mw_stats_[stats_key];
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        // First epoch: initialize arc
        if (stats.count == 0) {
            stats.mean    = mw_m;
            stats.M2      = 0.0;
            stats.count   = 1;
            stats.prev_mw = mw_m;
            stats.diff_window.clear();

            if (mwwl_trace_.is_open())
                writeTraceRecord(obs, sig1, sig2, mw_m, mw_m, 0.0, NaN,
                                 stats.count, lambda_wl, NaN, false, false);
            return false;
        }

        const double mw_diff = mw_m - stats.prev_mw;
        const double dev     = mw_m - stats.mean;
        const double thresh  = computeAdaptiveThreshold(stats, lambda_wl);
        const double sigma   = computeMadSigma(stats);
        const bool is_slip   = std::isfinite(thresh) && std::abs(dev) > thresh;

        if (mwwl_trace_.is_open())
            writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean, dev, sigma,
                             stats.count, lambda_wl, thresh, is_slip, false);

        if (is_slip) {
            auto logger = log_manager_.getLogger("mw_slip_detector");
            logger->warn(
                "MW slip detected for {} ({}): |dev|={:.3f} m ({:.1f} WL) > thresh={:.3f} m ({:.1f} WL), "
                "mean={:.3f} m, sigma={:.4f} m, arc_n={}, lambda_wl={:.3f} m",
                obs.satellite, PppUtils::getSystemString(obs.system),
                std::abs(dev), std::abs(dev) / lambda_wl,
                thresh, thresh / lambda_wl,
                stats.mean, std::isfinite(sigma) ? sigma : -1.0,
                stats.count, lambda_wl);
            mw_stats_.erase(stats_key);
            return true;
        }

        // Update running statistics (Welford / Hatch)
        stats.count++;
        const double delta  = mw_m - stats.mean;
        stats.mean += delta / static_cast<double>(stats.count);
        const double delta2 = mw_m - stats.mean;
        stats.M2 += delta * delta2;

        stats.diff_window.push_back(mw_diff);
        if (stats.diff_window.size() > kMwWindowSize)
            stats.diff_window.pop_front();
        stats.prev_mw = mw_m;

        return false;
    }

    // =================================================================
    // Core detection logic
    // =================================================================

    std::vector<std::string> MwSlipDetector::detectLargeMwResets(
        const std::vector<PppObservation>& observations) {

        std::vector<std::string> reset_keys;
        reset_keys.reserve(observations.size());
        auto logger = log_manager_.getLogger("mw_slip_detector");

        // Capture epoch time from first valid observation for tracing
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;

            const bool has_lli = hasAnyPhaseLockLoss(obs);

            // Try combined (PPP) path first
            std::string phase_if_key, code_if_key;
            const bool has_if = findIonoFreeLabels(obs, phase_if_key, code_if_key);

            if (has_if) {
                // ---- Combined (PPP) path ----
                std::string s1, s2, c1, c2;
                if (!splitIfLabel(phase_if_key, s1, s2)) continue;
                if (!splitIfLabel(code_if_key, c1, c2)) continue;

                if (has_lli) {
                    const std::string amb_key = obs.satellite + "_" + phase_if_key;
                    reset_keys.push_back(amb_key);
                    logger->warn("LLI lock loss detected: scheduling ambiguity reset for {}", amb_key);
                    if (mwwl_trace_.is_open()) {
                        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
                        writeTraceRecord(obs, s1, s2, computeMwMeters(obs, s1, s2, c1, c2),
                                         NaN, NaN, NaN, 0,
                                         computeWideLaneWavelengthMeters(obs, s1, s2),
                                         NaN, false, true);
                    }
                    mw_stats_.erase(obs.satellite);
                    continue;
                }

                const double lambda_wl = computeWideLaneWavelengthMeters(obs, s1, s2);
                if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) continue;
                const double mw_m = computeMwMeters(obs, s1, s2, c1, c2);
                if (!std::isfinite(mw_m)) continue;

                if (processMwSample(obs, obs.satellite, mw_m, lambda_wl, s1, s2)) {
                    const std::string amb_key = obs.satellite + "_" + phase_if_key;
                    reset_keys.push_back(amb_key);
                    logger->warn("Scheduling reset for {}", amb_key);
                }
            } else {
                // ---- Uncombined (PPP-RTK) path ----
                const auto uc_pairs = collectUncombinedPairs(obs);
                if (uc_pairs.empty()) continue;

                if (has_lli) {
                    for (const auto& [key, val] : obs.observed)
                        if (!key.empty() && key[0] == 'L' && key.find('_') == std::string::npos)
                            reset_keys.push_back(obs.satellite + "_" + key);
                    logger->warn("LLI lock loss (uncombined): scheduling all phase ambiguity resets for {}", obs.satellite);
                    for (auto it = mw_stats_.begin(); it != mw_stats_.end(); )
                        it = (it->first.rfind(obs.satellite, 0) == 0) ? mw_stats_.erase(it) : std::next(it);
                    continue;
                }

                for (const auto& pair : uc_pairs) {
                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, pair.phase1, pair.phase2);
                    if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) continue;
                    const double mw_m = computeMwMeters(obs, pair.phase1, pair.phase2, pair.code1, pair.code2);
                    if (!std::isfinite(mw_m)) continue;

                    const std::string stats_key = obs.satellite + "_" + pair.phase1 + "_" + pair.phase2;
                    if (processMwSample(obs, stats_key, mw_m, lambda_wl, pair.phase1, pair.phase2)) {
                        const std::string amb1 = obs.satellite + "_" + pair.phase1;
                        const std::string amb2 = obs.satellite + "_" + pair.phase2;
                        reset_keys.push_back(amb1);
                        reset_keys.push_back(amb2);
                        logger->warn("Scheduling reset for {}, {}", amb1, amb2);
                    }
                }
            }
        }

        return reset_keys;
    }

    // =================================================================
    // External reset
    // =================================================================

    void MwSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("mw_slip_detector");

        auto it = mw_stats_.find(satellite);
        if (it != mw_stats_.end()) {
            logger->debug("Cleared MW arc history for {} (external reset, arc_n={})",
                satellite, it->second.count);
            mw_stats_.erase(it);
        }

        const std::string prefix = satellite + "_";
        for (auto cit = mw_stats_.begin(); cit != mw_stats_.end(); )
            if (cit->first.rfind(prefix, 0) == 0) {
                logger->debug("Cleared MW arc history for {} (external reset, arc_n={})",
                    cit->first, cit->second.count);
                cit = mw_stats_.erase(cit);
            } else {
                ++cit;
            }
    }

} // namespace GnssProcessor