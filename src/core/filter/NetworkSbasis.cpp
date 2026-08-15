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

NetworkSbasis::ChannelFamily NetworkSbasis::channelFamilyOf(
    const std::string& constellation, const std::string& signal) {
    // Raw RINEX-3 code observable, "C<band><attribute>"; the attribute is signal[2]
    // and is NEVER canonicalized on the way in (MeasurementModel keys the sat-bias
    // state on the raw 3-char code), so reading it here is reading the receiver's
    // own declaration.
    if (signal.size() < 3) return ChannelFamily::Primary;
    const char attr = signal[2];
    // §7 ALLOW-LIST, enforced here rather than at the call site: only constellations
    // whose family split has been reasoned about can report a secondary family at
    // all.  An unrecognised constellation returns Primary for every signal, so its
    // census can never find a partition and the row emitter can never fire on it.
    // This is the guard against the original draft's open-ended "rows emit wherever
    // partition is detected" — combined with a naive attribute key that would have
    // started emitting rows on GPS postures that work today.
    if (constellation != "E" && constellation != "G" && constellation != "C")
        return ChannelFamily::Primary;
    // 'X' is the COMBINED I+Q correlator output — a physically distinct hardware
    // path with its own bias.  Everything else (Galileo A/B/C/I/Q/Z, GPS
    // C/W/P/Y/S/L/I/Q/M, BeiDou I/Q/D/P/A/Z) is a discrete component sharing the
    // receiver's component chain, and belongs to one family.  NB this is why the
    // family key cannot be the attribute character itself: the Galileo primary
    // family is C1C/C5Q/C7Q — attributes 'C','Q','Q'.
    return attr == 'X' ? ChannelFamily::Secondary : ChannelFamily::Primary;
}

NetworkSbasis::FamilyCensus NetworkSbasis::runFamilyCensus(
    const std::map<std::string, std::set<std::string>>& station_signals) {
    FamilyCensus out;
    // Per constellation: how many stations hold ONLY primary channels, ONLY
    // secondary channels, or BOTH (a bridge).  A bridging station is the entire
    // question — with one, the inter-family geometry-free difference is observable
    // through that receiver and the primary pin DOES reach the secondary columns,
    // so no extra row is warranted (and one would over-pin).
    struct Tally { int primary_only{0}; int secondary_only{0}; int bridges{0}; };
    std::map<std::string, Tally> per_const;
    for (const auto& [station, keys] : station_signals) {
        (void)station;
        std::map<std::string, std::pair<bool, bool>> seen;  // const -> (has primary, has secondary)
        for (const auto& key : keys) {
            // Key shape is "SAT|SIGNAL" (StateKeySchema); split rather than parse —
            // the constellation is the satellite's first character.
            const auto bar = key.find('|');
            if (bar == std::string::npos || bar == 0 || bar + 1 >= key.size()) continue;
            const std::string sat = key.substr(0, bar);
            const std::string sig = key.substr(bar + 1);
            if (sig.empty() || sig[0] != 'C') continue;      // code channels only
            const std::string cst = constellationOf(sat);
            auto& s = seen[cst];
            if (channelFamilyOf(cst, sig) == ChannelFamily::Secondary) s.second = true;
            else                                                       s.first  = true;
        }
        for (const auto& [cst, s] : seen) {
            auto& t = per_const[cst];
            if (s.first && s.second) ++t.bridges;
            else if (s.second)       ++t.secondary_only;
            else if (s.first)        ++t.primary_only;
        }
    }
    for (const auto& [cst, t] : per_const) {
        out.n_stations_primary   += t.primary_only;
        out.n_stations_secondary += t.secondary_only;
        out.n_bridges            += t.bridges;
        if (t.secondary_only == 0 && t.bridges == 0) {
            // No secondary family observed at all — the single-family case every
            // validated posture to date has run.  Say so anyway (invariant #4).
            out.lines.push_back(fmt::format(
                "constellation {} — no secondary (X) channel family observed on {} "
                "station(s): SINGLE-FAMILY, no extra rows.", cst, t.primary_only));
            continue;
        }
        if (t.bridges > 0) {
            // A bridge exists: the primary pin reaches the secondary columns through
            // that receiver, so the direction is NOT null and an extra row would be
            // an over-pin on a determinable quantity.  Refuse to partition and say
            // exactly why — this is the branch a future mixed-family roster lands in.
            out.lines.push_back(fmt::format(
                "constellation {} family X — {} station(s), {} BRIDGE(s) to the "
                "primary family: NOT partitioned, no extra rows (a bridging receiver "
                "makes the inter-family geometry-free difference observable, so the "
                "primary pin already reaches these columns).",
                cst, t.secondary_only, t.bridges));
            continue;
        }
        out.partitioned.insert(cst);
        out.lines.push_back(fmt::format(
            "constellation {} family X — {} station(s), 0 bridge(s): PARTITIONED.",
            cst, t.secondary_only));
    }
    out.latched = true;
    return out;
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

std::vector<NetworkEstimator::SharedPrior> NetworkSbasis::buildCodeBiasKaulaPriors(
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    double sigma_m) {
    std::vector<NetworkEstimator::SharedPrior> out;
    // A non-positive sigma would mean info = 1/0 = inf, i.e. a HARD pin on every
    // admitted OSB — the opposite of a loose physical bound, and a silent datum
    // takeover.  Refuse it here rather than trusting the loader alone.
    if (!(sigma_m > 0.0)) return out;
    const double info = 1.0 / (sigma_m * sigma_m);
    out.reserve(registered_code_keys.size());
    // First-seen registration order, mirroring buildCodeBiasDatumRows' own
    // contractual ordering over the same vector.  The prior is diagonal, so the
    // order cannot change the folded information — but keeping it identical means
    // the two W6 datum products read the same way in a trace.
    for (const auto& kv : registered_code_keys) {
        NetworkEstimator::SharedPrior p;
        p.kind   = SharedParamKind::CodeBias;
        p.sat    = kv.first;
        p.signal = kv.second;
        p.value  = 0.0;   // the physical bound is CENTRED on zero, not on an estimate
        p.info   = info;
        out.push_back(p);
    }
    return out;
}

NetworkSbasis::GaugeCompletion NetworkSbasis::completeSbasisDimension(
    const Eigen::MatrixXd& reduced_N_ss, int declared_dim, double tol,
    const Eigen::VectorXd& x_prev,
    const std::function<std::string(Eigen::Index)>& labelOf,
    int max_extra) const {
    GaugeCompletion out;
    const Eigen::Index n = reduced_N_ss.rows();
    if (n == 0 || reduced_N_ss.cols() != n || declared_dim < 0) return out;

    // Same decomposition family and cutoff arithmetic as assertNullSpaceRank —
    // the completion must count exactly what the gate will count, or it would
    // "complete" against a decision the gate does not make (the DG2 lesson,
    // extended to a fourth consumer).  Vectors are needed here (the rows ARE
    // the eigenvectors); this runs only when armed, and the gate-failure heal
    // already pays the same decomposition on exactly the epochs that fire.
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(reduced_N_ss,
                                                      Eigen::ComputeEigenvectors);
    if (es.info() != Eigen::Success) return out;   // the gate will report it loudly
    const Eigen::VectorXd& ev = es.eigenvalues();  // ascending
    const double smax   = ev.size() > 0 ? ev(ev.size() - 1) : 0.0;
    const double cutoff = tol * std::max(smax, 1.0);

    int observed = 0;
    for (Eigen::Index i = 0; i < ev.size(); ++i)
        if (ev(i) < cutoff) ++observed;
    out.observed = observed;
    // observed < declared is the gate's "over-specified" verdict (a tolerance
    // below this config's gauge depth) — nothing to complete either way.
    if (observed <= declared_dim) return out;

    // Per-DIRECTION degeneracy skip: the structural gauges sit at machine-zero
    // (locals absorb them exactly); an extra unseparated from that cluster (not
    // ≥ 1 decade above the running cluster top, or below the 1e-14·λmax
    // machine-zero floor — the relative test alone is defeated by an exactly-
    // 0.0 gauge, since 10·0 = 0 admits everything) is SKIPPED, because
    // eigenvectors mix arbitrarily within a degenerate cluster and a row there
    // could re-target a combination the declared pins own; an extra AT machine
    // zero is an orphaned EXACT gauge — Card PA's class.  Each skipped extra
    // extends the cluster (its |ev| joins the running top), so the first
    // CONSTRAINED extra is ≥ 1 decade above everything left unpinned below it
    // and cannot mix with any of it.  With declared_dim == 0 and no skips
    // there is nothing to mix with — exact-zero orphans are then legitimately
    // completable (the constraint-realized clock datums leave no pins to
    // conflict with).  |·| because near-zero eigenvalues carry rounding sign.
    // Attribution shared by constrained AND skipped directions: every column
    // at |component| ≥ kSbasisAttrSpan, heaviest first; the heaviest column is
    // always named even when the direction is spread below the threshold (an
    // anonymous direction would make the engage/skip event un-actionable).
    const auto attribute = [&](Eigen::Index j) {
        GaugeCompletion::Dir d;
        d.ev = ev(j);
        const Eigen::VectorXd v = es.eigenvectors().col(j);
        std::vector<std::pair<double, Eigen::Index>> part;
        Eigen::Index cmax = 0;
        double amax = -1.0;
        for (Eigen::Index c = 0; c < n; ++c) {
            const double a = std::abs(v(c));
            if (a > amax) { amax = a; cmax = c; }
            if (a >= kSbasisAttrSpan) part.emplace_back(a, c);
        }
        if (part.empty() && n > 0) part.emplace_back(amax, cmax);
        std::sort(part.begin(), part.end(),
                  [](const std::pair<double, Eigen::Index>& a,
                     const std::pair<double, Eigen::Index>& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;   // deterministic tie-break
                  });
        for (std::size_t p = 0; p < part.size(); ++p) {
            const std::string lab = labelOf
                ? labelOf(part[p].second)
                : fmt::format("col{}", static_cast<long long>(part[p].second));
            if (p == 0) { d.top_span = part[p].first; d.top_label = lab; }
            if (p < 6) {
                if (!d.detail.empty()) d.detail += ',';
                d.detail += fmt::format("{}:{:.2f}", lab, part[p].first);
            }
            // Satellite identity from the per-sat label kinds ("CLK:<sat>",
            // "CB:<sat>:<sig>", "PB:<sat>:<sig>"); SH coefficients carry none.
            if (lab.rfind("CLK:", 0) == 0 || lab.rfind("CB:", 0) == 0 ||
                lab.rfind("PB:", 0) == 0) {
                const auto p1 = lab.find(':');
                const auto p2 = lab.find(':', p1 + 1);
                const auto len =
                    (p2 == std::string::npos ? lab.size() : p2) - (p1 + 1);
                if (len > 0) d.sats.insert(lab.substr(p1 + 1, len));
            }
        }
        return d;
    };

    const double floor_abs = 1e-14 * std::max(smax, 1.0);
    double cluster_top = declared_dim > 0 ? std::abs(ev(declared_dim - 1)) : -1.0;
    Eigen::Index first = declared_dim;
    while (first < observed) {
        const double a = std::abs(ev(first));
        const bool in_cluster =
            cluster_top >= 0.0 && (a < 10.0 * cluster_top || a < floor_abs);
        if (!in_cluster) break;
        out.skipped.push_back(attribute(first));
        cluster_top = std::max(cluster_top, a);
        ++first;
    }
    out.n_skipped  = static_cast<int>(first - declared_dim);
    out.degenerate = out.n_skipped > 0;
    const int n_extra = observed - static_cast<int>(first);
    if (n_extra <= 0) return out;

    // K2 anti-fragility: the folded row must land ≥ 2 decades above the armed
    // cutoff, or the "completion" itself sits at the cutoff it exists to clear.
    if (kSbasisCompleteWeight < 100.0 * cutoff) {
        out.weight_marginal = true;
        return out;
    }
    // No silent mass-pinning: a deficiency burst beyond the cap is a
    // pathological epoch (or a mis-calibrated tolerance) — withhold honestly.
    if (max_extra > 0 && n_extra > max_extra) {
        out.capped = true;
        return out;
    }

    out.n_extra = n_extra;
    out.H.resize(n_extra, n);
    out.z.resize(n_extra);
    const bool have_prev = x_prev.size() == n;
    for (int k = 0; k < n_extra; ++k) {
        const Eigen::Index j = first + k;
        const Eigen::VectorXd v = es.eigenvectors().col(j);
        out.H.row(k) = v.transpose();
        out.z(k)     = have_prev ? v.dot(x_prev) : 0.0;
        GaugeCompletion::Dir d = attribute(j);
        d.target = out.z(k);
        out.dirs.push_back(std::move(d));
    }
    return out;
}

void NetworkSbasis::assertNullSpaceRank(const Eigen::MatrixXd& reduced_N_ss,
                                        int declared_dim, double tol,
                                        int* observed_out, double* ev_max_out) const {
    // DG6: report the observed count on EVERY exit that computed one, including
    // the throwing one — the caller cannot recompute it from the exception and
    // "over-specified" (observed < declared) has to be countable separately from
    // a generic withhold.  Structurally observational: nothing below reads it.
    if (observed_out) *observed_out = 0;
    // Invariant #7: same contract for λmax (the caller reconstructs the gate's own
    // cutoff from it).  -1 = "no spectrum was computed on this exit".
    if (ev_max_out) *ev_max_out = -1.0;
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
    if (observed_out) *observed_out = near_zero;
    if (ev_max_out)   *ev_max_out   = smax;

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


NetworkSbasis::CodeBiasDatumRows NetworkSbasis::buildCodeBiasDatumRows(
    const NetworkEstimator& est,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::function<double(const std::string&, const std::string&)>& freqOf,
    bool cb_time_linked, bool clock_signal_basis_hold, bool emit_clock_disc,
    bool gf_datum,
    const std::set<std::string>& gf_family_constellations,
    const std::map<std::string, std::map<std::string, double>>& code_bias_prev_,
    const std::map<std::string, std::pair<std::string, std::string>>& code_bias_ifpin_pair_,
    const std::set<std::string>& code_bias_sat_seen_,
    bool code_bias_datum_resumed_,
    std::map<std::string, std::pair<std::string, std::string>>& held_clock_ref,
    std::map<std::string, std::pair<std::string, std::string>>& this_epoch_if_pin_sel,
    std::map<std::string, std::pair<std::string, std::string>>& this_epoch_if_pin_real,
    std::set<std::string>& this_epoch_gf_pin_real,
    std::map<std::string, std::pair<std::string, std::string>>& this_epoch_family_gf_pin_real,
    std::map<std::string, std::pair<std::string, std::string>>& this_epoch_cb_ifpin,
    std::set<std::string>& this_epoch_cb_reseed,
    std::set<std::string>& basis_relatch_disc_pending,
    const std::function<void(const char*, const char*, const std::string&,
                             const std::string&, std::optional<double>,
                             std::optional<double>, const char*,
                             const std::string&)>& emitEvent) {
    CodeBiasDatumRows out;
    const Eigen::Index nshared = est.numShared();

    std::map<std::string, std::vector<std::string>> sat_signals;  // sat -> code signals
    std::vector<std::string> sat_order;
    for (const auto& kv : registered_code_keys) {
        auto& v = sat_signals[kv.first];
        if (v.empty()) sat_order.push_back(kv.first);
        v.push_back(kv.second);
    }

    std::vector<Eigen::VectorXd> rows;                 // length-nshared constraint Jacobians
    std::vector<double>          zs;                   // Card 2: per-row RHS target (all 0.0 unless code_datum_time_linked)

    // Card 2 (network.code_datum_time_linked): fetch a satellite's PREVIOUS
    // kept-epoch solved code bias for a signal — a "veteran" reference.  An
    // empty map (off, or first appearance) ⇒ always false ⇒ the zero-target
    // legacy path (byte-identical).
    auto prevCb = [&](const std::string& sat, const std::string& sig,
                      double& out) -> bool {
        const auto it = code_bias_prev_.find(
            NetworkSbasis::constellationOf(sat) + "|" + sig);
        if (it == code_bias_prev_.end()) return false;
        const auto jt = it->second.find(sat);
        if (jt == it->second.end()) return false;
        out = jt->second;
        return true;
    };

    // (2) Per-(constellation, signal) sum-to-zero — one row per registered
    // signal, summed over the satellites that carry it (keyed
    // "<constellation>|<signal>").  Pins every channel's absolute level; see
    // the block header for why this is per-SIGNAL (robust to non-co-tracked
    // same-band channels at network scale) rather than per-band.  Collected as
    // (sat, column) per channel (std::map → sorted-skey order, unchanged); the
    // rows are BUILT after the IF-pin loop so their order in `rows` is preserved
    // (IF pins first, then sum-to-zero) — and, under time-linking, each row
    // spans the channel's VETERANS with RHS Σ b̂_prev instead of all present
    // satellites with RHS 0.
    std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> st0_chan;
    for (const auto& kv : registered_code_keys) {
        Eigen::Index col;
        try { col = est.codeBiasColumn(kv.first, kv.second); } catch (...) { continue; }
        const std::string skey =
            NetworkSbasis::constellationOf(kv.first) + "|" + kv.second;
        st0_chan[skey].emplace_back(kv.first, col);
    }

    // (1) Per-satellite ionosphere-free pin (clock-common gauge).
    for (const std::string& sat : sat_order) {
        std::vector<std::string>& sigs = sat_signals[sat];
        // IF pin: two lowest DISTINCT-frequency code signals → a1+a2=1.
        // Selection is DETERMINISTIC in the signal SET (sorted by band
        // digit THEN full signal name), so same-band channels (C1C vs
        // C1W) no longer swap with registered_code_keys insertion order
        // and silently re-datum the emitted clock — see
        // NetworkSbasis::selectIfPinPair.
        const auto if_pin = NetworkSbasis::selectIfPinPair(
            sigs, [&](const std::string& s) { return freqOf(sat, s); });
        std::string sig1 = if_pin.first;
        std::string sig2 = if_pin.second;
        double      f1   = sig1.empty() ? 0.0 : freqOf(sat, sig1);
        double      f2   = sig2.empty() ? 0.0 : freqOf(sat, sig2);
        // network.clock_signal_basis_hold: anchor this satellite's clock to
        // ONE ionosphere-free reference pair for the whole run so the emitted
        // (δtˢ, dˢ) split is time-consistent, instead of re-selecting the
        // pair from THIS epoch's coverage (which re-references the clock by
        // the satellite's inter-channel code bias whenever a sparse channel
        // enters/leaves the ≥2-station set — the defect-2 differential-clock
        // wander a receiver clock cannot absorb).  Byte-identical OFF for the
        // HOLD feature: held_clock_ref is neither read nor written (the
        // deterministic selection above stands unchanged).
        if (clock_signal_basis_hold) {
            // Is a held reference signal still in this satellite's registered
            // ≥2-station set this epoch?  (Named to avoid shadowing the outer
            // per-station `present` vector.)
            auto held_present = [&](const std::string& s) {
                return !s.empty() &&
                       std::find(sigs.begin(), sigs.end(), s) != sigs.end();
            };
            auto held = held_clock_ref.find(sat);
            if (held != held_clock_ref.end() &&
                held_present(held->second.first) && held_present(held->second.second)) {
                // HOLD: both held reference signals are still registered —
                // reuse them so the clock stays on the prior signal basis.
                sig1 = held->second.first;
                sig2 = held->second.second;
                f1   = freqOf(sat, sig1);
                f2   = freqOf(sat, sig2);
            } else if (!sig2.empty() && f2 > 0.0) {
                // LATCH (first sight) / RE-LATCH (a held reference signal left
                // the ≥2-station set): adopt the freshly-selected pair and hold
                // it forward.  A re-latch is a genuine (rare) datum move — log
                // it for observability (network.trace_events); the emitted clock
                // step it causes is the satellite's inter-channel bias.
                if (held != held_clock_ref.end() &&
                    (held->second.first != sig1 || held->second.second != sig2)) {
                    emitEvent("CLK_BASIS", "relatch", sat, "", std::nullopt,
                              std::nullopt, "held_ref_change",
                              held->second.first + "+" + held->second.second +
                                  "->" + sig1 + "+" + sig2);
                    // T3c: the re-latch stepped the emitted clock↔code split — signal it as
                    // a per-sat discontinuity (bumped at the next emit) so the consumer resets
                    // instead of integrating across the unsignalled step.  Guarded by the
                    // clock-disc channel; the CLK_BASIS event above is logged either way.
                    if (emit_clock_disc) basis_relatch_disc_pending.insert(sat);
                }
                held_clock_ref[sat] = { sig1, sig2 };
            }
        }
        // Hold OFF: the pin is re-derived from THIS epoch's coverage every
        // epoch.  With the deterministic selection above the pair is a pure
        // function of the registered signal SET, so a change BETWEEN EMITTED
        // products means a channel actually entered/left the ≥2-station set —
        // the same defect-2 clock↔code re-reference a hold re-latch signals,
        // only here it happens silently every time coverage churns.  Record
        // THIS epoch's selection in the main-pass scratch and defer the compare
        // + disc bump to the EMIT site (against the last EMITTED pair), so a
        // transient pin move on a withheld epoch cannot fabricate a bump — the
        // same emit-only discipline prev_pivot / emitted_anchor follow.  Guarded
        // by emit_clock_disc AND a real IF pair (f1,f2 > 0) ⇒ byte-identical when
        // the disc channel is off or the sat is single-frequency.  Skipped under
        // hold (that path owns its own re-latch signalling above).
        if (!clock_signal_basis_hold && emit_clock_disc && f1 > 0.0 && f2 > 0.0)
            this_epoch_if_pin_sel[sat] = { sig1, sig2 };
        // S-contract §1.5 DECLARATION record — SEPARATE from the disc scratch above and
        // deliberately unguarded by either knob.  sig1/sig2 are POST-hold here, so this
        // is the pair the datum actually realizes on both the held and the re-selected
        // path.  The scratch above cannot serve: it is gated on the QC-channel
        // emit_clock_disc (default false, and absent from the entire ft63→pc305 field
        // chain), which is why every field posture-C product declared no cbpin= at all
        // while the sim arms declared fine.  Read only by the NetworkSolPrep fill ⇒ the
        // solve and every emitted numeric value are untouched; only the S line changes.
        // Written BELOW, after the row is known to have folded — see the record site.
        Eigen::VectorXd row = Eigen::VectorXd::Zero(nshared);
        bool row_ok = false;
        double zval = 0.0;   // Card 2: legacy zero target unless time-linked
        if (f1 > 0.0 && f2 > 0.0) {
            const double den = f1 * f1 - f2 * f2;
            const double a1 =  f1 * f1 / den;     // ionosphere-free (clock-preserving) weights
            const double a2 = -f2 * f2 / den;
            try {
                row(est.codeBiasColumn(sat, sig1)) = a1;
                row(est.codeBiasColumn(sat, sig2)) = a2;
                row_ok = true;
            } catch (...) { row.setZero(); row_ok = false; }
            // Card 2: pin the CHANGE of the clock↔code split — RHS is the
            // satellite's PREVIOUS solved ionosphere-free combination
            // a1·b̂_prev(σ1)+a2·b̂_prev(σ2).  This is genuine continuity ONLY
            // when the pair is UNCHANGED from the last kept epoch AND both
            // signals had a previous solved value.  A pair CHANGE (the band-1
            // C1C↔C1W flip, or a band entering/leaving) re-references the split
            // by the satellite's inter-channel bias even if the new pair's
            // signals both have prior values, so it must re-seed to 0 and bump —
            // otherwise the emitted clock steps unsignalled.  On any re-seed that
            // is NOT the satellite's first appearance (seen before, or the run
            // resumed), bump its disc so the value step coincides with a
            // signalled discontinuity.  Record this epoch's pair for the next
            // epoch's comparison (persisted at the harvest, kept epochs only).
            if (row_ok && cb_time_linked) {
                double v1 = 0.0, v2 = 0.0;
                const bool h1 = prevCb(sat, sig1, v1);
                const bool h2 = prevCb(sat, sig2, v2);
                const auto pit = code_bias_ifpin_pair_.find(sat);
                const bool pair_same =
                    pit != code_bias_ifpin_pair_.end() &&
                    pit->second.first == sig1 && pit->second.second == sig2;
                if (h1 && h2 && pair_same)
                    zval = a1 * v1 + a2 * v2;
                else if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_)
                    this_epoch_cb_reseed.insert(sat);
                this_epoch_cb_ifpin[sat] = { sig1, sig2 };
            }
        } else {
            // Single distinct frequency (degenerate): pin the all-ones gauge
            // so the satellite's code-bias common mode is still fixed.  No IF
            // split to time-link (kept at the zero target).
            for (const std::string& s : sigs)
                try { row(est.codeBiasColumn(sat, s)) = 1.0; row_ok = true; } catch (...) {}
        }
        if (row_ok && row.cwiseAbs().sum() > 0.0) {
            rows.push_back(std::move(row));
            zs.push_back(zval);
            // S-contract §1.5 DECLARATION record — placed HERE, after the push, so it
            // records only a pair whose IF row actually FOLDED.  Guarded on a real dual-
            // frequency pin (f1,f2 > 0), which also excludes the degenerate single-
            // frequency all-ones branch above: that one sets row_ok but realizes no IF
            // pair, so it must not claim one.  Recording it earlier (guarded on f1,f2
            // alone) let a satellite whose codeBiasColumn lookup threw still enter the
            // emit-site unanimity tally as a satellite WITHOUT a GF row, which reads as a
            // partial split and suppresses an otherwise legitimate gfpin= declaration —
            // the GF record below is correctly inside its own success branch, and this is
            // the matching guard.
            if (f1 > 0.0 && f2 > 0.0) this_epoch_if_pin_real[sat] = { sig1, sig2 };
        }
        // (1b) Per-satellite GEOMETRY-FREE pin — Odijk 2016 rank deficiency 3b
        // (Table 7), armed ONLY in the slant posture (the loader refuses it
        // alongside an ionosphere constraint).  Their Eq. (30):
        //
        //     mu_GF = [-1, 1, 0, ..., 0]^T / (mu_2 - mu_1),   mu_j = (f_1/f_j)^2
        //
        // so the pinned combination is (d_2 - d_1)/(mu_2 - mu_1) = 0 on the SAME pair
        // the IF pin uses.  Deliberately the same pair: IF and GF then span the pinned
        // pair's whole 2-D subspace, which drives d_1 = d_2 = 0 exactly — Odijk's
        // "code biases are not estimable for the first two frequencies" (§5.1)
        // realized as a datum rather than asserted.  What survives is the modernized
        // DCB d^s_1 - d^s_j for j >= 3, which is the entire point of the third band.
        //
        // TARGET 0 IS THE S-BASIS CHOICE, NOT A GUESS.  d^s_GF is a datum quantity: its
        // value is arbitrary and the estimable parameters absorb it (their Table 8 —
        // the geometry-free biases move INTO the slant ionosphere, iota~ = iota +
        // d_r,GF - d^s_GF).  That is why a zero target is safe HERE and is not the same
        // move as a zero-target ambiguity pin, which fights an injected integer truth
        // and discharges km-scale tension into the field (§12.8.1).  The quantity
        // pinned here is metre-scale and genuinely gauge.
        if (gf_datum && f1 > 0.0 && f2 > 0.0) {
            const double mu2_minus_mu1 = (f1 * f1) / (f2 * f2) - 1.0;
            if (std::abs(mu2_minus_mu1) > 1e-12) {
                Eigen::VectorXd grow = Eigen::VectorXd::Zero(nshared);
                bool gf_ok = false;
                try {
                    grow(est.codeBiasColumn(sat, sig1)) = -1.0 / mu2_minus_mu1;
                    grow(est.codeBiasColumn(sat, sig2)) =  1.0 / mu2_minus_mu1;
                    gf_ok = true;
                } catch (...) { grow.setZero(); gf_ok = false; }
                if (gf_ok && grow.cwiseAbs().sum() > 0.0) {
                    rows.push_back(std::move(grow));
                    zs.push_back(0.0);
                    ++out.n_gf_pin_sats;
                    // S-contract §1.5: this satellite's code-bias datum is the IF+GF
                    // PAIR, not the IF pin alone — a different estimable function on
                    // the same wire (the pinned pair's B records are identically the
                    // datum; only j >= 3 carries content, and the geometry-free lump
                    // moved into the slant ionosphere).  Record it so the S line can
                    // DECLARE it: a consumer or grader that reads the B records under
                    // the single-layer assumption applies the wrong S-transform, and
                    // nothing else on the wire distinguishes the two postures.
                    this_epoch_gf_pin_real.insert(sat);
                    // A satellite with no signal outside the pinned pair has NOTHING
                    // estimable left on the code side once IF and GF are both pinned.
                    // That is correct under Odijk, and it is also invisible downstream
                    // (an all-datum satellite emits values indistinguishable from a
                    // well-determined zero), so it is counted rather than left implicit.
                    bool has_third = false;
                    for (const std::string& s : sigs)
                        if (s != sig1 && s != sig2) { has_third = true; break; }
                    if (!has_third) ++out.n_gf_only_pair;
                }
            }
        }
        // (1c) FAMILY-GF — one EXTRA geometry-free row per PARTITIONED secondary
        // channel family (docs/SPEC_family_gf_datum.md §2).  Odijk rd 3b re-derives
        // PER RECEIVER CHANNEL FAMILY: the pin above is built on the satellite's two
        // lowest-ordered signals, which on a Galileo Q+X roster is always the Q pair
        // (C1C, C5Q) — and with the station population partitioned by family, NO
        // observation of an X station ever touches a pinned channel.  The X subsystem
        // therefore keeps its own null direction per satellite,
        //     Delta d_jx = mu_j * delta   (code AND phase together),
        // compensated by iota -> iota - delta on every X-station line of sight, which
        // is exactly what the 2026-08-12 estimability dump measured (band weights
        // 1 : 1.7933 : 1.7032 = mu on E1/E5a/E5b) and exactly why the null-space gate
        // withheld every epoch of `_pc305_gg_tri_slant_20m`.
        //
        // GEOMETRY-FREE ONLY, NEVER AN IF ROW.  The satellite clock is SHARED with the
        // primary family and is already realized by the primary IF pin plus the
        // per-constellation clock datum, so the secondary family's IF combination has a
        // fixed reference and is ESTIMABLE — it is the per-satellite inter-family ISC, a
        // real product quantity.  Pinning it would over-pin a data-determined quantity
        // and inject tension that has to discharge somewhere.  The IF row is in any case
        // structurally blind to this direction: with a1 = f1^2/(f1^2-f2^2) and
        // a2 = -f2^2/(f1^2-f2^2), a1*mu_1 + a2*mu_2 = a1 + a2*(f1^2/f2^2) = 0 identically.
        //
        // Target 0 for the same reason the primary GF row uses it, and the distinction
        // matters: `network.sbasis_complete` closes THIS SAME null space generically, by
        // constraining the numerically discovered direction at the CARRIED level — and on
        // this exact posture that passes the rank gate and destroys the ionosphere on a
        // cold start (a00 = -6292 TECU, SPEC_synthetic_minimum_network §12 rung 3).  A
        // gauge quantity pinned at 0 is a datum choice the estimable parameters absorb
        // (Odijk Table 8, per family); the same direction pinned at a cold carried value
        // is a hard constraint at a wrong value.  Gate closure is therefore NOT acceptance
        // for this row — the ionosphere level is (SPEC §6.0).
        if (gf_datum && !gf_family_constellations.empty() &&
            gf_family_constellations.count(NetworkSbasis::constellationOf(sat))) {
            // The family's own signal subset, and its own pin pair chosen by the SAME
            // selector and the SAME frequency source as the primary pin — determinism
            // contract preserved by construction rather than by a parallel implementation.
            std::vector<std::string> fam_sigs;
            for (const std::string& s : sigs)
                if (NetworkSbasis::channelFamilyOf(NetworkSbasis::constellationOf(sat), s) ==
                    ChannelFamily::Secondary)
                    fam_sigs.push_back(s);
            const auto fam_pin = NetworkSbasis::selectIfPinPair(
                fam_sigs, [&](const std::string& s) { return freqOf(sat, s); });
            const std::string& fs1 = fam_pin.first;
            const std::string& fs2 = fam_pin.second;
            const double ff1 = fs1.empty() ? 0.0 : freqOf(sat, fs1);
            const double ff2 = fs2.empty() ? 0.0 : freqOf(sat, fs2);
            if (ff1 > 0.0 && ff2 > 0.0) {
                const double fam_mu2_minus_mu1 = (ff1 * ff1) / (ff2 * ff2) - 1.0;
                if (std::abs(fam_mu2_minus_mu1) > 1e-12) {
                    Eigen::VectorXd frow = Eigen::VectorXd::Zero(nshared);
                    bool frow_ok = false;
                    try {
                        frow(est.codeBiasColumn(sat, fs1)) = -1.0 / fam_mu2_minus_mu1;
                        frow(est.codeBiasColumn(sat, fs2)) =  1.0 / fam_mu2_minus_mu1;
                        frow_ok = true;
                    } catch (...) { frow.setZero(); frow_ok = false; }
                    if (frow_ok && frow.cwiseAbs().sum() > 0.0) {
                        rows.push_back(std::move(frow));
                        zs.push_back(0.0);
                        ++out.n_family_gf_rows;
                        // S-contract §1.5: record the SECONDARY family's realized pair so
                        // the S line can declare the '+'-joined per-family gfpin=.  Without
                        // it the emit-site fill sees only the primary pair and declares a
                        // confident single-family token on a two-family product — which the
                        // grader then accepts, applying the primary family's transform to
                        // the secondary family's channels.
                        this_epoch_family_gf_pin_real[sat] = { fs1, fs2 };
                        // Same accounting as n_gf_only_pair, but scanned over the FAMILY's
                        // signals — `sigs` spans BOTH families here, so a satellite whose X
                        // family is a bare pair would otherwise always look like it had a
                        // third band (its Q channels) and never be counted.
                        bool fam_has_third = false;
                        for (const std::string& s : fam_sigs)
                            if (s != fs1 && s != fs2) { fam_has_third = true; break; }
                        if (!fam_has_third) ++out.n_family_gf_only_pair;
                    }
                }
            }
        }
    }
    // (2) Build the sum-to-zero rows now (after the IF pins, preserving row
    // order).  OFF: span every present satellite with RHS 0 (byte-identical).
    // Time-linked: span the channel's VETERANS with RHS Σ b̂_prev (pin the
    // change of the veteran mean); a no-veteran channel falls back to the
    // all-present Σ = 0 seed and disc-bumps its present satellites (the level
    // genuinely re-references) — first-ever appearances excepted, matching the
    // IF-pin rule.
    for (const auto& [skey, entries] : st0_chan) {
        Eigen::VectorXd srow = Eigen::VectorXd::Zero(nshared);
        double zval = 0.0;
        if (cb_time_linked) {
            std::vector<std::string>            present_sats;
            std::map<std::string, Eigen::Index> col_of;
            present_sats.reserve(entries.size());
            for (const auto& [sat, col] : entries) {
                present_sats.push_back(sat);
                col_of[sat] = col;
            }
            std::map<std::string, double> prev_vals;
            const auto pit = code_bias_prev_.find(skey);
            if (pit != code_bias_prev_.end())
                for (const auto& sat : present_sats) {
                    const auto vit = pit->second.find(sat);
                    if (vit != pit->second.end()) prev_vals[sat] = vit->second;
                }
            const auto tl = NetworkSbasis::codeS2zTimeLink(present_sats, prev_vals);
            for (const auto& sat : tl.jac_sats) srow(col_of[sat]) += 1.0;
            zval = tl.rhs;
            if (tl.reseed)
                for (const auto& sat : present_sats)
                    if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_)
                        this_epoch_cb_reseed.insert(sat);
        } else {
            for (const auto& [sat, col] : entries) srow(col) += 1.0;
        }
        if (srow.cwiseAbs().sum() > 0.0) {
            rows.push_back(std::move(srow));
            zs.push_back(zval);
        }
    }
    // Card 2: this_epoch_cb_reseed now holds every satellite whose datum
    // re-referenced THIS epoch.  It is NOT merged into the persistent
    // cb_reseed_disc_pending here — that merge is deferred to the EMIT site
    // (below), which every whole-epoch withhold path `continue`s BEFORE reaching.
    // So a transient re-seed on a since-withheld epoch (the frame reverts by the
    // next kept epoch) never enters the pending set and cannot fire a bump with no
    // emitted value step — the emit-only discipline, without needing to erase (an
    // erase would destroy a bump legitimately DEFERRED for a satellite re-seeded
    // while pruned from the emission).

    if (!rows.empty()) {
        Eigen::MatrixXd H(static_cast<Eigen::Index>(rows.size()), nshared);
        for (size_t r = 0; r < rows.size(); ++r)
            H.row(static_cast<Eigen::Index>(r)) = rows[r];
        // Card 2: per-row targets — all 0 on the legacy path (byte-identical),
        // the veteran-referenced / previous-IF values under time-linking.
        Eigen::VectorXd z(static_cast<Eigen::Index>(rows.size()));
        for (size_t r = 0; r < zs.size(); ++r)
            z(static_cast<Eigen::Index>(r)) = zs[r];
        out.H = std::move(H);
        out.z = std::move(z);
    }
    out.n_if_pin_sats  = static_cast<int>(sat_order.size());
    out.n_s2z_channels = static_cast<int>(st0_chan.size());
    return out;
}

} // namespace GnssProcessor
