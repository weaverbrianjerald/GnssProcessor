#pragma once
/**
 * @file RobustStats.hpp
 * @brief @internal Median/MAD robust helpers for the network run loop's
 *        attribution and thresholding blocks (post-fit RAIM, IF-phase pooling).
 *
 * The original helpers were moved unchanged out of the ApplicationNetwork.cpp
 * anonymous namespace (decomposition INC-2; see
 * docs/network_decomposition_plan.md).  Header-only: the pure
 * helpers are @c inline / templates with no state, so they are independently
 * unit-testable (tests/network/test_network_robust_stats.cpp).
 */

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Median of a copy (upper-middle for even counts, matching this file's convention).
inline double medianCopy(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

/// Robust prominence of each entry in @p eff: how many robust sigmas its effect sits
/// above the median of all entries (z = (effect − median) / (1.4826·MAD); a zero MAD is
/// replaced by 1).  Used to attribute a network residual spike to a bad satellite,
/// station or signal.
template <class KeyT>
std::map<KeyT, double> robustProminence(const std::map<KeyT, double>& eff) {
    std::vector<double> vals;
    vals.reserve(eff.size());
    for (const auto& kv : eff) vals.push_back(kv.second);
    const double m = medianCopy(vals);
    std::vector<double> ad;
    ad.reserve(vals.size());
    for (double v : vals) ad.push_back(std::abs(v - m));
    double mad = 1.4826 * medianCopy(ad);
    if (!(mad > 0.0)) mad = 1.0;
    std::map<KeyT, double> out;
    for (const auto& kv : eff) out[kv.first] = (kv.second - m) / mad;
    return out;
}

/// One post-fit RAIM phase row's contribution to a per-station variance factor
/// (QUEUE item 4 R2, `SPEC_t3_null_derivation.md` §7.5): the weighted squared
/// residual, the row's Baarda redundancy, and the same standardized statistic
/// the network σ̂₀ pool uses (for the outlier trim).
struct StationFactorRow {
    double wv2;    ///< w·v² (numerator contribution)
    double ri;     ///< per-row Baarda redundancy r_i (denominator contribution)
    double r_std;  ///< |v|·√w (÷√r_i under B2): the pooled statistic, for trimming
};

/**
 * @brief Trimmed per-station variance component
 *        σ̂²_sta = Σ w·v² / Σ r_i over the station's post-fit RAIM rows
 *        (QUEUE item 4 R2; the §7.5 form with redundancy in the denominator).
 *
 * Rows whose pooled statistic exceeds @p trim_above are excluded from both
 * sums.  A spiking row (the T3 detection population) must not set the slow
 * per-station scale, or a single slip would read as a station-scale departure;
 * §7.5 notes that the untrimmed form is outlier-sensitive and that the choice of
 * robust variant changes the result materially.  Trimming both sums keeps the
 * E[w·v²] = r_i identity intact for the surviving rows.
 * @p trim_above ≤ 0 disables the trim.
 *
 * @return the variance factor (≈1 when the stochastic model matches the data), or
 *         -1 as a sentinel when the station has no checkable redundancy this
 *         epoch (Σr_i ≤ 1e-9); callers treat that as unavailable, not as 0.
 */
inline double stationVarianceFactor(const std::vector<StationFactorRow>& rows,
                                    double trim_above) {
    double num = 0.0, den = 0.0;
    for (const auto& q : rows) {
        if (trim_above > 0.0 && q.r_std > trim_above) continue;
        num += q.wv2;
        den += q.ri;
    }
    return den > 1e-9 ? num / den : -1.0;
}

/// Internal level authority: the two-tier robust fleet reduction, pure and
/// unit-testable (SPEC_level_authority_internal §3, LV-6).
struct LevelAuthorityResult {
    double level_tecu;  ///< fleet median over per-station weighted means
    double mad_tecu;    ///< 1.4826·MAD across the station means (not a formal σ: the kernel variances are known to be optimistic)
    int    n_sta;       ///< stations that cleared min_los
    int    n_los;       ///< mature LOS folded across those stations
};

/**
 * @brief Two-tier reduction: per-station information-weighted mean of mature
 *        vertical-mapped LOS values (tier 1: equalizes station weighting; the
 *        station means also serve as the d_r,GF-class diagnostic), then the
 *        fleet median across stations (tier 2: suppresses the fleet-clustered
 *        bias tail and roster imbalance; the median of static per-station
 *        offsets is a deterministic order statistic, not a sampled one).
 *
 * @param sta_rows  per station: (vertical TECU, weight) pairs, mature only.
 * @param min_los   a station enters tier 2 only with at least this many rows
 *                  (and a positive weight sum).
 * @return nullopt when no station qualifies (unavailable, not 0).
 */
inline std::optional<LevelAuthorityResult> levelAuthorityReduce(
    const std::vector<std::vector<std::pair<double, double>>>& sta_rows,
    int min_los) {
    std::vector<double> means;
    int n_los = 0;
    for (const auto& rows : sta_rows) {
        if (static_cast<int>(rows.size()) < min_los) continue;
        double sw = 0.0, swv = 0.0;
        for (const auto& [v, w] : rows) { sw += w; swv += w * v; }
        if (!(sw > 0.0)) continue;
        means.push_back(swv / sw);
        n_los += static_cast<int>(rows.size());
    }
    if (means.empty()) return std::nullopt;
    const double med = medianCopy(means);
    std::vector<double> ad;
    ad.reserve(means.size());
    for (double m : means) ad.push_back(std::abs(m - med));
    return LevelAuthorityResult{med, 1.4826 * medianCopy(ad),
                                static_cast<int>(means.size()), n_los};
}

/// Result of a Tukey 2-way median polish: value ≈ overall + row_eff[row] + col_eff[col]
/// + interaction (parallel to the input cell order).
struct MedianPolish {
    double overall{0.0};
    std::map<int, double> row_eff;          ///< per-row (station) effect
    std::map<std::string, double> col_eff;  ///< per-column (satellite) effect
    std::vector<double> interaction;        ///< leftover per cell (isolated arc anomaly)
};

/// Robust additive decomposition of a sparse (row=int, col=string) → value table by
/// alternately sweeping out row and column medians (Tukey median polish).  Because the
/// sweeps use medians, a minority of large cells (the real outliers) does not distort the
/// additive row/column effects.  This separates a bad satellite, which raises a whole
/// column, from a bad station, which raises a whole row, and leaves isolated
/// (station,sat) arc slips in the interaction term.  O(iters · cells); no re-solve.
inline MedianPolish medianPolish2Way(
        const std::vector<std::tuple<int, std::string, double>>& cells, int iters = 5) {
    MedianPolish mp;
    std::vector<double> R(cells.size());
    std::map<int, std::vector<std::size_t>> by_row;
    std::map<std::string, std::vector<std::size_t>> by_col;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        R[k] = std::get<2>(cells[k]);
        by_row[std::get<0>(cells[k])].push_back(k);
        by_col[std::get<1>(cells[k])].push_back(k);
    }
    for (const auto& kv : by_row) mp.row_eff[kv.first] = 0.0;
    for (const auto& kv : by_col) mp.col_eff[kv.first] = 0.0;
    auto med_idx = [&](const std::vector<std::size_t>& idx) {
        std::vector<double> v;
        v.reserve(idx.size());
        for (std::size_t k : idx) v.push_back(R[k]);
        return medianCopy(v);
    };
    for (int it = 0; it < iters; ++it) {
        for (const auto& [row, idx] : by_row) {
            const double rm = med_idx(idx);
            for (std::size_t k : idx) R[k] -= rm;
            mp.row_eff[row] += rm;
        }
        {
            std::vector<double> re;
            for (const auto& kv : mp.row_eff) re.push_back(kv.second);
            const double m = medianCopy(re);
            mp.overall += m;
            for (auto& kv : mp.row_eff) kv.second -= m;
        }
        for (const auto& [col, idx] : by_col) {
            const double cm = med_idx(idx);
            for (std::size_t k : idx) R[k] -= cm;
            mp.col_eff[col] += cm;
        }
        {
            std::vector<double> ce;
            for (const auto& kv : mp.col_eff) ce.push_back(kv.second);
            const double m = medianCopy(ce);
            mp.overall += m;
            for (auto& kv : mp.col_eff) kv.second -= m;
        }
    }
    mp.interaction = std::move(R);
    return mp;
}

} // namespace GnssProcessor
