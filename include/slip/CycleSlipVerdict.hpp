#pragma once

/**
 * @file CycleSlipVerdict.hpp
 * @brief The adjudication of one satellite's MW mandates in one epoch
 *        (section S11 of @c CycleSlipManager::detectAndRepair), as a plain
 *        value.
 *
 * @c CycleSlipManager::adjudicateSatellite reads the epoch's evidence (the MW
 * pairs, the TurboEdit resolution, the soft-LLI bridge, the receiver's loss of
 * lock) and returns a @c SatelliteSlipVerdict.  It writes no filter state; every
 * state change of S11 belongs to the one action the verdict selects
 * (@c CycleSlipManager::handleSatelliteMandates).
 *
 * Internal to the slip layer: included by the CycleSlip*.cpp section files.
 */

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

/**
 * @brief What S11 does with one satellite's mandates.
 */
enum class SatelliteSlipAction {
    SkipBridgeRepaired,   ///< The soft-LLI bridge already integer-repaired the satellite this epoch (S6); its mandate is not acted on.
    SkipWlSettlePending,  ///< A TurboEdit WL-settle deferral is pending: the mandate is suppressed, the holds keep the phase out of the filter, and the GF arcs are left untouched.
    RepairCulprits,       ///< Culprit signals identified: each is repaired (known integer) or reset, pivot-affected ones deferred to S12 / S13; mandates the analysis did not adjudicate take a residual reset.
    SuppressMandates,     ///< TurboEdit adjudicated the flagged signals as needing no state action (code event, bridged moot, adjudicated clean); mandates outside that adjudication are reset.
    FullReset             ///< Every mandated key is reset, followed by the LLI arc handling and the satellite-wide re-inflation.
};

/**
 * @brief Where a verdict's culprit signals came from.
 */
enum class CulpritSource {
    None,              ///< No culprit analysis succeeded.
    CrossConsistency,  ///< Multi-frequency MW cross-consistency (two or more pairs, one of them clean).
    TurboEdit          ///< The dual-frequency TurboEdit MW+GF joint solve.
};

/**
 * @brief One satellite's adjudicated MW mandates for one epoch.
 *
 * Built by @c CycleSlipManager::adjudicateSatellite; the fields an action does
 * not use stay at their defaults.  A skip verdict carries its action only.
 */
struct SatelliteSlipVerdict {
    /// The action selected.
    SatelliteSlipAction action{ SatelliteSlipAction::FullReset };
    /// The receiver declared a loss of lock this epoch.  LLI is authoritative:
    /// no culprit repair and no TurboEdit suppression apply to the satellite.
    bool lli{ false };
    /// Origin of @c culprit_signals.
    CulpritSource source{ CulpritSource::None };
    /// Signals identified as slipped (@c RepairCulprits).
    std::set<std::string> culprit_signals;
    /// Culprit signal to its integer slip in its own cycles, where the slipped
    /// pairs agree on it; a culprit absent here is reset, not repaired.
    std::map<std::string, int> known_integers;
    /// Signals the culprit analysis spoke for: the culprits plus every signal
    /// of the analyzed pairs (cross-consistency) or the confidently clean
    /// partners (TurboEdit).  A mandate on any other signal takes a residual
    /// reset.
    std::set<std::string> adjudicated_signals;
    /// Signals TurboEdit adjudicated as needing no state action
    /// (@c SuppressMandates).
    std::set<std::string> suppressed_signals;
    /// The contradiction guard found a culprit that TurboEdit adjudicated as
    /// GF-still; both readings were withdrawn and the satellite takes the full
    /// reset.
    bool suppression_withdrawn{ false };
    /// Slipped MW pairs of the satellite (printed by the full-reset debug line).
    std::size_t n_slipped_pairs{ 0 };
    /// Slipped plus clean MW pairs of the satellite (the same line).
    std::size_t n_mature_pairs{ 0 };
};

} // namespace GnssProcessor
