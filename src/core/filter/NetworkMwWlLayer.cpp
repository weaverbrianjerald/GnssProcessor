/**
 * @file NetworkMwWlLayer.cpp
 * @brief Implementation of the MW-seeded wide-lane pseudo-observation layer.
 *        See NetworkMwWlLayer.hpp for the design rationale and SPEC_mw_wl_card.md.
 */

#include "filter/NetworkMwWlLayer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

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
        return out;   // OFF ⇒ no blocks ⇒ producer byte-identical.
    }

    const double floor_R = cfg_.sigma_floor_cyc * cfg_.sigma_floor_cyc;

    // One eligible WL row candidate: the folded terms + measured WL (cycles) + R (cyc²).
    // `contribs` records each arc's noise contribution g_a = c·√var_a/λ_WL keyed by the
    // arc's identity (station, band-1 col): two cycles of one component that share an
    // arc share that arc's MW̄ noise, so the block covariance is Pz(i,j) = Σ_shared
    // g_i(a)·g_j(a) — the diagonal-only form would double-count the shared arcs.
    struct Cand {
        std::size_t          block;   ///< source carry-block index (WL rows of one component group together).
        std::size_t          row{0};  ///< WL-2: source row (fundamental-cycle / chord) index within @c block.
        std::vector<Term>    terms;   ///< +c/λ1 on col1, -c/λ2 on col2 per cycle arc.
        double               z{0.0};  ///< prior mean in the CORRECTION frame (cyc): float = z_abs−z_lin; fixed = round(z_abs)−z_lin.
        double               R{0.0};  ///< pseudo-obs variance (cyc²), floored on the DIAGONAL only.
        bool                 fixed{false}; ///< WL-2: this row passed FFR ⇒ folded integer-hard.
        long long            fixed_integer{0}; ///< WL-2: the FFR-fixed EWL integer round(z_abs) (exposed via @ref FixedEwlRow).
        std::vector<std::pair<std::pair<std::size_t, Eigen::Index>, double>>
                             contribs; ///< (arc key = (station, col1), g_a) — sorted by key.  Cleared for a fixed row.
    };
    std::vector<Cand> cands;
    // UWL-1 §3.3: FFR-passed COLD cycles — integer exposed, row NEVER emitted
    // (decision-only; appended to fixed_rows AFTER the normal emission loop so the
    // legacy ordering of warm rows is untouched).
    std::vector<FixedEwlRow> cold_fixed;

    for (std::size_t b = 0; b < carry_blocks.size(); ++b) {
        const auto& blk = carry_blocks[b];
        for (std::size_t ridx = 0; ridx < blk.rows.size(); ++ridx) {
            const auto& row = blk.rows[ridx];       // row = one fundamental cycle (signed Terms)
            if (row.empty()) {
                continue;                            // chordless/degenerate carry slot — nothing to seed
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
            // UWL-1 per-cons routing: another instance's constellation — silently its
            // (a resolve failure above is counted by every instance; "unexpected" path).
            if (cfg_.cons_filter != '\0' && !ads.empty() && ads.front().cons != cfg_.cons_filter)
                continue;
            if (!all_primary)  { ++counters.skip_not_primary;  continue; }

            // A cycle we intend to seed (every arc on its estimator primary L1 signal).
            ++counters.cycles_seen;

            bool miss_band = false, miss_mw = false, young = false, bad_lambda = false;
            bool cold = false;   // UWL-1 §3.3: any arc served by the unlagged cold read
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

            // WL-2 station-1 quarantine (SPEC §7 pre-flight): a pathological ambiguity
            // linearization (the ~256 km receiver-clock-seed) ill-conditions the
            // correction-frame row (z_lin becomes a difference of ~10⁵-scale terms and the
            // cycle fails to converge even at authority — the probe).  Drop + count.
            if (cfg_.max_abs_xlin_m > 0.0) {
                bool bad = false;
                for (const auto& ad : ads)
                    if (std::abs(ad.x_lin1) > cfg_.max_abs_xlin_m ||
                        std::abs(ad.x_lin2) > cfg_.max_abs_xlin_m) { bad = true; break; }
                if (bad) { ++counters.skip_quarantine; continue; }
            }

            // Satellite-bias frame must be UNIFORM across the cycle: cancellation of
            // the per-satellite bias needs every arc of a satellite in the same frame
            // (all raw or all BIA-corrected).  Dormant on the self-contained path
            // (uniformly raw); guards the BIA era.  Strict whole-cycle check — simpler
            // than per-satellite and costs at most a rare extra skip.
            {
                bool mixed = false;
                for (std::size_t i = 1; i < ads.size(); ++i)
                    if (ads[i].bia_corrected != ads[0].bia_corrected) { mixed = true; break; }
                if (mixed) { ++counters.skip_mixed_frame; continue; }
            }

            // --- Build the WL/EWL row: cross-band terms + measured DD cycles + variance. ---
            Cand c;
            c.block = b;
            c.row   = ridx;
            c.terms.reserve(row.size() * 2);
            c.contribs.reserve(row.size());
            double z_abs = 0.0, z_lin = 0.0, R_raw = 0.0;
            for (std::size_t i = 0; i < row.size(); ++i) {
                const double coeff = row[i].coeff;   // ±1 (or ± integer) cycle sign
                const ArcData& ad  = ads[i];
                // +c/λ1 on the low-band column, -c/λ2 on the high-band column (ambiguity
                // state is in METERS), so Σ (coeff/λ1)·Na_m − (coeff/λ2)·Nb_m =
                // Σ coeff·(Na−Nb) cycles = the lane DD (WL for (1,2), EWL for (2,5)).
                c.terms.push_back(Term{row[i].station, ad.col1, +coeff / ad.lambda1});
                c.terms.push_back(Term{row[i].station, ad.col2, -coeff / ad.lambda2});
                z_abs += coeff * ad.mw_zd_mean_m / ad.lambda_wl;               // ABSOLUTE lane DD (cyc)
                z_lin += (coeff / ad.lambda1) * ad.x_lin1                      // C_L·x_lin (F5 frame term)
                       - (coeff / ad.lambda2) * ad.x_lin2;
                R_raw += (coeff * coeff) * ad.mw_var_m2 / (ad.lambda_wl * ad.lambda_wl);
                c.contribs.push_back({{row[i].station, ad.col1},
                                      coeff * std::sqrt(std::max(ad.mw_var_m2, 0.0))
                                          / ad.lambda_wl});
            }
            // Correction-frame re-expression (F5): the kernel solves the CORRECTION about
            // x_lin, so the prior mean is z_abs − C_L·x_lin.
            c.z = z_abs - z_lin;
            c.R = std::max(R_raw, floor_R);          // §4 multipath-honesty floor (diagonal only)

            // WL-2 FFR fix (SPEC §3.2): the DECISION reads the DETECTOR statistic (z_abs,
            // R_raw) — never the carry-contaminated posterior.  A row that passes σ<fix_sigma
            // AND |frac|<fix_frac folds INTEGER-hard (the rounded lane integer, tight Pz);
            // per-epoch re-derivation from the slip-reset statistic IS the un-fix wiring.
            if (cfg_.ffr_fix) {
                const double sigma_row = std::sqrt(std::max(R_raw, 0.0));
                const double frac      = z_abs - std::round(z_abs);
                if (sigma_row < cfg_.fix_sigma_cyc && std::abs(frac) < cfg_.fix_frac_cyc) {
                    c.z             = std::round(z_abs) - z_lin;   // integer target, correction frame
                    c.R             = cfg_.fix_weight_sigma_cyc * cfg_.fix_weight_sigma_cyc;
                    c.fixed         = true;
                    c.fixed_integer = std::lround(z_abs);          // WL-2: exposed for the phase bridge
                    c.contribs.clear();                    // fixed integer ⇒ diagonal-only Pz
                }
            }
            // UWL-1 §3.3: a COLD cycle is DECISION-ONLY.  Its statistic carries the
            // CURRENT epoch's observation (unlagged) plus possibly an a-priori variance,
            // so it must never become a folded row — not even under the ewl_fold_rows
            // study shape, and not as a float.  An FFR pass exposes the integer to the
            // bridge; an FFR fail drops the cycle entirely (both counted).
            if (cold) {
                if (c.fixed) {
                    ++counters.rows_fixed_cold;
                    cold_fixed.push_back(FixedEwlRow{ c.block, c.row, c.fixed_integer });
                } else {
                    ++counters.rows_cold_unfixed;
                }
                continue;
            }
            if (cfg_.debug_r_scale != 1.0 && !c.fixed) {  // F6 probe: scale Pz (diag R + off-diag cov) by s;
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
        // cycle is in a post-slip re-warm window) — still expose them.
        if (fixed_rows)
            for (const auto& f : cold_fixed) fixed_rows->push_back(f);
        return out;
    }

    // --- Optional perf cap: keep the lowest-R (most informative) rows first (§4). ---
    // Deterministic: sort candidate indices by (R, block, original order) so the same
    // set survives across runs.  Count what is dropped (no silent truncation).
    std::vector<std::size_t> keep(cands.size());
    std::iota(keep.begin(), keep.end(), std::size_t{0});
    if (cfg_.max_rows_per_epoch > 0 &&
        cands.size() > static_cast<std::size_t>(cfg_.max_rows_per_epoch)) {
        std::stable_sort(keep.begin(), keep.end(),
                         [&](std::size_t a, std::size_t b) { return cands[a].R < cands[b].R; });
        const std::size_t cap = static_cast<std::size_t>(cfg_.max_rows_per_epoch);
        counters.rows_capped += static_cast<long long>(cands.size() - cap);
        keep.resize(cap);
        // Restore source order so the emitted blocks are deterministic & contiguous.
        std::sort(keep.begin(), keep.end());
    }

    // --- Regroup the kept candidates by source carry block → one WL block each. ---
    // Rows of the SAME carry component land in ONE block so both their shared-column
    // coupling and their shared-arc NOISE covariance are folded jointly (exact).
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
        // Shared-arc covariance vs every earlier row of THIS block: sorted-merge
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
            // WL-2 (§3.4): expose the FFR-fixed integer keyed to its source carry chord so
            // the WL phase bridge can pair (block,row) → the chord's ArcKey.
            if (fixed_rows) fixed_rows->push_back(FixedEwlRow{ c.block, c.row, c.fixed_integer });
        }
        counters.sum_R += c.R;
    }

    // UWL-1 §3.3: cold decisions append AFTER the warm rows (legacy ordering of the
    // warm FixedEwlRows is untouched — the WL walk order feeds the solve bytes).
    if (fixed_rows)
        for (const auto& f : cold_fixed) fixed_rows->push_back(f);

    return out;
}

}  // namespace GnssProcessor
