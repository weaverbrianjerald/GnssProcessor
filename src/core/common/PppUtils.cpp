/**
 * @file PppUtils.cpp
 * @brief Implementations of PPP utility helpers declared in PppUtils.hpp.
 *
 * This file contains implementations for frequency/wavelength lookups and
 * system string conversion used throughout the PPP processing code.  It
 * implements only the @c PppUtils class (formerly @c PppTypes.cpp, renamed
 * when @c PppTypes.hpp was split into focused headers).
 */

#include "common/PppUtils.hpp"
#include "common/Constants.hpp"

namespace GnssProcessor {

    /**
     * @brief Default GLONASS frequency channel assignments.
     *
     * Used as the fallback for a satellite absent from the slot map given to
     * setGlonassSlots() (for example when the RINEX header lacks GLONASS SLOT/FRQ
     * records).  The assignments change over time as satellites are replaced.
     */
    const std::unordered_map<std::string, int> PppUtils::DEFAULT_GLONASS_CHANNELS = {
        {"R01",  1}, {"R02", -4}, {"R03",  5}, {"R04",  6},
        {"R05",  1}, {"R06", -4}, {"R07",  5}, {"R08",  6},
        {"R09", -2}, {"R10", -7}, {"R11",  0}, {"R12", -1},
        {"R13", -2}, {"R14", -7}, {"R15",  0}, {"R16", -1},
        {"R17",  4}, {"R18", -3}, {"R19",  3}, {"R20",  2},
        {"R21",  4}, {"R22", -3}, {"R23",  3}, {"R24",  2},
        {"R25", -5}, {"R26", -6},
    };

    /**
     * @brief Construct a new PppUtils instance.
     *
     * Stores a reference to the application's LogManager for internal logging.
     *
     * @param log_mgr Reference to LogManager used by this utility.
     */
    PppUtils::PppUtils(LogManager& log_mgr) : log_manager_(log_mgr) {
        log_manager_.getLogger("ppp_utils")->debug("PppUtils initialized");
    }

    /**
     * @brief Set GLONASS frequency slot (channel) assignments.
     *
     * @param slots Map of satellite ID to frequency channel number.
     */
    void PppUtils::setGlonassSlots(const std::map<std::string, int>& slots) {
        glonass_slots_ = slots;
        log_manager_.getLogger("ppp_utils")->debug("Set {} GLONASS frequency slots", slots.size());
    }

    /**
     * @brief Get the GLONASS frequency channel for a satellite.
     *
     * Looks up the slot map from setGlonassSlots() first, then the default table.
     *
     * @param satellite Satellite identifier (e.g., "R01").
     * @return FDMA channel k from the slot map or the default table; the
     *         sentinel -999 if the satellite is unknown (getFrequency()
     *         treats -999 as out-of-range and skips the satellite).
     */
    int PppUtils::getGlonassChannel(const std::string& satellite) const {
        // Check user-provided slots first
        auto it = glonass_slots_.find(satellite);
        if (it != glonass_slots_.end()) {
            return it->second;
        }

        // Fall back to default channels
        auto def_it = DEFAULT_GLONASS_CHANNELS.find(satellite);
        if (def_it != DEFAULT_GLONASS_CHANNELS.end()) {
            log_manager_.getLogger("ppp_utils")->trace("Using default GLONASS channel {} for satellite {}",
                def_it->second, satellite);
            return def_it->second;
        }

        log_manager_.getLogger("ppp_utils")->warn("No GLONASS frequency slot found for satellite {}, using channel -999 to trigger skip",
            satellite);
		return -999; // triggers warning in frequency computation, to skip this satellite
    }

    /**
     * @brief Convert a System enum value to a human-readable string.
     *
     * The returned string is suitable for log messages and textual output.
     *
     * @param system System enum value.
     * @return std::string Human-readable name (e.g. "GPS", "Galileo", "Unknown").
     */
    std::string PppUtils::getSystemString(System system) {
        switch (system) {
        case System::GPS: return "GPS";
        case System::Galileo: return "Galileo";
        case System::GLONASS: return "GLONASS";
        case System::BeiDou: return "BeiDou";
        case System::QZSS: return "QZSS";
        case System::SBAS: return "SBAS";
        default: return "Unknown";
        }
    }

    /**
     * @brief Convert a satellite identifier to a human-readable system string.
     *
     * Extracts the system from the first character of the satellite ID
     * (e.g., "G01" -> "GPS", "E11" -> "Galileo").
     *
     * @param satellite Satellite identifier string (e.g., "G01", "E11", "R05").
     * @return std::string Human-readable system name, or "Unknown" if unrecognized.
     */
    std::string PppUtils::getSystemString(const std::string& satellite) {
        if (satellite.empty()) return "Unknown";
        switch (satellite[0]) {
        case 'G': return "GPS";
        case 'E': return "Galileo";
        case 'R': return "GLONASS";
        case 'C': return "BeiDou";
        case 'J': return "QZSS";
        case 'S': return "SBAS";
        default:  return "Unknown";
        }
    }

    /**
     * @brief Get the actual carrier frequency (Hz) for a given observable type and system.
     *
     * For GLONASS, computes the satellite-specific frequency using FDMA:
     *   G1 = 1602.0 MHz + k * 0.5625 MHz
     *   G2 = 1246.0 MHz + k * 0.4375 MHz
     * The CDMA bands have fixed frequencies with no channel offset:
     *   G3 = 1202.025 MHz (band 3), G1a = 1600.995 MHz (band 4),
     *   G2a = 1248.06 MHz (band 6)
     *
     * For other systems, returns the nominal carrier frequency.
     *
     * The band is found by searching @p freq_type for any of C, L, D or S followed
     * by the band digit.
     *
     * @param freq_type Observable or signal string (e.g. "C1", "L1", "C5").
     * @param system GNSS system enumeration value.
     * @param satellite Satellite identifier (required for GLONASS FDMA frequencies).
     * @return double Frequency in Hz on success, 0.0 on unknown/unsupported input.
     */
    double PppUtils::getFrequency(const std::string& freq_type, System system, const std::string& satellite) const {
        // Helper: check whether freq_type matches any observable type for a given band digit.
        // RINEX observable codes follow {type}{band}{attr} (e.g. C1W, L1C, D1C, S1C).
        // All four observable type prefixes (C, L, D, S) are matched for each band.
        auto matchesBand = [&freq_type](char band) -> bool {
            const char prefixes[] = { 'C', 'L', 'D', 'S' };
            const std::string target(1, band);
            for (char p : prefixes) {
                const std::string pat = std::string(1, p) + target;
                if (freq_type.find(pat) != std::string::npos) return true;
            }
            return false;
        };

        if (system == System::GPS) {
            if (matchesBand('1')) return Constants::GPS_L1_FREQ;
            if (matchesBand('2')) return Constants::GPS_L2_FREQ;
            if (matchesBand('5')) return Constants::GPS_L5_FREQ;
        }
        else if (system == System::Galileo) {
            if (matchesBand('1')) return Constants::GAL_E1_FREQ;
            if (matchesBand('5')) return Constants::GAL_E5A_FREQ;
            if (matchesBand('7')) return Constants::GAL_E5B_FREQ;
            if (matchesBand('6')) return Constants::GAL_E6_FREQ;
            if (matchesBand('8')) return Constants::GAL_E5_FREQ;
        }
        else if (system == System::GLONASS) {
            // CDMA signals (bands 3, 4, 6) have fixed frequencies (no channel offset)
            if (matchesBand('3')) {
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G3 frequency for {} (CDMA): {:.3f} MHz",
                    satellite, Constants::GLONASS_G3_BASE_FREQ / 1e6);
                return Constants::GLONASS_G3_BASE_FREQ;
            }
            if (matchesBand('4')) {
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G1a frequency for {} (CDMA): {:.3f} MHz",
                    satellite, Constants::GLONASS_G1A_FREQ / 1e6);
                return Constants::GLONASS_G1A_FREQ;
            }
            if (matchesBand('6')) {
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G2a frequency for {} (CDMA): {:.3f} MHz",
                    satellite, Constants::GLONASS_G2A_FREQ / 1e6);
                return Constants::GLONASS_G2A_FREQ;
            }

            // FDMA signals (bands 1, 2) require the satellite-specific channel
            int channel = getGlonassChannel(satellite);

            // Validate channel range
            if (channel < -7 || channel > 6) {
                log_manager_.getLogger("ppp_utils")->warn("GLONASS channel {} out of valid range [-7, +6] for satellite {}, type: {}",
                    channel, satellite, freq_type);
                return 0.0;
            }

            double freq = 0.0;
            if (matchesBand('1')) {
                freq = Constants::GLONASS_G1_BASE_FREQ + channel * Constants::GLONASS_G1_CHANNEL_OFFSET;
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G1 frequency for {} (channel {}): {:.3f} MHz",
                    satellite, channel, freq / 1e6);
            }
            else if (matchesBand('2')) {
                freq = Constants::GLONASS_G2_BASE_FREQ + channel * Constants::GLONASS_G2_CHANNEL_OFFSET;
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G2 frequency for {} (channel {}): {:.3f} MHz",
                    satellite, channel, freq / 1e6);
            }
            else {
                log_manager_.getLogger("ppp_utils")->warn("Invalid GLONASS frequency type: {} for satellite {}",
                    freq_type, satellite);
                return 0.0;
            }

            if (freq <= 0.0 || !std::isfinite(freq)) {
                log_manager_.getLogger("ppp_utils")->warn("Computed invalid GLONASS frequency: {:.3f} MHz for type {}, satellite {}, channel {}",
                    freq / 1e6, freq_type, satellite, channel);
                return 0.0;
            }

            return freq;
        }
        else if (system == System::BeiDou) {
            if (matchesBand('1')) return Constants::BDS_B1C_FREQ;
            if (matchesBand('2')) return Constants::BDS_B1_FREQ;
            if (matchesBand('5')) return Constants::BDS_B2A_FREQ;
            if (matchesBand('7')) return Constants::BDS_B2_FREQ;
            if (matchesBand('8')) return Constants::BDS_B2AB_FREQ;
            if (matchesBand('6')) return Constants::BDS_B3_FREQ;
        }
        else if (system == System::QZSS) {
            if (matchesBand('1')) return Constants::QZSS_L1_FREQ;
            if (matchesBand('2')) return Constants::QZSS_L2_FREQ;
            if (matchesBand('5')) return Constants::QZSS_L5_FREQ;
            if (matchesBand('6')) return Constants::QZSS_L6_FREQ;
        }
        else if (system == System::SBAS) {
            if (matchesBand('1')) return Constants::SBAS_L1_FREQ;
            if (matchesBand('5')) return Constants::SBAS_L5_FREQ;
        }

        log_manager_.getLogger("ppp_utils")->warn("Unknown frequency for {} on system {}", freq_type, static_cast<int>(system));
        return 0.0;
    }

    /**
     * @brief Get a reference (nominal) frequency for a given observable type and system.
     *
     * For GLONASS this returns the center frequency of G1/G2 (channel k=0) or the fixed
     * frequency of the CDMA bands G3/G1a/G2a; only C and L observable prefixes match.
     * Use this for logging, records, or when satellite-specific frequency is not needed.
     * For other systems it forwards to getFrequency() to return the nominal carrier frequency.
     *
     * @param freq_type Observable or signal string (e.g. "C1", "L1", "C5").
     * @param system GNSS system enumeration value.
     * @return double Reference frequency in Hz, or 0.0 if unknown/invalid.
     */
    double PppUtils::getReferenceFrequency(const std::string& freq_type, System system) const {
        // Validate input
        if (freq_type.empty()) {
            log_manager_.getLogger("ppp_utils")->warn("Empty frequency type for reference frequency, system={}",
                getSystemString(system));
            return 0.0;
        }

        if (system == System::GLONASS) {
            // Return base/center frequency (channel k=0) for logging/reference purposes
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GLONASS_G1_BASE_FREQ; // Center frequency: 1602 MHz
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::GLONASS_G2_BASE_FREQ; // Center frequency: 1246 MHz
            }
            if (freq_type.find("C3") != std::string::npos || freq_type.find("L3") != std::string::npos) {
                return Constants::GLONASS_G3_BASE_FREQ; // Center frequency: 1202.025 MHz
            }
            if (freq_type.find("C4") != std::string::npos || freq_type.find("L4") != std::string::npos) {
                return Constants::GLONASS_G1A_FREQ; // 1600.995 MHz (G1a, CDMA)
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                return Constants::GLONASS_G2A_FREQ; // 1248.06 MHz (G2a, CDMA)
            }
            log_manager_.getLogger("ppp_utils")->warn("Invalid GLONASS frequency type for reference: {}", freq_type);
            return 0.0;
        }

        // For non-GLONASS systems, return actual frequency (same as reference)
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger("ppp_utils")->warn("Invalid reference frequency for type {} on system {}: {:.3f} MHz",
                freq_type, getSystemString(system), freq / 1e6);
        }
        return freq;
    }

    /**
     * @brief Compute the wavelength (meters) for a given system, frequency type, and satellite.
     *
     * For GLONASS, computes the satellite-specific wavelength from the FDMA frequency.
     * Wavelength is computed as Constants::SPEED_OF_LIGHT / frequency.
     *
     * @param system GNSS system enumeration value.
     * @param freq_type Observable or signal identifier (e.g. "L1", "C5").
     * @param satellite Satellite identifier (required for GLONASS).
     * @return double Wavelength in meters on success, 0.0 on invalid input.
     */
    double PppUtils::getWavelength(System system, const std::string& freq_type, const std::string& satellite) const {
        // Validate input
        if (freq_type.empty()) {
            log_manager_.getLogger("ppp_utils")->warn("Empty frequency type for wavelength computation, system={}",
                getSystemString(system));
            return 0.0;
        }

        // Get actual frequency (satellite-specific for GLONASS)
        double freq = getFrequency(freq_type, system, satellite);
        if (freq <= 0.0 || !std::isfinite(freq)) {
            log_manager_.getLogger("ppp_utils")->warn("Invalid frequency for type {} on system {}, satellite {}: {:.3f} MHz",
                freq_type, getSystemString(system), satellite, freq / 1e6);
            return 0.0;
        }

        // Compute wavelength (meters) = speed of light / frequency
		// TODO: use the Constants::*_WAVELENGTH values where they exist
        double wavelength = Constants::SPEED_OF_LIGHT / freq;
        if (!std::isfinite(wavelength) || wavelength <= 0.0) {
            log_manager_.getLogger("ppp_utils")->warn("Invalid wavelength for type {} on system {}, satellite {}: {:.3f} m",
                freq_type, getSystemString(system), satellite, wavelength);
            return 0.0;
        }

        log_manager_.getLogger("ppp_utils")->trace("Computed wavelength for type {} on system {}, satellite {}: {:.3f} m (freq={:.3f} MHz)",
            freq_type, getSystemString(system), satellite, wavelength, freq / 1e6);
        return wavelength;
    }

    /**
     * @brief Compute the wavelength (meters) for a given system and frequency type.
     *
     * For non-GLONASS systems.  For GLONASS, use the overload with the satellite ID:
     * without one, the FDMA bands resolve no channel and return 0.0.
     *
     * @param system GNSS system enumeration value.
     * @param freq_type Observable or signal identifier (e.g. "L1", "C5").
     * @return double Wavelength in meters on success, 0.0 on invalid input.
     */
    double PppUtils::getWavelength(System system, const std::string& freq_type) const {
        // Delegate to the GLONASS-aware overload with an empty satellite ID.
        // For non-GLONASS systems getFrequency() does not consult the satellite
        // string, so this returns the nominal-band wavelength.
        return getWavelength(system, freq_type, std::string{});
    }

} // namespace GnssProcessor
