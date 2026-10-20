/**
 * @file ClockDatum.cpp
 * @brief Stage 3 of the network solve: the clock datum's declaration, the
 *        election, the constraint-realized clock rows and the pivot-handover verdict.
 */

#include "network/solve/ClockDatum.hpp"
#include "network/solve/SolveGlue.hpp"   // SolveDatum and the other typed values of the solve
#include "network/solve/PivotElection.hpp"   // the coverage predictor and its ranking

#include "network/state/RunContext.hpp"
#include "common/GnssIdentity.hpp"   // systemOfSat, letterOfSat, constellationKey, SignalId
#include "config/GnssConfig.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/solve/ChainDatum.hpp"
#include "network/solve/ClockAnchor.hpp"   // guarded temporal clock anchor
#include "network/carry/ClockClone.hpp"    // the one-epoch clock clone
#include "network/carry/SharedHistory.hpp" // the shared history
#include "network/solve/DatumGraph.hpp"   // datum graph census: the per-epoch spectral hand-off
#include "network/diag/FrameDiag.hpp"   // networkDatumInfoRatio (invariant #7)
#include "network/solve/HandoverStransform.hpp"   // the clock-pivot handover S-transform (queue 1(b))
#include "network/solve/PhaseDatum.hpp"
#include "network/solve/PivotHandover.hpp"
#include "network/qc/PrefitCensus.hpp"   // networkPrefitSigma0, the census-fed H σ̂₀ (H-rider)
#include "network/qc/Raim.hpp"           // RaimPoolAdmissionTally: the within-epoch pool's admission tally
#include "network/qc/Relevel.hpp"
#include "network/qc/RhsAttrib.hpp"   // flight-recorder trip latch and inline A4 dump (INC-5)
#include "network/qc/RobustStats.hpp"
#include "network/assembly/RowClassify.hpp"   // the row tags' readers: the pools' admission rule
#include "network/solve/Sbasis.hpp"
#include "filter/StateKeySchema.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "math/TimeUtils.hpp"

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/carry/DdPriorBuild.hpp"   // the replay's builders (T-F)

namespace GnssProcessor {

SbasisDeclaration networkClockDatumDeclaration(const NetworkRunContext& ctx,
                                               const GnssConfig& config_,
                                               double clk_anchor_sigma) {
    const auto& nc = config_.getNetworkConfig();
    const bool phase_datum_eq34     = ctx.knobs.phase_datum_eq34;
    const bool clock_brdc_anchored  = ctx.knobs.clock_brdc_anchored;
    const bool clock_sum_to_zero    = ctx.knobs.clock_sum_to_zero;
    SbasisDeclaration decl;
    decl.reference_receiver_id = nc.clock_datum_station;
    decl.emit_code_bias = config_.estimateNetworkSatCodeBias();  // self-describe the OSB datum (W6)
    // Self-describe the Eq-34 single-receiver-chain phase-bias frame, so a consumer knows
    // the emitted between-satellite reference is integer-consistent (each chain hop is an
    // integer).  The wire value is 2, on the existing v2 S line.
    if (phase_datum_eq34)
        decl.amb_datum = SbasisDeclaration::AmbDatum::SingleReceiverChain;
    if (clock_brdc_anchored) {
        // Broadcast-anchored clock gauge: buildSharedDatum branches on this to emit
        // one anchor row per satellite (Δδtˢ → 0, toward its own broadcast a-priori)
        // at weight 1/σ².  The anchors are the datum (no pivot, empty datum, declared
        // dimension 0), and unlike sum-to-zero the frame does not redistribute when
        // membership changes.
        decl.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
        decl.broadcast_anchor_sigma_m = clk_anchor_sigma;  // D6-resolved (brdc_rts auto = 3.0 m; load-validated binding)
    } else if (clock_sum_to_zero) {
        // Anchor-free clock gauge: buildSharedDatum branches on this to emit
        // per-constellation sum-to-zero constraint rows (no pivot, empty datum).
        decl.isb_datum = SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
    } else if (!nc.clock_datum_satellite.empty()) {
        decl.clock_pivot_sat[constellationKey(systemOfSat(nc.clock_datum_satellite))]
            = nc.clock_datum_satellite;
    }
    return decl;
}

NetworkPivotElectionEpoch::NetworkPivotElectionEpoch(const NetworkRunContext& ctx,
                                                     const NetworkSetupResult& setup,
                                                     const GnssTime& t)
    : active_(ctx.knobs.pivot_election != 0) {
    if (!active_) return;
    standby_ = net::letterMap(ctx.entities.constellations, &net::ConstellationState::elected_pivot);
    election_.standby = &standby_;
    NetworkPivotCoverage* coverage = setup.pivot_coverage.get();   // null: nothing can be ranked
    const double t_s = TimeUtils::continuousGpsSeconds(t);
    election_.rank = [coverage, t_s](const std::string& constellation,
                                     const std::vector<std::string>& candidates) {
        return coverage ? coverage->rank(t_s, constellation, candidates).sat : std::string();
    };
}

namespace {

/// Records the epoch's coverage elections: the run-end table, the event rows and one log
/// line each; and carries the pick of every constellation forward as the standby of the
/// next epoch (the pick holds until an emitted product adopts it or it leaves).
void notePivotElections(NetworkRun& run, const GnssTime& t, const NetworkSbasis::Plan& plan) {
    NetworkRunContext& ctx = run.ctx;
    NetworkPivotCoverage* coverage = run.setup.pivot_coverage.get();
    net::NetworkEntities& entities = ctx.entities;   // written: constellations
    ctx.counters.piv_ranked        += plan.pivot_ranked;
    ctx.counters.piv_rank_failures += plan.pivot_rank_failures;
    ctx.counters.piv_standby_kept  += plan.pivot_standby_kept;
    for (const std::string& c : plan.pivot_ranked_consts) {
        const PivotRanking* ranking = coverage ? coverage->last(c) : nullptr;
        if (!ranking) continue;
        coverage->noteElection(*ranking);
        const PivotCandidateScore* winner = ranking->winner();
        run.log->info("Network: clock pivot election, constellation {}: {}.", c, ranking->describe());
        run.traces.emitEvent(t, "PIVOT_ELECT", "elected", ranking->sat, "",
                             winner ? ranking->meanStations(*winner) : 0.0, std::nullopt, "elect",
                             ranking->describe());
    }
    for (const std::string& c : plan.pivot_rank_failed_consts) {
        const auto it = plan.clock_pivot.find(c);
        const std::string& pick = it != plan.clock_pivot.end() ? it->second : std::string();
        run.log->warn("Network: clock pivot election, constellation {}: no candidate could be "
                      "ranked (no orbit product for any), so {} stands.", c, pick);
        run.traces.emitEvent(t, "PIVOT_ELECT", "unranked", pick, "", std::nullopt, std::nullopt,
                             "lexical", "no candidate has an orbit");
    }
    net::mergeLetterMap(entities, &net::ConstellationState::elected_pivot, plan.clock_pivot);
}

}  // namespace

// ---- Solve stages (generated by scripts/nom_sg_cut.py) ----
/// The clock datum: the election, the constraint-realized clock rows, the handover verdict (Doxygen at the declaration).
EpochAction networkClockDatum(NetworkRun& run, const NetworkEpoch& ep, SolveDatum& datum) {
    // ---- From the run, the epoch and the solve's values (generated by
    //      scripts/nom_sg_cut.py, each line checked by scripts/nom_sg_verify.py) ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkSetupResult& setup = run.setup;
    NetworkEstimator& est = run.engines.est;
    const SyncedEpoch& se = ep.se;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    const double clk_anchor_sigma   = ctx.knobs.clk_anchor_sigma;     // D6-resolved anchor sigma (m)
    // The constellation pivots as the election reads them (one-letter key -> satellite).
    const std::map<std::string, std::string> prev_pivot =
        net::letterMap(ctx.entities.constellations, &net::ConstellationState::prev_pivot);
    const std::map<std::string, std::string> last_pivot =
        net::letterMap(ctx.entities.constellations, &net::ConstellationState::last_pivot);
    const bool   pivot_vr_signal = ctx.knobs.pivot_vr_signal;  // also signal a vanish-return pivot change (A1)
    const bool   sticky_pivot    = ctx.knobs.sticky_pivot;              // keep the incumbent clock pivot while it stays registered (RTA-1)
    // ---- End of the bindings ----
    // --- Datum + null-space gate + solve --------------------------------
    NetworkSbasis sb;
    const NetworkPivotElectionEpoch election(ctx, setup, se.time);
    // The declaration the election reads (one implementation, shared with the handover preview
    // before PASS B).
    const SbasisDeclaration decl = networkClockDatumDeclaration(ctx, config_, clk_anchor_sigma);

    NetworkSbasis::Plan& plan = datum.plan;   // written
    try {
        // In sticky mode the incumbent baseline is last_pivot, not prev_pivot.
        // prev_pivot is replaced wholesale at each emit, so a constellation absent
        // from the last emitted epoch drops out of it, and its return would
        // re-select a pivot spuriously (and, with the vanish-return arm off,
        // re-reference the datum without a signal).  last_pivot is merge-updated at
        // the same emit site, so per constellation it holds the pivot of the last
        // emitted product that contained it: the datum a consumer of that
        // constellation last adopted.  Both maps advance only at the emit site
        // (P0-1), so stickiness follows the datum of the last emitted product.  A
        // handover epoch that is then withheld leaves the incumbent unchanged, and if
        // the incumbent returns before any new-datum epoch emits, the datum does not
        // move (RTA-1).
        // Broadcast health (network.sat_health): the election passes over a registered satellite the
        // broadcast flags unhealthy at this epoch.  In "exclude" mode the setup door removed
        // such a satellite's observations, so none is registered and the set stays empty.
        // The coverage election (network.pivot_election) reads the same series.
        std::set<std::string> hlt_ineligible;
        const bool health_read = ctx.knobs.sat_health != 0 || ctx.knobs.pivot_election != 0;
        if (health_read) {
            const double t_s = TimeUtils::continuousGpsSeconds(se.time);
            for (const std::string& s : registered_clock_sats)
                if (setup.sat_health.unhealthyAt(s, t_s)) hlt_ineligible.insert(s);
            if (ctx.knobs.sat_health != 0)
                ctx.counters.hlt_unhealthy_registered += static_cast<long long>(hlt_ineligible.size());
        }
        plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                                   sticky_pivot ? &last_pivot : nullptr,
                                   health_read ? &hlt_ineligible : nullptr,
                                   election.get());
        if (ctx.knobs.sat_health != 0) {
            ctx.counters.hlt_pivot_skips     += plan.pivot_health_skips;
            ctx.counters.hlt_pivot_fallbacks += plan.pivot_health_fallbacks;
        }
        if (election.get()) notePivotElections(run, se.time, plan);
    } catch (const std::exception& ex) {
        log->warn("Network: buildSharedDatum failed ({}); skipping epoch", ex.what());
        return EpochAction::SkipEpoch;
    }

    // Constraint-realized clock datums: fold the gauge rows onto the shared border
    // before the null-space gate.  Sum-to-zero mode carries one Σδtˢ = 0 row per
    // constellation; broadcast-anchored mode carries one Δδtˢ → 0 anchor row per
    // satellite at weight 1/σ² (there the anchors are the datum).  In both modes plan
    // carries constraint rows instead of hard pins, and declared_dimension is 0.  The
    // rows are folded like the code-bias gauge (additive information; the within-epoch
    // re-solve re-applies them from plan, which is in scope there).  In pin mode plan
    // has 0 rows and this is a no-op (byte-identical).
    if (plan.clock_constraint_H.rows() > 0) {
        est.contributionKind(ContributionKind::ClockConstraintRows);
        est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                    plan.clock_constraint_weight);
    }

    // Pivot handover: the previous epoch's shared estimate is in the old datum, so a
    // changed clock pivot invalidates the D2 carry-forward (and forces a discontinuity
    // bump downstream).  Detect a clock-pivot datum re-reference and record which
    // constellations changed, for the A2 per-constellation emit-bump scoping (A1, A2).
    // Arm (b), vanish-return, is gated on network.pivot_vanish_return_signal; with it
    // off, detection is the legacy adjacent-epoch-only test.
    std::set<char>& changed_consts = datum.changed_consts;   // written
    datum.datum_changed = pivotHandoverSignalled(
        plan.clock_pivot, prev_pivot, last_pivot, pivot_vr_signal, changed_consts);
    return EpochAction::Proceed;
}
// ---- End of the solve stages ----

} // namespace GnssProcessor
