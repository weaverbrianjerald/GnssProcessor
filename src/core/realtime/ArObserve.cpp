/**
 * @file ArObserve.cpp
 * @brief The consumer's observe-mode integer search: SD formation, the two searches, the
 *        conditioning, the rows and the trace formatters.
 *
 * See ArObserve.hpp for the algebra.  Everything here is a pure function of its arguments.
 */

#include "realtime/ArObserve.hpp"

#include "common/GnssIdentity.hpp"            // letterOfSat, systemOfSat, SignalId
#include "products/NetworkProductEmitter.hpp" // NetworkSolutionEpoch (absorbWireChain)

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>

namespace GnssProcessor {

// ===========================================================================
// Wire declarations
// ===========================================================================

void absorbWireChain(WireChainContext& ctx, const NetworkSolutionEpoch& epoch) {
    for (const NetworkSatSolution& s : epoch.sats) {
        // A satellite whose epoch record carries phase biases replaces its previous
        // entries wholesale, as the store replaces its phase-bias record; a satellite
        // with a clock record only leaves the phase biases it had.
        if (s.phase_bias.empty()) continue;
        const std::string prefix = s.sat + "|";
        auto it = ctx.cmp.lower_bound(prefix);
        while (it != ctx.cmp.end() && it->first.compare(0, prefix.size(), prefix) == 0)
            it = ctx.cmp.erase(it);
        for (const auto& [signal, pb] : s.phase_bias)
            if (pb.chain_component != 0) ctx.cmp[prefix + signal] = pb.chain_component;
    }
}

ArSearchMode arSearchModeOf(const std::string& name) {
    return name == "decorrelated_partial" ? ArSearchMode::DecorrelatedPartial : ArSearchMode::Full;
}

const char* arSearchModeName(ArSearchMode mode) {
    return mode == ArSearchMode::DecorrelatedPartial ? "decorrelated_partial" : "full";
}

// ===========================================================================
// The search
// ===========================================================================

namespace {

/// Solve A X = B for a symmetric positive definite A.
/// @return false when A is empty, not finite, not positive definite, or X is not finite.
bool solveSpd(const Eigen::MatrixXd& A, const Eigen::MatrixXd& B, Eigen::MatrixXd& X) {
    if (A.rows() == 0 || A.rows() != A.cols() || A.rows() != B.rows()) return false;
    if (!A.allFinite() || !B.allFinite()) return false;
    const Eigen::LDLT<Eigen::MatrixXd> ldlt(A);
    if (ldlt.info() != Eigen::Success || !(ldlt.vectorD().minCoeff() > 0.0)) return false;
    X = ldlt.solve(B);
    return X.allFinite();
}

/// Nearest integer of a finite value; 0 for a value no integer type holds.
long long nearestInt(double v) {
    return (std::isfinite(v) && std::fabs(v) < 9.0e18) ? std::llround(v) : 0LL;
}

/// The conditional floats and position of one epoch.
struct Conditioned {
    Eigen::VectorXd a;                 ///< Conditional floats (cycles).
    Eigen::MatrixXd Q;                 ///< Their covariance (cycles^2).
    Eigen::Vector3d x_pos;             ///< The conditioned position.
    std::vector<char> kept;            ///< Per SD: the search accepted it.
    int  z_fixed{ 0 };
    bool accepted{ false };
    bool conditioned{ false };
    bool failed{ false };
};

/// Full search: condition on the kept subset K,
///   a_c = a - Q_aK Q_KK^-1 (a_K - afix_K),  Q_c = Q - Q_aK Q_KK^-1 Q_Ka,
///   x_c = x_pos - Q_xK Q_KK^-1 (a_K - afix_K),
/// with the kept entries pinned to their integers and their rows and columns of Q_c zero.
Conditioned conditionFull(const ils::IlsResult& fix, const Eigen::VectorXd& a,
                          const Eigen::MatrixXd& Q, const Eigen::MatrixXd& Qxa,
                          const Eigen::Vector3d& x_pos, bool have_pos) {
    const int n = static_cast<int>(a.size());
    Conditioned c;
    c.a = a;
    c.Q = Q;
    c.x_pos = x_pos;
    c.kept.assign(static_cast<std::size_t>(n), 0);
    const int m = static_cast<int>(fix.kept.size());
    if (!fix.ok || m == 0 || fix.afix.size() != m) return c;
    c.accepted = true;
    c.z_fixed = m;
    for (int j = 0; j < m; ++j) {
        const int kj = fix.kept[static_cast<std::size_t>(j)];
        if (kj < 0 || kj >= n) { c.accepted = false; c.z_fixed = 0; c.kept.assign(static_cast<std::size_t>(n), 0); return c; }
        c.kept[static_cast<std::size_t>(kj)] = 1;
    }
    Eigen::MatrixXd Qkk(m, m), Qak(n, m), QxK = Eigen::MatrixXd::Zero(3, m);
    Eigen::VectorXd r(m);
    for (int j = 0; j < m; ++j) {
        const int kj = fix.kept[static_cast<std::size_t>(j)];
        r(j) = a(kj) - fix.afix(j);
        for (int i = 0; i < m; ++i) Qkk(i, j) = Q(fix.kept[static_cast<std::size_t>(i)], kj);
        Qak.col(j) = Q.col(kj);
        if (have_pos) QxK.col(j) = Qxa.col(kj);
    }
    Eigen::MatrixXd rhs(m, n + 1);
    rhs.leftCols(n) = Qak.transpose();
    rhs.col(n) = r;
    Eigen::MatrixXd sol;
    if (!solveSpd(Qkk, rhs, sol)) {
        c.failed = true;
        for (int j = 0; j < m; ++j) c.a(fix.kept[static_cast<std::size_t>(j)]) = fix.afix(j);
        return c;
    }
    const Eigen::MatrixXd W = sol.leftCols(n);   // Q_KK^-1 Q_Ka
    const Eigen::VectorXd w = sol.col(n);        // Q_KK^-1 (a_K - afix_K)
    c.a = a - Qak * w;
    c.Q = Q - Qak * W;
    c.Q = 0.5 * (c.Q + c.Q.transpose());
    if (have_pos) c.x_pos = x_pos - QxK * w;
    c.conditioned = true;
    for (int j = 0; j < m; ++j) {
        const int kj = fix.kept[static_cast<std::size_t>(j)];
        c.a(kj) = fix.afix(j);
        c.Q.row(kj).setZero();
        c.Q.col(kj).setZero();
    }
    return c;
}

/// Decorrelated search: the integer search returns the floats conditioned on the fixed
/// decorrelated components; the position conditioned on the same components is
///   x_c = x_pos + Q_xa Q_aa^-1 (a_cond - a_hat).
Conditioned conditionDecorrelated(const ils::IlsResult& fix, const Eigen::VectorXd& a,
                                  const Eigen::MatrixXd& Q, const Eigen::MatrixXd& Qxa,
                                  const Eigen::Vector3d& x_pos, bool have_pos) {
    const int n = static_cast<int>(a.size());
    Conditioned c;
    c.a = a;
    c.Q = Q;
    c.x_pos = x_pos;
    c.kept.assign(static_cast<std::size_t>(n), 0);
    const bool have_cond = fix.z_fixed > 0 && fix.a_cond.size() == n
                        && fix.Q_cond.rows() == n && fix.Q_cond.cols() == n;
    if (!have_cond) return c;
    c.accepted = true;
    c.z_fixed = fix.z_fixed;
    c.a = fix.a_cond;
    c.Q = fix.Q_cond;
    for (int k : fix.kept)
        if (k >= 0 && k < n) c.kept[static_cast<std::size_t>(k)] = 1;
    if (have_pos) {
        Eigen::MatrixXd sol;
        if (solveSpd(Q, c.a - a, sol)) {
            c.x_pos = x_pos + Qxa * sol.col(0);
            c.conditioned = true;
        } else {
            c.failed = true;
        }
    }
    return c;
}

} // namespace

ArSdProblem formArSdProblem(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                            const std::vector<ArCandidate>& candidates,
                            const ArObserveOptions& opt) {
    ArSdProblem prob;
    const int pos = opt.position_index;
    const bool have_pos = pos >= 0 && x.size() >= pos + 3 && P.rows() >= pos + 3
                       && P.cols() >= pos + 3;

    // --- Candidates that can form a difference, in group order --------------------------
    std::vector<int> order;
    order.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const ArCandidate& c = candidates[i];
        const bool ok = c.state_idx >= 0 && c.state_idx < x.size() && c.state_idx < P.rows()
                     && c.state_idx < P.cols() && std::isfinite(c.lambda_m) && c.lambda_m > 0.0
                     && !c.sat.empty() && !c.signal.empty();
        if (ok) order.push_back(static_cast<int>(i));
    }
    auto groupKey = [&](int i) {
        const ArCandidate& c = candidates[static_cast<std::size_t>(i)];
        return std::make_tuple(letterOfSat(c.sat), c.signal,
                               opt.split_by_component ? c.cmp : std::uint32_t{ 0 });
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto ka = groupKey(a), kb = groupKey(b);
        if (ka != kb) return ka < kb;
        return candidates[static_cast<std::size_t>(a)].sat < candidates[static_cast<std::size_t>(b)].sat;
    });

    // --- One reference per group (the highest elevation), one SD per other member -------
    for (std::size_t g = 0; g < order.size();) {
        std::size_t h = g + 1;
        while (h < order.size() && groupKey(order[h]) == groupKey(order[g])) ++h;
        if (h - g >= 2) {
            int ref = order[g];
            for (std::size_t k = g + 1; k < h; ++k)
                if (candidates[static_cast<std::size_t>(order[k])].elevation_deg
                    > candidates[static_cast<std::size_t>(ref)].elevation_deg)
                    ref = order[k];
            for (std::size_t k = g; k < h; ++k)
                if (order[k] != ref) { prob.cand.push_back(order[k]); prob.ref.push_back(ref); }
        }
        g = h;
    }
    const int n = static_cast<int>(prob.cand.size());
    prob.a = Eigen::VectorXd::Zero(n);
    prob.Q = Eigen::MatrixXd::Zero(n, n);
    prob.Qxa = Eigen::MatrixXd::Zero(3, n);
    if (n == 0) return prob;

    // --- The float, its covariance and the position cross-covariance --------------------
    auto cand = [&](int i) -> const ArCandidate& {
        return candidates[static_cast<std::size_t>(prob.cand[static_cast<std::size_t>(i)])];
    };
    auto ref = [&](int i) -> const ArCandidate& {
        return candidates[static_cast<std::size_t>(prob.ref[static_cast<std::size_t>(i)])];
    };
    for (int i = 0; i < n; ++i) {
        const int ii = cand(i).state_idx, ir = ref(i).state_idx;
        const double li = cand(i).lambda_m;
        prob.a(i) = (x(ii) - x(ir)) / li;
        if (have_pos)
            for (int d = 0; d < 3; ++d)
                prob.Qxa(d, i) = (P(pos + d, ii) - P(pos + d, ir)) / li;
        for (int j = 0; j < n; ++j) {
            const int ji = cand(j).state_idx, jr = ref(j).state_idx;
            prob.Q(i, j) = (P(ii, ji) - P(ii, jr) - P(ir, ji) + P(ir, jr))
                         / (li * cand(j).lambda_m);
        }
    }
    prob.Q = 0.5 * (prob.Q + prob.Q.transpose());
    return prob;
}

namespace {

/// The band rows: one per SD, from the conditional floats.
void appendBandRows(ArObserveEpoch& out, const ArSdProblem& prob,
                    const std::vector<ArCandidate>& candidates, const Conditioned& cond,
                    const ArObserveOptions& opt) {
    const bool full = (opt.mode == ArSearchMode::Full);
    const int n = static_cast<int>(prob.a.size());
    for (int i = 0; i < n; ++i) {
        const ArCandidate& ci = candidates[static_cast<std::size_t>(prob.cand[static_cast<std::size_t>(i)])];
        const ArCandidate& cr = candidates[static_cast<std::size_t>(prob.ref[static_cast<std::size_t>(i)])];
        ArFixRow row;
        row.system = letterOfSat(ci.sat);
        row.signal = ci.signal;
        row.sat = ci.sat;
        row.ref_sat = cr.sat;
        row.cmp = ci.cmp;
        row.float_cyc = prob.a(i);
        const double v = cond.a(i);
        const double sigma = std::sqrt(std::max(0.0, cond.Q(i, i)));
        row.cond_sigma_cyc = sigma;
        row.fixed_int = nearestInt(v);
        const bool kept = cond.kept[static_cast<std::size_t>(i)] != 0;
        row.kept = kept;
        row.determined = full ? kept : (std::isfinite(v) && sigma <= opt.determined_sigma_cyc);
        row.gen_sat = ci.hold_generation;
        row.gen_ref = cr.hold_generation;
        out.rows.push_back(std::move(row));
    }
}

/// The wide-lane rows: a satellite pair with an SD in each of its two lowest bands gets one
/// row, SD_b1 - SD_b2, from the same conditional floats and covariance.
void appendWideLaneRows(ArObserveEpoch& out, const ArSdProblem& prob, const Conditioned& cond,
                        const ArObserveOptions& opt) {
    const int n = static_cast<int>(prob.a.size());
    struct Band { char digit; std::string signal; int entry; };
    std::map<std::string, std::vector<Band>> pairs;   // "<letter>|<sat>|<ref>|<cmp>" -> its bands
    for (int i = 0; i < n; ++i) {
        const ArFixRow& r = out.rows[static_cast<std::size_t>(i)];
        const auto sig = SignalId::fromObsCode(systemOfSat(r.sat), r.signal);
        if (!sig) continue;
        pairs[fmt::format("{}|{}|{}|{}", r.system, r.sat, r.ref_sat, r.cmp)]
            .push_back({ sig->digit, r.signal, i });
    }
    for (auto& [key, bands] : pairs) {
        (void)key;
        std::sort(bands.begin(), bands.end(), [](const Band& p, const Band& q) {
            return std::tie(p.digit, p.signal) < std::tie(q.digit, q.signal);
        });
        std::size_t second = 1;
        while (second < bands.size() && bands[second].digit == bands[0].digit) ++second;
        if (second >= bands.size()) continue;                 // one band only
        const int i = bands[0].entry, j = bands[second].entry;
        const ArFixRow r1 = out.rows[static_cast<std::size_t>(i)];
        ArFixRow wl;
        wl.system = r1.system;
        wl.signal = "WL";
        wl.sat = r1.sat;
        wl.ref_sat = r1.ref_sat;
        wl.cmp = r1.cmp;
        wl.float_cyc = prob.a(i) - prob.a(j);
        const double v = cond.a(i) - cond.a(j);
        const double var = cond.Q(i, i) + cond.Q(j, j) - 2.0 * cond.Q(i, j);
        wl.cond_sigma_cyc = std::sqrt(std::max(0.0, var));
        wl.fixed_int = nearestInt(v);
        wl.determined = std::isfinite(v) && wl.cond_sigma_cyc <= opt.determined_sigma_cyc;
        wl.kept = wl.determined
               && std::fabs(v - static_cast<double>(wl.fixed_int)) <= opt.gates.nl_max_per_pair_frac;
        wl.gen_sat = r1.gen_sat;
        wl.gen_ref = r1.gen_ref;
        out.rows.push_back(std::move(wl));
    }
}

} // namespace

ArObserveEpoch observeIntegerSearch(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                    const std::vector<ArCandidate>& candidates,
                                    const ArObserveOptions& opt) {
    ArObserveEpoch out;
    out.valid = true;
    ArEpochSummary& sum = out.summary;

    const int pos = opt.position_index;
    const bool have_pos = pos >= 0 && x.size() >= pos + 3 && P.rows() >= pos + 3
                       && P.cols() >= pos + 3;
    if (have_pos) sum.float_ecef = x.segment<3>(pos);
    sum.fix_ecef = sum.float_ecef;

    const ArSdProblem prob = formArSdProblem(x, P, candidates, opt);
    const int n = static_cast<int>(prob.a.size());
    sum.n_entries = n;
    if (n == 0) return out;

    // An unconverged float is not searched (see the file banner): the epoch keeps its pairs'
    // count at zero, its float position and no rows.
    if (opt.max_position_sigma_m > 0.0 && have_pos) {
        const double sigma_3d = std::sqrt(std::max(P(pos, pos), 0.0) + std::max(P(pos + 1, pos + 1), 0.0)
                                          + std::max(P(pos + 2, pos + 2), 0.0));
        if (sigma_3d > opt.max_position_sigma_m) {
            sum.gated = true;
            return out;
        }
    }
    std::map<std::string, int> distinct_pairs;   // "<letter>|<sat>|<ref>" -> 1
    for (int i = 0; i < n; ++i) {
        const ArCandidate& ci = candidates[static_cast<std::size_t>(prob.cand[static_cast<std::size_t>(i)])];
        const ArCandidate& cr = candidates[static_cast<std::size_t>(prob.ref[static_cast<std::size_t>(i)])];
        distinct_pairs[fmt::format("{}|{}|{}", letterOfSat(ci.sat), ci.sat, cr.sat)] = 1;
    }
    sum.n_pairs = static_cast<int>(distinct_pairs.size());

    // The integer search, then the floats and the position conditioned on what it fixed.
    ils::IlsResult fix;
    try {
        fix = (opt.mode == ArSearchMode::DecorrelatedPartial)
            ? ils::ilsFixDecorrelatedPartial(prob.a, prob.Q, opt.gates, opt.determined_sigma_cyc)
            : ils::ilsFix(prob.a, prob.Q, opt.gates);
    } catch (...) {
        fix = ils::IlsResult();
    }
    out.fix = fix;
    const Eigen::Vector3d x_pos = sum.float_ecef;
    const Conditioned cond = (opt.mode == ArSearchMode::DecorrelatedPartial)
        ? conditionDecorrelated(fix, prob.a, prob.Q, prob.Qxa, x_pos, have_pos)
        : conditionFull(fix, prob.a, prob.Q, prob.Qxa, x_pos, have_pos);
    sum.z_total = n;
    sum.z_fixed = cond.z_fixed;
    sum.ratio = fix.ratio > 0.0 ? fix.ratio : fix.full_ratio;
    sum.p_s = fix.p_succ > 0.0 ? fix.p_succ : fix.full_p_succ;
    sum.accepted = cond.accepted;
    sum.conditioned = cond.conditioned;
    sum.cond_failed = cond.failed;
    sum.fix_ecef = cond.x_pos;

    appendBandRows(out, prob, candidates, cond, opt);
    appendWideLaneRows(out, prob, cond, opt);
    for (const ArFixRow& r : out.rows) {
        if (!(r.determined && r.kept)) continue;
        if (r.signal == "WL") ++sum.n_wl_det; else ++sum.n_band_det;
    }
    return out;
}

// ===========================================================================
// Run totals
// ===========================================================================

void accumulateArObserve(ArObserveCounters& c, const ArObserveEpoch& epoch,
                         long long n_candidates, long long n_eligible) {
    if (!epoch.valid) return;
    ++c.epochs;
    c.candidates += n_candidates;
    c.eligible += n_eligible;
    c.gated_out += n_candidates - n_eligible;
    if (epoch.summary.gated) ++c.epochs_gated;
    else if (epoch.summary.n_entries > 0) ++c.epochs_searched;
    if (epoch.summary.accepted) ++c.epochs_accepted;
    c.sd_pairs += epoch.summary.n_pairs;
    for (const ArFixRow& r : epoch.rows) {
        const bool wl = (r.signal == "WL");
        if (wl) ++c.wl_rows; else ++c.band_rows;
        if (r.determined) ++c.determined;
        if (r.kept) {
            ++c.kept;
            if (wl) ++c.wl_kept; else ++c.band_kept;
        }
    }
    if (epoch.summary.cond_failed) ++c.cond_failed;
}

std::string formatArObserveSummary(const ArObserveCounters& c, const std::string& tag,
                                   const std::string& search, double sigma, bool gate) {
    return fmt::format(
        "[RT] AR observe ({}, search '{}', determined bar {:.3f} cyc, chain gate {}): "
        "epochs {} / searched {} / accepted {} / gated by position sigma {}; ambiguity states {} -> "
        "eligible {} / gated out {}; "
        "SD pairs {}, band rows {} + wide-lane rows {}; determined {}, kept {} (band {} / wide "
        "lane {}); conditioning failures {}; closure {}",
        tag, search, sigma, gate ? "on" : "off",
        c.epochs, c.epochs_searched, c.epochs_accepted, c.epochs_gated, c.candidates, c.eligible,
        c.gated_out,
        c.sd_pairs, c.band_rows, c.wl_rows, c.determined, c.kept, c.band_kept, c.wl_kept,
        c.cond_failed, c.closureHolds() ? "HOLDS" : "BROKEN");
}

// ===========================================================================
// The two trace files
// ===========================================================================

ArEpochErrors arEpochErrors(const ArEpochSummary& summary, const Eigen::Vector3d& truth,
                            const Coordinates::Llh& truth_llh) {
    ArEpochErrors e;
    e.fix_enu = Coordinates::ecefToEnu(summary.fix_ecef - truth, truth_llh);
    e.float_enu = Coordinates::ecefToEnu(summary.float_ecef - truth, truth_llh);
    return e;
}

std::string formatArFixRow(int week, double tow, const ArFixRow& r, const ArEpochSummary& s) {
    return fmt::format("{},{:.3f},{},{},{},{},{},{:.6f},{:.6f},{},{},{},{},{},{:.3f},{:.6f},{},{}\n",
                       week, tow, r.system, r.signal, r.sat, r.ref_sat, r.cmp,
                       r.float_cyc, r.cond_sigma_cyc, r.fixed_int,
                       r.determined ? 1 : 0, r.kept ? 1 : 0,
                       s.z_fixed, s.z_total, s.ratio, s.p_s, r.gen_sat, r.gen_ref);
}

std::string formatArEpochRow(int week, double tow, const ArEpochSummary& s,
                             const ArEpochErrors& e) {
    return fmt::format("{},{:.3f},{},{},{},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                       week, tow, s.n_pairs, s.n_wl_det, s.n_band_det,
                       e.fix_enu.x(), e.fix_enu.y(), e.fix_enu.z(),
                       e.float_enu.x(), e.float_enu.y(), e.float_enu.z());
}

} // namespace GnssProcessor
