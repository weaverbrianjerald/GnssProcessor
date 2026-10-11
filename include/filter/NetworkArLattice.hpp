#pragma once
/**
 * @file NetworkArLattice.hpp
 * @brief The producer AR search's lattice observatory (card AGT, step AGT-6;
 *        @c network.ambiguity_resolution.trace_lattice, measure-only).
 *
 * ## What it measures
 *
 * Card ARL's hold (its task 2) will carry fixed integers from one epoch to the next.  The
 * integers it can carry form a lattice: a fixed DD is an integer combination of band-arcs, and
 * @ref arcgraph::CycleLattice keeps the fixed cycles in reduced echelon form with unit pivots, so
 * every cycle in their span has exactly one implied integer.  The observatory runs that lattice
 * beside the log-only search and measures, before any hold exists, what a hold would meet:
 *   - whether a new fix ever disagrees with the integers already held (Contradicted: a wrong fix,
 *     or a release the life readings missed);
 *   - whether each released arc that holds rows costs exactly one dimension (a unit row
 *     eliminates it) or several (no row holds it at +-1, and every row through it goes);
 *   - how often a fix cannot enter because its reduced residual has no unit entry
 *     (NotPrimitive), and how long the rows grow under eliminations.
 *
 * ## One search epoch, in order (@ref arLatticeStep)
 *
 *  1. **Release** (@ref arLatticeReleases).  Every band-arc a row holds is looked up in this
 *     search's life readings (@ref ArEpochFixes::arcs).  An arc the search did not read has
 *     vanished; an arc read as a new life (first seen, freed by the free-epoch latch, or a life
 *     stamp change) has ended the life its rows were fixed in.  Each such arc, in ascending
 *     ArcId order, is eliminated (@ref arcgraph::latticeEliminateArc), with its reading's cause,
 *     changed stamp fields and free-epoch causes.
 *  2. **Offer.**  Each DD the search determined, in basis order, is offered with its integer
 *     (@ref arcgraph::latticeOffer): added, confirmed, contradicted, or refused as not primitive.
 *
 * The order is the contract: a DD fixed again on an arc released this epoch enters as a new
 * row of the new life and never reads as a contradiction of the old one.
 *
 * ## One reading of a life
 *
 * The observatory keeps no stamp of its own.  Card AGT section 3 binds the readers of an arc's
 * life to one record, and the search's reading (@ref arcLifeStep, filter/NetworkArcLife.hpp) is
 * that record, with its echo rule (the ledger audit and the leveling harvest act after SB_NETAR,
 * so a life the last search began shows once more as an echo, which is not a new life).  A
 * lattice arc was in the pool of the search that fixed it, and every later search read it, so
 * its reading compares its stamp with the epoch before.  An epoch whose search block did not run
 * is skipped (counted): the next search's reading still compares against the last stamp read.
 *
 * ## What it writes
 *
 * Its own state (@c ctx.carry.ar_lattice) and tallies (@c ctx.counters.netar_lattice),
 * logs/network_ar_lattice.csv (one row per search epoch, @ref kArLatticeHeader),
 * logs/network_ar_lattice_events.csv (one row per release and per offer that is not a
 * confirmation, @ref kArLatticeEventsHeader; graded against truth by
 * scripts/simnet_ar_lattice_grade.py), a warning on an epoch with a contradiction, and the
 * run-end report @ref networkArLatticeRunEnd.  Nothing the solve reads.  Not snapshotted and not
 * fingerprinted: a resumed leg starts with an empty lattice.
 *
 * ## What it cannot show
 *
 * The evidence holds only while no hold exists: a hold would pull the floats the search fixes
 * toward the held integers, and Confirmed would then read the hold's own values.  The life
 * readings carry the stamp's known delays (a T3 / T4 drop or a gross-LOS erase at epoch t lands
 * after SB_NETAR and is read at t + 1; a datum-driven carry erase reads as a new life).
 */

#include "common/GnssIdentity.hpp"         // ArcId
#include "filter/NetworkArCycleBasis.hpp"  // ArEpochFixes, ArArcReading, ArFixedCycle
#include "filter/NetworkArcGraph.hpp"      // arcgraph::CycleLattice, LatticeVerdict
#include "filter/NetworkArcLife.hpp"       // kArcLifeFieldCount

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetStation;          // filter/NetworkRunTypes.hpp
struct NetworkRunContext;   // app/NetworkRunContext.hpp
struct NetworkRunEndView;   // filter/NetworkRunEnd.hpp
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp
struct SyncedEpoch;         // rinex/MultiRinexSync.hpp

/// Why the observatory released a lattice arc (step 1 of the file banner).
enum class ArLatticeRelease : std::uint8_t {
    Vanished,     ///< This search did not read the arc (it left the pool).
    FirstSeen,    ///< The last search did not read it (a gap between two searches).
    Freed,        ///< This epoch's free-epoch latch holds it.
    StampChange,  ///< Its life stamp changed, and not as an echo.
    Count         ///< The number of causes.
};

/// The number of @ref ArLatticeRelease causes.
inline constexpr std::size_t kArLatticeReleaseCount = static_cast<std::size_t>(ArLatticeRelease::Count);

/// The number of @ref arcgraph::LatticeVerdict values.
inline constexpr std::size_t kArLatticeVerdictCount = 4;

/// The number of free-epoch latch cause bits (@c RelevelEpochState::kCause*).
inline constexpr int kArLatticeFreeCauseBits = 8;

/**
 * @brief The trace name of a release cause: vanished, first_seen, freed, stamp.
 * @param r The cause.
 * @return The name.
 */
const char* arLatticeReleaseName(ArLatticeRelease r);

/**
 * @brief The trace name of a verdict: added, confirmed, contradicted, not_primitive.
 * @param v The verdict.
 * @return The name.
 */
const char* arLatticeVerdictName(arcgraph::LatticeVerdict v);

/// One released lattice arc.
struct ArLatticeReleaseEvent {
    ArcId            arc{};                             ///< The band-arc.
    ArLatticeRelease cause{ArLatticeRelease::Vanished}; ///< Why it was released.
    unsigned         change{0};       ///< The search's reading: the @ref ArcLifeField bits that changed (0 when vanished or first seen).
    std::uint8_t     free_cause{0};   ///< The free-epoch latch's cause bits for the arc (0 unless freed).
    int              rows_touched{0}; ///< Rows that held the arc when it was eliminated.
    int              rows_dropped{0}; ///< Rows dropped by its elimination: the dimensions lost.
};

/// One offered fix.
struct ArLatticeOffer {
    arcgraph::LatticeVerdict verdict{arcgraph::LatticeVerdict::Added};  ///< The lattice's verdict.
    long long                implied{0};  ///< Contradicted: the integer the lattice implies.
    int                      len{0};      ///< Added: the new row's length (arcs).
};

/// One search epoch of the observatory (@ref arLatticeStep).
struct ArLatticeEpoch {
    std::vector<ArLatticeReleaseEvent> releases;  ///< The released arcs, in elimination order (ascending ArcId).
    std::vector<ArLatticeOffer>        offers;    ///< One per fix of @ref ArEpochFixes::fixed, in its order.
    std::array<int, kArLatticeVerdictCount> verdicts{};     ///< Offers by verdict (LatticeVerdict order).
    std::array<int, kArLatticeReleaseCount> released_by{};  ///< Releases by cause.
    int    released_touching{0};  ///< Releases whose arc still held a row when eliminated.
    int    dims_lost{0};          ///< Rows dropped by the releases.
    int    no_unit{0};            ///< Releases that found no row holding the arc at +-1 (every row through it dropped).
    int    rows{0};               ///< Lattice dimension after the epoch.
    int    arcs{0};               ///< Distinct band-arcs the rows hold after the epoch.
    int    max_len{0};            ///< Longest row after the epoch (arcs).
    double mean_len{0.0};         ///< Mean row length after the epoch (arcs; 0 when empty).
};

/**
 * @brief The lattice arcs this search releases (step 1 of the file banner), ascending ArcId.
 *
 * An arc a row holds is released when @p arcs has no reading of it (Vanished) or reads it as a
 * new life (FirstSeen, Freed, StampChange); an arc read as None or Echo is kept.  The events
 * carry the reading's change bits and free causes; their row counts are left at 0 (filled by the
 * elimination, @ref arLatticeStep).
 *
 * @param L    The lattice.
 * @param arcs This search's life readings (@ref ArEpochFixes::arcs).
 * @return The releases.
 */
std::vector<ArLatticeReleaseEvent> arLatticeReleases(const arcgraph::CycleLattice& L,
                                                     const std::map<ArcId, ArArcReading>& arcs);

/**
 * @brief One search epoch: release the arcs whose life ended, then offer the fixes (the file
 *        banner's order).  Pure apart from @p L.
 * @param L     The lattice (released and offered in place).
 * @param fixes What the search handed over (@ref ArEpochFixes; @c searched is not read).
 * @return The epoch's releases, offers and figures.
 */
ArLatticeEpoch arLatticeStep(arcgraph::CycleLattice& L, const ArEpochFixes& fixes);

/// The observatory's run state (@c ctx.carry.ar_lattice; file banner).  The lattice only: the
/// life of each arc is the search's reading, never a second record here.
struct ArLatticeState {
    arcgraph::CycleLattice lattice;  ///< Fixed cycles and their integers, reduced echelon form.
};

/// Run tallies of the observatory (standing invariant 4: an armed observatory that never received
/// a fix says so).  All zero unless @c trace_lattice.
struct ArLatticeCounters {
    long long epochs{0};          ///< Search epochs observed.
    long long epochs_skipped{0};  ///< Epochs the search block did not run (nothing released or offered).
    long long fixes{0};           ///< Fixes offered.
    std::array<long long, kArLatticeVerdictCount> verdicts{};     ///< Offers by verdict.
    long long contradicted_epochs{0};  ///< Epochs with at least one contradiction.
    double    first_contradicted_tow{-1.0};  ///< TOW of the first such epoch (-1: none).
    std::array<long long, kArLatticeReleaseCount> released_by{};  ///< Releases by cause.
    std::array<long long, kArLatticeReleaseCount> touching_by{};  ///< ...of those, arcs that held a row.
    std::array<long long, kArcLifeFieldCount> change_by{};        ///< Releases that held a row, by changed stamp field.
    std::array<long long, kArLatticeFreeCauseBits> free_by{};     ///< Releases that held a row, by free-epoch cause bit.
    long long released_touching{0};  ///< Releases that held a row.
    long long dims_lost{0};          ///< Rows dropped by releases.
    long long no_unit{0};            ///< Releases with no unit row.
    long long dims_mismatch_epochs{0};  ///< Epochs whose dimensions lost differ from their releases that held a row.
    int       max_rows{0};           ///< Largest lattice dimension.
    int       max_len{0};            ///< Longest row of any epoch.
    std::vector<double> rows;        ///< Lattice dimension per epoch.
    std::vector<double> mean_len;    ///< Mean row length per epoch.
    std::vector<double> ms;          ///< Cost per epoch (ms).
};

/**
 * @brief Add one epoch to the run tallies.
 * @param c   The run's tallies.
 * @param e   The epoch.
 * @param tow Its GPS time of week (s).
 * @param ms  Its cost (ms).
 */
void arLatticeTally(ArLatticeCounters& c, const ArLatticeEpoch& e, double tow, double ms);

/**
 * @brief The lattice observatory at this epoch (the SB_NETAR bracket, after the search).
 *
 * Returns at once unless @c network.ambiguity_resolution.enable and @c .trace_lattice.  When the
 * search block did not run (@p fixes not searched) the epoch is counted as skipped.  Otherwise it runs
 * @ref arLatticeStep on @c ctx.carry.ar_lattice, adds to @c ctx.counters.netar_lattice, writes
 * the epoch's row and its events, and warns when a fix contradicted the lattice.
 *
 * @param ctx      [in,out] run context: @c carry.ar_lattice and @c counters.netar_lattice.
 * @param config_  The run configuration (the knob).
 * @param log      "main_log" (the contradiction warning).
 * @param traces   Trace-stream owner: @c ar_lattice_trace and @c ar_lattice_events_trace.
 * @param se       The epoch (week and TOW of the rows).
 * @param stations The network stations (names of the event rows).
 * @param fixes    What the search handed over (@ref networkProducerAr's return).
 */
void networkArLattice(NetworkRunContext& ctx, const GnssConfig& config_,
                      const std::shared_ptr<spdlog::logger>& log, NetworkRunTraces& traces,
                      const SyncedEpoch& se, const std::vector<NetStation>& stations,
                      const ArEpochFixes& fixes);

/**
 * @brief Run-end report: the producer AR search's lattice observatory (card AGT).
 *
 * The epochs observed and skipped; the offers by verdict; the lattice dimension and row lengths;
 * the releases by cause, the rows they held and the dimensions lost, by stamp field and by
 * free-epoch cause; the cost.  Warns on any contradiction, on dimensions lost that differ from
 * the released arcs that held rows, and (standing invariant 4) when armed and no fix was ever
 * offered.
 *
 * Gate: returns 0 at once unless @c network.ambiguity_resolution.enable and @c .trace_lattice.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkArLatticeRunEnd(const NetworkRunEndView& v);

}  // namespace GnssProcessor
