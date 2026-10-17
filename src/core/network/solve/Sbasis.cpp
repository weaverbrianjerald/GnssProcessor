/**
 * @file Sbasis.cpp
 * @brief Implementation of the network producer's minimum-constraint S-basis
 *        (shared clock datum builder + null-space rank gate).  See the header
 *        for the datum derivation.
 */

#include "network/solve/Sbasis.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents

#include "common/GnssIdentity.hpp"     // systemOfSat, constellationKey, SignalId, obsTypeOf
#include "filter/StateKeySchema.hpp"   // StateKeys::parseSatBiasKey

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

// The run-end reports at the end of this file:
#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include <cstddef>
#include <memory>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

std::pair<std::string, std::string> NetworkSbasis::selectIfPinPair(
    std::vector<std::string> sigs,
    const std::function<double(const std::string&)>& freqOf,
    const std::pair<std::string, std::string>& reference) {
    if (sigs.empty()) return {};
    // The bias product's clock-reference pair, when the caller supplies it and the
    // satellite registers both of its signals on distinct carrier frequencies.  The
    // product defines the clock and the broadcast group delay on that pair, so it is
    // the pair whose ionosphere-free combination carries a physical datum.
    if (!reference.first.empty() && !reference.second.empty()) {
        const auto has = [&](const std::string& s) {
            return std::find(sigs.begin(), sigs.end(), s) != sigs.end();
        };
        const double fa = freqOf(reference.first);
        const double fb = freqOf(reference.second);
        if (has(reference.first) && has(reference.second) && std::isfinite(fa) && fa > 0.0
            && std::isfinite(fb) && fb > 0.0 && std::abs(fa - fb) > 1.0)
            return reference;
    }
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
    const System sys = systemOfSat(sat);
    const std::optional<SignalId> s1 = SignalId::fromObsCode(sys, sig1);
    const std::optional<SignalId> s2 = SignalId::fromObsCode(sys, sig2);
    if (!s1 || !s2) return false;
    const char b1 = s1->digit;   // RINEX band digits
    const char b2 = s2->digit;
    // Galileo: the latched value is BGD(E1, E5a), which describes (E1, E5a) only.
    // A Galileo (E1, E5b) pair (a triple-frequency posture) would need the second
    // group delay, BGD(E1, E5b), which the setup does not latch: target zero there.
    if (sys == System::Galileo) return b1 == '1' && b2 == '5';
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

bool NetworkSbasis::SatHealth::unhealthyAt(const std::string& sat, double t_gps_s) const {
    const auto it = sats.find(sat);
    if (it == sats.end() || it->second.empty()) return false;
    const std::vector<SatHealthStep>& s = it->second;
    const auto next = std::upper_bound(
        s.begin(), s.end(), t_gps_s,
        [](double t, const SatHealthStep& x) { return t < x.t_gps_s; });
    return next == s.begin() ? s.front().unhealthy : std::prev(next)->unhealthy;
}

int NetworkSbasis::galileoHealthMask(const std::vector<std::string>& signals) {
    int mask = 0;
    for (const std::string& s : signals) {
        const std::optional<SignalId> sig = SignalId::fromObsCode(System::Galileo, s);
        if (!sig) continue;
        switch (sig->band()) {
        case FrequencyBand::GalE1:  mask |= 0x007;         break;   // E1-B: data validity bit 0, health bits 1-2
        case FrequencyBand::GalE5a: mask |= 0x038;         break;   // E5a: bits 3 to 5
        case FrequencyBand::GalE5b: mask |= 0x1C0;         break;   // E5b: bits 6 to 8
        case FrequencyBand::GalE5:  mask |= 0x038 | 0x1C0; break;   // E5 AltBOC: both E5 fields
        default:                                           break;   // E6 has no bits in the word
        }
    }
    return mask;
}

NetworkSbasis::SatHealth NetworkSbasis::satHealthSeries(const std::vector<BroadcastEphemeris>& nav,
                                                        int galileo_mask) {
    SatHealth out;
    // Per satellite and message source: (the time a record takes force, unhealthy).
    // Galileo F/NAV (data-source bit 1) carries the E5a health, I/NAV the E1-B and E5b
    // health, so each source keeps its own record in force.
    std::map<std::string, std::map<int, std::vector<std::pair<double, bool>>>> groups;
    for (const auto& e : nav) {
        if (e.satellite.empty() || e.system == System::GLONASS) continue;
        int source = 0;
        int word   = e.health;
        if (e.system == System::Galileo) {
            source = (e.data_source >= 0 && (e.data_source & 2)) ? 2 : 1;
            word  &= galileo_mask;
        }
        const bool bad = word != 0;
        ++out.records;
        if (bad) ++out.records_unhealthy;
        groups[e.satellite][source].emplace_back(brdcTocGpsSeconds(e), bad);
    }
    for (auto& [sat, by_source] : groups) {
        std::vector<double> times;
        for (auto& [source, recs] : by_source) {
            (void)source;
            // Stable: records sharing a time keep file order, and the last one decides.
            std::stable_sort(recs.begin(), recs.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& r : recs) times.push_back(r.first);
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        // Before the satellite's first record every group reads its earliest record, the
        // same status as at that first time, so the first step also covers the time before.
        const auto unhealthyAt = [&by_source](double t) {
            for (const auto& [source, recs] : by_source) {
                (void)source;
                const auto next = std::upper_bound(
                    recs.begin(), recs.end(), t,
                    [](double v, const std::pair<double, bool>& r) { return v < r.first; });
                if (next == recs.begin() ? recs.front().second : std::prev(next)->second)
                    return true;
            }
            return false;
        };
        std::vector<SatHealthStep> steps;
        for (double t : times) {
            const bool bad = unhealthyAt(t);
            if (steps.empty() || steps.back().unhealthy != bad) steps.push_back({t, bad});
        }
        out.sats.emplace(sat, std::move(steps));
    }
    return out;
}

NetworkSbasis::PivotPick NetworkSbasis::electClockPivot(const std::vector<std::string>& sats,
                                                        const std::string& declared,
                                                        const std::string& incumbent,
                                                        const std::set<std::string>* ineligible) {
    const auto present = [&sats](const std::string& s) {
        return !s.empty() && std::find(sats.begin(), sats.end(), s) != sats.end();
    };
    PivotPick out;
    if (present(declared)) {   // an explicit choice is kept, healthy or not
        out.sat = declared;
        return out;
    }
    const std::string& base = present(incumbent) ? incumbent : sats.front();
    if (!ineligible || !ineligible->count(base)) {
        out.sat = base;
        return out;
    }
    // The incumbent or the lexical first is unhealthy; the first healthy one.
    for (const std::string& s : sats) {
        if (!ineligible->count(s)) {
            out.sat            = s;
            out.health_skipped = true;
            return out;
        }
    }
    out.sat             = sats.front();
    out.health_fallback = true;
    return out;
}

NetworkSbasis::ChannelFamily NetworkSbasis::channelFamilyOf(
    const std::string& constellation, const std::string& signal) {
    // Raw RINEX-3 code observable, "C<band><attribute>".  The attribute is not
    // canonicalized on input (MeasurementModel keys the sat-bias state on the raw
    // 3-character code), so this reads the receiver's own declaration.
    //
    // The §7 allow-list is enforced here rather than at the call site: only
    // constellations whose family split has been analysed (Galileo, GPS, BeiDou) can
    // report a secondary family.  Any other constellation returns Primary for every
    // signal, so no family row can fire on it.
    const System sys = systemFromName(constellation);
    if (sys != System::Galileo && sys != System::GPS && sys != System::BeiDou)
        return ChannelFamily::Primary;
    const std::optional<SignalId> sig = SignalId::fromObsCode(sys, signal);
    if (!sig) return ChannelFamily::Primary;
    const char attr = sig->attribute;
    // 'X' is the combined I+Q correlator output, a physically distinct hardware path
    // with its own bias.  Every other attribute (Galileo A/B/C/I/Q/Z, GPS
    // C/W/P/Y/S/L/I/Q/M, BeiDou I/Q/D/P/A/Z) is a discrete component on the
    // receiver's component chain and belongs to one family.  For this reason the
    // family key cannot be the attribute character itself: the Galileo primary family
    // is C1C/C5Q/C7Q, attributes 'C', 'Q', 'Q'.
    return attr == 'X' ? ChannelFamily::Secondary : ChannelFamily::Primary;
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
    const std::map<std::string, std::string>* sticky_prev_pivot,
    const std::set<std::string>* pivot_ineligible) const {
    Plan plan;

    // Group the registered clock satellites by constellation in a stable (lexical)
    // order so the fallback auto-pick is deterministic.  The lexical auto-pick is
    // made per epoch and is not sticky: a lexically lower satellite rising flips the
    // pivot, which is a datum re-reference.  Stickiness is opt-in via
    // sticky_prev_pivot below (network.sticky_pivot, RTA-1).
    std::map<std::string, std::vector<std::string>> by_const;
    for (const auto& sat : registered_clock_sats) {
        if (sat.empty()) continue;
        by_const[constellationKey(systemOfSat(sat))].push_back(sat);
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
    // With pivot_ineligible, rules 2 and 3 pass over a satellite the
    // broadcast flags unhealthy (electClockPivot).
    for (const auto& [c, sats] : by_const) {
        std::string declared, incumbent;
        const auto it = decl.clock_pivot_sat.find(c);
        if (it != decl.clock_pivot_sat.end()) declared = it->second;
        if (sticky_prev_pivot) {
            // Sticky mode: keep the incumbent while it is registered.  When it is
            // absent, fall back to the lexical pick; the driver's emit-baseline map
            // then advances to the new pivot at the next emit, so the new pivot is
            // sticky in turn and the old one's return causes no flip-back (RTA-1).
            const auto pit = sticky_prev_pivot->find(c);
            if (pit != sticky_prev_pivot->end()) incumbent = pit->second;
        }
        const PivotPick pick = electClockPivot(sats, declared, incumbent, pivot_ineligible);
        if (pick.health_skipped)  ++plan.pivot_health_skips;
        if (pick.health_fallback) ++plan.pivot_health_fallbacks;
        const std::string& pivot = pick.sat;
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
        if (constellationKey(systemOfSat(sat)) == constellation) sats.push_back(sat);
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
    // decision the gate does not make (the shared-tolerance rule).
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
    // orphaned exact gauge.  Each skipped extra extends the cluster
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
    // generic withhold.  Observational only: nothing below reads it.
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


namespace {

/// A registered code channel's identity, kept beside its channel key so the
/// family-difference arm never re-parses "<constellation>|<signal>".
struct ChannelIdent {
    SignalId    id;     ///< Constellation, band digit, tracking attribute.
    std::string code;   ///< The RINEX code ("C1X"), the key of the previous-value map.
};

/// One satellite carrying both channels of a family-difference pair.
struct FamilyCommonSat {
    std::string  sat;     ///< RINEX satellite id.
    Eigen::Index x_col;   ///< Its secondary-family (X) code-bias column.
    Eigen::Index p_col;   ///< Its primary-family partner's code-bias column.
};

/// The partner of a secondary-family code channel for the family-difference row (knob
/// network.code_datum_family_difference).
struct FamilyPartner {
    std::string key;                       ///< The partner's channel key ("E|C1C"); empty when no same-band primary channel is registered.
    std::string signal;                    ///< The partner's code ("C1C"); empty as @ref key.
    std::vector<FamilyCommonSat> common;   ///< The satellites carrying both channels, in the secondary channel's order.
};

/**
 * @brief Find the primary-family partner of a secondary-family code channel.
 *
 * The partner is a registered channel of the same constellation and RINEX band whose
 * signal is in the primary family (@ref NetworkSbasis::channelFamilyOf).  Of several, the
 * one sharing the most satellites with the channel this epoch is chosen, because the
 * row's information is the common satellites; a tie goes to the lexically first signal
 * (@p channels is sorted by key, and only a strictly larger overlap replaces the
 * incumbent), so the choice is a pure function of this epoch's register.
 *
 * @param x        The secondary-family channel.
 * @param entries  Its (satellite, column) pairs this epoch.
 * @param channels Every registered channel, by channel key.
 * @param idents   Every registered channel's identity, by channel key.
 * @return The partner; an empty @c key when no same-band primary channel is registered.
 */
FamilyPartner familyPartnerOf(
    const ChannelIdent& x,
    const std::vector<std::pair<std::string, Eigen::Index>>& entries,
    const std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>>& channels,
    const std::map<std::string, ChannelIdent>& idents) {
    FamilyPartner out;
    const std::string cst = constellationKey(x.id.sys);
    for (const auto& [pkey, pentries] : channels) {
        const auto pit = idents.find(pkey);
        if (pit == idents.end()) continue;
        const ChannelIdent& p = pit->second;
        if (p.id.sys != x.id.sys || p.id.digit != x.id.digit) continue;   // another constellation or band
        if (NetworkSbasis::channelFamilyOf(cst, p.code) != NetworkSbasis::ChannelFamily::Primary)
            continue;
        const std::map<std::string, Eigen::Index> p_col_of(pentries.begin(), pentries.end());
        std::vector<FamilyCommonSat> common;
        for (const auto& [sat, x_col] : entries) {
            const auto it = p_col_of.find(sat);
            if (it != p_col_of.end()) common.push_back({sat, x_col, it->second});
        }
        if (out.key.empty() || common.size() > out.common.size()) {
            out.key    = pkey;
            out.signal = p.code;
            out.common = std::move(common);
        }
    }
    return out;
}

/// The previous kept-epoch solved values of a family pair's common satellites:
/// what the family row's time link and the first-sighting prior both read.
struct FamilyPrev {
    std::vector<std::string>      common_sats;   ///< The common satellites, in the secondary channel's order.
    std::map<std::string, double> prev_x;        ///< Satellite → previous solved bias on the secondary channel.
    std::map<std::string, double> prev_p;        ///< Satellite → previous solved bias on the partner channel.
};

/**
 * @brief Collect the previous solved values of a family pair's common satellites.
 *
 * @param fp             The partner, with its common satellites.
 * @param xkey           The secondary channel's key.
 * @param code_bias_prev The previous kept epoch's solved code biases, by channel key.
 * @return The common satellites and their previous values on both columns (a satellite
 *         without one is absent from that map).
 */
FamilyPrev familyPrevOf(const FamilyPartner& fp, const std::string& xkey,
                        const std::map<std::string, std::map<std::string, double>>& code_bias_prev) {
    const auto prevOf = [&](const std::string& key) {
        const auto it = code_bias_prev.find(key);
        return it == code_bias_prev.end() ? nullptr : &it->second;
    };
    const std::map<std::string, double>* xprev = prevOf(xkey);
    const std::map<std::string, double>* pprev = prevOf(fp.key);
    FamilyPrev out;
    for (const FamilyCommonSat& c : fp.common) {
        out.common_sats.push_back(c.sat);
        if (xprev) if (const auto v = xprev->find(c.sat); v != xprev->end()) out.prev_x[c.sat] = v->second;
        if (pprev) if (const auto v = pprev->find(c.sat); v != pprev->end()) out.prev_p[c.sat] = v->second;
    }
    return out;
}

/// The decision for one secondary-family channel: which row realizes its level,
/// and the family-difference row's span and target when it is that row.
struct FamilyRowPlan {
    NetworkSbasis::FamilyRowRealization realization;   ///< The outcome and the partner.
    std::vector<FamilyCommonSat>        span;          ///< The satellites the family row spans; empty unless @c realization.familyRow().
    double                              rhs{0.0};      ///< The family row's target.
    FamilyPartner                       partner;       ///< The partner and the common satellites (step 2's first-sighting prior reads them whatever the outcome); an empty key for NoPartner.
    FamilyPrev                          prev;          ///< The common satellites' previous values; empty for NoPartner and NoCommon.
};

/**
 * @brief Decide the level row of one secondary-family code channel.
 *
 * In order: no same-band primary channel registered (NoPartner), no satellite carrying
 * both (NoCommon), the family row's span reaching the channel's level only through
 * columns another row already pins (Dependent), the family row without a veteran while
 * the sum-to-zero time link has one (Bridged), else the family-difference row on its
 * veterans (FamilyVeteran) or re-seeded at 0 (FamilyReseed).
 *
 * Dependent is a rank guard.  The family row adds the channel's level to the datum only
 * through a spanned satellite whose secondary column no other row fixes: if every spanned
 * satellite's secondary column is pinned by a geometry-free row (pair, extra or carried
 * extra) or held by an extra-band hold, the row is a combination of those rows, while the
 * channel's sum-to-zero row, which survived the completeness test because some satellite
 * outside the span has a free column, would have pinned that satellite's level.  The
 * sum-to-zero row is then kept.
 *
 * Bridged is a continuity guard.  A family veteran needs a previous value on both
 * columns, so the common satellites are never veterans on the epoch the family row
 * (re)starts.  Re-seeding there would re-realize the inter-family offset over a new set
 * and mark every satellite of the channel, while the channel's own sum-to-zero time link
 * carries its level.  The sum-to-zero row is kept for that epoch, and the family row
 * continues from it at the next one.  The family row re-seeds only when the sum-to-zero
 * row would re-seed too (the run's first epoch, or a channel with no veteran at all).
 *
 * @param x               The secondary-family channel.
 * @param xkey            Its channel key.
 * @param entries         Its (satellite, column) pairs this epoch.
 * @param channels        Every registered channel, by channel key.
 * @param idents          Every registered channel's identity, by channel key.
 * @param code_bias_prev  The previous kept epoch's solved code biases, by channel key.
 * @param x_pinned        Satellites whose secondary column a geometry-free row pins this
 *                        epoch (nullptr: none).
 * @param extra_band_held This epoch's extra-band holds, "sat|signal".
 * @param s2z_veteran     Whether the channel's sum-to-zero time link has a veteran.
 * @return The decision.
 */
FamilyRowPlan planFamilyRow(
    const ChannelIdent& x, const std::string& xkey,
    const std::vector<std::pair<std::string, Eigen::Index>>& entries,
    const std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>>& channels,
    const std::map<std::string, ChannelIdent>& idents,
    const std::map<std::string, std::map<std::string, double>>& code_bias_prev,
    const std::set<std::string>* x_pinned,
    const std::set<std::string>& extra_band_held,
    bool s2z_veteran) {
    using Outcome = NetworkSbasis::FamilyRowOutcome;
    FamilyRowPlan plan;
    NetworkSbasis::FamilyRowRealization& r = plan.realization;
    r.channel = x.id;
    plan.partner = familyPartnerOf(x, entries, channels, idents);
    const FamilyPartner& fp = plan.partner;
    r.partner = fp.signal;
    if (fp.key.empty())     { r.outcome = Outcome::NoPartner; return plan; }
    if (fp.common.empty())  { r.outcome = Outcome::NoCommon;  return plan; }

    // The previous solved values of the common satellites on both columns.
    plan.prev = familyPrevOf(fp, xkey, code_bias_prev);
    std::map<std::string, const FamilyCommonSat*> by_sat;
    for (const FamilyCommonSat& c : fp.common) by_sat[c.sat] = &c;
    const NetworkSbasis::CodeFamilyDiffTimeLink tl =
        NetworkSbasis::codeFamilyDiffTimeLink(plan.prev.common_sats, plan.prev.prev_x,
                                              plan.prev.prev_p);

    bool reaches_level = false;
    for (const std::string& sat : tl.jac_sats) {
        const bool pinned = x_pinned && x_pinned->count(sat) != 0;
        const bool held   = extra_band_held.count(sat + "|" + x.code) != 0;
        if (!pinned && !held) { reaches_level = true; break; }
    }
    if (!reaches_level)            { r.outcome = Outcome::Dependent; return plan; }
    if (tl.reseed && s2z_veteran)  { r.outcome = Outcome::Bridged;   return plan; }

    r.outcome = tl.reseed ? Outcome::FamilyReseed : Outcome::FamilyVeteran;
    for (const std::string& sat : tl.jac_sats) plan.span.push_back(*by_sat.at(sat));
    plan.rhs = tl.rhs;
    return plan;
}

/**
 * @brief The pin pair a satellite can hold under network.code_datum_pair_continuity,
 *        or nothing when the hold is broken.
 *
 * The held pair is the satellite's pin pair at the last kept epoch.  It is a candidate
 * while both of its signals are registered this epoch and still form an ionosphere-free
 * pair (two distinct finite positive frequencies, the test
 * @ref NetworkSbasis::selectIfPinPair applies to its second signal).  A satellite with no
 * pair at the last kept epoch (a first sighting or a return) has none.  Whether the hold
 * applies is decided by the caller (the other conditions on
 * @ref NetworkSbasis::PinPairHold).
 *
 * @param sat                   RINEX satellite id.
 * @param sigs                  The satellite's registered code signals this epoch.
 * @param code_bias_ifpin_pair_ Satellite → its pin pair at the last kept epoch.
 * @param freqOf                Carrier-frequency oracle (satellite, signal) → Hz.
 * @return The held pair; nothing when the satellite has none or a signal of it left.
 */
std::optional<std::pair<std::string, std::string>> heldIfPinPair(
    const std::string& sat, const std::vector<std::string>& sigs,
    const std::map<std::string, std::pair<std::string, std::string>>& code_bias_ifpin_pair_,
    const std::function<double(const std::string&, const std::string&)>& freqOf) {
    const auto pit = code_bias_ifpin_pair_.find(sat);
    if (pit == code_bias_ifpin_pair_.end()) return std::nullopt;
    const auto registered = [&](const std::string& s) {
        return !s.empty() && std::find(sigs.begin(), sigs.end(), s) != sigs.end();
    };
    const auto& [h1, h2] = pit->second;
    if (!registered(h1) || !registered(h2)) return std::nullopt;
    const double f1 = freqOf(sat, h1);
    const double f2 = freqOf(sat, h2);
    if (!std::isfinite(f1) || !std::isfinite(f2) || !(f1 > 0.0) || !(f2 > 0.0)
        || std::abs(f1 - f2) <= 1.0)
        return std::nullopt;
    return pit->second;
}

/**
 * @brief Whether the broadcast group delay refuses a satellite's pin pair hold (
 *        the last condition on @ref NetworkSbasis::PinPairHold).
 *
 * Under @c network.sat_gf_datum broadcast_tgd a signal's first-sighting geometry-free row
 * anchors at the target of the pair pinned that epoch (@ref
 * NetworkSbasis::satGfBroadcastTarget).  An entering signal under a held pair the group
 * delay does not describe (Galileo (C1C, C7Q)) would anchor at that pair's zero, and the
 * time link would carry the zero onto the selected pair ((C1C, C5Q), which the group delay
 * does describe) when the hold is released, for the life of the satellite.  The hold is
 * refused in that case, so the selected pair is pinned at once and anchors at the group
 * delay, as without the arm.
 *
 * Whether a pair is described is @ref NetworkSbasis::broadcastGroupDelayDescribesPair, the
 * rule @ref NetworkSbasis::satGfBroadcastTarget applies first.  A described pair whose
 * satellite broadcasts exactly zero, or has no navigation record, also anchors at zero; the
 * hold is still refused there, which costs a re-seed where it changes no value but keeps
 * the rule a property of the pair alone.
 *
 * @param sat          RINEX satellite id.
 * @param held         The held pair.
 * @param selected     The pair @ref NetworkSbasis::selectIfPinPair names.
 * @param sat_gf_datum network.sat_gf_datum.
 * @return true under broadcast_tgd when the group delay describes @p selected and not
 *         @p held; false otherwise (always false under any other geometry-free datum).
 */
bool groupDelayRefusesPinHold(const std::string& sat,
                              const std::pair<std::string, std::string>& held,
                              const std::pair<std::string, std::string>& selected,
                              NetworkSatGfDatum sat_gf_datum) {
    if (sat_gf_datum != NetworkSatGfDatum::BroadcastTgd) return false;
    const auto described = [&](const std::pair<std::string, std::string>& p) {
        return NetworkSbasis::broadcastGroupDelayDescribesPair(sat, p.first, p.second);
    };
    return described(selected) && !described(held);
}

}  // namespace

const char* NetworkSbasis::familyRowOutcomeName(FamilyRowOutcome o) {
    switch (o) {
        case FamilyRowOutcome::FamilyVeteran: return "family_veteran";
        case FamilyRowOutcome::FamilyReseed:  return "family_reseed";
        case FamilyRowOutcome::Bridged:       return "bridged";
        case FamilyRowOutcome::NoCommon:      return "no_common";
        case FamilyRowOutcome::NoPartner:     return "no_partner";
        case FamilyRowOutcome::Dependent:     return "dependent";
    }
    return "unknown";
}

void NetworkSbasis::CodeDatumFamilyCounts::add(const FamilyRowRealization& r) {
    switch (r.outcome) {
        case FamilyRowOutcome::FamilyVeteran: ++family_veteran; break;
        case FamilyRowOutcome::FamilyReseed:  ++family_reseed;  break;
        case FamilyRowOutcome::Bridged:       ++bridged;        break;
        case FamilyRowOutcome::NoCommon:      ++no_common;      break;
        case FamilyRowOutcome::NoPartner:     ++no_partner;     break;
        case FamilyRowOutcome::Dependent:     ++dependent;      break;
    }
    reseed_sats += r.reseed_sats;
}

NetworkSbasis::CodeFamilyDiffTimeLink NetworkSbasis::codeFamilyDiffTimeLink(
    const std::vector<std::string>& common_sats,
    const std::map<std::string, double>& prev_x,
    const std::map<std::string, double>& prev_p) {
    CodeFamilyDiffTimeLink out;
    // Veterans: common satellites with a previous solved value on both columns.  The row
    // spans them with RHS the sum of their previous differences, which pins the change of
    // their mean difference; each is referenced to its own previous difference, so
    // membership changes do not move the target.
    for (const std::string& sat : common_sats) {
        const auto x = prev_x.find(sat);
        const auto p = prev_p.find(sat);
        if (x != prev_x.end() && p != prev_p.end()) {
            out.jac_sats.push_back(sat);
            out.rhs += x->second - p->second;
        }
    }
    // No veteran: every common satellite at RHS 0, the zero inter-family mean.  A real
    // re-reference of the offset, which the caller signals (reseed = true).
    if (out.jac_sats.empty()) {
        out.jac_sats = common_sats;
        out.rhs      = 0.0;
        out.reseed   = true;
    }
    return out;
}

const char* NetworkSbasis::familyBirthSideName(FamilyBirthSide s) {
    switch (s) {
        case FamilyBirthSide::Secondary: return "x_born";
        case FamilyBirthSide::Partner:   return "partner_born";
        case FamilyBirthSide::Both:      return "both_born";
    }
    return "unknown";
}

const char* NetworkSbasis::familyBirthSkipName(FamilyBirthSkip s) {
    switch (s) {
        case FamilyBirthSkip::Seeded:      return "seeded";
        case FamilyBirthSkip::Carried:     return "carried";
        case FamilyBirthSkip::NoReference: return "no_reference";
    }
    return "unknown";
}

NetworkSbasis::CodeFamilyBirthPlan NetworkSbasis::codeFamilyBirthPriors(
    const std::vector<std::string>& common_sats,
    const std::map<std::string, double>& prev_x,
    const std::map<std::string, double>& prev_p,
    const std::map<std::string, CodeBiasCarry>& carry_x,
    const std::map<std::string, CodeBiasCarry>& carry_p,
    bool family_reseed) {
    CodeFamilyBirthPlan out;
    // The target first: the mean previous difference over the veterans (a previous value
    // on both columns), the satellites the family row's time link spans.  It is a realized
    // family mean when a veteran exists, or when the family row re-seeds the offset at 0.
    double sum = 0.0;
    for (const std::string& sat : common_sats) {
        const auto x = prev_x.find(sat);
        const auto p = prev_p.find(sat);
        if (x != prev_x.end() && p != prev_p.end()) {
            sum += x->second - p->second;
            ++out.n_veterans;
        }
    }
    if (out.n_veterans > 0) out.mu_m = sum / out.n_veterans;
    out.mu_defined = out.n_veterans > 0 || family_reseed;
    // One column's standing: whether it continues (the carry folds it on, or it has a
    // previous value the carry keeps), and whether the carry alone makes it continue.
    struct Column {
        bool continues{false};
        bool loosened{false};               // a previous value the carry gives up
        std::optional<CodeBiasCarry> by;    // the carry, when it continues a column the map lacks
    };
    const auto column = [](const std::string& sat, const std::map<std::string, double>& prev,
                           const std::map<std::string, CodeBiasCarry>& carry) {
        Column c;
        const bool in_map = prev.count(sat) != 0;
        const auto it = carry.find(sat);
        const bool has_carry = it != carry.end();
        if (has_carry && it->second == CodeBiasCarry::Loosened) {
            c.loosened = in_map;
            return c;   // born: the carried information is given up
        }
        c.continues = in_map || has_carry;
        if (!in_map && has_carry) c.by = it->second;
        return c;
    };
    const auto sideOf = [](bool x, bool p) {
        return x && p ? FamilyBirthSide::Both
             : x      ? FamilyBirthSide::Secondary
                      : FamilyBirthSide::Partner;
    };
    // Then the births: a common satellite with a column that does not continue.  A
    // satellite whose columns both continue is carried and gets no row; it is recorded
    // when the carry alone continues a column the previous-value map lacks.
    for (const std::string& sat : common_sats) {
        const Column x = column(sat, prev_x, carry_x);
        const Column p = column(sat, prev_p, carry_p);
        if (!x.continues || !p.continues) {
            out.births.push_back({sat, sideOf(!x.continues, !p.continues),
                                  (!x.continues && x.loosened) || (!p.continues && p.loosened)});
            continue;
        }
        if (x.by || p.by) {
            // A READMIT seed is named before a plain fold: it is the return the map misses.
            const bool seeded = (x.by && *x.by == CodeBiasCarry::Seeded)
                             || (p.by && *p.by == CodeBiasCarry::Seeded);
            out.carried.emplace_back(sat,
                                     seeded ? FamilyBirthSkip::Seeded : FamilyBirthSkip::Carried,
                                     sideOf(x.by.has_value(), p.by.has_value()));
        }
    }
    return out;
}

void NetworkSbasis::CodeFamilyBirthCounts::add(const FamilyBirthPrior& b) {
    ++rows;
    switch (b.born) {
        case FamilyBirthSide::Secondary: ++x_born;       break;
        case FamilyBirthSide::Partner:   ++partner_born; break;
        case FamilyBirthSide::Both:      ++both_born;    break;
    }
    if (b.n_veterans == 0) ++no_veteran;
    if (b.seen_before) ++seen_before;
    if (b.loosened) ++loosened;
    max_abs_mu_m = std::max(max_abs_mu_m, std::abs(b.mu_m));
    sats.insert(b.sat);
}

void NetworkSbasis::CodeFamilyBirthCounts::add(const FamilyBirthSkipRecord& s) {
    switch (s.why) {
        case FamilyBirthSkip::Seeded:      ++seeded;       break;
        case FamilyBirthSkip::Carried:     ++carried;      break;
        case FamilyBirthSkip::NoReference: ++no_reference; break;
    }
}

const char* NetworkSbasis::pinPairContinuityBlocker(NetworkSatGfDatum sat_gf_datum,
                                                    bool sat_gf_datum_time_linked) {
    if (sat_gf_datum != NetworkSatGfDatum::NoneMode && !sat_gf_datum_time_linked)
        return "network.sat_gf_datum without network.sat_gf_datum_time_linked (its "
               "geometry-free rows re-assert their absolute target on a new pin pair)";
    return nullptr;
}

NetworkSbasis::CodeBiasDatumRows NetworkSbasis::buildCodeBiasDatumRows(
    const NetworkEstimator& est,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::function<double(const std::string&, const std::string&)>& freqOf,
    bool cb_time_linked, bool emit_clock_disc,
    NetworkSatGfDatum sat_gf_datum,
    const std::map<std::string, double>& sat_gf_target_m,
    bool sat_gf_datum_time_linked,
    bool sat_isc_state, const std::set<std::string>& isc_bridged,
    bool sat_gf_extra_band_free,
    const std::set<std::string>& extra_band_determined,
    const std::map<std::string, std::map<std::string, double>>& code_bias_prev_,
    const std::map<std::string, std::pair<std::string, std::string>>& code_bias_ifpin_pair_,
    const std::set<std::string>& code_bias_sat_seen_,
    bool code_bias_datum_resumed_,
    std::map<std::string, std::pair<std::string, std::string>>& this_epoch_if_pin_sel,
    std::map<std::string, std::pair<std::string, std::string>>& this_epoch_if_pin_real,
    std::map<std::string, std::pair<std::string, std::string>>& this_epoch_cb_ifpin,
    std::set<std::string>& this_epoch_cb_reseed,
    const CodeDatumFamilyArms& family_arms,
    const CodeBiasCarryMap& code_bias_carry) {
    CodeBiasDatumRows out;
    const Eigen::Index nshared = est.numShared();
    // Both arms refine the time link and are read only under it.  Pair
    // continuity (the pin pair hold and the continued change) is read only where it keeps
    // the datum: with no row that re-asserts an absolute target on a new pin pair
    // (pinPairContinuityBlocker).
    const bool pair_continuity =
        cb_time_linked && family_arms.pair_continuity
        && NetworkSbasis::pinPairContinuityBlocker(sat_gf_datum,
                                                   sat_gf_datum_time_linked) == nullptr;
    const bool family_difference = cb_time_linked && family_arms.family_difference;
    // The first-sighting family prior refines the family difference and
    // is read only with it (the loader refuses a negative or non-finite sigma).
    const double birth_sigma = family_arms.birth_prior_sigma_m;
    const bool   birth_prior = family_difference && std::isfinite(birth_sigma) && birth_sigma > 0.0;
    std::vector<Eigen::VectorXd> birth_rows;   // its soft rows, folded apart from `rows`
    std::vector<double>          birth_zs;

    std::map<std::string, std::vector<std::string>> sat_signals;  // sat -> code signals
    std::vector<std::string> sat_order;
    for (const auto& kv : registered_code_keys) {
        auto& v = sat_signals[kv.first];
        if (v.empty()) sat_order.push_back(kv.first);
        v.push_back(kv.second);
    }

    std::vector<Eigen::VectorXd> rows;                 // length-nshared constraint Jacobians
    std::vector<double>          zs;                   // per-row RHS target (all 0.0 unless code_datum_time_linked)

    // network.code_datum_time_linked: fetch a satellite's previous
    // kept-epoch solved code bias for a signal, the "veteran" reference.  With the
    // knob off, or at a first appearance, the lookup fails and the zero-target legacy
    // path runs (byte-identical).
    auto prevCb = [&](const std::string& sat, const std::string& sig,
                      double& out) -> bool {
        const auto it = code_bias_prev_.find(constellationKey(systemOfSat(sat)) + "|" + sig);
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
    // Each channel's typed identity beside its key, kept only when the
    // family-difference arm reads it.
    std::map<std::string, ChannelIdent> chan_ident;
    for (const auto& kv : registered_code_keys) {
        Eigen::Index col;
        try { col = est.codeBiasColumn(kv.first, kv.second); } catch (...) { continue; }
        const System sys = systemOfSat(kv.first);
        const std::string skey = constellationKey(sys) + "|" + kv.second;
        st0_chan[skey].emplace_back(kv.first, col);
        if (family_difference && !chan_ident.count(skey))
            if (const std::optional<SignalId> id = SignalId::fromObsCode(sys, kv.second))
                chan_ident.emplace(skey, ChannelIdent{*id, kv.second});
    }

    // (constellation|signal) -> the satellites whose column the geometry-free datum
    // pinned this epoch.  Used below to suppress the sum-to-zero row of any channel
    // the datum already fixes completely; see the suppression block (RF-0d).
    std::map<std::string, std::set<std::string>> sat_gf_pinned;

    // (1) Per-satellite ionosphere-free pin (clock-common gauge).
    for (const std::string& sat : sat_order) {
        std::vector<std::string>& sigs = sat_signals[sat];
        // IF pin: the two lowest distinct-frequency code signals, a1 + a2 = 1.
        // The selection is deterministic in the signal set (sorted by band digit,
        // then full signal name), so same-band channels (C1C vs C1W) do not swap
        // with the insertion order of registered_code_keys and re-datum the emitted
        // clock; see NetworkSbasis::selectIfPinPair.
        // The bias product's clock-reference pair of this satellite's constellation, only
        // when network.n3c_bia_pin_pair supplies it.
        std::pair<std::string, std::string> pin_ref;
        if (const auto rit = family_arms.pin_reference.find(letterOfSat(sat));
            rit != family_arms.pin_reference.end())
            pin_ref = rit->second;
        const auto if_pin = NetworkSbasis::selectIfPinPair(
            sigs, [&](const std::string& s) { return freqOf(sat, s); }, pin_ref);
        std::string sig1 = if_pin.first;
        std::string sig2 = if_pin.second;
        double      f1   = sig1.empty() ? 0.0 : freqOf(sat, sig1);
        double      f2   = sig2.empty() ? 0.0 : freqOf(sat, sig2);
        // Set when the group-delay test refused this satellite's pin pair hold,
        // so that the re-seeded change below can say why it was not held.
        bool hold_refused_gd = false;
        if (pair_continuity) {
            // network.code_datum_pair_continuity, step 1 (hold).  The selection
            // above is a function of this epoch's registered set, so a lower-ordered
            // signal reaching two stations (Galileo C5Q beside a pinned (C1C, C5X))
            // re-selects the pin, and the entering signal has no previous solved value to
            // continue from.  Instead the satellite keeps the last kept epoch's pair for
            // as long as the selected pair cannot be continued, and the time link below
            // carries that pair's own previous combination as for an unchanged pair (no
            // pair change, no re-seed of the pin, no event).  The entering signal is
            // registered meanwhile, so the harvest of this kept epoch gives it a previous
            // value, and at the next kept epoch the hold is released: the pin moves to the
            // selection through step 2 (the continuity branch of the time link below)
            // without a clock step.  A hold therefore lasts one kept epoch per entering
            // signal, and the pin does not depend on the satellite's history beyond it
            // (a lasting per-satellite split would drop the constellation's cbpin=
            // declaration).  No hold where it buys nothing (the held pair has no previous
            // values, so its pin would re-seed as the selection's does) or where it would
            // anchor the entering signal in the wrong geometry-free frame (the group-delay
            // test; see PinPairHold).  Everything keyed on sig1/sig2 from here on (the
            // geometry-free pair and extra rows, the extra-band roster key, the disc and
            // declaration records, this_epoch_cb_ifpin) follows the pinned pair.
            const auto held = heldIfPinPair(sat, sigs, code_bias_ifpin_pair_, freqOf);
            if (held && *held != if_pin) {
                double unused = 0.0;
                const auto hasPrev = [&](const std::string& s) { return prevCb(sat, s, unused); };
                const bool held_carries       = hasPrev(held->first) && hasPrev(held->second);
                const bool selected_continues = hasPrev(if_pin.first) && hasPrev(if_pin.second);
                if (held_carries && !selected_continues) {
                    if (groupDelayRefusesPinHold(sat, *held, if_pin, sat_gf_datum)) {
                        hold_refused_gd = true;   // the selection stands and re-seeds
                    } else {
                        out.if_pin_pair_holds[sat] = PinPairHold{if_pin, *held};
                        sig1 = held->first;
                        sig2 = held->second;
                        f1   = freqOf(sat, sig1);
                        f2   = freqOf(sat, sig2);
                    }
                }
            }
        }
        // The pin is re-derived from each epoch's coverage.  With the deterministic
        // selection above, the pair is a pure function of the registered signal set, so
        // a change between emitted products means a channel entered or left the
        // ≥2-station set: the defect-2 clock↔code re-reference, occurring on every
        // coverage change without an event.  This epoch's selection is recorded in the
        // main-pass scratch, and the compare and disc bump are deferred to the emit site
        // (against the last emitted pair), so a transient pin move on a withheld epoch
        // cannot produce a bump; prev_pivot and emitted_anchor follow the same emit-only
        // rule.  Guarded by emit_clock_disc and a real IF pair (f1, f2 > 0), so it is
        // byte-identical when the disc channel is off or the satellite is
        // single-frequency.  Under the family pair hold the record is the pinned (held)
        // pair, so a held satellite matches its last emitted pair and is not compared as
        // a re-selection; only a broken hold moves it.
        if (emit_clock_disc && f1 > 0.0 && f2 > 0.0)
            this_epoch_if_pin_sel[sat] = { sig1, sig2 };
        // The S-contract §1.5 declaration record is kept separate from the disc
        // scratch above and is deliberately not guarded by that knob.  sig1/sig2 are
        // post-hold here, so this is the pair the datum realizes on both the held and the
        // re-selected path.  The scratch above cannot serve as the declaration: it
        // is gated on the QC-channel knob emit_clock_disc (default false), and a
        // product whose run leaves it off would declare no cbpin= token.  The record is
        // read only by the NetworkSolPrep fill, so the solve and every emitted numeric
        // value are unchanged; only the S line changes.  It is written below, after the
        // row is known to have folded (see the record site).
        Eigen::VectorXd row = Eigen::VectorXd::Zero(nshared);
        bool row_ok = false;
        double zval = 0.0;   // legacy zero target unless time-linked
        if (f1 > 0.0 && f2 > 0.0) {
            const double den = f1 * f1 - f2 * f2;
            const double a1 =  f1 * f1 / den;     // ionosphere-free (clock-preserving) weights
            const double a2 = -f2 * f2 / den;
            try {
                row(est.codeBiasColumn(sat, sig1)) = a1;
                row(est.codeBiasColumn(sat, sig2)) = a2;
                row_ok = true;
            } catch (...) { row.setZero(); row_ok = false; }
            // Time-linking (the cb_time_linked block below) pins the change
            // of the clock↔code split: the RHS is the satellite's previous solved
            // ionosphere-free combination a1·b̂_prev(σ1) + a2·b̂_prev(σ2).  That is
            // continuity only when the pair is unchanged from the last kept epoch and
            // both signals have a previous solved value.  A pair change (the band-1
            // C1C↔C1W flip, or a band entering or leaving) re-references the split by
            // the satellite's inter-channel bias even when both new signals have prior
            // values, so the row re-seeds to 0 and the satellite is marked for a disc
            // bump; otherwise the emitted clock would step without a signal.  (Under
            // network.code_datum_pair_continuity the re-reference is avoided
            // where it can be: with previous values on both new signals the row targets
            // their previous IF combination, which the old pin realized, so the clock does
            // not step, provided every other row on the satellite's columns carries its
            // previous value too (see pinPairContinuityBlocker); and a change the hold
            // above deferred, because a new signal had no previous value yet, reaches here
            // at the next kept epoch with one.)
            // The bump (this_epoch_cb_reseed) applies to any re-seed that is not the
            // satellite's first appearance (seen before, or the run resumed), so the
            // value step coincides with a signalled discontinuity.  This epoch's pair
            // is recorded for the next epoch's comparison (persisted at the harvest,
            // kept epochs only).
            if (row_ok && cb_time_linked) {
                double v1 = 0.0, v2 = 0.0;
                const bool h1 = prevCb(sat, sig1, v1);
                const bool h2 = prevCb(sat, sig2, v2);
                const auto pit = code_bias_ifpin_pair_.find(sat);
                const bool pair_same =
                    pit != code_bias_ifpin_pair_.end() &&
                    pit->second.first == sig1 && pit->second.second == sig2;
                // A pair change is a satellite pinned at the last kept epoch on
                // a different pair (absent at the last kept epoch is a return, not a
                // change).  Recorded only while the arm is effective.
                const bool pair_changed =
                    pair_continuity && pit != code_bias_ifpin_pair_.end() && !pair_same;
                if (h1 && h2 && pair_same) {
                    zval = a1 * v1 + a2 * v2;
                } else if (pair_changed && h1 && h2) {
                    // Pair continuity.  The new pair's previous solved values
                    // were realized against the old pin, so their ionosphere-free
                    // combination is the value that keeps the satellite clock where the
                    // last kept epoch put it: the pin changes pair without moving the
                    // datum, so there is no re-seed to mark.  The emit site reads the
                    // record to leave the re-selection discontinuity undeclared, and the
                    // reconciliation after the level rows below demotes the record if a
                    // channel-level re-seed marks the satellite after all.
                    zval = a1 * v1 + a2 * v2;
                    out.if_pin_pair_changes[sat] =
                        PinPairChange{pit->second, {sig1, sig2}, PinPairOutcome::Continued};
                } else {
                    if (pair_changed)
                        out.if_pin_pair_changes[sat] =
                            PinPairChange{pit->second, {sig1, sig2}, PinPairOutcome::Reseeded,
                                          hold_refused_gd};
                    if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_)
                        this_epoch_cb_reseed.insert(sat);
                }
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
            out.row_kinds.push_back(ContributionKind::CbIfPin);
            zs.push_back(zval);
            // S-contract §1.5 declaration record, placed after the push so it records
            // only a pair whose IF row folded.  Guarded on a real dual-frequency pin
            // (f1, f2 > 0), which also excludes the degenerate single-frequency
            // all-ones branch above: that branch sets row_ok but realizes no IF pair.
            // A record made before the fold (guarded on f1, f2 alone) would let a
            // satellite whose codeBiasColumn lookup threw enter the emit-site unanimity
            // tally with a pair it never realized.
            if (f1 > 0.0 && f2 > 0.0) this_epoch_if_pin_real[sat] = { sig1, sig2 };
        }
        // (1a) Hard per-satellite geometry-free datum (network.sat_gf_datum, RF-0c).
        //
        // The estimable function is Odijk 2016 Eq. (30), (d_2 - d_1)/(mu_2 - mu_1) on the
        // ionosphere-free pin's own pair.  Under dual frequency the IF and GF pins
        // together leave nothing estimable on the pinned pair (Odijk §5.1), which is the
        // correct result rather than a defect; without the row the directions stay open
        // (RF-0b measured 31 per constellation, held only by a 3 m shrinkage prior).
        //
        // Why the target may be non-zero: d^s_GF is a datum quantity, so any value is
        // legal and the estimable parameters absorb the difference (Odijk 2016 Table 8:
        // the geometry-free lump moves into the slant ionosphere,
        // iota~ = iota + d_r,GF - d^s_GF).  Zero is the conventional choice;
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
        // describes: (L1, L2) for GPS T_GD, (E1, E5a) for Galileo BGD(E1, E5a) (the rule
        // lives in satGfBroadcastTarget).  Any other pair falls back to a
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
                    // The same branches, counted per constellation (R3 iii).
                    SatGfConstCounts& gcc =
                        out.sat_gf_by_const[constellationKey(systemOfSat(sat))];
                    if (sat_gf_datum == NetworkSatGfDatum::BroadcastTgd) {
                        // The pair must be one the latched group delay describes: (band 1,
                        // band 2) for GPS (T_GD), (band 1, band 5) for Galileo (BGD(E1,
                        // E5a)).  The rule and its order of tests live in
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
                    // this pair was pinned at (R3 ii).
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
                            // A satellite seen before and absent at
                            // the last kept epoch (a return, not a first sighting and not
                            // a link cut on a present satellite) re-anchors at its absolute
                            // target.  Measure-only.
                            if (code_bias_sat_seen_.count(sat) != 0
                                && code_bias_ifpin_pair_.count(sat) == 0)
                                ++out.n_sat_gf_reanchored;
                        }
                    }
                    rows.push_back(std::move(srow));
                    out.row_kinds.push_back(ContributionKind::CbSatGfPair);
                    zs.push_back(target);
                    ++out.n_sat_gf_rows;
                    ++gcc.rows;
                    sat_gf_pinned[constellationKey(systemOfSat(sat)) + "|" + sig1].insert(sat);
                    sat_gf_pinned[constellationKey(systemOfSat(sat)) + "|" + sig2].insert(sat);

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
                    const System sat_sys = systemOfSat(sat);
                    const std::optional<SignalId> pin1 = SignalId::fromObsCode(sat_sys, sig1);
                    const std::optional<SignalId> pin2 = SignalId::fromObsCode(sat_sys, sig2);
                    for (const std::string& sk : sigs) {
                        if (sk == sig1 || sk == sig2) continue;
                        const std::optional<SignalId> id_k = SignalId::fromObsCode(sat_sys, sk);
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
                            pin2 && id_k && id_k->digit == pin2->digit;
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
                        // quantity and needs a different argument: with the slant
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
                        if (sat_gf_extra_band_free && pin1 && pin2 && id_k
                            && id_k->digit != pin1->digit && id_k->digit != pin2->digit) {
                            // The roster is keyed on the pin pair the builder realized,
                            // not on an assumed (L1, L2): a held or re-selected pair
                            // changes which two bands the determination needs.
                            const std::string dkey = sat + "|" + sk + "|"
                                                   + std::string{pin1->digit, pin2->digit};
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
                                out.row_kinds.push_back(ContributionKind::CbExtraBandHeld);
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
                                    // its (C1C, C5Q) pair sits at zero (R3 ii).
                                    // For a GPS (L1, L2) pair the two give the same number
                                    // on every branch (hit, broadcast zero, missing
                                    // record), so a GPS-only run is byte-identical.
                                    k_target = abs_target;
                                    ++out.n_sat_gf_anchored;
                                    ++gcc.anchored;
                                    // A re-anchor of a satellite seen
                                    // before; see the pair row.  Measure-only.
                                    if (code_bias_sat_seen_.count(sat) != 0
                                        && code_bias_ifpin_pair_.count(sat) == 0)
                                        ++out.n_sat_gf_reanchored;
                                }
                            }
                            rows.push_back(std::move(krow));
                            out.row_kinds.push_back(ContributionKind::CbExtraGf);
                            zs.push_back(k_target);
                            ++out.n_sat_gf_extra_rows;
                            ++gcc.extra_rows;
                            if (sat_gf_datum == NetworkSatGfDatum::BroadcastTgd) {
                                ++out.n_sat_gf_isc_zero;
                                ++gcc.isc_zero;
                            }
                            sat_gf_pinned[constellationKey(systemOfSat(sat)) + "|" + sk].insert(sat);
                        }
                    }
                }
            }
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
        // The channel's sum-to-zero time link: its veterans and RHS.  Computed
        // ahead of the family-difference decision below, which reads its veteran
        // test; the sum-to-zero row is built from it unchanged.
        std::vector<std::string>            present_sats;
        std::map<std::string, Eigen::Index> col_of;
        NetworkSbasis::CodeS2zTimeLink      s2z_tl;
        if (cb_time_linked) {
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
            s2z_tl = NetworkSbasis::codeS2zTimeLink(present_sats, prev_vals);
        }
        // --- The family-difference row (network.code_datum_family_difference) ---
        // A secondary-family Galileo channel (combined I+Q, attribute X) that keeps its row
        // here carries a level no other row fixes: with no receiver tracking both families,
        // the offset between the families is a datum quantity, and this channel's
        // sum-to-zero row is what realizes it (seeded at 0 over the satellites present at
        // its first epoch).  The arm realizes it instead as Σ (d_X − d_P) over the
        // satellites that carry both channels, the inter-family mean of zero that COD's
        // product uses, so a satellite's clock sits on the same realization of the offset
        // whichever family pins it.  The row takes the sum-to-zero row's slot, so the row
        // order is unchanged.  planFamilyRow (above this function) decides, and keeps the
        // sum-to-zero row where the family row would add no rank or would re-seed a level
        // the sum-to-zero time link carries.  Galileo only: the evidence and COD's
        // convention are Galileo's, and a GPS or BeiDou X channel keeps its row.
        bool family_row = false;
        if (family_difference) {
            const auto id = chan_ident.find(skey);
            if (id != chan_ident.end() && id->second.id.sys == System::Galileo
                && NetworkSbasis::channelFamilyOf(constellationKey(System::Galileo),
                                                  id->second.code)
                       == ChannelFamily::Secondary) {
                const auto xp = sat_gf_pinned.find(skey);
                FamilyRowPlan plan = planFamilyRow(
                    id->second, skey, entries, st0_chan, chan_ident, code_bias_prev_,
                    xp == sat_gf_pinned.end() ? nullptr : &xp->second, out.extra_band_held,
                    !s2z_tl.reseed);
                if (plan.realization.familyRow()) {
                    family_row = true;
                    for (const FamilyCommonSat& c : plan.span) {
                        srow(c.x_col) += 1.0;
                        srow(c.p_col) -= 1.0;
                    }
                    zval = plan.rhs;
                    // A re-seed of the inter-family offset is the same decoupling edge as
                    // a sum-to-zero re-seed (standing invariant #7): the channel's
                    // satellites seen before (all of them on a resumed run) are marked for
                    // the emit site's discontinuity, as the sum-to-zero re-seed marks them.
                    if (plan.realization.outcome == FamilyRowOutcome::FamilyReseed)
                        for (const auto& sat : present_sats)
                            if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_) {
                                this_epoch_cb_reseed.insert(sat);
                                ++plan.realization.reseed_sats;
                            }
                }
                // --- The first-sighting family prior ---
                // (network.code_family_birth_prior_sigma_m.)  The family row fixes the
                // inter-family offset as a constellation mean, but a column born beside an
                // uncertain satellite clock still takes up that clock's error, and the stiff
                // code-bias process noise keeps it (reading R-a).  So a common
                // satellite whose X or partner column is born this epoch gets one soft row
                // d_X - d_P = mu, whatever row realized the channel's level: the prior is on
                // the satellite's own difference, which no level row reaches.  Band 1 only:
                // it is the band the family row acts on under the goldens' datum (the RF-0d
                // extra rows already tie a band-5 X column to the pinned pair), and the band
                // of the evidence.
                //
                // Born is read from the carry (codeFamilyBirthPriors, code_bias_carry), not
                // from the previous-value map alone: the map drops a departed column, while
                // READMIT parks its carry and seeds the column from it on a return inside
                // the seed horizon, so the map would call that return born and fold a second
                // row on information the seed already holds.  A column the clock monitor's
                // reinit loosens is born although it has a previous value.  That keeps the
                // fold to once per (satellite, birth): a born column is carried from the
                // next kept epoch on.
                //
                // mu is the family row's veteran mean, or 0 when the family row re-seeds
                // the offset at 0.  A birth with neither (Bridged, or Dependent with no
                // veteran) has no realized family mean to target, because the offset this
                // epoch is what the channel's sum-to-zero frame carries, so it folds no row
                // and is recorded (FamilyBirthSkip::NoReference).  The rows are kept out of
                // `rows`: they are soft priors, not gauge rows, and go to the driver with
                // their own weight (CodeBiasDatumRows::family_birth_H).
                if (birth_prior && id->second.id.digit == '1' && !plan.prev.common_sats.empty()) {
                    std::map<std::string, CodeBiasCarry> carry_x, carry_p;
                    if (!code_bias_carry.empty())
                        for (const std::string& sat : plan.prev.common_sats) {
                            const auto cx = code_bias_carry.find({sat, id->second.code});
                            if (cx != code_bias_carry.end()) carry_x[sat] = cx->second;
                            const auto cp = code_bias_carry.find({sat, plan.partner.signal});
                            if (cp != code_bias_carry.end()) carry_p[sat] = cp->second;
                        }
                    const NetworkSbasis::CodeFamilyBirthPlan bp = NetworkSbasis::codeFamilyBirthPriors(
                        plan.prev.common_sats, plan.prev.prev_x, plan.prev.prev_p, carry_x,
                        carry_p,
                        plan.realization.outcome == FamilyRowOutcome::FamilyReseed);
                    const auto skip = [&](const std::string& sat, FamilyBirthSkip why,
                                          FamilyBirthSide side) {
                        NetworkSbasis::FamilyBirthSkipRecord s;
                        s.sat      = sat;
                        s.channel  = id->second.id;
                        s.x_signal = id->second.code;
                        s.partner  = plan.partner.signal;
                        s.why      = why;
                        s.side     = side;
                        s.outcome  = plan.realization.outcome;
                        out.family_birth_skips.push_back(std::move(s));
                    };
                    for (const auto& [sat, why, side] : bp.carried) skip(sat, why, side);
                    for (const NetworkSbasis::FamilyBirth& b : bp.births) {
                        if (!bp.mu_defined) {
                            skip(b.sat, FamilyBirthSkip::NoReference, b.side);
                            continue;
                        }
                        const auto c = std::find_if(
                            plan.partner.common.begin(), plan.partner.common.end(),
                            [&](const FamilyCommonSat& s) { return s.sat == b.sat; });
                        if (c == plan.partner.common.end()) continue;   // defensive: births come from the common set
                        Eigen::VectorXd brow = Eigen::VectorXd::Zero(nshared);
                        brow(c->x_col) = 1.0;
                        brow(c->p_col) = -1.0;
                        birth_rows.push_back(std::move(brow));
                        birth_zs.push_back(bp.mu_m);
                        NetworkSbasis::FamilyBirthPrior rec;
                        rec.sat         = b.sat;
                        rec.channel     = id->second.id;
                        rec.x_signal    = id->second.code;
                        rec.partner     = plan.partner.signal;
                        rec.born        = b.side;
                        rec.loosened    = b.loosened;
                        rec.mu_m        = bp.mu_m;
                        rec.n_veterans  = bp.n_veterans;
                        rec.sigma_m     = birth_sigma;
                        rec.seen_before = code_bias_sat_seen_.count(b.sat) != 0;
                        out.family_birth_priors.push_back(std::move(rec));
                    }
                }
                out.family_rows[skey] = std::move(plan.realization);
            }
        }
        if (!family_row) {
            if (cb_time_linked) {
                for (const auto& sat : s2z_tl.jac_sats) srow(col_of[sat]) += 1.0;
                zval = s2z_tl.rhs;
                if (s2z_tl.reseed) {
                    // A re-seed is the channel's level re-referencing, so it is an event
                    // at both edges (standing invariant #7, B5D).  The marks (the
                    // decoupling edge) drive the emit-site disc bumps; the counters let a
                    // run summary tell "the level never re-referenced" apart from "nothing
                    // was instrumented".  They are counted here rather than at the emit
                    // site because a withheld epoch does not reach that site, and its
                    // re-seed is still a fact about the datum this run realized.
                    ++out.n_s2z_reseed_rows;
                    out.reseed_channels.insert(skey);   // the receiver IFB carry of this family
                    for (const auto& sat : present_sats)
                        if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_) {
                            this_epoch_cb_reseed.insert(sat);
                            ++out.n_s2z_reseed_sats;
                        }
                }
            } else {
                for (const auto& [sat, col] : entries) srow(col) += 1.0;
            }
        }
        if (srow.cwiseAbs().sum() > 0.0) {
            rows.push_back(std::move(srow));
            // A family-difference row is tagged as the sum-to-zero row it
            // replaces, deliberately.  The "contributions as armed" table and its manifest
            // record print every kind on every run, armed or not, so a kind of its own
            // would change every unarmed run's log and manifest, which the
            // default-path byte-identity rule forbids.  Which row realized each channel
            // is reported instead by the "CB_DATUM"/"family_row" events at every switch
            // and by the run-end line (networkCodeDatumFamilyRunEnd); a kind of
            // its own is a one-line change once a table line on every run is accepted.
            out.row_kinds.push_back(ContributionKind::CbSumToZero);
            zs.push_back(zval);
        }
    }
    // this_epoch_cb_reseed now holds every satellite whose datum re-referenced this
    // epoch.  It is not merged into the persistent cb_reseed_disc_pending
    // here; the caller defers that merge to the emit site (NetworkSolPrep), which
    // every whole-epoch withhold path skips.  A transient re-seed on a withheld epoch
    // (the frame reverts by the next kept epoch) therefore never enters the pending
    // set and cannot fire a bump without an emitted value step.  This is the emit-only
    // rule, and it needs no erase; an erase would drop a bump legitimately deferred
    // for a satellite re-seeded while pruned from the emission.
    //
    // A satellite whose pin continued at a pair change but that a channel-level
    // re-seed above marked anyway had its datum moved by that re-seed, so its pair change
    // is declared as without the arm (the emit site does not suppress it).
    for (auto& [sat, change] : out.if_pin_pair_changes)
        if (change.outcome == PinPairOutcome::Continued && this_epoch_cb_reseed.count(sat))
            change.outcome = PinPairOutcome::LevelReseeded;
    // The same for a held satellite: the hold kept its pin's own combination, but a
    // channel-level re-seed (typically the entering signal's channel with no veteran)
    // marked it, so the emit site declares a discontinuity for it and the run end must
    // not count it as a clean hold.
    for (auto& [sat, hold] : out.if_pin_pair_holds)
        if (this_epoch_cb_reseed.count(sat)) hold.reseeded = true;

    if (!rows.empty()) {
        Eigen::MatrixXd H(static_cast<Eigen::Index>(rows.size()), nshared);
        for (size_t r = 0; r < rows.size(); ++r)
            H.row(static_cast<Eigen::Index>(r)) = rows[r];
        // Per-row targets: all 0 on the legacy path (byte-identical); the
        // veteran-referenced and previous-IF values under time-linking.
        Eigen::VectorXd z(static_cast<Eigen::Index>(rows.size()));
        for (size_t r = 0; r < zs.size(); ++r)
            z(static_cast<Eigen::Index>(r)) = zs[r];
        out.H = std::move(H);
        out.z = std::move(z);
    }
    // The first-sighting family prior rows, stacked apart from the gauge
    // rows with their own weight (the driver folds and captures them on their own).
    if (!birth_rows.empty()) {
        const Eigen::Index nb = static_cast<Eigen::Index>(birth_rows.size());
        out.family_birth_H.resize(nb, nshared);
        out.family_birth_z.resize(nb);
        for (Eigen::Index r = 0; r < nb; ++r) {
            out.family_birth_H.row(r) = birth_rows[static_cast<std::size_t>(r)];
            out.family_birth_z(r)     = birth_zs[static_cast<std::size_t>(r)];
        }
        out.family_birth_weight = 1.0 / (birth_sigma * birth_sigma);
    }
    out.n_if_pin_sats  = static_cast<int>(sat_order.size());
    out.n_s2z_channels = static_cast<int>(st0_chan.size());
    return out;
}

void NetworkSbasis::foldFamilyBirthRows(NetworkEstimator& est, const Eigen::MatrixXd& H,
                                        const Eigen::VectorXd& z, double weight) {
    if (H.rows() == 0) return;
    est.contributionKind(ContributionKind::CbFamilyBirth);
    est.addSharedConstraintRows(H, z, weight);
}

// The carry's fold of each code-bias column, for the first-sighting prior (see the header).
NetworkSbasis::CodeBiasCarryMap networkCodeBiasCarryMap(const NetworkRunContext& ctx) {
    using Fold = NetworkSbasis::CodeBiasCarry;
    NetworkSbasis::CodeBiasCarryMap out;
    for (const CarryPrior& cp : ctx.carry.prev_carry) {
        if (cp.kind != SharedParamKind::CodeBias) continue;
        std::optional<Fold> f;
        switch (cp.fold.disposition) {
            case CarryDisposition::Folded:   f = Fold::Carried;  break;
            case CarryDisposition::Seeded:   f = Fold::Seeded;   break;
            case CarryDisposition::Reinit:   f = Fold::Loosened; break;
            case CarryDisposition::Undecided:   // no decision pass reached it: its own record
                if (!(cp.t_park_s > 0.0)) f = cp.inflate_q ? Fold::Loosened : Fold::Carried;
                break;
            default: break;   // Parked (absent, or past the horizon); no other applies to a code bias
        }
        if (f) out[{cp.sat, cp.signal}] = *f;
    }
    return out;
}

// The family clock datum's kept-epoch bookkeeping (see the header).
void networkCodeDatumFamilyKeptEpoch(
    NetworkRunContext& ctx,
    const std::function<void(const char*, const char*, const std::string&,
                             const std::string&, std::optional<double>,
                             std::optional<double>, const char*,
                             const std::string&)>& emitEvent, const EpochEvents& events) {
    net::NetworkEntities& entities = ctx.entities;   // written: constellations sats
    using Outcome = NetworkSbasis::PinPairOutcome;
    // The pin pair holds of this kept epoch (network.code_datum_pair_continuity): each
    // held satellite-epoch is counted, and a hold is declared at both edges against the
    // last kept epoch's holds (standing invariant #7: the pin stops following the
    // selection at one edge and resumes at the other).
    const auto pairText = [](const std::pair<std::string, std::string>& p) {
        return p.first + "+" + p.second;
    };
    const auto holdText = [&](const NetworkSbasis::PinPairHold& h) {
        return "selected " + pairText(h.selected) + " vs held " + pairText(h.held);
    };
    const auto startText = [&](const NetworkSbasis::PinPairHold& h) {
        return holdText(h) + (h.reseeded ? "; marked by a channel-level re-seed" : "");
    };
    // The hold baseline is not snapshotted, so the first kept epoch after a warm resume
    // cannot tell a hold that begins here from one the writer's last kept epoch carried:
    // such a hold is declared "hold_resumed" and counted apart from the holds begun.
    const bool after_resume = std::exchange(ctx.carry.resume.if_pin_holds, false);
    const auto& holds = events.code_datum.this_epoch_if_pin_holds;
    for (const auto& [sat, hold] : holds) {
        NetworkSbasis::CodeDatumPairCounts& c = ctx.counters.cb_pair_by_const[systemOfSat(sat)];
        ++c.held;
        if (hold.reseeded) ++c.held_reseeded;
        if (net::getSat(ctx.entities.sats, sat, &net::SatelliteState::last_if_pin_hold) != nullptr) continue;
        if (after_resume) {
            ++c.hold_resumed;
            emitEvent("CLK_BASIS", "held", sat, "", std::nullopt, std::nullopt, "hold_resumed",
                      startText(hold) + "; first kept epoch after a warm resume");
        } else {
            ++c.hold_begun;
            emitEvent("CLK_BASIS", "held", sat, "", std::nullopt, std::nullopt, "hold_begin",
                      startText(hold));
        }
    }
    for (const auto& [sat, hold] : net::satMap(ctx.entities.sats, &net::SatelliteState::last_if_pin_hold)) {
        if (holds.count(sat) != 0) continue;
        ++ctx.counters.cb_pair_by_const[systemOfSat(sat)].hold_ended;
        // What ended it, from the pair the satellite is pinned on now: still the held pair
        // (the selection named it again: the entering signal left), another pair (the pin
        // moved by a pair change, named with its outcome: continued when the hold was
        // released or a held signal left with previous values on the new pair), or no pin
        // (the satellite has no ionosphere-free pin this kept epoch).
        const auto now = events.code_datum.this_epoch_cb_ifpin.find(sat);
        std::string ending;
        if (now == events.code_datum.this_epoch_cb_ifpin.end()) {
            ending = "; not pinned now";
        } else if (now->second == hold.held) {
            ending = "; pinned " + pairText(now->second) + " (the selection named it again)";
        } else {
            const auto change = events.code_datum.this_epoch_if_pin_changes.find(sat);
            const char* how = "a pair change";
            if (change != events.code_datum.this_epoch_if_pin_changes.end()) {
                switch (change->second.outcome) {
                    case Outcome::Continued:     how = "continued"; break;
                    case Outcome::Reseeded:      how = "re-seeded"; break;
                    case Outcome::LevelReseeded: how = "continued, re-seeded by a level row"; break;
                }
            }
            ending = "; pinned " + pairText(now->second) + " (" + how + ")";
        }
        emitEvent("CLK_BASIS", "held", sat, "", std::nullopt, std::nullopt, "hold_end",
                  holdText(hold) + ending);
    }
    net::assignSat(entities, &net::SatelliteState::last_if_pin_hold, holds);
    // The pin pair changes of this kept epoch (network.code_datum_pair_continuity).
    for (const auto& [sat, change] : events.code_datum.this_epoch_if_pin_changes) {
        NetworkSbasis::CodeDatumPairCounts& c = ctx.counters.cb_pair_by_const[systemOfSat(sat)];
        switch (change.outcome) {
            case Outcome::Continued:
                ++c.continued;
                emitEvent("CLK_BASIS", "continued", sat, "", std::nullopt, std::nullopt,
                          "if_pin_pair_continued",
                          change.from.first + "+" + change.from.second + "->"
                              + change.to.first + "+" + change.to.second);
                break;
            case Outcome::Reseeded:
                ++c.reseeded;
                if (change.hold_refused_gd) ++c.hold_refused_gd;
                break;
            case Outcome::LevelReseeded: ++c.level_reseeded; break;
        }
    }
    // The secondary-family level realizations (network.code_datum_family_difference).  A
    // channel's row identity is the family row with its partner, or the sum-to-zero row
    // (empty partner); a change of identity against the last kept epoch is declared.
    const auto& rows = events.code_datum.this_epoch_cb_family_rows;
    if (!rows.empty()) ++ctx.counters.n_cb_family_epochs;
    const auto rowText = [](const std::string& partner) {
        return partner.empty() ? std::string("sum_to_zero") : "family_difference/" + partner;
    };
    for (const auto& [chan, r] : rows) {
        ctx.counters.cb_family_by_band[{r.channel.sys, r.channel.digit}].add(r);
        const std::string partner = r.familyRow() ? r.partner : std::string();
        // The channel's partner at the last kept epoch ("<c>|<signal>").
        const auto ch = net::channelOfKey(ctx.entities, chan);
        const std::string* last =
            ch ? net::get(ctx.entities.constellations, ch->first, &net::ConstellationState::channels,
                          ch->second, &net::ChannelState::last_cb_family_partner)
               : nullptr;
        if (last != nullptr && *last != partner) {
            ++ctx.counters.n_cb_family_switches;
            emitEvent("CB_DATUM", "family_row", "", "", std::nullopt, std::nullopt,
                      partner.empty() ? "to_sum_to_zero" : "to_family_difference",
                      chan + ": " + rowText(*last) + "->" + rowText(partner) + " ("
                          + NetworkSbasis::familyRowOutcomeName(r.outcome) + ")");
        }
        if (ch)
            net::put(entities.constellations, ch->first, &net::ConstellationState::channels,
                     ch->second, &net::ChannelState::last_cb_family_partner) = partner;
    }
    // The first-sighting family prior of this kept epoch (
    // network.code_family_birth_prior_sigma_m): each soft row folded on this kept epoch,
    // counted per (constellation, band) and declared once, and each common satellite given
    // no row, counted; a birth with no target is declared too (a birth the prior missed).
    const auto columnsOf = [](NetworkSbasis::FamilyBirthSide side, const std::string& x,
                              const std::string& p) {
        return side == NetworkSbasis::FamilyBirthSide::Partner     ? p
             : side == NetworkSbasis::FamilyBirthSide::Secondary   ? x
                                                                    : x + "+" + p;
    };
    for (const NetworkSbasis::FamilyBirthPrior& b : events.code_datum.this_epoch_cb_family_births) {
        ctx.counters.cb_family_birth_by_band[{b.channel.sys, b.channel.digit}].add(b);
        const char* born_text = b.born == NetworkSbasis::FamilyBirthSide::Secondary
                                    ? "the secondary column born"
                              : b.born == NetworkSbasis::FamilyBirthSide::Partner
                                    ? "the partner column born"
                                    : "both columns born";
        emitEvent("CB_DATUM", "family_birth_prior", b.sat, "", b.mu_m, b.sigma_m,
                  NetworkSbasis::familyBirthSideName(b.born),
                  fmt::format("{}: {}-{} = mu, {} ({}{}{}); mu {:+.4f} m over {} veteran(s){}; "
                              "sigma {:.3f} m",
                              b.sat, b.x_signal, b.partner, born_text,
                              columnsOf(b.born, b.x_signal, b.partner),
                              b.loosened ? ", a loosened carry" : "",
                              b.seen_before ? ", satellite seen before" : "",
                              b.mu_m, b.n_veterans,
                              b.n_veterans == 0 ? " (none: the family row's re-seed at 0)" : "",
                              b.sigma_m));
    }
    for (const NetworkSbasis::FamilyBirthSkipRecord& s :
         events.code_datum.this_epoch_cb_family_birth_skips) {
        ctx.counters.cb_family_birth_by_band[{s.channel.sys, s.channel.digit}].add(s);
        if (s.why != NetworkSbasis::FamilyBirthSkip::NoReference) continue;
        emitEvent("CB_DATUM", "family_birth_prior", s.sat, "", std::nullopt, std::nullopt,
                  NetworkSbasis::familyBirthSkipName(s.why),
                  fmt::format("{}: {}-{}, {} born with no realized family mean to target "
                              "(channel {}, no veteran): no row",
                              s.sat, s.x_signal, s.partner,
                              columnsOf(s.side, s.x_signal, s.partner),
                              NetworkSbasis::familyRowOutcomeName(s.outcome)));
    }
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

/**
 * @brief Run-end section: the S-basis datum arms.
 *
 * The S-basis datum arms: no ionosphere constraint, the satellite geometry-free datum
 * and its time link, the inter-signal correction state, the extra-band free columns and
 * the receiver code-bias datum, each under its own knob.
 *
 * Gate: each section under its own condition, as in the teardown:
 * `config_.getNetworkConfig().w6_iono_constraint == NetworkW6IonoConstraint::None`;
 * `config_.getNetworkConfig().sat_gf_datum != NetworkSatGfDatum::NoneMode`;
 * `config_.getNetworkConfig().sat_gf_datum_time_linked`;
 * `config_.getNetworkConfig().sat_isc_state`;
 * `config_.getNetworkConfig().sat_gf_extra_band_free`;
 * `config_.networkRxCodeBiasDatum()`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkSbasisDatumRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // No-ionosphere-constraint posture (network.w6_iono_constraint = none, RF-0b): run-end
    // line, printed only when armed.  It answers for the satellite half the question the
    // RXCB-DATUM line answers for the receivers: in this posture the sum-to-zero rows are
    // not a gauge beside an ionosphere constraint but the satellite code-bias datum
    // itself, so the fold count is what separates a live datum from an armed-but-inert
    // one (invariant #4).
    if (config_.getNetworkConfig().w6_iono_constraint == NetworkW6IonoConstraint::None) {
        if (ctx.counters.n_w6_iono_none_epochs == 0)
            log->warn("Network (W6-IONO-NONE): the no-ionosphere-constraint posture is ARMED but "
                      "the satellite code-bias datum folded on ZERO epoch(s) — no epoch ever "
                      "presented a registered code-bias column, so the satellite code biases ran "
                      "with NO datum and NO ionosphere constraint at once.  This run is NOT the "
                      "posture it claims (standing invariant #4): check "
                      "network.estimate_sat_code_bias and the signal coverage gate.");
        else if (ctx.counters.n_w6_iono_none_s2z_rows == 0)
            log->warn("Network (W6-IONO-NONE): the datum folded on {} epoch(s) but pinned ZERO "
                      "(constellation, signal) sum-to-zero level(s) — the ionosphere-free pin was "
                      "folded against nothing, so every channel's constellation-wide code-bias "
                      "LEVEL is unpinned on top of the between-satellite split the missing "
                      "ionosphere constraint already left open.  Do not grade the emitted code "
                      "biases from this run.",
                      ctx.counters.n_w6_iono_none_epochs);
        else
            log->info("Network (W6-IONO-NONE): NO ionosphere constraint over the run — the "
                      "satellite code-bias datum folded on {} epoch(s), pinning {} "
                      "(constellation, signal) sum-to-zero level(s) in total (mean {:.2f} per "
                      "epoch) alongside the per-satellite ionosphere-free pin.  Those rows pin "
                      "each channel's constellation-wide LEVEL; the BETWEEN-satellite "
                      "geometry-free directions are left to the OSB prior "
                      "(network.cb_kaula_prior {}), so read the null-space gate's "
                      "observed-vs-declared before grading the emitted code biases, and apply the "
                      "emitted ionosphere and code biases AS A PAIR.",
                      ctx.counters.n_w6_iono_none_epochs,
                      ctx.counters.n_w6_iono_none_s2z_rows,
                      static_cast<double>(ctx.counters.n_w6_iono_none_s2z_rows)
                          / static_cast<double>(ctx.counters.n_w6_iono_none_epochs),
                      config_.networkCbKaulaPrior()
                          ? fmt::format("ON, sigma {:.1f} m", config_.networkCbKaulaSigmaM())
                          : std::string{"OFF"});
    }
    // Satellite geometry-free datum (network.sat_gf_datum, RF-0c): run-end line, printed
    // only when armed.  Under the no-constraint posture this row alone closes each
    // satellite's geometry-free direction, so its fold count separates a datumed product
    // from one whose code biases were shaped by whatever loose prior was armed.
    if (config_.getNetworkConfig().sat_gf_datum != NetworkSatGfDatum::NoneMode) {
        const bool tgd = config_.getNetworkConfig().sat_gf_datum
                             == NetworkSatGfDatum::BroadcastTgd;
        const char* mode = tgd ? "broadcast_tgd" : "zero";
        if (ctx.counters.n_sat_gf_rows == 0)
            log->warn("Network (SAT-GF-DATUM): the hard per-satellite geometry-free datum "
                      "is ARMED at '{}' but folded ZERO row(s) over the run — no satellite "
                      "ever presented a two-distinct-frequency code-bias pair, so every "
                      "satellite's geometry-free direction ran UNPINNED under a free slant "
                      "ionosphere.  This run is NOT the posture it claims (standing "
                      "invariant #4); do not grade its code biases or its ionosphere.",
                      mode);
        else if (tgd && ctx.counters.n_sat_gf_tgd == 0)
            log->warn("Network (SAT-GF-DATUM): '{}' folded {} row(s) on {} epoch(s) but NOT "
                      "ONE carried a non-zero c*T_GD ({} broadcast T_GD = 0, {} fell back "
                      "to zero for want of a navigation record or an L1/L2 pinned pair) — "
                      "the run is numerically identical to sat_gf_datum = zero and must be "
                      "reported as such, not as the physical datum.",
                      mode, ctx.counters.n_sat_gf_rows, ctx.counters.n_sat_gf_epochs,
                      ctx.counters.n_sat_gf_tgd0, ctx.counters.n_sat_gf_fallback);
        else
            log->info("Network (SAT-GF-DATUM): hard per-satellite GEOMETRY-FREE datum '{}' "
                      "— {} pin row(s) over {} epoch(s) (mean {:.2f} satellites per epoch), "
                      "each on (d2-d1)/(mu2-mu1) over that satellite's ionosphere-free pin "
                      "pair, folded as a constraint row BEFORE the null-space gate so the "
                      "declared S-basis dimension is untouched.{}  The estimable slant "
                      "ionosphere absorbs the pinned value (Odijk 2016 Table 8), so the "
                      "emitted ionosphere and code biases must be applied AS A PAIR.",
                      mode, ctx.counters.n_sat_gf_rows, ctx.counters.n_sat_gf_epochs,
                      ctx.counters.n_sat_gf_epochs
                          ? static_cast<double>(ctx.counters.n_sat_gf_rows)
                                / static_cast<double>(ctx.counters.n_sat_gf_epochs)
                          : 0.0,
                      tgd ? fmt::format("  PER-SIGNAL (RF-0d): {} extra row(s) beyond the "
                                        "ionosphere-free pair, of which {} ASSERT an "
                                        "inter-signal correction of ZERO (RINEX navigation "
                                        "carries no GPS ISC — that assertion is the part of "
                                        "this 'broadcast' datum the message does not state); "
                                        "{} same-band column(s) left ESTIMABLE on purpose (no "
                                        "geometry-free combination exists against the "
                                        "reference, and the pure ISC needs no ionosphere to "
                                        "be observable); {} sum-to-zero row(s) SUPPRESSED as "
                                        "channels the datum already pins completely (folding "
                                        "both would fight: the pinned values sum to "
                                        "mu*c*Sum_s T_GD, not to zero).  TARGETS: {} row(s) "
                                        "at a non-zero c*T_GD, {} at a "
                                        "broadcast T_GD of exactly 0, {} FELL BACK to zero "
                                        "(no navigation record, or a pinned pair the "
                                        "navigation message never referenced) — the "
                                        "fallback count is the part of this product that "
                                        "is NOT the physical datum it is named for.",
                                        ctx.counters.n_sat_gf_extra,
                                        ctx.counters.n_sat_gf_isc0,
                                        ctx.counters.n_sat_gf_sameband,
                                        ctx.counters.n_sat_gf_s2z_suppressed,
                                        ctx.counters.n_sat_gf_tgd,
                                        ctx.counters.n_sat_gf_tgd0,
                                        ctx.counters.n_sat_gf_fallback)
                          : fmt::format("  PER-SIGNAL (RF-0d): {} extra row(s) beyond the "
                                        "ionosphere-free pair, {} same-band column(s) left "
                                        "ESTIMABLE, {} sum-to-zero row(s) suppressed as "
                                        "channels the datum already pins completely.",
                                        ctx.counters.n_sat_gf_extra,
                                        ctx.counters.n_sat_gf_sameband,
                                        ctx.counters.n_sat_gf_s2z_suppressed));
    }
    if (config_.getNetworkConfig().sat_gf_datum_time_linked) {
        const std::size_t tot = ctx.counters.n_sat_gf_carried + ctx.counters.n_sat_gf_anchored;
        if (tot == 0)
            log->warn("Network (SAT-GF TIME-LINK, RF-0e e2): ARMED but no geometry-free row "
                      "was ever targeted — the datum folded nothing, so this run did not "
                      "test the arm it claims to (standing invariant #4).");
        else
            log->info("Network (SAT-GF TIME-LINK, RF-0e e2): {} geometry-free row(s) CARRIED "
                      "the previous kept epoch's solved combination and {} were ANCHORED at "
                      "the absolute target ({:.1f} % anchored — first sightings).  The "
                      "anchored rows are the ONLY place the broadcast frame is re-asserted; "
                      "a run with very few of them has carried its frame from ignition and "
                      "never re-checked it, which is the cost of this arm.",
                      ctx.counters.n_sat_gf_carried, ctx.counters.n_sat_gf_anchored,
                      100.0 * static_cast<double>(ctx.counters.n_sat_gf_anchored)
                          / static_cast<double>(tot));
    }
    // Inter-signal correction state (RF-0f): run-end line, printed only when armed.  On the
    // wire an estimated and an asserted inter-signal correction differ only by the isc=
    // token and the missing record, so the run states the split (invariant #4).
    if (config_.getNetworkConfig().sat_isc_state) {
        const std::size_t tot = ctx.counters.n_isc_observed + ctx.counters.n_isc_pinned;
        if (tot == 0)
            log->warn("Network (ISC-STATE, RF-0f): ARMED but no band-2 same-band companion "
                      "column was ever seen — no satellite carried a second band-2 code, so "
                      "this run exercised nothing and is byte-equivalent to the unarmed "
                      "posture (standing invariant #4).");
        else
            log->info("Network (ISC-STATE, RF-0f): {} companion column-epoch(s) left FREE "
                      "because a station tracked both signals of that satellite, {} PINNED "
                      "at 0 for want of one ({:.1f} % pinned).  A free one is an estimated "
                      "inter-signal correction and its bias is emitted with isc=obs; a "
                      "pinned one is the RF-0d assertion of ISC = 0 and its bias is NOT "
                      "emitted, because a consumer cannot tell the two apart from the value.",
                      ctx.counters.n_isc_observed, ctx.counters.n_isc_pinned,
                      100.0 * static_cast<double>(ctx.counters.n_isc_pinned)
                          / static_cast<double>(tot));
    }
    // Extra-band free columns (B5D): run-end line, printed only when armed.  The effect is
    // which columns stop being asserted, and neither the wire nor the trace stream shows
    // it: a freed column looks the same as a pinned one in the B record (invariant #4).
    if (config_.getNetworkConfig().sat_gf_extra_band_free) {
        const std::size_t tot = ctx.counters.n_extra_band_free
                              + ctx.counters.n_extra_band_carried
                              + ctx.counters.n_extra_band_held;
        if (tot == 0)
            log->warn("Network (EXTRA-BAND FREE, B5D): ARMED but no extra-band code column "
                      "was ever seen — no satellite carried a registered code signal "
                      "outside its ionosphere-free pin pair's two bands, so this run "
                      "exercised nothing and is byte-equivalent to the unarmed posture "
                      "(standing invariant #4).  A dual-frequency lineage reads this way "
                      "by construction; on a triple one it means the third band never "
                      "reached the >= 2-station register.");
        else
            log->info("Network (EXTRA-BAND FREE, B5D): {} column-epoch(s) FREED (a station "
                      "carried code on both of the pin pair's bands and on the signal, so "
                      "the data determine the column up to its family constant and the "
                      "channel's sum-to-zero row is that constant's datum), {} CARRIED at "
                      "their own previous solved geometry-free combination because no "
                      "station determined them that epoch, {} HELD at a first sighting with "
                      "nothing to carry ({:.2f} % held) — a held column is pinned by a "
                      "level row in the channel's own frame and its bias is NOT emitted, "
                      "because a value chosen to keep the rank is indistinguishable from "
                      "an estimate.  Every one of these would have been an ASSERTION of a "
                      "zero inter-signal correction under RF-0d alone (counted beside this "
                      "as n_sat_gf_isc_zero = {}), which the three-band ignition study measured at "
                      "metres across "
                      "a band.  The freed channels' LEVEL now re-references whenever the "
                      "veteran link re-seeds: {} sum-to-zero row(s) re-seeded over the run, "
                      "marking {} satellite-epoch(s) for the consumer's discontinuity "
                      "channel (standing invariant #7, the decoupling edge).",
                      ctx.counters.n_extra_band_free, ctx.counters.n_extra_band_carried,
                      ctx.counters.n_extra_band_held,
                      100.0 * static_cast<double>(ctx.counters.n_extra_band_held)
                          / static_cast<double>(tot),
                      ctx.counters.n_sat_gf_isc0,
                      ctx.counters.n_s2z_reseed_rows, ctx.counters.n_s2z_reseed_sats);
    }
    // Receiver code-bias datum (RXCB-DATUM): run-end line, printed only when armed.  The
    // fold count is the only way to tell a live gauge from an armed-but-inert one, so it
    // is printed even when it is zero (invariant #4).
    if (config_.networkRxCodeBiasDatum()) {
        if (ctx.counters.n_rxcb_gauges == 0)
            log->warn("Network (RXCB-DATUM): receiver code-bias datum is ARMED but folded ZERO "
                      "prior row(s) over the run — no station ever presented a receiver DCB "
                      "column, so the receiver-DCB common mode was NEVER gauged and this run is "
                      "NOT the posture it claims (standing invariant #4).  Check that "
                      "estimation.estimate_ifb is on and that the DCB states exist.");
        else
            log->info("Network (RXCB-DATUM): {} zero-mean receiver code-bias prior row(s) folded "
                      "over the run (one per station-epoch per constellation, sigma {:.1f} m, "
                      "realized common-mode information 1/sigma^2 = {:.3e} INDEPENDENT of station "
                      "count).  Folded PRE-reduce through the diagonal prior channel, so it "
                      "composes with the invariant-carry DD priors that refuse a post-reduce Woodbury "
                      "gauge.  This is the IGS zero-mean-over-receivers DCB convention, replacing "
                      "the datum the satellite code-bias sum-to-zero silently also provided.",
                      ctx.counters.n_rxcb_gauges,
                      config_.networkRxCodeBiasDatumSigmaM(),
                      1.0 / (config_.networkRxCodeBiasDatumSigmaM()
                             * config_.networkRxCodeBiasDatumSigmaM()));
    }
    return 0;
}

// The family clock datum's run-end readout (see the header).
void networkCodeDatumFamilyRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                                  const std::shared_ptr<spdlog::logger>& log) {
    const bool pc = config_.networkCodeDatumPairContinuity();
    const bool fd = config_.networkCodeDatumFamilyDifference();
    // Step 2's first-sighting family prior (network.code_family_birth_prior_sigma_m).
    const double bp_sigma = config_.networkCodeFamilyBirthPriorSigmaM();
    const bool bp = bp_sigma > 0.0;
    if (!pc && !fd && !bp) return;
    // Every arm refines the time-linked code datum, which exists only with the W6 satellite
    // code bias; without either the builder never read them (standing invariant #4).
    std::string missing;
    if (!config_.networkCodeDatumTimeLinked()) missing = "network.code_datum_time_linked";
    if (!config_.estimateNetworkSatCodeBias())
        missing += std::string(missing.empty() ? "" : " and ") + "network.estimate_sat_code_bias";
    if (!missing.empty()) {
        if (pc || fd)
            log->warn("Network (GFC): {} ARMED but INERT without {}.  {} the time-linked code "
                      "datum, which this run does not realize, so the builder never read {} "
                      "and the run is the unarmed posture (standing invariant #4).",
                      pc && fd ? "network.code_datum_pair_continuity and "
                                 "network.code_datum_family_difference"
                      : pc     ? "network.code_datum_pair_continuity"
                               : "network.code_datum_family_difference",
                      missing, pc && fd ? "Both refine" : "It refines", pc && fd ? "them" : "it");
        // The prior also needs the family difference: name it too when it is off, so the
        // line lists every prerequisite the loader's warning does.
        if (bp)
            log->warn("Network (GFC family birth prior): network.code_family_birth_prior_sigma_m "
                      "= {:g} m ARMED but INERT without {}.  The prior refines the time-linked "
                      "family-difference datum, so the builder never read it and folded no "
                      "row (standing invariant #4).",
                      bp_sigma,
                      fd ? missing
                         : "network.code_datum_family_difference and " + missing);
        return;
    }
    if (pc) {
        const auto& nc = config_.getNetworkConfig();
        const char* blocker = NetworkSbasis::pinPairContinuityBlocker(
            nc.sat_gf_datum, nc.sat_gf_datum_time_linked);
        if (blocker) {
            log->warn("Network (GFC pair continuity): ARMED but INERT under {}.  A continued "
                      "pin would hold the new pair's ionosphere-free combination while that "
                      "row moves the pair's other combination, a step no discontinuity "
                      "declares, so the builder did not read the arm: the pin followed the "
                      "selection and every pair change re-seeded (standing invariant #4).",
                      blocker);
        } else {
            NetworkSbasis::CodeDatumPairCounts sum;
            std::string per, per_hold;
            for (const auto& [sys, c] : ctx.counters.cb_pair_by_const) {
                sum.continued += c.continued;
                sum.reseeded += c.reseeded;
                sum.level_reseeded += c.level_reseeded;
                sum.held += c.held;
                sum.hold_begun += c.hold_begun;
                sum.hold_ended += c.hold_ended;
                sum.held_reseeded += c.held_reseeded;
                sum.hold_resumed += c.hold_resumed;
                sum.hold_refused_gd += c.hold_refused_gd;
                sum.hold_split_decl += c.hold_split_decl;
                if (c.continued + c.reseeded + c.level_reseeded > 0)
                    per += fmt::format("{}{} {}/{}/{}", per.empty() ? "" : ", ",
                                       constellationKey(sys), c.continued, c.reseeded,
                                       c.level_reseeded);
                if (c.held + c.hold_begun + c.hold_ended + c.hold_resumed + c.hold_refused_gd
                        + c.hold_split_decl > 0)
                    per_hold += fmt::format("{}{} {}/{}/{}/{}/{}/{}/{}",
                                            per_hold.empty() ? "" : ", ",
                                            constellationKey(sys), c.held, c.held_reseeded,
                                            c.hold_begun, c.hold_ended, c.hold_resumed,
                                            c.hold_refused_gd, c.hold_split_decl);
            }
            // Step 1, the hold: the pin kept the last kept epoch's pair while the selected
            // pair could not be continued yet (an entering signal without a previous
            // value), so no pair change happened that epoch; the next kept epoch normally
            // releases it through a continued change (step 2, the next line).
            log->info("Network (GFC pair hold): over the kept epochs, {} satellite-epoch(s) "
                      "HELD the last kept pin pair while the selected pair could not be "
                      "continued yet (a lower-ordered signal had entered without a previous "
                      "solved value; the held pair's own previous combination carried, so the "
                      "pin neither changed pair nor re-seeded), {} of them marked anyway by a "
                      "channel-level re-seed (declared as a discontinuity); {} hold(s) begun, "
                      "{} ended and {} found at the first kept epoch after a warm resume; {} "
                      "hold(s) refused because the broadcast group delay describes the "
                      "selected pair and not the held one (re-seeded instead); {} emitted "
                      "epoch(s) dropped a constellation's cbpin= declaration for a pin pair "
                      "split that included a held satellite{}.  Events: network_events.csv "
                      "CLK_BASIS/held.",
                      sum.held, sum.held_reseeded, sum.hold_begun, sum.hold_ended,
                      sum.hold_resumed, sum.hold_refused_gd, sum.hold_split_decl,
                      per_hold.empty()
                          ? ""
                          : fmt::format(" (per constellation, held/re-seeded/begun/ended/"
                                        "resumed/refused/cbpin dropped: {})", per_hold));
            // Step 2, the continuation: the pair changes that did happen (the release of a
            // hold, a held signal leaving, or a satellite with no hold).
            log->info("Network (GFC pair continuity): over the kept epochs, {} ionosphere-free "
                      "pin pair change(s) CONTINUED (the target was the new pair's previous "
                      "solved combination, so the satellite clock did not step and no re-seed "
                      "was marked), {} RE-SEEDED at 0 (a signal of the new pair had no "
                      "previous solved value) and {} continued but "
                      "re-seeded by a channel level row the same epoch{}.  At the emit site "
                      "{} pair re-selection discontinuity(ies) were left undeclared for "
                      "continued satellites, {} declared anyway (the last emitted pair was not "
                      "the pair continued from, or a re-seed was marked or pending) and {} "
                      "continued satellite-epoch(s) were outside the emission and compared at "
                      "their next emit{}.  Events: network_events.csv CLK_BASIS/continued.",
                      sum.continued, sum.reseeded, sum.level_reseeded,
                      per.empty() ? ""
                                  : fmt::format(" (per constellation, continued/re-seeded/"
                                                "level re-seeded: {})", per),
                      ctx.counters.n_if_pin_continued_undeclared,
                      ctx.counters.n_if_pin_continued_declared,
                      ctx.counters.n_if_pin_continued_unemitted,
                      ctx.knobs.emit_clock_disc
                          ? ""
                          : "; network.emit_clock_disc is off, so this run declares no pair "
                            "re-selection discontinuity at all");
        }
    }
    if (fd) {
        if (ctx.counters.cb_family_by_band.empty()) {
            log->warn("Network (GFC family difference): ARMED but no secondary-family (X) "
                      "Galileo code channel kept a level row on any kept epoch (none "
                      "registered, or each suppressed by the geometry-free datum), so the "
                      "arm never acted and this run is the unarmed posture (standing "
                      "invariant #4).");
        } else {
            std::string per;
            int family_rows = 0;
            for (const auto& [band, c] : ctx.counters.cb_family_by_band) {
                family_rows += c.family_veteran + c.family_reseed;
                per += fmt::format("{}{} band {}: {} family row(s) on veterans, {} re-seeded "
                                   "(marking {} satellite(s)); the sum-to-zero row kept {} "
                                   "time(s) bridging a family re-seed, {} with no common "
                                   "satellite, {} with no same-band primary channel, {} "
                                   "dependent on pinned columns",
                                   per.empty() ? "" : "; ", constellationKey(band.first),
                                   band.second, c.family_veteran, c.family_reseed,
                                   c.reseed_sats, c.bridged, c.no_common, c.no_partner,
                                   c.dependent);
            }
            log->info("Network (GFC family difference): over {} kept epoch(s) with a "
                      "secondary-family channel, {} channel-epoch(s) carried Sum (d_X - d_P) "
                      "over the satellites tracking both families in place of the sum-to-zero "
                      "row (the inter-family mean, COD's convention): {}.  {} switch(es) "
                      "between the two rows declared (network_events.csv CB_DATUM/family_row).",
                      ctx.counters.n_cb_family_epochs, family_rows, per,
                      ctx.counters.n_cb_family_switches);
            if (family_rows == 0)
                log->warn("Network (GFC family difference): ARMED but the family-difference "
                          "row was never built (every channel-epoch kept the sum-to-zero row "
                          "for the reasons above), so this run is the unarmed posture "
                          "(standing invariant #4).");
        }
    }
    // Step 2: the first-sighting family prior.  It is read only with the family
    // difference; with it, 0 rows means no first sighting with a target beside a partner
    // ever occurred on a kept epoch (or no secondary-family channel at all), which the
    // counters cannot tell from inert wiring, so it is a warning (standing invariant #4).
    if (bp) {
        int rows = 0;
        std::set<std::string> sats;
        std::string per;
        for (const auto& [band, c] : ctx.counters.cb_family_birth_by_band) {
            rows += c.rows;
            sats.insert(c.sats.begin(), c.sats.end());
            per += fmt::format("{}{} band {}: {} row(s) on {} satellite(s) (secondary "
                               "column born {}, partner born {}, both {}; {} on satellites "
                               "seen before; {} on a loosened carry; {} at the family row's "
                               "re-seed target 0; max |mu| {:.4f} m); no row on {} READMIT "
                               "seed(s), {} carried column(s) without a previous value, {} "
                               "birth(s) with no realized family mean",
                               per.empty() ? "" : "; ", constellationKey(band.first),
                               band.second, c.rows, c.sats.size(), c.x_born,
                               c.partner_born, c.both_born, c.seen_before, c.loosened,
                               c.no_veteran, c.max_abs_mu_m, c.seeded, c.carried,
                               c.no_reference);
        }
        if (!fd) {
            log->warn("Network (GFC family birth prior): network.code_family_birth_prior_sigma_m "
                      "= {:g} m ARMED but INERT without network.code_datum_family_difference.  "
                      "The prior refines the family-difference datum, so the builder never "
                      "read it and folded no row (standing invariant #4).",
                      bp_sigma);
        } else if (rows == 0) {
            log->warn("Network (GFC family birth prior): sigma {:g} m ARMED but ZERO soft rows "
                      "folded: no Galileo band-1 secondary-family (X) column or partner was at "
                      "its first sighting beside the other with a realized family mean on any "
                      "kept epoch (or no such channel kept a level row), so this run is the "
                      "unarmed posture (standing invariant #4).{}",
                      bp_sigma, per.empty() ? std::string() : "  Counted: " + per + ".");
        } else {
            log->info("Network (GFC family birth prior): sigma {:g} m (information {:g}); over "
                      "the kept epochs {} soft row(s) d_X - d_P = mu folded, once per "
                      "(satellite, birth), on {} satellite(s): {}.  Events: "
                      "network_events.csv CB_DATUM/family_birth_prior.",
                      bp_sigma, 1.0 / (bp_sigma * bp_sigma), rows, sats.size(), per);
        }
    }
}

/**
 * @brief Run-end section: the family code-datum arms.
 *
 * The table's row for the family code-datum arms; the report is the overload above.
 *
 * Gate: none of its own: the overload above prints nothing unless
 * `config_.networkCodeDatumPairContinuity()`,
 * `config_.networkCodeDatumFamilyDifference()` or
 * `config_.networkCodeFamilyBirthPriorSigmaM() > 0`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkCodeDatumFamilyRunEnd(const NetworkRunEndView& v) {
    networkCodeDatumFamilyRunEnd(v.ctx, v.config, v.log);
    return 0;
}

// Run-end report of this subsystem: its sections in print order (
// declared in network/solve/Sbasis.hpp).
int networkSbasisRunEnd(const NetworkRunEndView& v) {
    return networkRunEndSections(v, {
        networkSbasisDatumRunEnd,
        networkCodeDatumFamilyRunEnd
    });
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
