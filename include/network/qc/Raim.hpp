#pragma once
/**
 * @file Raim.hpp
 * @brief Post-fit network RAIM (Receiver Autonomous Integrity Monitoring) for the
 *        PPP-RTK network producer.
 *
 * After the joint network solve, each station is reconstructed once and its post-fit
 * carrier-phase residuals are standardized by a robust network-wide a-posteriori
 * variance factor σ̂0.  Two screens then look for slips the pre-processor missed:
 *  - Consensus: a satellite whose normalized residual spikes at
 *    @c network.raim_min_stations or more stations has a slip leaking into its shared
 *    clock.  Its carried ambiguities are refloated network-wide.
 *  - Attribution (@c network.raim_attribution): a robust two-way median polish of the
 *    (station x satellite) residual table assigns the misfit to a bad satellite (column
 *    effect), a bad station (row effect, never acted on; see Raim.cpp) or an isolated
 *    arc (interaction).  When on, the attribution drives the refloat instead of the
 *    consensus.
 *
 * σ̂0 is also folded into an exponentially weighted moving average (EWMA) that scales
 * the per-satellite URA (user range accuracy) quality channel.  The satellites the
 * screens flag feed the retro-bump, when armed (the consumer resets the ambiguity that
 * integrated the contaminated epoch), and, with
 * @c network.clock_monitor_couple_raim, the clock refloat.
 *
 * This file covers only the post-fit pass.  The within-epoch RAIM adaptation (the
 * σ̂0-triggered culprit drop and re-solve, and the per-constellation withhold) runs
 * earlier, inside the solve (RaimWithinEpoch.hpp).  The pass owns no state: every carry
 * object is a reference into the run's NetworkRunContext.
 *
 * Ordering contract.  networkPostFitRaim mutates persistent and per-epoch state:
 *  - It erases entries of @c ctx.entities.arcs and marks the matching parameter-set
 *    ambiguities pending-reseed, for every refloated (station, satellite, signal).
 *  - It inserts consensus and whole-satellite culprits into @c clock_refloat_sats and
 *    @c retro_flag_sats of the post-fit hand-off.
 *  - It folds σ̂0 into @c ctx.carry.ura_sigma0_ewma (read one epoch later at the emit
 *    site) and advances @c net::LosState::arc_spike_streak and @c n_arc_refloat_epoch.
 *  - With @c network.raim_refloat_maturity_gate on it writes the gate's run-scope telemetry
 *    (@c ctx.counters.maturity_gate, the RAIM_MATURITY event rows and the lift line), which
 *    nothing in the solve reads.
 *  - With @c network.raim_prior_channel on (network/qc/RaimPriorChannel.hpp) the carried-prior
 *    rows leave the pools and are tested on their own channel: a line-of-sight flag takes the
 *    arc refloat above, and a receiver code-bias flag erases that carried code bias
 *    (@c net::StationSignalState::rx_cb of @c ctx.entities.stations), so the next epoch folds
 *    the column without its carry.  The tally (@c ctx.counters.prior_channel) and the
 *    @c T4_prior event rows are telemetry.
 *
 * It therefore runs:
 *  - after the joint solve and the carry harvest: it operates on the post-fit @c x_s
 *    and the freshly stashed carry, and erasing an entry is the refloat;
 *  - before networkClockQc, the consumer of the two culprit sets, and before the epoch
 *    stash/status tail.
 *
 * The caller brackets the call with networkSubBegin/networkSubEnd(SB_RAIM) and the
 * arming guard `if (raim_on && (carry_warm || ctx.knobs.raim_ci_on))`.  The function is
 * the guarded body.
 */

#include "network/assembly/Contributions.hpp"  // ContributionKind, kContributionKinds (RaimPoolAdmissionTally)
#include "network/solve/Estimator.hpp"      // NetworkEstimator + NetworkEstimator::Datum
#include "network/qc/PriorResidual.hpp"  // NetworkPriorResidual, the data-side feed of the global σ̂0
#include "network/driver/RunTypes.hpp"       // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"         // MultiRinexSync::SyncedEpoch

#include <Eigen/Core>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp, included in the .cpp only

/**
 * @brief Run-end telemetry of the RAIM pool re-host.
 *
 * The re-host (@c network.raim_carry_invariant) builds the post-fit RAIM pool from a
 * carry-neutral census of ambiguity columns (@c qc_amb) instead of the phase-leveling
 * carry's list, so the screen also runs on invariant-carry runs.  The "ci" in the
 * names stands for carry-invariant.
 *
 * The counters tell apart four outcomes that would otherwise all read as "RAIM found
 * nothing":
 *  - @c epochs == 0: networkPostFitRaim never ran (call-site gate closed).
 *  - @c epochs > 0 and @c pool_cells == 0: it ran and the census was empty (dead
 *    wiring, the defect the re-host fixes).
 *  - @c pool_cells > 0 and @c sats_flagged == 0: it screened real residuals and nothing
 *    was bad (no opportunity, which is not the same as no capability).
 *  - @c sats_flagged > 0: it flagged.  The re-host is measure-only, so no carry
 *    consumer acted on the flag.
 *
 * Every member stays at zero unless @c ctx.knobs.raim_ci_on, and the run-end readout is
 * written only then, so an off run reports nothing.
 */
struct NetworkRaimCiTallies {
    long long epochs = 0;            ///< Post-fit RAIM passes with the re-hosted pool armed.
    long long pool_stations = 0;     ///< Station-epochs whose qc_amb census was non-empty.
    long long pool_cells = 0;        ///< Total carried-ambiguity cells screened (the census size).
    long long sats_flagged = 0;      ///< Consensus-flagged satellite-epochs.
    long long arcs_flagged = 0;      ///< Attribution-flagged isolated (station,sat) arc-epochs.
    double    sigma0_max = 0.0;      ///< Largest σ̂0 seen over epochs with a non-empty pool.
    double    sigma0_sum = 0.0;      ///< Σ σ̂0 over epochs with a non-empty pool (mean = sigma0_sum / sigma0_n).
    long long sigma0_n = 0;          ///< Epochs contributing to sigma0_sum.
};

/**
 * @brief Admissions of one carried-phase RAIM pool over a run, per contribution kind.
 *
 * Which row kinds a pool admits is decided by the row classifier
 * (@c raimPoolAdmitsKind, network/assembly/RowClassify.hpp).  This struct counts what
 * the rule admitted.  The post-fit pool (@ref networkPostFitRaim) and the within-epoch
 * assess pool (RaimWithinEpoch.cpp) each keep one in @c NetworkRunContext::counters,
 * printed at run end by networkRaimPoolRunEnd.
 *
 * A pool walks the rows of each station once per ambiguity column of its pool, so a row
 * whose coefficient clears 0.5 on two columns is visited twice and enters the pool's
 * sample twice.
 *  - @c visits counts every admitted (row, column) pair.
 *  - @c rows counts each admitted row once per station-epoch walk.
 *  - @c refused counts the (row, column) visits whose coefficient cleared 0.5 but whose
 *    kind the rule does not admit, by kind (@c Unset in the last slot).  Zero is
 *    expected.  A non-zero count is a row that the coefficient test alone would have
 *    admitted.
 */
struct RaimPoolAdmissionTally {
    long long walks{0};  ///< Pool walks: post-fit epochs, or within-epoch assess calls.
    std::array<long long, kContributionKinds + 1> visits{};   ///< Admitted (row, column) visits, by kind.
    std::array<long long, kContributionKinds + 1> rows{};     ///< Admitted rows, each once per station-epoch walk, by kind.
    std::array<long long, kContributionKinds + 1> refused{};  ///< Visits refused by the kind rule, by kind (@c Unset last).
    /// Data-rows-only pool (@c network.raim_pool_data_rows_only on): (observation phase row, column)
    /// pairs whose coefficient exceeds 0.5 on a column the row's tag does not name.  Each
    /// is also a refused visit.  0 expected: a phase row carries +1 on its own column only.
    long long own_col_cleared_other{0};
    /// Data-rows-only pool, knob on: (observation phase row, column) pairs whose tag names the column
    /// while the coefficient is at or below 0.5 (admitted by the tag).  0 expected.
    long long own_col_not_cleared{0};
    /// Data-rows-only pool, knob on: (observation phase row, column) pairs where the tag and the
    /// coefficient test agree that the column is the row's own (the admitted phase visits).
    long long own_col_agree{0};

    /**
     * @brief Count one (row, column) visit that cleared 0.5 (or, with the data-rows-only pool knob on,
     *        that the row's tag admitted).
     * @param k        The row's kind.
     * @param admitted Whether the pool's rule (@ref raimPoolAdmitsRow) admitted it.
     * @param first    Whether this is the row's first admitted visit of this station's walk.
     */
    void note(ContributionKind k, bool admitted, bool first);

    /**
     * @brief The own-column check for one (row, column) pair, knob on: does the
     *        column the row's tag names equal the column on which its coefficient exceeds
     *        0.5?  Counts observation phase rows only; every other kind is ignored.
     * @param k      The row's kind.
     * @param clears Whether the row's coefficient on the column exceeds 0.5 in magnitude.
     * @param names  Whether the row's tag names the column (@ref rowTagNamesColumn).
     */
    void noteOwnColumn(ContributionKind k, bool clears, bool names);

    /**
     * @brief The run-end text: the admitted kinds with their visits and rows, the refused
     *        visits by kind.
     * @return One line, without a prefix: "walks N; admitted: <kind> visits V rows R, ...;
     *         refused by kind: none" (or the refused kinds with their visits).
     */
    std::string text() const;

    /**
     * @brief The run-end text of the own-column check (knob on).
     * @return "own column: tag and coefficient agree A, coefficient > 0.5 on another
     *         column B, tag's column at or below 0.5 C".
     */
    std::string ownColumnText() const;
};

/// One isolated-arc flag that the refloat maturity gate held out of the refloat.
struct RaimHeldArc {
    int         station{0};      ///< Roster index of the arc's station.
    std::string sat;             ///< The arc's satellite.
    double      statistic{0.0};  ///< The flag statistic: the arc's median-polish interaction over sigma0, above @c network.raim_k_sigma.
};

/**
 * @brief The isolated-arc scan of the T4 attribution: which flagged arcs the gates let
 *        through to the refloat, and which the maturity gate held.
 *
 * An isolated arc is a (station, satellite) cell of the median-polish table whose
 * interaction stands above @c network.raim_k_sigma times sigma0 while its satellite is not
 * caught wholesale (the whole-satellite refloat takes those, ungated).  A flag meets two
 * gates in this order:
 *  1. the maturity gate (@c network.raim_refloat_maturity_gate): until the run is mature
 *     the flag is held, which leaves the arc's stiff carry in the solve.  A held flag also
 *     resets the arc's persistence streak (it is absent from @c next_streak);
 *  2. the persistence gate (@c network.raim_arc_persist_epochs, K): with K > 1 an arc is
 *     refloated only on its K-th consecutive flagged epoch.  K = 1 refloats at once.
 *
 * Both gates open by default (@c mature true, K = 1), where every flag is refloated and
 * @c held and @c next_streak stay empty.
 */
struct RaimIsolatedArcs {
    /// The arcs T4 refloats this epoch, in table order: every flag that passed both gates.
    std::vector<std::pair<int, std::string>> refloat;
    /// The flags the maturity gate held, in table order (empty with the gate open or off).
    std::vector<RaimHeldArc> held;
    /// The persistence streaks to carry to the next epoch (K > 1 only): the arcs flagged
    /// this epoch that have not yet persisted K epochs.  An arc absent here restarts at 0.
    std::map<std::pair<int, std::string>, int> next_streak;
    /// Flags that reached the maturity gate: over the bar, their satellite not caught
    /// wholesale.  The gate's denominator: @c held.size() of them were held, the rest passed.
    std::size_t flagged{0};

    /**
     * @brief Whether T4 refloats the arc (@p station, @p sat) this epoch.
     * @return True for an arc in @c refloat; false for an arc held by either gate or not
     *         flagged.  The `acted` column of an ARC row of network_raim_attrib.csv reads this.
     */
    bool refloats(int station, const std::string& sat) const;
};

/**
 * @brief Select the isolated arcs T4 refloats from the median-polish interactions.
 *
 * A pure function of its arguments: the order of the tests is the T4 attribution block's
 * order (bar, wholesale satellite, maturity gate, persistence gate), so the unit tests pin
 * the seam's behaviour.  See @ref RaimIsolatedArcs for the gates.
 *
 * @param cells          The (station, satellite, value) table the median polish was taken over.
 * @param interaction    The polish's interaction per cell (parallel to @p cells).
 * @param sigma0         The epoch's robust a-posteriori variance factor.
 * @param k_sigma        The flag bar, in sigma0 (@c network.raim_k_sigma).
 * @param caught_sats    Satellites already caught wholesale this epoch (no double attribution).
 * @param mature         Whether the maturity gate is open (true with the gate off).
 * @param persist_epochs The persistence gate K (@c network.raim_arc_persist_epochs; 1 = off).
 * @param prior_streak   The arc's consecutive-flag count before this epoch (read for K > 1
 *                       on an arc that passed the maturity gate only).
 * @return The arcs to refloat, the flags held, and the persistence streaks to carry.
 */
RaimIsolatedArcs raimSelectIsolatedArcs(
    const std::vector<std::tuple<int, std::string, double>>& cells,
    const std::vector<double>& interaction, double sigma0, double k_sigma,
    const std::set<std::string>& caught_sats, bool mature, int persist_epochs,
    const std::function<int(int, const std::string&)>& prior_streak);

/// Where in the run an isolated-arc scan of the maturity gate happened.
struct RaimScanStamp {
    long long epoch{0};          ///< Processed-epoch index (RaimCounters::n_loop_epochs).
    int       week{0};           ///< GPS week of the epoch.
    double    tow{0.0};          ///< GPS time of week of the epoch, seconds.
    long long n_emitted{0};      ///< Epochs emitted before the scan: the horizon's counter.
    long long ewma_updates{0};   ///< sigma0 EWMA updates received before the scan: the horizon's second term.
};

/**
 * @brief Run-scope telemetry of the isolated-arc refloat maturity gate
 *        (@c network.raim_refloat_maturity_gate).
 *
 * The gate keeps a flagged arc's stiff carry in the solve until the run has emitted
 * max(horizon, carry warm-up) epochs and the sigma0 EWMA has received horizon updates, so it
 * changes which information reaches the carried ambiguities and has two edges (standing
 * invariant 7), both counted here:
 *  - decoupling: each held flag is an arc-epoch on which T4 would have dropped a carry and
 *    did not (@c held, on @c held_epochs epochs; the identities are the RAIM_MATURITY
 *    @c arc_held rows of network_events.csv);
 *  - re-coupling: the lift, the first scan with the gate open (@c lifted and the @c lift_*
 *    stamp; one RAIM_MATURITY @c lift event row).  From the lift T4 acts on every flag again,
 *    so a persistent misfit is refloated there and a warm-up transient is not.
 *
 * The denominators tell apart the outcomes that otherwise read alike:
 *  - @c scans == 0: the gate was never consulted (RAIM never ran, attribution does not drive
 *    the action, or the cell table was empty throughout);
 *  - @c scans > 0 and @c lifted false: the run ended inside the horizon, so every flag of the
 *    run (@c flags) was held;
 *  - @c lifted with @c held == 0 and @c lift_epoch equal to the first scan: the gate was open
 *    from the start and changed nothing;
 *  - @c flags == 0: the gate had no opportunity.
 *
 * Every member stays at zero unless the gate is armed and the T4 attribution path ran, and
 * nothing in the solve reads it.
 */
struct RaimMaturityGateTally {
    long long scans{0};              ///< Isolated-arc scans made with the gate armed: the opportunity count.
    long long flags{0};              ///< Isolated-arc flags that reached the gate, held or passed.
    long long held{0};               ///< Flags held by the closed gate: arc-epochs whose refloat did not run.
    long long held_epochs{0};        ///< Scans with at least one held flag.
    long long first_held_epoch{-1};  ///< Processed-epoch index of the first scan that held a flag (-1: none).
    long long last_held_epoch{-1};   ///< Processed-epoch index of the last scan that held a flag (-1: none).
    long long first_scan_epoch{-1};  ///< Processed-epoch index of the first scan (-1: none).
    bool      lifted{false};         ///< The gate opened during the run.
    RaimScanStamp lift;              ///< The scan that lifted the gate (meaningful when @c lifted).

    /**
     * @brief Record one isolated-arc scan made with the gate armed.
     * @param n_flagged Flags that reached the gate (RaimIsolatedArcs::flagged).
     * @param n_held    Flags the gate held (RaimIsolatedArcs::held.size()).
     * @param mature    The gate's verdict this scan.
     * @param at        Where in the run the scan happened.
     * @return True on the scan that lifts the gate: the first with @p mature true.
     */
    bool noteScan(std::size_t n_flagged, std::size_t n_held, bool mature, const RaimScanStamp& at);
};

/**
 * @brief Run-scope telemetry of the prior channel (@c network.raim_prior_channel,
 *        network/qc/RaimPriorChannel.hpp): the carried-prior rows tested once, in the cell of
 *        their own column, and the responses their flags took.
 *
 * The channel changes which test reaches the carried priors, so it is counted at both of its
 * edges (standing invariant 7): the rows it tests and the cells it flags (the decoupling of a
 * carry, by its refloat or reset), and the arcs both channels flagged.  The denominators tell
 * apart the outcomes that otherwise read alike (standing invariant 4):
 *  - @c passes == 0: the channel never ran (post-fit RAIM never reached the attribution);
 *  - no row of any kind: there were no carried-prior rows, so the key acts on nothing;
 *  - @c flags == 0 and @c held == 0: rows were tested and nothing stood out.
 *
 * Every member stays at zero unless the key is armed, and nothing in the solve reads it.
 */
struct RaimPriorChannelTally {
    long long passes{0};               ///< Post-fit passes with the channel armed.
    long long station_walks{0};        ///< Station-epochs with at least one tested carried-prior row.
    std::array<long long, kContributionKinds + 1> rows{};  ///< Rows tested, by kind (@c Unset last).
    long long rows_floor{0};           ///< Rows left out at the redundancy floor (@c network.raim_redundancy_normalize).
    long long rows_other_dominant{0};  ///< Tested rows whose largest coefficient is on another carried column than their own.
    long long cells{0};                ///< Prior-table cells over the run.
    long long scale_n{0};              ///< Passes with at least one tested row (the scale's denominator).
    double    scale_sum{0.0};          ///< Sum of the channel's robust scale over those passes.
    double    scale_max{0.0};          ///< Its largest value.
    long long flags{0};                ///< Cells over the isolated-arc bar that passed the maturity gate.
    long long held{0};                 ///< Cells over the bar held by the maturity gate.
    long long arcs{0};                 ///< Line-of-sight flags the channel refloated.
    long long arcs_ambiguities{0};     ///< Carried ambiguities those refloats reached (the arc response's count).
    long long arcs_both{0};            ///< Line-of-sight flags on an arc the observation channel refloated in the same pass (one refloat).
    long long arcs_empty{0};           ///< Channel refloats that found no ambiguity of the satellite in the station's pool.
    long long rxcb_resets{0};          ///< Receiver code-bias flags that erased the carried entry.
    long long rxcb_no_entry{0};        ///< Receiver code-bias flags whose carry held no entry to erase.
    long long unwired{0};              ///< Flags on an unwired subject: counted and written, no response.
    double      max_statistic{0.0};    ///< The largest row statistic of the run.
    double      max_tow{-1.0};         ///< Its epoch (GPS TOW, s); < 0 = never set.
    std::string max_station;           ///< Its station.
    std::string max_label;             ///< Its cell label.
    ContributionKind max_kind{ContributionKind::Unset};  ///< Its row kind.

    /**
     * @brief Count one tested row, and keep its identity when it is the run's largest.
     * @param kind           The row's kind.
     * @param statistic      Its statistic.
     * @param other_dominant Whether its largest coefficient is on another carried column.
     * @param station        Its station.
     * @param label          Its cell label.
     * @param tow            The epoch.
     */
    void noteRow(ContributionKind kind, double statistic, bool other_dominant,
                 const std::string& station, const std::string& label, double tow);

    /// @return The rows tested over every kind.
    long long rowsTested() const;
};

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct NetworkPostFitHandoff;   // network/solve/SolveGlue.hpp

/**
 * @brief Run the post-fit network RAIM pass for this epoch: the residual scan, the
 *        cross-station consensus, the median-polish attribution and the resulting
 *        refloats.
 *
 * The ordering contract is in the file header.
 *
 * @param run The run (network/driver/Run.hpp).
 *            Written: @c run.ctx (carry, counters and entity tables; knobs are read),
 *            @c run.traces (RAIM, attribution, station-residual and event traces),
 *            @c run.setup.stations (the parameter set's pending-reseed mark on a
 *            refloat) and @c run.engines.prires (see below).
 *            Read: @c run.config (the RAIM getters, among them
 *            networkRaimAttribution, networkRaimSigma0Trigger and
 *            networkRaimWithinEpoch), @c run.log (the refloat summary and the
 *            within-epoch warning) and @c run.engines.est (reconstructStation and
 *            rowRedundancy, both const).
 * @param ep The epoch (network/driver/Epoch.hpp), read only:
 *           - @c se, the synced epoch (week and TOW for the trace and log rows);
 *           - @c assembly.asm_res, the per-station assembly (ambiguity lists, A_s, A_r,
 *             y, w);
 *           - @c assembly.est_index_of, the station to reduced-system index (negative =
 *             absent);
 *           - @c pass_a.present, this epoch's presence mask (the scope of an attribution
 *             refloat);
 *           - @c solve.x_s, the post-fit shared solution vector;
 *           - @c solve.plan.shared_datum, the shared hard-pin datum (input of the Baarda
 *             redundancy);
 *           - @c solve.retro_feed, whether the retro-bump path is armed
 *             (@c ura_retro_bump or @c ura_refloat_inflate > 1).
 * @param handoff [in/out] The post-fit hand-off (@c ep.solve.handoff):
 *            - @c clock_refloat_sats, consensus and whole-satellite culprits whose
 *              shared clock also refloats (consumed by networkClockQc and the epoch
 *              stash);
 *            - @c retro_flag_sats, culprits whose emitted correction was contaminated
 *              (the retro-bump feed, consumed by networkClockQc);
 *            - @c n_arc_refloat_epoch, this epoch's isolated-arc refloat count (read
 *              later in the epoch by networkStashStatus for the epoch-status row).
 *
 * The prior-residual census accumulator (@c run.engines.prires) is used here only as the
 * data-side feed of the constraint-aware global variance component
 * (@c network.prior_residual_global_sigma0).  The residual walk already holds every
 * row's weight, residual and redundancy, so the feed costs a multiply and two adds
 * inside an existing loop.  It is inert unless that knob is on and measure-only in every
 * case: no decision of this pass (the σ̂0 pool, the consensus, the attribution, the
 * refloats) reads from it.  The accumulator is drained by networkStashStatus, which runs
 * after this pass, so the epoch's data side is complete when the column is written.
 */
void networkPostFitRaim(NetworkRun& run, const NetworkEpoch& ep, NetworkPostFitHandoff& handoff);

/**
 * @brief Record one isolated-arc scan of the refloat maturity gate: the run-scope tally, the
 *        lift and one event row per held flag.
 *
 * Called by networkPostFitRaim once per scan, after the gates have run, when
 * @c network.raim_refloat_maturity_gate is on and attribution drives the action.  It
 * writes only telemetry (nothing in the solve reads it):
 *  - @c ctx.counters.maturity_gate (RaimMaturityGateTally::noteScan), stamped with the
 *    processed-epoch index, @c ctx.counters.n_emitted and @c ctx.carry.ura_ewma_updates, the
 *    values the gate's verdict was formed from;
 *  - on the scan that lifts the gate, one RAIM_MATURITY @c lift event row and one info line
 *    (the row carries n_emitted and the sigma0 EWMA update count);
 *  - one RAIM_MATURITY @c arc_held event row per held flag (station, satellite, the flag
 *    statistic and bar).
 *
 * @param run      The run (network/driver/Run.hpp).
 *                 Written: @c run.ctx (the tally) and @c run.traces (the events trace).
 *                 Read: @c run.setup.stations (the station names), @c run.log, the knobs.
 * @param ep       The epoch (network/driver/Epoch.hpp), read only: @c se.time stamps the rows.
 * @param isolated The scan's result (raimSelectIsolatedArcs).
 * @param mature   The gate's verdict this scan.
 * @param sigma0   The epoch's sigma0, for the rows.
 */
void networkRaimMaturityGateNote(NetworkRun& run, const NetworkEpoch& ep,
                                 const RaimIsolatedArcs& isolated, bool mature, double sigma0);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report for RAIM.
 *
 * The subsystem's row of kNetworkRunEndReports.  Its sections, defined beside it in
 * Raim.cpp, print in this order, each gated on its own knob:
 *   - networkRaimPoolRunEnd: the admissions of the carried-phase RAIM pools and the
 *     pool re-host readout.
 *   - networkRaimWithholdRunEnd: the withhold-era release policy, the per-station
 *     variance factor (@c network.raim_station_factor) and the flag-rate hygiene
 *     (@c network.raim_rate_hygiene).
 *   - networkRaimMaturityGateRunEnd: the isolated-arc refloat maturity gate
 *     (@c network.raim_refloat_maturity_gate): the arcs and epochs it held, the lift, and
 *     the outcomes the denominators tell apart (RaimMaturityGateTally).
 *   - networkRaimPriorChannelRunEnd (network/qc/RaimPriorChannel.cpp): the prior channel
 *     (@c network.raim_prior_channel): the rows it tested, the cells it flagged and the
 *     responses taken (RaimPriorChannelTally).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkRaimRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- PASS B station steps, declarations ----

struct StationBuild;                       // network/assembly/StationBuild.hpp
struct StationCarryFrame;                  // network/assembly/StationBuild.hpp
enum class StationStep : std::uint8_t;     // network/assembly/StationBuild.hpp

/**
 * @brief PASS B station step: the carry-neutral census of checkable ambiguity columns
 *        for the RAIM pool re-host.
 *
 * Row @c raim_columns of kStationBuildSteps (network/assembly/StationBuild.hpp).  PASS B
 * is the epoch's per-station assembly.  The step creates no rows or priors.  It records
 * @c qc_amb on the station's assembly: every ambiguity column present in the station's
 * local block (A_r) this epoch and not gauge-pinned, whichever carry owns it.  Under the
 * invariant carry the leveling list @c pl_amb is never filled, so without this census
 * every post-fit QC tier would see an empty pool.
 *
 * Returns at once unless @c raim_ci_on or @c pd_handover_continuous (the continuous
 * phase-datum handover is a second consumer of the census).
 *
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkRaimColumnCensus(StationBuild& build);

// ---- End of the PASS B station steps, declarations ----

/// @brief Run-scope counters of the RAIM tiers (post-fit and within-epoch pools,
///        per-station factor, rate hygiene) and the processed-epoch count.
///
/// A base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RaimCounters {
    /// Run-end telemetry of the RAIM pool re-host.  All zero unless ctx.knobs.raim_ci_on.
    /// The counters tell "post-fit pass never ran" from "census empty" from "nothing
    /// flagged" (see NetworkRaimCiTallies).
    NetworkRaimCiTallies raim_ci;

    /// Run-end telemetry of the isolated-arc refloat maturity gate
    /// (network.raim_refloat_maturity_gate): the arcs and epochs it held and the lift.  All
    /// zero unless the gate is armed and the T4 attribution path ran (see
    /// RaimMaturityGateTally).
    RaimMaturityGateTally maturity_gate;

    /// Run-end telemetry of the prior channel (network.raim_prior_channel): the carried-prior
    /// rows tested, the cells flagged and the responses taken.  All zero unless the key is
    /// armed (see RaimPriorChannelTally).
    RaimPriorChannelTally prior_channel;

    // Ambiguity-vehicle telemetry.  These counters are the on-path evidence for the
    // float ambiguity vehicle (network.ambiguity_vehicle float_kf).  A float_kf run with
    // 0 drops is a miswire, not a quiet run.  Together with the ambiguity-state trace
    // counters (n_amb_state_rows, n_amb_state_events, n_amb_state_epochs) and the
    // S-transform counters, they are reported at run end, so an arm that enables a
    // mechanism and moves none of them is visible at once.
    long long n_amb_drop              = 0;  ///< Carried priors dropped back to their birth prior, absolute value kept.
    long long n_amb_drop_t4           = 0;  ///< Of n_amb_drop, from the attribution refloat (the leveling vehicle erases the entry instead).
    long long n_amb_drop_t3           = 0;  ///< Of n_amb_drop, from the consensus refloat (the leveling vehicle erases the entry instead).
    // Drop-floor telemetry, counted whether or not the floor is armed.
    /// Drops whose restored variance came from network.amb_float_drop_var_m2, that is,
    /// the drop floor at this line of sight exceeded max(var_birth, var_first, birth
    /// floor).  0 by construction with the knob at 0.
    long long n_amb_drop_floored      = 0;
    /// Drops taken before network.amb_float_drop_floor_after_ep opened, with an armed
    /// drop floor: the population the gate withheld the floor from.  0 with no gate and
    /// 0 with no floor.
    long long n_amb_drop_pre_gate     = 0;
    /// In-block iono carries erased by an attribution arc refloat.  The refloat
    /// statistic reads the level error and the iono error as a sum, so an arc whose
    /// level is no longer trusted cannot vouch for the iono estimated with it.  The
    /// line of sight re-draws both.
    long long n_iono_blk_erased_t4  = 0;
    /// Post-fit RAIM pool admissions (station-epochs x rows) whose admitted row is a
    /// whitened block row of the iono columns.  Counted only; the pool rule is
    /// unchanged.  See n_raim_pool_zwd_rows for why block rows enter the pool.
    long long n_raim_pool_iono_rows = 0;
    /// Drops at birth strength that cleared a regression or block row
    /// (network.amb_float_drop_decouple_always).
    long long n_amb_drop_decoupled_idem = 0;
    /// Post-fit RAIM pool admissions (station-epochs x rows) whose admitted row is a
    /// whitened block row of the clock-family columns (receiver clock or ISB, the
    /// inter-system bias).  Counted only; the pool rule is unchanged.  See
    /// n_raim_pool_zwd_rows for why block rows enter the pool.
    long long n_raim_pool_rxclk_rows = 0;
    /// Post-fit RAIM pool admissions (station-epochs) whose admitted row is the ZWD
    /// (zenith wet delay) whitened block row.  The pool admits a row whose coefficient on
    /// the ambiguity column exceeds 0.5 and whose kind @c raimPoolAdmitsKind accepts
    /// (network/assembly/RowClassify.hpp).  A block row is a row of L⁻¹ (L Lᵀ = the
    /// block's prior covariance).  It is lower-triangular, so it carries coefficients on
    /// the ambiguity columns pushed before its own column and can clear 0.5 on them.
    /// Ambiguity columns are pushed first, so a ZWD block row (a carried-prior kind the
    /// rule admits) reaches them.
    /// Counted only; the pool rule is unchanged.
    long long n_raim_pool_zwd_rows  = 0;
    /// The carried-phase RAIM pools' admissions per contribution kind (see
    /// @c raimPoolAdmitsKind in network/assembly/RowClassify.hpp): the post-fit pool
    /// (Raim.cpp, one walk per epoch it runs) and the within-epoch assess pool
    /// (RaimWithinEpoch.cpp, one walk per assess call, the first and each replay's).
    /// Read at run end.  The declared rule admits the observation phase rows, the
    /// coupling and block carried-prior rows and the SD-EWL (single-difference
    /// extra-wide-lane) constraint rows, so the non-data rows' share is on record.
    RaimPoolAdmissionTally raim_pool_postfit;
    RaimPoolAdmissionTally raim_pool_within;   ///< As @c raim_pool_postfit, for the within-epoch assess pool.

    // Refloats that act in one solve: the carried entry is erased (or dropped, on the
    // float vehicle) after the harvest, so the arc refloats on the next epoch.
    long long n_cr_raim_attrib        = 0;  ///< Attribution refloats (live only when network.raim_attribution is on).
    /// Consensus refloats (live only when network.raim_attribution is off).  The two are
    /// a run-constant either/or, never a split.
    long long n_cr_raim_consensus     = 0;
    long long n_pc_erase_raim         = 0;  ///< Phase-colour entries erased with the line of sight by an attribution refloat.

    // Telemetry for the per-station variance factor (network.raim_station_factor) and
    // the flag-rate hygiene (network.raim_rate_hygiene).  A station departs when its
    // EWMA factor, relative to the network median, rises above a threshold, and returns
    // when it falls back below 0.8 times that threshold.  Both edges are counted.
    long long n_staf_departs          = 0;  ///< Depart events (EWMA ratio above the threshold).
    long long n_staf_returns          = 0;  ///< Return events (EWMA ratio below 0.8 x the threshold).
    double    staf_max_ratio          = 0.0;///< Run maximum of the EWMA ratio to the network median.
    long long n_staf_epochs           = 0;  ///< Epochs the factor was computed on.
    /// Cells with no phase row this epoch (raw residual exactly 0), which can never flag.
    /// They are excluded from the cell flag-rate opportunity count (rate hygiene).
    long long n_t3_untestable_cells   = 0;

    // The processed-epoch count (epochs decimated out are not counted).
    std::size_t n_loop_epochs = 0;
};

} // namespace GnssProcessor
