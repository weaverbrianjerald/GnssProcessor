#pragma once
/**
 * @file AbsorbedSharedStates.hpp
 * @brief The record a station keeps of the shared satellite states a network gate
 *        withheld from it this epoch (
 *        docs/refound/NOM_network_object_model_2026-10-08.md section 14).
 *
 * The record lives on the station's @c MeasurementModel, because the first gates that
 * withhold a state sit in @c MeasurementModel::ensureNetworkSharedStates.  It is kept in
 * this small header, apart from the parameter inventory that classifies and counts it
 * (network/assembly/ParamInventory.hpp), so that the user-side measurement header includes
 * no network header.
 *
 * A note is the raw (container, key, reason) triple a gate hands in; nothing here reads
 * a key.  The inventory classifies each note with its one classifier when it records the
 * station's parameters after the split (@c classifyAbsorbed, @c recordStationParams).
 *
 * Plain value types and one inline function.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Where a column sits in a station's @c StateManager: one value per index map the
     *        network can populate, plus the fixed core.  The parameter classifier's input.
     *
     * This is not a second parameter kind.  The kind cannot be read from a key alone: keys
     * collide across containers ("G05" is the key of a slant-ionosphere state in
     * @c getIonosphereStates() and of a phase-colour state in @c getPhaseColourStates()),
     * and the fixed-core columns have no key at all.  The (container, key) pair is how the
     * state manager itself addresses a column, so the classifier takes both.  The values
     * are named after the @c StateManager accessors so they cannot be read as
     * @c NetworkParamKind values.
     */
    enum class StateContainer : std::uint8_t {
        PositionIndex = 0,        ///< @c getPositionIndex(): the three position columns (no key).
        ClockIndex,               ///< @c getClockIndex(): the receiver clock (no key).
        TropoIndex,               ///< @c getTropoIndex(): the zenith wet delay (no key).
        SystemTimeOffsetStates,   ///< @c getSystemTimeOffsetStates(): key = system name.
        IonosphereStates,         ///< @c getIonosphereStates(): key = satellite.
        PhaseColourStates,        ///< @c getPhaseColourStates(): key = satellite.
        Ambiguities,              ///< @c getAmbiguities(): key per @c StateKeys::makeAmbiguityKey (or the NL / WL forms).
        ReceiverPhaseBiasStates,  ///< @c getReceiverPhaseBiasStates(): key per @c StateKeys::makeReceiverPhaseBiasKey.
        CodeBiasStates,           ///< @c getCodeBiasStates(): key per the DCB / IFB / ISC / per-slot makers.
        SatClockStates,           ///< @c getSatClockStates(): key per @c StateKeys::makeSatClockKey.
        SatPhaseBiasStates,       ///< @c getSatPhaseBiasStates(): key per @c StateKeys::makeSatPhaseBiasKey.
        SatBiasStates,            ///< @c getSatBiasStates(): key per @c StateKeys::makeSatBiasKey.
        Unlisted,                 ///< Any other column (velocity, clock rate, gradients, ...); key = its @c getStateLabels() label.
    };

    /**
     * @brief Why a shared satellite state was withheld from a station: one value per gate.
     *
     * A withheld state is a parameter instance with disposition @c Absorbed: the station's
     * observation still builds its row, and another parameter of the station takes the
     * withheld term up.  The reason table @c absorbedReasonSpec
     * (network/assembly/ParamInventory.hpp) gives each value its rule, its code site and what
     * takes the state up.  The first four gates sit in
     * @c MeasurementModel::ensureNetworkSharedStates (PASS A, at allocation), the last two
     * prune states the station had allocated (serial, before PASS B).
     */
    enum class AbsorbedReason : std::uint8_t {
        GlonassCodeOnlyScope = 0,  ///< GLONASS on the network's code-only scope: no satellite phase or code bias under FDMA.
        CoverageGate,              ///< The signal is outside the coverage allowlist (network.phase_bias_min_station_fraction).
        Allowlist,                 ///< Legacy single-pivot datum: the key is not among the pivot station's phase-bias keys.
        CodeCensusCogate,          ///< The CB-ADM census quarantined the signal's companion code pair (network.cb_admission).
        AnchoredUnionCoverage,     ///< Fewer than network.phase_datum_min_stations stations hold the key this epoch.
        AnchorHygieneBlock,        ///< eq34 hygiene: every candidate host arc is younger than network.eq34_anchor_min_arc_ep.
    };

    /// Number of @ref AbsorbedReason values.
    inline constexpr std::size_t kAbsorbedReasons = 6;

    /**
     * @brief One shared satellite state a gate withheld from a station, as the gate saw
     *        it: where the state sits (or would sit), its key and the gate.
     */
    struct AbsorbedNote {
        StateContainer container{StateContainer::Unlisted};  ///< @c SatPhaseBiasStates or @c SatBiasStates.
        std::string    key;                                  ///< The key the state has, or would have had.
        AbsorbedReason reason{AbsorbedReason::GlonassCodeOnlyScope};  ///< The gate.
    };

    /**
     * @brief A station's withheld shared states for one epoch.  Kept on the station's
     *        @c MeasurementModel, written in network mode only, read by the parameter
     *        inventory after the split.
     *
     * The gates see disjoint sets (each acts on the states the previous one left), so a
     * (container, key) appears at most once.
     */
    struct StationAbsorbed {
        std::vector<AbsorbedNote> notes;  ///< One per withheld state, in the order the gates noted them.
    };

    /**
     * @brief Note one withheld state: append it to a station's list, unread.
     *
     * The key is classified later, by the parameter inventory (@c classifyAbsorbed); a key
     * its classifier refuses is counted there as unclassified, never dropped.
     *
     * @param into      The station's list.
     * @param container @c SatPhaseBiasStates or @c SatBiasStates: where the state would sit.
     * @param key       The key the state has, or would have had
     *                  (@c StateKeys::makeSatPhaseBiasKey, @c StateKeys::makeSatBiasKey).
     * @param reason    The gate.
     */
    inline void noteAbsorbed(StationAbsorbed& into, StateContainer container, std::string_view key,
                             AbsorbedReason reason) {
        into.notes.push_back(AbsorbedNote{container, std::string(key), reason});
    }

} // namespace GnssProcessor
