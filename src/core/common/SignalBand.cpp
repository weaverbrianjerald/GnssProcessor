/**
 * @file SignalBand.cpp
 * @brief Implementation of the cross-receiver RINEX-3 signal classifier.
 *
 * The classification logic follows RINEX 3.05 section 5.1 (observation
 * codes by system).  It is intentionally exhaustive on the (system,
 * frequency digit) pair so that any new tracking attribute reported by
 * a future receiver is classified correctly without code changes — the
 * attribute character is preserved as-is in the result and only used
 * when a canonical code must be picked.
 */

#include "common/SignalBand.hpp"

#include "common/Constants.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace GnssProcessor {

    namespace {

        /// First-char → ObsType lookup; unrecognised first chars return @c Unknown.
        ObsType firstCharToObsType(char c) {
            switch (c) {
            case 'C': return ObsType::Code;
            case 'L': return ObsType::Phase;
            case 'D': return ObsType::Doppler;
            case 'S': return ObsType::Snr;
            default:  return ObsType::Unknown;
            }
        }

        /**
         * @brief Map (constellation, frequency digit) to a @c FrequencyBand.
         *
         * The frequency digit is the 2nd character of the RINEX-3 code
         * (e.g. '2' in "C2W"), which uniquely identifies the band within
         * a constellation.  Some constellations (BeiDou, Galileo) overload
         * the same digit across bands using the tracking attribute — those
         * are disambiguated in @c classifyByDigitWithAttr().
         */
        FrequencyBand bandFromDigitGps(char d) {
            switch (d) {
            case '1': return FrequencyBand::GpsL1;
            case '2': return FrequencyBand::GpsL2;
            case '5': return FrequencyBand::GpsL5;
            default:  return FrequencyBand::Unknown;
            }
        }

        FrequencyBand bandFromDigitGalileo(char d) {
            switch (d) {
            case '1': return FrequencyBand::GalE1;
            case '5': return FrequencyBand::GalE5a;
            case '7': return FrequencyBand::GalE5b;
            case '8': return FrequencyBand::GalE5;
            case '6': return FrequencyBand::GalE6;
            default:  return FrequencyBand::Unknown;
            }
        }

        FrequencyBand bandFromDigitGlonass(char d) {
            switch (d) {
            case '1': return FrequencyBand::GloG1;
            case '2': return FrequencyBand::GloG2;
            case '3': return FrequencyBand::GloG3;
            case '4': return FrequencyBand::GloG1a;
            case '6': return FrequencyBand::GloG2a;
            default:  return FrequencyBand::Unknown;
            }
        }

        /**
         * @brief BeiDou disambiguation by attribute.
         *
         * Digit 1: 'I'/'Q'/'X' = B1I (legacy), 'D'/'P'/'X' = B1C (BDS-3).
         *          'X' is shared — interpret as B1C when at least one of
         *          the data/pilot components ('D' or 'P') is present in
         *          the same observation set.  Without that context here
         *          we default 'X' on digit 1 to B1C (modernized signal).
         * Digit 7: 'I'/'Q'/'X' = B2I, 'D'/'P'/'Z' = B2b.
         */
        FrequencyBand bandFromDigitBeiDou(char d, char attr) {
            switch (d) {
            case '1':
                if (attr == 'I' || attr == 'Q')           return FrequencyBand::BdsB1I;
                if (attr == 'D' || attr == 'P')           return FrequencyBand::BdsB1C;
                if (attr == 'X')                          return FrequencyBand::BdsB1C; // see note above
                return FrequencyBand::Unknown;
            case '2':
                if (attr == 'I' || attr == 'Q' || attr == 'X') return FrequencyBand::BdsB1I;
                return FrequencyBand::Unknown;
            case '5':
                if (attr == 'D' || attr == 'P' || attr == 'X') return FrequencyBand::BdsB2a;
                return FrequencyBand::Unknown;
            case '7':
                if (attr == 'I' || attr == 'Q')           return FrequencyBand::BdsB2I;
                if (attr == 'D' || attr == 'P' || attr == 'Z') return FrequencyBand::BdsB2b;
                if (attr == 'X')                          return FrequencyBand::BdsB2I; // legacy default
                return FrequencyBand::Unknown;
            case '8':
                if (attr == 'D' || attr == 'P' || attr == 'X') return FrequencyBand::BdsB2ab;
                return FrequencyBand::Unknown;
            case '6':
                if (attr == 'I' || attr == 'Q' || attr == 'X') return FrequencyBand::BdsB3;
                return FrequencyBand::Unknown;
            default:
                return FrequencyBand::Unknown;
            }
        }

        FrequencyBand bandFromDigitQzss(char d) {
            switch (d) {
            case '1': return FrequencyBand::QzsL1;
            case '2': return FrequencyBand::QzsL2;
            case '5': return FrequencyBand::QzsL5;
            case '6': return FrequencyBand::QzsL6;
            default:  return FrequencyBand::Unknown;
            }
        }

        FrequencyBand bandFromDigitSbas(char d) {
            switch (d) {
            case '1': return FrequencyBand::SbsL1;
            case '5': return FrequencyBand::SbsL5;
            default:  return FrequencyBand::Unknown;
            }
        }

        FrequencyBand bandFromDigitIrnss(char d) {
            switch (d) {
            case '5': return FrequencyBand::IrnL5;
            case '9': return FrequencyBand::IrnS;
            default:  return FrequencyBand::Unknown;
            }
        }

        /**
         * @brief Resolve a constellation name to a normalized internal label.
         *
         * Accepts both the long form ("Galileo") and the leading-letter
         * abbreviations sometimes used internally ("E").  Anything else
         * returns an empty string and the caller should treat the code
         * as Unknown.
         */
        std::string normalizeConstellation(const std::string& s) {
            if (s == "GPS"     || s == "G") return "GPS";
            if (s == "Galileo" || s == "E") return "Galileo";
            if (s == "GLONASS" || s == "R") return "GLONASS";
            if (s == "BeiDou"  || s == "C") return "BeiDou";
            if (s == "QZSS"    || s == "J") return "QZSS";
            if (s == "SBAS"    || s == "S") return "SBAS";
            if (s == "IRNSS"   || s == "I") return "IRNSS";
            return {};
        }

        /// Sentinel for "attribute not in priority list" — sorts last.
        constexpr int kPriorityNoMatch = 999;

        /**
         * @brief Look up an attribute character in an ordered priority array.
         *
         * The first array element is the highest-priority attribute (returns 0).
         * Returns @c kPriorityNoMatch when not found.
         */
        template <std::size_t N>
        int lookupPriority(const std::array<char, N>& order, char attr) {
            for (std::size_t i = 0; i < N; ++i) {
                if (order[i] == attr) return static_cast<int>(i);
            }
            return kPriorityNoMatch;
        }

    } // namespace

    SignalClassification classifySignal(const std::string& constellation,
                                        const std::string& rinex_code) {
        SignalClassification out{};
        if (rinex_code.size() < 3) return out;

        const char first = rinex_code[0];
        const char digit = rinex_code[1];
        const char attr  = rinex_code[2];

        out.type      = firstCharToObsType(first);
        out.attribute = attr;
        if (out.type == ObsType::Unknown) return out;

        const std::string c = normalizeConstellation(constellation);
        if (c.empty()) return out;

        if      (c == "GPS")     out.band = bandFromDigitGps(digit);
        else if (c == "Galileo") out.band = bandFromDigitGalileo(digit);
        else if (c == "GLONASS") out.band = bandFromDigitGlonass(digit);
        else if (c == "BeiDou")  out.band = bandFromDigitBeiDou(digit, attr);
        else if (c == "QZSS")    out.band = bandFromDigitQzss(digit);
        else if (c == "SBAS")    out.band = bandFromDigitSbas(digit);
        else if (c == "IRNSS")   out.band = bandFromDigitIrnss(digit);

        return out;
    }

    SignalClassification classifySignalForSat(const std::string& satellite,
                                              const std::string& rinex_code) {
        if (satellite.empty()) return {};
        const char sys = satellite[0];
        std::string c;
        switch (sys) {
        case 'G': c = "GPS";     break;
        case 'E': c = "Galileo"; break;
        case 'R': c = "GLONASS"; break;
        case 'C': c = "BeiDou";  break;
        case 'J': c = "QZSS";    break;
        case 'S': c = "SBAS";    break;
        case 'I': c = "IRNSS";   break;
        default: return {};
        }
        return classifySignal(c, rinex_code);
    }

    int signalAttributePriority(const std::string& constellation,
                                FrequencyBand band,
                                char attribute) {
        // Priority order rationale per band.  The first attribute in
        // each array is the most preferred.  Order is loosely "best
        // tracking quality / least ambiguity for receiver-DCB".  A few
        // notes:
        //   - GPS L2: C2W (semi-codeless P-code) is the historical
        //     workhorse on geodetic receivers and is what most precise
        //     products are tied to; preferred over C2C (consumer-grade L2
        //     C/A) for canonical selection in mixed-receiver fleets.
        //   - Galileo E1: C1X (composite I+Q) preferred when present;
        //     otherwise C1C is the safest interoperable choice.
        switch (band) {
        case FrequencyBand::GpsL1: {
            constexpr std::array<char, 6> order = { 'W','P','C','S','L','X' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::GpsL2: {
            constexpr std::array<char, 6> order = { 'W','P','L','C','S','X' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::GpsL5: {
            constexpr std::array<char, 3> order = { 'X','I','Q' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::GalE1: {
            constexpr std::array<char, 5> order = { 'X','C','B','A','Z' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::GalE5a:
        case FrequencyBand::GalE5b:
        case FrequencyBand::GalE5: {
            constexpr std::array<char, 3> order = { 'X','I','Q' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::GalE6: {
            constexpr std::array<char, 5> order = { 'X','C','B','A','Z' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::GloG1:
        case FrequencyBand::GloG2: {
            constexpr std::array<char, 2> order = { 'P','C' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::GloG3:
        case FrequencyBand::GloG1a:
        case FrequencyBand::GloG2a: {
            constexpr std::array<char, 4> order = { 'X','I','Q','A' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::BdsB1I:
        case FrequencyBand::BdsB2I:
        case FrequencyBand::BdsB3: {
            constexpr std::array<char, 3> order = { 'X','I','Q' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::BdsB1C:
        case FrequencyBand::BdsB2a:
        case FrequencyBand::BdsB2ab: {
            constexpr std::array<char, 3> order = { 'X','D','P' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::BdsB2b: {
            constexpr std::array<char, 3> order = { 'X','D','P' };
            const int p = lookupPriority(order, attribute);
            if (p != kPriorityNoMatch) return p;
            // 'Z' is acceptable as last resort.
            return (attribute == 'Z') ? 3 : kPriorityNoMatch;
        }
        case FrequencyBand::QzsL1:
        case FrequencyBand::QzsL2:
        case FrequencyBand::QzsL5:
        case FrequencyBand::QzsL6: {
            constexpr std::array<char, 5> order = { 'X','C','L','S','Z' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::SbsL1:
        case FrequencyBand::SbsL5: {
            constexpr std::array<char, 3> order = { 'C','X','I' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::IrnL5:
        case FrequencyBand::IrnS: {
            constexpr std::array<char, 4> order = { 'X','A','B','C' };
            return lookupPriority(order, attribute);
        }
        case FrequencyBand::Unknown:
        default:
            return kPriorityNoMatch;
        }
    }

    const char* bandLabel(FrequencyBand band) {
        switch (band) {
        case FrequencyBand::GpsL1:    return "GPS_L1";
        case FrequencyBand::GpsL2:    return "GPS_L2";
        case FrequencyBand::GpsL5:    return "GPS_L5";
        case FrequencyBand::GalE1:    return "GAL_E1";
        case FrequencyBand::GalE5a:   return "GAL_E5a";
        case FrequencyBand::GalE5b:   return "GAL_E5b";
        case FrequencyBand::GalE5:    return "GAL_E5";
        case FrequencyBand::GalE6:    return "GAL_E6";
        case FrequencyBand::GloG1:    return "GLO_G1";
        case FrequencyBand::GloG2:    return "GLO_G2";
        case FrequencyBand::GloG3:    return "GLO_G3";
        case FrequencyBand::GloG1a:   return "GLO_G1a";
        case FrequencyBand::GloG2a:   return "GLO_G2a";
        case FrequencyBand::BdsB1I:   return "BDS_B1I";
        case FrequencyBand::BdsB1C:   return "BDS_B1C";
        case FrequencyBand::BdsB2I:   return "BDS_B2I";
        case FrequencyBand::BdsB2a:   return "BDS_B2a";
        case FrequencyBand::BdsB2b:   return "BDS_B2b";
        case FrequencyBand::BdsB2ab:  return "BDS_B2ab";
        case FrequencyBand::BdsB3:    return "BDS_B3";
        case FrequencyBand::QzsL1:    return "QZS_L1";
        case FrequencyBand::QzsL2:    return "QZS_L2";
        case FrequencyBand::QzsL5:    return "QZS_L5";
        case FrequencyBand::QzsL6:    return "QZS_L6";
        case FrequencyBand::SbsL1:    return "SBS_L1";
        case FrequencyBand::SbsL5:    return "SBS_L5";
        case FrequencyBand::IrnL5:    return "IRN_L5";
        case FrequencyBand::IrnS:     return "IRN_S";
        case FrequencyBand::Unknown:
        default:                      return "UNKNOWN";
        }
    }

    double bandNominalWavelengthMeters(FrequencyBand band) {
        namespace K = Constants;
        switch (band) {
        // GPS
        case FrequencyBand::GpsL1:   return K::GPS_L1_WAVELENGTH;
        case FrequencyBand::GpsL2:   return K::GPS_L2_WAVELENGTH;
        case FrequencyBand::GpsL5:   return K::GPS_L5_WAVELENGTH;
        // Galileo
        case FrequencyBand::GalE1:   return K::GAL_E1_WAVELENGTH;
        case FrequencyBand::GalE5a:  return K::GAL_E5A_WAVELENGTH;
        case FrequencyBand::GalE5b:  return K::GAL_E5B_WAVELENGTH;
        case FrequencyBand::GalE5:   return K::GAL_E5_WAVELENGTH;
        case FrequencyBand::GalE6:   return K::GAL_E6_WAVELENGTH;
        // GLONASS — FDMA bands use the base (k=0) wavelength (slot offset not
        // applied here; see the header note), the CDMA bands are fixed.
        case FrequencyBand::GloG1:   return K::SPEED_OF_LIGHT / K::GLONASS_G1_BASE_FREQ;
        case FrequencyBand::GloG2:   return K::SPEED_OF_LIGHT / K::GLONASS_G2_BASE_FREQ;
        case FrequencyBand::GloG3:   return K::GLONASS_G3_WAVELENGTH;
        case FrequencyBand::GloG1a:  return K::GLONASS_G1A_WAVELENGTH;
        case FrequencyBand::GloG2a:  return K::GLONASS_G2A_WAVELENGTH;
        // BeiDou
        case FrequencyBand::BdsB1I:  return K::BDS_B1_WAVELENGTH;
        case FrequencyBand::BdsB1C:  return K::BDS_B1C_WAVELENGTH;
        case FrequencyBand::BdsB2I:  return K::BDS_B2_WAVELENGTH;   // 1207.14 MHz (B2b freq)
        case FrequencyBand::BdsB2a:  return K::BDS_B2A_WAVELENGTH;
        case FrequencyBand::BdsB2b:  return K::BDS_B2_WAVELENGTH;
        case FrequencyBand::BdsB2ab: return K::BDS_B2AB_WAVELENGTH;
        case FrequencyBand::BdsB3:   return K::BDS_B3_WAVELENGTH;
        // QZSS
        case FrequencyBand::QzsL1:   return K::QZSS_L1_WAVELENGTH;
        case FrequencyBand::QzsL2:   return K::QZSS_L2_WAVELENGTH;
        case FrequencyBand::QzsL5:   return K::QZSS_L5_WAVELENGTH;
        case FrequencyBand::QzsL6:   return K::QZSS_L6_WAVELENGTH;
        // SBAS
        case FrequencyBand::SbsL1:   return K::SBAS_L1_WAVELENGTH;
        case FrequencyBand::SbsL5:   return K::SBAS_L5_WAVELENGTH;
        // IRNSS / NavIC — L5 shares the 1176.45 MHz band; the S band (2492 MHz)
        // has no Constants entry, so it falls through to the 0.0 sentinel.
        case FrequencyBand::IrnL5:   return K::GPS_L5_WAVELENGTH;
        case FrequencyBand::IrnS:
        case FrequencyBand::Unknown:
        default:                     return 0.0;
        }
    }

} // namespace GnssProcessor
