#pragma once
#ifndef CONSTANTS_HPP
#define CONSTANTS_HPP

/**
 * @file Constants.hpp
 * @brief Fundamental physical and GNSS signal constants used across the codebase.
 *
 * This header provides a small collection of constexpr constants grouped under
 * `GnssProcessor::Constants`. Values are expressed in SI units unless otherwise noted.
 *
 * Note: These are compile-time constants and intended for use in calculations where
 * precise, documented units are required.
 */

namespace GnssProcessor {

    /**
     * @namespace Constants
     * @brief Container for physical constants, GNSS carrier frequencies and derived wavelengths.
     *
     * All frequency values are in Hertz (Hz). Wavelengths are computed from the
     * speed of light and are in meters (m). Earth-related constants are provided
     * in SI units.
     */
    namespace Constants {

        // ---------------------------------------------------------------------
        // Fundamental physical constants
        // ---------------------------------------------------------------------

        /** @brief Speed of light in vacuum (m/s). Exact value used for GNSS computations. */
        constexpr double SPEED_OF_LIGHT = 299792458.0;       // Speed of light in vacuum (m/s)

        /** @brief Mathematical constant pi (unitless). */
        constexpr double PI = 3.14159265358979323846;        // Mathematical constant pi

        /** @brief Earth's gravitational parameter mu = G * M_earth (m^3 / s^2). */
        constexpr double GRAVITATIONAL_CONSTANT = 3.986005e14; // Gravitational constant * Earth mass (m^3/s^2)

        /** @brief Earth's mass (kg). */
        constexpr double EARTH_MASS = 5.9722e24;             // Earth mass (kg)

        /** @brief Earth's angular velocity (rad/s). Used for ECEF/ECI transformations. */
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5; // Earth's angular velocity (rad/s)

        // ---------------------------------------------------------------------
        // GPS (Global Positioning System) - frequencies (Hz) and wavelengths (m)
        // ---------------------------------------------------------------------

        /** @brief GPS L1 carrier frequency (Hz). */
        constexpr double GPS_L1_FREQ = 1575.42e6;            // GPS L1 frequency (Hz)

        /** @brief GPS L2 carrier frequency (Hz). */
        constexpr double GPS_L2_FREQ = 1227.60e6;            // GPS L2 frequency (Hz)

        /** @brief GPS L5 carrier frequency (Hz). */
        constexpr double GPS_L5_FREQ = 1176.45e6;            // GPS L5 frequency (Hz)

        /** @brief GPS L1 carrier wavelength (m) = c / f. */
        constexpr double GPS_L1_WAVELENGTH = SPEED_OF_LIGHT / GPS_L1_FREQ; // L1 wavelength (m, ~0.190 m)

        /** @brief GPS L2 carrier wavelength (m) = c / f. */
        constexpr double GPS_L2_WAVELENGTH = SPEED_OF_LIGHT / GPS_L2_FREQ; // L2 wavelength (m, ~0.244 m)

        /** @brief GPS L5 carrier wavelength (m) = c / f. */
        constexpr double GPS_L5_WAVELENGTH = SPEED_OF_LIGHT / GPS_L5_FREQ; // L5 wavelength (m, ~0.255 m)

        // ---------------------------------------------------------------------
        // Galileo - frequencies (Hz) and wavelengths (m)
        // ---------------------------------------------------------------------

        /** @brief Galileo E1 carrier frequency (Hz). */
        constexpr double GAL_E1_FREQ = 1575.42e6;            // Galileo E1 frequency (Hz)

        /** @brief Galileo E5a carrier frequency (Hz). */
        constexpr double GAL_E5A_FREQ = 1176.45e6;           // Galileo E5a frequency (Hz)

        /** @brief Galileo E5b carrier frequency (Hz). */
        constexpr double GAL_E5B_FREQ = 1207.14e6;           // Galileo E5b frequency (Hz)

        /** @brief Galileo E6 carrier frequency (Hz). */
        constexpr double GAL_E6_FREQ = 1278.75e6;            // Galileo E6 frequency (Hz)

        /** @brief Galileo E5 AltBOC representative frequency (Hz). */
        constexpr double GAL_E5_FREQ = 1191.795e6;           // Galileo E5 AltBOC frequency (Hz)

        /** @brief Galileo E1 wavelength (m) = c / f. */
        constexpr double GAL_E1_WAVELENGTH = SPEED_OF_LIGHT / GAL_E1_FREQ;   // E1 wavelength (m, ~0.191 m)

        /** @brief Galileo E5a wavelength (m) = c / f. */
        constexpr double GAL_E5A_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5A_FREQ; // E5a wavelength (m, ~0.255 m)

        /** @brief Galileo E5b wavelength (m) = c / f. */
        constexpr double GAL_E5B_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5B_FREQ; // E5b wavelength (m, ~0.248 m)

        /** @brief Galileo E6 wavelength (m) = c / f. */
        constexpr double GAL_E6_WAVELENGTH = SPEED_OF_LIGHT / GAL_E6_FREQ;   // E6 wavelength (m, ~0.234 m)

        /** @brief Galileo E5 AltBOC representative wavelength (m) = c / f. */
        constexpr double GAL_E5_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5_FREQ;   // E5 AltBOC wavelength (m, ~0.252 m)

        // ---------------------------------------------------------------------
        // GLONASS - base frequencies (Hz), channel offsets (Hz) and wavelengths (m)
        // ---------------------------------------------------------------------

        /**
         * @brief GLONASS G1 base frequency (Hz) for channel k=0.
         *
         * Actual per-satellite frequency: G1 = GLONASS_G1_BASE_FREQ + k * GLONASS_G1_CHANNEL_OFFSET,
         * where k ∈ [-7, +6] is the frequency channel number.
         */
        constexpr double GLONASS_G1_BASE_FREQ = 1602.0e6;    // GLONASS G1 base frequency (Hz)

        /** @brief GLONASS G1 per-channel offset (Hz). */
        constexpr double GLONASS_G1_CHANNEL_OFFSET = 0.5625e6; // G1 channel offset (Hz)

        /**
         * @brief GLONASS G2 base frequency (Hz) for channel k=0.
         *
         * Actual per-satellite frequency: G2 = GLONASS_G2_BASE_FREQ + k * GLONASS_G2_CHANNEL_OFFSET.
         */
        constexpr double GLONASS_G2_BASE_FREQ = 1246.0e6;    // GLONASS G2 base frequency (Hz)

        /** @brief GLONASS G2 per-channel offset (Hz). */
        constexpr double GLONASS_G2_CHANNEL_OFFSET = 0.4375e6; // G2 channel offset (Hz)

        /** @brief GLONASS G3 (fixed) frequency (Hz). */
        constexpr double GLONASS_G3_BASE_FREQ = 1202.025e6;  // GLONASS G3 frequency (Hz, fixed)

        /** @brief GLONASS G3 wavelength (m) = c / f. */
        constexpr double GLONASS_G3_WAVELENGTH = SPEED_OF_LIGHT / GLONASS_G3_BASE_FREQ; // G3 wavelength (m, ~0.249 m)

        // ---------------------------------------------------------------------
        // BeiDou (BDS) - frequencies (Hz) and wavelengths (m)
        // ---------------------------------------------------------------------

        /** @brief BeiDou B1 frequency (Hz). */
        constexpr double BDS_B1_FREQ = 1561.098e6;           // BeiDou B1 frequency (Hz)

        /** @brief BeiDou B2 frequency (Hz). */
        constexpr double BDS_B2_FREQ = 1207.14e6;            // BeiDou B2 frequency (Hz)

        /** @brief BeiDou B3 frequency (Hz). */
        constexpr double BDS_B3_FREQ = 1268.52e6;            // BeiDou B3 frequency (Hz)

        /** @brief BeiDou B1 wavelength (m) = c / f. */
        constexpr double BDS_B1_WAVELENGTH = SPEED_OF_LIGHT / BDS_B1_FREQ; // B1 wavelength (m, ~0.192 m)

        /** @brief BeiDou B2 wavelength (m) = c / f. */
        constexpr double BDS_B2_WAVELENGTH = SPEED_OF_LIGHT / BDS_B2_FREQ; // B2 wavelength (m, ~0.248 m)

        /** @brief BeiDou B3 wavelength (m) = c / f. */
        constexpr double BDS_B3_WAVELENGTH = SPEED_OF_LIGHT / BDS_B3_FREQ; // B3 wavelength (m, ~0.236 m)

        // ---------------------------------------------------------------------
        // QZSS - frequencies (Hz) and wavelengths (m)
        // ---------------------------------------------------------------------

        /** @brief QZSS L1 frequency (Hz). */
        constexpr double QZSS_L1_FREQ = 1575.42e6;           // QZSS L1 frequency (Hz)

        /** @brief QZSS L2 frequency (Hz). */
        constexpr double QZSS_L2_FREQ = 1227.60e6;           // QZSS L2 frequency (Hz)

        /** @brief QZSS L5 frequency (Hz). */
        constexpr double QZSS_L5_FREQ = 1176.45e6;           // QZSS L5 frequency (Hz)

        /** @brief QZSS L1 wavelength (m) = c / f. */
        constexpr double QZSS_L1_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L1_FREQ; // L1 wavelength (m, ~0.191 m)

        /** @brief QZSS L2 wavelength (m) = c / f. */
        constexpr double QZSS_L2_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L2_FREQ; // L2 wavelength (m, ~0.244 m)

        /** @brief QZSS L5 wavelength (m) = c / f. */
        constexpr double QZSS_L5_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L5_FREQ; // L5 wavelength (m, ~0.255 m)

        // ---------------------------------------------------------------------
        // SBAS - frequencies (Hz) and wavelengths (m)
        // ---------------------------------------------------------------------

        /** @brief SBAS L1 frequency (Hz). */
        constexpr double SBAS_L1_FREQ = 1575.42e6;           // SBAS L1 frequency (Hz)

        /** @brief SBAS L5 frequency (Hz). */
        constexpr double SBAS_L5_FREQ = 1176.45e6;           // SBAS L5 frequency (Hz)

        /** @brief SBAS L1 wavelength (m) = c / f. */
        constexpr double SBAS_L1_WAVELENGTH = SPEED_OF_LIGHT / SBAS_L1_FREQ; // L1 wavelength (m, ~0.191 m)

        /** @brief SBAS L5 wavelength (m) = c / f. */
        constexpr double SBAS_L5_WAVELENGTH = SPEED_OF_LIGHT / SBAS_L5_FREQ; // L5 wavelength (m, ~0.255 m)

    } // namespace Constants

} // namespace GnssProcessor

#endif // CONSTANTS_HPP
