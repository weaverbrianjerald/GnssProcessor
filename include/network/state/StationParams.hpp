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
 * ### Today
 *
 * The set is built from the station's state manager, one slot per state column (slot k is
 * column k), after the station's measurement pre-pass and split, and checked against it
 * (network/state/StationParamsShadow.hpp).  The parameter inventory, the trace labels, the
 * diagnostics and the carried-prior steps of PASS B (the ambiguity, slant ionosphere, receiver
 * clock, receiver code-bias, phase-colour and wet-delay carries, the local block, and the
 * double-difference prior build) read it, and so do the slip drains, the admissions' reads of
 * the ambiguity columns, the phase-datum and anchor rows and the ionosphere constraint rows.
 * The columns are still born in the state manager, so the writers of a column (births, prunes,
 * the re-seed marks) are the state manager's, and a reader that needs what a later stage of the
 * epoch changes (a re-seed mark set by a screen after the split, a border column the
 * admissions allocate or prune) still asks the state manager.  The set holds the values of the
 * split: the linearization points and the dispositions as they were when the station's rows
 * were built.
 *
 * The set is rebuilt when a station reaches its split, and, without dispositions, when its
 * assembly ends before the split (@c networkRefreshStationParams): the measurement pre-pass is
 * the one writer of a local column outside the split, so after every assembly the set holds the
 * station's local columns as the state manager does, and a reader that runs before the next
 * split (the slip drains, the admissions, the election) reads the previous epoch's set and is
 * right.  @c networkCheckStationParamsFresh verifies this at the start of every epoch.  A reader
 * of a border column, or of a local column after a stage that changed it, reads the state
 * manager.  A station that was not assembled keeps its last build.  A station whose set cannot
 * be built fails its assembly for the epoch (the run names it and drops it), so that no reader
 * meets an empty set.
 *
 * Plain value types and free functions; no behaviour classes.
 */

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "network/assembly/ParamInventory.hpp"   // ParamDisposition, AbsorbedReason

#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief One parameter of a station's set.
 */
struct LocalSlot {
    ParamRef         ref;                                       ///< The parameter.
    double           x_lin{0.0};                                ///< Its linearization point: the value the epoch's rows are evaluated at.
    ParamDisposition disp{ParamDisposition::Unobserved};        ///< What the split did with it this epoch.
    bool             pending_reseed{false};                     ///< An ambiguity marked to be re-seeded at its next use.
    /// The state manager holds a column here that no typed identity names (a layout column of
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
    std::vector<AbsorbedParam> absorbed;        ///< Withheld this epoch whose absorbing column the station holds, in the gates' order.
    /// Withheld this epoch whose absorbing column the station no longer holds at the split (the
    /// observation left before it): counted, not in @ref absorbed.
    long long                  absorbed_no_absorber{0};
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
 *         set does not hold it (the old state manager's @c getAmbiguityIndex).
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
 * @brief The state-key text of a parameter, as the traces print it.
 *
 * The text the single-receiver state manager keys the column by, so that a trace column that
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
 * @brief The label the state manager gives a column (@c StateManager::getStateLabels), for a
 *        slot of the set.
 *
 * "POS_X", "CLK", "ZWD", "ISB_Galileo" for the core and the inter-system biases; the family's
 * prefix and the key for the dynamic families ("AMB:G05_L1C", "IONO:G05", "CBIAS:GPS_DCB",
 * "SATB:G05|C1C", "SATPB:G05|L1C|PB", "RPB:GPS_RPB_L1C", "PCOL:G05", "SATCLK:G05|SC"; the
 * prefixes are the rows of @c kStateFamilies, a trace contract).  A column no family names has
 * the state manager's own "STATE_<index>".
 *
 * @param slot  The slot.
 * @param index The slot's index, which is its state column.
 * @return The label.
 */
std::string stateLabel(const LocalSlot& slot, std::size_t index);

} // namespace GnssProcessor
