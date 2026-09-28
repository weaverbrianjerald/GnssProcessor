/**
 * @file NetworkClockAnchor.cpp
 * @brief Implementation of the guarded temporal clock anchor (card Q3,
 *        docs/SPEC_clock_temporal_anchor.md).  The design rationale, the two exact
 *        identities the guard rests on, and the invariant #1 and #7 contract are in
 *        the header's file comment.
 */

#include "filter/NetworkClockAnchor.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "filter/NetworkPivotHandover.hpp"   // datumChangeAffectsKey

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace GnssProcessor {

namespace {

/// Maximum in-epoch withdraw→re-solve passes.  A pass can expose a second
/// satellite (removing one trip moves the per-constellation median), but the
/// target population is a handful of genuine events per day, so an epoch that
/// needs more than three passes points at a mis-specified anchor rather than three
/// data faults.  The cap is a backstop, and reaching it is logged.
constexpr int kCtaMaxPasses = 3;

/// Minimum fraction of the folded prior variance that the data must have removed
/// (`(P⁻ − P⁺)/P⁻`, the redundancy of the carried row) before a refloat is
/// admissible.  Below it the satellite's clock column is prior-dominated:
/// withdrawing its prior would leave a nearly unobservable column, which the
/// re-solve's relative-pivot guard would reject.  Such a satellite still decouples
/// and is still marked; only the same-epoch value correction is skipped, and the
/// skip is logged.
constexpr double kCtaMinCarryRedundancy = 0.05;

/// Comma-join a satellite set for a log line (fmt/ranges is not pulled in here).
std::string joinSats(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) { if (!out.empty()) out += ','; out += v; }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The gauge projection, shared by the guard and the census (§2.1).
// ---------------------------------------------------------------------------

std::vector<double> clockAnchorGaugeProject(const Eigen::MatrixXd& cov,
                                            const std::vector<Eigen::Index>& cols) {
    const std::size_t n = cols.size();
    if (n < 2) return {};                      // a lone clock is the gauge
    std::vector<double> rowmean(n, 0.0);
    double grand = 0.0;
    for (std::size_t a = 0; a < n; ++a) {
        double sum = 0.0;
        for (std::size_t b = 0; b < n; ++b) sum += cov(cols[a], cols[b]);
        rowmean[a] = sum / static_cast<double>(n);
        grand += sum;
    }
    grand /= static_cast<double>(n) * static_cast<double>(n);
    std::vector<double> out(n, 0.0);
    for (std::size_t a = 0; a < n; ++a)
        out[a] = cov(cols[a], cols[a]) - 2.0 * rowmean[a] + grand;
    return out;
}

std::vector<double> clockAnchorGaugeProjectDiag(const std::vector<double>& var) {
    const std::size_t n = var.size();
    if (n < 2) return {};
    const double dn = static_cast<double>(n);
    double sum = 0.0;
    for (double v : var) sum += v;
    const double grand = sum / (dn * dn);
    std::vector<double> out(n, 0.0);
    for (std::size_t a = 0; a < n; ++a)
        out[a] = var[a] - 2.0 * var[a] / dn + grand;   // rowmean_a = var[a]/n on a diagonal
    return out;
}

// ---------------------------------------------------------------------------
// Fold side: the variance decision and the per-satellite bookkeeping.
// ---------------------------------------------------------------------------

void clockAnchorBeginEpoch(NetworkRunContext& ctx) {
    if (!ctx.knobs.cta_on) return;
    for (auto& kv : ctx.carry.clk_anchor.sat) {
        kv.second.folded          = false;
        kv.second.folded_anchored = false;
        kv.second.stale_pending   = false;
    }
    ctx.carry.clk_anchor.tripped.clear();
}

ClockAnchorFold clockAnchorFoldVariance(NetworkRunContext& ctx,
                                        const std::string& sat,
                                        double prev_var, double dt_epoch,
                                        double pl_now_s, double value,
                                        double seed_horizon_s) {
    ClockAnchorFold out;
    const double dt_leg = std::max(0.0, dt_epoch);
    // Off path: the legacy expression, character for character, so an unarmed run
    // cannot differ in the last bit.
    out.var_eff = prev_var + ctx.knobs.q_clock * dt_leg;
    if (!ctx.knobs.cta_on) return out;

    ClockAnchorSat& st = ctx.carry.clk_anchor.sat[sat];

    // Per-satellite elapsed time, not the driver's dt: every skip path advances
    // prev_tow while this carry survives untouched, so dt understates a stale
    // carry's age without bound.  A first sighting has no elapsed time and falls
    // back to dt (it is LOOSE in any case).
    const double dt_sat = (st.t_last > 0.0) ? (pl_now_s - st.t_last) : dt_leg;
    // Re-admission continuity: a parked pre-gap realization seeding a re-registered
    // column is stale only beyond the seed horizon.  The gap it spans is the evidence
    // gap the park holds, and the guard (not the staleness bound) re-validates it.
    const double gap_bound = (seed_horizon_s > ctx.knobs.cta_max_gap_s)
                                 ? seed_horizon_s : ctx.knobs.cta_max_gap_s;
    const bool stale = !(dt_sat > 0.0)
                    || (ctx.knobs.cta_max_gap_s > 0.0 && dt_sat > gap_bound);
    // A LOOSE seed inflates over the true gap, not the driver's dt (the legacy loose
    // expression is kept character for character when nothing is seeding).
    if (seed_horizon_s > 0.0 && dt_sat > 0.0)
        out.var_eff = prev_var + ctx.knobs.q_clock * dt_sat;
    if (stale && st.anchored) {
        // Detected here because it decides the variance.  The state transition,
        // the event and the wire mark belong to the guard, so every decoupling edge
        // goes through one path with one identity.
        st.stale_pending = true;
        out.decoupled    = true;
    }

    if (st.anchored && !st.stale_pending) {
        out.var_eff  = prev_var + ctx.knobs.cta_q * dt_sat;
        out.anchored = true;
        if (ctx.knobs.cta_var_floor > 0.0 && out.var_eff < ctx.knobs.cta_var_floor) {
            out.var_eff   = ctx.knobs.cta_var_floor;
            out.floor_hit = true;
            ++ctx.counters.cta_floor_hits;
        }
        ++ctx.counters.cta_folded_anchored;
        // The same count per constellation letter, for the run-end readout only
        // (card GAL).
        ++ctx.counters.mgnss.anchor_folds_anchored[sat.substr(0, 1)];
    } else {
        // LOOSE folds at the legacy rate over the legacy dt, the behaviour without
        // the anchor (a re-admission seed inflates over its true gap, above).  That
        // bounds a decoupled satellite (standing invariant #7): it is not
        // unconstrained, it is on the carry that the existing clock_runaway_check
        // monitor already bounds.
        ++ctx.counters.cta_folded_loose;
        ++ctx.counters.mgnss.anchor_folds_loose[sat.substr(0, 1)];   // per constellation (card GAL)
    }

    st.t_last          = pl_now_s;
    st.last_value      = value;
    st.last_var_eff    = out.var_eff;
    st.folded          = true;
    st.folded_anchored = out.anchored;
    return out;
}

// ---------------------------------------------------------------------------
// Guard side.
// ---------------------------------------------------------------------------

namespace {

/// The decoupling edge: state transition, event and wire mark, in one path.
/// Returns true when an edge fired (the satellite was ANCHORED).
bool decouple(NetworkRunContext& ctx, NetworkRunTraces& traces, const GnssTime& t,
              const std::string& sat, const char* reason,
              std::optional<double> statistic, std::optional<double> threshold,
              const std::string& detail) {
    ClockAnchorSat& st = ctx.carry.clk_anchor.sat[sat];
    const bool was_anchored = st.anchored;
    st.anchored = false;
    st.in_band  = 0;
    if (!was_anchored) return false;   // already LOOSE: no edge, nothing to mark
    ctx.carry.clk_anchor.tripped.insert(sat);
    ++ctx.counters.mgnss.anchor_trips[sat.substr(0, 1)];   // per constellation (card GAL)
    // Re-admission continuity: a trip within the verify window of a clock seed is the
    // data rejecting the seeded pre-gap realization (invariant #7's mark-if-moved),
    // attributed in the event detail and counted.
    std::string seed_tag;
    if (!ctx.carry.readmit_seeded_s.empty()) {
        const auto rs = ctx.carry.readmit_seeded_s.find(sat);
        if (rs != ctx.carry.readmit_seeded_s.end()
            && ctx.carry.readmit_now_s - rs->second <= kReadmitVerifyWindowS) {
            ++ctx.counters.n_readmit_seed_moved;
            seed_tag = ";readmit_seed_moved";
        }
    }
    traces.emitEvent(t, "CLK_ANCHOR", "cta_decouple", sat, "", statistic, threshold,
                     "refloat", fmt::format("reason={};{}{}", reason, detail, seed_tag));
    if (ctx.knobs.cta_mark) {
        ctx.carry.clk_anchor_disc_pending.insert(sat);
        ++ctx.counters.cta_marks;
    }
    return true;
}

}  // namespace

void networkClockAnchorGuard(NetworkRunContext& ctx, const GnssConfig& config_,
                             const std::shared_ptr<spdlog::logger>& log,
                             NetworkRunTraces& traces, const GnssTime& se_time,
                             NetworkEstimator& est,
                             const NetworkSbasis::Plan& plan,
                             Eigen::VectorXd& x_s, EpochWorkspace& ws,
                             const std::vector<std::string>& registered_clock_sats,
                             const std::set<std::string>& clock_refloat_sats,
                             bool datum_changed,
                             const std::set<char>& changed_consts) {
    if (!ctx.knobs.cta_on) return;
    (void)config_;   // the seam signature is uniform across the run loop
    auto& anchor = ctx.carry.clk_anchor;
    ++ctx.counters.cta_epochs;

    // --- Non-statistical decoupling causes, applied before the test ----------
    // In each case the carry this satellite would be tested against is not the
    // anchored carry, so testing it would only produce a misattributed statistic.
    for (const std::string& sat : registered_clock_sats) {
        const auto it = anchor.sat.find(sat);
        if (it == anchor.sat.end() || !it->second.stale_pending) continue;
        ++ctx.counters.cta_trip_gap;
        decouple(ctx, traces, se_time, sat, "gap", std::nullopt,
                 ctx.knobs.cta_max_gap_s,
                 "carried clock older than the staleness bound");
    }
    if (datum_changed) {
        for (const std::string& sat : registered_clock_sats) {
            const auto it = anchor.sat.find(sat);
            if (it == anchor.sat.end() || !it->second.anchored) continue;
            if (!datumChangeAffectsKey(sat, changed_consts, ctx.knobs.scoped_handover))
                continue;
            ++ctx.counters.cta_trip_datum;
            decouple(ctx, traces, se_time, sat, "datum", std::nullopt, std::nullopt,
                     "retired clock frame");
        }
    }
    for (const std::string& sat : clock_refloat_sats) {
        const auto it = anchor.sat.find(sat);
        if (it == anchor.sat.end() || !it->second.anchored) continue;
        ++ctx.counters.cta_trip_ext;
        decouple(ctx, traces, se_time, sat, "ext", std::nullopt, std::nullopt,
                 "external QC tier refloated this satellite");
    }

    // --- The test -----------------------------------------------------------
    const double k       = ctx.knobs.cta_k;
    const double abs_m   = ctx.knobs.cta_abs_m;
    const double s_floor = std::max(1e-12, ctx.knobs.cta_sigma_floor_m
                                         * ctx.knobs.cta_sigma_floor_m);
    // The resolved predicate, not the Q3 knob (card C-GF).  A withdraw must un-fold
    // exactly what was folded; reading a different flag than the fold used leaves a
    // negative-semidefinite residue in the shared normals (see NetworkRunContext.hpp).
    const bool gf         = ctx.knobs.carry_gauge_free;     // the fold (what a withdraw must un-fold)
    // Closure is part of what was folded: it selects which directions U spans, so
    // every withdraw below passes it alongside `gf` (card C-GF).
    const bool gfc        = ctx.knobs.carry_gauge_free_closure;
    const bool proj_post  = ctx.knobs.cta_stat_proj_post;   // the denominator's posterior half
    const bool proj_prior = ctx.knobs.cta_stat_proj_prior;  // …and its prior half
    int pass = 0;
    for (; pass < kCtaMaxPasses; ++pass) {
        const Eigen::MatrixXd& cov = est.sharedCovariance();
        struct Row { std::string sat; double e{}, S{}, post_var{}, var_eff{}; bool anchored{}; };
        std::vector<Row> rows;
        std::map<char, std::vector<double>> e_by_const;
        // The statistic is on the common-removed residual, so both halves of
        // S = P⁻ − P⁺ have to be the variance of that contrast rather than of the
        // raw state (card Q3 §2.1; @ref clockAnchorGaugeProject documents why, and
        // why the raw marginal is wrong for either fold).  Which halves are
        // projected is set by `network.clock_anchor_gauge_free_stat`, resolved into
        // the two predicates above.  It is a bisection knob: the gauge-free arm
        // changed the fold and this denominator in one step, and the knob separates
        // the two.
        std::map<std::string, double> post_gf, prior_gf;
        if (proj_post || proj_prior) {
            std::map<char, std::vector<std::pair<std::string, Eigen::Index>>> by_c;
            for (const std::string& sat : registered_clock_sats) {
                if (sat.empty()) continue;
                const auto it = anchor.sat.find(sat);
                if (it == anchor.sat.end() || !it->second.folded) continue;
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                if (col >= 0 && col < cov.rows() && col < cov.cols())
                    by_c[sat[0]].emplace_back(sat, col);
            }
            for (const auto& [c, v] : by_c) {
                (void)c;
                if (v.size() < 2) continue;   // no estimable contrast: keep the raw marginals
                if (proj_post) {
                    std::vector<Eigen::Index> cols;
                    cols.reserve(v.size());
                    for (const auto& m : v) cols.push_back(m.second);
                    const std::vector<double> pv = clockAnchorGaugeProject(cov, cols);
                    for (std::size_t a = 0; a < pv.size(); ++a) post_gf[v[a].first] = pv[a];
                }
                if (proj_prior) {
                    // The prior side uses the variance the fold applied, read back
                    // per satellite and not re-derived: a re-derived variance can
                    // differ from the one the mechanism applied.  The census makes
                    // the same choice.
                    std::vector<double> vv;
                    vv.reserve(v.size());
                    for (const auto& m : v) vv.push_back(anchor.sat[m.first].last_var_eff);
                    const std::vector<double> pv = clockAnchorGaugeProjectDiag(vv);
                    for (std::size_t a = 0; a < pv.size(); ++a) prior_gf[v[a].first] = pv[a];
                }
            }
        }
        rows.reserve(registered_clock_sats.size());
        for (const std::string& sat : registered_clock_sats) {
            if (sat.empty()) continue;
            const auto it = anchor.sat.find(sat);
            if (it == anchor.sat.end() || !it->second.folded) continue;  // no standing prior ⇒ no predicted residual
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            if (col < 0 || col >= x_s.size()) continue;
            const double z = x_s(col);
            if (!std::isfinite(z)) continue;
            Row r;
            r.sat      = sat;
            r.e        = z - it->second.last_value;
            r.var_eff  = it->second.last_var_eff;
            r.post_var = (col < cov.rows() && col < cov.cols()) ? cov(col, col) : 0.0;
            if (proj_post) {
                const auto pit = post_gf.find(sat);
                if (pit != post_gf.end() && std::isfinite(pit->second) && pit->second >= 0.0)
                    r.post_var = pit->second;
            }
            if (proj_prior) {
                const auto qit = prior_gf.find(sat);
                if (qit != prior_gf.end() && std::isfinite(qit->second) && qit->second > 0.0)
                    r.var_eff = qit->second;
            }
            // Var(x̂ − μ) = P⁻ − P⁺ exactly.  The floor is a lower bound on the
            // denominator, not an addition to it (adding it is the pre-B3 data-incest
            // form).  A weak-data epoch removes no information, so S → 0 and the
            // unfloored statistic would divide 0 by 0 on the lowest-confidence
            // satellites.
            r.S        = std::max(s_floor, r.var_eff - r.post_var);
            r.anchored = it->second.folded_anchored;
            if (!std::isfinite(r.e) || !std::isfinite(r.S)) continue;
            rows.push_back(r);
            e_by_const[sat[0]].push_back(r.e);
        }
        if (rows.empty()) break;
        // The per-constellation common mode is the free per-epoch clock gauge (rx
        // clocks are white, q = 1000).  Removing it is standing invariant #5:
        // without it every satellite of a re-realizing constellation reports the
        // gauge shift and the guard trips on all of them.
        std::map<char, double> med;
        for (auto& [c, v] : e_by_const) {
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                             v.end());
            med[c] = v[v.size() / 2];
        }

        std::set<std::string> trip;          // anchored satellites that failed
        std::set<std::string> trip_resolve;  // …whose prior may also be withdrawn
        for (const Row& r : rows) {
            const double eh = r.e - med[r.sat[0]];
            const double w  = eh / std::sqrt(r.S);
            const double aw = std::abs(w);
            if (aw > ctx.counters.cta_max_w) {
                ctx.counters.cta_max_w     = aw;
                ctx.counters.cta_max_w_tow = se_time.tow;
                ctx.counters.cta_max_w_sat = r.sat;
            }
            const bool bad_nis = aw > k;
            const bool bad_abs = abs_m > 0.0 && std::abs(eh) > abs_m;
            // The k derivation of §3 needs the realized distribution, not only its
            // max: the denominator √S shrinks as q tightens, so a fixed k means a
            // different number of metres at every rung of the ladder.  Accumulated
            // over anchored rows on the first pass only; a later pass re-tests the
            // survivors against a moved median and would count them twice.  The
            // population is still truncated across epochs by the guard's own action;
            // the untruncated null needs a k-disarmed arm (see the counter docs).
            if (pass == 0 && r.anchored) {
                const double sqrt_s = std::sqrt(r.S);
                ++ctx.counters.cta_w_rows;
                ctx.counters.cta_sum_sqrt_s += sqrt_s;
                ctx.counters.cta_sum_abs_e  += std::abs(eh);
                ctx.counters.cta_sum_w2     += w * w;
                for (std::size_t b = 0; b < kCtaWLadder.size(); ++b)
                    if (aw >= kCtaWLadder[b]) ++ctx.counters.cta_w_ge[b];
                for (std::size_t b = 0; b < kCtaELadder.size(); ++b)
                    if (std::abs(eh) >= kCtaELadder[b]) ++ctx.counters.cta_e_ge[b];
            }
            if (r.anchored) {
                if (!bad_nis && !bad_abs) continue;
                if (bad_nis) ++ctx.counters.cta_trip_nis;
                else         ++ctx.counters.cta_trip_abs;
                decouple(ctx, traces, se_time, r.sat, bad_nis ? "nis" : "abs", aw,
                         bad_nis ? k : abs_m,
                         fmt::format("e={:.4f};med={:.4f};sqrtS={:.4f};pass={}",
                                     r.e, med[r.sat[0]], std::sqrt(r.S), pass));
                trip.insert(r.sat);
                // Admission screen for the same-epoch value correction only; the
                // decouple and the mark are unconditional.
                const double redun = (r.var_eff > 0.0)
                                         ? (r.var_eff - r.post_var) / r.var_eff : 0.0;
                if (redun >= kCtaMinCarryRedundancy) trip_resolve.insert(r.sat);
            } else {
                // LOOSE: the invariant #7 re-validation.  The same statistic,
                // against the loose fold's own null: the question at this edge is
                // whether the event is over, not whether the clock is quiet at 2 cm
                // (the re-seed and the next anchored pass answer that).
                ClockAnchorSat& st = anchor.sat[r.sat];
                if (bad_nis || bad_abs) { st.in_band = 0; continue; }
                ++st.in_band;
                if (st.in_band >= std::max(1, ctx.knobs.cta_recouple_ep)) {
                    st.anchored = true;
                    st.in_band  = 0;
                    ++ctx.counters.cta_recouples;
                    // Not marked on the wire: re-engaging a prior whose mean is the
                    // last posterior adds information without stepping the emitted
                    // value, so a consumer has nothing to reset.  It is an event with
                    // an identity and its evidence, as invariant #7 asks of a
                    // re-coupling edge, and the next fold re-seeds from the LOOSE
                    // posterior variance instead of resuming a steady-state tight one.
                    traces.emitEvent(se_time, "CLK_ANCHOR", "cta_recouple", r.sat, "",
                                     aw, k, "reengage",
                                     fmt::format("in_band={};reseed_var={:.3e}",
                                                 std::max(1, ctx.knobs.cta_recouple_ep),
                                                 r.post_var));
                }
            }
        }
        // A tripped satellite is out of the test for the rest of the epoch: its
        // prior is either withdrawn below or knowingly left standing, and either
        // way re-testing it would re-count the same edge.
        for (const std::string& s : trip) anchor.sat[s].folded = false;
        if (trip.empty()) break;
        if (!ctx.knobs.cta_resolve) break;
        if (trip_resolve.empty()) {
            log->warn("Network (Card Q3): TOW {:.1f} — {} clock-anchor trip(s) [{}] decoupled "
                      "and marked, but their carry redundancy (P⁻−P⁺)/P⁻ is below {:.2f}: the "
                      "column is prior-dominated, so withdrawing the prior would leave it "
                      "near-unobservable.  The epoch emits the anchored value for those "
                      "satellites; they fold LOOSE from the next epoch.",
                      se_time.tow, trip.size(), joinSats(trip), kCtaMinCarryRedundancy);
            break;
        }

        // --- The value correction: withdraw and re-solve (adapt and emit) -----
        // Split the withdrawal by fold path.  A key in the C1 dense block is
        // marginalized out of the block (rows/cols dropped, block re-inverted); a
        // diagonal key is subtracted outright.  Both are exact inverses of what
        // was folded, which is why the capture records the folded (value, info)
        // instead of recomputing them.
        std::set<std::string> dense_drop;
        for (const auto& kk : ws.cap_dense_keys)
            if (kk.kind == SharedParamKind::Clock && trip_resolve.count(kk.sat))
                dense_drop.insert(kk.sat);
        std::vector<NetworkEstimator::SharedPrior> diag_drop;
        for (const auto& p : ws.cap_priors)
            if (p.kind == SharedParamKind::Clock && trip_resolve.count(p.sat)
                && !dense_drop.count(p.sat))
                diag_drop.push_back(p);
        if (dense_drop.empty() && diag_drop.empty()) {
            // The capture channel is the only record of what was folded; with it
            // unarmed there is nothing to withdraw exactly, and a guess would leave
            // residual prior information behind.  Log it and stop.
            log->warn("Network (Card Q3): TOW {:.1f} — {} clock-anchor trip(s) cannot be "
                      "value-corrected in-epoch: no captured clock prior to withdraw (the "
                      "RAIM-replay capture channel is unarmed for this fold).  Decoupled and "
                      "marked; the correction lands next epoch.",
                      se_time.tow, trip_resolve.size());
            break;
        }

        bool folded_dense = false;
        Eigen::VectorXd x_try;
        try {
            if (!dense_drop.empty()) {
                est.attribTag("cta_refloat");
                // The gauge-free flag has to match the fold, or the withdrawn block
                // is not the block that was applied.
                est.refoldSharedPriorDenseDroppingClocks(ws.cap_dense_keys,
                                                         ws.cap_dense_cov, dense_drop,
                                                         /*restore=*/false, gf, gfc);
                folded_dense = true;
            }
            if (!diag_drop.empty()) {
                est.attribTag("cta_refloat");
                // A gauge-free "diagonal" block is not diagonal: removing one key
                // moves the group's information-weighted mean, so every survivor's
                // anchor target changes.  Re-fold the group instead of subtracting
                // one entry.
                if (gf) {
                    std::set<std::string> ds;
                    for (const auto& p : diag_drop) ds.insert(p.sat);
                    est.refoldSharedPriorClockGaugeFreeDropping(ws.cap_priors, ds,
                                                                /*restore=*/false, gfc);
                } else {
                    est.withdrawSharedPrior(diag_drop);
                }
            }
            x_try = est.solve(plan.shared_datum);
        } catch (const std::exception& ex) {
            ++ctx.counters.cta_resolve_failed;
            log->warn("Network (Card Q3): TOW {:.1f} in-epoch clock refloat re-solve failed "
                      "({}); restoring the anchored solve.  The {} tripped satellite(s) stay "
                      "decoupled and marked — only the same-epoch VALUE correction is lost.",
                      se_time.tow, ex.what(), trip_resolve.size());
            try {
                if (folded_dense)
                    est.refoldSharedPriorDenseDroppingClocks(ws.cap_dense_keys,
                                                             ws.cap_dense_cov, dense_drop,
                                                             /*restore=*/true, gf, gfc);
                if (!diag_drop.empty()) {
                    if (gf) {
                        std::set<std::string> ds;
                        for (const auto& p : diag_drop) ds.insert(p.sat);
                        est.refoldSharedPriorClockGaugeFreeDropping(ws.cap_priors, ds,
                                                                    /*restore=*/true, gfc);
                    } else {
                        est.addSharedPrior(diag_drop);
                    }
                }
                x_s = est.solve(plan.shared_datum);   // refresh shared_cov_ consistently
            } catch (const std::exception& ex2) {
                log->error("Network (Card Q3): TOW {:.1f} the restore after a failed refloat "
                           "re-solve ALSO failed ({}) — this epoch's shared covariance no "
                           "longer matches its solution.",
                           se_time.tow, ex2.what());
            }
            break;
        }
        x_s = x_try;
        ++ctx.counters.cta_resolves;
        // Keep the RAIM replay channel in step: the within-epoch re-solve replays
        // cap_priors / cap_dense_* verbatim onto a fresh arrowhead, so a withdrawn
        // prior left in the capture would be re-injected.
        if (!diag_drop.empty()) {
            auto& cp = ws.cap_priors;
            cp.erase(std::remove_if(cp.begin(), cp.end(),
                                    [&](const NetworkEstimator::SharedPrior& p) {
                                        return p.kind == SharedParamKind::Clock
                                            && trip_resolve.count(p.sat) > 0;
                                    }),
                     cp.end());
        }
        if (!dense_drop.empty()) {
            // Marginalize the dropped keys out of the captured block too, so a
            // replay re-folds the same reduced block this solve used.
            std::vector<Eigen::Index> keep;
            keep.reserve(ws.cap_dense_keys.size());
            for (std::size_t i = 0; i < ws.cap_dense_keys.size(); ++i) {
                const auto& kk = ws.cap_dense_keys[i];
                if (kk.kind == SharedParamKind::Clock && dense_drop.count(kk.sat)) continue;
                keep.push_back(static_cast<Eigen::Index>(i));
            }
            std::vector<NetworkEstimator::SharedPrior> nk;
            Eigen::MatrixXd nc(static_cast<Eigen::Index>(keep.size()),
                               static_cast<Eigen::Index>(keep.size()));
            nk.reserve(keep.size());
            for (std::size_t a = 0; a < keep.size(); ++a) {
                nk.push_back(ws.cap_dense_keys[static_cast<std::size_t>(keep[a])]);
                for (std::size_t b = 0; b < keep.size(); ++b)
                    nc(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
                        ws.cap_dense_cov(keep[a], keep[b]);
            }
            ws.cap_dense_keys = std::move(nk);
            ws.cap_dense_cov  = std::move(nc);
        }
        log->info("Network (Card Q3): TOW {:.1f} clock-anchor refloat — {} satellite(s) [{}] "
                  "withdrawn and the epoch RE-SOLVED before emission (pass {}).",
                  se_time.tow, trip_resolve.size(), joinSats(trip_resolve), pass + 1);
    }
    if (pass >= kCtaMaxPasses)
        log->warn("Network (Card Q3): TOW {:.1f} hit the {}-pass in-epoch refloat cap — any "
                  "further clock-anchor trip this epoch is decoupled and marked but not "
                  "value-corrected.  A run that reaches this cap regularly is telling you the "
                  "anchor q or k is mis-specified, not that the data has that many faults.",
                  se_time.tow, kCtaMaxPasses);
}

}  // namespace GnssProcessor
