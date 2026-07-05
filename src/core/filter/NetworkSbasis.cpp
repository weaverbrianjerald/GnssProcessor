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
    const SbasisDeclaration& decl,
    const std::map<std::string, std::string>* sticky_prev_pivot) const {
    Plan plan;

    // Group the registered clock satellites by constellation in a stable
    // (lexical) order so the fallback auto-pick is deterministic.  NOTE: the
    // lexical auto-pick is per-EPOCH, not sticky — a lexically lower satellite
    // rising flips the pivot (a datum re-reference).  Stickiness is opt-in via
    // sticky_prev_pivot (network.sticky_pivot, RTA-1) below.
    std::map<std::string, std::vector<std::string>> by_const;
    for (const auto& sat : registered_clock_sats) {
        if (sat.empty()) continue;
        by_const[constellationOf(sat)].push_back(sat);
    }
    for (auto& [c, sats] : by_const) std::sort(sats.begin(), sats.end());

    // --- SUM-TO-ZERO clock datum (network.clock_datum_sum_to_zero) -----------
    // Anchor-free: fix each constellation's clock common-mode with a sum-to-zero
    // over its OWN satellite clocks (Σ δtˢ = 0), realized exactly like the
    // code-bias per-(const,signal) gauge — a tight all-ones row folded onto the
    // shared border BEFORE the rank gate.  No pivot satellite is chosen, so the
    // shared hard-pin datum and the clock_pivot map stay EMPTY (no handover seam
    // ever) and the declared residual dimension is 0 (the folded rows close the
    // per-constellation clock deficiency).  Pivot overrides / stickiness are moot.
    if (decl.isb_datum == SbasisDeclaration::IsbDatum::SumToZeroPerConstellation) {
        const Eigen::Index nshared = est.numShared();
        Eigen::MatrixXd H(static_cast<Eigen::Index>(by_const.size()), nshared);
        H.setZero();
        Eigen::Index r = 0;
        for (const auto& [c, sats] : by_const) {
            for (const std::string& s : sats) H(r, est.clockColumn(s)) = 1.0;
            ++r;
        }
        plan.clock_constraint_H = std::move(H);
        plan.clock_constraint_z =
            Eigen::VectorXd::Zero(static_cast<Eigen::Index>(by_const.size()));
        plan.clock_constraint_weight = kClockGaugeWeight;
        plan.declared_dimension = 0;   // clock gauges pinned before the gate → full rank
        return plan;                   // shared_datum + clock_pivot intentionally empty
    }

    // One satellite-clock pin per constellation (the clock gauge per timing
    // system).  Pivot priority: (1) the declared config pin, if present in the
    // registered set (BOTH modes — user intent wins, including the deliberate
    // flip-back when a pinned satellite returns); (2) sticky mode only: the
    // incumbent pivot from sticky_prev_pivot, kept as long as it is still
    // registered; (3) the first registered satellite of that constellation.
    for (const auto& [c, sats] : by_const) {
        const auto present = [&sats](const std::string& s) {
            return std::find(sats.begin(), sats.end(), s) != sats.end();
        };
        std::string pivot;
        auto it = decl.clock_pivot_sat.find(c);
        if (it != decl.clock_pivot_sat.end() && !it->second.empty()
            && present(it->second)) {
            pivot = it->second;
        } else if (sticky_prev_pivot) {
            // RTA-1 sticky mode: keep the incumbent while it is registered.  When
            // it is absent, fall back to the lexical pick — the driver's emit-
            // baseline map then advances to the NEW pivot at the next emit, so the
            // NEW pivot is sticky in turn (no flip-back on the old one's return).
            const auto pit = sticky_prev_pivot->find(c);
            if (pit != sticky_prev_pivot->end() && !pit->second.empty()
                && present(pit->second)) {
                pivot = pit->second;
            } else {
                pivot = sats.front();
            }
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

    // The reduced shared normals are SYMMETRIC (the kernel symmetrizes them) and
    // positive-SEMIdefinite (a Schur complement of weighted Gram normals, gated
    // BEFORE any indefinite modification such as the D2 prior), so their
    // eigenvalues equal their singular values.  A self-adjoint eigensolve
    // (eigenvalues only) is therefore the SAME count test as the SVD at a small
    // fraction of JacobiSVD's cost (this runs once per emitted epoch).  Eigenvalues
    // come back ASCENDING, so the largest is the LAST entry.  Count those below the
    // RELATIVE threshold (a true gauge => ~0 eigenvalue; a well-observed direction
    // => O(largest)).  A tiny-negative gauge eigenvalue (rounding) is still below
    // the positive cutoff, so it is counted exactly as the SVD magnitude would be.
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(reduced_N_ss,
                                                      Eigen::EigenvaluesOnly);
    if (es.info() != Eigen::Success) {
        throw std::runtime_error(
            "NetworkSbasis null-space gate: eigenvalue decomposition of the reduced "
            "shared normals failed to converge");
    }
    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
    const double smax = ev.size() > 0 ? ev(ev.size() - 1) : 0.0;
    const double cutoff = tol * std::max(smax, 1.0);

    int near_zero = 0;
    for (Eigen::Index i = 0; i < ev.size(); ++i)
        if (ev(i) < cutoff) ++near_zero;

    if (near_zero != declared_dim) {
        throw std::runtime_error(fmt::format(
            "NetworkSbasis null-space gate: observed {} (near-)zero eigenvalue(s) "
            "below {:.3e} but the declared S-basis dimension is {} "
            "(largest ev={:.3e}, smallest ev={:.3e}).  The datum is "
            "{}-specified — fix the pin set before solving.",
            near_zero, cutoff, declared_dim, smax,
            ev.size() > 0 ? ev(0) : 0.0,
            near_zero > declared_dim ? "under" : "over"));
    }
}

} // namespace GnssProcessor
