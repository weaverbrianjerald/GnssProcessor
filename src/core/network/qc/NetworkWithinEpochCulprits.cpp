/**
 * @file NetworkWithinEpochCulprits.cpp
 * @brief The culprit hand-off's verdict and its run-end report (card WCF).
 *
 * What the culprit sets are, who writes and reads them and which defect the counters
 * expose are in the header's file banner, include/network/qc/NetworkWithinEpochCulprits.hpp.
 * The writer's count is taken in the T2 block of networkSolveGlue
 * (network/solve/NetworkSolveGlue.cpp), the reader's in networkBuildCarryDdPriors
 * (network/carry/NetworkDdPriorBuild.cpp).
 */

#include "network/qc/NetworkWithinEpochCulprits.hpp"

#include "network/state/NetworkRunContext.hpp"   // ctx.knobs (carry_invariant, pl_on), ctx.counters.we_culprit_withhold
#include "config/GnssConfig.hpp"       // network.raim_within_epoch (run-end gate)
#include "network/runend/NetworkRunEnd.hpp"    // NetworkRunEndView

#include <fmt/format.h>
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

bool withinEpochCulpritReportArmed(bool carry_invariant, bool raim_within_epoch) {
    return carry_invariant && raim_within_epoch;
}

std::string withinEpochCulpritQuietReason(bool pl_on, long long pool_walks, long long pool_rows) {
    if (!pl_on)
        return " (the tier runs only with network.phase_leveling, which is off)";
    if (pool_walks == 0)
        return " (the tier never assessed: it runs only on a warm phase-leveling carry)";
    if (pool_rows == 0)
        return fmt::format(
            " (the tier's phase pool admitted no row in {} assessment(s): under "
            "network.carry_invariant the assembly fills no leveling ambiguity column, so the "
            "tier cannot name a satellite or arc culprit and the withhold is structurally idle "
            "until the within-epoch pool is re-hosted, docs/SPEC_raim_pool_rehost.md deferral "
            "D1)",
            pool_walks);
    return "";
}

int networkWithinEpochCulpritWithholdRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    if (!withinEpochCulpritReportArmed(ctx.knobs.carry_invariant,
                                       v.config.networkRaimWithinEpoch()))
        return 0;
    const WithinEpochCulpritTally& t = ctx.counters.we_culprit_withhold;
    const WithinEpochCulpritReading r = withinEpochCulpritVerdict(t);
    // A run with no culprit says which quiet case it is (the tier never assessed, its phase
    // pool was empty, or it assessed rows and named none), so a structurally idle withhold is
    // not read as a quiet field.
    std::string note;
    if (r.verdict == WithinEpochCulpritVerdict::NoCulprit) {
        const RaimPoolAdmissionTally& pool = ctx.counters.raim_pool_within;
        long long pool_rows = 0;
        for (const long long n : pool.rows) pool_rows += n;
        note = withinEpochCulpritQuietReason(ctx.knobs.pl_on, pool.walks, pool_rows);
    }
    v.log->log(r.warn ? spdlog::level::warn : spdlog::level::info,
               "Network (T2 culprit withhold, DD carry): {} re-solve(s) ran with a satellite "
               "or arc culprit; {} DD-carry rebuild(s) read one and withheld {} satellite(s) "
               "and {} arc(s) summed over them, {} arc ambiguity(ies) of the carry graph "
               "(tree edges included, so at most that many carried DDs) marked slipped by a "
               "culprit and not by the slip funnel; {}{}.",
               t.resolves, t.rebuilds, t.sats, t.arcs, t.ambs, r.text, note);
    return 0;
}

} // namespace GnssProcessor
