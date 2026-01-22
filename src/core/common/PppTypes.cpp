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
     * @brief Get the carrier frequency (Hz) for a given observable type and system.
     *
     * This routine maps common observable labels (e.g. "C1", "L1", "C5") together
     * with the GNSS system to the nominal carrier frequency in Hz. GLONASS uses
     * system-dependent base/reference frequencies and therefore returns the
     * GLONASS center/reference frequency rather than a satellite-specific
     * channel offset (rescaling is handled elsewhere when needed).
     *
     * @param freq_type Observable or signal string (e.g. "C1", "L1", "C5").
     * @param system GNSS system enumeration value.
     * @param satellite Satellite identifier (unused for most systems; may be used for satellite-specific handling).
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
        }
        else if (system == System::GLONASS) {
            // Return reference frequency for rescaled phase observations
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                log_manager_.getLogger("ppp_utils")->trace("Using GLONASS G1 reference frequency: {} MHz for type {}",
                    Constants::GLONASS_G1_BASE_FREQ / 1e6, freq_type);
                return Constants::GLONASS_G1_BASE_FREQ; // 1602 MHz
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                log_manager_.getLogger("ppp_utils")->trace("Using GLONASS G2 reference frequency: {} MHz for type {}",
                    Constants::GLONASS_G2_BASE_FREQ / 1e6, freq_type);
                return Constants::GLONASS_G2_BASE_FREQ; // 1246 MHz
            }
            log_manager_.getLogger("ppp_utils")->warn("Invalid GLONASS frequency type: {}", freq_type);
            return 0.0;
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::BDS_B1_FREQ;
            }
            if (freq_type.find("C7") != std::string::npos || freq_type.find("L7") != std::string::npos) {
                return Constants::BDS_B2_FREQ;
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
     * For GLONASS this returns the system's center/reference frequency (G1/G2/G3).
     * For other systems it forwards to getFrequency() to return the nominal
     * carrier frequency. Warnings are logged when inputs are invalid or unknown.
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
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GLONASS_G1_BASE_FREQ; // Center frequency: 1602 MHz
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::GLONASS_G2_BASE_FREQ; // Center frequency: 1246 MHz
            }
            if (freq_type.find("C3") != std::string::npos || freq_type.find("L3") != std::string::npos) {
                return Constants::GLONASS_G3_BASE_FREQ; // Center frequency: 1202.025 MHz
            }
        }

        // For non-GLONASS systems, return actual frequency (no rescaling needed)
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger("ppp_utils")->warn("Invalid reference frequency for type {} on system {}: {:.3f} MHz",
                freq_type, getSystemString(system), freq / 1e6);
        }
        return freq;
    }

    /**
     * @brief Compute the wavelength (meters) for a given system and observable type.
     *
     * Wavelength is computed as SPEED_OF_LIGHT / frequency. Input validation is
     * performed and warnings are logged on invalid inputs or computation errors.
     *
     * @param system GNSS system enumeration value.
     * @param freq_type Observable or signal identifier (e.g. "L1", "C5").
     * @return double Wavelength in meters on success, 0.0 on invalid input.
     */
    double PppUtils::getWavelength(System system, const std::string& freq_type) const {
        // Validate input
        if (freq_type.empty()) {
            log_manager_.getLogger("ppp_utils")->warn("Empty frequency type for wavelength computation, system={}",
                getSystemString(system));
            return 0.0;
        }

        // Get frequency, passing empty satellite ID as specific satellite not needed here
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0 || !std::isfinite(freq)) {
            log_manager_.getLogger("ppp_utils")->warn("Invalid frequency for type {} on system {}: {:.3f} MHz",
                freq_type, getSystemString(system), freq / 1e6);
            return 0.0;
        }

        // Compute wavelength (meters) = speed of light / frequency
        double wavelength = Constants::SPEED_OF_LIGHT / freq;
        if (!std::isfinite(wavelength) || wavelength <= 0.0) {
            log_manager_.getLogger("ppp_utils")->warn("Invalid wavelength for type {} on system {}: {:.3f} m",
                freq_type, getSystemString(system), wavelength);
            return 0.0;
        }

        log_manager_.getLogger("ppp_utils")->trace("Computed wavelength for type {} on system {}: {:.3f} m (freq={:.3f} MHz)",
            freq_type, getSystemString(system), wavelength, freq / 1e6);
        return wavelength;
    }

}