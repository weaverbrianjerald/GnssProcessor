/**
 * @file GraphicSlipDetector.cpp
 * @brief GRAPHIC per-signal cycle slip detector implementation.
 *
 * Organization:
 *  - Construction / tracing
 *  - Signal helpers
 *  - GRAPHIC computation
 *  - Statistics / threshold
 *  - Core detection logic
 *  - External reset
 */

#include "GraphicSlipDetector.hpp"
#include "Constants.hpp"
#include "TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>

namespace GnssProcessor {

    // =================================================================
    // Construction
    // =================================================================

    GraphicSlipDetector::GraphicSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("cycle_slip_manager")->info(
            "GraphicSlipDetector initialized (MAD-based, k={:.1f}, floor={:.3f} m, min_epochs={})",
            kMadSigmaMultiplier, kMinThresholdM, kMinEpochsForAdaptive);
    }

    // =================================================================
    // Tracing
    // =================================================================

    bool GraphicSlipDetector::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("cycle_slip_manager");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "graphic_slip_trace.csv";
        trace_file_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!trace_file_.is_open()) {
            logger->warn("Could not open GRAPHIC trace file '{}'", trace_path.string());
            return false;
        }

        trace_file_ << "week,tow_s,satellite,system,elevation_deg,"
                    << "signal,graphic_m,dg_m,"
                    << "mad_sigma_m,threshold_m,arc_count,"
                    << "slip_cycles,fractional,is_slip\n";

        logger->info("GRAPHIC tracing enabled: {}", trace_path.string());
        return true;
    }

    // =================================================================
    // Signal helpers
    // =================================================================

    std::vector<GraphicSlipDetector::SignalPair>
    GraphicSlipDetector::collectSignalPairs(const PppObservation& obs) const {
        // Group code and phase signals by RINEX band number.
        struct BandSignals { std::string code; std::string phase; };
        std::map<char, BandSignals> bands;

        for (const auto& [key, val] : obs.raw_observed) {
            if (key.size() < 2) continue;
            const char prefix = key[0];
            if (prefix != 'C' && prefix != 'L') continue;
            // Skip combined (IF) labels containing '_'
            if (key.find('_') != std::string::npos) continue;
            const char band = key[1];
            if (band < '1' || band > '9') continue;

            auto& bs = bands[band];
            if (prefix == 'C' && bs.code.empty())  bs.code  = key;
            if (prefix == 'L' && bs.phase.empty()) bs.phase = key;
        }

        std::vector<SignalPair> pairs;
        for (const auto& [band, bs] : bands) {
            if (!bs.code.empty() && !bs.phase.empty())
                pairs.push_back({bs.phase, bs.code});
        }
        return pairs;
    }

    // =================================================================
    // GRAPHIC computation
    // =================================================================

    double GraphicSlipDetector::computeGraphic(const PppObservation& obs,
                                                const std::string& phase_sig,
                                                const std::string& code_sig) const {
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        auto itL = obs.raw_observed.find(phase_sig);
        auto itP = obs.raw_observed.find(code_sig);
        if (itL == obs.raw_observed.end() || itP == obs.raw_observed.end())
            return NaN;

        const double L_cyc = itL->second;
        const double P_m   = itP->second;
        if (!std::isfinite(L_cyc) || !std::isfinite(P_m))
            return NaN;

        const double lambda = ppp_utils_.getWavelength(obs.system, phase_sig, obs.satellite);
        if (lambda <= 0.0)
            return NaN;

        const double L_m = L_cyc * lambda;

        // GRAPHIC = (P + lambda*L) / 2 = (P_m + L_m) / 2
        const double graphic = (P_m + L_m) / 2.0;
        return std::isfinite(graphic) ? graphic : NaN;
    }

    // =================================================================
    // Statistics / threshold
    // =================================================================

    double GraphicSlipDetector::computeMadSigma(const std::deque<double>& window) {
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

    double GraphicSlipDetector::computeAdaptiveThreshold(const GraphicArcStats& stats) const {
        const double sigma = computeMadSigma(stats.diff_window);
        if (!std::isfinite(sigma))
            return std::numeric_limits<double>::quiet_NaN();

        return std::max(kMadSigmaMultiplier * sigma, kMinThresholdM);
    }

    // =================================================================
    // Core detection logic
    // =================================================================

    std::vector<GraphicSlipResult> GraphicSlipDetector::detect(
        const std::vector<PppObservation>& observations) {

        std::vector<GraphicSlipResult> results;
        auto logger = log_manager_.getLogger("cycle_slip_manager");

        // Capture epoch time from first valid observation
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg())
                continue;

            const auto pairs = collectSignalPairs(obs);
            const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

            for (const auto& sp : pairs) {
                const double graphic = computeGraphic(obs, sp.phase, sp.code);
                if (!std::isfinite(graphic)) continue;

                const std::string stats_key = obs.satellite + "_" + sp.phase;
                auto& stats = graphic_stats_[stats_key];

                // Data gap detection
                if (stats.count > 0 && stats.last_epoch_s > 0.0) {
                    const double gap = epoch_s - stats.last_epoch_s;
                    if (gap > kMaxGapSeconds) {
                        logger->debug("GRAPHIC data gap {:.1f}s > {:.0f}s for {}: resetting arc",
                                      gap, kMaxGapSeconds, stats_key);
                        stats = GraphicArcStats{};
                    }
                }

                constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

                // First epoch: store and continue
                if (stats.count == 0) {
                    stats.prev_graphic = graphic;
                    stats.last_epoch_s = epoch_s;
                    stats.count        = 1;
                    stats.diff_window.clear();

                    if (trace_file_.is_open()) {
                        trace_file_ << std::fixed << std::setprecision(3)
                                    << current_epoch_.week << "," << current_epoch_.tow << ","
                                    << obs.satellite << ","
                                    << PppUtils::getSystemString(obs.system) << ","
                                    << std::setprecision(2) << obs.elevation << ","
                                    << sp.phase << ","
                                    << std::setprecision(6) << graphic << ","
                                    << NaN << ","
                                    << NaN << "," << NaN << ","
                                    << stats.count << ","
                                    << 0 << "," << NaN << "," << 0 << "\n";
                    }
                    continue;
                }

                // Epoch 1+: between-epoch difference
                const double dg = graphic - stats.prev_graphic;
                const double thresh = computeAdaptiveThreshold(stats);
                const double sigma  = computeMadSigma(stats.diff_window);
                const bool exceeds  = std::isfinite(thresh) && std::abs(dg) > thresh;

                // Estimate integer slip magnitude
                const double lambda = ppp_utils_.getWavelength(obs.system, sp.phase, obs.satellite);
                int    slip_cycles = 0;
                double fractional  = 0.0;
                bool   slip_known  = false;

                if (exceeds && lambda > 0.0) {
                    // dG_slip = dN * lambda / 2, so dN = 2 * dG / lambda
                    const double dn_float = 2.0 * dg / lambda;
                    slip_cycles = static_cast<int>(std::round(dn_float));
                    fractional  = std::abs(dn_float - slip_cycles);
                    slip_known  = (fractional < kMaxFractional) && (slip_cycles != 0);
                }

                const bool is_slip = exceeds;

                // Trace
                if (trace_file_.is_open()) {
                    trace_file_ << std::fixed << std::setprecision(3)
                                << current_epoch_.week << "," << current_epoch_.tow << ","
                                << obs.satellite << ","
                                << PppUtils::getSystemString(obs.system) << ","
                                << std::setprecision(2) << obs.elevation << ","
                                << sp.phase << ","
                                << std::setprecision(6) << graphic << ","
                                << dg << ","
                                << (std::isfinite(sigma) ? sigma : -1.0) << ","
                                << (std::isfinite(thresh) ? thresh : -1.0) << ","
                                << stats.count << ","
                                << slip_cycles << ","
                                << std::setprecision(3) << fractional << ","
                                << (is_slip ? 1 : 0) << "\n";
                }

                if (is_slip) {
                    logger->info(
                        "GRAPHIC slip detected for {} {}: |dG|={:.4f} m > thresh={:.4f} m, "
                        "dN_est={}, frac={:.3f}, sigma={:.4f} m, arc_n={}",
                        obs.satellite, sp.phase,
                        std::abs(dg), thresh,
                        slip_cycles, fractional,
                        std::isfinite(sigma) ? sigma : -1.0, stats.count);

                    results.push_back({obs.satellite, sp.phase,
                                       obs.satellite + "_" + sp.phase,
                                       slip_cycles, slip_known, fractional});

                    // Reset arc
                    graphic_stats_.erase(stats_key);
                    continue;
                }

                // Update running statistics
                stats.count++;
                stats.diff_window.push_back(dg);
                if (stats.diff_window.size() > kWindowSize)
                    stats.diff_window.pop_front();
                stats.prev_graphic = graphic;
                stats.last_epoch_s = epoch_s;
            }
        }

        return results;
    }

    // =================================================================
    // External reset
    // =================================================================

    void GraphicSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("cycle_slip_manager");

        const std::string prefix = satellite + "_";
        for (auto it = graphic_stats_.begin(); it != graphic_stats_.end(); )
            if (it->first.rfind(prefix, 0) == 0) {
                logger->debug("Cleared GRAPHIC arc for {} (external reset, arc_n={})",
                    it->first, it->second.count);
                it = graphic_stats_.erase(it);
            } else {
                ++it;
            }
    }

} // namespace GnssProcessor
