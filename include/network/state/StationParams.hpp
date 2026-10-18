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
 * (network/state/StationParamsShadow.hpp).  Nothing reads the set yet.
 *
 * Plain value types and free functions; no behaviour classes.
 */

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "network/assembly/ParamInventory.hpp"   // ParamDisposition, AbsorbedReason

#include <array>
#include <map>
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
 * @brief The slot of a parameter.
 *
 * @param set The set.
 * @param ref The parameter.
 * @return Its slot index, or -1 when the set does not hold it or holds it in no typed map.
 */
int findParam(const StationParams& set, const ParamRef& ref);

} // namespace GnssProcessor
