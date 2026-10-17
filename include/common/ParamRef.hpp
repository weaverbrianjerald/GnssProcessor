#pragma once
/**
 * @file ParamRef.hpp
 * @brief The identity of a network parameter: its kind, form, class and instance, and the
 *        order of the state keys of the station-local families.
 *
 * ### The identities
 *
 *  - @ref NetworkParamKind: the parameter kinds the network estimates (the registry
 *    declares one property set per kind, network/assembly/ParamRegistry.hpp).
 *  - @ref ParamForm: the shape of an instance where one kind has more than one.
 *  - @ref ParamClass: one line of the "parameters as armed" table (kind, form, constellation,
 *    signals or bands).
 *  - @ref ParamInstanceId: a class plus the subject fields a table line sums over
 *    (satellite, GLONASS slot, station).
 *  - @ref ParamRef: one parameter without its station, the identity the observation equation
 *    names its terms by.
 *
 * They live in common/ so that code on the user side of the estimator, which must include no
 * network header, can name a network parameter.
 *
 * ### The order of the state keys
 *
 * The network's rank-1 update order and its fold order iterate the station's key maps in
 * the @c std::string order of their keys, so a map re-keyed by a typed identity keeps the
 * numbers only if the typed order reproduces the string order exactly.  The satellite and
 * signal identities of common/GnssIdentity.hpp do (they order by RINEX letter, which is
 * the string order of a satellite id).  A key that opens with the constellation NAME does
 * not: the names order as "GLONASS" < "GPS" < "Galileo", the letters as 'E' < 'G' < 'R'.
 * The families that open with a name, and the typed key and comparator that reproduces each:
 *
 *  - inter-system bias, key "Galileo": @c System ordered by @ref SystemNameOrder;
 *  - receiver phase bias, key "GPS_RPB_L1C": @c SignalId ordered by @ref SignalNameOrder;
 *  - receiver code bias, keys "GPS_DCB", "GPS_IFB5", "GLONASS_IFB1_k-4", "GPS_ISC_C1W":
 *    @ref RxCbKey;
 *  - ambiguity, satellite bias and satellite phase bias, keys "G05_L1C", "G05|C1C",
 *    "G05|L1C|PB": @c SatSignalId (GnssIdentity.hpp);
 *  - slant ionosphere, phase colour and satellite clock, keys "G05", "G05", "G05|SC":
 *    @c SatId (GnssIdentity.hpp).
 *
 * The receiver code-bias keys sort by name, then DCB before the IFB keys before the ISC
 * keys, the IFB keys by band digit with the per-band key ahead of its per-slot keys, and the
 * per-slot keys by the text of the signed slot ("+0" ... "+6" before "-1" ... "-7"): the
 * order of the declared form (@ref ParamForm) alone does not give this.  The tests in
 * tests/test_param_ref.cpp compare every order above with the string order over the signals
 * of the golden configurations and over every classified signal.
 *
 * Plain value types and free functions.
 */

#include "common/GnssIdentity.hpp"   // System, SatId, SignalId, StationId, FrequencyBand

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace GnssProcessor {

// =======================================================================
// Kinds, forms, classes and instances
// =======================================================================

/**
 * @brief Every parameter kind of the network estimator.
 *
 * The first four are the shared border (the satellite parameters every station sees,
 * the existing @c SharedParamKind values, which stay as the border subset); the rest
 * are station-local (a column of one station's local block).  The gross-screen holds are
 * instances of Ambiguity and SlantIono with a hold state, not kinds.
 */
enum class NetworkParamKind : int {
    SatClock = 0,     ///< Satellite clock (border; signal-independent).
    SatPhaseBias,     ///< Satellite phase bias per signal (border; carries the pivot lump).
    SatCodeBias,      ///< Satellite code bias per signal (border).
    VtecSh,           ///< Global SH-VTEC coefficient (border; network-wide).
    Ambiguity,        ///< Float ambiguity per (station, satellite, signal) (local).
    Zwd,              ///< Zenith wet delay per station (local).
    SlantIono,        ///< Slant ionosphere per (station, satellite) (local).
    RxClock,          ///< Receiver clock and ISB per station (local).
    RxPhaseBias,      ///< Receiver phase bias of the eq34 chain datum (local).
    RxCodeBias,       ///< Receiver code bias per (station, signal) (local).
    PhaseColour,      ///< Band-common phase nuisance per (station, satellite) (local).
    /// Receiver position per station: three columns of the station's state manager that
    /// the network holds at the roster coordinate and never estimates (the
    /// parameter inventory's coordinate instances, treatment Pinned).
    RxPosition,
};

/// Number of @ref NetworkParamKind enumerators (the registry's table size).
inline constexpr std::size_t kNetworkParamKindCount = 12;

/**
 * @brief Lower-case table name of a kind (e.g. "sat_clock").
 * @param kind The kind.
 * @return The name; "?" for a value outside the enumeration.
 */
const char* paramKindName(NetworkParamKind kind);

/**
 * @brief The form of an instance, where one registry kind has more than one shape.
 *
 * The registry declares the receiver clock and its inter-system biases as
 * one kind, because one carry, its three response sites and its value site act on
 * both; likewise the receiver code bias and the ambiguity forms.  The inventory tells
 * the shapes apart, so a table line is never "inter-system bias | rx_clock".  The
 * (kind, form) pair decides which fields a class carries (the table in
 * @ref ParamClass).
 */
enum class ParamForm : std::uint8_t {
    None = 0,    ///< The kind's plain form.
    Isb,         ///< rx_clock: the inter-system bias of one constellation ("<system>", @c StateKeys::makeSystemTimeOffsetKey).
    IfPair,      ///< ambiguity: of an ionosphere-free signal pair ("<sat>_L1C_L2W").
    NarrowLane,  ///< ambiguity: narrow-lane, per band (@c StateKeys::makeNlAmbiguityKey).
    WideLane,    ///< ambiguity: wide-lane, per band pair (@c StateKeys::makeWlAmbiguityKey).
    Dcb,         ///< rx_code_bias: "<system>_DCB", the differential code bias of the clock-reference pair.
    Ifb,         ///< rx_code_bias: "<system>_IFB<band>", the inter-frequency bias of a band outside that pair.
    Isc,         ///< rx_code_bias: "<system>_ISC_<signal>", the inter-signal correction of a second code.
    GloSlotIfb,  ///< rx_code_bias: "<system>_IFB<band>_k<slot>", the GLONASS per-slot inter-frequency bias.
};

/**
 * @brief The name of a form, appended to the kind in the table ("rx_code_bias.ifb").
 * @param f The form.
 * @return "" for None; "isb", "if", "nl", "wl", "dcb", "ifb", "isc" or "slot".
 */
const char* paramFormName(ParamForm f);

/**
 * @brief One line of the table: an instance class.
 *
 * The fields a class carries are fixed by its (kind, form); every other field keeps its
 * default.  "phase" and "code" name how the signal is rendered (L1C, C1C).
 *
 * | kind            | form            | sys | signal    | signal_b  | band | band_b |
 * |-----------------|-----------------|-----|-----------|-----------|------|--------|
 * | sat_clock       | none            |  x  |           |           |      |        |
 * | sat_phase_bias  | none            |  x  | phase     |           |      |        |
 * | sat_code_bias   | none            |  x  | code      |           |      |        |
 * | ambiguity       | none            |  x  | phase     |           |      |        |
 * | ambiguity       | if              |  x  | phase (1) | phase (2) |      |        |
 * | ambiguity       | nl              |  x  |           |           |  x   |        |
 * | ambiguity       | wl              |  x  |           |           |  x   |   x    |
 * | zwd             | none            |     |           |           |      |        |
 * | slant_iono      | none            |  x  |           |           |      |        |
 * | rx_clock        | none            |     |           |           |      |        |
 * | rx_clock        | isb             |  x  |           |           |      |        |
 * | rx_phase_bias   | none            |  x  | phase     |           |      |        |
 * | rx_code_bias    | dcb             | (x) |           |           |      |        |
 * | rx_code_bias    | ifb             | (x) |           |           |  x   |        |
 * | rx_code_bias    | isc             |  x  | code      |           |      |        |
 * | rx_code_bias    | slot            | (x) |           |           |  x   |        |
 * | phase_colour    | none            |  x  |           |           |      |        |
 * | rx_position     | none            |     |           |           |      |        |
 *
 * (x): the receiver code-bias forms without a signal keep their class when the system
 * name in the key is not a known constellation (@c System::Unknown); see
 * @c classifyStateColumn.  A band is read from the key's RINEX digit through
 * @ref bandOfDigit (BeiDou digit '7' reads as B2I).  @c vtec_sh is never a station
 * column.
 */
struct ParamClass {
    NetworkParamKind kind{NetworkParamKind::SatClock};  ///< Registry kind.
    ParamForm        form{ParamForm::None};             ///< Form within the kind.
    System           sys{System::Unknown};              ///< Constellation.
    SignalId         signal{};                          ///< Signal (the first member of an ionosphere-free pair).
    SignalId         signal_b{};                        ///< Second member of an ionosphere-free pair.
    FrequencyBand    band{FrequencyBand::Unknown};      ///< Band (receiver IFB, slot IFB, narrow lane, first band of a wide lane).
    FrequencyBand    band_b{FrequencyBand::Unknown};    ///< Second band of a wide lane.
};

/**
 * @brief Order of two classes: kind, form, constellation (enum order; none first),
 *        bands, signals.  The table's line order.
 * @param a First class.
 * @param b Second class.
 * @return True when @p a orders before @p b.
 */
bool operator<(const ParamClass& a, const ParamClass& b);

/**
 * @brief The identity of one parameter instance: its class plus the subject fields
 *        that a table line sums over.
 *
 * @ref sat is set for the kinds keyed by satellite (sat_clock, sat_phase_bias,
 * sat_code_bias, ambiguity, slant_iono, phase_colour); @ref glo_slot for form
 * @c GloSlotIfb only; @ref station for every instance (by
 * @ref recordStationParams).  The class is a member, not a copy: the line an
 * instance counts on is @c id.cls.
 */
struct ParamInstanceId {
    ParamClass cls{};        ///< The class (the table line).
    SatId      sat{};        ///< The satellite (satellite-keyed kinds).
    int        glo_slot{0};  ///< The GLONASS FDMA slot (form GloSlotIfb).
    StationId  station{};    ///< The station.
};

// =======================================================================
// The order of the station-local state keys
// =======================================================================

/**
 * @brief Order of two constellations by name: "BeiDou" < "GLONASS" < "GPS" < "Galileo" <
 *        "QZSS" < "SBAS", the string order of a state key that opens with the name.
 *
 * Differs from the order of @ref SatId, which follows the RINEX letter (BeiDou 'C', Galileo
 * 'E', GPS 'G', GLONASS 'R').  Equal to the order of the full keys because no constellation
 * name is a prefix of another.
 *
 * @param a First constellation.
 * @param b Second constellation.
 * @return True when the name of @p a orders before the name of @p b.
 */
inline bool systemNameLess(System a, System b) {
    return std::string_view(systemName(a)) < std::string_view(systemName(b));
}

/**
 * @brief Map comparator for the inter-system-bias family (key = the system name): the order
 *        of @ref systemNameLess.
 */
struct SystemNameOrder {
    /**
     * @brief Order of two constellations by name.
     * @param a First constellation.
     * @param b Second constellation.
     * @return @ref systemNameLess.
     */
    bool operator()(System a, System b) const { return systemNameLess(a, b); }
};

/**
 * @brief Map comparator for the receiver phase-bias family (key "<system>_RPB_<signal>"):
 *        the constellation name, then the band digit and the tracking attribute.
 *
 * The signal of one constellation orders as the text of its observation code (the
 * observation type is the same in every key of the map).
 */
struct SignalNameOrder {
    /**
     * @brief Order of two signals by (constellation name, digit, attribute).
     * @param a First signal.
     * @param b Second signal.
     * @return True when @p a orders before @p b.
     */
    bool operator()(const SignalId& a, const SignalId& b) const {
        if (a.sys != b.sys) return systemNameLess(a.sys, b.sys);
        if (a.digit != b.digit) return a.digit < b.digit;
        return a.attribute < b.attribute;
    }
};

/**
 * @brief The key of a station's receiver code-bias map: one differential bias, inter-frequency
 *        bias, inter-signal correction or GLONASS per-slot inter-frequency bias.
 *
 * The fields a form does not use keep their defaults, as @c classifyStateColumn leaves them:
 *
 * | form        | state key                  | fields used          |
 * |-------------|----------------------------|----------------------|
 * | Dcb         | "GPS_DCB"                  | sys                  |
 * | Ifb         | "GPS_IFB5"                 | sys, band            |
 * | GloSlotIfb  | "GLONASS_IFB1_k-4"         | sys, band, glo_slot  |
 * | Isc         | "GPS_ISC_C1W"              | sys, signal          |
 *
 * Ordered as the state keys: see @ref operator<(const RxCbKey&, const RxCbKey&).
 */
struct RxCbKey {
    System        sys{System::Unknown};         ///< Constellation.
    ParamForm     form{ParamForm::Dcb};         ///< Dcb, Ifb, GloSlotIfb or Isc.
    FrequencyBand band{FrequencyBand::Unknown}; ///< Band (Ifb, GloSlotIfb).
    SignalId      signal{};                     ///< Code signal (Isc).
    int           glo_slot{0};                  ///< GLONASS FDMA channel number (GloSlotIfb).
};

/**
 * @brief Equality of two receiver code-bias keys (all fields).
 * @param a First key.
 * @param b Second key.
 * @return True when every field is equal.
 */
inline bool operator==(const RxCbKey& a, const RxCbKey& b) {
    return a.sys == b.sys && a.form == b.form && a.band == b.band && a.signal == b.signal
        && a.glo_slot == b.glo_slot;
}

/**
 * @brief Inequality of two receiver code-bias keys.
 * @param a First key.
 * @param b Second key.
 * @return @c !(a == b).
 */
inline bool operator!=(const RxCbKey& a, const RxCbKey& b) { return !(a == b); }

/**
 * @brief Order of two receiver code-bias keys: the string order of their state keys.
 *
 * By constellation name; then Dcb before the Ifb and GloSlotIfb keys before the Isc keys;
 * the Ifb family by band digit, the per-band key ahead of its per-slot keys and the per-slot
 * keys by the text of the signed slot ("+5" before "-4", "-1" before "-2"); the Isc keys by
 * signal.  A form outside the four orders after them.
 *
 * @param a First key.
 * @param b Second key.
 * @return True when @p a orders before @p b.
 */
bool operator<(const RxCbKey& a, const RxCbKey& b);

// =======================================================================
// One parameter, without its station
// =======================================================================

/**
 * @brief A column of the single-receiver state that is no network parameter: a fixed-core
 *        column the user side adds by configuration flag.
 *
 * The observation equation (measurement/ObservationEquation.hpp) is shared by the
 * single-receiver filter and the network producer.  The receiver phase clock and the
 * troposphere gradients are terms of the equation that only a user configuration switches on;
 * they are named here so that the equation names every column it can write.  Every network
 * term has @c None.  The columns of the Doppler rows (velocity, clock rate, the IMU states)
 * are not terms of the shared equation.
 */
enum class UserCore : std::uint8_t {
    None = 0,       ///< A network parameter (the default).
    PhaseClock,     ///< The receiver phase-clock offset: +1 on every phase row.
    GradientNorth,  ///< The troposphere north gradient: mapping function times cos(azimuth).
    GradientEast,   ///< The troposphere east gradient: mapping function times sin(azimuth).
};

/**
 * @brief The identity of one parameter, without its station: what the observation equation
 *        names, and what a view of the parameters answers for.
 *
 * Which fields a reference carries is fixed by its (kind, form), as for @ref ParamClass,
 * and every other field keeps its default:
 *
 * | term                         | kind           | form | fields used                      |
 * |------------------------------|----------------|------|----------------------------------|
 * | receiver position component  | rx_position    | none | axis                             |
 * | zenith wet delay             | zwd            | none |                                  |
 * | receiver clock               | rx_clock       | none |                                  |
 * | inter-system bias            | rx_clock       | isb  | sys                              |
 * | receiver phase clock         | rx_clock       | none | core                             |
 * | troposphere gradient         | zwd            | none | core                             |
 * | slant ionosphere             | slant_iono     | none | sat                              |
 * | receiver code bias           | rx_code_bias   | key  | the fields of @ref RxCbKey       |
 * | satellite code bias          | sat_code_bias  | none | sat, signal                      |
 * | satellite code bias, IF pair | sat_code_bias  | if   | sat, signal, signal_b            |
 * | satellite phase bias         | sat_phase_bias | none | sat, signal                      |
 * | receiver phase bias          | rx_phase_bias  | none | sys, signal                      |
 * | satellite clock              | sat_clock      | none | sat                              |
 * | ambiguity                    | ambiguity      | none | sat, signal                      |
 * | ambiguity, IF pair           | ambiguity      | if   | sat, signal, signal_b            |
 * | narrow-lane ambiguity        | ambiguity      | nl   | sat, band (the reference band)   |
 * | wide-lane ambiguity          | ambiguity      | wl   | sat, band (reference), band_b    |
 * | phase colour                 | phase_colour   | none | sat                              |
 *
 * The receiver code-bias row repeats @ref RxCbKey; @ref rxCodeBiasRef and @ref rxCbKeyOf
 * convert.  A user-only column has a non-default @ref core and the kind that names its place
 * in the row (the phase clock sits with the receiver clock, a gradient with the
 * troposphere).  The factory functions below build each row of the table.
 */
struct ParamRef {
    NetworkParamKind kind{NetworkParamKind::RxClock};  ///< Registry kind.
    ParamForm        form{ParamForm::None};            ///< Form within the kind.
    System           sys{System::Unknown};             ///< Constellation (inter-system bias, receiver phase and code bias).
    SatId            sat{};                            ///< The satellite (satellite-keyed kinds).
    SignalId         signal{};                         ///< Signal (the first member of an ionosphere-free pair).
    SignalId         signal_b{};                       ///< Second member of an ionosphere-free pair.
    FrequencyBand    band{FrequencyBand::Unknown};     ///< Band (receiver IFB, slot IFB; the reference band of a narrow or wide lane).
    FrequencyBand    band_b{FrequencyBand::Unknown};   ///< Second band of a wide lane.
    int              glo_slot{0};                      ///< GLONASS FDMA slot (form GloSlotIfb).
    std::uint8_t     axis{0};                          ///< Component of the receiver position (0, 1, 2 for x, y, z).
    UserCore         core{UserCore::None};             ///< The user-only column, or None.
};

/**
 * @brief Equality of two parameter references (all fields).
 * @param a First reference.
 * @param b Second reference.
 * @return True when every field is equal.
 */
bool operator==(const ParamRef& a, const ParamRef& b);

/**
 * @brief Inequality of two parameter references.
 * @param a First reference.
 * @param b Second reference.
 * @return @c !(a == b).
 */
inline bool operator!=(const ParamRef& a, const ParamRef& b) { return !(a == b); }

/**
 * @brief A short text of a reference for logs and test messages ("ambiguity.if G05 L1C L2W").
 * @param r The reference.
 * @return Kind, form, then the fields the kind carries.
 */
std::string describe(const ParamRef& r);

/**
 * @brief One component of the receiver position.
 * @param axis 0, 1 or 2 (x, y, z).
 * @return The reference (kind rx_position).
 */
ParamRef rxPositionRef(std::uint8_t axis);
/// @return The zenith wet delay of the receiver.
ParamRef zwdRef();
/// @return The receiver clock.
ParamRef rxClockRef();
/**
 * @brief The inter-system bias of a constellation against the time reference.
 * @param sys The constellation.
 * @return The reference (kind rx_clock, form isb).
 */
ParamRef isbRef(System sys);
/// @return The receiver phase-clock offset (user-only).
ParamRef phaseClockRef();
/**
 * @brief A troposphere gradient (user-only).
 * @param north True for the north gradient, false for the east gradient.
 * @return The reference.
 */
ParamRef gradientRef(bool north);
/**
 * @brief The slant ionosphere of a satellite.
 * @param sat The satellite.
 * @return The reference (kind slant_iono).
 */
ParamRef slantIonoRef(SatId sat);
/**
 * @brief A receiver code-bias state.
 * @param key The typed key (differential, inter-frequency, per-slot or inter-signal bias).
 * @return The reference (kind rx_code_bias; the key's form and fields).
 */
ParamRef rxCodeBiasRef(const RxCbKey& key);
/**
 * @brief The receiver code-bias key of a reference.
 * @param r A reference of kind rx_code_bias.
 * @return The key (fields the form does not use keep their defaults).
 */
RxCbKey rxCbKeyOf(const ParamRef& r);
/**
 * @brief The code bias of one satellite signal.
 * @param sat  The satellite.
 * @param code The code signal.
 * @return The reference (kind sat_code_bias).
 */
ParamRef satCodeBiasRef(SatId sat, SignalId code);
/**
 * @brief The code bias of one satellite on an ionosphere-free pair of codes.
 * @param sat  The satellite.
 * @param pair The pair.
 * @return The reference (kind sat_code_bias, form if).
 */
ParamRef satCodeBiasIfRef(SatId sat, const SignalPair& pair);
/**
 * @brief The phase bias of one satellite signal.
 * @param sat   The satellite.
 * @param phase The phase signal.
 * @return The reference (kind sat_phase_bias).
 */
ParamRef satPhaseBiasRef(SatId sat, SignalId phase);
/**
 * @brief The receiver phase bias of one constellation signal.
 * @param sys   The constellation.
 * @param phase The phase signal.
 * @return The reference (kind rx_phase_bias).
 */
ParamRef rxPhaseBiasRef(System sys, SignalId phase);
/**
 * @brief The clock of one satellite.
 * @param sat The satellite.
 * @return The reference (kind sat_clock).
 */
ParamRef satClockRef(SatId sat);
/**
 * @brief The float ambiguity of one satellite signal.
 * @param sat   The satellite.
 * @param phase The phase signal.
 * @return The reference (kind ambiguity).
 */
ParamRef ambiguityRef(SatId sat, SignalId phase);
/**
 * @brief The float ambiguity of an ionosphere-free pair of phase signals.
 * @param sat  The satellite.
 * @param pair The pair.
 * @return The reference (kind ambiguity, form if).
 */
ParamRef ambiguityIfRef(SatId sat, const SignalPair& pair);
/**
 * @brief The narrow-lane ambiguity of a satellite on its reference band.
 * @param sat      The satellite.
 * @param ref_band The reference band.
 * @return The reference (kind ambiguity, form nl).
 */
ParamRef narrowLaneRef(SatId sat, FrequencyBand ref_band);
/**
 * @brief The wide-lane ambiguity of a satellite between its reference band and another.
 * @param sat      The satellite.
 * @param ref_band The reference band.
 * @param band     The other band.
 * @return The reference (kind ambiguity, form wl).
 */
ParamRef wideLaneRef(SatId sat, FrequencyBand ref_band, FrequencyBand band);
/**
 * @brief The phase colour of a satellite.
 * @param sat The satellite.
 * @return The reference (kind phase_colour).
 */
ParamRef phaseColourRef(SatId sat);

} // namespace GnssProcessor
