/**
 * @file CouplingExact.cpp
 * @brief The exact conditional time update of the carried coupling rows; the
 *        derivation and the index spaces are in the header.
 */

#include "network/carry/CouplingExact.hpp"

#include <algorithm>
#include <cmath>

// The run-end reports at the end of this file:
#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace GnssProcessor {

bool couplingExactFactor(CouplingExactEpoch& ep) {
    ep.ok = false;
    const Eigen::Index n = ep.nS();
    if (ep.q_s.size() != n || ep.shat_s.size() != n
        || static_cast<Eigen::Index>(ep.col_of_s.size()) != n)
        return false;
    if (n == 0) {            // nothing carried with a finite inflation: every term re-attributes to nothing
        ep.F.resize(0, 0);
        ep.ok = true;
        return true;
    }
    Eigen::MatrixXd A(n, n);
    for (Eigen::Index a = 0; a < n; ++a) {
        const int pa = ep.p_of_s[static_cast<std::size_t>(a)];
        for (Eigen::Index b = 0; b < n; ++b) {
            const int pb = ep.p_of_s[static_cast<std::size_t>(b)];
            A(a, b) = 0.5 * (ep.P(pa, pb) + ep.P(pb, pa));
        }
        A(a, a) += ep.q_s(a);
    }
    for (const auto& [a, b, v] : ep.q_off)   // the seam's level noise
        if (a >= 0 && b >= 0 && a < n && b < n) A(a, b) += v;
    if (!A.allFinite()) return false;
    // Correlation scaling, as the dense fold inverts its block: the matrix mixes
    // phase-stiff entries (~1e-9 m^2) with loose clock entries (~1e-2 m^2).
    Eigen::VectorXd dinv(n);
    for (Eigen::Index a = 0; a < n; ++a) {
        if (!(A(a, a) > 0.0)) return false;
        dinv(a) = 1.0 / std::sqrt(A(a, a));
    }
    const Eigen::MatrixXd C = dinv.asDiagonal() * A * dinv.asDiagonal();
    const Eigen::LDLT<Eigen::MatrixXd> f(C);
    if (f.info() != Eigen::Success || !f.isPositive()) return false;
    // A zero pivot slips through isPositive() on a semidefinite matrix.
    const Eigen::VectorXd dvec = f.vectorD();
    const double dmax = dvec.cwiseAbs().maxCoeff();
    if (!(dmax > 0.0) || !(dvec.minCoeff() > 1e-14 * dmax)) return false;
    ep.F = dinv.asDiagonal() * f.solve(Eigen::MatrixXd::Identity(n, n)) * dinv.asDiagonal();
    ep.F = 0.5 * (ep.F + ep.F.transpose()).eval();
    if (!ep.F.allFinite()) return false;
    ep.ok = true;
    return true;
}

CouplingExactRow couplingExactRow(const CouplingExactEpoch& ep,
                                  const std::vector<std::pair<int, double>>& terms) {
    CouplingExactRow r;
    const Eigen::Index nS = ep.nS();
    r.m_s = Eigen::VectorXd::Zero(nS);
    r.u_d = Eigen::VectorXd::Zero(nS);
    r.z   = Eigen::VectorXd::Zero(nS);
    if (!ep.ok) return r;
    std::map<int, double> md;                        // D terms, a repeated P row summed
    std::map<char, double> clk_old;
    for (const auto& [p, c] : terms) {
        if (p < 0 || p >= static_cast<int>(ep.s_of_p.size()) || c == 0.0 || !std::isfinite(c))
            continue;
        if (ep.clk_p[static_cast<std::size_t>(p)])
            clk_old[ep.cc_p[static_cast<std::size_t>(p)]] += c;
        const int s = ep.s_of_p[static_cast<std::size_t>(p)];
        if (s >= 0) {
            r.m_s(s) += c;
            ++r.n_s;
        } else {
            md[p] += c;
            const int route = ep.route_p[static_cast<std::size_t>(p)];
            if (route >= 0 && route < kCouplingExactRoutes) ++r.n_d_route[route];
        }
    }
    r.m_d.assign(md.begin(), md.end());
    // g = Q_S M_S; Q_S is diagonal except on a switch-on seam's epoch (q_off), where the
    // level noise couples a satellite's clock and phase-bias columns.  Every formula below
    // holds for a general symmetric Q_S.
    Eigen::VectorXd gd = ep.q_s.cwiseProduct(r.m_s);
    for (const auto& [a, b, v] : ep.q_off)
        if (a >= 0 && b >= 0 && a < nS && b < nS) gd(a) += v * r.m_s(b);
    for (Eigen::Index s = 0; s < nS; ++s) {
        const double gv = gd(s);
        if (gv != 0.0) r.g.emplace_back(static_cast<int>(s), gv);
    }
    // m_new = M_S - F g + z, with z = F u_d and u_d = P_{S,D} M_D.
    r.m_new = r.m_s;
    for (const auto& [s, gv] : r.g) r.m_new.noalias() -= ep.F.col(s) * gv;
    if (!r.m_d.empty() && nS > 0) {
        for (const auto& [p, c] : r.m_d)
            for (Eigen::Index a = 0; a < nS; ++a)
                r.u_d(a) += ep.P(ep.p_of_s[static_cast<std::size_t>(a)], p) * c;
        r.z.noalias() = ep.F * r.u_d;
        r.m_new += r.z;
    }
    // M'BM = m_new'g + g'z + (M_D' P_DD M_D - u_d'z)
    double t = 0.0;
    for (const auto& [s, gv] : r.g) t += (r.m_new(s) + r.z(s)) * gv;
    double dd = 0.0;
    for (const auto& [p1, c1] : r.m_d)
        for (const auto& [p2, c2] : r.m_d) dd += c1 * c2 * ep.P(p1, p2);
    if (nS > 0) dd -= r.u_d.dot(r.z);
    r.var_raw = t + dd;
    r.var_add = std::max(0.0, r.var_raw);
    // The clock-family deficit per constellation (the pinned pivot takes it).
    for (const auto& [cc, s0] : clk_old) r.clk_deficit[cc] += s0;
    for (Eigen::Index s = 0; s < nS; ++s) {
        const int p = ep.p_of_s[static_cast<std::size_t>(s)];
        if (ep.clk_p[static_cast<std::size_t>(p)])
            r.clk_deficit[ep.cc_p[static_cast<std::size_t>(p)]] -= r.m_new(s);
    }
    r.ok = r.m_new.allFinite() && r.z.allFinite() && std::isfinite(r.var_raw);
    return r;
}

double couplingExactCross(const CouplingExactEpoch& ep, const CouplingExactRow& a,
                          const CouplingExactRow& b) {
    if (!a.ok || !b.ok) return 0.0;
    // M_a'BM_b = m_new,a'g_b + g_a'z_b + (M_D,a' P_DD M_D,b - u_d,a'z_b)
    double t = 0.0;
    for (const auto& [s, gv] : b.g) t += a.m_new(s) * gv;
    for (const auto& [s, gv] : a.g) t += b.z(s) * gv;
    double dd = 0.0;
    for (const auto& [p1, c1] : a.m_d)
        for (const auto& [p2, c2] : b.m_d) dd += c1 * c2 * ep.P(p1, p2);
    if (ep.nS() > 0) dd -= a.u_d.dot(b.z);
    return t + dd;
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the exact coupling time update (CTU, WSX, SCL, SCL2).
int networkCouplingExactRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    if (!ctx.knobs.carry_cpl_exact) return 0;
    // Run-end telemetry (invariant 4).  `rows exact = 0` means the rule never
    // ran and every row took the scalar kappa, however clean the rest reads.  The rows
    // and the fold read one decision per key; the fold line counts
    // what a clock-pivot handover changed after the rows were built.
    const auto& c  = ctx.counters;
    const auto& X  = c.cxu;
    using Rt = CouplingExactDRoute;
    const auto d = [&](Rt r) { return X.terms_d[static_cast<int>(r)]; };
    const auto kd = [&](Rt r) { return c.cxu_keys_d[static_cast<int>(r)]; };
    log->info("Network (exact coupling time update): {} epoch(s), {} built, "
              "{} unfactored, {} without a valid stash; keys {} in S, D {} departed / {} "
              "reinit / {} reref / {} skipped; {} negative inflation(s) clamped.",
              c.cxu_epochs, c.cxu_epochs_built, c.cxu_epochs_unfactored,
              c.cxu_epochs_nostash, c.cxu_keys_s, kd(Rt::Departed), kd(Rt::Reinit),
              kd(Rt::Reref), kd(Rt::Skipped), c.cxu_q_negative);
    log->info("Network: rows {} exact, {} stale (coupling-carry route), {} on an "
              "unbuilt epoch; terms {} updated in S, re-attributed {} departed / {} reinit "
              "/ {} reref / {} skipped, {} pinned, {} scalar outside the block, {} "
              "marginalized outside, {} SH; {} coefficient(s) written; clock-family "
              "deficit {} to the pivot, {} spread over present clocks (pivot left), {} ORPHAN; inflation "
              "mass {:.4e} m^2 ({} clamped); |M - M_new| mass {:.4e}; s_prev mismatches {}.",
              X.rows_exact, X.rows_stale, X.rows_unfactored, X.terms_s,
              d(Rt::Departed), d(Rt::Reinit), d(Rt::Reref), d(Rt::Skipped), X.terms_pinned,
              X.terms_out_scalar, X.terms_out_departed, X.terms_out_sh, X.terms_emitted,
              X.gauge_held, X.gauge_spread, X.gauge_orphan, X.var_add_sum, X.var_negative,
              X.shrink_abs_sum, X.sprev_mismatch);
    log->info("Network: the rows and the fold read one decision per key"
              "; {} handover epoch(s), on which {} key(s) in S were folded on "
              "a variance the S-transform re-expressed after the rows were built and {} "
              "key(s) in S were retired; {} dense-fold diagonal fallback(s), {} epoch(s) "
              "with no recorded clock pivot.",
              c.cxu_fold_handover_epochs, c.cxu_fold_reexpressed_in_s,
              c.cxu_fold_retired_in_s, c.cxu_fold_diag_fallback, c.cxu_pivot_missing);
    if (X.rows_exact == 0)
        log->warn("Network: network.carry_coupling_exact_update is ARMED but no "
                  "row took the exact route: the exact update is INERT for this run.");
    if (ctx.knobs.carry_cpl_exact_cross) {
        // The within-station bracket, its reach and its size.
        const auto hist = [&X](int cls) {
            std::string s;
            for (int b = 0; b < CouplingExactCounters::kCrossRhoBins; ++b)
                s += (b ? " " : "") + std::to_string(X.cross_rho_hist[cls][b]);
            return s;
        };
        log->info("Network (within-station bracket): {} block(s), {} pair(s) took "
                  "M_i'BM_k, {} pair(s) skipped (a row off the exact route); |M_i'BM_k| mass "
                  "{:.4e} m^2; |rho| against the pair's diagonal by bin (<0.01 <0.03 <0.1 <0.2 "
                  "<0.3 <0.5 <0.8 >=0.8): ambiguity pairs [{}] max {:.3f}, pairs with a "
                  "receiver code bias [{}] max {:.3f}.",
                  X.cross_blocks, X.cross_pairs, X.cross_pairs_skipped, X.cross_abs_sum,
                  hist(0), X.cross_rho_max[0], hist(1), X.cross_rho_max[1]);
        if (X.cross_pairs == 0)
            log->warn("Network: network.carry_coupling_exact_cross is ARMED but no "
                      "pair took the term (no whitened local block, or no two exact rows in "
                      "one): the knob is INERT for this run.");
    }
    networkClockCloneRunEnd(ctx, log);   // the clock clone's run-end line and INERT warning; no-op off
    networkSharedHistoryRunEnd(ctx, log);   // the shared history's run-end line and INERT warning; no-op off
    if (c.cxu_fold_reexpressed_in_s > 0 || c.cxu_fold_retired_in_s > 0)
        log->warn("Network: at a clock-pivot handover the fold folded {} key(s) "
                  "of S on a re-expressed variance and retired {}, after the coupling rows "
                  "were built on the decided ones; those rows were not conditioned on the "
                  "fold's Q at that epoch (the rows' re-expression at a handover is owed to "
                  "parameter-registry phase 2).",
                  c.cxu_fold_reexpressed_in_s, c.cxu_fold_retired_in_s);
    return 0;
}

// ---- End of the run-end reports ----

// ---- PASS B serial tallies (the produce wrote each station's slot; these fold the slots into the run) ----

void networkCouplingExactTally(NetworkRunContext& ctx,
                               const std::vector<StationAssembly>& asm_res) {
    // Fold each station's exact-update counts into the run totals (serial; the
    // produce wrote only its own slot).
    if (ctx.knobs.carry_cpl_exact)
        for (const StationAssembly& Rc : asm_res) ctx.counters.cxu.add(Rc.cxu);

}

// ---- End of the PASS B serial tallies ----

}  // namespace GnssProcessor
