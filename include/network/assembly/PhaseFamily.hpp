/**
 * @file PhaseFamily.hpp
 * @brief The roster's phase-family table: member phase signals that share a family's identity signal (card NRF section
 *        109, point 1, with the managing session's amendment 1, section 109.1).
 *
 * On one constellation's band, a member phase signal (Galileo L1X, the combined E1 tracking) shares its family's identity
 * signal (L1C) in everything the network estimates or emits per signal for the carrier phase: the satellite phase bias,
 * the receiver phase bias, the chain's group, the ring fix's family and the stream's phase-bias records.  The member
 * keeps its own codes and code biases, its own ambiguities (a receiver tracks one member of a band) and its own slip
 * checks, which pair its phase with its own code.  The table is declared by network.phase_families, typed once at run
 * start by the roster (@c signalRosterPhaseFamilies) and empty by default: every signal is then its own family and the
 * run is byte-identical.
 */
#pragma once

#include "common/GnssIdentity.hpp"   // SignalId, System
#include "common/SignalBand.hpp"     // ObsType

#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

/// One row of the phase-family table: a member phase signal and its family's identity signal, on one band.
struct PhaseFamilyRow {
    SignalId member;     ///< The member phase signal (Galileo L1X).
    SignalId identity;   ///< The family's identity signal on the same constellation and band (Galileo L1C).
};

/// The phase-family table of a run; empty (the default): every phase signal is its own family.
using PhaseFamilyTable = std::vector<PhaseFamilyRow>;

/**
 * @brief A phase signal's family identity.
 * @param t The table.
 * @param s A phase signal.
 * @return The identity of @p s's row, or @p s itself when no row names it.
 */
inline SignalId phaseFamilyOf(const PhaseFamilyTable& t, const SignalId& s) {
    for (const PhaseFamilyRow& r : t)
        if (r.member == s) return r.identity;
    return s;
}

/**
 * @brief A phase code's family identity code, for the maps still keyed by text ("L1X" on Galileo gives "L1C").
 * @param t           The table.
 * @param sys         The code's constellation.
 * @param phase_code  A phase observation code.
 * @return The identity's phase code; @p phase_code itself when the table is empty, the code does not parse or no row
 *         names it.
 */
inline std::string phaseFamilyCodeOf(const PhaseFamilyTable& t, System sys, const std::string& phase_code) {
    if (t.empty()) return phase_code;
    const std::optional<SignalId> s = SignalId::fromObsCode(sys, phase_code);
    if (!s) return phase_code;
    const SignalId f = phaseFamilyOf(t, *s);
    return f == *s ? phase_code : f.obsCode(ObsType::Phase);
}

} // namespace GnssProcessor
