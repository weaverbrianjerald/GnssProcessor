#pragma once
/**
 * @file RtcmSignalMap.hpp
 * @brief Mapping from RTCM 3 signal IDs to RINEX 3 observation code suffixes.
 *
 * RTCM 3 numbers the signals of each GNSS system twice, and the two numberings
 * differ:
 *   - **MSM** observation messages identify a signal by its bit position in the
 *     cell signal mask (GPS L1 C/A is ID 2, L2 Z-tracking ID 10).
 *   - **SSR** bias messages (legacy code bias 1059, 1065, 1242, 1248, 1254, 1260;
 *     phase bias 1265 to 1270; IGS-SSR 4076 code- and phase-bias subtypes) carry a
 *     5-bit signal and tracking mode identifier from a separate table (GPS L1 C/A is
 *     ID 0, L2 Z-tracking ID 11).  The tables here follow the cssrlib reference
 *     decoder (`rtcm.py`, `ssig2rsig`), which serves both SSR dialects.
 *
 * The two numberings agree for only a few IDs (Galileo E1 C is ID 2 in both), so a
 * bias decoded through the MSM table lands on a wrong signal or on none.
 *
 * The suffix returned is the RINEX-3 frequency-band + tracking-mode code (e.g.
 * "1C", "5X"); callers prepend 'C', 'L', 'D' or 'S' according to the observable
 * type being decoded.  Tables exist for GPS, GLONASS, Galileo, BeiDou, QZSS and SBAS.
 */

#include "common/GnssTypes.hpp"
#include <string>

namespace GnssProcessor::RtcmSignalMap {

/**
 * @brief Translate an MSM signal ID (cell signal-mask position) to a RINEX-3 suffix.
 *
 * For MSM observation messages only; SSR bias messages use @ref ssrToRinexSuffix.
 *
 * @param sys       GNSS constellation.
 * @param signal_id MSM signal ID (0..31).  Many IDs are reserved/unassigned.
 * @return Two-character RINEX suffix (e.g. "1C"), or an empty string when the
 *         ID is not assigned for @p sys, @p signal_id is outside 0..31, or
 *         @p sys has no table.  Empty returns are expected for reserved IDs
 *         and are not errors.
 */
std::string msmToRinexSuffix(System sys, int signal_id);

/**
 * @brief Map a RINEX-3 suffix back to its MSM signal ID for a given system.
 *
 * The inverse of @ref msmToRinexSuffix, used for round-tripping and in unit tests.
 *
 * @param sys          GNSS constellation.
 * @param rinex_suffix Two-character RINEX-3 suffix (e.g. "2W").
 * @return The MSM signal ID (0..31), or -1 when no ID is assigned to that
 *         suffix on @p sys, the suffix is not two characters, or @p sys has
 *         no table.
 */
int msmFromRinexSuffix(System sys, const std::string& rinex_suffix);

/**
 * @brief Translate an SSR bias signal ID to a RINEX-3 suffix.
 *
 * For the signal field of SSR code- and phase-bias messages, legacy and IGS-SSR
 * alike; MSM observation messages use @ref msmToRinexSuffix.
 *
 * @param sys       GNSS constellation.
 * @param signal_id SSR signal and tracking mode identifier (0..31).
 * @return Two-character RINEX suffix (e.g. "1C" for GPS ID 0, "2W" for GPS ID 11),
 *         or an empty string when the ID is not assigned for @p sys, @p signal_id is
 *         outside 0..31, or @p sys has no table.  A bias with an empty suffix is
 *         dropped by the decoder.
 */
std::string ssrToRinexSuffix(System sys, int signal_id);

/**
 * @brief Map a RINEX-3 suffix back to its SSR bias signal ID for a given system.
 *
 * The inverse of @ref ssrToRinexSuffix, used by tests that hand-build SSR messages.
 *
 * @param sys          GNSS constellation.
 * @param rinex_suffix Two-character RINEX-3 suffix (e.g. "5Q").
 * @return The SSR signal ID (0..31), or -1 when no ID is assigned to that suffix on
 *         @p sys, the suffix is not two characters, or @p sys has no table.
 */
int ssrFromRinexSuffix(System sys, const std::string& rinex_suffix);

} // namespace GnssProcessor::RtcmSignalMap
