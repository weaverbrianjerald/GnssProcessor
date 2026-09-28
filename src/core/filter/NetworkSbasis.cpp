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
    // Strict total order on (RINEX band digit, full signal name).  The band digit
    // orders bands (GPS L1 before L2 before L5), and the full name breaks same-band
    // ties (C1C before C1W, C2L before C2W), so the result does not depend on the
    // order in which signals were registered or passed.  A same-band swap would
    // re-datum the emitted clock without any other sign.
    std::sort(sigs.begin(), sigs.end(),
              [](const std::string& a, const std::string& b) {
                  const char ba = a.size() >= 2 ? a[1] : '\0';  // RINEX band digit
                  const char bb = b.size() >= 2 ? b[1] : '\0';
                  if (ba != bb) return ba < bb;
                  return a < b;
              });
    const std::string first = sigs.front();
    const double      f1     = freqOf(first);
    // second = the first later signal on a distinct frequency (skips a same-band,
    // same-frequency co-channel such as C1C vs C1W); empty ⇒ single-frequency sat.
    for (std::size_t k = 1; k < sigs.size(); ++k) {
        const double fk = freqOf(sigs[k]);
        if (std::isfinite(fk) && fk > 0.0 && std::isfinite(f1) &&
            std::abs(fk - f1) > 1.0)
            return { first, sigs[k] };
    }
    return { first, std::string{} };
}

bool NetworkSbasis::broadcastGroupDelayDescribesPair(const std::string& sat,
                                                     const std::string& sig1,
                                                     const std::string& sig2) {
    if (sig1.size() < 2 || sig2.size() < 2) return false;
    const char b1 = sig1[1];
    const char b2 = sig2[1];
    // Galileo: the latched value is BGD(E1, E5a), which describes (E1, E5a) only.
    // A Galileo (E1, E5b) pair (a triple-frequency posture) would need the second
    // group delay, BGD(E1, E5b), which the setup does not latch: target zero there.
    if (!sat.empty() && sat.front() == 'E') return b1 == '1' && b2 == '5';
    // Every other constellation: the RF-0c band-digit rule, unchanged.
    return b1 == '1' && b2 == '2';
}

NetworkSbasis::SatGfTarget NetworkSbasis::satGfBroadcastTarget(
    const std::string& sat, const std::string& sig1, const std::string& sig2,
    const std::map<std::string, double>& sat_gf_target_m) {
    SatGfTarget t;
    if (!broadcastGroupDelayDescribesPair(sat, sig1, sig2)) {
        t.kind = SatGfTargetKind::NoGroupDelay;
        return t;
    }
    const auto it = sat_gf_target_m.find(sat);
    if (it == sat_gf_target_m.end()) {
        t.kind = SatGfTargetKind::Missing;
        return t;
    }
    if (it->second == 0.0) {
        t.kind = SatGfTargetKind::BroadcastZero;   // physically pinned, numerically zero
        return t;
    }
    t.kind     = SatGfTargetKind::BroadcastHit;
    t.target_m = it->second;
    return t;
}

NetworkSbasis::GroupDelayLatch NetworkSbasis::latchBroadcastGroupDelays(
        const std::vector<BroadcastEphemeris>& nav, double t0_gps_s, double t1_gps_s) {
    GroupDelayLatch out;
    std::map<std::string, std::vector<const BroadcastEphemeris*>> by_sat;
    for (const auto& e : nav) {
        if (e.satellite.empty() || e.system == System::GLONASS) continue;
        by_sat[e.satellite].push_back(&e);   // file order within a satellite
    }
    for (const auto& [sat, recs] : by_sat) {
        GroupDelayLatchEntry g;
        g.n_records = static_cast<int>(recs.size());
        g.first_s   = recs.front()->kepler.tgd0;
        for (const auto* e : recs)
            g.spread_file_s = std::max(g.spread_file_s, std::abs(e->kepler.tgd0 - g.first_s));

        // Health: the healthy records, unless the satellite has none at all.
        std::vector<const BroadcastEphemeris*> pool;
        for (const auto* e : recs)
            if (e->health == 0) pool.push_back(e);
        const bool all_unhealthy = pool.empty();
        if (all_unhealthy) pool = recs;
        else out.dropped_unhealthy += static_cast<long long>(recs.size() - pool.size());

        // Galileo: the F/NAV (E1, E5a) records when present (data-source bits 1 and 8).
        if (recs.front()->system == System::Galileo) {
            std::vector<const BroadcastEphemeris*> fnav;
            for (const auto* e : pool)
                if (e->data_source >= 0 && (e->data_source & 2) && (e->data_source & 256))
                    fnav.push_back(e);
            if (!fnav.empty()) {
                out.dropped_inav += static_cast<long long>(pool.size() - fnav.size());
                pool = std::move(fnav);
            }
        }

        // The window: the records whose clock reference epoch lies in [t0, t1).
        std::vector<double> inside;
        for (const auto* e : pool) {
            const double t = brdcTocGpsSeconds(*e);
            if (t >= t0_gps_s && t < t1_gps_s) inside.push_back(e->kepler.tgd0);
        }
        out.outside_window += static_cast<long long>(pool.size() - inside.size());
        if (!inside.empty()) {
            std::sort(inside.begin(), inside.end());
            g.value_s = inside[(inside.size() - 1) / 2];   // the LOW median: a broadcast value
            g.n_used  = static_cast<int>(inside.size());
            g.reason  = all_unhealthy ? GroupDelayLatchReason::AllUnhealthy
                                      : GroupDelayLatchReason::Window;
        } else {
            // None in the window: the record in force at its start (the latest at or
            // before t0), else the earliest after it.
            const BroadcastEphemeris* pick = nullptr;
            for (const auto* e : pool)
                if (brdcTocGpsSeconds(*e) <= t0_gps_s
                    && (!pick || brdcTocGpsSeconds(*e) > brdcTocGpsSeconds(*pick)))
                    pick = e;
            if (!pick)
                for (const auto* e : pool)
                    if (!pick || brdcTocGpsSeconds(*e) < brdcTocGpsSeconds(*pick)) pick = e;
            g.value_s = pick->kepler.tgd0;
            g.n_used  = 1;
            g.reason  = all_unhealthy ? GroupDelayLatchReason::AllUnhealthyInForceAtStart
                                      : GroupDelayLatchReason::InForceAtStart;
        }
        // A value equal to the first record's broadcast value keeps the first record's
        // exact bits.  A merged navigation file can print one broadcast value with
        // different last digits in different records (for example "-1.769512891770E-08"
        // and "-1.769512891769E-08" for one GPS satellite), so the median can pick a
        // representation about 1e-20 s away from the first record's.  That is not a
        // datum change, but it moves the solve at the numerical-noise level.  The 1 ps
        // threshold is two orders below the smallest broadcast step (BeiDou, 0.1 ns) and
        // eight orders above that noise.
        if (std::abs(g.value_s - g.first_s) < kSameBroadcastGroupDelayS) g.value_s = g.first_s;
        if (all_unhealthy)                                          ++out.n_all_unhealthy;
        else if (g.reason == GroupDelayLatchReason::InForceAtStart) ++out.n_in_force;
        else                                                        ++out.n_window;
        out.sats.emplace(sat, g);
    }
    return out;
}

NetworkSbasis::ChannelFamily NetworkSbasis::channelFamilyOf(
    const std::string& constellation, const std::string& signal) {
    // Raw RINEX-3 code observable, "C<band><attribute>".  The attribute is signal[2]
    // and is not canonicalized on input (MeasurementModel keys the sat-bias state on
    // the raw 3-character code), so this reads the receiver's own declaration.
    if (signal.size() < 3) return ChannelFamily::Primary;
    const char attr = signal[2];
    // The §7 allow-list is enforced here rather than at the call site: only
    // constellations whose family split has been analysed (Galileo, GPS, BeiDou) can
    // report a secondary family.  Any other constellation returns Primary for every
    // signal, so its census cannot find a partition and the row emitter cannot fire
    // on it.  This keeps extra rows from appearing on a posture merely because a
    // partition is detected.
    if (constellation != "E" && constellation != "G" && constellation != "C")
        return ChannelFamily::Primary;
    // 'X' is the combined I+Q correlator output, a physically distinct hardware path
    // with its own bias.  Every other attribute (Galileo A/B/C/I/Q/Z, GPS
    // C/W/P/Y/S/L/I/Q/M, BeiDou I/Q/D/P/A/Z) is a discrete component on the
    // receiver's component chain and belongs to one family.  For this reason the
    // family key cannot be the attribute character itself: the Galileo primary family
    // is C1C/C5Q/C7Q, attributes 'C', 'Q', 'Q'.
    return attr == 'X' ? ChannelFamily::Secondary : ChannelFamily::Primary;
}

NetworkSbasis::FamilyCensus NetworkSbasis::runFamilyCensus(
    const std::map<std::string, std::set<std::string>>& station_signals) {
    FamilyCensus out;
    // Per constellation: how many stations hold only primary channels, only
    // secondary channels, or both (a bridge).  The bridge count decides the outcome:
    // with a bridging station, the inter-family geometry-free difference is
    // observable through that receiver and the primary pin reaches the secondary
    // columns, so no extra row is warranted (one would over-pin).
    struct Tally { int primary_only{0}; int secondary_only{0}; int bridges{0}; };
    std::map<std::string, Tally> per_const;
    for (const auto& [station, keys] : station_signals) {
        (void)station;
        std::map<std::string, std::pair<bool, bool>> seen;  // const -> (has primary, has secondary)
        for (const auto& key : keys) {
            // Key shape is "SAT|SIGNAL" (StateKeySchema); a plain split suffices,
            // since the constellation is the satellite's first character.
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
            // No secondary family observed: the single-family case, which every
            // validated posture runs.  It is still logged (invariant #4).
            out.lines.push_back(fmt::format(
                "constellation {} — no secondary (X) channel family observed on {} "
                "station(s): SINGLE-FAMILY, no extra rows.", cst, t.primary_only));
            continue;
        }
        if (t.bridges > 0) {
            // A bridge exists: the primary pin reaches the secondary columns through
            // that receiver, so the direction is not null and an extra row would
            // over-pin a determinable quantity.  The constellation is not
            // partitioned, and the log line states why.  A mixed-family roster takes
            // this branch.
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
    // The row spans them with RHS Σ b̂_prev, which pins the change of their mean to
    // zero.  Each satellite is referenced to its own previous value, so membership
    // changes do not move the target.
    for (const auto& sat : present_sats) {
        const auto it = prev_vals.find(sat);
        if (it != prev_vals.end()) {
            out.jac_sats.push_back(sat);
            out.rhs += it->second;
        }
    }
    // No veteran (cold start, warm-resume miss, or every satellite new): fall back to
    // the legacy all-present Σ = 0 seed.  This is a real re-reference, and the driver
    // must signal it (reseed = true).
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

    // Group the registered clock satellites by constellation in a stable (lexical)
    // order so the fallback auto-pick is deterministic.  The lexical auto-pick is
    // made per epoch and is not sticky: a lexically lower satellite rising flips the
    // pivot, which is a datum re-reference.  Stickiness is opt-in via
    // sticky_prev_pivot below (network.sticky_pivot, RTA-1).
    std::map<std::string, std::vector<std::string>> by_const;
    for (const auto& sat : registered_clock_sats) {
        if (sat.empty()) continue;
        by_const[constellationOf(sat)].push_back(sat);
    }
    for (auto& [c, sats] : by_const) std::sort(sats.begin(), sats.end());

    // --- Broadcast-anchored clock datum (network.clock_datum_broadcast_anchored)
    // brdc_rts only: each satellite's Δδtˢ (relative to its own broadcast a-priori)
    // is anchored toward 0 by a soft single-entry constraint row at weight 1/σ².
    // The anchors are the gauge and are folded before the rank gate.  There is no
    // pivot (so no handover seam), and unlike sum-to-zero there is no redistribution
    // when membership changes: the common mode stays bound to the external broadcast
    // reference, so the absolute clock-product frame does not move.  The rows are
    // deliberately soft (σ ≈ 3 m, far weaker than the between-satellite data
    // information) so the estimable clock structure stays data-driven; the rank gate
    // still sees the common mode at about N/σ², far above the relative cutoff.
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

    // --- Sum-to-zero clock datum (network.clock_datum_sum_to_zero) -----------
    // Anchor-free: each constellation's clock common mode is fixed by a sum-to-zero
    // over that constellation's satellite clocks (Σ δtˢ = 0), realized like the
    // code-bias per-(const, signal) gauge: a tight all-ones row folded onto the
    // shared border before the rank gate.  No pivot satellite is chosen, so the
    // shared hard-pin datum and the clock_pivot map stay empty (there is no handover
    // seam) and the declared residual dimension is 0 (the folded rows close the
    // per-constellation clock deficiency).  Pivot overrides and stickiness do not
    // apply.
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

    // One satellite-clock pin per constellation (the clock gauge per timing system).
    // Pivot priority: (1) the declared config pin, if present in the registered set
    // (in both modes; the configured pin wins, including the deliberate flip-back
    // when a pinned satellite returns); (2) in sticky mode only, the incumbent pivot
    // from sticky_prev_pivot, kept while it is still registered; (3) the first
    // registered satellite of that constellation.
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
            // Sticky mode: keep the incumbent while it is registered.  When it is
            // absent, fall back to the lexical pick; the driver's emit-baseline map
            // then advances to the new pivot at the next emit, so the new pivot is
            // sticky in turn and the old one's return causes no flip-back (RTA-1).
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

std::vector<std::pair<std::string, double>> NetworkSbasis::clockNullDirection(
    const std::vector<std::string>& registered_clock_sats,
    const std::string& constellation) {
    // Same grouping and lexical sort as buildSharedDatum, so the direction spans the
    // columns that builder pinned this epoch (§8.1 rule 1: the direction must be the
    // structural one this class owns).
    std::vector<std::string> sats;
    for (const auto& sat : registered_clock_sats) {
        if (sat.empty()) continue;
        if (constellationOf(sat) == constellation) sats.push_back(sat);
    }
    std::sort(sats.begin(), sats.end());
    std::vector<std::pair<std::string, double>> out;
    out.reserve(sats.size());
    // Coefficient 1.0 on every satellite clock of the constellation: the gauge is the
    // common shift (header §Datum).  No other shared family has a component here; the
    // header Doxygen cites the measurement that confirms it.
    for (auto& s : sats) out.emplace_back(std::move(s), 1.0);
    return out;
}

std::vector<NetworkEstimator::SharedPrior> NetworkSbasis::buildCodeBiasKaulaPriors(
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    double sigma_m) {
    std::vector<NetworkEstimator::SharedPrior> out;
    // A zero sigma would mean info = 1/0 = inf, a hard pin on every admitted OSB:
    // the opposite of a loose physical bound, and an unannounced datum takeover.  A
    // non-positive (or NaN) sigma therefore returns no priors here, independent of the
    // loader's own check.
    if (!(sigma_m > 0.0)) return out;
    const double info = 1.0 / (sigma_m * sigma_m);
    out.reserve(registered_code_keys.size());
    // First-seen registration order, matching the contractual ordering of
    // buildCodeBiasDatumRows over the same vector.  The prior is diagonal, so the
    // order cannot change the folded information; keeping it identical makes the two
    // W6 datum products read the same way in a trace.
    for (const auto& kv : registered_code_keys) {
        NetworkEstimator::SharedPrior p;
        p.kind   = SharedParamKind::CodeBias;
        p.sat    = kv.first;
        p.signal = kv.second;
        p.value  = 0.0;   // the physical bound is centred on zero, not on an estimate
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

    // Same decomposition family and cutoff arithmetic as assertNullSpaceRank: the
    // completion must count what the gate will count, or it would complete against a
    // decision the gate does not make (the shared-tolerance rule of card DG2).
    // Eigenvectors are needed here because the rows are the eigenvectors.  This runs
    // only when armed, and the gate-failure drop-and-heal already pays for the same
    // decomposition on the epochs where the gate fails.
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(reduced_N_ss,
                                                      Eigen::ComputeEigenvectors);
    if (es.info() != Eigen::Success) return out;   // the gate reports the failure (throws)
    const Eigen::VectorXd& ev = es.eigenvalues();  // ascending
    const double smax   = ev.size() > 0 ? ev(ev.size() - 1) : 0.0;
    const double cutoff = tol * std::max(smax, 1.0);

    int observed = 0;
    for (Eigen::Index i = 0; i < ev.size(); ++i)
        if (ev(i) < cutoff) ++observed;
    out.observed = observed;
    // observed < declared is the gate's "over-specified" verdict (a tolerance below
    // this configuration's gauge depth); there is nothing to complete in either case.
    if (observed <= declared_dim) return out;

    // Per-direction degeneracy skip.  The structural gauges sit at machine zero (the
    // locals absorb them exactly).  An extra that is not separated from that cluster
    // is skipped: less than one decade above the running cluster top, or below the
    // machine-zero floor 1e-14·max(λmax, 1).  The floor is needed because the
    // relative test alone admits everything above an exactly-0.0 gauge (10·0 = 0).
    // Eigenvectors mix arbitrarily within a degenerate cluster, so a row there could
    // re-target a combination the declared pins own; an extra at machine zero is an
    // orphaned exact gauge (card PA's class).  Each skipped extra extends the cluster
    // (its |ev| joins the running top), so the first constrained extra is at least
    // one decade above everything left unpinned below it and cannot mix with any of
    // it.  With declared_dim == 0 there is no cluster to mix with, nothing is
    // skipped, and exact-zero orphans are legitimately completable (the
    // constraint-realized clock datums leave no pins to conflict with).  |·| is used
    // because near-zero eigenvalues carry a rounding sign.
    // Attribution, shared by constrained and skipped directions: every column at
    // |component| ≥ kSbasisAttrSpan, heaviest first.  The heaviest column is named
    // even when the direction is spread below that threshold, so every engage or
    // skip event identifies at least one column.
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

    // The folded row must land at least two decades above the armed cutoff;
    // otherwise the completion itself sits at the cutoff it is meant to clear (K2).
    if (kSbasisCompleteWeight < 100.0 * cutoff) {
        out.weight_marginal = true;
        return out;
    }
    // A deficiency burst beyond the cap indicates a pathological epoch (or a
    // mis-calibrated tolerance); no rows are emitted, the flag is counted, and the
    // gate withholds the epoch rather than pinning many directions at once.
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
    // Report the observed count on every exit that computed one, including the
    // throwing one: the caller cannot recover it from the exception, and
    // "over-specified" (observed < declared) must be countable separately from a
    // generic withhold (card DG6).  Observational only: nothing below reads it.
    if (observed_out) *observed_out = 0;
    // Same contract for λmax, from which the caller reconstructs the gate's cutoff
    // (invariant #7).  -1 means no spectrum was computed on this exit.
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

    // The reduced shared normals are symmetric (the kernel symmetrizes them) and
    // positive semidefinite (a Schur complement of weighted Gram normals, gated
    // before any indefinite modification such as the D2 prior), so their eigenvalues
    // equal their singular values.  A self-adjoint eigensolve (eigenvalues only)
    // therefore gives the same count as an SVD at a small fraction of JacobiSVD's
    // cost; this runs once per emitted epoch.  Eigenvalues come back ascending, so
    // the largest is the last entry.  The count is of eigenvalues below the relative
    // threshold (a true gauge has an eigenvalue near 0; a well-observed direction is
    // of the order of the largest).  A slightly negative gauge eigenvalue (rounding)
    // is still below the positive cutoff, so it is counted as the SVD magnitude would
    // be.
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
    NetworkSatGfDatum sat_gf_datum,
    const std::map<std::string, double>& sat_gf_target_m,
    bool sat_if_level_datum, bool sat_gf_datum_time_linked,
    bool sat_isc_state, const std::set<std::string>& isc_bridged,
    bool sat_gf_extra_band_free,
    const std::set<std::string>& extra_band_determined,
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
    std::vector<double>          zs;                   // per-row RHS target (all 0.0 unless code_datum_time_linked; card 2)

    // network.code_datum_time_linked (card 2): fetch a satellite's previous
    // kept-epoch solved code bias for a signal, the "veteran" reference.  With the
    // knob off, or at a first appearance, the lookup fails and the zero-target legacy
    // path runs (byte-identical).
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

    // (2) Per-(constellation, signal) sum-to-zero: one row per registered signal,
    // summed over the satellites that carry it (keyed "<constellation>|<signal>").
    // It pins every channel's absolute level per signal rather than per band, which
    // is robust to same-band channels that are not co-tracked at network scale (see
    // the header documentation).  The (sat, column) pairs are collected per channel
    // here (std::map, so sorted channel-key order); the rows are built after the
    // IF-pin loop so that their position in `rows` stays after the IF pins.  Under
    // time-linking each row spans the channel's veterans with RHS Σ b̂_prev instead
    // of all present satellites with RHS 0.
    std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> st0_chan;
    for (const auto& kv : registered_code_keys) {
        Eigen::Index col;
        try { col = est.codeBiasColumn(kv.first, kv.second); } catch (...) { continue; }
        const std::string skey =
            NetworkSbasis::constellationOf(kv.first) + "|" + kv.second;
        st0_chan[skey].emplace_back(kv.first, col);
    }

    // (constellation|signal) -> the satellites whose column the geometry-free datum
    // pinned this epoch.  Used below to suppress the sum-to-zero row of any channel
    // the datum already fixes completely; see the suppression block (RF-0d).
    std::map<std::string, std::set<std::string>> sat_gf_pinned;

    // The absolute ionosphere-free level row, accumulated per constellation as the
    // satellite loop finds each satellite's IF pin weights (RF-0e e1).  It is built in
    // the loop rather than in a second pass because a1/a2 are the pin's own post-hold
    // weights; deriving them again from the frequencies would disagree with the pin on
    // any satellite whose pair was held or re-selected.
    std::map<std::string, Eigen::VectorXd> if_level_row;
    std::map<std::string, int>             if_level_n;

    // (1) Per-satellite ionosphere-free pin (clock-common gauge).
    for (const std::string& sat : sat_order) {
        std::vector<std::string>& sigs = sat_signals[sat];
        // IF pin: the two lowest distinct-frequency code signals, a1 + a2 = 1.
        // The selection is deterministic in the signal set (sorted by band digit,
        // then full signal name), so same-band channels (C1C vs C1W) do not swap
        // with the insertion order of registered_code_keys and re-datum the emitted
        // clock; see NetworkSbasis::selectIfPinPair.
        const auto if_pin = NetworkSbasis::selectIfPinPair(
            sigs, [&](const std::string& s) { return freqOf(sat, s); });
        std::string sig1 = if_pin.first;
        std::string sig2 = if_pin.second;
        double      f1   = sig1.empty() ? 0.0 : freqOf(sat, sig1);
        double      f2   = sig2.empty() ? 0.0 : freqOf(sat, sig2);
        // network.clock_signal_basis_hold: anchor this satellite's clock to one
        // ionosphere-free reference pair for the whole run, so the emitted (δtˢ, dˢ)
        // split is time-consistent.  Re-selecting the pair from each epoch's
        // coverage re-references the clock by the satellite's inter-channel code
        // bias whenever a sparse channel enters or leaves the ≥2-station set (the
        // defect-2 differential-clock wander, which a receiver clock cannot absorb).
        // With the hold off, held_clock_ref is neither read nor written and the
        // deterministic selection above stands (byte-identical).
        if (clock_signal_basis_hold) {
            // Is a held reference signal still in this satellite's registered
            // ≥2-station set this epoch?
            auto held_present = [&](const std::string& s) {
                return !s.empty() &&
                       std::find(sigs.begin(), sigs.end(), s) != sigs.end();
            };
            auto held = held_clock_ref.find(sat);
            if (held != held_clock_ref.end() &&
                held_present(held->second.first) && held_present(held->second.second)) {
                // Hold: both held reference signals are still registered, so reuse
                // them and keep the clock on the prior signal basis.
                sig1 = held->second.first;
                sig2 = held->second.second;
                f1   = freqOf(sat, sig1);
                f2   = freqOf(sat, sig2);
            } else if (!sig2.empty() && f2 > 0.0) {
                // Latch (first sighting) or re-latch (a held reference signal left
                // the ≥2-station set): adopt the newly selected pair and hold it
                // forward.  A re-latch is a real (rare) datum move and is logged as
                // an event (network.trace_events); the emitted clock step it causes
                // equals the satellite's inter-channel bias.
                if (held != held_clock_ref.end() &&
                    (held->second.first != sig1 || held->second.second != sig2)) {
                    emitEvent("CLK_BASIS", "relatch", sat, "", std::nullopt,
                              std::nullopt, "held_ref_change",
                              held->second.first + "+" + held->second.second +
                                  "->" + sig1 + "+" + sig2);
                    // The re-latch stepped the emitted clock↔code split: signal it as a
                    // per-satellite discontinuity (bumped at the next emit) so the consumer
                    // resets instead of integrating across the step (T3c).  Guarded by the
                    // clock-disc channel; the CLK_BASIS event above is logged either way.
                    if (emit_clock_disc) basis_relatch_disc_pending.insert(sat);
                }
                held_clock_ref[sat] = { sig1, sig2 };
            }
        }
        // Hold off: the pin is re-derived from each epoch's coverage.  With the
        // deterministic selection above, the pair is a pure function of the
        // registered signal set, so a change between emitted products means a
        // channel entered or left the ≥2-station set: the same defect-2 clock↔code
        // re-reference that a hold re-latch signals, here occurring on every
        // coverage change without an event.  This epoch's selection is recorded in
        // the main-pass scratch, and the compare and disc bump are deferred to the
        // emit site (against the last emitted pair), so a transient pin move on a
        // withheld epoch cannot produce a bump; prev_pivot and emitted_anchor follow
        // the same emit-only rule.  Guarded by emit_clock_disc and a real IF pair
        // (f1, f2 > 0), so it is byte-identical when the disc channel is off or the
        // satellite is single-frequency.  Skipped under hold, whose re-latch
        // signalling is above.
        if (!clock_signal_basis_hold && emit_clock_disc && f1 > 0.0 && f2 > 0.0)
            this_epoch_if_pin_sel[sat] = { sig1, sig2 };
        // The S-contract §1.5 declaration record is kept separate from the disc
        // scratch above and is deliberately not guarded by either knob.  sig1/sig2 are
        // post-hold here, so this is the pair the datum realizes on both the held and
        // the re-selected path.  The scratch above cannot serve as the declaration: it
        // is gated on the QC-channel knob emit_clock_disc (default false), and a
        // product whose run leaves it off would declare no cbpin= token.  The record is
        // read only by the NetworkSolPrep fill, so the solve and every emitted numeric
        // value are unchanged; only the S line changes.  It is written below, after the
        // row is known to have folded (see the record site).
        Eigen::VectorXd row = Eigen::VectorXd::Zero(nshared);
        bool row_ok = false;
        double zval = 0.0;   // legacy zero target unless time-linked (card 2)
        if (f1 > 0.0 && f2 > 0.0) {
            const double den = f1 * f1 - f2 * f2;
            const double a1 =  f1 * f1 / den;     // ionosphere-free (clock-preserving) weights
            const double a2 = -f2 * f2 / den;
            try {
                row(est.codeBiasColumn(sat, sig1)) = a1;
                row(est.codeBiasColumn(sat, sig2)) = a2;
                row_ok = true;
            } catch (...) { row.setZero(); row_ok = false; }
            // Time-linking (card 2, the cb_time_linked block below) pins the change
            // of the clock↔code split: the RHS is the satellite's previous solved
            // ionosphere-free combination a1·b̂_prev(σ1) + a2·b̂_prev(σ2).  That is
            // continuity only when the pair is unchanged from the last kept epoch and
            // both signals have a previous solved value.  A pair change (the band-1
            // C1C↔C1W flip, or a band entering or leaving) re-references the split by
            // the satellite's inter-channel bias even when both new signals have prior
            // values, so the row re-seeds to 0 and the satellite is marked for a disc
            // bump; otherwise the emitted clock would step without a signal.  The bump
            // (this_epoch_cb_reseed) applies to any re-seed that is not the
            // satellite's first appearance (seen before, or the run resumed), so the
            // value step coincides with a signalled discontinuity.  This epoch's pair
            // is recorded for the next epoch's comparison (persisted at the harvest,
            // kept epochs only).
            // Accumulate this satellite into its constellation's absolute
            // ionosphere-free level row, using the pin's own a1/a2 (RF-0e e1).  Gated
            // on row_ok so a satellite whose columns could not be resolved is not
            // summed into a row that would then span a column it never contributed.
            if (row_ok && sat_if_level_datum) {
                const std::string cst = NetworkSbasis::constellationOf(sat);
                auto it = if_level_row.find(cst);
                if (it == if_level_row.end())
                    it = if_level_row.emplace(cst, Eigen::VectorXd::Zero(nshared)).first;
                try {
                    it->second(est.codeBiasColumn(sat, sig1)) += a1;
                    it->second(est.codeBiasColumn(sat, sig2)) += a2;
                    ++if_level_n[cst];
                } catch (...) { /* resolved above; defensive only */ }
            }
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
            // S-contract §1.5 declaration record, placed after the push so it records
            // only a pair whose IF row folded.  Guarded on a real dual-frequency pin
            // (f1, f2 > 0), which also excludes the degenerate single-frequency
            // all-ones branch above: that branch sets row_ok but realizes no IF pair.
            // A record made before the fold (guarded on f1, f2 alone) would let a
            // satellite whose codeBiasColumn lookup threw enter the emit-site unanimity
            // tally as a satellite without a GF row; that reads as a partial split and
            // suppresses a legitimate gfpin= declaration.  The GF record below sits
            // inside its own success branch for the same reason.
            if (f1 > 0.0 && f2 > 0.0) this_epoch_if_pin_real[sat] = { sig1, sig2 };
        }
        // (1a) Hard per-satellite geometry-free datum (network.sat_gf_datum, RF-0c).
        //
        // The same estimable function as the (1b) block below, Odijk 2016 Eq. (30),
        // (d_2 - d_1)/(mu_2 - mu_1) on the ionosphere-free pin's own pair.  It is a
        // separate block because it belongs to a different card, with a different
        // precondition, a different RHS and its own counters.  (1b) belongs to
        // network.code_bias_gf_datum, which the loader refuses under
        // frequency_mode = dual (there the IF and GF pins together leave nothing
        // estimable, as Odijk §5.1 states).  RF-0c is armed independently of that gate
        // because under dual, "nothing estimable on the pinned pair" is the correct
        // result rather than a defect; without the row the directions stay open (RF-0b
        // measured 31 per constellation, held only by a 3 m shrinkage prior).  The
        // loader rejects the configuration when both knobs are armed, so the two blocks
        // cannot both fold a row for the same satellite.
        //
        // Why the target may be non-zero here while (1b)'s must be 0: d^s_GF is a datum
        // quantity, so any value is legal and the estimable parameters absorb the
        // difference (Odijk 2016 Table 8: the geometry-free lump moves into the slant
        // ionosphere, iota~ = iota + d_r,GF - d^s_GF).  Zero is the conventional choice;
        // the broadcast T_GD is the physical one, and it composes with the IF pin
        // exactly.  IS-GPS-200 §20.3.3.3.3.2 defines the single-frequency corrections
        // Δt_L1 = Δt_SV - T_GD and Δt_L2 = Δt_SV - gamma*T_GD, gamma = (f1/f2)^2, against
        // a broadcast clock referenced to the L1/L2 ionosphere-free combination, so
        // d^s_1 = c*T_GD and d^s_2 = c*gamma*T_GD in this model's sign convention.
        // - Their IF combination a1*d_1 + a2*d_2 is identically zero (a1 + a2*gamma = 0
        //   follows from the ionosphere-free weights), so the T_GD target does not
        //   conflict with the IF pin's own zero target.
        // - Their geometry-free combination is
        //   (d_2 - d_1)/(mu_2 - mu_1) = c*T_GD*(gamma - 1)/(gamma - 1) = c*T_GD exactly.
        // That identity is why the RHS below is the latched value in metres, with no
        // frequency factor: it is already the value of the pinned combination.
        //
        // mu_2 here is relative to sig1 (the builder's convention, mu_1 = 1), so the
        // identity holds only when the pinned pair is the pair the broadcast group delay
        // describes: (L1, L2) for GPS T_GD, (E1, E5a) for Galileo BGD(E1, E5a) (card
        // GAL; the rule lives in satGfBroadcastTarget).  Any other pair falls back to a
        // zero target and is counted, because a group delay asserted for a pair it does
        // not describe would be an arbitrary datum presented as a physical one.
        if (sat_gf_datum != NetworkSatGfDatum::NoneMode && f1 > 0.0 && f2 > 0.0) {
            const double mu2_minus_mu1 = (f1 * f1) / (f2 * f2) - 1.0;
            if (std::abs(mu2_minus_mu1) > 1e-12) {
                Eigen::VectorXd srow = Eigen::VectorXd::Zero(nshared);
                bool s_ok = false;
                try {
                    srow(est.codeBiasColumn(sat, sig1)) = -1.0 / mu2_minus_mu1;
                    srow(est.codeBiasColumn(sat, sig2)) =  1.0 / mu2_minus_mu1;
                    s_ok = true;
                } catch (...) { srow.setZero(); s_ok = false; }
                if (s_ok && srow.cwiseAbs().sum() > 0.0) {
                    double target = 0.0;
                    // The same branches, counted per constellation (card GAL, R3 iii).
                    SatGfConstCounts& gcc =
                        out.sat_gf_by_const[NetworkSbasis::constellationOf(sat)];
                    if (sat_gf_datum == NetworkSatGfDatum::BroadcastTgd) {
                        // The pair must be one the latched group delay describes: (band 1,
                        // band 2) for GPS (T_GD), (band 1, band 5) for Galileo (BGD(E1,
                        // E5a), card GAL).  The rule and its order of tests live in
                        // satGfBroadcastTarget, which the extra-channel anchor below
                        // reads too, so the two cannot disagree about a satellite.
                        const SatGfTarget gt =
                            NetworkSbasis::satGfBroadcastTarget(sat, sig1, sig2,
                                                                sat_gf_target_m);
                        switch (gt.kind) {
                        case SatGfTargetKind::NoGroupDelay:
                            ++out.n_sat_gf_pair_not_l1l2;
                            ++gcc.pair_no_gd;
                            break;
                        case SatGfTargetKind::Missing:
                            ++out.n_sat_gf_tgd_missing;
                            ++gcc.tgd_missing;
                            break;
                        case SatGfTargetKind::BroadcastZero:
                            ++out.n_sat_gf_tgd_zero;   // broadcast says zero: still physical
                            ++gcc.tgd_zero;
                            break;
                        case SatGfTargetKind::BroadcastHit:
                            target = gt.target_m;
                            ++out.n_sat_gf_tgd_hits;
                            ++gcc.tgd_hits;
                            break;
                        }
                    }
                    // The absolute target of this satellite, saved before the time
                    // link below replaces the value with the carried one, because the
                    // extra-channel anchor at a first sighting must assert the value
                    // this pair was pinned at (card GAL, R3 ii).
                    const double abs_target = target;
                    // Time-link the geometry-free row (RF-0e e2).  Without it, RF-0c and
                    // RF-0d re-assert the absolute target every epoch while the IF pins
                    // beside them are change pins under code_datum_time_linked (the
                    // cb_time_linked block above), so two row families act on the same
                    // satellite's columns with different reference epochs.  Carrying the
                    // previous kept epoch's solved geometry-free combination makes them
                    // agree; the absolute target remains as the first-sighting anchor,
                    // the only place the broadcast frame needs to be asserted.
                    if (sat_gf_datum_time_linked) {
                        double p1 = 0.0, p2 = 0.0;
                        if (prevCb(sat, sig1, p1) && prevCb(sat, sig2, p2)) {
                            target = (p2 - p1) / mu2_minus_mu1;
                            ++out.n_sat_gf_carried;
                            ++gcc.carried;
                        } else {
                            ++out.n_sat_gf_anchored;   // first sighting: absolute
                            ++gcc.anchored;
                        }
                    }
                    rows.push_back(std::move(srow));
                    zs.push_back(target);
                    ++out.n_sat_gf_rows;
                    ++gcc.rows;
                    sat_gf_pinned[NetworkSbasis::constellationOf(sat) + "|" + sig1]
                        .insert(sat);
                    sat_gf_pinned[NetworkSbasis::constellationOf(sat) + "|" + sig2]
                        .insert(sat);

                    // --- Per-signal extension (RF-0d) ----------------------------
                    // The pair row above closes every rank-deficiency-3b direction (the
                    // mu-shift moves all of the satellite's columns together, so one row
                    // on the pair reaches it; shown on a 4-signal roster).  It does not
                    // make the extra channels' levels a datum: d(C2X) stays determined
                    // by the stations that track C2X, which can be a sparse set, and a
                    // sparse set leaves a large within-arc scatter on that channel's
                    // phase-bias wire (measured in RF-0c).
                    //
                    // The extra signals are therefore pinned too.  Each extra signal k is
                    // pinned through its geometry-free combination against the band-1
                    // reference the pair pin uses, (d_k - d_ref)/(mu_k - 1), at the same
                    // c*T_GD.  Under the broadcast convention d_j = c*T_GD*mu_j, so
                    // (d_k - d_ref)/(mu_k - 1) = c*T_GD for every band k, and one latched
                    // value serves the whole satellite.
                    //
                    // What the row asserts: an inter-signal correction (ISC) of zero
                    // between the extra channel and the pinned pair.  RINEX navigation
                    // carries no ISC for GPS, only T_GD (tgd0 in
                    // BroadcastEphemerisTypes.hpp), so there is no broadcast value to use.
                    // The real ISC is centimetre-class between same-band channels and is
                    // a datum quantity here like the rest: a consumer that applies the
                    // emitted ionosphere and code biases as a pair absorbs it.  Every
                    // such row is counted, so the product reports how much of it is
                    // asserted rather than broadcast.
                    //
                    // Extras in the reference band are skipped, not pinned: with
                    // mu_k = mu_ref their geometry-free combination does not exist (the
                    // divisor is zero), and their difference to the reference is the pure
                    // ISC, which needs no ionosphere to be observable and is estimable
                    // from the data at any coverage.  Pinning it would discard real
                    // information; leaving it free costs nothing.
                    for (const std::string& sk : sigs) {
                        if (sk == sig1 || sk == sig2) continue;
                        const double fk = freqOf(sat, sk);
                        if (!(fk > 0.0)) { ++out.n_sat_gf_extra_nofreq; continue; }
                        const double mu_k = (f1 * f1) / (fk * fk);
                        if (std::abs(mu_k - 1.0) <= 1e-12) {
                            ++out.n_sat_gf_sameband_free;   // ISC stays estimable
                            ++gcc.sameband_free;
                            continue;
                        }
                        // --- Band-2 same-band companion (RF-0f) ----------------------
                        // The pin's second band gets the treatment the first band gets
                        // above: a companion sharing sig2's band differs from sig2 by a pure
                        // inter-signal correction, with no ionosphere (equal frequency, so
                        // the iono terms cancel exactly) and no clock.  It needs no datum to
                        // be observable, only a station that tracks both signals.  Without
                        // network.sat_isc_state it is pinned below at c*T_GD, which asserts
                        // ISC = 0, a value RINEX navigation does not carry (the cost was
                        // measured in RF-0e).
                        //
                        // With the ISC state armed, the companion is left free where it is
                        // observable, and only there.  A satellite with no station tracking
                        // both signals this epoch has an ISC the data cannot reach; that
                        // column falls through to the pin below and is counted separately
                        // (n_isc_pinned), so the wire can report which satellites' C2X is an
                        // estimate and which is an assertion.
                        const bool same_band_as_sig2 =
                            sig2.size() >= 2 && sk.size() >= 2 && sk[1] == sig2[1];
                        if (sat_isc_state && same_band_as_sig2) {
                            if (isc_bridged.count(sat + "|" + sk)) {
                                ++out.n_isc_observed;   // free: the data determines it
                                continue;
                            }
                            ++out.n_isc_pinned;         // unobservable: pinned below, flagged
                        }
                        // --- Extra band, not a companion (B5D) -----------------------
                        // The band-2 companion above can be freed because its difference to
                        // the pinned signal is a pure inter-signal correction the data
                        // reach without any ionosphere.  A third band is a different
                        // quantity and needs a different argument (card TBI): with the slant
                        // ionosphere and the satellite clock already fixed by the pinned
                        // pair, a cell carrying code on both of the pair's bands and on this
                        // signal also determines d_k, up to one constant per (constellation,
                        // signal) family, and that constant is what the receiver's own
                        // extra-band code bias absorbs.  The channel's sum-to-zero row is
                        // the constant's datum, and it stays active below because the
                        // completeness test suppresses only a channel all of whose columns
                        // this loop pinned.
                        //
                        // Cost of the pin (TBI section 5): the row asserts an inter-signal
                        // correction of zero, which is centimetres between same-band
                        // channels but metres across bands (GPS Block III and Block IIF
                        // satellites differ by several metres in the triple-code
                        // combination).  The network pushes the unmodelled part into band 2,
                        // and a dual-frequency consumer replayed on the triple wire lags by
                        // the same amount.
                        //
                        // Where the data do not reach the column, the row still must not
                        // assert the broadcast value.  Once the family is in the sum-to-zero
                        // frame, folding c*T_GD on one of its columns is the RF-0d defect in
                        // reverse: an absolute target and a carried level acting on the same
                        // channel at the same gauge weight.  The column therefore carries its
                        // own previous geometry-free combination when one exists, and at a
                        // first sighting is held with a level row in the channel's own frame
                        // (below) and kept off the wire.  Both cases are counted, which is
                        // the data standing invariant #3 requires for an "inert" claim.
                        bool extra_band_carry = false;
                        bool extra_band_hold  = false;
                        if (sat_gf_extra_band_free && sig1.size() >= 2 && sig2.size() >= 2
                            && sk[1] != sig1[1] && sk[1] != sig2[1]) {
                            // The roster is keyed on the pin pair the builder realized,
                            // not on an assumed (L1, L2): a held or re-selected pair
                            // changes which two bands the determination needs.
                            const std::string dkey = sat + "|" + sk + "|"
                                                   + std::string{sig1[1], sig2[1]};
                            if (extra_band_determined.count(dkey)) {
                                ++out.n_extra_band_free;   // free: the data determine it
                                continue;
                            }
                            double q1 = 0.0, qk = 0.0;
                            if (prevCb(sat, sig1, q1) && prevCb(sat, sk, qk)) {
                                extra_band_carry = true;   // its own previous combination
                                ++out.n_extra_band_carried;
                            } else {
                                extra_band_hold = true;    // first sighting: hold + withhold
                            }
                        }
                        if (extra_band_hold) {
                            // A single +1 on the column, target 0, in the channel's own
                            // frame, where the sum-to-zero row puts the family's level near
                            // zero.  The row asserts that the new column sits at its
                            // family's level, not at the broadcast T_GD.  Deliberately not
                            // recorded in sat_gf_pinned: the hold's zero is referred to the
                            // channel's sum-to-zero row, so suppressing that row would
                            // leave the hold without a reference.  The value does not reach
                            // a consumer (NetworkSolPrep withholds it), because an asserted
                            // bias cannot be told from an estimate by its value alone (the
                            // RF-0f rule).
                            Eigen::VectorXd hrow = Eigen::VectorXd::Zero(nshared);
                            bool h_ok = false;
                            try {
                                hrow(est.codeBiasColumn(sat, sk)) = 1.0;
                                h_ok = true;
                            } catch (...) { hrow.setZero(); h_ok = false; }
                            if (h_ok) {
                                rows.push_back(std::move(hrow));
                                zs.push_back(0.0);
                                ++out.n_extra_band_held;
                                out.extra_band_held.insert(sat + "|" + sk);
                            }
                            continue;
                        }
                        Eigen::VectorXd krow = Eigen::VectorXd::Zero(nshared);
                        bool k_ok = false;
                        try {
                            krow(est.codeBiasColumn(sat, sig1)) = -1.0 / (mu_k - 1.0);
                            krow(est.codeBiasColumn(sat, sk))   =  1.0 / (mu_k - 1.0);
                            k_ok = true;
                        } catch (...) { krow.setZero(); k_ok = false; }
                        if (k_ok && krow.cwiseAbs().sum() > 0.0) {
                            // The same c*T_GD by the identity above, except under e2, where
                            // this row carries its own previous geometry-free combination
                            // (d_k - d_ref)/(mu_k - 1).  Reusing the pair's carried value
                            // here would time-link the extra column to a combination it is
                            // not part of.
                            // An undetermined extra-band column carries even when the e2
                            // time link is off (B5D): the R3 rule "no absolute
                            // broadcast-frame value in a sum-to-zero family" belongs to
                            // sat_gf_extra_band_free, not to the e2 knob (the loader warns
                            // about the mixed frame that combination leaves).
                            // extra_band_carry is set only when both previous values
                            // exist, so the anchored fallback below is unreachable for it.
                            double k_target = target;
                            if (sat_gf_datum_time_linked || extra_band_carry) {
                                double q1 = 0.0, qk = 0.0;
                                if (prevCb(sat, sig1, q1) && prevCb(sat, sk, qk)) {
                                    k_target = (qk - q1) / (mu_k - 1.0);
                                    ++out.n_sat_gf_carried;
                                    ++gcc.carried;
                                } else {
                                    // The first-sighting anchor asserts the target the pair
                                    // was pinned at (abs_target, from satGfBroadcastTarget),
                                    // not the latched value unconditionally; otherwise a
                                    // Galileo C5X column could be anchored at c*BGD while
                                    // its (C1C, C5Q) pair sits at zero (card GAL, R3 ii).
                                    // For a GPS (L1, L2) pair the two give the same number
                                    // on every branch (hit, broadcast zero, missing
                                    // record), so a GPS-only run is byte-identical.
                                    k_target = abs_target;
                                    ++out.n_sat_gf_anchored;
                                    ++gcc.anchored;
                                }
                            }
                            rows.push_back(std::move(krow));
                            zs.push_back(k_target);
                            ++out.n_sat_gf_extra_rows;
                            ++gcc.extra_rows;
                            if (sat_gf_datum == NetworkSatGfDatum::BroadcastTgd) {
                                ++out.n_sat_gf_isc_zero;
                                ++gcc.isc_zero;
                            }
                            sat_gf_pinned[NetworkSbasis::constellationOf(sat) + "|" + sk]
                                .insert(sat);
                        }
                    }
                }
            }
        }
        // (1b) Per-satellite geometry-free pin: Odijk 2016 rank deficiency 3b
        // (Table 7), armed only in the slant posture (the loader refuses it alongside
        // an ionosphere constraint).  Odijk Eq. (30):
        //
        //     mu_GF = [-1, 1, 0, ..., 0]^T / (mu_2 - mu_1),   mu_j = (f_1/f_j)^2
        //
        // so the pinned combination is (d_2 - d_1)/(mu_2 - mu_1) = 0 on the pair the IF
        // pin uses.  The pair is deliberately the same: IF and GF then span the pinned
        // pair's whole 2-D subspace, which drives d_1 = d_2 = 0 exactly.  This realizes
        // Odijk's "code biases are not estimable for the first two frequencies" (§5.1)
        // as a datum.  What remains estimable is the modernized DCB d^s_1 - d^s_j for
        // j >= 3, the quantity the third band exists to provide.
        //
        // Target 0 is the S-basis choice.  d^s_GF is a datum quantity: its value is
        // arbitrary and the estimable parameters absorb it (Odijk Table 8: the
        // geometry-free biases move into the slant ionosphere,
        // iota~ = iota + d_r,GF - d^s_GF).  A zero target is therefore safe here, unlike
        // a zero-target ambiguity pin, which conflicts with an injected integer truth
        // and discharges km-scale tension into the field (§12.8.1).  The quantity
        // pinned here is metre-scale and a pure gauge.
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
                    // pair, not the IF pin alone, which is a different estimable
                    // function on the same wire (the pinned pair's B records are
                    // identically the datum; only j >= 3 carries content, and the
                    // geometry-free lump has moved into the slant ionosphere).  It is
                    // recorded so the S line can declare it: a consumer or grader that
                    // reads the B records under the single-layer assumption applies the
                    // wrong S-transform, and nothing else on the wire distinguishes the
                    // two postures.
                    this_epoch_gf_pin_real.insert(sat);
                    // A satellite with no signal outside the pinned pair has nothing
                    // estimable left on the code side once IF and GF are both pinned.
                    // That is correct under Odijk, but it cannot be seen downstream (an
                    // all-datum satellite emits values indistinguishable from a
                    // well-determined zero), so it is counted.
                    bool has_third = false;
                    for (const std::string& s : sigs)
                        if (s != sig1 && s != sig2) { has_third = true; break; }
                    if (!has_third) ++out.n_gf_only_pair;
                }
            }
        }
        // (1c) FAMILY-GF: one extra geometry-free row per partitioned secondary channel
        // family (docs/SPEC_family_gf_datum.md §2).  Odijk rank deficiency 3b applies
        // per receiver channel family.  The pin above is built on the satellite's two
        // lowest-ordered signals, which on a Galileo Q+X roster is always the Q pair
        // (C1C, C5Q), and with the station population partitioned by family no
        // observation of an X station touches a pinned channel.  The X subsystem
        // therefore keeps its own null direction per satellite,
        //     Delta d_jx = mu_j * delta   (code and phase together),
        // compensated by iota -> iota - delta on every X-station line of sight.  An
        // estimability dump shows this direction (band weights 1 : 1.7933 : 1.7032 = mu
        // on E1/E5a/E5b), and without this row the null-space gate withholds every epoch
        // of such a posture.
        //
        // Geometry-free only, never an IF row.  The satellite clock is shared with the
        // primary family and is already realized by the primary IF pin plus the
        // per-constellation clock datum, so the secondary family's IF combination has a
        // fixed reference and is estimable: it is the per-satellite inter-family ISC, a
        // real product quantity.  Pinning it would over-pin a data-determined quantity
        // and inject tension into the solution.  The IF row is in any case structurally
        // blind to this direction: with a1 = f1^2/(f1^2-f2^2) and a2 = -f2^2/(f1^2-f2^2),
        // a1*mu_1 + a2*mu_2 = a1 + a2*(f1^2/f2^2) = 0 identically.
        //
        // Target 0, for the same reason as the primary GF row.  The distinction from
        // `network.sbasis_complete` matters: that knob closes the same null space
        // generically, by constraining the numerically found direction at the carried
        // level, and on this posture it passes the rank gate but drives the ionosphere
        // level to an unphysical value on a cold start (a00 of several thousand TECU,
        // SPEC_synthetic_minimum_network §12 rung 3).  A gauge quantity pinned at 0 is a
        // datum choice the estimable parameters absorb (Odijk Table 8, per family); the
        // same direction pinned at a cold carried value is a hard constraint at a wrong
        // value.  Passing the gate is therefore not the acceptance test for this row; the
        // ionosphere level is (docs/SPEC_family_gf_datum.md §6.0).
        if (gf_datum && !gf_family_constellations.empty() &&
            gf_family_constellations.count(NetworkSbasis::constellationOf(sat))) {
            // The family's own signal subset, and its own pin pair chosen by the same
            // selector and the same frequency source as the primary pin, so the
            // determinism contract holds by construction rather than through a parallel
            // implementation.
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
                        // S-contract §1.5: record the secondary family's realized pair so
                        // the S line can declare the '+'-joined per-family gfpin=.  Without
                        // it the emit-site fill sees only the primary pair and declares a
                        // single-family token on a two-family product, which the grader
                        // accepts and then applies the primary family's transform to the
                        // secondary family's channels.
                        this_epoch_family_gf_pin_real[sat] = { fs1, fs2 };
                        // Same accounting as n_gf_only_pair, but over the family's signals:
                        // `sigs` spans both families here, so a satellite whose X family is
                        // a bare pair would otherwise always appear to have a third band
                        // (its Q channels) and would never be counted.
                        bool fam_has_third = false;
                        for (const std::string& s : fam_sigs)
                            if (s != fs1 && s != fs2) { fam_has_third = true; break; }
                        if (!fam_has_third) ++out.n_family_gf_only_pair;
                    }
                }
            }
        }
    }
    // (1c) The absolute ionosphere-free level row, one per constellation (RF-0e).
    //
    // Pushed after the per-satellite loop and before the sum-to-zero rows, so the row
    // order stays IF pins (with their GF companions), then level, then sum-to-zero, and
    // a reader of the stacked H can tell which family a row came from by its position.
    //
    // What it constrains, and what it deliberately does not.  The geometry-free pins
    // fix each satellite's code-bias differences.  Once code_datum_time_linked turns the
    // IF pins into change pins (the cb_time_linked block in the satellite loop), nothing
    // else in this builder fixes the ionosphere-free level absolutely: each satellite is
    // anchored at its own first sighting and carried afterwards at a finite weight, so
    // the level of the set can walk and take the clock / code-bias split with it.  This
    // row restores it.  It is deliberately one row per constellation, not one per
    // satellite: per-satellite absolute pins would overwrite the carry the time link
    // exists to provide.  It is not weak, though: its Jacobian carries one entry per
    // satellite, so at equal gauge weight its leverage on the level outweighs the n
    // single-entry change pins, and the realized level lands at carried/(n+1).  The
    // level is re-zeroed each epoch; only the differences carry.
    //
    // Under broadcast_tgd the target 0 is exact, not conventional.  The IS-GPS-200
    // broadcast clock is referenced to the L1/L2 ionosphere-free combination, so a
    // satellite whose code biases are the broadcast ones has
    // d_IF = a1*c*T_GD + a2*c*gamma*T_GD = 0 identically (a1 + a2*gamma = 0 follows from
    // the weights).  A pinned roster therefore satisfies this row with zero residual:
    // the two datum families are consistent by construction, which the per-signal
    // sum-to-zero rows are not (see the suppression block below).
    if (sat_if_level_datum) {
        for (auto& [cst, lrow] : if_level_row) {
            if (lrow.cwiseAbs().sum() <= 0.0) continue;
            rows.push_back(std::move(lrow));
            zs.push_back(0.0);                 // absolute: the level, not its change
            ++out.n_if_level_rows;
            out.n_if_level_sats += if_level_n[cst];
        }
    }
    // (2) Build the sum-to-zero rows now (after the IF pins, preserving row order).
    // Time link off: span every present satellite with RHS 0 (byte-identical).
    // Time-linked: span the channel's veterans with RHS Σ b̂_prev (pinning the change
    // of the veteran mean).  A channel with no veteran falls back to the all-present
    // Σ = 0 seed and disc-bumps its present satellites, since the level really does
    // re-reference; first-ever appearances are excepted, as in the IF-pin rule.
    for (const auto& [skey, entries] : st0_chan) {
        // --- Suppress a channel the geometry-free datum already fixes (RF-0d) ---
        // The sum-to-zero row asserts Sum_s d^s_j = 0 over the channel's satellites.
        // Once the geometry-free datum has pinned every one of those columns, that row
        // is not only redundant but inconsistent with them: the pinned values sum to
        // mu_j * c * Sum_s T_GD^s, which is far from zero (GPS T_GD is not centred on
        // zero, roughly -12..+7 ns, so the sum over about 32 satellites is tens of
        // metres).  Both families fold at the same gauge weight, and the least-squares
        // compromise shifts every emitted code bias of the channel by -Sum/(n+1), a
        // systematic error that the wire carries and nothing declares.  Under `zero`
        // the two agree and the row is only redundant.  The suppression is correct in
        // both modes and is applied in both, so the two modes differ only by the datum
        // value and not structurally.
        //
        // The test is completeness, per channel: a channel with even one unpinned
        // satellite column keeps its row, because that column's level is then still
        // free and the sum-to-zero row is the only constraint holding it.
        if (sat_gf_datum != NetworkSatGfDatum::NoneMode) {
            const auto pit = sat_gf_pinned.find(skey);
            if (pit != sat_gf_pinned.end()) {
                bool all_pinned = true;
                for (const auto& [sat, col] : entries) {
                    (void)col;
                    if (!pit->second.count(sat)) { all_pinned = false; break; }
                }
                if (all_pinned) { ++out.n_s2z_suppressed; continue; }
            }
        }
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
            if (tl.reseed) {
                // A re-seed is the channel's level re-referencing, so it is an event
                // at both edges (standing invariant #7, B5D).  The marks (the
                // decoupling edge) drive the emit-site disc bumps; the counters let a
                // run summary tell "the level never re-referenced" apart from "nothing
                // was instrumented".  They are counted here rather than at the emit
                // site because a withheld epoch does not reach that site, and its
                // re-seed is still a fact about the datum this run realized.
                ++out.n_s2z_reseed_rows;
                for (const auto& sat : present_sats)
                    if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_) {
                        this_epoch_cb_reseed.insert(sat);
                        ++out.n_s2z_reseed_sats;
                    }
            }
        } else {
            for (const auto& [sat, col] : entries) srow(col) += 1.0;
        }
        if (srow.cwiseAbs().sum() > 0.0) {
            rows.push_back(std::move(srow));
            zs.push_back(zval);
        }
    }
    // this_epoch_cb_reseed now holds every satellite whose datum re-referenced this
    // epoch (card 2).  It is not merged into the persistent cb_reseed_disc_pending
    // here; the caller defers that merge to the emit site (NetworkSolPrep), which
    // every whole-epoch withhold path skips.  A transient re-seed on a withheld epoch
    // (the frame reverts by the next kept epoch) therefore never enters the pending
    // set and cannot fire a bump without an emitted value step.  This is the emit-only
    // rule, and it needs no erase; an erase would drop a bump legitimately deferred
    // for a satellite re-seeded while pruned from the emission.

    if (!rows.empty()) {
        Eigen::MatrixXd H(static_cast<Eigen::Index>(rows.size()), nshared);
        for (size_t r = 0; r < rows.size(); ++r)
            H.row(static_cast<Eigen::Index>(r)) = rows[r];
        // Per-row targets (card 2): all 0 on the legacy path (byte-identical); the
        // veteran-referenced and previous-IF values under time-linking.
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
