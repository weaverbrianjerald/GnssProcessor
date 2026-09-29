/**
 * @file NetworkCouplingExact.cpp
 * @brief The exact conditional time update of the carried coupling rows (card CTU); the
 *        derivation and the index spaces are in the header.
 */

#include "filter/NetworkCouplingExact.hpp"

#include <algorithm>
#include <cmath>

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
    for (const auto& [a, b, v] : ep.q_off)   // card CTU G5: the seam's level noise
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

}  // namespace GnssProcessor
