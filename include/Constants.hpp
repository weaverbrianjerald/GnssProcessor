#pragma once

namespace GnssProcessor {

	namespace Constants {

		constexpr double SPEED_OF_LIGHT = 299792458.0; // m/s
		constexpr double PI = 3.14159265358979323846;
		constexpr double GRAVITATIONAL_CONSTANT = 3.986005e14; // m³/s²

		// GPS frequencies and wavelengths
		constexpr double GPS_L1_FREQ = 1575.42e6; // Hz
		constexpr double GPS_L2_FREQ = 1227.60e6; // Hz
		constexpr double GPS_L5_FREQ = 1176.45e6; // Hz
		constexpr double GPS_L1_WAVELENGTH = SPEED_OF_LIGHT / GPS_L1_FREQ; // m
		constexpr double GPS_L2_WAVELENGTH = SPEED_OF_LIGHT / GPS_L2_FREQ; // m
		constexpr double GPS_L5_WAVELENGTH = SPEED_OF_LIGHT / GPS_L5_FREQ; // m

		// Galileo frequencies and wavelengths
		constexpr double GAL_E1_FREQ = 1575.42e6; // Hz
		constexpr double GAL_E5A_FREQ = 1176.45e6; // Hz
		constexpr double GAL_E5B_FREQ = 1207.14e6; // Hz
		constexpr double GAL_E1_WAVELENGTH = SPEED_OF_LIGHT / GAL_E1_FREQ; // m
		constexpr double GAL_E5A_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5A_FREQ; // m
		constexpr double GAL_E5B_WAVELENGTH = SPEED_OF_LIGHT / GAL_E5B_FREQ; // m

		// GLONASS base frequencies and channel offsets
		constexpr double GLONASS_G1_BASE_FREQ = 1602.0e6; // Hz
		constexpr double GLONASS_G1_CHANNEL_OFFSET = 0.5625e6; // Hz per channel
		constexpr double GLONASS_G2_BASE_FREQ = 1246.0e6; // Hz
		constexpr double GLONASS_G2_CHANNEL_OFFSET = 0.4375e6; // Hz per channel
		// GLONASS frequency for channel k: G1 = 1602 + k*0.5625 MHz, G2 = 1246 + k*0.4375 MHz, k = -7 to +6

		// BeiDou frequencies and wavelengths
		constexpr double BDS_B1I_FREQ = 1561.098e6; // Hz
		constexpr double BDS_B2I_FREQ = 1207.14e6; // Hz
		constexpr double BDS_B3I_FREQ = 1268.52e6; // Hz
		constexpr double BDS_B1I_WAVELENGTH = SPEED_OF_LIGHT / BDS_B1I_FREQ; // m
		constexpr double BDS_B2I_WAVELENGTH = SPEED_OF_LIGHT / BDS_B2I_FREQ; // m
		constexpr double BDS_B3I_WAVELENGTH = SPEED_OF_LIGHT / BDS_B3I_FREQ; // m

		// QZSS frequencies and wavelengths
		constexpr double QZSS_L1_FREQ = 1575.42e6; // Hz
		constexpr double QZSS_L2_FREQ = 1227.60e6; // Hz
		constexpr double QZSS_L5_FREQ = 1176.45e6; // Hz
		constexpr double QZSS_L1_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L1_FREQ; // m
		constexpr double QZSS_L2_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L2_FREQ; // m
		constexpr double QZSS_L5_WAVELENGTH = SPEED_OF_LIGHT / QZSS_L5_FREQ; // m

		// SBAS frequencies and wavelengths
		constexpr double SBAS_L1_FREQ = 1575.42e6; // Hz
		constexpr double SBAS_L5_FREQ = 1176.45e6; // Hz
		constexpr double SBAS_L1_WAVELENGTH = SPEED_OF_LIGHT / SBAS_L1_FREQ; // m
		constexpr double SBAS_L5_WAVELENGTH = SPEED_OF_LIGHT / SBAS_L5_FREQ; // m

	}

} // namespace GnssProcessor
//#endif // CONSTANTS_HPP