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
        // (σ·w).  Clamped at 10° — matching the elevation mask — so a
        // near-horizon arc can neither blow up the factor (w ≤ 1/sin10° ≈ 5.76)
        // nor hit the sin→0 singularity.  This is a geometric *shape* on top of
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
        // LAGGED Welford variance of the mean = M2_lag / (n_lag(n_lag-1)); NaN below
        // two samples.  All reads below use the lagged snapshot (stats as of the end
        // of the PREVIOUS epoch) — SPEC_mw_wl_card §3.3: the layer's same-epoch read
        // must be independent of the current epoch's observations.
        auto varOfMean = [](const MwArcStats& a) -> double {
            if (a.count_lag < 2) return std::numeric_limits<double>::quiet_NaN();
            const double n = static_cast<double>(a.count_lag);
            return a.M2_lag / (n * (n - 1.0));
        };

        const std::string key = sat + "_" + sig1 + "_" + sig2;
        const auto it = mw_stats_.find(key);
        if (it == mw_stats_.end()) return out;   // no arc on this (sat, signal-pair)
        const MwArcStats& s = it->second;
        out.exists = true;                 // the arc key is present — see MwArcZdMean::exists
        out.bia_corrected = s.bia_corrected;
        // The code channels are a property of THIS arc's observable and are served
        // regardless of maturity (a caller that skips an immature arc still wants to
        // report which channels it looked at).
        out.code1 = s.code1;
        out.code2 = s.code2;
        if (s.count_lag < 2 || !(s.lambda_wl > 0.0)) { out.count = s.count_lag; return out; }

        if (!s.bia_corrected) {
            // Undifferenced already: a no-BIA-fallback arc (raw MW, receiver+satellite
            // bias present — both cancel in a closed cycle) OR the group's MW pivot arc
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
        // cycle the common per-station pivot term cancels EXACTLY across that station's
        // two arcs, so this over-counts the pivot's contribution — a safe (loose) row R.
        out.var_of_mean_m2 = varOfMean(s) + varOfMean(p);
        out.count          = std::min(s.count_lag, p.count_lag);
        out.lambda_wl      = s.lambda_wl;
        out.valid          = std::isfinite(out.mean_m) && std::isfinite(out.var_of_mean_m2);
        return out;
    }

    MwSlipDetector::MwArcZdLast
    MwSlipDetector::arcZdLast(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2) const {
        MwArcZdLast out;
        // WL-3 §3.2: serve the LAGGED raw sample (the previous processed epoch's MW)
        // with the PER-SAMPLE Welford variance — the lane-carry per-epoch increment.
        // Every read below is from the lag snapshot, mirroring arcZdMean's F1 discipline.
        auto varOfSample = [](std::size_t n_lag, double M2_lag) -> double {
            if (n_lag < 2) return std::numeric_limits<double>::quiet_NaN();
            return M2_lag / (static_cast<double>(n_lag) - 1.0);
        };

        const std::string key = sat + "_" + sig1 + "_" + sig2;
        const auto it = mw_stats_.find(key);
        if (it == mw_stats_.end()) return out;
        const MwArcStats& s = it->second;
        out.bia_corrected = s.bia_corrected;
        out.count         = s.count_lag;
        if (s.count_lag < 2 || !(s.lambda_wl > 0.0) || !std::isfinite(s.prev_mw_lag))
            return out;

        if (!s.bia_corrected) {
            // Undifferenced already (no-BIA fallback or the group's MW pivot arc) —
            // ZD-consistent per-(station,sat) value, return directly.
            out.value_m   = s.prev_mw_lag;
            out.var_m2    = varOfSample(s.count_lag, s.M2_lag);
            out.epoch_s   = s.prev_epoch_lag_s;
            out.lambda_wl = s.lambda_wl;
            out.valid     = std::isfinite(out.value_m) && std::isfinite(out.var_m2);
            return out;
        }

        // Non-pivot BIA arc: stored sample = MW_bia(sat) − MW_bia(pivot), formed against
        // the pivot's SAME-epoch raw sample (pass-2 sd_mw) — re-add the pivot's own
        // lagged raw sample to recover MW_bia(sat).  The reconstruction is exact only
        // when both lag stamps name the same epoch; otherwise the arc missed an epoch
        // relative to its pivot and the value would mix epochs — return invalid.
        const std::string group_key =
            std::string(1, sat.empty() ? '?' : sat.front()) + "_" + sig1 + "_" + sig2;
        const auto pit = sd_pivot_sat_.find(group_key);
        if (pit == sd_pivot_sat_.end()) return out;
        const auto pvt = mw_stats_.find(pit->second + "_" + sig1 + "_" + sig2);
        if (pvt == mw_stats_.end() || pvt->second.count_lag < 2) return out;
        const MwArcStats& p = pvt->second;
        if (!std::isfinite(p.prev_mw_lag)) return out;
        if (std::abs(s.prev_epoch_lag_s - p.prev_epoch_lag_s) > 0.1) return out;  // epoch mismatch

        out.value_m   = s.prev_mw_lag + p.prev_mw_lag;
        // Conservative: SD per-sample variance + pivot per-sample variance (the common
        // pivot term cancels in a closed DD cycle, so this over-counts — a loose R).
        out.var_m2    = varOfSample(s.count_lag, s.M2_lag) + varOfSample(p.count_lag, p.M2_lag);
        out.epoch_s   = s.prev_epoch_lag_s;
        out.count     = std::min(s.count_lag, p.count_lag);
        out.lambda_wl = s.lambda_wl;
        out.valid     = std::isfinite(out.value_m) && std::isfinite(out.var_m2);
        return out;
    }

    // =================================================================
    // M5 card WL-2: tri-frequency phase bridge (SPEC §3.4)
    // =================================================================

    MwSlipDetector::MwBridgeMean
    MwSlipDetector::arcBridge(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2,
                              const std::string& sig5) const {
        MwBridgeMean out;
        const std::string key = sat + "_" + sig1 + "_" + sig2 + "_" + sig5;
        const auto it = bridge_stats_.find(key);
        if (it == bridge_stats_.end()) return out;
        const BridgeArcStats& s = it->second;
        // Serve the LAGGED snapshot (state as of the previous epoch's end) so a same-epoch
        // consumer never re-ingests the current observations — the §3.3/§3.4 double-count
        // guard, identical to arcZdMean.
        if (s.count_lag < 2 || !(s.lambda_wl > 0.0) || !(s.lambda_ewl > 0.0)) {
            out.count = s.count_lag;
            return out;
        }
        const double n = static_cast<double>(s.count_lag);
        out.gf_mean_m       = s.mean_lag;
        out.var_of_mean_m2  = s.M2_lag / (n * (n - 1.0));
        out.count           = s.count_lag;
        out.lambda_wl       = s.lambda_wl;
        out.lambda_ewl      = s.lambda_ewl;
        out.iono_coeff      = s.iono_coeff;
        out.valid           = std::isfinite(out.gf_mean_m) && std::isfinite(out.var_of_mean_m2);
        return out;
    }

    MwSlipDetector::MwBridgeLast
    MwSlipDetector::arcBridgeLast(const std::string& sat,
                                  const std::string& sig1,
                                  const std::string& sig2,
                                  const std::string& sig5) const {
        MwBridgeLast out;
        const std::string key = sat + "_" + sig1 + "_" + sig2 + "_" + sig5;
        const auto it = bridge_stats_.find(key);
        if (it == bridge_stats_.end()) return out;
        const BridgeArcStats& s = it->second;
        out.count = s.count_lag;
        // WL-3 §3.2: the LAGGED raw GF_b sample + per-sample Welford variance — the
        // bridge lane's per-epoch increment (once-only; the mean stays decision-side).
        if (s.count_lag < 2 || !(s.lambda_wl > 0.0) || !(s.lambda_ewl > 0.0)
            || !std::isfinite(s.prev_gf_lag))
            return out;
        out.gf_m       = s.prev_gf_lag;
        out.var_m2     = s.M2_lag / (static_cast<double>(s.count_lag) - 1.0);
        out.epoch_s    = s.prev_epoch_lag_s;
        out.lambda_wl  = s.lambda_wl;
        out.lambda_ewl = s.lambda_ewl;
        out.iono_coeff = s.iono_coeff;
        out.valid      = std::isfinite(out.gf_m) && std::isfinite(out.var_m2);
        return out;
    }

    // =================================================================
    // M5 card UWL-1: cold-fix (n=1) DECISION-ONLY accessors (SPEC §3.3)
    // =================================================================

    MwSlipDetector::MwArcZdMean
    MwSlipDetector::arcZdMeanCold(const std::string& sat,
                                  const std::string& sig1,
                                  const std::string& sig2) const {
        MwArcZdMean out;
        // UNLAGGED n ≥ 1 serve for integer DECISIONS only (header contract): the
        // variance of the mean = M2/(n(n−1)) is NaN at n==1 — the CALLER substitutes
        // the a-priori per-sample σ (uwl_cold_sigma_m); the Welford variance takes
        // over from n ≥ 2.  Never feeds a folded row or a lane increment.
        auto varOfMean = [](std::size_t n, double M2) -> double {
            if (n < 2) return std::numeric_limits<double>::quiet_NaN();
            const double dn = static_cast<double>(n);
            return M2 / (dn * (dn - 1.0));
        };

        const std::string key = sat + "_" + sig1 + "_" + sig2;
        const auto it = mw_stats_.find(key);
        if (it == mw_stats_.end()) return out;
        const MwArcStats& s = it->second;
        out.exists        = true;          // same contract as arcZdMean (key present)
        out.bia_corrected = s.bia_corrected;
        out.code1         = s.code1;
        out.code2         = s.code2;
        out.count         = s.count;
        if (s.count < 1 || !(s.lambda_wl > 0.0)) return out;

        if (!s.bia_corrected) {
            // Undifferenced (no-BIA fallback or the group's MW pivot arc) — ZD already.
            out.mean_m         = s.mean;
            out.var_of_mean_m2 = varOfMean(s.count, s.M2);
            out.lambda_wl      = s.lambda_wl;
            out.valid          = std::isfinite(out.mean_m);
            return out;
        }
        // Non-pivot BIA arc: re-add the group pivot's UNLAGGED mean (same ZD
        // reconstruction as arcZdMean; conservative variance sum — NaN when either
        // side is single-sample, covered by the caller's a-priori σ).
        const std::string group_key =
            std::string(1, sat.empty() ? '?' : sat.front()) + "_" + sig1 + "_" + sig2;
        const auto pit = sd_pivot_sat_.find(group_key);
        if (pit == sd_pivot_sat_.end()) return out;
        const auto pvt = mw_stats_.find(pit->second + "_" + sig1 + "_" + sig2);
        if (pvt == mw_stats_.end() || pvt->second.count < 1) return out;
        const MwArcStats& p = pvt->second;
        out.mean_m         = s.mean + p.mean;
        out.var_of_mean_m2 = varOfMean(s.count, s.M2) + varOfMean(p.count, p.M2);
        out.count          = std::min(s.count, p.count);
        out.lambda_wl      = s.lambda_wl;
        out.valid          = std::isfinite(out.mean_m);
        return out;
    }

    MwSlipDetector::MwBridgeMean
    MwSlipDetector::arcBridgeCold(const std::string& sat,
                                  const std::string& sig1,
                                  const std::string& sig2,
                                  const std::string& sig5) const {
        MwBridgeMean out;
        const std::string key = sat + "_" + sig1 + "_" + sig2 + "_" + sig5;
        const auto it = bridge_stats_.find(key);
        if (it == bridge_stats_.end()) return out;
        const BridgeArcStats& s = it->second;
        out.count = s.count;
        // UNLAGGED n ≥ 2 serve (variance-of-mean needs two samples) — one epoch
        // earlier than the lagged arcBridge (count_lag ≥ 2 ⇔ three samples).
        // DECISION-ONLY (header contract); the lane increment stays on arcBridgeLast.
        if (s.count < 2 || !(s.lambda_wl > 0.0) || !(s.lambda_ewl > 0.0)) return out;
        const double n = static_cast<double>(s.count);
        out.gf_mean_m      = s.mean;
        out.var_of_mean_m2 = s.M2 / (n * (n - 1.0));
        out.lambda_wl      = s.lambda_wl;
        out.lambda_ewl     = s.lambda_ewl;
        out.iono_coeff     = s.iono_coeff;
        out.valid          = std::isfinite(out.gf_mean_m) && std::isfinite(out.var_of_mean_m2);
        return out;
    }

    void MwSlipDetector::updateBridgeArc(const PppObservation& obs, bool has_lli) {
        if (!bridge_enabled_ || obs.satellite.empty()) return;

        const char cons = obs.satellite.front();
        // UWL-1 (SPEC_uwl_card.md §3.1): the per-constellation lane-tier band digits.
        // Empty table = the legacy hardcoded {1,2,5} scan for EVERY constellation
        // (byte-identical — and the silent GPS-only failure mode: Galileo has no band
        // '2').  A pushed table with no entry for this constellation forms NO bridge
        // arc (BDS-3 deferred on its clock divergence; the wiring warns loudly).
        char b1 = '1', b2 = '2', bt = '5', bm = '\0', bt2 = '\0';
        if (!lane_tiers_.empty()) {
            const auto tit = lane_tiers_.find(cons);
            if (tit == lane_tiers_.end()) return;
            b1 = tit->second.b1; b2 = tit->second.b2; bt = tit->second.b_top;
            bm = tit->second.b_mid; bt2 = tit->second.b_top2;
        }

        // KF-aligned single phase signal per lane-tier band: min-name over obs.observed,
        // matching the ApplicationNetwork primary(sig1<sig2 by band then name) + band-top
        // (first b_top) selection so this arc's key aligns with the caller's arcBridge query.
        // WL-3: when the network pushed its channel census (@ref setBridgePreferredChannels),
        // the preferred channel WINS where tracked — the caller's census-selected primary
        // pair would otherwise miss the min-name-keyed arc at every (rx,sat) whose channels
        // differ (measured: the WL decision path starved to 9-of-29 cycles).
        std::string s1, s2, s5, sm, st2;  // WL-pair bands (b1, b2) + top band (b_top) + optional mid (b_mid) / fallback top (b_top2)
        for (const auto& [k, v] : obs.observed) {
            if (k.size() < 2 || k[0] != 'L') continue;
            (void)v;
            std::string* dst = (k[1] == b1) ? &s1 : (k[1] == b2) ? &s2
                             : (k[1] == bt) ? &s5
                             : (bm  != '\0' && k[1] == bm)  ? &sm
                             : (bt2 != '\0' && k[1] == bt2) ? &st2 : nullptr;
            if (!dst) continue;
            if (!bridge_pref_.empty()) {
                const auto pit = bridge_pref_.find({cons, k[1]});
                if (pit != bridge_pref_.end()) {
                    if (k == pit->second) *dst = k;              // preferred channel wins
                    else if (dst->empty()) *dst = k;             // fallback until seen
                    else if (*dst != pit->second && k < *dst) *dst = k;  // min-name among fallbacks
                    continue;
                }
            }
            if (dst->empty() || k < *dst) *dst = k;
        }
        // Increment 5 (uwl_dual_top): under a dual-top tier the FALLBACK triple must
        // form even when the preferred band is untracked at this (rx,sat) — that is
        // the whole point of the fallback rung.  Single-top tiers keep the legacy
        // early-out verbatim (bt2 == '\0' ⇒ have_top2 is false ⇒ identical flow).
        const bool have_top  = !s5.empty();
        const bool have_top2 = (bt2 != '\0') && !st2.empty();
        if (s1.empty() || s2.empty() || (!have_top && !have_top2)) return;

        const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

        // One bridge triple (sa, sb, sc): lane A = (sa, sb), lane B = (hi, lo of (sb, sc)).
        // GF = λ_A(φ_sa−φ_sb) − λ_B(φ_hi−φ_lo) = λ_A·N_A − λ_B·N_B + iono + noise.
        // Extracted so the E6 MID HOP (UWL-1, census-motivated) can feed additional
        // triples per observation: the base (b1,b2,b_top) always (byte-identical to the
        // single-triple code for the legacy and mid-off shapes), plus (b_mid,b2,b_top)
        // [hop-hi: mid lane vs top lane] and (b1,b2,b_mid) [hop-lo: WL lane vs mid lane]
        // when the constellation's tier arms a mid rung and the band is tracked.
        auto feedTriple = [&](const std::string& sa, const std::string& sb,
                              const std::string& sc) {
            const std::string key = obs.satellite + "_" + sa + "_" + sb + "_" + sc;

            // Loss-of-lock on any phase re-warms the bridge (it has no detection statistic
            // to catch a real slip, unlike MW's threshold gate — resetting is the safe choice).
            if (has_lli) { bridge_stats_.erase(key); return; }

            // Raw cycle values (matching computeMwMeters' raw-observation convention).
            const auto i1 = obs.raw_observed.find(sa);
            const auto i2 = obs.raw_observed.find(sb);
            const auto i5 = obs.raw_observed.find(sc);
            if (i1 == obs.raw_observed.end() || i2 == obs.raw_observed.end() ||
                i5 == obs.raw_observed.end())
                return;
            const double L1c = i1->second, L2c = i2->second, L5c = i5->second;
            if (!(std::isfinite(L1c) && std::isfinite(L2c) && std::isfinite(L5c))) return;

            const double f1 = ppp_utils_.getFrequency(sa, obs.system, obs.satellite);
            const double f2 = ppp_utils_.getFrequency(sb, obs.system, obs.satellite);
            const double f5 = ppp_utils_.getFrequency(sc, obs.system, obs.satellite);
            if (!(std::isfinite(f1) && std::isfinite(f2) && std::isfinite(f5) &&
                  f1 > 0.0 && f2 > 0.0 && f5 > 0.0))
                return;
            const double df_wl  = f1 - f2;   // lane A: sa − sb (f_sa > f_sb by table order)
            // Lane-B pair (hi − lo) by ACTUAL frequency: GPS's shared band b2 is the top
            // pair's HIGH side (L2 > L5), Galileo's is its LOW side (E5a < E5b/E5/E6) —
            // UWL-1 §2 keeps λ_B positive either way.  For GPS this reproduces the
            // legacy f2 − f5 expression bit-exactly.
            const bool   b2_hi  = f2 > f5;
            const double df_top = b2_hi ? (f2 - f5) : (f5 - f2);
            if (std::abs(df_wl) < 1.0 || std::abs(df_top) < 1.0) return;

            const double c        = Constants::SPEED_OF_LIGHT;
            const double lam_wl   = c / df_wl;
            const double lam_ewl  = c / df_top;
            // GF iono sensitivity to the L1-referenced slant iono I₁ (the estimator's
            // iono-state frame, f_ref = GPS L1): C_iono = f_ref²·(1/(f1·f2) − 1/(f_hi·f_lo))
            // — the product is order-free, so the f2·f5 expression is generic (SPEC §3.4 v2 —
            // the consumer subtracts C_iono·Σc·Î₁ to de-bias the bridge).
            const double f_ref    = Constants::GPS_L1_FREQ;
            const double iono_co  = f_ref * f_ref * (1.0 / (f1 * f2) - 1.0 / (f2 * f5));
            // Lane-A phase (m) = λ_A·(φ_sa−φ_sb); lane-B phase (m) = λ_B·(φ_hi−φ_lo).
            // GF = φ_A − φ_B is geometry-free → λ_A·N_A − λ_B·N_B + differential-iono
            // residue + phase noise; N_B = N_hi − N_lo matches the MW(hi,lo) integer.
            double gf = lam_wl * (L1c - L2c)
                      - lam_ewl * (b2_hi ? (L2c - L5c) : (L5c - L2c));
            if (!std::isfinite(gf)) return;
            // Per-sample iono de-bias (setBridgePerSampleDebias — the 60-ep age-bias
            // fix): fold GF_k − C_iono·ı̃(t_k) so the accumulated statistic is a
            // STATIONARY estimable-function realization; every downstream consumer
            // (Welford mean, prev_gf staging, gross-jump guard) then compares like
            // frames.  A sample without an estimate is skipped, never mixed-frame.
            if (bridge_debias_) {
                const auto iit = bridge_iono_.find(obs.satellite);
                if (iit == bridge_iono_.end() || !std::isfinite(iit->second)) {
                    ++bridge_debias_skipped_;
                    return;
                }
                gf -= iono_co * iit->second;
            }

            BridgeArcStats& st = bridge_stats_[key];

            // Data-gap reset (same tolerance as the MW arcs).
            if (st.count > 0 && st.last_epoch_s > 0.0 &&
                (epoch_s - st.last_epoch_s) > max_gap_seconds_) {
                st = BridgeArcStats{};
            }
            // Gross-jump refloat: a lane slip the ambiguity funnel missed (≥ kBridgeJumpResetM
            // above the sub-decimetre iono residue).  Refloat rather than poison the slow mean.
            if (st.count > 0 && std::abs(gf - st.mean) > kBridgeJumpResetM) {
                st = BridgeArcStats{};
            }

            if (st.count == 0) {
                st.mean       = gf;
                st.M2         = 0.0;
                st.count      = 1;
                st.prev_gf    = gf;        // WL-3: raw-sample staging for arcBridgeLast
                st.prev_epoch_s = epoch_s;
                st.last_epoch_s = epoch_s;
                st.lambda_wl  = lam_wl;
                st.lambda_ewl = lam_ewl;
                st.iono_coeff = iono_co;
                return;
            }

            // Lagged snapshot BEFORE this epoch's value folds in (arcBridge serves these).
            st.mean_lag  = st.mean;
            st.M2_lag    = st.M2;
            st.count_lag = st.count;
            // WL-3 (arcBridgeLast): the raw-sample lag under the same once-only discipline.
            st.prev_gf_lag      = st.prev_gf;
            st.prev_epoch_lag_s = st.prev_epoch_s;
            st.count++;
            const double eff_n = static_cast<double>(std::min<std::size_t>(st.count, kMaxHatchCount));
            const double delta = gf - st.mean;
            st.mean += delta / eff_n;
            const double delta2 = gf - st.mean;
            st.M2 += delta * delta2;
            st.prev_gf      = gf;        // WL-3
            st.prev_epoch_s = epoch_s;
            st.last_epoch_s = epoch_s;
            st.lambda_wl  = lam_wl;
            st.lambda_ewl = lam_ewl;
            st.iono_coeff = iono_co;
        };

        if (have_top)  feedTriple(s1, s2, s5);   // base: WL lane vs preferred top lane (as before)
        if (have_top2) feedTriple(s1, s2, st2);  // increment 5: WL lane vs FALLBACK top lane
        if (bm != '\0' && !sm.empty()) {         // E6 mid hop: extra triples, decision-side
            if (have_top)  feedTriple(sm, s2, s5);   // hop-hi: mid lane (b_mid,b2) vs preferred top
            if (have_top2) feedTriple(sm, s2, st2);  // hop-hi vs the fallback top (dual only)
            feedTriple(s1, s2, sm);                  // hop-lo: WL lane vs mid lane (b_mid,b2)
        }
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
        //  mw_frequency_mode → BAND scope:
        //    Mirror (default): obs.observed — exactly the bands the KF
        //      processes, keeping the detector aligned with the KF.
        //    Dual / Triple: obs.raw_observed filtered to the band set
        //      that @c getAllowedBandDigits returns for this constellation
        //      and FrequencyMode — independent of the KF's band scope.
        //    All:           obs.raw_observed unfiltered.
        //
        //  mw_channel_mode → WITHIN-BAND tracking-mode multiplicity:
        //    Mirror (default): inherits the KF's @c ChannelMode.
        //    Single: first code and first phase per band (e.g. L1W only
        //      on GPS L1).  Matches the legacy behaviour.
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
        //   1. KF-chosen (key is in obs.observed) — keeps MW and KF aligned
        //      on which channel represents the band.  Without this,
        //      iterating raw_observed alphabetically would pick L1C even
        //      when the KF estimates L1W.
        //   2. Canonical tracking-attribute priority via
        //      @c signalAttributePriority — picks the best channel for
        //      bands the KF doesn't track (e.g. L5X over L5I/L5Q for GPS
        //      L5 under signal_mode=dual).
        // This mirrors the same preference logic the KF uses to populate
        // obs.observed in @c PppModel::buildFrequencyBandMap.
        auto isKfChosen = [&obs](const std::string& key) {
            return obs.observed.find(key) != obs.observed.end();
        };
        auto priorityOf = [&obs](const std::string& key) -> int {
            if (key.size() < 3) return std::numeric_limits<int>::max();
            const auto cls = classifySignalForSat(obs.satellite, key);
            if (cls.band == FrequencyBand::Unknown)
                return std::numeric_limits<int>::max();
            // signalAttributePriority switches on band only; the
            // constellation string is unused but the API takes it for
            // forward compatibility — pass the sat id as a stable hint.
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
        // (computeMwMeters uses the signed f_diff at line 484; the
        // wavelength helper uses |f_diff|).  Without this normalization
        // the lexical map-iteration order of band characters produces
        // pairs whose f1 < f2 for constellations where the band-letter
        // sort doesn't match the frequency sort — notably Galileo
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
        // [0, mw_window_size) can be summarized directly — no unwrapping.
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
        // Pooling by FREQUENCY pair rather than full RINEX signal pair joins
        // L1L/L2L, L1W/L2W, L1L/L2W, L1W/L2L … into one statistic.  The MW
        // noise scale is set by the frequency pair (shared widelane wavelength,
        // similar code noise), not the attribute, so this multiplies the
        // sample count for sparse attribute combinations — e.g. when only a
        // few GPS sats track L1L/L2L the per-attribute group never reaches
        // kMinCrossSatSamples and collapses to the kMinSigmaMeters floor (a
        // too-tight threshold that resets every epoch).  Distinct frequency
        // pairs (L1/L2 vs L1/L5) stay separate — only the attribute is
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
        // kMinSigmaMeters floor.  The pooled quantity is the zenith-normalized
        // deviation from each arc's own mean — MW noise with the per-arc bias
        // removed — and MW noise in metres is code-dominated, hence broadly
        // comparable across band-pairs and (clean) constellations.
        //   Tier 1: per (constellation × band-pair)  [cross_sat_sigma_m_]
        //   Tier 2: per constellation, all band-pairs [cross_sat_sigma_cons_]
        //   Tier 3: global, all constellations EXCEPT GLONASS [..._global_]
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
        // elevation on use.  All tiers are empirical MAD — no stochastic-model
        // σ — just pooled at coarser granularity when the fine group is thin.
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
        // Fully data-driven gate — no stochastic-model-derived σ sources are
        // consulted at any stage (empirically the predicted σ_MW saturated the
        // gate at the cap, making it constant rather than statistical).  The
        // gate has two regimes with a clean warmup→per-arc handover:
        //
        //   MATURE arc — the caller passes the mature spread of the ACTIVE
        //   detection statistic (Hatch dev → TD1 MAD; local dev → √2-scaled
        //   ring MAD; TD1 dev → TD1 MAD; TD2 dev → TD2 MAD — see the header
        //   table) and it GOVERNS the gate.  Deviation and spread are then
        //   always the SAME quantity by construction: gating one statistic
        //   with another's spread is the original false-slip mechanism
        //   (trend-lagged level dev vs trend-collapsed TD1 MAD).  The
        //   cross-sat ("all satellites together") σ is intentionally NOT
        //   applied as a floor here: it is recomputed each epoch from a thin
        //   cross-section and a single noisy population value must not
        //   override a converged per-arc estimate (the prior behavior, which
        //   made threshold_cycles jitter while mw_sigma_cycles stayed
        //   smooth).  Only the absolute kMinSigmaMeters floor remains.
        if (std::isfinite(mode_sigma_m)) {
            return sigma_multiplier_ * std::max(mode_sigma_m, kMinSigmaMeters);
        }

        //   WARMUP arc — no trustworthy per-arc MAD yet.  Floor the gate on the
        //   (EWMA-smoothed) cross-sat σ for this group, plus a per-arc spread
        //   term once samples exist.  The multiplier matches the mature branch
        //   so the gate is continuous across the handover at
        //   kMinEpochsForAdaptive.  The per-arc spread term is mode-specific:
        //
        //   - Level modes (HatchMean / LocalMedian): the Welford σ_emp over
        //     raw MW levels — statistic-consistent with a level deviation,
        //     and the legacy behavior (bit-identical).
        //
        //   - TimeDifference modes: the YOUNG TD MAD — RobustStats MAD over
        //     the partial diff_window (TD1) / td2_window (TD2), even below
        //     kMinEpochsForAdaptive.  σ_emp must NOT be used here: it is a
        //     LEVEL variance, and on a trending arc it measures trend + noise
        //     (a ramp of span S contributes ≈ S/√12), ballooning the warmup
        //     gate by whole cycles while the TD deviation it gates stays at
        //     noise scale — observed blinding a fresh arc to ≤2-cycle slips
        //     for ~10 epochs.  The young MAD is trend-immune like the mature
        //     branch, and it is collapse-safe in this position: the historical
        //     hazard of a young MAD (lucky-quiet samples → near-zero) only
        //     applied when it GOVERNED alone; as one term inside
        //     max(kMinSigma, cross-sat floor, young MAD) a collapse simply
        //     leaves the other floors in charge — it can lift, never undercut.
        //     It also makes the warmup→mature handover continuous in the
        //     statistic itself (young MAD at 14 samples → trusted MAD at 15).
        const double xsat_floor   = crossSatSigmaFloor(stats_key, elev_deg);
        const bool   have_cross_sat = std::isfinite(xsat_floor);

        double sigma_floor = kMinSigmaMeters;
        if (have_cross_sat)
            sigma_floor = std::max(sigma_floor, xsat_floor);
        if (detect_reference_ == MwDetectReference::TimeDifference) {
            // Key the window on the epoch's ACTIVE statistic, not the
            // configured order: under MwTdOrder::Second the gate falls back
            // to TD1 whenever prev_diff is NaN (fresh-arc second sample,
            // post-outlier excision), and on a sub-mature LOCKED arc the
            // post-excision epoch reaches this branch with a NON-empty
            // td2_window — flooring a TD1 (√2-scale) deviation with a young
            // TD2 (√6-scale) MAD would run that one epoch ~√3 loose.
            // Matching the window keeps the per-arc term scale-consistent
            // with the deviation it gates in every fallback case.  (A fresh
            // arc's fallback epoch has both windows effectively empty —
            // cross-sat floor governs; a 1-sample window gives MAD = 0 —
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

        // True cold start: no cross-sat σ at ANY cascade tier (the first epoch
        // or two before any arc has a poolable deviation, or a pathologically
        // sparse constellation).  No trustworthy noise estimate at all, so floor
        // the gate at a gross-slip threshold of min(λ_WL, 1 m) — applied
        // directly, NOT ×sigma_multiplier_: with zero noise information we
        // only flag a jump of ~1 widelane cycle or more.  σ_emp can lift the
        // gate but never lower it.  Drops out the moment any tier populates.
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

        // DETECTION STRENGTH (mw_evidence_refloat): the lane CLASS is known here
        // and nowhere downstream, so publish it up front; the ratio and the arc
        // maturity are written at each firing gate from the quantities that gate
        // actually tested.  A path that has no meaningful ratio leaves NaN.
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
        // The threshold is overridable via setMaxGapSeconds — Application
        // tightens it to the RINEX sampling interval when IMU isn't
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
        // reset, so a fresh arc is stamped too).  Bookkeeping only — mw_m arrives
        // pre-computed, so this cannot change a detection.  Served by arcZdMean so a
        // consumer modelling the arc's satellite CODE bias names the OSB column the
        // observable actually carries instead of guessing it from the phase attribute.
        if (!code1.empty()) stats.code1 = code1;
        if (!code2.empty()) stats.code2 = code2;

        // Snapshot the pre-update arc state for potential rollback
        // by resetSatellite / resetSatelliteSignal.  Used when the KF
        // post-fit residual check identifies a slip that MW's own
        // detector missed — the reset functions restore this snapshot
        // (preserving accumulated noise statistics) instead of wiping
        // the arc.  For first-epoch arcs (count==0) and gap-reset arcs
        // (just zeroed above) the snapshot captures the empty MwArcStats
        // so a rollback gives erase-equivalent behavior naturally.
        arc_snapshots_[stats_key] = stats;

        // First epoch: initialize arc.
        if (stats.count == 0) {
            stats.mean         = mw_m;
            stats.M2           = 0.0;
            stats.count        = 1;
            stats.prev_mw      = mw_m;
            stats.prev_epoch_s = epoch_s;   // WL-3: prev_mw's sample stamp (arcZdLast alignment)
            stats.last_epoch_s = epoch_s;
            stats.diff_window.clear();
            stats.bia_corrected = bia_corrected;
            stats.lambda_wl     = lambda_wl;
            stats.wl_locked     = false;
            stats.rounded_wl    = 0;
            stats.mw_window_head = 0;
            stats.mw_window_size = 0;
            pushLocalWindow(stats, mw_m);  // seed the local ring like the mean

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

                // Effective threshold: config-driven init_frac_threshold_
                // floor, lifted by 3σ from the cross-sat MAD pool when
                // available (3σ = conventional 99.7% Gaussian gate).
                // Stochastic-σ lifts (zenith / predicted) are
                // intentionally excluded — see computeAdaptiveThreshold.
                double effective_thresh = init_frac_threshold_;
                {
                    const std::string group_key = makeGroupKey(stats_key);
                    if (!group_key.empty()) {
                        auto it = cross_sat_sigma_m_.find(group_key);
                        if (it != cross_sat_sigma_m_.end() && std::isfinite(it->second)) {
                            // The pool holds ACTIVE-statistic deviations
                            // (TD-scaled in the TimeDifference modes), but
                            // the quantity gated here — the fractional
                            // offset of a single LEVEL sample from
                            // integer·λ — has level noise.  De-scale back
                            // to the level domain (÷√2 TD1, ÷√6 TD2; no-op
                            // in the level modes) or the 3σ lift inflates
                            // past the structural 0.5-frac ceiling and
                            // silently disables this check in exactly the
                            // mode that leans on it (TD erase-restarts).
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
                    // STRENGTH: the gate tested frac against effective_thresh, so
                    // that ratio IS this path's |dev|/threshold.  arc_epochs stays
                    // 0 — an init-suspicious flag fires on the arc's FIRST sample
                    // and has no maturity to report (which is precisely why the
                    // evidence rule's maturity floor refuses it).
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
        // One epoch ago the TD gate fired on this LOCKED arc and set
        // td_confirm_pending instead of silently absorbing.  A step and a
        // one-epoch outlier are perfectly distinguishable now: after an
        // outlier the level returns to the pre-jump track, after a real
        // step it stays on the post-jump track.  Compare the two level
        // hypotheses against the current sample and act.
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
            // levels across a frame jump).  Otherwise the pending is simply
            // dropped — the legacy machinery (drift demotion) remains the
            // backstop for these rare corners.
            if (detect_reference_ == MwDetectReference::TimeDifference
                && stats.wl_locked
                && bia_corrected == stats.bia_corrected) {
                // A pivot ZD slip at THIS epoch moves the SD frame between
                // hypothesis formation (flag epoch) and testing (now): both
                // residuals would be measured against old-frame levels and
                // the outcome would be meaningless.  The flag was 12σ
                // evidence of a real event; unclassifiable evidence
                // resolves to the safe action — unknown-integer slip.
                const bool frame_clean =
                    (pivot_zd_slip_in == 0) && pivot_zd_known_in;

                const double r_outlier = std::abs(mw_m - pre_mw);
                const double r_step    = std::abs(mw_m - (pre_mw + jump_m));

                if (!frame_clean || r_step < r_outlier) {
                    // Confirmed step (or unclassifiable under a moved
                    // frame): demote and emit the slip NOW instead of
                    // waiting ~a dozen epochs for Hatch-mean migration to
                    // trip drift demotion (during which the stale integer
                    // would have been re-exposed to AR).  The arc is
                    // erased — its mean/M2/windows straddle the step — and
                    // the init-frac check independently verifies the
                    // restart, mirroring the unlocked-slip path.
                    //
                    // ALWAYS an unknown integer (downstream reset, never
                    // repair): a deferred result is one epoch late, so a
                    // published integer would (a) be SD→ZD re-referenced
                    // with the WRONG epoch's pivot slip, (b) pair in
                    // TurboEdit with this epoch's GF value — the
                    // sign-flipped echo of the real jump — and (c) race
                    // the KF post-fit / TurboEdit actions already taken at
                    // the flag epoch.  Reset is the only safe deferred
                    // action (reset-over-repair).
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
                    // showed is_slip=1; this row is the ACTION).  The gate
                    // quantities are CARRIED from the flag epoch rather
                    // than recomputed — recomputation does not reproduce
                    // the firing gate for warmup-branch locks (the
                    // cross-sat EWMA floor updates every epoch and
                    // re-expands at the new elevation) or for TD1-fallback
                    // flags inside TD2 order — so |active_dev|/threshold
                    // > 1 holds on this row by construction.
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
                    // STRENGTH: the gate quantities are the ones CARRIED from the
                    // flag epoch (see the trace comment above — recomputing them
                    // here does not reproduce the firing gate), so the ratio is
                    // |flag_dev| / flag_thresh, which is > 1 by construction.
                    if (out_evidence) {
                        if (flag_thresh > 0.0)
                            out_evidence->evidence_ratio =
                                std::abs(flag_dev) / flag_thresh;
                        out_evidence->arc_epochs = static_cast<int>(stats.count);
                    }
                    // FLOAT step estimate (cycles): the integer stays
                    // withheld by the deferred-path contract above, but the
                    // confirmed level gap's magnitude is sound — exported as
                    // a candidate selector (TurboEdit gap-bridge solve).
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
                // the lock and EXCISE the outlier epoch from the TD chain:
                // this epoch's TD re-bases on the pre-outlier level (the
                // honest jump with the outlier removed — otherwise the
                // gate would fire again on the −jump echo), and prev_diff
                // is invalidated so TD2 takes its one-epoch TD1 fallback
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
        // and k_σ = 3 (one-sided 99.7% confidence that round(mean/λ_WL)
        // lands on the true integer).  This replaces the fixed
        // `count ≥ 5` + `|frac| < 0.25` heuristic with an adaptive gate:
        //
        //   - Clean BIA-corrected arcs (sub-cm phase, modest code noise)
        //     have shrunk σ_mean enough by n=kWlLockMinN to pass the kσ
        //     test on the next eligible epoch.
        //   - Noisy arcs (low elevation, high multipath) wait until √n
        //     has averaged the noise down enough; locks happen when
        //     warranted, not on a fixed clock.
        //
        // Hard floor of n ≥ kWlLockMinN: empirical Welford σ from just a
        // handful of samples severely underestimates the long-term noise
        // envelope on a low-elevation arc — the first few epochs are
        // routinely tight by luck, and a kσ test on that tight σ green-
        // lights a premature lock that later snaps to the wrong integer
        // once the true noise envelope emerges.  A floor of 10 lets the
        // empirical variance stabilize before the lock test trusts it,
        // without significantly delaying clean arcs (whose σ_mean shrinks
        // monotonically with n and will pass at n=10 just as cleanly as
        // it would have at n=3).
        //
        // Hard cap on raw |frac|: the statistical test alone trusts
        // sufficiently-tight σ_mean to round 3.33 → 3 with 99.7%
        // confidence — but if a sat has a real per-(sat, signal) bias
        // not captured by BIA, the running mean settles to a non-integer
        // value with small standard error.  The math is internally
        // consistent (mean is well-determined to be 3.33) but the
        // rounded integer can be off by a cycle if the true integer is
        // 4 with bias −0.67 instead of 3 with bias +0.33.  Refuse to
        // lock when |frac| exceeds @c kWlLockMaxFrac regardless of
        // statistical confidence; treat large |frac| as a real bias
        // signal that disqualifies the lock.
        //
        // Note: on lock the running mean is NOT snapped to integer × λ.
        // Keeping it live lets Welford continue to track the arc, which
        // is the foundation of the drift-demotion check applied below
        // (after the slip-detection branch).  If the empirical mean
        // drifts past kWlLockDriftMaxCyc cycles from the locked integer,
        // the lock is demoted on the spot — that's the safety net that
        // catches both wrong-from-the-start locks and accumulated slip
        // sequences that the per-epoch slip detector absorbed as
        // outliers.
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

        // In-house bias commit path: when the toggle is enabled, no external
        // BIA was applied, and the running mean is statistically converged
        // close enough to integer for a confident commit.  Promotes the
        // arc to bia_corrected + wl_locked in one step (the function snaps
        // the mean to integer × λ and stores the residual as the bias).
        // After commit, apply the just-stored bias to the current sample
        // so the dev / slip-check / trace below operate in the
        // bias-corrected frame.  Without this rebalancing, dev would equal
        // the (large) bias on the commit epoch, the slip detector would
        // false-fire, and the trace would show a non-integer mw_m next
        // to a snapped (integer) mw_mean_m — visually inconsistent with
        // wl_locked=1.
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
        // unlocked-arc level-disagreement backstop is level-vs-level —
        // it compares the Hatch mean itself against the local median
        // and never reads dev_hatch.)  The local reference (median of the
        // per-arc raw-MW ring, samples through k−1 only) is likewise
        // always computed — cheap, and tracing it in every mode gives a
        // free offline A/B.
        //
        // The ACTIVE statistic — the (deviation, mature-spread) pair the
        // gate tests — is then picked per detect_reference_; deviation and
        // spread are the same quantity in every mode by construction:
        //  - HatchMean:      dev = mw − hatch_mean,    σ = MAD(TD1).
        //    (Legacy pairing; mismatched under trends — kept for A/B.)
        //  - LocalMedian:    dev = mw − ring median,   σ = √2·MAD(ring).
        //    Trend-immune to first order (lag ~N/2, MAD grows with trend),
        //    but a soft QUADRATIC still walks the level off the median.
        //  - TimeDifference: dev = TD1 (or TD2),       σ = MAD(TD1/TD2).
        //    Memoryless — immune to trend AND curvature; pays √2 (√6)
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
        double mode_sigma;      // mature spread of the SAME statistic (NaN → warmup floors)
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
            // LocalMedian with an empty ring — unreachable after arc init
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

        // Level-frame slip indicator with the LEGACY pairing (Hatch dev vs
        // the TD1-MAD gate), used solely to protect the Welford M2
        // admission below.  M2 integrates the HATCH-frame delta, so its
        // "no slip energy" rule must be judged in the hatch frame: in the
        // memoryless TD modes a locked arc's POST-slip samples pass the TD
        // gate as clean (TD ≈ 0) while still sitting dN·λ off the
        // not-yet-migrated mean — admitting them would pump (dN·λ)² per
        // epoch into the per-sample variance estimator until drift
        // demotion fires.  In HatchMean mode this is identical to is_slip
        // (bit-identity preserved); in TD1 mode thresh_level reuses the
        // already-computed gate.
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
        // score — scale-consistent in every branch EXCEPT TD2 (last_dev
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
        // within a conservative fraction of λ_WL of the arc mean — the
        // wide-lane integer is demonstrably unchanged this epoch.
        // Deliberately NOT keyed on wl_locked: an unlocked mature arc with
        // a small dev is equally valid evidence of ΔN_WL = 0.  A locked
        // arc absorbing a real slip (is_slip true, no result emitted) is
        // exactly the state this set must exclude.
        //
        // Affirmation must have discriminating power, so two extra
        // conditions beyond the 0.3-cycle bound:
        //  - real maturity: count ≥ kEpochCleanMinArcN.  A finite adaptive
        //    threshold is NOT a maturity proxy (the warm-up and cold-start
        //    branches return finite values for 2-sample arcs).
        //  - σ-awareness: a true ΔN_WL = ±1 slip (|dev| ≈ λ_WL) must sit
        //    ≥ 3σ outside the clean bound, i.e. 3σ ≤ (1 − 0.3)·λ_WL,
        //    else noise can pull a real slip inside the bound and the
        //    false affirmation feeds TurboEdit a wrong ΔN_WL = 0 split
        //    (which the frac gate ACCEPTS on L1/L5-class pairs where
        //    λ1/(λ1−λ2) ≈ −2.95 lands near-integer).  With the 3σ
        //    condition the false-affirm probability is ≤ Φ(−3) ≈ 0.13 %.
        // The 0.3-cycle bound itself is a cleanliness gate on a
        // known-integer hypothesis, not a detection threshold — it stays
        // fixed rather than scaling with the empirical σ tiers; the σ
        // condition only decides whether the test is usable at all
        // (noisy low-elevation arcs simply never affirm, and GF-only
        // flags there fall back to reset).
        // The deviation bound is deliberately the HATCH deviation in both
        // reference modes.  The local median absorbs a missed step within
        // ~N/2 epochs, after which the local deviation of a genuinely
        // slipped arc reads near-zero — affirming cleanliness on the local
        // dev would hand TurboEdit a confident false ΔN_WL = 0 for its
        // GF-only split.  The ~100-epoch Hatch memory keeps an absorbed
        // step visible here long after the local window has forgotten it;
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
        }

        // Contribute this dev to the cross-satellite pool for this
        // (constellation × signal-pair) group, but only when this
        // sample wasn't itself flagged as a slip.  The pool is
        // finalized at the end of detectLargeMwResets into
        // cross_sat_sigma_m_ which feeds the next epoch's
        // σ_floor_static.  Skipping the push on is_slip prevents a
        // cycle-jump dev (multi-cycle × λ) from entering the
        // constellation noise estimator.  MAD is statistically
        // robust to a single outlier in 5+, but the slip-dev isn't
        // noise — it's a discrete event, semantically wrong to
        // include in a noise model, and compounds badly when slips
        // happen on multiple sats in close succession.  Pivot arcs
        // (dev ≈ 0 by SD construction) still contribute a central
        // value that doesn't bias MAD.
        const std::string group_key = makeGroupKey(stats_key);
        if (!group_key.empty() && !is_slip) {
            // Zenith-normalize before pooling so the cross-sat MAD is a clean
            // homoscedastic estimate rather than a blend of low-el (noisy) and
            // high-el (quiet) arcs.  computeAdaptiveThreshold re-expands it
            // per-arc via mwElevationWeight.
            //
            // The pooled quantity is the ACTIVE deviation: the warmup floor
            // this pool feeds gates active-statistic deviations, so pooling
            // Hatch deviations in the other modes would mix statistics — a
            // post-absorbed step or trend-lagged Hatch dev (multi-decimeter)
            // would inflate the EWMA floor for the whole constellation and
            // mask fresh-arc slips.  (In the TimeDifference modes the pool
            // therefore holds TD deviations — already on the differenced
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
                // slip-shift trigger.  Pre-refactor, a single sample
                // landing two cycles off mean would round to a new
                // integer and the arc would be shifted there — a
                // brittle decision on noisy low-elevation arcs where
                // dev|>thresh routinely occurred from variance bursts,
                // not real slips.
                //
                // Level modes (HatchMean / LocalMedian): absorb into
                // Welford (the mean is no longer pinned to rounded_wl×λ
                // on lock), so noise outliers barely perturb the mean
                // while accumulated genuinely-slipped samples migrate
                // it past kWlLockDriftMaxCyc — at which point the
                // drift-demotion block below unlocks the arc and
                // signals a slip with unknown integer for downstream
                // re-resolution.  A real slip keeps re-flagging every
                // epoch in these modes, so absorption loses nothing.
                //
                // TimeDifference modes: the memoryless TD flags a real
                // step exactly ONCE — silent absorption would discard
                // the only detection and leave the stale locked integer
                // exposed to AR for the ~dozen epochs of mean migration
                // before drift demotion.  Instead, defer one epoch: set
                // the confirmation pending state (resolved at the top of
                // the next processed sample — outlier vs step are
                // perfectly separable there) and withhold the integer
                // from AR meanwhile.  The sample is still absorbed below
                // exactly as in the level modes, so a declared outlier
                // leaves legacy-identical arc state.
                if (use_td && bia_corrected != stats.bia_corrected) {
                    // Frame-flip artifact: the incoming sample is in a
                    // different bias frame than the arc (per-epoch
                    // code-channel pairing can flip the BIA availability
                    // of the same key between epochs).  The deviation
                    // reflects the frame change, not a step/outlier
                    // hypothesis — never enter the pending store with a
                    // garbage jump; take the legacy absorb instead.
                    logger->debug(
                        "MW TD flag on locked {} ({}) coincides with a "
                        "bias-frame flip (sample bia={}, arc bia={}); "
                        "absorbing as a frame artifact, no confirmation.",
                        obs.satellite, PppUtils::getSystemString(obs.system),
                        bia_corrected, stats.bia_corrected);
                } else if (use_td && (pivot_zd_slip_in != 0 || !pivot_zd_known_in)) {
                    // The pivot's OWN ZD slipped this epoch: every locked
                    // peer's SD series steps by −dN_pivot in common mode.
                    // Deferring would desynchronize from the pass-2 SD→ZD
                    // re-referencing and from CycleSlipManager's pivot-slip
                    // consensus / in-place-repair machinery, which operate
                    // on THIS epoch's results — so behave like the unlocked
                    // path with an unknown integer instead: immediate slip,
                    // erase, downstream reset, synchronous with the
                    // machinery built for exactly this event.
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
                    // STRENGTH: this epoch's own gate quantities (the flag is
                    // immediate here, not deferred), read BEFORE the erase.
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
                // rounded from a SINGLE differenced sample whose noise is
                // √2 (TD1) / √6 (TD2) the level noise — the fixed 0.25-frac
                // gate alone is near-transparent once σ_frac approaches a
                // quarter cycle (TD2 at ordinary MW noise).  Require the
                // integer to be statistically resolvable from one sample
                // (3·σ_active ≤ 0.25·λ, mirroring the epoch-clean
                // σ-awareness condition) before publishing; otherwise leave
                // wl_known=false so downstream resets instead of repairing
                // — reset-over-repair for low-confidence integers.  Not
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
                // FLOAT jump estimate (cycles) regardless of the rounding
                // gate above — even an unresolvable integer carries a sound
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

                // STRENGTH (the principal emission path): |dev| / thresh is the
                // gate this branch just failed, on the ACTIVE statistic and the
                // arc's own adaptive threshold — the quantity a detect-only
                // consumer wants and the one slip_frac cannot substitute for.
                // stats.count is read BEFORE the erase below.
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
        // including for locked arcs — the live mean is the foundation
        // of the drift-demotion check below.  M2 (and therefore
        // σ_emp) is NOT updated on slip epochs: a slip-sized delta
        // would inflate the variance accumulator with a discrete-
        // event contribution that doesn't represent the underlying
        // per-sample noise.  Mean update still runs so a real cycle
        // slip migrates the running mean past kWlLockDriftMaxCyc and
        // trips drift demotion below; for a noise-outlier the 1/n
        // weighting keeps the mean shift negligible.
        // Lagged snapshot BEFORE this epoch's value folds in: arcZdMean serves
        // these so the MW-WL layer's same-epoch read never re-ingests the
        // current observations (SPEC_mw_wl_card §3.3 double-count guard).
        stats.mean_lag  = stats.mean;
        stats.M2_lag    = stats.M2;
        stats.count_lag = stats.count;
        // WL-3 (arcZdLast): snapshot the raw-sample lag under the same discipline —
        // prev_mw still holds the PREVIOUS processed epoch's raw value here.
        stats.prev_mw_lag     = stats.prev_mw;
        stats.prev_epoch_lag_s = stats.prev_epoch_s;
        stats.count++;
        {
            const double eff_n  = static_cast<double>(std::min(stats.count, kMaxHatchCount));
            const double delta  = mw_m - stats.mean;
            stats.mean += delta / eff_n;
            // M2 admission is gated in BOTH frames: the active statistic
            // (is_slip) and the hatch frame (is_slip_level), because the
            // accumulated delta is hatch-frame — see the is_slip_level
            // comment above.  Identical to !is_slip in HatchMean mode.
            if (!is_slip && !is_slip_level) {
                const double delta2 = mw_m - stats.mean;
                stats.M2 += delta * delta2;
            }
        }

        // diff_window push: skip slip epochs so cycle-jump diffs
        // (multi-meter on WL) don't enter the per-arc MAD estimator.
        // MAD is statistically robust to a single outlier but
        // partially-filled windows or successive slip events
        // compromise the robustness, and a slip-diff isn't a noise
        // sample regardless — semantically right to exclude.  The local
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
            // admission rule and cap.  Maintained in every mode — one
            // subtraction and a deque push — so the statistic is warm if
            // the user switches modes between runs on the same data.
            if (std::isfinite(td2)) {
                stats.td2_window.push_back(td2);
                if (stats.td2_window.size() > kMwWindowSize)
                    stats.td2_window.pop_front();
            }
        }
        // prev_mw / prev_diff update unconditionally (slip epochs included),
        // mirroring the long-standing prev_mw semantics: after a locked-arc
        // absorbed REAL step, the next TD1 is post−post ≈ 0 (no echo); a
        // locked-arc noise OUTLIER echoes once (next TD1 = clean − outlier),
        // is flagged again, and is absorbed again — benign, and excluded
        // from every window by the !is_slip admission rule.  Unlocked arcs
        // erase at the slip epoch, so neither case arises.
        stats.prev_diff    = mw_diff;
        stats.prev_mw      = mw_m;
        stats.prev_epoch_s = epoch_s;   // WL-3: prev_mw's sample stamp (arcZdLast alignment)
        stats.last_epoch_s = epoch_s;

        // Drift-based lock demotion.  When the running mean has
        // drifted past kWlLockDriftMaxCyc cycles from the locked
        // integer, the locked integer no longer matches the
        // cumulative evidence — either the lock was premature or
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
                // FLOAT estimate: the migrated mean's offset from the
                // demoted integer IS the absorbed cumulative slip.
                if (out_slip_wl_float) *out_slip_wl_float = drift_cyc;
                // STRENGTH: dev = the drift, threshold = the lock-drift bound —
                // the same pairing the trace row for this path reports.  Nothing
                // is erased here, so stats.count is already the count at the flag.
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
        // ONE epoch with no re-test ever — both far faster than the Hatch
        // mean's ~kMaxHatchCount-epoch memory — and drift demotion above
        // only protects LOCKED arcs.  Without a backstop, an unlocked arc
        // that quietly absorbed a real slip has no escape path: count
        // keeps rising, the statistical lock criterion eventually locks
        // the WRONG integer (the frac/3σ lock gates only DELAY locking,
        // they never signal a slip), and that integer feeds AR via
        // getResolvedWlAmbiguities.  When the two level estimators (slow
        // Hatch mean vs short window median — both maintained and
        // frame-shifted identically in every mode) disagree by more than
        // the lock-drift bound, the arc's level history is internally
        // inconsistent — a step was absorbed, or the arc is on a drift
        // steep enough (≳ 4–5 mm/epoch sustained) that no level estimate
        // is trustworthy.  Mirror the unlocked-slip path: unknown-integer
        // slip, erase, fresh restart (the init-frac check then
        // independently verifies the new arc's first sample).  HatchMean
        // mode is excluded for bit-identity with the legacy gate — its
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
                // passed).  Without a flagged row this reset class would be
                // invisible to the trace — the project's primary slip-chain
                // verification artifact — so mirror the init-slip row
                // convention and write the actual decision quantities:
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
                // FLOAT estimate: the absorbed step is the level gap itself
                // (slow Hatch mean minus the post-step local median).
                if (out_slip_wl_float) *out_slip_wl_float = -level_gap_cyc;
                // STRENGTH: dev = the level gap, threshold = the drift bound —
                // the pairing this path's own trace row writes.  Count read
                // BEFORE the erase below.
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

            // M5 card WL-2 (SPEC §3.4): fold this satellite's tri-frequency phase into its
            // GF_b bridge arc (no-op unless armed via setBridgeEnabled ⇒ byte-identical OFF).
            updateBridgeArc(obs, has_lli);

            // Try combined (PPP) path first
            const auto if_pairs = collectIonoFreePairs(obs);
            const bool has_if = !if_pairs.empty();

            // [DIAG] Per-sat coverage trace.  Lists the keys MwSlipDetector
            // saw in each source map at this epoch — lets us tell if a
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
                    // MW stats intentionally PRESERVED across LLI.  The
                    // running MW = (φ_1 − φ_2)/λ_WL − code-NL/λ_WL is a
                    // function of raw observations only — independent
                    // of the filter ambiguity state — so a short LLI
                    // gap doesn't invalidate the EWMA.  When observations
                    // resume:
                    //  - a real cycle slip jumps MW by ΔN_1 − ΔN_2 cycles
                    //    and trips the threshold check in processMwSample
                    //    on the very next sample (which then resets its
                    //    own per-arc stats with full slip-integer info);
                    //  - a spurious / no-slip LLI leaves MW unchanged, so
                    //    the running mean keeps its converged precision.
                    // Wiping the per-arc EWMA here would throw away that
                    // precision in the no-slip case and be redundant in
                    // the real-slip case.  The LLI ambiguity reset above
                    // remains as a conservative belt-and-suspenders on
                    // the *filter* side; MW gets to keep its observation-
                    // domain history.
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
        // This runs for BOTH combined (PPP) and uncombined (PPP-RTK) paths.
        // The MW observable is always formed from raw RINEX signals and is
        // independent of the filter's measurement model parameterization.
        // -----------------------------------------------------------------
        // [DIAG] Pass 2 entry summary: which (cons, band-pair) groups made
        // it from Pass 1, and how many sats per group.  A group with 1
        // satellite still gets processed (the lone sat is treated as
        // pivot with bia_corrected=false).  A missing group means Pass 1
        // dropped every candidate — usually computeMwMeters NaN or
        // computeSatMwBias coverage issue.
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
            // The transform is only valid when the new pivot was
            // BIA-corrected AND integer-locked at the prior epoch — i.e.
            // we have a reliable integer SD reference.  When that
            // precondition fails we fall back to erase-and-restart for
            // the bia_corrected non-pivot arcs (the legacy behavior).
            //
            // Statistics on epoch-to-epoch differences (diff_window, M2,
            // last_dev, last_sigma) are unchanged by the SD-reference
            // change to first order because the new and old pivot have
            // similar BIA-corrected MW noise levels.  Only the running
            // mean (mean), the previous-epoch raw value (prev_mw), and
            // the locked integer (rounded_wl) are affected.
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
                double piv_old_prev_mw_lag =
                    std::numeric_limits<double>::quiet_NaN();   // WL-3: shifts prev_mw_lag alongside
                double piv_old_prev_diff =
                    std::numeric_limits<double>::quiet_NaN();
                int    piv_old_int     = 0;
                if (new_piv_arc_it != mw_stats_.end()
                    && new_piv_arc_it->second.bia_corrected
                    && new_piv_arc_it->second.wl_locked) {
                    can_s_transform = true;
                    piv_old_mean    = new_piv_arc_it->second.mean;
                    piv_old_prev_mw = new_piv_arc_it->second.prev_mw;
                    piv_old_prev_mw_lag = new_piv_arc_it->second.prev_mw_lag;
                    piv_old_prev_diff = new_piv_arc_it->second.prev_diff;
                    piv_old_int     = new_piv_arc_it->second.rounded_wl;
                }

                // In-house bias S-transform: when the new pivot had a stored
                // bias under the old frame, every other sat's stored bias
                // re-references by subtracting it.  Without an anchor we
                // erase the affected entries — safer to re-bootstrap than
                // to apply a stale-frame bias.
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

                    // The OLD pivot, now becoming a non-pivot, holds a
                    // ZD-frame arc (bia_corrected=false) whose Hatch mean
                    // still contains the receiver WL bias and is NOT
                    // comparable to the SD-frame samples that will start
                    // arriving this epoch.  Without an estimate of the
                    // receiver bias we cannot S-transform it into the new
                    // SD frame; left alone, the next sample manifests as
                    // a multi-cycle pseudo-slip versus the stale mean and
                    // triggers a spurious targeted reset/repair.  Drop
                    // the arc and any stale bias entry — a clean SD arc
                    // rebuilds against the new pivot within ~5 epochs.
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
                            // WL-3: the lagged raw sample re-references by the new pivot's
                            // OWN old-frame lagged sample (exact through the shared terms);
                            // when that is not yet formed, degrade to invalid for one epoch.
                            arc.prev_mw_lag =
                                (std::isfinite(arc.prev_mw_lag)
                                 && std::isfinite(piv_old_prev_mw_lag))
                                    ? arc.prev_mw_lag - piv_old_prev_mw_lag
                                    : std::numeric_limits<double>::quiet_NaN();
                            // A demoted-level stash for this arc lives in
                            // the same SD frame — shift it by the same
                            // constant so the across-gap difference
                            // (new mean − stash) stays frame-invariant.
                            if (auto dl = demoted_levels_.find(stats_key);
                                dl != demoted_levels_.end())
                                dl->second.pre_level_m -= piv_old_mean;
                            // The local raw-MW ring holds old-SD-frame
                            // samples; shift by the same constant as the
                            // mean or the epoch after EVERY handover each
                            // surviving arc would show a local deviation of
                            // piv_old_mean (typically multiple WL cycles)
                            // — a mass false slip across the constellation.
                            shiftLocalWindow(arc, -piv_old_mean);
                            // prev_diff (TD1[k−1]) must be re-referenced to
                            // the new frame too: TD1_new = TD1_old −
                            // TD1_old(new pivot), exactly — through the
                            // shared old-pivot samples its contribution
                            // (slip-sized after a slip-forced handover, not
                            // just epoch noise) cancels identically.  Left
                            // untouched, the next epoch's TD2 on every
                            // surviving arc would read +dN_oldpivot·λ — the
                            // mass false slip this transform exists to
                            // prevent, recreated in the TD2 statistic.  NaN
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
                            // meaningless — drop it (the lock survives;
                            // if the flag was a real step, drift demotion
                            // remains the backstop for this rare corner).
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

                // Drop the new pivot's own stored bias entry — it's the
                // SD anchor now (zero by definition); no bias to track.
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
            // slip_wl_cycles to be the satellite's OWN undifferenced (ZD) slip,
            // so we capture the pivot's ZD slip here and add it back below:
            // dN_sat = (dN_sat - dN_pivot) + dN_pivot.  When the pivot's own
            // integer can't be resolved the conversion is impossible, so the
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
                    // LANE CLASS: the sample carried bia_corrected=false into
                    // processMwSample (the pivot cannot integer-lock — the
                    // receiver bias is still in it), but the VALUE is the
                    // BIA-corrected mw_bia and this arc IS the S-transform
                    // reference of its group.  A pivot re-reference is precisely
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
                    // Re-reference the SD jump to this satellite's OWN ZD slip:
                    // dN_sat = SD + dN_pivot.  Adding an integer leaves the
                    // rounding fraction (slip_frac) unchanged.  Unusable if the
                    // pivot integer was unknown -> consumer resets.
                    slip_cyc = slip_cyc + pivot_zd_slip;
                    wl_known = wl_known && pivot_zd_known;
                    // The float re-references identically — and becomes
                    // UNANCHORED (NaN) when the pivot's own ZD slip is
                    // unresolved, exactly like wl_known.
                    slip_wl_f = (pivot_zd_known && std::isfinite(slip_wl_f))
                        ? slip_wl_f + static_cast<double>(pivot_zd_slip)
                        : std::numeric_limits<double>::quiet_NaN();
                    // LANE CLASS true: this arc is single-differenced against the
                    // detector's MW pivot, so its "slip" population contains the
                    // pivot's own S-transform re-references (measured: 8 satellites
                    // at one station stepping 37-40 WL in the same epoch).  The
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

        // Finalize per-(constellation × signal-pair) cross-sat σ from
        // this epoch's pool of per-sat deviations.  Next epoch's
        // computeAdaptiveThreshold will pick up the result via
        // cross_sat_sigma_m_.  (One-epoch lag is intentional — the
        // current epoch's pool isn't fully assembled until every sat
        // has been processed.)
        finalizeCrossSatSigmas();

        // Per-epoch flush so the trace survives non-graceful termination
        // (console close, second Ctrl-C, crash): nothing else ever flushes
        // this stream — RealTimeEngine::flush() only drains pending
        // observations — so without this, the tail of the run sits in the
        // stream buffer and dies with the process, truncating the file at
        // a buffer boundary.  At ≤10 Hz the flush cost is negligible.
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
            // the step-vs-outlier decision lands next sample — the locked
            // integer is suspect for exactly this epoch, so withhold it
            // from AR rather than expose a possibly-stale value.
            if (stats.td_confirm_pending) continue;
            resolved[key] = stats.rounded_wl;
        }

        // BSSD MW pivots: SD vs self is identically 0 cycles by
        // construction.  Even though pivot arcs have
        // @c bia_corrected=false (receiver bias is not removed from the
        // undifferenced MW), their value in the SD frame is
        // unambiguously 0, so AR can use them as the reference end of
        // a DD.  Without this entry, AR fails whenever its pivot
        // coincides with the MW pivot for that (cons, signal-pair) —
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
        // reported.  Since the lock no longer pins the mean to
        // rounded_wl × λ, the running fractional residual
        // |mean/λ − rounded_wl| is genuinely informative — it tracks
        // the live drift between cumulative evidence and the locked
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
        // getResolvedWlFractions — an arc whose INTEGER is withheld must not
        // report a σ either, or a consumer could weight a lock it cannot use.
        for (const auto& [key, stats] : mw_stats_) {
            if (!stats.wl_locked) continue;
            if (!stats.bia_corrected) continue;
            if (!(stats.lambda_wl > 0.0)) continue;
            if (stats.count < 2) continue;
            if (stats.td_confirm_pending) continue;

            // σ of the MEAN, in cycles — the same expression
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
        // uncertainty, so success is 1 regardless of maturity.  try_emplace so
        // a pivot that ALSO has a real locked arc above keeps its measured
        // quality — mirrors the fractions accessor's treatment of the same keys.
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

        // M5 card WL-2: refloat every bridge arc of this satellite (a slip on any of the
        // three bands invalidates GF_b's constant-ambiguity assumption).  No rollback — the
        // bridge re-warms fresh, and its σ guard keeps a mid-warm arc from arming a fix.
        if (!bridge_stats_.empty())
            for (auto bit = bridge_stats_.begin(); bit != bridge_stats_.end(); )
                if (bit->first.rfind(prefix, 0) == 0) bit = bridge_stats_.erase(bit);
                else ++bit;

        // An external reset invalidates any demoted-level stash for the
        // sat: a rolled-back arc's mean reverts to the PRE-event level, so
        // (mean − stash) would read ~0 with full rollback confidence — a
        // convincingly wrong "no slip" settle estimate.
        demoted_levels_.erase(satellite);
        for (auto dit = demoted_levels_.begin(); dit != demoted_levels_.end(); )
            if (dit->first.rfind(prefix, 0) == 0) dit = demoted_levels_.erase(dit);
            else ++dit;

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
                // No "<sig1>_<sig2>" structure (e.g. raw signal key) — match exact.
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

        // M5 card WL-2: refloat any bridge arc whose triple ("sat_s1_s2_s5") involves the
        // slipped signal (a slip on band 1, 2, OR 5 jumps GF_b) — mirrors the per-pair
        // erase above, extended to the third band.
        if (!bridge_stats_.empty())
            for (auto bit = bridge_stats_.begin(); bit != bridge_stats_.end(); ) {
                bool involves = false;
                if (bit->first.rfind(prefix, 0) == 0) {
                    const std::string rest = bit->first.substr(prefix.size());  // "s1_s2_s5"
                    std::size_t p = 0;
                    while (p < rest.size()) {
                        const std::size_t q = rest.find('_', p);
                        const std::string tok = rest.substr(p, q == std::string::npos ? q : q - p);
                        if (tok == signal) { involves = true; break; }
                        if (q == std::string::npos) break;
                        p = q + 1;
                    }
                }
                if (involves) bit = bridge_stats_.erase(bit);
                else ++bit;
            }

        // Invalidate demoted-level stashes touching this signal (same
        // rationale as resetSatellite: a rollback poisons the settle
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
        bridge_stats_.clear();   // M5 card WL-2: hard restart drops the bridge accumulators too
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
            // full WL bias — decimeters).
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
            // Empirical settle criterion — identical in form to the WL lock
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
        // exact-integer and the lock criterion sees a fully-corrected arc.
        // This mirrors what the BIA-corrected path produces for free.
        // The mean is NOT pinned to rounded_wl × λ — the bias subtraction
        // already centers it near integer (commit gating requires
        // |frac| < kBiasCommitMaxFrac = 0.20 and σ_mean < 0.05 cyc, so
        // the residual after subtraction is well under the drift
        // demotion threshold), and keeping it live lets Welford continue
        // tracking the arc so the same drift-demotion safety net that
        // protects BIA-corrected locks applies here too.
        const auto& stored = sat_pair_bias_.at(stats_key);
        stats.mean         -= stored.bias_m;
        stats.mean_lag     -= stored.bias_m;   // lagged snapshot rides the same frame (MwArcStats doc)
        stats.prev_mw      -= stored.bias_m;
        if (std::isfinite(stats.prev_mw_lag))
            stats.prev_mw_lag -= stored.bias_m;   // WL-3: the lagged raw sample rides too
        // The local raw-MW ring holds pre-commit (uncorrected) samples;
        // re-anchor it with the mean or the commit epoch's local deviation
        // jumps by the full bias (decimeters) — the same false-fire the
        // in-place mw_m adjustment in processMwSample prevents for the
        // Hatch deviation.
        shiftLocalWindow(stats, -stored.bias_m);
        stats.bia_corrected = true;
        stats.rounded_wl    = static_cast<int>(rounded);
        stats.wl_locked     = true;
        return true;
    }

} // namespace GnssProcessor