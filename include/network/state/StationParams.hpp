#pragma once
/**
 * @file StationParams.hpp
 * @brief The parameter set of one network station: its parameters, each with a typed
 *        identity, a linearization point and the disposition the split gave it, and the typed
 *        lookups over them.
 *
 * ### What the set is
 *
 * A station's model is a list of columns: the receiver position (held, never estimated), the
 * receiver clock, the zenith wet delay, and the families the network allocates per
 * station (inter-system biases, slant ionosphere and phase colour per satellite, ambiguities
 * per satellite and signal, receiver phase and code biases), together with the shared border
 * parameters the station's rows reference (satellite clocks and satellite phase and code
 * biases).  The set names each of them by a @ref ParamRef instead of a state-key string, and
 * keeps what a reader of the column needs: the linearization point the epoch's rows are
 * evaluated at, the split's disposition, and whether an ambiguity is marked to be re-seeded.
 *
 * ### The order contract
 *
 * @ref StationParams::slots is in column order, the order of the station's state vector:
 * parameters are appended when they are born and survivors keep their relative order.  The
 * order is part of the contract, because the Schur kernel factorizes the local block in
 * column order and the rounding of its result follows it.  The typed maps hold a slot index
 * and iterate in the order of the state-key strings they stand for (the comparators of
 * common/ParamRef.hpp and common/GnssIdentity.hpp), because the rank-1 update order and the
 * fold order walk the key maps in string order.
 *
 * ### Who writes it
 *
 * The set is the station's own state: no other object holds the station's columns.  It is
 * written inside the parallel produce phase by the station's own build, and by the serial
 * stages that withhold, prune or mark a station's parameters:
 *  - the station's layout at construction (@ref startStationParams): the receiver position,
 *    clock and wet delay, and one inter-system bias per active constellation other than the
 *    time reference;
 *  - the births, re-seeds and prunes of the local families, once per epoch before the rows
 *    (network/assembly/LocalBirth.hpp);
 *  - the admission and pruning of the shared satellite parameters the station references
 *    (network/assembly/SharedAdmission.hpp and the screens that remove observations);
 *  - the re-seed marks of the ambiguities (@ref markPendingReseed), set by the slip
 *    detectors and the screens and consumed by the next birth;
 *  - the split's dispositions (@ref settleStationParams).
 * A birth appends the parameter, so a parameter born in an epoch is the last of its station.
 * A reader asks the set by identity; none holds a state-key text for a column.
 *
 * ### What a reader sees, and when
 *
 * The set is complete after the station's births and the shared-state admission of the
 * epoch.  A reader that runs before the next birth reads the previous epoch's set and is
 * right, since nothing changes a local column between two births except the marks and the
 * prunes the set itself records.  A re-seed mark set after the births (by a screen after the
 * solve) is seen by the next reader, which is why the marks are the set's own and not a
 * property of a slot.
 *
 * Plain value types and free functions; no behaviour classes.
 */

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "network/assembly/ParamInventory.hpp"   // ParamDisposition, AbsorbedReason

#include <Eigen/Dense>

#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;

/**
 * @brief One parameter of a station's set.
 */
struct LocalSlot {
    ParamRef         ref;                                       ///< The parameter.
    double           x_lin{0.0};                                ///< Its linearization point: the value the epoch's rows are evaluated at.
    ParamDisposition disp{ParamDisposition::Unobserved};        ///< What the split did with it this epoch.
    /// An ambiguity the integer search leaves out because no phase bias is applied to its
    /// signal this epoch (the measurement pre-pass marks it, and clears every mark first).
    bool             unresolvable{false};
    /// A column here that no typed identity names (a layout column of
    /// the single-receiver filter, or a key the classifier refuses).  @ref ref is left at its
    /// default and means nothing (it would read as the receiver clock); the slot is in no typed
    /// map, so no reader of a family sees it, and the inventory lists it as unclassified.  It
    /// keeps slot k the column k.
    bool             unnamed{false};
    /// The elevation (degrees) of the line of sight a phase colour belongs to this epoch, the
    /// argument of its stationary prior; NaN for every other parameter and for a phase colour
    /// with none recorded.
    double           elevation_deg{std::numeric_limits<double>::quiet_NaN()};
};

/**
 * @brief A shared satellite parameter a gate withheld from the station this epoch, with the
 *        gate.
 *
 * Another parameter of the station takes it up (@ref absorbedBy); the parameter has no slot.
 */
struct AbsorbedParam {
    ParamRef       ref;                                                        ///< The withheld parameter (a satellite phase or code bias).
    AbsorbedReason reason{AbsorbedReason::GlonassCodeOnlyScope};               ///< The gate that withheld it.
};

/**
 * @brief The shared border parameters the station holds, and the ones withheld from it.
 *
 * The maps hold slot indices into @ref StationParams::slots.
 */
struct StationShared {
    std::map<SatId, int>       sat_clock;       ///< Satellite clocks, in the order of "G05|SC".
    std::map<SatSignalId, int> sat_phase_bias;  ///< Satellite phase biases, in the order of "G05|L1C|PB".
    std::map<SatSignalId, int> sat_code_bias;   ///< Satellite code biases, in the order of "G05|C1C".
    /// The shared parameters the gates withheld from the station this epoch, in the order the
    /// gates ran: cleared when the station's shared parameters are admitted, appended by each
    /// gate that withholds one.  @ref settleStationParams sorts them into @ref absorbed and
    /// @ref absorbed_no_absorber.
    std::vector<AbsorbedParam> withheld;
    /// Labels of withheld parameters whose identity could not be typed (a signal the
    /// constellation does not carry), in the gates' order: "absorbed <container>:<key>".
    std::vector<std::string>   withheld_unnamed;
    std::vector<AbsorbedParam> absorbed;        ///< Withheld this epoch whose absorbing column the station holds, in the gates' order.
    /// Withheld this epoch whose absorbing column the station no longer holds at the split (the
    /// observation left before it): counted, not in @ref absorbed.
    long long                  absorbed_no_absorber{0};
};

/**
 * @brief The linearization points a warm resume installs for a station's receiver code biases,
 *        used in place of the first-epoch median seed.
 *
 * Keyed by the text of the receiver code-bias key ("GPS_DCB", "GPS_IFB5"), the form the
 * snapshot stores.  The two counters belong to the station: its birth reports into them, and
 * the run-end readout sums them over the stations.
 */
struct CodeBiasRestore {
    std::map<std::string, double> by_key;          ///< Installed value (m) per key text; empty: nothing restored.
    long long*                    applied{nullptr};   ///< Out-slot incremented per covered key seeded from the map.
    long long*                    uncovered{nullptr}; ///< Out-slot incremented per covered key the map could not seed.
};

/**
 * @brief Which first-epoch seeds of a station have run.
 *
 * A seed that finds no reference observation retries on the next epoch and does not latch.
 */
struct SeedLatches {
    bool isb{false};         ///< The inter-system biases are seeded.
    bool cold_clock{false};  ///< The receiver clock is seeded (or was not cold).
    bool code_bias{false};   ///< The receiver code biases are seeded.
};

/**
 * @brief The parameter set of one station.
 *
 * Every typed map holds a slot index.  A parameter with no typed map (a form only the
 * single-receiver filter allocates: iono-free, narrow-lane or wide-lane ambiguity, phase
 * clock, gradients) still has its slot, counted in @ref untyped.
 */
struct StationParams {
    std::vector<LocalSlot> slots;   ///< Every parameter, in column order.

    std::array<int, 3> position{-1, -1, -1};   ///< Slots of the receiver position components (x, y, z).
    int clock{-1};                              ///< Slot of the receiver clock.
    int zwd{-1};                                ///< Slot of the zenith wet delay.

    /// Inter-system biases, in the order of the constellation names ("GLONASS" < "GPS" < "Galileo").
    std::map<System, int, SystemNameOrder>      isb;
    std::map<SatId, int>                        iono;     ///< Slant ionospheres, in the order of "G05".
    std::map<SatId, int>                        colour;   ///< Phase colours, in the order of "G05".
    std::map<SatSignalId, int>                  amb;      ///< Ambiguities, in the order of "G05_L1C".
    std::map<SignalId, int, SignalNameOrder>    rx_pb;    ///< Receiver phase biases, in the order of "GPS_RPB_L1C".
    std::map<RxCbKey, int>                      rx_cb;    ///< Receiver code biases, in the order of "GPS_DCB", "GPS_IFB5", "GPS_ISC_C1W".

    StationShared shared;   ///< The border parameters.

    /// Ambiguities marked to be re-seeded at their next birth.  The marks are the set's, not
    /// the columns': a detector marks an arc whose column the pre-pass is about to create (or
    /// has pruned), and the birth that consumes the mark clears it.
    std::set<SatSignalId> pending_reseed;

    SeedLatches    seeded;        ///< The first-epoch seeds that have run.
    CodeBiasRestore rx_cb_restore; ///< The warm resume's receiver code-bias points.

    /// Records the ambiguities a prune removes (the arc ledger audits them); set once for the
    /// run by the roster.
    bool record_pruned{false};
    /// The ambiguities pruned since they were last taken, in prune order.
    std::vector<SatSignalId> pruned_ambiguities;

    int untyped{0};         ///< Slots no typed map holds.

    /// Slots below this index have a disposition from the split.  A named slot at or above it
    /// (the split's vector is shorter than the state: a defect) keeps @c Unobserved and is
    /// counted by the inventory, never recorded.
    std::size_t decided{0};

    /// "<container>:<key>" of each state column the classifier refused (held as an unnamed
    /// slot), then "absorbed <container>:<key>" of each withheld state it refused.  Empty on a
    /// sound run; printed by the inventory as its integrity line.
    std::vector<std::string> unclassified;
};

/**
 * @brief Append a parameter to a station's set and register it in its typed map.
 *
 * The slot is always appended, so slot k stays column k; the return value says whether a
 * typed lookup now finds it.  A second parameter with a key a map already holds (two
 * columns whose keys read as one identity) is not registered.
 *
 * @param set  The set (written).
 * @param slot The parameter, its linearization point and its disposition.
 * @return True when the typed map of the parameter's kind took it; false when it has no typed
 *         map or the key was taken (counted in @ref StationParams::untyped).
 */
bool addParam(StationParams& set, const LocalSlot& slot);

/**
 * @brief The slot a typed map entry or a state column names.
 *
 * @param set   The set.
 * @param index A slot index (a value of a typed map, a state column), in range.
 * @return The slot.
 */
inline const LocalSlot& slotAt(const StationParams& set, int index) {
    return set.slots[static_cast<std::size_t>(index)];
}

/**
 * @brief The slot of a parameter.
 *
 * @param set The set.
 * @param ref The parameter.
 * @return Its slot index, or -1 when the set does not hold it or holds it in no typed map.
 */
int findParam(const StationParams& set, const ParamRef& ref);

/**
 * @brief The slot of the ambiguity a state-key text names.
 *
 * For a reader whose identity is the text of an election or a funnel ("G05_L1C").
 *
 * @param set The set.
 * @param key The ambiguity key, "<satellite>_<phase signal>".
 * @return The ambiguity's slot index, or -1 when the key does not read as an ambiguity or the
 *         set does not hold it.
 */
int findAmbiguity(const StationParams& set, const std::string& key);

/**
 * @brief The slot of the slant ionosphere of a satellite text.
 *
 * @param set The set.
 * @param sat The satellite ("G05").
 * @return The slot index, or -1 when the text does not read as a satellite or the set holds no
 *         slant ionosphere for it (the old @c getIonosphereIndex).
 */
int findSlantIono(const StationParams& set, const std::string& sat);

/**
 * @brief The signal a receiver phase-bias state-key text names.
 *
 * @param key The receiver phase-bias key, "<system name>_RPB_<phase signal>" ("GPS_RPB_L1C").
 * @return The signal (its system is the key's), or @c std::nullopt when the key does not read
 *         as a receiver phase bias.
 */
std::optional<SignalId> receiverPhaseBiasSignal(const std::string& key);

/**
 * @brief The slot of the receiver phase bias a state-key text names.
 *
 * @param set The set.
 * @param key The receiver phase-bias key, "<system name>_RPB_<phase signal>" ("GPS_RPB_L1C").
 * @return The slot index, or -1 when the key does not read as a receiver phase bias or the set
 *         does not hold it (the old @c getReceiverPhaseBiasIndex).
 */
int findReceiverPhaseBias(const StationParams& set, const std::string& key);

/**
 * @brief The typed key of a receiver code-bias key text ("GPS_DCB", "GPS_IFB5",
 *        "GLONASS_IFB2_k-4", "GPS_ISC_C1W").
 *
 * For a reader whose identity is a key a configuration, a snapshot row or a carry holds as
 * text.  A system name that is not a known constellation gives @c System::Unknown for the forms
 * that carry no signal (a differential or inter-frequency bias), so that the receiver IFB rule
 * names the keys the text test on "_IFB" named; an inter-signal correction needs a known system.
 *
 * @param key The key text.
 * @return The key, or @c std::nullopt when the text is none of the four forms.
 */
std::optional<RxCbKey> receiverCodeBiasKey(const std::string& key);

/**
 * @brief The slot of the receiver code bias a state-key text names.
 *
 * For a reader whose identity is the key a carry or a snapshot row holds ("GPS_DCB",
 * "GPS_IFB5", "GPS_ISC_C1W", "GLONASS_IFB2_k-4"): the key is compared with the text of each
 * receiver code bias of the set (@ref keyText), so the lookup agrees with the key every
 * writer of such a row renders.
 *
 * @param set The set.
 * @param key The receiver code-bias key.
 * @return The slot index, or -1 when the set holds no receiver code bias with that key.
 */
int findReceiverCodeBias(const StationParams& set, const std::string& key);

/**
 * @brief The state-key text of a parameter, as the traces print it.
 *
 * The text the single-receiver filter keyed the column by, so that a trace column that
 * has always carried a state key keeps its bytes: "G05" (slant ionosphere, phase colour),
 * "G05_L1C" (ambiguity), "GPS_DCB", "GPS_IFB5", "GPS_ISC_C1W" (receiver code bias), "Galileo"
 * (inter-system bias), "GPS_RPB_L1C" (receiver phase bias), "G05|C1C" (satellite code bias),
 * "G05|L1C|PB" (satellite phase bias), "G05|SC" (satellite clock).
 *
 * @param ref The parameter.
 * @return The key; empty for a parameter with no text key (the fixed core) and for an identity
 *         the key formats cannot render.
 */
std::string keyText(const ParamRef& ref);

/**
 * @brief The text of a satellite phase bias as the epoch's shared-key sets carry it
 *        ("G05|L1C|PB").
 * @param id The satellite and phase signal.
 * @return The key text.
 */
std::string satPhaseBiasText(const SatSignalId& id);

/**
 * @brief The text of a satellite code bias as the epoch's shared-key sets carry it
 *        ("G05|C1C").
 * @param id The satellite and code signal.
 * @return The key text.
 */
std::string satCodeBiasText(const SatSignalId& id);

/**
 * @brief The text of a satellite's clock ("G05|SC").
 * @param sat The satellite.
 * @return The key text.
 */
std::string satClockText(const SatId& sat);

/**
 * @brief The label of a slot's column in the single-receiver layout, for a
 *        slot of the set.
 *
 * "POS_X", "CLK", "ZWD", "ISB_Galileo" for the core and the inter-system biases; the family's
 * prefix and the key for the dynamic families ("AMB:G05_L1C", "IONO:G05", "CBIAS:GPS_DCB",
 * "SATB:G05|C1C", "SATPB:G05|L1C|PB", "RPB:GPS_RPB_L1C", "PCOL:G05", "SATCLK:G05|SC"; the
 * prefixes are the rows of @c kStateFamilies, a trace contract).  A column no family names has
 * the layout's own "STATE_<index>".
 *
 * @param slot  The slot.
 * @param index The slot's index, which is its state column.
 * @return The label.
 */
std::string stateLabel(const LocalSlot& slot, std::size_t index);

// =======================================================================
// Writing the set
// =======================================================================

/**
 * @brief The set of a station before its first epoch: the fixed layout.
 *
 * Slots 0 to 2 are the receiver position (held at @p position, never estimated), slot 3 the
 * receiver clock, slot 4 the zenith wet delay, and one inter-system bias follows for each
 * active constellation other than the time reference, in the order of the configuration's
 * list.  Every value but the position is 0.  The layout does not follow the single-receiver
 * filter's optional columns (velocity, clock rate, phase clock, gradients, IMU states): a
 * network station has none.
 *
 * @param config   Processing configuration (the active constellations).
 * @param position The receiver's ECEF position (m).
 * @return The set.
 */
StationParams startStationParams(const GnssConfig& config, const Eigen::Vector3d& position);

/**
 * @brief Remove slots from the set, keeping the others in order.
 *
 * The typed maps drop the entries that point at a removed slot and renumber the rest, so
 * slot k stays column k of the set that remains.  The core slots (position, clock, wet delay)
 * are never removed.
 *
 * @param set     The set (written).
 * @param indices The slots to remove (any order; duplicates and out-of-range indices are
 *                ignored).
 */
void removeSlots(StationParams& set, std::vector<int> indices);

/**
 * @brief Remove the ambiguities that are not in a keep set.
 *
 * With @ref StationParams::record_pruned set, the removed ambiguities are appended to
 * @ref StationParams::pruned_ambiguities in key order.
 *
 * @param set  The set (written).
 * @param keep The ambiguities to keep.
 */
void pruneAmbiguitiesExcept(StationParams& set, const std::set<SatSignalId>& keep);

/// Remove the slant ionospheres of the satellites that are not in @p keep.
void pruneSlantIonoExcept(StationParams& set, const std::set<SatId>& keep);

/// Remove the phase colours of the satellites that are not in @p keep.
void prunePhaseColourExcept(StationParams& set, const std::set<SatId>& keep);

/// Remove the receiver code biases that are not in @p keep.
void pruneReceiverCodeBiasExcept(StationParams& set, const std::set<RxCbKey>& keep);

/// Remove the receiver phase biases whose signal is not in @p keep.
void pruneReceiverPhaseBiasExcept(StationParams& set, const std::set<SignalId, SignalNameOrder>& keep);

/// Remove the satellite clocks of the satellites that are not in @p keep.
void pruneSatClockExcept(StationParams& set, const std::set<SatId>& keep);

/// Remove the satellite phase biases that are not in @p keep.
void pruneSatPhaseBiasExcept(StationParams& set, const std::set<SatSignalId>& keep);

/// Remove the satellite code biases that are not in @p keep.
void pruneSatCodeBiasExcept(StationParams& set, const std::set<SatSignalId>& keep);

/**
 * @brief Mark an ambiguity to be re-seeded at its next birth.
 *
 * A mark on an arc whose column does not exist is kept: the birth that creates the column
 * consumes it.
 *
 * @param set The set (written).
 * @param id  The ambiguity.
 */
void markPendingReseed(StationParams& set, const SatSignalId& id);

/**
 * @brief The same mark by the ambiguity's text key ("G05_L1C").
 *
 * A key that does not read as a satellite and a phase signal marks nothing.
 *
 * @param set The set (written).
 * @param key The ambiguity key.
 */
void markPendingReseed(StationParams& set, const std::string& key);

/**
 * @brief Whether an ambiguity is marked to be re-seeded.
 * @param set The set.
 * @param id  The ambiguity.
 * @return True when marked, whether or not its column exists.
 */
bool pendingReseed(const StationParams& set, const SatSignalId& id);

/// The same test by the ambiguity's text key; false for a key that does not read as an ambiguity.
bool pendingReseed(const StationParams& set, const std::string& key);

/// Clear the mark of an ambiguity (no effect when unmarked).
void clearPendingReseed(StationParams& set, const SatSignalId& id);

/// The same by the ambiguity's text key; a key that does not read as an ambiguity clears nothing.
void clearPendingReseed(StationParams& set, const std::string& key);

/**
 * @brief Take the recorded pruned ambiguities.
 * @param set The set (the record is cleared).
 * @return The ambiguities pruned since the last take, in prune order.
 */
std::vector<SatSignalId> takePrunedAmbiguities(StationParams& set);

/**
 * @brief Record what the split decided for each slot, and sort the withheld shared parameters.
 *
 * Slot k takes @p col_disp[k] (a slot beyond the vector is left @c Unobserved and is not
 * @ref StationParams::decided).  Each withheld shared parameter whose absorbing column the
 * station holds becomes an @ref StationShared::absorbed entry: a satellite phase bias is
 * taken up by the float ambiguity of the same satellite and signal, a satellite code bias by
 * the satellite's clock column.  One whose column is gone (the observation left before the
 * split) is counted in @ref StationShared::absorbed_no_absorber.  The unclassified list is
 * rebuilt from the withheld parameters whose identity could not be typed.
 *
 * @param set      The set (written).
 * @param col_disp The split's disposition per slot.
 */
void settleStationParams(StationParams& set, const std::vector<ParamDisposition>& col_disp);

} // namespace GnssProcessor
