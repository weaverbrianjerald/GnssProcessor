/**
 * @file MwSlipDetectorZd.cpp
 * @brief The MW detector's zero-differenced arc reads (network-only, owned with card NOM).
 *
 * A (satellite, signal-pair) arc served in a zero-differenced frame: the lagged Welford
 * mean (WL-1), the lagged raw sample (WL-3) and the unlagged cold read (UWL-1).  Only
 * the network producer's wide-lane layer and lane carry call these.
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class);
 * card UOM step U5m-c moved these bodies here unchanged (scripts/uom_u5m_verify.py
 * proves the move against the single source file of f823a36).
 */

#include "slip/MwSlipDetector.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

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

    MwSlipDetector::MwArcZdLast
    MwSlipDetector::arcZdLast(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2) const {
        MwArcZdLast out;
        // Serve the lagged raw sample (the previous processed epoch's MW) with the
        // per-sample Welford variance: the lane-carry per-epoch increment (WL-3 §3.2).
        // Every read below is from the lag snapshot, like arcZdMean's one-epoch lag (F1).
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
            // Undifferenced already (no-BIA fallback or the group's MW pivot arc):
            // a ZD-consistent per-(station,sat) value, returned directly.
            out.value_m   = s.prev_mw_lag;
            out.var_m2    = varOfSample(s.count_lag, s.M2_lag);
            out.epoch_s   = s.prev_epoch_lag_s;
            out.lambda_wl = s.lambda_wl;
            out.valid     = std::isfinite(out.value_m) && std::isfinite(out.var_m2);
            return out;
        }

        // Non-pivot BIA arc: stored sample = MW_bia(sat) − MW_bia(pivot), formed against
        // the pivot's same-epoch raw sample (pass-2 sd_mw).  Re-add the pivot's own
        // lagged raw sample to recover MW_bia(sat).  The reconstruction is exact only
        // when both lag stamps name the same epoch; otherwise the arc missed an epoch
        // relative to its pivot and the value would mix epochs, so return invalid.
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
        // pivot term cancels in a closed DD cycle, so this over-counts: a loose R).
        out.var_m2    = varOfSample(s.count_lag, s.M2_lag) + varOfSample(p.count_lag, p.M2_lag);
        out.epoch_s   = s.prev_epoch_lag_s;
        out.count     = std::min(s.count_lag, p.count_lag);
        out.lambda_wl = s.lambda_wl;
        out.valid     = std::isfinite(out.value_m) && std::isfinite(out.var_m2);
        return out;
    }

    // =================================================================
    // Cold-fix (n=1) decision-only accessors (M5 card UWL-1, SPEC §3.3)
    // =================================================================

    MwSlipDetector::MwArcZdMean
    MwSlipDetector::arcZdMeanCold(const std::string& sat,
                                  const std::string& sig1,
                                  const std::string& sig2) const {
        MwArcZdMean out;
        // Unlagged serve from n ≥ 1, for integer decisions only (header contract).
        // The variance of the mean = M2/(n(n−1)) is NaN at n==1, where the caller
        // substitutes the a-priori per-sample σ (uwl_cold_sigma_m); the Welford
        // variance takes over from n ≥ 2.  Never feeds a folded row or a lane increment.
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
            // Undifferenced (no-BIA fallback or the group's MW pivot arc): ZD already.
            out.mean_m         = s.mean;
            out.var_of_mean_m2 = varOfMean(s.count, s.M2);
            out.lambda_wl      = s.lambda_wl;
            out.valid          = std::isfinite(out.mean_m);
            return out;
        }
        // Non-pivot BIA arc: re-add the group pivot's unlagged mean (same ZD
        // reconstruction as arcZdMean; conservative variance sum, NaN when either
        // side is single-sample, which the caller's a-priori σ covers).
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

} // namespace GnssProcessor
