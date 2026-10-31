#pragma once

/**
 * @file KalmanRowIdentity.hpp
 * @brief @internal The two row-identity helpers the KalmanFilter files share: the BSSD
 *        pair split and the band wavelength of a phase row.
 *
 * A measurement row of the user-side filter is identified by two parallel strings of
 * @c EpochMeasurement: the satellite id ("G01", or the BSSD single-difference pair
 * "G01-G05", non-pivot first) and the signal ("L1C", or an ionosphere-free pair
 * "L1C_L2W").  The KalmanFilter files parse them with these two functions only.
 * The driver, the post-fit QC and the outlier responses share this one copy.  The typed
 * identities of common/GnssIdentity.hpp are to replace them: the band table below
 * disagrees with PppUtils and GnssIdentity on QZSS L6, SBAS band 2, the GLONASS slots and
 * the BeiDou RINEX 3.02 B1I spelling.
 *
 * Included only by src/core/filter/Kalman*.cpp.
 */

#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"   // SatPairId
#include <string>
#include <utility>

namespace GnssProcessor {
namespace kalman {

    /**
     * @brief Carrier wavelength of a (satellite, band) pair, for the cycle floors.
     *
     * Pivot-slip detection, the Huber pass, the persistent-bias inflation and the
     * per-row phase outlier dispatch use it for the wavelength-fraction floor below
     * which a residual cannot be an integer cycle slip.  The constellation is read
     * with @c systemOfSat (common/GnssIdentity.hpp) and the frequency from the band's
     * digit; GLONASS uses the FDMA centre frequency, which suffices for a fractional
     * floor.  The digit table stays here: it differs from
     * @c carrierFrequencyHz(bandOfDigit(...)) on QZSS L6 (0.0 here) and SBAS digit 2
     * (GPS L2 here), so moving it changes what those two bands compute and is a separate
     * change from this reading of the constellation.
     *
     * @param sat       Satellite ID (e.g. "G11"); constellation from its letter.
     * @param band_str  Band prefix (e.g. "L1"); only the frequency digit is used.
     * @return          Wavelength in meters, or 0.0 if the pair is unknown.
     */
    inline double bandWavelengthForSat(const std::string& sat,
                                        const std::string& band_str) {
        if (sat.empty() || band_str.size() < 2) return 0.0;
        const char d      = band_str[1];
        double freq = 0.0;
        switch (systemOfSat(sat)) {
        case System::GPS: case System::QZSS: case System::SBAS:  // share band-digit mapping
            if      (d == '1') freq = Constants::GPS_L1_FREQ;
            else if (d == '2') freq = Constants::GPS_L2_FREQ;
            else if (d == '5') freq = Constants::GPS_L5_FREQ;
            break;
        case System::Galileo:
            if      (d == '1') freq = Constants::GAL_E1_FREQ;
            else if (d == '5') freq = Constants::GAL_E5A_FREQ;
            else if (d == '7') freq = Constants::GAL_E5B_FREQ;
            else if (d == '6') freq = Constants::GAL_E6_FREQ;
            else if (d == '8') freq = Constants::GAL_E5_FREQ;
            break;
        case System::BeiDou:
            if      (d == '1') freq = Constants::BDS_B1C_FREQ;
            else if (d == '2') freq = Constants::BDS_B1_FREQ;   // B1I (legacy band-2 RINEX code)
            else if (d == '5') freq = Constants::BDS_B2A_FREQ;
            else if (d == '7') freq = Constants::BDS_B2_FREQ;   // B2b
            else if (d == '6') freq = Constants::BDS_B3_FREQ;
            else if (d == '8') freq = Constants::BDS_B2AB_FREQ;
            break;
        case System::GLONASS:  // the FDMA center frequency suffices for the λ/4 floor
            if      (d == '1') freq = Constants::GLONASS_G1_BASE_FREQ;
            else if (d == '2') freq = Constants::GLONASS_G2_BASE_FREQ;
            else if (d == '3') freq = Constants::GLONASS_G3_BASE_FREQ;
            else if (d == '4') freq = Constants::GLONASS_G1A_FREQ;
            else if (d == '6') freq = Constants::GLONASS_G2A_FREQ;
            break;
        case System::Unknown: break;  // IRNSS 'I' and any other letter: no wavelength
        }
        if (!(freq > 0.0)) return 0.0;
        return Constants::SPEED_OF_LIGHT / freq;
    }

    /**
     * @brief Split a possibly-BSSD satellite ID ("G01-G05") into primary and reference.
     *
     * BssdTransform forms single-difference pairs as "non_pivot-pivot", so the
     * second element is the pivot when present.  A plain ID returns it with an empty
     * reference.  Forwards to @c SatPairId::parse (common/GnssIdentity.hpp), which owns
     * the rule; kept until the KalmanFilter call sites read @c SatPairId directly.
     *
     * @param sat_id Row satellite ID: "G01", or the pair "G01-G05".
     * @return {primary, reference}; reference is empty for a non-pair ID.
     */
    inline std::pair<std::string, std::string> splitBssdSatId(const std::string& sat_id) {
        SatPairId p = SatPairId::parse(sat_id);
        return { std::move(p.primary), std::move(p.reference) };
    }

} // namespace kalman
} // namespace GnssProcessor
