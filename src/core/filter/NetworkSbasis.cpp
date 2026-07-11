/**
 * @file NetworkSbasis.cpp
 * @brief Implementation of the network producer's minimum-constraint S-basis
 *        (shared clock datum builder + null-space rank gate).  See the header
 *        for the datum derivation.
 */

#include "filter/NetworkSbasis.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <fmt/format.h>

namespace GnssProcessor {

std::pair<std::string, std::string> NetworkSbasis::selectIfPinPair(
    std::vector<std::string> sigs,
    const std::function<double(const std::string&)>& freqOf) {
    if (sigs.empty()) return {};
    // Strict TOTAL order — (RINEX band digit, full signal name).  The band digit
    // orders bands (GPS L1 before L2 before L5), and the full name breaks
    // same-band ties (C1C before C1W, C2L before C2W) so the result is invariant
    // to the order signals were registered/passed in — the whole point of the
    // pin (a same-band swap would silently re-datum the emitted clock).
    std::sort(sigs.begin(), sigs.end(),
              [](const std::string& a, const std::string& b) {
                  const char ba = a.size() >= 2 ? a[1] : '\0';  // RINEX band digit
                  const char bb = b.size() >= 2 ? b[1] : '\0';
                  if (ba != bb) return ba < bb;
                  return a < b;
              });
    const std::string first = sigs.front();
    const double      f1     = freqOf(first);
    // second = first later signal on a DISTINCT frequency (skips a same-band,
    // same-frequency co-channel like C1C vs C1W); empty ⇒ single-frequency sat.
    for (std::size_t k = 1; k < sigs.size(); ++k) {
        const double fk = freqOf(sigs[k]);
        if (std::isfinite(fk) && fk > 0.0 && std::isfinite(f1) &&
            std::abs(fk - f1) > 1.0)
            return { first, sigs[k] };
    }
    return { first, std::string{} };
}

NetworkSbasis::CodeS2zTimeLink NetworkSbasis::codeS2zTimeLink(
    const std::vector<std::string>& present_sats,
    const std::map<std::string, double>& prev_vals) {
    CodeS2zTimeLink out;
    // Veterans = present satellites that also have a previous-epoch solved value.
    // The row spans them with RHS Σ b̂_prev, pinning the CHANGE of their mean to zero
    // (each referenced to itself → membership changes never move the target).
    for (const auto& sat : present_sats) {
        const auto it = prev_vals.find(sat);
        if (it != prev_vals.end()) {
            out.jac_sats.push_back(sat);
            out.rhs += it->second;
        }
    }
    // No veteran (cold start / warm-resume miss / every satellite fresh): fall back to
    // the legacy all-present Σ = 0 seed — a REAL re-reference the driver must signal.
    if (out.jac_sats.empty()) {
        out.jac_sats = present_sats;
        out.rhs      = 0.0;
        out.reseed   = true;
    }
    return out;
}

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

    // --- BROADCAST-ANCHORED clock datum (network.clock_datum_broadcast_anchored)
    // brdc_rts only: each satellite's Δδtˢ (relative to its OWN broadcast
    // a-priori) is anchored toward 0 by a soft per-satellite constraint row at
    // 1/σ² — the anchors ARE the gauge, folded BEFORE the rank gate.  No pivot
    // (no handover seam), and unlike sum-to-zero there is no redistribution
    // when membership changes: the common-mode stays bound to the external
    // broadcast reference, so the absolute clock-product frame holds still.
    // Rows are per-SATELLITE (single-entry), deliberately soft (σ ≈ 3 m ≫ the
    // between-sat data information) so the estimable clock structure stays
    // data-driven; the rank gate still sees the common-mode at ≈ N/σ², far
    // above the relative cutoff.
    if (decl.isb_datum == SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite) {
        const double sigma = decl.broadcast_anchor_sigma_m;
        if (!(std::isfinite(sigma) && sigma > 0.0)) {
            throw std::runtime_error(fmt::format(
                "NetworkSbasis: broadcast-anchored clock datum with invalid anchor "
                "sigma {} m — the gauge would be undefined", sigma));
        }
        Eigen::Index nsat = 0;
        for (const auto& [c, sats] : by_const)
            nsat += static_cast<Eigen::Index>(sats.size());
        Eigen::MatrixXd H(nsat, est.numShared());
        H.setZero();
        Eigen::Index r = 0;
        for (const auto& [c, sats] : by_const)
            for (const std::string& s : sats) H(r++, est.clockColumn(s)) = 1.0;
        plan.clock_constraint_H = std::move(H);
        plan.clock_constraint_z = Eigen::VectorXd::Zero(nsat);
        plan.clock_constraint_weight = 1.0 / (sigma * sigma);
        plan.declared_dimension = 0;   // the anchors close every clock gauge pre-gate
        return plan;                   // shared_datum + clock_pivot intentionally empty
    }

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
