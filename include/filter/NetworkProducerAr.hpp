#pragma once
/**
 * @file NetworkProducerAr.hpp
 * @brief Producer-side network ambiguity-resolution diagnostic (Phase 2, log only)
 *        plus the one-shot full-arrowhead normal-equation export.
 *
 * Runs on the final accepted network solution and only reads it: it neither
 * re-pins the datum nor re-solves, so the network solution is byte-identical
 * whether this block runs or not.  Two cooperating pieces:
 *
 *  1. **Producer network AR, Phase 2** (`network.ambiguity_resolution.enable` +
 *     `min_epochs`): reconstructs the per-station float ambiguities, forms an
 *     independent between-station double-difference basis (star topology per
 *     (const,signal), GPS+Galileo only), builds the exact DD covariance from
 *     @ref NetworkEstimator::jointConditionalCovariance, converts to cycles, and runs
 *     @ref ils::ilsFix only to log how many integers the network could fix (with the
 *     median DD formal-σ and float-fraction convergence gauges).  The DD ambiguities
 *     are datum-invariant, so the fix attempt is independent of the phase-bias datum.
 *     With `network.ambiguity_resolution.trace` also armed it writes every DD of the
 *     basis to `logs/network_producer_ar.csv` (@ref kProducerArHeader) with its
 *     identity, float value, formal σ, subset membership and returned integer, so the
 *     search can be graded against simulator truth offline
 *     (`scripts/simnet_producer_ar_grade.py`); a fix count alone is not a grade.  The
 *     run-end readout is the "Network (producer AR, Phase 2 log-only)" clause of
 *     networkProducerArRunEnd (below), fed by the `ctx.counters.netar_*` tallies.
 *     With `network.ambiguity_resolution.suspect_epochs` (card ARL) every DD with a
 *     (station, satellite) corner that the slip layer freed, dropped or flagged in the
 *     last few epochs stays in the basis and the trace but is kept out of the search
 *     (filter/NetworkArSuspect.hpp, @ref netar::maskedSearchOrder); the trace then
 *     carries the DD's suspect cause bits and its re-entry flag, and the release back
 *     into the search is counted (@ref netar::reentryOutcome).  The basis is the star
 *     unless `tree_basis` (card ARL: a spanning tree of station baselines per
 *     (constellation, signal)) or `cycle_basis` (card AGT: a sticky, full-coverage cycle
 *     basis per family of a constellation's signals, filter/NetworkArCycleBasis.hpp,
 *     searched one block per station, @ref netar::searchBlockKey) is armed.  Under
 *     `trace_lattice` (card AGT step AGT-6) the seam returns the epoch's determined DDs as
 *     cycles with every band-arc's life reading (@ref ArEpochFixes), which the measure-only
 *     lattice observatory (filter/NetworkArLattice.hpp) reads next in the same bracket.
 *  2. **One-shot NEQ export** (`network.neq_export_epoch`): a post-solve, read-only dump
 *     of the assembled arrowhead normal equations (border N_ss raw and reduced, with
 *     RHS; each station's N_sr / N_rr / u_r; shared and local column labels) as
 *     row-major little-endian double `.bin` for offline spectral / weak-direction
 *     attribution.  Fires on one epoch per run; the solution is untouched.
 *
 * Relocated out of Application::runNetwork's SB_NETAR block at decomposition INC-9b
 * (see docs/network_decomposition_plan.md, the SOLVE-REGION decomposition).  Owns no
 * state.  The caller keeps the @c networkSubBegin/End(SB_NETAR) brackets.
 */

#include "filter/IlsCore.hpp"            // ils::IlsResult / IlsGateConfig (blockDecorrelatedFix)
#include "filter/NetworkArCycleBasis.hpp" // ArEpochFixes (the lattice observatory's input; card AGT)
#include "filter/NetworkArSuspect.hpp"   // ArSuspectReentry (reentryOutcome; card ARL)
#include "filter/NetworkEstimator.hpp"   // NetworkEstimator
#include "filter/NetworkSbasis.hpp"      // NetworkSbasis::Plan (shared_datum)
#include "filter/NetworkRunTypes.hpp"    // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"      // MultiRinexSync::SyncedEpoch (bare SyncedEpoch)

#include <Eigen/Core>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only
struct NetworkRunContext;  // app/NetworkRunContext.hpp, included in the .cpp only

/**
 * @brief Pure helpers for the Phase-2 ILS cap permutation (unit-tested in
 *        tests/test_producer_ar_trace.cpp).
 *
 * They are separate from @ref networkProducerAr because the ILS receives a
 * capped, re-ordered copy of the DD basis: @c ils::IlsResult::kept indexes the
 * search vector, not the basis, and a trace that maps those indices back
 * directly would label the wrong satellite with the wrong integer while still
 * looking well-formed.  Both steps are pure functions of their inputs, so the
 * mapping can be tested on a constructed case rather than on a 24 h run.
 */
namespace netar {

/**
 * @brief Indices of the @p cap lowest-formal-σ DDs, i.e. the search subset.
 *
 * Sorts 0..n-1 ascending by @f$Q_{aa}@f$ (the standard partial-AR ordering) and
 * truncates to @p cap.  @p cap <= 0 or >= n returns the identity permutation,
 * unsorted: the uncapped search consumes the basis in its original order.
 *
 * @param Q_cyc  DD covariance in cycles²; only the diagonal is read.
 * @param cap    Search-dimension cap (@c network.ambiguity_resolution.ils_max_dim).
 * @return `search_of`: search row a ↦ its index in the full DD basis.
 */
std::vector<int> lowestSigmaOrder(const Eigen::MatrixXd& Q_cyc, int cap);

/**
 * @brief The search subset under the suspect-corner mask (card ARL,
 *        filter/NetworkArSuspect.hpp).
 *
 * With @p masked empty or all zero this is exactly @ref lowestSigmaOrder, so an unarmed
 * or quiet epoch searches the same subset in the same order.  Otherwise the masked DDs
 * are left out first and the cap applies to the rest: uncapped (or a cap at or above the
 * eligible count) gives the eligible DDs in basis order, a cap below it gives the @p cap
 * lowest-formal-σ eligible DDs.
 *
 * @param Q_cyc   DD covariance in cycles²; only the diagonal is read.
 * @param cap     Search-dimension cap (@c network.ambiguity_resolution.ils_max_dim).
 * @param masked  One flag per basis DD (non-zero = kept out of the search), or empty.
 * @return `search_of`: search row a ↦ its index in the full DD basis.
 */
std::vector<int> maskedSearchOrder(const Eigen::MatrixXd& Q_cyc, int cap,
                                   const std::vector<char>& masked);

/// Per-basis-DD outcome of one epoch's search: the content of the trace's
/// @c in_subset / @c fixed_int / @c kept columns.
struct DdFixLabel {
    bool   in_subset = false;  ///< This DD entered the (possibly capped) ILS search.
    bool   kept      = false;  ///< The ILS returned an integer for it.
    double fixed_int = 0.0;    ///< That integer (cycles).  Meaningless unless @c kept.
};

/**
 * @brief Map an @c ils::IlsResult back onto the full DD basis.
 *
 * @param n_dd       Size of the full DD basis (the trace's row population).
 * @param search_of  Search row ↦ basis index (from @ref lowestSigmaOrder).
 * @param kept       @c IlsResult::kept: indices into the search vector.
 * @param afix       @c IlsResult::afix: integers in @p kept order.
 * @return One label per basis DD, indexed 0..n_dd-1.
 *
 * Out-of-range entries in @p search_of / @p kept are dropped, and a @p kept
 * longer than @p afix stops at the length of @p afix, so a mis-sized result
 * loses rows instead of mislabelling them.
 */
std::vector<DdFixLabel> mapFixToBasis(int n_dd,
                                      const std::vector<int>& search_of,
                                      const std::vector<int>& kept,
                                      const Eigen::VectorXd& afix);

/// One basis DD's pair identity for @ref pairLaneColumns: two DDs with the same
/// @c key (constellation, sat_j, ref_sat, sta_r, ref_sta) and different
/// wavelengths are the two bands of one pair.
struct DdPairKey {
    std::string key;     ///< Pair identity without the signal.
    double lambda = 0.0; ///< The DD's wavelength (m).
};

/**
 * @brief The decorrelated partial fix run block by block (card ARL, block_search).
 *
 * Search rows sharing a @p block_of id (one baseline: its L1 and L2 DDs) are fixed
 * together by @ref ils::ilsFixDecorrelatedPartial on their marginal covariance, each
 * with its own bootstrap and ratio test, so one weak baseline no longer refuses the
 * whole network and the search cost is the sum of small blocks instead of one large
 * one.  The merged result: @c kept / @c afix over all blocks (search indices,
 * ascending), @c z_fixed / @c z_total summed, @c ratio the smallest accepted block
 * ratio, @c p_succ the product of the accepted blocks' bootstrap rates (a bound for
 * the epoch), @c full_ratio the smallest block ratio tried.  @c a_cond / @c Q_cond
 * hold each fixed block's conditioned values inside its block and the floats
 * (@p y, @p Q) elsewhere, so a combination inside one block reads its conditioned
 * value.  Pure.
 *
 * @param y        Search floats (cycles).
 * @param Q        Search covariance (cycles²).
 * @param block_of Block id per search row (any integers; rows with equal ids share a block).
 * @param gates    Acceptance gates, as for @ref ils::ilsFixDecorrelatedPartial.
 * @param determined_sigma_cyc As for @ref ils::ilsFixDecorrelatedPartial.
 */
ils::IlsResult blockDecorrelatedFix(const Eigen::VectorXd& y, const Eigen::MatrixXd& Q,
                                    const std::vector<int>& block_of,
                                    const ils::IlsGateConfig& gates,
                                    double determined_sigma_cyc);

/// One basis DD's pair lanes (card ARL, decorrelated_partial): the values behind the
/// trace's four pair columns, equal on the two rows of a pair.
struct PairLane {
    int    partner        = -1;     ///< Basis index of the other band's DD; -1 = no partner in the search.
    double wl_float_sigma = 0.0;    ///< The pair's wide lane a1 - a2 formal sigma before the fix (cycles).
    bool   have_cond      = false;  ///< The search returned a conditioned solution (something fixed).
    double wl_cond        = 0.0;    ///< The wide lane conditioned on the fixed components (cycles).
    double wl_cond_sigma  = 0.0;    ///< Its sigma (cycles).
    double nl_cond_sigma  = 0.0;    ///< The conditioned narrow lane's sigma (narrow-lane cycles).
};

/**
 * @brief The pair lanes per basis DD (card ARL, decorrelated_partial).
 *
 * For each basis DD whose pair partner (same @c key, other wavelength) is also in
 * the search: the pair's wide lane a1 - a2 formal sigma before the fix (from
 * @p Q_search), its wide lane conditioned on the fixed decorrelated components
 * (value and sigma, from @p a_cond / @p Q_cond), and the conditioned narrow lane
 * sigma, (f1 a1 - f2 a2) / (f1 - f2) in narrow-lane cycles, where band 1 is the
 * shorter wavelength.  Both DDs of a pair get the same values.  Without a partner the
 * entry stays default (@c partner -1); with @p a_cond empty (nothing fixed) the
 * conditioned values are unset (@c have_cond false).
 *
 * @param keys      One entry per basis DD.
 * @param search_of Search row ↦ basis index (@ref lowestSigmaOrder); rows past
 *                  @p n_search are not searched.
 * @param n_search  Search dimension.
 * @param Q_search  Search covariance (cycles²), search order.
 * @param a_cond    @c IlsResult::a_cond (search order), may be empty.
 * @param Q_cond    @c IlsResult::Q_cond (search order), may be empty.
 * @return One entry per basis DD.
 */
std::vector<PairLane> pairLanes(const std::vector<DdPairKey>& keys,
                                const std::vector<int>& search_of, int n_search,
                                const Eigen::MatrixXd& Q_search,
                                const Eigen::VectorXd& a_cond,
                                const Eigen::MatrixXd& Q_cond);

/**
 * @brief The trace's four pair columns from @ref pairLanes.
 * @param lanes One entry per basis DD.
 * @return One string per basis DD: ",wl_float_sigma,wl_cond,wl_cond_sigma,nl_cond_sigma"
 *         (leading comma, six decimals; ",,,," without a partner, the last three fields
 *         empty without a conditioned solution).
 */
std::vector<std::string> pairLaneText(const std::vector<PairLane>& lanes);

/**
 * @brief @ref pairLaneText of @ref pairLanes: the trace's four pair columns per basis DD.
 * @param keys      One entry per basis DD.
 * @param search_of Search row ↦ basis index.
 * @param n_search  Search dimension.
 * @param Q_search  Search covariance (cycles²), search order.
 * @param a_cond    @c IlsResult::a_cond (search order), may be empty.
 * @param Q_cond    @c IlsResult::Q_cond (search order), may be empty.
 * @return One string per basis DD.
 */
std::vector<std::string> pairLaneColumns(const std::vector<DdPairKey>& keys,
                                         const std::vector<int>& search_of, int n_search,
                                         const Eigen::MatrixXd& Q_search,
                                         const Eigen::VectorXd& a_cond,
                                         const Eigen::MatrixXd& Q_cond);

/**
 * @brief What the DDs the suspect-corner mask released got at their first search (card
 *        ARL, the release edge of filter/NetworkArSuspect.hpp).
 *
 * Counts the DDs flagged in @p reentry and those of them kept, and over the L1/L2 pairs
 * whose two DDs both re-entered (each pair once): both singles kept, or else the wide lane
 * determined (@c have_cond and @c wl_cond_sigma <= @p det_sigma_cyc), the classes of
 * scripts/simnet_producer_ar_pairs.py.  Pure.
 *
 * @param reentry        One flag per basis DD (@ref ArSuspectSearch::reentry); empty = none.
 * @param labels         @ref mapFixToBasis of this search.
 * @param lanes          @ref pairLanes of this search; empty = no pair counts.
 * @param det_sigma_cyc  The wide-lane determination bar
 *                       (@c network.ambiguity_resolution.decorrelated_determined_sigma_cyc).
 * @return The counts.
 */
ArSuspectReentry reentryOutcome(const std::vector<char>& reentry,
                                const std::vector<DdFixLabel>& labels,
                                const std::vector<PairLane>& lanes, double det_sigma_cyc);

/**
 * @brief The search block of a DD: "cons|sta_r|ref_sta" (one block per baseline, card ARL
 *        block_search), or "cons|sta_r" under the cycle basis (one block per station, card
 *        AGT: a station's partner and borrowed DDs in every band, so the two bands of a chord
 *        share a block).  The covariance readback and the search use the same key.
 * @param cons        Constellation key ("G").
 * @param sta_r       The DD's station.
 * @param ref_sta     Its reference station.
 * @param per_station True under @c network.ambiguity_resolution.cycle_basis.
 * @return The block key.
 */
std::string searchBlockKey(const std::string& cons, const std::string& sta_r, const std::string& ref_sta,
                           bool per_station);

} // namespace netar

struct NetworkRun;              // app/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // app/NetworkEpoch.hpp (card NOM3 step EP1)

/**
 * @brief Run the log-only producer-AR and NEQ-export tail for this epoch.
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log), run.engines.est
 *            (est), run.setup.stations (stations), run.log_dir (log_dir_).
 * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se), ep.solve.x_s
 *           (x_s), ep.assembly.est_index_of (est_index_of), ep.assembly.asm_res (asm_res),
 *           ep.solve.plan (plan).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: [in,out] run context: reads `counters.n_emitted` (the min_epochs and
 *     NEQ-export gates) and `knobs.carry_aware_covariance`, and adds to the `counters.netar_*`
 *     run-end tallies. With `knobs.ar_suspect_epochs` > 0 (card ARL, the suspect-corner mask,
 *     filter/NetworkArSuspect.hpp) it also folds this epoch's `carry.relevel` latch and
 *     `entities.arcs` stamps into `carry.ar_suspect` at loop epoch
 *     `counters.n_loop_epochs`, keeps every DD with a suspect corner out of the search, counts
 *     the corners it releases back into the search and what their DDs get there
 *     (`carry.ar_suspect.masking`), and adds to `counters.netar_suspect`. Under
 *     `network.ambiguity_resolution.cycle_basis` (card AGT) it reads each family's memory in
 *     `carry.ar_basis` (the last search's links, basis and arc lives), the arc ledger,
 *     `entities.arcs`, `the station table's amb_flag_ep` and `carry.relevel.free_ambs` (the arc life
 *     stamps, filter/NetworkArcLife.hpp), stores the memory back after the search and adds to
 *     `counters.netar_cycle`. Under `trace_lattice` it also reads `carry.relevel.free_cause`
 *     (each freed arc's cause bits, for the observatory's attribution). Taken whole for
 *     seam-family uniformity.
 *   - @p config_ = run.config: Reconstructs `nc = config_.getNetworkConfig()` and `netar =
 *     nc.ambiguity_resolution`.
 *   - @p log = run.log: "main_log": the Phase-2 AR summary and NEQ-export info rows.
 *   - @p traces = run.traces: Trace-stream owner; the seam writes @c producer_ar_trace (one
 *     row per DD, @ref kProducerArHeader) when @c network.ambiguity_resolution.trace is armed.
 *   - @p se = ep.se: Current synced epoch (week/TOW for the log rows).
 *   - @p est = run.engines.est: Post-solve estimator (const): reconstructStation /
 *     jointConditionalCovariance / normals / column labels.
 *   - @p x_s = ep.solve.x_s: Post-fit shared solution vector (non-empty guard and the
 *     per-station reconstruction).
 *   - @p stations = run.setup.stations: This run's network stations (float-amb reconstruction
 *     source).
 *   - @p est_index_of = ep.assembly.est_index_of: Per-station estimator-slot map (< 0 = not in
 *     this epoch).
 *   - @p asm_res = ep.assembly.asm_res: Per-station assembly results (h2local for local-column
 *     labels).
 *   - @p plan = ep.solve.plan: S-basis plan (shared_datum for the conditional covariance).
 *   - @p log_dir_ = run.log_dir: Output root for the NEQ-export artifacts
 *     (log_dir_/neq_export).
 *
 * @return What the lattice observatory reads this epoch (card AGT step AGT-6): the cycle
 *         basis built, the determined DDs as cycles and every band-arc's life reading.
 *         Empty (@c searched false) unless `network.ambiguity_resolution.trace_lattice` and
 *         the search block ran.
 */
ArEpochFixes networkProducerAr(NetworkRun& run, const NetworkEpoch& ep);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the producer ambiguity-resolution search.
 *
 * The producer ambiguity-resolution search (Phase 2, log-only): epochs searched, the
 * ratio and success-rate medians and the fix counts, and a warning when the search never
 * reached the ILS.
 *
 * Gate: returns 0 at once unless `nc.ambiguity_resolution.enable`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkProducerArRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
