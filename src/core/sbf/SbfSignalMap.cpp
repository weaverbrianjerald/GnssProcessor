/**
 * @file SbfSignalMap.cpp
 * @brief SBF signal-number and SVID tables.
 *
 * Signal numbering per Septentrio SBF Reference Guide (mosaic-X5 v4.8.0,
 * §4.1.10), verified against pysbf2.  Only the GPS, Galileo and BeiDou entries
 * (the active constellations) have to be exact for the current pipeline; QZSS,
 * GLONASS and SBAS are mapped for completeness and filtered out upstream by
 * active_systems.
 *
 * Not yet validated against a real capture: the RINEX tracking-mode letters of
 * a few signals depend on firmware and convention (e.g. GAL E5b "7Q" vs "7X",
 * BDS B2I "7I" vs "7Q").  The table uses the SBF-guide defaults; if a signal's
 * biases or ATX corrections look wrong, compare against the receiver's own
 * RINEX observation codes.
 */

#include "sbf/SbfSignalMap.hpp"
#include "common/Constants.hpp"
#include <cstdio>

namespace GnssProcessor::SbfSignalMap {

SignalInfo signalInfo(int n) {
    switch (n) {
    case 0:  return {System::GPS,     "1C", Constants::GPS_L1_FREQ,  0};  // GPS L1CA
    case 1:  return {System::GPS,     "1W", Constants::GPS_L1_FREQ,  0};  // GPS L1P(Y)
    case 2:  return {System::GPS,     "2W", Constants::GPS_L2_FREQ,  0};  // GPS L2P(Y)
    case 3:  return {System::GPS,     "2L", Constants::GPS_L2_FREQ,  0};  // GPS L2C
    case 4:  return {System::GPS,     "5Q", Constants::GPS_L5_FREQ,  0};  // GPS L5
    case 5:  return {System::GPS,     "1L", Constants::GPS_L1_FREQ,  0};  // GPS L1C
    case 6:  return {System::QZSS,    "1C", Constants::QZSS_L1_FREQ, 0};  // QZSS L1CA
    case 7:  return {System::QZSS,    "2L", Constants::QZSS_L2_FREQ, 0};  // QZSS L2C
    case 8:  return {System::GLONASS, "1C", 0.0, 1};                      // GLO L1CA (FDMA)
    case 9:  return {System::GLONASS, "1P", 0.0, 1};                      // GLO L1P  (FDMA)
    case 10: return {System::GLONASS, "2P", 0.0, 2};                      // GLO L2P  (FDMA)
    case 11: return {System::GLONASS, "2C", 0.0, 2};                      // GLO L2CA (FDMA)
    case 12: return {System::GLONASS, "3Q", Constants::GLONASS_G3_BASE_FREQ, 0}; // GLO L3 CDMA
    case 13: return {System::BeiDou,  "1P", Constants::BDS_B1C_FREQ, 0};  // BDS B1C
    case 14: return {System::BeiDou,  "5P", Constants::BDS_B2A_FREQ, 0};  // BDS B2a
    case 15: return {System::Unknown, "",   0.0, 0};                      // NavIC L5 (no System enum)
    case 16: return {System::Unknown, "",   0.0, 0};                      // reserved
    case 17: return {System::Galileo, "1C", Constants::GAL_E1_FREQ,  0};  // GAL E1 (E1-B/C)
    case 18: return {System::Unknown, "",   0.0, 0};                      // reserved
    case 19: return {System::Galileo, "6C", Constants::GAL_E6_FREQ,  0};  // GAL E6 (E6-B/C)
    case 20: return {System::Galileo, "5Q", Constants::GAL_E5A_FREQ, 0};  // GAL E5a
    case 21: return {System::Galileo, "7Q", Constants::GAL_E5B_FREQ, 0};  // GAL E5b
    case 22: return {System::Galileo, "8Q", Constants::GAL_E5_FREQ,  0};  // GAL E5 AltBOC
    case 23: return {System::Unknown, "",   0.0, 0};                      // L-Band / MSS
    case 24: return {System::SBAS,    "1C", Constants::SBAS_L1_FREQ, 0};  // SBAS L1CA
    case 25: return {System::SBAS,    "5I", Constants::SBAS_L5_FREQ, 0};  // SBAS L5
    case 26: return {System::QZSS,    "5Q", Constants::QZSS_L5_FREQ, 0};  // QZSS L5
    case 27: return {System::Unknown, "",   0.0, 0};                      // QZSS L6 (no std RINEX code)
    case 28: return {System::BeiDou,  "2I", Constants::BDS_B1_FREQ,  0};  // BDS B1I
    case 29: return {System::BeiDou,  "7I", Constants::BDS_B2_FREQ,  0};  // BDS B2I
    case 30: return {System::BeiDou,  "6I", Constants::BDS_B3_FREQ,  0};  // BDS B3I
    case 31: return {System::Unknown, "",   0.0, 0};                      // reserved / escape marker
    case 32: return {System::QZSS,    "1L", Constants::QZSS_L1_FREQ, 0};  // QZSS L1C
    case 33: return {System::QZSS,    "1Z", Constants::QZSS_L1_FREQ, 0};  // QZSS L1S
    case 34: return {System::BeiDou,  "7D", Constants::BDS_B2_FREQ,  0};  // BDS B2b
    case 35: return {System::Unknown, "",   0.0, 0};                      // reserved
    case 36: return {System::Unknown, "",   0.0, 0};                      // NavIC S
    case 38: return {System::QZSS,    "1E", Constants::QZSS_L1_FREQ, 0};  // QZSS L1C/B (newer FW)
    case 39: return {System::QZSS,    "5P", Constants::QZSS_L5_FREQ, 0};  // QZSS L5S   (newer FW)
    default: return {System::Unknown, "",   0.0, 0};
    }
}

double glonassFdmaFreqHz(int band, int k) {
    if (band == 1)
        return Constants::GLONASS_G1_BASE_FREQ + k * Constants::GLONASS_G1_CHANNEL_OFFSET;
    if (band == 2)
        return Constants::GLONASS_G2_BASE_FREQ + k * Constants::GLONASS_G2_CHANNEL_OFFSET;
    return 0.0;
}

std::string svidToPrn(int svid, System& system_out) {
    char buf[8];
    const auto fmt = [&](char c, int nn) {
        std::snprintf(buf, sizeof(buf), "%c%02d", c, nn);
        return std::string(buf);
    };
    if (svid >= 1   && svid <= 37 ) { system_out = System::GPS;     return fmt('G', svid);       }
    if (svid >= 38  && svid <= 61 ) { system_out = System::GLONASS; return fmt('R', svid - 37);  }
    if (svid >= 63  && svid <= 68 ) { system_out = System::GLONASS; return fmt('R', svid - 38);  }
    if (svid >= 71  && svid <= 106) { system_out = System::Galileo; return fmt('E', svid - 70);  }
    if (svid >= 120 && svid <= 140) { system_out = System::SBAS;    return fmt('S', svid - 100); }
    if (svid >= 141 && svid <= 180) { system_out = System::BeiDou;  return fmt('C', svid - 140); }
    if (svid >= 181 && svid <= 187) { system_out = System::QZSS;    return fmt('J', svid - 180); }
    if (svid >= 198 && svid <= 215) { system_out = System::SBAS;    return fmt('S', svid - 157); }
    if (svid >= 223 && svid <= 245) { system_out = System::BeiDou;  return fmt('C', svid - 182); }
    // 62 (GLO unknown slot), 107-119 (L-band), 191-197 / 216-222 (NavIC): no RINEX PRN.
    system_out = System::Unknown;
    return "";
}

} // namespace GnssProcessor::SbfSignalMap
