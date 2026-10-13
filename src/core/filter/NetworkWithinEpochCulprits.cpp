/**
 * @file NetworkWithinEpochCulprits.cpp
 * @brief The culprit hand-off's verdict and its run-end report (card WCF).
 *
 * What the culprit sets are, who writes and reads them and which defect the counters
 * expose are in the header's file banner, include/filter/NetworkWithinEpochCulprits.hpp.
 * The writer's count is taken in the T2 block of networkSolveGlue
 * (filter/NetworkSolveGlue.cpp), the reader's in networkBuildCarryDdPriors
 * (filter/NetworkDdPriorBuild.cpp).
 */

#include "filter/NetworkWithinEpochCulprits.hpp"

#include "app/NetworkRunContext.hpp"   // ctx.knobs (carry_invariant, pl_on), ctx.counters.we_culprit_withhold
#include "config/GnssConfig.hpp"       // network.raim_within_epoch (run-end gate)
#include "filter/NetworkRunEnd.hpp"    // NetworkRunEndView

#include <spdlog/spdlog.h>

namespace GnssProcessor {

WithinEpochCulpritReading withinEpochCulpritVerdict(const WithinEpochCulpritTally& t) {
    if (t.resolves == 0 && t.rebuilds == 0)
        return {WithinEpochCulpritVerdict::NoCulprit, false,
                "no re-solve had a satellite or arc culprit, so the withhold had nothing to do"};
    if (t.resolves == t.rebuilds)
        return {WithinEpochCulpritVerdict::Agree, false,
                "every such re-solve's rebuild read its culprits"};
    if (t.rebuilds == 0)
        return {WithinEpochCulpritVerdict::InertWiring, true,
                "INERT WIRING: re-solves had culprits and no rebuild read one, so the "
                "culprits' stale carried DDs were folded again (card WCF)"};
    return {WithinEpochCulpritVerdict::OutOfStep, true,
            "the two counts differ, so the culprit sets and the rebuilds are out of step "
            "(card WCF)"};
}

int networkWithinEpochCulpritWithholdRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    if (!ctx.knobs.carry_invariant || !v.config.networkRaimWithinEpoch()) return 0;
    const WithinEpochCulpritTally& t = ctx.counters.we_culprit_withhold;
    const WithinEpochCulpritReading r = withinEpochCulpritVerdict(t);
    // The tier assesses only on a warm phase-leveling carry, so a quiet run without it is
    // named as such rather than read as a quiet field.
    const char* note = (r.verdict == WithinEpochCulpritVerdict::NoCulprit && !ctx.knobs.pl_on)
                           ? " (the tier runs only with network.phase_leveling, which is off)"
                           : "";
    v.log->log(r.warn ? spdlog::level::warn : spdlog::level::info,
               "Network (T2 culprit withhold, DD carry): {} re-solve(s) ran with a satellite "
               "or arc culprit; {} DD-carry rebuild(s) read one and withheld {} satellite(s) "
               "and {} arc(s) summed over them, {} carried ambiguity(ies) marked slipped by a "
               "culprit and not by the slip funnel; {}{}.",
               t.resolves, t.rebuilds, t.sats, t.arcs, t.ambs, r.text, note);
    return 0;
}

} // namespace GnssProcessor
