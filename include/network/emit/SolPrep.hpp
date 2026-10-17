#pragma once
/**
 * @file SolPrep.hpp
 * @brief Emit-side solution preparation and discontinuity signalling for the PPP-RTK
 *        network producer: the front half of the emit pipeline (the back half, store
 *        push + product write + closed-loop, stays in SB_EMITSTORE).
 *
 * Maps the post-fit shared estimate to a @ref NetworkSolutionEpoch and prepares it
 * for emission:
 *   - build @c sol via @c est.toSolutionEpoch; stamp per-sat C1 drift (two-state
 *     clock_model), generation time/id, and, in
 *     brdc_rts mode, the per-sat broadcast IODE (clock_iode);
 *   - erase record values the solve did not measure: pinned band-2 companion ISCs
 *     (RF-0f) and held extra-band columns (B5D);
 *   - RT0-5 clock-runaway quarantine + RTA-10 per-constellation σ̂0 carve-out, and
 *     the per-satellite GL-2 wire withhold and RF-3b declared-LOS withhold (all
 *     prune @c sol.sats);
 *   - B1 per-sat URA (+ RTB-7 cold-start quality ramp) and B1b per-(sat,signal)
 *     phase-bias σ;
 *   - datum-bump + PIVOT events, the retro-bump / eph-cutover / CB-excursion /
 *     clock-anchor / IF-pin-reselect / CB-reseed per-sat discontinuity landings
 *     (all emit-only, replay-faithful present/pending splits), the
 *     prev_pivot/last_pivot + emitted_anchor emit-baseline advance, the deferred v2
 *     contract-writer open, the M4 combined-correction σ (sigc/sigcc/rho), and the
 *     L14 satellite bias-state trace.
 *
 * Created at decomposition INC-11pre, which moved the SB_SOLPREP block unchanged out
 * of Application::runNetwork (dedent only, no substitution).  Owns no state: every
 * carry / emit-side object is an explicit reference into the caller's
 * NetworkRunContext / NetworkSetupResult.
 *
 * Crossing outputs: unlike the self-contained QC seams, this block produces five
 * locals that SB_EMITSTORE (and the epoch heartbeat) consume, returned in
 * @ref NetworkSolPrepResult: @c sol (emitToStore + product write), the withheld-
 * constellation set (heartbeat), the datum-bump serialization (global_bump +
 * serialized_consts, product write), and the applied retro-bump set (heartbeat).
 *
 * Ordering contract: runs on the post-fit @p x_s after SB_CLOCKUPD (whose
 * @c clock_osc drift it publishes as C1) and before SB_EMITSTORE
 * (which consumes the returned @ref NetworkSolPrepResult).  It mutates persistent
 * carries (the disc-pending sets, prev_pivot/last_pivot, emitted_anchor,
 * last_if_pin_sel, ura_retro_sats, the RTB-7 ramp state) and the emit-side objects
 * (@c emitter datum/disc bumps; opens @c product_writer at first emit).  The caller
 * keeps the @c networkSubBegin/End(SB_SOLPREP) brackets.
 */

#include "network/driver/SetupResult.hpp"        // NetworkSetupResult
#include "network/solve/Estimator.hpp"       // NetworkEstimator + PerConstSigma0
#include "network/solve/Sbasis.hpp"          // NetworkSbasis::Plan
#include "products/NetworkProductEmitter.hpp"// NetworkSolutionEpoch
#include "rinex/MultiRinexSync.hpp"          // SyncedEpoch

#include <Eigen/Core>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <cstddef>
#include <cstdint>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp, included in the .cpp only

/**
 * @brief The five SB_SOLPREP locals that SB_EMITSTORE and the heartbeat consume.
 *
 * Returned by value (moved out of the seam's body-locals); the caller re-aliases
 * each member so the unchanged downstream SB_EMITSTORE code reads the old names.
 */
struct NetworkSolPrepResult {
    NetworkSolutionEpoch     sol;                   ///< The prepared solution epoch (post prune/URA/σ stamps).
    std::set<char>           epoch_withheld_consts; ///< Constellations withheld by RT0-5 / RTA-10 (H heartbeat).
    bool                     global_bump = false;   ///< A global (non-scoped) datum bump this emit.
    std::set<char>           serialized_consts;     ///< The per-constellation datum-bump set (product write).
    std::set<std::string>    retro_emit_sats;       ///< Per-sat disc bumps that landed this emit: B1 retro plus the
                                                    ///< folded queue marks (eph-cutover / CB-excursion / clock-anchor /
                                                    ///< IF-pin reselect / CB-reseed), serialized as the R record
                                                    ///< (replay) + heartbeat.
};

struct SignalRoster;            // network/assembly/SignalRoster.hpp

/// @brief What one @ref networkWireFillModelledSignals pass added.
struct NetworkWireFillCounts {
    long long sats{0};    ///< Satellites that received at least one record.
    long long phase{0};   ///< Phase-bias records added.
    long long code{0};    ///< Code-bias records added.
};

/**
 * @brief Give every satellite with a clock record one bias per modelled signal
 *        (@c network.n3c_wire_per_signal).
 *
 * The signals are the roster's modelled phases (second phases included) and modelled
 * and companion codes on the satellite's constellation, taken over all stations. For a
 * modelled signal the satellite has no record for, the value is that of a record the
 * satellite does have on the same band digit and in the same channel family
 * (@c NetworkSbasis::channelFamilyOf, "X against the rest"); the lexically first such
 * record when several qualify. The whole phase-bias record is copied (value, integer
 * flags, sigma), a code bias copies the value. A signal with no such source stays
 * absent, so the wire never asserts a value across families or bands. Records are
 * added after every source has been read, so a filled record is never itself a source.
 *
 * @param sol    The solution epoch to complete (modified).
 * @param roster The signal roster of this epoch.
 * @return Counts of what was added.
 */
NetworkWireFillCounts networkWireFillModelledSignals(NetworkSolutionEpoch& sol,
                                                     const SignalRoster& roster);

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct EpochEvents;             // network/driver/Epoch.hpp

/**
 * @brief Prepare + stamp this epoch's emitted solution and land the emit-only
 *        discontinuity signals.  See the file banner for the full step list and the
 *        ordering contract.
 *
 * Epoch form; the explicit form and its forwarding adapter
 * retired (the unit tests call this form through
 * tests/support/NetworkTestRun.hpp).
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces), run.setup (setup). Read: run.config (config_), run.log
 *            (log), run.engines.est (est).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se), ep.x_emit()
 *           (x_s), ep.solve.plan (plan), ep.clock.pl_now_s (pl_now_s), ep.solve.datum_changed
 *           (datum_changed), ep.solve.changed_consts (changed_consts), ep.solve.sigma0_adapt
 *           (sigma0_adapt), ep.solve.perconst_active (perconst_active),
 *           ep.solve.perconst_publish (perconst_publish), ep.solve.perconst_dec_withhold
 *           (perconst_dec_withhold), ep.solve.perconst_stats (perconst_stats).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: [in/out] carry (disc-pending sets, pivots, anchors, URA/ramp state) + counters (iod, generation_id, n_emitted, runaway/perconst tallies) + knobs.  Passed whole so that a missing `&` cannot drop a carry/emit mutation; the body opens with an alias preamble copied from the former run-TU alias block.
 *   - @p config_ = run.config: networkEmitPhaseBiasSigma / networkEmitCombinedSigma + nc = getNetworkConfig() (warmup_ura_floor_m/epochs).
 *   - @p log = run.log: "main_log": generation-id / runaway / withhold / contract / anchor-handover lines.
 *   - @p traces = run.traces: events_trace + the emitEvent forwarder.
 *   - @p setup = run.setup: [in/out] emit-side objects: emitter (datum/disc bumps), product_writer (opened at first emit), prod_path, pending_contract, contract_pending, brdc_rts.
 *   - @p se = ep.se: Current synced epoch (content time / week / TOW).
 *   - @p est = run.engines.est: toSolutionEpoch / sharedCovariance / *Column / zeroPhaseGaugeColumns (all const).
 *   - @p x_s = ep.x_emit(): Post-fit shared solution vector.
 *   - @p plan = ep.solve.plan: Shared datum (pins) + per-constellation clock pivots.
 *   - @p pl_now_s = ep.clock.pl_now_s: Continuous-GPS seconds of this epoch (per-sat C1 Δt, ramp / contract timing).
 *   - @p datum_changed = ep.solve.datum_changed: Whether a pivot handover re-referenced the datum.
 *   - @p changed_consts = ep.solve.changed_consts: Constellations whose pivot changed (scoped bump).
 *   - @p sigma0_adapt = ep.solve.sigma0_adapt: This epoch's final within-epoch σ̂0 (0 = did not run).
 *   - @p perconst_active = ep.solve.perconst_active: A publishable per-constellation subset exists.
 *   - @p perconst_publish = ep.solve.perconst_publish: Constellations to keep in the emission.
 *   - @p perconst_dec_withhold = ep.solve.perconst_dec_withhold: Constellations the σ̂0 decision withheld.
 *   - @p perconst_stats = ep.solve.perconst_stats: Per-constellation σ̂0 stats (events / log).
 *
 * @return the five crossing locals (see @ref NetworkSolPrepResult).
 *
 * @param events The epoch's events; written: qc (rhs_trip_pending).
 */
NetworkSolPrepResult networkSolPrep(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: solution prep: the clock-runaway quarantine (RT0-5), the per-constellation
 *        sigma0 carve-out (RTA-10).
 *
 * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
 * beside it in SolPrep.cpp, print in this order, each gated on its own knob:
 *   - networkClockRunawayRunEnd: the clock-runaway quarantine (RT0-5).
 *   - networkPerConstellationRunEnd: the per-constellation sigma0 carve-out (RTA-10).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkSolPrepRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of SolPrep, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct SolPrepCounters {
    // Per-run solution-generation id, minted at the first emit from that epoch's GPS
    // time: deterministic under replay (the id is serialized per epoch), and different
    // across live restarts, which make their first emit later.  0 = unset.
    uint32_t generation_id = 0;
    // Product time (s) of the last written S contract line (the deferred v2 header
    // counts as the first); -1 until the contract writer opens.
    double last_contract_emit_s = -1.0;
    long long n_resume_datum_first_reattach = 0; ///< PHASE_DATUM/eq34_reattach (tree-edge handovers) there
    long long n_resume_datum_first_cb_disc  = 0; ///< CB_DATUM/reseed_disc_landed satellites there
    /// Emit-site outcome of the continued pair changes (emit_clock_disc
    /// only): re-selection discontinuities left undeclared for a continued satellite
    /// (its last emitted pair was the pair the builder continued from, and no re-seed
    /// was marked or pending); declared anyway (the last emitted pair differed, as for
    /// a satellite pruned from an earlier kept epoch's emission, or a re-seed was
    /// marked or pending); and continued satellites outside that epoch's emission,
    /// compared when they next emit (the conservative direction).
    size_t n_if_pin_continued_undeclared = 0, n_if_pin_continued_declared = 0,
           n_if_pin_continued_unemitted = 0;
    size_t n_perconst_partial_epochs = 0;     // epochs published partially: the σ̂0 carve-out pruned >=1 constellation (RTA-10)
    size_t n_perconst_diffuse_published = 0;  // epochs published in full although the global σ̂0 tripped, every constellation being individually clean: the diffuse cross-constellation edge of the per-const approximation (RTA-10)
    long long cb_exc_bumps           = 0;   ///< code-bias discontinuity bumps landed
    NetworkWireFillCounts wire_fill;        ///< Records added under network.n3c_wire_per_signal.
    long long wire_fill_epochs = 0;         ///< Emitted epochs on which the fill ran.
};

} // namespace GnssProcessor
