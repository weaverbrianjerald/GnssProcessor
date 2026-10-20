#pragma once
/**
 * @file RtcmSatId.hpp
 * @brief Satellite identifier strings for the RTCM decoders.
 *
 * RTCM numbers the satellites of most systems by their PRN or slot, so the
 * string is the system letter plus the two-digit number ("G05").  SBAS is the
 * exception on both sides: RTCM counts SBAS satellites from 1 at PRN 120
 * (identifier = PRN - 119), while RINEX, and every observation and navigation
 * record in this project, names a SBAS satellite by its PRN minus 100 ("S20"
 * for PRN 120).  Every RTCM decoder that produces SBAS satellite strings goes
 * through @ref RtcmSatId::sbas so that a decoded record matches the same
 * satellite read from RINEX or SBF.
 */

#include <cstdio>
#include <string>

namespace GnssProcessor::RtcmSatId {

/// PRN of the first SBAS satellite; RTCM identifier 1.
inline constexpr int kSbasFirstPrn = 120;

/// RINEX names a SBAS satellite by its PRN minus this value (PRN 120 is S20).
inline constexpr int kRinexSbasPrnShift = 100;

/**
 * @brief The RINEX-style string of a SBAS satellite given its RTCM identifier.
 *
 * @param rtcm_id RTCM SBAS satellite identifier (1 for PRN 120, 39 for PRN 158).
 * @return "S" and the two-digit number PRN - 100, so identifier 1 gives "S20".
 */
inline std::string sbas(int rtcm_id) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "S%02d",
                  rtcm_id + (kSbasFirstPrn - 1) - kRinexSbasPrnShift);
    return std::string(buf);
}

} // namespace GnssProcessor::RtcmSatId
