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
    // Cold cycles that passed FFR (UWL-1 §3.3): the integer is exposed and the row is
    // never emitted.  They are appended to fixed_rows after the normal emission loop,
    // so the order of the warm rows is unchanged.
    std::vector<FixedEwlRow> cold_fixed;

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
            // Per-constellation routing (UWL-1): a cycle of another constellation belongs to
            // that instance and is skipped uncounted.  A resolve failure above (an unexpected
            // path) is counted by every instance.
            if (cfg_.cons_filter != '\0' && !ads.empty() && ads.front().cons != cfg_.cons_filter)
                continue;
            if (!all_primary)  { ++counters.skip_not_primary;  continue; }

            // A cycle to seed (every arc on its estimator primary L1 signal).
            ++counters.cycles_seen;

            bool miss_band = false, miss_mw = false, young = false, bad_lambda = false;
            bool cold = false;   // any arc served by the unlagged cold read (UWL-1 §3.3)
            for (const auto& ad : ads) {
                if (ad.col2 < 0)                       { miss_band  = true; break; }
                if (!ad.have_mw)                       { miss_mw    = true; break; }
                if (ad.mw_cold)                        { cold       = true; }
                if (ad.mw_count < cfg_.min_arc_epochs
                    && !ad.mw_cold)                    { young      = true; break; }
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
            // A cold cycle is decision-only, per UWL-1 §3.3: its statistic carries the current
            // epoch's observation (unlagged) and possibly an a-priori variance, so it never
            // becomes a folded row: not as a float, and not under the ewl_fold_rows study
            // shape.  An FFR pass exposes the integer to the bridge; an FFR fail drops the
            // cycle.  Both are counted.
            if (cold) {
                if (c.fixed) {
                    ++counters.rows_fixed_cold;
                    cold_fixed.push_back(FixedEwlRow{ c.block, c.row, c.fixed_integer });
                } else {
                    ++counters.rows_cold_unfixed;
                }
                continue;
            }
            if (cfg_.debug_r_scale != 1.0 && !c.fixed) {  // debug_r_scale (F6 probe): scale Pz (diag R + off-diag cov) by s;
                c.R *= cfg_.debug_r_scale;                // contribs carry √s so Pz(i,j)=Σ(√s g_i)(√s g_j)=s·Σg_ig_j.
                const double rs = std::sqrt(std::max(cfg_.debug_r_scale, 0.0));
                for (auto& g : c.contribs) g.second *= rs;
            }
            std::sort(c.contribs.begin(), c.contribs.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            cands.push_back(std::move(c));
        }
    }

    if (cands.empty()) {
        // Cold decisions can exist without any emittable row (e.g. every eligible
        // cycle is in a post-slip re-warm window); they are still exposed.
        if (fixed_rows)
            for (const auto& f : cold_fixed) fixed_rows->push_back(f);
        return out;
    }

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

    // Cold decisions are appended after the warm rows (UWL-1 §3.3), so the order of the
    // warm FixedEwlRows is unchanged; the WL walk order affects the solve bytes.
    if (fixed_rows)
        for (const auto& f : cold_fixed) fixed_rows->push_back(f);

    return out;
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the MW/WL lanes: WL-1, EWL, the WL inject, UWL, WL-3.
int networkLanesRunEnd(const NetworkRunEndView& v) {
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkInvariantCarryDriver& carry_driver = v.run.engines.carry_driver;
    const NetworkMwWlLayer::Counters& mw_wl_run = v.run.lanes.mw_wl_run;
    const std::vector<EwlLane>& ewl_lanes = v.run.lanes.ewl_lanes;
    const WlCounters& wl_run = v.run.lanes.wl_run;
    const std::map<char, WlCounters>& wl_cons_run = v.run.lanes.wl_cons_run;
    const bool mw_wl_on = v.ctx.knobs.mw_wl_on;
    const bool ewl_on = v.ctx.knobs.ewl_on;
    const bool wl_inject_on = v.ctx.knobs.wl_inject_on;
    const bool uwl_on = v.ctx.knobs.uwl_on;
    const bool lane_on = v.ctx.knobs.lane_on;
    const auto& nmw = v.config.getNetworkConfig().mw_wl;
    const auto& lane_pair_arcs = v.ctx.carry.lane_pair_arcs;
    const auto& lane_resid_arcs = v.ctx.carry.lane_resid_arcs;
    const auto& lane_pair_tree_mismatch = v.ctx.carry.lane_pair_tree_mismatch;

    if (mw_wl_on) {
        const double mean_R = mw_wl_run.rows_built > 0
            ? mw_wl_run.sum_R / static_cast<double>(mw_wl_run.rows_built) : 0.0;
        log->info("Network (M5/WL-1 MW-seeded wide-lane): {} WL row(s) folded over {} primary "
                  "cycle(s) inspected (mean R = {:.4f} cyc², i.e. σ ≈ {:.3f} cyc); {} capped by "
                  "max_rows_per_epoch.  Skips — {} non-primary group, {} missing L2 band, {} no "
                  "MW arc (channel/pivot), {} young arc (< {} ep), {} bad λ, {} mixed bias "
                  "frame.  Cross-check the KF-WL circR vs scripts/mw_census.py (Phase-A success "
                  "= circR climbing toward ~0.6 as arcs mature; SPEC §7.5).",
                  mw_wl_run.rows_built, mw_wl_run.cycles_seen, mean_R, std::sqrt(mean_R),
                  mw_wl_run.rows_capped, mw_wl_run.skip_not_primary, mw_wl_run.skip_missing_band,
                  mw_wl_run.skip_missing_mw, mw_wl_run.skip_young_arc,
                  config_.getNetworkConfig().mw_wl.min_arc_epochs, mw_wl_run.skip_bad_lambda,
                  mw_wl_run.skip_mixed_frame);
    }
    if (ewl_on) {
        // One line per lane instance (UWL-1): per constellation when the card is armed,
        // since a pooled line hides which lane the rows come from and an armed
        // constellation that decided 0 cycles gets its own 0 line (invariant #4); the
        // legacy pooled line otherwise (lane.cons == 0).
        for (const auto& lane : ewl_lanes) {
            const auto& er = lane.run;
            const double mean_R = er.rows_built > 0
                ? er.sum_R / static_cast<double>(er.rows_built) : 0.0;
            const System lane_sys = systemFromLetter(lane.cons);
            const char* label = lane.cons == '\0'           ? "all-cons"
                              : lane_sys == System::GPS     ? "GPS EWL (L2,L5) λ 5.861 m"
                              : lane_sys == System::Galileo
                                  ? (lane.top == '8'
                                         ? (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5,E5a) λ 19.537 m [preferred]"
                                                : "Galileo UWL (E5,E5a) λ 19.537 m")
                                         : (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5b,E5a) λ 9.768 m [fallback]"
                                                : "Galileo UWL (E5b,E5a) λ 9.768 m"))
                                  : "?";
            log->info("Network (M5/WL-2 {} lane): {} row(s) decided ({} INTEGER-FIXED via FFR, {} "
                      "cold-fixed n<2, {} cold-unfixed; rows {} into the solve) over "
                      "{} primary cycle(s) inspected (mean R = {:.4f} cyc²); {} capped.  Skips — {} "
                      "non-primary, {} missing band, {} no MW arc, {} young arc (< {} ep), {} bad "
                      "λ, {} mixed frame, {} x_lin quarantine.  Uncarried top band; the integers feed "
                      "the WL bridge from the STATISTIC; success = fixes by ep ~{}.",
                      label, er.rows_built + er.rows_fixed_cold, er.rows_fixed,
                      er.rows_fixed_cold, er.rows_cold_unfixed,
                      config_.getNetworkConfig().mw_wl.ewl_fold_rows ? "FOLDED" : "NOT folded",
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
    if (uwl_on && (wl_inject_on || lane_on)) {
        // Per-constellation WL-walk breakdown, never pooled only (SPEC §2; UWL-1).
        for (const auto& [wcons, wr] : wl_cons_run) {
            const double mean_R = wr.cycles_decided > 0
                ? wr.sum_bridge_R / static_cast<double>(wr.cycles_decided) : 0.0;
            const double med_dl = wr.injects > 0
                ? wr.inject_abs_sum / static_cast<double>(wr.injects) : 0.0;
            log->info("Network (M5/UWL-1 '{}' WL walk): {} row(s) bridged → {} decided (mean "
                      "bridge R = {:.4f} cyc², {} via COLD bridge, {} via E6 MID hop), {} hold(s), "
                      "{} lane fix(es) moved + {} re-pinned, {} inject(s) (mean |Δlane| = {:.2f} "
                      "cyc).  Skips — {} no/young bridge, {} no-iono, {} bridge-wide, {} "
                      "FFR-unfixed, {} closure, {} quarantine, {} basis; mid path — {} FFR "
                      "fallback, {} MW(6,5) closure, {} direct-disagree (skipped).",
                      wcons, wr.rows_seen, wr.cycles_decided, mean_R, wr.cold_bridge_cycles,
                      wr.mid_decided,
                      wr.holds_folded, wr.lane_fix_folded, wr.lane_fix_unchanged,
                      wr.injects, med_dl,
                      wr.skip_bridge_missing, wr.skip_no_iono, wr.skip_bridge_wide,
                      wr.skip_not_fixed, wr.skip_closure, wr.skip_quarantine, wr.skip_basis,
                      wr.mid_fallback, wr.mid_closure, wr.mid_disagree);
            // Dual-rung and sibling-closure breakdown (increment 5, uwl_dual_top).
            // mean|frac| ≈ 0 means the 2·N(8,5) = N(7,5) identity holds on this data;
            // ≈ 0.5 means a receiver half-cycle convention offsets it, and a constant has
            // to be learned before arming uwl_sib_gate (SPEC reconciliation note 2).
            if (nmw.uwl_dual_top
                && (wr.top2_decided || wr.dual_dedupe || wr.sib_checked
                    || wr.sib_unavail || wr.sib_blocked))
                log->info("Network (M5/UWL-1 '{}' dual-top): {} of {} decided via the "
                          "FALLBACK (E5b,E5a) rung, {} fallback row(s) deduped (preferred "
                          "already decided).  Sibling closure: {} checked (mean |frac| = "
                          "{:.3f}), {} off-integer, {} unmeasurable, {} BLOCKED (gate {}).",
                          wcons, wr.top2_decided, wr.cycles_decided, wr.dual_dedupe,
                          wr.sib_checked,
                          wr.sib_checked > 0
                              ? wr.sib_abs_frac_sum / static_cast<double>(wr.sib_checked)
                              : 0.0,
                          wr.sib_int_off, wr.sib_unavail, wr.sib_blocked,
                          nmw.uwl_sib_gate ? "ON" : "off/measure-only");
        }
    }
    if (lane_on) {
        // Lane-coordinate carry run-end line (WL-3; invariant #4).  In the lane shape the
        // numbers to read are the continuous increments, the fold-on-change ratio and the
        // inject retirement count (about 0 expected; the WL-2 fallback path).
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-3 lane-coordinate carry): {} pair component(s) reconciled "
                  "({} pair arc(s), {} residual primary-band arc(s), {} tree-tag mismatch(es), "
                  "{} spanning-forest completion(s)); "
                  "lane increments folded — {} MW(1,2) row(s), {} bridge row(s), {} skipped "
                  "(immature/stale/mixed/slip/quarantine), {} pinned-skipped (LF-1 INC-2).  "
                  "FFR lane fixing: {} decided "
                  "cycle(s) (mean bridge R = {:.4f} cyc²) → {} integer(s) MOVED/new, {} "
                  "re-pinned (idempotent lane SET — information-once-only).  Inject FALLBACK: "
                  "{} fired (mean |Δlane| = {:.2f} cyc; ≈0 expected — continuous tightening "
                  "supersedes the WL-2 injects), {} not-1:1 basis skips.",
                  carry_driver.counters().pair_components,
                  lane_pair_arcs, lane_resid_arcs, lane_pair_tree_mismatch,
                  carry_driver.counters().pair_tree_promotions,
                  wl_run.lane_mw_rows, wl_run.lane_bridge_rows, wl_run.lane_skip_meas,
                  wl_run.lane_pinned_skips,
                  wl_run.cycles_decided, mean_R,
                  wl_run.lane_fix_folded, wl_run.lane_fix_unchanged,
                  wl_run.injects, med_dl, wl_run.skip_basis);
    }
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
