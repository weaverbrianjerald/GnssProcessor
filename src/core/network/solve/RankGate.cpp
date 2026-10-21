/**
 * @file RankGate.cpp
 * @brief Stages 5 and 8 of the network solve: the estimability dump and the
 *        null-space rank gate with the drop-and-heal; the gate's run-end report.
 */

#include "network/solve/RankGate.hpp"
#include "network/solve/SolveGlue.hpp"   // SolveDatum and the other typed values of the solve
#include "network/runend/RunEnd.hpp"     // NetworkRunEndView (the run-end report)

#include "network/state/RunContext.hpp"
#include "common/GnssIdentity.hpp"   // systemOfSat, letterOfSat, constellationKey, SignalId
#include "config/GnssConfig.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/solve/ChainDatum.hpp"
#include "network/solve/ClockAnchor.hpp"   // guarded temporal clock anchor
#include "network/carry/ClockClone.hpp"    // the one-epoch clock clone
#include "network/carry/SharedHistory.hpp" // the shared history
#include "network/solve/DatumGraph.hpp"   // datum graph census: the per-epoch spectral hand-off
#include "network/diag/FrameDiag.hpp"   // networkDatumInfoRatio
#include "network/solve/HandoverStransform.hpp"   // the clock-pivot handover S-transform
#include "network/solve/PhaseDatum.hpp"
#include "network/solve/PivotHandover.hpp"
#include "network/qc/PrefitCensus.hpp"   // networkPrefitSigma0, the census-fed H σ̂₀
#include "network/qc/Raim.hpp"           // RaimPoolAdmissionTally: the within-epoch pool's admission tally
#include "network/qc/Relevel.hpp"
#include "network/qc/RhsAttrib.hpp"   // flight-recorder trip latch and inline dump
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
#include "network/carry/DdPriorBuild.hpp"   // the replay's builders

namespace GnssProcessor {

// ---- Solve stages ----
// The prioritized phase-bias datum drop-and-heal (healNearNullPhaseBias) lives in
// network/solve/PhaseDatum, with the anchor election.  Its function
// Doxygen holds the rationale and the performance note: the full eigenvector
// decomposition runs on the gate-failure path only, never per epoch.

// Null-space rank gate.  The reduced shared normals are ill-conditioned by design:
// the phase rows pin the satellite-clock / phase-bias sum to about σ_phase
// precision (weight about 1e5), while only code separates the two (the
// clock-vs-bias split), so that split is orders of magnitude weaker but still
// non-zero.  Measured spectra show a clean gap of about 7 decades between the true
// gauge freedoms (the per-constellation clock datum, σ ~ 1e-10 of σ_max) and the
// weakest real direction (σ ~ 1e-2).  A relative tolerance of 1e-6 sits above the
// weak but real directions and miscounts them as deficiencies; 1e-9 lands in the
// GPS+Galileo gap, counting only the true gauges while still catching an unpinned
// datum.
//
// The gap moves with the constellation mix, so the tolerance is a per-config
// calibration parameter (network.rank_gate_tol), resolved once into
// est.setRankGateTol() and read here.  The estimability dump and the drop-and-heal
// read the same value: a private tolerance would make the dump describe a
// decision the gate did not make, or make the heal soft-anchor weak but real
// directions.  It defaults to NetworkEstimator::kNullSpaceTol (1e-9), so an unset
// key is byte-identical.  With BeiDou the default miscounts in the same way: the
// BDS weak-but-real family layer sits at about 1.7e-10 of λmax, about 3 decades
// deeper, with a measured six-decade gap down to the declared gauges.

/// The budgeted spectrum of the system the gate is about to judge (Doxygen at the declaration).
void networkEstimabilityDump(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum) {
    // ---- From the run, the epoch and the solve's values ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    const SyncedEpoch& se = ep.se;
    const std::vector<std::string>& col_desc = ep.columns.col_desc;
    const auto& nc = config_.getNetworkConfig();
    auto& spectrum_diag_budget = ctx.counters.spectrum_diag_budget;
    const NetworkSbasis::Plan& plan = datum.plan;
    // ---- End of the bindings ----
    // ---- Estimability diagnostic (default off) --------------------------
    // The spectrum dump is NetworkEstimator::dumpEstimabilitySpectrum, whose
    // Doxygen holds the derivation and the eigen-vs-SVD cost rationale.  The returned
    // struct holds the numbers the warn lines print, mirrored into
    // network_estimability.csv so analysis reads a CSV instead of parsing
    // log text.
    // The budget is re-armable: network.trace_estimability_budget sets how many
    // evaluations it may spend (default 6), and network.trace_estimability_deficit_only
    // spends them only on epochs whose census shows a deficit.  A fixed budget of 6 is
    // spent during initial convergence on a field run, leaving later withhold stretches
    // without a spectrum.  The budget is decremented after the dump, not before, so a clean epoch
    // skipped under deficit_only does not consume it.  Otherwise every epoch that
    // reaches this block decrements it, whether or not the dump is armed, and nothing
    // between the two points reads the counter.  The first-N code-bias datum log
    // (networkCodeBiasDatum, an earlier stage) is gated on the same budget.
    if (spectrum_diag_budget > 0) {
        bool spend = true;
        if (nc.trace_estimability) {
            const auto dump = est.dumpEstimabilitySpectrum(
                se.time.tow, plan.declared_dimension, col_desc, log);
            if (dump.valid) {
                // The census has to run to tell whether this epoch is in deficit
                // (there is no cheaper predicate for "n_below > declared"), so the
                // eigen-decomposition cost is paid on every budgeted epoch either
                // way; deficit_only saves the budget and the rows.
                if (nc.trace_estimability_deficit_only
                    && dump.n_below <= dump.declared_dim) {
                    spend = false;
                } else {
                    traces.writeEstimabilityEpochRow(se.time, dump.tow, dump.n_shared,
                                                     dump.declared_dim, dump.ev_max,
                                                     dump.cutoff, dump.n_below);
                    for (const auto& d : dump.dirs)
                        traces.writeEstimabilityDirRow(se.time, d.ev, d.top);
                }
            }
        }
        if (spend) --spectrum_diag_budget;
    }
}

/// The null-space rank gate with the prioritized drop-and-heal and one re-gate (Doxygen at the declaration).
EpochAction networkRankGate(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                            EpochWorkspace& ws) {
    // ---- From the run, the epoch and the solve's values ----
    NetworkRunContext& ctx = run.ctx;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    const SyncedEpoch& se = ep.se;
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    const bool   within_epoch_armed = ctx.knobs.within_epoch_armed;   // network.raim_within_epoch
    auto& n_gate_withheld = ctx.counters.n_gate_withheld;
    auto& n_healed_epochs = ctx.counters.n_healed_epochs;
    auto& n_healed_cols = ctx.counters.n_healed_cols;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    auto& cap_priors = ws.cap_priors;
    const NetworkSbasis::Plan& plan = datum.plan;
    NetworkSbasis sb;
    // ---- End of the bindings ----
    bool gate_ok = true;
    // The observed near-null count is captured on both the passing and the throwing
    // path, because the exception text cannot be parsed back into a number.
    // "Under-specified" (observed > declared: a missing datum constraint) and
    // "over-specified" (observed < declared: a tolerance below this config's gauge
    // depth, a mis-calibration that withholds a sound config) need opposite fixes, so
    // they are counted separately.
    int gate_observed       = -1;   // the last evaluation this epoch (after the heal if a re-gate ran)
    int gate_first_observed = -1;   // this epoch's first evaluation: the min/max sample
    double gate_ev_max      = -1.0; // λmax of the first evaluation, which sets the gate's own cutoff
    ++ctx.counters.n_gate_evaluated;
    // With clock clones registered, the gate judges the border with the clones
    // marginalized and the idle ones dropped (network/carry/ClockClone): a clone has a prior
    // and coupling rows, not observations, and an idle clone is not a gauge.  The view is
    // re-formed at every evaluation (the heal below folds priors first).  Off, no view is
    // formed and every evaluation reads the reduced normals as before.
    Eigen::MatrixXd ccl_gate_N;
    bool ccl_gate = networkClockCloneGateView(ctx, est, ccl_gate_N);
    try {
        const Eigen::MatrixXd* gate_N = ccl_gate ? &ccl_gate_N : &est.reducedSharedNormals();
        sb.assertNullSpaceRank(*gate_N, plan.declared_dimension,
                               est.rankGateTol(), &gate_observed, &gate_ev_max);
        gate_first_observed = gate_observed;
    } catch (const std::exception& ex) {
        // assertNullSpaceRank writes observed_out before the equality test, so the
        // count that caused this throw is already in gate_observed; latch it now,
        // before a possible re-gate overwrites it.
        gate_first_observed = gate_observed;
        // The near-null count differs from the declared dimension.  Typically the
        // reduced shared system carries more gauge freedoms than the declared datum
        // can pin: a satellite that is momentarily unobservable (seen by a single
        // station at marginal geometry) or, under the prioritized datum, a
        // phase-bias column whose anchor was dropped or whose separation is weak.
        // In the prioritized case, soft-anchor those near-null phase-bias columns and
        // gate again before withholding (the eigenvector heal runs only here, not
        // every epoch).  If the datum is still deficient after the heal (or on the
        // legacy path), the product would be an arbitrary least-norm split, so
        // withhold.
        bool recovered = false;
        // Per-healed-column attribution for the trace_events rows below.
        // Declared outside the inner try so the rows are still emitted when the
        // re-gate, not the heal, throws: a heal that fired and did not recover the
        // epoch is the case these rows exist for.
        std::vector<PhaseHealEvent> heal_events;
        int healed_n = 0;   // > 0 only once the heal has returned (its priors are folded)
        // The heal / re-gate failure text is captured rather than logged in place, so
        // the single withhold warning below carries the whole user-visible report.
        // It stays empty when the inner try returns without throwing, which
        // includes healed == 0: the re-gate is skipped, nothing in the block can
        // throw, and the epoch reaches the withhold with no exception of its own.
        std::string heal_fail_what;
        try {
            const int healed = healNearNullPhaseBias(
                est, shared_phase_keys, phase_datum_prioritized,
                within_epoch_armed, cap_priors,    // 0 unless prioritized + near-null PB
                &heal_events, &ws.cap_prior_kinds);
            healed_n = healed;
            // Count the attempt before the re-gate can throw: the success-only pair
            // below cannot tell "the heal never ran" from "the heal ran every epoch
            // and never recovered the gate".
            if (healed > 0) {
                ++ctx.counters.n_heal_attempt_epochs;
                ctx.counters.n_heal_attempt_cols += static_cast<size_t>(healed);
            }
            if (healed > 0) {
                // The view again, on the healed normals (off: not formed).
                ccl_gate = networkClockCloneGateView(ctx, est, ccl_gate_N);
                const Eigen::MatrixXd* heal_N = ccl_gate ? &ccl_gate_N : &est.reducedSharedNormals();
                sb.assertNullSpaceRank(*heal_N,
                                       plan.declared_dimension,
                                       est.rankGateTol(), &gate_observed);
                recovered = true;
                ++n_healed_epochs;
                n_healed_cols += static_cast<size_t>(healed);
                log->debug("Network: TOW {:.1f} soft-anchored {} near-null phase-bias "
                           "column(s) (dropped anchor / weak separation) — gate recovered.",
                           se.time.tow, healed);
            }
        } catch (const std::exception& ex2) {
            // The heal (eigensolve / addSharedPrior) or the re-gate reported a
            // deficiency it could not resolve: withhold rather than crash.  The report
            // is deferred to the single warning on the !recovered path, because this
            // catch is not the only route to a withhold; a warning emitted only here
            // would leave the other routes visible at debug level alone.
            heal_fail_what = ex2.what();
        }
        // One row per soft-anchored column, with no station because the heal
        // acts on a shared column.  statistic is the eigenvalue of the near-null
        // direction the column spans most heavily, threshold is that |component|, and
        // action records whether the heal recovered the epoch, so an attributable but
        // ineffective heal can be told from a working one.  Gated on healed_n > 0 so
        // a heal that threw mid-fold cannot emit rows claiming anchored columns when
        // no prior was installed; the re-gate throw (healed_n > 0, recovered false)
        // still emits its rows.  No-op unless network.trace_events.
        if (healed_n > 0) {
            for (const auto& h : heal_events)
                emitEvent("GATE", "phase_heal", h.sat, "", h.ev, h.span,
                          recovered ? "soft_anchor" : "soft_anchor_insufficient",
                          h.signal);
        }
        if (!recovered) {
            gate_ok = false;
            ++n_gate_withheld;
            // Split the withhold by direction.  observed > declared is a missing
            // datum constraint; observed < declared means the
            // tolerance sits below this config's own gauge depth, so the gauges are no
            // longer counted: a mis-calibration withholding a sound config, which no
            // datum work will fix.  gate_observed is what the last gate evaluation of
            // this epoch measured; an unknown count (< 0) is counted as under.
            if (gate_observed >= 0 && gate_observed < plan.declared_dimension)
                ++ctx.counters.n_gate_withheld_over;
            else
                ++ctx.counters.n_gate_withheld_under;
            // One warning line on every withhold, whichever route reached it,
            // reporting both gate evaluations.  A heal that anchors 0 columns
            // (a machine-zero orphan direction with no phase-bias span) skips the
            // re-gate, so the inner catch never runs; this line still reports the
            // withhold.  The count reported first is the first evaluation's
            // (gate_first_observed; ex.what() is also from the first evaluation), not
            // the re-gate's, which measures only the deficiency left after the heal's
            // soft anchors and is reported after it.  This line and the gate_withheld
            // CSV row below therefore report the same count.
            const std::string regate_part =
                (healed_n > 0) ? fmt::format(", re-gate observed {}", gate_observed)
                               : std::string(" (no re-gate ran)");
            log->warn("Network: observability gate at TOW {:.1f} withheld product — "
                      "first gate observed {}, declared {}; heal anchored {} column(s){}."
                      "  First gate: {}{}",
                      se.time.tow, gate_first_observed, plan.declared_dimension,
                      healed_n, regate_part, ex.what(),
                      heal_fail_what.empty()
                          ? std::string{}
                          : fmt::format("  drop-and-heal failed: {}", heal_fail_what));
            emitEvent("GATE", "gate_withheld", "", "", std::nullopt, std::nullopt,
                      "skip_epoch", ex.what());
        }
    }
    // Run-wide envelope of the observed near-null count, one sample per epoch from the
    // first gate evaluation; a re-gate after the heal answers a different
    // question and is counted by n_healed_epochs.  The envelope audits the tolerance's
    // placement: a run whose observed count never leaves {declared} needs
    // no calibration.
    if (gate_first_observed >= 0) {
        auto& lo = ctx.counters.nullspace_obs_min;
        auto& hi = ctx.counters.nullspace_obs_max;
        if (lo < 0 || gate_first_observed < lo) lo = gate_first_observed;
        if (hi < 0 || gate_first_observed > hi) hi = gate_first_observed;
    }
    // Measure how much information each declared datum direction carries, on the same
    // normals and cutoff the gate just judged.  The gate's integer count cannot tell "the tolerance
    // is misplaced" from "a carry filled a gauge direction", and the two need opposite
    // fixes; this ratio tells them apart, so a filled gauge direction is reported.
    // Measure-only, unconditional, about one k×k quadratic form per constellation.
    // Runs after the withhold accounting above so the coincidence counter (an
    // over-specified withhold on a breached epoch) is formed here in one place.
    networkDatumInfoRatio(ctx, traces, se, est, plan.declared_dimension,
                          registered_clock_sats, gate_ev_max,
                          /*gate_over_withheld=*/!gate_ok && gate_observed >= 0
                              && gate_observed < plan.declared_dimension,
                          ccl_gate ? &ccl_gate_N : nullptr);   // the gate's own view
    // Datum graph census: the fallback spectral hand-off, for runs without S-basis completion (SBC).  The
    // gate's own first evaluation is then the only realized near-null count, a count
    // with no direction identities, which the census labels `obs_src=count` rather
    // than attributing it.  An SBC capture is not overwritten: with the completion
    // armed, the gate inspects the completed system and reads back the declared
    // dimension, so its count would report the completion's success instead of the
    // deficiency the census reconciles against.
    if (ctx.knobs.dg_on) {
        auto& dgs = ctx.carry.dg_spec;
        if (!dgs.from_sbc && gate_first_observed >= 0) {
            dgs.valid    = true;
            dgs.observed = gate_first_observed;
            dgs.declared = plan.declared_dimension;
        }
    }
    return gate_ok ? EpochAction::Proceed : EpochAction::SkipEpoch;
}
// ---- End of the solve stages ----

// ---- Run-end report ----

// Run-end report: the observability rank gate and the datum-direction ratio.
int networkRankGateRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const auto& n_emitted = v.ctx.counters.n_emitted;

    // Rank-gate calibration telemetry.  Printed only when the tolerance is set
    // to a non-default value, so a default run's log text is byte-identical.  The same
    // numbers reach every trace-armed run through the RUN record of network_estimability.csv, so
    // the control arm still gets them.  An armed calibration whose counters never move is
    // a miswire, not a quiet run.
    if (config_.networkRankGateTol() != NetworkEstimator::kNullSpaceTol)
        log->info("Network (datum graph gate): rank gate evaluated on {} epoch(s) at tol={:g} — {} "
                  "emitted, {} withheld UNDER-specified (observed > declared: a missing "
                  "datum constraint), {} withheld OVER-specified (observed < declared: a "
                  "declared gauge stopped being counted).  Observed near-null count over "
                  "the run: min {}, max {} (declared dim is the target).",
                  ctx.counters.n_gate_evaluated, config_.networkRankGateTol(),
                  n_emitted, ctx.counters.n_gate_withheld_under,
                  ctx.counters.n_gate_withheld_over,
                  ctx.counters.nullspace_obs_min, ctx.counters.nullspace_obs_max);
    // An over-specified withhold has two causes with opposite fixes.  A tolerance set
    // below the config's own gauge depth is fixed by re-calibrating.  A pre-gate fold that
    // deposits information along a declared gauge direction moves that direction up into
    // the real-parameter layer, and no tolerance recovers it: a cutoff below the filled
    // value still does not count it as gauge, and a cutoff above it starts counting real
    // parameter directions as gauge.  (On a simulated 24-station network: gauge depth 1e-11,
    // carry-filled direction 9.43e-3, weakest real phase-bias layer 1.25e-2, so no cutoff
    // meets the placement rule's 2 decades on either side.)  The datum-direction information
    // ratio separates the two cases, so the advice below branches on it.
    if (ctx.counters.n_gate_withheld_over > 0) {
        if (ctx.counters.ddi_over_breach > 0)
            log->warn("Network (datum graph gate): {} of the {} OVER-specified withhold(s) landed on an "
                      "epoch where a DECLARED datum direction was already carrying information "
                      "(invariant-#7 ratio >= 1; run max R={:.3e} on {} at TOW {:.1f}).  Those "
                      "are NOT a mis-calibrated tolerance and re-calibrating cannot fix them: "
                      "the direction has moved up into the real-parameter layer, so every "
                      "cutoff below it still fails to count it as gauge and every cutoff above "
                      "it starts counting real directions as gauge.  ATTRIBUTE THE FOLD "
                      "instead.  ⚠ The ratio is measured on the reduced normals AS THE GATE "
                      "SEES THEM, i.e. AFTER the station blocks (which carry the "
                      "coupling rows), the clock-constraint rows, the SH prior, "
                      "the pin bridge and any SBC rows — but BEFORE the D2/C1 dense carry fold "
                      "and the clock-osc prior, which are applied later and are therefore NOT "
                      "observable in this number.  So the candidates it CAN indict are the "
                      "coupling rows (a truncated family sum — arm "
                      "network.carry_coupling_gauge_project) and the pre-gate datum/constraint "
                      "rows.  The ddi_breach events in network_events.csv name the direction and "
                      "the epoch.",
                      ctx.counters.ddi_over_breach, ctx.counters.n_gate_withheld_over,
                      ctx.counters.ddi_ratio_max, ctx.counters.ddi_ratio_max_dir,
                      ctx.counters.ddi_ratio_max_tow);
        if (ctx.counters.n_gate_withheld_over > ctx.counters.ddi_over_breach)
            log->info("Network (datum graph gate): {} OVER-specified withhold(s) occurred with every "
                      "declared datum direction still free (invariant-#7 ratio < 1).  THOSE are "
                      "the calibration case — the tolerance sits below this config's own gauge "
                      "depth, so RE-CALIBRATE rather than chasing the datum.  [Placement rule: "
                      "cutoff >= 2 decades above the gauge depth and >= 2 decades below the "
                      "weakest-real layer, read off this config's own network_estimability.csv.]",
                      ctx.counters.n_gate_withheld_over - ctx.counters.ddi_over_breach);
    }
    // The ratio's own readout, independent of whether the gate ever withheld: a run
    // whose declared directions all sit decades below the cutoff has a genuinely free
    // datum, and that is worth stating positively rather than inferring from silence.
    if (ctx.counters.ddi_epochs > 0 && ctx.counters.ddi_dirs == 0)
        log->warn("Network (invariant #7): the ratio was reached on {} epoch(s) but sampled "
                  "ZERO directions — every declared group was below two registered clocks, or "
                  "its columns fell outside the reduced normals.  The instrument is INERT: "
                  "'0 breaches' says nothing about this run's datum.",
                  ctx.counters.ddi_epochs);
    if (ctx.counters.ddi_epochs > 0 && ctx.counters.ddi_dirs > 0)
        log->info("Network (invariant #7): datum-direction information ratio measured on {} "
                  "epoch(s), {} (epoch x direction) samples — R range [{:.3e}, {:.3e}], max on "
                  "{} at TOW {:.1f}; {} epoch(s) breached R >= 1 across {} edge(s).  R << 1 "
                  "means the datum still owns its gauge directions; R >= 1 means something "
                  "folded PRE-gate is pinning one.",
                  ctx.counters.ddi_epochs, ctx.counters.ddi_dirs,
                  ctx.counters.ddi_ratio_min, ctx.counters.ddi_ratio_max,
                  ctx.counters.ddi_ratio_max_dir.empty() ? "(none)"
                                                         : ctx.counters.ddi_ratio_max_dir,
                  ctx.counters.ddi_ratio_max_tow,
                  ctx.counters.ddi_breach_epochs, ctx.counters.ddi_breach_edges);
    return 0;
}

} // namespace GnssProcessor
