/**
 * @file MwSlipDetectorZd.cpp
 * @brief The MW detector's zero-differenced arc reads (network-only).
 *
 * A (satellite, signal-pair) arc served in a zero-differenced frame: the lagged Welford
 * mean, read by the network producer's SD pre-reduce EWL rows.
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class).
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
        // of the previous epoch): per SPEC_mw_wl_card §3.3, a same-epoch read (the SD
        // pre-reduce EWL rows) must be independent of the current epoch's observations.
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

} // namespace GnssProcessor
