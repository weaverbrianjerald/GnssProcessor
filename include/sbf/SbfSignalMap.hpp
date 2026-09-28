#pragma once
/**
 * @file SbfSignalMap.hpp
 * @brief Septentrio SBF signal-number / SVID mapping to RINEX-3 + PRN.
 *
 * SBF identifies a signal by a "signal number" (Type field, bits 0-4, with an
 * escape to >31) and a satellite by an "SVID" byte.  Both differ from the RTCM
 * signal-ID and satellite-mask conventions, so this is an SBF-specific map (the
 * analogue of @c RtcmSignalMap).  Numbering is transcribed from the Septentrio
 * SBF Reference Guide (mosaic-X5 v4.8.0, section 4.1.10) and cross-checked
 * against pysbf2; the .cpp holds the per-number table.
 *
 * The returned suffix is the RINEX-3 frequency-band and tracking-mode code
 * (e.g. "1C", "5Q"); callers prepend 'C', 'L', 'D' or 'S'.  An empty suffix means
 * the signal has no RINEX mapping (reserved, L-band, NavIC) and is skipped.
 */

#include "common/GnssTypes.hpp"
#include <string>

namespace GnssProcessor::SbfSignalMap {

/**
 * @brief Static properties of one SBF signal number.
 */
struct SignalInfo {
    System      system{System::Unknown};   ///< Constellation the signal belongs to.
    std::string suffix;                     ///< RINEX-3 obs-code suffix ("1C", …); empty if unmapped.
    double      freq_hz{0.0};               ///< Carrier frequency (Hz); 0 for GLONASS FDMA (needs FreqNr).
    int         glonass_fdma_band{0};       ///< 0 = fixed freq; 1 = GLONASS L1 FDMA; 2 = GLONASS L2 FDMA.
};

/// Look up the static signal info for an SBF signal number (0..39).
SignalInfo signalInfo(int signal_number);

/// GLONASS FDMA carrier frequency [Hz] for band (1=L1, 2=L2) and channel k (−7..+13).
double glonassFdmaFreqHz(int band, int k);

/**
 * @brief Map an SBF SVID to a RINEX PRN string ("G01", "E11", "C08", …).
 *
 * Uses the SBF Reference Guide section 4.1.9 SVID ranges.  The produced ID
 * format matches the RTCM path's satellite IDs, so downstream state keys and
 * product lookups are identical regardless of decoder.
 *
 * @param svid       SBF SVID byte (0 = Do-Not-Use).
 * @param system_out Set to the constellation (or System::Unknown).
 * @return PRN string, or empty if the SVID is unmapped/out-of-range.
 */
std::string svidToPrn(int svid, System& system_out);

} // namespace GnssProcessor::SbfSignalMap
