#pragma once
/**
 * @file NetworkArLattice.hpp
 * @brief The ledger of fixed integer cycles: card AGT's lattice observatory (step AGT-6,
 *        @c network.ambiguity_resolution.trace_lattice), re-scoped by card FCF (step FCF-3) as
 *        the one owner of the integers a fixed-cycle hold may carry, with its one release seam.
 *
 * ## What it holds
 *
 * A fixed DD is an integer combination of band-arcs.  @ref arcgraph::CycleLattice keeps the
 * fixed cycles in reduced echelon form with unit pivots, so every cycle in their span has
 * exactly one implied integer.  The ledger is that lattice, the streaks of fixes waiting to
 * enter it, and the epoch of its last step.  It writes nothing the solve reads.
 *
 * ## One ledger epoch, in order
 *
 * The step runs inside the fixed-cycle seam (filter/NetworkFixedCycles.hpp, SB_SOLPREP after
 * the gross-LOS Decide), whenever @c trace_lattice is armed or the fold reads the lattice
 * (@c fixed_cycles.source lattice):
 *
 *  1. **Seam gap** (@ref arLatticeBegin).  A loop epoch since the last step that ran without
 *     one (a withheld epoch, an epoch without a search) releases every row: the evidence of
 *     that epoch (its replay culprits, its free-epoch latch) left no stamp.
 *  2. **Evidence** (@ref arLatticeEvidenceReleases).  Every band-arc a row holds is released
 *     when the search did not read it (vanished), read it as a new life (first seen, freed by
 *     the free-epoch latch with its cause bits, a life-stamp change with its field bits), or
 *     when this epoch's landed within-epoch replay or gross-LOS Decide named its line of sight.
 *  3. **Contradictions.**  A fix of this epoch whose cycle the lattice implies at another
 *     integer releases the arcs of that cycle at once.
 *  4. **Re-validation** (the fold's, between steps 3 and 5: filter/NetworkFixedCycles.hpp).
 *     Every L1/L2 unit the lattice implies is tested against the search's float; failures are
 *     released through @ref arLatticeRelease.
 *  5. **Admission** (@ref arLatticeAdmit).  A fix enters only after @c persist_ep consecutive
 *     epochs at one integer with no new life on its arcs; it is then offered (added,
 *     confirmed, or refused as not primitive).  An added row folds from the next epoch.
 *
 * The fold's own refusals (a unit dropped by block acceptance, a degenerate block) also go
 * through @ref arLatticeRelease.  Every release, whatever its cause, is one
 * @ref ArLatticeReleaseEvent from one table of causes (@ref arLatticeReleaseSpec): its name,
 * its counter, its trace column and its event row.
 *
 * ## A verdict on a cycle releases its arcs
 *
 * A contradiction, a failed float test with no arc shared by another failure, and the fold's
 * refusals each put one cycle's integer in question without saying which of its band-arcs moved.
 * The release eliminates every one of its arcs (@ref arcgraph::latticeEliminateArc), so the
 * ledger keeps exactly the part of its lattice that holds none of them, and no integer it implies
 * later depends on their old integers.  Dropping only the rows behind the cycle (the rows whose
 * pivots it holds) is not enough: in reduced echelon form other rows can hold one of its arcs as
 * a non-pivot term, keep that arc's old integer, and imply the released cycle again once
 * unrelated fixes complete the span (card FCFC section 8: an injected (7, 9) slip, dropped by
 * acceptance, came back twice at its stale integer).  The cost is at most one dimension per arc
 * a unit row holds.
 *
 * ## One reading of a life
 *
 * The ledger keeps no stamp of its own.  Card AGT section 3 binds the readers of an arc's life
 * to one record, and the search's reading (@ref arcLifeStep, filter/NetworkArcLife.hpp) is that
 * record, with its echo rule.  Post-fit QC at epoch t (RAIM T3 / T4, the gross-LOS Act) acts
 * after the emission; its stamp is read at t + 1 (counted as late by the fold).
 *
 * ## What it writes
 *
 * Its state (@c ctx.carry.ar_lattice) and tallies (@c ctx.counters.netar_lattice); under
 * @c trace_lattice logs/network_ar_lattice.csv (one row per step, @ref kArLatticeHeader) and
 * logs/network_ar_lattice_events.csv (one row per release and per offer that is not a
 * confirmation, @ref kArLatticeEventsHeader; graded against truth by
 * scripts/simnet_ar_lattice_grade.py and scripts/simnet_fixed_cycle_grade.py), a warning on an
 * epoch with a contradiction, and the run-end report @ref networkArLatticeRunEnd.  Not
 * snapshotted and not fingerprinted: a resumed leg starts with an empty ledger.
 *
 * ## What it cannot show
 *
 * A contradiction count of zero is no evidence of a clean release (card AGTB section 5): a slip
 * no detector flags leaves the zero-q float on the old integer, the search fixes it again, and
 * the lattice confirms it.  The ledger then keeps the stale integer after the search stops
 * fixing it too (the S8 twin without the slip fixes: 30 epochs confirmed by the search, 34 more
 * held by the ledger alone, no guard firing; card FCFC section 5).  The guard there is the slip
 * fixes of card ARL section 13; the float re-validation acts once the float moves, and the fold's
 * lapse (@c fixed_cycles.lapse_ep) stops the folding once the search no longer determines the
 * pair.
 */

#include "common/GnssIdentity.hpp"         // ArcId, LosId, SatId, StationId
#include "network/ar/NetworkArCycleBasis.hpp"  // ArEpochFixes, ArArcReading, ArFixedCycle, ArDdIdentity
#include "network/ar/NetworkArcGraph.hpp"      // arcgraph::CycleLattice, LatticeVerdict
#include "network/ar/NetworkArcLife.hpp"       // kArcLifeFieldCount

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct EpochEvents;   // app/NetworkEpoch.hpp

class GnssConfig;
struct NetStation;           // filter/NetworkRunTypes.hpp
struct NetworkRunContext;    // app/NetworkRunContext.hpp
struct NetworkRunEndView;    // filter/NetworkRunEnd.hpp
struct NetworkRunTraces;     // logging/NetworkRunTraces.hpp
struct SyncedEpoch;          // rinex/MultiRinexSync.hpp
struct WithinEpochCulprits;  // filter/NetworkWithinEpochCulprits.hpp

/// Why the ledger released lattice content (the file banner's steps; one row each of
/// @ref arLatticeReleaseSpec).
enum class ArLatticeRelease : std::uint8_t {
    Vanished,       ///< This search did not read the arc (it left the pool).
    FirstSeen,      ///< The last search did not read it (a gap between two searches).
    Freed,          ///< This epoch's free-epoch latch holds it (with the latch's cause bits).
    StampChange,    ///< Its life stamp changed, and not as an echo (with the changed fields).
    ReplayCulprit,  ///< (card FCF) The landed within-epoch replay refloated or excluded its line of sight.
    GrossLos,       ///< (card FCF) This epoch's gross-LOS Decide declared its line of sight or its station.
    Contradiction,  ///< (card FCF) A fix of this epoch implies another integer: the cycle's arcs released.
    Revalidation,   ///< (card FCF) Its unit failed the float test: the arc two failing units share, else both cycles' arcs, released.
    Acceptance,     ///< (card FCF) Its unit was dropped from a held-solve block that failed acceptance: both cycles' arcs released.
    Degenerate,     ///< (card FCF) Its unit's block was refused by the kernel's screens: both cycles' arcs released.
    SeamGap,        ///< (card FCF) A loop epoch since the last step went without one: every row released.
    Count           ///< The number of causes.
};

/// The number of @ref ArLatticeRelease causes.
inline constexpr std::size_t kArLatticeReleaseCount = static_cast<std::size_t>(ArLatticeRelease::Count);

/// The number of @ref arcgraph::LatticeVerdict values.
inline constexpr std::size_t kArLatticeVerdictCount = 4;

/// The number of free-epoch latch cause bits (@c RelevelEpochState::kCause*).
inline constexpr int kArLatticeFreeCauseBits = 8;

/// What one release acts on (set by the step that asks for it: the evidence releases arcs, a
/// contradiction and the fold's refusals the arcs of a cycle, the re-validation either, a seam
/// gap everything).
enum class ArLatticeScope : std::uint8_t {
    Arc,    ///< One band-arc, eliminated (@ref arcgraph::latticeEliminateArc).
    Cycle,  ///< Every band-arc of one cycle, eliminated (file banner: a verdict on a cycle releases its arcs).
    All     ///< Every row.
};

/// One row of the cause table.
struct ArLatticeReleaseSpec {
    ArLatticeRelease cause;   ///< The cause (the row's index).
    const char*      name;    ///< Trace, event and run-end name.
    const char*      column;  ///< Its column of network_ar_lattice.csv.
    const char*      what;    ///< One line for the run end.
};

/**
 * @brief The cause table's row for @p r (rows in enum order; unit-tested).
 * @param r The cause.
 * @return Its row.
 */
const ArLatticeReleaseSpec& arLatticeReleaseSpec(ArLatticeRelease r);

/**
 * @brief The trace name of a release cause (@ref ArLatticeReleaseSpec::name): vanished,
 *        first_seen, freed, stamp, replay, gross, contradiction, revalidation, acceptance,
 *        degenerate, seam_gap.
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

/// One release: its request and, once applied, the rows it found and dropped.
struct ArLatticeReleaseEvent {
    ArLatticeRelease cause{ArLatticeRelease::Vanished};  ///< Why.
    ArLatticeScope   scope{ArLatticeScope::Arc};         ///< What it acts on.
    ArcId            arc{};             ///< Arc scope: the band-arc.
    unsigned         change{0};         ///< Arc scope, from the search's reading: the @ref ArcLifeField bits that changed (0 when vanished or first seen).
    std::uint8_t     free_cause{0};     ///< Arc scope: the free-epoch latch's cause bits for the arc (0 unless freed).
    arcgraph::Cycle  cycle;             ///< Cycle scope: the cycle whose arcs are released.
    ArDdIdentity     dd{};              ///< Cycle scope: the cycle's DD identity.
    long long        offered{0};        ///< Contradiction: the integer the fix offered.
    long long        implied{0};        ///< Contradiction: the integer the lattice implied.
    int              rows_touched{0};   ///< Rows that held the arc (arc scope), any of the cycle's arcs (cycle scope) or anything (all) when applied.
    int              rows_dropped{0};   ///< Rows the release dropped: the dimensions lost.
};

/// One fix of the epoch as the ledger met it.
struct ArLatticeOffer {
    arcgraph::LatticeVerdict verdict{arcgraph::LatticeVerdict::Added};  ///< The verdict (when offered or contradicted).
    bool                     waiting{false};  ///< Not yet offered: its streak is below @c persist_ep.
    int                      streak{0};       ///< Consecutive epochs at this integer, this one included.
    long long                implied{0};      ///< Contradicted: the integer the lattice implies.
    int                      len{0};          ///< Added: the new row's length (arcs).
};

/// One ledger epoch (file banner).
struct ArLatticeEpoch {
    std::vector<ArLatticeReleaseEvent> releases;  ///< In application order.
    std::vector<ArLatticeOffer>        offers;    ///< One per fix of @ref ArEpochFixes::fixed, in its order.
    std::set<ArcId> broken;       ///< Arcs whose life or integer was put in question this epoch (new lives, the replay's and the gross-LOS lines of sight, released arcs, the arcs of released cycles): a streak through one restarts.
    std::array<int, kArLatticeVerdictCount> verdicts{};     ///< Fixes by verdict (offered or contradicted).
    int    waiting{0};            ///< Fixes not yet offered.
    std::array<int, kArLatticeReleaseCount> released_by{};  ///< Releases by cause.
    std::array<int, kArLatticeReleaseCount> dims_by{};      ///< Dimensions lost by cause.
    int    released_touching{0};  ///< Releases that found rows.
    int    dims_lost{0};          ///< Rows dropped by the releases.
    int    no_unit{0};            ///< Arc eliminations (of an arc release or of a cycle release's arcs) that found rows through the arc but none holding it at +-1 (every row through it dropped).
    int    units{0};              ///< (card FCF) Units the lattice implied this epoch (the fold's re-validation).
    int    units_failed{0};       ///< (card FCF) ...of those, failing their float test.
    int    rows{0};               ///< Lattice dimension after the epoch.
    int    arcs{0};               ///< Distinct band-arcs the rows hold after the epoch.
    int    max_len{0};            ///< Longest row after the epoch (arcs).
    double mean_len{0.0};         ///< Mean row length after the epoch (arcs; 0 when empty).
};

/// A fix waiting to enter the ledger (step 5 of the file banner).
struct ArFixStreak {
    long long value{0};    ///< The integer of the streak.
    int       epochs{0};   ///< Consecutive epochs at that integer.
    long long last_ep{-1}; ///< Loop epoch of the last fix of the streak.
};

/// The ledger's run state (@c ctx.carry.ar_lattice; file banner).  The life of each arc is the
/// search's reading, never a second record here.
struct ArLatticeState {
    arcgraph::CycleLattice              lattice;          ///< Fixed cycles and their integers, reduced echelon form.
    std::map<ArDdIdentity, ArFixStreak> streaks;          ///< (card FCF) Fixes waiting for @c persist_ep, by DD identity.
    long long                           last_step_ep{-1}; ///< (card FCF) Loop epoch of the last step (-1: none yet).
};

/// Lines of sight and their wider scopes, every signal of each (the replay's and the gross-LOS
/// screen's own granularity).
struct ArLosScope {
    std::set<SatId>     sats;      ///< A satellite at every station.
    std::set<StationId> stations;  ///< A station, every satellite.
    std::set<LosId>     los;       ///< One line of sight.
    /**
     * @brief True when the scope names the arc's line of sight.
     * @param a The arc.
     * @return Its satellite, its station or its line of sight is in the scope.
     */
    bool covers(const ArcId& a) const;
    /// @return True when the scope names nothing.
    bool empty() const { return sats.empty() && stations.empty() && los.empty(); }
};

/// The epoch's release evidence from outside the search's life readings (card FCF), typed once.
struct ArLatticeEvidence {
    ArLosScope replay;  ///< The landed within-epoch replay's culprits: satellites refloated network-wide, lines of sight refloated, stations excluded.
    ArLosScope gross;   ///< This epoch's gross-LOS Decide: declared lines of sight, declared stations, cold re-floats.
};

/**
 * @brief The epoch's evidence (card FCF): the landed replay's culprits and this epoch's gross-LOS
 *        Decide (@c ctx.carry.gl_act_declare, @c gl_act_station declared, @c gl_act_cold_refloat;
 *        empty unless @c network.gross_los_emit_exclude split the screen).
 * @param ctx The run context.
 * @param we  The landed replay's culprit sets (@c NetworkSolveGlueResult::we_landed).
 * @return The evidence, typed (a satellite name that does not parse is left out).
 * @param events The epoch's events, read: the gross-LOS decisions.
 */
ArLatticeEvidence networkArLatticeEvidence(const NetworkRunContext& ctx, const WithinEpochCulprits& we, const EpochEvents& events);

/**
 * @brief The lattice arcs the evidence releases (step 2 of the file banner), ascending ArcId.
 *
 * An arc a row holds is released when @p arcs has no reading of it (Vanished), reads it as a new
 * life (FirstSeen, Freed, StampChange), or @p ev names its line of sight (ReplayCulprit, then
 * GrossLos).  One release per arc, the first cause in that order; an arc read as None or Echo and
 * named by nothing is kept.  The events carry the reading's change bits and free causes.
 *
 * @param L    The lattice.
 * @param arcs This search's life readings (@ref ArEpochFixes::arcs).
 * @param ev   The epoch's evidence.
 * @return The releases (not applied).
 */
std::vector<ArLatticeReleaseEvent> arLatticeEvidenceReleases(const arcgraph::CycleLattice& L,
                                                             const std::map<ArcId, ArArcReading>& arcs,
                                                             const ArLatticeEvidence& ev);

/**
 * @brief The four band-arcs of a DD identity: (sta_r, sat_j), (sta_r, ref_sat), (ref_sta, sat_j),
 *        (ref_sta, ref_sat), in its signal.
 * @param id The DD.
 * @return Its arcs.
 */
std::array<ArcId, 4> arDdArcs(const ArDdIdentity& id);

/**
 * @brief THE release function (card FCF section 3): apply @p events in order and tally them.
 *
 * An arc release eliminates its arc (an arc already eliminated this epoch finds no row); a
 * cycle release eliminates each of its cycle's arcs (file banner); an all release drops every
 * row.  Each event gets its rows touched and dropped; @p e counts it by cause, its dimensions by
 * cause, and adds its arc, or its cycle's arcs, to @c e.broken.  Every streak through those arcs
 * is erased, so the fix restarts from one.  An all release erases every streak.
 *
 * @param S      The ledger.
 * @param events The releases (moved into @c e.releases).
 * @param e      The epoch.
 */
void arLatticeRelease(ArLatticeState& S, std::vector<ArLatticeReleaseEvent> events, ArLatticeEpoch& e);

/**
 * @brief Steps 1 to 3 of one ledger epoch: the seam gap, the evidence releases, the
 *        contradictions.
 *
 * The seam gap: when the ledger stepped before (@c S.last_step_ep at least 0) and @p ep is more
 * than one epoch later, every row is released (SeamGap).  Then the evidence releases
 * (@ref arLatticeEvidenceReleases), and the arcs this search read as a new life or the evidence
 * names join @c e.broken.  Then each fix of @p fixes, in basis order, whose cycle the lattice
 * implies at another integer: the offer is Contradicted (with the implied integer) and the
 * cycle's arcs are released (Contradiction) before the next fix is read.
 *
 * @param S     The ledger.
 * @param fixes What the search handed over.
 * @param ev    The epoch's evidence.
 * @param ep    The loop epoch.
 * @return The epoch so far.
 */
ArLatticeEpoch arLatticeBegin(ArLatticeState& S, const ArEpochFixes& fixes, const ArLatticeEvidence& ev,
                              long long ep);

/**
 * @brief Step 5 of one ledger epoch: admission.
 *
 * Each fix not contradicted at this epoch continues its streak when the streak's integer is the
 * same, its last fix was the epoch before, and none of its four arcs is in @c e.broken; else its
 * streak restarts at one.  A fix whose streak reaches @p persist_ep is offered
 * (@ref arcgraph::latticeOffer at @p ep): added, confirmed or not primitive; a contradiction here
 * (the epoch's own fixes disagree through a row added this epoch) is released as at step 3.  The
 * rest wait.  Streaks not continued at this epoch are dropped.
 *
 * @param S          The ledger.
 * @param fixes      What the search handed over.
 * @param persist_ep Consecutive epochs before a fix is offered (at least 1).
 * @param ep         The loop epoch.
 * @param e          The epoch.
 */
void arLatticeAdmit(ArLatticeState& S, const ArEpochFixes& fixes, int persist_ep, long long ep,
                    ArLatticeEpoch& e);

/**
 * @brief Close one ledger epoch: its figures after every release and offer, and @p ep as the
 *        last step.
 * @param S  The ledger.
 * @param ep The loop epoch.
 * @param e  The epoch.
 */
void arLatticeClose(ArLatticeState& S, long long ep, ArLatticeEpoch& e);

/// Run tallies of the ledger (standing invariant 4: an armed ledger that never received a fix
/// says so).  All zero unless the ledger runs.
struct ArLatticeCounters {
    long long epochs{0};          ///< Ledger epochs (steps).
    long long epochs_skipped{0};  ///< Epochs the search block did not run (no step; the next one releases everything).
    long long fixes{0};           ///< Fixes met.
    long long waiting{0};         ///< ...not yet offered (below persist_ep).
    std::array<long long, kArLatticeVerdictCount> verdicts{};     ///< Fixes by verdict.
    long long contradicted_epochs{0};  ///< Epochs with at least one contradiction.
    double    first_contradicted_tow{-1.0};  ///< TOW of the first such epoch (-1: none).
    std::array<long long, kArLatticeReleaseCount> released_by{};  ///< Releases by cause.
    std::array<long long, kArLatticeReleaseCount> touching_by{};  ///< ...of those, releases that found rows.
    std::array<long long, kArLatticeReleaseCount> dims_by{};      ///< Dimensions lost by cause.
    std::array<long long, kArcLifeFieldCount> change_by{};        ///< Arc releases that held a row, by changed stamp field.
    std::array<long long, kArLatticeFreeCauseBits> free_by{};     ///< Arc releases that held a row, by free-epoch cause bit.
    long long released_touching{0};  ///< Releases that found rows.
    long long dims_lost{0};          ///< Rows dropped by releases.
    long long no_unit{0};            ///< Arc eliminations that found rows through the arc but no unit row.
    long long units{0};              ///< (card FCF) Units implied, summed over epochs.
    long long units_failed{0};       ///< (card FCF) ...failing their float test.
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
 * @brief Record one ledger epoch: add it to @c ctx.counters.netar_lattice, write its row and its
 *        events when the two files are open, and warn when a fix contradicted the lattice.
 * @param ctx      [in,out] run context: @c counters.netar_lattice.
 * @param log      "main_log" (the contradiction warning).
 * @param traces   Trace-stream owner: @c ar_lattice_trace and @c ar_lattice_events_trace.
 * @param se       The epoch (week and TOW of the rows).
 * @param stations The network stations (names of the event rows).
 * @param fixes    What the search handed over (the offers' identities).
 * @param e        The epoch.
 * @param ms       Its cost (ms).
 */
void networkArLatticeRecord(NetworkRunContext& ctx, const std::shared_ptr<spdlog::logger>& log,
                            NetworkRunTraces& traces, const SyncedEpoch& se,
                            const std::vector<NetStation>& stations, const ArEpochFixes& fixes,
                            const ArLatticeEpoch& e, double ms);

/**
 * @brief Whether the ledger runs on this configuration: the search with @c trace_lattice, or a
 *        fixed-cycle fold that reads the lattice (@c fixed_cycles.source lattice).
 * @param config_ The run configuration.
 * @return True when the fixed-cycle seam steps the ledger.
 */
bool arLatticeArmed(const GnssConfig& config_);

/**
 * @brief Run-end report: the ledger of fixed cycles (cards AGT and FCF).
 *
 * The epochs stepped and skipped; the fixes by verdict and waiting; the lattice dimension and
 * row lengths; the releases by cause, the rows they found and the dimensions lost, by stamp
 * field and by free-epoch cause; the units implied and failing; the cost.  Warns on any
 * contradiction and (standing invariant 4) when armed and no fix was ever offered.
 *
 * Gate: returns 0 at once unless @ref arLatticeArmed.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkArLatticeRunEnd(const NetworkRunEndView& v);

}  // namespace GnssProcessor
