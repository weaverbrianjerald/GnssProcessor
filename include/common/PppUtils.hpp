#pragma once

/**
 * @file PppUtils.hpp
 * @brief Frequency / wavelength / system-string helpers used across PPP processing.
 *
 * @c PppUtils wraps the per-(system, signal) frequency tables and the
 * GLONASS FDMA channel adjustment that turns a base frequency into a
 * satellite-specific frequency.  One instance is held per receiver, so
 * the GLONASS slot map can be set from that receiver's RINEX header
 * before any GLONASS observation is processed.
 *
 * The static @c getSystemString helpers don't need an instance and
 * are used for log formatting and constellation lookup throughout.
 *
 * Implementation lives in @c PppUtils.cpp.
 */

#include "common/GnssTypes.hpp"
#include "logging/LogManager.hpp"
#include <map>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

    /**
     * @brief Utility functions used by PPP processing modules.
     *
     * Wraps frequency/wavelength lookups and system string conversions.
     * For GLONASS, actual satellite-specific frequencies are computed using
     * the frequency channel number (FDMA). Call setGlonassSlots() to provide
     * the satellite-to-channel mapping before processing GLONASS observations.
     */
    class PppUtils{
    public:
        /**
         * @brief Construct a new PppUtils instance.
         * @param log_mgr Reference to the application's LogManager for logging.
         */
        explicit PppUtils(LogManager& log_mgr);

        /**
         * @brief Set GLONASS frequency slot (channel) assignments.
         *
         * Must be called before processing GLONASS observations to enable
         * satellite-specific frequency computation.
         *
         * @param slots Map of satellite ID (e.g., "R01") to frequency channel number (k ∈ [-7, +6]).
         */
        void setGlonassSlots(const std::map<std::string, int>& slots);

        /**
         * @brief Get the GLONASS frequency channel for a satellite.
         * @param satellite Satellite identifier (e.g., "R01").
         * @return FDMA channel k (from the user-provided slot map, else the
         *         built-in default table). Returns the sentinel -999 if the
         *         satellite is unknown, which getFrequency() treats as
         *         out-of-range and skips.
         */
        int getGlonassChannel(const std::string& satellite) const;

        /**
         * @brief Get the actual frequency (Hz) for a given frequency type and satellite.
         *
         * For GLONASS FDMA bands (1, 2), this returns the satellite-specific frequency
         * computed from the base frequency and channel offset; the CDMA bands (3, 4, 6)
         * have fixed frequencies.  For other systems, returns the nominal carrier
         * frequency.
         *
         * @param freq_type Observable or signal string; the band is found by searching it
         *                  for C, L, D or S followed by the band digit (e.g. "L1", "C2W",
         *                  "L5Q").
         * @param system The GNSS system enum for the satellite.
         * @param satellite Satellite identifier string (required for GLONASS).
         * @return Frequency in Hz, or 0.0 on error.
         */
        double getFrequency(const std::string& freq_type, System system, const std::string& satellite) const;

        /**
         * @brief Get the wavelength (meters) for a given system, frequency type, and satellite.
         *
         * For GLONASS, computes satellite-specific wavelength using actual frequency.
         *
         * @param system The GNSS system enum.
         * @param freq_type Frequency identifier string.
         * @param satellite Satellite identifier (required for GLONASS).
         * @return Wavelength in meters, or 0.0 on error.
         */
        double getWavelength(System system, const std::string& freq_type, const std::string& satellite) const;

        /**
         * @brief Get the wavelength (meters) for a given system and frequency type.
         *
         * For non-GLONASS systems only.  For GLONASS, use the overload with the satellite
         * ID: without one, the FDMA bands resolve no channel and return 0.0.
         *
         * @param system The GNSS system enum.
         * @param freq_type Frequency identifier string.
         * @return Wavelength in meters, or 0.0 on error.
         */
        double getWavelength(System system, const std::string& freq_type) const;

        /**
         * @brief Get a nominal reference frequency (Hz) for a frequency type and system.
         *
         * For GLONASS, returns the base/center frequency (channel k=0) for G1/G2 and the
         * fixed frequency for the CDMA bands.
         * Use this for logging, records, or when satellite-specific frequency is not needed.
         *
         * @param freq_type Frequency identifier string.
         * @param system The GNSS system enum.
         * @return Reference frequency in Hz, or 0.0 on error.
         */
        double getReferenceFrequency(const std::string& freq_type, System system) const;

        /**
         * @brief Convert a System enum value to its printable string representation.
         * @param system System enum value.
         * @return std::string Human-readable system name (e.g. "GPS", "Galileo").
         */
        static std::string getSystemString(System system);

        /**
        * @brief Convert a satellite identifier to its system string representation.
        * @param satellite Satellite identifier (e.g., "G01", "E11", "R05").
        * @return std::string Human-readable system name (e.g. "GPS", "Galileo").
        */
        static std::string getSystemString(const std::string& satellite);

    private:
        LogManager& log_manager_;                     /**< Reference to LogManager used for logging inside utilities */
        std::map<std::string, int> glonass_slots_;    /**< GLONASS satellite -> frequency channel mapping */

        /**
         * @brief Default GLONASS frequency channel assignments.
         *
         * Used as fallback when no slot data is provided via setGlonassSlots().
         */
        static const std::unordered_map<std::string, int> DEFAULT_GLONASS_CHANNELS;
    };

} // namespace GnssProcessor
