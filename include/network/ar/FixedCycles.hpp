#pragma once
/**
 * @file FixedCycles.hpp
 * @brief The fixed-cycle fold (@c network.ambiguity_resolution.fixed_cycles): the
 *        integer L1/L2 pairs the producer AR search fixed, folded into a detached held solve of
 *        the shared parameters that only the emission reads.
 *
 * ## What it does
 *
 * The producer AR search (network/ar/ProducerAr.cpp, on the arc graph's cycle basis) fixes about
 * half the between-station double differences (DDs) of the settled float, and the float stays
 * float.  This seam folds those integers into a SECOND solve of the shared parameters (clocks,
 * phase biases, code biases), made from a copy of the float solve's own reduced normals
 * (@ref NetworkEstimator::solveHeldDetached), beside the float solve of the same epoch.  The
 * estimator, the carried ambiguities, the search, the DD trace and post-fit QC keep the float
 * (with zero-q ambiguities the float at an epoch already holds the whole
 * history, so a hold fed back into the filter would add only cross terms and would let the hold
 * confirm itself).  Under @c fold: emit, solution prep and the emitter read the held vector;
 * every other reader keeps the float.
 *
 * ## One epoch, source epoch (step FCF-2, the same-epoch probe)
 *
 *  1. **Units** (@ref fcyc::epochUnits).  A unit is one DD identity (signal pair, sta_r,
 *     ref_sta, sat_j, ref_sat) of a pair family that the search determined in BOTH bands at
 *     this epoch: two integers.  A DD determined in one band only, or a band-only DD, is never a
 *     unit (its single-band test has little power).
 *  2. **The per-unit test** (@ref fcyc::scoreUnits).  T = v' Sigma_pair^-1 v, v = y - N in
 *     cycles, Sigma_pair the pair's float covariance from the search's own two-by-two readback:
 *     chi-square with 2 degrees of freedom under right integers, exactly.  A unit above
 *     chi2_2(1 - @c unit_alpha) is not placed (the overall block test
 *     cannot see one wrong (1, 1) pair among the rows of a network-sized block).
 *  3. **Placement** (@ref fcyc::placeUnits).  Each unit becomes two @ref
 *     NetworkNormalEquations::CycleRow (one per band) on the live ambiguity columns of its four
 *     band-arcs, with the target lambda N - sum c x_lin re-based on this epoch's linearization
 *     points and the hold variance (sigma lambda)^2; one block per connected station
 *     component, so no two blocks share a station.
 *  4. **The held solve** (@ref NetworkEstimator::solveHeldDetached).  A refused float
 *     reproduction, a non-finite held estimate, a kernel throw or a shift above
 *     @c max_shift_m on any shared column leaves the epoch float, counted.
 *  5. **Acceptance** (@ref fcyc::foldUnits).  Each block's own statistic against
 *     chi2_K(1 - @c gate_alpha); a failing block drops its unit of largest T and refolds, up to
 *     @c max_iter passes; a block that still fails stays float for the epoch.
 *
 * Source epoch has no memory and nothing to release: each epoch stands alone.
 *
 * ## One epoch, source lattice (step FCF-3, the hold under Observe)
 *
 * The units are the L1/L2 pairs the ledger of fixed cycles (network/ar/ArLattice.hpp)
 * implies.  The seam steps the ledger and the fold together, in this order:
 *
 *  1. **The ledger opens** (@ref arLatticeBegin): a skipped epoch releases everything; the
 *     evidence releases the arcs whose life ended (the search's readings, the landed
 *     within-epoch replay, this epoch's gross-LOS Decide); a fix that contradicts the ledger
 *     releases the arcs of its cycle.
 *  2. **A planted integer** (the event harness's @c fixed_cycle_plant), when one is due.
 *  3. **Re-validation** (@ref fcyc::revalidateUnits).  Every pair whose two cycles the ledger
 *     implies from rows entered before this epoch is a unit, tested against the search's float
 *     exactly as in step 2 above.  An arc shared by two or more failing units is eliminated; a
 *     failing unit with no such arc releases the arcs of its two cycles.  A passing unit whose
 *     rows those releases took is not placed (@ref UnitAction::Released).
 *  4. **Admission** (@ref fcyc::admitFixes): fixes that kept one integer for @c persist_ep
 *     epochs enter the ledger; they fold from the next epoch.  A contradiction among the
 *     epoch's own fixes releases the arcs of its cycle, and a unit those arcs took is marked
 *     released here too.
 *  5. **The lapse** (@ref fcyc::markLapsed, @c lapse_ep; 0, the default, is off).  A unit whose
 *     pair the search has not determined for @c lapse_ep consecutive epochs is not placed
 *     (@ref UnitAction::Lapsed); its rows stay in the ledger.
 *  6. **The fold**, as for source epoch (placement, held solve, acceptance).  A unit dropped by
 *     acceptance, or in a degenerate block, releases the arcs of its cycles
 *     (@ref fcyc::foldReleases), through the ledger's one release function.
 *  7. **The edges**: each folded unit's onset and offset (with the cause of the offset), and
 *     the late post-fit flags (RAIM T3 / T4 and the gross-LOS Act act after the emission, so a
 *     flag on an arc of a unit folded at t is read through the life stamp at t + 1).
 *
 * Every verdict on a cycle releases its arcs, never the rows behind it alone
 * (network/ar/ArLattice.hpp): other rows can hold one of its arcs and would otherwise keep that
 * arc's old integer.
 *
 * ## What the tests cannot see
 *
 * Steps 2 and 6 see a wrong integer only where the float disagrees with it.  A slip no detector
 * flags leaves the zero-q float carry on the wrong integer, so v is small and both tests pass it.
 * On the S8 twin without the slip fixes the stale pair is held 64 epochs and
 * folded at 63, its T at most 8.6 against a bar of 27.6: the search fixes the stale integer for
 * 30 epochs, and the ledger holds it for 34 more after the search stops, with no guard firing,
 * until its arc leaves the pool one epoch before the run ends.  The lapse (step 5) bounds the
 * second part; the guard against the first is the slip fixes, which the
 * loader requires under Emit and names when missing under Observe.  Block acceptance and an
 * isolated failure blame the unit of largest T, and a stale unit's T stays small, so where the
 * zero-q carry has warped a station they can release innocent units there and keep the stale one
 *.
 *
 * ## What it writes
 *
 * Its tallies (@c ctx.counters.fixed_cycles) and its run state (@c ctx.carry.fixed_cycles, the
 * folded runs), and under @c fixed_cycles.trace the files of logs/ (@ref kFixedCyclesHeader,
 * @ref kFixedCycleShiftHeader, and under source lattice @ref kFixedCycleEventsHeader, in
 * network/diag/RunTraces.hpp) and the run-end report @ref networkFixedCyclesRunEnd.  Nothing
 * the solve reads.  Not snapshotted and not fingerprinted; the loader refuses
 * @c network.state_snapshot_path under Emit, so an Emit leg neither writes nor resumes a
 * snapshot.  The ledger's own files are written by network/ar/ArLattice.cpp.
 */

#include "common/GnssIdentity.hpp"          // ArcId, SignalId
#include "network/ar/ArCycleBasis.hpp"   // ArEpochFixes, SearchedCycle, ArcColumn, ArDdIdentity
#include "network/ar/ArLattice.hpp"      // ArLatticeReleaseEvent, arcgraph::CycleLattice
#include "network/solve/Estimator.hpp"      // NetworkEstimator::Datum, HeldSolve
#include "network/solve/NormalEquations.hpp" // NetworkNormalEquations::CycleRow

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetStation;           // network/driver/RunTypes.hpp
struct NetworkRunContext;    // network/state/RunContext.hpp
struct NetworkRunEndView;    // network/runend/RunEnd.hpp
struct NetworkRunTraces;     // network/diag/RunTraces.hpp
struct SyncedEpoch;          // rinex/MultiRinexSync.hpp
struct WithinEpochCulprits;  // network/qc/WithinEpochCulprits.hpp

namespace fcyc {

/// One fold unit: a DD identity with an integer in both bands of a pair family (file banner,
/// step 1 of each source).
struct Unit {
    int       l1{-1};     ///< Index in @ref ArEpochFixes::floats of the shorter-wavelength band's DD.
    int       l2{-1};     ///< Index of the longer-wavelength band's DD (its twin).
    long long n1{0};      ///< The integer of @c l1 (cycles).
    long long n2{0};      ///< The integer of @c l2 (cycles).
    double    t{0.0};     ///< The unit statistic v' Sigma_pair^-1 v (chi-square, 2 dof, under right integers); NaN when the pair covariance is not positive definite.
    bool      fail{false};///< T above the unit bar, or not computable: not placed.
    bool      released{false};  ///< (source lattice) It passed, but a release of this epoch (the re-validation's, or an admission contradiction's) took the rows behind it: not placed.
    bool      lapsed{false};    ///< (source lattice) The search has not determined its pair for @c lapse_ep consecutive epochs: not placed (its rows stay).
};

/**
 * @brief This epoch's determined L1/L2 pairs as units (the source epoch).
 *
 * One unit per pair of @ref ArEpochFixes::floats twins that are both @c fixed, in basis order
 * of the shorter-wavelength DD; band 1 is the shorter wavelength.  A twin that was not
 * determined, and a band-only DD, give no unit.
 *
 * @param fixes The search's hand-over.
 * @return The units, unscored.
 */
std::vector<Unit> epochUnits(const ArEpochFixes& fixes);

/**
 * @brief The unit statistic of one pair: T = v' Sigma^-1 v with v = y - N.
 * @param y1,y2     The float DDs (cycles).
 * @param n1,n2     The integers.
 * @param var1,var2 Their float variances (cycles squared).
 * @param cov12     Their float covariance (cycles squared).
 * @return T; NaN when the two-by-two covariance is not positive definite or not finite.
 */
double unitStatistic(double y1, double y2, long long n1, long long n2, double var1, double var2,
                     double cov12);

/**
 * @brief Score every unit (file banner, step 2): its T, and @c fail when T is above
 *        chi2_2(1 - @p alpha) = -2 ln(alpha) or not computable.
 * @param units The units (scored in place).
 * @param fixes The search's hand-over (the floats the units index).
 * @param alpha The false-refusal probability per unit-epoch.
 */
void scoreUnits(std::vector<Unit>& units, const ArEpochFixes& fixes, double alpha);

/**
 * @brief The units the ledger implies (source lattice; file banner, step 3).
 *
 * One unit per pair of @ref ArEpochFixes::floats twins whose two cycles the lattice implies,
 * each from rows entered before @p ep (an added row folds from the next epoch); n1 and n2 are
 * the implied integers.  Basis order of the shorter-wavelength DD; a band-only DD gives none.
 *
 * @param L     The ledger's lattice.
 * @param fixes The search's hand-over (the floats).
 * @param ep    The loop epoch.
 * @return The units, unscored.
 */
std::vector<Unit> impliedUnits(const arcgraph::CycleLattice& L, const ArEpochFixes& fixes, long long ep);

/**
 * @brief The re-validation's releases (file banner, step 3).
 *
 * Over the failing units: each band-arc found in two or more of them is eliminated (cause
 * Revalidation, ascending ArcId); each failing unit with no such arc releases the arcs of both
 * its cycles (cycle releases, cause Revalidation, the shorter wavelength first).  Two units that
 * fail on one arc point at that arc; a unit that fails alone points at its own arcs, any of
 * which may have moved.
 *
 * @param units The scored units.
 * @param fixes The search's hand-over (their cycles and identities).
 * @return The releases, for @ref arLatticeRelease.
 */
std::vector<ArLatticeReleaseEvent> localizeFailures(const std::vector<Unit>& units, const ArEpochFixes& fixes);

/**
 * @brief Mark the passing units whose integers the ledger no longer implies (their rows went
 *        with a release of this epoch) as released.
 * @param units The scored units (marked in place; a unit already marked stays marked).
 * @param L     The lattice after the releases.
 * @param fixes The search's hand-over.
 */
void markReleased(std::vector<Unit>& units, const arcgraph::CycleLattice& L, const ArEpochFixes& fixes);

/**
 * @brief Step 3 of a source-lattice epoch (file banner): the ledger's units, re-validated.
 *
 * The units the ledger implies (@ref impliedUnits) are scored (@ref scoreUnits); the failures'
 * releases (@ref localizeFailures) are applied through @ref arLatticeRelease; the passing units
 * whose rows they took are marked released (@ref markReleased).  @p le counts the units and the
 * failures.
 *
 * @param S     The ledger.
 * @param fixes The search's hand-over.
 * @param ep    The loop epoch.
 * @param alpha The unit test's false-refusal probability per unit-epoch.
 * @param le    The ledger epoch.
 * @return The units.
 */
std::vector<Unit> revalidateUnits(ArLatticeState& S, const ArEpochFixes& fixes, long long ep, double alpha,
                                  ArLatticeEpoch& le);

/**
 * @brief Step 4 of a source-lattice epoch (file banner): admission (@ref arLatticeAdmit), then
 *        the units a contradiction among the epoch's own fixes took are marked released, so a
 *        unit implied before admission is not folded from rows admission released.
 * @param S          The ledger.
 * @param fixes      The search's hand-over.
 * @param persist_ep Consecutive epochs before a fix is offered.
 * @param ep         The loop epoch.
 * @param le         The ledger epoch.
 * @param ledger_units This epoch's ledger units (@ref revalidateUnits; marked in place), or null
 *                   when the fold does not read the ledger (@c trace_lattice alone, or source
 *                   epoch, whose units are not the ledger's).
 */
void admitFixes(ArLatticeState& S, const ArEpochFixes& fixes, int persist_ep, long long ep, ArLatticeEpoch& le,
                std::vector<Unit>* ledger_units);

/**
 * @brief Step 5 of a source-lattice epoch (file banner): the lapse.
 *
 * A unit's count is the consecutive searched epochs at which the ledger implied it and the search
 * did not determine its pair (both bands @c fixed); an epoch that determined it resets the count
 * to 0, and an epoch at which the unit is not implied ends it.  A unit whose count reaches
 * @p lapse_ep is marked lapsed (not placed; its rows stay).  @p lapse_ep 0 (off) marks nothing
 * and keeps no counts.
 *
 * @param units    This epoch's units (marked in place).
 * @param fixes    The search's hand-over.
 * @param unkept   The counts, by the unit's shorter-wavelength DD identity (replaced by this
 *                 epoch's).
 * @param lapse_ep Consecutive epochs after which a unit lapses (0: off).
 */
void markLapsed(std::vector<Unit>& units, const ArEpochFixes& fixes, std::map<ArDdIdentity, int>& unkept,
                int lapse_ep);

/**
 * @brief The chi-square quantile with @p dof degrees of freedom at probability @p p.
 *
 * Exact to the bisection's resolution: the root of the regularized lower incomplete gamma
 * function P(dof / 2, x / 2) = p (series below a + 1, continued fraction above), with the
 * closed form -2 ln(1 - p) for two degrees of freedom.
 *
 * @param dof Degrees of freedom (at least 1).
 * @param p   Probability, in (0, 1).
 * @return The quantile.
 * @throws std::invalid_argument on dof below 1 or p outside (0, 1).
 */
double chi2Quantile(int dof, double p);

/// The units placed in the estimator (file banner, step 3).
struct FoldPlan {
    std::vector<std::vector<NetworkNormalEquations::CycleRow>> blocks;  ///< One per connected station component; rows 2k and 2k+1 are unit @c block_units[b][k]'s two bands.
    std::vector<std::vector<int>> block_units;  ///< Per block: the unit indices, in placement order.
    std::vector<int>              unplaced;     ///< Active units with a band-arc that has no live column (not folded).
};

/**
 * @brief Place the active units on the estimator's local ambiguity columns.
 *
 * Each band of a unit gives one row: its cycle's signed terms on the live columns of its
 * band-arcs (@ref ArEpochFixes::columns), the target lambda N - sum c x_lin (m) and the hold
 * variance (@p sigma_cyc lambda)^2.  The blocks are the connected components of the rows'
 * estimator stations (union-find), ordered by their first unit, so no two blocks share a
 * station.
 *
 * @param units     The units.
 * @param active    Per unit: 1 when it is to be placed.
 * @param fixes     The search's hand-over (floats and columns).
 * @param sigma_cyc The hold sigma (cycles of each row's band).
 * @return The plan.
 */
FoldPlan placeUnits(const std::vector<Unit>& units, const std::vector<char>& active,
                    const ArEpochFixes& fixes, double sigma_cyc);

/// What became of a unit at this epoch (the trace's @c action column).
enum class UnitAction : std::uint8_t {
    Folded,         ///< In the accepted held solve.
    UnitRefused,    ///< Above the per-unit bar (step 2): not placed.
    Unplaced,       ///< A band-arc without a live column: not placed.
    Dropped,        ///< Dropped from a block that failed acceptance (the block's largest T).
    LeftFloat,      ///< In a block that still failed acceptance after @c max_iter passes.
    Degenerate,     ///< In a block the kernel's screens refused (dependent rows or a bad pivot).
    FloatMismatch,  ///< The held solve was refused: the cached system did not reproduce the float.
    NonFinite,      ///< The held solve was refused: the held estimate is not finite.
    KernelError,    ///< The kernel threw: the epoch stays float.
    ShiftRefused,   ///< The held estimate moved a shared column beyond @c max_shift_m: the epoch stays float.
    Released,       ///< (source lattice) Passed its test, but a release of this epoch took the rows behind it: not placed.
    Lapsed,         ///< (source lattice) The search has not determined its pair for @c lapse_ep epochs: not placed.
    Count
};

/// The number of @ref UnitAction values.
inline constexpr std::size_t kUnitActionCount = static_cast<std::size_t>(UnitAction::Count);

/**
 * @brief The trace name of an action: folded, unit_refused, unplaced, dropped, left_float,
 *        degenerate, float_mismatch, non_finite, kernel_error, shift_refused, released, lapsed.
 * @param a The action.
 * @return The name.
 */
const char* unitActionName(UnitAction a);

/// A block's acceptance figures at the pass that decided its units.
struct BlockFigures {
    int    k{0};               ///< Rows.
    double chi2{0.0};          ///< Its own statistic (@ref NetworkEstimator::HeldSolve::chi2).
    double chi2_expect{0.0};   ///< Its mean under right integers.
    double bar{0.0};           ///< chi2_K(1 - gate_alpha).
};

/// One unit's fate at this epoch.
struct UnitFate {
    UnitAction   action{UnitAction::UnitRefused};  ///< What became of it.
    int          block{-1};   ///< Its block at the deciding pass (-1 when never placed).
    int          pass{-1};    ///< The deciding pass (-1 when never placed).
    BlockFigures fig;         ///< Its block's figures at that pass (zero when never placed or the block degenerate).
};

/// The knobs of one fold, read once per epoch from the configuration.
struct FoldKnobs {
    double sigma_cyc{0.003};    ///< Hold sigma (cycles).
    double gate_alpha{1e-4};    ///< Block acceptance probability.
    int    max_iter{3};         ///< Drop-and-refold passes.
    double max_shift_m{0.5};    ///< Shift bound (m).
};

/// One epoch's fold (file banner, steps 3 to 5).
struct EpochFold {
    std::vector<UnitFate>       fate;       ///< Per unit.
    FoldPlan                    plan;       ///< The plan of the last pass.
    NetworkEstimator::HeldSolve held;       ///< The held solve of the last pass (its @c x_s is the float unless accepted).
    std::vector<BlockFigures>   blocks;     ///< The last pass's blocks (degenerate ones zero).
    bool        accepted{false};            ///< A held solve with at least one folded block passed every guard.
    int         passes{0};                  ///< Held solves run.
    int         failures{0};                ///< Block acceptance failures, over every pass.
    std::string error;                      ///< The kernel's message on a throw.
};

/**
 * @brief Fold the scored units into a held solve with acceptance (file banner, steps 3 to 5).
 *
 * Places every unit that passed its test, solves, and refuses the epoch on a throw, a refused
 * float reproduction, a non-finite held estimate or a shift beyond the bound.  Each block whose
 * own statistic exceeds its bar drops its unit of largest T and the units are refolded, up to
 * @c max_iter passes; at the last pass a block that still fails drops all its units (left
 * float) and the rest are folded once more.  A block's own statistic depends on its rows alone,
 * so dropping one block does not move another's verdict.
 *
 * @param est   The estimator after this epoch's float solve.
 * @param datum The datum of that solve.
 * @param x_s   The float shared estimate.
 * @param units The scored units.
 * @param fixes The search's hand-over.
 * @param k     The knobs.
 * @return The epoch's fold.
 */
EpochFold foldUnits(const NetworkEstimator& est, const NetworkEstimator::Datum& datum,
                    const Eigen::VectorXd& x_s, const std::vector<Unit>& units,
                    const ArEpochFixes& fixes, const FoldKnobs& k);

/**
 * @brief The fold's releases (source lattice; file banner, step 6): a unit dropped by block
 *        acceptance releases the arcs of both its cycles (cause Acceptance), and a unit in a
 *        degenerate block likewise (cause Degenerate).  A unit left float in a block that still
 *        failed after the last pass, or refused by a guard of the whole epoch, keeps its rows.
 * @param units The units.
 * @param f     Their fold.
 * @param fixes The search's hand-over.
 * @return The releases, for @ref arLatticeRelease.
 */
std::vector<ArLatticeReleaseEvent> foldReleases(const std::vector<Unit>& units, const EpochFold& f,
                                                const ArEpochFixes& fixes);

}  // namespace fcyc

/// One unit's current folded run (source lattice; @ref FixedCycleState).
struct FixedCycleRun {
    long long          since{0};  ///< Loop epoch the run began (its onset).
    long long          n1{0};     ///< The integer of its shorter-wavelength band.
    long long          n2{0};     ///< The integer of its longer-wavelength band.
    SignalId           sig2{};    ///< The longer-wavelength band's signal.
    std::vector<ArcId> arcs;      ///< Its eight band-arcs (the late post-fit flags are read on them).
};

/// The fold's run state (@c ctx.carry.fixed_cycles; source lattice): the units folded at the
/// last fold step, for the onset and offset edges and the late post-fit flags, and the lapse
/// counts.  Not snapshotted.
struct FixedCycleState {
    std::map<ArDdIdentity, FixedCycleRun> folded;  ///< By the unit's shorter-wavelength DD identity.
    long long last_ep{-1};                         ///< Loop epoch of that step (-1: none).
    std::map<ArDdIdentity, int> unkept;            ///< Per implied unit: consecutive epochs the search has not determined its pair (@ref fcyc::markLapsed; empty unless @c lapse_ep is set).
};

/// Run tallies of the fixed-cycle fold (standing invariant 4: an armed fold that never held a
/// solve says so).  All zero unless the fold is armed.
struct FixedCycleCounters {
    long long epochs{0};            ///< Seam calls with the fold armed.
    long long epochs_searched{0};   ///< ...of those, with the cycle basis built (the search ran).
    long long epochs_with_pair{0};  ///< ...of those, with at least one unit (a pair determined in both bands).
    long long held_solves{0};       ///< Epochs with an accepted held solve.
    long long emitted{0};           ///< Emit: epochs whose held solve was handed to solution prep and the emitter.
    long long passes{0};            ///< Held solves run, over every pass.
    long long units{0};             ///< Units, summed over epochs.
    std::array<long long, fcyc::kUnitActionCount> by_action{};  ///< Units by their fate.
    long long blocks_folded{0};     ///< Blocks in accepted held solves.
    long long blocks_degenerate{0}; ///< Blocks the kernel's screens refused.
    long long blocks_dependent{0};  ///< ...of those, with linearly dependent rows.
    long long blocks_failed{0};     ///< Block acceptance failures, over every pass.
    long long epochs_refused{0};    ///< Epochs with a pair and no accepted held solve.
    long long factor_nnz_max{0};    ///< Largest summed factor size of a held solve.
    std::vector<double> unit_t;         ///< Every unit's T.
    std::vector<double> chi2_ratio;     ///< Per folded block of an accepted solve: chi2 / chi2_expect.
    std::vector<double> expect_per_dof; ///< ...: chi2_expect / K.
    std::vector<double> block_k;        ///< ...: K.
    std::vector<double> repro;          ///< float_repro_rel of every held solve run.
    std::vector<double> shift_m;        ///< max_shift_m of every accepted held solve.
    std::vector<double> units_per_epoch;///< Folded units per accepted epoch.
    std::vector<double> seam_ms;        ///< Seam cost per epoch with a pair (ms).
    // --- source lattice (step FCF-3)
    std::vector<double> implied_per_epoch; ///< Units the ledger implied, per searched epoch.
    long long plants{0};       ///< Planted integers applied (the event harness's fixed_cycle_plant).
    long long onsets{0};       ///< Folded runs begun (re-coupling edges).
    long long offsets{0};      ///< Folded runs ended (decoupling edges).
    long long late_t3t4{0};    ///< Arcs of a unit folded at the step before whose life stamp now carries a post-fit flag.
    long long folded_unkept{0};///< Folded unit-epochs whose pair the search did not determine at that epoch (held by the ledger alone).
};

/// What the seam hands the driver.
struct FixedCycleEmission {
    bool            emit{false};  ///< Emit armed and a held solve accepted: solution prep and the emitter read @c x_held.
    Eigen::VectorXd x_held;       ///< The held shared estimate (empty unless @c emit).
};

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief The fixed-cycle fold and the ledger of fixed cycles at this epoch (SB_SOLPREP, right
 *        after the gross-LOS Decide).
 *
 * Returns at once (nothing emitted, nothing counted) unless the fold is armed or the ledger
 * runs (@ref arLatticeArmed).  An epoch whose search did not run steps nothing (counted; the
 * ledger's next step releases everything).  Otherwise it runs the file banner's steps for the
 * source in force (the ledger alone under @c trace_lattice without the fold), adds to
 * @c ctx.counters.fixed_cycles and @c netar_lattice, writes the trace files that are open, and
 * returns the held vector only under Emit with an accepted held solve.  The estimator is not
 * modified.
 *
 * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log), run.engines.est
 *            (est), run.setup.stations (stations).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se), ep.solve.x_s
 *           (x_s), ep.solve.plan.shared_datum (datum), ep.ar_fixes (fixes), ep.solve.we_landed
 *           (we).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: [in,out] run context: @c carry.ar_lattice, @c carry.fixed_cycles,
 *     @c counters.fixed_cycles, @c counters.netar_lattice, @c inject (a planted integer);
 *     reads @c counters.n_loop_epochs and the gross-LOS lists.
 *   - @p config_ = run.config: The run configuration (the knobs).
 *   - @p log = run.log: "main_log".
 *   - @p traces = run.traces: Trace-stream owner: the fold's files, the ledger's files,
 *     @c events_trace.
 *   - @p se = ep.se: The epoch.
 *   - @p est = run.engines.est: The estimator after this epoch's float solve.
 *   - @p x_s = ep.solve.x_s: The float shared estimate.
 *   - @p datum = ep.solve.plan.shared_datum: The datum of the float solve
 *     (@c plan.shared_datum).
 *   - @p fixes = ep.ar_fixes: The search's hand-over (@ref networkProducerAr's return).
 *   - @p we = ep.solve.we_landed: The landed within-epoch replay's culprits
 *     (@c NetworkSolveGlueResult::we_landed).
 *   - @p stations = run.setup.stations: The network stations (names of the trace rows).
 *
 * @return What the emission reads.
 */
FixedCycleEmission networkFixedCycles(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Run-end report: the fixed-cycle fold.
 *
 * The epochs with a pair and the held solves, the units by fate, the blocks, the refusals, the
 * unit statistic's quantiles and exceedance, block acceptance against the stated mean, the
 * float reproduction and the shift, the cost; under source lattice also the units implied,
 * the re-validation's trips, the edges and the late post-fit flags.  Warns (standing
 * invariant 4) when the held solves differ from the epochs with a pair, when armed and nothing
 * was ever held, on any kernel error, float mismatch or degenerate block, and (source lattice)
 * when no unit was ever implied.
 *
 * Gate: returns 0 at once unless the fold is armed.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkFixedCyclesRunEnd(const NetworkRunEndView& v);

/// @brief Run-scope counters of FixedCycles, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct FixedCyclesCounters {
    /// The fixed-cycle fold (network/ar/FixedCycles.hpp): epochs with a
    /// determined pair and held solves, units by fate, blocks, the guards' refusals, the
    /// unit statistic, block acceptance against its stated mean, the float reproduction,
    /// the shift and the cost.  All zero unless network.ambiguity_resolution.fixed_cycles.
    /// fold is armed.
    FixedCycleCounters   fixed_cycles;
};

}  // namespace GnssProcessor
