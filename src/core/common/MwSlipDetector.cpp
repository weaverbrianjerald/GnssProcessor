/**
 * @file MwSlipDetector.cpp
 * @brief Implementation of MwSlipDetector for detecting large wide-lane (MW) resets/slips.
 *
 * The MwSlipDetector computes an MW-like metric from ionosphere-free phase/code
 * combinations and maintains a short per-satellite history. When the change in
 * the MW-like metric between epochs exceeds a configured threshold (expressed
 * in wide-lane cycles) the detector flags the corresponding ambiguity key for reset.
 *
 * The detector also flags immediate resets when loss-of-lock indicators (LLI)
 * are observed on phase observables.
 */

#include "MwSlipDetector.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>

namespace GnssProcessor {

    /**
     * @brief Construct a MwSlipDetector.
     *
     * Stores references to configuration and logging utilities and initializes
     * the local PppUtils helper. Emits an informational log indicating mode and
     * the configured large-MW threshold.
     *
     * @param config Reference to GNSS processing configuration.
     * @param log_manager Reference to LogManager used for diagnostic messages.
     */
    MwSlipDetector::MwSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        auto logger = log_manager_.getLogger("mw_slip_detector");
        logger->info("MwSlipDetector initialized (large MW only): thresh={:.2f} WL cycles", large_mw_threshold_cycles_);
    }

    /**
     * @brief Enable CSV tracing for MWWL diagnostics.
     *
     * Opens or creates the file "mwwl_trace.csv" in the specified directory.
     * Writes a CSV header line with column descriptions.
     *
     * @param log_dir Directory where the trace file will be created.
     * @return true if the file was successfully opened, false otherwise.
     */
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

        // Write CSV header
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

    /**
     * @brief Write a single MW observation record to the trace CSV.
     *
     * Called internally during detectLargeMwResets() when tracing is enabled.
     */
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

        const double mw_cycles       = to_cyc(mw_m);
        const double mean_cycles     = to_cyc(mw_mean);
        const double dev_cycles      = to_cyc(dev_from_mean);
        const double sigma_cycles    = to_cyc(mw_sigma);
        const double thresh_cycles   = to_cyc(thresh_m);

        const double elev_deg = std::isfinite(obs.elevation) ? obs.elevation
                                                             : std::numeric_limits<double>::quiet_NaN();

        mwwl_trace_ << std::fixed << std::setprecision(3)
                    << current_epoch_.week << "," << current_epoch_.tow << ","
                    << obs.satellite << ","
                    << PppUtils::getSystemString(obs.system) << ","
                    << std::setprecision(2) << elev_deg << ","
                    << sig1 << "," << sig2 << ","
                    << std::setprecision(6) << mw_m << "," << mw_cycles << ",";

        // Running mean
        if (std::isfinite(mw_mean)) {
            mwwl_trace_ << mw_mean << "," << mean_cycles << ",";
        } else {
            mwwl_trace_ << ",,";
        }

        // Deviation from mean
        if (std::isfinite(dev_from_mean)) {
            mwwl_trace_ << dev_from_mean << "," << dev_cycles << ",";
        } else {
            mwwl_trace_ << ",,";
        }

        // MAD-derived sigma
        if (std::isfinite(mw_sigma)) {
            mwwl_trace_ << mw_sigma << "," << sigma_cycles << ",";
        } else {
            mwwl_trace_ << ",,";
        }

        mwwl_trace_ << arc_count << ","
                    << lambda_wl << "," << thresh_m << "," << thresh_cycles << ","
                    << (is_slip ? 1 : 0) << "," << (is_lli ? 1 : 0) << "\n";
    }

    /**
     * @brief Find ionosphere-free phase and code labels in an observation.
     *
     * The function scans the observation's `observed` map looking for the first
     * ionosphere-free phase label (starts with 'L' and contains '_') and the
     * first ionosphere-free code label (starts with 'C' and contains '_').
     * On success the found labels are written to @p phase_if_key and @p code_if_key.
     *
     * @param obs Observation to inspect.
     * @param[out] phase_if_key Output ionosphere-free phase label (e.g., "L1C_L2W").
     * @param[out] code_if_key Output ionosphere-free code label  (e.g., "C1C_C2W").
     * @return true if both phase and code IF labels were found, false otherwise.
     */
    bool MwSlipDetector::findIonoFreeLabels(const PppObservation& obs, std::string& phase_if_key, std::string& code_if_key) const {
        phase_if_key.clear();
        code_if_key.clear();

        // Find first iono-free phase+code combos produced by PppModel ("L..._L..." and "C..._C...")
        for (const auto& [k, v] : obs.observed) {
            if (k.find('_') == std::string::npos) continue;
            if (k.empty()) continue;

            if (k[0] == 'L' && phase_if_key.empty()) phase_if_key = k;
            if (k[0] == 'C' && code_if_key.empty()) code_if_key = k;

            if (!phase_if_key.empty() && !code_if_key.empty()) return true;
        }
        return (!phase_if_key.empty() && !code_if_key.empty());
    }

    /**
     * @brief Split an ionosphere-free label into its two signal components.
     *
     * Example: "L1C_L2W" -> sig1 = "L1C", sig2 = "L2W".
     *
     * @param label Input IF label containing exactly one underscore.
     * @param[out] sig1 First signal token.
     * @param[out] sig2 Second signal token.
     * @return true when the label was successfully split into two non-empty tokens.
     */
    bool MwSlipDetector::splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const {
        sig1.clear();
        sig2.clear();
        const size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= label.size()) return false;
        sig1 = label.substr(0, pos);
        sig2 = label.substr(pos + 1);
        return (!sig1.empty() && !sig2.empty());
    }

    /**
     * @brief Compute the wide-lane (WL) wavelength in meters for two signals.
     *
     * Uses PppUtils to obtain per-system frequencies for the two signal identifiers
     * and computes the WL wavelength as c / |f1 - f2|. Returns 0.0 on invalid input
     * (non-finite or non-positive frequencies, zero frequency difference, etc.).
     *
     * @param obs Observation whose system/satellite are used to resolve frequencies.
     * @param sig1 First signal identifier (e.g. "L1C").
     * @param sig2 Second signal identifier (e.g. "L2W").
     * @return double Wide-lane wavelength (meters) or 0.0 on error.
     */
    double MwSlipDetector::computeWideLaneWavelengthMeters(const PppObservation& obs,
        const std::string& sig1,
        const std::string& sig2) const {

        // Use per-system frequency mapping (and per-sat if GLONASS ever becomes channel-aware in PppUtils)
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

    /**
     * @brief Compute the Melbourne-Wübbena Wide-Lane (MWWL) observable in meters.
     *
     * Computes the full MW combination:
     *   MW = (f1·L1 - f2·L2)/(f1-f2) - (f1·P1 + f2·P2)/(f1+f2)
     *
     * where L1, L2 are phase observables (meters) and P1, P2 are code
     * observables (meters). The result is geometry-free and ionosphere-free,
     * proportional to the wide-lane ambiguity plus noise.
     *
     * @param obs Observation containing raw phase/code values and metadata.
     * @param sig1 First signal identifier (e.g. "L1C").
     * @param sig2 Second signal identifier (e.g. "L2W").
     * @return double MW value in meters, or quiet_NaN if missing/invalid.
     */
    double MwSlipDetector::computeMwMeters(const PppObservation& obs,
        const std::string& sig1,
        const std::string& sig2) const {

        // Build observable keys: phase uses "L..." prefix, code uses "C..." prefix
        if (sig1.empty() || sig2.empty() || sig1[0] != 'L' || sig2[0] != 'L') {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const std::string code1 = "C" + sig1.substr(1);  // e.g., "L1C" -> "C1C"
        const std::string code2 = "C" + sig2.substr(1);  // e.g., "L2W" -> "C2W"

        auto itL1 = obs.raw_observed.find(sig1);
        auto itL2 = obs.raw_observed.find(sig2);
        auto itP1 = obs.raw_observed.find(code1);
        auto itP2 = obs.raw_observed.find(code2);

        if (itL1 == obs.raw_observed.end() || itL2 == obs.raw_observed.end() ||
            itP1 == obs.raw_observed.end() || itP2 == obs.raw_observed.end()) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        // RINEX phase is in cycles, code is in meters
        const double L1_cyc = itL1->second;  // Phase on freq 1 (cycles)
        const double L2_cyc = itL2->second;  // Phase on freq 2 (cycles)
        const double P1_m = itP1->second;    // Code on freq 1 (meters)
        const double P2_m = itP2->second;    // Code on freq 2 (meters)

        if (!std::isfinite(L1_cyc) || !std::isfinite(L2_cyc) ||
            !std::isfinite(P1_m) || !std::isfinite(P2_m)) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);

        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const double f_diff = f1 - f2;
        const double f_sum = f1 + f2;

        if (std::abs(f_diff) < 1.0 || std::abs(f_sum) < 1.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        // Convert phase from cycles to meters: L_m = L_cyc * c / f
        const double L1_m = L1_cyc * Constants::SPEED_OF_LIGHT / f1;
        const double L2_m = L2_cyc * Constants::SPEED_OF_LIGHT / f2;

        // Wide-lane wavelength
        const double lambda_wl = Constants::SPEED_OF_LIGHT / std::abs(f_diff);

        // Wide-lane phase in meters: (f1*L1 - f2*L2) / (f1 - f2)
        const double wl_phase_meters = (f1 * L1_m - f2 * L2_m) / f_diff;

        // Narrow-lane code combination in meters
        const double nl_code_m = (f1 * P1_m + f2 * P2_m) / f_sum;

        // MW observable in meters
        const double mw_m = wl_phase_meters - nl_code_m;

        return std::isfinite(mw_m) ? mw_m : std::numeric_limits<double>::quiet_NaN();
    }

    /**
     * @brief Check whether any phase observable in the observation reports loss-of-lock.
     *
     * Scans the LLI map for entries whose key starts with 'L' and whose flag is non-zero.
     * Returns true when any such indicator is present (used to force immediate ambiguity resets).
     *
     * @param obs Observation to inspect.
     * @return true if any phase LLI indicates loss-of-lock, false otherwise.
     */
    bool MwSlipDetector::hasAnyPhaseLockLoss(const PppObservation& obs) const {
        for (const auto& [type, lli] : obs.lli_flags) {
            if (!type.empty() && type[0] == 'L' && lli != 0) return true;
        }
        return false;
    }

    /**
     * @brief Compute the MAD-based adaptive threshold for a satellite arc.
     *
     * The Median Absolute Deviation (MAD) of the values in the rolling window
     * is scaled by 1.4826 to give a Gaussian-equivalent sigma, then multiplied
     * by kMadSigmaMultiplier.  The result is clamped to a minimum of
     * kMinThresholdCycles * lambda_wl to avoid overly tight thresholds during
     * convergence.
     *
     * @param stats  Current arc statistics for the satellite.
     * @param lambda_wl Wide-lane wavelength (meters).
     * @return double Adaptive threshold in meters.
     */
    double MwSlipDetector::computeAdaptiveThreshold(const MwArcStats& stats, double lambda_wl) const {
        if (stats.count < kMinEpochsForAdaptive || stats.window.size() < kMinEpochsForAdaptive) {
            return large_mw_threshold_cycles_ * lambda_wl;
        }

        // Compute median of the window values
        std::vector<double> sorted(stats.window.begin(), stats.window.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        const double median = (n % 2 == 0) ? 0.5 * (sorted[n / 2 - 1] + sorted[n / 2])
                                           : sorted[n / 2];

        // Compute MAD = median(|x_i - median|)
        std::vector<double> abs_devs(n);
        for (size_t i = 0; i < n; ++i) {
            abs_devs[i] = std::abs(sorted[i] - median);
        }
        std::sort(abs_devs.begin(), abs_devs.end());
        const double mad = (n % 2 == 0) ? 0.5 * (abs_devs[n / 2 - 1] + abs_devs[n / 2])
                                        : abs_devs[n / 2];

        // Convert MAD to Gaussian-equivalent sigma:  sigma = MAD * 1.4826
        const double sigma_mad = mad * 1.4826;

        // Adaptive threshold: k * sigma, but at least kMinThresholdCycles WL cycles
        const double min_thresh = kMinThresholdCycles * lambda_wl;
        return std::max(kMadSigmaMultiplier * sigma_mad, min_thresh);
    }

    /**
     * @brief Detect large MW resets for a set of epoch observations.
     *
     * For each satellite the method:
     * - Skips invalid or low-elevation observations;
     * - Identifies ionosphere-free phase/code labels;
     * - Computes the wide-lane wavelength and MW observable;
     * - Compares the deviation of the current MW from the running mean
     *   against an adaptive threshold derived from the MAD of the
     *   satellite's MW history window.  Before enough history has
     *   accumulated (< kMinEpochsForAdaptive epochs) the fixed
     *   large_mw_threshold_cycles_ is used as a fallback.
     * - Schedules an ambiguity reset when a slip is detected or
     *   when LLI indicates loss-of-lock.
     * - On a clean epoch, updates the running mean (Hatch filter),
     *   Welford variance, and rolling window.
     *
     * The returned vector contains ambiguity keys to reset
     * (format: "<sat>_<phase_if_label>").
     *
     * @param observations Vector of observations at the epoch.
     * @return std::vector<std::string> Ambiguity keys to reset this epoch.
     */
    std::vector<std::string> MwSlipDetector::detectLargeMwResets(const std::vector<PppObservation>& observations) {
        std::vector<std::string> reset_keys;
        reset_keys.reserve(observations.size());

        auto logger = log_manager_.getLogger("mw_slip_detector");

        // Capture epoch time from first valid observation for tracing
        for (const auto& obs : observations) {
            if (!obs.satellite.empty()) {
                current_epoch_ = obs.time;
                break;
            }
        }

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;

            std::string phase_if_key, code_if_key;
            if (!findIonoFreeLabels(obs, phase_if_key, code_if_key)) continue;

            // Split the IF phase label into its component signals to compute WL wavelength
            std::string s1, s2;
            if (!splitIfLabel(phase_if_key, s1, s2)) continue;

            // Check for LLI-based reset
            const bool has_lli = hasAnyPhaseLockLoss(obs);
            if (has_lli) {
                const std::string amb_key = obs.satellite + "_" + phase_if_key;
                reset_keys.push_back(amb_key);
                logger->warn("LLI lock loss detected: scheduling ambiguity reset for {}", amb_key);

                // Write trace record for LLI event
                if (mwwl_trace_.is_open()) {
                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, s1, s2);
                    const double mw_m = computeMwMeters(obs, s1, s2);
                    const double thresh_m = large_mw_threshold_cycles_ * lambda_wl;
                    writeTraceRecord(obs, s1, s2, mw_m,
                                     std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::quiet_NaN(),
                                     0, lambda_wl, thresh_m, false, true);
                }

                mw_stats_.erase(obs.satellite);
                continue;
            }

            const double lambda_wl = computeWideLaneWavelengthMeters(obs, s1, s2);
            if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) continue;

            const double mw_m = computeMwMeters(obs, s1, s2);
            if (!std::isfinite(mw_m)) continue;

            auto& stats = mw_stats_[obs.satellite];

            if (stats.count == 0) {
                // First observation for this satellite arc - initialize statistics
                stats.mean  = mw_m;
                stats.M2    = 0.0;
                stats.count = 1;
                stats.window.clear();
                stats.window.push_back(mw_m);

                const double fallback_thresh = large_mw_threshold_cycles_ * lambda_wl;
                if (mwwl_trace_.is_open()) {
                    writeTraceRecord(obs, s1, s2, mw_m,
                                     mw_m, 0.0,
                                     std::numeric_limits<double>::quiet_NaN(),
                                     stats.count,
                                     lambda_wl, fallback_thresh, false, false);
                }
                continue;
            }

            // Deviation of current MW from running mean
            const double dev = mw_m - stats.mean;

            // Compute adaptive threshold from MAD of the rolling window
            const double thresh_m = computeAdaptiveThreshold(stats, lambda_wl);

            // Compute MAD-derived sigma for logging/tracing
            double sigma_mad = std::numeric_limits<double>::quiet_NaN();
            if (stats.window.size() >= kMinEpochsForAdaptive) {
                std::vector<double> sorted(stats.window.begin(), stats.window.end());
                std::sort(sorted.begin(), sorted.end());
                const size_t wn = sorted.size();
                const double median = (wn % 2 == 0) ? 0.5 * (sorted[wn / 2 - 1] + sorted[wn / 2])
                                                    : sorted[wn / 2];
                std::vector<double> abs_devs(wn);
                for (size_t j = 0; j < wn; ++j) abs_devs[j] = std::abs(sorted[j] - median);
                std::sort(abs_devs.begin(), abs_devs.end());
                const double mad = (wn % 2 == 0) ? 0.5 * (abs_devs[wn / 2 - 1] + abs_devs[wn / 2])
                                                 : abs_devs[wn / 2];
                sigma_mad = mad * 1.4826;
            }

            const bool is_slip = std::abs(dev) > thresh_m;

            // Write trace record for every observation
            if (mwwl_trace_.is_open()) {
                writeTraceRecord(obs, s1, s2, mw_m,
                                 stats.mean, dev, sigma_mad,
                                 stats.count,
                                 lambda_wl, thresh_m, is_slip, false);
            }

            if (is_slip) {
                const std::string amb_key = obs.satellite + "_" + phase_if_key;
                reset_keys.push_back(amb_key);

                const double dev_cycles = std::abs(dev) / lambda_wl;
                const double thresh_cycles = thresh_m / lambda_wl;

                logger->warn(
                    "MW slip detected for {} ({}): |dev|={:.3f} m ({:.1f} WL) > thresh={:.3f} m ({:.1f} WL), "
                    "mean={:.3f} m, sigma={:.4f} m, arc_n={}, lambda_wl={:.3f} m; reset {}",
                    obs.satellite, PppUtils::getSystemString(obs.system),
                    std::abs(dev), dev_cycles, thresh_m, thresh_cycles,
                    stats.mean, std::isfinite(sigma_mad) ? sigma_mad : -1.0,
                    stats.count, lambda_wl, amb_key);

                mw_stats_.erase(obs.satellite);
            } else {
                // Update running statistics (Hatch / Welford)
                stats.count++;
                const double delta  = mw_m - stats.mean;
                stats.mean += delta / static_cast<double>(stats.count);
                const double delta2 = mw_m - stats.mean;
                stats.M2 += delta * delta2;

                // Maintain rolling window
                stats.window.push_back(mw_m);
                if (stats.window.size() > kMwWindowSize) {
                    stats.window.pop_front();
                }
            }
        }

        return reset_keys;
    }

} // namespace GnssProcessor