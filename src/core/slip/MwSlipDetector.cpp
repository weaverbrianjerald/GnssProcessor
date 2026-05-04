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

#include "slip/MwSlipDetector.hpp"
#include "math/TimeUtils.hpp"
#include "common/Constants.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <set>

namespace GnssProcessor {

    // =================================================================
    // Construction
    // =================================================================

    MwSlipDetector::MwSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("mw_slip_detector")->info(
            "MwSlipDetector initialized (adaptive MAD-based, k={:.1f}, floor={:.1f} WL, "
            "initial={:.1f} WL, min_epochs={}, wl_lock_epochs={}, gap_tol={:.1f}s, mw_signals={})",
            kMadSigmaMultiplier, kMinThresholdCycles, kInitialThresholdCycles,
            kMinEpochsForAdaptive, kMinEpochsForWlLock, max_gap_seconds_,
            config_.getMwSignalMode() == MwSignalMode::All ? "all" : "filter");
    }

    void MwSlipDetector::setMaxGapSeconds(double seconds) {
        if (seconds > 0.0) {
            max_gap_seconds_ = seconds;
            log_manager_.getLogger("mw_slip_detector")->info(
                "MW gap tolerance set to {:.1f}s (was default {:.1f}s)",
                seconds, kDefaultMaxGapSeconds);
        }
    }

    // =================================================================
    // Per-satellite MW quality query
    // =================================================================

    SatelliteMwQuality MwSlipDetector::getSatelliteMwQuality(const std::string& satellite) const {
        SatelliteMwQuality q;
        const std::string prefix = satellite + "_";
        bool any_bia = false;
        bool all_bia_locked = true;
        bool sigma_initialized = false;

        for (const auto& [key, stats] : mw_stats_) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;

            ++q.num_pairs;
            if (stats.count < q.min_arc_length || q.num_pairs == 1)
                q.min_arc_length = stats.count;

            const double sigma = computeMadSigma(stats);
            if (std::isfinite(sigma)) {
                if (!sigma_initialized || sigma > q.worst_sigma) {
                    q.worst_sigma = sigma;
                    sigma_initialized = true;
                }
            }

            if (stats.bia_corrected) {
                any_bia = true;
                if (!stats.wl_locked) all_bia_locked = false;
            }
        }

        q.all_wl_locked = any_bia && all_bia_locked && q.num_pairs > 0;
        if (!sigma_initialized) q.worst_sigma = 1e9;
        return q;
    }

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
                    << "is_slip,is_lli,"
                    << "bia_corrected,sat_mw_bias_m,rounded_wl,wl_locked\n";

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
                                           bool is_lli,
                                           bool bia_corrected,
                                           double sat_mw_bias_m,
                                           int rounded_wl,
                                           bool wl_locked) const {
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
                    << (is_slip ? 1 : 0) << "," << (is_lli ? 1 : 0) << ","
                    << (bia_corrected ? 1 : 0) << ","
                    << std::setprecision(6) << sat_mw_bias_m << ","
                    << rounded_wl << "," << (wl_locked ? 1 : 0) << "\n";
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

    std::vector<MwSlipDetector::IfPair>
    MwSlipDetector::collectIonoFreePairs(const PppObservation& obs) const {
        // Build maps: band-pair key -> (phase label, code label).
        // An IF label like "L1W_L5Q" has band-pair key "1_5".
        std::map<std::string, std::string> phase_by_bands;
        std::map<std::string, std::string> code_by_bands;
        for (const auto& [k, v] : obs.observed) {
            if (k.size() < 2 || k.find('_') == std::string::npos) continue;
            const size_t us = k.find('_');
            if (us < 2 || us + 2 >= k.size()) continue;
            // Extract band digits from each half: e.g. "L1W" -> '1', "L5Q" -> '5'.
            const char b1 = k[1];
            const char b2 = k[us + 2];
            if (b1 < '1' || b1 > '9' || b2 < '1' || b2 > '9') continue;
            const std::string bkey = std::string(1, b1) + "_" + std::string(1, b2);
            if (k[0] == 'L') phase_by_bands[bkey] = k;
            else if (k[0] == 'C') code_by_bands[bkey] = k;
        }
        std::vector<IfPair> result;
        for (const auto& [bkey, pkey] : phase_by_bands) {
            auto it = code_by_bands.find(bkey);
            if (it != code_by_bands.end())
                result.push_back({pkey, it->second});
        }
        return result;
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

        // Choose signal source based on config:
        //  - Filter (default): obs.observed — only bands the KF processes,
        //    keeping the MW detector aligned with signal_mode and avoiding
        //    noisy EWL pairs the filter doesn't use.
        //  - All: obs.raw_observed — every available RINEX band for maximum
        //    multi-frequency coverage.
        const auto& source = (config_.getMwSignalMode() == MwSignalMode::All)
                                 ? obs.raw_observed
                                 : obs.observed;

        for (const auto& [key, val] : source) {
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

    bool MwSlipDetector::computeSatMwBias(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2,
        const std::string& code1, const std::string& code2,
        double& bias_m) const {

        bias_m = 0.0;
        if (!corrections_) return false;

        // Check all four BIA entries exist
        if (!corrections_->hasBiasEntry(obs.satellite, sig1) ||
            !corrections_->hasBiasEntry(obs.satellite, sig2) ||
            !corrections_->hasBiasEntry(obs.satellite, code1) ||
            !corrections_->hasBiasEntry(obs.satellite, code2))
            return false;

        const double b_L1 = corrections_->getBiasCorrection(obs.satellite, sig1, obs.time);
        const double b_L2 = corrections_->getBiasCorrection(obs.satellite, sig2, obs.time);
        const double b_C1 = corrections_->getBiasCorrection(obs.satellite, code1, obs.time);
        const double b_C2 = corrections_->getBiasCorrection(obs.satellite, code2, obs.time);

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);
        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) return false;

        const double f_diff = f1 - f2;
        const double f_sum  = f1 + f2;
        if (std::abs(f_diff) < 1.0 || std::abs(f_sum) < 1.0) return false;

        // b_MW = (f1*b_L1 - f2*b_L2)/(f1-f2) - (f1*b_C1 + f2*b_C2)/(f1+f2)
        const double wl_phase_bias = (f1 * b_L1 - f2 * b_L2) / f_diff;
        const double nl_code_bias  = (f1 * b_C1 + f2 * b_C2) / f_sum;
        bias_m = wl_phase_bias - nl_code_bias;

        return std::isfinite(bias_m);
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
            return kInitialThresholdCycles * lambda_wl;

        const double min_thresh = kMinThresholdCycles * lambda_wl;
        return std::max(kMadSigmaMultiplier * sigma, min_thresh);
    }

    bool MwSlipDetector::processMwSample(const PppObservation& obs,
        const std::string& stats_key,
        double mw_m, double lambda_wl,
        const std::string& sig1, const std::string& sig2,
        bool bia_corrected, double sat_mw_bias,
        int* out_slip_cycles,
        bool* out_wl_known,
        double* out_slip_frac) {

        if (out_slip_cycles) *out_slip_cycles = 0;
        if (out_wl_known)    *out_wl_known = false;
        if (out_slip_frac)   *out_slip_frac = 0.5;

        auto& stats = mw_stats_[stats_key];
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
        const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

        // Detect data gaps: if the satellite was absent for longer than
        // the configured threshold, discard the stale arc and start fresh.
        // The threshold is overridable via setMaxGapSeconds — Application
        // tightens it to the RINEX sampling interval when IMU isn't
        // available and the data rate is coarser than 5 s.
        if (stats.count > 0 && stats.last_epoch_s > 0.0) {
            const double gap = epoch_s - stats.last_epoch_s;
            if (gap > max_gap_seconds_) {
                log_manager_.getLogger("mw_slip_detector")->debug(
                    "Data gap {:.1f}s > {:.1f}s for {}: resetting MW arc (n={})",
                    gap, max_gap_seconds_, stats_key, stats.count);
                stats = MwArcStats{};          // reset to default-initialized
            }
        }

        // First epoch: initialize arc.
        if (stats.count == 0) {
            stats.mean         = mw_m;
            stats.M2           = 0.0;
            stats.count        = 1;
            stats.prev_mw      = mw_m;
            stats.last_epoch_s = epoch_s;
            stats.diff_window.clear();
            stats.bia_corrected = bia_corrected;
            stats.lambda_wl     = lambda_wl;
            stats.wl_locked     = false;
            stats.rounded_wl    = 0;

            // WL integer locking is deferred until kMinEpochsForWlLock
            // consecutive epochs have been accumulated.  A single MW
            // sample is too noisy (especially for EWL pairs) for a
            // reliable rounding decision.

            // Sanity check: a BIA-corrected MW value should land near
            // integer × λ_WL on a clean phase track.  When the very first
            // sample of a fresh arc has a large fractional offset, the
            // arc was almost certainly initialized with a slip already
            // baked in (e.g., a slip occurred during the data gap that
            // triggered the reset, OR this is the first observation of
            // the satellite after a long absence and the tracking loop
            // is still settling).  Declare a slip on init so the
            // ambiguity gets reset rather than letting the corrupt
            // value become the new running mean — silent baseline
            // corruption is precisely the failure mode that lets a
            // 30-cycle slip slip through to the post-fit residuals.
            if (bia_corrected && lambda_wl > 0.0) {
                const double init_cyc = mw_m / lambda_wl;
                const double frac = std::abs(init_cyc - std::round(init_cyc));
                if (frac > kInitFracThresh) {
                    log_manager_.getLogger("mw_slip_detector")->warn(
                        "MW init suspicious for {}: BIA-corrected MW={:.4f} m → "
                        "{:.3f} WL cyc has fractional |frac|={:.3f} > {:.2f}. "
                        "Forcing ambiguity reset (unknown integer slip during arc reset).",
                        stats_key, mw_m, init_cyc, frac, kInitFracThresh);

                    if (mwwl_trace_.is_open())
                        writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                                         0.0, NaN, stats.count, lambda_wl, NaN,
                                         /*is_slip=*/true, /*excessive=*/false,
                                         bia_corrected, sat_mw_bias,
                                         stats.rounded_wl, stats.wl_locked);

                    if (out_slip_cycles) *out_slip_cycles = 0;     // unknown integer
                    if (out_wl_known)    *out_wl_known    = false;
                    if (out_slip_frac)   *out_slip_frac   = frac;
                    return true;
                }
            }

            if (mwwl_trace_.is_open())
                writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean, 0.0, NaN,
                                 stats.count, lambda_wl, NaN, false, false,
                                 bia_corrected, sat_mw_bias,
                                 stats.rounded_wl, stats.wl_locked);
            return false;
        }

        const double mw_diff = mw_m - stats.prev_mw;

        // For BIA-corrected arcs not yet locked: attempt rounding once
        // the Hatch mean has accumulated enough epochs.  Waiting for
        // kMinEpochsForWlLock samples lets the mean converge to within a
        // fraction of a WL cycle before the irreversible decision.
        if (stats.bia_corrected && !stats.wl_locked && lambda_wl > 0.0
            && stats.count >= kMinEpochsForWlLock) {
            const double mean_cyc = stats.mean / lambda_wl;
            const double frac = mean_cyc - std::round(mean_cyc);
            if (std::abs(frac) < 0.25) {
                stats.rounded_wl = static_cast<int>(std::round(mean_cyc));
                stats.mean = stats.rounded_wl * lambda_wl;
                stats.wl_locked = true;
                log_manager_.getLogger("mw_slip_detector")->info(
                    "WL locked for {} (epoch {}): N_WL={}, mean_cyc={:.3f}, frac={:.3f}",
                    stats_key, stats.count, stats.rounded_wl, mean_cyc, frac);
            }
        }

        const double dev     = mw_m - stats.mean;
        const double thresh  = computeAdaptiveThreshold(stats, lambda_wl);
        const double sigma   = computeMadSigma(stats);
        const bool is_slip   = std::isfinite(thresh) && std::abs(dev) > thresh;

        // Cache for external queries (e.g. outlier tiebreaker).
        stats.last_dev   = dev;
        stats.last_sigma = sigma;

        if (mwwl_trace_.is_open())
            writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean, dev, sigma,
                             stats.count, lambda_wl, thresh, is_slip, false,
                             stats.bia_corrected, sat_mw_bias,
                             stats.rounded_wl, stats.wl_locked);

        if (is_slip) {
            auto logger = log_manager_.getLogger("mw_slip_detector");

            // Compute WL slip magnitude.
            // The WL combination N_WL = N_1 - N_2 cannot by itself distinguish
            // which signal slipped.  Multi-frequency cross-consistency in the
            // CycleSlipManager identifies the culprit signal.
            int slip_cycles = 0;
            bool wl_known = false;
            if (stats.wl_locked && lambda_wl > 0.0) {
                const double new_cyc = mw_m / lambda_wl;
                const double new_frac = new_cyc - std::round(new_cyc);
                if (std::abs(new_frac) < 0.25) {
                    const int new_wl = static_cast<int>(std::round(new_cyc));
                    slip_cycles = new_wl - stats.rounded_wl;
                    wl_known = (slip_cycles != 0);
                    if (out_slip_frac) *out_slip_frac = std::abs(new_frac);
                }
            }

            // Fallback for non-locked arcs (e.g. BSSD pivot with BIA):
            // The epoch-to-epoch MW jump cancels the receiver bias, so
            // dev / lambda_wl should be near-integer for a true cycle slip.
            if (!wl_known && !stats.wl_locked && lambda_wl > 0.0) {
                const double dev_cyc = dev / lambda_wl;
                const double dev_frac = dev_cyc - std::round(dev_cyc);
                if (std::abs(dev_frac) < 0.25 && std::abs(dev_cyc) >= 0.5) {
                    slip_cycles = static_cast<int>(std::round(dev_cyc));
                    wl_known = true;
                    if (out_slip_frac) *out_slip_frac = std::abs(dev_frac);
                    log_manager_.getLogger("mw_slip_detector")->info(
                        "WL slip estimated from jump for {}: dN_WL={}, dev_cyc={:.3f}, frac={:.3f}",
                        stats_key, slip_cycles, dev_cyc, dev_frac);
                }
            }
            if (out_slip_cycles) *out_slip_cycles = slip_cycles;
            if (out_wl_known)    *out_wl_known = wl_known;

            logger->warn(
                "MW slip detected for {} ({}): |dev|={:.3f} m ({:.1f} WL) > thresh={:.3f} m ({:.1f} WL), "
                "mean={:.3f} m, sigma={:.4f} m, arc_n={}, lambda_wl={:.3f} m, bia={}, locked={}, delta_wl={}",
                obs.satellite, PppUtils::getSystemString(obs.system),
                std::abs(dev), std::abs(dev) / lambda_wl,
                thresh, thresh / lambda_wl,
                stats.mean, std::isfinite(sigma) ? sigma : -1.0,
                stats.count, lambda_wl,
                stats.bia_corrected ? "yes" : "no",
                stats.wl_locked ? "yes" : "no",
                slip_cycles != 0 ? std::to_string(slip_cycles) + " WL" : "n/a");

            // For bias-corrected locked arcs with a known integer slip,
            // shift the arc by the integer offset to maintain continuity
            // rather than discarding the accumulated statistics.
            if (stats.wl_locked && wl_known && slip_cycles != 0) {
                stats.rounded_wl += slip_cycles;
                stats.mean = stats.rounded_wl * lambda_wl;
                stats.prev_mw = mw_m;
                stats.last_epoch_s = epoch_s;
                // Clear the diff window so the MAD is not contaminated by
                // the pre-slip differences.
                stats.diff_window.clear();
                logger->info("MW arc shifted for {} (bia-locked): new N_WL={}, mean={:.3f} m",
                             stats_key, stats.rounded_wl, stats.mean);
            } else {
                mw_stats_.erase(stats_key);
            }
            return true;
        }

        // Update running statistics (Welford / Hatch)
        // When the WL is locked, skip mean update (it's deterministic).
        stats.count++;
        if (!stats.wl_locked) {
            const double eff_n  = static_cast<double>(std::min(stats.count, kMaxHatchCount));
            const double delta  = mw_m - stats.mean;
            stats.mean += delta / eff_n;
            const double delta2 = mw_m - stats.mean;
            stats.M2 += delta * delta2;
        }

        stats.diff_window.push_back(mw_diff);
        if (stats.diff_window.size() > kMwWindowSize)
            stats.diff_window.pop_front();
        stats.prev_mw      = mw_m;
        stats.last_epoch_s = epoch_s;

        return false;
    }

    // =================================================================
    // Core detection logic
    // =================================================================

    std::vector<MwSlipResult> MwSlipDetector::detectLargeMwResets(
        const std::vector<PppObservation>& observations) {

        std::vector<MwSlipResult> results;
        results.reserve(observations.size());
        auto logger = log_manager_.getLogger("mw_slip_detector");

        // Helper: push a reset result (no known WL integer)
        auto pushReset = [&](const std::string& key, double lwl = 0.0,
                             const std::string& s1 = {}, const std::string& s2 = {},
                             const std::string& sat = {}) {
            results.push_back({ key, 0, false, lwl, s1, s2, sat });
        };

        // Capture epoch time from first valid observation for tracing
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        // -----------------------------------------------------------------
        // Intermediate storage for the BSSD two-pass MW approach.
        // Used for BOTH combined (PPP) and uncombined (PPP-RTK) paths
        // whenever BIA satellite biases are available.  The internal
        // between-satellite differencing eliminates the receiver WL bias
        // and enables integer WL locking, regardless of the filter's
        // measurement model parameterization.
        //
        // Keyed by (constellation_char + "_" + phase1 + "_" + phase2).
        // -----------------------------------------------------------------
        struct SatMwEntry {
            const PppObservation* obs;
            UncombinedPair pair;
            double mw_raw;          // raw MW in meters
            double mw_bia;          // MW after subtracting satellite BIA bias (meters)
            double sat_mw_bias;     // satellite BIA MW bias that was subtracted (meters)
            bool   has_bia;         // true if satellite BIA was available
            double lambda_wl;       // WL wavelength (meters)
            bool   is_combined;     // true if this came from the combined (PPP) path
            std::string if_phase_key; // IF phase label for combined reset keys (e.g. "L1C_L2W")
        };
        // group_key -> satellite -> entry
        std::map<std::string, std::map<std::string, SatMwEntry>> sd_groups;

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;

            const bool has_lli = hasAnyPhaseLockLoss(obs);

            // Try combined (PPP) path first
            const auto if_pairs = collectIonoFreePairs(obs);
            const bool has_if = !if_pairs.empty();

            if (has_if) {
                // ---- Combined (PPP) path ----
                if (has_lli) {
                    for (const auto& ifp : if_pairs) {
                        const std::string amb_key = obs.satellite + "_" + ifp.phase_key;
                        pushReset(amb_key);
                        logger->warn("LLI lock loss detected: scheduling ambiguity reset for {}", amb_key);
                        std::string s1, s2;
                        if (mwwl_trace_.is_open() && splitIfLabel(ifp.phase_key, s1, s2)) {
                            std::string c1, c2;
                            splitIfLabel(ifp.code_key, c1, c2);
                            constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
                            writeTraceRecord(obs, s1, s2, computeMwMeters(obs, s1, s2, c1, c2),
                                             NaN, NaN, NaN, 0,
                                             computeWideLaneWavelengthMeters(obs, s1, s2),
                                             NaN, false, true);
                        }
                    }
                    for (auto it = mw_stats_.begin(); it != mw_stats_.end(); )
                        it = (it->first.rfind(obs.satellite, 0) == 0) ? mw_stats_.erase(it) : std::next(it);
                    continue;
                }

                // Track which band-pairs already have IF labels (e.g. "1_2" from "L1W_L2W")
                // so we can add supplementary uncombined MW pairs for bands not covered
                // by the combined signal_mode (e.g. L1-L5, L2-L5 when signal_mode=dual).
                std::set<std::string> if_band_pairs;

                for (const auto& ifp : if_pairs) {
                    std::string s1, s2, c1, c2;
                    if (!splitIfLabel(ifp.phase_key, s1, s2)) continue;
                    if (!splitIfLabel(ifp.code_key, c1, c2)) continue;

                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, s1, s2);
                    if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) continue;
                    const double mw_raw = computeMwMeters(obs, s1, s2, c1, c2);
                    if (!std::isfinite(mw_raw)) continue;

                    // Record the band pair as covered only after confirming
                    // a valid MW could be formed.  Otherwise, a failed IF
                    // pair would block the supplementary uncombined path
                    // from producing the MW for that band combination.
                    if (s1.size() >= 2 && s2.size() >= 2)
                        if_band_pairs.insert(std::string(1, s1[1]) + "_" + std::string(1, s2[1]));

                    // Compute satellite BIA MW bias
                    double sat_bias = 0.0;
                    const bool has_bia = computeSatMwBias(obs, s1, s2, c1, c2, sat_bias);
                    const double mw_bia = has_bia ? (mw_raw - sat_bias) : mw_raw;

                    if (has_bia) {
                        // Collect for two-pass BSSD integer processing
                        const char cons = obs.satellite.empty() ? '?' : obs.satellite[0];
                        const std::string group_key = std::string(1, cons) + "_" + s1 + "_" + s2;
                        sd_groups[group_key][obs.satellite] = {
                            &obs, { s1, s2, c1, c2 }, mw_raw, mw_bia,
                            sat_bias, has_bia, lambda_wl,
                            /*is_combined=*/true, ifp.phase_key
                        };
                    } else {
                        // No BIA: undifferenced processing only, no integer locking
                        const std::string stats_key = obs.satellite + "_" + ifp.phase_key;
                        int slip_cyc = 0;
                        bool wl_known = false;
                        double slip_frac = 0.5;
                        if (processMwSample(obs, stats_key, mw_raw, lambda_wl, s1, s2,
                                            /*bia_corrected=*/false, /*sat_mw_bias=*/0.0,
                                            &slip_cyc, &wl_known, &slip_frac)) {
                            results.push_back({ obs.satellite + "_" + ifp.phase_key,
                                                slip_cyc, wl_known, lambda_wl,
                                                s1, s2, obs.satellite, slip_frac });
                            logger->warn("Scheduling reset for {} (wl_known={}, slip_wl={})",
                                         obs.satellite + "_" + ifp.phase_key, wl_known, slip_cyc);
                        }
                    }
                }

                // Supplementary uncombined MW pairs from raw_observed for band
                // pairs not already covered by the combined IF labels.  This
                // provides extra multi-frequency constraints for cycle slip
                // detection (e.g. L1-L5, L2-L5) even when signal_mode limits
                // the KF to dual-frequency IF combinations.
                const auto uc_pairs = collectUncombinedPairs(obs);
                for (const auto& pair : uc_pairs) {
                    // Skip pairs already handled via the combined path
                    if (pair.phase1.size() >= 2 && pair.phase2.size() >= 2) {
                        const std::string bkey = std::string(1, pair.phase1[1]) + "_"
                                               + std::string(1, pair.phase2[1]);
                        if (if_band_pairs.count(bkey)) continue;
                    }

                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, pair.phase1, pair.phase2);
                    if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) continue;
                    const double mw_raw = computeMwMeters(obs, pair.phase1, pair.phase2, pair.code1, pair.code2);
                    if (!std::isfinite(mw_raw)) continue;

                    double sat_bias = 0.0;
                    const bool has_bia = computeSatMwBias(obs, pair.phase1, pair.phase2,
                                                          pair.code1, pair.code2, sat_bias);
                    const double mw_bia = has_bia ? (mw_raw - sat_bias) : mw_raw;

                    if (has_bia) {
                        const char cons = obs.satellite.empty() ? '?' : obs.satellite[0];
                        const std::string group_key = std::string(1, cons) + "_" + pair.phase1 + "_" + pair.phase2;
                        sd_groups[group_key][obs.satellite] = {
                            &obs, pair, mw_raw, mw_bia,
                            sat_bias, has_bia, lambda_wl,
                            /*is_combined=*/false, {}
                        };
                    } else {
                        const std::string stats_key = obs.satellite + "_" + pair.phase1 + "_" + pair.phase2;
                        int slip_cyc = 0;
                        bool wl_known = false;
                        double slip_frac = 0.5;
                        if (processMwSample(obs, stats_key, mw_raw, lambda_wl, pair.phase1, pair.phase2,
                                            /*bia_corrected=*/false, /*sat_mw_bias=*/0.0,
                                            &slip_cyc, &wl_known, &slip_frac)) {
                            // Supplementary pairs produce per-signal reset entries
                            // with full signal info for the multi-frequency solver.
                            results.push_back({ obs.satellite + "_" + pair.phase1,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac });
                            results.push_back({ obs.satellite + "_" + pair.phase2,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac });
                            logger->warn("Scheduling supplementary MW reset for {}_{}, {}_{} (wl_known={}, slip_wl={})",
                                         obs.satellite, pair.phase1, obs.satellite, pair.phase2,
                                         wl_known, slip_cyc);
                        }
                    }
                }
            } else {
                // ---- Uncombined (PPP-RTK) path ----
                const auto uc_pairs = collectUncombinedPairs(obs);
                if (uc_pairs.empty()) continue;

                if (has_lli) {
                    for (const auto& [key, val] : obs.raw_observed)
                        if (!key.empty() && key[0] == 'L' && key.find('_') == std::string::npos)
                            pushReset(obs.satellite + "_" + key);
                    logger->warn("LLI lock loss (uncombined): scheduling all phase ambiguity resets for {}", obs.satellite);
                    for (auto it = mw_stats_.begin(); it != mw_stats_.end(); )
                        it = (it->first.rfind(obs.satellite, 0) == 0) ? mw_stats_.erase(it) : std::next(it);
                    continue;
                }

                for (const auto& pair : uc_pairs) {
                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, pair.phase1, pair.phase2);
                    if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) continue;
                    const double mw_raw = computeMwMeters(obs, pair.phase1, pair.phase2, pair.code1, pair.code2);
                    if (!std::isfinite(mw_raw)) continue;

                    // Compute satellite BIA MW bias
                    double sat_bias = 0.0;
                    const bool has_bia = computeSatMwBias(obs, pair.phase1, pair.phase2,
                                                          pair.code1, pair.code2, sat_bias);
                    const double mw_bia = has_bia ? (mw_raw - sat_bias) : mw_raw;

                    if (has_bia) {
                        // Collect for two-pass BSSD integer processing
                        const char cons = obs.satellite.empty() ? '?' : obs.satellite[0];
                        const std::string group_key = std::string(1, cons) + "_" + pair.phase1 + "_" + pair.phase2;
                        sd_groups[group_key][obs.satellite] = {
                            &obs, pair, mw_raw, mw_bia,
                            sat_bias, has_bia, lambda_wl,
                            /*is_combined=*/false, {}
                        };
                    } else {
                        // No BIA: undifferenced processing, no integer locking
                        const std::string stats_key = obs.satellite + "_" + pair.phase1 + "_" + pair.phase2;
                        int slip_cyc = 0;
                        bool wl_known = false;
                        double slip_frac = 0.5;
                        if (processMwSample(obs, stats_key, mw_raw, lambda_wl, pair.phase1, pair.phase2,
                                            /*bia_corrected=*/false, /*sat_mw_bias=*/0.0,
                                            &slip_cyc, &wl_known, &slip_frac)) {
                            results.push_back({ obs.satellite + "_" + pair.phase1,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac });
                            results.push_back({ obs.satellite + "_" + pair.phase2,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac });
                            logger->warn("Scheduling reset for {}_{}, {}_{} (wl_known={}, slip_wl={})",
                                         obs.satellite, pair.phase1, obs.satellite, pair.phase2,
                                         wl_known, slip_cyc);
                        }
                    }
                }
            }
        }

        // -----------------------------------------------------------------
        // Pass 2: BSSD single-difference MW processing
        //
        // For each (constellation, band-pair) group that has BIA-corrected
        // MW values, select a pivot (highest elevation) and form SD values.
        // The SD eliminates the receiver WL bias, making the result near-
        // integer in WL cycles when combined with the satellite BIA correction.
        //
        // This runs for BOTH combined (PPP) and uncombined (PPP-RTK) paths.
        // The MW observable is always formed from raw RINEX signals and is
        // independent of the filter's measurement model parameterization.
        // -----------------------------------------------------------------
        for (auto& [group_key, sat_entries] : sd_groups) {
            if (sat_entries.empty()) continue;

            // Select pivot: highest elevation satellite in this group
            std::string best_pivot;
            double best_elev = -999.0;
            for (const auto& [sat, entry] : sat_entries) {
                if (entry.obs->elevation > best_elev) {
                    best_elev = entry.obs->elevation;
                    best_pivot = sat;
                }
            }
            if (best_pivot.empty()) continue;

            // Detect pivot change and reset SD arcs if needed
            auto prev_pivot_it = sd_pivot_sat_.find(group_key);
            if (prev_pivot_it != sd_pivot_sat_.end() && prev_pivot_it->second != best_pivot) {
                logger->debug("BSSD MW pivot change for {}: {} -> {} (resetting SD arcs)",
                              group_key, prev_pivot_it->second, best_pivot);
                for (const auto& [sat, entry] : sat_entries) {
                    if (sat == best_pivot) continue;
                    const std::string stats_key = sat + "_" + entry.pair.phase1 + "_" + entry.pair.phase2;
                    auto arc_it = mw_stats_.find(stats_key);
                    if (arc_it != mw_stats_.end()) {
                        if (arc_it->second.bia_corrected) {
                            mw_stats_.erase(arc_it);
                        }
                    }
                }
            }
            sd_pivot_sat_[group_key] = best_pivot;

            const auto& pivot_entry = sat_entries.at(best_pivot);

            for (const auto& [sat, entry] : sat_entries) {
                const std::string stats_key = sat + "_" + entry.pair.phase1 + "_" + entry.pair.phase2;

                if (sat == best_pivot) {
                    // Pivot: process undifferenced BIA-corrected MW.
                    // Cannot integer-lock (receiver bias still present)
                    // but BIA subtraction improves the Hatch mean stability.
                    // The fallback jump-based WL estimation in processMwSample
                    // can still resolve the integer slip from the deviation.
                    int pivot_slip_cyc = 0;
                    bool pivot_wl_known = false;
                    double pivot_slip_frac = 0.5;
                    if (processMwSample(*entry.obs, stats_key, entry.mw_bia, entry.lambda_wl,
                                        entry.pair.phase1, entry.pair.phase2,
                                        /*bia_corrected=*/false, entry.sat_mw_bias,
                                        &pivot_slip_cyc, &pivot_wl_known, &pivot_slip_frac)) {
                        if (entry.is_combined) {
                            results.push_back({ sat + "_" + entry.if_phase_key,
                                                pivot_slip_cyc, pivot_wl_known, entry.lambda_wl,
                                                entry.pair.phase1, entry.pair.phase2, sat, pivot_slip_frac });
                        } else {
                            results.push_back({ sat + "_" + entry.pair.phase1,
                                                pivot_slip_cyc, pivot_wl_known, entry.lambda_wl,
                                                entry.pair.phase1, entry.pair.phase2, sat, pivot_slip_frac });
                            results.push_back({ sat + "_" + entry.pair.phase2,
                                                pivot_slip_cyc, pivot_wl_known, entry.lambda_wl,
                                                entry.pair.phase1, entry.pair.phase2, sat, pivot_slip_frac });
                        }
                        logger->warn("Scheduling reset for {} (pivot, wl_known={}, slip_wl={})",
                                     entry.is_combined ? (sat + "_" + entry.if_phase_key)
                                                       : (sat + "_" + entry.pair.phase1 + "+" + entry.pair.phase2),
                                     pivot_wl_known, pivot_slip_cyc);
                    }
                } else {
                    // Non-pivot: form single-difference MW.
                    // SD = MW_sat(BIA-corrected) - MW_pivot(BIA-corrected)
                    // This removes the receiver WL bias, leaving:
                    //   SD_MW = (N_WL_sat - N_WL_pivot) * lambda_wl + noise
                    const double sd_mw = entry.mw_bia - pivot_entry.mw_bia;

                    int slip_cyc = 0;
                    bool wl_known = false;
                    double slip_frac = 0.5;
                    if (processMwSample(*entry.obs, stats_key, sd_mw, entry.lambda_wl,
                                        entry.pair.phase1, entry.pair.phase2,
                                        /*bia_corrected=*/true, entry.sat_mw_bias,
                                        &slip_cyc, &wl_known, &slip_frac)) {
                        if (entry.is_combined) {
                            results.push_back({ sat + "_" + entry.if_phase_key,
                                                slip_cyc, wl_known, entry.lambda_wl,
                                                entry.pair.phase1, entry.pair.phase2, sat, slip_frac });
                        } else {
                            results.push_back({ sat + "_" + entry.pair.phase1,
                                                slip_cyc, wl_known, entry.lambda_wl,
                                                entry.pair.phase1, entry.pair.phase2, sat, slip_frac });
                            results.push_back({ sat + "_" + entry.pair.phase2,
                                                slip_cyc, wl_known, entry.lambda_wl,
                                                entry.pair.phase1, entry.pair.phase2, sat, slip_frac });
                        }
                        logger->warn("Scheduling reset for {} (wl_known={}, slip_wl={})",
                                     entry.is_combined ? (sat + "_" + entry.if_phase_key)
                                                       : (sat + "_" + entry.pair.phase1 + "+" + entry.pair.phase2),
                                     wl_known, slip_cyc);
                    }
                }
            }
        }

        return results;
    }

    // =================================================================
    // Resolved WL accessor
    // =================================================================

    std::map<std::string, int> MwSlipDetector::getResolvedWlAmbiguities() const {
        std::map<std::string, int> resolved;
        for (const auto& [key, stats] : mw_stats_) {
            if (stats.wl_locked)
                resolved[key] = stats.rounded_wl;
        }
        return resolved;
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

        // If this satellite was an SD pivot, remove the pivot entry so that
        // a new pivot is selected next epoch and all dependent SD arcs reset.
        for (auto pit = sd_pivot_sat_.begin(); pit != sd_pivot_sat_.end(); )
            if (pit->second == satellite) {
                logger->debug("Cleared SD pivot {} for group {} (external satellite reset)",
                    satellite, pit->first);
                pit = sd_pivot_sat_.erase(pit);
            } else {
                ++pit;
            }
    }

    void MwSlipDetector::resetAll() {
        auto logger = log_manager_.getLogger("mw_slip_detector");
        const size_t n_arcs   = mw_stats_.size();
        const size_t n_pivots = sd_pivot_sat_.size();
        mw_stats_.clear();
        sd_pivot_sat_.clear();
        if (n_arcs > 0 || n_pivots > 0) {
            logger->info("MW detector resetAll: cleared {} arc(s), {} SD pivot(s)",
                         n_arcs, n_pivots);
        }
    }

} // namespace GnssProcessor