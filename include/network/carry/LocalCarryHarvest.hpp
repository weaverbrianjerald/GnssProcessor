#pragma once
/**
 * @file LocalCarryHarvest.hpp
 * @brief The station-local carries' post-solve harvest: its table of contents and the
 *        machinery its kinds share.
 *
 * After the epoch's solve, networkLeveling (network/diag/Leveling.cpp) calls
 * networkHarvestLocalCarries, which reads each station's posterior out of the solved normal
 * equations and refreshes the carried maps that the next epoch's PASS B folds as priors.
 * The orchestrator is a list of calls, one per carried kind, in a fixed order; each kind's
 * harvest lives in the file of its carry:
 *
 *   carried state                       harvest                                  file
 *   anchor handover ledger              networkHarvestHandoverLedger             filter/NetworkLevelingCarry
 *   ambiguities (local_carry)           networkHarvestAmbiguityStation,          filter/NetworkLevelingCarry
 *                                       networkAmbiguityMonitorFeeds,
 *                                       networkPruneAmbiguities
 *   ionosphere-free variance trace      networkAmbIfTraceStation                 filter/NetworkAmbStateTrace
 *   phase colour (pc_carry)             networkHarvestPhaseColourStation,        filter/NetworkPhaseColourCarry
 *                                       networkPrunePhaseColour
 *   slant ionosphere (iono_blk_carry)   networkHarvestSlantIonoBlock             filter/NetworkSlantIonoCarry
 *   receiver clock (rx_clk_carry/rate)  networkHarvestRxClock                    filter/NetworkRxClockCarry
 *   receiver code bias (rx_cb_carry)    networkHarvestRxCbBlock,                 filter/NetworkRxCodeBiasCarry
 *                                       networkRxCbDatumGaugeCensus,
 *                                       networkHarvestRxCbCarry
 *   zenith wet delay (zwd_carry)        networkHarvestZwd, networkZwdSeamWindowStep  filter/NetworkZwdCarry
 *   eq34 receiver phase bias (rpb)      networkHarvestRxPhaseBias                filter/NetworkChainDatum
 *   the first post-resume harvest       networkResumeCouplingCensusRow,          app/NetworkWarmResume
 *                                       networkResumeFirstEpochRetire
 *
 * The order is load-bearing (each constraint is checked by scripts/nom_hv1_verify.py):
 *   1. The handover ledger reads each arc's first-stash variance from the previous epoch's
 *      carry, so it runs before the ambiguity stash rewrites it.
 *   2. Per station: the block layout, the ambiguity rows, the ionosphere-free trace (it reads
 *      the rows just written), then phase colour.  The shared-history notes therefore run
 *      ambiguities then phase colour per station, then kind by kind (every station's
 *      ionosphere, then receiver clock, receiver code bias and ZWD); the shared history's compression
 *      is sensitive to that order.
 *   3. The ambiguity rows read the pending-reseed mark before they clear it; RAIM and the
 *      gross line-of-sight screen run after the harvest because they set new marks.
 *   4. The leveling clock pl_prev_now_s is advanced after every station (the stash's trace age
 *      reads the previous stamp) and before the prunes.
 *   5. Every writer of an F3 block row runs before networkHarvestBorderEnd (the clone end and
 *      the shared-history compression read the finished blocks).
 *   6. networkResumeFirstEpochRetire runs last: it closes the latch both census rows read.
 *   7. The receiver code-bias census and carry run outside the warm bracket: the carry must
 *      work on the carry_invariant lineage without phase leveling.
 *
 * Each function opens with generated bindings (scripts/nom_hv1_cut.py): the run, epoch and
 * harvest values it reads, under the names the code below them uses.
 */
#include "network/carry/ClockClone.hpp"      // ClockCloneHarvest
#include "network/carry/SharedHistory.hpp"   // SharedHistoryHarvest
#include "network/driver/RunTypes.hpp"        // CoupledShared, PlHarvestBlock
#include <Eigen/Core>
#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

struct NetworkRun;
struct NetworkEpoch;

/// The shared border every carried column of one warm epoch regresses on (formerly four
/// locals of networkHarvestLocalCarries).  Built by networkHarvestBorderBegin, read by
/// networkHarvestColumn for every carried column, closed by networkHarvestBorderEnd after the
/// last row.  Empty on the legacy diagonal path (no network.carry_conditional_coupling).
struct HarvestBorder {
    std::map<Eigen::Index, CoupledShared> col2desc;  ///< Shared column -> its identity (clock, phase bias, code bias, SH coefficient).
    Eigen::MatrixXd      cov_df;  ///< The shared covariance in the datum frame: datum-pinned and ensemble-gauge columns zeroed.
    ClockCloneHarvest    ccl_h;   ///< Clock clones: the clones' posterior regression on the rest of the border.
    SharedHistoryHarvest shx_h;   ///< Shared history: where each harvested row's entry was written.
};

/// The segments of a station's F3 harvest block, in the order their columns are appended.
/// The ZWD is last, so its row is the block's last (the B9-in-block invariant).  PASS B's
/// block build (the carry steps of network/assembly/StationBuild.hpp) must
/// open a block on the same station-epochs; it pairs entries by block identity and row
/// index, not by this order.
enum class BlockSegment : int { Amb, Iono, RxClock, RxCodeBias, PhaseColour, Zwd };
inline constexpr std::size_t kBlockSegmentCount = 6;

/// One segment's rows of the block: its first row and its column count (0: absent).
struct BlockSpan { Eigen::Index row0{0}; Eigen::Index n{0}; };

/**
 * @brief The segment spans of one station's block, from the station layout's row offsets
 *        (the one place the layout arithmetic lives; tests/network/test_network_harvest_layout.cpp).
 * @param iono_row0   First row of the ionosphere segment (= the ambiguity count).
 * @param rxclk_row0  First row of the receiver-clock segment.
 * @param rxcb_row0   First row of the receiver code-bias segment.
 * @param pc_row0     First row of the phase-colour segment.
 * @param n_cols      The block's column count, the ZWD column included.
 * @param zwd_in_cols The ZWD column is the block's last.
 * @return Each segment's {first row, column count}, in BlockSegment order.
 */
std::array<BlockSpan, kBlockSegmentCount> harvestBlockSpans(
    Eigen::Index iono_row0, Eigen::Index rxclk_row0, Eigen::Index rxcb_row0,
    Eigen::Index pc_row0, Eigen::Index n_cols, bool zwd_in_cols);

/// One station's harvest layout for this epoch (replaces the eleven per-station hand-forward
/// vectors).  Written by networkHarvestStationBlock, read by every kind that writes a row of
/// the station's block.  A station the loop head skips keeps ok false and no block.
struct StationHarvestBlock {
    bool                      ok{false};  ///< The station is harvested this epoch.
    std::vector<Eigen::Index> cols;       ///< The block's local columns, segment by segment.
    std::array<BlockSpan, kBlockSegmentCount> span{};  ///< Each segment's rows.
    Eigen::VectorXd           xr;         ///< The station's back-substituted locals (reconstructStation).
    Eigen::VectorXd           vard;       ///< The legacy diagonal variances (empty under the coupling carry).
    std::shared_ptr<PlHarvestBlock> blk;  ///< The F3 block; null without one this station-epoch.

    /// The block a segment's rows go to: blk when the segment has columns, else null.
    std::shared_ptr<PlHarvestBlock> segmentBlock(BlockSegment s) const {
        return span[static_cast<std::size_t>(s)].n > 0 ? blk : nullptr;
    }
    /// The segment's first block row.
    Eigen::Index rowOf(BlockSegment s) const { return span[static_cast<std::size_t>(s)].row0; }
};

/**
 * @brief Harvest, stash and prune the station-local carries from this epoch's solve.
 *
 * The table of contents above, in its order.  Writes run state only (the carried maps, the
 * service counters, the station models' reseed marks and L5B tallies, the VCE and
 * prior-residual engines); reads the epoch's solution, assembly, columns and clock.
 * Called by networkLeveling at the SB_LEVELING spine line.
 *
 * @param run The run: its context (carry, counters, knobs), setup (stations) and engines.
 * @param ep  This epoch after the solve: solve.x_s, solve.plan, assembly, columns,
 *            pass_a.shared_phase_keys and clock (carry_warm, pl_now_s) are read.
 * @param events The epoch's events; writes harvest.
 */
void networkHarvestLocalCarries(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events);

/**
 * @brief The warm epoch's shared border: column identities, the datum-frame covariance and
 *        the clone and shared-history begin (inert pieces stay empty when their knobs are off).
 * @param run The run (the estimator, the context).
 * @param ep  This epoch (the plan's datum, the registered shared columns).
 * @return The border every carried column of this epoch regresses on.
 */
HarvestBorder networkHarvestBorderBegin(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief One carried column's pure conditional variance and its sparsified, identity-keyed
 *        regression on the free shared border (formerly the harvestCoupling lambda).
 * @param run        The run (the estimator, the context's clone counters).
 * @param ep         This epoch (solve.x_s: the shared estimates each kept term records).
 * @param border     This harvest's border (identities, datum-frame covariance, clone rows).
 * @param est_idx    The station's estimator slot.
 * @param col        The local column.
 * @param cross_cols With F3: the station's other block columns, gathered by the same solve; else null.
 * @param cross      With F3: receives (N_rr^-1)_{c c_j} over @p cross_cols; else null.
 * @return {cond_var, coupling}; cond_var <= 0 when the regression failed (the caller skips).
 */
std::pair<double, std::vector<CoupledShared>> networkHarvestColumn(
    NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border, std::size_t est_idx,
    Eigen::Index col, const std::vector<Eigen::Index>* cross_cols, Eigen::VectorXd* cross);

/**
 * @brief Station @p i's layout: whether it is harvested, its block columns in BlockSegment
 *        order, its back-substituted locals and its F3 block (registered with the clone
 *        harvest).
 * @param run    The run (the estimator, the stations, the knobs).
 * @param ep     This epoch (the solution, the assembly, the clock).
 * @param border This harvest's border (the clone harvest the block is registered with).
 * @param i      The station index.
 * @return The layout; ok false when the station is not harvested this epoch.
 */
StationHarvestBlock networkHarvestStationBlock(NetworkRun& run, const NetworkEpoch& ep,
                                               HarvestBorder& border, std::size_t i);

/**
 * @brief Close the border: the clones' within-station covariance into the F3 blocks, then the
 *        shared-history compression (both no-ops when their knobs are off).
 * @param run    The run (the context).
 * @param ep     This epoch (read by no statement; the seam form).
 * @param border This harvest's border, after the last carried row.
 */
void networkHarvestBorderEnd(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border);

} // namespace GnssProcessor
