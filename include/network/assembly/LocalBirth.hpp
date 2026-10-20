#pragma once
/**
 * @file LocalBirth.hpp
 * @brief The births, re-seeds and prunes of a network station's local parameters for one
 *        epoch, and the first-epoch seeds.
 *
 * ### What the station needs each epoch
 *
 * From the epoch's observations the station needs: a slant ionosphere for each satellite with a
 * code or phase signal, a phase colour for each satellite with a phase signal (when the phase
 * colour is armed), a float ambiguity for each phase signal, and a receiver code bias for each
 * code signal the receiver-bias rule names (measurement/ReceiverBias.hpp).  A parameter that is
 * needed and not held is born; one that is held and not needed is pruned, with no grace epoch;
 * an ambiguity marked for a re-seed is given a new value.  The shared satellite parameters
 * (clocks and phase and code biases) are admitted apart (network/assembly/SharedAdmission.hpp).
 *
 * ### The order
 *
 * The order is a dependency chain, because each seed is the observation less the terms that
 * are already known:
 *  1. the first-epoch inter-system biases, from the code of each constellation against the
 *     time reference;
 *  2. the slant ionospheres, from the dual-frequency code that the bias product corrects;
 *  3. the phase colours (value 0);
 *  4. the receiver clock, once, from the time reference's code, when it is still cold and the
 *     cold-clock seed is armed;
 *  5. the ambiguities, each born at the phase observation less every term known at that
 *     moment, then the prune of those not observed;
 *  6. the marks that keep an ambiguity out of the integer search (no applied phase bias);
 *  7. the receiver code biases (value 0), then, once, their first-epoch seed (or the warm
 *     resume's installed points).
 * A parameter is appended when born, so the order of the births is the order of the
 * columns, and the Schur kernel factorizes in that order.
 *
 * Plain free functions over the station's set.
 */

#include "common/PppObservation.hpp"
#include "network/assembly/StationRows.hpp"
#include "network/state/StationParams.hpp"

#include <vector>

namespace GnssProcessor {

/**
 * @brief Runs the station's births, re-seeds, prunes and first-epoch seeds for one epoch.
 *
 * Does nothing for an epoch with no observation.  Uses the network posture: uncombined
 * processing, one ambiguity per phase signal, no between-satellite differencing.  With
 * @ref StationParams::record_pruned the pruned ambiguities are recorded in the set.
 *
 * @param c            The station's references.
 * @param set          The station's set (written).
 * @param observations The station's modeled observations of the epoch.
 */
void networkBirthLocalStates(const StationRowContext& c, StationParams& set,
                             const std::vector<PppObservation>& observations);

} // namespace GnssProcessor
