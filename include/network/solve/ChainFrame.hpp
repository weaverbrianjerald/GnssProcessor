#pragma once
/**
 * @file ChainFrame.hpp
 * @brief The frame observatory of the eq34 chain datum: the chain-event table, the record of
 *        each epoch's tree, the previous epoch's chord table and the run-end census.
 *        Measure-only.
 *
 * ## The frame
 *
 * Per (constellation, signal) component the chain datum (network/solve/ChainDatum.hpp) pins the
 * ambiguities of a spanning tree at targets k_e and the root receiver's phase bias.  Those pins
 * are the frame.  Two frames give phase biases that differ, per satellite, by an integer number
 * of cycles plus a constant common to the component only when every event that changes the tree
 * keeps the pins' integers consistent with the carried estimates.  An event that sets a new pin
 * from a float estimate steps the frame by a fraction, and no consumer can recover that.
 *
 * The observatory records, for every tree edge of every epoch, which row of the chain-event
 * table (@ref kChainEventSpec) the edge took part in, what the as-built (legacy) rule did to it,
 * and what the integer rule of that row would have needed: for a re-host, an integer certified
 * from the previous epoch's chord table.  No row of the table is armed here, so the election
 * reads nothing this file writes; every output is a trace row, an event row or a counter.
 *
 * ## One epoch, in order
 *
 *  1. The election (@c networkElectChainDatum), with @c network.trace_chain_datum armed, records
 *     each tree edge with its legacy facts, each component, the legacy shift and re-reference
 *     sets, and each chord's fundamental cycle into the epoch's @ref ChainEpochObs
 *     (@c EpochEvents::PhaseDatum::chain).
 *  2. After the solve and the harvests (the StashTail observer slot), @ref networkChainFrameObserve
 *     classifies each edge against the observatory's memory of the last observed epoch (its
 *     vertices, components and chord table), certifies the re-hosts from that chord table, writes
 *     @c network_chain_edges.csv and the CHAIN rows of @c network_events.csv, builds this
 *     epoch's chord table (@c network_chain_frame.csv) and replaces the memory.  With
 *     @c network.trace_rpb_states armed it also writes @c network_rpb_states.csv.
 *  3. The run end (@ref networkChainFrameRunEnd) prints the census.
 *
 * An epoch the run skips after the election leaves the memory at the last observed epoch, so
 * the next classification compares against that epoch; the chord table records its epoch.
 *
 * ## Certification
 *
 * A re-host moves a vertex's parent edge onto an arc that was a chord at the last observed epoch.
 * That chord's integer in the incumbent frame is read from the chord table: its two-band float
 * (the arc's band-1 and band-2 ambiguities in cycles) and their 2x2 conditional covariance, scaled
 * by @ref kChainCertSigmaScale in sigma, are fixed with @c ils::ilsFixDecorrelatedPartial and
 * the producer search's acceptance gates; the band's integer is certified when it is determined
 * (conditional sigma at most @ref kChainCertDeterminedSigmaCyc) and kept.  When the producer's
 * lattice of fixed cycles (@c network.ambiguity_resolution.trace_lattice) implies the chord's
 * integer, it is a cross-check: a disagreement is a conflict and counts as uncertified.
 */

#include "network/ar/ArcGraph.hpp"                 // arcgraph::Cycle
#include "network/solve/ChainFrameCounters.hpp"   // ChainFrameCounters, kChainEventRowCount

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;          // network/driver/Run.hpp
struct NetworkEpoch;        // network/driver/Epoch.hpp
struct NetworkRunEndView;   // network/runend/RunEnd.hpp

// ---- The chain-event table ----------------------------------------------------------------------

/// The rows of the chain-event table, one per event class.
enum class ChainEventRow : std::uint8_t {
    Kept,               ///< The edge stays in the tree (its arc and direction, or its arc flipped).
    Birth,              ///< A vertex never seen before joins the tree.
    Returning,          ///< A vertex seen earlier, absent at the last observed epoch, rejoins.
    RehostCertified,    ///< A vertex moves to a new parent edge whose integer the chord table certifies.
    RehostUncertified,  ///< A vertex moves to a new parent edge the chord table does not certify.
    RootHandover,       ///< The component's datum receiver changed.
    TreeEdgeSlip,       ///< The edge's arc slipped this epoch (the free-epoch latch named it).
    Merge,              ///< The edge joins two components of the last observed epoch.
    Split,              ///< The component split off a component of the last observed epoch.
};

static_assert(kChainEventRowCount == static_cast<std::size_t>(ChainEventRow::Split) + 1,
              "kChainEventRowCount (ChainFrameCounters.hpp) counts the ChainEventRow values");

/// The frame step the integer rule of a row puts on the frame.
enum class ChainStep : std::uint8_t {
    Zero,              ///< None.
    New,               ///< A new key enters the frame.
    ZeroOrCutRelease,  ///< None when certified, else a cut release.
    CutRelease,        ///< The moved side takes an integer step, declared.
};

/// What the integer rule of a row does to the carries.
enum class ChainCarry : std::uint8_t {
    Keep,              ///< Every carry kept.
    KeepOrCutRelease,  ///< Kept when certified, else the cut is released.
    CutRelease,        ///< The carried priors across the cut are erased.
    ReleaseArc,        ///< The slipped arc's carry is released.
};

/// What the wire declares under the integer rule of a row.
enum class ChainWire : std::uint8_t {
    None,                ///< Nothing.
    NewKey,              ///< A new key.
    NoneOrDeclareMoved,  ///< Nothing when certified, else the moved keys.
    DeclareMoved,        ///< The moved keys.
    ComponentChange,     ///< The component token changes.
};

/// One row of the chain-event table: the event class, the as-built rule, the integer rule and
/// what that rule does to the frame, the carries and the wire.
struct ChainEventSpec {
    ChainEventRow row;           ///< The class.
    const char*   name;          ///< Its name in the traces ("Kept", "RehostCertified", ...).
    const char*   rows_entry;    ///< The entry of network.phase_datum_eq34_event_rows that will arm its integer rule ("" when none).
    const char*   event;         ///< Its CHAIN event name in network_events.csv ("" when the class writes none).
    const char*   legacy_rule;   ///< What the as-built election does.
    const char*   integer_rule;  ///< What the integer rule does.
    ChainStep     step;          ///< The integer rule's frame step.
    ChainCarry    carry;         ///< The integer rule's carry action.
    ChainWire     wire;          ///< The integer rule's declaration.
};

/// The chain-event table, in @ref ChainEventRow order.
extern const std::array<ChainEventSpec, kChainEventRowCount> kChainEventSpec;

/**
 * @brief The table row of a class.
 * @param r The class.
 * @return Its row of @ref kChainEventSpec.
 */
const ChainEventSpec& chainEventSpec(ChainEventRow r);

/// The name of a @ref ChainStep value ("Zero", "New", "ZeroOrCutRelease", "CutRelease").
const char* chainStepName(ChainStep s);
/// The name of a @ref ChainCarry value.
const char* chainCarryName(ChainCarry c);
/// The name of a @ref ChainWire value.
const char* chainWireName(ChainWire w);

// ---- What the legacy rule did to one pin --------------------------------------------------------

/// Why the as-built election set a pin's target and step as it did.
enum class ChainLegacyCause : std::uint8_t {
    SameEdge,       ///< The incumbent edge kept: the step is the target difference.
    FirstSighting,  ///< No incumbent parent edge: the step is the target against the last target or 0.
    RehostCarry,    ///< A new parent edge with a carry at the new host: the step is target minus that carry.
    RehostFramed,   ///< A new parent edge, the step from the new host's own state (seed-round re-host frame).
    RehostCold,     ///< A new parent edge without a carry: an unknown step (the key is re-referenced).
    SeedDeferred,   ///< A seed-round target decided at the fold: an unknown step.
    SeedStep,       ///< A seed-round target that moved: an unknown step.
    RootHandover,   ///< The satellite's remembered root differs from this epoch's: an unknown step.
};

/// The name of a @ref ChainLegacyCause value ("same_edge", "first_sighting", ...).
const char* chainLegacyCauseName(ChainLegacyCause c);

/// Which source certified a re-host's integer.
enum class ChainCertSource : std::uint8_t {
    None,     ///< Uncertified.
    Pair,     ///< The two-band fix of the chord table.
    Lattice,  ///< The producer's lattice of fixed cycles.
};

/// The name of a @ref ChainCertSource value ("none", "pair", "lattice").
const char* chainCertSourceName(ChainCertSource s);

/// Sigma scale of the chord covariance in the certification (the measured optimism of the
/// carried covariance); the variances are multiplied by its square.  3.0: at 2.0 the would-be
/// certifications of the sim beds were wrong against truth, and the frozen certification rule
/// raises the scale to 3.0 for the re-measurement.
inline constexpr double kChainCertSigmaScale = 3.0;
/// Conditional sigma (cycles) at or below which the certification counts a band's integer as
/// determined.
inline constexpr double kChainCertDeterminedSigmaCyc = 0.05;

// ---- One epoch's record -------------------------------------------------------------------------

/**
 * @brief One tree edge of the epoch as the election realized it, with its legacy facts; the
 *        observer fills the classification fields.
 *
 * A vertex is named by a key: "R:<station index>" for a receiver, "S:<satellite>" for a
 * satellite.
 */
struct ChainEdgeObs {
    std::string group;          ///< "<constellation key>|<signal>", the election's group key.
    std::string sys;            ///< Constellation key ("G", "E", ...).
    std::string signal;         ///< Phase signal of the group.
    int         comp{0};        ///< Component index within the group (the election's order).
    std::string r1;             ///< The component's datum receiver.
    int         station{-1};    ///< The arc's station (roster index).
    std::string station_name;   ///< Its name.
    std::string sat;            ///< The arc's satellite.
    bool        sat_is_child{true};   ///< The edge attaches the satellite (else the station).
    std::string child;          ///< Vertex key of the attached endpoint.
    std::string parent;         ///< Vertex key of the other endpoint.
    int         depth{0};       ///< The child's depth below its root (an edge on r1 has depth 1).
    double      lambda_m{0.0};  ///< The group's carrier wavelength (0: no integer target, an FDMA group).
    double      target_m{0.0};  ///< The pin target (m).
    double      prev_target_m{std::numeric_limits<double>::quiet_NaN()};  ///< The last target on this (station, ambiguity) (NaN: none).
    double      carry_m{std::numeric_limits<double>::quiet_NaN()};        ///< The new host's carried value (NaN: none read).
    double      pinmove_m{0.0}; ///< The legacy pin displacement.
    bool        known{true};    ///< The legacy step is known (else the key is re-referenced).
    double      step_m{std::numeric_limits<double>::quiet_NaN()};  ///< The child's frame step from the shift walk (NaN: unknown).
    ChainLegacyCause cause{ChainLegacyCause::SameEdge};  ///< What the legacy rule did.
    bool        root_unknown{false};    ///< The satellite's remembered root differs from r1.
    bool        edge_changed{false};    ///< The legacy rule saw a new parent edge.
    bool        had_parent{false};      ///< The child had an incumbent parent edge.
    std::string prev_parent;            ///< That incumbent parent's vertex key ("" when none).
    bool        incumbent_same{false};  ///< The incumbent parent edge is this arc, same direction.
    bool        incumbent_flipped{false};  ///< This arc was an incumbent tree edge attaching the other endpoint.
    bool        from_incumbent{false};  ///< Grown from the incumbent seed.
    bool        second_pass{false};     ///< Admitted only in the walk's second pass.
    double      sigma_scale{1.0};       ///< The pin's sigma multiplier (a soft pin above 1).
    std::string life_stamp;             ///< The arc's life stamp, as a token.
    // Filled by the classification.
    ChainEventRow   row{ChainEventRow::Kept};        ///< The table row.
    bool            certified{false};                ///< A re-host whose integer the chord table certifies.
    ChainCertSource cert_source{ChainCertSource::None};  ///< Its source.
    long long       cert_int{0};                     ///< The certified integer (cycles).
    bool            conflict{false};                 ///< The pair and the lattice disagree.
    bool            returning{false};                ///< The child was absent at the last observed epoch and seen before.
    std::uint8_t    slip_cause{0};                   ///< Free-epoch cause bits of the arc (TreeEdgeSlip).
};

/// One component of one group.
struct ChainComponentObs {
    std::string              group;     ///< Group key.
    std::string              sys;       ///< Constellation key.
    std::string              signal;    ///< Phase signal.
    int                      comp{0};   ///< Index within the group.
    std::string              r1;        ///< Datum receiver name.
    int                      r1_station{-1};  ///< Its roster index.
    std::vector<int>         stations;  ///< Member stations (roster index).
    std::vector<std::string> sats;      ///< Member satellites.
    std::vector<std::string> bridged;   ///< Satellites grounded by a shared-side bridge row (roots).
    int                      declared{0};  ///< n_c + m_c, the gauge dimension.
    int                      n_edges{0};   ///< Tree edges recorded.
};

/// The fundamental cycle of one chord in the epoch's tree, with the tree pins on its path.
struct ChainChordCycle {
    int              station{-1};   ///< The chord's station (roster index).
    std::string      sat;           ///< Its satellite.
    std::string      signal;        ///< Its signal.
    arcgraph::Cycle  cycle;         ///< The chord (+1, station to satellite) and its tree path.
    double           pin_sum_cyc{0.0};  ///< Sum over the tree terms of coefficient times target (cycles).
    bool             pins_integer{true};  ///< Every target on the path is an integer number of cycles.
};

/// The observation of one epoch's chain (EpochEvents::PhaseDatum::chain).  Empty unless
/// network.trace_chain_datum is armed.
struct ChainEpochObs {
    bool armed{false};                        ///< The election recorded this epoch.
    std::vector<ChainEdgeObs>      edges;     ///< Tree edges, in election order.
    std::vector<ChainComponentObs> comps;     ///< Components, in election order.
    std::vector<ChainChordCycle>   chords;    ///< Chords of the tree, per component in arc order.
    std::map<std::pair<std::string, std::string>, double> shift_sat;  ///< (sat, signal) -> the legacy known frame step (m).
    std::map<std::pair<int, std::string>, double>         shift_rpb;  ///< (station, receiver phase-bias key) -> its step (m).
    long long gated_keys{0};                  ///< Usable (station, ambiguity) keys the coverage gate held out of the union.
    long long soft_pins{0};                   ///< Pins with a sigma multiplier above 1.
    long long bridges{0};                     ///< Shared-side bridge pins.
};

// ---- The chord table ----------------------------------------------------------------------------

/// One chord of the chord table: an arc carried in both bands of a pair family and a chord of both
/// bands' trees.
struct ChainChordRow {
    std::string family;          ///< "<constellation key>|<band-1 signal>+<band-2 signal>".
    std::string sig1;            ///< Band-1 signal.
    std::string sig2;            ///< Band-2 signal.
    int         station{-1};     ///< Station (roster index).
    std::string station_name;    ///< Its name.
    std::string sat;             ///< Satellite.
    double      float_cyc_1{0.0};   ///< Band-1 ambiguity (cycles).
    double      float_cyc_2{0.0};   ///< Band-2 ambiguity (cycles).
    double      q11{0.0};        ///< Its conditional covariance (cycles^2), unscaled.
    double      q12{0.0};
    double      q22{0.0};
    bool        have_int_1{false};  ///< The two-band fix determined band 1.
    bool        have_int_2{false};  ///< ... band 2.
    long long   int_1{0};
    long long   int_2{0};
    bool        have_lat_1{false};  ///< The lattice implies band 1's chord integer.
    bool        have_lat_2{false};
    long long   lat_1{0};
    long long   lat_2{0};
    bool        conflict{false};    ///< The pair and the lattice disagree on a band.
};

// ---- The observatory's run state ----------------------------------------------------------------

/// The observatory's memory of the last observed epoch (nothing in the solve reads it).
struct ChainFrameMemory {
    bool   have{false};      ///< An epoch has been observed.
    int    week{0};          ///< Its week.
    double tow{0.0};         ///< Its time of week (s).
    /// group -> vertex key -> component index at that epoch.
    std::map<std::string, std::map<std::string, int>> vertex_comp;
    /// group -> component index -> datum receiver name.
    std::map<std::string, std::map<int, std::string>> comp_root;
    /// group -> vertex key -> its parent vertex key at that epoch ("" for a root).
    std::map<std::string, std::map<std::string, std::string>> vertex_parent;
    /// group -> vertex key -> time (continuous GPS seconds) it was last a vertex.
    std::map<std::string, std::map<std::string, double>> last_seen_s;
    /// (station, satellite, signal) -> (chord row, band 1 or 2): the chord table of that epoch.
    std::map<std::tuple<int, std::string, std::string>, std::pair<ChainChordRow, int>> chords;
};

// ChainFrameCounters, the run tallies with this memory behind a pointer, is in
// network/solve/ChainFrameCounters.hpp.

// ---- The classification ---------------------------------------------------------------------------

/// One component's event at this epoch, against the last observed epoch.
struct ChainComponentEvent {
    std::string group;           ///< Group key.
    std::string signal;          ///< Phase signal.
    int         comp{0};         ///< Component index.
    std::string r1;              ///< Its datum receiver.
    bool        root_changed{false};  ///< Its root differs from its predecessor's (and it is no split piece).
    bool        split{false};    ///< It split off a component of the last epoch (not the piece that continues it).
    int         merged{0};       ///< Components of the last epoch it absorbed beyond the first.
    std::string old_root;        ///< The predecessor's datum receiver ("" when none).
};

/// One epoch's classification: the edges with their table rows and the component events.
struct ChainEpochClass {
    std::vector<ChainEdgeObs>        edges;   ///< The epoch's edges, classified, in election order.
    std::vector<ChainComponentEvent> comps;   ///< One per component, in election order.
};

/**
 * @brief Classify one epoch's chain against the memory of the last observed epoch.
 *
 * Each edge takes the first row that applies: TreeEdgeSlip (the free-epoch latch names its
 * arc); Kept (its arc was the child's incumbent parent edge, in this direction or flipped);
 * RehostCertified or RehostUncertified (the child had another incumbent parent edge; certified
 * when the memory's chord table determines the new arc's band integer and the lattice, when it
 * has one, agrees); Merge (child and parent lay in different components); Returning or Birth
 * (the child was absent, seen earlier or never); RootHandover (the child was a root).  A Kept
 * edge whose satellite remembers another root takes RootHandover, or Split in a split-off
 * piece.  Every count of @p cnt that the classification determines is accumulated.
 *
 * @param obs        The epoch's chain record.
 * @param mem        The memory of the last observed epoch.
 * @param free_ambs  This epoch's free-epoch latch, (station, ambiguity key).
 * @param free_cause Its cause bits.
 * @param cnt        [in/out] The run tallies.
 * @return The classification.
 */
ChainEpochClass chainClassifyEpoch(const ChainEpochObs& obs, const ChainFrameMemory& mem,
                                   const std::set<std::pair<int, std::string>>& free_ambs,
                                   const std::map<std::pair<int, std::string>, std::uint8_t>& free_cause,
                                   ChainFrameCounters& cnt);

/**
 * @brief The memory after an observed epoch: its vertices, components, parents, sightings and
 *        chord table.  Counts the vertices of the last epoch that left.
 *
 * @param obs    The epoch's chain record.
 * @param cls    Its classification.
 * @param chords This epoch's chord table, keyed (station, satellite, signal) -> (row, band).
 * @param week   The epoch's week.
 * @param tow    Its time of week (s).
 * @param now_s  Its time in continuous GPS seconds.
 * @param cnt    [in/out] The run tallies: the memory is replaced, cf_departed accumulated.
 */
void chainRememberEpoch(const ChainEpochObs& obs, const ChainEpochClass& cls,
                        std::map<std::tuple<int, std::string, std::string>, std::pair<ChainChordRow, int>> chords,
                        int week, double tow, double now_s, ChainFrameCounters& cnt);

// ---- The observer and the run end -----------------------------------------------------------------

/**
 * @brief Classify, trace and remember this epoch's chain (file banner, step 2).
 *
 * Observer of the StashTail slot.  Gate: network.trace_chain_datum (the chain trace and the
 * memory) or network.trace_rpb_states (the receiver phase-bias trace), on an eq34 run.  Writes
 * only its traces, its CHAIN event rows and @ref ChainFrameCounters.
 *
 * @param run The run (its context, traces, setup and engines).
 * @param ep  The epoch after the solve and the harvests.
 */
void networkChainFrameObserve(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Run-end report: the frame census of the eq34 chain datum.
 *
 * Gate: network.trace_chain_datum on an eq34 run.  Prints the events by table row with their
 * legacy step classes, the certified share by source, the conflicts and the wrong-certification
 * candidates, the targets, soft pins and bridges, the legacy re-references and fractional
 * shifts, the cut releases and declarations (0 while no row is armed), the component census,
 * the pin identity, the per-site moved-value counts of the chain shift, and the receiver
 * phase-bias time-link NEES when network.trace_rpb_states is armed.
 *
 * @param v The run at its end.
 * @return 0 (this report does not set the exit code).
 */
int networkChainFrameRunEnd(const NetworkRunEndView& v);

/**
 * @brief The certification of one chord: the two-band fix of its float with the scaled
 *        covariance.
 *
 * @param f1        Band-1 float (cycles).
 * @param f2        Band-2 float (cycles).
 * @param q11       Covariance (cycles^2), unscaled.
 * @param q12       Covariance.
 * @param q22       Covariance.
 * @param min_success_rate  The search's bootstrap gate.
 * @param ratio_threshold   The search's ratio gate.
 * @param ratio_upper_bound The search's ratio upper bound.
 * @param max_frac          The search's per-entry fraction cap.
 * @param row       [out] have_int_1/2 and int_1/2 set when determined and kept.
 */
void chainCertifyChord(double f1, double f2, double q11, double q12, double q22,
                       double min_success_rate, double ratio_threshold,
                       double ratio_upper_bound, double max_frac, ChainChordRow& row);

} // namespace GnssProcessor
