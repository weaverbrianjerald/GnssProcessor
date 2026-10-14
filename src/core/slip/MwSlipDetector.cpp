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
#include "math/RobustStats.hpp"
#include "common/Constants.hpp"
#include "common/SignalBand.hpp"
#include "model/PppModel.hpp"  // getAllowedBandDigits
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <set>

namespace GnssProcessor {

    namespace {
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
        inline double mwElevationWeight(double elev_deg) {
            constexpr double kMinElevDeg = 10.0;
            const double el = (std::isfinite(elev_deg) && elev_deg > kMinElevDeg)
                                  ? elev_deg : kMinElevDeg;
            return 1.0 / std::sin(el * Constants::PI / 180.0);
        }
    }  // namespace

    // =================================================================
    // Construction
    // =================================================================

    MwSlipDetector::MwSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager),
          init_frac_threshold_(config.getMwInitFracThreshold()),
          cross_sat_ewma_alpha_(config.getMwCrossSatEwmaAlpha()),
          sigma_multiplier_(config.getCycleSlipConfig().mw_sigma_multiplier),
          detect_reference_(config.getCycleSlipConfig().mw_detect_reference),
          td_order_(config.getCycleSlipConfig().mw_td_order),
          local_window_len_(std::clamp<size_t>(
              static_cast<size_t>(std::max(config.getCycleSlipConfig().mw_local_window, 1)),
              kMinEpochsForAdaptive, kMwLocalWindowCap)) {
        auto freqLabel = [](FrequencyMode m) {
            switch (m) {
            case FrequencyMode::Triple: return "triple";
            case FrequencyMode::All:    return "all";
            case FrequencyMode::Dual:
            default:                    return "dual";
            }
        };
        log_manager_.getLogger("mw_slip_detector")->info(
            "MwSlipDetector initialized (adaptive MAD-based, k={:.1f}, sigma_floor={:.2f} m, "
            "init_frac_thresh={:.2f} WL, min_epochs={}, "
            "wl_lock=adaptive(3σ), gap_tol={:.1f}s, mw_freq={}, mw_chan={}, inhouse_bias={}, "
            "xsat_ewma_α={:.2f}, detect_ref={}, td_order={}, local_window={})",
            sigma_multiplier_, kMinSigmaMeters,
            init_frac_threshold_,
            kMinEpochsForAdaptive, max_gap_seconds_,
            freqLabel(config_.getResolvedMwFrequencyMode()),
            config_.getResolvedMwChannelMode() == ChannelMode::All ? "all" : "single",
            config_.getInhouseBiasEstimation() ? "enabled" : "disabled",
            cross_sat_ewma_alpha_,
            detect_reference_ == MwDetectReference::LocalMedian    ? "local_median"
          : detect_reference_ == MwDetectReference::TimeDifference ? "time_difference"
                                                                   : "hatch_mean",
            td_order_ == MwTdOrder::Second ? "second" : "first",
            local_window_len_);
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
    // ISlipDetector contract
    // =================================================================

    std::vector<SlipEvent> MwSlipDetector::detect(const SlipEpochContext& context) {
        const std::vector<MwSlipResult> raw = detectLargeMwResets(context.observations);
        std::vector<SlipEvent> events;
        events.reserve(raw.size());
        for (const auto& r : raw) {
            SlipEvent ev;
            ev.satellite   = r.satellite;
            ev.sig1        = r.sig1;
            ev.sig2        = r.sig2;
            ev.amb_key     = r.amb_key;
            ev.slip_cycles = r.slip_wl_cycles;
            ev.slip_known  = r.slip_wl_known;
            ev.fractional  = r.slip_frac;
            ev.lambda      = r.lambda_wl;
            ev.granularity = r.sig2.empty() ? SlipGranularity::PerSignal
                                            : SlipGranularity::BandDifference;
            ev.source      = SlipDetectorKind::MelbourneWubbena;
            events.push_back(std::move(ev));
        }
        return events;
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

    MwSlipDetector::MwArcZdMean
    MwSlipDetector::arcZdMean(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2) const {
        MwArcZdMean out;
        // Lagged Welford variance of the mean = M2_lag / (n_lag(n_lag-1)); NaN below
        // two samples.  All reads below use the lagged snapshot (stats as of the end
        // of the previous epoch): per SPEC_mw_wl_card §3.3, the layer's same-epoch
        // read must be independent of the current epoch's observations.
        auto varOfMean = [](const MwArcStats& a) -> double {
            if (a.count_lag < 2) return std::numeric_limits<double>::quiet_NaN();
            const double n = static_cast<double>(a.count_lag);
            return a.M2_lag / (n * (n - 1.0));
        };

        const std::string key = sat + "_" + sig1 + "_" + sig2;
        const auto it = mw_stats_.find(key);
        if (it == mw_stats_.end()) return out;   // no arc on this (sat, signal-pair)
        const MwArcStats& s = it->second;
        out.exists = true;                 // the arc key is present; see MwArcZdMean::exists
        out.bia_corrected = s.bia_corrected;
        // The code channels are a property of this arc's observable and are served
        // regardless of maturity (a caller that skips an immature arc still wants to
        // report which channels it looked at).
        out.code1 = s.code1;
        out.code2 = s.code2;
        if (s.count_lag < 2 || !(s.lambda_wl > 0.0)) { out.count = s.count_lag; return out; }

        if (!s.bia_corrected) {
            // Undifferenced already: a no-BIA-fallback arc (raw MW, receiver+satellite
            // bias present; both cancel in a closed cycle) or the group's MW pivot arc
            // (BIA-subtracted, receiver bias present).  Either is a ZD-consistent
            // per-(station,sat) value; return it directly.
            out.mean_m         = s.mean_lag;
            out.var_of_mean_m2 = varOfMean(s);
            out.count          = s.count_lag;
            out.lambda_wl      = s.lambda_wl;
            out.valid          = std::isfinite(out.mean_m) && std::isfinite(out.var_of_mean_m2);
            return out;
        }

        // Non-pivot BIA arc: stored mean = MW_bia(sat) - MW_bia(pivot).  Re-add the
        // group pivot's undifferenced mean to recover MW_bia(sat).  The pivot key format
        // matches the pass-2 assignment (sd_pivot_sat_["<cons>_<sig1>_<sig2>"]).
        const std::string group_key =
            std::string(1, sat.empty() ? '?' : sat.front()) + "_" + sig1 + "_" + sig2;
        const auto pit = sd_pivot_sat_.find(group_key);
        if (pit == sd_pivot_sat_.end()) { out.count = s.count; return out; }  // no pivot ⇒ can't reconstruct
        const std::string pivot_key = pit->second + "_" + sig1 + "_" + sig2;
        const auto pvt = mw_stats_.find(pivot_key);
        if (pvt == mw_stats_.end() || pvt->second.count_lag < 2) { out.count = s.count_lag; return out; }
        const MwArcStats& p = pvt->second;

        out.mean_m = s.mean_lag + p.mean_lag;
        // Conservative: SD arc variance + pivot variance.  In a closed fundamental
        // cycle the common per-station pivot term cancels exactly across that station's
        // two arcs, so this over-counts the pivot's contribution: a safe (loose) row R.
        out.var_of_mean_m2 = varOfMean(s) + varOfMean(p);
        out.count          = std::min(s.count_lag, p.count_lag);
        out.lambda_wl      = s.lambda_wl;
        out.valid          = std::isfinite(out.mean_m) && std::isfinite(out.var_of_mean_m2);
        return out;
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
                    << "cross_sat_floor_m,cross_sat_floor_cycles,"
                    << "is_slip,is_lli,"
                    << "bia_corrected,sat_mw_bias_m,rounded_wl,wl_locked,"
                    << "mw_pivot,"
                    << "local_ref_m,local_ref_cycles,"
                    << "local_sigma_m,local_sigma_cycles,"
                    << "detect_mode,"
                    << "active_dev_m,active_dev_cycles\n";

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
                                           double cross_sat_floor_m,
                                           bool is_slip,
                                           bool is_lli,
                                           bool bia_corrected,
                                           double sat_mw_bias_m,
                                           int rounded_wl,
                                           bool wl_locked,
                                           const std::string& mw_pivot,
                                           double local_ref_m,
                                           double local_sigma_m,
                                           int detect_mode,
                                           double active_dev_m) const {
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
                    << lambda_wl << "," << thresh_m << "," << to_cyc(thresh_m) << ",";
        csv_pair(cross_sat_floor_m);   // diagnostic: the cross-sat warmup floor (m + cycles)
        mwwl_trace_ << (is_slip ? 1 : 0) << "," << (is_lli ? 1 : 0) << ","
                    << (bia_corrected ? 1 : 0) << ","
                    << std::setprecision(6) << sat_mw_bias_m << ","
                    << rounded_wl << "," << (wl_locked ? 1 : 0) << ","
                    << mw_pivot << ",";
        csv_pair(local_ref_m);
        csv_pair(local_sigma_m);
        mwwl_trace_ << detect_mode << ",";
        if (std::isfinite(active_dev_m))
            mwwl_trace_ << active_dev_m << "," << to_cyc(active_dev_m) << "\n";
        else
            mwwl_trace_ << ",\n";
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
        struct BandSignals {
            std::vector<std::string> codes;
            std::vector<std::string> phases;
        };
        std::map<char, BandSignals> bands;

        // Two independent knobs control which observables form MW pairs:
        //
        //  mw_frequency_mode → band scope:
        //    Mirror (default): obs.observed, exactly the bands the KF
        //      processes, keeping the detector aligned with the KF.
        //    Dual / Triple: obs.raw_observed filtered to the band set
        //      that @c getAllowedBandDigits returns for this constellation
        //      and FrequencyMode, independent of the KF's band scope.
        //    All:           obs.raw_observed unfiltered.
        //
        //  mw_channel_mode → within-band tracking-mode multiplicity:
        //    Mirror (default): inherits the KF's @c ChannelMode.
        //    Single: one code and one phase per band (e.g. L1W only on
        //      GPS L1), picked by the preference below.  Matches the
        //      legacy behaviour.
        //    All:    every tracking-mode channel in the band (e.g. both
        //      L1C and L1W on GPS L1), enabling additional cross-channel
        //      MW pairs for slip cross-consistency.
        const MwFrequencyMode mw_freq_raw = config_.getMwFrequencyMode();
        const bool use_raw       = (mw_freq_raw != MwFrequencyMode::Mirror);
        const bool all_channels  = (config_.getResolvedMwChannelMode() == ChannelMode::All);
        const auto& source       = use_raw ? obs.raw_observed : obs.observed;

        // Only Dual/Triple need a band-digit filter applied to raw_observed.
        // Mirror reads the already-band-filtered obs.observed, and All is
        // unfiltered by definition.  Map the constellation prefix once.
        std::set<char> allowed_digits;  // empty means "no filter"
        if (mw_freq_raw == MwFrequencyMode::Dual || mw_freq_raw == MwFrequencyMode::Triple) {
            auto consName = [](char c) -> std::string {
                switch (c) {
                case 'G': return "GPS";
                case 'E': return "Galileo";
                case 'R': return "GLONASS";
                case 'C': return "BeiDou";
                case 'J': return "QZSS";
                case 'S': return "SBAS";
                case 'I': return "IRNSS";
                default:  return {};
                }
            };
            const std::string cons = obs.satellite.empty() ? "" : consName(obs.satellite[0]);
            const FrequencyMode fm =
                (mw_freq_raw == MwFrequencyMode::Dual) ? FrequencyMode::Dual
                                                       : FrequencyMode::Triple;
            allowed_digits = getAllowedBandDigits(cons, fm);
        }

        // In single-channel mode the bucket holds one key per band, picked
        // by a two-level preference:
        //   1. KF-chosen (key is in obs.observed): keeps MW and KF aligned
        //      on which channel represents the band.  Without this,
        //      iterating raw_observed alphabetically would pick L1C even
        //      when the KF estimates L1W.
        //   2. Canonical tracking-attribute priority via
        //      @c signalAttributePriority: picks the best channel for
        //      bands the KF doesn't track (e.g. L5X over L5I/L5Q for GPS
        //      L5 under signal_mode=dual).
        // This mirrors the same preference logic the KF uses to populate
        // obs.observed in @c PppModel::buildFrequencyBandMap.
        // A companion code row is not KF-chosen for this detector's purposes
        // (card SCC, R4).  With `network.second_code_channel` the KF estimates a
        // second band-1 code, so an unguarded `obs.observed` test would promote C1W
        // over C1C at every station that reports both, moving the detector's code for
        // the band and with it every MW arc, every threshold and every slip event the
        // run reports.  The rule is that the knob leaves the detector unchanged, so a
        // companion reads as it did without the knob: absent from obs.observed.
        // `companion_codes` is empty on every unarmed posture, so this costs one
        // lookup on an empty set and the verdict is byte-identical.  A fallback code
        // is deliberately not in that set and stays eligible as before.
        // The same rule applies to a second phase (card SID phase 6a, R4).  With
        // `network.second_phase_channel` the KF estimates a second band-1 phase, and
        // GPS L1 attribute priority puts 'W' ahead of 'C', so an unguarded test would
        // promote L1W over L1C in the band's phase bucket at every station that
        // reports both, moving the detector's phase for the band and with it every
        // MW arc and every slip event.  `second_phases` is empty on every unarmed
        // posture, so this costs one lookup on an empty set and the verdict is
        // byte-identical.
        auto isKfChosen = [&obs](const std::string& key) {
            if (obs.companion_codes.count(key)) return false;
            if (obs.second_phases.count(key))   return false;
            return obs.observed.find(key) != obs.observed.end();
        };
        auto priorityOf = [&obs](const std::string& key) -> int {
            if (key.size() < 3) return std::numeric_limits<int>::max();
            const auto cls = classifySignalForSat(obs.satellite, key);
            if (cls.band == FrequencyBand::Unknown)
                return std::numeric_limits<int>::max();
            // signalAttributePriority switches on band only; the
            // constellation string is unused but the API takes it for
            // forward compatibility, so the sat id is passed as a stable hint.
            return signalAttributePriority(obs.satellite, cls.band, cls.attribute);
        };
        auto preferNew = [&](const std::string& incumbent,
                              const std::string& candidate) {
            const bool inc_kf = isKfChosen(incumbent);
            const bool cand_kf = isKfChosen(candidate);
            if (cand_kf != inc_kf) return cand_kf;   // KF-chosen always wins.
            return priorityOf(candidate) < priorityOf(incumbent);
        };

        for (const auto& [key, val] : source) {
            if (key.size() < 2) continue;
            const char prefix = key[0];
            if (prefix != 'C' && prefix != 'L') continue;
            if (key.find('_') != std::string::npos) continue;
            // Second half of the companion-code guard (card SCC, R4).  When `source`
            // is obs.observed (mw_frequency_mode Mirror) a companion key would enter a
            // bucket that did not hold it without the knob; skip it, so Mirror sees the
            // same key set as unarmed.  When `source` is obs.raw_observed the key was
            // always there and dropping it would change the unarmed behaviour, so it
            // stays, and the isKfChosen guard above keeps it from winning the band.
            // With the set empty (knob off) both paths are byte-identical.
            if (!use_raw && obs.companion_codes.count(key)) continue;
            // Second half of the second-phase guard (card SID phase 6a, R4), with one
            // deliberate difference from the line above.  In a single-channel bucket
            // (mw_channel_mode mirror or single) the second phase must be invisible, so
            // the detector keeps the key set it had unarmed.  In all-channel mode the
            // bucket asks for every tracking-mode channel of the band, which is the
            // mode the card's G1c arm measures, so the second phase enters and the
            // cross-band pair loop below forms its own pairs with it.  Measured, not
            // assumed: with mw_frequency_mode Mirror the card SCC line above has already
            // removed the companion code, so `matchChannel` pairs an L1W phase with the
            // latched C1C.  That puts the receiver's C1C-minus-C1W inter-signal delay into
            // the MW value as a constant per arc, which the arc's own running reference
            // absorbs; it is a level offset, not a slope, so it does not by itself
            // produce jump detections.  With the set empty (knob off) the result is
            // byte-identical.
            if (!use_raw && !all_channels && obs.second_phases.count(key)) continue;
            const char band = key[1];
            if (band < '1' || band > '9') continue;
            // Band-scope filter (empty set = "no filter").
            if (!allowed_digits.empty() && allowed_digits.count(band) == 0)
                continue;

            auto& bs = bands[band];
            auto& bucket = (prefix == 'C') ? bs.codes : bs.phases;
            if (all_channels) {
                bucket.push_back(key);
            } else if (bucket.empty()) {
                bucket.push_back(key);
            } else if (preferNew(bucket.front(), key)) {
                bucket.front() = key;
            }
        }

        std::vector<char> band_ids;
        for (const auto& [b, bs] : bands)
            if (!bs.codes.empty() && !bs.phases.empty())
                band_ids.push_back(b);

        // Sort band_ids by carrier frequency descending so every pair
        // built below has f1 > f2.  The MW sign convention requires
        // signed Δf for slip-integer attribution to come out right
        // (computeMwMeters uses the signed f_diff; the wavelength
        // helper uses |f_diff|).  Without this normalization the
        // lexical map-iteration order of band characters produces
        // pairs whose f1 < f2 for constellations where the band-letter
        // sort doesn't match the frequency sort, notably Galileo
        // (bands 1,5,6,7,8 with frequencies 1575/1176/1278/1207/1192
        // MHz; the (5,*) pairs come out with f1 < f2), QZSS, and
        // BeiDou.  The downstream culprit-attribution sign flip in
        // CycleSlipManager::computeCulpritSlipIntegers assumes the
        // signed-Δf convention, so a wrong-order pair contributes a
        // sign-flipped slip integer to the cross-pair consistency
        // check, masking what would otherwise be a clean attribution
        // and forcing fall-through to reset instead of integer repair.
        //
        // Tie-break with band character so the order stays
        // deterministic across runs with identical signal sets.
        std::sort(band_ids.begin(), band_ids.end(), [&](char a, char b) {
            const auto& pa = bands.at(a).phases.front();
            const auto& pb = bands.at(b).phases.front();
            const double fa = ppp_utils_.getFrequency(pa, obs.system, obs.satellite);
            const double fb = ppp_utils_.getFrequency(pb, obs.system, obs.satellite);
            if (std::isfinite(fa) && std::isfinite(fb) && fa != fb)
                return fa > fb;
            return a < b;
        });

        // Helper: pick the code on the same RINEX-3 tracking-mode channel
        // as the phase (e.g. L1C → C1C, L1W → C1W) so the MW combination
        // sees consistent inter-channel biases on both ends.  Fall back
        // to the first code in the band if no same-channel code exists
        // (rare; happens when only one tracking mode publishes a code).
        auto matchChannel = [](const std::string& phase,
                                const std::vector<std::string>& codes) -> std::string {
            if (codes.empty()) return {};
            if (phase.size() >= 3) {
                const char ch = phase[2];
                for (const auto& c : codes) {
                    if (c.size() >= 3 && c[2] == ch) return c;
                }
            }
            return codes.front();
        };

        std::vector<UncombinedPair> pairs;
        for (size_t i = 0; i < band_ids.size(); ++i) {
            for (size_t j = i + 1; j < band_ids.size(); ++j) {
                const auto& b1 = bands[band_ids[i]];
                const auto& b2 = bands[band_ids[j]];
                for (const auto& phase1 : b1.phases) {
                    const std::string code1 = matchChannel(phase1, b1.codes);
                    if (code1.empty()) continue;
                    for (const auto& phase2 : b2.phases) {
                        const std::string code2 = matchChannel(phase2, b2.codes);
                        if (code2.empty()) continue;
                        pairs.push_back({phase1, phase2, code1, code2});
                    }
                }
            }
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

    bool MwSlipDetector::processMwSample(const PppObservation& obs,
        const std::string& stats_key,
        double mw_m, double lambda_wl,
        const std::string& sig1, const std::string& sig2,
        bool bia_corrected, double sat_mw_bias,
        int* out_slip_cycles,
        bool* out_wl_known,
        double* out_slip_frac,
        double* out_slip_wl_float,
        const std::string& pivot_sat,
        int pivot_zd_slip_in,
        bool pivot_zd_known_in,
        const std::string& code1,
        const std::string& code2,
        MwSampleEvidence* out_evidence) {

        // Detection strength (mw_evidence_refloat): the lane class is known here
        // and nowhere downstream, so publish it up front; the ratio and the arc
        // maturity are written at each firing gate from the quantities that gate
        // tested.  A path that has no meaningful ratio leaves NaN.
        if (out_evidence) {
            out_evidence->evidence_ratio = std::numeric_limits<double>::quiet_NaN();
            out_evidence->arc_epochs     = 0;
            out_evidence->bia_corrected  = bia_corrected;
        }

        if (out_slip_cycles) *out_slip_cycles = 0;
        if (out_wl_known)    *out_wl_known = false;
        if (out_slip_frac)   *out_slip_frac = 0.5;
        if (out_slip_wl_float)
            *out_slip_wl_float = std::numeric_limits<double>::quiet_NaN();

        // Apply stored in-house bias (if any) before the sample is
        // accumulated.  Modifies mw_m and bia_corrected in place; downstream
        // logic then treats the arc as if external BIA had been applied.
        if (config_.getInhouseBiasEstimation()) {
            applyEstimatedBias(mw_m, bia_corrected, stats_key);
        }

        auto& stats = mw_stats_[stats_key];
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
        const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

        // Detect data gaps: if the satellite was absent for longer than
        // the configured threshold, discard the stale arc and start fresh.
        // The threshold is overridable via setMaxGapSeconds; Application
        // tightens it to 1.5 × the sampling interval when IMU isn't
        // available and the data rate is coarser than 5 s.
        if (stats.count > 0 && stats.last_epoch_s > 0.0) {
            const double gap = epoch_s - stats.last_epoch_s;
            if (gap > max_gap_seconds_) {
                log_manager_.getLogger("mw_slip_detector")->debug(
                    "Data gap {:.3f}s > {:.3f}s for {}: resetting MW arc (n={})",
                    gap, max_gap_seconds_, stats_key, stats.count);
                stats = MwArcStats{};          // reset to default-initialized
            }
        }

        // Record the code channels the caller formed this MW from (after the gap
        // reset, so a fresh arc is stamped too).  Bookkeeping only: mw_m arrives
        // pre-computed, so this cannot change a detection.  Served by arcZdMean so a
        // consumer modelling the arc's satellite code bias names the OSB column the
        // observable carries instead of guessing it from the phase attribute.
        if (!code1.empty()) stats.code1 = code1;
        if (!code2.empty()) stats.code2 = code2;

        // Snapshot the pre-update arc state for potential rollback
        // by resetSatellite / resetSatelliteSignal.  Used when the KF
        // post-fit residual check identifies a slip that the MW test
        // missed: the reset functions restore this snapshot (preserving
        // accumulated noise statistics) instead of wiping the arc.  For
        // first-epoch arcs (count==0) and gap-reset arcs (just zeroed
        // above) the snapshot captures the empty MwArcStats, so a
        // rollback behaves like an erase.
        arc_snapshots_[stats_key] = stats;

        // Common-band scope (getEpochPairStatus, SPEC_event_harness §14): stamp the
        // arc as processed this epoch here, after the gap reset (so an arc the gap
        // just zeroed reports its fresh, immature count and cannot be read as
        // evidence of silence) and before every early return below, including the
        // two that erase the arc.  A sample that reaches the flag decision
        // overwrites this with the real threshold and verdict; one that leaves
        // earlier keeps the count, which is all a maturity gate needs.  Bookkeeping
        // only.  LLI shadow (§14.11): is this the first sample after an epoch the
        // receiver flagged on one of this pair's bands?  Computed once here and
        // carried into the second stamp below, so both records agree.
        const bool lli_shadowed = [&] {
            const auto it = prev_lli_bands_.find(obs.satellite);
            return it != prev_lli_bands_.end()
                && (it->second.count(sig1) != 0 || it->second.count(sig2) != 0);
        }();
        {
            auto& st = epoch_pair_status_[stats_key];
            st.arc_n_at_sample = static_cast<int>(stats.count);
            st.flagged         = false;
            st.thresh_m        = std::numeric_limits<double>::quiet_NaN();
            st.lambda_wl       = lambda_wl;
            st.lli_shadow      = lli_shadowed;
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
            stats.mw_window_head = 0;
            stats.mw_window_size = 0;
            pushLocalWindow(stats, mw_m);  // seed the local ring like the mean

            // WL integer locking waits for the statistical lock criterion
            // below (at least kWlLockMinN samples).  A single MW sample is
            // too noisy (especially for EWL pairs) for a reliable rounding
            // decision.

            // Sanity check: a BIA-corrected MW value should land near
            // integer × λ_WL on a clean phase track.  When the very first
            // sample of a fresh arc has a large fractional offset, the
            // arc was almost certainly initialized with a slip already
            // included (e.g., a slip occurred during the data gap that
            // triggered the reset, or this is the first observation of
            // the satellite after a long absence and the tracking loop
            // is still settling).  Declare a slip on init so the
            // ambiguity gets reset, instead of letting the corrupt value
            // become the new running mean; an unflagged corrupt baseline
            // lets even a 30-cycle slip pass through to the post-fit
            // residuals.
            if (bia_corrected && lambda_wl > 0.0) {
                const double init_cyc = mw_m / lambda_wl;
                const double frac = std::abs(init_cyc - std::round(init_cyc));

                // Effective threshold: config-driven init_frac_threshold_
                // floor, lifted by 3σ from the cross-sat MAD pool when
                // available (3σ = conventional 99.7% Gaussian gate).
                // Stochastic-σ lifts (zenith / predicted) are
                // intentionally excluded (see computeAdaptiveThreshold).
                double effective_thresh = init_frac_threshold_;
                {
                    const std::string group_key = makeGroupKey(stats_key);
                    if (!group_key.empty()) {
                        auto it = cross_sat_sigma_m_.find(group_key);
                        if (it != cross_sat_sigma_m_.end() && std::isfinite(it->second)) {
                            // The pool holds active-statistic deviations
                            // (TD-scaled in the TimeDifference modes), but
                            // the quantity gated here, the fractional
                            // offset of a single level sample from
                            // integer·λ, has level noise.  De-scale back
                            // to the level domain (÷√2 TD1, ÷√6 TD2; no-op
                            // in the level modes), or the 3σ lift inflates
                            // past the structural 0.5-frac ceiling and
                            // silently disables this check in the mode
                            // that relies on it most (TD erase-restarts).
                            const double sigma_xsat_level_m =
                                it->second / activeStatScale();
                            const double sigma_xsat_cyc = sigma_xsat_level_m / lambda_wl;
                            effective_thresh = std::max(effective_thresh, 3.0 * sigma_xsat_cyc);
                        }
                    }
                }

                if (frac > effective_thresh) {
                    log_manager_.getLogger("mw_slip_detector")->warn(
                        "MW init suspicious for {}: BIA-corrected MW={:.4f} m → "
                        "{:.3f} WL cyc has fractional |frac|={:.3f} > {:.2f}. "
                        "Forcing ambiguity reset (unknown integer slip during arc reset).",
                        stats_key, mw_m, init_cyc, frac, effective_thresh);

                    // Trace record: write the actual quantities the
                    // decision used.  dev = frac · λ is the suspicious
                    // offset from integer × λ in meters; thresh =
                    // effective_thresh · λ is the meter equivalent of
                    // the cycle-domain gate.  Matches the column
                    // semantics of regular slip rows so trace consumers
                    // can compare init-slip rows to running-slip rows
                    // on the same |dev| vs thresh basis.
                    if (mwwl_trace_.is_open()) {
                        const double dev_init_m    = frac * lambda_wl;
                        const double thresh_init_m = effective_thresh * lambda_wl;
                        writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                                         dev_init_m, NaN, stats.count, lambda_wl,
                                         thresh_init_m, /*cross_sat_floor_m=*/NaN,
                                         /*is_slip=*/true, /*excessive=*/false,
                                         bia_corrected, sat_mw_bias,
                                         stats.rounded_wl, stats.wl_locked, pivot_sat);
                    }

                    if (out_slip_cycles) *out_slip_cycles = 0;     // unknown integer
                    if (out_wl_known)    *out_wl_known    = false;
                    if (out_slip_frac)   *out_slip_frac   = frac;
                    // Strength: the gate tested frac against effective_thresh, so
                    // that ratio is this path's |dev|/threshold.  arc_epochs stays
                    // 0: an init-suspicious flag fires on the arc's first sample
                    // and has no maturity to report (which is why the evidence
                    // rule's maturity floor refuses it).
                    if (out_evidence && effective_thresh > 0.0)
                        out_evidence->evidence_ratio = frac / effective_thresh;
                    return true;
                }
            }

            if (mwwl_trace_.is_open())
                writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean, 0.0, NaN,
                                 stats.count, lambda_wl, NaN, /*cross_sat_floor_m=*/NaN,
                                 false, false,
                                 bia_corrected, sat_mw_bias,
                                 stats.rounded_wl, stats.wl_locked, pivot_sat);
            return false;
        }

        double mw_diff = mw_m - stats.prev_mw;

        // ---- Locked-arc TD confirmation: resolution (TimeDifference) ----
        // One epoch ago the TD gate fired on this locked arc and set
        // td_confirm_pending instead of silently absorbing.  A step and a
        // one-epoch outlier are distinguishable now: after an outlier the
        // level returns to the pre-jump track, and after a real step it
        // stays on the post-jump track.  Compare the two level hypotheses
        // against the current sample and act.
        if (stats.td_confirm_pending) {
            const double jump_m      = stats.td_confirm_jump_m;
            const double pre_mw      = stats.td_confirm_pre_mw;
            const double flag_dev    = stats.td_confirm_dev_m;
            const double flag_thresh = stats.td_confirm_thresh_m;
            const int    flag_mode   = stats.td_confirm_mode;
            stats.td_confirm_pending = false;  // resolved (or moot) either way

            // Resolution requires the same frame the hypotheses were formed
            // in: still TD mode, still locked, and the incoming sample in
            // the same bias frame (a bia_corrected flip would compare
            // levels across a frame jump).  Otherwise the pending flag is
            // dropped, and drift demotion remains the backstop for these rare
            // cases.
            if (detect_reference_ == MwDetectReference::TimeDifference
                && stats.wl_locked
                && bia_corrected == stats.bia_corrected) {
                // A pivot ZD slip at this epoch moves the SD frame between
                // hypothesis formation (flag epoch) and testing (now): both
                // residuals would be measured against old-frame levels and
                // the outcome would be meaningless.  The flag (a k·σ gate
                // exceedance) is evidence of a real event, so unclassifiable
                // evidence resolves to the safe action: unknown-integer slip.
                const bool frame_clean =
                    (pivot_zd_slip_in == 0) && pivot_zd_known_in;

                const double r_outlier = std::abs(mw_m - pre_mw);
                const double r_step    = std::abs(mw_m - (pre_mw + jump_m));

                if (!frame_clean || r_step < r_outlier) {
                    // Confirmed step (or unclassifiable under a moved
                    // frame): demote and emit the slip now instead of
                    // waiting about a dozen epochs for Hatch-mean migration
                    // to trip drift demotion (during which the stale integer
                    // would be re-exposed to AR).  The arc is erased (its
                    // mean/M2/windows straddle the step), and the init-frac
                    // check independently verifies the restart, as on the
                    // unlocked-slip path.
                    //
                    // Always an unknown integer (downstream reset, never
                    // repair): a deferred result is one epoch late, so a
                    // published integer would (a) be SD→ZD re-referenced
                    // with the wrong epoch's pivot slip, (b) pair in
                    // TurboEdit with this epoch's GF value, the sign-flipped
                    // echo of the real jump, and (c) conflict with the KF
                    // post-fit / TurboEdit actions already taken at the
                    // flag epoch.  Reset is the only safe deferred action
                    // (reset-over-repair).
                    log_manager_.getLogger("mw_slip_detector")->warn(
                        "MW locked-arc TD slip CONFIRMED for {} (epoch {}): "
                        "jump={:.3f} m ({:.2f} WL), {}. Demoting N_WL={} and "
                        "restarting arc; unknown integer -> reset.",
                        stats_key, stats.count, jump_m,
                        lambda_wl > 0.0 ? jump_m / lambda_wl : 0.0,
                        frame_clean
                            ? "level held post-jump (r_step < r_outlier)"
                            : "SD frame moved by a pivot slip this epoch "
                              "(hypotheses unclassifiable)",
                        stats.rounded_wl);

                    // Flagged trace row (the flag epoch's row already
                    // showed is_slip=1; this row is the action).  The gate
                    // quantities are carried from the flag epoch rather
                    // than recomputed, because recomputation does not
                    // reproduce the firing gate for warmup-branch locks (the
                    // cross-sat EWMA floor updates every epoch and
                    // re-expands at the new elevation) or for TD1-fallback
                    // flags inside TD2 order; so |active_dev|/threshold > 1
                    // holds on this row by construction.
                    if (mwwl_trace_.is_open()) {
                        double local_sigma_conf = NaN;
                        const double local_ref_conf =
                            computeLocalReference(stats, &local_sigma_conf);
                        writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                                         /*dev_from_mean=*/mw_m - stats.mean,
                                         computeMadSigma(stats), stats.count,
                                         lambda_wl, flag_thresh,
                                         crossSatSigmaFloor(stats_key, obs.elevation),
                                         /*is_slip=*/true, /*is_lli=*/false,
                                         stats.bia_corrected, sat_mw_bias,
                                         stats.rounded_wl, stats.wl_locked,
                                         pivot_sat, local_ref_conf,
                                         local_sigma_conf,
                                         flag_mode,
                                         /*active_dev_m=*/flag_dev);
                    }

                    if (out_slip_cycles) *out_slip_cycles = 0;
                    if (out_wl_known)    *out_wl_known    = false;
                    if (out_slip_frac)   *out_slip_frac   = 0.5;
                    // Strength: the gate quantities are the ones carried from the
                    // flag epoch (see the trace comment above; recomputing them
                    // here does not reproduce the firing gate), so the ratio is
                    // |flag_dev| / flag_thresh, which is > 1 by construction.
                    if (out_evidence) {
                        if (flag_thresh > 0.0)
                            out_evidence->evidence_ratio =
                                std::abs(flag_dev) / flag_thresh;
                        out_evidence->arc_epochs = static_cast<int>(stats.count);
                    }
                    // Float step estimate (cycles): the integer stays
                    // withheld by the deferred-path contract above, but the
                    // confirmed level gap's magnitude is sound, so it is
                    // exported to select candidates (TurboEdit gap-bridge solve).
                    // Withheld when the SD frame moved this epoch (a pivot
                    // slip makes the gap's frame ambiguous).
                    if (out_slip_wl_float && frame_clean && lambda_wl > 0.0)
                        *out_slip_wl_float = jump_m / lambda_wl;
                    // Stash the pre-jump level for the settling across-gap
                    // WL estimate (getAcrossGapWlEvidence): the float above
                    // is a single-sample jump biased by the re-acquisition
                    // NL-code transient; (post-restart Hatch mean − this
                    // level) re-estimates the step with √n-averaged noise
                    // as the new arc accumulates.  Same SD frame as the
                    // live arcs (re-anchors shift it alongside); frame
                    // comparability is re-checked at query time.
                    if (frame_clean && lambda_wl > 0.0)
                        demoted_levels_[stats_key] = DemotedArcLevel{
                            pre_mw, lambda_wl, epoch_s, stats.bia_corrected };
                    mw_stats_.erase(stats_key);
                    return true;
                }

                // Outlier: the level came back to the pre-jump track.  Keep
                // the lock and excise the outlier epoch from the TD chain:
                // this epoch's TD re-bases on the pre-outlier level (the
                // jump with the outlier removed; otherwise the gate would
                // fire again on the −jump echo), and prev_diff is
                // invalidated so TD2 takes its one-epoch TD1 fallback
                // instead of differencing against the outlier-sized TD1.
                log_manager_.getLogger("mw_slip_detector")->debug(
                    "MW locked-arc TD flag for {} resolved as one-epoch "
                    "outlier (r_outlier={:.3f} m < r_step={:.3f} m); lock "
                    "kept, outlier epoch excised from the TD chain.",
                    stats_key, r_outlier, r_step);
                mw_diff = mw_m - pre_mw;
                stats.prev_diff = NaN;
            }
        }

        // Statistical WL lock criterion.  The Hatch mean is a Gaussian
        // estimator of integer × λ_WL plus per-sample noise; its standard
        // error decreases as σ_per_sample / √n.  Lock when the worst-case
        // (kσ-bounded) fractional residual is comfortably inside the
        // rounding basin:
        //
        //     |frac| + k_σ · σ_mean / λ_WL  <  0.5
        //
        // with σ_mean derived from the Welford accumulator
        //
        //     σ²_mean = M2 / (n · (n − 1))
        //
        // and k_σ = 3 (the ±3σ interval stays inside the rounding basin, so
        // round(mean/λ_WL) lands on the true integer with 99.7% confidence).
        // The gate adapts to the arc instead of using a fixed count and
        // frac heuristic:
        //
        //   - Clean BIA-corrected arcs (sub-cm phase, modest code noise)
        //     have shrunk σ_mean enough by n=kWlLockMinN to pass the kσ
        //     test on the next eligible epoch.
        //   - Noisy arcs (low elevation, high multipath) wait until √n
        //     has averaged the noise down enough; locks happen when
        //     warranted, not on a fixed clock.
        //
        // Hard floor of n ≥ kWlLockMinN: the empirical Welford σ from a
        // handful of samples severely underestimates the long-term noise
        // envelope on a low-elevation arc.  The first few epochs are
        // routinely tight by chance, and a kσ test on that tight σ passes
        // a premature lock that later snaps to the wrong integer once the
        // true noise envelope emerges.  A floor of 10 lets the empirical
        // variance stabilize before the lock test trusts it, without
        // significantly delaying clean arcs (whose σ_mean shrinks
        // monotonically with n, so they pass at n=10 as cleanly as they
        // would at n=3).
        //
        // Hard cap on raw |frac|: the statistical test alone trusts a
        // sufficiently tight σ_mean to round 3.33 → 3 with 99.7%
        // confidence, but if a sat has a real per-(sat, signal) bias
        // not captured by BIA, the running mean settles to a non-integer
        // value with small standard error.  The math is internally
        // consistent (mean is well-determined to be 3.33) but the
        // rounded integer can be off by a cycle if the true integer is
        // 4 with bias −0.67 instead of 3 with bias +0.33.  Refuse to
        // lock when |frac| exceeds @c kWlMaxFrac regardless of
        // statistical confidence; treat large |frac| as a real bias
        // signal that disqualifies the lock.
        //
        // Note: on lock the running mean is not snapped to integer × λ.
        // Keeping it live lets Welford continue to track the arc, which
        // is the basis of the drift-demotion check applied below (after
        // the slip-detection branch).  If the empirical mean drifts past
        // kWlLockDriftMaxCyc cycles from the locked integer, the lock is
        // demoted immediately.  That safety net catches both locks that
        // were wrong from the start and accumulated slip sequences that
        // the per-epoch slip detector absorbed as outliers.
        constexpr double kWlLockKSigma  = 3.0;
        constexpr size_t kWlLockMinN    = 10;
        if (stats.bia_corrected && !stats.wl_locked && lambda_wl > 0.0
            && stats.count >= kWlLockMinN) {
            const double mean_cyc = stats.mean / lambda_wl;
            const double frac     = mean_cyc - std::round(mean_cyc);
            const double var_per_sample = stats.M2 / static_cast<double>(stats.count - 1);
            const double sigma_mean_m   = std::sqrt(var_per_sample / static_cast<double>(stats.count));
            const double sigma_mean_cyc = sigma_mean_m / lambda_wl;

            const double worst_case_frac = std::abs(frac) + kWlLockKSigma * sigma_mean_cyc;
            if (worst_case_frac < 0.5 && std::abs(frac) < kWlMaxFrac) {
                stats.rounded_wl = static_cast<int>(std::round(mean_cyc));
                stats.wl_locked  = true;
                log_manager_.getLogger("mw_slip_detector")->info(
                    "WL locked for {} (epoch {}): N_WL={}, mean_cyc={:.3f}, "
                    "frac={:+.3f}, σ_mean={:.3f} cyc, worst-case={:.3f} cyc",
                    stats_key, stats.count, stats.rounded_wl, mean_cyc,
                    frac, sigma_mean_cyc, worst_case_frac);
            }
        }

        // In-house bias commit path: runs when the toggle is enabled, no
        // external BIA was applied, and the running mean has converged
        // statistically close enough to an integer for a confident commit.
        // Promotes the arc to bia_corrected + wl_locked in one step (the
        // function stores the residual as the bias and subtracts it from
        // the mean, which centers the mean near integer × λ).  After
        // commit, apply the just-stored bias to the current sample so the
        // dev / slip-check / trace below operate in the bias-corrected
        // frame.  Without this rebalancing, dev would equal the (large)
        // bias on the commit epoch, the slip detector would false-fire,
        // and the trace would show a non-integer mw_m next to an
        // integer-near mw_mean_m, inconsistent with wl_locked=1.
        if (config_.getInhouseBiasEstimation()
            && !stats.bia_corrected
            && !stats.wl_locked) {
            if (tryCommitOrUpdateBias(stats, stats_key, lambda_wl, epoch_s)) {
                auto it = sat_pair_bias_.find(stats_key);
                if (it != sat_pair_bias_.end()) {
                    mw_m -= it->second.bias_m;
                }
                bia_corrected = true;
            }
        }

        // Statistic selection.  dev_hatch (vs the slow recursive mean) is
        // always computed: it backs the legacy reference mode, the
        // epoch-clean affirmation below, and the trace record.  (The
        // unlocked-arc level-disagreement backstop is level-vs-level: it
        // compares the Hatch mean itself against the local median and
        // never reads dev_hatch.)  The local reference (median of the
        // per-arc raw-MW ring, samples through k−1 only) is likewise
        // always computed; it is cheap, and tracing it in every mode gives
        // a free offline A/B.
        //
        // The active statistic (the deviation and mature-spread pair the
        // gate tests) is then picked per detect_reference_.  Deviation and
        // spread are the same quantity by construction in every mode except
        // the legacy HatchMean pairing:
        //  - HatchMean:      dev = mw − hatch_mean,    σ = MAD(TD1).
        //    (Legacy pairing; mismatched under trends; kept for A/B.)
        //  - LocalMedian:    dev = mw − ring median,   σ = √2·MAD(ring).
        //    Trend-immune to first order (lag ~N/2, MAD grows with trend),
        //    but a soft quadratic still walks the level off the median.
        //  - TimeDifference: dev = TD1 (or TD2),       σ = MAD(TD1/TD2).
        //    Memoryless, so immune to trend and curvature; pays √2 (√6)
        //    noise amplification.  TD2's first computable epoch per arc is
        //    the third sample; until then the TD1 pair substitutes (gated
        //    by warmup floors regardless at that age).
        const double dev_hatch = mw_m - stats.mean;
        double local_sigma_scaled = NaN;
        const double local_ref = computeLocalReference(stats, &local_sigma_scaled);
        const double td2 = std::isfinite(stats.prev_diff)
                               ? (mw_diff - stats.prev_diff) : NaN;

        const bool use_local = detect_reference_ == MwDetectReference::LocalMedian
                               && std::isfinite(local_ref);
        const bool use_td    = detect_reference_ == MwDetectReference::TimeDifference;

        double dev;             // deviation the gate tests
        double mode_sigma;      // mature spread of the same statistic (NaN → warmup floors)
        int    detect_mode_code; // trace tag: 0=hatch, 1=local, 2=td1, 3=td2
        if (use_local) {
            dev              = mw_m - local_ref;
            mode_sigma       = local_sigma_scaled;
            detect_mode_code = 1;
        } else if (use_td && td_order_ == MwTdOrder::Second && std::isfinite(td2)) {
            dev              = td2;
            mode_sigma       = computeTd2MadSigma(stats);
            detect_mode_code = 3;
        } else if (use_td) {
            dev              = mw_diff;
            mode_sigma       = computeMadSigma(stats);
            detect_mode_code = 2;
        } else if (detect_reference_ == MwDetectReference::HatchMean) {
            // Legacy pairing: the TD1 MAD governs the mature branch exactly
            // as it always has (bit-identical to the pre-refactor gate).
            dev              = dev_hatch;
            mode_sigma       = computeMadSigma(stats);
            detect_mode_code = 0;
        } else {
            // LocalMedian with an empty ring: unreachable after arc init
            // (the ring is seeded with the first sample); conservative
            // fallback to the legacy deviation under warmup floors.
            dev              = dev_hatch;
            mode_sigma       = NaN;
            detect_mode_code = 0;
        }

        const double thresh  = computeAdaptiveThreshold(stats, stats_key,
                                                        obs.elevation, mode_sigma);
        const double sigma   = computeMadSigma(stats);
        const bool is_slip   = std::isfinite(thresh) && std::abs(dev) > thresh;

        // Complete this sample's detection record (SPEC_event_harness §14) now
        // that the adaptive threshold and the flag verdict exist.  stats.count is
        // still the count before this sample (the Welford increment is further
        // down), the same maturity the cleanliness set tests, so the two records
        // agree on what "n" means.
        {
            auto& st = epoch_pair_status_[stats_key];
            st.arc_n_at_sample = static_cast<int>(stats.count);
            st.flagged         = is_slip;
            st.thresh_m        = thresh;
            st.lambda_wl       = lambda_wl;
            st.lli_shadow      = lli_shadowed;
        }

        // Decision-line readout (SPEC_event_harness §14.10): the same four numbers
        // as a log line.  The per-sample record above is only readable through a
        // verdict, and a verdict exists only on epochs where some pair fired, so
        // without this line the epochs that matter most (the silent lane on either
        // side of a slip) cannot be read.  Debug level: silent at the shipped
        // `warn` level, and a log line cannot change a decision, so an armed run
        // stays byte-identical on every product.  `n` is the arc count before this
        // sample, so a small n on a settled arc means the arc was just reset (gap
        // or LLI); that distinguishes "evaluated against the arc" from "arc
        // restarted and this sample re-based".
        log_manager_.getLogger("mw_slip_detector")->debug(
            "MWDEC sta={} {} t={:.1f} dev={:+.4f} m ({:+.3f} WL) thr={:.4f} m ({:.3f} WL) "
            "n={} lam_wl={:.4f} flag={} lli={} lli_shadow={}",
            station_label_,
            stats_key, epoch_s, dev, (lambda_wl > 0.0 ? dev / lambda_wl : 0.0),
            thresh, (lambda_wl > 0.0 ? thresh / lambda_wl : 0.0),
            stats.count, lambda_wl, is_slip ? 1 : 0,
            (obs.lli_flags.count(sig1) ? obs.lli_flags.at(sig1) : 0)
                | (obs.lli_flags.count(sig2) ? obs.lli_flags.at(sig2) : 0),
            lli_shadowed ? 1 : 0);

        // Level-frame slip indicator with the legacy pairing (Hatch dev vs
        // the TD1-MAD gate), used only to protect the Welford M2
        // admission below.  M2 integrates the Hatch-frame delta, so its
        // "no slip energy" rule must be judged in the Hatch frame: in the
        // memoryless TD modes a locked arc's post-slip samples pass the TD
        // gate as clean (TD ≈ 0) while still sitting dN·λ off the
        // not-yet-migrated mean, and admitting them would add (dN·λ)² per
        // epoch to the per-sample variance estimator until drift demotion
        // fires.  In HatchMean mode this is identical to is_slip
        // (bit-identity preserved); in TD1 mode thresh_level is recomputed
        // from the same TD1 spread and equals the gate above.
        const double thresh_level =
            (detect_reference_ == MwDetectReference::HatchMean)
                ? thresh
                : computeAdaptiveThreshold(stats, stats_key, obs.elevation, sigma);
        const bool is_slip_level =
            std::isfinite(thresh_level) && std::abs(dev_hatch) > thresh_level;

        // Cache for external queries (e.g. outlier tiebreaker).  Active-
        // reference quantities so the |dev|/σ score keeps its meaning in
        // every mode; while the active statistic's window is still immature
        // the gate σ is floor-driven, so fall back to the TD1 σ for the
        // score.  That is scale-consistent in every branch except TD2 (last_dev
        // would be √6-scaled against a √2-scaled σ, inflating the score
        // ~√3 during the one-epoch td2_window maturity lag); there, leave
        // the σ NaN so getWorstMwScore's isfinite guard skips the arc for
        // that epoch instead of returning a mis-scaled score.
        stats.last_dev   = dev;
        stats.last_sigma = std::isfinite(mode_sigma)
                               ? mode_sigma
                               : (detect_mode_code == 3 ? NaN : sigma);

        // Affirmative per-epoch cleanliness for the TurboEdit GF-only gate
        // (getEpochCleanPairs): the arc did not flag and the sample sits
        // within a conservative fraction of λ_WL of the arc mean, so the
        // wide-lane integer is demonstrably unchanged this epoch.
        // Deliberately not keyed on wl_locked: an unlocked mature arc with
        // a small dev is equally valid evidence of ΔN_WL = 0.  A locked
        // arc absorbing a real slip (is_slip true, no result emitted) is
        // the state this set must exclude.
        //
        // Affirmation must have discriminating power, so two extra
        // conditions apply beyond the 0.3-cycle bound:
        //  - real maturity: count ≥ kEpochCleanMinArcN.  A finite adaptive
        //    threshold is not a maturity proxy (the warm-up and cold-start
        //    branches return finite values for 2-sample arcs).
        //  - σ-awareness: a true ΔN_WL = ±1 slip (|dev| ≈ λ_WL) must sit
        //    ≥ 3σ outside the clean bound, i.e. 3σ ≤ (1 − 0.3)·λ_WL.
        //    Otherwise noise can pull a real slip inside the bound, and the
        //    false affirmation feeds TurboEdit a wrong ΔN_WL = 0 split
        //    (which the frac gate accepts on L1/L5-class pairs, where
        //    λ1/(λ1−λ2) ≈ −2.95 lands near-integer).  With the 3σ
        //    condition the false-affirm probability is ≤ Φ(−3) ≈ 0.13 %.
        // The 0.3-cycle bound itself is a cleanliness gate on a
        // known-integer hypothesis, not a detection threshold, so it stays
        // fixed instead of scaling with the empirical σ tiers; the σ
        // condition only decides whether the test is usable at all
        // (noisy low-elevation arcs never affirm, and GF-only flags there
        // fall back to reset).
        // The deviation bound deliberately uses the Hatch deviation in every
        // reference mode.  The local median absorbs a missed step within
        // ~N/2 epochs, after which the local deviation of a genuinely
        // slipped arc reads near zero; affirming cleanliness on the local
        // dev would hand TurboEdit a confident false ΔN_WL = 0 for its
        // GF-only split.  The ~100-epoch Hatch memory keeps an absorbed
        // step visible here long after the local window has lost it;
        // combined with !is_slip (the active-reference test) the gate is
        // conjunctive across both references in LocalMedian mode.
        constexpr double kEpochCleanMaxDevWlCyc = 0.3;
        constexpr std::size_t kEpochCleanMinArcN = 10;
        if (!is_slip && lambda_wl > 0.0
            && stats.count >= kEpochCleanMinArcN
            && std::isfinite(sigma)
            && 3.0 * sigma <= (1.0 - kEpochCleanMaxDevWlCyc) * lambda_wl
            && std::abs(dev_hatch) <= kEpochCleanMaxDevWlCyc * lambda_wl) {
            epoch_clean_pairs_.insert(stats_key);
            // The arc's sample count travels alongside the key so a consumer can
            // apply its own maturity bar and count what that bar refused.
            // kEpochCleanMinArcN above is this detector's internal floor for
            // TurboEdit's equal-slip split.  (The network's common-band scope,
            // SPEC_event_harness §14, runs its configurable maturity gate on the
            // getEpochPairStatus record instead.)
            epoch_clean_arc_n_[stats_key] = static_cast<int>(stats.count);
        }

        // Contribute this dev to the cross-satellite pool for this
        // (constellation × band-pair) group, but only when this sample
        // was not itself flagged as a slip.  The pool is finalized at
        // the end of detectLargeMwResets into cross_sat_sigma_m_, which
        // feeds the next epoch's warmup floor.  Skipping the push on
        // is_slip keeps a cycle-jump dev (multi-cycle × λ) out of the
        // constellation noise estimator.  MAD is statistically robust
        // to a single outlier in 5+, but the slip-dev is not noise: it
        // is a discrete event, wrong to include in a noise model, and
        // it compounds when slips happen on multiple sats in close
        // succession.  The group's MW pivot arc is processed
        // undifferenced and contributes its own deviation like any
        // other arc.
        const std::string group_key = makeGroupKey(stats_key);
        if (!group_key.empty() && !is_slip) {
            // Zenith-normalize before pooling so the cross-sat MAD is a clean
            // homoscedastic estimate rather than a blend of low-el (noisy) and
            // high-el (quiet) arcs.  computeAdaptiveThreshold re-expands it
            // per-arc via mwElevationWeight.
            //
            // The pooled quantity is the active deviation: the warmup floor
            // this pool feeds gates active-statistic deviations, so pooling
            // Hatch deviations in the other modes would mix statistics.  A
            // post-absorbed step or trend-lagged Hatch dev (multi-decimeter)
            // would inflate the EWMA floor for the whole constellation and
            // mask fresh-arc slips.  (In the TimeDifference modes the pool
            // therefore holds TD deviations, already on the differenced
            // noise scale the TD gate needs.)
            cross_sat_devs_[group_key].push_back(dev / mwElevationWeight(obs.elevation));
        }

        if (mwwl_trace_.is_open())
            writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                             /*dev_from_mean=*/dev_hatch, sigma,
                             stats.count, lambda_wl, thresh,
                             crossSatSigmaFloor(stats_key, obs.elevation),
                             is_slip, false,
                             stats.bia_corrected, sat_mw_bias,
                             stats.rounded_wl, stats.wl_locked, pivot_sat,
                             local_ref, local_sigma_scaled,
                             detect_mode_code, /*active_dev_m=*/dev);

        if (is_slip) {
            auto logger = log_manager_.getLogger("mw_slip_detector");

            if (stats.wl_locked) {
                // Locked arc: a single-sample threshold exceedance is
                // treated as a noise-outlier candidate rather than a
                // slip-shift trigger.  Rounding a single sample that lands
                // two cycles off the mean to a new integer and shifting
                // the arc there would be a brittle decision on noisy
                // low-elevation arcs, where |dev| > thresh routinely comes
                // from variance bursts, not real slips.
                //
                // Level modes (HatchMean / LocalMedian): absorb into
                // Welford (the mean is not pinned to rounded_wl×λ on
                // lock), so noise outliers barely perturb the mean while
                // accumulated genuinely-slipped samples migrate it past
                // kWlLockDriftMaxCyc, at which point the drift-demotion
                // block below unlocks the arc and signals a slip with
                // unknown integer for downstream re-resolution.  A real
                // slip keeps re-flagging every epoch in these modes, so
                // absorption loses nothing.
                //
                // TimeDifference modes: the memoryless TD flags a real
                // step exactly once, so silent absorption would discard
                // the only detection and leave the stale locked integer
                // exposed to AR for the dozen or so epochs of mean
                // migration before drift demotion.  Instead, defer one
                // epoch: set the confirmation pending state (resolved at
                // the top of the next processed sample, where outlier and
                // step are separable) and withhold the integer from AR
                // meanwhile.  The sample is still absorbed below as in the
                // level modes, so a declared outlier leaves
                // legacy-identical arc state.
                if (use_td && bia_corrected != stats.bia_corrected) {
                    // Frame-flip artifact: the incoming sample is in a
                    // different bias frame than the arc (per-epoch
                    // code-channel pairing can flip the BIA availability
                    // of the same key between epochs).  The deviation
                    // reflects the frame change, not a step/outlier
                    // hypothesis, so it must not enter the pending store as
                    // a jump; take the legacy absorb instead.
                    logger->debug(
                        "MW TD flag on locked {} ({}) coincides with a "
                        "bias-frame flip (sample bia={}, arc bia={}); "
                        "absorbing as a frame artifact, no confirmation.",
                        obs.satellite, PppUtils::getSystemString(obs.system),
                        bia_corrected, stats.bia_corrected);
                } else if (use_td && (pivot_zd_slip_in != 0 || !pivot_zd_known_in)) {
                    // The pivot's own ZD slipped this epoch: every locked
                    // peer's SD series steps by −dN_pivot in common mode.
                    // Deferring would desynchronize from the pass-2 SD→ZD
                    // re-referencing and from CycleSlipManager's pivot-slip
                    // consensus / in-place-repair handling, which operate
                    // on this epoch's results.  Behave like the unlocked
                    // path with an unknown integer instead: immediate slip,
                    // erase, downstream reset, synchronous with the handling
                    // built for this event.
                    logger->warn(
                        "MW TD flag on locked {} ({}) during a pivot ZD "
                        "slip (pivot dN={}, known={}): emitting immediate "
                        "unknown-integer slip and restarting arc (no "
                        "deferral across a moved SD frame).",
                        obs.satellite, PppUtils::getSystemString(obs.system),
                        pivot_zd_slip_in, pivot_zd_known_in);
                    if (out_slip_cycles) *out_slip_cycles = 0;
                    if (out_wl_known)    *out_wl_known    = false;
                    if (out_slip_frac)   *out_slip_frac   = 0.5;
                    // Strength: this epoch's own gate quantities (the flag is
                    // immediate here, not deferred), read before the erase.
                    if (out_evidence) {
                        if (thresh > 0.0)
                            out_evidence->evidence_ratio = std::abs(dev) / thresh;
                        out_evidence->arc_epochs = static_cast<int>(stats.count);
                    }
                    mw_stats_.erase(stats_key);
                    return true;
                } else if (use_td) {
                    stats.td_confirm_pending = true;
                    stats.td_confirm_jump_m  = mw_diff;
                    // Algebraic reference (mw − jump) rather than prev_mw:
                    // identical in the normal case, but stays correct when
                    // this epoch's TD was itself re-based by an outlier
                    // excision (prev_mw then holds the excised outlier).
                    stats.td_confirm_pre_mw  = mw_m - mw_diff;
                    // Carry the firing gate for the action trace row.
                    stats.td_confirm_dev_m    = dev;
                    stats.td_confirm_thresh_m = thresh;
                    stats.td_confirm_mode     = detect_mode_code;
                    logger->info(
                        "MW TD flag on locked {} ({}): |dev|={:.3f} m "
                        "({:.1f} WL) > thresh={:.3f} m, N_WL={}; pending "
                        "one-epoch step/outlier confirmation (integer "
                        "withheld from AR).",
                        obs.satellite, PppUtils::getSystemString(obs.system),
                        std::abs(dev), std::abs(dev) / lambda_wl, thresh,
                        stats.rounded_wl);
                } else {
                    logger->debug(
                        "MW outlier on locked {} ({}): |dev|={:.3f} m ({:.1f} WL) "
                        "> thresh={:.3f} m, N_WL={}, mean_cyc={:.3f}; "
                        "absorbing into Welford (drift-demotion will fire if "
                        "persistent).",
                        obs.satellite, PppUtils::getSystemString(obs.system),
                        std::abs(dev), std::abs(dev) / lambda_wl, thresh,
                        stats.rounded_wl, stats.mean / lambda_wl);
                }
                // Fall through to the Welford / drift-demotion block.
            } else {
                // Unlocked arc: no anchored integer to repair against,
                // so the slip is either resolved via a jump-based
                // estimate (BSSD pivot with BIA where dev/λ is near-
                // integer by construction) or surfaced as an unknown
                // slip; in both cases the arc is erased so the next
                // sample initializes a fresh one.
                int slip_cycles = 0;
                bool wl_known   = false;
                // In the TimeDifference modes the published integer is
                // rounded from a single differenced sample whose noise is
                // √2 (TD1) / √6 (TD2) times the level noise, so the fixed
                // 0.25-frac gate alone passes nearly everything once σ_frac
                // approaches a quarter cycle (TD2 at ordinary MW noise).
                // Require the integer to be statistically resolvable from
                // one sample (3·σ_active ≤ 0.25·λ, mirroring the epoch-clean
                // σ-awareness condition) before publishing; otherwise leave
                // wl_known=false so downstream resets instead of repairing
                // (reset-over-repair for low-confidence integers).  Not
                // applied to the level modes (their repair behavior is the
                // shipping baseline); a NaN σ (warmup) refuses to publish.
                const bool integer_resolvable =
                    !use_td || (std::isfinite(mode_sigma)
                                && 3.0 * mode_sigma <= 0.25 * lambda_wl);
                if (lambda_wl > 0.0 && integer_resolvable) {
                    const double dev_cyc  = dev / lambda_wl;
                    const double dev_frac = dev_cyc - std::round(dev_cyc);
                    if (std::abs(dev_frac) < 0.25 && std::abs(dev_cyc) >= 0.5) {
                        slip_cycles = static_cast<int>(std::round(dev_cyc));
                        wl_known    = true;
                        if (out_slip_frac) *out_slip_frac = std::abs(dev_frac);
                        logger->info(
                            "WL slip estimated from jump for {}: dN_WL={}, "
                            "dev_cyc={:.3f}, frac={:.3f}",
                            stats_key, slip_cycles, dev_cyc, dev_frac);
                    }
                }
                // Float jump estimate (cycles), regardless of the rounding
                // gate above: even an unresolvable integer carries a sound
                // magnitude for downstream candidate selection.
                if (out_slip_wl_float && lambda_wl > 0.0)
                    *out_slip_wl_float = dev / lambda_wl;
                if (out_slip_cycles) *out_slip_cycles = slip_cycles;
                if (out_wl_known)    *out_wl_known    = wl_known;

                const char* stat_name =
                    detect_mode_code == 1 ? "local_median"
                  : detect_mode_code == 2 ? "td1"
                  : detect_mode_code == 3 ? "td2"
                                          : "hatch_mean";
                logger->warn(
                    "MW slip detected for {} ({}): |dev|={:.3f} m ({:.1f} WL) "
                    "> thresh={:.3f} m ({:.1f} WL), ref={:.3f} m ({}), "
                    "sigma={:.4f} m, arc_n={}, lambda_wl={:.3f} m, bia={}, "
                    "locked=no, delta_wl={}",
                    obs.satellite, PppUtils::getSystemString(obs.system),
                    std::abs(dev), std::abs(dev) / lambda_wl,
                    thresh, thresh / lambda_wl,
                    use_local ? local_ref
                              : (use_td ? stats.prev_mw : stats.mean),
                    stat_name,
                    std::isfinite(sigma) ? sigma : -1.0,
                    stats.count, lambda_wl,
                    stats.bia_corrected ? "yes" : "no",
                    slip_cycles != 0 ? std::to_string(slip_cycles) + " WL" : "n/a");

                // Strength (the principal emission path): |dev| / thresh is the
                // gate this branch just failed, on the active statistic and the
                // arc's own adaptive threshold.  A detect-only consumer needs
                // this quantity, and slip_frac cannot substitute for it.
                // stats.count is read before the erase below.
                if (out_evidence) {
                    if (thresh > 0.0)
                        out_evidence->evidence_ratio = std::abs(dev) / thresh;
                    out_evidence->arc_epochs = static_cast<int>(stats.count);
                }

                mw_stats_.erase(stats_key);
                return true;
            }
        }

        // Update running statistics (Welford / Hatch).  Always runs,
        // including for locked arcs: the live mean is the basis of the
        // drift-demotion check below.  M2 (and therefore σ_emp) is not
        // updated on slip epochs: a slip-sized delta would inflate the
        // variance accumulator with a discrete-event contribution that
        // does not represent the underlying per-sample noise.  The mean
        // update still runs, so a real cycle slip migrates the running
        // mean past kWlLockDriftMaxCyc and trips drift demotion below; for
        // a noise outlier the 1/n weighting keeps the mean shift negligible.
        // Lagged snapshot before this epoch's value folds in: arcZdMean serves
        // these so the MW-WL layer's same-epoch read never re-ingests the
        // current observations (SPEC_mw_wl_card §3.3 double-count guard).
        stats.mean_lag  = stats.mean;
        stats.M2_lag    = stats.M2;
        stats.count_lag = stats.count;
        stats.count++;
        {
            const double eff_n  = static_cast<double>(std::min(stats.count, kMaxHatchCount));
            const double delta  = mw_m - stats.mean;
            stats.mean += delta / eff_n;
            // M2 admission is gated in both frames, the active statistic
            // (is_slip) and the Hatch frame (is_slip_level), because the
            // accumulated delta is Hatch-frame (see the is_slip_level
            // comment above).  Identical to !is_slip in HatchMean mode.
            if (!is_slip && !is_slip_level) {
                const double delta2 = mw_m - stats.mean;
                stats.M2 += delta * delta2;
            }
        }

        // diff_window push: skip slip epochs so cycle-jump diffs
        // (multi-meter on WL) don't enter the per-arc MAD estimator.
        // MAD is statistically robust to a single outlier but
        // partially-filled windows or successive slip events
        // compromise the robustness, and a slip-diff is not a noise
        // sample in any case, so excluding it is correct.  The local
        // raw-MW ring follows the same admission rule so a locked arc
        // absorbing a real slip keeps its pre-slip level as the local
        // reference (the deviation stays visible until drift demotion
        // resolves it) and a noise outlier never tilts the median.
        if (!is_slip) {
            stats.diff_window.push_back(mw_diff);
            if (stats.diff_window.size() > kMwWindowSize)
                stats.diff_window.pop_front();
            pushLocalWindow(stats, mw_m);
            // TD2 history (spread source for MwTdOrder::Second), same
            // admission rule and cap.  Maintained in every mode (one
            // subtraction and a deque push), so the statistic is warm if
            // the user switches modes between runs on the same data.
            if (std::isfinite(td2)) {
                stats.td2_window.push_back(td2);
                if (stats.td2_window.size() > kMwWindowSize)
                    stats.td2_window.pop_front();
            }
        }
        // prev_mw / prev_diff update unconditionally (slip epochs included),
        // as prev_mw always has.  After a locked arc absorbs a real step, the
        // next TD1 is post−post ≈ 0 (no echo).  A locked-arc noise outlier
        // echoes once (next TD1 = clean − outlier); where that echo is
        // flagged and absorbed again it is benign and kept out of every
        // window by the !is_slip admission rule, and in the TimeDifference
        // modes the confirmation above excises a declared outlier before
        // the echo is tested.  Unlocked arcs erase at the slip epoch, so
        // neither case arises.
        stats.prev_diff    = mw_diff;
        stats.prev_mw      = mw_m;
        stats.last_epoch_s = epoch_s;

        // Drift-based lock demotion.  When the running mean has
        // drifted past kWlLockDriftMaxCyc cycles from the locked
        // integer, the locked integer does not match the
        // cumulative evidence: either the lock was premature or
        // the arc has absorbed enough slipped samples to migrate.
        // Unlock and signal a slip with unknown integer so the
        // downstream AR / KF reset the ambiguity and let the arc
        // re-lock at the right integer once Welford settles.
        // Mean / M2 / count / diff_window are intentionally kept:
        // they carry the now-current noise estimate, and the lock
        // criterion's |frac| < kWlMaxFrac gate will keep the arc
        // unlocked until the mean has migrated close enough to the
        // new integer to round confidently.
        if (stats.wl_locked && lambda_wl > 0.0) {
            const double drift_cyc = stats.mean / lambda_wl - stats.rounded_wl;
            if (std::abs(drift_cyc) > kWlLockDriftMaxCyc) {
                log_manager_.getLogger("mw_slip_detector")->warn(
                    "WL lock demoted for {} (epoch {}): mean drifted to "
                    "{:.3f} cycles from N_WL={} (drift={:+.3f} cyc, "
                    "threshold={:.2f}). Signaling unknown-integer slip.",
                    stats_key, stats.count, stats.mean / lambda_wl,
                    stats.rounded_wl, drift_cyc, kWlLockDriftMaxCyc);
                stats.wl_locked  = false;
                stats.rounded_wl = 0;
                if (out_slip_cycles) *out_slip_cycles = 0;
                if (out_wl_known)    *out_wl_known    = false;
                if (out_slip_frac)   *out_slip_frac   = std::abs(drift_cyc);
                // Float estimate: the migrated mean's offset from the
                // demoted integer is the absorbed cumulative slip.
                if (out_slip_wl_float) *out_slip_wl_float = drift_cyc;
                // Strength: dev = the drift, threshold = the lock-drift bound
                // (this path writes no trace row of its own).  Nothing is
                // erased here, so stats.count is already the count at the flag.
                if (out_evidence) {
                    out_evidence->evidence_ratio =
                        std::abs(drift_cyc) / kWlLockDriftMaxCyc;
                    out_evidence->arc_epochs = static_cast<int>(stats.count);
                }
                return true;
            }
        }

        // Unlocked-arc level-disagreement backstop (LocalMedian and
        // TimeDifference modes).  The local median absorbs a missed step
        // within ~N/2 epochs and the memoryless TD statistic absorbs it in
        // one epoch with no re-test, both far faster than the Hatch mean's
        // ~kMaxHatchCount-epoch memory, and drift demotion above protects
        // only locked arcs.  Without a backstop, an unlocked arc that
        // quietly absorbed a real slip has no escape path: count keeps
        // rising, the statistical lock criterion eventually locks the wrong
        // integer (the frac/3σ lock gates only delay locking; they never
        // signal a slip), and that integer feeds AR via
        // getResolvedWlAmbiguities.  When the two level estimators (slow
        // Hatch mean vs short window median, both maintained and
        // frame-shifted identically in every mode) disagree by more than
        // the lock-drift bound, the arc's level history is internally
        // inconsistent: a step was absorbed, or the arc is on a drift
        // steep enough (≳ 4–5 mm/epoch sustained) that no level estimate
        // is trustworthy.  Mirror the unlocked-slip path: unknown-integer
        // slip, erase, fresh restart (the init-frac check then
        // independently verifies the new arc's first sample).  HatchMean
        // mode is excluded for bit-identity with the legacy gate; its
        // ~kMaxHatchCount-epoch dev_hatch persistence provides the re-test
        // behavior natively.
        const bool backstop_applies =
            use_local || (use_td && std::isfinite(local_ref));
        if (backstop_applies && !stats.wl_locked && lambda_wl > 0.0
            && stats.mw_window_size >= kMinEpochsForAdaptive) {
            const double level_gap_cyc = (stats.mean - local_ref) / lambda_wl;
            if (std::abs(level_gap_cyc) > kWlLockDriftMaxCyc) {
                log_manager_.getLogger("mw_slip_detector")->warn(
                    "MW level disagreement for {} (epoch {}): hatch mean "
                    "{:.3f} m vs local median {:.3f} m ({:+.3f} WL cyc > "
                    "{:.2f}) — an absorbed step or untrustworthy drift. "
                    "Signaling unknown-integer slip and restarting arc.",
                    stats_key, stats.count, stats.mean, local_ref,
                    level_gap_cyc, kWlLockDriftMaxCyc);

                // Trace record for the backstop reset: this epoch's regular
                // row was already written with is_slip=0 (the dev test
                // passed).  Without a flagged row this reset class would not
                // appear in the trace, the primary slip-chain verification
                // artifact, so mirror the init-slip row convention and write
                // the actual decision quantities:
                // dev = the level gap, thresh = the drift bound, giving
                // |dev|/thresh > 1 on the flagged row.
                if (mwwl_trace_.is_open()) {
                    writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                                     /*dev_from_mean=*/level_gap_cyc * lambda_wl,
                                     sigma, stats.count, lambda_wl,
                                     /*thresh_m=*/kWlLockDriftMaxCyc * lambda_wl,
                                     crossSatSigmaFloor(stats_key, obs.elevation),
                                     /*is_slip=*/true, /*is_lli=*/false,
                                     stats.bia_corrected, sat_mw_bias,
                                     stats.rounded_wl, stats.wl_locked, pivot_sat,
                                     local_ref, local_sigma_scaled,
                                     /*detect_mode=*/1,
                                     /*active_dev_m=*/level_gap_cyc * lambda_wl);
                }

                if (out_slip_cycles) *out_slip_cycles = 0;
                if (out_wl_known)    *out_wl_known    = false;
                if (out_slip_frac)   *out_slip_frac   = std::abs(level_gap_cyc);
                // Float estimate: the absorbed step is the level gap with its
                // sign reversed (post-step local median minus the slow Hatch mean).
                if (out_slip_wl_float) *out_slip_wl_float = -level_gap_cyc;
                // Strength: dev = the level gap, threshold = the drift bound,
                // the pairing this path's own trace row writes.  Count read
                // before the erase below.
                if (out_evidence) {
                    out_evidence->evidence_ratio =
                        std::abs(level_gap_cyc) / kWlLockDriftMaxCyc;
                    out_evidence->arc_epochs = static_cast<int>(stats.count);
                }
                mw_stats_.erase(stats_key);
                return true;
            }
        }

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

        // Drop any pre-epoch snapshots from the previous epoch.  Each
        // processMwSample call below will save a fresh snapshot for
        // its arc; this ensures the map only ever holds current-epoch
        // data for the resetSatellite / resetSatelliteSignal rollback
        // path (called by the KF post-fit residual handler, which
        // runs after this function returns).
        arc_snapshots_.clear();

        // Per-epoch affirmative-cleanliness set (getEpochCleanPairs):
        // rebuilt from scratch each pass so stale affirmations can never
        // leak into a later epoch's TurboEdit GF-only gating.
        epoch_clean_pairs_.clear();
        epoch_clean_arc_n_.clear();
        // The detection-grade per-epoch record (SPEC_event_harness §14): same
        // lifetime as the cleanliness set above, different question.
        epoch_pair_status_.clear();
        // LLI shadow (§14.11): rotate the one-epoch loss-of-lock memory before any
        // sample is processed, so every stamp this pass reads the previous epoch's
        // flags.  The LLI epoch itself never reaches processMwSample (the branch
        // below `continue`s after scheduling the resets), so its shadow falls on
        // the next epoch's first sample, the one compared against a mean that
        // never saw the epoch the receiver declared broken.
        prev_lli_bands_.swap(cur_lli_bands_);
        cur_lli_bands_.clear();

        // Helper: push a reset result (no known WL integer)
        auto pushReset = [&](const std::string& key, double lwl = 0.0,
                             const std::string& s1 = {}, const std::string& s2 = {},
                             const std::string& sat = {}) {
            results.push_back({ key, 0, false, lwl, s1, s2, sat });
        };

        // Capture epoch time from first valid observation for tracing
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        // Age out demoted-level stashes (across-gap WL settle evidence):
        // past kDemotedLevelMaxAgeS the consumer's own deferral cap has
        // long since expired and a stale level must not feed a future
        // unrelated demote's estimate.
        if (!demoted_levels_.empty()) {
            const double now_s = TimeUtils::continuousGpsSeconds(current_epoch_);
            for (auto it = demoted_levels_.begin(); it != demoted_levels_.end(); ) {
                if (now_s > 0.0 && it->second.epoch_s > 0.0 &&
                    now_s - it->second.epoch_s > kDemotedLevelMaxAgeS)
                    it = demoted_levels_.erase(it);
                else
                    ++it;
            }
        }

        // -----------------------------------------------------------------
        // Intermediate storage for the BSSD two-pass MW approach.
        // Used for both combined (PPP) and uncombined (PPP-RTK) paths
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

            // [DIAG] Per-sat coverage trace.  Lists the keys MwSlipDetector
            // saw in each source map at this epoch, which shows whether a
            // missing arc later in the loop is due to (i) missing keys
            // upstream, (ii) the IF/uncombined path branching, or (iii) a
            // silent skip inside computeMwMeters / computeSatMwBias.
            /*
            if (logger->should_log(spdlog::level::debug)) {
                std::string raw_keys, obs_keys, if_keys;
                for (const auto& [k, v] : obs.raw_observed) {
                    if (!raw_keys.empty()) raw_keys += ",";
                    raw_keys += k;
                }
                for (const auto& [k, v] : obs.observed) {
                    if (k.find('_') == std::string::npos) {
                        if (!obs_keys.empty()) obs_keys += ",";
                        obs_keys += k;
                    } else {
                        if (!if_keys.empty()) if_keys += ",";
                        if_keys += k;
                    }
                }
                logger->debug("[DIAG MW src] sat={} elev={:.1f} has_if={} "
                              "obs.observed_raw=[{}] obs.observed_if=[{}] "
                              "raw_observed=[{}]",
                              obs.satellite, obs.elevation, has_if ? 1 : 0,
                              obs_keys, if_keys, raw_keys);
                for (const auto& ifp : if_pairs) {
                    logger->debug("[DIAG MW if_pair] sat={} phase_if={} code_if={}",
                                  obs.satellite, ifp.phase_key, ifp.code_key);
                }
                const auto uc_pairs_diag = collectUncombinedPairs(obs);
                for (const auto& p : uc_pairs_diag) {
                    logger->debug("[DIAG MW uc_pair] sat={} {}/{} ({}/{})",
                                  obs.satellite, p.phase1, p.phase2, p.code1, p.code2);
                }
            }
            */

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
                                             NaN, /*cross_sat_floor_m=*/NaN, false, true);
                        }
                    }
                    // Same LLI band memory (§14.11) on the combined path.  Here
                    // the arcs are erased below, so the next sample starts a
                    // fresh, immature arc that the maturity gate already refuses;
                    // the stamp is recorded for symmetry and for the basis
                    // string, not because this path needs it.
                    {
                        auto& bands = cur_lli_bands_[obs.satellite];
                        for (const auto& [code, lli] : obs.lli_flags)
                            if (!code.empty() && code[0] == 'L' && lli != 0)
                                bands.insert(code);
                    }
                    for (auto it = mw_stats_.begin(); it != mw_stats_.end(); )
                        it = (it->first.rfind(obs.satellite, 0) == 0) ? mw_stats_.erase(it) : std::next(it);
                    continue;
                }

                // Track which band-pairs already have IF labels (e.g. "1_2" from "L1W_L2W")
                // so that supplementary uncombined MW pairs can be added for bands not
                // covered by the combined signal_mode (e.g. L1-L5, L2-L5 when signal_mode=dual).
                std::set<std::string> if_band_pairs;

                for (const auto& ifp : if_pairs) {
                    std::string s1, s2, c1, c2;
                    if (!splitIfLabel(ifp.phase_key, s1, s2)) {
                        logger->debug("[DIAG MW skip] sat={} if_phase={}: splitIfLabel(phase) failed",
                                      obs.satellite, ifp.phase_key);
                        continue;
                    }
                    if (!splitIfLabel(ifp.code_key, c1, c2)) {
                        logger->debug("[DIAG MW skip] sat={} if_code={}: splitIfLabel(code) failed",
                                      obs.satellite, ifp.code_key);
                        continue;
                    }

                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, s1, s2);
                    if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) {
                        logger->debug("[DIAG MW skip] sat={} {}/{}: invalid lambda_wl={} (combined IF path)",
                                      obs.satellite, s1, s2, lambda_wl);
                        continue;
                    }
                    const double mw_raw = computeMwMeters(obs, s1, s2, c1, c2);
                    if (!std::isfinite(mw_raw)) {
                        logger->debug("[DIAG MW skip] sat={} {}/{}: NaN mw_raw (combined IF path) "
                                      "— check raw_observed has all of L:{},{} C:{},{}",
                                      obs.satellite, s1, s2, s1, s2, c1, c2);
                        continue;
                    }

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
                        double slip_wl_f = std::numeric_limits<double>::quiet_NaN();
                        MwSampleEvidence ev;
                        if (processMwSample(obs, stats_key, mw_raw, lambda_wl, s1, s2,
                                            /*bia_corrected=*/false, /*sat_mw_bias=*/0.0,
                                            &slip_cyc, &wl_known, &slip_frac, &slip_wl_f,
                                            /*pivot_sat=*/{}, /*pivot_zd_slip_in=*/0,
                                            /*pivot_zd_known_in=*/true, c1, c2, &ev)) {
                            results.push_back({ obs.satellite + "_" + ifp.phase_key,
                                                slip_cyc, wl_known, lambda_wl,
                                                s1, s2, obs.satellite, slip_frac, slip_wl_f,
                                                ev.evidence_ratio, ev.arc_epochs,
                                                /*bia_corrected=*/false });
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
                        if (if_band_pairs.count(bkey)) {
                            logger->debug("[DIAG MW skip] sat={} {}/{}: bands {} already covered by combined path",
                                          obs.satellite, pair.phase1, pair.phase2, bkey);
                            continue;
                        }
                    }

                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, pair.phase1, pair.phase2);
                    if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) {
                        logger->debug("[DIAG MW skip] sat={} {}/{}: invalid lambda_wl={} (supplementary uc path)",
                                      obs.satellite, pair.phase1, pair.phase2, lambda_wl);
                        continue;
                    }
                    const double mw_raw = computeMwMeters(obs, pair.phase1, pair.phase2, pair.code1, pair.code2);
                    if (!std::isfinite(mw_raw)) {
                        logger->debug("[DIAG MW skip] sat={} {}/{}: NaN mw_raw (supplementary uc path) "
                                      "— check raw_observed has all of L:{},{} C:{},{}",
                                      obs.satellite, pair.phase1, pair.phase2,
                                      pair.phase1, pair.phase2, pair.code1, pair.code2);
                        continue;
                    }

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
                        double slip_wl_f = std::numeric_limits<double>::quiet_NaN();
                        MwSampleEvidence ev;
                        if (processMwSample(obs, stats_key, mw_raw, lambda_wl, pair.phase1, pair.phase2,
                                            /*bia_corrected=*/false, /*sat_mw_bias=*/0.0,
                                            &slip_cyc, &wl_known, &slip_frac, &slip_wl_f,
                                            /*pivot_sat=*/{}, /*pivot_zd_slip_in=*/0,
                                            /*pivot_zd_known_in=*/true, pair.code1, pair.code2, &ev)) {
                            // Supplementary pairs produce per-signal reset entries
                            // with full signal info for the multi-frequency solver.
                            results.push_back({ obs.satellite + "_" + pair.phase1,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac, slip_wl_f,
                                                ev.evidence_ratio, ev.arc_epochs,
                                                /*bia_corrected=*/false });
                            results.push_back({ obs.satellite + "_" + pair.phase2,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac, slip_wl_f,
                                                ev.evidence_ratio, ev.arc_epochs,
                                                /*bia_corrected=*/false });
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
                    // Remember which bands the receiver flagged (§14.11), so the
                    // next epoch's stamp can mark the pairs that contain one of
                    // them as LLI-shadowed.  Band-scoped on purpose: a lane made
                    // only of unflagged bands is untouched by the receiver's
                    // report and its silence still carries information, which
                    // keeps the correct late narrowings available.
                    {
                        auto& bands = cur_lli_bands_[obs.satellite];
                        for (const auto& [code, lli] : obs.lli_flags)
                            if (!code.empty() && code[0] == 'L'
                                && code.find('_') == std::string::npos && lli != 0)
                                bands.insert(code);
                        // Readout (§14.10): the LLI epoch has no MWDEC line of its
                        // own, so log it explicitly; otherwise the gap reads as
                        // missing data.
                        std::string blist;
                        for (const auto& b : bands) {
                            if (!blist.empty()) blist += ";";
                            blist += b;
                        }
                        logger->debug("MWSKIP sta={} {} t={:.1f} lli_bands=[{}] — EVERY pair's "
                                      "sample WITHHELD this epoch (MW arc preserved, "
                                      "count not reset); next epoch's first sample is "
                                      "LLI-shadowed on every pair containing one of them.",
                                      station_label_, obs.satellite, obs.time.tow, blist);
                    }
                    // MW stats are preserved across LLI on purpose.  The
                    // running MW = (φ_1 − φ_2)/λ_WL − code-NL/λ_WL is a
                    // function of raw observations only, independent of
                    // the filter ambiguity state, so a short LLI gap does
                    // not invalidate the running statistics.  When
                    // observations resume:
                    //  - a real cycle slip jumps MW by ΔN_1 − ΔN_2 cycles
                    //    and trips the threshold check in processMwSample
                    //    on the very next sample (which then resets its
                    //    own per-arc stats with full slip-integer info);
                    //    a band-common slip (ΔN_1 = ΔN_2) leaves MW
                    //    unchanged, which is why the next sample is
                    //    marked LLI-shadowed (above);
                    //  - a spurious / no-slip LLI leaves MW unchanged, so
                    //    the running mean keeps its converged precision.
                    // Wiping the per-arc statistics here would discard that
                    // precision in the no-slip case and be redundant in the
                    // real-slip case.  The LLI ambiguity reset above remains
                    // a conservative second safeguard on the *filter* side;
                    // MW keeps its observation-domain history.
                    continue;
                }

                for (const auto& pair : uc_pairs) {
                    const double lambda_wl = computeWideLaneWavelengthMeters(obs, pair.phase1, pair.phase2);
                    if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) {
                        logger->debug("[DIAG MW skip] sat={} {}/{}: invalid lambda_wl={} (uncombined path)",
                                      obs.satellite, pair.phase1, pair.phase2, lambda_wl);
                        continue;
                    }
                    const double mw_raw = computeMwMeters(obs, pair.phase1, pair.phase2, pair.code1, pair.code2);
                    if (!std::isfinite(mw_raw)) {
                        logger->debug("[DIAG MW skip] sat={} {}/{}: NaN mw_raw (uncombined path) "
                                      "— check raw_observed has all of L:{},{} C:{},{}",
                                      obs.satellite, pair.phase1, pair.phase2,
                                      pair.phase1, pair.phase2, pair.code1, pair.code2);
                        continue;
                    }

                    // Compute satellite BIA MW bias
                    double sat_bias = 0.0;
                    const bool has_bia = computeSatMwBias(obs, pair.phase1, pair.phase2,
                                                          pair.code1, pair.code2, sat_bias);
                    const double mw_bia = has_bia ? (mw_raw - sat_bias) : mw_raw;

                    logger->debug("[DIAG MW pair] sat={} {}/{}: lambda_wl={:.3f} m, mw_raw={:.3f} m, "
                                  "has_bia={}, path={} -> sd_groups[{}]",
                                  obs.satellite, pair.phase1, pair.phase2, lambda_wl, mw_raw,
                                  has_bia ? 1 : 0,
                                  has_bia ? "BSSD-pass2" : "direct-no-BIA",
                                  has_bia ? (std::string{ obs.satellite[0] } + "_" + pair.phase1 + "_" + pair.phase2) : std::string{ "<none>" });

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
                        double slip_wl_f = std::numeric_limits<double>::quiet_NaN();
                        MwSampleEvidence ev;
                        if (processMwSample(obs, stats_key, mw_raw, lambda_wl, pair.phase1, pair.phase2,
                                            /*bia_corrected=*/false, /*sat_mw_bias=*/0.0,
                                            &slip_cyc, &wl_known, &slip_frac, &slip_wl_f,
                                            /*pivot_sat=*/{}, /*pivot_zd_slip_in=*/0,
                                            /*pivot_zd_known_in=*/true, pair.code1, pair.code2, &ev)) {
                            results.push_back({ obs.satellite + "_" + pair.phase1,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac, slip_wl_f,
                                                ev.evidence_ratio, ev.arc_epochs,
                                                /*bia_corrected=*/false });
                            results.push_back({ obs.satellite + "_" + pair.phase2,
                                                slip_cyc, wl_known, lambda_wl,
                                                pair.phase1, pair.phase2, obs.satellite, slip_frac, slip_wl_f,
                                                ev.evidence_ratio, ev.arc_epochs,
                                                /*bia_corrected=*/false });
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
        // This runs for both combined (PPP) and uncombined (PPP-RTK) paths.
        // The MW observable is always formed from raw RINEX signals and is
        // independent of the filter's measurement model parameterization.
        // -----------------------------------------------------------------
        // [DIAG] Pass 2 entry summary: which (cons, band-pair) groups made
        // it from Pass 1, and how many sats per group.  A group with 1
        // satellite still gets processed (the lone sat is treated as
        // pivot with bia_corrected=false).  A missing group means Pass 1
        // dropped every candidate, usually because computeMwMeters returned
        // NaN or computeSatMwBias lacked coverage.
        if (logger->should_log(spdlog::level::debug)) {
            std::string groups_summary;
            for (const auto& [gk, ents] : sd_groups) {
                std::string sat_list;
                for (const auto& [s, e] : ents) {
                    if (!sat_list.empty()) sat_list += ",";
                    sat_list += s;
                    sat_list += e.is_combined ? "(IF)" : "(uc)";
                }
                if (!groups_summary.empty()) groups_summary += " | ";
                groups_summary += gk + "={" + sat_list + "}";
            }
            //logger->debug("[DIAG MW pass2 groups] {}", groups_summary); // debug check
        }

        for (auto& [group_key, sat_entries] : sd_groups) {
            if (sat_entries.empty()) continue;

            // Select pivot: highest elevation satellite, but skip any sat
            // flagged via notifyPivotForcedHandover.  Falls back to the
            // overall highest-elevation candidate if every sat in this
            // group is deprioritized (single-sat groups, or a
            // constellation where every sat is flagged this epoch).
            std::string best_pivot;
            double      best_elev    = -999.0;
            std::string fallback_pivot;
            double      fallback_elev = -999.0;
            for (const auto& [sat, entry] : sat_entries) {
                if (entry.obs->elevation > fallback_elev) {
                    fallback_elev = entry.obs->elevation;
                    fallback_pivot = sat;
                }
                if (deprioritized_pivot_sats_.count(sat)) continue;
                if (entry.obs->elevation > best_elev) {
                    best_elev = entry.obs->elevation;
                    best_pivot = sat;
                }
            }
            if (best_pivot.empty()) {
                if (fallback_pivot.empty()) continue;
                best_pivot = fallback_pivot;
            }

            // Detect pivot change.  Rather than erasing every locked arc
            // (which floats the WLs through every handover), S-transform
            // each non-pivot arc to be referenced against the new pivot:
            //
            //     SD_new(sat) = SD_old(sat) − SD_old(new_pivot)
            //     int_new(sat) = int_old(sat) − int_old(new_pivot)
            //
            // where SD_old(new_pivot) is the new pivot's running mean
            // under the prior referencing (it was a non-pivot then).
            // The transform is valid only when the new pivot was
            // BIA-corrected and integer-locked at the prior epoch, i.e.
            // a reliable integer SD reference exists.  When that
            // precondition fails, the bia_corrected non-pivot arcs are
            // erased and restart (the legacy behavior).
            //
            // Statistics on epoch-to-epoch differences (diff_window, M2,
            // last_dev, last_sigma) are unchanged by the SD-reference
            // change to first order, because the new and old pivot have
            // similar BIA-corrected MW noise levels.  The transform below
            // shifts the level quantities (mean, mean_lag, prev_mw,
            // the local ring, a demoted-level stash),
            // re-references prev_diff, shifts the locked integer
            // (rounded_wl) and drops a pending TD confirmation.
            auto prev_pivot_it = sd_pivot_sat_.find(group_key);
            if (prev_pivot_it != sd_pivot_sat_.end() && prev_pivot_it->second != best_pivot) {
                const std::string old_pivot = prev_pivot_it->second;
                const std::string& new_pivot = best_pivot;

                const auto& new_piv_entry = sat_entries.at(new_pivot);
                const std::string new_piv_old_key =
                    new_pivot + "_" + new_piv_entry.pair.phase1 + "_"
                    + new_piv_entry.pair.phase2;
                auto new_piv_arc_it = mw_stats_.find(new_piv_old_key);

                // Capture reference values before any erase below.
                bool   can_s_transform = false;
                double piv_old_mean    = 0.0;
                double piv_old_prev_mw = 0.0;
                double piv_old_prev_diff =
                    std::numeric_limits<double>::quiet_NaN();
                int    piv_old_int     = 0;
                if (new_piv_arc_it != mw_stats_.end()
                    && new_piv_arc_it->second.bia_corrected
                    && new_piv_arc_it->second.wl_locked) {
                    can_s_transform = true;
                    piv_old_mean    = new_piv_arc_it->second.mean;
                    piv_old_prev_mw = new_piv_arc_it->second.prev_mw;
                    piv_old_prev_diff = new_piv_arc_it->second.prev_diff;
                    piv_old_int     = new_piv_arc_it->second.rounded_wl;
                }

                // In-house bias S-transform: when the new pivot had a stored
                // bias under the old frame, every other sat's stored bias
                // re-references by subtracting it.  Without an anchor the
                // affected entries are erased; re-bootstrapping is safer
                // than applying a stale-frame bias.
                auto piv_bias_it = sat_pair_bias_.find(new_piv_old_key);
                bool   can_s_transform_bias = false;
                double piv_old_bias = 0.0;
                if (piv_bias_it != sat_pair_bias_.end()
                    && piv_bias_it->second.commit_count > 0) {
                    can_s_transform_bias = true;
                    piv_old_bias = piv_bias_it->second.bias_m;
                }

                int n_xform  = 0;
                int n_erased = 0;
                int n_bias_xform  = 0;
                int n_bias_erased = 0;
                int n_old_pivot_dropped = 0;
                for (const auto& [sat, entry] : sat_entries) {
                    if (sat == new_pivot) continue;
                    const std::string stats_key =
                        sat + "_" + entry.pair.phase1 + "_" + entry.pair.phase2;

                    // The old pivot, now becoming a non-pivot, holds a
                    // ZD-frame arc (bia_corrected=false) whose Hatch mean
                    // still contains the receiver WL bias and is not
                    // comparable to the SD-frame samples that start
                    // arriving this epoch.  Without an estimate of the
                    // receiver bias it cannot be S-transformed into the new
                    // SD frame; left alone, the next sample would appear as
                    // a multi-cycle pseudo-slip against the stale mean and
                    // trigger a spurious targeted reset/repair.  Drop the
                    // arc and any stale bias entry; a clean SD arc restarts
                    // against the new pivot at the next sample.
                    if (sat == old_pivot) {
                        auto arc_it_op = mw_stats_.find(stats_key);
                        if (arc_it_op != mw_stats_.end()) {
                            mw_stats_.erase(arc_it_op);
                            ++n_old_pivot_dropped;
                        }
                        auto bias_it_op = sat_pair_bias_.find(stats_key);
                        if (bias_it_op != sat_pair_bias_.end()) {
                            sat_pair_bias_.erase(bias_it_op);
                        }
                        continue;
                    }

                    auto arc_it = mw_stats_.find(stats_key);
                    if (arc_it != mw_stats_.end()) {
                        auto& arc = arc_it->second;
                        if (!arc.bia_corrected) {
                            // building; SD ref doesn't matter yet
                        } else if (can_s_transform) {
                            arc.mean     -= piv_old_mean;
                            arc.mean_lag -= piv_old_mean;   // lagged snapshot rides the same frame (MwArcStats doc)
                            arc.prev_mw  -= piv_old_prev_mw;
                            // A demoted-level stash for this arc lives in
                            // the same SD frame; shift it by the same
                            // constant so the across-gap difference
                            // (new mean − stash) stays frame-invariant.
                            if (auto dl = demoted_levels_.find(stats_key);
                                dl != demoted_levels_.end())
                                dl->second.pre_level_m -= piv_old_mean;
                            // The local raw-MW ring holds old-SD-frame
                            // samples; shift by the same constant as the
                            // mean, or the epoch after every handover each
                            // surviving arc would show a local deviation of
                            // piv_old_mean (typically multiple WL cycles),
                            // a false slip across the whole constellation.
                            shiftLocalWindow(arc, -piv_old_mean);
                            // prev_diff (TD1[k−1]) must be re-referenced to
                            // the new frame too: TD1_new = TD1_old −
                            // TD1_old(new pivot), exactly, because the
                            // contribution of the shared old-pivot samples
                            // (slip-sized after a slip-forced handover, not
                            // just epoch noise) cancels identically.  Left
                            // untouched, the next epoch's TD2 on every
                            // surviving arc would read +dN_oldpivot·λ, the
                            // constellation-wide false slip this transform
                            // prevents, recreated in the TD2 statistic.  NaN
                            // (either side) degrades to the documented
                            // fresh-arc TD1 fallback for one epoch.
                            arc.prev_diff =
                                (std::isfinite(arc.prev_diff)
                                 && std::isfinite(piv_old_prev_diff))
                                    ? arc.prev_diff - piv_old_prev_diff
                                    : std::numeric_limits<double>::quiet_NaN();
                            // A pending TD confirmation stores old-frame
                            // level quantities (jump / pre-level); after
                            // the frame change its hypotheses are
                            // meaningless, so drop it (the lock survives;
                            // if the flag was a real step, drift demotion
                            // remains the backstop for this rare case).
                            arc.td_confirm_pending = false;
                            if (arc.wl_locked) {
                                arc.rounded_wl -= piv_old_int;
                            }
                            ++n_xform;
                        } else {
                            mw_stats_.erase(arc_it);
                            ++n_erased;
                        }
                    }

                    auto bias_it = sat_pair_bias_.find(stats_key);
                    if (bias_it != sat_pair_bias_.end()
                        && bias_it->second.commit_count > 0) {
                        if (can_s_transform_bias) {
                            bias_it->second.bias_m -= piv_old_bias;
                            ++n_bias_xform;
                        } else {
                            sat_pair_bias_.erase(bias_it);
                            ++n_bias_erased;
                        }
                    }
                }

                // Drop the new pivot's own old (non-pivot) arc.  It'll be
                // re-initialized this epoch as the pivot (BIA-corrected,
                // un-differenced, no integer lock).
                if (new_piv_arc_it != mw_stats_.end()) mw_stats_.erase(new_piv_arc_it);

                // Drop the new pivot's own stored bias entry: it is the
                // SD anchor now (zero by definition), with no bias to track.
                if (piv_bias_it != sat_pair_bias_.end()) {
                    sat_pair_bias_.erase(piv_bias_it);
                }

                if (can_s_transform) {
                    logger->info("BSSD MW pivot change for {}: {} -> {}: "
                                 "S-transformed {} non-pivot arcs, dropped {} stale old-pivot arc "
                                 "(int shift {} cyc, mean shift {:.4f} m); locks preserved",
                                 group_key, old_pivot, new_pivot,
                                 n_xform, n_old_pivot_dropped,
                                 piv_old_int, piv_old_mean);
                } else {
                    logger->info("BSSD MW pivot change for {}: {} -> {}: "
                                 "new pivot not integer-locked; erased {} non-pivot arcs, "
                                 "dropped {} stale old-pivot arc (no S-transform possible)",
                                 group_key, old_pivot, new_pivot,
                                 n_erased, n_old_pivot_dropped);
                }
                if (n_bias_xform > 0 || n_bias_erased > 0) {
                    logger->info("In-house bias re-reference for {}: "
                                 "xform={}, erased={} (anchor={}, shift={:.4f} m)",
                                 group_key, n_bias_xform, n_bias_erased,
                                 can_s_transform_bias ? "available" : "missing",
                                 piv_old_bias);
                }
            }
            sd_pivot_sat_[group_key] = best_pivot;

            const auto& pivot_entry = sat_entries.at(best_pivot);

            // --- Pivot first: its undifferenced (ZD) slip anchors the frame ---
            // The non-pivot arcs below are single-differenced against this
            // pivot (sd_mw = MW_sat - MW_pivot), so processMwSample sees the SD
            // jump dN_sat - dN_pivot.  Downstream consumers expect each result's
            // slip_wl_cycles to be the satellite's own undifferenced (ZD) slip,
            // so the pivot's ZD slip is captured here and added back below:
            // dN_sat = (dN_sat - dN_pivot) + dN_pivot.  When the pivot's own
            // integer cannot be resolved the conversion is impossible, so the
            // non-pivot slips are flagged wl_known=false (-> reset) rather than
            // emitting an SD value under a ZD contract.
            int    pivot_zd_slip  = 0;     // dN_pivot, WL cycles (0 if pivot clean)
            bool   pivot_zd_known = true;  // pivot clean => dN_pivot = 0, known
            {
                const std::string piv_key = best_pivot + "_"
                    + pivot_entry.pair.phase1 + "_" + pivot_entry.pair.phase2;
                int    pcyc   = 0;
                bool   pknown = false;
                double pfrac  = 0.5;
                // Pivot uses undifferenced BIA-corrected MW: it cannot integer-
                // lock (receiver bias still present), but the jump-based estimate
                // still resolves the slip and that estimate is already ZD.
                double pfloat = std::numeric_limits<double>::quiet_NaN();
                MwSampleEvidence pev;
                if (processMwSample(*pivot_entry.obs, piv_key, pivot_entry.mw_bia,
                                    pivot_entry.lambda_wl, pivot_entry.pair.phase1,
                                    pivot_entry.pair.phase2, /*bia_corrected=*/false,
                                    pivot_entry.sat_mw_bias, &pcyc, &pknown, &pfrac,
                                    &pfloat, best_pivot, /*pivot_zd_slip_in=*/0,
                                    /*pivot_zd_known_in=*/true,
                                    pivot_entry.pair.code1, pivot_entry.pair.code2,
                                    &pev)) {
                    pivot_zd_slip  = pcyc;
                    pivot_zd_known = pknown;
                    // Lane class: the sample carried bia_corrected=false into
                    // processMwSample (the pivot cannot integer-lock, because
                    // the receiver bias is still in it), but the value is the
                    // BIA-corrected mw_bia and this arc is the S-transform
                    // reference of its group.  A pivot re-reference belongs to
                    // the false-alarm population the evidence rule must refuse,
                    // so the reported class is the lane's, not the call's: true.
                    if (pivot_entry.is_combined) {
                        results.push_back({ best_pivot + "_" + pivot_entry.if_phase_key,
                                            pcyc, pknown, pivot_entry.lambda_wl,
                                            pivot_entry.pair.phase1, pivot_entry.pair.phase2,
                                            best_pivot, pfrac, pfloat,
                                            pev.evidence_ratio, pev.arc_epochs,
                                            /*bia_corrected=*/true });
                    } else {
                        results.push_back({ best_pivot + "_" + pivot_entry.pair.phase1,
                                            pcyc, pknown, pivot_entry.lambda_wl,
                                            pivot_entry.pair.phase1, pivot_entry.pair.phase2,
                                            best_pivot, pfrac, pfloat,
                                            pev.evidence_ratio, pev.arc_epochs,
                                            /*bia_corrected=*/true });
                        results.push_back({ best_pivot + "_" + pivot_entry.pair.phase2,
                                            pcyc, pknown, pivot_entry.lambda_wl,
                                            pivot_entry.pair.phase1, pivot_entry.pair.phase2,
                                            best_pivot, pfrac, pfloat,
                                            pev.evidence_ratio, pev.arc_epochs,
                                            /*bia_corrected=*/true });
                    }
                    logger->warn("Scheduling reset for {} (pivot, wl_known={}, slip_wl={} ZD)",
                                 pivot_entry.is_combined
                                     ? (best_pivot + "_" + pivot_entry.if_phase_key)
                                     : (best_pivot + "_" + pivot_entry.pair.phase1 + "+"
                                        + pivot_entry.pair.phase2),
                                 pknown, pcyc);
                }
            }

            for (const auto& [sat, entry] : sat_entries) {
                if (sat == best_pivot) continue;  // ZD slip handled before the loop

                const std::string stats_key =
                    sat + "_" + entry.pair.phase1 + "_" + entry.pair.phase2;

                // Non-pivot: single-difference MW to cancel the receiver WL bias.
                //   SD_MW = (N_WL_sat - N_WL_pivot) * lambda_wl + noise
                const double sd_mw = entry.mw_bia - pivot_entry.mw_bia;

                int slip_cyc = 0;
                bool wl_known = false;
                double slip_frac = 0.5;
                double slip_wl_f = std::numeric_limits<double>::quiet_NaN();
                MwSampleEvidence ev;
                if (processMwSample(*entry.obs, stats_key, sd_mw, entry.lambda_wl,
                                    entry.pair.phase1, entry.pair.phase2,
                                    /*bia_corrected=*/true, entry.sat_mw_bias,
                                    &slip_cyc, &wl_known, &slip_frac, &slip_wl_f,
                                    best_pivot,
                                    /*pivot_zd_slip_in=*/pivot_zd_slip,
                                    /*pivot_zd_known_in=*/pivot_zd_known,
                                    entry.pair.code1, entry.pair.code2, &ev)) {
                    // Re-reference the SD jump to this satellite's own ZD slip:
                    // dN_sat = SD + dN_pivot.  Adding an integer leaves the
                    // rounding fraction (slip_frac) unchanged.  Unusable if the
                    // pivot integer was unknown -> consumer resets.
                    slip_cyc = slip_cyc + pivot_zd_slip;
                    wl_known = wl_known && pivot_zd_known;
                    // The float re-references identically, and becomes
                    // unanchored (NaN) when the pivot's own ZD slip is
                    // unresolved, like wl_known.
                    slip_wl_f = (pivot_zd_known && std::isfinite(slip_wl_f))
                        ? slip_wl_f + static_cast<double>(pivot_zd_slip)
                        : std::numeric_limits<double>::quiet_NaN();
                    // Lane class true: this arc is single-differenced against the
                    // detector's MW pivot, so its "slip" population contains the
                    // pivot's own S-transform re-references (observed as many
                    // satellites at one station stepping in the same epoch).  The
                    // evidence rule refuses this class outright.
                    if (entry.is_combined) {
                        results.push_back({ sat + "_" + entry.if_phase_key,
                                            slip_cyc, wl_known, entry.lambda_wl,
                                            entry.pair.phase1, entry.pair.phase2, sat, slip_frac,
                                            slip_wl_f, ev.evidence_ratio, ev.arc_epochs,
                                            /*bia_corrected=*/true });
                    } else {
                        results.push_back({ sat + "_" + entry.pair.phase1,
                                            slip_cyc, wl_known, entry.lambda_wl,
                                            entry.pair.phase1, entry.pair.phase2, sat, slip_frac,
                                            slip_wl_f, ev.evidence_ratio, ev.arc_epochs,
                                            /*bia_corrected=*/true });
                        results.push_back({ sat + "_" + entry.pair.phase2,
                                            slip_cyc, wl_known, entry.lambda_wl,
                                            entry.pair.phase1, entry.pair.phase2, sat, slip_frac,
                                            slip_wl_f, ev.evidence_ratio, ev.arc_epochs,
                                            /*bia_corrected=*/true });
                    }
                    logger->warn("Scheduling reset for {} (wl_known={}, slip_wl={} ZD)",
                                 entry.is_combined ? (sat + "_" + entry.if_phase_key)
                                                   : (sat + "_" + entry.pair.phase1 + "+" + entry.pair.phase2),
                                 wl_known, slip_cyc);
                }
            }
        }

        // Consume the one-shot deprioritization hints.  Each call to
        // notifyPivotForcedHandover applies to exactly one downstream
        // detectLargeMwResets pass; the upstream caller must re-issue
        // the hint each epoch the deprioritization should hold.
        deprioritized_pivot_sats_.clear();

        // Finalize the cross-sat σ tiers (per constellation × band-pair,
        // per constellation, global) from this epoch's pool of per-sat
        // deviations.  Next epoch's computeAdaptiveThreshold picks up the
        // result via cross_sat_sigma_m_ and the coarser tiers.  (The
        // one-epoch lag is intentional: the current epoch's pool is not
        // fully assembled until every sat has been processed.)
        finalizeCrossSatSigmas();

        // Per-epoch flush so the trace survives non-graceful termination
        // (console close, second Ctrl-C, crash).  Nothing else flushes this
        // stream (RealTimeEngine::flush() only drains pending observations),
        // so without this the tail of the run stays in the stream buffer and
        // is lost with the process, truncating the file at a buffer boundary.
        // At ≤10 Hz the flush cost is negligible.
        if (mwwl_trace_.is_open()) mwwl_trace_.flush();

        return results;
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

    // =================================================================
    // External reset
    // =================================================================

    void MwSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("mw_slip_detector");

        // Helper: roll back one arc using its pre-epoch snapshot if
        // present (preserves accumulated noise statistics, breaks the
        // lock), or full-erase if no snapshot exists (legacy behavior).
        // Returns true if the arc was rolled back, false if erased.
        auto rollback_or_erase = [&](decltype(mw_stats_.begin())& cit) -> bool {
            const std::string key = cit->first;
            const size_t arc_n   = cit->second.count;
            auto snap_it = arc_snapshots_.find(key);
            if (snap_it != arc_snapshots_.end()) {
                cit->second = snap_it->second;
                cit->second.wl_locked  = false;
                cit->second.rounded_wl = 0;
                arc_snapshots_.erase(snap_it);
                logger->debug("Rolled back MW arc {} to pre-epoch snapshot "
                              "(external reset, arc_n was {}, post-rollback {}; "
                              "lock broken)",
                              key, arc_n, cit->second.count);
                ++cit;
                return true;
            }
            logger->debug("Cleared MW arc history for {} "
                          "(external reset, arc_n={}; no snapshot available)",
                          key, arc_n);
            cit = mw_stats_.erase(cit);
            return false;
        };

        auto it = mw_stats_.find(satellite);
        if (it != mw_stats_.end()) {
            rollback_or_erase(it);
        }

        const std::string prefix = satellite + "_";
        for (auto cit = mw_stats_.begin(); cit != mw_stats_.end(); ) {
            if (cit->first.rfind(prefix, 0) == 0) {
                rollback_or_erase(cit);
            } else {
                ++cit;
            }
        }

        // An external reset invalidates any demoted-level stash for the
        // sat: a rolled-back arc's mean reverts to the pre-event level, so
        // (mean − stash) would read ~0 with full rollback confidence, a
        // convincing but wrong "no slip" settle estimate.
        demoted_levels_.erase(satellite);
        for (auto dit = demoted_levels_.begin(); dit != demoted_levels_.end(); )
            if (dit->first.rfind(prefix, 0) == 0) dit = demoted_levels_.erase(dit);
            else ++dit;

        // If this satellite was an SD pivot, remove the pivot entry so the
        // pivot is re-selected next epoch.  The pivot-change handling in
        // detectLargeMwResets (S-transform or erase of the group's SD arcs)
        // needs a stored previous pivot, so it does not run for this group
        // next epoch; the dependent SD arcs are not reset here.
        for (auto pit = sd_pivot_sat_.begin(); pit != sd_pivot_sat_.end(); )
            if (pit->second == satellite) {
                logger->debug("Cleared SD pivot {} for group {} (external satellite reset)",
                    satellite, pit->first);
                pit = sd_pivot_sat_.erase(pit);
            } else {
                ++pit;
            }
    }

    void MwSlipDetector::resetSatelliteSignal(const std::string& satellite,
                                                const std::string& signal) {
        if (satellite.empty() || signal.empty()) return;
        auto logger = log_manager_.getLogger("mw_slip_detector");

        // Arc keys are formatted "<sat>_<sig1>_<sig2>".  Rollback (via
        // pre-epoch snapshot) or erase (fallback) entries where either
        // signal in the pair matches the slipped signal; preserve
        // entries on signal pairs that don't involve it.
        const std::string prefix = satellite + "_";
        int n_rolled = 0, n_erased = 0;
        for (auto cit = mw_stats_.begin(); cit != mw_stats_.end(); ) {
            if (cit->first.rfind(prefix, 0) != 0) { ++cit; continue; }
            const std::string rest = cit->first.substr(prefix.size());
            const auto us = rest.find('_');
            bool involves = false;
            if (us != std::string::npos) {
                const std::string sig1 = rest.substr(0, us);
                const std::string sig2 = rest.substr(us + 1);
                involves = (sig1 == signal || sig2 == signal);
            } else {
                // No "<sig1>_<sig2>" structure (e.g. raw signal key): match exactly.
                involves = (rest == signal);
            }
            if (involves) {
                const std::string key = cit->first;
                const size_t arc_n   = cit->second.count;
                auto snap_it = arc_snapshots_.find(key);
                if (snap_it != arc_snapshots_.end()) {
                    cit->second = snap_it->second;
                    cit->second.wl_locked  = false;
                    cit->second.rounded_wl = 0;
                    arc_snapshots_.erase(snap_it);
                    logger->debug("Rolled back MW arc {} to pre-epoch snapshot "
                                  "(per-signal reset, signal={}, arc_n was {}, "
                                  "post-rollback {}; lock broken)",
                                  key, signal, arc_n, cit->second.count);
                    ++cit;
                    ++n_rolled;
                } else {
                    logger->debug("Cleared MW arc history for {} (per-signal reset, "
                                  "signal={}, arc_n={}; no snapshot available)",
                                  key, signal, arc_n);
                    cit = mw_stats_.erase(cit);
                    ++n_erased;
                }
            } else {
                ++cit;
            }
        }

        if (n_rolled + n_erased > 0) {
            logger->info("MW per-signal reset {} {}: rolled back {} arc(s), "
                         "erased {} (no snapshot); arcs not involving this "
                         "signal preserved",
                         satellite, signal, n_rolled, n_erased);
        }

        // Invalidate demoted-level stashes touching this signal (same
        // rationale as resetSatellite: a rollback corrupts the settle
        // estimate's post-restart mean).
        for (auto dit = demoted_levels_.begin(); dit != demoted_levels_.end(); ) {
            bool involves = false;
            if (dit->first.rfind(prefix, 0) == 0) {
                const std::string rest = dit->first.substr(prefix.size());
                const auto us = rest.find('_');
                involves = (us != std::string::npos)
                    ? (rest.substr(0, us) == signal || rest.substr(us + 1) == signal)
                    : (rest == signal);
            }
            if (involves) dit = demoted_levels_.erase(dit);
            else ++dit;
        }
    }

    void MwSlipDetector::resetAll() {
        auto logger = log_manager_.getLogger("mw_slip_detector");
        const size_t n_arcs   = mw_stats_.size();
        const size_t n_pivots = sd_pivot_sat_.size();
        const size_t n_biases = sat_pair_bias_.size();
        const size_t n_xsat   = cross_sat_sigma_m_.size();
        const size_t n_snaps  = arc_snapshots_.size();
        mw_stats_.clear();
        sd_pivot_sat_.clear();
        sat_pair_bias_.clear();
        cross_sat_sigma_m_.clear();
        cross_sat_sigma_cons_.clear();
        cross_sat_sigma_global_ = std::numeric_limits<double>::quiet_NaN();
        cross_sat_devs_.clear();
        arc_snapshots_.clear();
        demoted_levels_.clear();
        if (n_arcs > 0 || n_pivots > 0 || n_biases > 0 || n_xsat > 0 || n_snaps > 0) {
            logger->info("MW detector resetAll: cleared {} arc(s), {} SD pivot(s), {} stored bias(es), {} cross-sat σ entries, {} arc snapshot(s)",
                         n_arcs, n_pivots, n_biases, n_xsat, n_snaps);
        }
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

    void MwSlipDetector::notifyPivotForcedHandover(const std::string& satellite,
                                                    const std::string& reason) {
        if (satellite.empty()) return;
        auto logger = log_manager_.getLogger("mw_slip_detector");
        const auto [it, inserted] = deprioritized_pivot_sats_.insert(satellite);
        if (inserted) {
            logger->info("MW pivot deprioritization: {} ({}); next pivot "
                         "pick will avoid this satellite if alternatives exist.",
                         satellite, reason);
        }
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