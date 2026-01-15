#pragma once
#ifndef CONSTANTS_HPP
#define CONSTANTS_HPP

namespace GnssProcessor {

	namespace Constants {

		// Fundamental physical constants
		constexpr double SPEED_OF_LIGHT = 299792458.0;       // Speed of light in vacuum (m/s)
		constexpr double PI = 3.14159265358979323846;        // Mathematical constant pi
		constexpr double GRAVITATIONAL_CONSTANT = 3.986005e14; // Gravitational constant * Earth mass (m^3/s^2)
		constexpr double EARTH_MASS = 5.9722e24;             // Earth mass (kg)
		constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5; // Earth's angular velocity (rad/s)

		// GPS frequencies and wavelengths (L1, L2, L5 bands)
		constexpr double GPS_L1_FREQ = 1575.42e6;            // GPS L1 frequency (Hz)
		constexpr double GPS_L2_FREQ = 1227.60e6;            // GPS L2 frequency (Hz)
		constexpr double GPS_L5_FREQ = 1176.45e6;            // GPS L5 frequency (Hz)
		constexpr double GPS_L1_WAVELENGTH = SPEED_OF_LIGHT / GPS_L1_FREQ; // L1 wavelength (m, ~0.190 m)
		constexpr double GPS_L2_WAVELENGTH = SPEED_OF_LIGHT / GPS_L2_FREQ; // L2 wavelength (m, ~0.244 m)
		constexpr double GPS_L5_WAVELENGTH = SPEED_OF_LIGHT / GPS_L5_FREQ; // L5 wavelength (m, ~0.255 m)

		// Galileo frequencies and wavelengths (E1, E5a, E5b, E6, E5 AltBOC)
		constexpr double GAL_E1_FREQ = 1575.42e6;            // Galileo E1 frequency (Hz)
		constexpr double GAL_E5A_FREQ = 1176.45e6;           // Galileo E5a frequency (Hz)
		constexpr double GAL_E5B_FREQ = 1207.14e6;           // Galileo E5b frequency (Hz)
		constexpr double GAL_E6_FREQ = 1278.75e6;            // Galileo E6 frequency (Hz)
		constexpr double GAL_E5_FREQ = 1191.795e6;           // Galileo E5 AltBOC frequency (Hz)
		constexpr double GAL_E1_WAVELENGTH = SPEED_OF_LIGHT / GAL_E1_FREQ;   // E1 wavelength (m, ~0.191 m)
		constexpr double GAL_E5A_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5A_FREQ; // E5a wavelength (m, ~0.255 m)
		constexpr double GAL_E5B_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5B_FREQ; // E5b wavelength (m, ~0.248 m)
		constexpr double GAL_E6_WAVELENGTH = SPEED_OF_LIGHT / GAL_E6_FREQ;   // E6 wavelength (m, ~0.234 m)
		constexpr double GAL_E5_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5_FREQ;   // E5 AltBOC wavelength (m, ~0.252 m)

		// GLONASS base frequencies and channel offsets (G1, G2, G3 bands)
		// G1 = 1602 + k * 0.5625 MHz, G2 = 1246 + k * 0.4375 MHz, k = -7 to +6
		constexpr double GLONASS_G1_BASE_FREQ = 1602.0e6;    // GLONASS G1 base frequency (Hz)
		constexpr double GLONASS_G1_CHANNEL_OFFSET = 0.5625e6; // G1 channel offset (Hz)
		constexpr double GLONASS_G2_BASE_FREQ = 1246.0e6;    // GLONASS G2 base frequency (Hz)
		constexpr double GLONASS_G2_CHANNEL_OFFSET = 0.4375e6; // G2 channel offset (Hz)
		constexpr double GLONASS_G3_BASE_FREQ = 1202.025e6;  // GLONASS G3 frequency (Hz, fixed)
		constexpr double GLONASS_G3_WAVELENGTH = SPEED_OF_LIGHT / GLONASS_G3_BASE_FREQ; // G3 wavelength (m, ~0.249 m)

		// BeiDou frequencies and wavelengths (B1, B2, B3 bands)
		constexpr double BDS_B1_FREQ = 1561.098e6;           // BeiDou B1 frequency (Hz)
		constexpr double BDS_B2_FREQ = 1207.14e6;            // BeiDou B2 frequency (Hz)
		constexpr double BDS_B3_FREQ = 1268.52e6;            // BeiDou B3 frequency (Hz)
		constexpr double BDS_B1_WAVELENGTH = SPEED_OF_LIGHT / BDS_B1_FREQ; // B1 wavelength (m, ~0.192 m)
		constexpr double BDS_B2_WAVELENGTH = SPEED_OF_LIGHT / BDS_B2_FREQ; // B2 wavelength (m, ~0.248 m)
		constexpr double BDS_B3_WAVELENGTH = SPEED_OF_LIGHT / BDS_B3_FREQ; // B3 wavelength (m, ~0.236 m)

		// QZSS frequencies and wavelengths (L1, L2, L5 bands)
		constexpr double QZSS_L1_FREQ = 1575.42e6;           // QZSS L1 frequency (Hz)
		constexpr double QZSS_L2_FREQ = 1227.60e6;           // QZSS L2 frequency (Hz)
		constexpr double QZSS_L5_FREQ = 1176.45e6;           // QZSS L5 frequency (Hz)
		constexpr double QZSS_L1_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L1_FREQ; // L1 wavelength (m, ~0.191 m)
		constexpr double QZSS_L2_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L2_FREQ; // L2 wavelength (m, ~0.244 m)
		constexpr double QZSS_L5_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L5_FREQ; // L5 wavelength (m, ~0.255 m)

		// SBAS frequencies and wavelengths (L1, L5 bands)
		constexpr double SBAS_L1_FREQ = 1575.42e6;           // SBAS L1 frequency (Hz)
		constexpr double SBAS_L5_FREQ = 1176.45e6;           // SBAS L5 frequency (Hz)
		constexpr double SBAS_L1_WAVELENGTH = SPEED_OF_LIGHT / SBAS_L1_FREQ; // L1 wavelength (m, ~0.191 m)
		constexpr double SBAS_L5_WAVELENGTH = SPEED_OF_LIGHT / SBAS_L5_FREQ; // L5 wavelength (m, ~0.255 m)

	} // namespace Constants

} // namespace GnssProcessor

#endif // CONSTANTS_HPP
