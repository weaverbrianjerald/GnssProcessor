#pragma once
/**
 * @file StateColumns.hpp
 * @brief The bridge from a station's state manager to typed parameter identities: its columns
 *        listed, each classified, and the withheld shared states classified.
 *
 * ### Why it exists
 *
 * A network station's columns are still created by the single-receiver measurement model, in
 * its @c StateManager, under string keys (filter/StateKeySchema.hpp).  Everything the network
 * reads about them goes through the station's parameter set (network/state/StationParams.hpp),
 * which names each column by a @ref ParamRef.  This file is the one place that recovers a typed
 * identity from a (container, key) pair: @ref listStateColumns walks the state manager,
 * @ref classifyStateColumn reads one key through the @c StateKeySchema parsers and the identity
 * types of common/GnssIdentity.hpp, and @ref classifyAbsorbed does the same for a shared state
 * a gate withheld.  The set's build (network/state/StationParamsShadow.hpp) is its one caller
 * on the way in; the receiver IFB rule and the receiver code-bias carry still classify a text
 * key through it.  It goes when the network allocates its own columns and so knows each
 * identity at birth.
 *
 * Plain value types and free functions.
 */

#include "common/ParamRef.hpp"                    // ParamInstanceId
#include "measurement/AbsorbedSharedStates.hpp"   // StateContainer, AbsorbedReason, AbsorbedNote

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GnssProcessor {

class StateManager;

// StateContainer (where a column sits in a station's StateManager, the classifier's input) is
// declared in measurement/AbsorbedSharedStates.hpp, beside the absorbed-state record that uses it,
// so that the user-side measurement header includes no network header.

/**
 * @brief The name of a container, used in the unclassified labels ("<container>:<key>").
 * @param c The container.
 * @return The accessor's name in snake case ("ionosphere_states", ...; "unlisted").
 */
const char* stateContainerName(StateContainer c);

/**
 * @brief One column of a station's state manager.
 */
struct StateColumn {
    std::size_t    index{0};                             ///< State index (the H column).
    StateContainer container{StateContainer::Unlisted};  ///< The container that holds it.
    std::string    key;                                  ///< Its key (empty for the fixed core).
};

/**
 * @brief List every column of a station's state manager.
 *
 * Walks the fixed core (position, receiver clock, wet delay) and every index map the
 * network can populate; each state index in [0, @c getStateSize()) appears exactly
 * once, an index in no known container as @c Unlisted with its
 * @c StateManager::getStateLabels label, so a family added to the state manager
 * later is listed (and counted unclassified) rather than missed.
 *
 * @param sm The station's state manager.
 * @return The columns in ascending state-index order.
 */
std::vector<StateColumn> listStateColumns(const StateManager& sm);

/**
 * @brief The one classifier: (container, key) to a parameter instance.
 *
 * Every key is read through its @c StateKeySchema parser and the identity types; the
 * instance's station is left unset.  The refusal policy: a key is refused when any
 * of its parts fails to parse, with one exception.  The receiver code-bias
 * forms that carry no signal (DCB, IFB, slot IFB) are decided by the key's shape, and
 * a system name that is not a known constellation gives @c System::Unknown rather
 * than a refusal, so that the IFB rule names exactly the columns the former substring
 * test on "_IFB" named (StateInitializer produces no such key).  An ISC's signal is a
 * part like any other: it needs a known system.
 *
 * @param container The container the key is held in.
 * @param key       The key (ignored for the fixed core).
 * @return The instance, or @c std::nullopt for a key it does not know: the container
 *         @c Unlisted, a double-difference ambiguity key (the baseline mode's, which
 *         no network station holds), or a malformed key.
 */
std::optional<ParamInstanceId> classifyStateColumn(StateContainer container, std::string_view key);

/**
 * @brief One shared satellite state a gate withheld from a station this epoch, classified.
 */
struct AbsorbedInstance {
    /// The instance: kind @c SatPhaseBias or @c SatCodeBias, its constellation, satellite
    /// and signal, as @ref classifyStateColumn reads the key the state would have had.
    /// The station is set by the inventory's record.
    ParamInstanceId id{};
    AbsorbedReason  reason{AbsorbedReason::GlonassCodeOnlyScope};  ///< The gate.
};

/**
 * @brief Classify one withheld state a gate noted (@c noteAbsorbed): its key read by the
 *        one classifier, @ref classifyStateColumn.
 *
 * @param note The gate's note (container, key, reason).
 * @return The instance with the note's reason, or @c std::nullopt for a key the
 *         classifier refuses (the caller counts it unclassified under
 *         @ref absorbedNoteLabel, never drops it).
 */
std::optional<AbsorbedInstance> classifyAbsorbed(const AbsorbedNote& note);

/**
 * @brief The label of a note in the unclassified list: "<container>:<key>".
 * @param note The gate's note.
 * @return @ref stateContainerName of the container, a colon and the key.
 */
std::string absorbedNoteLabel(const AbsorbedNote& note);

/**
 * @brief Does the station still hold the column that takes a withheld state up?
 *
 * The test that the instance's observation reached the split: for a satellite phase bias
 * the station's float ambiguity of the same satellite and signal
 * (@c StateKeys::makeAmbiguityKey, the per-band key the network uses), for a satellite
 * code bias the satellite's clock column (the code row's shared column).
 *
 * @param sm The station's state manager after its measurement pre-pass.
 * @param a  The withheld state.
 * @return True when the column is in @p sm.
 */
bool absorberPresent(const StateManager& sm, const AbsorbedInstance& a);

} // namespace GnssProcessor
