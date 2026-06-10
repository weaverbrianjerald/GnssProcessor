#pragma once
/**
 * @file RtcmSignalMap.hpp
 * @brief Mapping from RTCM 3 signal IDs to RINEX 3 observation code suffixes.
 *
 * Each MSM cell mask bit and each SSR signal-ID field encodes one of up
 * to 32 signals per GNSS system, defined in RTCM 3.x Annex 1 (Tables
 * 3.5-91 GPS, 3.5-93 GLONASS, 3.5-95 Galileo, 3.5-97 QZSS, 3.5-105 BeiDou).
 * The suffix returned here is the RINEX-3 frequency-band + tracking-mode
 * code (e.g. "1C", "5X"); callers prepend 'C', 'L', 'D', 'S' depending on
 * the observable type they're decoding.
 */

#include "common/GnssTypes.hpp"
#include <string>

namespace GnssProcessor::RtcmSignalMap {

/**
 * @brief Translate an RTCM signal ID to a RINEX-3 obs-code suffix.
 *
 * @param sys       GNSS constellation.
 * @param signal_id RTCM signal ID (0..31).  Many IDs are reserved/unassigned.
 * @return Two-character RINEX suffix (e.g. "1C") or an empty string if the
 *         ID is not assigned for @p sys.  Empty-string returns are
 *         expected for reserved IDs and should not log as errors.
 */
std::string toRinexSuffix(System sys, int signal_id);

/**
 * @brief Map a RINEX-3 obs suffix back to an RTCM signal ID for a given system.
 *
 * Used for round-tripping (e.g. when serialising decoded SSR biases for
 * comparison with config) and unit tests.  Returns -1 if no RTCM signal
 * ID is assigned to that suffix on @p sys.
 */
int fromRinexSuffix(System sys, const std::string& rinex_suffix);

} // namespace GnssProcessor::RtcmSignalMap
