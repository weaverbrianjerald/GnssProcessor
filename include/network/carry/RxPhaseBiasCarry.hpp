#pragma once
/**
 * @file RxPhaseBiasCarry.hpp
 * @brief The eq34 chain's receiver phase bias carried inside the station's local block
 *        (network.phase_datum_eq34_rpb_in_block).
 *
 * The receiver phase bias of the eq34 chain datum is a local column of every station's block.
 * Its carry has two routes:
 *
 *   route      inject (PASS B)                              harvest (after the solve)
 *   diagonal   the time-link row of networkPhaseDatumPinRows  networkHarvestRxPhaseBias
 *              (network/solve/PhaseDatum.cpp)                  (network/solve/ChainDatum.cpp)
 *   in block   networkRxPbBlockColumns (here)                 networkHarvestRxPbBlock (here)
 *
 * The diagonal route folds a prior toward the carried value at the column's conditional
 * variance.  The station's block already carries the ambiguities, the slant ionosphere and the
 * receiver code bias with their cross covariances, which hold the same information, so the
 * diagonal fold counts it again every epoch and the bias's formal sigma collapses with no new
 * information behind it.  In the block the bias is one more column of the same whitened block:
 * its regression on the shared border is harvested with it, and its overlap with the station's
 * other columns is the harvested cross covariance.  There is no diagonal fallback: a column
 * that does not fold through the block carries nothing that epoch, and is counted.
 *
 * Both routes write the same carried entry (net::StationSignalState::rpb), which the chain's
 * root row reads when a new datum receiver is pinned at its carried value, and both share the
 * gap prune of networkHarvestRxPhaseBias.  The pinned bias of a datum receiver is in neither
 * route: it is not recorded in StationAssembly::pl_rpb.
 *
 * Event responses of the carried column, each counted (network/assembly/Responses.hpp):
 *   - the chain's unknown step or cut release erases the entry (rpb_eq34_erase);
 *   - a known frame shift moves the value and the regression's terms (eq34ShiftCarry);
 *   - an entry past the gap horizon is not folded (rpb_gap_skip) and is pruned (rpb_gap_prune);
 *   - a step between two harvested values beyond rxPbStepExceeds erases the entry
 *     (rpb_step_erase, this route only).
 */

#include <Eigen/Core>

#include <vector>

namespace GnssProcessor {

struct StationCarryFrame;     // network/assembly/StationBuild.hpp
struct NetworkRun;            // network/driver/Run.hpp
struct NetworkEpoch;          // network/driver/Epoch.hpp
struct HarvestBorder;         // network/carry/LocalCarryHarvest.hpp
struct StationHarvestBlock;   // network/carry/LocalCarryHarvest.hpp
struct NetworkRunEndView;     // network/runend/RunEnd.hpp

/// The step test's threshold, in sigma of the two harvested variances, the walk and the guard.
inline constexpr double kRxPbStepSigma = 10.0;
/// The step test's guard variance, (0.05 m)^2: with both harvested variances at zero a step of
/// 0.5 m still trips, about two and a half L1 cycles, and estimation noise does not.
inline constexpr double kRxPbStepGuardVarM2 = 0.0025;

/**
 * @brief The in-block harvest's step test: whether this harvest's receiver phase bias stepped
 *        away from the carried one by more than the two harvests, the random walk over the gap
 *        and the guard allow.
 *
 * Both values are absolute realizations in the chain's current frame (the election has already
 * moved or erased the carried one for every frame step it made), so the test is frame-invariant.
 * A gap outside [0, @p max_gap_s] is no test: the gap gate and the prune own that entry.
 *
 * @param value_now      This harvest's absolute value (m).
 * @param var_now        Its conditional variance (m^2).
 * @param value_carried  The carried absolute value (m).
 * @param var_carried    Its conditional variance (m^2).
 * @param q_m2s          The bias's random-walk process noise (m^2/s).
 * @param gap_s          This harvest's time less the carried entry's (s).
 * @param max_gap_s      The carry's gap horizon (s).
 * @return True when the step exceeds kRxPbStepSigma times the root of the summed variances.
 */
bool rxPbStepExceeds(double value_now, double var_now, double value_carried, double var_carried,
                     double q_m2s, double gap_s, double max_gap_s);

/// What the block-column step does with one carried receiver phase bias.
enum class RxPbBlockAction {
    Fold,          ///< The entry folds as a column of the station's block.
    SkipGap,       ///< Its gap is outside [0, horizon]: the decoupling edge of a gap (rpb_gap_skip past the horizon).
    SkipNoBlock,   ///< It holds no row of a harvest block that the station's pending columns share.
    SkipVariance,  ///< Its variance plus the walk is not a positive number.
};

/**
 * @brief The block-column step's decision for one carried receiver phase bias (a pure function,
 *        so the gap gate and the block rule are tested apart from the station build).
 *
 * An entry folds when its gap is inside the horizon, it holds a row of a harvest block and
 * that block is shared by another pending column of the station (or no other column is
 * pending), and its variance plus the random walk over the gap is positive. An entry whose
 * block no pending column shares would fold with every cross term zeroed, that is, as a
 * diagonal prior at the conditional variance: the double count the route removes.
 *
 * @param gap_s           This epoch's time less the entry's stash time (s).
 * @param max_gap_s       The carry's gap horizon (s).
 * @param var             The entry's conditional variance (m^2).
 * @param q_m2s           The bias's random-walk process noise (m^2/s).
 * @param has_block_row   The entry holds a row of a harvest block.
 * @param others_pending  Another carried column of the station is pending for the block.
 * @param block_shared    One of those columns was harvested into the entry's block.
 * @param var_eff         Written on Fold: @p var + @p q_m2s * @p gap_s (m^2).
 * @return The action.
 */
RxPbBlockAction rxPbBlockGate(double gap_s, double max_gap_s, double var, double q_m2s,
                              bool has_block_row, bool others_pending, bool block_shared,
                              double& var_eff);

// ---- PASS B station steps, declarations ----

/**
 * @brief Collects the station's carried receiver phase-bias columns for its local block.
 *
 * Step `rx_pb_block` of kStationCarrySteps (network/assembly/StationBuild.hpp); it creates no
 * row itself (the local-block step builds the rows, kind BlockRxPbCarry). It returns at once
 * unless network.phase_datum_eq34_rpb_in_block is live and the leveling carry injects here.
 * A column folds when its entry is inside the gap horizon and holds a row of a harvest block
 * that another pending column of the station shares (or no other column is pending); an entry
 * with no such row would fold as a diagonal prior, the double count this route removes, so it
 * is left out, counted, and harvested again this epoch.
 * Records: pl_rxpb_no_block_skip, prm_rpb_gap_skip; rpb_bc.
 * @param carry The station's carry frame (the build, the block columns collected so far).
 */
void networkRxPbBlockColumns(StationCarryFrame& carry);

// ---- End of the PASS B station steps, declarations ----

// ---- Harvest half, declarations ----

/**
 * @brief network.phase_datum_eq34_rpb_in_block: the unpinned receiver phase-bias columns with
 *        their border regression and their row of the station's harvest block.
 *
 * Called by networkHarvestLocalCarries after the receiver code bias and before the border is
 * closed (every writer of a block row runs before networkHarvestBorderEnd). The step test runs
 * before the stash; an entry it erases is not stashed again this epoch.
 * @param run    The run: its context (the station table, counters, knobs) and engines.
 * @param ep     This epoch after the solve (the solution, the assembly and the clock are read).
 * @param border This harvest's border (the regression's identities; the shared-history notes).
 * @param blocks Every station's harvest layout (the receiver phase-bias segment's rows).
 */
void networkHarvestRxPbBlock(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border,
                             const std::vector<StationHarvestBlock>& blocks);

// ---- End of the harvest half, declarations ----

/// @brief Run-scope counters of RxPhaseBiasCarry, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RxPhaseBiasCarryCounters {
    // The block's side (counted per station in the parallel produce, folded by
    // networkLocalBlockTally).  An armed run with n_pl_blk_rxpb_cols == 0 is unwired, not
    // clean; the not-folded count is a "carried nothing" count (no diagonal fallback).
    long long n_pl_blk_rxpb_cols = 0;           ///< receiver phase-bias columns admitted into a block build
    long long n_pl_blk_rxpb_not_folded = 0;     ///< receiver phase-bias columns pushed but not folded (non-SPD block)
    long long n_pl_blk_rxpb_missing_cross = 0;  ///< (receiver phase-bias, other) pairs zeroed for a missing harvest cross term
    long long n_rxpb_blk_no_block_skip = 0;     ///< carried entries left out of the block for want of a shared harvest-block row
    // The harvest's side.
    long long n_rxpb_blk_harvested = 0;         ///< entries stashed with a border regression and a harvest-block row
    long long n_rxpb_blk_harvest_fail = 0;      ///< columns whose coupling harvest failed (not stashed; an existing entry is left as it is)
    long long n_rxpb_blk_no_block = 0;          ///< entries stashed without a harvest-block row (left out of the block next epoch)
    long long n_rxpb_blk_step_reset = 0;        ///< entries erased by the step test (the event response to a jump in the carried value)
};

// ---- Run-end reports ----

/**
 * @brief Run-end report: the receiver phase bias carried in the station block.
 *
 * One row of kNetworkRunEndReports. Prints nothing unless
 * network.phase_datum_eq34_rpb_in_block is set; warns when the key is set and no column
 * entered a block over the run (inert wiring).
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0.
 */
int networkRxPhaseBiasCarryRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
