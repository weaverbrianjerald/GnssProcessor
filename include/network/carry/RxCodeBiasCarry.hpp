#pragma once
/**
 * @file RxCodeBiasCarry.hpp
 * @brief The receiver code-bias carry family: the IFB re-coupling edge, the in-block columns, the diagonal priors and the receiver-DCB datum rows.
 *
 * A family file of PASS B's station build: the steps of the family in
 * kStationBuildSteps / kStationCarrySteps (network/assembly/StationBuild.hpp).
 *
 * The harvest half is the family's post-solve harvest, stash and prune, called by
 * networkHarvestLocalCarries (network/carry/LocalCarryHarvest.hpp, its table of contents).
 */

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <string>
#include <utility>

namespace GnssProcessor {

// ---- PASS B station steps, declarations ----

struct StationBuild;                       // network/assembly/StationBuild.hpp
struct StationCarryFrame;                  // network/assembly/StationBuild.hpp
enum class StationStep : std::uint8_t;     // network/assembly/StationBuild.hpp

/**
 * @brief The re-coupling edge of an estimated receiver inter-frequency bias (IFB).
 *
 * Step `rx_ifb_recouple` of kStationBuildSteps (network/assembly/StationBuild.hpp); its kinds:
 * none (it records only). It returns at once unless
 * `ctx.param_inventory.rx_ifb.estimated_by_knob`. Records: st.l5b.recouple,
 * st.l5b.last_est_epoch.
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkRxIfbRecoupleEdge(StationBuild& build);

/**
 * @brief The station's carried receiver code-bias columns for the local block
 *        (network.carry_rx_code_bias_in_block).
 *
 * Step `rx_cb_block` of kStationCarrySteps (network/assembly/StationBuild.hpp); its kinds: none
 * (it records only). It returns at once unless legacy_local_inject && do_amb_carry.
 * Records: pl_rxcb_restored_skip, prm_rxcb_gap_skip; rxcb_bc, rxcb_ifb.
 * @param carry The station's carry frame (the build, the priors, the coupling builder, the
 *              block columns).
 */
void networkRxCbBlockColumns(StationCarryFrame& carry);

/**
 * @brief The rx-CB carry's diagonal priors and the zero-mean receiver-DCB datum rows.
 *
 * Step `rx_cb` of kStationCarrySteps (network/assembly/StationBuild.hpp); its kinds:
 * PriorRxCbDatum, PriorRxCbDatumPrePd, PriorRxCbCarry. It returns at once unless `rx_cb_on ||
 * rx_cb_datum`. Records: pl_cb, pl_cb_*, prm_rxcb_*; st.l5b.diag_*.
 * @param carry The station's carry frame (the build, the priors, the coupling builder, the
 *              block columns).
 */
void networkRxCbPriors(StationCarryFrame& carry);

/**
 * @brief The weight of the hard receiver geometry-free row (@c network.rx_gf_datum hard).
 *
 * The weight of the chain's existing hard station-local pins (the receiver phase-bias pin and the
 * ambiguity pins of the phase datum, network/solve/PhaseDatum.cpp; @c kAnchorLevelHoldWeight), so
 * the station block carries one hard scale.  Not a tuned value: the row is a pure datum (the
 * third-band datum record, XBD, section 4), so any weight that makes it exact gives the same
 * estimable solution.
 */
inline constexpr double kRxGfHardWeight = 1e10;

// ---- End of the PASS B station steps, declarations ----

// ---- Harvest half, declarations ----

struct NetworkRun;                          // network/driver/Run.hpp
struct NetworkEpoch;                        // network/driver/Epoch.hpp
struct HarvestBorder;                       // network/carry/LocalCarryHarvest.hpp
struct StationHarvestBlock;                 // network/carry/LocalCarryHarvest.hpp

/**
 * @brief network.carry_rx_code_bias_in_block: receiver code-bias columns with their regression and
 *        harvest-block row, the L5B family re-seed erase and the 10-sigma step test.
 *
 * @param run The run: its context (carry, counters, knobs), setup (stations) and engines.
 * @param ep This epoch after the solve (the solution, the assembly, the columns and the clock are
 *           read; nothing is written).
 * @param border This harvest's shared border (column identities, datum-frame covariance, the clone
 *               and shared-history handles).
 * @param blocks Every station's harvest layout, by station index (ok false: not harvested).
 */
void networkHarvestRxCbBlock(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border,
                             const std::vector<StationHarvestBlock>& blocks);

/**
 * @brief The zero-mean receiver-DCB datum's folds, counted under its own knob
 *        (network.rx_code_bias_datum) outside every carry bracket.
 *
 * @param run The run: its context (carry, counters, knobs), setup (stations) and engines.
 * @param ep This epoch after the solve (the assembly is read; nothing is written).
 */
void networkRxCbDatumGaugeCensus(NetworkRun& run, const NetworkEpoch& ep);

struct NetworkRunContext;                   // network/state/RunContext.hpp
struct NetworkRunTraces;                    // network/diag/RunTraces.hpp
struct SyncedEpoch;                         // rinex/MultiRinexSync.hpp
struct NetStation;                          // network/driver/RunTypes.hpp
struct StationAssembly;                     // network/driver/RunTypes.hpp

/**
 * @brief The serial tally of the receiver geometry-free datum (@c network.rx_gf_datum birth_prior
 *        or hard): its counters, each station's memo and the datum's edges as events.
 *
 * Runs after the parallel produce, in station order.  For every DCB record of a station
 * (StationAssembly::rx_gf) it classifies a column without a carried row against the station's
 * memo: @c birth (no memo for the constellation), @c return (the column was last present more
 * than the carry's horizon, kRxCbMaxGapS, ago) or @c uncarried (present within the horizon,
 * its carried entry erased, restored without its block or not yet warm).  Edges are events on
 * the @c RX_GF_DATUM tier of network_events.csv (standing invariant 7): @c place at a birth or
 * return, @c reseed when a carried column loses its carry while present, @c release when a
 * column present last epoch is absent (the station out of the solve, or the column gone), each
 * naming the station and the constellation.  Does nothing under prior_refold.
 *
 * @param ctx The run context: the knobs (read), the counters and the station memos (written).
 * @param traces The run traces (the events stream).
 * @param se This epoch (its time stamps the events).
 * @param stations The roster (station names).
 * @param asm_res Every station's produce result, by roster index.
 */
void networkRxGfDatumTally(NetworkRunContext& ctx, NetworkRunTraces& traces, const SyncedEpoch& se,
                           const std::vector<NetStation>& stations,
                           const std::vector<StationAssembly>& asm_res);

/**
 * @brief network.carry_rx_code_bias: the produce tallies, the diagonal stash (skipped under the
 *        in-block route) with its prior-residual row and L5B erase, then the gap prune. Runs
 *        outside the warm bracket: the carry works on lineages without phase leveling.
 *
 * @param run The run: its context (carry, counters, knobs), setup (stations) and engines.
 * @param ep This epoch after the solve (the solution, the assembly, the columns and the clock are
 *           read; nothing is written).
 */
void networkHarvestRxCbCarry(NetworkRun& run, const NetworkEpoch& ep);

// ---- End of the harvest half, declarations ----

/// @brief Run-scope counters of RxCodeBiasCarry, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RxCodeBiasCarryCounters {
    long long n_rxcb_blk_harvested = 0;              ///< receiver code-bias entries stashed with a border regression and a harvest-block row
    long long n_rxcb_blk_harvest_fail = 0;           ///< receiver code-bias columns whose coupling harvest failed (not stashed; an existing entry is left as it is)
    long long n_rxcb_blk_step_reset = 0;             ///< receiver code-bias entries erased at the harvest by the frame-invariant step test (10 sigma of the two harvested variances, the walk and a 0.5 m guard): the event response to a jump in the carried value
    long long n_rxcb_blk_no_block = 0;               ///< receiver code-bias entries stashed without a harvest-block row (cross terms to the station's other columns missing next epoch)
    // rx-CB carry run counters (armed-only run-end line).
    /// Run totals of the rx-CB carry: receiver code-bias priors folded, priors withheld by the
    /// 10-sigma innovation gate (the receiver-reset response), and harvests (entries stashed).
    long long rx_cb_injected_total = 0, rx_cb_innov_skips_total = 0, /**< Run total of priors withheld by the 10-sigma innovation gate (the receiver-reset response). */
              rx_cb_harvested_total = 0;   /**< Run total of rx-CB harvests (entries stashed). */
    /// Per-station fold/withhold tally of the rx-CB carry, kept on
    /// every run where the carry is armed; trace-class, with byte-identical products.
    /// It shows what the pooled "N folded, M withheld" line hides.  Keyed by station
    /// name (the teardown has no station roster); value = {station-epochs with at least
    /// one rx-CB carry decision, station-epochs in which every decision was a
    /// withhold}.  A station whose second number is > 90 % of its first carried nothing
    /// for the run, while the run-end line still counted it among the "N carried".
    /// The cause is that
    /// the harvest overwrites a withheld entry unconditionally and the prune drops only
    /// entries older than 900 s, so a refused station is refreshed and refused for the
    /// whole run.  Written serially at the post-solve harvest from the produce phase's
    /// per-station StationAssembly tallies, so it is race-free like the counters beside
    /// it.
    std::map<std::string, std::pair<long long, long long>> rx_cb_sta_tally;
    // Zero-mean receiver code-bias prior rows folded over the run, one per
    // station-epoch per constellation.  0 with the toggle armed means
    // that no receiver DCB column ever existed.
    size_t n_rxcb_gauges = 0;   ///< zero-mean receiver code-bias prior rows folded over the run
    // The receiver geometry-free datum (network.rx_gf_datum birth_prior or hard; armed-only
    // run-end line).  Station-epoch counts are per DCB column (one per receiver and constellation).
    long long rxgf_records = 0;          ///< DCB column-epochs the datum saw (every record)
    long long rxgf_carried = 0;          ///< ... of which folded a carried row (birth_prior: the prior not refolded)
    long long rxgf_prior_rows = 0;       ///< birth_prior: loose prior rows folded (columns without a carried row)
    long long rxgf_hard_rows = 0;        ///< hard: hard rows placed
    long long rxgf_place_birth = 0;      ///< placements at a birth (no memo for the constellation)
    long long rxgf_place_return = 0;     ///< placements at a return after more than the carry's horizon
    long long rxgf_reseed_uncarried = 0; ///< column-epochs without a carried row within the horizon (erase, restore, warm-up)
    long long rxgf_reseed_edges = 0;     ///< ... of which edges (the column carried last epoch, not this one)
    long long rxgf_releases = 0;         ///< columns present last epoch and absent this epoch
    double    rxgf_max_abs_target = 0.0; ///< hard: largest |carried target| (m): the held DCB stays at its seed 0
};

struct RxCbKey;   // common/ParamRef.hpp

/**
 * @brief The random-walk process noise of a carried receiver code-bias column (m²/s).
 *
 * A receiver IFB column (@c RxIfbRule::governs: the extra-band code delay of a band outside each constellation's
 * clock-reference pair) takes @p ebd_q when it is above 0 (@c network.rx_extra_band_delay_q); every other column,
 * and every column when @p ebd_q is 0, takes @p cb_q (@c network.rx_cb_carry_q).  The one rule every carry site
 * of the family reads (the inject gates, the harvest's step test and the prior-residual census).
 *
 * @param key   The column's receiver code-bias key.
 * @param cb_q  @c network.rx_cb_carry_q (m²/s).
 * @param ebd_q @c network.rx_extra_band_delay_q (m²/s; 0: none).
 * @return The column's process noise (m²/s).
 */
double rxCbCarryQ(const RxCbKey& key, double cb_q, double ebd_q);

/**
 * @brief @ref rxCbCarryQ for a column named by its key text (@c receiverCodeBiasKey); a key that does not parse
 *        takes @p cb_q.
 * @param key_text The column's receiver code-bias key text.
 * @param cb_q     @c network.rx_cb_carry_q (m²/s).
 * @param ebd_q    @c network.rx_extra_band_delay_q (m²/s; 0: none).
 * @return The column's process noise (m²/s).
 */
double rxCbCarryQ(const std::string& key_text, double cb_q, double ebd_q);

} // namespace GnssProcessor
