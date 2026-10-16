/**
 * @file GnssIdentity.cpp
 * @brief The name map, the frequency functions and the parsers of the identity types
 *        (see GnssIdentity.hpp, which holds the constellation traits table and the inline
 *        letter reads), and the call-site helpers.
 */

#include "common/GnssIdentity.hpp"

#include "common/Constants.hpp"

#include <string>

namespace GnssProcessor {

    namespace {

        constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }

        constexpr bool isObsTypeChar(char c) { return c == 'C' || c == 'L' || c == 'D' || c == 'S'; }

        using FB = FrequencyBand;

    } // namespace

    System systemFromName(std::string_view text) {
        if (text.size() == 1) return systemFromLetter(text.front());
        for (System s : {System::GPS, System::Galileo, System::GLONASS, System::BeiDou,
                         System::QZSS, System::SBAS})
            if (text == traitsOf(s).name) return s;
        return System::Unknown;
    }

    std::string constellationKey(System sys) { return std::string(1, rinexLetter(sys)); }

    double carrierFrequencyHz(FrequencyBand band, int glo_slot) {
        namespace K = Constants;
        switch (band) {
        case FB::GpsL1:   return K::GPS_L1_FREQ;
        case FB::GpsL2:   return K::GPS_L2_FREQ;
        case FB::GpsL5:   return K::GPS_L5_FREQ;
        case FB::GalE1:   return K::GAL_E1_FREQ;
        case FB::GalE5a:  return K::GAL_E5A_FREQ;
        case FB::GalE5b:  return K::GAL_E5B_FREQ;
        case FB::GalE5:   return K::GAL_E5_FREQ;
        case FB::GalE6:   return K::GAL_E6_FREQ;
        case FB::GloG1:   return K::GLONASS_G1_BASE_FREQ + glo_slot * K::GLONASS_G1_CHANNEL_OFFSET;
        case FB::GloG2:   return K::GLONASS_G2_BASE_FREQ + glo_slot * K::GLONASS_G2_CHANNEL_OFFSET;
        case FB::GloG3:   return K::GLONASS_G3_BASE_FREQ;
        case FB::GloG1a:  return K::GLONASS_G1A_FREQ;
        case FB::GloG2a:  return K::GLONASS_G2A_FREQ;
        case FB::BdsB1I:  return K::BDS_B1_FREQ;
        case FB::BdsB1C:  return K::BDS_B1C_FREQ;
        case FB::BdsB2I:  return K::BDS_B2_FREQ;    // 1207.14 MHz, shared with B2b
        case FB::BdsB2a:  return K::BDS_B2A_FREQ;
        case FB::BdsB2b:  return K::BDS_B2_FREQ;
        case FB::BdsB2ab: return K::BDS_B2AB_FREQ;
        case FB::BdsB3:   return K::BDS_B3_FREQ;
        case FB::QzsL1:   return K::QZSS_L1_FREQ;
        case FB::QzsL2:   return K::QZSS_L2_FREQ;
        case FB::QzsL5:   return K::QZSS_L5_FREQ;
        case FB::QzsL6:   return K::QZSS_L6_FREQ;
        case FB::SbsL1:   return K::SBAS_L1_FREQ;
        case FB::SbsL5:   return K::SBAS_L5_FREQ;
        case FB::IrnL5:   return K::GPS_L5_FREQ;    // 1176.45 MHz
        case FB::IrnS:                              // 2492.028 MHz has no Constants entry
        case FB::Unknown:
        default:          return 0.0;
        }
    }

    double nominalCarrierFrequencyHz(FrequencyBand band) { return carrierFrequencyHz(band, 0); }

    double wavelengthM(FrequencyBand band, int glo_slot) {
        const double f = carrierFrequencyHz(band, glo_slot);
        return f > 0.0 ? Constants::SPEED_OF_LIGHT / f : 0.0;
    }

    double nominalWavelengthM(FrequencyBand band) { return wavelengthM(band, 0); }

    double nominalCarrierFrequencyOfDigitHz(System sys, char digit) {
        return nominalCarrierFrequencyHz(bandOfDigit(sys, digit));
    }

    double nominalWavelengthOfDigitM(System sys, char digit) {
        return nominalWavelengthM(bandOfDigit(sys, digit));
    }

    double ionoDelayMetresPerTecu(double carrier_hz) {
        // Term for term as the call sites wrote it, so the double is the same bit for bit.
        return 40.3 * 1.0e16 / (carrier_hz * carrier_hz);
    }

    // -----------------------------------------------------------------------
    // SatId
    // -----------------------------------------------------------------------

    std::optional<SatId> SatId::parse(std::string_view text) {
        if (text.size() < 2 || text.size() > 3) return std::nullopt;
        const System sys = systemFromLetter(text[0]);
        if (sys == System::Unknown) return std::nullopt;
        int prn = 0;
        if (text.size() == 2) {                       // "G5"
            if (!isDigit(text[1])) return std::nullopt;
            prn = text[1] - '0';
        } else if (text[1] == ' ') {                  // "G 5"
            if (!isDigit(text[2])) return std::nullopt;
            prn = text[2] - '0';
        } else {                                      // "G05"
            if (!isDigit(text[1]) || !isDigit(text[2])) return std::nullopt;
            prn = 10 * (text[1] - '0') + (text[2] - '0');
        }
        if (prn < 1) return std::nullopt;
        return SatId{sys, prn};
    }

    std::string SatId::str() const {
        std::string out(1, letter());
        if (prn >= 0 && prn <= 99) {
            out.push_back(static_cast<char>('0' + prn / 10));
            out.push_back(static_cast<char>('0' + prn % 10));
        } else {
            out += std::to_string(prn);
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // SignalId, ObservableId, SignalPair
    // -----------------------------------------------------------------------

    std::optional<SignalId> SignalId::fromObsCode(System sys, std::string_view code) {
        if (sys == System::Unknown || code.size() != 3) return std::nullopt;
        if (!isObsTypeChar(code[0]) || !isDigit(code[1])) return std::nullopt;
        if (code[2] < 'A' || code[2] > 'Z') return std::nullopt;
        return SignalId{sys, code[1], code[2]};
    }

    FrequencyBand SignalId::band() const {
        return classifySignal(traitsOf(sys).name, obsCode(ObsType::Code)).band;
    }

    double SignalId::wavelengthM(int glo_slot) const {
        return GnssProcessor::wavelengthM(band(), glo_slot);
    }

    std::string SignalId::obsCode(ObsType type) const {
        std::string out;
        out.reserve(3);
        out.push_back(static_cast<char>(type));
        out.push_back(digit);
        out.push_back(attribute);
        return out;
    }

    std::string SignalId::str() const {
        std::string out;
        out.push_back(digit);
        out.push_back(attribute);
        return out;
    }

    std::optional<ObservableId> ObservableId::parse(System sys, std::string_view code) {
        const auto sig = SignalId::fromObsCode(sys, code);
        if (!sig) return std::nullopt;
        return ObservableId{*sig, static_cast<ObsType>(code[0])};
    }

    std::optional<SignalPair> SignalPair::parse(System sys, std::string_view text) {
        if (text.size() != 7 || text[3] != '_' || text[0] != text[4]) return std::nullopt;
        const auto a = SignalId::fromObsCode(sys, text.substr(0, 3));
        const auto b = SignalId::fromObsCode(sys, text.substr(4, 3));
        if (!a || !b) return std::nullopt;
        return SignalPair{*a, *b};
    }

    std::string SignalPair::str(ObsType type) const {
        return a.obsCode(type) + "_" + b.obsCode(type);
    }

} // namespace GnssProcessor
