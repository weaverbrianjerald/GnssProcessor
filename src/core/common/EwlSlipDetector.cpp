/**
 * @file EwlSlipDetector.cpp
 * @brief Extra-Wide-Lane (EWL) cycle slip detector implementation.
 *
 * Organization:
 *  - Construction / tracing
 *  - Signal helpers
 *  - EWL MW computation
 *  - Statistics / threshold
 *  - Core detection logic
 *  - External reset
 */

#include "EwlSlipDetector.hpp"
#include "Constants.hpp"
#include "TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>

namespace GnssProcessor {

    // =================================================================
    // Construction
    // =================================================================

    EwlSlipDetector::EwlSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("cycle_slip_manager")->info(
            "EwlSlipDetector initialized (MAD-based, k={:.1f}, floor={:.1f} EWL-cyc, min_epochs={})",
            kMadSigmaMultiplier, kMinThresholdCycles, kMinEpochsForAdaptive);
    }

    // =================================================================
    // Tracing
    // =================================================================

    bool EwlSlipDetector::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("cycle_slip_manager");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "ewl_slip_trace.csv";
        trace_file_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!trace_file_.is_open()) {
            logger->warn("Could not open EWL trace file '{}'", trace_path.string());
            return false;
        }

        trace_file_ << "week,tow_s,satellite,system,elevation_deg,"
                    << "sig1,sig2,ewl_mw_m,ewl_mw_cycles,"
                    << "ewl_mean_m,ewl_mean_cycles,"
                    << "dev_from_mean_m,dev_from_mean_cycles,"
                    << "ewl_sigma_m,ewl_sigma_cycles,"
                    << "arc_count,"
                    << "lambda_ewl_m,threshold_m,threshold_cycles,"
                    << "is_slip,slip_cycles,fractional,"
                    << "bia_corrected,rounded_ewl,ewl_locked\n";

        logger->info("EWL tracing enabled: {}", trace_path.string());
        return true;
    }

    // =================================================================
    // Signal helpers
    // =================================================================

    std::vector<EwlSlipDetector::EwlPair>
    EwlSlipDetector::collectEwlPairs(const PppObservation& obs) const {
        // Group code and phase signals by RINEX band number (digit after C/L prefix).
        // Only consider bands >= 2 (non-L1) for EWL formation.
        struct BandSignals { std::string code; std::string phase; };
        std::map<char, BandSignals> bands;

        for (const auto& [key, val] : obs.raw_observed) {
            if (key.size() < 2) continue;
            const char prefix = key[0];
            if (prefix != 'C' && prefix != 'L') continue;
            // Skip combined (IF) labels containing '_'
            if (key.find('_') != std::string::npos) continue;
            const char band = key[1];
            if (band < '2' || band > '9') continue;  // non-L1 only

            auto& bs = bands[band];
            if (prefix == 'C' && bs.code.empty())  bs.code  = key;
            if (prefix == 'L' && bs.phase.empty()) bs.phase = key;
        }

        // Form all pairs of non-L1 bands that have both code and phase
        std::vector<EwlPair> pairs;
        std::vector<std::pair<char, BandSignals>> valid_bands;
        for (const auto& [band, bs] : bands) {
            if (!bs.code.empty() && !bs.phase.empty())
                valid_bands.emplace_back(band, bs);
        }

        for (size_t i = 0; i < valid_bands.size(); ++i) {
            for (size_t j = i + 1; j < valid_bands.size(); ++j) {
                // Order by frequency: higher freq first (lower band number)
                const auto& [band_a, bs_a] = valid_bands[i];
                const auto& [band_b, bs_b] = valid_bands[j];

                const double f_a = ppp_utils_.getFrequency(bs_a.phase, obs.system, obs.satellite);
                const double f_b = ppp_utils_.getFrequency(bs_b.phase, obs.system, obs.satellite);

                if (f_a <= 0.0 || f_b <= 0.0) continue;

                if (f_a >= f_b) {
                    pairs.push_back({bs_a.phase, bs_b.phase, bs_a.code, bs_b.code});
                } else {
                    pairs.push_back({bs_b.phase, bs_a.phase, bs_b.code, bs_a.code});
                }
            }
        }

        return pairs;
    }

    // =================================================================
    // EWL computation
    // =================================================================

    double EwlSlipDetector::computeEwlWavelength(const PppObservation& obs,
                                                   const std::string& sig1,
                                                   const std::string& sig2) const {
        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);

        if (f1 <= 0.0 || f2 <= 0.0 || std::abs(f1 - f2) < 1.0)
            return 0.0;

        return Constants::SPEED_OF_LIGHT / std::abs(f1 - f2);
    }

    double EwlSlipDetector::computeEwlMwMeters(const PppObservation& obs,
                                                 const EwlPair& pair) const {
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        // Get phase observables (cycles)
        auto itL1 = obs.raw_observed.find(pair.phase1);
        auto itL2 = obs.raw_observed.find(pair.phase2);
        if (itL1 == obs.raw_observed.end() || itL2 == obs.raw_observed.end())
            return NaN;

        // Get code observables (meters)
        auto itP1 = obs.raw_observed.find(pair.code1);
        auto itP2 = obs.raw_observed.find(pair.code2);
        if (itP1 == obs.raw_observed.end() || itP2 == obs.raw_observed.end())
            return NaN;

        const double L1_cyc = itL1->second;
        const double L2_cyc = itL2->second;
        const double P1_m   = itP1->second;
        const double P2_m   = itP2->second;

        if (!std::isfinite(L1_cyc) || !std::isfinite(L2_cyc) ||
            !std::isfinite(P1_m) || !std::isfinite(P2_m))
            return NaN;

        const double f1 = ppp_utils_.getFrequency(pair.phase1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(pair.phase2, obs.system, obs.satellite);
        if (f1 <= 0.0 || f2 <= 0.0 || std::abs(f1 - f2) < 1.0)
            return NaN;

        const double c = Constants::SPEED_OF_LIGHT;
        const double lam1 = c / f1;
        const double lam2 = c / f2;

        // Convert phase from cycles to meters
        const double L1_m = L1_cyc * lam1;
        const double L2_m = L2_cyc * lam2;

        // MW combination:
        //   MW = (f1·L1_m - f2·L2_m) / (f1 - f2) - (f1·P1_m + f2·P2_m) / (f1 + f2)
        const double mw = (f1 * L1_m - f2 * L2_m) / (f1 - f2)
                        - (f1 * P1_m + f2 * P2_m) / (f1 + f2);

        return std::isfinite(mw) ? mw : NaN;
    }

    bool EwlSlipDetector::computeSatEwlBias(const PppObservation& obs,
                                              const EwlPair& pair,
                                              double& bias_m) const {
        bias_m = 0.0;
        if (!corrections_) return false;

        const double f1 = ppp_utils_.getFrequency(pair.phase1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(pair.phase2, obs.system, obs.satellite);
        if (f1 <= 0.0 || f2 <= 0.0 || std::abs(f1 - f2) < 1.0)
            return false;

        // Check all four BIA entries exist
        if (!corrections_->hasBiasEntry(obs.satellite, pair.phase1) ||
            !corrections_->hasBiasEntry(obs.satellite, pair.phase2) ||
            !corrections_->hasBiasEntry(obs.satellite, pair.code1) ||
            !corrections_->hasBiasEntry(obs.satellite, pair.code2))
            return false;

        const double b_L1 = corrections_->getBiasCorrection(obs.satellite, pair.phase1, obs.time);
        const double b_L2 = corrections_->getBiasCorrection(obs.satellite, pair.phase2, obs.time);
        const double b_C1 = corrections_->getBiasCorrection(obs.satellite, pair.code1, obs.time);
        const double b_C2 = corrections_->getBiasCorrection(obs.satellite, pair.code2, obs.time);

        const double f_diff = f1 - f2;
        const double f_sum  = f1 + f2;

        // b_MW = (f1*b_L1 - f2*b_L2)/(f1-f2) - (f1*b_C1 + f2*b_C2)/(f1+f2)
        bias_m = (f1 * b_L1 - f2 * b_L2) / f_diff
               - (f1 * b_C1 + f2 * b_C2) / f_sum;

        return std::isfinite(bias_m);
    }

    // =================================================================
    // Statistics / threshold
    // =================================================================

    double EwlSlipDetector::computeMadSigma(const std::deque<double>& window) {
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

    double EwlSlipDetector::computeAdaptiveThreshold(const EwlArcStats& stats) const {
        const double sigma = computeMadSigma(stats.diff_window);
        if (!std::isfinite(sigma))
            return std::numeric_limits<double>::quiet_NaN();

        const double lambda_ewl = stats.lambda_ewl;
        const double floor_m = (lambda_ewl > 0.0) ? kMinThresholdCycles * lambda_ewl : 0.5;
        return std::max(kMadSigmaMultiplier * sigma, floor_m);
    }

    // =================================================================
    // Resolved ambiguities
    // =================================================================

    std::map<std::string, int> EwlSlipDetector::getResolvedEwlAmbiguities() const {
        std::map<std::string, int> result;
        for (const auto& [key, stats] : ewl_stats_) {
            if (stats.ewl_locked)
                result[key] = stats.rounded_ewl;
        }
        return result;
    }

    // =================================================================
    // Core detection logic
    // =================================================================

    std::vector<EwlSlipResult> EwlSlipDetector::detect(
        const std::vector<PppObservation>& observations) {

        std::vector<EwlSlipResult> results;
        auto logger = log_manager_.getLogger("cycle_slip_manager");

        // Capture epoch time from first valid observation
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg())
                continue;

            const auto ewl_pairs = collectEwlPairs(obs);
            if (ewl_pairs.empty()) continue;  // dual-freq only, no EWL available

            const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

            for (const auto& pair : ewl_pairs) {
                const double ewl_mw = computeEwlMwMeters(obs, pair);
                if (!std::isfinite(ewl_mw)) continue;

                const double lambda_ewl = computeEwlWavelength(obs, pair.phase1, pair.phase2);
                if (lambda_ewl <= 0.0) continue;

                // Try to remove satellite MW bias for near-integer EWL
                double sat_bias_m = 0.0;
                const bool bia_ok = computeSatEwlBias(obs, pair, sat_bias_m);
                const double ewl_corrected = bia_ok ? (ewl_mw - sat_bias_m) : ewl_mw;

                const std::string stats_key = obs.satellite + "_" + pair.phase1 + "_" + pair.phase2;
                auto& stats = ewl_stats_[stats_key];

                // Data gap detection
                if (stats.count > 0 && stats.last_epoch_s > 0.0) {
                    const double gap = epoch_s - stats.last_epoch_s;
                    if (gap > kMaxGapSeconds) {
                        logger->debug("EWL data gap {:.1f}s > {:.0f}s for {}: resetting arc",
                                      gap, kMaxGapSeconds, stats_key);
                        stats = EwlArcStats{};
                    }
                }

                stats.lambda_ewl = lambda_ewl;
                constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

                // First epoch: initialize
                if (stats.count == 0) {
                    stats.mean = ewl_corrected;
                    stats.count = 1;
                    stats.prev_ewl = ewl_corrected;
                    stats.last_epoch_s = epoch_s;
                    stats.bia_corrected = bia_ok;
                    stats.diff_window.clear();

                    // For BIA-corrected arcs, lock the EWL integer immediately
                    if (bia_ok) {
                        stats.rounded_ewl = static_cast<int>(std::round(ewl_corrected / lambda_ewl));
                        stats.ewl_locked = true;
                    }

                    if (trace_file_.is_open()) {
                        const double ewl_cyc = ewl_corrected / lambda_ewl;
                        trace_file_ << std::fixed << std::setprecision(3)
                                    << current_epoch_.week << "," << current_epoch_.tow << ","
                                    << obs.satellite << ","
                                    << PppUtils::getSystemString(obs.system) << ","
                                    << std::setprecision(2) << obs.elevation << ","
                                    << pair.phase1 << "," << pair.phase2 << ","
                                    << std::setprecision(6) << ewl_corrected << "," << ewl_cyc << ","
                                    << ewl_corrected << "," << ewl_cyc << ","
                                    << 0.0 << "," << 0.0 << ","
                                    << NaN << "," << NaN << ","
                                    << stats.count << ","
                                    << lambda_ewl << "," << NaN << "," << NaN << ","
                                    << 0 << "," << 0 << "," << NaN << ","
                                    << (bia_ok ? 1 : 0) << ","
                                    << stats.rounded_ewl << "," << (stats.ewl_locked ? 1 : 0) << "\n";
                    }
                    continue;
                }

                // Epoch 1+: compute deviation and check for slip
                const double diff = ewl_corrected - stats.prev_ewl;
                const double dev_from_mean = ewl_corrected - stats.mean;
                const double thresh = computeAdaptiveThreshold(stats);
                const double sigma = computeMadSigma(stats.diff_window);
                const bool exceeds = std::isfinite(thresh) && std::abs(dev_from_mean) > thresh;

                // Estimate integer slip magnitude
                int    slip_cycles = 0;
                double fractional  = 0.0;
                bool   slip_known  = false;

                if (exceeds && lambda_ewl > 0.0) {
                    const double dn_float = dev_from_mean / lambda_ewl;
                    slip_cycles = static_cast<int>(std::round(dn_float));
                    fractional  = std::abs(dn_float - slip_cycles);
                    slip_known  = (fractional < kMaxFractional) && (slip_cycles != 0);
                }

                const bool is_slip = exceeds;
                const double ewl_cyc = ewl_corrected / lambda_ewl;
                const double mean_cyc = stats.mean / lambda_ewl;
                const double dev_cyc = dev_from_mean / lambda_ewl;

                // Trace output
                if (trace_file_.is_open()) {
                    trace_file_ << std::fixed << std::setprecision(3)
                                << current_epoch_.week << "," << current_epoch_.tow << ","
                                << obs.satellite << ","
                                << PppUtils::getSystemString(obs.system) << ","
                                << std::setprecision(2) << obs.elevation << ","
                                << pair.phase1 << "," << pair.phase2 << ","
                                << std::setprecision(6) << ewl_corrected << "," << ewl_cyc << ","
                                << stats.mean << "," << mean_cyc << ","
                                << dev_from_mean << "," << dev_cyc << ","
                                << (std::isfinite(sigma) ? sigma : -1.0) << ","
                                << (std::isfinite(sigma) && lambda_ewl > 0.0 ? sigma / lambda_ewl : -1.0) << ","
                                << stats.count << ","
                                << lambda_ewl << ","
                                << (std::isfinite(thresh) ? thresh : -1.0) << ","
                                << (std::isfinite(thresh) && lambda_ewl > 0.0 ? thresh / lambda_ewl : -1.0) << ","
                                << (is_slip ? 1 : 0) << ","
                                << slip_cycles << ","
                                << std::setprecision(3) << fractional << ","
                                << (bia_ok ? 1 : 0) << ","
                                << stats.rounded_ewl << "," << (stats.ewl_locked ? 1 : 0) << "\n";
                }

                if (is_slip) {
                    logger->info(
                        "EWL slip detected for {} {}-{}: |dev|={:.4f} m ({:.3f} EWL-cyc) > thresh={:.4f} m, "
                        "dN_EWL={}, frac={:.3f}, lambda_EWL={:.3f} m, arc_n={}",
                        obs.satellite, pair.phase1, pair.phase2,
                        std::abs(dev_from_mean), std::abs(dev_cyc), thresh,
                        slip_cycles, fractional, lambda_ewl, stats.count);

                    EwlSlipResult sr;
                    sr.satellite = obs.satellite;
                    sr.sig1 = pair.phase1;
                    sr.sig2 = pair.phase2;
                    sr.amb_key = obs.satellite + "_" + pair.phase1;
                    sr.slip_ewl_cycles = slip_cycles;
                    sr.slip_ewl_known = slip_known;
                    sr.lambda_ewl = lambda_ewl;
                    sr.fractional = fractional;
                    results.push_back(sr);

                    // Reset this arc
                    ewl_stats_.erase(stats_key);
                    continue;
                }

                // Update running statistics (Hatch filter)
                stats.count++;
                const size_t effective_n = std::min(stats.count, kMaxHatchCount);
                stats.mean = stats.mean + (ewl_corrected - stats.mean) / static_cast<double>(effective_n);

                stats.diff_window.push_back(diff);
                if (stats.diff_window.size() > kWindowSize)
                    stats.diff_window.pop_front();

                stats.prev_ewl = ewl_corrected;
                stats.last_epoch_s = epoch_s;
                stats.bia_corrected = bia_ok;

                // Lock EWL integer once we have enough samples or immediately for BIA-corrected
                if (!stats.ewl_locked && bia_ok) {
                    stats.rounded_ewl = static_cast<int>(std::round(stats.mean / lambda_ewl));
                    stats.ewl_locked = true;
                } else if (!stats.ewl_locked && stats.count >= kMinEpochsForAdaptive) {
                    const double mean_frac = std::abs(stats.mean / lambda_ewl - std::round(stats.mean / lambda_ewl));
                    if (mean_frac < 0.15) {
                        stats.rounded_ewl = static_cast<int>(std::round(stats.mean / lambda_ewl));
                        stats.ewl_locked = true;
                    }
                }
            }
        }

        return results;
    }

    // =================================================================
    // External reset
    // =================================================================

    void EwlSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("cycle_slip_manager");

        const std::string prefix = satellite + "_";
        for (auto it = ewl_stats_.begin(); it != ewl_stats_.end(); )
            if (it->first.rfind(prefix, 0) == 0) {
                logger->debug("Cleared EWL arc for {} (external reset, arc_n={})",
                    it->first, it->second.count);
                it = ewl_stats_.erase(it);
            } else {
                ++it;
            }
    }

} // namespace GnssProcessor
