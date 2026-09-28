/**
 * @file IonoVariabilityMonitor.cpp
 * @brief Implementation of the per-satellite GF second-difference
 *        ionospheric variability monitor.
 */

#include "slip/IonoVariabilityMonitor.hpp"
#include "math/TimeUtils.hpp"
#include "math/RobustStats.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>

namespace GnssProcessor {

    // =================================================================
    // Construction
    // =================================================================

    IonoVariabilityMonitor::IonoVariabilityMonitor(const GnssConfig& config,
                                                   LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        const auto& cfg = config_.getAdaptiveIonoQConfig();
        log_manager_.getLogger("iono_variability")->info(
            "IonoVariabilityMonitor initialized (recent_window={}, baseline_window={}, "
            "max_multiplier={:.1f}, min_epochs={})",
            cfg.recent_window, cfg.baseline_window, cfg.max_multiplier, cfg.min_epochs);
    }

    // =================================================================
    // Tracing
    // =================================================================

    bool IonoVariabilityMonitor::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("iono_variability");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}",
                             log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "iono_variability_trace.csv";
        trace_file_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!trace_file_.is_open()) {
            logger->warn("Could not open iono variability trace file '{}'",
                         trace_path.string());
            return false;
        }

        trace_file_ << "week,tow_s,satellite,system,elevation_deg,sig1,sig2,"
                    << "gf_m,td2_gf_m,arc_count,window_n,"
                    << "sigma_recent_m,sigma_baseline_m,multiplier,q_zenith_eff\n";

        logger->info("Iono variability tracing enabled: {}", trace_path.string());
        return true;
    }

    // =================================================================
    // Signal / GF helpers  (adapted from the staged, uncompiled GfSlipDetector)
    // =================================================================

    bool IonoVariabilityMonitor::selectPrimaryPair(const PppObservation& obs,
                                                   std::string& sig1,
                                                   std::string& sig2) const {
        // Group phase signals by RINEX band digit; std::map keeps bands
        // ordered, so the first two entries are the lowest-band pair
        // (typically L1 + L2).  Within a band the first key in map order is used.
        std::map<char, std::string> phase_by_band;
        for (const auto& kv : obs.raw_observed) {
            const std::string& key = kv.first;
            if (key.size() < 2) continue;
            if (key[0] != 'L') continue;
            if (key.find('_') != std::string::npos) continue; // skip combined labels
            const char band = key[1];
            if (band < '1' || band > '9') continue;
            if (phase_by_band.find(band) == phase_by_band.end())
                phase_by_band[band] = key;
        }
        if (phase_by_band.size() < 2) return false;

        auto it = phase_by_band.begin();
        sig1 = it->second;
        ++it;
        sig2 = it->second;
        return true;
    }

    double IonoVariabilityMonitor::computeGfMeters(const PppObservation& obs,
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

        // Convert cycles to meters.
        const double lam1 = ppp_utils_.getWavelength(obs.system, sig1, obs.satellite);
        const double lam2 = ppp_utils_.getWavelength(obs.system, sig2, obs.satellite);
        if (lam1 <= 0.0 || lam2 <= 0.0)
            return NaN;

        // GF = L1 - L2 (contains ionosphere + ambiguity).
        const double gf = L1_cyc * lam1 - L2_cyc * lam2;
        return std::isfinite(gf) ? gf : NaN;
    }

    double IonoVariabilityMonitor::computeMadSigma(const std::vector<double>& values) {
        if (values.size() < 5)
            return std::numeric_limits<double>::quiet_NaN();
        return RobustStats::madSigma(values);
    }

    // =================================================================
    // Per-epoch update
    // =================================================================

    void IonoVariabilityMonitor::update(const std::vector<PppObservation>& observations) {
        auto logger = log_manager_.getLogger("iono_variability");
        const auto& cfg = config_.getAdaptiveIonoQConfig();
        const double q_baseline = config_.getProcessNoise().ionosphere;
        const double elev_mask  = config_.getElevationMaskDeg();
        constexpr double NaN    = std::numeric_limits<double>::quiet_NaN();

        // Capture epoch time from the first valid observation.
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        std::map<std::string, double> new_overrides;
        std::map<std::string, double> new_multipliers;

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < elev_mask)
                continue;

            std::string sig1, sig2;
            if (!selectPrimaryPair(obs, sig1, sig2)) continue;

            const double gf = computeGfMeters(obs, sig1, sig2);
            if (!std::isfinite(gf)) continue;

            const std::string key = obs.satellite + "_" + sig1 + "_" + sig2;
            GfArc& arc = arcs_[key];
            const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

            // Data-gap reset: a long outage breaks GF continuity.
            if (arc.count > 0 && arc.last_epoch_s > 0.0 &&
                (epoch_s - arc.last_epoch_s) > kMaxGapSeconds) {
                arc = GfArc{};
            }

            // Epochs 0 and 1: seed the recurrence, no TD2 available yet.
            if (arc.count < 2) {
                if (arc.count == 0) {
                    arc.prev_gf = gf;
                } else {
                    arc.prev_prev_gf = arc.prev_gf;
                    arc.prev_gf      = gf;
                }
                arc.last_epoch_s = epoch_s;
                ++arc.count;
                continue;
            }

            // Second-order time difference (TEC-acceleration proxy).
            const double td2 = gf - 2.0 * arc.prev_gf + arc.prev_prev_gf;

            arc.td2_window.push_back(td2);
            while (arc.td2_window.size() > static_cast<std::size_t>(cfg.baseline_window))
                arc.td2_window.pop_front();

            // Advance the GF recurrence.
            arc.prev_prev_gf = arc.prev_gf;
            arc.prev_gf      = gf;
            arc.last_epoch_s = epoch_s;
            ++arc.count;

            // Multiplier: squared ratio of recent vs baseline MAD-sigma of
            // the TD2 series, clamped to [1, max_multiplier].
            double multiplier     = 1.0;
            double sigma_recent   = NaN;
            double sigma_baseline = NaN;
            if (arc.td2_window.size() >= static_cast<std::size_t>(cfg.min_epochs)) {
                const std::vector<double> baseline(arc.td2_window.begin(),
                                                   arc.td2_window.end());
                const std::size_t rw = static_cast<std::size_t>(cfg.recent_window);
                std::vector<double> recent;
                if (arc.td2_window.size() > rw)
                    recent.assign(arc.td2_window.end() - rw, arc.td2_window.end());
                else
                    recent = baseline;

                sigma_recent   = computeMadSigma(recent);
                sigma_baseline = computeMadSigma(baseline);

                if (std::isfinite(sigma_recent) && std::isfinite(sigma_baseline)) {
                    const double denom = std::max(sigma_baseline, kBaselineSigmaFloorM);
                    const double ratio = sigma_recent / denom;
                    multiplier = std::min(std::max(ratio * ratio, 1.0),
                                          cfg.max_multiplier);
                }
            }

            const double q_eff = q_baseline * multiplier;
            new_overrides[obs.satellite] = q_eff;
            new_multipliers[obs.satellite] = multiplier;

            if (trace_file_.is_open()) {
                trace_file_ << std::fixed << std::setprecision(3)
                            << current_epoch_.week << "," << current_epoch_.tow << ","
                            << obs.satellite << ","
                            << PppUtils::getSystemString(obs.system) << ","
                            << std::setprecision(2) << obs.elevation << ","
                            << sig1 << "," << sig2 << ","
                            << std::setprecision(6) << gf << "," << td2 << ","
                            << arc.count << "," << arc.td2_window.size() << ","
                            << (std::isfinite(sigma_recent)   ? sigma_recent   : -1.0) << ","
                            << (std::isfinite(sigma_baseline) ? sigma_baseline : -1.0) << ","
                            << std::setprecision(3) << multiplier << ","
                            << std::scientific << std::setprecision(4) << q_eff << "\n";
            }
        }

        overrides_.swap(new_overrides);
        multipliers_.swap(new_multipliers);

        // Diagnostic: report the worst-case multiplier this epoch.
        if (!overrides_.empty() && q_baseline > 0.0) {
            double max_mult = 1.0;
            std::string worst;
            for (const auto& [sat, q] : overrides_) {
                const double m = q / q_baseline;
                if (m > max_mult) { max_mult = m; worst = sat; }
            }
            if (max_mult > 1.0) {
                logger->debug("Iono variability: {} sats monitored, peak multiplier "
                              "{:.1f}x ({})", overrides_.size(), max_mult, worst);
            }
        }
    }

    // =================================================================
    // External reset
    // =================================================================

    void IonoVariabilityMonitor::resetSatellite(const std::string& satellite) {
        const std::string prefix = satellite + "_";
        for (auto it = arcs_.begin(); it != arcs_.end(); ) {
            if (it->first.rfind(prefix, 0) == 0)
                it = arcs_.erase(it);
            else
                ++it;
        }
    }

} // namespace GnssProcessor
