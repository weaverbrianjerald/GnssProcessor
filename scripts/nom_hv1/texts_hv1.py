"""Hand-written text of card NOM3 step HV1a, emitted by scripts/nom_hv1_cut.py: the spine
header, the per-function Doxygen of the owner headers, the layout epilogue, the two resume
declarations, networkLeveling's call and the declared comment edits.  Checked by
scripts/nom_hv1_verify.py (H11: one declaration per function with @brief and every @param).
"""

# ---- per-function @brief (design table 2.3) ------------------------------------------------
DOC = {
    "networkHarvestHandoverLedger":
        "The two-edge continuous handover's ledger: each anchor candidate's absolute ambiguity "
        "in this epoch's frame and its border-exact variance, consumed by the next epoch's "
        "election.\n\nRuns before the ambiguity stash (it reads each arc's first-stash variance "
        "from the previous epoch's carry) and outside the warm bracket.  No-op unless "
        "network.phase_datum_handover_continuous.",
    "networkHarvestAmbiguityStation":
        "Station @p i's ambiguities: variance and regression, the monitor feeds, the "
        "post-resume census, the stash (first-stash variance, birth floor, drop walk, birth "
        "and re-level stamps, the amb-state row), the F3 row, and the cleared reseed mark.",
    "networkAmbiguityMonitorFeeds":
        "One ambiguity's LS-VCE row and prior-residual \"pl\" row, the PASS B inject predicate "
        "reproduced, read before the stash overwrites the prior.  Measure-only.",
    "networkPruneAmbiguities":
        "The ambiguity gap prune (the registry's horizon) with its retirement census, the "
        "amb-state erase rows, the roster death buffer and PRM's prune-after-drop count.",
    "networkAmbIfTraceStation":
        "Card AIL: the ionosphere-free ambiguity's prior and posterior variance per line of "
        "sight with both bands in the block, raw and de-meaned per constellation.  "
        "Measure-only; no-op unless network.trace_amb_if.",
    "networkHarvestPhaseColourStation":
        "Card LGM: each phase-colour column's posterior, regression and F3 row, and its trace "
        "row.  No-op unless the station carries phase-colour columns under the coupling carry.",
    "networkPrunePhaseColour":
        "Card LGM: drop phase-colour entries past network.phase_colour_max_gap_s (counted).",
    "networkHarvestSlantIonoBlock":
        "network.carry_iono_in_block: each line of sight's slant ionosphere with its "
        "regression and F3 row, then the horizon prune.",
    "networkHarvestRxClock":
        "network.carry_rx_clock_in_block: receiver clock and ISB columns with their F3 rows; "
        "the primary clock's (bias, drift) filter, its gate census and jump response; the "
        "prune.",
    "networkHarvestRxCbBlock":
        "network.carry_rx_code_bias_in_block: receiver code-bias columns with their "
        "regression and F3 row, the L5B family re-seed erase and the 10-sigma step test.",
    "networkRxCbDatumGaugeCensus":
        "RXCB-DATUM: the zero-mean receiver-DCB datum's folds, counted under its own knob "
        "outside every carry bracket.",
    "networkHarvestRxCbCarry":
        "network.carry_rx_code_bias: the produce tallies, the diagonal stash (skipped under the "
        "in-block route) with its prior-residual row and L5B erase, then the gap prune.  Runs "
        "outside the warm bracket: the carry works on lineages without phase leveling.",
    "networkHarvestZwd":
        "B9: the ZWD posterior (carry, live seam window, or capture-only for the snapshot), its "
        "prior-residual row, the post-resume census, the stash and F3 row, the prune, the seam "
        "window step.",
    "networkZwdSeamWindowStep":
        "network.zwd_seam_reseed: count the window's folds, advance its retirement clock and "
        "log the decoupling edge.",
    "networkHarvestRxPhaseBias":
        "Card 3 (eq34): each receiver phase bias with its diagonal conditional variance (no "
        "coupling, by design), then the prune.  No-op unless network.eq34_rpb_q > 0.",
}

PARAM_DOC = {
    "run": "The run: its context (carry, counters, knobs), setup (stations) and engines.",
    "ep": "This epoch after the solve (the solution, the assembly, the columns and the clock "
          "are read; nothing is written).",
    "border": "This harvest's shared border (column identities, datum-frame covariance, the "
              "clone and shared-history handles).",
    "blocks": "Every station's harvest layout, by station index (ok false: not harvested).",
    "sb": "Station @p i's harvest layout (its block columns, segment rows, back-substituted "
          "locals and F3 block).",
    "i": "The station index.",
    "pa": "The ambiguity's assembly row.",
    "value_abs": "Its absolute posterior estimate (x_lin plus the back-substituted step).",
    "var": "Its posterior variance (the conditional or the diagonal harvest).",
}

# ---- the spine header (design 2.1) -----------------------------------------------------------
SPINE_HEADER = r'''#pragma once
/**
 * @file NetworkLocalCarryHarvest.hpp
 * @brief The station-local carries' post-solve harvest: its table of contents and the
 *        machinery its kinds share (card NOM3 step HV1).
 *
 * After the epoch's solve, networkLeveling (filter/NetworkLeveling.cpp) calls
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
 *      ionosphere, then receiver clock, receiver code bias and ZWD); card SCL2's compression
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
#include "filter/NetworkClockClone.hpp"      // ClockCloneHarvest
#include "filter/NetworkSharedHistory.hpp"   // SharedHistoryHarvest
#include "filter/NetworkRunTypes.hpp"        // CoupledShared, PlHarvestBlock
#include <Eigen/Core>
#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;

/// The shared border every carried column of one warm epoch regresses on (formerly four
/// locals of networkHarvestLocalCarries).  Built by networkHarvestBorderBegin, read by
/// networkHarvestColumn for every carried column, closed by networkHarvestBorderEnd after the
/// last row.  Empty on the legacy diagonal path (no network.carry_conditional_coupling).
struct HarvestBorder {
    std::map<Eigen::Index, CoupledShared> col2desc;  ///< Shared column -> its identity (clock, phase bias, code bias, SH coefficient).
    Eigen::MatrixXd      cov_df;  ///< The shared covariance in the datum frame: datum-pinned and ensemble-gauge columns zeroed.
    ClockCloneHarvest    ccl_h;   ///< Card SCL: the clones' posterior regression on the rest of the border.
    SharedHistoryHarvest shx_h;   ///< Card SCL2 X2: where each harvested row's entry was written.
};

/// The segments of a station's F3 harvest block, in the order their columns are appended.
/// The ZWD is last, so its row is the block's last (the B9-in-block invariant).  PASS B's
/// block build (the carry steps of filter/NetworkStationBuild.hpp, card NOM3 step PB1b) must
/// open a block on the same station-epochs; it pairs entries by block identity and row
/// index, not by this order.
enum class BlockSegment : int { Amb, Iono, RxClock, RxCodeBias, PhaseColour, Zwd };
inline constexpr std::size_t kBlockSegmentCount = 6;

/// One segment's rows of the block: its first row and its column count (0: absent).
struct BlockSpan { Eigen::Index row0{0}; Eigen::Index n{0}; };

/**
 * @brief The segment spans of one station's block, from the station layout's row offsets
 *        (the one place the layout arithmetic lives; tests/test_network_harvest_layout.cpp).
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
 */
void networkHarvestLocalCarries(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief The warm epoch's shared border: column identities, the datum-frame covariance and
 *        the clone and shared-history begin (inert pieces stay empty when their cards are off).
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
 *        shared-history compression (both no-ops when their cards are off).
 * @param run    The run (the context).
 * @param ep     This epoch (read by no statement; the seam form).
 * @param border This harvest's border, after the last carried row.
 */
void networkHarvestBorderEnd(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border);

} // namespace GnssProcessor
'''

SPINE_FILE_HEAD = r'''/**
 * @file NetworkLocalCarryHarvest.cpp
 * @brief The station-local carries' post-solve harvest: the orchestrator and the machinery its
 *        kinds share (the border, the column regression, the station layout).
 *
 * Card NOM3 step HV1 cut the former 1 871-line networkHarvestLocalCarries by carried kind:
 * the orchestrator below calls each kind's harvest in the file of its carry (the table of
 * contents is in app/NetworkLocalCarryHarvest.hpp).  Every function is generated by
 * scripts/nom_hv1_cut.py: its bindings, then the frozen text below them; the relocation is
 * checked by scripts/nom_hv1_verify.py.
 */

#include "app/NetworkLocalCarryHarvest.hpp"
#include "app/NetworkRun.hpp"
#include "app/NetworkEpoch.hpp"
#include "app/NetworkRunContext.hpp"   // networkResumeFirstEpochRetire (app/NetworkWarmResume.cpp)
#include "filter/NetworkAmbStateTrace.hpp"
#include "filter/NetworkChainDatum.hpp"
#include "filter/NetworkLevelingCarry.hpp"
#include "filter/NetworkPhaseColourCarry.hpp"
#include "filter/NetworkRxClockCarry.hpp"
#include "filter/NetworkRxCodeBiasCarry.hpp"
#include "filter/NetworkSlantIonoCarry.hpp"
#include "filter/NetworkZwdCarry.hpp"

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, traitsOf, SignalId, nominalCarrierFrequencyHz
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkSbasis.hpp"          // NetworkSbasis::Plan
#include "filter/StateKeySchema.hpp"         // StateKeys: ambiguity and phase-bias keys, GLONASS IFB keys

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace GnssProcessor {
'''

SPANS_DEF = r'''std::array<BlockSpan, kBlockSegmentCount> harvestBlockSpans(
    Eigen::Index iono_row0, Eigen::Index rxclk_row0, Eigen::Index rxcb_row0,
    Eigen::Index pc_row0, Eigen::Index n_cols, bool zwd_in_cols) {
    const Eigen::Index z = n_cols - (zwd_in_cols ? 1 : 0);
    return {{BlockSpan{0, iono_row0},
             BlockSpan{iono_row0, rxclk_row0 - iono_row0},
             BlockSpan{rxclk_row0, rxcb_row0 - rxclk_row0},
             BlockSpan{rxcb_row0, pc_row0 - rxcb_row0},
             BlockSpan{pc_row0, z - pc_row0},
             BlockSpan{z, zwd_in_cols ? Eigen::Index{1} : Eigen::Index{0}}}};
}'''

EPILOGUE = [
    "    // ---- The station's layout, recorded once (card NOM3 step HV1; generated) ----",
    "    sb.ok   = true;",
    "    sb.span = harvestBlockSpans(iono_row0, rxclk_row0, rxcb_row0, pc_row0,",
    "                                static_cast<Eigen::Index>(cols.size()), zwd_in_cols);",
    "    sb.cols = std::move(cols);",
    "    sb.xr   = std::move(xr);",
    "    sb.vard = std::move(vard);",
    "    sb.blk  = std::move(hblk);",
    "    return sb;",
]

CENSUS_DECLS = r'''    /// @brief The first post-resume harvest's check of one restored regression against this
    ///        epoch's fresh one: matched and unmatched terms, the largest relative coefficient
    ///        change and the RHS change, filed in the run's census (§18.12).  Measure-only.
    ///        Called for an ambiguity (L) row by networkHarvestAmbiguityStation and for a ZWD
    ///        (Z) row by networkHarvestZwd; card NOM3 step HV1 merged the two copies.
    /// @param run      The run (its context's counters and census samples).
    /// @param restored The restored carry entry (the L or Z row read from the snapshot).
    /// @param coupling This epoch's freshly derived regression of the same row.
    /// Defined in src/core/app/NetworkWarmResume.cpp.
    void networkResumeCouplingCensusRow(NetworkRun& run, const LocalCarry& restored,
                                        const std::vector<CoupledShared>& coupling);

    /// @brief Card P9a: the first post-resume fold's CTA trip count, then the first-warm-epoch
    ///        latch (resume_cpl_census_pending) retired.  Last step of the harvest.
    /// @param run The run (its context's carry and counters).
    /// Defined in src/core/app/NetworkWarmResume.cpp.
    void networkResumeFirstEpochRetire(NetworkRun& run);'''

LEVELING_PREAMBLE_OLD = """    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    NetworkSetupResult& setup = run.setup;
    const SyncedEpoch& se = ep.se;
    NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    NetworkCarryVce& carry_vce_est = run.engines.carry_vce;
    NetworkPriorResidual& prires_est = run.engines.prires;
    bool carry_warm = ep.clock.carry_warm;
    double pl_now_s = ep.clock.pl_now_s;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys =
        ep.columns.registered_code_keys;
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    // ---- End of the bindings ----
    // --- Alias preamble: mechanically-derived run-TU aliases (+ reconstructions). --
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;


    networkHarvestLocalCarries(ctx, config_, est, x_s, stations, est_index_of,
                               asm_res, plan, sh_model.get(), sh_model2.get(),
                               carry_vce_est, prires_est,
                               carry_warm, pl_now_s, registered_clock_sats,
                               shared_phase_keys, registered_code_keys);
"""

LEVELING_NEW = """    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    // ---- End of the bindings ----
    // The station-local carries' harvest, stash and prune, kind by kind
    // (app/NetworkLocalCarryHarvest.hpp, card NOM3 step HV1).
    networkHarvestLocalCarries(run, ep);
"""

# ---- declared comment edits (design 2.7, comment-only citations) ----------------------------
COMMENT_EDITS = [
    ("src/core/app/NetworkResumeRunEnd.cpp",
     "measure-only observatory in networkHarvestLocalCarries)",
     "measure-only observatory networkResumeCouplingCensusRow)"),
    ("src/core/app/NetworkStateSnapshotWrite.cpp",
     "// harvestCoupling assigns cs.s_prev",
     "// networkHarvestColumn assigns cs.s_prev"),
    ("src/core/app/NetworkStateSnapshotWrite.cpp",
     "(the harvest lambda in networkHarvestLocalCarries)",
     "(app/NetworkLocalCarryHarvest.cpp)"),
    ("src/core/filter/NetworkRedundancyCensus.cpp",
     "the pending-reseed set (NetworkLocalCarryHarvest.cpp,",
     "the pending-reseed set (NetworkLevelingCarry.cpp,"),
    ("src/core/filter/NetworkRxCodeBiasCarryRunEnd.cpp",
     "The carry is harvested in app/NetworkLocalCarryHarvest.cpp and folded",
     "The carry is harvested in filter/NetworkRxCodeBiasCarry.cpp and folded"),
    ("include/products/NetworkStateSnapshot.hpp",
     "(Card 1e; the harvest lambda in networkHarvestLocalCarries,",
     "(Card 1e; networkHarvestColumn, the former harvest lambda,"),
    ("include/products/NetworkStateSnapshot.hpp",
     "@c harvestCoupling (the harvest lambda in networkHarvestLocalCarries,",
     "@c networkHarvestColumn (the former harvest lambda harvestCoupling,"),
    ("include/products/NetworkStateSnapshot.hpp",
     "harvested (networkHarvestLocalCarries, NetworkLocalCarryHarvest.cpp) and injected",
     "harvested (networkHarvestZwd, filter/NetworkZwdCarry.cpp) and injected"),
    ("include/app/NetworkRunContext.hpp",
     "    //   NetworkLocalCarryHarvest.cpp    the station-local carries: harvest, stash, prune\n",
     "    //   NetworkLocalCarryHarvest.cpp    the station-local carries: harvest, stash, prune\n"
     "    //                                   (since card NOM3 step HV1 declared in\n"
     "    //                                   app/NetworkLocalCarryHarvest.hpp and cut by\n"
     "    //                                   carried kind; its two resume steps are in\n"
     "    //                                   NetworkWarmResume.cpp)\n"),
    ("tests/test_carry_zwd_in_block.cpp",
     "`harvestCoupling(est_idx, R.pl_zwd.col, nullptr, nullptr)` call).",
     "`networkHarvestColumn(..., est_idx, R.pl_zwd.col, nullptr, nullptr)` call)."),
    ("tests/test_resume_coupling_restore.cpp",
     "//    harvestCoupling assigns both values",
     "//    networkHarvestColumn assigns both values"),
]
