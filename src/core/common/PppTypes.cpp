/**
 * @file PppTypes.cpp
 * @brief Implementations of PPP utility helpers declared in PppTypes.hpp.
 *
 * This file contains implementations for frequency/wavelength lookups and
 * system string conversion used throughout the PPP processing code.
 */

#include "PppTypes.hpp"

namespace GnssProcessor {

    /**
     * @brief Default GLONASS frequency channel assignments.
     *
     * Used as fallback when RINEX header lacks GLONASS SLOT/FRQ records.
     * Note: These may change over time as satellites are replaced.
     */
    const std::unordered_map<std::string, int> PppUtils::DEFAULT_GLONASS_CHANNELS = {
        {"R01",  1}, {"R02", -4}, {"R03",  5}, {"R04",  6},
        {"R05",  1}, {"R06", -4}, {"R07",  5}, {"R08",  6},
        {"R09", -2}, {"R10", -7}, {"R11",  0}, {"R12", -1},
        {"R13", -2}, {"R14", -7}, {"R15",  0}, {"R16", -1},
        {"R17",  4}, {"R18", -3}, {"R19",  3}, {"R20",  2},
        {"R21",  4}, {"R22", -3}, {"R23",  3}, {"R24",  2},
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
     * Looks up in user-provided slots first, then falls back to defaults.
     *
     * @param satellite Satellite identifier (e.g., "R01").
     * @return Channel number if found, 0 otherwise.
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
     *   G3 = 1202.025 MHz (CDMA, no channel offset)
     *
     * For other systems, returns the nominal carrier frequency.
     *
     * @param freq_type Observable or signal string (e.g. "C1", "L1", "C5").
     * @param system GNSS system enumeration value.
     * @param satellite Satellite identifier (required for GLONASS FDMA frequencies).
     * @return double Frequency in Hz on success, 0.0 on unknown/unsupported input.
     */
    double PppUtils::getFrequency(const std::string& freq_type, System system, const std::string& satellite) const {
        if (system == System::GPS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GPS_L1_FREQ;
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::GPS_L2_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::GPS_L5_FREQ;
            }
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GAL_E1_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::GAL_E5A_FREQ;
            }
            if (freq_type.find("C7") != std::string::npos || freq_type.find("L7") != std::string::npos) {
                return Constants::GAL_E5B_FREQ;
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                return Constants::GAL_E6_FREQ;
            }
            if (freq_type.find("C8") != std::string::npos || freq_type.find("L8") != std::string::npos) {
                return Constants::GAL_E5_FREQ;
            }
        }
        else if (system == System::GLONASS) {
            // CDMA signals (bands 3, 4, 6) have fixed frequencies — no channel offset
            if (freq_type.find("C3") != std::string::npos || freq_type.find("L3") != std::string::npos) {
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G3 frequency for {} (CDMA): {:.3f} MHz",
                    satellite, Constants::GLONASS_G3_BASE_FREQ / 1e6);
                return Constants::GLONASS_G3_BASE_FREQ;
            }
            if (freq_type.find("C4") != std::string::npos || freq_type.find("L4") != std::string::npos) {
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G1a frequency for {} (CDMA): {:.3f} MHz",
                    satellite, Constants::GLONASS_G1A_FREQ / 1e6);
                return Constants::GLONASS_G1A_FREQ;
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G2a frequency for {} (CDMA): {:.3f} MHz",
                    satellite, Constants::GLONASS_G2A_FREQ / 1e6);
                return Constants::GLONASS_G2A_FREQ;
            }

            // FDMA signals (bands 1, 2) require satellite-specific channel
            int channel = getGlonassChannel(satellite);

            // Validate channel range
            if (channel < -7 || channel > 6) {
                log_manager_.getLogger("ppp_utils")->warn("GLONASS channel {} out of valid range [-7, +6] for satellite {}, type: {}",
                    channel, satellite, freq_type);
                return 0.0;
            }

            double freq = 0.0;
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                freq = Constants::GLONASS_G1_BASE_FREQ + channel * Constants::GLONASS_G1_CHANNEL_OFFSET;
                log_manager_.getLogger("ppp_utils")->trace("GLONASS G1 frequency for {} (channel {}): {:.3f} MHz",
                    satellite, channel, freq / 1e6);
            }
            else if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
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
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::BDS_B1C_FREQ;
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::BDS_B1_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::BDS_B2A_FREQ;
            }
            if (freq_type.find("C7") != std::string::npos || freq_type.find("L7") != std::string::npos) {
                return Constants::BDS_B2_FREQ;
            }
            if (freq_type.find("C8") != std::string::npos || freq_type.find("L8") != std::string::npos) {
                return Constants::BDS_B2AB_FREQ;
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                return Constants::BDS_B3_FREQ;
            }
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::QZSS_L1_FREQ;
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::QZSS_L2_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::QZSS_L5_FREQ;
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                return Constants::QZSS_L6_FREQ;
            }
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::SBAS_L1_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::SBAS_L5_FREQ;
            }
        }

        log_manager_.getLogger("ppp_utils")->warn("Unknown frequency for {} on system {}", freq_type, static_cast<int>(system));
        return 0.0;
    }

    /**
     * @brief Get a reference (nominal) frequency for a given observable type and system.
     *
     * For GLONASS this returns the system's center/reference frequency (G1/G2/G3 at k=0).
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
                return Constants::GLONASS_G1A_FREQ; // G1a CDMA: 1600.995 MHz
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                return Constants::GLONASS_G2A_FREQ; // G2a CDMA: 1248.06 MHz
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
     * For GLONASS, computes satellite-specific wavelength using actual FDMA frequency.
     * Wavelength is computed as SPEED_OF_LIGHT / frequency.
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
		// TODO: Use CONSTANTS::WAVELENGTH_xx if available
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
     * For non-GLONASS systems. For GLONASS, use the overload with satellite ID.
     *
     * @param system GNSS system enumeration value.
     * @param freq_type Observable or signal identifier (e.g. "L1", "C5").
     * @return double Wavelength in meters on success, 0.0 on invalid input.
     */
    double PppUtils::getWavelength(System system, const std::string& freq_type) const {
        if (system == System::GLONASS) {
            log_manager_.getLogger("ppp_utils")->warn("getWavelength called for GLONASS without satellite ID; using reference frequency");
        }
        return getWavelength(system, freq_type, "");
    }
}