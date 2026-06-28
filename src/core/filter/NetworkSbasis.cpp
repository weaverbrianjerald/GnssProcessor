/**
 * @file NetworkSbasis.cpp
 * @brief Implementation of the network producer's minimum-constraint S-basis
 *        (shared clock datum builder + null-space rank gate).  See the header
 *        for the datum derivation.
 */

#include "filter/NetworkSbasis.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

namespace GnssProcessor {

NetworkSbasis::Plan NetworkSbasis::buildSharedDatum(
    const NetworkEstimator& est,
    const std::vector<std::string>& registered_clock_sats,
    const SbasisDeclaration& decl) const {
    Plan plan;

    // Group the registered clock satellites by constellation, preserving a
    // stable (lexical) order so the auto-picked pivot is deterministic across
    // epochs (only changing when the incumbent sets — handover then re-declares).
    std::map<std::string, std::vector<std::string>> by_const;
    for (const auto& sat : registered_clock_sats) {
        if (sat.empty()) continue;
        by_const[constellationOf(sat)].push_back(sat);
    }
    for (auto& [c, sats] : by_const) std::sort(sats.begin(), sats.end());

    // One satellite-clock pin per constellation (the clock gauge per timing
    // system).  Pivot = declared override if present in the registered set, else
    // the first registered satellite of that constellation.
    for (const auto& [c, sats] : by_const) {
        std::string pivot;
        auto it = decl.clock_pivot_sat.find(c);
        if (it != decl.clock_pivot_sat.end() && !it->second.empty()
            && std::find(sats.begin(), sats.end(), it->second) != sats.end()) {
            pivot = it->second;
        } else {
            pivot = sats.front();
        }
        plan.clock_pivot[c] = pivot;
        plan.shared_datum.push_back(
            NetworkEstimator::Pin{ est.clockColumn(pivot), 0.0 });
    }

    plan.declared_dimension = static_cast<int>(plan.shared_datum.size());
    return plan;
}

void NetworkSbasis::assertNullSpaceRank(const Eigen::MatrixXd& reduced_N_ss,
                                        int declared_dim, double tol) const {
    const Eigen::Index n = reduced_N_ss.rows();
    if (n == 0) {
        if (declared_dim != 0) {
            throw std::runtime_error(fmt::format(
                "NetworkSbasis null-space gate: empty reduced normals but declared "
                "dimension {}", declared_dim));
        }
        return;
    }
    if (reduced_N_ss.cols() != n) {
        throw std::runtime_error(
            "NetworkSbasis null-space gate: reduced normals not square");
    }

    // Singular values of the SYMMETRIC reduced normals == eigenvalue magnitudes;
    // SVD is robust and gives them sorted descending.  Count those below the
    // RELATIVE threshold (a true gauge => exactly-zero singular value; a
    // well-observed direction => O(largest)).
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(reduced_N_ss);
    const Eigen::VectorXd& sv = svd.singularValues();
    const double smax = sv.size() > 0 ? sv(0) : 0.0;
    const double cutoff = tol * std::max(smax, 1.0);

    int near_zero = 0;
    for (Eigen::Index i = 0; i < sv.size(); ++i)
        if (sv(i) < cutoff) ++near_zero;

    if (near_zero != declared_dim) {
        throw std::runtime_error(fmt::format(
            "NetworkSbasis null-space gate: observed {} (near-)zero singular "
            "value(s) below {:.3e} but the declared S-basis dimension is {} "
            "(largest sv={:.3e}, smallest sv={:.3e}).  The datum is "
            "{}-specified — fix the pin set before solving.",
            near_zero, cutoff, declared_dim, smax,
            sv.size() > 0 ? sv(sv.size() - 1) : 0.0,
            near_zero > declared_dim ? "under" : "over"));
    }
}

} // namespace GnssProcessor
