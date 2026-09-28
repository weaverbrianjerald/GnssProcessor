/**
 * @file NetworkHandoverStransform.cpp
 * @brief Implementation of the clock-pivot handover S-transform (queue 1(b),
 *        docs/SPEC_pivot_visibility_gauge_defect.md §8).
 */

#include "filter/NetworkHandoverStransform.hpp"

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

namespace GnssProcessor {

ClockStransformResult applyClockStransform(
    std::vector<CarryPrior>& carry,
    const std::vector<std::pair<std::string, double>>& v_c,
    const std::string& q) {
    ClockStransformResult out;
    if (v_c.empty() || q.empty()) return out;

    // Membership and coefficients come from the structural direction alone.
    std::map<std::string, double> coeff;
    for (const auto& e : v_c) coeff.emplace(e.first, e.second);

    const auto qit = coeff.find(q);
    if (qit == coeff.end() || !(std::abs(qit->second) > 0.0)) return out;
    const double vq = qit->second;

    // x_q_pre / var_q: the incoming pivot's carried level in the outgoing frame.
    // If it is absent there is nothing to re-reference from (a first sighting, or a
    // vanish-return whose carry aged out); the caller counts this no-op and writes
    // an event.
    const CarryPrior* qrow = nullptr;
    for (const auto& cp : carry)
        if (cp.kind == SharedParamKind::Clock && cp.sat == q) { qrow = &cp; break; }
    if (qrow == nullptr) return out;

    out.c0    = qrow->value / vq;
    out.var_q = qrow->var;

    // x <- x - v_c*c0 ; var <- var + v_c[i]^2 * var_q.  Clocks only: every other
    // shared family has a zero component along this direction because its gauge is
    // realized independently (NetworkSbasis::clockNullDirection documents the
    // measurement).
    for (auto& cp : carry) {
        if (cp.kind != SharedParamKind::Clock) continue;
        const auto cit = coeff.find(cp.sat);
        if (cit == coeff.end()) continue;
        const double vi = cit->second;
        cp.value -= vi * out.c0;
        cp.var   += vi * vi * out.var_q;
        ++out.rows;
    }
    if (out.rows == 0) return out;
    out.applied = true;

    // Exactness self-check (§8.4 INC-3): the incoming pivot's carried level must now
    // equal its pinned value (0) within kStsExactTolM, and re-deriving the coefficient
    // must also give 0 (idempotence).  A failure is reported, not thrown.
    for (const auto& cp : carry)
        if (cp.kind == SharedParamKind::Clock && cp.sat == q) { out.x_q_post = cp.value; break; }
    out.selfcheck_ok = std::abs(out.x_q_post) <= kStsExactTolM
                    && std::abs(out.x_q_post / vq) <= kStsExactTolM;
    return out;
}

void networkHandoverStransform(NetworkRunContext& ctx,
                               const GnssConfig& config_,
                               const std::shared_ptr<spdlog::logger>& log,
                               NetworkRunTraces& traces,
                               const GnssTime& t,
                               const NetworkSbasis::Plan& plan,
                               bool datum_changed,
                               const std::set<char>& changed_consts,
                               const std::vector<std::string>& registered_clock_sats) {
    (void)config_;   // seam signature is uniform across the run loop

    // The release set is per-epoch state.  Clear it unconditionally and first, so a
    // handover epoch's release cannot reach the next epoch's folds (which would
    // un-skip a retired datum without any signal).  Every non-handover epoch, and
    // every epoch with the knob off, therefore leaves an empty set: the off path is
    // byte-identical regardless of call order.
    ctx.carry.sts_consts.clear();
    ctx.carry.sts_c0.clear();

    if (!datum_changed || changed_consts.empty()) return;

    // Count every handover edge whether or not the transform is armed (the baseline
    // count increment 0 called for), so an off run reports the population an on run
    // acts on.
    for (char c : changed_consts) ++ctx.counters.pivot_handovers[c];

    if (!ctx.knobs.handover_stransform) return;

    for (char c : changed_consts) {
        const std::string key(1, c);
        const auto pit = plan.clock_pivot.find(key);
        if (pit == plan.clock_pivot.end() || pit->second.empty()) continue;
        const std::string& q = pit->second;   // the incoming pivot

        // v_c: the structural direction that NetworkSbasis owns (§8.1 rule 1).
        const auto v_c = NetworkSbasis::clockNullDirection(registered_clock_sats, key);
        if (v_c.empty()) continue;

        const ClockStransformResult r = applyClockStransform(ctx.carry.prev_carry, v_c, q);
        if (!r.applied) {
            ++ctx.counters.sts_noop_no_carry;
            traces.emitEvent(t, "STRANSFORM", "sts_noop", q, "", std::nullopt, std::nullopt,
                             "skip",
                             fmt::format("const={};reason=no_carried_level_for_incoming_pivot",
                                         key));
            continue;
        }

        ctx.counters.sts_applied += r.rows;
        ++ctx.counters.sts_edges;
        ctx.counters.sts_last_c0 = r.c0;
        ctx.counters.sts_max_abs_c0 = std::max(ctx.counters.sts_max_abs_c0, std::abs(r.c0));
        ctx.carry.sts_c0[c] = r.c0;
        ctx.carry.sts_consts.insert(c);   // release the fold skip for this constellation (INC-1)

        if (!r.selfcheck_ok) {
            ++ctx.counters.sts_selfcheck_fail;
            log->warn("Network (queue 1(b) STRANSFORM): exactness self-check FAILED for {} — "
                      "after the transform the incoming pivot {} carries {:.3e} m (expected 0)."
                      "  The transform is mis-wired; the carry it released is NOT in the "
                      "incoming datum.",
                      key, q, r.x_q_post);
            traces.emitEvent(t, "STRANSFORM", "sts_selfcheck", q, "", r.x_q_post, kStsExactTolM,
                             "fail", fmt::format("const={};c0={:.6e}", key, r.c0));
        }

        // The decoupling edge, written as an event with identities (invariant #7,
        // §8.4 INC-2).  The re-coupling edge is the transformed carry's first fold:
        // counted as sts_rows_retained at the fold sites and summarised in the
        // run-end clause.
        traces.emitEvent(t, "STRANSFORM", "sts_applied", q, "", r.c0, std::nullopt,
                         "transform",
                         fmt::format("const={};c0={:.6f};rows={};var_q={:.6e};nv={}",
                                     key, r.c0, r.rows, r.var_q, v_c.size()));
    }
}

}  // namespace GnssProcessor
