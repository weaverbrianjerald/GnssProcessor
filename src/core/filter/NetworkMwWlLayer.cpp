/**
 * @file NetworkMwWlLayer.cpp
 * @brief Implementation of the MW-seeded wide-lane pseudo-observation layer.
 *        See NetworkMwWlLayer.hpp for the design rationale and SPEC_mw_wl_card.md.
 */

#include "filter/NetworkMwWlLayer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunGroups.hpp"
#include <memory>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

std::vector<NetworkNormalEquations::DdPriorBlock>
NetworkMwWlLayer::build(
    const std::vector<NetworkNormalEquations::DdPriorBlock>& carry_blocks,
    const ArcResolver& resolve, Counters& counters,
    std::vector<FixedEwlRow>* fixed_rows) const {

    using Term = NetworkNormalEquations::DdPriorBlock::Term;

    if (fixed_rows) fixed_rows->clear();

    std::vector<NetworkNormalEquations::DdPriorBlock> out;
    if (!cfg_.enable) {
        return out;   // Off: no blocks, so the producer output is byte-identical.
    }

    const double floor_R = cfg_.sigma_floor_cyc * cfg_.sigma_floor_cyc;

    // One eligible WL row candidate: the folded terms, the measured WL (cycles) and R (cyc²).
    // `contribs` records each arc's noise contribution g_a = c·√var_a/λ_WL keyed by the
    // arc's identity (station, band-1 col).  Two cycles of one component that share an
    // arc share that arc's MW-mean noise, so the block covariance is Pz(i,j) = Σ_shared
    // g_i(a)·g_j(a); a diagonal-only Pz would treat the shared noise as independent.
    struct Cand {
        std::size_t          block;   ///< source carry-block index (WL rows of one component group together).
        std::size_t          row{0};  ///< source row (fundamental-cycle / chord) index within @c block (WL-2).
        std::vector<Term>    terms;   ///< +c/λ1 on col1, -c/λ2 on col2 per cycle arc.
        double               z{0.0};  ///< prior mean in the correction frame (cyc): float = z_abs−z_lin; fixed = round(z_abs)−z_lin.
        double               R{0.0};  ///< pseudo-obs variance (cyc²), floored on the diagonal only.
        bool                 fixed{false}; ///< this row passed FFR and folds integer-hard (WL-2).
        long long            fixed_integer{0}; ///< the FFR-fixed EWL integer round(z_abs), exposed via @ref FixedEwlRow (WL-2).
        std::vector<std::pair<std::pair<std::size_t, Eigen::Index>, double>>
                             contribs; ///< (arc key = (station, col1), g_a), sorted by key.  Cleared for a fixed row.
    };
    std::vector<Cand> cands;

    for (std::size_t b = 0; b < carry_blocks.size(); ++b) {
        const auto& blk = carry_blocks[b];
        for (std::size_t ridx = 0; ridx < blk.rows.size(); ++ridx) {
            const auto& row = blk.rows[ridx];       // row = one fundamental cycle (signed Terms)
            if (row.empty()) {
                continue;                            // chordless/degenerate carry slot: nothing to seed
            }

            // --- Resolve every cycle arc; classify primary vs data eligibility. ---
            std::vector<ArcData> ads;
            ads.reserve(row.size());
            bool resolved_all = true;
            bool all_primary  = true;
            for (const auto& t : row) {
                ArcData ad;
                if (!resolve(t.station, t.local_col, ad)) { resolved_all = false; break; }
                if (!ad.is_primary_sig1) all_primary = false;
                ads.push_back(ad);
            }
            if (!resolved_all) { ++counters.skip_missing_band; continue; }
            if (!all_primary)  { ++counters.skip_not_primary;  continue; }

            // A cycle to seed (every arc on its estimator primary L1 signal).
            ++counters.cycles_seen;

            bool miss_band = false, miss_mw = false, young = false, bad_lambda = false;
            for (const auto& ad : ads) {
                if (ad.col2 < 0)                       { miss_band  = true; break; }
                if (!ad.have_mw)                       { miss_mw    = true; break; }
                if (ad.mw_count < cfg_.min_arc_epochs) { young      = true; break; }
                if (!(std::isfinite(ad.lambda1) && ad.lambda1 > 0.0 &&
                      std::isfinite(ad.lambda2) && ad.lambda2 > 0.0 &&
                      std::isfinite(ad.lambda_wl) && ad.lambda_wl > 0.0)) {
                    bad_lambda = true; break;
                }
                if (!(std::isfinite(ad.mw_zd_mean_m) && std::isfinite(ad.mw_var_m2) &&
                      ad.mw_var_m2 >= 0.0)) {
                    miss_mw = true; break;
                }
            }
            if (miss_band)  { ++counters.skip_missing_band; continue; }
            if (miss_mw)    { ++counters.skip_missing_mw;   continue; }
            if (young)      { ++counters.skip_young_arc;    continue; }
            if (bad_lambda) { ++counters.skip_bad_lambda;   continue; }

            // Station-1 quarantine (WL-2, SPEC §7 pre-flight): a pathological ambiguity
            // linearization (the ~256 km receiver-clock seed) ill-conditions the
            // correction-frame row: z_lin becomes a difference of ~10⁵-scale terms and the
            // cycle fails to converge even at full authority (the probe).  Such cycles are
            // dropped and counted.
            if (cfg_.max_abs_xlin_m > 0.0) {
                bool bad = false;
                for (const auto& ad : ads)
                    if (std::abs(ad.x_lin1) > cfg_.max_abs_xlin_m ||
                        std::abs(ad.x_lin2) > cfg_.max_abs_xlin_m) { bad = true; break; }
                if (bad) { ++counters.skip_quarantine; continue; }
            }

            // The satellite-bias frame must be uniform across the cycle: cancelling the
            // per-satellite bias needs every arc of a satellite in the same frame (all raw
            // or all BIA-corrected).  Never fires on the self-contained path (uniformly
            // raw); guards runs with BIA.  The check is whole-cycle rather than
            // per-satellite: simpler, at the cost of a rare extra skip.
            {
                bool mixed = false;
                for (std::size_t i = 1; i < ads.size(); ++i)
                    if (ads[i].bia_corrected != ads[0].bia_corrected) { mixed = true; break; }
                if (mixed) { ++counters.skip_mixed_frame; continue; }
            }

            // --- Build the WL/EWL row: cross-band terms, measured DD cycles, variance. ---
            Cand c;
            c.block = b;
            c.row   = ridx;
            c.terms.reserve(row.size() * 2);
            c.contribs.reserve(row.size());
            double z_abs = 0.0, z_lin = 0.0, R_raw = 0.0;
            for (std::size_t i = 0; i < row.size(); ++i) {
                const double coeff = row[i].coeff;   // ±1 (or ± integer) cycle sign
                const ArcData& ad  = ads[i];
                // +c/λ1 on the low-band column, -c/λ2 on the high-band column (the ambiguity
                // state is in metres), so Σ (coeff/λ1)·Na_m − (coeff/λ2)·Nb_m =
                // Σ coeff·(Na−Nb) cycles = the lane DD (WL for (1,2), EWL for (2,5)).
                c.terms.push_back(Term{row[i].station, ad.col1, +coeff / ad.lambda1});
                c.terms.push_back(Term{row[i].station, ad.col2, -coeff / ad.lambda2});
                z_abs += coeff * ad.mw_zd_mean_m / ad.lambda_wl;               // absolute lane DD (cyc)
                z_lin += (coeff / ad.lambda1) * ad.x_lin1                      // C_L·x_lin (F5 frame term)
                       - (coeff / ad.lambda2) * ad.x_lin2;
                R_raw += (coeff * coeff) * ad.mw_var_m2 / (ad.lambda_wl * ad.lambda_wl);
                c.contribs.push_back({{row[i].station, ad.col1},
                                      coeff * std::sqrt(std::max(ad.mw_var_m2, 0.0))
                                          / ad.lambda_wl});
            }
            // Correction-frame re-expression (F5): the kernel solves the correction about
            // x_lin, so the prior mean is z_abs − C_L·x_lin.
            c.z = z_abs - z_lin;
            c.R = std::max(R_raw, floor_R);          // multipath floor, §4 (diagonal only)

            // FFR fix (WL-2, SPEC §3.2): the decision reads the detector statistic (z_abs,
            // R_raw), never the carry-contaminated posterior.  A row with σ < fix_sigma and
            // |frac| < fix_frac folds integer-hard (the rounded lane integer, tight Pz).  The
            // decision is re-derived every epoch from the statistic, which a slip resets, and
            // that re-derivation is the un-fix path.
            if (cfg_.ffr_fix) {
                const double sigma_row = std::sqrt(std::max(R_raw, 0.0));
                const double frac      = z_abs - std::round(z_abs);
                if (sigma_row < cfg_.fix_sigma_cyc && std::abs(frac) < cfg_.fix_frac_cyc) {
                    c.z             = std::round(z_abs) - z_lin;   // integer target, correction frame
                    c.R             = cfg_.fix_weight_sigma_cyc * cfg_.fix_weight_sigma_cyc;
                    c.fixed         = true;
                    c.fixed_integer = std::lround(z_abs);          // exposed for the phase bridge (WL-2)
                    c.contribs.clear();                    // fixed integer: diagonal-only Pz
                }
            }
            std::sort(c.contribs.begin(), c.contribs.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            cands.push_back(std::move(c));
        }
    }

    if (cands.empty()) return out;

    // --- Optional perf cap: keep the lowest-R (most informative) rows first (§4). ---
    // Deterministic: a stable sort of the candidate indices by R, ties in candidate
    // (block, row) order, so the same set survives across runs.  Dropped rows are counted.
    std::vector<std::size_t> keep(cands.size());
    std::iota(keep.begin(), keep.end(), std::size_t{0});
    if (cfg_.max_rows_per_epoch > 0 &&
        cands.size() > static_cast<std::size_t>(cfg_.max_rows_per_epoch)) {
        std::stable_sort(keep.begin(), keep.end(),
                         [&](std::size_t a, std::size_t b) { return cands[a].R < cands[b].R; });
        const std::size_t cap = static_cast<std::size_t>(cfg_.max_rows_per_epoch);
        counters.rows_capped += static_cast<long long>(cands.size() - cap);
        keep.resize(cap);
        // Restore source order so the emitted blocks are deterministic and contiguous.
        std::sort(keep.begin(), keep.end());
    }

    // --- Regroup the kept candidates by source carry block → one WL block each. ---
    // Rows of one carry component land in one block, so both their shared-column
    // coupling and their shared-arc noise covariance are folded jointly (exact).
    // Pz(i,j) = Σ over shared arcs g_i(a)·g_j(a); the diagonal additionally carries
    // the σ floor (adding a non-negative diagonal to the Gram matrix keeps Pz PSD).
    std::vector<std::size_t> block_of_out;   // out-block index per distinct source block, or SIZE_MAX
    block_of_out.assign(carry_blocks.size(), static_cast<std::size_t>(-1));
    std::vector<std::vector<std::size_t>> block_cands;   // out-block → kept cand indices (for the covariance)
    for (std::size_t idx : keep) {
        const Cand& c = cands[idx];
        std::size_t ob = block_of_out[c.block];
        if (ob == static_cast<std::size_t>(-1)) {
            ob = out.size();
            block_of_out[c.block] = ob;
            out.emplace_back();
            block_cands.emplace_back();
        }
        auto& dst = out[ob];
        dst.rows.push_back(c.terms);
        const Eigen::Index k = static_cast<Eigen::Index>(dst.rows.size());
        dst.zhat.conservativeResize(k);
        dst.zhat(k - 1) = c.z;
        dst.Pz.conservativeResize(k, k);
        dst.Pz.row(k - 1).setZero();
        dst.Pz.col(k - 1).setZero();
        dst.Pz(k - 1, k - 1) = c.R;
        // Shared-arc covariance against every earlier row of this block: sorted-merge
        // intersection of the two contrib lists (cycle lengths are ~4-10 arcs).
        const auto& prev = block_cands[ob];
        for (std::size_t j = 0; j < prev.size(); ++j) {
            const Cand& cj = cands[prev[j]];
            double cov = 0.0;
            std::size_t a = 0, bmi = 0;
            while (a < c.contribs.size() && bmi < cj.contribs.size()) {
                if (c.contribs[a].first < cj.contribs[bmi].first)      ++a;
                else if (cj.contribs[bmi].first < c.contribs[a].first) ++bmi;
                else { cov += c.contribs[a].second * cj.contribs[bmi].second; ++a; ++bmi; }
            }
            if (cov != 0.0) {
                dst.Pz(k - 1, static_cast<Eigen::Index>(j)) = cov;
                dst.Pz(static_cast<Eigen::Index>(j), k - 1) = cov;
            }
        }
        block_cands[ob].push_back(idx);
        ++counters.rows_built;
        if (c.fixed) {
            ++counters.rows_fixed;
            // Expose the FFR-fixed integer keyed to its source carry chord (WL-2 §3.4),
            // so the WL phase bridge can pair (block,row) → the chord's ArcKey.
            if (fixed_rows) fixed_rows->push_back(FixedEwlRow{ c.block, c.row, c.fixed_integer });
        }
        counters.sum_R += c.R;
    }

    return out;
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the MW/WL lanes: EWL and the WL inject.
int networkLanesRunEnd(const NetworkRunEndView& v) {
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const std::vector<EwlLane>& ewl_lanes = v.run.lanes.ewl_lanes;
    const WlCounters& wl_run = v.run.lanes.wl_run;
    const bool ewl_on = v.ctx.knobs.ewl_on;
    const bool wl_inject_on = v.ctx.knobs.wl_inject_on;

    if (ewl_on) {
        // One line per lane instance (a single all-constellation instance; invariant #4).
        for (const auto& lane : ewl_lanes) {
            const auto& er = lane.run;
            const double mean_R = er.rows_built > 0
                ? er.sum_R / static_cast<double>(er.rows_built) : 0.0;
            log->info("Network (M5/WL-2 {} lane): {} row(s) decided ({} INTEGER-FIXED via FFR; "
                      "rows NOT folded into the solve) over "
                      "{} primary cycle(s) inspected (mean R = {:.4f} cyc²); {} capped.  Skips — {} "
                      "non-primary, {} missing band, {} no MW arc, {} young arc (< {} ep), {} bad "
                      "λ, {} mixed frame, {} x_lin quarantine.  Uncarried top band; the integers feed "
                      "the WL bridge from the STATISTIC; success = fixes by ep ~{}.",
                      "all-cons", er.rows_built, er.rows_fixed,
                      er.cycles_seen, mean_R,
                      er.rows_capped, er.skip_not_primary, er.skip_missing_band,
                      er.skip_missing_mw, er.skip_young_arc,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs, er.skip_bad_lambda,
                      er.skip_mixed_frame, er.skip_quarantine,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs + 1);
        }
    }
    if (wl_inject_on) {
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-2 WL lane, phase bridge + inject): {} EWL-fixed row(s) bridged → "
                  "{} WL cycle(s) decided (mean bridge R = {:.4f} cyc², σ ≈ {:.3f} cyc, iono-de-biased); "
                  "{} hold row(s) folded, {} inject(s) (mean |Δlane| = {:.2f} cyc).  Skips — {} no/young "
                  "bridge arc, {} no iono estimate, {} bridge σ>{} guard, {} FFR-unfixed, {} MW(1,2) "
                  "closure, {} x_lin quarantine, {} not-1:1 basis.  Bridge = phase-only, DD-iono removed "
                  "via the network estimate; residual estimate-error gated by FFR |frac| + MW(1,2) "
                  "closure (§3.4 v2); inject moves the carried lane to the integer, NL preserved (§3.3).",
                  wl_run.rows_seen, wl_run.cycles_decided, mean_R, std::sqrt(mean_R),
                  wl_run.holds_folded, wl_run.injects, med_dl,
                  wl_run.skip_bridge_missing, wl_run.skip_no_iono, wl_run.skip_bridge_wide,
                  config_.getNetworkConfig().mw_wl.bridge_max_sigma_cyc,
                  wl_run.skip_not_fixed, wl_run.skip_closure, wl_run.skip_quarantine,
                  wl_run.skip_basis);
    }
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
