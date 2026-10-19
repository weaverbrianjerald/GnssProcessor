#pragma once
/**
 * @file SharedAdmission.hpp
 * @brief Which shared satellite parameters a network station references this epoch: the
 *        satellite clocks, satellite phase biases and satellite code biases its rows write
 *        into, the gates that withhold some of them, and the notes of what was withheld.
 *
 * ### The shared parameters of a station
 *
 * The joint solve estimates one clock per satellite and one phase and code bias per satellite
 * and signal.  A station's rows carry a column for each of them that its observations touch,
 * and only for those: a column no row writes would be an unobserved direction that the
 * null-space gate rejects.  So the set of shared parameters a station holds is the set its
 * usable observations imply, less the ones a gate withholds, and it is recomputed every epoch.
 * @ref networkAdmitSharedStates is that computation.
 *
 * ### The gates
 *
 * A gate withholds a satellite phase bias or code bias from the station; the station's phase
 * rows are still built, and its float ambiguity (or its satellite clock, for a code bias)
 * absorbs the term.  Each withheld parameter is noted with the gate in
 * @ref StationShared::withheld (and the parameter inventory counts it as absorbed after the
 * split):
 *  - GLONASS on the code-only network scope: a shared per-signal phase or code bias is
 *    unphysical under FDMA, so GLONASS phase contributes to the clock alone and its code bias
 *    stays a receiver per-slot parameter;
 *  - the coverage allowlist: a signal tracked by too few stations gets no phase bias;
 *  - the legacy single-pivot datum: only the keys of the pivot station are shared;
 *  - the code-census co-gate: a signal whose code pair the census quarantined also gets no
 *    phase bias.
 * Two later, serial gates withhold a parameter the station had admitted (the anchored-union
 * coverage gate and the eq34 hygiene block); they note it through
 * @ref noteWithheldPhaseBias.
 *
 * Plain free functions over the station's set.
 */

#include "common/PppObservation.hpp"
#include "network/assembly/StationRows.hpp"
#include "network/state/StationParams.hpp"

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Admits the shared satellite parameters a station's observations imply, withholds the
 *        gated ones, and prunes the rest.
 *
 * Clears the station's list of withheld parameters first.  A parameter that is needed and not
 * held is appended (value 0: the solve treats a shared parameter as absolute and never writes
 * it back); one held and no longer needed is removed.  Each family is touched only when its
 * estimation switch is on.  The admission mirrors the row loop's per-observation gates (the
 * stored geometry, the elevation mask and the usability rule), so every admitted column is
 * written by at least one row of the epoch.
 *
 * @param c                The station's references.
 * @param set              The station's set (written).
 * @param observations     The station's modeled observations of the epoch.
 * @param allowed_phase_keys If non-empty, only these "<sat>|<signal>|PB" keys may have a phase
 *                         bias: the legacy datum's pivot set.  Empty admits every usable phase
 *                         signal.
 * @param cbadm_admitted_code_keys If non-null, a phase bias is admitted only when its companion
 *                         code key "<sat>|C<band><attribute>" is in this set.  Null: no
 *                         co-gate.
 */
void networkAdmitSharedStates(const StationRowContext& c, StationParams& set,
                              const std::vector<PppObservation>& observations,
                              const std::set<std::string>& allowed_phase_keys,
                              const std::set<std::string>* cbadm_admitted_code_keys);

/**
 * @brief Notes a satellite phase bias that a serial gate withheld from the station after it
 *        had been admitted.
 *
 * @param set    The station's set (written).
 * @param id     The withheld phase bias.
 * @param reason The gate.
 */
void noteWithheldPhaseBias(StationParams& set, const SatSignalId& id, AbsorbedReason reason);

} // namespace GnssProcessor
